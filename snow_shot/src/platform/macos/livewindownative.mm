#include "../../presentation/pinned/livewindownative_p.h"
#include "selectorwindowtarget_p.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <QCoreApplication>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace snow_shot::presentation {
namespace {
QString nativeText(const char* text) {
    return QCoreApplication::translate("LiveWindowSource", text);
}
QRect windowBounds(CGWindowID id, pid_t expectedPid, bool* exists = nullptr,
                   bool* querySucceeded = nullptr) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, id);
    if (querySucceeded)
        *querySucceeded = windows != nullptr;
    bool valid = false;
    CGRect bounds{};
    if (windows) {
        for (CFIndex index = 0; index < CFArrayGetCount(windows); ++index) {
            const auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, index));
            const auto number =
                static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowNumber));
            const auto process =
                static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowOwnerPID));
            CGWindowID nativeId = 0;
            pid_t pid = 0;
            if (!number || !process || !CFNumberGetValue(number, kCFNumberIntType, &nativeId) ||
                !CFNumberGetValue(process, kCFNumberIntType, &pid) || nativeId != id ||
                pid != expectedPid)
                continue;
            valid = true;
            const auto rect =
                static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds));
            if (rect)
                CGRectMakeWithDictionaryRepresentation(rect, &bounds);
            break;
        }
    }
    if (windows)
        CFRelease(windows);
    if (exists)
        *exists = valid;
    if (!valid)
        return {};
    return QRect(qRound(bounds.origin.x), qRound(bounds.origin.y), qRound(bounds.size.width),
                 qRound(bounds.size.height));
}
bool attributeBoolean(AXUIElementRef element, CFStringRef name, bool* result,
                      AXError* nativeError = nullptr) {
    CFTypeRef value = nullptr;
    const AXError error = AXUIElementCopyAttributeValue(element, name, &value);
    if (nativeError)
        *nativeError = error;
    const bool valid =
        error == kAXErrorSuccess && value && CFGetTypeID(value) == CFBooleanGetTypeID();
    if (valid)
        *result = CFBooleanGetValue(static_cast<CFBooleanRef>(value));
    if (value)
        CFRelease(value);
    return valid;
}
std::optional<QRectF> axBounds(AXUIElementRef element) {
    CFTypeRef position = nullptr;
    CFTypeRef size = nullptr;
    const AXError positionError =
        AXUIElementCopyAttributeValue(element, kAXPositionAttribute, &position);
    const AXError sizeError = AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &size);
    CGPoint point{};
    CGSize extent{};
    const bool valid =
        positionError == kAXErrorSuccess && sizeError == kAXErrorSuccess && position && size &&
        CFGetTypeID(position) == AXValueGetTypeID() && CFGetTypeID(size) == AXValueGetTypeID() &&
        AXValueGetValue(static_cast<AXValueRef>(position), kAXValueTypeCGPoint, &point) &&
        AXValueGetValue(static_cast<AXValueRef>(size), kAXValueTypeCGSize, &extent);
    if (position)
        CFRelease(position);
    if (size)
        CFRelease(size);
    if (!valid)
        return std::nullopt;
    return QRectF(point.x, point.y, extent.width, extent.height);
}
bool matchingBounds(AXUIElementRef element, const QRect& nativeBounds) {
    const auto bounds = axBounds(element);
    constexpr qreal tolerance = 2;
    return bounds && qAbs(bounds->x() - nativeBounds.x()) <= tolerance &&
           qAbs(bounds->y() - nativeBounds.y()) <= tolerance &&
           qAbs(bounds->width() - nativeBounds.width()) <= tolerance &&
           qAbs(bounds->height() - nativeBounds.height()) <= tolerance;
}

// AX messaging can take its configured timeout. Its retained element and state
// outlive the monitor only until the cancelled background query returns.
struct MacWindowPollState {
    ~MacWindowPollState() {
        if (window)
            CFRelease(window);
    }
    AXUIElementRef window = nullptr;
    snow_shot::runtime::RuntimeActivityLease retirementActivity;
    std::atomic<bool> closed{false};
    std::atomic<bool> minimized{false};
    std::atomic<bool> available{true};
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::condition_variable changed;
};

class MacLiveWindow final : public live_window_detail::NativeWindowMonitor {
  public:
    explicit MacLiveWindow(LiveWindowTarget target) : m_target(std::move(target)) {}
    ~MacLiveWindow() override {
        if (m_localCloseObserver) {
            [NSNotificationCenter.defaultCenter removeObserver:m_localCloseObserver];
            [m_localCloseObserver release];
        }
        {
            std::lock_guard lock(m_poll->mutex);
            m_poll->retirementActivity =
                snow_shot::runtime::RuntimeActivityTracker::shared().acquire();
            m_poll->stopping = true;
        }
        m_poll->changed.notify_all();
        if (m_observer) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(m_observer),
                                  kCFRunLoopCommonModes);
            CFRelease(m_observer);
        }
        if (m_window)
            CFRelease(m_window);
        if (m_application)
            CFRelease(m_application);
        [m_localWindow release];
        [m_targetApplication release];
    }
    bool initialize(QString* error) override {
        if (!CGPreflightScreenCaptureAccess()) {
            if (error)
                *error = nativeText(QT_TRANSLATE_NOOP("LiveWindowSource",
                                                      "Allow Screen & System Audio Recording in "
                                                      "System Settings to pin a live window."));
            return false;
        }
        if (m_target.processId == NSProcessInfo.processInfo.processIdentifier) {
            m_localWindow =
                [[NSApp windowWithWindowNumber:static_cast<NSInteger>(m_target.nativeId)] retain];
            if (m_localWindow) {
                m_localCloseObserver = [[NSNotificationCenter.defaultCenter
                    addObserverForName:NSWindowWillCloseNotification
                                object:m_localWindow
                                 queue:nil
                            usingBlock:^(NSNotification*) {
                              m_poll->closed = true;
                              const auto notify = changed;
                              if (notify)
                                  notify();
                            }] retain];
                return true;
            }
            return fail(error);
        }
        if (!AXIsProcessTrusted()) {
            if (error)
                *error = nativeText(QT_TRANSLATE_NOOP(
                    "LiveWindowSource",
                    "Allow Accessibility in System Settings to monitor the live window's state."));
            return false;
        }
        const pid_t pid = static_cast<pid_t>(m_target.processId);
        m_targetApplication =
            [[NSRunningApplication runningApplicationWithProcessIdentifier:pid] retain];
        if (!m_targetApplication || m_targetApplication.terminated)
            return fail(error);
        m_application = AXUIElementCreateApplication(pid);
        AXUIElementSetMessagingTimeout(m_application, 0.1F);
        AXUIElementRef hit = nullptr;
        if (AXUIElementCopyElementAtPosition(
                m_application, static_cast<float>(m_target.hitPoint.x()),
                static_cast<float>(m_target.hitPoint.y()), &hit) == kAXErrorSuccess &&
            hit) {
            CFTypeRef window = nullptr;
            if (AXUIElementCopyAttributeValue(hit, kAXWindowAttribute, &window) ==
                    kAXErrorSuccess &&
                window && CFGetTypeID(window) == AXUIElementGetTypeID()) {
                m_window = static_cast<AXUIElementRef>(window);
            } else {
                if (window)
                    CFRelease(window);
                CFTypeRef role = nullptr;
                if (AXUIElementCopyAttributeValue(hit, kAXRoleAttribute, &role) ==
                        kAXErrorSuccess &&
                    role && CFEqual(role, kAXWindowRole))
                    m_window = static_cast<AXUIElementRef>(CFRetain(hit));
                if (role)
                    CFRelease(role);
            }
            CFRelease(hit);
        }
        if (m_window && !matchingBounds(m_window, m_target.desktopBounds)) {
            CFRelease(m_window);
            m_window = nullptr;
        }
        if (!m_window) {
            // Providers without hit testing can expose AXWindows. Resolve only
            // one exact geometry match; ambiguity must never retarget capture.
            CFTypeRef windows = nullptr;
            if (AXUIElementCopyAttributeValue(m_application, kAXWindowsAttribute, &windows) ==
                    kAXErrorSuccess &&
                windows && CFGetTypeID(windows) == CFArrayGetTypeID()) {
                const auto array = static_cast<CFArrayRef>(windows);
                for (CFIndex i = 0; i < CFArrayGetCount(array); ++i) {
                    const auto candidate =
                        static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(array, i));
                    if (!matchingBounds(candidate, m_target.desktopBounds))
                        continue;
                    if (m_window) {
                        CFRelease(m_window);
                        m_window = nullptr;
                        CFRelease(windows);
                        return fail(error);
                    }
                    m_window = static_cast<AXUIElementRef>(CFRetain(candidate));
                }
            }
            if (windows)
                CFRelease(windows);
        }
        if (!m_window)
            return fail(error);
        AXUIElementSetMessagingTimeout(m_window, 0.1F);
        bool minimized = false;
        if (!attributeBoolean(m_window, kAXMinimizedAttribute, &minimized))
            return fail(error);
        m_poll->minimized = minimized;
        if (AXObserverCreate(pid, observed, &m_observer) == kAXErrorSuccess && m_observer) {
            m_notificationsAvailable = true;
            for (CFStringRef notification :
                 {kAXWindowMiniaturizedNotification, kAXWindowDeminiaturizedNotification,
                  kAXUIElementDestroyedNotification})
                m_notificationsAvailable &=
                    AXObserverAddNotification(m_observer, m_window, notification, this) ==
                    kAXErrorSuccess;
            CFRunLoopAddSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(m_observer),
                               kCFRunLoopCommonModes);
        }
        m_poll->window = static_cast<AXUIElementRef>(CFRetain(m_window));
        std::thread([state = m_poll, notifications = m_notificationsAvailable] {
            while (!state->stopping && !state->closed) {
                bool minimized = false;
                AXError error = kAXErrorSuccess;
                state->available =
                    attributeBoolean(state->window, kAXMinimizedAttribute, &minimized, &error);
                if (error == kAXErrorInvalidUIElement)
                    state->closed = true;
                if (state->available)
                    state->minimized = minimized;
                std::unique_lock lock(state->mutex);
                state->changed.wait_for(lock, std::chrono::milliseconds(notifications ? 2000 : 250),
                                        [&] { return state->stopping.load(); });
            }
        }).detach();
        return true;
    }
    live_window_detail::NativeState inspect() override {
        if (m_targetApplication && m_targetApplication.terminated)
            m_poll->closed = true;
        if (m_poll->closed)
            return {LiveWindowState::Closed, {}, {}};
        // WindowServer may temporarily omit minimized or transitioning windows.
        // Native/AX lifecycle state is authoritative; absence is not closure.
        if ((m_localWindow && m_localWindow.miniaturized) ||
            (!m_localWindow && m_window && m_poll->available && m_poll->minimized))
            return {LiveWindowState::Minimized, {}, {}};
        bool exists = false;
        bool querySucceeded = false;
        const QRect bounds =
            windowBounds(static_cast<CGWindowID>(m_target.nativeId),
                         static_cast<pid_t>(m_target.processId), &exists, &querySucceeded);
        if (!querySucceeded || !CGPreflightScreenCaptureAccess())
            return {LiveWindowState::Unavailable,
                    {},
                    nativeText(QT_TRANSLATE_NOOP(
                        "LiveWindowSource",
                        "The window content is temporarily unavailable. Retrying…"))};
        if (!exists || bounds.isEmpty())
            return {LiveWindowState::Unavailable,
                    {},
                    nativeText(QT_TRANSLATE_NOOP(
                        "LiveWindowSource",
                        "The window content is temporarily unavailable. Retrying…"))};
        if (m_localWindow)
            return {LiveWindowState::Live, bounds, {}};
        if (!m_window || !m_poll->available)
            return {
                LiveWindowState::Unavailable, bounds,
                nativeText(QT_TRANSLATE_NOOP(
                    "LiveWindowSource", "The window state is temporarily unavailable. Retrying…"))};
        return {LiveWindowState::Live, bounds, {}};
    }
    bool hasLifecycleNotifications() const override {
        // Local NSWindow state is cheap and still polled until all lifecycle
        // notifications (not just close) are registered for that path.
        return m_notificationsAvailable;
    }

  private:
    bool fail(QString* error) {
        if (error)
            *error = nativeText(QT_TRANSLATE_NOOP(
                "LiveWindowSource",
                "The selected window's identity and state could not be verified."));
        return false;
    }
    static void observed(AXObserverRef, AXUIElementRef, CFStringRef notification, void* data) {
        auto* self = static_cast<MacLiveWindow*>(data);
        if (CFEqual(notification, kAXUIElementDestroyedNotification))
            self->m_poll->closed = true;
        if (CFEqual(notification, kAXWindowMiniaturizedNotification))
            self->m_poll->minimized = true;
        if (CFEqual(notification, kAXWindowDeminiaturizedNotification))
            self->m_poll->minimized = false;
        const auto changed = self->changed;
        if (changed)
            changed();
    }
    LiveWindowTarget m_target;
    NSWindow* m_localWindow = nil;
    NSRunningApplication* m_targetApplication = nil;
    id m_localCloseObserver = nil;
    AXUIElementRef m_application = nullptr;
    AXUIElementRef m_window = nullptr;
    AXObserverRef m_observer = nullptr;
    bool m_notificationsAvailable = false;
    std::shared_ptr<MacWindowPollState> m_poll = std::make_shared<MacWindowPollState>();
};
} // namespace

std::optional<LiveWindowTarget> liveWindowTargetAtPoint(const QPoint& point,
                                                        const QVector<quintptr>& excluded) {
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return std::nullopt;
    std::vector<std::uintptr_t> ids;
    ids.reserve(static_cast<size_t>(excluded.size()));
    for (const auto id : excluded)
        ids.push_back(id);
    const auto id = platform::detail::selectorWindowAtDesktopPoint(point, ids);
    if (!id)
        return std::nullopt;
    CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, id);
    const auto target = platform::detail::recaptureWindowTarget(windows, id);
    QString title;
    if (windows) {
        for (CFIndex index = 0; index < CFArrayGetCount(windows); ++index) {
            const auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, index));
            const auto name = static_cast<CFStringRef>(CFDictionaryGetValue(info, kCGWindowName));
            if (name && CFGetTypeID(name) == CFStringGetTypeID()) {
                title = QString::fromCFString(name);
                break;
            }
        }
    }
    if (windows)
        CFRelease(windows);
    if (!target.id || target.pid <= 0)
        return std::nullopt;
    return LiveWindowTarget{target.id, target.pid,
                            QRect(qRound(target.bounds.origin.x), qRound(target.bounds.origin.y),
                                  qRound(target.bounds.size.width),
                                  qRound(target.bounds.size.height)),
                            point, title};
}
namespace live_window_detail {
std::unique_ptr<NativeWindowMonitor> createNativeWindowMonitor(const LiveWindowTarget& target) {
    return std::make_unique<MacLiveWindow>(target);
}
} // namespace live_window_detail
} // namespace snow_shot::presentation

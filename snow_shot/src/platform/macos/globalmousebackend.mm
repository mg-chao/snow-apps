#include "globalmousebackend_p.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <QDesktopServices>
#include <QGuiApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QUrl>
#include <atomic>
#include <deque>
#include <utility>
#include <vector>

namespace snow_shot::presentation {
namespace {
using Status = GlobalMousePermissionState::Status;
constexpr CGEventMask eventMask =
    CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
    CGEventMaskBit(kCGEventLeftMouseDragged) | CGEventMaskBit(kCGEventRightMouseDown) |
    CGEventMaskBit(kCGEventRightMouseUp) | CGEventMaskBit(kCGEventRightMouseDragged) |
    CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp) |
    CGEventMaskBit(kCGEventOtherMouseDragged) | CGEventMaskBit(kCGEventMouseMoved) |
    CGEventMaskBit(kCGEventFlagsChanged) | CGEventMaskBit(kCGEventKeyDown) |
    CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventScrollWheel);

detail::MacGlobalMouseApi nativeApi() {
    return {[] { return CGPreflightListenEventAccess(); },
            [] { return AXIsProcessTrusted(); },
            [](CGEventMask mask, CGEventTapCallBack callback, void* context) {
                return CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                        kCGEventTapOptionDefault, mask, callback, context);
            },
            [](CFMachPortRef tap, bool enabled) { CGEventTapEnable(tap, enabled); },
            [](CFMachPortRef tap) { return CGEventTapIsEnabled(tap); },
            [] {
                Qt::MouseButtons buttons;
                for (unsigned i = 0; i < 27; ++i) {
                    if (CGEventSourceButtonState(kCGEventSourceStateHIDSystemState,
                                                 static_cast<CGMouseButton>(i)))
                        buttons |= static_cast<Qt::MouseButton>(quint32{1} << i);
                }
                return buttons;
            },
            []() -> std::optional<QPointF> {
                CGEventRef event = CGEventCreate(nullptr);
                if (!event)
                    return std::nullopt;
                const CGPoint point = CGEventGetLocation(event);
                CFRelease(event);
                return QPointF(point.x, point.y);
            },
            [] {
                // Request one missing permission at a time, only after a user action.
                if (!CGPreflightListenEventAccess()) {
                    static_cast<void>(CGRequestListenEventAccess());
                } else if (!AXIsProcessTrusted()) {
                    const void* keys[] = {kAXTrustedCheckOptionPrompt};
                    const void* values[] = {kCFBooleanTrue};
                    CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault, keys, values,
                                                                 1, &kCFTypeDictionaryKeyCallBacks,
                                                                 &kCFTypeDictionaryValueCallBacks);
                    static_cast<void>(AXIsProcessTrustedWithOptions(options));
                    if (options)
                        CFRelease(options);
                }
            },
            [](bool listen) {
                QDesktopServices::openUrl(
                    QUrl(listen ? QStringLiteral("x-apple.systempreferences:com.apple.preference."
                                                 "security?Privacy_ListenEvent")
                                : QStringLiteral("x-apple.systempreferences:com.apple.preference."
                                                 "security?Privacy_Accessibility")));
            },
            [] {
                CFDictionaryRef session = CGSessionCopyCurrentDictionary();
                if (!session)
                    return false;
                const bool active =
                    CFDictionaryGetValue(session, kCGSessionOnConsoleKey) == kCFBooleanTrue;
                CFRelease(session);
                return active;
            },
            [] { return CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState); },
            [] { return CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, 53); }};
}

class MacOSGlobalMouseBackend final : public QObject, public GlobalMouseBackend {
  public:
    explicit MacOSGlobalMouseBackend(detail::MacGlobalMouseApi native, bool observeSession)
        : api(std::move(native)), nativeSession(observeSession) {
        if (!nativeSession)
            return;
        auto* workspaceCenter = NSWorkspace.sharedWorkspace.notificationCenter;
        for (NSNotificationName name :
             {NSWorkspaceWillSleepNotification, NSWorkspaceSessionDidResignActiveNotification}) {
            observers.push_back([workspaceCenter addObserverForName:name
                                                             object:nil
                                                              queue:NSOperationQueue.mainQueue
                                                         usingBlock:^(NSNotification*) {
                                                           QMetaObject::invokeMethod(
                                                               this,
                                                               [this] {
                                                                   suspended = true;
                                                                   refreshPermission();
                                                               },
                                                               Qt::QueuedConnection);
                                                         }]);
        }
        for (NSNotificationName name :
             {NSWorkspaceDidWakeNotification, NSWorkspaceSessionDidBecomeActiveNotification}) {
            observers.push_back([workspaceCenter addObserverForName:name
                                                             object:nil
                                                              queue:NSOperationQueue.mainQueue
                                                         usingBlock:^(NSNotification*) {
                                                           QMetaObject::invokeMethod(
                                                               this,
                                                               [this] {
                                                                   suspended = false;
                                                                   refreshPermission();
                                                               },
                                                               Qt::QueuedConnection);
                                                         }]);
        }
        displayObserver = [NSNotificationCenter.defaultCenter
            addObserverForName:NSApplicationDidChangeScreenParametersNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification*) {
                      QMetaObject::invokeMethod(
                          this, [this] { submit([this] { interrupt(); }); }, Qt::QueuedConnection);
                    }];
    }
    ~MacOSGlobalMouseBackend() override {
        for (id observer : observers)
            [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:observer];
        if (displayObserver)
            [NSNotificationCenter.defaultCenter removeObserver:displayObserver];
        stop();
    }

    void setStateHandler(StateHandler handler) override {
        QMutexLocker lock(&mutex);
        stateHandler = std::move(handler);
    }
    GlobalMousePermissionState permissionState() const override {
        QMutexLocker lock(&mutex);
        return state;
    }
    void start(Handler handler, FailureHandler failure) override {
        if (thread && thread->isRunning())
            return;
        if (thread) {
            thread->wait();
            thread.reset();
        }
        onEvent = std::move(handler);
        Q_UNUSED(failure);
        if (nativeSession && QGuiApplication::platformName() != u"cocoa") {
            publish({Status::Unavailable});
            return;
        }
        stopping = false;
        thread.reset(QThread::create([this] { run(); }));
        thread->start();
    }
    void stop() override {
        stopping = true;
        wake();
        if (thread && QThread::currentThread() == thread.get())
            return;
        if (thread) {
            thread->wait();
            thread.reset();
        }
        QMutexLocker lock(&mutex);
        commands.clear();
        onEvent = {};
    }
    void configure(const GlobalMouseConfiguration& value) override {
        QMutexLocker lock(&mutex);
        desired = value;
        if (loop) {
            CFRunLoopSourceSignal(commandWake);
            CFRunLoopWakeUp(loop);
        }
    }
    void cancel(quint64 id) override {
        submit([this, id] { input.gesture.cancel(id); });
    }
    void beginButtonDrag(settings::SettingsGlobalMouseAction action) override {
        // Sample at the GUI press, not after queued work may see a release.
        if (!thread || !thread->isRunning() || !api.buttons().testFlag(Qt::LeftButton))
            return;
        const auto startPosition = api.cursor();
        if (!startPosition)
            return;
        submit([this, action, startPosition] {
            if (!tap || !configuration.captureAvailable || input.gesture.pending())
                return;
            const auto result = input.gesture.beginButtonDrag(action, *startPosition);
            if (!result.event)
                return;
            input.lastPosition = *startPosition;
            emitEvent(result.event);
            if (!api.buttons().testFlag(Qt::LeftButton)) {
                const auto end = api.cursor().value_or(*startPosition);
                emitEvent(input.gesture
                              .handle({GlobalMouseInput::Kind::Release, end, Qt::LeftButton},
                                      configuration)
                              .event);
            }
        });
    }
    void refreshPermission() override {
        submit([this] { reconcile(); });
    }
    void requestPermission() override {
        api.requestAccess();
        refreshPermission();
    }
    void openPermissionSettings() override {
        api.openSettings(!api.listenAccess());
    }

  private:
    void emitEvent(const std::optional<GlobalMouseDragEvent>& event) {
        if (event && onEvent) {
            auto translated = *event;
            translated.coordinateSpace = GlobalMouseCoordinateSpace::DesktopPoints;
            onEvent(translated);
        }
    }
    void interrupt() {
        emitEvent(input.interrupt());
    }
    void recoverInputState() {
        // Sampling is restricted to transport recovery, never the event hot path.
        input.resynchronize(api.buttons(), api.modifierFlags ? api.modifierFlags() : 0,
                            api.escapeDown && api.escapeDown());
    }
    void publish(GlobalMousePermissionState value) {
        StateHandler handler;
        {
            QMutexLocker lock(&mutex);
            if (state == value)
                return;
            state = value;
            handler = stateHandler;
        }
        if (handler)
            handler(value);
    }
    void wake() {
        QMutexLocker lock(&mutex);
        if (loop) {
            CFRunLoopSourceSignal(commandWake);
            CFRunLoopWakeUp(loop);
        }
    }
    void submit(std::function<void()> command) {
        QMutexLocker lock(&mutex);
        if (stopping)
            return;
        commands.push_back(std::move(command));
        if (loop) {
            CFRunLoopSourceSignal(commandWake);
            CFRunLoopWakeUp(loop);
        }
    }
    void retireTap() {
        if (source) {
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
            CFRelease(source);
            source = nullptr;
        }
        if (tap) {
            api.enableTap(tap, false);
            CFMachPortInvalidate(tap);
            CFRelease(tap);
            tap = nullptr;
        }
        input.reset();
    }
    void reconcile() {
        const bool listen = api.listenAccess();
        const bool accessibility = api.accessibilityAccess();
        const bool inactive = suspended || (api.sessionActive && !api.sessionActive());
        if (inactive || !listen || !accessibility) {
            interrupt();
            retireTap();
            publish({inactive  ? Status::Suspended
                     : !listen ? Status::ListenRequired
                               : Status::AccessibilityRequired,
                     listen, accessibility, false});
            return;
        }
        if (!tap) {
            tap = api.createTap(eventMask, callback, this);
            if (tap)
                source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
            if (!source) {
                retireTap();
                publish({Status::Unavailable, listen, accessibility, false});
                return;
            }
            input.heldButtons = api.buttons();
            if (const auto cursor = api.cursor())
                input.lastPosition = *cursor;
            CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
            api.enableTap(tap, true);
        }
        if (!api.tapEnabled(tap)) {
            interrupt();
            recoverInputState();
            api.enableTap(tap, true);
            if (!api.tapEnabled(tap)) {
                retireTap();
                publish({Status::Unavailable, listen, accessibility, false});
                return;
            }
        }
        publish({Status::Ready, listen, accessibility, true});
    }
    void run() {
        @autoreleasepool {
            // A source keeps the run loop asleep even when permission prevents
            // tap creation. Wakeups process commands; the one-second deadline
            // rechecks TCC without polling pointer movement.
            CFRunLoopSourceContext context{};
            context.perform = [](void*) {};
            CFRunLoopSourceRef commandSource = CFRunLoopSourceCreate(nullptr, 0, &context);
            CFRunLoopRef current = CFRunLoopGetCurrent();
            CFRunLoopAddSource(current, commandSource, kCFRunLoopCommonModes);
            {
                QMutexLocker lock(&mutex);
                loop = current;
                commandWake = commandSource;
                configuration = desired;
            }
            reconcile();
            auto nextCheck = CFAbsoluteTimeGetCurrent() + 1.0;
            while (!stopping) {
                std::deque<std::function<void()>> batch;
                {
                    QMutexLocker lock(&mutex);
                    configuration = desired;
                    batch.swap(commands);
                }
                for (auto& command : batch) {
                    if (stopping)
                        break;
                    command();
                }
                const auto now = CFAbsoluteTimeGetCurrent();
                if (now >= nextCheck) {
                    reconcile();
                    nextCheck = now + 1.0;
                }
                if (!stopping)
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
            }
            interrupt();
            retireTap();
            publish({Status::Unknown});
            {
                QMutexLocker lock(&mutex);
                loop = nullptr;
                commandWake = nullptr;
                commands.clear();
            }
            CFRunLoopRemoveSource(current, commandSource, kCFRunLoopCommonModes);
            CFRelease(commandSource);
        }
    }
    static CGEventRef callback(CGEventTapProxy, CGEventType type, CGEventRef event, void* context) {
        auto& self = *static_cast<MacOSGlobalMouseBackend*>(context);
        // Disabled notifications do not necessarily carry a CGEvent.
        if (self.stopping)
            return event;
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            self.interrupt();
            self.recoverInputState();
            if (self.tap)
                self.api.enableTap(self.tap, true);
            self.reconcile();
            return event;
        }
        if (self.stopping || !event)
            return event;
        const auto result = self.input.handle(type, event, self.configuration);
        self.emitEvent(result.event);
        return result.consumed ? nullptr : event;
    }

    detail::MacGlobalMouseApi api;
    const bool nativeSession;
    std::vector<id> observers;
    id displayObserver = nil;
    mutable QMutex mutex;
    GlobalMousePermissionState state;
    StateHandler stateHandler;
    GlobalMouseConfiguration desired;
    std::deque<std::function<void()>> commands;
    CFRunLoopRef loop = nullptr;              // Protected by mutex, valid until the worker exits.
    CFRunLoopSourceRef commandWake = nullptr; // Protected by mutex.
    std::unique_ptr<QThread> thread;
    std::atomic_bool stopping{true};
    Handler onEvent;

    // Everything below is confined to the event-tap thread.
    GlobalMouseConfiguration configuration;
    detail::MacGlobalMouseInput input;
    std::atomic_bool suspended{false};
    CFMachPortRef tap = nullptr;
    CFRunLoopSourceRef source = nullptr;
};
} // namespace

detail::MacGlobalMouseApi detail::nativeMacGlobalMouseApi() {
    return nativeApi();
}

std::unique_ptr<GlobalMouseBackend> detail::createMacGlobalMouseBackend(MacGlobalMouseApi api) {
    return std::make_unique<MacOSGlobalMouseBackend>(std::move(api), false);
}
std::unique_ptr<GlobalMouseBackend> createMacOSGlobalMouseBackend() {
    return std::make_unique<MacOSGlobalMouseBackend>(nativeApi(), true);
}
} // namespace snow_shot::presentation

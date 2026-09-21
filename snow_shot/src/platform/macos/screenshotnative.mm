#include "snow_shot/platform/screenshotnative.h"
#include "screenshotwindowtarget_p.h"
#include "screenshotinputregion_p.h"
#include <QCursor>
#include <QTimer>
#include "snow_shot/platform/macos/recapturefocus.h"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>
#import <objc/runtime.h>
#include <algorithm>
#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QSet>
#include <QScopedValueRollback>
#include <QEvent>
#include <QPointer>
#include <QVariant>
#include <QWindow>
#include <QWidget>

namespace {
struct ScreenshotNativeSettings {
    NSInteger level;
    NSWindowCollectionBehavior collectionBehavior;
    BOOL hidesOnDeactivate;
};
} // namespace

// Qt can rewrite native settings during show and modal cleanup. Keep its requested
// settings separate from capture's requirements, for the lifetime of this NSWindow.
// A pooled QWidget may outlive multiple native surfaces with different defaults.
@interface SnowScreenshotWindowPolicy : NSObject {
  @public
    ScreenshotNativeSettings required;
    ScreenshotNativeSettings requested;
    bool applying;
}
@end
@implementation SnowScreenshotWindowPolicy
@end

namespace snow_shot::platform {
namespace {
constexpr auto kScreenshotLayer = "snowScreenshotWindowLayer";
constexpr int kOverlayLayer = 0;
constexpr int kRecognitionLayer = 1;
constexpr int kToolbarLayer = 2;
constexpr int kPopupLayer = 3;

char nativePolicyKey;

SnowScreenshotWindowPolicy* nativePolicyState(NSWindow* window) {
    return static_cast<SnowScreenshotWindowPolicy*>(
        objc_getAssociatedObject(window, &nativePolicyKey));
}

template <typename Value>
void installNativeSettingPolicy(Class windowClass, SEL selector,
                                Value ScreenshotNativeSettings::* setting,
                                Value (^readSetting)(NSWindow*)) {
    const Method method = class_getInstanceMethod(windowClass, selector);
    const IMP originalSetter = method_getImplementation(method);
    const IMP setter = imp_implementationWithBlock(^(NSWindow* receiver, Value requested) {
      if (auto* state = nativePolicyState(receiver)) {
          if (!state->applying)
              state->requested.*setting = requested;
          requested = state->required.*setting;
          if (readSetting(receiver) == requested)
              return;
      }
      reinterpret_cast<void (*)(id, SEL, Value)>(originalSetter)(receiver, selector, requested);
    });
    // Unmanaged windows forward to the original setter with the original value.
    class_replaceMethod(windowClass, selector, setter, method_getTypeEncoding(method));
}

void attachNativeWindowPolicy(NSWindow* window) {
    // Preserve the concrete Qt window class and AppKit's KVO bookkeeping. Changing
    // an existing NSWindow's isa invalidates its forwarded _windowLayerContext
    // observation and raises NSRangeException when the autorelease pool drains.
    // -class also resolves through any KVO-generated notifying subclass.
    Class windowClass = [window class];
    static QSet<Class> installedClasses;
    if (installedClasses.contains(windowClass))
        return;
    // Replace/add only on this concrete Qt class, never on its AppKit superclass.
    installNativeSettingPolicy(windowClass, @selector(setLevel:), &ScreenshotNativeSettings::level,
                               ^NSInteger(NSWindow* receiver) {
                                 return receiver.level;
                               });
    installNativeSettingPolicy(windowClass, @selector(setCollectionBehavior:),
                               &ScreenshotNativeSettings::collectionBehavior,
                               ^NSWindowCollectionBehavior(NSWindow* receiver) {
                                 return receiver.collectionBehavior;
                               });
    installNativeSettingPolicy(windowClass, @selector(setHidesOnDeactivate:),
                               &ScreenshotNativeSettings::hidesOnDeactivate,
                               ^BOOL(NSWindow* receiver) {
                                 return receiver.hidesOnDeactivate;
                               });
    installedClasses.insert(windowClass);
}

void applyNativeSettings(NSWindow* window, const ScreenshotNativeSettings& settings) {
    if (window.level != settings.level)
        window.level = settings.level;
    if (window.collectionBehavior != settings.collectionBehavior)
        window.collectionBehavior = settings.collectionBehavior;
    if (window.hidesOnDeactivate != settings.hidesOnDeactivate)
        window.hidesOnDeactivate = settings.hidesOnDeactivate;
}

void applyNativeWindowPolicy(NSWindow* window, NSInteger level, bool enforceQtSettings = true) {
    if (!window)
        return;
    auto* state = nativePolicyState(window);
    if (!state) {
        state = [SnowScreenshotWindowPolicy new];
        state->requested = {window.level, window.collectionBehavior, window.hidesOnDeactivate};
        objc_setAssociatedObject(window, &nativePolicyKey, state,
                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [state release];
        if (enforceQtSettings)
            attachNativeWindowPolicy(window);
    }
    state->required = {level,
                       NSWindowCollectionBehaviorCanJoinAllSpaces |
                           NSWindowCollectionBehaviorFullScreenAuxiliary,
                       NO};
    const QScopedValueRollback guard(state->applying, true);
    applyNativeSettings(window, state->required);
}

void releaseNativeWindowPolicy(NSWindow* window) {
    if (auto* state = nativePolicyState(window)) {
        const ScreenshotNativeSettings requested = state->requested;
        // Remove enforcement before restoring all settings, including Space membership
        // and deactivation behavior. Subsequent ownership starts with a fresh snapshot.
        objc_setAssociatedObject(window, &nativePolicyKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        applyNativeSettings(window, requested);
    }
}

void synchronizeScreenshotLayers();

// Roles belong to Qt windows, not NSWindows: pooled native surfaces can be destroyed
// and recreated. Unmarked transient windows (including AdQt Tool popups) belong
// above both the overlay and its drawing toolbar.
int screenshotLayer(QWindow* window, int modalFloor = 0) {
    if (!window)
        return -1;
    const QVariant role = window->property(kScreenshotLayer);
    if (role.isValid())
        return role.toInt();
    const int ownerLayer = screenshotLayer(window->transientParent(), modalFloor);
    if (ownerLayer < 0)
        return -1;
    const int layer = std::max(kPopupLayer, ownerLayer + 1);
    return window->modality() == Qt::NonModal ? layer : std::max(layer, modalFloor);
}

void applyScreenshotLayer(QWidget* widget, int modalFloor) {
    if (!widget || !widget->isWindow() || !widget->internalWinId())
        return;
    QWindow* handle = widget->windowHandle();
    if (!handle)
        return;
    // Observe ordinary surfaces too: a visible pooled tool can enter capture by
    // changing only its transient owner, without another show or expose event.
    if (!handle->property("snowScreenshotStackingObserved").toBool()) {
        handle->setProperty("snowScreenshotStackingObserved", true);
        QObject::connect(handle, &QWindow::transientParentChanged, handle,
                         &synchronizeScreenshotLayers);
    }
    const QVariant role = widget->property(kScreenshotLayer);
    if (role.isValid())
        handle->setProperty(kScreenshotLayer, role);
    int layer = screenshotLayer(handle, modalFloor);
    if (layer < 0 && widget->parentWidget()) {
        const int ownerLayer =
            screenshotLayer(widget->parentWidget()->window()->windowHandle(), modalFloor);
        if (ownerLayer >= 0)
            layer = std::max(kPopupLayer, ownerLayer + 1);
    }
    NSWindow* window = reinterpret_cast<NSView*>(widget->internalWinId()).window;
    if (layer < 0) {
        releaseNativeWindowPolicy(window);
        return;
    }
    if (widget->windowModality() != Qt::NonModal)
        layer = std::max(layer, modalFloor);
    const NSInteger level = CGWindowLevelForKey(kCGScreenSaverWindowLevelKey) + layer;
    applyNativeWindowPolicy(window, level);
}

void synchronizeScreenshotLayers() {
    static bool synchronizing = false;
    if (synchronizing)
        return;
    const QScopedValueRollback guard(synchronizing, true);
    const auto windows = QApplication::topLevelWidgets();
    // Restore explicit roles before resolving transient descendants. QWidget
    // retains its role when a pooled QWindow/native surface is recreated.
    for (QWidget* widget : windows) {
        const QVariant role = widget->property(kScreenshotLayer);
        if (role.isValid() && widget->windowHandle())
            widget->windowHandle()->setProperty(kScreenshotLayer, role);
    }
    // A selection modal and an OCR result can be siblings of the same overlay.
    // Transient depth alone cannot order them. Put modals above every visible
    // non-modal screenshot surface, regardless of its popup nesting depth.
    int modalFloor = kPopupLayer;
    for (QWidget* widget : windows) {
        if (!widget->isVisible())
            continue;
        QWindow* handle = widget->windowHandle();
        bool belongsToModal = false;
        for (QWindow* owner = handle; owner; owner = owner->transientParent())
            belongsToModal |= owner->modality() != Qt::NonModal;
        if (!belongsToModal)
            modalFloor = std::max(modalFloor, screenshotLayer(handle) + 1);
    }
    NSInteger panelLevel = 0;
    for (QWidget* widget : windows) {
        applyScreenshotLayer(widget, modalFloor);
        if (widget->isVisible() && widget->internalWinId()) {
            NSWindow* native = reinterpret_cast<NSView*>(widget->internalWinId()).window;
            if (nativePolicyState(native))
                panelLevel = std::max(panelLevel, native.level + 1);
        }
    }
    // QFileDialog's Cocoa helper presents NSSavePanel/NSOpenPanel without a
    // QWidget native surface or Qt transient parent. Include them explicitly,
    // above even the deepest screenshot modal. Do not swizzle AppKit classes.
    for (NSWindow* window in NSApp.windows) {
        if (![window isKindOfClass:[NSSavePanel class]])
            continue;
        if (panelLevel && window.visible)
            applyNativeWindowPolicy(window, panelLevel, false);
        else
            releaseNativeWindowPolicy(window);
    }
}

class ScreenshotStackingPolicy final : public QObject {
  public:
    explicit ScreenshotStackingPolicy(QObject* parent) : QObject(parent) {
        // Cocoa ends Qt's native modal sessions after the dialog's finished/hide
        // callbacks. Restore the owner only once that cleanup has reached idle.
        connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, this,
                [this] {
                    if (!m_focusOwner)
                        return;
                    QWindow* owner = m_focusOwner;
                    m_focusOwner.clear();
                    if (!owner->isVisible() || screenshotLayer(owner) < 0)
                        return;
                    if (QWindow* modal = QGuiApplication::modalWindow(); modal && modal != owner)
                        return;
                    owner->requestActivate();
                });
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        // Native file panels bypass Qt show/expose events, including in exec().
        m_panelShown =
            [center addObserverForName:NSWindowDidBecomeKeyNotification
                                object:nil
                                 queue:nil
                            usingBlock:^(NSNotification* notification) {
                              if ([notification.object isKindOfClass:[NSSavePanel class]])
                                  synchronizeScreenshotLayers();
                            }];
        m_panelClosed = [center
            addObserverForName:NSWindowWillCloseNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* notification) {
                      if ([notification.object isKindOfClass:[NSSavePanel class]])
                          releaseNativeWindowPolicy(static_cast<NSWindow*>(notification.object));
                    }];
    }

    ~ScreenshotStackingPolicy() override {
        [NSNotificationCenter.defaultCenter removeObserver:m_panelShown];
        [NSNotificationCenter.defaultCenter removeObserver:m_panelClosed];
        for (NSWindow* window in NSApp.windows)
            if ([window isKindOfClass:[NSSavePanel class]])
                releaseNativeWindowPolicy(window);
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        // ShowToParent runs after QWidget's native show, which can reset the level.
        // Expose also covers QWindow-driven reveals and native surface recreation.
        if (event->type() == QEvent::HideToParent) {
            auto* widget = qobject_cast<QWidget*>(watched);
            if (widget && widget->isWindow() && widget->windowModality() != Qt::NonModal) {
                QWindow* handle = widget->windowHandle();
                QWindow* owner = handle ? handle->transientParent() : nullptr;
                if (owner && screenshotLayer(owner) >= 0)
                    m_focusOwner = owner;
            }
        }
        if (event->type() == QEvent::ShowToParent || event->type() == QEvent::ZOrderChange ||
            event->type() == QEvent::HideToParent || event->type() == QEvent::ParentChange ||
            event->type() == QEvent::Expose || event->type() == QEvent::ApplicationActivate) {
            synchronizeScreenshotLayers();
        }
        return false;
    }

  private:
    QPointer<QWindow> m_focusOwner;
    id m_panelShown = nil;
    id m_panelClosed = nil;
};

void registerScreenshotLayer(QWidget* widget, int layer) {
    if (!widget || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    static QPointer<ScreenshotStackingPolicy> policy;
    if (!policy) {
        policy = new ScreenshotStackingPolicy(qApp);
        qApp->installEventFilter(policy);
    }
    widget->setProperty(kScreenshotLayer, layer);
    static_cast<void>(widget->winId());
    applyScreenshotLayer(widget, kPopupLayer);
    // A toolbar or popup may have been materialized before the overlay was shown.
    synchronizeScreenshotLayers();
}

// Custom Qt drags must be the only owner of window movement. AppKit's
// server-side drag loop otherwise applies its own screen-relative frame change
// after QWidget::move(), causing a second translation at display boundaries.
class ControlledWindowDragging final : public QObject {
  public:
    explicit ControlledWindowDragging(QWidget* widget) : QObject(widget), m_widget(widget) {
        widget->installEventFilter(this);
        apply();
    }

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::WinIdChange || event->type() == QEvent::Show)
            apply();
        return false;
    }

  private:
    void apply() {
        if (!m_widget->internalWinId())
            return;
        NSWindow* window = reinterpret_cast<NSView*>(m_widget->internalWinId()).window;
        window.movable = NO;
        window.movableByWindowBackground = NO;
    }
    QWidget* m_widget;
};

// AppKit's per-window ignoresMouseEvents controls WindowServer routing. A Qt
// mask only clips the view; rejecting an event there cannot deliver it to another
// application's window. Observe pointer movement on both sides of the hole so
// native routing is already correct when the next wheel/trackpad event arrives.
class ScreenshotInputPassThrough final : public QObject {
  public:
    explicit ScreenshotInputPassThrough(QWidget* widget) : QObject(widget), m_widget(widget) {
        widget->installEventFilter(this);
    }
    ~ScreenshotInputPassThrough() override {
        stopMonitoring();
        restore();
    }

    void setRegion(const QRegion& region) {
        m_region.passThrough = region;
        if (region.isEmpty()) {
            stopMonitoring();
            m_region.heldButtons = 0;
        } else if (!m_localMonitor) {
            constexpr NSEventMask events =
                NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged |
                NSEventMaskOtherMouseDragged | NSEventMaskLeftMouseDown |
                NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown | NSEventMaskLeftMouseUp |
                NSEventMaskRightMouseUp | NSEventMaskOtherMouseUp;
            m_localMonitor =
                [NSEvent addLocalMonitorForEventsMatchingMask:events
                                                      handler:^NSEvent*(NSEvent* event) {
                                                        handlePointerEvent(event, true);
                                                        return event;
                                                      }];
            // Mouse monitoring requires no Accessibility/Input Monitoring permission.
            // Once transparent, movement is delivered to the application underneath.
            m_globalMonitor =
                [NSEvent addGlobalMonitorForEventsMatchingMask:events
                                                       handler:^(NSEvent* event) {
                                                         handlePointerEvent(event, false);
                                                       }];
        }
        synchronize(); // Also handles activation with a stationary pointer.
    }

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        switch (event->type()) {
        case QEvent::Hide:
            m_region.heldButtons = 0;
            restore();
            break;
        case QEvent::Show:
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::WinIdChange:
            synchronize();
            break;
        default:
            break;
        }
        return false;
    }

  private:
    void handlePointerEvent(NSEvent* event, bool local) {
        const bool down = event.type == NSEventTypeLeftMouseDown ||
                          event.type == NSEventTypeRightMouseDown ||
                          event.type == NSEventTypeOtherMouseDown;
        const bool up = event.type == NSEventTypeLeftMouseUp ||
                        event.type == NSEventTypeRightMouseUp ||
                        event.type == NSEventTypeOtherMouseUp;
        if (down && local && event.window == nativeWindow())
            m_region.press(static_cast<unsigned>(event.buttonNumber));
        if (up) {
            m_region.release(static_cast<unsigned>(event.buttonNumber));
            // Let Qt dispatch the release before making its NSWindow transparent.
            QTimer::singleShot(0, this, [this] { synchronize(); });
            return;
        }
        synchronize();
    }

    NSWindow* nativeWindow() const {
        if (!m_widget || !m_widget->internalWinId())
            return nil;
        return reinterpret_cast<NSView*>(m_widget->internalWinId()).window;
    }

    void synchronize() {
        if (m_region.passThrough.isEmpty()) {
            restore();
            return;
        }
        NSWindow* window = nativeWindow();
        if (m_native != window) {
            restore();
            m_native = window;
            m_originalIgnoresMouse = window.ignoresMouseEvents;
        }
        if (!window)
            return;
        const bool transparent =
            m_region.transparentAt(m_widget->mapFromGlobal(QCursor::pos()), m_widget->isVisible());
        window.ignoresMouseEvents = transparent || m_originalIgnoresMouse;
    }

    void restore() {
        if (m_native && m_native == nativeWindow())
            m_native.ignoresMouseEvents = m_originalIgnoresMouse;
        m_native = nil;
    }

    void stopMonitoring() {
        if (m_localMonitor)
            [NSEvent removeMonitor:m_localMonitor];
        if (m_globalMonitor)
            [NSEvent removeMonitor:m_globalMonitor];
        m_localMonitor = nil;
        m_globalMonitor = nil;
    }

    QPointer<QWidget> m_widget;
    detail::ScreenshotInputRegion m_region;
    NSWindow* m_native = nil;
    BOOL m_originalIgnoresMouse = NO;
    id m_localMonitor = nil;
    id m_globalMonitor = nil;
};

detail::WindowTarget windowTarget(pid_t owner, const QPoint* point) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    const auto result = point ? detail::scrollWindowTarget(
                                    windows, NSProcessInfo.processInfo.processIdentifier, *point)
                              : detail::focusedWindowTarget(windows, owner);
    if (windows)
        CFRelease(windows);
    return result;
}
} // namespace
void configureControlledWindowDragging(QWidget* widget) {
    if (!widget || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    for (QObject* child : widget->children()) {
        if (dynamic_cast<ControlledWindowDragging*>(child))
            return;
    }
    new ControlledWindowDragging(widget);
}

void configureScreenshotOverlayWindow(QWidget* widget) {
    configureControlledWindowDragging(widget);
    registerScreenshotLayer(widget, kOverlayLayer);
}

void configureScreenshotRecognitionWindow(QWidget* widget) {
    registerScreenshotLayer(widget, kRecognitionLayer);
}

void setScreenshotInputPassThroughRegion(QWidget* widget, const QRegion& region) {
    if (!widget || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    ScreenshotInputPassThrough* policy = nullptr;
    for (QObject* child : widget->children()) {
        if ((policy = dynamic_cast<ScreenshotInputPassThrough*>(child)))
            break;
    }
    if (!policy && !region.isEmpty())
        policy = new ScreenshotInputPassThrough(widget);
    if (policy)
        policy->setRegion(region);
}

void configureScreenshotToolbarWindow(QWidget* widget) {
    configureControlledWindowDragging(widget);
    registerScreenshotLayer(widget, kToolbarLayer);
}

quint32 screenshotDisplayAtCursor() {
    CGEventRef event = CGEventCreate(nullptr);
    if (!event)
        return 0;
    const CGPoint point = CGEventGetLocation(event);
    CFRelease(event);
    CGDirectDisplayID display = 0;
    uint32_t count = 0;
    return CGGetDisplaysWithPoint(point, 1, &display, &count) == kCGErrorSuccess && count ? display
                                                                                          : 0;
}
quint32 screenshotFocusedWindow() {
    const pid_t pid = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    return pid > 0 ? windowTarget(pid, nullptr).id : 0;
}
bool screenshotScrollPermission() {
    return AXIsProcessTrusted();
}
ScrollInputResult sendScreenshotScroll(const QRect& selection, const QPoint& delta) {
    if (selection.isEmpty() || delta.isNull())
        return {ScrollInputResult::Status::InvalidRequest, 0};
    if (!screenshotScrollPermission())
        return {ScrollInputResult::Status::PostFailed, 1};
    const QPoint center = selection.center();
    const detail::WindowTarget target = windowTarget(0, &center);
    if (!target.id)
        return {ScrollInputResult::Status::TargetNotFound, 0};

    CGEventRef event =
        CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 2, delta.y(), -delta.x());
    if (!event)
        return {ScrollInputResult::Status::PostFailed, 0};
    NSEvent* associated = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                             location:NSZeroPoint
                                        modifierFlags:0
                                            timestamp:NSProcessInfo.processInfo.systemUptime
                                         windowNumber:target.id
                                              context:nil
                                          eventNumber:0
                                           clickCount:0
                                             pressure:0];
    CGEventRef routed = associated.CGEvent ? CGEventCreateCopy(associated.CGEvent) : nullptr;
    if (!routed) {
        CFRelease(event);
        return {ScrollInputResult::Status::PostFailed, 0};
    }
    CGEventSetType(routed, kCGEventScrollWheel);
    for (const auto field : {kCGScrollWheelEventDeltaAxis1, kCGScrollWheelEventDeltaAxis2,
                             kCGScrollWheelEventPointDeltaAxis1, kCGScrollWheelEventPointDeltaAxis2,
                             kCGScrollWheelEventIsContinuous})
        CGEventSetIntegerValueField(routed, field, CGEventGetIntegerValueField(event, field));
    for (const auto field :
         {kCGScrollWheelEventFixedPtDeltaAxis1, kCGScrollWheelEventFixedPtDeltaAxis2})
        CGEventSetDoubleValueField(routed, field, CGEventGetDoubleValueField(event, field));
    // Posting directly to a PID bypasses WindowServer's coordinate conversion.
    // CoreGraphics exports this bridge but omits it from the public SDK. Resolve
    // it at runtime: if unavailable, fail without warping the user's pointer.
    using SetWindowLocation = void (*)(CGEventRef, CGPoint);
    static const auto setWindowLocation =
        reinterpret_cast<SetWindowLocation>(dlsym(RTLD_DEFAULT, "CGEventSetWindowLocation"));
    if (!setWindowLocation) {
        CFRelease(routed);
        CFRelease(event);
        return {ScrollInputResult::Status::PostFailed, 0};
    }
    CGEventSetLocation(routed, CGPointMake(center.x(), center.y()));
    setWindowLocation(routed, CGPointMake(center.x() - target.bounds.origin.x,
                                          center.y() - target.bounds.origin.y));
    CGEventPostToPid(target.pid, routed);
    CFRelease(routed);
    CFRelease(event);
    return {ScrollInputResult::Status::Posted, 0};
}
} // namespace snow_shot::platform

namespace snow_shot::platform::macos {
namespace {
struct RecaptureNativeState {
    struct Surface {
        NSWindow* native;
        BOOL ignoredMouse;
        QPointer<QWindow> qtWindow;
        bool transparentForInput;
    };
    QVector<Surface> surfaces;
    QVector<CGWindowID> excludedWindowIds;
    NSWindow* localTarget = nil;
    AXUIElementRef application = nullptr;
    AXUIElementRef targetWindow = nullptr;
    NSRunningApplication* targetApp = nil;
    CGWindowID targetId = 0;
    bool desktop = false;
    CFMachPortRef mouseTap = nullptr;
    CFRunLoopSourceRef mouseSource = nullptr;
    bool mouseDelivered = false;

    ~RecaptureNativeState() {
        for (const auto& surface : surfaces)
            [surface.native release];
        if (targetWindow)
            CFRelease(targetWindow);
        if (application)
            CFRelease(application);
        [targetApp release];
        [localTarget release];
    }

    detail::WindowTarget targetAt(CGPoint location) const {
        const NSPoint point =
            NSMakePoint(location.x, NSMaxY(NSScreen.screens.firstObject.frame) - location.y);
        const CGWindowID hit = detail::recaptureWindowAtPoint(
            {excludedWindowIds.constData(), static_cast<size_t>(excludedWindowIds.size())},
            [point](CGWindowID below) {
                return static_cast<CGWindowID>([NSWindow windowNumberAtPoint:point
                                                 belowWindowWithWindowNumber:below]);
            });
        if (!hit)
            return {};
        CFArrayRef windows = CGWindowListCopyWindowInfo(
            kCGWindowListOptionIncludingWindow | kCGWindowListExcludeDesktopElements, hit);
        const auto target = detail::recaptureWindowTarget(windows, hit);
        if (windows)
            CFRelease(windows);
        return target;
    }

    bool begin(const QVector<QWidget*>& windows) {
        // Use the same exact surfaces for input transparency and hit-test exclusions.
        // Retain them before any activation can recreate a Qt native surface.
        for (QWidget* widget : windows) {
            NSWindow* native = reinterpret_cast<NSView*>(widget->winId()).window;
            if (!native)
                return false;
            QWindow* handle = widget->windowHandle();
            surfaces.push_back({[native retain],
                                native.ignoresMouseEvents,
                                handle,
                                handle->flags().testFlag(Qt::WindowTransparentForInput)});
            excludedWindowIds.push_back(static_cast<CGWindowID>(native.windowNumber));
        }
        CGEventRef position = CGEventCreate(nullptr);
        if (!position)
            return false;
        const CGPoint location = CGEventGetLocation(position);
        CFRelease(position);
        const auto target = targetAt(location);
        targetId = target.id;
        desktop = !target.id;
        if (!desktop) {
            targetApp =
                [[NSRunningApplication runningApplicationWithProcessIdentifier:target.pid] retain];
            if (!targetApp)
                return false;
            if (target.pid == NSProcessInfo.processInfo.processIdentifier) {
                localTarget = [[NSApp windowWithWindowNumber:target.id] retain];
                if (!localTarget || !localTarget.visible || localTarget.ignoresMouseEvents)
                    return false;
            } else if (!resolveExternalTarget(location, target.pid)) {
                return false;
            }
        }
        for (const auto& surface : surfaces) {
            // Native transparency alone leaves Qt's enter/leave and cursor
            // tracking active, allowing a delayed overlay event to reset the
            // cursor of another window in the same process.
            surface.qtWindow->setFlag(Qt::WindowTransparentForInput, true);
            surface.native.ignoresMouseEvents = YES;
        }
        if (desktop)
            return true;
        if (localTarget) {
            // AX hit testing in our own process can see the overlay instead of
            // the selected window. Native identity avoids that ambiguity entirely.
            [NSApp activate];
            [localTarget makeKeyAndOrderFront:nil];
            return true;
        }
        // Activate only the window under the pointer. Activation alone can choose
        // another window belonging to the same application.
        const auto raiseError = AXUIElementPerformAction(targetWindow, kAXRaiseAction);
        const auto focusError =
            AXUIElementSetAttributeValue(application, kAXFocusedWindowAttribute, targetWindow);
        if (raiseError != kAXErrorSuccess || focusError != kAXErrorSuccess)
            return false;
        [NSApp yieldActivationToApplication:targetApp];
        return [targetApp activateWithOptions:0];
    }

    bool resolveExternalTarget(CGPoint location, pid_t pid) {
        if (!AXIsProcessTrusted())
            return false;
        application = AXUIElementCreateApplication(pid);
        AXUIElementSetMessagingTimeout(application, 0.25F);
        AXUIElementRef element = nullptr;
        if (AXUIElementCopyElementAtPosition(application, static_cast<float>(location.x),
                                             static_cast<float>(location.y),
                                             &element) != kAXErrorSuccess)
            return false;
        CFTypeRef window = nullptr;
        const auto error = AXUIElementCopyAttributeValue(element, kAXWindowAttribute, &window);
        if (error == kAXErrorSuccess && window) {
            targetWindow = static_cast<AXUIElementRef>(window);
        } else {
            CFTypeRef role = nullptr;
            if (AXUIElementCopyAttributeValue(element, kAXRoleAttribute, &role) ==
                    kAXErrorSuccess &&
                role && CFEqual(role, kAXWindowRole))
                targetWindow = static_cast<AXUIElementRef>(CFRetain(element));
            if (role)
                CFRelease(role);
        }
        CFRelease(element);
        return targetWindow != nullptr;
    }

    bool ready() const {
        if (desktop)
            return true;
        if (!targetApp.active)
            return false;
        if (localTarget)
            return localTarget.visible &&
                   (!localTarget.canBecomeKeyWindow || localTarget.keyWindow);
        CFTypeRef focused = nullptr;
        const bool ready = AXUIElementCopyAttributeValue(application, kAXFocusedWindowAttribute,
                                                         &focused) == kAXErrorSuccess &&
                           focused && CFEqual(focused, targetWindow);
        if (focused)
            CFRelease(focused);
        return ready;
    }

    bool refreshCursor() {
        if (!desktop && !localTarget) {
            mouseTap = CGEventTapCreateForPid(
                targetApp.processIdentifier, kCGTailAppendEventTap, kCGEventTapOptionListenOnly,
                CGEventMaskBit(kCGEventMouseMoved),
                [](CGEventTapProxy, CGEventType type, CGEventRef event,
                   void* context) -> CGEventRef {
                    auto* state = static_cast<RecaptureNativeState*>(context);
                    if (type == kCGEventMouseMoved &&
                        CGEventGetIntegerValueField(event, kCGEventSourceUserData) ==
                            reinterpret_cast<intptr_t>(state))
                        state->mouseDelivered = true;
                    return event;
                },
                this);
            if (!mouseTap)
                return false;
            mouseSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, mouseTap, 0);
            if (!mouseSource)
                return false;
            CFRunLoopAddSource(CFRunLoopGetMain(), mouseSource, kCFRunLoopCommonModes);
        }
        // A stationary pointer will otherwise keep the overlay's last cursor.
        // No click, drag, or pointer warp is needed to update native cursor rects.
        CGEventRef position = CGEventCreate(nullptr);
        if (!position)
            return false;
        const CGPoint location = CGEventGetLocation(position);
        CFRelease(position);
        // Revalidate with the same exclusions and stacking policy used at begin.
        // Do not replay a stale pointer position if the user moved during activation.
        if (targetAt(location).id != targetId)
            return false;
        if (localTarget) {
            // The hit was revalidated above, so dispatch to that exact native
            // window. A posted stationary move can be coalesced into enter/exit
            // tracking events and has no reliable local delivery acknowledgement.
            [localTarget
                sendEvent:[NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                             location:localTarget.mouseLocationOutsideOfEventStream
                                        modifierFlags:NSEvent.modifierFlags
                                            timestamp:NSProcessInfo.processInfo.systemUptime
                                         windowNumber:localTarget.windowNumber
                                              context:nil
                                          eventNumber:0
                                           clickCount:0
                                             pressure:0]];
            mouseDelivered = true;
            return true;
        }
        CGEventRef move =
            CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, location, kCGMouseButtonLeft);
        if (!move)
            return false;
        CGEventSetIntegerValueField(move, kCGEventSourceUserData, reinterpret_cast<intptr_t>(this));
        CGEventPost(kCGSessionEventTap, move);
        CFRelease(move);
        return true;
    }

    bool cursorReady() const {
        if (desktop)
            return true;
        // Local dispatch completed synchronously before this poll. For a foreign app,
        // the synchronous focus query also crosses its main thread after the tap
        // observes routing, before the asynchronous ScreenCaptureKit request.
        if (!mouseDelivered || !ready())
            return false;
        if (localTarget) {
            // With a stationary pointer, AppKit can keep the target's tracking
            // area entered across the temporary overlay focus. Qt then retains
            // the right per-view cursor, but no cursorUpdate restores it globally.
            // Ask the actual responder to apply its cursor after mouse dispatch.
            const NSPoint location = localTarget.mouseLocationOutsideOfEventStream;
            NSView* content = localTarget.contentView;
            NSView* hit = [content hitTest:[content.superview convertPoint:location fromView:nil]];
            [hit cursorUpdate:[NSEvent enterExitEventWithType:NSEventTypeCursorUpdate
                                                     location:location
                                                modifierFlags:NSEvent.modifierFlags
                                                    timestamp:NSProcessInfo.processInfo.systemUptime
                                                 windowNumber:localTarget.windowNumber
                                                      context:nil
                                                  eventNumber:0
                                               trackingNumber:0
                                                     userData:nullptr]];
        }
        return true;
    }

    void restore() {
        if (mouseSource) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), mouseSource, kCFRunLoopCommonModes);
            CFRelease(mouseSource);
            mouseSource = nullptr;
        }
        if (mouseTap) {
            CFMachPortInvalidate(mouseTap);
            CFRelease(mouseTap);
            mouseTap = nullptr;
        }
        for (const auto& surface : surfaces) {
            // Do not apply saved Qt flags to a replacement native surface.
            if (surface.qtWindow && surface.qtWindow->handle() &&
                reinterpret_cast<NSView*>(surface.qtWindow->winId()).window == surface.native)
                surface.qtWindow->setFlag(Qt::WindowTransparentForInput,
                                          surface.transparentForInput);
            surface.native.ignoresMouseEvents = surface.ignoredMouse;
        }
    }
};
} // namespace

std::unique_ptr<RecaptureFocus> createRecaptureFocus(const QVector<QWidget*>& windows) {
    auto state = std::make_shared<RecaptureNativeState>();
    QVector<QPointer<QWidget>> guarded;
    for (QWidget* window : windows)
        guarded.push_back(window);
    return std::make_unique<RecaptureFocus>(RecaptureFocus::Backend{
        [state, guarded] {
            QVector<QWidget*> live;
            for (const auto& window : guarded) {
                if (!window || !window->isVisible())
                    return false;
                live.push_back(window);
            }
            return state->begin(live);
        },
        [state] { return state->ready(); }, [state] { return state->refreshCursor(); },
        [state] { state->restore(); }, [state] { return state->cursorReady(); }});
}
} // namespace snow_shot::platform::macos

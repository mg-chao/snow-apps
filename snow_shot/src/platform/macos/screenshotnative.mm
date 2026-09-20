#include "snow_shot/platform/screenshotnative.h"
#include "screenshotwindowtarget_p.h"
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
    const int layer = std::max(2, ownerLayer + 1);
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
            layer = std::max(2, ownerLayer + 1);
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
    int modalFloor = 2;
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
    applyScreenshotLayer(widget, 2);
    // A toolbar or popup may have been materialized before the overlay was shown.
    synchronizeScreenshotLayers();
}

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
void configureScreenshotOverlayWindow(QWidget* widget) {
    registerScreenshotLayer(widget, 0);
}

void configureScreenshotToolbarWindow(QWidget* widget) {
    registerScreenshotLayer(widget, 1);
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

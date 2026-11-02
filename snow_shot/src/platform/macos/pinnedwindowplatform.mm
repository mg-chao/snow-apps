#include "../../presentation/pinned/pinnedwindowplatform.h"
#include "capturewindowlayers_p.h"
#include "windowcursorcoordinator.h"
#include "widgets/detail/native_window_policy_mac_p.h"
#include "widgets/detail/window_surface_observer.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <QGuiApplication>
#include <QScopedValueRollback>
#include <QThread>
#include <QWindow>
#include <cmath>

namespace snow_shot::presentation {
namespace {
qreal desktopTop() {
    return NSMaxY(NSScreen.screens.firstObject.frame);
}
NSRect cocoaRect(const QRectF& rect) {
    return NSMakeRect(rect.x(), desktopTop() - rect.y() - rect.height(), rect.width(),
                      rect.height());
}
QRectF desktopRect(NSRect rect) {
    return {rect.origin.x, desktopTop() - NSMaxY(rect), rect.size.width, rect.size.height};
}

bool hasPinnedLayer(QWidget* widget) {
    using namespace snow_shot::platform::detail;
    return widget && widget->property(kScreenshotLayer).isValid() &&
           widget->property(kCaptureFamily).toInt() == static_cast<int>(CaptureFamily::Pinned);
}

class CocoaPinnedWakeNotifications final : public QObject {
  public:
    explicit CocoaPinnedWakeNotifications(QObject* parent) : QObject(parent) {
        // NSWorkspace retains internal power notifiers after removing subscriptions on some
        // macOS releases. Share one subscription across native surfaces for the app's lifetime.
        m_observer = [[NSWorkspace.sharedWorkspace.notificationCenter
            addObserverForName:NSWorkspaceDidWakeNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      const auto platforms = m_platforms;
                      for (const auto& platform : platforms) {
                          if (platform && m_platforms.contains(platform)) {
                              const auto changed = platform->environmentChanged;
                              if (changed)
                                  changed(false);
                          }
                      }
                    }] retain];
    }

    ~CocoaPinnedWakeNotifications() override {
        [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:m_observer];
        [m_observer release];
    }

    void add(PinnedWindowPlatform* platform) {
        if (!m_platforms.contains(platform))
            m_platforms.append(platform);
    }

    void remove(PinnedWindowPlatform* platform) {
        m_platforms.removeAll(platform);
    }

  private:
    id m_observer = nil;
    QList<QPointer<PinnedWindowPlatform>> m_platforms;
};

CocoaPinnedWakeNotifications* pinnedWakeNotifications() {
    static QPointer<CocoaPinnedWakeNotifications> notifications;
    if (!notifications)
        notifications = new CocoaPinnedWakeNotifications(qApp);
    return notifications;
}

class CocoaPinnedWindowPlatform final : public PinnedWindowPlatform {
  public:
    CocoaPinnedWindowPlatform(QWidget* window, Role role) : PinnedWindowPlatform(window, role) {
        if (role == Role::Image)
            snow_shot::platform::macos::configureWindowCursorUpdates(window);
        m_nativeObserver = std::make_unique<adqt::widgets::detail::WindowSurfaceObserver>(
            window, [this] { static_cast<void>(attach()); }, this);
    }
    ~CocoaPinnedWindowPlatform() override {
        m_nativeObserver.reset();
        detach();
    }
    bool attach() override {
        if (m_detaching || !m_window || !m_window->internalWinId() ||
            QThread::currentThread() != qApp->thread())
            return false;
        if (m_attaching)
            return m_native != nil;
        const QScopedValueRollback attaching(m_attaching, true);
        observeNativeSurface();
        NSView* view = reinterpret_cast<NSView*>(m_window->internalWinId());
        NSWindow* window = view.window;
        if (!window)
            return false;
        if (window != m_native) {
            detach();
            m_native = [window retain];
            m_observers = [[NSMutableArray alloc] init];
            for (NSNotificationName name in @[
                     NSWindowDidChangeScreenNotification,
                     NSWindowDidChangeBackingPropertiesNotification
                 ]) {
                id token = [NSNotificationCenter.defaultCenter
                    addObserverForName:name
                                object:window
                                 queue:nil
                            usingBlock:^(NSNotification*) {
                              if (!m_applying && environmentChanged)
                                  environmentChanged(false);
                            }];
                [m_observers addObject:token];
            }
            m_wakeNotifications = pinnedWakeNotifications();
            for (NSNotificationName name in @[
                     NSWindowDidBecomeKeyNotification, NSWindowDidChangeOcclusionStateNotification
                 ]) {
                id token = [NSNotificationCenter.defaultCenter
                    addObserverForName:name
                                object:window
                                 queue:nil
                            usingBlock:^(NSNotification* note) {
                              if ([note.name isEqualToString:NSWindowDidBecomeKeyNotification] &&
                                  stackingActivated)
                                  stackingActivated();
                              if (stackingChanged)
                                  stackingChanged();
                            }];
                [m_observers addObject:token];
            }
            id activated = [NSNotificationCenter.defaultCenter
                addObserverForName:NSApplicationDidBecomeActiveNotification
                            object:NSApp
                             queue:nil
                        usingBlock:^(NSNotification*) {
                          if (stackingChanged)
                              stackingChanged();
                        }];
            [m_observers addObject:activated];
            m_wakeNotifications->add(this);
        }
        adqt::widgets::detail::NativeWindowConstraints constraints;
        const bool pinnedLayer = hasPinnedLayer(m_window);
        const bool otherLayer =
            m_window->property(snow_shot::platform::detail::kScreenshotLayer).isValid() &&
            !pinnedLayer;
        if (m_role == Role::Image) {
            // The shared pin controller owns proportional edge resizing and background
            // dragging. Qt leaves these native policies enabled on its frameless NSPanel,
            // so AppKit consumes the pointer gesture before the controller can begin and
            // the controller subsequently restores AppKit's untracked geometry change.
            constexpr NSWindowStyleMask nativeChrome =
                NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
            constraints.excludedStyleMask = nativeChrome;
            constraints.movable = false;
            constraints.movableByWindowBackground = false;
            constraints.hasShadow = false;
        }
        if (!m_staysOnTop && !otherLayer)
            constraints.level = NSNormalWindowLevel;
        constraints.requiredCollectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                                 NSWindowCollectionBehaviorFullScreenAuxiliary;
        constraints.excludedCollectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace |
                                                 NSWindowCollectionBehaviorFullScreenPrimary |
                                                 NSWindowCollectionBehaviorFullScreenNone;
        constraints.hidesOnDeactivate = false;
        // Normal pins leave temporary Qt input ownership (for example recapture)
        // intact. Click-through pins require pass-through until explicitly exited.
        if (m_transparent)
            constraints.ignoresMouseEvents = true;
        // Release the old level owner before applying its replacement. Otherwise
        // capture's unchanged-level fast path can bypass the normal-level setter.
        if (pinnedLayer && !m_staysOnTop)
            snow_shot::platform::detail::setPinnedWindowLayer(m_window, false);
        m_interactionPolicy.apply(window, constraints);
        if (!pinnedLayer && !otherLayer && m_staysOnTop)
            snow_shot::platform::detail::setPinnedWindowLayer(m_window, true);
        return true;
    }
    void detach() override {
        if (m_detaching)
            return;
        m_detaching = true;
        for (id observer in m_observers)
            [NSNotificationCenter.defaultCenter removeObserver:observer];
        [m_observers release];
        m_observers = nil;
        if (m_wakeNotifications) {
            m_wakeNotifications->remove(this);
            m_wakeNotifications.clear();
        }
        if (NSWindow* native = m_native) {
            // Changing the style mask may synchronously tear down Qt's platform
            // surface. Clear our attachment first so that notification cannot
            // recursively restore a partially updated NSWindow.
            m_native = nil;
            if (hasPinnedLayer(m_window))
                snow_shot::platform::detail::setPinnedWindowLayer(m_window, false);
            NSView* view = m_window && m_window->internalWinId()
                               ? reinterpret_cast<NSView*>(m_window->internalWinId())
                               : nil;
            if (view.window != native)
                snow_shot::platform::detail::releaseNativeWindowLayer(native);
            m_interactionPolicy.reset();
            [native release];
        }
        m_detaching = false;
    }
    bool applyPlacement(const PinnedPlacement& placement, QScreen* screen,
                        GeometryUpdate) override {
        if (!m_window || !screen || !placement.isValid() ||
            placement.units != storage::PinnedGeometryUnits::LogicalPixels)
            return false;
        NSScreen* nativeScreen = nil;
        for (NSScreen* candidate in NSScreen.screens) {
            if (desktopRect(candidate.frame) == QRectF(screen->geometry())) {
                nativeScreen = candidate;
                break;
            }
        }
        if (!nativeScreen)
            return false;
        const QScopedValueRollback<bool> applying(m_applying, true);
        m_window->setScreen(screen);
        if (!attach())
            return false;
        // QWidget owns an integer logical frame. Quantize the pointer-derived
        // origin once, then apply Cocoa's level-aware frame constraints (topmost
        // pins may overlap system chrome) before committing through Qt. A second, fractional
        // NSWindow write makes Qt and AppKit disagree about the frame and can cancel an otherwise
        // valid drag.
        const QRectF requested = pinnedDesktopRect(placement, *screen);
        const QRect logicalFrame(requested.topLeft().toPoint(), placement.windowSize);
        const NSRect frame = m_preservePlacementOrigin
                                 ? cocoaRect(logicalFrame)
                                 : [m_native constrainFrameRect:cocoaRect(logicalFrame)
                                                       toScreen:nativeScreen];
        const QRect target(desktopRect(frame).topLeft().toPoint(), placement.windowSize);
        m_window->setGeometry(target);
        // QWidget defers native geometry changes while hidden. Pin creation and
        // pooled shells must commit their frame before show() and before native
        // verification; QWindow applies it immediately through the same Qt path.
        if (!m_window->isVisible())
            m_window->windowHandle()->setGeometry(target);
        const auto actual = this->placement();
        // Backing-display changes must not alter the logical frame.
        return actual && actual->windowSize == placement.windowSize &&
               std::abs(desktopRect(m_native.frame).x() - target.x()) < 0.01 &&
               std::abs(desktopRect(m_native.frame).y() - target.y()) < 0.01;
    }
    std::optional<PinnedPlacement> placement() const override {
        if (!m_native || !m_window || !m_window->screen())
            return std::nullopt;
        QScreen* screen = m_window->screen();
        const QRectF nativeScreen = desktopRect(m_native.screen.frame);
        for (QScreen* candidate : QGuiApplication::screens()) {
            if (QRectF(candidate->geometry()) == nativeScreen) {
                screen = candidate;
                break;
            }
        }
        const QRectF rect = desktopRect(m_native.frame);

        return PinnedPlacement{screen->name(), screen->serialNumber(),
                               rect.topLeft() - QPointF(screen->geometry().topLeft()),
                               QSize(qRound(rect.width()), qRound(rect.height()))};
    }
    bool setInputTransparent(bool transparent) override {
        const bool previous = m_transparent;
        m_transparent = transparent;
        if (attach()) {
            if (!transparent)
                m_native.ignoresMouseEvents = NO;
            if (m_native.ignoresMouseEvents == transparent)
                return true;
        }
        m_transparent = previous;
        return false;
    }
    bool setStaysOnTop(bool staysOnTop) override {
        m_staysOnTop = staysOnTop;
        if (!attach())
            return false;
        if (stackingChanged)
            stackingChanged();
        return m_native.level == (staysOnTop ? snow_shot::platform::detail::pinnedWindowLevel()
                                             : NSNormalWindowLevel);
    }
    bool activate() override {
        if (m_transparent || !attach())
            return false;
        [NSApp activate];
        m_window->activateWindow();
        if (QWindow* handle = m_window->windowHandle())
            handle->requestActivate();
        [m_native makeKeyAndOrderFront:nil];
        // Activation is asynchronous on macOS 14+. Report whether the request
        // can receive focus; key-window notifications remain the focus authority.
        return m_native.canBecomeKeyWindow;
    }
    std::optional<QPointF> pointerPosition() const override {
        CGEventRef event = CGEventCreate(nullptr);
        if (!event)
            return std::nullopt;
        const CGPoint position = CGEventGetLocation(event);
        CFRelease(event);
        return QPointF(position.x, position.y);
    }
    bool usesControlledInteraction() const override {
        return true;
    }

  private:
    NSWindow* m_native = nil;
    NSMutableArray* m_observers = nil;
    QPointer<CocoaPinnedWakeNotifications> m_wakeNotifications;
    adqt::widgets::detail::MacNativeWindowPolicy m_interactionPolicy;
    std::unique_ptr<adqt::widgets::detail::WindowSurfaceObserver> m_nativeObserver;
    bool m_applying = false;
    bool m_detaching = false;
    bool m_attaching = false;
    bool m_staysOnTop = true;
};
} // namespace
QRect cocoaPinnedUsableGeometry(const QScreen& screen) {
    for (NSScreen* native in NSScreen.screens) {
        const QRectF frame = desktopRect(native.frame);
        if (frame.toAlignedRect() != screen.geometry())
            continue;
        const NSEdgeInsets safe = native.safeAreaInsets;
        return desktopRect(native.visibleFrame)
            .intersected(frame.adjusted(safe.left, safe.top, -safe.right, -safe.bottom))
            .toRect();
    }
    return screen.availableGeometry();
}
std::unique_ptr<PinnedWindowPlatform>
createCocoaPinnedWindowPlatform(QWidget* window, PinnedWindowPlatform::Role role) {
    return std::make_unique<CocoaPinnedWindowPlatform>(window, role);
}
} // namespace snow_shot::presentation

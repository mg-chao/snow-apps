#include "../../presentation/pinned/pinnedwindowplatform.h"
#include "capturewindowlayers_p.h"
#include "windowcursorcoordinator.h"

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
    }
    ~CocoaPinnedWindowPlatform() override {
        detach();
    }
    bool attach() override {
        if (m_detaching || !m_window || !m_window->internalWinId() ||
            QThread::currentThread() != qApp->thread())
            return false;
        observeNativeSurface();
        NSView* view = reinterpret_cast<NSView*>(m_window->internalWinId());
        NSWindow* window = view.window;
        if (!window)
            return false;
        if (window != m_native) {
            detach();
            m_native = [window retain];
            m_styleMask = window.styleMask;
            m_movable = window.movable;
            m_movableByWindowBackground = window.movableByWindowBackground;
            m_hasShadow = window.hasShadow;
            m_level = window.level;
            m_behavior = window.collectionBehavior;
            m_ignoresMouse = window.ignoresMouseEvents;
            m_hidesOnDeactivate = window.hidesOnDeactivate;
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
            m_wakeNotifications->add(this);
            // Register each native surface once. Descendants then inherit the
            // same stacking policy as recording windows, including modal sessions.
            snow_shot::platform::detail::setPinnedWindowLayer(m_window, m_staysOnTop);
        }
        if (m_role == Role::Image) {
            // The shared pin controller owns proportional edge resizing and background
            // dragging. Qt leaves these native policies enabled on its frameless NSPanel,
            // so AppKit consumes the pointer gesture before the controller can begin and
            // the controller subsequently restores AppKit's untracked geometry change.
            constexpr NSWindowStyleMask nativeChrome =
                NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
            const NSWindowStyleMask currentStyle = window.styleMask;
            const NSWindowStyleMask desiredStyle = currentStyle & ~nativeChrome;
            if (currentStyle != desiredStyle)
                window.styleMask = desiredStyle;
            if (window.movable)
                window.movable = NO;
            if (window.movableByWindowBackground)
                window.movableByWindowBackground = NO;
            if (window.hasShadow)
                window.hasShadow = NO;
        }
        if (!m_staysOnTop && window.level != NSNormalWindowLevel)
            window.level = NSNormalWindowLevel;
        const NSWindowCollectionBehavior currentBehavior = window.collectionBehavior;
        const NSWindowCollectionBehavior desiredBehavior =
            (currentBehavior & ~(NSWindowCollectionBehaviorMoveToActiveSpace |
                                 NSWindowCollectionBehaviorFullScreenPrimary)) |
            NSWindowCollectionBehaviorCanJoinAllSpaces |
            NSWindowCollectionBehaviorFullScreenAuxiliary;
        if (currentBehavior != desiredBehavior)
            window.collectionBehavior = desiredBehavior;
        if (window.hidesOnDeactivate)
            window.hidesOnDeactivate = NO;
        if (window.ignoresMouseEvents != m_transparent)
            window.ignoresMouseEvents = m_transparent;
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
            snow_shot::platform::detail::setPinnedWindowLayer(m_window, false);
            native.styleMask = m_styleMask;
            native.movable = m_movable;
            native.movableByWindowBackground = m_movableByWindowBackground;
            native.hasShadow = m_hasShadow;
            native.level = m_level;
            native.collectionBehavior = m_behavior;
            native.ignoresMouseEvents = m_ignoresMouse;
            native.hidesOnDeactivate = m_hidesOnDeactivate;
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
        if (!attach())
            return false;
        if (m_native.ignoresMouseEvents != transparent)
            m_native.ignoresMouseEvents = transparent;
        if (m_native.ignoresMouseEvents != transparent)
            return false;
        m_transparent = transparent;
        return true;
    }
    bool setStaysOnTop(bool staysOnTop) override {
        m_staysOnTop = staysOnTop;
        if (!attach())
            return false;
        snow_shot::platform::detail::setPinnedWindowLayer(m_window, staysOnTop);
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
    NSWindowStyleMask m_styleMask = NSWindowStyleMaskBorderless;
    bool m_movable = true;
    bool m_movableByWindowBackground = false;
    bool m_hasShadow = true;
    NSInteger m_level = NSNormalWindowLevel;
    NSWindowCollectionBehavior m_behavior = NSWindowCollectionBehaviorDefault;
    bool m_ignoresMouse = false;
    bool m_hidesOnDeactivate = false;
    bool m_applying = false;
    bool m_detaching = false;
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

#include "../../presentation/pinned/pinnedwindowplatform.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <QGuiApplication>
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
class CocoaPinnedWindowPlatform final : public PinnedWindowPlatform {
  public:
    CocoaPinnedWindowPlatform(QWidget* window, Role role) : PinnedWindowPlatform(window, role) {}
    ~CocoaPinnedWindowPlatform() override {
        detach();
    }
    bool attach() override {
        if (!m_window || !m_window->internalWinId() || QThread::currentThread() != qApp->thread())
            return false;
        observeNativeSurface();
        NSView* view = reinterpret_cast<NSView*>(m_window->internalWinId());
        NSWindow* window = view.window;
        if (!window)
            return false;
        if (window != m_native) {
            detach();
            m_native = [window retain];
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
                                  environmentChanged();
                            }];
                [m_observers addObject:token];
            }
            m_wakeObserver = [[NSWorkspace.sharedWorkspace.notificationCenter
                addObserverForName:NSWorkspaceDidWakeNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification*) {
                          if (environmentChanged)
                              environmentChanged();
                        }] retain];
        }
        window.level = NSFloatingWindowLevel;
        window.collectionBehavior =
            (window.collectionBehavior & ~(NSWindowCollectionBehaviorMoveToActiveSpace |
                                           NSWindowCollectionBehaviorFullScreenPrimary)) |
            NSWindowCollectionBehaviorCanJoinAllSpaces |
            NSWindowCollectionBehaviorFullScreenAuxiliary;
        window.hidesOnDeactivate = NO;
        window.ignoresMouseEvents = m_transparent;
        return true;
    }
    void detach() override {
        for (id observer in m_observers)
            [NSNotificationCenter.defaultCenter removeObserver:observer];
        [m_observers release];
        m_observers = nil;
        if (m_wakeObserver) {
            [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:m_wakeObserver];
            [m_wakeObserver release];
            m_wakeObserver = nil;
        }
        if (m_native) {
            m_native.level = m_level;
            m_native.collectionBehavior = m_behavior;
            m_native.ignoresMouseEvents = m_ignoresMouse;
            m_native.hidesOnDeactivate = m_hidesOnDeactivate;
            [m_native release];
            m_native = nil;
        }
    }
    bool applyPlacement(const PinnedPlacement& placement, QScreen* screen,
                        GeometryUpdate) override {
        if (!m_window || !screen || !placement.isValid())
            return false;
        m_applying = true;
        m_window->setScreen(screen);
        const QRectF target = pinnedDesktopRect(placement, *screen);
        // Qt owns the backing store; AppKit supplies the sub-point native frame.
        // The enclosing Qt surface can contain one transparent excess pixel.
        m_window->setGeometry(target.toAlignedRect());
        const bool attached = attach();
        if (attached)
            [m_native setFrame:cocoaRect(target) display:YES animate:NO];
        m_applying = false;
        const auto actual = this->placement();
        // A frame application may itself change the native backing display.
        // Report the frame result; the shared transaction uses actual backing
        // pixels to reconcile that transition around its interaction anchor.
        return attached && actual &&
               std::abs(desktopRect(m_native.frame).x() - target.x()) <
                   0.51 / screen->devicePixelRatio() &&
               std::abs(desktopRect(m_native.frame).y() - target.y()) <
                   0.51 / screen->devicePixelRatio();
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
        const qreal dpr = m_native.backingScaleFactor;
        return PinnedPlacement{screen->name(), screen->serialNumber(),
                               rect.topLeft() - QPointF(screen->geometry().topLeft()),
                               QSize(qRound(rect.width() * dpr), qRound(rect.height() * dpr))};
    }
    bool setInputTransparent(bool transparent) override {
        if (!attach())
            return false;
        m_native.ignoresMouseEvents = transparent;
        if (m_native.ignoresMouseEvents != transparent)
            return false;
        m_transparent = transparent;
        return true;
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
    id m_wakeObserver = nil;
    NSInteger m_level = NSNormalWindowLevel;
    NSWindowCollectionBehavior m_behavior = NSWindowCollectionBehaviorDefault;
    bool m_ignoresMouse = false;
    bool m_hidesOnDeactivate = false;
    bool m_applying = false;
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

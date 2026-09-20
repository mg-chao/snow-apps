#include "snow_shot/platform/macos/applicationactivation.h"

#import <AppKit/AppKit.h>

#include <QApplication>
#include <QGuiApplication>
#include <QThread>
#include <QWidget>
#include <QWindow>

#include <array>
#include <utility>

@interface SnowShotApplicationDelegateProxy : NSObject <NSApplicationDelegate> {
  @private
    id<NSApplicationDelegate> m_forwardDelegate;
    std::function<void()> m_reopen;
}
- (instancetype)initWithForwardDelegate:(id<NSApplicationDelegate>)forwardDelegate
                                 reopen:(std::function<void()>)reopen;
@end

@implementation SnowShotApplicationDelegateProxy
- (instancetype)initWithForwardDelegate:(id<NSApplicationDelegate>)forwardDelegate
                                 reopen:(std::function<void()>)reopen {
    self = [super init];
    if (self != nil) {
        m_forwardDelegate = [forwardDelegate retain];
        m_reopen = std::move(reopen);
    }
    return self;
}

- (void)dealloc {
    [m_forwardDelegate release];
    [super dealloc];
}

- (BOOL)respondsToSelector:(SEL)selector {
    return [super respondsToSelector:selector] || [m_forwardDelegate respondsToSelector:selector];
}

- (id)forwardingTargetForSelector:(SEL)selector {
    if ([m_forwardDelegate respondsToSelector:selector]) {
        return m_forwardDelegate;
    }
    return [super forwardingTargetForSelector:selector];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)application
                    hasVisibleWindows:(BOOL)hasVisibleWindows {
    if ([m_forwardDelegate
            respondsToSelector:@selector(applicationShouldHandleReopen:hasVisibleWindows:)]) {
        [m_forwardDelegate applicationShouldHandleReopen:application
                                       hasVisibleWindows:hasVisibleWindows];
    }
    if (m_reopen) {
        m_reopen();
    }
    return YES;
}
@end

namespace snow_shot::platform::macos {
namespace {
constexpr CGFloat TRAFFIC_LIGHT_LEFT_MARGIN = 14.0;
constexpr CGFloat TRAFFIC_LIGHT_SPACING = 6.0;

void positionStandardWindowButtons(NSWindow* window, int titleBarHeight) {
    if (window == nil || titleBarHeight <= 0 || window.contentView == nil) {
        return;
    }

    [window.contentView.superview layoutSubtreeIfNeeded];
    const NSRect contentBoundsInWindow = [window.contentView convertRect:window.contentView.bounds
                                                                  toView:nil];
    const CGFloat buttonCenterY = NSMaxY(contentBoundsInWindow) - titleBarHeight / 2.0;
    CGFloat buttonLeft = NSMinX(contentBoundsInWindow) + TRAFFIC_LIGHT_LEFT_MARGIN;
    const std::array<NSWindowButton, 3> buttonKinds = {
        NSWindowCloseButton,
        NSWindowMiniaturizeButton,
        NSWindowZoomButton,
    };

    for (const NSWindowButton kind : buttonKinds) {
        NSButton* button = [window standardWindowButton:kind];
        NSView* buttonSuperview = button.superview;
        if (button == nil || buttonSuperview == nil) {
            continue;
        }

        const NSRect frame = button.frame;
        const NSPoint originInWindow =
            NSMakePoint(buttonLeft, buttonCenterY - NSHeight(frame) / 2.0);
        const NSPoint originInSuperview = [buttonSuperview convertPoint:originInWindow
                                                               fromView:nil];
        [button setFrameOrigin:originInSuperview];
        button.hidden = NO;
        buttonLeft += NSWidth(frame) + TRAFFIC_LIGHT_SPACING;
    }
}
} // namespace

void configureMainWindowTitleBar(QWidget* window, int titleBarHeight) {
    if (window == nullptr || titleBarHeight <= 0 || QThread::currentThread() != qApp->thread() ||
        QGuiApplication::platformName() != QStringLiteral("cocoa") ||
        window->internalWinId() == 0) {
        return;
    }

    NSView* view = reinterpret_cast<NSView*>(window->internalWinId());
    NSWindow* nativeWindow = view.window;
    if (nativeWindow == nil) {
        return;
    }

    // The main interface is an ordinary app window, even if its native surface
    // previously acquired a floating level. Reapply this on show and state changes.
    nativeWindow.level = NSNormalWindowLevel;
    nativeWindow.titleVisibility = NSWindowTitleHidden;
    nativeWindow.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    nativeWindow.movableByWindowBackground = NO;
    positionStandardWindowButtons(nativeWindow, titleBarHeight);
}

void activateWindow(QWidget* window) {
    if (window == nullptr || QThread::currentThread() != qApp->thread() ||
        QGuiApplication::platformName() != QStringLiteral("cocoa")) {
        return;
    }
    static_cast<void>(window->winId());
    NSView* view = reinterpret_cast<NSView*>(window->internalWinId());
    NSWindow* nativeWindow = view.window;
    [NSApp activate];
    window->raise();
    window->activateWindow();
    if (QWindow* handle = window->windowHandle()) {
        handle->requestActivate();
    }
    [nativeWindow makeKeyAndOrderFront:nil];
}

class ApplicationReopenHandler::Impl final {
  public:
    explicit Impl(std::function<void()> reopen) {
        previousDelegate = [NSApp.delegate retain];
        proxy =
            [[SnowShotApplicationDelegateProxy alloc] initWithForwardDelegate:previousDelegate
                                                                       reopen:std::move(reopen)];
        NSApp.delegate = proxy;
    }

    ~Impl() {
        if (NSApp.delegate == proxy) {
            NSApp.delegate = previousDelegate;
        }
        [proxy release];
        [previousDelegate release];
    }

  private:
    id<NSApplicationDelegate> previousDelegate = nil;
    SnowShotApplicationDelegateProxy* proxy = nil;
};

ApplicationReopenHandler::ApplicationReopenHandler(std::function<void()> reopen)
    : m_impl(std::make_unique<Impl>(std::move(reopen))) {}

ApplicationReopenHandler::~ApplicationReopenHandler() = default;
} // namespace snow_shot::platform::macos

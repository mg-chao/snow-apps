#include "snow_shot/platform/macos/applicationactivation.h"

#import <AppKit/AppKit.h>

#include <QApplication>
#include <QCoreApplication>
#include <QWidget>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void flushEvents() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);

    id<NSApplicationDelegate> originalDelegate = NSApp.delegate;
    bool reopened = false;
    {
        snow_shot::platform::macos::ApplicationReopenHandler handler([&]() { reopened = true; });
        id<NSApplicationDelegate> proxy = NSApp.delegate;
        require(proxy != originalDelegate, "the reopen handler must install a forwarding proxy");
        require(
            [proxy respondsToSelector:@selector(applicationShouldHandleReopen:hasVisibleWindows:)],
            "the forwarding proxy must handle Dock reopen callbacks");
        [proxy applicationShouldHandleReopen:NSApp hasVisibleWindows:NO];
        require(reopened, "a Dock reopen callback must reach the application handler");
    }
    require(NSApp.delegate == originalDelegate,
            "destroying the reopen handler must restore Qt's application delegate");

    QWidget window(nullptr, Qt::Window | Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint);
    window.setAttribute(Qt::WA_LayoutOnEntireRect);
    window.resize(320, 180);
    window.show();
    flushEvents();
    NSView* view = reinterpret_cast<NSView*>(window.internalWinId());
    NSWindow* nativeWindow = view.window;
    require(nativeWindow != nil, "a Cocoa widget must expose an NSWindow");
    constexpr int titleBarHeight = 32;
    snow_shot::platform::macos::configureMainWindowTitleBar(&window, titleBarHeight);
    flushEvents();
    const NSWindowStyleMask expected =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
        NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView;
    require((nativeWindow.styleMask & expected) == expected,
            "the SnowShot title bar must retain resizable native macOS window behavior");
    require(nativeWindow.titleVisibility == NSWindowTitleHidden &&
                nativeWindow.titlebarAppearsTransparent,
            "the native title text and surface must yield to SnowShot's in-content title bar");

    NSButton* closeButton = [nativeWindow standardWindowButton:NSWindowCloseButton];
    NSButton* minimizeButton = [nativeWindow standardWindowButton:NSWindowMiniaturizeButton];
    NSButton* zoomButton = [nativeWindow standardWindowButton:NSWindowZoomButton];
    require(closeButton != nil && minimizeButton != nil && zoomButton != nil &&
                !closeButton.hidden && !minimizeButton.hidden && !zoomButton.hidden,
            "AppKit must continue to own all three native traffic-light buttons");
    const NSRect closeFrame = [closeButton.superview convertRect:closeButton.frame toView:nil];
    const NSRect minimizeFrame = [minimizeButton.superview convertRect:minimizeButton.frame
                                                                toView:nil];
    const NSRect zoomFrame = [zoomButton.superview convertRect:zoomButton.frame toView:nil];
    const NSRect contentFrame =
        [nativeWindow.contentView convertRect:nativeWindow.contentView.bounds toView:nil];
    NSView* frameView = nativeWindow.contentView.superview;
    const NSRect contentFrameInFrameView =
        [nativeWindow.contentView convertRect:nativeWindow.contentView.bounds toView:frameView];
    require(std::abs(NSMaxY(contentFrameInFrameView) - NSMaxY(frameView.bounds)) <= 1.0,
            "the Qt client area must reach the top of the native frame without a second title row");
    require(NSMidX(closeFrame) < NSMidX(minimizeFrame) && NSMidX(minimizeFrame) < NSMidX(zoomFrame),
            "native traffic lights must retain close/minimize/zoom order on the left");
    require(std::abs((NSMinX(closeFrame) - NSMinX(contentFrame)) - 14.0) <= 1.0,
            "native traffic lights must align with SnowShot's left title-bar inset");
    require(std::abs((NSMaxY(contentFrame) - NSMidY(closeFrame)) - titleBarHeight / 2.0) <= 1.0,
            "native traffic lights must be vertically centered in SnowShot's title bar");
    snow_shot::platform::macos::activateWindow(&window);
    flushEvents();
    require(nativeWindow.visible, "activation must keep the native window ordered in front");
    require(nativeWindow.keyWindow, "activation must make the native window key");
    return 0;
}

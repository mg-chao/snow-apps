#include "snow_shot/platform/macos/applicationactivation.h"

#import <AppKit/AppKit.h>

#include <QApplication>
#include <QCoreApplication>
#include <QWidget>

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

    QWidget window;
    window.resize(320, 180);
    window.show();
    flushEvents();
    NSView* view = reinterpret_cast<NSView*>(window.internalWinId());
    NSWindow* nativeWindow = view.window;
    require(nativeWindow != nil, "a Cocoa widget must expose an NSWindow");
    const NSWindowStyleMask expected =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    require((nativeWindow.styleMask & expected) == expected,
            "the main-window activation path must retain native macOS window chrome");
    snow_shot::platform::macos::activateWindow(&window);
    flushEvents();
    require(nativeWindow.visible, "activation must keep the native window ordered in front");
    require(nativeWindow.keyWindow, "activation must make the native window key");
    return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/platform/macos/screenshotsessionfocus.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLineEdit>
#include <QProcess>
#include <QTimer>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
bool waitFor(const std::function<bool()>& predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3000) {
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return predicate();
}
void activate(QWidget& widget) {
    [NSApp activate];
    [reinterpret_cast<NSView*>(widget.winId()).window makeKeyAndOrderFront:nil];
    widget.activateWindow();
    widget.setFocus();
    require(waitFor([&] { return NSApp.active && widget.isActiveWindow(); }),
            "test window must own foreground focus");
}

struct RestoreApplication {
    NSRunningApplication* application = [NSWorkspace.sharedWorkspace.frontmostApplication retain];
    ~RestoreApplication() {
        if (application && !application.terminated) {
            [NSApp yieldActivationToApplication:application];
            [application activateWithOptions:0];
        }
        [application release];
    }
};

void localWindow() {
    QLineEdit original;
    original.resize(320, 90);
    original.show();
    activate(original);
    auto focus = snow_shot::platform::macos::createScreenshotSessionFocus();
    focus->prepare();
    focus->begin(true);
    QLineEdit overlay;
    overlay.show();
    activate(overlay);
    focus->beforeHide(true);
    overlay.hide();
    focus->afterHide();
    require(waitFor([&] { return original.isActiveWindow() && original.hasFocus(); }),
            "local restoration must restore the original Qt input control");
}

void externalWindow(QApplication& app) {
    const bool appOnly = app.arguments().contains(QStringLiteral("--app-only"));
    const bool closedWindow = app.arguments().contains(QStringLiteral("--closed-window"));
    QProcess helper;
    helper.start(app.applicationFilePath(), {QStringLiteral("--target")});
    require(helper.waitForStarted(), "external focus helper must start");
    require(waitFor([&] { return helper.canReadLine(); }), "helper must publish readiness");
    require(helper.readLine().trimmed() == "READY", "helper must focus its first window");
    NSRunningApplication* target = [NSRunningApplication
        runningApplicationWithProcessIdentifier:static_cast<pid_t>(helper.processId())];
    require(waitFor([&] { return target.active; }), "helper must be foreground before capture");
    auto focus = snow_shot::platform::macos::createScreenshotSessionFocus(
        [appOnly] { return !appOnly && AXIsProcessTrusted(); });
    focus->prepare();
    focus->begin(true);

    // Change the helper's active window after the snapshot. App-only activation
    // would now pick the wrong window, even though the process identity is right.
    AXUIElementRef application = AXUIElementCreateApplication(target.processIdentifier);
    AXUIElementSetMessagingTimeout(application, 0.1F);
    CFTypeRef windows = nullptr;
    require(AXUIElementCopyAttributeValue(application, kAXWindowsAttribute, &windows) ==
                    kAXErrorSuccess &&
                windows && CFGetTypeID(windows) == CFArrayGetTypeID(),
            "helper must expose its windows through Accessibility");
    AXUIElementRef second = nullptr;
    AXUIElementRef first = nullptr;
    const auto list = static_cast<CFArrayRef>(windows);
    for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
        const auto window = static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(list, i));
        CFTypeRef title = nullptr;
        if (AXUIElementCopyAttributeValue(window, kAXTitleAttribute, &title) == kAXErrorSuccess &&
            title && CFEqual(title, CFSTR("Second focus fixture")))
            second = window;
        if (title && CFEqual(title, CFSTR("First focus fixture")))
            first = window;
        if (title)
            CFRelease(title);
    }
    require(second && AXUIElementPerformAction(second, kAXRaiseAction) == kAXErrorSuccess,
            "helper's second window must become available for activation");
    static_cast<void>(AXUIElementSetAttributeValue(second, kAXMainAttribute, kCFBooleanTrue));
    if (closedWindow) {
        CFTypeRef closeButton = nullptr;
        require(first &&
                    AXUIElementCopyAttributeValue(first, kAXCloseButtonAttribute, &closeButton) ==
                        kAXErrorSuccess &&
                    closeButton,
                "original window must expose a close button");
        require(AXUIElementPerformAction(static_cast<AXUIElementRef>(closeButton),
                                         kAXPressAction) == kAXErrorSuccess,
                "original window must close before restoration");
        CFRelease(closeButton);
    }
    CFRelease(windows);
    CFRelease(application);

    QLineEdit overlay;
    overlay.show();
    activate(overlay);
    focus->beforeHide(true);
    overlay.hide();
    focus->afterHide();
    require(waitFor([&] { return target.active; }), "original application must regain activation");
    // Post a real keyboard event without clicking, and observe the receiving control.
    for (const bool down : {true, false}) {
        CGEventRef key = CGEventCreateKeyboardEvent(nullptr, 0, down); // A
        const UniChar character = 'x';
        CGEventKeyboardSetUnicodeString(key, 1, &character);
        CGEventPost(kCGSessionEventTap, key);
        CFRelease(key);
    }
    require(waitFor([&] { return helper.canReadLine(); }), "restored editor must receive typing");
    require(helper.readLine().trimmed() == (appOnly || closedWindow ? "SECOND x" : "FIRST x"),
            "typing must reach the captured window, or the app's current window when unavailable");
    helper.terminate();
    if (!helper.waitForFinished(1000)) {
        helper.kill();
        helper.waitForFinished(1000);
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    if (app.arguments().contains(QStringLiteral("--target"))) {
        QLineEdit first;
        first.setWindowTitle(QStringLiteral("First focus fixture"));
        first.setGeometry(100, 100, 320, 90);
        first.show();
        QLineEdit second;
        second.setWindowTitle(QStringLiteral("Second focus fixture"));
        second.setGeometry(500, 100, 320, 90);
        second.show();
        activate(first);
        QObject::connect(&first, &QLineEdit::textChanged, &app, [](const QString& text) {
            std::cout << "FIRST " << text.toStdString() << std::endl;
        });
        QObject::connect(&second, &QLineEdit::textChanged, &app, [](const QString& text) {
            std::cout << "SECOND " << text.toStdString() << std::endl;
        });
        std::cout << "READY" << std::endl;
        QTimer::singleShot(15000, &app, &QApplication::quit);
        return app.exec();
    }
    RestoreApplication restore;
    if (app.arguments().contains(QStringLiteral("--local"))) {
        localWindow();
    } else {
        if (!AXIsProcessTrusted())
            return 77;
        externalWindow(app);
    }
    return 0;
}

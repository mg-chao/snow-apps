// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/platform/macos/screenshotsessionfocus.h"
#include "snow_shot/platform/screenshotnative.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLineEdit>
#include <QProcess>
#include <QScopeGuard>
#include <QTimer>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
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
    widget.raise();
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

bool coversWindow(CGWindowID cover, CGWindowID covered) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!list)
        return false;
    const auto release = qScopeGuard([&] { CFRelease(list); });
    CGRect coveringBounds{};
    bool foundCover = false;
    for (CFIndex index = 0; index < CFArrayGetCount(list); ++index) {
        auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, index));
        auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(entry, kCGWindowNumber));
        CGWindowID id = 0;
        if (!number || !CFNumberGetValue(number, kCFNumberSInt32Type, &id))
            continue;
        CGRect bounds{};
        auto rectangle = static_cast<CFDictionaryRef>(CFDictionaryGetValue(entry, kCGWindowBounds));
        if (!rectangle || !CGRectMakeWithDictionaryRepresentation(rectangle, &bounds))
            continue;
        if (id == cover) {
            foundCover = true;
            coveringBounds = bounds;
        }
        if (id == covered)
            return foundCover && CGRectContainsRect(coveringBounds, bounds);
    }
    return false;
}

class SurfaceFixture final : public QWidget {
  public:
    void replaceSurface() {
        destroy();
        create();
    }
};

void backgroundWindow(QApplication& app) {
    QWidget main;
    main.setGeometry(150, 135, 140, 35);
    main.show();
    activate(main);
    NSWindow* native = reinterpret_cast<NSView*>(main.winId()).window;
    const auto identity = main.internalWinId();
    const auto geometry = main.geometry();
    QProcess helper;
    helper.start(app.applicationFilePath(), {QStringLiteral("--background-target")});
    require(helper.waitForStarted(), "background helper must start");
    require(waitFor([&] { return helper.canReadLine(); }), "helper must publish its window ID");
    bool valid = false;
    const auto cover = helper.readLine().trimmed().toUInt(&valid);
    require(valid && cover, "helper must publish a native window ID");
    const auto cleanup = qScopeGuard([&] {
        helper.terminate();
        if (!helper.waitForFinished(1000)) {
            helper.kill();
            helper.waitForFinished(1000);
        }
    });
    NSRunningApplication* target = [NSRunningApplication
        runningApplicationWithProcessIdentifier:static_cast<pid_t>(helper.processId())];
    require(waitFor([&] {
                return target.active &&
                       coversWindow(cover, static_cast<CGWindowID>(native.windowNumber));
            }),
            "foreign helper must completely obscure the still-visible main window");
    const bool withoutGuard =
        app.arguments().contains(QStringLiteral("--without-background-guard"));
    QPointer<QWidget> registered = &main;
    auto focus = snow_shot::platform::macos::createScreenshotSessionFocus(
        [] { return false; },
        withoutGuard ? std::function<QWidget*()>{} : [&] { return registered.data(); });
    focus->prepare();
    require(native.visible, "preparation must not hide a window before admission");
    focus->begin(true);

    QWidget overlay(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlay.setAttribute(Qt::WA_ShowWithoutActivating);
    overlay.setAttribute(Qt::WA_TranslucentBackground);
    overlay.setGeometry(100, 100, 320, 90);
    snow_shot::platform::configureScreenshotOverlayWindow(&overlay);
    QWidget toolbar(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    toolbar.setAttribute(Qt::WA_ShowWithoutActivating);
    toolbar.setGeometry(100, 220, 200, 40);
    snow_shot::platform::configureScreenshotToolbarWindow(&toolbar);
    overlay.show();
    overlay.raise();
    overlay.activateWindow();
    toolbar.show();
    toolbar.raise();
    require(waitFor([&] { return NSApp.active; }),
            "the regression must exercise Qt's process-activating raise path");
    require(!native.visible, "capture activation must not reveal the obscured main window");
    for (bool scrolling : {true, false, true, false}) {
        // The production overlay's scrolling transition uses this adapter. The
        // controller test also exercises its actual renderer/presentation path.
        snow_shot::platform::setScreenshotInputTransparent(&overlay, scrolling);
        overlay.raise();
        toolbar.raise();
        focus->begin(true); // Recapture/restart keeps the same session lease.
        QCoreApplication::processEvents();
        require(!native.visible && main.isVisible() && main.internalWinId() == identity &&
                    main.geometry() == geometry,
                "scrolling and editing must retain native suppression without changing Qt state");
    }
    focus->beforeHide(true);
    toolbar.hide();
    overlay.hide();
    focus->afterHide();
    require(waitFor([&] {
                return native.visible && target.active &&
                       coversWindow(cover, static_cast<CGWindowID>(native.windowNumber));
            }),
            "completion must restore the main window behind the foreign application");

    // Invalidating the Qt owner or its native surface must never resurrect it.
    for (bool close : {false, true}) {
        focus->prepare();
        focus->begin(true);
        require(!native.visible, "the next capture must suppress the main window again");
        if (close)
            main.close();
        else
            main.hide();
        focus->beforeHide(false);
        focus->afterHide();
        QCoreApplication::processEvents();
        require(!main.isVisible(), "explicit hide and close must survive capture completion");
        main.setAttribute(Qt::WA_ShowWithoutActivating);
        main.show();
        native = reinterpret_cast<NSView*>(main.winId()).window;
        [native orderBack:nil];
        require(waitFor([&] {
                    return coversWindow(cover, static_cast<CGWindowID>(native.windowNumber));
                }),
                "the next fixture must remain behind the helper");
    }
    for (int state = 0; state < 4; ++state) {
        auto candidate = std::make_unique<SurfaceFixture>();
        registered = candidate.get();
        candidate->setAttribute(Qt::WA_ShowWithoutActivating);
        candidate->setGeometry(150, 135, 140, 35);
        candidate->show();
        NSWindow* oldWindow = [reinterpret_cast<NSView*>(candidate->winId()).window retain];
        const auto release = qScopeGuard([&] { [oldWindow release]; });
        [oldWindow orderBack:nil];
        if (state == 0)
            candidate->hide();
        else if (state == 1) {
            candidate->showMinimized();
            require(waitFor([&] { return candidate->isMinimized() && oldWindow.miniaturized; }),
                    "the native minimization transition must finish before capture");
        }
        require(waitFor([&] { return target.active; }),
                "eligibility cases must begin over the foreign application");
        focus->prepare();
        focus->begin(true);
        if (state == 2)
            candidate->replaceSurface();
        else if (state == 3)
            candidate.reset();
        focus->beforeHide(false);
        focus->afterHide();
        QCoreApplication::processEvents();
        if (state == 0)
            require(!candidate->isVisible() && !oldWindow.visible,
                    "an initially hidden main window must stay hidden");
        else if (state == 1)
            require(candidate->isMinimized() && oldWindow.miniaturized,
                    "an initially minimized main window must stay minimized");
        else
            require(!oldWindow.visible,
                    "surface replacement and destruction must retire the original native identity");
    }
    registered = &main;
    focus->prepare();
    focus->begin(true);
    focus->releaseBackgroundWindow();
    [native makeKeyAndOrderFront:nil];
    activate(main);
    focus->beforeHide(true);
    focus->afterHide();
    QCoreApplication::processEvents();
    require(native.visible && NSApp.active && native.keyWindow,
            "explicit reopening must win over queued background and focus restoration");
    focus->prepare();
    focus->begin(true);
    require(native.visible, "foreground self-capture must keep the main window capturable");
    focus->beforeHide(false);
    focus->afterHide();
    QCoreApplication::processEvents();
}
} // namespace

int main(int argc, char** argv) try {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    if (app.arguments().contains(QStringLiteral("--background-target"))) {
        QWidget cover;
        cover.setGeometry(100, 100, 320, 90);
        cover.show();
        activate(cover);
        std::cout << reinterpret_cast<NSView*>(cover.winId()).window.windowNumber << std::endl;
        QTimer::singleShot(30000, &app, &QApplication::quit);
        return app.exec();
    }
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
    if (app.arguments().contains(QStringLiteral("--background-window"))) {
        backgroundWindow(app);
    } else if (app.arguments().contains(QStringLiteral("--local"))) {
        localWindow();
    } else {
        if (!AXIsProcessTrusted())
            return 77;
        externalWindow(app);
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}

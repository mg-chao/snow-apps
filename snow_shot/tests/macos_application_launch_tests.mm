#import <AppKit/AppKit.h>

#include "snow_shot/app/applicationlaunchpolicy.h"
#include "snow_shot/platform/macos/applicationreopenhandler.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QObject>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void requestReopen(bool hasVisibleWindows) {
    id<NSApplicationDelegate> delegate = NSApplication.sharedApplication.delegate;
    require([delegate respondsToSelector:@selector(applicationShouldHandleReopen:
                                                               hasVisibleWindows:)],
            "application delegate did not preserve the native reopen callback");
    require([delegate applicationShouldHandleReopen:NSApplication.sharedApplication
                                  hasVisibleWindows:hasVisibleWindows],
            "reopen callback prevented normal AppKit handling");
}

void startupArguments() {
    using snow_shot::app::shouldShowMainWindowOnStartup;
    require(shouldShowMainWindowOnStartup({QStringLiteral("snow_shot")}),
            "manual macOS startup did not request the main window");
    require(shouldShowMainWindowOnStartup(
                {QStringLiteral("snow_shot"), QStringLiteral("--show-main-window")}),
            "explicit window startup did not request the main window");
    require(!shouldShowMainWindowOnStartup(
                {QStringLiteral("snow_shot"), QStringLiteral("--autostart")}),
            "autostart requested a visible main window");
    require(!shouldShowMainWindowOnStartup({QStringLiteral("snow_shot"),
                                            QStringLiteral("--show-main-window"),
                                            QStringLiteral("--autostart")}),
            "explicit show flag overrode background autostart");
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        // This test exercises native delegate delivery without opening windows or activating
        // the user's desktop. Finder/Dock integration is checked separately with the bundle.
        qputenv("QT_QPA_PLATFORM", "offscreen");
        auto reopen = std::make_unique<snow_shot::platform::macos::ApplicationReopenHandler>();
        requestReopen(false);
        requestReopen(false);
        QApplication application(argc, argv);
        startupArguments();

        int requests = 0;
        auto controller = std::make_unique<QObject>();
        reopen->setHandler(controller.get(), [&requests] { ++requests; });
        require(requests == 0, "early reopen was delivered inside handler installation");
        QCoreApplication::processEvents();
        require(requests == 1, "early reopen requests were lost or not coalesced");

        QEvent activation(QEvent::ApplicationActivate);
        QCoreApplication::sendEvent(&application, &activation);
        QCoreApplication::processEvents();
        require(requests == 1, "ordinary activation reopened the main window");

        requestReopen(false);
        QCoreApplication::processEvents();
        requestReopen(true);
        QCoreApplication::processEvents();
        require(requests == 3, "native reopen failed for hidden or visible windows");

        requestReopen(false);
        controller.reset();
        QCoreApplication::processEvents();
        require(requests == 3, "reopen callback ran after controller destruction");
        requestReopen(false);
        QCoreApplication::processEvents();
        require(requests == 3, "reopen callback used a destroyed controller context");

        QObject survivingController;
        reopen->setHandler(&survivingController, [&requests] { ++requests; });
        requestReopen(false);
        reopen.reset();
        QCoreApplication::processEvents();
        require(requests == 3, "queued callback survived native bridge destruction");
        require(NSApplication.sharedApplication.delegate == nil,
                "native bridge did not restore the previous delegate");
    }
    return 0;
}

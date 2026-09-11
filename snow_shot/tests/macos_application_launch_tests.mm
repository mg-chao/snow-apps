#import <AppKit/AppKit.h>

#include "snow_shot/app/applicationlaunchpolicy.h"
#include "snow_shot/app/deferredpermissionprompt.h"
#include "snow_shot/platform/macos/applicationreopenhandler.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QTimer>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <memory>

@interface SnowShotStartupProbeDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic) int finishedCalls;
@property(nonatomic) BOOL wouldActivate;
@end

@implementation SnowShotStartupProbeDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    ++self.finishedCalls;
    // Qt performs this same environment check after forwarding to our native delegate.
    self.wouldActivate =
        qEnvironmentVariableIsEmpty("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM");
}
@end

namespace {
constexpr auto kDisableForegroundTransform = "QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM";

struct ForegroundEnvironment {
    ForegroundEnvironment()
        : wasSet(qEnvironmentVariableIsSet(kDisableForegroundTransform)),
          value(qgetenv(kDisableForegroundTransform)) {}
    ~ForegroundEnvironment() {
        if (wasSet) {
            qputenv(kDisableForegroundTransform, value);
        } else {
            qunsetenv(kDisableForegroundTransform);
        }
    }
    bool wasSet;
    QByteArray value;
};

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
    require(!shouldShowMainWindowOnStartup({QStringLiteral("snow_shot")}, true),
            "a native login item launch without command-line flags must stay in the background");
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

void backgroundPermissionWaitsForAVisibleWindow() {
    snow_shot::app::DeferredPermissionPrompt permission;
    QObject controller;
    QWidget mainWindow;
    mainWindow.setAttribute(Qt::WA_QuitOnClose, false);
    int prompts = 0;
    const auto presentPending = [&] {
        if (permission.takeIfWindowVisible(mainWindow.isVisible())) {
            ++prompts;
        }
    };
    const auto queueForWindowAccess = [&] { QTimer::singleShot(0, &controller, presentPending); };

    // Missing AX access is discovered by background startup before any window is shown.
    for (int i = 0; i < 3; ++i) {
        permission.request();
        presentPending();
        QCoreApplication::processEvents();
    }
    require(prompts == 0 && !mainWindow.isVisible(),
            "background initialization must retain permission requests without showing a window");

    // ensureMainWindow queues before its caller shows the main page or a settings page.
    queueForWindowAccess();
    queueForWindowAccess();
    mainWindow.show();
    require(prompts == 0, "window access must not show a permission dialog before its caller");
    QCoreApplication::processEvents();
    require(prompts == 1,
            "manual window access must deliver exactly one pending permission prompt");

    permission.request();
    queueForWindowAccess();
    mainWindow.close();
    QCoreApplication::processEvents();
    require(prompts == 1, "closing a window before queued delivery must keep startup quiet");
    queueForWindowAccess();
    mainWindow.show();
    QCoreApplication::processEvents();
    require(prompts == 2, "a cancelled presentation must remain available on the next manual open");

    permission.request();
    presentPending();
    require(prompts == 3, "a repeated user-triggered permission failure must be presentable again");
}

void requestDidFinishLaunching() {
    id<NSApplicationDelegate> delegate = NSApplication.sharedApplication.delegate;
    require([delegate respondsToSelector:@selector(applicationDidFinishLaunching:)],
            "the bridge must receive the native launch completion");
    [delegate applicationDidFinishLaunching:
                  [NSNotification notificationWithName:NSApplicationDidFinishLaunchingNotification
                                                object:NSApplication.sharedApplication]];
}

void startupDecisionWaitsForNativeLaunch() {
    ForegroundEnvironment environment;
    for (const bool atLogin : {false, true}) {
        for (const bool installHandlerEarly : {false, true}) {
            qunsetenv(kDisableForegroundTransform);
            SnowShotStartupProbeDelegate* probe = [[SnowShotStartupProbeDelegate alloc] init];
            NSApplication.sharedApplication.delegate = probe;
            int detectorReads = 0;
            auto reopen =
                std::make_unique<snow_shot::platform::macos::ApplicationReopenHandler>([&] {
                    ++detectorReads;
                    return atLogin;
                });
            QObject controller;
            int deliveries = 0;
            bool showWindow = false;
            const auto handler = [&](bool nativeLogin) {
                ++deliveries;
                showWindow = snow_shot::app::shouldShowMainWindowOnStartup(
                    {QStringLiteral("snow_shot")}, nativeLogin);
            };
            if (installHandlerEarly) {
                reopen->setStartupHandler(&controller, handler);
                QCoreApplication::processEvents();
            }
            require(
                detectorReads == 0 && deliveries == 0,
                "QApplication construction and handler setup must not decide launch visibility");
            requestDidFinishLaunching();
            require(detectorReads == 1 && deliveries == 0 && reopen->launchedAtLogin() == atLogin &&
                        probe.finishedCalls == 1 && bool(probe.wouldActivate) == !atLogin,
                    "native completion must detect login before Qt can automatically take focus");
            if (!installHandlerEarly) {
                QCoreApplication::processEvents();
                require(deliveries == 0,
                        "launch completion without a controller must not invent a handler");
                reopen->setStartupHandler(&controller, handler);
            }
            require(deliveries == 0, "the startup callback must be queued, including late setup");
            QCoreApplication::processEvents();
            require(
                deliveries == 1 && showWindow == !atLogin &&
                    !qEnvironmentVariableIsSet(kDisableForegroundTransform),
                "manual startup must show once, login must stay hidden, and Qt state must restore");
            reopen->setStartupHandler(&controller, handler);
            requestDidFinishLaunching();
            QCoreApplication::processEvents();
            require(detectorReads == 1 && deliveries == 1,
                    "repeated native completion must not duplicate the initial window decision");
            reopen.reset();
            require(NSApplication.sharedApplication.delegate == probe,
                    "the native bridge must preserve the prior application delegate");
            NSApplication.sharedApplication.delegate = nil;
        }
    }
}

void startupCallbacksRespectLifetimeAndExistingEnvironment() {
    ForegroundEnvironment environment;
    for (const bool destroyBridge : {false, true}) {
        qunsetenv(kDisableForegroundTransform);
        auto reopen = std::make_unique<snow_shot::platform::macos::ApplicationReopenHandler>(
            [] { return true; });
        auto controller = std::make_unique<QObject>();
        int deliveries = 0;
        reopen->setStartupHandler(controller.get(), [&](bool) { ++deliveries; });
        requestDidFinishLaunching();
        require(qgetenv(kDisableForegroundTransform) == QByteArrayLiteral("1"),
                "background startup must suppress only Qt's pending automatic activation");
        if (destroyBridge) {
            reopen.reset();
        } else {
            controller.reset();
        }
        QCoreApplication::processEvents();
        require(
            deliveries == 0 && !qEnvironmentVariableIsSet(kDisableForegroundTransform),
            "destroyed startup contexts must not receive callbacks or retain temporary Qt state");
    }

    qputenv(kDisableForegroundTransform, QByteArrayLiteral("external-policy"));
    auto reopen =
        std::make_unique<snow_shot::platform::macos::ApplicationReopenHandler>([] { return true; });
    QObject controller;
    reopen->setStartupHandler(&controller, [](bool) {});
    requestDidFinishLaunching();
    QCoreApplication::processEvents();
    reopen.reset();
    require(qgetenv(kDisableForegroundTransform) == QByteArrayLiteral("external-policy"),
            "pre-existing Qt activation policy must not be overwritten or cleared");
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
        backgroundPermissionWaitsForAVisibleWindow();

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
        startupDecisionWaitsForNativeLaunch();
        startupCallbacksRespectLifetimeAndExistingEnvironment();
    }
    return 0;
}

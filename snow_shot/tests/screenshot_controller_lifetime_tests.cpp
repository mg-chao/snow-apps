#include "snow_shot/presentation/screenshotcontroller.h"
#include "snow_shot/presentation/directcapturecontroller.h"
#include "snow_shot/app/applicationcontroller.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/storage/applicationstorage.h"

#include "widgets/platform_compatibility.h"

#include <QApplication>
#include <QEvent>
#include <QStandardPaths>
#include <QTemporaryDir>

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

void directCaptureDestructionKeepsControllerAvailableToActivityObservers() {
    for (const bool explicitShutdown : {false, true}) {
        auto controller = std::make_unique<snow_shot::presentation::DirectCaptureController>();
        QObject observer;
        int inactiveNotifications = 0;
        auto* retiring = controller.get();
        QObject::connect(
            retiring, &snow_shot::presentation::DirectCaptureController::captureActivityChanged,
            &observer, [&](const QString&, bool active) {
                require(!active && !retiring->blocksApplicationUpdate(),
                        "direct capture shutdown observers can inspect the inactive controller");
                ++inactiveNotifications;
            });
        if (explicitShutdown) {
            controller->shutdown();
        }
        controller.reset();
        require(inactiveNotifications == 2,
                "direct capture shutdown must release each activity source exactly once");
    }
}
} // namespace

// Linked only into test builds to exercise the real controller and all its dependencies.
// Entered before singleton acquisition or diagnostics can access the user's configuration.
int runScreenshotControllerLifetimeTests(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    adqt::widgets::initializePlatformCompatibility(application);
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "controller lifetime tests require isolated storage");

    directCaptureDestructionKeepsControllerAvailableToActivityObservers();
    auto controller = std::make_unique<ScreenshotController>();
    require(!controller->blocksApplicationUpdate(), "idle capture initially permits handoff");
    controller->setCaptureSuspended(true);
    require(controller->blocksApplicationUpdate() && !controller->blocksApplicationUpdate(false),
            "own admission closure stays distinct from actual capture work in final validation");
    controller->setCaptureSuspended(false);
    QObject observer;
    int inactiveNotifications = 0;
    auto* retiring = controller.get();
    QObject::connect(
        retiring, &ScreenshotController::captureActivityChanged, &observer,
        [&](const QString&, bool active) {
            require(!active && !retiring->captureAcquisitionActive(),
                    "final capture activity observers can inspect the inactive controller");
            static_cast<void>(retiring->captureAvailable());
            static_cast<void>(retiring->blocksMemoryTrimming());
            ++inactiveNotifications;
        });
    controller.reset();
    require(inactiveNotifications > 0, "controller destruction must release capture activity");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    storage.shutdown();
    std::cout << "Screenshot controller lifetime tests passed\n";
    return 0;
}

int runOcrProxyStartupTests(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    adqt::widgets::initializePlatformCompatibility(application);
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "OCR proxy startup tests require isolated storage");
    const QString saved = QStringLiteral("http://user:p%40ss@127.0.0.1:7890");
    require(storage.configuration().setValues(
                {{QStringLiteral("network/proxy"), QStringLiteral("manual")},
                 {QStringLiteral("network/http_proxy"), saved},
                 {QStringLiteral("tray/enabled"), false}}) &&
                storage.flushNow().success,
            "save proxy preferences before application construction");
    storage.shutdown();
    require(storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "reload saved proxy settings as on application restart");
    {
        snow_shot::app::ApplicationController controller(application);
        auto* service = controller.findChild<ScreenshotOcrRecognitionService*>();
        require(service != nullptr && service->proxyUrl() == saved && service->processId() == 0,
                "application-owned OCR receives the saved proxy before it starts");
        require(controller.findChild<ScreenshotController*>() == nullptr,
                "OCR proxy initialization does not require a screenshot controller");
        {
            ScreenshotController consumer(nullptr, nullptr, service);
            require(service->proxyUrl() == saved,
                    "injecting the shared service preserves its proxy configuration");
        }
        require(storage.configuration().setValue(QStringLiteral("network/http_proxy"),
                                                 QStringLiteral("http://127.0.0.1:7891")) &&
                    service->proxyUrl() == QStringLiteral("http://127.0.0.1:7891"),
                "the proxy binding survives destruction of a screenshot consumer");
        require(storage.configuration().setValue(QStringLiteral("network/proxy"),
                                                 QStringLiteral("none")) &&
                    service->proxyUrl().isEmpty(),
                "application-owned OCR switches to direct access without a screenshot consumer");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    storage.shutdown();
    std::cout << "OCR proxy startup tests passed\n";
    return 0;
}

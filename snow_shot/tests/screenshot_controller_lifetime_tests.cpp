#include "snow_shot/presentation/screenshotcontroller.h"
#include "snow_shot/presentation/directcapturecontroller.h"
#include "snow_shot/app/applicationcontroller.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "screenshot_controller_test_access.h"
#include "snow_shot/presentation/screenshotselectionparams.h"

#include "widgets/platform_compatibility.h"

#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPointer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <functional>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

snow_shot::storage::StorageInitializationOptions isolatedStorageOptions(const QString& root) {
    snow_shot::storage::StorageInitializationOptions options{root, root, 60000};
    options.isolateAuxiliaryStorage = true;
    return options;
}

void drainRetiredCaptureSurfaces() {
    QVector<QPointer<QWidget>> retired;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (qobject_cast<ScreenshotOverlayWindow*>(widget) ||
            qobject_cast<ScreenshotPinnedWindow*>(widget))
            retired.append(widget);
    }
    // Controller shutdown releases native surfaces and queues QObject deletion. A local
    // processEvents loop in the next fixture does not drain DeferredDelete on its behalf.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    for (const auto& surface : retired)
        require(!surface,
                "retired controller surfaces must finish queued deletion before the next fixture");
}

ScreenshotPinnedWindow* fixturePin(const QString& id) {
    ScreenshotPinnedWindow* found = nullptr;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* pin = qobject_cast<ScreenshotPinnedWindow*>(widget);
            pin && pin->persistenceId() == id) {
            require(!found, "coalesced restoration cannot duplicate a persisted pin identity");
            found = pin;
        }
    }
    return found;
}

void reportRestoreFailure(const QString& id, const ScreenshotController& controller) {
    auto* pin = fixturePin(id);
    std::cerr << "restore fixture id=" << qPrintable(id)
              << " blocked=" << controller.blocksApplicationUpdate(false)
              << " matching_pin=" << (pin != nullptr);
    if (pin) {
        const auto image = pin->persistenceSnapshot().image;
        std::cerr
            << " visible=" << pin->isVisible() << " image=" << image.width() << 'x'
            << image.height() << " format=" << image.format() << " state="
            << QJsonDocument(pin->automationState()).toJson(QJsonDocument::Compact).constData();
    }
    const auto summary =
        snow_shot::storage::ApplicationStorage::instance().pinnedWindows().summary(id);
    if (summary)
        std::cerr << " stored_hidden=" << summary->hidden << " stored_pending=" << summary->pending
                  << " stored_ignored=" << summary->ignored;
    std::cerr << '\n';
}

void requireFixturePixels(const ScreenshotPinnedWindow& pin, const QImage& expected) {
    // Windows reloads persisted PNGs as BGRA; compare canonical pixel rows rather than
    // QImage storage format, padding, color-space metadata or the decoder's allocation owner.
    const auto actual = pin.persistenceSnapshot().image.convertToFormat(QImage::Format_RGBA8888);
    const auto reference = expected.convertToFormat(QImage::Format_RGBA8888);
    require(!actual.isNull() && actual.size() == reference.size(),
            "restored pin retains the complete original source dimensions");
    for (int row = 0; row != reference.height(); ++row)
        require(std::memcmp(actual.constScanLine(row), reference.constScanLine(row),
                            static_cast<std::size_t>(reference.width()) * 4) == 0,
                "restored pin retains every original canonical RGBA pixel");
}

void closeFixturePin(const QString& id) {
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* pin = qobject_cast<ScreenshotPinnedWindow*>(widget);
            pin && pin->persistenceId() == id)
            require(pin->close(), "close restored fixture before its storage is removed");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* pin = qobject_cast<ScreenshotPinnedWindow*>(widget))
            require(pin->persistenceId() != id, "restored fixture releases its native window");
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

class CapturePreparationModal final : public QWidget {
  public:
    std::function<void()> closing;
    bool acceptClose = true;

  protected:
    void closeEvent(QCloseEvent* event) override {
        if (acceptClose)
            hide();
        closing();
        event->setAccepted(acceptClose);
    }
};

void refusedPreparationPublishesCurrentAvailabilityAfterUnwinding() {
    enum class Entry { Action, GlobalMouse, Mcp };
    for (const auto entry : {Entry::Action, Entry::GlobalMouse, Entry::Mcp}) {
        bool lastAvailability = true;
        bool returned = false;
        int restoredNotifications = 0;
        auto controller = std::make_unique<ScreenshotController>();
        QObject::connect(controller.get(), &ScreenshotController::captureAvailabilityChanged,
                         controller.get(), [&](bool available) {
                             lastAvailability = available;
                             if (available) {
                                 require(returned,
                                         "availability observers run after capture callers unwind");
                                 ++restoredNotifications;
                             }
                         });
        CapturePreparationModal modal;
        modal.acceptClose = false;
        modal.setWindowModality(Qt::ApplicationModal);
        modal.closing = [&] {
            controller->setCaptureSuspended(true);
            controller->setCaptureSuspended(false);
        };
        modal.show();
        require(QApplication::activeModalWidget() == &modal,
                "refused preparation owns modal input");
        bool accepted = true;
        if (entry == Entry::Action) {
            accepted = controller->captureForAction(ScreenshotController::CaptureAction::Copy);
        } else if (entry == Entry::GlobalMouse) {
            accepted = controller->beginGlobalMouseCapture(
                snow_shot::presentation::settings::SettingsGlobalMouseAction::ScreenshotCopy, 1,
                QPointF(2, 3));
        } else {
            QString error;
            accepted = controller->mcpBegin({}, &error);
            require(error == QStringLiteral("capture_unavailable"), "refused MCP preparation");
        }
        returned = true;
        require(!accepted && modal.isVisible() && controller->captureAvailable() &&
                    !controller->captureAcquisitionActive(),
                "a refused blocker releases preparation without starting native capture");
        QCoreApplication::processEvents();
        require(lastAvailability && restoredNotifications == 1,
                "refused preparation restores availability for subscribed input producers");
    }
}

void preparationAvailabilityRefreshIsCurrentAndLifetimeBound() {
    bool lastAvailability = true;
    auto controller = std::make_unique<ScreenshotController>();
    QObject::connect(controller.get(), &ScreenshotController::captureAvailabilityChanged,
                     controller.get(), [&](bool available) { lastAvailability = available; });
    CapturePreparationModal modal;
    modal.acceptClose = false;
    modal.setWindowModality(Qt::ApplicationModal);
    modal.closing = [&] {
        controller->setCaptureSuspended(true);
        controller->setCaptureSuspended(false);
    };
    modal.show();
    require(!controller->captureForAction(ScreenshotController::CaptureAction::Copy),
            "queue an availability refresh after refused preparation");
    controller->setCaptureSuspended(true);
    QCoreApplication::processEvents();
    require(!lastAvailability && !controller->captureAvailable(),
            "an obsolete idle snapshot cannot reopen a subsequently suspended controller");
    controller->setCaptureSuspended(false);
    require(lastAvailability, "ordinary suspension restoration still publishes immediately");

    require(!controller->captureForAction(ScreenshotController::CaptureAction::Copy),
            "queue another refresh before owner retirement");
    controller.reset();
    QCoreApplication::processEvents();
    require(!controller, "queued availability publication cannot retain its retired controller");

    controller = std::make_unique<ScreenshotController>();
    QObject::connect(controller.get(), &ScreenshotController::captureAvailabilityChanged,
                     controller.get(), [&](bool available) {
                         if (available)
                             controller.reset();
                     });
    require(!controller->captureForAction(ScreenshotController::CaptureAction::Copy),
            "refused capture returns before availability observers can retire its controller");
    QCoreApplication::processEvents();
    require(!controller, "restored availability safely permits observer-driven retirement");
}

void capturePreparationReservesAdmissionAndRechecksAfterBlockers() {
    int selectionStarts = 0;
    int delayedStarts = 0;
    ScreenshotController controller;
    QObject::connect(&controller, &ScreenshotController::captureActivityChanged, &controller,
                     [&](const QString& source, bool active) {
                         if (!active)
                             return;
                         if (source == QStringLiteral("selection")) {
                             ++selectionStarts;
                             // Keep this characterization isolated even if admission regresses:
                             // a declined workflow must never invoke native desktop capture.
                             controller.setCaptureSuspended(true);
                         }
                         if (source == QStringLiteral("delay"))
                             ++delayedStarts;
                     });
    CapturePreparationModal modal;
    modal.setWindowModality(Qt::ApplicationModal);
    bool nestedAccepted = true;
    bool preparationBlocked = false;
    modal.closing = [&] {
        preparationBlocked =
            !controller.captureAvailable() && controller.blocksApplicationUpdate(false) &&
            controller.blocksMemoryTrimming() && controller.blocksMemoryTrimming(false);
        nestedAccepted = controller.captureForAction(ScreenshotController::CaptureAction::Copy);
        controller.startDelayedCapture(1);
        controller.setCaptureSuspended(true);
    };
    modal.show();
    require(QApplication::activeModalWidget() == &modal,
            "capture preparation fixture owns modal input");
    const bool accepted = controller.captureForAction(ScreenshotController::CaptureAction::None);
    require(
        !accepted && !nestedAccepted && preparationBlocked && selectionStarts == 0 &&
            delayedStarts == 0 && !controller.captureAcquisitionActive(),
        "blocker close reserves admission, rejects nested/new delay work and honors suspension");
    controller.setCaptureSuspended(false);
    require(controller.captureAvailable(),
            "failed preparation releases its reservation for the next independent request");
    controller.setCaptureSuspended(true);
    require(controller.blocksMemoryTrimming() && !controller.blocksMemoryTrimming(false),
            "final Store validation excludes only its own pause on an otherwise idle controller");
    controller.setCaptureSuspended(false);
}

void capturePreparationCannotReplaceWorkCreatedWhileClosingABlocker() {
    ScreenshotController controller;
    CapturePreparationModal modal;
    modal.setWindowModality(Qt::ApplicationModal);
    QImage image(24, 24, QImage::Format_RGBA8888);
    image.fill(Qt::blue);
    ScreenshotSelectionParams selection;
    selection.selection = QRect(2, 3, 8, 9);
    modal.closing = [&] {
        ScreenshotControllerTestAccess::prepareSelection(controller, image, selection);
    };
    modal.show();
    require(!controller.captureForAction(ScreenshotController::CaptureAction::Copy) &&
                controller.mcpState().value(QStringLiteral("capture_phase")).toString() ==
                    QStringLiteral("editing") &&
                controller.blocksMemoryTrimming(false),
            "final admission preserves an editor established by blocker close callbacks");
}

void capturePreparationRechecksAfterActivityObservers() {
    int starts = 0;
    int retires = 0;
    ScreenshotController controller;
    QObject::connect(&controller, &ScreenshotController::captureActivityChanged, &controller,
                     [&](const QString& source, bool active) {
                         if (source != QStringLiteral("selection"))
                             return;
                         if (active) {
                             ++starts;
                             controller.setCaptureSuspended(true);
                         } else {
                             ++retires;
                         }
                     });
    require(!controller.captureForAction(ScreenshotController::CaptureAction::Copy) &&
                starts == 1 && retires == 1 && !controller.captureAcquisitionActive(),
            "activity observer suspension declines before native dispatch and retires activity");
    controller.setCaptureSuspended(false);
    require(controller.captureAvailable(),
            "activity observer refusal releases the preparation reservation");
}

void capturePreparationSurvivesControllerRetirement() {
    enum class Entry { Action, GlobalMouse, Mcp };
    for (const auto entry : {Entry::Action, Entry::GlobalMouse, Entry::Mcp}) {
        auto controller = std::make_unique<ScreenshotController>();
        auto* retiring = controller.get();
        CapturePreparationModal modal;
        modal.setWindowModality(Qt::ApplicationModal);
        modal.closing = [&] { controller.reset(); };
        modal.show();
        require(QApplication::activeModalWidget() == &modal,
                "retirement fixture closes the blocker before any native capture dispatch");
        bool accepted = true;
        if (entry == Entry::Action) {
            accepted = retiring->captureForAction(ScreenshotController::CaptureAction::Copy);
        } else if (entry == Entry::GlobalMouse) {
            accepted = retiring->beginGlobalMouseCapture(
                snow_shot::presentation::settings::SettingsGlobalMouseAction::ScreenshotCopy, 1,
                QPointF(2, 3));
        } else {
            QString error;
            accepted = retiring->mcpBegin({}, &error);
            require(error == QStringLiteral("capture_unavailable"),
                    "MCP reports preparation refusal after its controller is retired");
        }
        require(!accepted && !controller,
                "each capture caller survives blocker-driven retirement without trailing access");
    }
}

void delayedPresentationWaitsForSuspensionRestoration() {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.configuration().setValue(
                QStringLiteral("pin_to_screen/automatic_text_recognition"), false),
            "disable recognition in the isolated restore fixture");
    snow_shot::storage::PinnedWindowRecord record;
    record.id = QStringLiteral("22222222-2222-4222-8222-222222222222");
    record.image = QImage(8, 8, QImage::Format_RGBA8888);
    record.image.fill(Qt::blue);
    record.nativeGeometry = QRect(0, 0, 8, 8);
    record.canvasSourceRect = QRectF(record.nativeGeometry);
    record.contentCanvasRect = record.canvasSourceRect;
    record.surfaceCanvasRect = record.canvasSourceRect;
    record.initialWindowSize = record.image.size();
    require(storage.pinnedWindows().upsert(record).success &&
                storage.pinnedWindows().flush().success,
            "persist isolated content for delayed restoration");
    {
        ScreenshotController controller;
        controller.prewarmResources();
        controller.restorePinnedWindows();
        controller.setCaptureSuspended(true);
        controller.restoreActivePinnedGroupWindows();
        for (int i = 0; i != 12; ++i)
            QCoreApplication::processEvents();
        for (auto* widget : QApplication::topLevelWidgets()) {
            require(!qobject_cast<ScreenshotOverlayWindow*>(widget) &&
                        !qobject_cast<ScreenshotPinnedWindow*>(widget),
                    "queued prewarm and restore cannot construct presentation while suspended");
        }
        controller.setCaptureSuspended(false);
        QElapsedTimer timer;
        timer.start();
        QPointer<ScreenshotPinnedWindow> restored;
        while (!restored && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            if (auto* pin = fixturePin(record.id);
                pin && pin->isVisible() &&
                pin->automationState().value(QStringLiteral("ready")).toBool() &&
                !controller.blocksApplicationUpdate(false))
                restored = pin;
            if (!restored)
                QThread::msleep(1);
        }
        if (!restored)
            reportRestoreFailure(record.id, controller);
        require(restored, "resuming retains and presents the deferred persisted pin");
        requireFixturePixels(*restored, record.image);
        closeFixturePin(record.id);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(storage.pinnedWindows().remove(record.id).success,
            "remove completed isolated restoration fixture");
}

void acceptedRestorationBlocksApplicationTransitionsUntilPresentationSettles() {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    snow_shot::storage::PinnedWindowRecord record;
    record.id = QStringLiteral("33333333-3333-4333-8333-333333333333");
    record.image = QImage(8, 8, QImage::Format_RGBA8888);
    record.image.fill(Qt::green);
    record.nativeGeometry = QRect(0, 0, 8, 8);
    record.canvasSourceRect = QRectF(record.nativeGeometry);
    record.contentCanvasRect = record.canvasSourceRect;
    record.surfaceCanvasRect = record.canvasSourceRect;
    record.initialWindowSize = record.image.size();
    require(storage.pinnedWindows().upsert(record).success &&
                storage.pinnedWindows().flush().success,
            "persist isolated content for accepted restoration");
    {
        ScreenshotController controller;
        require(!controller.blocksApplicationUpdate() && !controller.blocksMemoryTrimming(),
                "an idle controller initially permits transitions");
        controller.showPinnedRecord(record.id);
        // No event delivery follows admission: even an already-finished load still owns a
        // queued GUI/native presentation, which a pool drain cannot retire on its behalf.
        require(controller.blocksApplicationUpdate() && controller.blocksMemoryTrimming(),
                "accepted restore blocks transitions before its queued presentation completes");
        controller.setCaptureSuspended(true);
        require(controller.blocksApplicationUpdate(false),
                "excluding a pause reservation does not hide an accepted restore");
        controller.setCaptureSuspended(false);
        QElapsedTimer timer;
        timer.start();
        while (controller.blocksApplicationUpdate() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            if (controller.blocksApplicationUpdate())
                QThread::msleep(1);
        }
        auto* restored = fixturePin(record.id);
        const bool ready = restored && restored->isVisible() &&
                           restored->automationState().value(QStringLiteral("ready")).toBool();
        if (!ready || controller.blocksApplicationUpdate())
            reportRestoreFailure(record.id, controller);
        require(ready && !controller.blocksApplicationUpdate() &&
                    !controller.blocksMemoryTrimming(),
                "restoration releases its blocker only after actual native presentation settles");
        requireFixturePixels(*restored, record.image);
        closeFixturePin(record.id);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(storage.pinnedWindows().remove(record.id).success,
            "remove completed accepted restoration fixture");
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
                storage.initialize(isolatedStorageOptions(temporary.path())).success,
            "controller lifetime tests require isolated storage");

    directCaptureDestructionKeepsControllerAvailableToActivityObservers();
    refusedPreparationPublishesCurrentAvailabilityAfterUnwinding();
    drainRetiredCaptureSurfaces();
    preparationAvailabilityRefreshIsCurrentAndLifetimeBound();
    drainRetiredCaptureSurfaces();
    capturePreparationReservesAdmissionAndRechecksAfterBlockers();
    drainRetiredCaptureSurfaces();
    capturePreparationCannotReplaceWorkCreatedWhileClosingABlocker();
    drainRetiredCaptureSurfaces();
    capturePreparationRechecksAfterActivityObservers();
    drainRetiredCaptureSurfaces();
    capturePreparationSurvivesControllerRetirement();
    drainRetiredCaptureSurfaces();
    delayedPresentationWaitsForSuspensionRestoration();
    acceptedRestorationBlocksApplicationTransitionsUntilPresentationSettles();
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
            require(!retiring->captureAvailable(),
                    "shutdown closes capture admission before notifying activity observers");
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
                storage.initialize(isolatedStorageOptions(temporary.path())).success,
            "OCR proxy startup tests require isolated storage");
    const QString saved = QStringLiteral("http://user:p%40ss@127.0.0.1:7890");
    require(storage.configuration().setValues(
                {{QStringLiteral("network/proxy"), QStringLiteral("manual")},
                 {QStringLiteral("network/http_proxy"), saved},
                 {QStringLiteral("tray/enabled"), false}}) &&
                storage.flushNow().success,
            "save proxy preferences before application construction");
    storage.shutdown();
    require(storage.initialize(isolatedStorageOptions(temporary.path())).success,
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

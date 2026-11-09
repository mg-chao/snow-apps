#include "snow_shot/app/applicationcontroller.h"
#include "snow_shot/app/mcp/screenshotmcpserver.h"
#include "snow_shot/app/mcp/screenshotmcpsession.h"
#include "snow_shot/app/mcp/mcpdocumentservice.h"
#include "snow_shot/app/mcp/mcpjobregistry.h"
#include "snow_shot/app/mcp/mcpruntimegraph.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/update/updateservice.h"
#include "widgets/platform_compatibility.h"
#ifdef Q_OS_WIN
#include "snow_shot/app/shellimagelaunch.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#endif

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QLockFile>
#include <QImage>
#include <QKeyEvent>
#include <QPointer>
#include <QScopeGuard>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <iostream>
#include <stdexcept>

int runMcpApplicationLifecycleTests(int argc, char** argv);

namespace {
class TestApplication final : public QApplication {
  public:
    using QApplication::QApplication;

    int quitObserverCount() const {
        return receivers(SIGNAL(aboutToQuit()));
    }
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <typename Predicate> void await(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), "MCP application lifecycle transition completes");
}
#ifdef Q_OS_WIN
class InputProbe final : public QObject {
  public:
    bool received = false;
    bool event(QEvent* event) override {
        if (event->type() == QEvent::KeyPress) {
            received = true;
            return true;
        }
        return QObject::event(event);
    }
};

QList<ScreenshotPinnedWindow*> visiblePins() {
    QList<ScreenshotPinnedWindow*> pins;
    for (auto* window : QApplication::topLevelWidgets()) {
        auto* pin = qobject_cast<ScreenshotPinnedWindow*>(window);
        if (pin && pin->isVisible() && !pin->persistenceSnapshot().image.isNull())
            pins.append(pin);
    }
    return pins;
}
#endif
} // namespace

// Linked only into test builds, before startup can access the user's configuration.
int runMcpApplicationLifecycleTests(int argc, char** argv) try {
    using namespace snow_shot;
    using namespace snow_shot::app::mcp;
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated MCP lifecycle storage");
    qputenv("SNOW_SHOT_MCP_DESCRIPTOR",
            temporary.filePath(QStringLiteral("descriptor.json")).toUtf8());
    TestApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    adqt::widgets::initializePlatformCompatibility(application);
    auto& storage = storage::ApplicationStorage::instance();
    require(storage
                .initialize({temporary.filePath(QStringLiteral("bin")),
                             temporary.filePath(QStringLiteral("data")), 60000})
                .success,
            "initialize MCP lifecycle storage");
    auto& configuration = storage.configuration();
    require(configuration.setValues(
                {{QStringLiteral("mcp/enabled"), true},
                 {QStringLiteral("tray/enabled"), false},
                 {QStringLiteral("pin_to_screen/automatic_text_recognition"), false},
                 {QStringLiteral("screenshot/copy_after_pinning_to_screen"), false}}),
            "enable MCP without a tray");
    presentation::LanguageManager::instance().initialize();
    require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("en_US")),
            "start the isolated failure fixture in English");
    presentation::styles::ThemeManager::instance().initialize(application);
    {
        QLockFile endpointLock(temporary.filePath(QStringLiteral("endpoint.lock")));
        require(endpointLock.tryLock(0), "fixture reserves only its isolated MCP endpoint");
        app::ApplicationController controller(application);
        auto* runtime = controller.findChild<McpRuntimeGraph*>();
        auto* initiallyUnavailable = controller.findChild<ScreenshotMcpServer*>();
        require(runtime && initiallyUnavailable && !initiallyUnavailable->isRunning() &&
                    !controller.findChild<ScreenshotMcpSession*>() &&
                    !controller.findChild<McpDocumentService*>() &&
                    !controller.findChild<McpJobRegistry*>() && !runtime->busy() &&
                    initiallyUnavailable->startupError() == McpStartupError::EndpointOwned &&
                    runtime->startupError() == McpStartupError::EndpointOwned &&
                    application.property("snowShotMcpStartError").value<McpStartupError>() ==
                        McpStartupError::EndpointOwned &&
                    configuration.value(QStringLiteral("mcp/enabled")).toBool(),
                "endpoint failure preserves desired enable but retires the partial service graph");
        int failedRuntimeStarts = 0;
        const auto failureConnection = QObject::connect(
            initiallyUnavailable, &ScreenshotMcpServer::runningChanged, &controller,
            [&](bool running) { failedRuntimeStarts += running ? 1 : 0; });
        for (const auto& locale :
             {QStringLiteral("zh_CN"), QStringLiteral("zh_TW"), QStringLiteral("en_US")}) {
            require(presentation::LanguageManager::instance().setLanguage(locale),
                    "change language while the failed runtime stays enabled");
            QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
            const auto error =
                application.property("snowShotMcpStartError").value<McpStartupError>();
            const QString message = mcpStartupErrorText(error);
            const QString expected =
                locale == u"zh_CN" ? QStringLiteral("另一个 Snow Shot 实例正在使用 MCP 端点")
                : locale == u"zh_TW"
                    ? QStringLiteral("另一個 Snow Shot 執行個體正在使用 MCP 端點")
                    : QStringLiteral("Another Snow Shot instance owns the MCP endpoint");
            require(error == McpStartupError::EndpointOwned && message == expected &&
                        !initiallyUnavailable->isRunning() && failedRuntimeStarts == 0,
                    "real catalogs retranslate retained startup failures without restarting MCP");
        }
        QObject::disconnect(failureConnection);
        endpointLock.unlock();
        require(configuration.setValue(QStringLiteral("mcp/enabled"), false),
                "disable the failed runtime before explicit retry");
        QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
        require(application.property("snowShotMcpStartError").value<McpStartupError>() ==
                    McpStartupError::None,
                "disable clears the previous startup failure");
        require(configuration.setValue(QStringLiteral("mcp/enabled"), true),
                "retry through the existing enable setting after endpoint recovery");
        await([&] { return initiallyUnavailable->isRunning(); });
        require(controller.findChild<ScreenshotMcpSession*>() &&
                    controller.findChild<McpDocumentService*>() &&
                    initiallyUnavailable->startupError() == McpStartupError::None &&
                    application.property("snowShotMcpStartError").value<McpStartupError>() ==
                        McpStartupError::None,
                "retry constructs a fresh graph and publishes actual availability");
        const int quitObservers = application.quitObserverCount();
        for (int iteration = 0; iteration < 3; ++iteration) {
            require(application.quitObserverCount() == quitObservers,
                    "MCP restarts do not accumulate application quit observers");
            auto* server = controller.findChild<ScreenshotMcpServer*>();
            require(server && server->isRunning(), "MCP server starts or restarts");
            QPointer<ScreenshotMcpSession> session = controller.findChild<ScreenshotMcpSession*>();
            require(session, "a running MCP server owns a screenshot session");
            ScreenshotMcpRequest request;
            request.connectionId = 1;
            request.requestId = QStringLiteral("status");
            request.method = QStringLiteral("snow_shot_mcp_status");
            bool completed = false;
            session->request(request, [&](auto response) {
                require(response.ok, "screenshot tools remain available after MCP restarts");
                completed = true;
            });
            require(completed, "session status completes synchronously");
            require(configuration.setValue(QStringLiteral("mcp/enabled"), false),
                    "disable MCP through application configuration");
            await([&] { return !server->isRunning(); });
            require(!session, "stopping MCP retires the terminal screenshot session");
            require(configuration.setValue(QStringLiteral("mcp/enabled"), true),
                    "re-enable MCP through application configuration");
            await([&] { return server->isRunning(); });
        }

        // Process only the controller's queued configuration callbacks. The listener's drain
        // completion targets the application and cannot retire this graph between these steps.
        auto* server = controller.findChild<ScreenshotMcpServer*>();
        QPointer<ScreenshotMcpSession> oldSession = controller.findChild<ScreenshotMcpSession*>();
        QPointer<McpDocumentService> oldDocuments = controller.findChild<McpDocumentService*>();
        QPointer<McpJobRegistry> oldJobs = controller.findChild<McpJobRegistry*>();
        require(server && oldSession && oldDocuments && oldJobs && !runtime->busy(),
                "MCP graph exists and has no reserved work before interleaving");
        ScreenshotMcpRequest delayed;
        delayed.connectionId = 17;
        delayed.requestId = QStringLiteral("delayed-capture-before-disable");
        delayed.idempotencyKey = delayed.requestId;
        delayed.method = QStringLiteral("snow_shot_document_open");
        delayed.params = {{QStringLiteral("source"), QStringLiteral("capture")},
                          {QStringLiteral("delay_seconds"), 10}};
        bool admitted = false;
        oldDocuments->request(delayed, [&](auto response) {
            require(response.ok && response.result.contains(QStringLiteral("job_id")),
                    "delayed capture is admitted as a cancellable job without native acquisition");
            admitted = true;
        });
        require(admitted, "delayed source admission returns before execution");
        require(runtime->busy(),
                "graph admission accounts for a delayed source through its running job");
#ifdef Q_OS_WIN
        require(
            !storage.requestDirectoryChange(temporary.filePath(QStringLiteral("busy-target")), true)
                    .success &&
                !storage.directoryChanging() && server->isRunning() && oldSession,
            "pending delayed job vetoes relocation without pausing live services");
#endif
        require(configuration.setValue(QStringLiteral("mcp/enabled"), false),
                "disable with delayed source pending");
        QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
        require(server->isRunning() && oldSession,
                "disable starts asynchronous drain before disposing the old graph");
        require(configuration.setValue(QStringLiteral("mcp/enabled"), true),
                "re-enable before old graph has drained");
        QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
        require(oldSession && oldDocuments,
                "re-enable does not reuse or prematurely dispose the draining graph");
        await([&] { return !oldSession && !oldDocuments && !oldJobs && server->isRunning(); });
        require(controller.findChild<ScreenshotMcpSession*>() &&
                    controller.findChild<McpDocumentService*>() &&
                    controller.findChild<McpJobRegistry*>() && !runtime->busy() &&
                    application.quitObserverCount() == quitObservers,
                "drain completion cancels delayed sources and starts a fresh service graph once");

#ifdef Q_OS_WIN
        // Synchronous suspension publishes signals before relocation owns any storage state.
        // New work admitted by such an observer must veto the final preparation check.
        {
            auto gate = std::make_shared<QSemaphore>();
            const auto release = qScopeGuard([gate] { gate->release(); });
            auto& exports = ScreenshotExportCoordinator::shared();
            require(exports.pendingJobCount() == 0,
                    "relocation initially observes no export reservation");
            ScreenshotExportJobHandle lateExport;
            bool injected = false;
            bool completed = false;
            bool inputClosed = false;
            InputProbe input;
            const auto connection = QObject::connect(
                server, &ScreenshotMcpServer::runningChanged, &controller, [&](bool running) {
                    if (running || injected)
                        return;
                    injected = true;
                    QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
                    QCoreApplication::sendEvent(&input, &key);
                    inputClosed = !input.received;
                    lateExport = exports.submit(
                        &controller, ScreenshotExportCoordinator::Priority::Foreground,
                        [gate](const auto&) {
                            gate->acquire();
                            return ScreenshotExportTaskResult{};
                        },
                        [&](auto result) {
                            require(result.succeeded(),
                                    "independently admitted export survives relocation rejection");
                            completed = true;
                        });
                });
            const QString originalRoot = storage.configurationDirectory();
            const auto refused = storage.requestDirectoryChange(
                temporary.filePath(QStringLiteral("late-export-target")), true);
            QObject::disconnect(connection);
            require(
                !refused.success && injected && inputClosed && lateExport.isValid() &&
                    exports.pendingJobCount() == 1 && !completed && !storage.directoryChanging() &&
                    storage.configurationDirectory() == originalRoot && server->isRunning() &&
                    !runtime->busy(),
                "new work during suspension rejects relocation and restores the original graph");
            QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
            QCoreApplication::sendEvent(&input, &key);
            require(input.received, "rejected preparation restores input admission");
            gate->release();
            await([&] { return completed && exports.pendingJobCount() == 0; });
        }

        // A settings mutation can still be queued when a synchronous storage move begins.
        // Restore must read its desired value before reopening listener admission.
        int unexpectedStarts = 0;
        int firstFileAttempts = 0;
        bool shellInjected = false;
        bool pinPresentedDuringPreparation = false;
        const QString shellPath =
            temporary.filePath(QStringLiteral("shell-during-preparation.png"));
        QImage shellImage(32, 24, QImage::Format_RGBA8888);
        shellImage.fill(QColor(46, 91, 172));
        require(shellImage.save(shellPath, "PNG"), "write isolated deferred shell file");
        const auto runningConnection = QObject::connect(
            server, &ScreenshotMcpServer::runningChanged, &controller, [&](bool running) {
                unexpectedStarts += running ? 1 : 0;
                if (!running && !shellInjected) {
                    shellInjected = true;
                    require(!storage.directoryChanging(),
                            "listener shutdown reenters launch before storage publishes moving");
                    controller.handleLaunchRequest(app::shellImageLaunchArguments({shellPath}),
                                                   [&] { ++firstFileAttempts; });
                    pinPresentedDuringPreparation = !visiblePins().isEmpty();
                }
            });
        require(configuration.setValue(QStringLiteral("mcp/enabled"), false),
                "queue disable immediately before relocation");
        const QString relocatedRoot = temporary.filePath(QStringLiteral("relocated"));
        require(storage.requestDirectoryChange(relocatedRoot, true).success &&
                    storage.directoryChanging() && !server->isRunning(),
                "relocation synchronously closes MCP admission before worker launch");
        require(shellInjected && firstFileAttempts == 1 && !pinPresentedDuringPreparation &&
                    visiblePins().isEmpty(),
                "preparation preserves shell intent without pin presentation or repeated callback");
        require(!configuration.setValue(QStringLiteral("mcp/enabled"), true),
                "configuration writes remain frozen during relocation");
        storage.waitForDirectoryChange();
        require(!storage.directoryChanging() && storage.configurationDirectory() == relocatedRoot &&
                    !server->isRunning() && unexpectedStarts == 0 &&
                    !configuration.value(QStringLiteral("mcp/enabled")).toBool(),
                "resume respects latest disabled policy without a transient listener restart");
        QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
        require(!server->isRunning() && unexpectedStarts == 0,
                "delayed configuration reconciliation cannot reopen disabled MCP");
        await([&] {
            const auto pins = visiblePins();
            return pins.size() == 1 &&
                   pins.first()->automationState().value(QStringLiteral("ready")).toBool();
        });
        auto* replayedPin = visiblePins().first();
        const auto samePixels = [](const QImage& image, const QImage& expected) {
            return image.convertToFormat(QImage::Format_ARGB32_Premultiplied) ==
                   expected.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        };
        require(firstFileAttempts == 1 &&
                    samePixels(replayedPin->persistenceSnapshot().image, shellImage) &&
                    storage.configurationDirectory() == relocatedRoot,
                "accepted file replays exactly once with original pixels after root commitment");
        replayedPin->checkpointStorage();
        require(storage.pinnedWindows().flush().success,
                "replayed file checkpoint flushes through relocated repository");
        const auto persisted = storage.pinnedWindows().loadRecord(replayedPin->persistenceId());
        require(persisted && samePixels(persisted->image, shellImage),
                "replayed pin persists on the authoritative storage root");
        replayedPin->close();
        await([] { return visiblePins().isEmpty(); });
        require(configuration.setValue(QStringLiteral("mcp/enabled"), true),
                "enable MCP after resources have rebound to relocated storage");
        await([&] { return server->isRunning(); });
        require(unexpectedStarts == 1 && controller.findChild<ScreenshotMcpSession*>(),
                "post-relocation enable starts one fresh graph on the authoritative root");
        QObject::disconnect(runningConnection);
#endif

        controller.showMainWindow();
        MainWindow* mainWindow = nullptr;
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* window = qobject_cast<MainWindow*>(widget))
                mainWindow = window;
        require(mainWindow, "isolated main window exposes the real ingress signal bindings");
        storage::PinnedWindowRecord protectedPin;
        protectedPin.id = QStringLiteral("22222222-2222-4222-8222-222222222222");
        protectedPin.image = QImage(8, 8, QImage::Format_RGBA8888);
        protectedPin.image.fill(Qt::blue);
        protectedPin.nativeGeometry = QRect(0, 0, 8, 8);
        protectedPin.canvasSourceRect = QRectF(0, 0, 8, 8);
        protectedPin.contentCanvasRect = protectedPin.canvasSourceRect;
        protectedPin.surfaceCanvasRect = protectedPin.canvasSourceRect;
        protectedPin.initialWindowSize = protectedPin.image.size();
        require(storage.pinnedWindows().upsert(protectedPin).success &&
                    storage.pinnedWindows().flush().success,
                "persist an isolated pin for suspended show/delete requests");
        const auto compositionOwnerCount = [&] {
            int count = 0;
            for (auto* object : controller.findChildren<QObject*>()) {
                const QByteArray name = object->metaObject()->className();
                if (name.endsWith("ImageEditingWindowCoordinator") ||
                    name.endsWith("ImageStitchingWindowCoordinator") ||
                    name.endsWith("ImageSplitWindowCoordinator"))
                    ++count;
            }
            return count;
        };
        const int initialCompositionOwners = compositionOwnerCount();
        const auto pausedIngress = [&] {
            storage::CaptureHistoryRecord record;
            record.id = QStringLiteral("paused-history");
            record.contentKind = storage::CaptureHistoryContentKind::Image;
            mainWindow->screenshotHistoryImageEditingRequested(record);
            mainWindow->screenshotHistoryStitchImagesRequested({record});
            mainWindow->screenshotHistorySplitRequested(record);
            mainWindow->pinnedWindowImageEditingRequested(protectedPin.id);
            mainWindow->pinnedWindowStitchImagesRequested({protectedPin.id});
            mainWindow->pinnedWindowSplitRequested(protectedPin.id);
            storage.pinnedWindowShowRequested(protectedPin.id);
            storage.pinnedWindowsShowRequested({protectedPin.id});
            storage.pinnedWindowDeleteRequested({protectedPin.id});
            require(compositionOwnerCount() == initialCompositionOwners &&
                        storage.pinnedWindows().summary(protectedPin.id).has_value(),
                    "paused real bindings refuse document creation and destructive pin ingress");
#ifdef Q_OS_WIN
            require(visiblePins().isEmpty(), "paused show signals cannot create a live pin");
#endif
        };

        // Drive the controller's committed transition without launching a native updater.
        // The helper protocol/authority is covered separately by the adapter fixtures.
        auto* updates =
            application.findChild<update::UpdateService*>(QStringLiteral("snowShotUpdateService"));
        require(updates && server->isRunning(), "running services exist before committed handoff");
        bool suspendedWithoutListener = false;
        bool watchdogFired = false;
        int listenerRestarts = 0;
        const auto committedRunningConnection =
            QObject::connect(server, &ScreenshotMcpServer::runningChanged, &controller,
                             [&](bool running) { listenerRestarts += running ? 1 : 0; });
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &application, [&] {
            watchdogFired = true;
            application.exit(79); // Direct exit ends a failed test even if Quit is still filtered.
        });
        watchdog.start(3000);
        QTimer::singleShot(0, &application, [&] {
            updates->handoffPendingChanged(true);
            suspendedWithoutListener = !server->isRunning();
            pausedIngress();
            updates->handoffCommitted();
        });
        const int quitResult = application.exec();
        watchdog.stop();
        QObject::disconnect(committedRunningConnection);
        require(quitResult == 0 && !watchdogFired && suspendedWithoutListener &&
                    listenerRestarts == 0 && !server->isRunning(),
                "committed handoff permits terminal Quit while keeping feature resources closed");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    storage.shutdown();
    std::cout << "MCP application lifecycle tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}

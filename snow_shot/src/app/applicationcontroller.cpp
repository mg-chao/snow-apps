#include "snow_shot/app/applicationcontroller.h"
#include "snow_shot/app/applicationrestart.h"
#include "snow_shot/app/featureavailability.h"
#include "snow_shot/presentation/apppermissionservice.h"
#ifdef Q_OS_MACOS
#include "snow_shot/platform/macos/applicationactivation.h"
#include "snow_shot/presentation/permissionguidecontroller.h"
#endif
#include "snow_shot/platform/windows/administratorlaunch.h"
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/update/updateservice.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QMessageBox>

#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/globalmousemanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/screenshotcontroller.h"
#include "snow_shot/presentation/screenshotautofiltercontroller.h"
#include "snow_shot/presentation/directcapturecontroller.h"
#include "snow_shot/presentation/selectedtexttranslationcoordinator.h"
#include "snow_shot/presentation/selectedtexttranslationcontroller.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenrecordingfolder.h"
#include "snow_shot/presentation/systemtraycontroller.h"
#include "snow_shot/app/mcp/screenshotmcpserver.h"
#include "snow_shot/app/mcp/screenshotmcpsession.h"
#include "snow_shot/app/mcp/mcpapplicationservice.h"
#include "snow_shot/app/mcp/mcpdocumentservice.h"
#include "snow_shot/app/mcp/mcpjobregistry.h"
#include "snow_shot/app/mcp/mcpmediaservice.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshothistorytypes.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/platform/selectedfiles.h"
#include "snow_shot/platform/screenshotnative.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "widgets/message.h"

#include <QApplication>
#include <QClipboard>
#include <QFutureWatcher>
#include "mcp/mcpasyncwork_p.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QtMath>
#include <QUuid>

#include <memory>
#include <atomic>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace snow_shot::app {
namespace {
const QString kPinBorderColorKey = QStringLiteral("pin_to_screen/border_color");
const QString kPinBorderActiveColorKey = QStringLiteral("pin_to_screen/border_active_color");
const QString kTrayEnabledKey = QStringLiteral("tray/enabled");
const QString kTrayIconKey = QStringLiteral("tray/icon");
const QString kTrayCustomIconKey = QStringLiteral("tray/custom_icon");
const QString kTrayLeftClickActionKey = QStringLiteral("tray/left_click_action");
const QString kTrayMiddleClickActionKey = QStringLiteral("tray/middle_click_action");
const QString kTrayMenuOptionsKey = QStringLiteral("tray/menu_options");
const QString kScreenshotDelaySecondsKey = QStringLiteral("screenshot/delay_seconds");
const QString kFullscreenSuppressionKey =
    QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window");
const QString kOcrModelTypeKey = QStringLiteral("text_recognition/model_type");
const QString kOcrDirectMlKey = QStringLiteral("text_recognition/direct_ml_acceleration");
const QString kMcpEnabledKey = QStringLiteral("mcp/enabled");

QStringList stringList(const QJsonValue& value) {
    QStringList result;
    for (const QJsonValue& item : value.toArray()) {
        result.push_back(item.toString());
    }
    return result;
}

storage::PinnedWindowRepository* initializedPinnedWindowRepository() {
    auto& applicationStorage = storage::ApplicationStorage::instance();
    if (!applicationStorage.isInitialized()) {
        static_cast<void>(applicationStorage.initialize());
    }
    return applicationStorage.isInitialized() ? &applicationStorage.pinnedWindows() : nullptr;
}
} // namespace

class ApplicationController::Impl {
  public:
    Impl(ApplicationController& owner, QApplication& application)
        : q(owner), app(application), restartCoordinator(application),
          groupManager(initializedPinnedWindowRepository()),
          systemTray(presentation::settings::builtInTrayCommandManifest(), &groupManager),
          featureRouter([this](FeatureFamily feature) { showUnavailableFeature(feature); }) {
        QObject::connect(
            &systemTray, &presentation::SystemTrayController::screenshotRequested, &q, [this]() {
                if (!allowPermissions(presentation::requiredPermissions(
                        presentation::GlobalShortcutAction::Screenshot,
                        permissions.microphoneEnabled())))
                    return;
                static_cast<void>(featureRouter.dispatch(FeatureFamily::Screenshot, [this]() {
                    if (ScreenshotController* controller = ensureScreenshotController()) {
                        controller->startCapture();
                    }
                }));
            });
        QObject::connect(&systemTray, &presentation::SystemTrayController::showMainWindowRequested,
                         &q, [this]() { showMainWindow(); });
        QObject::connect(
            &systemTray, &presentation::SystemTrayController::restartRequested, &q, [this]() {
                const auto result = restartCoordinator.restart(
                    [this]() { return restartAllowed(); },
                    []() { return storage::ApplicationStorage::instance().flushNow().success; });
                if (!result.success) {
                    systemTray.showWarningMessage(ApplicationController::tr("Restart failed"),
                                                  result.error);
                }
            });
        QObject::connect(&systemTray,
                         &presentation::SystemTrayController::openFunctionSettingsRequested, &q,
                         [this]() { ensureMainWindow().showFunctionSettings(); });
        QObject::connect(&systemTray, &presentation::SystemTrayController::openAboutRequested, &q,
                         [this]() { ensureMainWindow().showAbout(); });
        QObject::connect(&systemTray, &presentation::SystemTrayController::exitRequested, &q,
                         [this]() {
                             systemTray.hide();
                             QApplication::quit();
                         });
        QObject::connect(
            &systemTray, &presentation::SystemTrayController::quickActionRequested, &q,
            [this](presentation::GlobalShortcutAction action) { dispatchQuickAction(action); });
        auto& pinnedStorage = storage::ApplicationStorage::instance();
        QObject::connect(&pinnedStorage, &storage::ApplicationStorage::pinnedWindowShowRequested,
                         &q, [this](const QString& id) {
                             if (auto* controller = ensureScreenshotController())
                                 controller->showPinnedRecord(id);
                         });
        QObject::connect(&pinnedStorage, &storage::ApplicationStorage::pinnedWindowDeleteRequested,
                         &q, [this](const QVector<QString>& ids) {
                             if (auto* controller = ensureScreenshotController())
                                 controller->destroyPinnedRecords(ids);
                         });
        QObject::connect(&pinnedStorage, &storage::ApplicationStorage::pinnedWindowsChanged,
                         &groupManager,
                         &presentation::PinnedWindowGroupManager::onPinnedRecordsChanged);
        QObject::connect(
            &groupManager,
            &presentation::PinnedWindowGroupManager::restoreActiveGroupWindowsRequested, &q,
            [this]() {
                static_cast<void>(featureRouter.dispatch(
                    FeatureFamily::PinToScreen,
                    [this]() {
                        if (ScreenshotController* controller = ensureScreenshotController()) {
                            controller->restoreActivePinnedGroupWindows();
                        }
                    },
                    started));
            });
        QObject::connect(
            &globalShortcutManager, &presentation::GlobalShortcutManager::activated, &q,
            [this](presentation::GlobalShortcutAction action) { dispatchQuickAction(action); });
        QObject::connect(&globalShortcutManager, &presentation::GlobalShortcutManager::stateChanged,
                         &q,
                         [this](presentation::GlobalShortcutAction action,
                                const presentation::GlobalShortcutRegistrationState& state) {
                             systemTray.setGlobalShortcuts(action, state.shortcuts);
                         });
        QObject::connect(&globalShortcutManager,
                         &presentation::GlobalShortcutManager::globalHotkeysEnabledChanged, &q,
                         [this](bool enabled) {
                             systemTray.setQuickActionChecked(
                                 presentation::GlobalShortcutAction::ToggleGlobalHotkeys, !enabled);
                         });
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &systemTray,
                         &presentation::SystemTrayController::hide);
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &globalMouseManager,
                         &presentation::GlobalMouseManager::shutdown);
        QObject::connect(
            &globalMouseManager, &presentation::GlobalMouseManager::operationFailed, &q,
            [this](const QString& message) {
#ifdef Q_OS_MACOS
                const auto state = globalMouseManager.permissionState().status;
                if (state == presentation::GlobalMousePermissionState::Status::ListenRequired ||
                    state ==
                        presentation::GlobalMousePermissionState::Status::AccessibilityRequired)
                    return;
#endif
                systemTray.showCaptureMessage(message, true);
            });
        QObject::connect(
            &globalMouseManager, &presentation::GlobalMouseManager::dragEvent, &q,
            [this](const presentation::GlobalMouseDragEvent& event) {
                using Kind = presentation::GlobalMouseDragEvent::Kind;
                const FeatureFamily feature = featureFamilyFor(event.action);
                if (event.kind == Kind::Begin) {
                    if (!allowPermissions(presentation::requiredPermissions(
                            event.action, permissions.microphoneEnabled()))) {
                        globalMouseManager.cancelGesture(event.id);
                        return;
                    }
                    static_cast<void>(featureRouter.beginGesture(
                        feature, [this, id = event.id]() { globalMouseManager.cancelGesture(id); },
                        [this, event]() { dispatchGlobalMouseEvent(event); }));
                    return;
                }
                static_cast<void>(featureRouter.dispatch(
                    feature, [this, event]() { dispatchGlobalMouseEvent(event); }, false));
            });
        auto& applicationStorage = storage::ApplicationStorage::instance();
        if (!applicationStorage.isInitialized()) {
            static_cast<void>(applicationStorage.initialize());
        }
        translationClient =
            std::make_unique<SnowShotApiClient>(SnowShotApiClient::configuredBaseUrl());
        translationService = &translation::TranslationService::forClient(
            *translationClient, applicationStorage.configuration(),
            presentation::LanguageManager::instance().currentLocale());
        QObject::connect(&presentation::LanguageManager::instance(),
                         &presentation::LanguageManager::languageChanged, translationService,
                         [this](const QString&, const QLocale& locale) {
                             translationService->setLocale(locale);
                         });
        // OCR process ownership is application-scoped. ScreenshotController
        // instances receive a consumer of this service instead of creating a
        // second child process for each controller.
        ScreenshotOcrRecognitionService::Options ocrOptions;
        ocrOptions.offlineRoot =
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/ocr"));
        if (applicationStorage.isInitialized() &&
            !applicationStorage.configurationDirectory().trimmed().isEmpty()) {
            ocrOptions.cacheRoot = QDir(applicationStorage.configurationDirectory())
                                       .filePath(QStringLiteral("assets/ocr"));
        }
        ocrOptions.modelType = screenshotOcrModelTypeFromValue(
            applicationStorage.configuration()
                .value(QStringLiteral("text_recognition/model_type"))
                .toString());
        const auto backendPreference =
            applicationStorage.configuration()
                    .value(QStringLiteral("text_recognition/direct_ml_acceleration"))
                    .toBool()
                ? ScreenshotOcrBackendPreference::DirectMl
                : ScreenshotOcrBackendPreference::Cpu;
        ocrRecognition =
            std::make_unique<ScreenshotOcrRecognitionService>(ocrOptions, backendPreference, &q);
        auto& configuration = applicationStorage.configuration();
        update::UpdateService::Options updateOptions;
        updateOptions.applicationDirectory = QCoreApplication::applicationDirPath();
        updateOptions.root = QFileInfo(updateOptions.applicationDirectory).dir().absolutePath();
        const QString updateId = QString::fromLatin1(
            QCryptographicHash::hash(updateOptions.root.toUtf8(), QCryptographicHash::Sha256)
                .toHex()
                .left(24));
        updateOptions.cacheDirectory =
            QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                .filePath(QStringLiteral("updates/") + updateId);
        updateOptions.baseUrl = QUrl(QStringLiteral(SNOW_SHOT_API_BASE_URL));
        updates = new update::UpdateService(std::move(updateOptions), &app);
        updates->setMode(configuration.value(QStringLiteral("updates/mode")).toString());
        updates->setSystemProxy(configuration.value(QStringLiteral("network/proxy")).toString() ==
                                u"system");
        QObject::connect(updates, &update::UpdateService::automaticUpdateAvailable, &q,
                         [this](const QString& version) {
                             systemTray.showUpdateMessage(
                                 ApplicationController::tr(
                                     "Snow Shot %1 is available. Open About for update options.")
                                     .arg(version));
                         });
#ifndef Q_OS_MACOS
        QObject::connect(updates, &update::UpdateService::updateReady, &q, [this] {
            systemTray.showUpdateMessage(ApplicationController::tr(
                "An update is ready. Open About to restart and update Snow Shot."));
        });
#endif
        platform::windows::setAdministratorRestartGuard([this] { return restartAllowed(); });
        QObject::connect(updates, &update::UpdateService::restartRequested, &q, [this] {
            if (platform::windows::administratorOperationPending())
                return;
            if ((screenshotController != nullptr &&
                 screenshotController->blocksApplicationUpdate()) ||
                (directCaptureController != nullptr &&
                 directCaptureController->blocksApplicationUpdate())) {
                updates->reportBlocked(ApplicationController::tr(
                    "Finish capturing, recording, or exporting before updating."));
                return;
            }
            const auto answer = QMessageBox::question(
                mainWindow, ApplicationController::tr("Restart and update"),
                ApplicationController::tr(
                    "Snow Shot will close and restart to install the update. Continue?"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) {
                return;
            }
            if (!storage::ApplicationStorage::instance().flushNow().success) {
                updates->reportBlocked(ApplicationController::tr(
                    "Your settings could not be saved. Please retry before updating."));
                return;
            }
            updates->beginApply();
        });
        QObject::connect(updates, &update::UpdateService::handoffReady, &q, [this] {
            if ((screenshotController != nullptr &&
                 screenshotController->blocksApplicationUpdate()) ||
                (directCaptureController != nullptr &&
                 directCaptureController->blocksApplicationUpdate()) ||
                !storage::ApplicationStorage::instance().flushNow().success) {
                updates->reportBlocked(ApplicationController::tr(
                    "Finish capturing, recording, or exporting before updating."));
                return;
            }
            globalShortcutManager.setGlobalHotkeysEnabled(false);
            globalMouseManager.shutdown();
            QApplication::quit();
        });
        applyRuntimeConfiguration(configuration.value(kPinBorderColorKey), kPinBorderColorKey);
        applyRuntimeConfiguration(configuration.value(kPinBorderActiveColorKey),
                                  kPinBorderActiveColorKey);
        applyRuntimeConfiguration(configuration.value(kTrayEnabledKey), kTrayEnabledKey);
        applyRuntimeConfiguration(configuration.value(kTrayIconKey), kTrayIconKey);
        applyRuntimeConfiguration(configuration.value(kTrayCustomIconKey), kTrayCustomIconKey);
        applyRuntimeConfiguration(configuration.value(kTrayLeftClickActionKey),
                                  kTrayLeftClickActionKey);
        applyRuntimeConfiguration(configuration.value(kTrayMiddleClickActionKey),
                                  kTrayMiddleClickActionKey);
        applyRuntimeConfiguration(configuration.value(kTrayMenuOptionsKey), kTrayMenuOptionsKey);
        applyRuntimeConfiguration(configuration.value(kScreenshotDelaySecondsKey),
                                  kScreenshotDelaySecondsKey);
        applyRuntimeConfiguration(configuration.value(kFullscreenSuppressionKey),
                                  kFullscreenSuppressionKey);
        QObject::connect(&configuration, &storage::ConfigurationStore::valueChanged, &q,
                         [this](const QString& key, const QJsonValue& value) {
                             applyRuntimeConfiguration(value, key);
                         });
        QObject::connect(&configuration, &storage::ConfigurationStore::valueChanged, &q,
                         [this](const QString& key, const QJsonValue&) {
                             if (key == kMcpEnabledKey) {
                                 QTimer::singleShot(0, &q, [this] {
                                     if (storage::ApplicationStorage::instance()
                                             .configuration()
                                             .value(kMcpEnabledKey)
                                             .toBool())
                                         startMcp();
                                     else if (mcpServer)
                                         mcpServer->drainAndStop([this] { stopMcp(); });
                                     else
                                         stopMcp();
                                 });
                             }
                         });
        if (configuration.value(kMcpEnabledKey).toBool()) {
            startMcp();
        }
#ifdef Q_OS_MACOS
        reopenHandler = std::make_unique<platform::macos::ApplicationReopenHandler>(
            [this]() { showMainWindow(); });
#endif
    }

    ~Impl() {
        stopMcp();
        platform::windows::setAdministratorRestartGuard({});
        if (mainWindow != nullptr) {
            mainWindow->setAttribute(Qt::WA_DeleteOnClose, false);
            delete mainWindow;
        }
    }

    [[nodiscard]] bool restartAllowed() const {
        return updates != nullptr && updates->status().state != update::UpdateState::Applying &&
               !(screenshotController && screenshotController->blocksApplicationUpdate()) &&
               !(directCaptureController && directCaptureController->blocksApplicationUpdate());
    }

    void applyOcrConfiguration() {
        if (ocrRecognition == nullptr)
            return;
        const auto& configuration = storage::ApplicationStorage::instance().configuration();
        ocrRecognition->setRuntimeConfiguration(
            {screenshotOcrModelTypeFromValue(configuration.value(kOcrModelTypeKey).toString()),
             configuration.value(kOcrDirectMlKey).toBool()
                 ? ScreenshotOcrBackendPreference::DirectMl
                 : ScreenshotOcrBackendPreference::Cpu,
             configuration.value(QStringLiteral("text_recognition/resident_process")).toBool(),
             configuration.value(QStringLiteral("text_recognition/model_hot_start")).toBool()});
    }

    void startMcp() {
        if (mcpServer && mcpServer->isRunning())
            return;
        auto* controller = ensureScreenshotController();
        ensureDirectCaptureController();
        ensureSettingsRuntime();
        if (!mcpJobs) {
            mcpSourceWorkers.setMaxThreadCount(2);
            mcpJobs = std::make_unique<mcp::McpJobRegistry>(&q);
            mcp::McpApplicationService::Ports appPorts;
            appPorts.storage = &storage::ApplicationStorage::instance();
            appPorts.settings = runtimeSession.get();
            appPorts.permissions = &permissions;
            appPorts.translation = translationService;
            appPorts.updates = updates;
            appPorts.jobs = mcpJobs.get();
            appPorts.artifactWriter = [this](quint64 owner, QByteArray bytes, QString mime) {
                return mcpDocuments ? mcpDocuments->storeArtifact(owner, std::move(bytes), mime)
                                    : QJsonObject();
            };
            appPorts.restartAllowed = [this] { return restartAllowed(); };
            appPorts.action = [this](const QString& action, const QJsonObject& params) {
                if (action == u"show_main")
                    showMainWindow();
                else if (action == u"show_history")
                    ensureMainWindow().showScreenshotHistory();
                else if (action == u"show_pinned")
                    ensureMainWindow().showPinToScreenManagement();
                else if (action == u"show_translation")
                    ensureMainWindow().showTranslation({});
                else if (action == u"show_settings") {
                    const auto page = params.value(QStringLiteral("page_id")).toString();
                    if (page.isEmpty())
                        ensureMainWindow().showFunctionSettings();
                    else if (settingsRegistry->catalog().page(page))
                        ensureMainWindow().showSettingsLocation(
                            page, params.value(QStringLiteral("section_id")).toString());
                    else
                        return false;
                } else if (action == u"restart") {
                    return restartCoordinator
                        .restart(
                            [this] { return restartAllowed(); },
                            [] {
                                return storage::ApplicationStorage::instance().flushNow().success;
                            })
                        .success;
                } else if (action == u"quit") {
                    if (!restartAllowed())
                        return false;
                    app.quit();
                } else
                    return false;
                return true;
            };
            appPorts.historyAction = [controller](const QString& id, const QString& action) {
                if (controller->blocksApplicationUpdate())
                    return false;
                if (action == u"edit")
                    controller->editHistoryRecord(id);
                else if (action == u"pin")
                    controller->pinHistoryRecord(id);
                else
                    return false;
                return true;
            };
            appPorts.selectedText = [this](auto done) -> std::function<void()> {
                auto* capture =
                    new presentation::SelectedTextTranslationController(mcpApplication.get());
                QObject::connect(capture,
                                 &presentation::SelectedTextTranslationController::textReady,
                                 capture, [capture, done = std::move(done)](const QString& text) {
                                     done(text, {});
                                     capture->deleteLater();
                                 });
                capture->capture();
                return
                    [capture = QPointer<presentation::SelectedTextTranslationController>(capture)] {
                        if (capture) {
                            capture->cancel();
                            capture->deleteLater();
                        }
                    };
            };
            mcpApplication = std::make_unique<mcp::McpApplicationService>(std::move(appPorts), &q);
            mcp::McpDocumentService::Ports documentPorts;
            documentPorts.jobs = mcpJobs.get();
            documentPorts.recognition = ocrRecognition.get();
            documentPorts.qrRecognition = controller->mcpQrRecognition();
            documentPorts.api = translationClient.get();
            documentPorts.autoFilter = ScreenshotAutoFilterController::detectRegions;
            documentPorts.cancelSource = [this](quint64 owner, const QString& requestId) {
                cancelMcpSource(owner, requestId);
            };
            documentPorts.resolveSource = [this](const mcp::ScreenshotMcpRequest& request,
                                                 auto completion, auto budget) {
                resolveMcpSource(request, std::move(completion), std::move(budget));
            };
            documentPorts.pin = [controller](QImage image, auto done) {
                ScreenshotClipboardContent content;
                content.image = std::move(image);
                return controller->mcpPinContent(std::move(content), std::move(done));
            };
            const auto historyEntryFromSource = [](mcp::McpDocumentService::Source source) {
                ScreenshotHistoryEntry entry;
                entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                entry.createdUtc = QDateTime::currentDateTimeUtc();
                entry.persistent = false;
                entry.recordedCanvasBounds = source.canvasBounds.toAlignedRect();
                if (source.images.isEmpty() && !source.image.isNull()) {
                    if (entry.recordedCanvasBounds.isEmpty())
                        entry.recordedCanvasBounds = QRect(QPoint(), source.image.size());
                    source.images.append({source.image, QRectF(entry.recordedCanvasBounds)});
                }
                entry.selection = source.selection.value_or(ScreenshotSelectionParams{});
                if (!source.selection)
                    entry.selection.selection = entry.recordedCanvasBounds;
                entry.canvasHistory = std::move(source.documentHistory);
                entry.documentSession = std::move(source.documentSession);
                entry.originalContent = std::move(source.originalContent);
                entry.recognitionResults = std::move(source.recognitionResults);
                entry.tool = std::move(source.tool);
                for (const auto& image : source.images) {
                    ScreenshotHistoryDisplay display;
                    display.stableId = QStringLiteral("mcp:%1").arg(entry.displays.size());
                    display.name = display.stableId;
                    display.image = image.image;
                    display.sourceCanvasRect = image.canvasRect.toAlignedRect();
                    display.sourceCanvasOrigin = display.sourceCanvasRect->topLeft();
#ifdef Q_OS_MACOS
                    display.canvasUsesPoints = true;
#endif
                    display.backingScale = image.canvasRect.width() > 0
                                               ? image.image.width() / image.canvasRect.width()
                                               : 1.0;
                    entry.displays.append(std::move(display));
                }
                return entry;
            };
            documentPorts.present = [controller, historyEntryFromSource](auto source, auto done) {
                return controller->mcpPresentDocument(historyEntryFromSource(std::move(source)),
                                                      std::move(done));
            };
            documentPorts.pinDocument =
                [controller, historyEntryFromSource](auto source, QImage background, auto done) {
                    return controller->mcpPinDocument(historyEntryFromSource(std::move(source)),
                                                      std::move(background), std::move(done));
                };
            mcpDocuments = std::make_unique<mcp::McpDocumentService>(std::move(documentPorts), &q);
            mcpMedia = std::make_unique<mcp::McpMediaService>(*controller, groupManager, &q);
            auto artifactWriter = [this](quint64 owner, QByteArray bytes, const QString& mime) {
                return mcpDocuments ? mcpDocuments->storeArtifact(owner, std::move(bytes), mime)
                                    : QJsonObject();
            };
            mcpJobs->setArtifactPublisher(artifactWriter);
            mcpMedia->setArtifactWriter(artifactWriter);
            mcpMedia->setFileArtifactWriter(
                [this](quint64 owner, QString path, QString mime, auto completion) {
                    if (mcpDocuments)
                        mcpDocuments->storeFileArtifact(owner, std::move(path), std::move(mime),
                                                        std::move(completion));
                    else
                        completion(QJsonObject(), QStringLiteral("unavailable"));
                });
            QObject::connect(
                mcpJobs.get(), &mcp::McpJobRegistry::changed, &q,
                [this](quint64 owner, const QString& id) {
                    if (mcpServer)
                        mcpServer->publishEvent(
                            owner,
                            {{QStringLiteral("event"), QStringLiteral("resource_changed")},
                             {QStringLiteral("uri"), QStringLiteral("snow-shot://jobs/") + id}});
                });
            QObject::connect(
                mcpDocuments.get(), &mcp::McpDocumentService::changed, &q,
                [this](quint64 owner, const QString& id) {
                    if (mcpServer)
                        mcpServer->publishEvent(
                            owner, {{QStringLiteral("event"), QStringLiteral("resource_changed")},
                                    {QStringLiteral("uri"),
                                     QStringLiteral("snow-shot://documents/") + id}});
                });
            QObject::connect(
                mcpDocuments.get(), &mcp::McpDocumentService::artifactChanged, &q,
                [this](quint64 owner, const QString& id) {
                    if (mcpServer)
                        mcpServer->publishEvent(
                            owner, {{QStringLiteral("event"), QStringLiteral("resource_changed")},
                                    {QStringLiteral("uri"),
                                     QStringLiteral("snow-shot://artifacts/") + id}});
                });
        }
        if (!mcpSession) {
            mcp::ScreenshotMcpSession::Ports ports;
            ports.state = [controller] { return controller->mcpState(); };
            ports.command = [controller](const QString& method, const QJsonObject& params,
                                         mcp::ScreenshotMcpSession::Ports::CommandCompletion done) {
                controller->mcpCommand(method, params, std::move(done));
            };
            ports.cancelCommand = [controller] { controller->mcpCancelCommand(); };
            ports.detached = [controller] { controller->mcpDetached(); };
            ports.begin = [this, controller](const QJsonObject& options, QString* error) {
                if (directCaptureController->blocksApplicationUpdate()) {
                    if (error)
                        *error = QStringLiteral("busy");
                    return false;
                }
#ifdef Q_OS_MACOS
                permissions.refresh();
                if (!permissions.missing({presentation::AppPermission::ScreenRecording})
                         .isEmpty()) {
                    if (error)
                        *error = QStringLiteral("permission_required");
                    return false;
                }
#endif
                return controller->mcpBegin(options, error);
            };
            ports.cancel = [this, controller]() {
                controller->mcpCancelCapture();
                if (directCaptureController)
                    directCaptureController->cancelMcpCapture();
            };
            ports.selection = [controller](const QJsonObject& params, QString* error) {
                return controller->mcpSetSelection(params, error);
            };
            ports.tool = [controller](const QString& tool, QString* error) {
                return controller->mcpSetTool(tool, error);
            };
            ports.annotations = [controller](const QByteArray& bytes, QJsonObject* result,
                                             QString* error) {
                return controller->mcpApplyAnnotations(bytes, result, error);
            };
            ports.history = [controller](bool redo) {
                if (redo)
                    controller->mcpRedoCanvasEdit();
                else
                    controller->mcpUndoCanvasEdit();
            };
            ports.artifact = [controller](qreal scale) {
                return controller->mcpExportArtifact(scale);
            };
            ports.pin = [controller](auto artifact, auto completion) {
                return controller->mcpPinArtifact(std::move(artifact), std::move(completion));
            };
            ports.direct = [this](const QJsonObject& params, auto completion) {
                return directCaptureController->mcpCapture(params, std::move(completion));
            };
            mcpSession = std::make_unique<mcp::ScreenshotMcpSession>(std::move(ports), &q);
            QObject::connect(controller, &ScreenshotController::mcpCapturePresented,
                             mcpSession.get(), &mcp::ScreenshotMcpSession::capturePresented);
            QObject::connect(controller, &ScreenshotController::mcpCaptureTerminated,
                             mcpSession.get(), &mcp::ScreenshotMcpSession::captureTerminated);
            QObject::connect(controller, &ScreenshotController::mcpCanvasChanged, mcpSession.get(),
                             &mcp::ScreenshotMcpSession::observe);
            QObject::connect(&app, &QCoreApplication::aboutToQuit, &q, [this] { stopMcp(); });
        }
        if (!mcpServer) {
            mcpServer = std::make_unique<mcp::ScreenshotMcpServer>(&q);
            mcpServer->setRequestHandler([this](const auto& request, auto completion) {
                if (mcpApplication && mcpApplication->handles(request.method))
                    mcpApplication->request(request, std::move(completion));
                else if (mcpDocuments && mcpDocuments->handles(request.method))
                    mcpDocuments->request(request, std::move(completion));
                else if (mcpMedia && mcpMedia->handles(request.method))
                    mcpMedia->request(request, std::move(completion));
                else
                    mcpSession->request(request, std::move(completion));
            });
            mcpServer->setClientDisconnectedHandler([this](quint64 connection) {
                const auto prefix = QString::number(connection) + u':';
                for (auto it = mcpSources.begin(); it != mcpSources.end();)
                    if (it.key().startsWith(prefix)) {
                        it.value()->store(true);
                        delete mcpSourceDelays.take(it.key());
                        it = mcpSources.erase(it);
                    } else
                        ++it;
                if (mcpSourceCaptureOwner == connection && directCaptureController) {
                    directCaptureController->cancelMcpCapture();
                    mcpSourceCaptureOwner = 0;
                }
                mcpSession->disconnected(connection);
                if (mcpApplication)
                    mcpApplication->disconnected(connection);
                if (mcpDocuments)
                    mcpDocuments->disconnected(connection);
                if (mcpMedia)
                    mcpMedia->disconnected(connection);
                if (mcpJobs)
                    mcpJobs->disconnected(connection);
            });
            mcpServer->setRequestCancellationHandler([this](quint64 owner, const QString& id) {
                bool accepted = cancelMcpSource(owner, id);
                if (mcpDocuments)
                    accepted |= mcpDocuments->cancelRequest(owner, id);
                if (mcpApplication)
                    accepted |= mcpApplication->cancelRequest(owner, id);
                if (mcpMedia)
                    accepted |= mcpMedia->cancelRequest(owner, id);
                if (mcpSession)
                    accepted |= mcpSession->cancelRequest(owner, id);
                return accepted;
            });
        }
        QString error;
        if (!mcpServer->start(&error))
            qWarning("Unable to start Snow Shot MCP server: %s", qPrintable(error));
    }
    bool cancelMcpSource(quint64 owner, const QString& requestId) {
        const auto key = QString::number(owner) + u':' + requestId;
        const auto cancel = mcpSources.take(key);
        if (!cancel)
            return false;
        cancel->store(true);
        delete mcpSourceDelays.take(key);
        if (mcpSourceCaptureOwner == owner && mcpSourceCaptureRequest == requestId &&
            directCaptureController) {
            directCaptureController->cancelMcpCapture();
            mcpSourceCaptureOwner = 0;
            mcpSourceCaptureRequest.clear();
        }
        return true;
    }
    void stopMcp() {
        for (const auto& canceled : mcpSources)
            canceled->store(true);
        mcpSources.clear();
        qDeleteAll(mcpSourceDelays);
        mcpSourceDelays.clear();
        if (mcpSourceCaptureOwner && directCaptureController)
            directCaptureController->cancelMcpCapture();
        mcpSourceCaptureOwner = 0;
        mcpSourceCaptureRequest.clear();
        if (mcpSession)
            mcpSession->shutdown();
        if (mcpServer)
            mcpServer->stop();
        if (mcpDocuments)
            mcpDocuments->shutdown();
        if (mcpMedia)
            mcpMedia->shutdown();
        if (mcpApplication)
            mcpApplication->shutdown();
        if (mcpJobs)
            mcpJobs->shutdown();
        mcpSourceWorkers.waitForDone();
        mcpSourceWork = 0;
        mcpDocuments.reset();
        mcpMedia.reset();
        mcpApplication.reset();
        mcpJobs.reset();
    }

    void resolveMcpSource(const mcp::ScreenshotMcpRequest& request,
                          mcp::McpDocumentService::Ports::SourceCompletion completion,
                          mcp::McpDocumentService::Ports::SourceBudget sourceBudget) {
        using Source = mcp::McpDocumentService::Source;
        const auto& params = request.params;
        const auto source = params.value(QStringLiteral("source")).toString();
        const auto key = QString::number(request.connectionId) + u':' + request.requestId;
        const auto canceled = std::make_shared<std::atomic_bool>(false);
        const auto rejected = std::make_shared<std::atomic_bool>(false);
        const auto peak = std::make_shared<std::atomic<qint64>>(0);
        const auto budget = [sourceBudget = std::move(sourceBudget), canceled, rejected,
                             peak](qint64 bytes) {
            if (canceled->load())
                return false;
            const qint64 total = std::max(peak->load(), bytes);
            if (bytes < 0 || !sourceBudget || !sourceBudget(total)) {
                rejected->store(true);
                return false;
            }
            peak->store(total);
            return true;
        };
        const auto rasterBytes = [](QSize size) -> qint64 {
            const qint64 pixels = static_cast<qint64>(size.width()) * size.height();
            return size.width() > 0 && size.height() > 0 && pixels <= 64000000 ? pixels * 8 : -1;
        };
        mcpSources.insert(key, canceled);
        const QPointer<mcp::McpDocumentService> guard(mcpDocuments.get());
        auto sourceDone = [this, guard, key, canceled, rejected, completion = std::move(completion)](
                              Source sourceData, QString error) mutable {
            if (!guard || canceled->exchange(true))
                return;
            mcpSources.remove(key);
            completion(std::move(sourceData), rejected->load() ? QStringLiteral("resource_limit")
                                                               : std::move(error));
        };
        auto done = [sourceDone](QImage image, QJsonObject metadata, QString error) mutable {
            Source sourceData;
            sourceData.image = std::move(image);
            sourceData.metadata = std::move(metadata);
            sourceDone(std::move(sourceData), std::move(error));
        };
        if (source == u"capture") {
            const int delay = params.value(QStringLiteral("delay_seconds")).toInt(0);
            if (delay < 0 || delay > 10) {
                done({}, {}, QStringLiteral("invalid_parameters"));
                return;
            }
            auto* delayed = new QTimer(mcpDocuments.get());
            delayed->setSingleShot(true);
            mcpSourceDelays.insert(key, delayed);
            QObject::connect(
                delayed, &QTimer::timeout, mcpDocuments.get(),
                [this, guard, delayed, key, params, owner = request.connectionId,
                 id = request.requestId, canceled, budget, rasterBytes,
                 done = std::move(done)]() mutable {
                    mcpSourceDelays.remove(key);
                    delayed->deleteLater();
                    if (!guard || canceled->load())
                        return;
#ifdef Q_OS_MACOS
                    permissions.refresh();
                    if (!permissions.missing({presentation::AppPermission::ScreenRecording})
                             .isEmpty()) {
                        done({}, {}, QStringLiteral("permission_required"));
                        return;
                    }
#endif
                    if (mcpSourceCaptureOwner ||
                        (screenshotController &&
                         screenshotController->captureAcquisitionActive())) {
                        done({}, {}, QStringLiteral("busy"));
                        return;
                    }
                    // Direct capture acquires every display before selecting its result.
                    // Reserve all native frames and the possible scaled output first.
                    qint64 captureBytes = 0;
                    QSize largest;
#ifdef Q_OS_MACOS
                    qreal maximumDpr = 1.0;
#endif
                    for (const auto* screen : QGuiApplication::screens()) {
#ifdef Q_OS_MACOS
                        maximumDpr = std::max(maximumDpr, screen->devicePixelRatio());
#endif
                        const QSize pixels(qCeil(screen->size().width() * screen->devicePixelRatio()),
                                           qCeil(screen->size().height() * screen->devicePixelRatio()));
                        const auto bytes = rasterBytes(pixels);
                        if (bytes < 0 || !budget(captureBytes + bytes)) {
                            done({}, {}, QStringLiteral("resource_limit"));
                            return;
                        }
                        captureBytes += bytes;
                        largest = largest.expandedTo(pixels);
                    }
                    if (params.value(QStringLiteral("target")).toString() == u"focused_window") {
                        QSize pixels;
#ifdef Q_OS_WIN
                        RECT bounds{};
                        if (const auto window = GetForegroundWindow();
                            window && GetWindowRect(GetAncestor(window, GA_ROOT), &bounds))
                            pixels = QSize(bounds.right - bounds.left, bounds.bottom - bounds.top);
#elif defined(Q_OS_MACOS)
                        const auto bounds = platform::screenshotFocusedWindowBounds();
                        pixels = QSize(qCeil(bounds.width() * maximumDpr),
                                       qCeil(bounds.height() * maximumDpr));
#endif
                        if (!pixels.isEmpty()) {
                            const auto bytes = rasterBytes(pixels);
                            if (bytes < 0 || !budget(captureBytes + bytes)) {
                                done({}, {}, QStringLiteral("resource_limit"));
                                return;
                            }
                            captureBytes += bytes;
                            largest = largest.expandedTo(pixels);
                        }
                    }
                    const auto scale = params.value(QStringLiteral("scale")).toDouble(1.0);
                    if (!std::isfinite(scale) || scale < 0.1 || scale > 4.0) {
                        done({}, {}, QStringLiteral("invalid_parameters"));
                        return;
                    }
                    const auto outputBytes = rasterBytes(
                        QSize(qCeil(largest.width() * scale), qCeil(largest.height() * scale)));
                    if (outputBytes < 0 || !budget(captureBytes + outputBytes)) {
                        done({}, {}, QStringLiteral("resource_limit"));
                        return;
                    }
                    mcpSourceCaptureOwner = owner;
                    mcpSourceCaptureRequest = id;
                    if (!directCaptureController->mcpCapture(
                            params, [this, done](QImage image, QJsonObject metadata,
                                                 QString error) mutable {
                                mcpSourceCaptureOwner = 0;
                                mcpSourceCaptureRequest.clear();
                                done(std::move(image), std::move(metadata), std::move(error));
                            })) {
                        mcpSourceCaptureOwner = 0;
                        mcpSourceCaptureRequest.clear();
                        done({}, {}, QStringLiteral("busy"));
                    }
                });
            delayed->start(delay * 1000);
        } else if (source == u"clipboard" || source == u"history" || source == u"pinned" ||
                   source == u"text" || source == u"html") {
            if (mcpSourceWork >= 2) {
                done({}, {}, QStringLiteral("queue_full"));
                return;
            }
            ++mcpSourceWork;
            std::optional<ScreenshotClipboardContentSnapshot> clipboard;
            if (source == u"clipboard")
                clipboard =
                    ScreenshotClipboardContentReader::snapshot(QApplication::clipboard(), 1.0,
                                                               budget);
            std::optional<storage::PinnedWindowRecord> livePin;
            std::optional<ScreenshotRecognitionResults> liveRecognition;
            QString livePinTool;
            if (source == u"pinned") {
                if (const auto* window = groupManager.liveWindow(
                        params.value(QStringLiteral("source_id")).toString())) {
                    const auto state = window->automationState();
                    const auto size = state.value(QStringLiteral("source_size")).toArray();
                    const auto pixels = size.size() == 2
                                            ? rasterBytes(QSize(size.at(0).toInt(), size.at(1).toInt()))
                                            : -1;
                    // The engine caps a serialized document session at 16 MiB.
                    // Allow both serialization buffers and the owned snapshot.
                    if (pixels < 0 || !budget(pixels + 64 * 1024 * 1024)) {
                        --mcpSourceWork;
                        done({}, {}, QStringLiteral("resource_limit"));
                        return;
                    }
                    livePin = window->persistenceSnapshot();
                    liveRecognition = window->recognitionSnapshot();
                    livePinTool =
                        state.value(QStringLiteral("active_tool")).toString();
                }
            }
            auto* watcher = new QFutureWatcher<Source>(mcpDocuments.get());
            QObject::connect(watcher, &QFutureWatcher<Source>::finished, mcpDocuments.get(),
                             [this, watcher, sourceDone = std::move(sourceDone)]() mutable {
                                 --mcpSourceWork;
                                 auto result = watcher->result();
                                 watcher->deleteLater();
                                 const auto error = result.image.isNull() && result.images.isEmpty()
                                                        ? QStringLiteral("invalid_source")
                                                        : QString();
                                 sourceDone(std::move(result), error);
                             });
            watcher->setFuture(mcp::runMcpWork(&mcpSourceWorkers, [source, clipboard, livePin,
                                                                   liveRecognition, livePinTool,
                                                                   params, canceled, budget,
                                                                   rasterBytes] {
                Source result;
                if (canceled->load())
                    return result;
                if (source == u"clipboard") {
                    if (!clipboard)
                        return result;
                    const auto content = ScreenshotClipboardContentReader::decode(
                        *clipboard, [canceled] { return canceled->load(); }, budget);
                    if (content) {
                        result.image = content->image;
                        result.originalContent = content->originalContent;
                    }
                    return result;
                }
                if (source == u"text" || source == u"html") {
                    ScreenshotClipboardOriginalContent original;
                    original.text = params.value(QStringLiteral("text")).toString();
                    if (source == u"html")
                        original.html = params.value(QStringLiteral("html")).toString();
                    result.originalContent = original;
                    const auto content =
                        ScreenshotClipboardContentReader::renderOriginalText(original, 1.0, {},
                                                                             budget);
                    if (content)
                        result.image = content->image;
                    return result;
                }
                if (source == u"pinned") {
                    const auto record =
                        livePin
                            ? livePin
                            : storage::ApplicationStorage::instance().pinnedWindows().loadRecord(
                                  params.value(QStringLiteral("source_id")).toString(), budget);
                    if (!record)
                        return result;
                    result.image = record->image;
                    result.originalContent = {record->originalHtml, record->originalText,
                                              record->originalFilePath};
                    result.recognitionResults =
                        liveRecognition.value_or(ScreenshotPinnedWindow::decodeRecognitionSnapshot(
                            record->recognitionResults));
                    if (record->sourceKind == storage::PinnedWindowSourceKind::ClipboardText) {
                        const auto content = ScreenshotClipboardContentReader::renderOriginalText(
                            {record->originalHtml, record->originalText, {}},
                            record->firstCreationTextDpi, {}, budget);
                        if (content)
                            result.image = content->image;
                    }
                    // A pinned record retains original pixels plus a separate
                    // transform. Its annotations already use the current canvas
                    // coordinates, so import the transformed background into the
                    // current content rectangle without flattening annotations.
                    const auto originalPixels = result.image.size();
                    const auto transformedPixels =
                        record->imageTransform.mapRect(QRectF(QPointF(), originalPixels))
                            .toAlignedRect().size();
                    const auto sourceBytes = rasterBytes(originalPixels);
                    const auto transformedBytes = rasterBytes(transformedPixels);
                    if (sourceBytes < 0 || transformedBytes < 0 ||
                        !budget(sourceBytes + transformedBytes +
                                (livePin ? 64 * 1024 * 1024 : 0) +
                                8LL * (record->canvasSession.size() +
                                       record->recognitionResults.size() +
                                       record->originalHtml.size() + record->originalText.size())))
                        return Source{};
                    if (!record->imageTransform.isIdentity())
                        result.image = result.image.transformed(record->imageTransform,
                                                                Qt::SmoothTransformation);
                    const auto style = decodeScreenshotResultStyle(record->resultStyle);
                    if (result.image.isNull() || !style)
                        return Source{};
                    const auto contentBounds = !record->contentCanvasRect.isEmpty()
                                                   ? record->contentCanvasRect
                                                   : record->canvasSourceRect;
                    result.canvasBounds = record->surfaceCanvasRect;
                    if (result.canvasBounds.isEmpty())
                        result.canvasBounds = QRectF(QPointF(), result.image.size());
                    const auto selectionBounds =
                        contentBounds.isEmpty() ? result.canvasBounds : contentBounds;
                    result.recognitionResults =
                        ScreenshotPinnedWindow::transformedRecognitionSnapshot(
                            std::move(result.recognitionResults), record->canvasSourceRect,
                            originalPixels, record->imageTransform, result.image.size(),
                            selectionBounds);
                    result.images.append({result.image, selectionBounds});
                    result.selection = ScreenshotSelectionParams{selectionBounds.toAlignedRect(),
                                                                 style->cornerRadius,
                                                                 style->shadowWidth,
                                                                 style->shadowColor,
                                                                 false,
                                                                 false,
                                                                 style->region};
                    result.documentSession = record->canvasSession;
                    if (!livePinTool.isEmpty())
                        result.tool = livePinTool;
                    return result;
                }
                auto& history = storage::ApplicationStorage::instance().captureHistory();
                for (const auto& record : history.records()) {
                    if (record.id != params.value(QStringLiteral("source_id")).toString())
                        continue;
                    qint64 historyBytes = record.canvasBytes >= 0 &&
                                                  record.canvasBytes <= 64 * 1024 * 1024
                                              ? 8 * record.canvasBytes
                                              : -1;
                    if (!budget(historyBytes))
                        return result;
                    const auto addImage = [&](QSize size, qint64 encodedBytes) {
                        const auto bytes = rasterBytes(size);
                        if (bytes < 0 || encodedBytes < 0 || encodedBytes > 256 * 1024 * 1024)
                            return budget(-1);
                        historyBytes += bytes + 2 * encodedBytes;
                        return budget(historyBytes);
                    };
                    for (const auto& display : record.displays) {
                        if (!addImage(display.imageSize, display.encodedBytes))
                            return result;
                    }
                    if (record.result &&
                        !addImage(record.result->imageSize, record.result->encodedBytes))
                        return result;
                    const auto payload = history.load(record);
                    if (!payload)
                        return result;
                    result.documentHistory = payload->canvasHistory;
                    result.canvasBounds = record.canvasBounds;
                    const auto& selected = record.selection;
                    result.selection = ScreenshotSelectionParams{
                        selected.rectangle,       selected.cornerRadius,
                        selected.shadowWidth,     selected.shadowColor,
                        selected.lockAspectRatio, selected.lockDragAspectRatio,
                        selected.region};
                    for (qsizetype index = 0;
                         index < payload->displayImages.size() && index < record.displays.size();
                         ++index) {
                        const auto& image = payload->displayImages[index];
                        const auto& display = record.displays[index];
                        const auto bounds = display.sourceCanvasRect.value_or(
                            QRect(display.sourceCanvasOrigin.value_or(QPoint()), image.size()));
                        result.images.append({image, QRectF(bounds)});
                    }
                    if (result.images.isEmpty())
                        result.image = history.loadResultImage(record).value_or(QImage());
                    return result;
                }
                return result;
            }));
        } else
            done({}, {}, QStringLiteral("unsupported_source"));
    }

    void start() {
        if (started) {
            return;
        }
        started = true;
        updates->start();

#ifdef Q_OS_MACOS
        permissions.setMicrophoneEnabled(storage::RecordingSettings().microphoneEnabled());
        globalMouseManager.usePermissionSnapshot(false, false);
        QObject::connect(&permissions, &presentation::AppPermissionService::refreshed, &q, [this] {
            const auto& snapshot = permissions.snapshot();
            globalMouseManager.usePermissionSnapshot(
                snapshot.granted(presentation::AppPermission::InputMonitoring),
                snapshot.granted(presentation::AppPermission::Accessibility));
            const auto missing = permissions.takeStartupMissing();
            if (!missing.isEmpty())
                ensureMainWindow().showAppPermissions(
                    presentation::appPermissionId(missing.first()));
        });
        QObject::connect(&globalMouseManager,
                         &presentation::GlobalMouseManager::permissionRefreshRequested,
                         &permissions, &presentation::AppPermissionService::refresh);
        permissions.refresh();
#endif
        systemTray.show();
        globalShortcutManager.initialize();
        globalMouseManager.setCaptureAvailable(ensureScreenshotController()->captureAvailable());
        globalMouseManager.initialize();
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, &globalMouseManager,
                         [this](Qt::ApplicationState state) {
                             if (state == Qt::ApplicationActive) {
#ifdef Q_OS_MACOS
                                 permissions.refresh();
#else
                                 globalMouseManager.refreshPermission();
#endif
                             }
                         });
        static_cast<void>(featureRouter.dispatch(
            FeatureFamily::Screenshot,
            [this]() {
                QTimer::singleShot(0, &q, [this]() {
                    if (ScreenshotController* controller = ensureScreenshotController()) {
                        controller->prewarmResources();
                    }
                });
            },
            false));
        static_cast<void>(featureRouter.dispatch(
            FeatureFamily::PinToScreen,
            [this]() { QTimer::singleShot(0, &q, [this]() { restorePinnedWindows(); }); }, false));
        QTimer::singleShot(0, &q, [this]() { applyOcrConfiguration(); });
    }

    ScreenshotController* ensureScreenshotController() {
        if (screenshotController == nullptr) {
            screenshotController = std::make_unique<ScreenshotController>(
                &q, &groupManager, ocrRecognition.get(), translationClient.get());
#ifdef Q_OS_MACOS
            screenshotController->setRecordingPermissionCheck([this](bool microphone, bool input,
                                                                     bool notify) {
                permissions.refresh();
                presentation::AppPermissions required{presentation::AppPermission::ScreenRecording};
                if (microphone)
                    required.append(presentation::AppPermission::Microphone);
                if (input)
                    required.append(presentation::AppPermission::InputMonitoring);
                return notify ? allowPermissions(required)
                              : permissions.missing(required).isEmpty();
            });
#else
            screenshotController->setRecordingPermissionCheck(
                [](bool, bool, bool) { return true; });
#endif
            QObject::connect(
                screenshotController.get(), &ScreenshotController::accessibilityPermissionRequested,
                &q, [this] {
                    permissions.refresh();
                    ensureMainWindow().showAppPermissions(
                        presentation::appPermissionId(presentation::AppPermission::Accessibility));
                });
            QObject::connect(
                screenshotController.get(), &ScreenshotController::selectedFilePinFailed, &q,
                [this](const QString& message) {
                    if (systemTray.canShowMessages() &&
                        (!mainWindow || !mainWindow->isVisible() || mainWindow->isMinimized())) {
                        systemTray.showWarningMessage(
                            ApplicationController::tr("Could not pin selected files"), message);
                        return;
                    }
                    MainWindow& window = ensureMainWindow();
                    window.showAndActivate();
                    adqt::widgets::AdMessage::Request request;
                    request.key = QStringLiteral("pin-selected-files-error");
                    request.content = message;
                    adqt::widgets::AdMessageService::warning(std::move(request), &window);
                });
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::showMainWindowRequested, &q,
                             [this]() { showMainWindow(); });
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::translationPageRequested, &q,
                             [this](const QString& text) {
                                 ensureSelectedTextTranslationCoordinator().presentText(text);
                             });
            if (isFeatureAvailable(FeatureFamily::Screenshot) ||
                isFeatureAvailable(FeatureFamily::PinToScreen) ||
                isFeatureAvailable(FeatureFamily::ScreenRecording)) {
                QObject::connect(
                    screenshotController.get(), &ScreenshotController::captureAvailabilityChanged,
                    &globalMouseManager, &presentation::GlobalMouseManager::setCaptureAvailable);
            }
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::globalMouseCaptureEnded, &globalMouseManager,
                             &presentation::GlobalMouseManager::cancelGesture);
        }
        return screenshotController.get();
    }

    void applyRuntimeConfiguration(const QJsonValue& value, const QString& key) {
        if (key == QStringLiteral("screen_recording/enable_microphone"))
            permissions.setMicrophoneEnabled(value.toBool());
        if (key == QStringLiteral("extended_features/translation_page_enabled")) {
            systemTray.setMenuOptions(
                stringList(storage::ApplicationStorage::instance().configuration().value(
                    kTrayMenuOptionsKey)));
        }
        if (key == u"updates/mode" && updates != nullptr) {
            updates->setMode(value.toString());
        } else if (key == u"network/proxy" && updates != nullptr) {
            updates->setSystemProxy(value.toString() == u"system");
        } else if (key == kPinBorderColorKey) {
            QColor color = storage::colorFromRgbaString(value.toString());
            if (!color.isValid()) {
                color = QColor(219, 219, 219, 255);
            }
            ScreenshotPinnedWindow::setRuntimeBorderColor(color);
        } else if (key == kPinBorderActiveColorKey) {
            QColor color = storage::colorFromRgbaString(value.toString());
            if (!color.isValid()) {
                color = QColor(105, 177, 255, 255);
            }
            ScreenshotPinnedWindow::setRuntimeBorderActiveColor(color);
        } else if (key == kTrayEnabledKey) {
            const bool enabled = value.isBool() ? value.toBool() : true;
            systemTray.setEnabled(enabled);
            ScreenshotPinnedWindow::setRuntimeTrayEnabled(enabled);
        } else if (key == kTrayIconKey) {
            systemTray.setIconSelection(value.toString(QStringLiteral("default")));
        } else if (key == kTrayCustomIconKey) {
            systemTray.setCustomIconPath(value.toString());
        } else if (key == kTrayLeftClickActionKey) {
            systemTray.setLeftClickAction(value.toString(QStringLiteral("screenshot")));
        } else if (key == kTrayMiddleClickActionKey) {
            systemTray.setMiddleClickAction(value.toString(QStringLiteral("screenshot_fixed")));
        } else if (key == kTrayMenuOptionsKey) {
            systemTray.setMenuOptions(stringList(value));
        } else if (key == kScreenshotDelaySecondsKey) {
            systemTray.setScreenshotDelaySeconds(value.toInt(3));
        } else if (key == kFullscreenSuppressionKey) {
            systemTray.setQuickActionChecked(
                presentation::GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow,
                value.toBool());
        } else if (key == kOcrModelTypeKey || key == kOcrDirectMlKey ||
                   key == QStringLiteral("text_recognition/resident_process") ||
                   key == QStringLiteral("text_recognition/model_hot_start")) {
            if (started)
                applyOcrConfiguration();
        }
    }

    MainWindow& ensureMainWindow() {
        if (mainWindow == nullptr) {
            ensureSettingsRuntime();
            mainWindow = new MainWindow(*settingsRegistry, *runtimeSession, nullptr,
                                        translationClient.get());
            QObject::connect(mainWindow, &QObject::destroyed, &q,
                             [this]() { mainWindow = nullptr; });
            QObject::connect(mainWindow, &MainWindow::screenshotRequested, &q, [this]() {
                if (!allowPermissions(presentation::requiredPermissions(
                        presentation::GlobalShortcutAction::Screenshot,
                        permissions.microphoneEnabled())))
                    return;
                static_cast<void>(featureRouter.dispatch(FeatureFamily::Screenshot, [this]() {
                    if (ScreenshotController* controller = ensureScreenshotController()) {
                        controller->startCapture();
                    }
                }));
            });
            QObject::connect(
                mainWindow, &MainWindow::quickActionRequested, &q,
                [this](presentation::GlobalShortcutAction action) { dispatchQuickAction(action); });
            QObject::connect(mainWindow, &MainWindow::globalMouseDragRequested, &q,
                             [this](presentation::settings::SettingsGlobalMouseAction action) {
                                 if (!allowPermissions(presentation::requiredPermissions(
                                         action, permissions.microphoneEnabled())))
                                     return;
                                 static_cast<void>(featureRouter.dispatch(
                                     featureFamilyFor(action), [this, action]() {
                                         globalMouseManager.beginButtonDrag(action);
                                     }));
                             });
            QObject::connect(
                mainWindow, &MainWindow::screenshotHistoryEditRequested, &q,
                [this](const QString& recordId) {
                    static_cast<void>(
                        featureRouter.dispatch(FeatureFamily::Screenshot, [this, recordId]() {
                            if (ScreenshotController* controller = ensureScreenshotController()) {
                                controller->editHistoryRecord(recordId);
                            }
                        }));
                });
            QObject::connect(
                mainWindow, &MainWindow::screenshotHistoryPinRequested, &q,
                [this](const QString& recordId) {
                    static_cast<void>(
                        featureRouter.dispatch(FeatureFamily::PinToScreen, [this, recordId]() {
                            if (ScreenshotController* controller = ensureScreenshotController()) {
                                controller->pinHistoryRecord(recordId);
                            }
                        }));
                });
        }
        return *mainWindow;
    }

    void ensureSettingsRuntime() {
        if (settingsRegistry == nullptr) {
            settingsRegistry = std::make_unique<presentation::settings::SettingsRegistry>(
                presentation::settings::buildBuiltInSettingsRegistry());
        }
        if (settingsBackend == nullptr) {
            settingsBackend = std::make_unique<presentation::settings::BuiltInSettingsBackend>(
                globalShortcutManager, nullptr, &globalMouseManager, &permissions);
        }
        if (runtimeSession == nullptr) {
            runtimeSession = std::make_unique<presentation::settings::SettingsRuntimeSession>(
                *settingsRegistry, *settingsBackend);
        }
    }

    bool allowPermissions(const presentation::AppPermissions& requirements) {
#ifdef Q_OS_MACOS
        return permissions.allow(requirements, [this](const presentation::AppPermissions& missing) {
            ensureMainWindow().showAppPermissions(presentation::appPermissionId(missing.first()));
        });
#else
        Q_UNUSED(requirements);
#endif
        return true;
    }

    void dispatchQuickAction(presentation::GlobalShortcutAction action) {
        if (!allowPermissions(
                presentation::requiredPermissions(action, permissions.microphoneEnabled())))
            return;
        const std::optional<FeatureFamily> feature = featureFamilyFor(action);
        if (feature && !featureRouter.dispatch(
                           *feature, [this, action]() { dispatchAvailableQuickAction(action); })) {
            return;
        }
        if (!feature) {
            dispatchAvailableQuickAction(action);
        }
    }

    void dispatchAvailableQuickAction(presentation::GlobalShortcutAction action) {
        switch (action) {
        case presentation::GlobalShortcutAction::Screenshot:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->startCapture();
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotDelay:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->startDelayedCapture(storage::ScreenshotSettings().delaySeconds());
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotFixed:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->captureAndPinSelection();
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotOcr:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->captureAndRecognizeText();
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotTranslation:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->captureAndTranslateText();
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotCopy:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->captureAndCopySelection();
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotFullScreen:
            ensureDirectCaptureController().captureCurrentMonitor();
            break;
        case presentation::GlobalShortcutAction::ScreenshotFocusedWindow:
            ensureDirectCaptureController().captureFocusedWindow();
            break;
        case presentation::GlobalShortcutAction::ScreenRecord:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->captureAndStartScreenRecording();
            }
            break;
        case presentation::GlobalShortcutAction::ScreenRecordCopy:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->startOrStopScreenRecordingAndCopy();
            }
            break;
        case presentation::GlobalShortcutAction::OpenScreenRecordingFolder:
            static_cast<void>(presentation::recording::openScreenRecordingFolder());
            break;
        case presentation::GlobalShortcutAction::OpenCaptureHistory:
            ensureMainWindow().showScreenshotHistory();
            break;
        case presentation::GlobalShortcutAction::OpenPinToScreenManagement:
            ensureMainWindow().showPinToScreenManagement();
            break;
        case presentation::GlobalShortcutAction::OpenSettings:
            showInterfaceSettings();
            break;
        case presentation::GlobalShortcutAction::TranslateSelectedText:
            if (storage::ExtendedFeaturesSettings().translationPageEnabled()) {
                ensureSelectedTextTranslationCoordinator().capture();
            }
            break;
        case presentation::GlobalShortcutAction::PinSelectedFiles: {
            const auto target = platform::createSelectedFileBackend()->captureTarget();
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->pinSelectedFilesToScreen(target);
            }
            break;
        }
        case presentation::GlobalShortcutAction::RestoreLastClosedWindows:
            if (ScreenshotController* controller = ensureScreenshotController())
                controller->restoreLastClosedPinnedWindow();
            break;
        case presentation::GlobalShortcutAction::PinClipboardContent:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->pinClipboardContentToScreen();
            }
            break;
        case presentation::GlobalShortcutAction::ToggleGlobalHotkeys:
            globalShortcutManager.setGlobalHotkeysEnabled(
                !globalShortcutManager.globalHotkeysEnabled());
            break;
        case presentation::GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow: {
            storage::GlobalShortcutSettings shortcutSettings;
            shortcutSettings.setDisableOnFocusedFullscreenWindow(
                !shortcutSettings.disableOnFocusedFullscreenWindow());
            break;
        }
        }
    }

    presentation::SelectedTextTranslationCoordinator& ensureSelectedTextTranslationCoordinator() {
        if (!selectedTextTranslationCoordinator) {
            selectedTextTranslationCoordinator =
                std::make_unique<presentation::SelectedTextTranslationCoordinator>(
                    storage::ApplicationStorage::instance().configuration(),
                    translationClient.get());
            QObject::connect(
                selectedTextTranslationCoordinator.get(),
                &presentation::SelectedTextTranslationCoordinator::mainTranslationRequested, &q,
                [this](const QString& text) {
                    if (storage::ExtendedFeaturesSettings().translationPageEnabled()) {
                        ensureMainWindow().showTranslation(text);
                    }
                });
            QObject::connect(&app, &QCoreApplication::aboutToQuit,
                             selectedTextTranslationCoordinator.get(),
                             &presentation::SelectedTextTranslationCoordinator::shutdown);
        }
        return *selectedTextTranslationCoordinator;
    }

    presentation::DirectCaptureController& ensureDirectCaptureController() {
        if (!directCaptureController) {
            directCaptureController = std::make_unique<presentation::DirectCaptureController>(&q);
            QObject::connect(directCaptureController.get(),
                             &presentation::DirectCaptureController::operationFailed, &q,
                             [this](const QString& message, bool warning) {
                                 systemTray.showCaptureMessage(message, warning);
                             });
            QObject::connect(&app, &QCoreApplication::aboutToQuit, directCaptureController.get(),
                             &presentation::DirectCaptureController::shutdown);
        }
        return *directCaptureController;
    }

    void showMainWindow() {
        ensureMainWindow().showAndActivate();
    }

    void restorePinnedWindows() {
        static_cast<void>(featureRouter.dispatch(FeatureFamily::PinToScreen, [this]() {
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->restorePinnedWindows();
            }
        }));
    }

    void dispatchGlobalMouseEvent(const presentation::GlobalMouseDragEvent& event) {
        auto* controller = ensureScreenshotController();
        using Kind = presentation::GlobalMouseDragEvent::Kind;
        switch (event.kind) {
        case Kind::Begin:
            if (!controller->beginGlobalMouseCapture(event.action, event.id, event.position,
                                                     event.coordinateSpace)) {
                globalMouseManager.cancelGesture(event.id);
            }
            break;
        case Kind::Update:
            controller->updateGlobalMouseCapture(event.id, event.position);
            break;
        case Kind::Finish:
            controller->finishGlobalMouseCapture(event.id, event.position);
            break;
        case Kind::Cancel:
            controller->cancelGlobalMouseCapture(event.id);
            break;
        }
    }

    QString unavailableFeatureMessage(FeatureFamily feature) const {
        Q_UNUSED(feature);
        return {};
    }

    QString unavailableFeatureKey(FeatureFamily feature) const {
        Q_UNUSED(feature);
        return {};
    }

    void showUnavailableFeatureInWindow(MainWindow& window, FeatureFamily feature) {
        adqt::widgets::AdMessage::Request request;
        request.key = unavailableFeatureKey(feature);
        request.content = unavailableFeatureMessage(feature);
        adqt::widgets::AdMessageService::warning(std::move(request), &window);
    }

    void showUnavailableFeature(FeatureFamily feature) {
        if (mainWindow != nullptr && mainWindow->isVisible() && !mainWindow->isMinimized()) {
            showUnavailableFeatureInWindow(*mainWindow, feature);
            return;
        }
        if (systemTray.canShowMessages()) {
            systemTray.showWarningMessage(ApplicationController::tr("Feature unavailable"),
                                          unavailableFeatureMessage(feature));
            return;
        }
        MainWindow& window = ensureMainWindow();
        window.showAndActivate();
        showUnavailableFeatureInWindow(window, feature);
    }

    void showInterfaceSettings() {
        ensureMainWindow().showInterfaceSettings();
    }

    ApplicationController& q;
    QApplication& app;
    ApplicationRestartCoordinator restartCoordinator;
    // These services outlive the disposable configuration window.
    presentation::PinnedWindowGroupManager groupManager;
    presentation::SystemTrayController systemTray;
    FeatureActionRouter featureRouter;
    presentation::GlobalShortcutManager globalShortcutManager;
    presentation::AppPermissionService permissions;
#ifdef Q_OS_MACOS
    presentation::PermissionGuideController permissionGuide{permissions};
#endif
    presentation::GlobalMouseManager globalMouseManager;
    // Settings are intentionally constructed on first window access.  The
    // tray and shortcut manager use only their compact bootstrap data.
    std::unique_ptr<presentation::settings::SettingsRegistry> settingsRegistry;
    std::unique_ptr<presentation::settings::BuiltInSettingsBackend> settingsBackend;
    std::unique_ptr<presentation::settings::SettingsRuntimeSession> runtimeSession;
    std::unique_ptr<SnowShotApiClient> translationClient;
    translation::TranslationService* translationService = nullptr;
    std::unique_ptr<ScreenshotOcrRecognitionService> ocrRecognition;
    std::unique_ptr<ScreenshotController> screenshotController;
    std::unique_ptr<mcp::ScreenshotMcpServer> mcpServer;
    std::unique_ptr<mcp::ScreenshotMcpSession> mcpSession;
    std::unique_ptr<mcp::McpJobRegistry> mcpJobs;
    std::unique_ptr<mcp::McpApplicationService> mcpApplication;
    std::unique_ptr<mcp::McpDocumentService> mcpDocuments;
    std::unique_ptr<mcp::McpMediaService> mcpMedia;
    QHash<QString, std::shared_ptr<std::atomic_bool>> mcpSources;
    QHash<QString, QTimer*> mcpSourceDelays;
    quint64 mcpSourceCaptureOwner = 0;
    QString mcpSourceCaptureRequest;
    QThreadPool mcpSourceWorkers;
    int mcpSourceWork = 0;
    std::unique_ptr<presentation::DirectCaptureController> directCaptureController;
    std::unique_ptr<presentation::SelectedTextTranslationCoordinator>
        selectedTextTranslationCoordinator;
    QPointer<MainWindow> mainWindow;
#ifdef Q_OS_MACOS
    std::unique_ptr<platform::macos::ApplicationReopenHandler> reopenHandler;
#endif
    bool started = false;
    update::UpdateService* updates = nullptr;
};

ApplicationController::ApplicationController(QApplication& application, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, application)) {}

ApplicationController::~ApplicationController() = default;

void ApplicationController::start() {
    m_impl->start();
}

void ApplicationController::showMainWindow() {
    m_impl->showMainWindow();
}

void ApplicationController::handleLaunchRequest(const QStringList& arguments) {
    if (arguments.contains(QStringLiteral("--autostart"))) {
        return;
    }
    m_impl->showMainWindow();
}

void ApplicationController::restorePinnedWindows() {
    m_impl->restorePinnedWindows();
}
} // namespace snow_shot::app

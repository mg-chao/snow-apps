#include "snow_shot/app/edition.h"
#include "snow_shot/app/applicationcontroller.h"
#include "snow_shot/app/distribution.h"
#include "snow_shot/app/storeupdatelifecycle.h"
#include "snow_shot/app/shellimagelaunch.h"
#include "snow_shot/app/shelllaunchcoordinator.h"
#include "snow_shot/runtime/memoryoptimizationcontroller.h"
#include "snow_shot/runtime/memoryoptimizationuisafety.h"
#include "snow_shot/app/applicationrestart.h"
#include "snow_shot/app/applicationactiongate.h"
#include "snow_shot/app/applicationinputguard.h"
#include "snow_shot/presentation/apppermissionservice.h"
#ifdef Q_OS_MACOS
#include "snow_shot/platform/macos/applicationactivation.h"
#include "snow_shot/presentation/permissionguidecontroller.h"
#endif
#include "snow_shot/platform/windows/administratorlaunch.h"
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/app/accountsessionservice.h"
#endif
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "theme/theme_manager.h"
#include "snow_shot/update/updateservice.h"
#include "snow_shot/update/startupupdate.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/collagewindowcoordinator.h"
#include "snow_shot/presentation/imagesplitwindow.h"
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotexportcache.h"

#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/globalcanvascontroller.h"
#include "snow_shot/presentation/globalmousemanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/pinnedwindowstackingcoordinator.h"
#include "snow_shot/presentation/windowgroupswitchercontroller.h"
#include "snow_shot/presentation/screenshotcontroller.h"
#include "snow_shot/presentation/screenshotautofiltercontroller.h"
#include "snow_shot/presentation/directcapturecontroller.h"
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "snow_shot/presentation/selectedtexttranslationcoordinator.h"
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "snow_shot/presentation/selectedtexttranslationcontroller.h"
#endif
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotocrproxyconfiguration.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenrecordingfolder.h"
#include "snow_shot/presentation/systemtraycontroller.h"
#include "snow_shot/presentation/systemnotificationfeedback.h"
#include "snow_shot/presentation/floatingtoolbarcontroller.h"
#include "snow_shot/app/mcp/mcpruntimelifecycle.h"
#include "snow_shot/app/mcp/mcpruntimegraph.h"
#include "snow_shot/app/operationadmission.h"
#include "snow_shot/app/quiescencesession.h"
#include "snow_shot/app/deferredstartup.h"
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
#include <QEvent>
#include <QDir>
#include <QJsonArray>
#include <QJsonValue>
#include <QPointer>
#include <QScopeGuard>
#include <QScreen>
#include <QTimer>
#include <QtMath>
#include <QUuid>

#include <memory>
#include <utility>
#include <future>
#include <thread>
#include <vector>
#ifdef Q_OS_WIN
#include "snow_shot/platform/windows/windowalwaysontop.h"
#include <qt_windows.h>
#endif

namespace snow_shot::app {
namespace {
const QString kPinBorderColorKey = QStringLiteral("pin_to_screen/border_color");
const QString kPinBorderActiveColorKey = QStringLiteral("pin_to_screen/border_active_color");
const QString kPinLockedBorderColorKey = QStringLiteral("pin_to_screen/locked_border_color");
const QString kPinShadowColorKey = QStringLiteral("pin_to_screen/shadow_color");
const QString kPinShadowActiveColorKey = QStringLiteral("pin_to_screen/shadow_active_color");
const QString kPinLockedShadowColorKey = QStringLiteral("pin_to_screen/locked_shadow_color");
const QString kTrayEnabledKey = QStringLiteral("tray/enabled");
const QString kTrayUseSystemIconColorKey = QStringLiteral("tray/use_system_icon_color");
const QString kTrayIconKey = QStringLiteral("tray/icon");
const QString kTrayCustomIconKey = QStringLiteral("tray/custom_icon");
const QString kTrayDarkIconKey = QStringLiteral("tray/dark_icon");
const QString kTrayDarkCustomIconKey = QStringLiteral("tray/dark_custom_icon");
const QString kTrayLeftClickActionKey = QStringLiteral("tray/left_click_action");
const QString kTrayMiddleClickActionKey = QStringLiteral("tray/middle_click_action");
const QString kTrayMenuOptionsKey = QStringLiteral("tray/menu_options");
const QString kScreenshotDelaySecondsKey = QStringLiteral("screenshot/delay_seconds");
const QString kFullscreenSuppressionKey =
    QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window");
const QString kOcrModelTypeKey = QStringLiteral("text_recognition/model_type");
const QString kOcrDetectorResizePolicyKey =
    QStringLiteral("text_recognition/detector_resize_policy");
const QString kOcrTextDetectionProcessingKey =
    QStringLiteral("text_recognition/text_detection_processing");
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
    enum class QuickActionOrigin { Other, GlobalShortcut };
    enum class OwnedPause { None, Relocation, UpdateHandoff };

    Impl(ApplicationController& owner, QApplication& application)
        : q(owner), app(application), restartCoordinator(application),
          groupManager(initializedPinnedWindowRepository()),
          systemTray(presentation::settings::builtInTrayCommandManifest(), &groupManager) {
        QObject::connect(&systemTray,
                         &presentation::SystemTrayController::notificationDeliveryFinished, &q,
                         [this](const platform::SystemNotificationRequest& request,
                                const platform::SystemNotificationResult& result) {
                             presentation::presentSystemNotificationResult(
                                 request, result, [this]() -> QWidget* {
                                     MainWindow& window = ensureMainWindow();
                                     window.showAndActivate();
                                     return &window;
                                 });
                         });
        QObject::connect(&floatingToolbar,
                         &presentation::FloatingToolbarController::actionRequested, &q,
                         [this](const QString& action) { dispatchFloatingAction(action); });
        QObject::connect(&floatingToolbar,
                         &presentation::FloatingToolbarController::customizeRequested, &q, [this] {
                             ensureMainWindow().showSettingsLocation(
                                 QStringLiteral("desktop-tools"),
                                 QStringLiteral("floating-toolbar"));
                         });
        QObject::connect(
            &floatingToolbar, &presentation::FloatingToolbarController::contentDropped, &q,
            [this](ScreenshotClipboardContentSnapshot snapshot, QStringList paths) {
                if (storage::ApplicationStorage::instance().directoryChanging())
                    return;
                static_cast<void>(actionGate.dispatch(
                    [this, snapshot = std::move(snapshot), paths = std::move(paths)]() mutable {
                        if (auto* controller = ensureScreenshotController())
                            controller->pinDroppedContent(std::move(snapshot), std::move(paths));
                    }));
            });
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &floatingToolbar,
                         &presentation::FloatingToolbarController::shutdown);
        QObject::connect(
            &systemTray, &presentation::SystemTrayController::screenshotRequested, &q,
            [this]() { dispatchQuickAction(presentation::GlobalShortcutAction::Screenshot); });
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
                         [this]() { ensureMainWindow().showScreenshotSettings(); });
        QObject::connect(&systemTray, &presentation::SystemTrayController::openAboutRequested, &q,
                         [this]() { ensureMainWindow().showAbout(); });
        QObject::connect(
            &systemTray, &presentation::SystemTrayController::openRecordingFileRequested, &q,
            [](const QString& path) {
                static_cast<void>(presentation::recording::revealScreenRecordingFile(path));
            });
        QObject::connect(&systemTray, &presentation::SystemTrayController::exitRequested, &q, []() {
            if (storage::ApplicationStorage::instance().directoryChanging())
                return;
            QApplication::quit();
        });
        QObject::connect(
            &systemTray, &presentation::SystemTrayController::quickActionRequested, &q,
            [this](presentation::GlobalShortcutAction action) { dispatchQuickAction(action); });
        auto& pinnedStorage = storage::ApplicationStorage::instance();
        QObject::connect(&pinnedStorage, &storage::ApplicationStorage::pinnedWindowShowRequested,
                         &q, [this](const QString& id) {
                             static_cast<void>(actionGate.dispatch([this, id] {
                                 if (auto* controller = ensureScreenshotController())
                                     controller->showPinnedRecord(id);
                             }));
                         });
        QObject::connect(&pinnedStorage, &storage::ApplicationStorage::pinnedWindowsShowRequested,
                         &q, [this](const QStringList& ids) {
                             static_cast<void>(actionGate.dispatch([this, ids] {
                                 if (auto* controller = ensureScreenshotController())
                                     controller->showPinnedRecords(ids);
                             }));
                         });
        QObject::connect(&pinnedStorage, &storage::ApplicationStorage::pinnedWindowDeleteRequested,
                         &q, [this](const QVector<QString>& ids) {
                             static_cast<void>(actionGate.dispatch([this, ids] {
                                 if (auto* controller = ensureScreenshotController())
                                     controller->destroyPinnedRecords(ids);
                             }));
                         });
        QObject::connect(&pinnedStorage, &storage::ApplicationStorage::pinnedWindowsChanged,
                         &groupManager,
                         &presentation::PinnedWindowGroupManager::onPinnedRecordsChanged);
        QObject::connect(
            &groupManager,
            &presentation::PinnedWindowGroupManager::restoreActiveGroupWindowsRequested, &q,
            [this]() {
                static_cast<void>(actionGate.dispatch([this]() {
                    if (ScreenshotController* controller = ensureScreenshotController()) {
                        controller->restoreActivePinnedGroupWindows();
                    }
                }));
            });
        QObject::connect(&globalShortcutManager, &presentation::GlobalShortcutManager::activated,
                         &q, [this](presentation::GlobalShortcutAction action) {
                             if (action != presentation::GlobalShortcutAction::SwitchWindowGroup)
                                 dispatchQuickAction(action, QuickActionOrigin::GlobalShortcut);
                         });
        QObject::connect(&globalShortcutManager,
                         &presentation::GlobalShortcutManager::bindingActivated, &q,
                         [this](presentation::GlobalShortcutAction action, int registrationId) {
                             if (action == presentation::GlobalShortcutAction::SwitchWindowGroup)
                                 ensureWindowGroupSwitcher().activateShortcut(registrationId);
                         });
        QObject::connect(&globalShortcutManager, &presentation::GlobalShortcutManager::stateChanged,
                         &q,
                         [this](presentation::GlobalShortcutAction action,
                                const presentation::GlobalShortcutRegistrationState& state) {
                             systemTray.setGlobalShortcuts(action, state.shortcuts);
                         });
        QObject::connect(&globalShortcutManager,
                         &presentation::GlobalShortcutManager::globalShortcutsEnabledChanged, &q,
                         [this](bool enabled) {
                             systemTray.setQuickActionChecked(
                                 presentation::GlobalShortcutAction::ToggleGlobalShortcuts,
                                 !enabled);
                         });
        QObject::connect(
            &globalShortcutManager,
            &presentation::GlobalShortcutManager::focusedFullscreenSuppressionChanged, &systemTray,
            &presentation::SystemTrayController::setFocusedFullscreenSuppressionActive);
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
        QObject::connect(&globalMouseManager, &presentation::GlobalMouseManager::dragEvent, &q,
                         [this](const presentation::GlobalMouseDragEvent& event) {
                             using Kind = presentation::GlobalMouseDragEvent::Kind;
                             if (event.kind == Kind::Begin) {
                                 const QPointer<ApplicationController> lifetime(&q);
                                 static_cast<void>(actionGate.beginGesture(
                                     [this, lifetime, id = event.id] {
                                         if (lifetime)
                                             globalMouseManager.cancelGesture(id);
                                     },
                                     [this, event] { dispatchGlobalMouseEvent(event); },
                                     [this, event] {
                                         return allowPermissions(presentation::requiredPermissions(
                                             event.action, permissions.microphoneEnabled()));
                                     }));
                                 return;
                             }
                             static_cast<void>(actionGate.dispatch(
                                 [this, event]() { dispatchGlobalMouseEvent(event); }));
                         });
        auto& applicationStorage = storage::ApplicationStorage::instance();
        auto* stacking = groupManager.stackingCoordinator();
        QObject::connect(&applicationStorage, &storage::ApplicationStorage::checkpointRequested,
                         stacking, &presentation::PinnedWindowStackingCoordinator::checkpoint);
        QObject::connect(&app, &QCoreApplication::aboutToQuit, stacking,
                         &presentation::PinnedWindowStackingCoordinator::shutdown);
        if (!applicationStorage.isInitialized()) {
            static_cast<void>(applicationStorage.initialize());
        }
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        translationClient =
            std::make_unique<SnowShotApiClient>(SnowShotApiClient::configuredBaseUrl(
                applicationStorage.configuration()
                    .value(QStringLiteral("api_configuration/server_url"))
                    .toString()));
        auto* accountSession = AccountSessionService::forClient(translationClient.get());
        QObject::connect(accountSession, &AccountSessionService::registrationActivated, &q, [this] {
            static_cast<void>(actionGate.dispatch(
                [this] { ensureMainWindow().showSettingsLocation(QStringLiteral("account")); }));
        });
        translationService = &translation::TranslationService::forClient(
            *translationClient, applicationStorage.configuration(),
            presentation::LanguageManager::instance().currentLocale());
        QObject::connect(&presentation::LanguageManager::instance(),
                         &presentation::LanguageManager::languageChanged, translationService,
                         [this](const QString&, const QLocale& locale) {
                             translationService->setLocale(locale);
                         });
#endif
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
        ocrOptions.detectorResizePolicy = screenshotOcrDetectorResizePolicyFromValue(
            applicationStorage.configuration().value(kOcrDetectorResizePolicyKey).toString());
        ocrOptions.textDetectionProcessing = screenshotOcrTextDetectionProcessingFromValue(
            applicationStorage.configuration().value(kOcrTextDetectionProcessingKey).toString());
        ocrRecognition = std::make_unique<ScreenshotOcrRecognitionService>(ocrOptions, &q);
        auto& configuration = applicationStorage.configuration();
        presentation::bindOcrProxyConfiguration(*ocrRecognition, configuration);
        updates = new update::UpdateService(update::defaultUpdateServiceOptions(), &app);
        updates->setProgressAppearance(
            presentation::styles::ThemeManager::instance().updateProgressAppearance());
        QObject::connect(
            &adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
            updates, [updateService = updates] {
                updateService->setProgressAppearance(
                    presentation::styles::ThemeManager::instance().updateProgressAppearance());
            });
        updates->setMode(configuration.value(QStringLiteral("updates/mode")).toString());
        updates->setProxyConfiguration(
            configuration.value(QStringLiteral("network/proxy")).toString(),
            configuration.value(QStringLiteral("network/http_proxy")).toString());
        QObject::connect(&presentation::LanguageManager::instance(),
                         &presentation::LanguageManager::languageChanged, updates,
                         [updateService = updates] {
                             QEvent event(QEvent::LanguageChange);
                             QCoreApplication::sendEvent(updateService, &event);
                         });
        QObject::connect(
            updates, &update::UpdateService::automaticUpdateAvailable, &q,
            [this](const QString& version) {
                systemTray.showUpdateMessage(
                    (edition::isMini
                         ? ApplicationController::tr(
                               "%1 %2 is available. Open About for update options")
                               .arg(edition::productName(), version)
                         : ApplicationController::tr(
                               "Snow Shot %1 is available. Open About for update options")
                               .arg(version)));
            });
#ifndef Q_OS_MACOS
        QObject::connect(updates, &update::UpdateService::updateReady, &q, [this] {
            systemTray.showUpdateMessage(
                (edition::isMini
                     ? ApplicationController::tr(
                           "An update is ready. Open About to restart and update %1")
                           .arg(edition::productName())
                     : ApplicationController::tr(
                           "An update is ready. Open About to restart and update Snow Shot")));
        });
#endif
        platform::windows::setAdministratorRestartGuard([this] { return restartAllowed(); });
#ifdef Q_OS_WIN
        migrationInput = new ApplicationInputGuard(&q);
        app.installEventFilter(migrationInput);
        applicationStorage.setDirectoryChangeHooks(
            [this] {
                if (!inspectAdmission(ApplicationOperation::Relocation).allowed())
                    return storage::StorageResult::failure(ApplicationController::tr(
                        "Finish capturing, recording, exporting, recognizing text, or updating "
                        "before changing storage"));
                auto pause = std::make_unique<DirectoryPause>();
                pause->sourceRoot =
                    storage::ApplicationStorage::instance().configurationDirectory();
                pause->restoredRoot = pause->sourceRoot;
                acquireApplicationPause(pause->resources, *migrationInput);
                auto* context = pause.get();
                // Restored root is assigned by storage after its durable transaction resolves.
                for (auto* window : QApplication::topLevelWidgets()) {
                    if (auto* pin = qobject_cast<ScreenshotPinnedWindow*>(window)) {
                        pin->suspendStorageWrites();
                        pause->resources.acquired(
                            [pin = QPointer<ScreenshotPinnedWindow>(pin), context] {
                                if (pin)
                                    pin->resumeStorageWrites(context->sourceRoot,
                                                             context->restoredRoot);
                            });
                    }
                }
                if (ocrRecognition) {
                    ocrRecognition->suspendStorage();
                    pause->resources.acquired([this, context] {
                        ocrRecognition->resumeStorage(
                            QDir(context->restoredRoot).filePath(QStringLiteral("assets/ocr")));
                    });
                }
                // Suspension can publish checkpoints and retire listeners synchronously.
                // Exclude only our reservation after every participant has finished acquiring.
                if (!inspectAdmission(ApplicationOperation::Relocation, OwnedPause::Relocation)
                         .allowed())
                    return storage::StorageResult::failure(ApplicationController::tr(
                        "Finish capturing, recording, exporting, recognizing text, or updating "
                        "before changing storage"));
                migrationPause = std::move(pause);
                return storage::StorageResult::ok();
            },
            [this](const QString& root) {
                if (migrationPause) {
                    migrationPause->restoredRoot = root;
                    migrationPause.reset();
                }
                reconcileMcpConfiguration();
            },
            [this] {
                if (ocrRecognition)
                    ocrRecognition->drainStorage();
            });
        QObject::connect(&applicationStorage, &storage::ApplicationStorage::directoryChangeFinished,
                         &q, [this] {
                             const auto intent = shellLaunches.takeStorage();
                             if (intent.paths.isEmpty())
                                 return;
                             q.handleLaunchRequest(intent.replayArguments());
                         });
#endif
        QObject::connect(updates, &update::UpdateService::restartRequested, &q, [this] {
            if (platform::windows::administratorOperationPending())
                return;
            if (!inspectAdmission(ApplicationOperation::UpdateRequest).allowed()) {
                updates->reportBlocked(ApplicationController::tr(
                    "Finish capturing, recording, or exporting before updating"));
                return;
            }
            if (!storage::ApplicationStorage::instance().flushNow().success) {
                updates->reportBlocked(ApplicationController::tr(
                    "Your settings could not be saved. Please retry before updating"));
                return;
            }
            if (imageEditingWindowCoordinator) {
                imageEditingWindowCoordinator->confirmShutdown([this](bool accepted) {
                    if (!accepted ||
                        !inspectAdmission(ApplicationOperation::UpdateRequest).allowed())
                        return;
                    imageEditingWindowCoordinator->shutdown();
                    updates->beginApply();
                });
            } else
                updates->beginApply();
        });
        handoffInput = new ApplicationInputGuard(&q);
        app.installEventFilter(handoffInput);
        QObject::connect(updates, &update::UpdateService::handoffPendingChanged, &q,
                         [this](bool pending) { setUpdateHandoffPending(pending); });
        updates->setHandoffApproval(&q, [this] {
            const QPointer<ApplicationController> lifetime(&q);
            const auto prepare = [this, &lifetime] {
                if (!inspectAdmission(ApplicationOperation::UpdateHandoff).allowed())
                    return false;
                if (imageEditingWindowCoordinator &&
                    !imageEditingWindowCoordinator->prepareForUpdateHandoff())
                    return false;
                return lifetime && inspectAdmission(ApplicationOperation::UpdateHandoff).allowed();
            };
            const auto refuse = [this, &lifetime] {
                if (lifetime)
                    updates->reportBlocked(ApplicationController::tr(
                        "Finish capturing, recording, or exporting before updating"));
                return false;
            };
            if (!prepare())
                return refuse();
            const bool flushed = storage::ApplicationStorage::instance().flushNow().success;
            if (!lifetime)
                return false;
            // Checkpoint observers can admit work or create/change an editor. Reprepare pending
            // edits without prompting, then inspect immediately before the updater freezes input.
            if (!flushed || !prepare())
                return refuse();
            return true;
        });
        updates->setHandoffValidation(&q, [this] {
            const QPointer<ApplicationController> lifetime(&q);
            // Pause acquisition can synchronously publish checkpoints or retire MCP resources.
            // Ignore only this handoff's reservation, retaining every newly observed blocker.
            const auto prepare = [this, &lifetime] {
                if (!handoffPause || !inspectAdmission(ApplicationOperation::UpdateHandoff,
                                                       OwnedPause::UpdateHandoff)
                                          .allowed())
                    return false;
                if (imageEditingWindowCoordinator &&
                    !imageEditingWindowCoordinator->prepareForUpdateHandoff())
                    return false;
                return lifetime && handoffPause &&
                       inspectAdmission(ApplicationOperation::UpdateHandoff,
                                        OwnedPause::UpdateHandoff)
                           .allowed();
            };
            if (!prepare())
                return false;
            // Closing admission can checkpoint stacking/configuration after initial approval's
            // flush. Persist that state with the pause held, then recheck flush observers too.
            const bool flushed = storage::ApplicationStorage::instance().flushNow().success;
            if (!lifetime)
                return false;
            if (!flushed) {
                updates->reportBlocked(ApplicationController::tr(
                    "Your settings could not be saved. Please retry before updating"));
                return false;
            }
            return prepare();
        });
        QObject::connect(updates, &update::UpdateService::handoffCommitted, &q, [this] {
            if (handoffPause)
                handoffPause->commit();
            globalShortcutManager.setGlobalShortcutsEnabled(false);
            globalMouseManager.shutdown();
            // Keep resources paused, but permit the terminal Quit event through the input filter.
            handoffInput->active = false;
            QApplication::quit();
        });
        applyRuntimeConfiguration(configuration.value(kPinBorderColorKey), kPinBorderColorKey);
        applyRuntimeConfiguration(configuration.value(kPinBorderActiveColorKey),
                                  kPinBorderActiveColorKey);
        applyRuntimeConfiguration(configuration.value(kPinLockedBorderColorKey),
                                  kPinLockedBorderColorKey);
        applyRuntimeConfiguration(configuration.value(kPinShadowColorKey), kPinShadowColorKey);
        applyRuntimeConfiguration(configuration.value(kPinShadowActiveColorKey),
                                  kPinShadowActiveColorKey);
        applyRuntimeConfiguration(configuration.value(kPinLockedShadowColorKey),
                                  kPinLockedShadowColorKey);
        applyRuntimeConfiguration(configuration.value(kTrayEnabledKey), kTrayEnabledKey);
        applyRuntimeConfiguration(configuration.value(kTrayUseSystemIconColorKey),
                                  kTrayUseSystemIconColorKey);
        applyRuntimeConfiguration(configuration.value(kTrayIconKey), kTrayIconKey);
        applyRuntimeConfiguration(configuration.value(kTrayCustomIconKey), kTrayCustomIconKey);
        applyRuntimeConfiguration(configuration.value(kTrayDarkIconKey), kTrayDarkIconKey);
        applyRuntimeConfiguration(configuration.value(kTrayDarkCustomIconKey),
                                  kTrayDarkCustomIconKey);
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
        mcpLifecycle = std::make_unique<mcp::McpRuntimeLifecycle>(mcp::McpRuntimeLifecycle::Hooks{
            [this] { return startMcp(); }, [this](auto done) { mcpGraph.drain(std::move(done)); },
            [this] { mcpGraph.stop(); }});
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &q, [this] {
            shuttingDown = true;
            deferredStartup.stop();
            mcpLifecycle->shutdown();
        });
        QObject::connect(&configuration, &storage::ConfigurationStore::valueChanged, &q,
                         [this](const QString& key, const QJsonValue&) {
                             if (key == kMcpEnabledKey) {
                                 // Yield so a settings mutation can return through its current
                                 // service graph.
                                 QTimer::singleShot(0, &q, [this] { reconcileMcpConfiguration(); });
                             }
                         });
        reconcileMcpConfiguration();
#ifdef Q_OS_MACOS
        reopenHandler = std::make_unique<platform::macos::ApplicationReopenHandler>(
            [this]() { showMainWindow(); });
#endif
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        auto memoryOptions = runtime::nativeMemoryOptimizationOptions();
        memoryOptions.enabled = [] {
            return storage::ApplicationStorage::instance()
                       .configuration()
                       .value(QStringLiteral("system/memory_optimization_policy"))
                       .toString() == QStringLiteral("smart_control");
        };
        memoryOptions.safe = [this] { return memoryTrimmingIsSafe(); };
        memoryOptions.trim = [nativeTrim = std::move(memoryOptions.trim)] {
            ScreenshotExportCache::pinnedPixels().clear();
            ScreenshotExportCache::encodedBytes().clear();
            ScreenshotExportCache::encodedFiles().clear();
            return nativeTrim();
        };
        memoryOptimization = std::make_unique<runtime::MemoryOptimizationController>(
            runtime::RuntimeActivityTracker::shared(), std::move(memoryOptions), &q);
#endif
        if (distribution::isStoreManaged()) {
            storeLifecycle = std::make_unique<StoreUpdateLifecycle>(StoreUpdateLifecycle::Hooks{
                [this] { return storeUpdateBusy(); },
                [this](bool pending) { setUpdateHandoffPending(pending); },
                [this] { return checkpointForStoreUpdate(); },
                [this](int timeoutMs) {
                    if (screenshotController)
                        screenshotController->prepareForSessionEnd(timeoutMs);
                },
                [this] {
                    shuttingDown = true;
                    deferredStartup.stop();
                    if (handoffPause)
                        handoffPause->commit();
                    handoffInput->active = true;
                    mcpGraph.setSourcesSuspended(true);
                    if (screenshotController)
                        screenshotController->setCaptureSuspended(true);
                    static_cast<void>(globalShortcutManager.suspendRegistrations());
                    globalMouseManager.setCaptureAvailable(false);
                    systemTray.setEnabled(false);
                },
                [this] { return terminalCheckpointForStoreUpdate(); },
                [this] {
                    return handoffPause &&
                           inspectAdmission(ApplicationOperation::Store, OwnedPause::UpdateHandoff)
                               .allowed();
                },
            });
            storeLifecycle->install();
        }
    }

    ~Impl() {
        shuttingDown = true;
        deferredStartup.stop();
        if (updates) {
            updates->setHandoffApproval(nullptr, {});
            updates->setHandoffValidation(nullptr, {});
        }
        if (mcpLifecycle)
            mcpLifecycle->shutdown();
        storage::ApplicationStorage::instance().waitForDirectoryChange();
        if (handoffPause)
            handoffPause->commit();
        groupManager.stackingCoordinator()->shutdown();
        storeLifecycle.reset();
        if (memoryOptimization)
            memoryOptimization->stop();
        floatingToolbar.shutdown();
        mcpGraph.stop();
        platform::windows::setAdministratorRestartGuard({});
        storage::ApplicationStorage::instance().setDirectoryChangeHooks({}, {});
        if (mainWindow != nullptr) {
            mainWindow->setAttribute(Qt::WA_DeleteOnClose, false);
            delete mainWindow;
        }
    }

    [[nodiscard]] OperationAdmission
    inspectAdmission(ApplicationOperation operation,
                     OwnedPause ownedPause = OwnedPause::None) const {
        const auto& applicationStorage = storage::ApplicationStorage::instance();
        OperationFacts facts;
        facts.suspended =
            (ownedPause != OwnedPause::Relocation && migrationInput && migrationInput->active) ||
            (ownedPause != OwnedPause::UpdateHandoff &&
             (handoffPauseRequested || (handoffInput && handoffInput->active) ||
              (handoffPause && handoffPause->committed())));
        facts.storageChanging = applicationStorage.directoryChanging();
        facts.updaterUnavailableOrApplying =
            !updates || updates->status().state == update::UpdateState::Applying;
        facts.updaterBusy = updates && (updates->busy() || updates->handoffPending());
        facts.captureBusy = screenshotController && screenshotController->blocksApplicationUpdate(
                                                        ownedPause == OwnedPause::None);
        facts.directCaptureBusy =
            directCaptureController && directCaptureController->blocksApplicationUpdate();
        facts.unsavedEditors =
            imageEditingWindowCoordinator && imageEditingWindowCoordinator->hasUnsavedChanges();
        facts.ocrBusy = ocrRecognition && ocrRecognition->storageBusy();
        facts.mcpBusy = mcpGraph.busy();
        facts.diagnosticsBusy =
            operation == ApplicationOperation::Trim
                ? applicationStorage.status().diagnostics.exporting
                : diagnostics::DiagnosticsService::instance().status().exporting;
        facts.exports = ScreenshotExportCoordinator::shared().pendingJobCount() != 0;
        facts.administratorPending = platform::windows::administratorOperationPending();
        if (operation == ApplicationOperation::Trim || operation == ApplicationOperation::Store) {
            facts.captureInteracting =
                screenshotController &&
                screenshotController->blocksMemoryTrimming(ownedPause == OwnedPause::None);
            facts.pinOrToolbarInteraction = floatingToolbar.blocksMemoryTrimming();
        }
        if (operation == ApplicationOperation::Store) {
            facts.globalCanvas = globalCanvasController && globalCanvasController->active();
            for (const auto* window : QApplication::topLevelWidgets()) {
                if (const auto* pin = qobject_cast<const ScreenshotPinnedWindow*>(window);
                    pin && pin->blocksMemoryTrimming())
                    facts.pinOrToolbarInteraction = true;
            }
        }
        if (operation == ApplicationOperation::Trim)
            facts.foreground = runtime::currentProcessOwnsForegroundWindow();
        return inspectOperation(operation, facts);
    }

    [[nodiscard]] bool storeUpdateBusy() const {
        return !inspectAdmission(ApplicationOperation::Store).allowed();
    }

    bool checkpointForStoreUpdate() {
        for (auto* window : QApplication::topLevelWidgets()) {
            if (auto* pin = qobject_cast<ScreenshotPinnedWindow*>(window))
                pin->checkpointStorage();
        }
        return storage::ApplicationStorage::instance().flushNow().success;
    }

    std::shared_future<void> terminalCheckpointForStoreUpdate() {
        groupManager.stackingCoordinator()->shutdown();
        auto& storage = storage::ApplicationStorage::instance();
        auto* configuration = &storage.configuration();
        auto* pins = &storage.pinnedWindows();
        auto completion = std::make_shared<std::promise<void>>();
        auto result = completion->get_future().share();
        // The terminal native callback exits the process directly after its deadline.
        // Keep these existing repositories alive and avoid a destructor-driven worker join.
        // Cooperative queries checkpoint complete live documents. Forced termination finishes
        // already queued pin writes; serializing a large live canvas here could delay capture stop.
        try {
            std::thread([configuration, pins, completion] {
                try {
                    static_cast<void>(configuration->flushNow());
                    static_cast<void>(pins->flush());
                } catch (...) {
                    qWarning("Store update terminal checkpoint failed");
                }
                completion->set_value();
            }).detach();
        } catch (...) {
            qWarning("Could not start Store update terminal checkpoint");
            completion->set_value();
        }
        return result;
    }

    [[nodiscard]] bool restartAllowed() const {
        return inspectAdmission(ApplicationOperation::Restart).allowed();
    }

    [[nodiscard]] bool memoryTrimmingIsSafe() const {
        if (!inspectAdmission(ApplicationOperation::Trim).allowed())
            return false;
        return runtime::memoryOptimizationUiIsSafe([this](const QWidget* window) {
            if (const auto* pin = qobject_cast<const ScreenshotPinnedWindow*>(window))
                return !pin->blocksMemoryTrimming();
            return floatingToolbar.ownsIdleSurface(window);
        });
    }

    void restoreApplicationInput(ApplicationInputGuard& input) {
        const QPointer<ApplicationController> lifetime(&q);
        input.active = &input == handoffInput && handoffPauseRequested;
        reconcileSuspension();
        if (!lifetime)
            return;
        globalMouseManager.setCaptureAvailable(
            !applicationSuspended() &&
            (!screenshotController || screenshotController->captureAvailable()));
        if (!lifetime)
            return;
        systemTray.setEnabled(!applicationSuspended() && storage::ApplicationStorage::instance()
                                                             .configuration()
                                                             .value(kTrayEnabledKey)
                                                             .toBool());
    }

    void acquireApplicationPause(QuiescenceSession& pause, ApplicationInputGuard& input) {
        const QPointer<ApplicationController> lifetime(&q);
        input.active = true;
        pause.acquired([this, lifetime, &input] {
            if (lifetime)
                restoreApplicationInput(input);
        });
        // Close admission before the stacking checkpoint can notify reentrant producers.
        if (screenshotController)
            screenshotController->setCaptureSuspended(true);
        if (!lifetime)
            return;
        mcpGraph.setSourcesSuspended(true);
        if (!lifetime)
            return;
        groupManager.stackingCoordinator()->beginMutation();
        if (!lifetime)
            return;
        pause.acquired([this, lifetime] {
            if (lifetime)
                groupManager.stackingCoordinator()->endMutation();
        });
        reconcileSuspension(); // close delayed/programmatic admission before draining producers
        if (!lifetime)
            return;
        systemTray.setEnabled(false);
        globalMouseManager.setCaptureAvailable(false);
        if (!lifetime)
            return;
        const auto token = globalShortcutManager.suspendRegistrations();
        pause.acquired([this, lifetime, token] {
            if (lifetime)
                globalShortcutManager.resumeRegistrations(token);
        });
    }

    void setUpdateHandoffPending(bool pending) {
        // Retain the latest request, but let the current acquisition/restoration stack
        // finish before changing its resource ownership. In particular a listener-stop
        // observer may cancel the handoff before acquisition has returned.
        if (shuttingDown || (handoffPause && handoffPause->committed()))
            return;
        handoffPauseRequested = pending;
        if (handoffPauseReconciling) {
            // A restoration observer can enter a native event loop before reacquisition.
            // Its new request closes widget input immediately while the old owner is visible.
            if (pending)
                handoffInput->active = true;
            return;
        }
        const QPointer<ApplicationController> lifetime(&q);
        reconcileUpdateHandoffPause();
        // Replay can dispatch another native query. Its pause must acquire and restore
        // synchronously, outside the resource reconciliation stack that has just finished.
        // A failed acquisition may already have retired its owner before this cancellation.
        if (lifetime && !shuttingDown && !handoffPauseRequested && !handoffPause)
            resumeAfterUpdateHandoff();
    }

    void reconcileUpdateHandoffPause() {
        handoffPauseReconciling = true;
        const QPointer<ApplicationController> lifetime(&q);
        const auto reconciliation = qScopeGuard([this, lifetime] {
            if (lifetime)
                handoffPauseReconciling = false;
        });
        while (lifetime && !shuttingDown && !(handoffPause && handoffPause->committed()) &&
               (handoffPauseRequested != static_cast<bool>(handoffPause) ||
                (!handoffPauseRequested && handoffInput->active))) {
            if (!handoffPauseRequested) {
                if (const auto pause = handoffPause) {
                    // Terminal callbacks must still see and commit the restoring owner.
                    pause->restore();
                    if (!lifetime || pause->committed())
                        return;
                    handoffPause.reset();
                } else {
                    // A request cancelled inside restoration can leave input preclosed
                    // after its owner retires. Reconcile that state before replay too.
                    restoreApplicationInput(*handoffInput);
                }
                continue;
            }
            std::shared_ptr<QuiescenceSession> pause;
            try {
                pause = std::make_shared<QuiescenceSession>();
                // Publish before callbacks, keeping a local reference through acquisition.
                handoffPause = pause;
                acquireApplicationPause(*pause, *handoffInput);
            } catch (...) {
                if (lifetime && !shuttingDown) {
                    handoffPauseRequested = false;
                    if (pause)
                        pause->restore();
                    else
                        restoreApplicationInput(*handoffInput);
                    if (lifetime && (!pause || !pause->committed()))
                        handoffPause.reset();
                }
                throw;
            }
        }
    }

    void resumeAfterUpdateHandoff() {
        if (shuttingDown || handoffPauseRequested || (updates && updates->handoffPending()))
            return;
        const QPointer<ApplicationController> lifetime(&q);
        reconcileMcpConfiguration();
        if (!lifetime || shuttingDown || handoffPauseRequested ||
            (handoffPause && handoffPause->committed()))
            return;
#ifdef Q_OS_WIN
        const auto intent = shellLaunches.takeUpdate();
        updates->clearRelaunchArguments();
        if (!intent.paths.isEmpty())
            pinShellFiles(intent.paths);
        if (intent.foreground)
            showMainWindow();
#endif
    }

    void reportSelectedFilePinFailure(const QString& message) {
        if (!mainWindow || !mainWindow->isVisible() || mainWindow->isMinimized()) {
            systemTray.showWarningMessage(
                ApplicationController::tr("Could not pin selected files to screen"), message);
            return;
        }
        MainWindow& window = ensureMainWindow();
        window.showAndActivate();
        adqt::widgets::AdMessage::Request request;
        request.key = QStringLiteral("pin-selected-files-error");
        request.content = message;
        adqt::widgets::AdMessageService::warning(std::move(request), &window);
    }

#ifdef Q_OS_WIN
    void pinShellFiles(QStringList paths, std::function<void()> finished = {}) {
        if (!actionGate.dispatch([this, paths = std::move(paths), finished]() mutable {
                if (auto* controller = ensureScreenshotController())
                    controller->pinFilesToScreen(std::move(paths), std::move(finished));
                else if (finished)
                    finished();
            }) &&
            finished)
            finished();
    }

    void preserveShellLaunchForUpdate(const ShellLaunchCoordinator::Request& request) {
        const auto intent = shellLaunches.retainUpdate(request);
        // Transport admission checked the cumulative intent before its receipt. Retain
        // accepted paths until helper acknowledgement or normal replay after failure.
        updates->setRelaunchArguments(
            update::startupUpdateRelaunchArguments(intent.foreground, intent.paths));
    }
#endif

    void applyOcrConfiguration() {
        if (ocrRecognition == nullptr)
            return;
        const auto& configuration = storage::ApplicationStorage::instance().configuration();
        ocrRecognition->setRuntimeConfiguration(
            {screenshotOcrModelTypeFromValue(configuration.value(kOcrModelTypeKey).toString()),
             screenshotOcrDetectorResizePolicyFromValue(
                 configuration.value(kOcrDetectorResizePolicyKey).toString()),
             configuration.value(QStringLiteral("text_recognition/resident_process")).toBool(),
             configuration.value(QStringLiteral("text_recognition/model_hot_start")).toBool(),
             screenshotOcrTextDetectionProcessingFromValue(
                 configuration.value(kOcrTextDetectionProcessingKey).toString()),
             configuration.value(QStringLiteral("text_recognition/hardware_acceleration"))
                 .toBool()});
    }

    void reconcileMcpConfiguration() {
        const bool enabled =
            storage::ApplicationStorage::instance().configuration().value(kMcpEnabledKey).toBool();
        if (!enabled)
            app.setProperty("snowShotMcpStartError", QVariant());
        if (mcpLifecycle)
            mcpLifecycle->setEnabled(enabled);
    }

    bool applicationSuspended() const {
        return (migrationInput && migrationInput->active) || handoffPauseRequested ||
               (handoffInput && handoffInput->active) ||
               (handoffPause && handoffPause->committed());
    }

    bool actionsAvailable() const {
        return !shuttingDown && !applicationSuspended() &&
               !storage::ApplicationStorage::instance().directoryChanging() &&
               (!updates || !updates->handoffPending());
    }

    void reconcileSuspension() {
        const bool suspended = applicationSuspended();
        if (screenshotController)
            screenshotController->setCaptureSuspended(suspended);
        mcpGraph.setSourcesSuspended(suspended);
        if (mcpLifecycle) {
            if (suspended)
                mcpLifecycle->setSuspended(true);
            // Read desired state before reopening: its deferred settings callback may not
            // have run while a synchronous storage completion was waiting for the worker.
            reconcileMcpConfiguration();
            if (!suspended)
                mcpLifecycle->setSuspended(false);
        }
        if (!suspended && !shuttingDown) {
            deferredStartup.resume();
        }
    }

    mcp::McpRuntimeLifecycle::StartOutcome startMcp() {
        app.setProperty("snowShotMcpStartError", QVariant());
        if (shuttingDown || applicationSuspended() ||
            storage::ApplicationStorage::instance().directoryChanging())
            return {};
        auto* controller = ensureScreenshotController();
        ensureDirectCaptureController();
        ensureSettingsRuntime();
        mcp::McpRuntimeGraph::Ports graphPorts;
        graphPorts.screenshots = controller;
        graphPorts.groups = &groupManager;
        auto& sourcePorts = graphPorts.sources;
        sourcePorts.captureAllowed = [this] {
            if (applicationSuspended() || shuttingDown)
                return QStringLiteral("busy");
#ifdef Q_OS_MACOS
            if (!permissions.allow({presentation::AppPermission::ScreenRecording}))
                return QStringLiteral("permission_required");
#endif
            if (screenshotController && screenshotController->captureAcquisitionActive())
                return QStringLiteral("busy");
            return QString();
        };
        sourcePorts.capture = [this](const auto& params, auto done, auto budget) {
            return directCaptureController->mcpCapture(params, std::move(done), std::move(budget));
        };
        sourcePorts.cancelCapture = [this] { directCaptureController->cancelMcpCapture(); };
        sourcePorts.history = &storage::ApplicationStorage::instance().captureHistory();
        sourcePorts.loadPin = [](const QString& id, auto budget) {
            return storage::ApplicationStorage::instance().pinnedWindows().loadRecord(id, budget);
        };
        sourcePorts.livePin =
            [this](const QString& id,
                   auto budget) -> std::optional<mcp::McpSourceResolver::LivePin> {
            const auto* window = groupManager.liveWindow(id);
            if (!window)
                return std::nullopt;
            const auto state = window->automationState();
            const auto size = state.value(QStringLiteral("source_size")).toArray();
            const qint64 width = size.size() == 2 ? size[0].toInt() : 0;
            const qint64 height = size.size() == 2 ? size[1].toInt() : 0;
            const qint64 pixels = width * height;
            if (width <= 0 || height <= 0 || pixels > 64000000) {
                static_cast<void>(budget(-1));
                return std::nullopt;
            }
            if (!budget(pixels * 8 + 64 * 1024 * 1024))
                return std::nullopt;
            return mcp::McpSourceResolver::LivePin{
                window->persistenceSnapshot(), window->recognitionSnapshot(),
                state.value(QStringLiteral("active_tool")).toString()};
        };
        auto& appPorts = graphPorts.application;
        appPorts.storage = &storage::ApplicationStorage::instance();
        appPorts.settings = runtimeSession.get();
        appPorts.permissions = &permissions;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        appPorts.translation = translationService;
#endif
        appPorts.updates = updates;
        appPorts.restartAllowed = [this] { return restartAllowed(); };
        appPorts.action = [this](const QString& action, const QJsonObject& params) {
            if (action == u"show_main")
                showMainWindow();
            else if (action == u"show_history")
                ensureMainWindow().showScreenshotHistory();
            else if (action == u"show_pinned")
                ensureMainWindow().showPinToScreenManagement();
            else if (action == u"show_translation") {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
                ensureMainWindow().openTranslationPage();
#else
                return false;
#endif
            } else if (action == u"show_settings") {
                const auto page = params.value(QStringLiteral("page_id")).toString();
                if (page.isEmpty())
                    ensureMainWindow().showScreenshotSettings();
                else if (settingsRegistry->catalog().page(page))
                    ensureMainWindow().showSettingsLocation(
                        page, params.value(QStringLiteral("section_id")).toString());
                else
                    return false;
            } else if (action == u"restart") {
                return restartCoordinator
                    .restart(
                        [this] { return restartAllowed(); },
                        [] { return storage::ApplicationStorage::instance().flushNow().success; })
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
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        graphPorts.selectedText = [](QObject* lifetime, auto done) -> std::function<void()> {
            auto* capture = new presentation::SelectedTextTranslationController(lifetime);
            QObject::connect(capture, &presentation::SelectedTextTranslationController::textReady,
                             capture, [capture, done = std::move(done)](const QString& text) {
                                 done(text, {});
                                 capture->deleteLater();
                             });
            capture->capture();
            return [capture = QPointer<presentation::SelectedTextTranslationController>(capture)] {
                if (capture) {
                    capture->cancel();
                    capture->deleteLater();
                }
            };
        };
#endif
        auto& documentPorts = graphPorts.documents;
        documentPorts.recognition = ocrRecognition.get();
        documentPorts.qrRecognition = controller->mcpQrRecognition();
        documentPorts.api = apiClient();
        documentPorts.autoFilter = ScreenshotAutoFilterController::detectRegions;
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
        auto& ports = graphPorts.session;
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
            if (!permissions.allow({presentation::AppPermission::ScreenRecording})) {
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
        ports.artifact = [controller](qreal scale) { return controller->mcpExportArtifact(scale); };
        ports.pin = [controller](auto artifact, auto completion) {
            return controller->mcpPinArtifact(std::move(artifact), std::move(completion));
        };
        ports.direct = [this](const QJsonObject& params, auto completion) {
            return directCaptureController->mcpCapture(params, std::move(completion));
        };
        auto outcome = mcpGraph.start(std::move(graphPorts));
        if (!outcome.available) {
            app.setProperty("snowShotMcpStartError", QVariant::fromValue(mcpGraph.startupError()));
            qWarning("Unable to start Snow Shot MCP server: %s", qPrintable(outcome.error));
        }
        return outcome;
    }

    void start(const QStringList& initialArguments, std::function<void()> maintenance) {
        if (started) {
            return;
        }
        started = true;

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
        const QPointer<ApplicationController> guard(&q);
        auto resume = [guard, maintenance = std::move(maintenance)]() mutable {
            if (guard)
                guard->m_impl->finishStartup(std::move(maintenance));
        };
        if (parseShellImageLaunchRequest(initialArguments).requested)
            q.handleLaunchRequest(initialArguments, std::move(resume));
        else
            resume();
    }

    void finishStartup(std::function<void()> maintenance) {
        deferredStartup.start({[this] { updates->start(); }, std::move(maintenance),
                               [this] {
                                   if (auto* controller = ensureScreenshotController())
                                       controller->prewarmResources();
                               },
                               [this] { restorePinnedWindows(); },
                               [this] { applyOcrConfiguration(); },
                               [this] {
                                   if (distribution::isStoreManaged()) {
                                       if (auto* controller = ensureScreenshotController())
                                           controller->offerRecordingRecovery();
                                   }
                               }});
    }

    ScreenshotController* ensureScreenshotController() {
        if (screenshotController == nullptr) {
            screenshotController = std::make_unique<ScreenshotController>(
                &q, &groupManager, ocrRecognition.get(), apiClient());
            screenshotController->setCaptureSuspended(applicationSuspended());
#ifdef Q_OS_MACOS
            screenshotController->setCaptureBackgroundWindow(mainWindow);
#endif
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::recordingExportNotificationRequested,
                             &systemTray,
                             &presentation::SystemTrayController::showRecordingExportMessage);
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::captureActivityChanged, &floatingToolbar,
                             &presentation::FloatingToolbarController::setCaptureActive);
#ifdef Q_OS_MACOS
            screenshotController->setRecordingPermissionCheck([this](bool microphone, bool input,
                                                                     bool notify) {
                presentation::AppPermissions required{presentation::AppPermission::ScreenRecording};
                if (microphone)
                    required.append(presentation::AppPermission::Microphone);
                if (input)
                    required.append(presentation::AppPermission::InputMonitoring);
                return notify ? allowPermissions(required) : permissions.allow(required);
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
                [this](const QString& message) { reportSelectedFilePinFailure(message); });
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::showMainWindowRequested, &q,
                             [this]() { showMainWindow(); });
            QObject::connect(
                screenshotController.get(), &ScreenshotController::imageStitchingRequested, &q,
                [this](const QVector<ScreenshotPinnedImageSnapshot>& snapshots) {
                    static_cast<void>(actionGate.dispatch([this, snapshots] {
                        ensureImageStitchingWindowCoordinator().presentPinnedWindows(snapshots);
                    }));
                });
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::imageSplittingRequested, &q,
                             [this](const presentation::ImageSplitSource& source) {
                                 static_cast<void>(actionGate.dispatch([this, source] {
                                     ensureImageSplitWindowCoordinator().presentSource(source);
                                 }));
                             });
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::scrollingImageEditingRequested, &q,
                             [this](const ScreenshotExportSource& source) {
                                 static_cast<void>(actionGate.dispatch([this, source] {
                                     ensureImageEditingWindowCoordinator().presentSource(source);
                                 }));
                             });
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::pinnedImageEditingRequested, &q,
                             [this](const QString& id) {
                                 static_cast<void>(actionGate.dispatch([this, id] {
                                     ensureImageEditingWindowCoordinator().presentPinnedRecord(id);
                                 }));
                             });
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::translationPageRequested, &q,
                             [this](const QString& text) {
                                 ensureSelectedTextTranslationCoordinator().presentText(text);
                             });
#endif
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::captureAvailabilityChanged, &globalMouseManager,
                             &presentation::GlobalMouseManager::setCaptureAvailable);
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::globalMouseCaptureEnded, &globalMouseManager,
                             &presentation::GlobalMouseManager::cancelGesture);
        }
        return screenshotController.get();
    }

    void applyRuntimeConfiguration(const QJsonValue& value, const QString& key) {
        if (key == u"system/memory_optimization_policy" && memoryOptimization)
            memoryOptimization->synchronizePolicy();
        if (key == QStringLiteral("screen_recording/enable_microphone"))
            permissions.setMicrophoneEnabled(value.toBool());
#if SNOW_SHOT_ENABLE_EXTENDED_FEATURES
        if (key == QStringLiteral("extended_features/translation_page_enabled")) {
            systemTray.setMenuOptions(
                stringList(storage::ApplicationStorage::instance().configuration().value(
                    kTrayMenuOptionsKey)));
        }
#endif
        if (key == u"updates/mode" && updates != nullptr) {
            updates->setMode(value.toString());
        } else if ((key == u"network/proxy" || key == u"network/http_proxy") &&
                   updates != nullptr) {
            const auto& configuration = storage::ApplicationStorage::instance().configuration();
            updates->setProxyConfiguration(
                configuration.value(QStringLiteral("network/proxy")).toString(),
                configuration.value(QStringLiteral("network/http_proxy")).toString());
        } else if (key == kPinBorderColorKey) {
            QColor color = storage::colorFromRgbaString(value.toString());
            if (!color.isValid()) {
                color = QColor(219, 219, 219, 255);
            }
            ScreenshotPinnedWindow::setRuntimeBorderColor(color);
        } else if (key == kPinLockedBorderColorKey) {
            ScreenshotPinnedWindow::setRuntimeLockedBorderColor(
                storage::colorFromRgbaString(value.toString()));
        } else if (key == kPinBorderActiveColorKey) {
            QColor color = storage::colorFromRgbaString(value.toString());
            if (!color.isValid()) {
                color = QColor(105, 177, 255, 255);
            }
            ScreenshotPinnedWindow::setRuntimeBorderActiveColor(color);
        } else if (key == kPinShadowColorKey) {
            ScreenshotPinnedWindow::setRuntimeShadowColor(
                storage::colorFromRgbaString(value.toString()));
        } else if (key == kPinShadowActiveColorKey) {
            ScreenshotPinnedWindow::setRuntimeShadowActiveColor(
                storage::colorFromRgbaString(value.toString()));
        } else if (key == kPinLockedShadowColorKey) {
            ScreenshotPinnedWindow::setRuntimeLockedShadowColor(
                storage::colorFromRgbaString(value.toString()));
        } else if (key == kTrayEnabledKey) {
            const bool enabled = value.isBool() ? value.toBool() : true;
            systemTray.setEnabled(enabled && !applicationSuspended());
            ScreenshotPinnedWindow::setRuntimeTrayEnabled(enabled);
        } else if (key == kTrayUseSystemIconColorKey) {
            systemTray.setUseSystemIconColor(value.toBool(false));
        } else if (key == kTrayIconKey) {
            systemTray.setIconSelection(value.toString(QStringLiteral("default")));
        } else if (key == kTrayCustomIconKey) {
            systemTray.setCustomIconPath(value.toString());
        } else if (key == kTrayDarkIconKey) {
            systemTray.setDarkIconSelection(value.toString(QStringLiteral("inherit")));
        } else if (key == kTrayDarkCustomIconKey) {
            systemTray.setDarkCustomIconPath(value.toString());
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
        } else if (key == kOcrModelTypeKey || key == kOcrDetectorResizePolicyKey ||
                   key == kOcrTextDetectionProcessingKey ||
                   key == QStringLiteral("text_recognition/resident_process") ||
                   key == QStringLiteral("text_recognition/model_hot_start") ||
                   key == QStringLiteral("text_recognition/hardware_acceleration")) {
            if (started)
                applyOcrConfiguration();
        }
    }

    MainWindow& ensureMainWindow() {
        if (mainWindow == nullptr) {
            ensureSettingsRuntime();
            mainWindow = new MainWindow(*settingsRegistry, *runtimeSession, nullptr, apiClient());
#ifdef Q_OS_MACOS
            if (screenshotController)
                screenshotController->setCaptureBackgroundWindow(mainWindow);
            QObject::connect(mainWindow, &MainWindow::aboutToShowAndActivate, &q, [this] {
                if (screenshotController)
                    screenshotController->releaseCaptureBackgroundWindow();
            });
#endif
            QObject::connect(mainWindow, &QObject::destroyed, &q,
                             [this]() { mainWindow = nullptr; });
            QObject::connect(mainWindow, &MainWindow::screenshotRequested, &q, [this]() {
                dispatchQuickAction(presentation::GlobalShortcutAction::Screenshot);
            });
            QObject::connect(
                mainWindow, &MainWindow::quickActionRequested, &q,
                [this](presentation::GlobalShortcutAction action) { dispatchQuickAction(action); });
            QObject::connect(
                mainWindow, &MainWindow::globalMouseDragRequested, &q,
                [this](presentation::settings::SettingsGlobalMouseAction action) {
                    static_cast<void>(actionGate.dispatch(
                        [this, action]() { globalMouseManager.beginButtonDrag(action); },
                        [this, action] {
                            return allowPermissions(presentation::requiredPermissions(
                                action, permissions.microphoneEnabled()));
                        }));
                });
            QObject::connect(mainWindow, &MainWindow::screenshotHistoryEditRequested, &q,
                             [this](const QString& recordId) {
                                 static_cast<void>(actionGate.dispatch([this, recordId]() {
                                     if (ScreenshotController* controller =
                                             ensureScreenshotController()) {
                                         controller->editHistoryRecord(recordId);
                                     }
                                 }));
                             });
            QObject::connect(mainWindow, &MainWindow::screenshotHistoryPinRequested, &q,
                             [this](const QString& recordId) {
                                 static_cast<void>(actionGate.dispatch([this, recordId]() {
                                     if (ScreenshotController* controller =
                                             ensureScreenshotController()) {
                                         controller->pinHistoryRecord(recordId);
                                     }
                                 }));
                             });
            QObject::connect(mainWindow, &MainWindow::screenshotHistoryStitchImagesRequested, &q,
                             [this](const QVector<storage::CaptureHistoryRecord>& records) {
                                 static_cast<void>(actionGate.dispatch([this, records] {
                                     ensureImageStitchingWindowCoordinator().present(records);
                                 }));
                             });
            QObject::connect(mainWindow, &MainWindow::screenshotHistorySplitRequested, &q,
                             [this](const storage::CaptureHistoryRecord& record) {
                                 static_cast<void>(actionGate.dispatch([this, record] {
                                     ensureImageSplitWindowCoordinator().presentHistory(record);
                                 }));
                             });
            QObject::connect(mainWindow, &MainWindow::screenshotHistoryImageEditingRequested, &q,
                             [this](const storage::CaptureHistoryRecord& record) {
                                 static_cast<void>(actionGate.dispatch([this, record] {
                                     ensureImageEditingWindowCoordinator().presentHistory(record);
                                 }));
                             });
            QObject::connect(mainWindow, &MainWindow::pinnedWindowStitchImagesRequested, &q,
                             [this](const QStringList& recordIds) {
                                 static_cast<void>(actionGate.dispatch([this, recordIds] {
                                     ensureImageStitchingWindowCoordinator().presentPinnedRecords(
                                         recordIds);
                                 }));
                             });
            QObject::connect(mainWindow, &MainWindow::pinnedWindowSplitRequested, &q,
                             [this](const QString& recordId) {
                                 static_cast<void>(actionGate.dispatch([this, recordId] {
                                     ensureImageSplitWindowCoordinator().presentPinnedRecord(
                                         recordId);
                                 }));
                             });
            QObject::connect(mainWindow, &MainWindow::pinnedWindowImageEditingRequested, &q,
                             [this](const QString& recordId) {
                                 static_cast<void>(actionGate.dispatch([this, recordId] {
                                     ensureImageEditingWindowCoordinator().presentPinnedRecord(
                                         recordId);
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
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
            settingsBackend->setTranslationService(translationService);
#endif
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

    presentation::WindowGroupSwitcherController& ensureWindowGroupSwitcher() {
        if (!windowGroupSwitcher) {
            windowGroupSwitcher = std::make_unique<presentation::WindowGroupSwitcherController>(
                globalShortcutManager, groupManager);
            QObject::connect(
                windowGroupSwitcher.get(),
                &presentation::WindowGroupSwitcherController::errorOccurred, &q,
                [](const QString& message) { adqt::widgets::AdMessageService::error(message); });
        }
        return *windowGroupSwitcher;
    }

    void dispatchFloatingAction(const QString& id) {
        runtime::RuntimeActivityTracker::shared().markActivity();
        using Quick = presentation::GlobalShortcutAction;
        static const QHash<QString, Quick> quickActions{
            {QStringLiteral("screenshot"), Quick::Screenshot},
            {QStringLiteral("screenshot-delay"), Quick::ScreenshotDelay},
            {QStringLiteral("pin-to-screen"), Quick::ScreenshotPinToScreen},
            {QStringLiteral("text-recognition"), Quick::ScreenshotOcr},
            {QStringLiteral("text-translation"), Quick::ScreenshotTranslation},
            {QStringLiteral("record-screen"), Quick::ScreenRecord},
            {QStringLiteral("screenshot-history"), Quick::OpenCaptureHistory},
            {QStringLiteral("global-canvas"), Quick::GlobalCanvas},
            {QStringLiteral("stitch-selected-images"), Quick::StitchSelectedImages},
            {QStringLiteral("split-selected-images"), Quick::SplitSelectedImages},
            {QStringLiteral("edit-selected-images"), Quick::EditSelectedImages},
        };
        if (const auto action = quickActions.constFind(id); action != quickActions.cend()) {
            dispatchQuickAction(*action);
            return;
        }
        using Action = ScreenshotController::CaptureAction;
        static const QHash<QString, Action> selectionActions{
            {QStringLiteral("scrolling-screenshot"), Action::StartScrolling},
            {QStringLiteral("table-recognition"), Action::RecognizeTable},
            {QStringLiteral("barcode-recognition"), Action::RecognizeQr},
            {QStringLiteral("latex-recognition"), Action::RecognizeFormula},
            {QStringLiteral("convert-to-markdown"), Action::ConvertMarkdown},
            {QStringLiteral("convert-to-html"), Action::ConvertHtml},
            {QStringLiteral("save-as-file"), Action::Save},
        };
        const auto action = selectionActions.constFind(id);
        if (action == selectionActions.cend())
            return;
        static_cast<void>(actionGate.dispatch(
            [this, action = *action] {
                if (auto* controller = ensureScreenshotController())
                    static_cast<void>(controller->captureForAction(action));
            },
            [this] {
                return allowPermissions(
                    presentation::requiredPermissions(Quick::Screenshot, false));
            }));
    }

    void dispatchQuickAction(presentation::GlobalShortcutAction action,
                             QuickActionOrigin origin = QuickActionOrigin::Other) {
        runtime::RuntimeActivityTracker::shared().markActivity();
        static_cast<void>(actionGate.dispatch(
            [this, action, origin] { dispatchAvailableQuickAction(action, origin); },
            [this, action] {
                return allowPermissions(
                    presentation::requiredPermissions(action, permissions.microphoneEnabled()));
            }));
    }

    void dispatchAvailableQuickAction(presentation::GlobalShortcutAction action,
                                      QuickActionOrigin origin) {
        if (origin == QuickActionOrigin::GlobalShortcut && screenshotController &&
            screenshotController->handleActiveScreenshotShortcut(action))
            return;
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
        case presentation::GlobalShortcutAction::ScreenshotPinToScreen:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->captureAndPinSelection();
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotOcr:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->captureAndRecognizeText();
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotOcrCopy:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                static_cast<void>(controller->captureForAction(
                    ScreenshotController::CaptureAction::RecognizeTextAndCopy));
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotTranslation:
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->captureAndTranslateText();
            }
#endif
            break;
        case presentation::GlobalShortcutAction::ScreenshotScrolling:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                static_cast<void>(controller->captureForAction(
                    ScreenshotController::CaptureAction::StartScrolling));
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotCopy:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->captureAndCopySelection();
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotSave:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                static_cast<void>(
                    controller->captureForAction(ScreenshotController::CaptureAction::Save));
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotQuickSave:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                static_cast<void>(
                    controller->captureForAction(ScreenshotController::CaptureAction::QuickSave));
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
        case presentation::GlobalShortcutAction::GlobalCanvas:
            if (!globalCanvasController) {
                globalCanvasController = std::make_unique<presentation::GlobalCanvasController>();
                QObject::connect(globalCanvasController.get(),
                                 &presentation::GlobalCanvasController::activeChanged,
                                 &globalShortcutManager,
                                 &presentation::GlobalShortcutManager::setGlobalCanvasActive);
                QObject::connect(globalCanvasController.get(),
                                 &presentation::GlobalCanvasController::errorOccurred, &q,
                                 [](const QString& message) {
                                     adqt::widgets::AdMessageService::error(message);
                                 });
            }
            globalCanvasController->activate();
            break;
        case presentation::GlobalShortcutAction::SwitchWindowGroup:
            ensureWindowGroupSwitcher().openPicker();
            break;
        case presentation::GlobalShortcutAction::StitchSelectedImages: {
            auto backend = platform::createSelectedFileBackend();
            const auto target = backend->captureTarget();
            ensureImageStitchingWindowCoordinator().presentSelectedFiles(std::move(backend),
                                                                         target);
            break;
        }
        case presentation::GlobalShortcutAction::SplitSelectedImages: {
            auto backend = platform::createSelectedFileBackend();
            const auto target = backend->captureTarget();
            ensureImageSplitWindowCoordinator().presentSelectedFiles(std::move(backend), target);
            break;
        }
        case presentation::GlobalShortcutAction::EditSelectedImages: {
            auto backend = platform::createSelectedFileBackend();
            const auto target = backend->captureTarget();
            ensureImageEditingWindowCoordinator().presentSelectedFiles(std::move(backend), target);
            break;
        }
        case presentation::GlobalShortcutAction::OpenPinToScreenManagement:
            ensureMainWindow().showPinToScreenManagement();
            break;
        case presentation::GlobalShortcutAction::OpenSettings:
            showGeneralSettings();
            break;
        case presentation::GlobalShortcutAction::TranslateSelectedText:
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
            if (storage::ExtendedFeaturesSettings().translationPageEnabled()) {
                ensureSelectedTextTranslationCoordinator().capture();
            }
#endif
            break;
        case presentation::GlobalShortcutAction::ToggleWindowUnderCursorAlwaysOnTop:
#ifdef Q_OS_WIN
            static_cast<void>(platform::windows::toggleWindowUnderCursorAlwaysOnTop(
                &ScreenshotPinnedWindow::toggleAlwaysOnTopForSurface));
#endif
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
        case presentation::GlobalShortcutAction::HideShowAllWindows:
            if (ScreenshotController* controller = ensureScreenshotController())
                controller->togglePinnedWindowsVisibility();
            break;
        case presentation::GlobalShortcutAction::PinClipboardContent:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->pinClipboardContentToScreen();
            }
            break;
        case presentation::GlobalShortcutAction::ToggleGlobalShortcuts:
            globalShortcutManager.setGlobalShortcutsEnabled(
                !globalShortcutManager.globalShortcutsEnabled());
            break;
        case presentation::GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow: {
            storage::GlobalShortcutSettings shortcutSettings;
            shortcutSettings.setDisableOnFocusedFullscreenWindow(
                !shortcutSettings.disableOnFocusedFullscreenWindow());
            break;
        }
        }
    }

    ScreenshotPinnedImageSnapshotRequest pinnedImageSnapshotRequest(
        ScreenshotPinnedRenderMode mode = ScreenshotPinnedRenderMode::WindowAppearance) {
        return [this, mode](const QString& id, auto complete) {
            if (auto* pin = groupManager.liveWindow(id)) {
                pin->requestRenderedImageSnapshot(
                    [complete = std::move(complete)](ScreenshotPinnedImageSnapshot snapshot) {
                        complete(std::move(snapshot));
                    },
                    mode);
            } else {
                complete(std::nullopt);
            }
        };
    }

    presentation::ImageEditingWindowCoordinator& ensureImageEditingWindowCoordinator() {
        if (!imageEditingWindowCoordinator) {
            presentation::ImageEditingServices services;
            services.pins = &groupManager;
            services.pin = [this](QImage image, auto completion) {
                ScreenshotClipboardContent content;
                content.image = std::move(image);
                return ensureScreenshotController()->mcpPinContent(std::move(content),
                                                                   std::move(completion));
            };
            services.providers = [this] {
                return ScreenshotPinnedRecognitionProviders{
                    ocrRecognition.get(), ensureScreenshotController()->mcpQrRecognition(),
                    apiClient()};
            };
            imageEditingWindowCoordinator =
                std::make_unique<presentation::ImageEditingWindowCoordinator>(std::move(services),
                                                                              &q);
            QObject::connect(&app, &QCoreApplication::aboutToQuit,
                             imageEditingWindowCoordinator.get(),
                             &presentation::ImageEditingWindowCoordinator::shutdown);
        }
        return *imageEditingWindowCoordinator;
    }

    presentation::ImageSplitWindowCoordinator& ensureImageSplitWindowCoordinator() {
        if (!imageSplitWindowCoordinator) {
            presentation::ImageSplitWindowServices services;
            services.requestPinnedSnapshot =
                pinnedImageSnapshotRequest(ScreenshotPinnedRenderMode::SourceResolution);
            services.stitchToPdf = [this](presentation::ImageSplitSnapshot snapshot) {
                return ensureImageStitchingWindowCoordinator().presentSplitImagesAsPdf(
                    std::move(snapshot));
            };
            imageSplitWindowCoordinator =
                std::make_unique<presentation::ImageSplitWindowCoordinator>(&q,
                                                                            std::move(services));
            QObject::connect(&app, &QCoreApplication::aboutToQuit,
                             imageSplitWindowCoordinator.get(),
                             &presentation::ImageSplitWindowCoordinator::shutdown);
        }
        return *imageSplitWindowCoordinator;
    }

    presentation::ImageStitchingWindowCoordinator& ensureImageStitchingWindowCoordinator() {
        if (!imageStitchingWindowCoordinator) {
            presentation::ImageStitchingWindowServices services;
            services.requestPinnedSnapshot = pinnedImageSnapshotRequest();
            imageStitchingWindowCoordinator =
                std::make_unique<presentation::ImageStitchingWindowCoordinator>(
                    &q, std::move(services));
            QObject::connect(&app, &QCoreApplication::aboutToQuit,
                             imageStitchingWindowCoordinator.get(),
                             &presentation::ImageStitchingWindowCoordinator::shutdown);
        }
        return *imageStitchingWindowCoordinator;
    }

#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    presentation::SelectedTextTranslationCoordinator& ensureSelectedTextTranslationCoordinator() {
        if (!selectedTextTranslationCoordinator) {
            selectedTextTranslationCoordinator =
                std::make_unique<presentation::SelectedTextTranslationCoordinator>(
                    storage::ApplicationStorage::instance().configuration(), apiClient());
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

#endif
    presentation::DirectCaptureController& ensureDirectCaptureController() {
        if (!directCaptureController) {
            directCaptureController = std::make_unique<presentation::DirectCaptureController>(&q);
            QObject::connect(directCaptureController.get(),
                             &presentation::DirectCaptureController::captureActivityChanged,
                             &floatingToolbar,
                             &presentation::FloatingToolbarController::setCaptureActive);
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
        if (storage::ApplicationStorage::instance().directoryChanging())
            return;
        ensureMainWindow().showAndActivate();
    }

    void restorePinnedWindows() {
        static_cast<void>(actionGate.dispatch([this]() {
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->restorePinnedWindows();
            }
        }));
    }

    void dispatchGlobalMouseEvent(const presentation::GlobalMouseDragEvent& event) {
        runtime::RuntimeActivityTracker::shared().markActivity();
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

    void showGeneralSettings() {
        ensureMainWindow().showGeneralSettings();
    }

    ApplicationController& q;
    QApplication& app;
    ApplicationRestartCoordinator restartCoordinator;
    ApplicationInputGuard* migrationInput = nullptr;
    ApplicationInputGuard* handoffInput = nullptr;
    struct DirectoryPause {
        QString sourceRoot;
        QString restoredRoot;
        QuiescenceSession resources;
    };
    std::unique_ptr<DirectoryPause> migrationPause;
    std::shared_ptr<QuiescenceSession> handoffPause;
    bool handoffPauseRequested = false;
    bool handoffPauseReconciling = false;
    // These services outlive the disposable configuration window.
    presentation::PinnedWindowGroupManager groupManager;
    presentation::SystemTrayController systemTray;
    presentation::FloatingToolbarController floatingToolbar;
    ApplicationActionGate actionGate{[this, lifetime = QPointer<ApplicationController>(&q)] {
        return lifetime && actionsAvailable();
    }};
    presentation::GlobalShortcutManager globalShortcutManager;
    std::unique_ptr<presentation::WindowGroupSwitcherController> windowGroupSwitcher;
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
    SnowShotApiClient* apiClient() const {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        return translationClient.get();
#else
        return nullptr;
#endif
    }
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    std::unique_ptr<SnowShotApiClient> translationClient;
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    translation::TranslationService* translationService = nullptr;
#endif
    std::unique_ptr<ScreenshotOcrRecognitionService> ocrRecognition;
    std::unique_ptr<ScreenshotController> screenshotController;
    std::unique_ptr<presentation::GlobalCanvasController> globalCanvasController;
    // Graph cancellation borrows direct capture even when construction unwinds before Impl's
    // destructor can run its explicit shutdown sequence. Destroy both MCP owners first.
    std::unique_ptr<presentation::DirectCaptureController> directCaptureController;
    mcp::McpRuntimeGraph mcpGraph{&q};
    std::unique_ptr<mcp::McpRuntimeLifecycle> mcpLifecycle;
    std::unique_ptr<presentation::ImageStitchingWindowCoordinator> imageStitchingWindowCoordinator;
    std::unique_ptr<presentation::ImageSplitWindowCoordinator> imageSplitWindowCoordinator;
    std::unique_ptr<presentation::ImageEditingWindowCoordinator> imageEditingWindowCoordinator;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    std::unique_ptr<presentation::SelectedTextTranslationCoordinator>
        selectedTextTranslationCoordinator;
#endif
    QPointer<MainWindow> mainWindow;
#ifdef Q_OS_MACOS
    std::unique_ptr<platform::macos::ApplicationReopenHandler> reopenHandler;
#endif
    bool started = false;
    bool shuttingDown = false;
    DeferredStartup deferredStartup{q, [this] { return actionsAvailable(); }};
    std::unique_ptr<runtime::MemoryOptimizationController> memoryOptimization;
    std::unique_ptr<StoreUpdateLifecycle> storeLifecycle;
    update::UpdateService* updates = nullptr;
#ifdef Q_OS_WIN
    ShellLaunchCoordinator shellLaunches;
#endif
};

ApplicationController::ApplicationController(QApplication& application, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, application)) {}

ApplicationController::~ApplicationController() = default;

void ApplicationController::start(const QStringList& initialArguments,
                                  std::function<void()> maintenance) {
    m_impl->start(initialArguments, std::move(maintenance));
}

void ApplicationController::showMainWindow() {
    m_impl->showMainWindow();
}

bool ApplicationController::acceptsLaunchRequest(const QStringList& arguments) const {
#ifdef Q_OS_WIN
    if (m_impl->updates && m_impl->updates->handoffPending()) {
        return m_impl->shellLaunches.acceptsUpdate(
            ShellLaunchCoordinator::parse(arguments), m_impl->updates->acceptsRelaunchArguments({}),
            [this](const auto& intent) {
                return m_impl->updates->acceptsRelaunchArguments(
                    update::startupUpdateRelaunchArguments(intent.foreground, intent.paths));
            });
    }
#else
    static_cast<void>(arguments);
#endif
    return true;
}

void ApplicationController::handleLaunchRequest(const QStringList& arguments,
                                                std::function<void()> firstFileFinished) {
    ShellLaunchCoordinator::FirstFileCompletion completion(std::move(firstFileFinished));
    if (!acceptsLaunchRequest(arguments)) {
        m_impl->reportSelectedFilePinFailure(QCoreApplication::translate(
            "UpdateErrors", "The selected files exceed the update restart limit"));
        return;
    }
#ifdef Q_OS_WIN
    const auto request = ShellLaunchCoordinator::parse(arguments);
    if (request.filesRequested) {
        if (!request.valid) {
            m_impl->reportSelectedFilePinFailure(tr("The selected file path is invalid"));
            return;
        }
        for (const QString& path : request.unsupportedPaths)
            m_impl->reportSelectedFilePinFailure(
                tr("Could not open the selected file: %1").arg(path));
        if (storage::ApplicationStorage::instance().directoryChanging() ||
            (m_impl->migrationInput && m_impl->migrationInput->active)) {
            m_impl->shellLaunches.retainStorage(request);
        } else if ((m_impl->updates && m_impl->updates->handoffPending()) ||
                   (m_impl->handoffInput && m_impl->handoffInput->active)) {
            m_impl->preserveShellLaunchForUpdate(request);
        } else {
            if (!request.paths.isEmpty())
                m_impl->pinShellFiles(request.paths, completion.take());
            if (request.foreground)
                m_impl->showMainWindow();
        }
        return;
    }
#endif
    if (storage::ApplicationStorage::instance().directoryChanging() ||
        (m_impl->migrationInput && m_impl->migrationInput->active))
        return;
    if (arguments.contains(QStringLiteral("--autostart"))) {
        return;
    }
#ifdef Q_OS_WIN
    if ((m_impl->updates && m_impl->updates->handoffPending()) ||
        (m_impl->handoffInput && m_impl->handoffInput->active)) {
        m_impl->preserveShellLaunchForUpdate(request);
        return;
    }
#endif
    m_impl->showMainWindow();
}

void ApplicationController::restorePinnedWindows() {
    m_impl->restorePinnedWindows();
}
} // namespace snow_shot::app

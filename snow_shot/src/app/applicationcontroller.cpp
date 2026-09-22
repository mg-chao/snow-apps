#include "snow_shot/app/applicationcontroller.h"
#include "snow_shot/app/featureavailability.h"
#include "snow_shot/presentation/apppermissionservice.h"
#ifdef Q_OS_MACOS
#include "snow_shot/platform/macos/applicationactivation.h"
#include "snow_shot/presentation/permissionguidecontroller.h"
#include "snow_shot/presentation/components/updatenotice.h"
#endif
#include "snow_shot/platform/windows/administratorlaunch.h"
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/update/updateservice.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QMessageBox>

#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/globalmousemanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/screenshotcontroller.h"
#include "snow_shot/presentation/directcapturecontroller.h"
#include "snow_shot/presentation/selectedtexttranslationcoordinator.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenrecordingfolder.h"
#include "snow_shot/presentation/systemtraycontroller.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/platform/selectedfiles.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "widgets/message.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QPointer>
#include <QTimer>

#include <memory>

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
        : q(owner), app(application), groupManager(initializedPinnedWindowRepository()),
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
#ifdef Q_OS_MACOS
        QObject::connect(updates, &update::UpdateService::automaticUpdateAvailable, &q,
                         [this](const QString& version) {
                             auto* notice = new presentation::UpdateNotice(version, mainWindow);
                             notice->setAttribute(Qt::WA_DeleteOnClose);
                             notice->show();
                         });
#endif
        QObject::connect(updates, &update::UpdateService::updateReady, &q, [this] {
            systemTray.showUpdateMessage(ApplicationController::tr(
                "An update is ready. Open About to restart and update Snow Shot."));
        });
        platform::windows::setAdministratorRestartGuard([this] {
            return updates->status().state != update::UpdateState::Applying &&
                   !(screenshotController && screenshotController->blocksApplicationUpdate()) &&
                   !(directCaptureController && directCaptureController->blocksApplicationUpdate());
        });
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
#ifdef Q_OS_MACOS
        reopenHandler = std::make_unique<platform::macos::ApplicationReopenHandler>(
            [this]() { showMainWindow(); });
#endif
    }

    ~Impl() {
        platform::windows::setAdministratorRestartGuard({});
        if (mainWindow != nullptr) {
            mainWindow->setAttribute(Qt::WA_DeleteOnClose, false);
            delete mainWindow;
        }
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
            screenshotController->setRecordingPermissionCheck([this](bool microphone, bool input,
                                                                     bool notify) {
#ifdef Q_OS_MACOS
                permissions.refresh();
                presentation::AppPermissions required{presentation::AppPermission::ScreenRecording};
                if (microphone)
                    required.append(presentation::AppPermission::Microphone);
                if (input)
                    required.append(presentation::AppPermission::InputMonitoring);
                return notify ? allowPermissions(required)
                              : permissions.missing(required).isEmpty();
#else
                Q_UNUSED(microphone);
                Q_UNUSED(input);
                Q_UNUSED(notify);
                return true;
#endif
            });
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

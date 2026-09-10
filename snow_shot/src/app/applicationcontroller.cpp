#include "snow_shot/app/applicationcontroller.h"
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/update/updateservice.h"
#include "snow_shot/update/updatetransaction.h"
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
#include "snow_shot/presentation/selectedtexttranslationcontroller.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/systemtraycontroller.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonValue>
#include <QPointer>
#include <QTimer>

#include <memory>

namespace snow_shot::app {
namespace {
const QString kPinBorderColorKey = QStringLiteral("pin_to_screen/border_color");
const QString kTrayEnabledKey = QStringLiteral("tray/enabled");
const QString kTrayIconKey = QStringLiteral("tray/icon");
const QString kTrayCustomIconKey = QStringLiteral("tray/custom_icon");
const QString kTrayLeftClickActionKey = QStringLiteral("tray/left_click_action");
const QString kTrayMiddleClickActionKey = QStringLiteral("tray/middle_click_action");
const QString kTrayMenuOptionsKey = QStringLiteral("tray/menu_options");
const QString kScreenshotDelaySecondsKey = QStringLiteral("screenshot/delay_seconds");
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
          systemTray(presentation::settings::builtInTrayCommandManifest(), &groupManager) {
        QObject::connect(&systemTray, &presentation::SystemTrayController::screenshotRequested, &q,
                         [this]() {
                             if (ScreenshotController* controller = ensureScreenshotController()) {
                                 controller->startCapture();
                             }
                         });
        QObject::connect(&systemTray, &presentation::SystemTrayController::showMainWindowRequested,
                         &q, [this]() { showMainWindow(); });
        QObject::connect(&systemTray,
                         &presentation::SystemTrayController::openFunctionSettingsRequested, &q,
                         [this]() { ensureMainWindow().showFunctionSettings(); });
        QObject::connect(&systemTray, &presentation::SystemTrayController::exitRequested, &q,
                         [this]() {
                             systemTray.hide();
                             QApplication::quit();
                         });
        QObject::connect(
            &systemTray, &presentation::SystemTrayController::quickActionRequested, &q,
            [this](presentation::GlobalShortcutAction action) { dispatchQuickAction(action); });
        QObject::connect(
            &systemTray, &presentation::SystemTrayController::globalHotkeysDisabledChanged, &q,
            [this](bool disabled) { globalShortcutManager.setGlobalHotkeysEnabled(!disabled); });
        QObject::connect(
            &groupManager,
            &presentation::PinnedWindowGroupManager::restoreActiveGroupWindowsRequested, &q,
            [this]() {
                if (ScreenshotController* controller = ensureScreenshotController()) {
                    controller->restoreActivePinnedGroupWindows();
                }
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
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &systemTray,
                         &presentation::SystemTrayController::hide);
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &globalMouseManager,
                         &presentation::GlobalMouseManager::shutdown);
        QObject::connect(
            &globalMouseManager, &presentation::GlobalMouseManager::operationFailed, &q,
            [this](const QString& message) { systemTray.showCaptureMessage(message, true); });
        QObject::connect(&globalMouseManager, &presentation::GlobalMouseManager::dragEvent, &q,
                         [this](const presentation::GlobalMouseDragEvent& event) {
                             auto* controller = ensureScreenshotController();
                             using Kind = presentation::GlobalMouseDragEvent::Kind;
                             switch (event.kind) {
                             case Kind::Begin:
                                 if (!controller->beginGlobalMouseCapture(event.action, event.id,
                                                                          event.position)) {
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
        updateOptions.root = update::installationRoot(QCoreApplication::applicationDirPath());
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
        QObject::connect(updates, &update::UpdateService::updateReady, &q, [this] {
            systemTray.showCaptureMessage(
                ApplicationController::tr(
                    "An update is ready. Open About to restart and update Snow Shot."),
                false);
        });
        QObject::connect(updates, &update::UpdateService::restartRequested, &q, [this] {
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
        QObject::connect(&configuration, &storage::ConfigurationStore::valueChanged, &q,
                         [this](const QString& key, const QJsonValue& value) {
                             applyRuntimeConfiguration(value, key);
                         });
    }

    ~Impl() {
        if (mainWindow != nullptr) {
            mainWindow->setAttribute(Qt::WA_DeleteOnClose, false);
            delete mainWindow;
        }
    }

    void start() {
        if (started) {
            return;
        }
        started = true;
        updates->start();

        systemTray.show();
        globalShortcutManager.initialize();
        globalMouseManager.setCaptureAvailable(ensureScreenshotController()->captureAvailable());
        globalMouseManager.initialize();
        QTimer::singleShot(0, &q, [this]() {
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->prewarmResources();
            }
        });
        QTimer::singleShot(0, &q, [this]() { restorePinnedWindows(); });
    }

    ScreenshotController* ensureScreenshotController() {
        if (screenshotController == nullptr) {
            screenshotController = std::make_unique<ScreenshotController>(
                &q, &groupManager, ocrRecognition.get(), translationClient.get());
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::showMainWindowRequested, &q,
                             [this]() { showMainWindow(); });
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
        } else if (key == kOcrModelTypeKey && ocrRecognition != nullptr) {
            ocrRecognition->setModelType(screenshotOcrModelTypeFromValue(value.toString()));
        } else if (key == kOcrDirectMlKey && ocrRecognition != nullptr) {
            ocrRecognition->setBackendPreference(value.toBool()
                                                     ? ScreenshotOcrBackendPreference::DirectMl
                                                     : ScreenshotOcrBackendPreference::Cpu);
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
                if (ScreenshotController* controller = ensureScreenshotController()) {
                    controller->startCapture();
                }
            });
            QObject::connect(
                mainWindow, &MainWindow::quickActionRequested, &q,
                [this](presentation::GlobalShortcutAction action) { dispatchQuickAction(action); });
            QObject::connect(mainWindow, &MainWindow::globalMouseDragRequested, &globalMouseManager,
                             &presentation::GlobalMouseManager::beginButtonDrag);
            QObject::connect(mainWindow, &MainWindow::screenshotHistoryEditRequested, &q,
                             [this](const QString& recordId) {
                                 if (ScreenshotController* controller =
                                         ensureScreenshotController()) {
                                     controller->editHistoryRecord(recordId);
                                 }
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
                globalShortcutManager);
        }
        if (runtimeSession == nullptr) {
            runtimeSession = std::make_unique<presentation::settings::SettingsRuntimeSession>(
                *settingsRegistry, *settingsBackend);
        }
    }

    void dispatchQuickAction(presentation::GlobalShortcutAction action) {
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
        case presentation::GlobalShortcutAction::OpenCaptureHistory:
            ensureMainWindow().showScreenshotHistory();
            break;
        case presentation::GlobalShortcutAction::OpenSettings:
            showInterfaceSettings();
            break;
        case presentation::GlobalShortcutAction::TranslateSelectedText:
            ensureSelectedTextTranslationController().capture();
            break;
        case presentation::GlobalShortcutAction::PinClipboardContent:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->pinClipboardContentToScreen();
            }
            break;
        }
    }

    presentation::SelectedTextTranslationController& ensureSelectedTextTranslationController() {
        if (!selectedTextTranslationController) {
            selectedTextTranslationController =
                std::make_unique<presentation::SelectedTextTranslationController>();
            QObject::connect(
                selectedTextTranslationController.get(),
                &presentation::SelectedTextTranslationController::textReady, &q,
                [this](const QString& text) { ensureMainWindow().showTranslation(text); });
            QObject::connect(selectedTextTranslationController.get(),
                             &presentation::SelectedTextTranslationController::operationFailed,
                             &systemTray,
                             &presentation::SystemTrayController::showTranslationMessage);
            QObject::connect(&app, &QCoreApplication::aboutToQuit,
                             selectedTextTranslationController.get(),
                             &presentation::SelectedTextTranslationController::shutdown);
        }
        return *selectedTextTranslationController;
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
        if (ScreenshotController* controller = ensureScreenshotController()) {
            controller->restorePinnedWindows();
        }
    }

    void showInterfaceSettings() {
        ensureMainWindow().showInterfaceSettings();
    }

    ApplicationController& q;
    QApplication& app;
    // These services outlive the disposable configuration window.
    presentation::PinnedWindowGroupManager groupManager;
    presentation::SystemTrayController systemTray;
    presentation::GlobalShortcutManager globalShortcutManager;
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
    std::unique_ptr<presentation::SelectedTextTranslationController>
        selectedTextTranslationController;
    QPointer<MainWindow> mainWindow;
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

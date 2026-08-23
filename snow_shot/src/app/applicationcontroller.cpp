#include "snow_shot/app/applicationcontroller.h"

#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/screenshotcontroller.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/systemtraycontroller.h"
#include "snow_shot/presentation/settings/settingsruntimebindings.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "../presentation/services/screenshotlifecycleperfinstrumentation.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonValue>
#include <QPointer>
#include <QTimer>

#include <memory>

namespace snow_shot::app {
namespace {
constexpr int kIdlePrewarmDelayMs = 1500;
constexpr int kFirstFramePrewarmFallbackMs = 2500;
const QString kPinBorderColorKey = QStringLiteral("pin_to_screen/border_color");
const QString kTrayEnabledKey = QStringLiteral("tray/enabled");
const QString kTrayIconKey = QStringLiteral("tray/icon");
const QString kTrayCustomIconKey = QStringLiteral("tray/custom_icon");
const QString kTrayLeftClickActionKey = QStringLiteral("tray/left_click_action");
const QString kTrayMenuOptionsKey = QStringLiteral("tray/menu_options");
const QString kScreenshotDelaySecondsKey = QStringLiteral("screenshot/delay_seconds");
const QString kE2eAllowOverlayCaptureArgument =
    QStringLiteral("--e2e-allow-overlay-capture");
const QString kE2eImmediateCaptureCancelArgument =
    QStringLiteral("--e2e-immediate-capture-cancel");
#if defined(SNOW_SHOT_SCREENSHOT_LIFECYCLE_PERF_INSTRUMENTATION)
const QString kE2eStartScreenshotArgument = QStringLiteral("--e2e-start-screenshot");
#endif
#if defined(SNOW_SHOT_SCREENSHOT_MEMORY_FOOTPRINT_INSTRUMENTATION)
const QString kE2eOpenTrayMenuArgument = QStringLiteral("--e2e-open-tray-menu");
const QString kE2eOpenMainInterfaceArgument =
    QStringLiteral("--e2e-open-main-interface");
const QString kE2eOpenScreenshotHistoryArgument =
    QStringLiteral("--e2e-open-screenshot-history");
const QString kE2eOpenInterfaceSettingsArgument =
    QStringLiteral("--e2e-open-interface-settings");
const QString kE2eStartFixedScreenshotArgument =
    QStringLiteral("--e2e-start-fixed-screenshot");
const QString kE2eStartScreenRecordingArgument =
    QStringLiteral("--e2e-start-screen-recording");
#endif

QStringList stringList(const QJsonValue& value) {
    QStringList result;
    for (const QJsonValue& item : value.toArray()) {
        result.push_back(item.toString());
    }
    return result;
}
} // namespace

class ApplicationController::Impl {
  public:
    Impl(ApplicationController& owner, QApplication& application) : q(owner), app(application) {
        QObject::connect(&systemTray, &presentation::SystemTrayController::screenshotRequested, &q,
                         [this]() {
                             if (ScreenshotController* controller = ensureScreenshotController()) {
                                 controller->startCapture();
                             }
                         });
        QObject::connect(&systemTray,
                         &presentation::SystemTrayController::showApplicationInterfaceRequested,
                         &q, [this]() { showMainWindow(); });
        QObject::connect(&systemTray, &presentation::SystemTrayController::exitRequested, &q,
                         [this]() {
                             systemTray.hide();
                             QApplication::quit();
                         });
        QObject::connect(&systemTray,
                         &presentation::SystemTrayController::quickActionRequested, &q,
                         [this](presentation::GlobalShortcutAction action) {
                             dispatchQuickAction(action);
                         });
        QObject::connect(
            &systemTray,
            &presentation::SystemTrayController::shortcutFunctionsDisabledChanged, &q,
            [this](bool disabled) {
                globalShortcutManager.setShortcutFunctionsEnabled(!disabled);
            });
        QObject::connect(&globalShortcutManager, &presentation::GlobalShortcutManager::activated,
                         &q, [this](presentation::GlobalShortcutAction action) {
                             dispatchQuickAction(action);
                         });
        QObject::connect(&globalShortcutManager, &presentation::GlobalShortcutManager::stateChanged,
                         &q,
                         [this](presentation::GlobalShortcutAction action,
                                const presentation::GlobalShortcutRegistrationState& state) {
                             systemTray.setGlobalShortcuts(action, state.shortcuts);
                         });
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &systemTray,
                         &presentation::SystemTrayController::hide);
        prewarmTimer.setSingleShot(true);
        QObject::connect(&prewarmTimer, &QTimer::timeout, &q,
                         [this]() { prewarmScreenshotResources(); });
        auto& applicationStorage = storage::ApplicationStorage::instance();
        if (!applicationStorage.isInitialized()) {
            static_cast<void>(applicationStorage.initialize());
        }
        runtimeBindings = std::make_unique<presentation::settings::BuiltInSettingsRuntimeBindings>(
            globalShortcutManager);
        auto& configuration = applicationStorage.configuration();
        applyRuntimeConfiguration(configuration.value(kPinBorderColorKey), kPinBorderColorKey);
        applyRuntimeConfiguration(configuration.value(kTrayEnabledKey), kTrayEnabledKey);
        applyRuntimeConfiguration(configuration.value(kTrayIconKey), kTrayIconKey);
        applyRuntimeConfiguration(configuration.value(kTrayCustomIconKey), kTrayCustomIconKey);
        applyRuntimeConfiguration(configuration.value(kTrayLeftClickActionKey),
                                   kTrayLeftClickActionKey);
        applyRuntimeConfiguration(configuration.value(kTrayMenuOptionsKey), kTrayMenuOptionsKey);
        applyRuntimeConfiguration(configuration.value(kScreenshotDelaySecondsKey),
                                   kScreenshotDelaySecondsKey);
        QObject::connect(&configuration, &storage::ConfigurationStore::valueChanged, &q,
                         [this](const QString& key, const QJsonValue& value) {
                             applyRuntimeConfiguration(value, key);
                         });
    }

    void start() {
        if (started) {
            return;
        }
        started = true;

        systemTray.show();
        globalShortcutManager.initialize();
        prewarmTimer.start(kIdlePrewarmDelayMs);
    }

    void prewarmScreenshotResources() {
        if (screenshotResourcesPrewarmStarted) {
            return;
        }
        screenshotResourcesPrewarmStarted = true;
        prewarmTimer.stop();
        if (ScreenshotController* controller = ensureScreenshotController()) {
            controller->prewarmResources();
        }
    }

    ScreenshotController* ensureScreenshotController() {
        if (screenshotController == nullptr) {
            screenshotController = std::make_unique<ScreenshotController>();
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::showApplicationInterfaceRequested, &q,
                             [this]() { showMainWindow(); });
        }
        return screenshotController.get();
    }

    void applyRuntimeConfiguration(const QJsonValue& value, const QString& key) {
        if (key == kPinBorderColorKey) {
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
        } else if (key == kTrayMenuOptionsKey) {
            systemTray.setMenuOptions(stringList(value));
        } else if (key == kScreenshotDelaySecondsKey) {
            systemTray.setScreenshotDelaySeconds(value.toInt(3));
        }
    }

    MainWindow& ensureMainWindow() {
        if (mainWindow == nullptr) {
            prewarmTimer.stop();
            mainWindow = std::make_unique<MainWindow>(*runtimeBindings);
            QObject::connect(
                mainWindow.get(), &MainWindow::closed, &q,
                [this, closedWindow = QPointer<MainWindow>(mainWindow.get())]() {
                    // A widget must not delete itself from closeEvent(). Defer the ownership
                    // release until the event returns, and retain the identity check so a
                    // close/reopen race cannot delete a newly-created window.
                    QTimer::singleShot(0, &q, [this, closedWindow]() {
                        if (closedWindow.isNull() || mainWindow.get() != closedWindow.data() ||
                            closedWindow->isVisible()) {
                            return;
                        }
                        mainWindow.reset();
                    });
                });
            QObject::connect(mainWindow.get(), &MainWindow::quickActionRequested, &q,
                             [this](presentation::GlobalShortcutAction action) {
                                 dispatchQuickAction(action);
                             });
            QObject::connect(mainWindow.get(), &MainWindow::screenshotRequested, &q,
                             [this]() {
                                 if (ScreenshotController* controller = ensureScreenshotController()) {
                                     controller->startCapture();
                                 }
                             });
            QObject::connect(mainWindow.get(), &MainWindow::screenshotHistoryEditRequested, &q,
                             [this](const QString& recordId) {
                                 if (ScreenshotController* controller = ensureScreenshotController()) {
                                     controller->editHistoryRecord(recordId);
                                 }
                             });
            if (!screenshotResourcesPrewarmStarted) {
                prewarmTimer.start(kFirstFramePrewarmFallbackMs);
            }
        }
        return *mainWindow;
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
                controller->startDelayedCapture(
                    storage::ScreenshotSettings().delaySeconds());
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
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->captureCurrentMonitor();
            }
            break;
        case presentation::GlobalShortcutAction::ScreenshotFocusedWindow:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->captureFocusedWindow();
            }
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
        case presentation::GlobalShortcutAction::PinClipboardContent:
            if (ScreenshotController* controller = ensureScreenshotController()) {
                controller->pinClipboardContentToScreen();
            }
            break;
        }
    }

    void showMainWindow() {
        ensureMainWindow().showAndActivate();
    }

    void showInterfaceSettings() {
        ensureMainWindow().showInterfaceSettings();
    }

    ApplicationController& q;
    QApplication& app;
    presentation::SystemTrayController systemTray;
    presentation::GlobalShortcutManager globalShortcutManager;
    std::unique_ptr<presentation::settings::SettingsRuntimeBindings> runtimeBindings;
    std::unique_ptr<ScreenshotController> screenshotController;
    std::unique_ptr<MainWindow> mainWindow;
    QTimer prewarmTimer;
    bool started = false;
    bool screenshotResourcesPrewarmStarted = false;
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
    if (QApplication::arguments().contains(kE2eAllowOverlayCaptureArgument) &&
        arguments.contains(kE2eImmediateCaptureCancelArgument)) {
        if (ScreenshotController* controller = m_impl->ensureScreenshotController()) {
            controller->startCapture();
            QTimer::singleShot(0, controller, &ScreenshotController::cancelCapture);
        }
        return;
    }
#if defined(SNOW_SHOT_SCREENSHOT_LIFECYCLE_PERF_INSTRUMENTATION)
    if (QApplication::arguments().contains(kE2eAllowOverlayCaptureArgument) &&
        arguments.contains(kE2eStartScreenshotArgument)) {
        if (ScreenshotController* controller = m_impl->ensureScreenshotController()) {
            presentation::screenshot_lifecycle_perf::beginCapture();
            controller->startCapture();
        }
        return;
    }
#endif
#if defined(SNOW_SHOT_SCREENSHOT_MEMORY_FOOTPRINT_INSTRUMENTATION)
    if (QApplication::arguments().contains(kE2eAllowOverlayCaptureArgument) &&
        arguments.contains(kE2eOpenTrayMenuArgument)) {
        m_impl->systemTray.showMemoryFootprintTestMenu();
        return;
    }
    if (QApplication::arguments().contains(kE2eAllowOverlayCaptureArgument) &&
        arguments.contains(kE2eOpenMainInterfaceArgument)) {
        m_impl->showMainWindow();
        return;
    }
    if (QApplication::arguments().contains(kE2eAllowOverlayCaptureArgument) &&
        arguments.contains(kE2eOpenScreenshotHistoryArgument)) {
        m_impl->ensureMainWindow().showScreenshotHistory();
        return;
    }
    if (QApplication::arguments().contains(kE2eAllowOverlayCaptureArgument) &&
        arguments.contains(kE2eOpenInterfaceSettingsArgument)) {
        m_impl->showInterfaceSettings();
        return;
    }
    if (QApplication::arguments().contains(kE2eAllowOverlayCaptureArgument) &&
        arguments.contains(kE2eStartFixedScreenshotArgument)) {
        if (ScreenshotController* controller = m_impl->ensureScreenshotController()) {
            presentation::screenshot_lifecycle_perf::beginCapture();
            controller->captureAndPinSelection();
        }
        return;
    }
    if (QApplication::arguments().contains(kE2eAllowOverlayCaptureArgument) &&
        arguments.contains(kE2eStartScreenRecordingArgument)) {
        if (ScreenshotController* controller = m_impl->ensureScreenshotController()) {
            presentation::screenshot_lifecycle_perf::beginCapture();
            controller->captureAndStartScreenRecording();
        }
        return;
    }
#endif
    m_impl->showMainWindow();
}
} // namespace snow_shot::app

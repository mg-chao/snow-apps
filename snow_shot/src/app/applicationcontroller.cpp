#include "snow_shot/app/applicationcontroller.h"

#include "idlememoryreclaimpolicy.h"

#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/screenshotasyncactivitytracker.h"
#include "snow_shot/presentation/screenshotcontroller.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"
#include "snow_shot/presentation/systemtraycontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "../presentation/services/screenshotlifecycleperfinstrumentation.h"

#include <QApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonValue>
#include <QPixmapCache>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "icon_registry.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <memory>

namespace snow_shot::app {
namespace {
constexpr int kIdlePrewarmDelayMs = 1500;
constexpr int kFirstFramePrewarmFallbackMs = 2500;
constexpr int kIdleMemoryReclaimDelayMs = 250;
constexpr int kMaximumWorkingSetTrimAttempts = 3;
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
                             beginForegroundOperation();
                             if (ScreenshotController* controller = ensureScreenshotController()) {
                                 controller->startCapture();
                             }
                         });
        QObject::connect(&systemTray, &presentation::SystemTrayController::showMainWindowRequested,
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
        QObject::connect(&systemTray, &presentation::SystemTrayController::transientUiHidden, &q,
                         [this]() { scheduleIdleMemoryReclaim(); });
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
        idleMemoryReclaimTimer.setSingleShot(true);
        QObject::connect(&idleMemoryReclaimTimer, &QTimer::timeout, &q,
                         [this]() { reclaimIdleMemory(); });
        auto& applicationStorage = storage::ApplicationStorage::instance();
        if (!applicationStorage.isInitialized()) {
            static_cast<void>(applicationStorage.initialize());
        }
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

    ~Impl() {
        app.removeEventFilter(&q);
    }

    void start() {
        if (started) {
            return;
        }
        started = true;

        app.installEventFilter(&q);
        ScreenshotAsyncActivityTracker::shared().observeIdle(
            &q, [this]() { scheduleIdleMemoryReclaim(); });
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
                             &ScreenshotController::showMainWindowRequested, &q,
                             [this]() { showMainWindow(); });
            QObject::connect(screenshotController.get(),
                             &ScreenshotController::idleResourcesReleased, &q,
                             [this](bool trimWorkingSet) {
                                 scheduleIdleMemoryReclaim(trimWorkingSet);
                             });
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
            ScreenshotController* controller = ensureScreenshotController();
            Q_ASSERT(controller != nullptr);
            mainWindow = std::make_unique<MainWindow>(*controller, globalShortcutManager);
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
                        scheduleIdleMemoryReclaim();
                    });
                });
            QObject::connect(mainWindow.get(), &MainWindow::firstFramePresented, &q,
                             [this]() { prewarmScreenshotResources(); });
            QObject::connect(mainWindow.get(), &MainWindow::quickActionRequested, &q,
                             [this](presentation::GlobalShortcutAction action) {
                                 dispatchQuickAction(action);
                             });
            if (!screenshotResourcesPrewarmStarted) {
                prewarmTimer.start(kFirstFramePrewarmFallbackMs);
            }
        }
        return *mainWindow;
    }

    void beginForegroundOperation() {
        // The command establishes its active work or visible surface synchronously. Re-evaluate
        // on the next turn as well so a rejected/no-op command cannot strand a prior request.
        idleMemoryReclaimTimer.stop();
        postIdleMemoryReclaimReevaluation();
    }

    void dispatchQuickAction(presentation::GlobalShortcutAction action) {
        beginForegroundOperation();
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
        beginForegroundOperation();
        ensureMainWindow().showAndActivate();
    }

    void showInterfaceSettings() {
        beginForegroundOperation();
        ensureMainWindow().showInterfaceSettings();
    }

    void scheduleIdleMemoryReclaim(bool trimWorkingSet = false) {
        if (trimWorkingSet) {
            workingSetTrimAttemptCount = 0;
        }
        idleMemoryReclaimPolicy.request(trimWorkingSet);
        reevaluateIdleMemoryReclaim();
    }

    void reevaluateIdleMemoryReclaim(int delayMs = kIdleMemoryReclaimDelayMs) {
        if (!idleMemoryReclaimPolicy.shouldArmTimer(hasActiveScreenshotWork(),
                                                    hasVisibleApplicationWindow())) {
            idleMemoryReclaimTimer.stop();
            return;
        }
        idleMemoryReclaimTimer.start(delayMs);
    }

    void postIdleMemoryReclaimReevaluation() {
        if (!idleMemoryReclaimPolicy.hasPendingRequest() || idleMemoryReevaluationPosted) {
            return;
        }
        idleMemoryReevaluationPosted = true;
        QTimer::singleShot(0, &q, [this]() {
            idleMemoryReevaluationPosted = false;
            reevaluateIdleMemoryReclaim();
        });
    }

    void observeApplicationEvent(QObject* watched, QEvent* event) {
        if (!idleMemoryReclaimPolicy.hasPendingRequest() || event == nullptr) {
            return;
        }
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget == nullptr || !widget->isWindow()) {
            return;
        }
        switch (event->type()) {
        case QEvent::Show:
            idleMemoryReclaimTimer.stop();
            break;
        case QEvent::Hide:
        case QEvent::Close:
        case QEvent::DeferredDelete:
            // Event filters run before close acceptance and visibility settle. The queued rescan
            // handles rejected closes and destruction without polling while a window stays open.
            postIdleMemoryReclaimReevaluation();
            break;
        default:
            break;
        }
    }

    [[nodiscard]] bool hasActiveScreenshotWork() const {
        return (screenshotController != nullptr && screenshotController->hasActiveWork()) ||
               ScreenshotAsyncActivityTracker::shared().activeActivityCount() > 0;
    }

    [[nodiscard]] bool hasVisibleApplicationWindow() const {
        if (mainWindow != nullptr && mainWindow->isVisible()) {
            return true;
        }
        const auto visibleWindow = [](QWidget* window) {
            return window != nullptr && window->isWindow() && window->isVisible();
        };
        const QWidgetList topLevelWidgets = QApplication::topLevelWidgets();
        return std::any_of(topLevelWidgets.cbegin(), topLevelWidgets.cend(), visibleWindow);
    }

    void reclaimIdleMemory() {
        const IdleMemoryReclaimDecision decision = idleMemoryReclaimPolicy.takeIfIdle(
            hasActiveScreenshotWork(), hasVisibleApplicationWindow());
        if (!decision.shouldReclaim) {
            return;
        }

        // At this point every application-owned surface is closed and deferred teardown has
        // completed. Drop the shared raster caches before asking Windows to evict freed heap and
        // backing-store pages from the private working set. EmptyWorkingSet changes residency,
        // not object reachability, so it complements rather than substitutes deterministic
        // ownership release above.
        adqt::icons::trimIconCache(0);
        QPixmapCache::clear();
        ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (decision.trimWorkingSet && EmptyWorkingSet(GetCurrentProcess()) == FALSE) {
            const int nativeErrorCode = static_cast<int>(GetLastError());
            ++workingSetTrimAttemptCount;
            if (workingSetTrimAttemptCount < kMaximumWorkingSetTrimAttempts) {
                idleMemoryReclaimPolicy.request(true);
                const int retryDelay =
                    kIdleMemoryReclaimDelayMs * (1 << workingSetTrimAttemptCount);
                reevaluateIdleMemoryReclaim(retryDelay);
            } else {
                qWarning("Failed to trim the idle process working set after %d attempts",
                         workingSetTrimAttemptCount);
                presentation::screenshot_lifecycle_perf::idleMemoryReclaimCompleted(
                    true, false, workingSetTrimAttemptCount, nativeErrorCode);
                workingSetTrimAttemptCount = 0;
            }
        } else if (decision.trimWorkingSet) {
            presentation::screenshot_lifecycle_perf::idleMemoryReclaimCompleted(
                true, true, workingSetTrimAttemptCount + 1);
            workingSetTrimAttemptCount = 0;
        } else {
            presentation::screenshot_lifecycle_perf::idleMemoryReclaimCompleted(false, true, 0);
        }
#else
        presentation::screenshot_lifecycle_perf::idleMemoryReclaimCompleted(
            false, !decision.trimWorkingSet, 0);
#endif
    }

    ApplicationController& q;
    QApplication& app;
    presentation::SystemTrayController systemTray;
    presentation::GlobalShortcutManager globalShortcutManager;
    std::unique_ptr<ScreenshotController> screenshotController;
    std::unique_ptr<MainWindow> mainWindow;
    QTimer prewarmTimer;
    QTimer idleMemoryReclaimTimer;
    IdleMemoryReclaimPolicy idleMemoryReclaimPolicy;
    int workingSetTrimAttemptCount = 0;
    bool started = false;
    bool screenshotResourcesPrewarmStarted = false;
    bool idleMemoryReevaluationPosted = false;
};

ApplicationController::ApplicationController(QApplication& application, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, application)) {}

ApplicationController::~ApplicationController() = default;

bool ApplicationController::eventFilter(QObject* watched, QEvent* event) {
    if (m_impl != nullptr) {
        m_impl->observeApplicationEvent(watched, event);
    }
    return QObject::eventFilter(watched, event);
}

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
        m_impl->beginForegroundOperation();
        if (ScreenshotController* controller = m_impl->ensureScreenshotController()) {
            controller->startCapture();
            QTimer::singleShot(0, controller, &ScreenshotController::cancelCapture);
        }
        return;
    }
#if defined(SNOW_SHOT_SCREENSHOT_LIFECYCLE_PERF_INSTRUMENTATION)
    if (QApplication::arguments().contains(kE2eAllowOverlayCaptureArgument) &&
        arguments.contains(kE2eStartScreenshotArgument)) {
        m_impl->beginForegroundOperation();
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
        m_impl->beginForegroundOperation();
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
        m_impl->beginForegroundOperation();
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
        m_impl->beginForegroundOperation();
        if (ScreenshotController* controller = m_impl->ensureScreenshotController()) {
            presentation::screenshot_lifecycle_perf::beginCapture();
            controller->captureAndPinSelection();
        }
        return;
    }
#endif
    m_impl->showMainWindow();
}
} // namespace snow_shot::app

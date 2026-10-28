#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "eraser_toolbar_test_support.h"
#include "window_close_shortcut_test_support.h"
#include "../src/presentation/recording/recordingrenderjob.h"
#include "../src/presentation/recording/recordingrecovery.h"
#include "snow_shot/app/distribution.h"
#include "snow_shot/app/mcp/recordingoptioncontracts.h"
#include <QFontDatabase>
#include <QFontMetrics>
#include "recording_effect_test_source.h"
#include "../src/presentation/recording/recordingwebcamcontrol.h"
#include "../src/presentation/recording/recordingeffectstyle.h"
#include "../src/presentation/recording/recordingeffectgeometry.h"
#ifdef SNOW_RECORDING_EFFECTS_BENCHMARK
#include "recording_effects_performance_benchmark.h"
#endif
#ifdef SNOW_RECORDING_WINDOW_STARTUP_BENCHMARK
#include "screen_recording_window_startup_performance_benchmark.h"
#endif
#include "snow_shot/presentation/canvasstatusreadout.h"
#include <QDialog>
#include <QSlider>
#include <QToolButton>
#include <QFileDialog>
#include "../src/presentation/recording/recordingtrimtoolbar.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include <QAbstractButton>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QLineF>
#include <QTranslator>
#include "snow_shot/presentation/screenrecordingcontroller.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include "physical_key_test_support.h"
#include "snow_capture.h"
#include "snow_shot/platform/windowcaptureexclusion.h"
#include "snow_shot/platform/screenshotwindownames.h"
#include "snow_recording.h"
#include "widgets/button.h"
#include "antd_icons.h"
#include "../src/presentation/recording/recordingaudiogainpopover.h"
#include "widgets/popover.h"
#include "widgets/popup_surface_guard.h"
#include "widgets/dpi_stable_window_controller.h"
#include "widgets/message.h"

#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QClipboard>
#include <QMimeData>
#include <QtMath>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QProcess>
#include <QProcessEnvironment>
#ifdef Q_OS_WIN
#include "toolbar_capture_exclusion_native_tests.h"
#include "../src/platform/windows/captureexclusioncomposition.h"
#endif
#include <QElapsedTimer>
#include <QThread>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWindow>
#include <QScreen>
#include <QPointer>
#include <QScopeGuard>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QLabel>
#include <QLayout>
#include "widgets/color_picker.h"
#include "widgets/form.h"
#include "widgets/modal.h"
#include "widgets/progress.h"
#include "widgets/select.h"
#include "widgets/slider.h"
#include "widgets/switch.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsformfield.h"
#include <future>
#ifdef Q_OS_MACOS
#include "macos_capture_exclusion_probe.h"
#include "macos_recording_modal_probe.h"
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
int recordingToolbarAcrossNativeDisplays();
#pragma push_macro("snow_recording_last_error_message")
#undef snow_recording_last_error_message
extern "C" const char* snow_recording_last_error_message();
const char* nativeCaptureError() {
    return snow_recording_last_error_message();
}
#pragma pop_macro("snow_recording_last_error_message")
#endif
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>

void recordingPopupExclusionTests();
void runScreenRecordingToolbarAutoHideTests();
void renderScreenRecordingToolbarAutoHidePreviews();
void setRecordingToolbarAutoHideTestCursor(ScreenRecordingToolbarWindow& toolbar,
                                           const QPoint& position);

struct SnowRecordingClipImpl {
    quint64 revision = 0;
    quint64 position = 0;
    bool playing = false;
    uint32_t speedPercent = 100;
};
struct SnowRecordingClipFrameImpl {
    QByteArray rgba{32 * 24 * 4, '\x7f'};
};
struct SnowRecordingClipExportImpl {};
struct SnowRecordingSessionImpl {
    QString stagingFile;
    QString temporaryDirectory;
    QString outputFile;
    std::promise<void>* destroyCompleted = nullptr;
    bool stopRequested = false;
};
struct SnowRecordingAudioMonitorImpl {
    uint32_t source = 0;
    int gain = 0;
};
struct SnowRecordingSourceImpl {};
struct SnowRecordingRenderTaskImpl {};
struct SnowWebcam {
    uint32_t size = 150;
    uint32_t fps = 30;
    bool active = false;
};
namespace {
std::atomic<int> webcamCreates = 0, webcamConfigures = 0, webcamDestroys = 0;
bool failWebcamCreation = false;
QByteArray webcamError;
std::vector<std::weak_ptr<RecordingEffectTestState>> effectSources;
std::unique_ptr<RecordingEffectsSource> testEffectsSource() {
    auto state = std::make_shared<RecordingEffectTestState>();
    effectSources.push_back(state);
    return std::make_unique<RecordingEffectTestSource>(std::move(state));
}

SnowRecordingSession* lastCreatedSession = nullptr;
std::atomic<int> clipOpens = 0, clipDestroys = 0, clipExports = 0;
std::atomic<uint32_t> clipExportState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
std::atomic<int> clipAutoplays = 0;
std::atomic<int> clipSpeedChanges = 0;
std::atomic<uint32_t> clipPreviewSpeed = 100, clipExportSpeed = 100;
bool failClipSpeed = false;
bool clipPreviewEnabled = false, failClipSeek = false;
int clipPreviewReads = 0, clipSeekCalls = 0;
QByteArray clipPreviewError;
QByteArray clipExportError;
QVector<quint64> clipBoundaries;
std::function<void(SnowRecordingClipPreview&)> clipPreviewOverride;
std::atomic<bool> clipDeferred = false;
QString clipPath;
quint64 clipFirst = 0, clipEnd = 0;
std::atomic<int> starts = 0;
std::atomic<int> audioMonitorCreates = 0;
std::atomic<int> audioMonitorDestroys = 0;
std::atomic<int> audioMonitorsActive = 0;
std::atomic<bool> holdAudioMonitorDestroy = false;
std::atomic<uint32_t> audioMeterMask = 0;
std::atomic<int> liveSystemGain = 0;
std::atomic<int> liveMicrophoneGain = 0;
std::atomic<int> audioLevelReads = 0;
std::atomic<bool> holdDimensions = false;
std::atomic<bool> dimensionsEntered = false;
std::atomic<int> dimensionsCompleted = 0;
std::shared_future<void> dimensionsGate;
std::atomic<uint32_t> dimensionsScale = 1;
SnowCaptureDirectRecordingConfig lastDirectConfig{};
QByteArray lastKeyboardFontFamily;
QByteArray lastKeyboardCjkFontFamily;
std::vector<uint32_t> lastExcludedWindows;
std::atomic<int> deferredCreates = 0;
std::atomic<bool> recordingStopRequested = false;
SnowRecordingDeferredOptions lastDeferredOptions{};
std::atomic<int> renderStarts = 0;
std::atomic<int> sourceDestroys = 0;
std::atomic<int> sourceDiscards = 0;
std::atomic<int> taskDestroys = 0;
std::atomic<int> activeRenderTasks = 0;
std::atomic<bool> failSourceDiscard = false;
std::atomic<bool> failRenderPoll = false;
std::atomic<int> renderPolls = 0;
std::shared_future<void> renderStartGate;
std::atomic<bool> renderStartEntered = false;
std::shared_future<void> renderCancelGate;
std::atomic<bool> renderCancelEntered = false;
std::atomic<uint32_t> renderState = SNOW_RECORDING_RENDER_STATE_RUNNING;
std::atomic<float> renderPercent = 0;
QByteArray renderSourcePath("D:/recordings/test-source");
std::atomic<int> exports = 0;
std::shared_future<void> exportGate;
std::promise<void>* exportEntered = nullptr;
std::atomic<bool> failExport = false;
std::atomic<bool> failStart = false;
std::atomic<bool> failStartOperation = false;
std::atomic<int> destroyedSessions = 0;
std::promise<void>* nextSessionDestroyCompleted = nullptr;
std::shared_future<void> sessionDestroyGate;
std::atomic<bool> sessionDestroyEntered = false;
std::shared_future<void> sessionCreateGate;
std::atomic<bool> sessionCreateEntered = false;
bool recordingFileFixture = false;
int windowNameLookups = 0;
QPointF windowNameLookupPoint;
snow_shot::storage::ScreenshotWindowNameFields windowNameLookupFields;
std::vector<std::uintptr_t> windowNameLookupExclusions;
snow_shot::storage::ScreenshotWindowNames desktopWindowNames;
QString recordingOutputPath;
std::shared_future<void> clipDestroyGate;
std::atomic<bool> clipDestroyEntered = false;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void requireMemoryAdmissionBlocked(const char* message) {
    auto& activity = snow_shot::runtime::RuntimeActivityTracker::shared();
    const auto snapshot = activity.snapshot();
    bool entered = false;
    require(snapshot.activeCount > 0 &&
                !activity.tryRunWhenIdle(snapshot.generation, [&] { entered = true; }) && !entered,
            message);
}
ScreenshotToolPalette* palette() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(widget);
            toolbar != nullptr && toolbar->isVisible()) {
            return toolbar->palette();
        }
    }
    require(false, "recording toolbar must exist");
    return nullptr;
}

ScreenRecordingAreaWindow* recordingAreaWindow() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* area = qobject_cast<ScreenRecordingAreaWindow*>(widget);
            area && area->isVisible())
            return area;
    }
    require(false, "recording area must exist");
    return nullptr;
}

void requireModalCenteredOnArea(adqt::widgets::AdModal* modal, const QRect& areaGeometry,
                                QScreen* screen) {
    const auto* window = modal->contentWidget()->window();
    const QRect available = screen->availableGeometry().adjusted(16, 16, -16, -16);
    QPoint expected(areaGeometry.center().x() - window->width() / 2,
                    areaGeometry.center().y() - window->height() / 2);
    expected.setX(
        std::clamp(expected.x(), available.left(), available.right() - window->width() + 1));
    expected.setY(
        std::clamp(expected.y(), available.top(), available.bottom() - window->height() + 1));
    require((window->geometry().topLeft() - expected).manhattanLength() <= 2,
            "recording modals must center on the recording area within the available display");
}

class ErrorObserver final : public QObject {
  public:
    int shown = 0;
    QString lastText;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Show) {
            if (auto* dialog = qobject_cast<QMessageBox*>(watched)) {
                ++shown;
                lastText = dialog->text();
                QTimer::singleShot(0, dialog, &QMessageBox::accept);
            }
        }
        return false;
    }
};

class WindowInputBlockObserver final : public QObject {
  public:
    bool blocked = false;
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::WindowBlocked)
            blocked = true;
        else if (event->type() == QEvent::WindowUnblocked)
            blocked = false;
        return false;
    }
};

void waitForIdle(ScreenRecordingController& controller) {
    QElapsedTimer deadline;
    deadline.start();
    while (controller.isRecording() && deadline.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 100);
    }
    require(!controller.isRecording(), "controlled finalization must return to idle");
}

void waitForRecording(ScreenRecordingController& controller) {
    QElapsedTimer deadline;
    deadline.start();
    while (!controller.isRecording() && deadline.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 100);
    }
    require(controller.isRecording(), "controlled start must reach the recording state");
}

int recordingWindowCount() {
    int count = 0;
    for (auto* widget : QApplication::topLevelWidgets()) {
        count += qobject_cast<ScreenRecordingAreaWindow*>(widget) != nullptr ||
                 qobject_cast<ScreenRecordingToolbarWindow*>(widget) != nullptr;
    }
    return count;
}

adqt::widgets::AdButton* recordingToolbarButton(const char* accessibleName) {
    for (auto* button : palette()->mainPanel()->findChildren<adqt::widgets::AdButton*>()) {
        if (button->accessibleName() == QLatin1String(accessibleName)) {
            return button;
        }
    }
    return nullptr;
}

void joinHeldExport(std::promise<void>& release, int previousExports) {
    release.set_value();
    exportEntered = nullptr;
    exportGate = {};
    QElapsedTimer elapsed;
    elapsed.start();
    while (exports.load() == previousExports && elapsed.elapsed() < 2000) {
        QThread::msleep(1);
    }
    require(exports.load() == previousExports + 1,
            "controlled finalization must finish the backend stop");
}

void stopAndCopyBusyIndicatorsStayOnTheInitiatingControl() {
    for (const bool copy : {false, true}) {
        ScreenRecordingController controller(testEffectsSource);
        std::promise<void> release;
        std::promise<void> entered;
        auto enteredFuture = entered.get_future();
        exportGate = release.get_future().share();
        exportEntered = &entered;
        const int previousExports = exports.load();
        controller.open({40, 40, 320, 240});
        controller.startRecording();
        waitForRecording(controller);
        auto* startButton = recordingToolbarButton("Start recording");
        auto* stopButton = recordingToolbarButton("Stop recording");
        auto* copyButton = recordingToolbarButton("Copy recording");
        require(startButton != nullptr && stopButton != nullptr && copyButton != nullptr,
                "recording start, stop, and copy controls must exist");
        if (copy) {
            palette()->recordingCopyRequested();
        } else {
            palette()->recordingStopRequested();
        }
        require(enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "finalization must reach the controlled backend");
        QCoreApplication::processEvents();
        require(palette()->recordingBusyOperation() ==
                    (copy ? ScreenshotToolPalette::RecordingBusyOperation::Copying
                          : ScreenshotToolPalette::RecordingBusyOperation::Stopping),
                "finalization must publish the initiating busy operation");
        require(stopButton->busy() == !copy && copyButton->busy() == copy && !startButton->busy(),
                "only the initiating stop or copy control may show a loading indicator");
        require(!stopButton->isEnabled() && !copyButton->isEnabled(),
                "busy finalization must lock stop and copy");
        joinHeldExport(release, previousExports);
        if (!copy) {
            waitForIdle(controller);
            require(palette()->recordingBusyOperation() ==
                            ScreenshotToolPalette::RecordingBusyOperation::None &&
                        !stopButton->busy() && !copyButton->busy(),
                    "a completed stop must clear every recording busy indicator");
        }
    }
}

void renderJobDestructionKeepsStateAvailableToModalObservers() {
    auto job =
        std::make_unique<RecordingRenderJob>(new SnowRecordingSource, true, nullptr, nullptr);
    job->start();
    auto* modal = job->findChild<adqt::widgets::AdModal*>();
    require(modal && modal->isOpen(), "render teardown fixture must open its modal");
    QObject observer;
    int closedNotifications = 0;
    auto* retiring = job.get();
    QObject::connect(modal, &adqt::widgets::AdModal::closed, &observer, [&] {
        require(retiring->state().contains(QStringLiteral("render_phase")),
                "modal close observers can inspect the retiring render job");
        ++closedNotifications;
    });
    job.reset();
    require(closedNotifications == 1, "render job destruction must close its modal once");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void destructionKeepsControllerAvailableToActivityObservers() {
    for (const bool recording : {false, true}) {
        std::promise<void> destroyed;
        auto destroyedFuture = destroyed.get_future();
        if (recording)
            nextSessionDestroyCompleted = &destroyed;
        auto controller = std::make_unique<ScreenRecordingController>(testEffectsSource);
        controller->open(QRect(40, 40, 320, 240));
        if (recording) {
            controller->startRecording();
            waitForRecording(*controller);
        }
        QObject observer;
        int inactiveNotifications = 0;
        auto* retiring = controller.get();
        QObject::connect(
            retiring, &ScreenRecordingController::captureActivityChanged, &observer,
            [&](bool active) {
                if (!active) {
                    require(retiring->automationState().contains(QStringLiteral("state")),
                            "final activity observers must be able to inspect the controller");
                    static_cast<void>(retiring->isOpen());
                    static_cast<void>(retiring->blocksMemoryTrimming());
                    ++inactiveNotifications;
                }
            });
        controller.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(inactiveNotifications == 1 && recordingWindowCount() == 0,
                "controller destruction must release capture activity and retire its windows once");
        if (recording) {
            // Destruction deliberately transfers native cleanup off the GUI. Drain this fixture's
            // session after the immediate UI assertions before reusing the fake native owner.
            destroyedFuture.wait();
        }
    }
}

void recordingCanCloseDuringAndAfterFinalization() {
    // Earlier destruction fixtures intentionally leave cleanup on native workers. Their activity
    // reservations must retire before these scenarios snapshot process-wide backend counters.
    while (snow_shot::runtime::RuntimeActivityTracker::shared().snapshot().activeCount != 0) {
        QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 100);
    }
    const QRect screenRegion =
        ScreenshotGeometryMapper::physicalRectForScreen(*QGuiApplication::primaryScreen());
    for (const QRect& region : {QRect(40, 40, 320, 240), screenRegion}) {
        for (const bool paused : {false, true}) {
            for (const bool copy : {false, true}) {
                for (const bool closeWhileBusy : {false, true}) {
                    ScreenRecordingController controller(testEffectsSource);
                    int finalized = 0;
                    QObject::connect(&controller, &ScreenRecordingController::finalized,
                                     &controller, [&] { ++finalized; });
                    std::promise<void> release;
                    std::promise<void> entered;
                    auto enteredFuture = entered.get_future();
                    exportGate = release.get_future().share();
                    exportEntered = &entered;
                    const int previousExports = exports.load();
                    const int previousDestroyed = destroyedSessions.load();
                    controller.open(region);
                    controller.startRecording();
                    waitForRecording(controller);
                    if (paused)
                        palette()->recordingPauseRequested();
                    if (copy)
                        palette()->recordingCopyRequested();
                    else
                        palette()->recordingStopRequested();
                    require(enteredFuture.wait_for(std::chrono::seconds(2)) ==
                                std::future_status::ready,
                            "finalization must reach the controlled backend");
                    if (!closeWhileBusy) {
                        joinHeldExport(release, previousExports);
                        waitForIdle(controller);
                    }
                    auto* closeButton = recordingToolbarButton("Close recording");
                    require(closeButton && closeButton->isEnabled(),
                            "Close must remain available during and after recording finalization");
                    QPointer<QWidget> toolbar(closeButton->window());
                    const QPointF localPosition(closeButton->rect().center());
                    const QPointF globalPosition(closeButton->mapToGlobal(localPosition.toPoint()));
                    QMouseEvent down(QEvent::MouseButtonPress, localPosition, globalPosition,
                                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QMouseEvent up(QEvent::MouseButtonRelease, localPosition, globalPosition,
                                   Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(closeButton, &down);
                    QCoreApplication::sendEvent(closeButton, &up);
                    require(!controller.isOpen(),
                            "clicking Close must immediately hide recording UI");
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    require(toolbar.isNull() && recordingWindowCount() == 0,
                            "clicking Close must retire both recording windows");
                    if (closeWhileBusy) {
                        require(
                            controller.isRecording() && finalized == 0 &&
                                destroyedSessions.load() == previousDestroyed,
                            "closing must preserve pending finalization and its native session");
                        joinHeldExport(release, previousExports);
                        waitForIdle(controller);
                    }
                    if (finalized != 1 || exports.load() != previousExports + 1 ||
                        destroyedSessions.load() != previousDestroyed + 1 ||
                        recordingWindowCount() != 0) {
                        std::cerr << "close finalization: paused=" << paused << " copy=" << copy
                                  << " closeWhileBusy=" << closeWhileBusy
                                  << " finalized=" << finalized
                                  << " exports=" << exports.load() - previousExports
                                  << " destroyed=" << destroyedSessions.load() - previousDestroyed
                                  << " windows=" << recordingWindowCount() << '\n';
                    }
                    require(finalized == 1 && exports.load() == previousExports + 1 &&
                                destroyedSessions.load() == previousDestroyed + 1 &&
                                recordingWindowCount() == 0,
                            "background export must finish once without reopening recording UI");
                    if (copy) {
                        const auto* mime = QApplication::clipboard()->mimeData();
                        require(mime && mime->hasUrls() &&
                                    mime->urls() == QList<QUrl>{QUrl::fromLocalFile(
                                                        controller.automationState()
                                                            .value(QStringLiteral("path"))
                                                            .toString())},
                                "closing during Copy must preserve its finalized clipboard result");
                    }
                }
            }
        }
    }
}

void recordingDeleteOnCloseTests(const QString& directory) {
    using snow_shot::storage::RecordingSettings;
    const auto wait = [](auto predicate) {
        QElapsedTimer deadline;
        deadline.start();
        while (!predicate() && deadline.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(1);
        }
        require(predicate(), "recording close fixture must reach its controlled boundary");
    };
    const auto idle = [&] {
        wait([] {
            return snow_shot::runtime::RuntimeActivityTracker::shared().snapshot().activeCount == 0;
        });
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };
    const auto finishRender = [&](ScreenRecordingController& controller) {
        wait([&] {
            return controller.automationState()
                       .value(QStringLiteral("render_duration_ms"))
                       .toInteger() == 2300;
        });
        renderState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
    };
    idle();
    recordingFileFixture = true;
    require(RecordingSettings().setNotifyAfterExportCompletes(true),
            "enable notifications to detect unwanted exports");
    const auto reset = qScopeGuard([] {
        recordingFileFixture = false;
        RecordingSettings().setDeleteFilesOnRecordingClose(false);
        RecordingSettings().setNotifyAfterExportCompletes(false);
    });
    QFile previous(QDir(directory).filePath(QStringLiteral("previous-recording.mp4")));
    require(previous.open(QIODevice::WriteOnly) && previous.write("previous recording") > 0,
            "create an earlier exported recording");
    previous.close();
    int fixture = 0;
    for (bool deferred : {false, true}) {
        for (int delay : {0, 10}) {
            require(RecordingSettings().setDeleteFilesOnRecordingClose(true),
                    "enable deletion for canceled scheduled starts");
            const int beforeStarts = starts, beforeExports = exports,
                      beforeDestroyed = destroyedSessions, beforeDeferred = deferredCreates;
            ScreenRecordingController controller(testEffectsSource);
            QString error;
            require(controller.startAutomation(QRect(80, 80, 320, 240),
                                               {{QStringLiteral("post_processing"), deferred},
                                                {QStringLiteral("start_delay_seconds"), delay}},
                                               &error) &&
                        controller.controlAutomation(QStringLiteral("close"), {}, &error),
                    "Close cancels a scheduled start or countdown");
            idle();
            require(!controller.isOpen() && !controller.isRecording() && starts == beforeStarts &&
                        exports == beforeExports && destroyedSessions == beforeDestroyed &&
                        deferredCreates == beforeDeferred && recordingWindowCount() == 0,
                    "Close before native startup creates no recording files or session");
        }
        for (bool paused : {false, true}) {
            // Exercise toolbar, configured shortcut, native window Close, and
            // both automation aliases through the same controller handler.
            for (int route = 0; route < 5; ++route) {
                idle();
                require(RecordingSettings().setDeleteFilesOnRecordingClose(true),
                        "enable deletion for direct Close");
                const QString output =
                    QDir(directory).filePath(QStringLiteral("discard-%1.mp4").arg(++fixture));
                const int beforeExports = exports, beforeDestroyed = destroyedSessions,
                          beforeRenders = renderStarts, beforeSources = sourceDestroys;
                ScreenRecordingController controller(testEffectsSource);
                int finalized = 0, notified = 0;
                QVector<bool> captureActivity;
                QObject::connect(&controller, &ScreenRecordingController::finalized, &controller,
                                 [&] { ++finalized; });
                QObject::connect(&controller,
                                 &ScreenRecordingController::exportNotificationRequested,
                                 &controller, [&] { ++notified; });
                QObject::connect(&controller, &ScreenRecordingController::captureActivityChanged,
                                 &controller, [&](bool active) { captureActivity.append(active); });
                QString error;
                require(controller.startAutomation(QRect(80, 80, 320, 240),
                                                   {{QStringLiteral("path"), output},
                                                    {QStringLiteral("post_processing"), deferred}},
                                                   &error),
                        "start a recording to discard");
                waitForRecording(controller);
                if (paused)
                    require(controller.controlAutomation(QStringLiteral("pause"), {}, &error),
                            "pause a recording to discard");
                QApplication::clipboard()->setText(QStringLiteral("keep clipboard"));
                std::promise<void> release;
                sessionDestroyGate = release.get_future().share();
                sessionDestroyEntered = false;
                if (route == 0) {
                    recordingToolbarButton("Close recording")->click();
                } else if (route == 1) {
                    require(palette()->activateRecordingShortcut(QStringLiteral("end_recording")),
                            "configured recording Close shortcut must activate");
                } else if (route == 2) {
                    palette()->window()->close();
                } else {
                    require(controller.controlAutomation(route == 3 ? QStringLiteral("close")
                                                                    : QStringLiteral("cancel"),
                                                         {}, &error),
                            "automation Close aliases discard an active recording");
                }
                require(!controller.isOpen() && captureActivity == QVector<bool>({true, false}),
                        "Close hides recording UI and releases capture suppression immediately");
                wait([] { return sessionDestroyEntered.load(); });
                require(controller.blocksMemoryTrimming() && controller.isRecording(),
                        "pending cancellation keeps the session busy and blocks memory trimming");
                requireMemoryAdmissionBlocked("cancellation retains its native activity lease");
                require(!controller.startAutomation(QRect(80, 80, 320, 240), {}, &error) &&
                            error == QStringLiteral("busy"),
                        "a new recording cannot overlap cancellation teardown");
                require(exports == beforeExports && !recordingStopRequested &&
                            destroyedSessions == beforeDestroyed,
                        "Close cancels without requesting Stop or finalization");
                require(controller.controlAutomation(QStringLiteral("close"), {}, &error),
                        "repeated Close during cancellation is harmless");
                release.set_value();
                waitForIdle(controller);
                idle();
                sessionDestroyGate = {};
                require(
                    destroyedSessions == beforeDestroyed + 1 && finalized == 0 && notified == 0 &&
                        renderStarts == beforeRenders && sourceDestroys == beforeSources &&
                        !QFileInfo::exists(output) &&
                        !QFileInfo::exists(output + QStringLiteral(".test-staging")) &&
                        !QFileInfo::exists(output + QStringLiteral(".test-temporary")) &&
                        QApplication::clipboard()->text() == QStringLiteral("keep clipboard"),
                    "discard removes current artifacts exactly once without exporting or copying");
                require(
                    controller.automationState()
                            .value(QStringLiteral("path"))
                            .toString()
                            .isEmpty() &&
                        !controller.automationState().value(QStringLiteral("finalized")).toBool() &&
                        !controller.blocksMemoryTrimming() && recordingWindowCount() == 0,
                    "discarded sessions return to idle without publishing a file");
            }
        }
        for (bool enabled : {false, true}) {
            idle();
            require(RecordingSettings().setDeleteFilesOnRecordingClose(enabled),
                    "set startup Close preference");
            const QString output =
                QDir(directory).filePath(QStringLiteral("starting-%1.mp4").arg(++fixture));
            const int beforeExports = exports, beforeDestroyed = destroyedSessions;
            auto controller = std::make_unique<ScreenRecordingController>(testEffectsSource);
            std::promise<void> releaseStart, releaseDestroy;
            sessionCreateGate = releaseStart.get_future().share();
            sessionCreateEntered = false;
            QString error;
            require(controller->startAutomation(QRect(80, 80, 320, 240),
                                                {{QStringLiteral("path"), output},
                                                 {QStringLiteral("post_processing"), deferred}},
                                                &error),
                    "start delayed backend fixture");
            wait([] { return sessionCreateEntered.load(); });
            require(controller->controlAutomation(QStringLiteral("close"), {}, &error) &&
                        !controller->isOpen(),
                    "Close stays responsive during native startup");
            require(RecordingSettings().setDeleteFilesOnRecordingClose(!enabled) &&
                        controller->controlAutomation(QStringLiteral("close"), {}, &error),
                    "repeated Close preserves the original startup decision");
            if (enabled) {
                sessionDestroyGate = releaseDestroy.get_future().share();
                sessionDestroyEntered = false;
            }
            releaseStart.set_value();
            wait([&] {
                return enabled ? sessionDestroyEntered.load() : destroyedSessions > beforeDestroyed;
            });
            sessionCreateGate = {};
            if (enabled) {
                controller.reset();
                requireMemoryAdmissionBlocked(
                    "controller destruction retains pending cancellation");
                releaseDestroy.set_value();
            } else {
                if (deferred)
                    finishRender(*controller);
                waitForIdle(*controller);
                controller.reset();
            }
            idle();
            sessionDestroyGate = {};
            require(destroyedSessions == beforeDestroyed + 1 &&
                        exports == beforeExports + (enabled ? 0 : 1) &&
                        QFileInfo::exists(output) == !enabled &&
                        !QFileInfo::exists(output + QStringLiteral(".test-staging")) &&
                        !QFileInfo::exists(output + QStringLiteral(".test-temporary")),
                    "startup Close snapshots deletion and survives controller destruction");
        }
        for (bool paused : {false, true}) {
            idle();
            require(RecordingSettings().setDeleteFilesOnRecordingClose(false),
                    "disable deletion to preserve direct Close behavior");
            const QString output =
                QDir(directory).filePath(QStringLiteral("closed-%1.mp4").arg(++fixture));
            const int beforeExports = exports, beforeDestroyed = destroyedSessions;
            ScreenRecordingController controller(testEffectsSource);
            QString error;
            require(controller.startAutomation(QRect(80, 80, 320, 240),
                                               {{QStringLiteral("path"), output},
                                                {QStringLiteral("post_processing"), deferred}},
                                               &error),
                    "start default Close behavior fixture");
            waitForRecording(controller);
            if (paused)
                require(controller.controlAutomation(QStringLiteral("pause"), {}, &error),
                        "pause default Close behavior fixture");
            require(controller.controlAutomation(QStringLiteral("close"), {}, &error),
                    "disabled delete-on-close still saves the recording");
            if (deferred)
                finishRender(controller);
            waitForIdle(controller);
            idle();
            require(exports == beforeExports + 1 && destroyedSessions == beforeDestroyed + 1 &&
                        QFileInfo::exists(output),
                    "disabled delete-on-close saves both active and paused recordings");
        }
        // Stop, Copy, and Trim take ownership of finalization before Close.
        for (int action = 0; action < 3; ++action) {
            idle();
            require(RecordingSettings().setDeleteFilesOnRecordingClose(true),
                    "enable deletion while testing explicit save actions");
            const QString output =
                QDir(directory).filePath(QStringLiteral("saved-%1.mp4").arg(++fixture));
            const int beforeExports = exports, beforeDestroyed = destroyedSessions;
            ScreenRecordingController controller(testEffectsSource);
            QString error;
            require(controller.startAutomation(QRect(80, 80, 320, 240),
                                               {{QStringLiteral("path"), output},
                                                {QStringLiteral("post_processing"), deferred}},
                                               &error),
                    "start explicit save fixture");
            waitForRecording(controller);
            std::promise<void> release, entered;
            auto reached = entered.get_future();
            exportGate = release.get_future().share();
            exportEntered = &entered;
            if (action == 2)
                recordingToolbarButton("Trim Video")->click();
            else
                require(controller.controlAutomation(action == 0 ? QStringLiteral("stop")
                                                                 : QStringLiteral("copy"),
                                                     {}, &error),
                        "explicit Stop or Copy accepts finalization");
            require(reached.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                    "explicit save reaches the gated finalization worker");
            require(controller.controlAutomation(QStringLiteral("close"), {}, &error),
                    "Close preserves an accepted save operation");
            joinHeldExport(release, beforeExports);
            if (deferred && action != 2)
                finishRender(controller);
            waitForIdle(controller);
            idle();
            require(exports == beforeExports + 1 && destroyedSessions == beforeDestroyed + 1 &&
                        QFileInfo::exists(output),
                    "Stop, Copy, and Trim still save when delete-on-close is enabled");
        }
    }
    require(previous.open(QIODevice::ReadOnly) && previous.readAll() == "previous recording",
            "deleting current recording artifacts preserves earlier exports");
}

void recordingAutoExitAfterSuccessfulFinalization(bool notifications = false) {
    const snow_shot::storage::RecordingSettings settings;
    require(!settings.autoExitAfterRecordingEnds(), "recording auto-exit starts disabled");
    require(settings.setNotifyAfterExportCompletes(notifications),
            "set export notification preference");
    const auto wait = [](auto predicate) {
        QElapsedTimer deadline;
        deadline.start();
        while (!predicate() && deadline.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(predicate(), "recording auto-exit operation must complete asynchronously");
    };
    for (const bool enabled : {false, true}) {
        require(settings.setAutoExitAfterRecordingEnds(enabled), "set auto-exit preference");
        for (const bool deferred : {false, true}) {
            for (const bool copy : {false, true}) {
                for (const bool succeeds : {false, true}) {
                    ScreenRecordingController controller(testEffectsSource);
                    int finalized = 0;
                    QStringList notificationPaths;
                    QObject::connect(
                        &controller, &ScreenRecordingController::exportNotificationRequested,
                        &controller, [&](const QString& path) { notificationPaths.append(path); });
                    QObject::connect(&controller, &ScreenRecordingController::finalized,
                                     &controller, [&] {
                                         ++finalized;
                                         require(controller.isOpen() == !enabled,
                                                 "auto-exit precedes completion observers");
                                     });
                    QString error;
                    require(
                        controller.startAutomation({40, 40, 320, 240},
                                                   {{QStringLiteral("post_processing"), deferred},
                                                    {QStringLiteral("start_delay_seconds"), 0}},
                                                   &error),
                        "prepare the auto-exit recording fixture");
                    waitForRecording(controller);
                    std::promise<void> release;
                    std::promise<void> entered;
                    auto enteredFuture = entered.get_future();
                    exportGate = release.get_future().share();
                    exportEntered = &entered;
                    failExport = !succeeds;
                    QApplication::clipboard()->clear();
                    const int previousExports = exports.load();
                    require(controller.controlAutomation(
                                copy ? QStringLiteral("copy") : QStringLiteral("stop"), {}, &error),
                            "finish the recording through Stop or Copy");
                    require(enteredFuture.wait_for(std::chrono::seconds(2)) ==
                                    std::future_status::ready &&
                                controller.isOpen(),
                            "auto-exit must wait for pending recording finalization");
                    require(notificationPaths.isEmpty(), "pending exports must not notify");
                    joinHeldExport(release, previousExports);
                    if (deferred && succeeds) {
                        wait([&] {
                            return controller.automationState()
                                       .value(QStringLiteral("render_duration_ms"))
                                       .toInteger() == 2300;
                        });
                        require(controller.isOpen(), "auto-exit must wait for deferred rendering");
                        require(notificationPaths.isEmpty(),
                                "pending deferred rendering must not notify");
                        renderState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
                    }
                    waitForIdle(controller);
                    failExport = false;
                    require(controller.isOpen() == !(enabled && succeeds) &&
                                finalized == (succeeds ? 1 : 0),
                            "only successful recordings auto-exit when enabled");
                    require(notificationPaths.size() == (notifications && succeeds ? 1 : 0),
                            "only successful exports notify once when enabled");
                    if (!notificationPaths.isEmpty())
                        require(notificationPaths.first() == controller.automationState()
                                                                 .value(QStringLiteral("path"))
                                                                 .toString(),
                                "notification identifies the successfully published export");
                    const auto* mime = QApplication::clipboard()->mimeData();
                    if (copy && succeeds) {
                        require(mime && mime->urls() == QList<QUrl>{QUrl::fromLocalFile(
                                                            controller.automationState()
                                                                .value(QStringLiteral("path"))
                                                                .toString())},
                                "auto-exit preserves the finalized Copy result");
                    } else {
                        require(!mime || !mime->hasUrls(),
                                "Stop and failed Copy never publish clipboard files");
                    }
                    require(controller.controlAutomation(QStringLiteral("close"), {}, &error),
                            "retire the recording fixture");
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    require(recordingWindowCount() == 0, "auto-exit leaves no recording windows");
                }
            }
        }
    }
    for (const bool enabled : {false, true}) {
        require(settings.setAutoExitAfterRecordingEnds(enabled), "set trim auto-exit preference");
        for (const bool deferred : {false, true}) {
            for (const bool succeeds : {false, true}) {
                ScreenRecordingController controller(testEffectsSource);
                QStringList notificationPaths;
                QObject::connect(
                    &controller, &ScreenRecordingController::exportNotificationRequested,
                    &controller, [&](const QString& path) { notificationPaths.append(path); });
                QString error;
                require(controller.startAutomation({40, 40, 320, 240},
                                                   {{QStringLiteral("post_processing"), deferred},
                                                    {QStringLiteral("start_delay_seconds"), 0}},
                                                   &error),
                        "prepare the trim auto-exit fixture");
                waitForRecording(controller);
                palette()->recordingTrimRequested();
                wait([&] {
                    return controller.automationState().value(QStringLiteral("state")) ==
                           QStringLiteral("trimming");
                });
                require(controller.isOpen(), "auto-exit must allow trimming before export");
                palette()
                    ->findChild<QSlider*>(QStringLiteral("screenRecordingTrimStart"))
                    ->setValue(5);
                clipExportState = SNOW_RECORDING_RENDER_STATE_RUNNING;
                QApplication::clipboard()->clear();
                require(controller.controlAutomation(QStringLiteral("copy"), {}, &error),
                        "export the trimmed recording");
                require(controller.isOpen(), "auto-exit must wait for trimmed export");
                require(notificationPaths.isEmpty(), "pending trimmed exports must not notify");
                clipExportState = succeeds ? SNOW_RECORDING_RENDER_STATE_SUCCEEDED
                                           : SNOW_RECORDING_RENDER_STATE_CANCELED;
                wait([&] {
                    return !controller.automationState().value(QStringLiteral("busy")).toBool();
                });
                require(controller.isOpen() == !(enabled && succeeds),
                        "trimmed recording auto-exits only after successful publication");
                const auto* mime = QApplication::clipboard()->mimeData();
                require((mime && mime->hasUrls()) == succeeds,
                        "trimmed Copy result survives auto-exit");
                require(notificationPaths.size() == (notifications && succeeds ? 1 : 0),
                        "successful trimmed exports notify once, canceled exports stay silent");
                if (!notificationPaths.isEmpty())
                    require(
                        mime->urls() == QList<QUrl>{QUrl::fromLocalFile(notificationPaths.first())},
                        "trimmed notification identifies the exported clip rather than its source");
                require(controller.controlAutomation(QStringLiteral("close"), {}, &error),
                        "retire the trim fixture");
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
        }
    }
    clipExportState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
    require(settings.setAutoExitAfterRecordingEnds(false), "restore default auto-exit preference");
    require(settings.setNotifyAfterExportCompletes(false),
            "restore default export notification preference");
}

#ifdef Q_OS_MACOS
void standardCloseFromRecordingArea() {
    ScreenRecordingController controller(testEffectsSource);
    controller.open({40, 40, 320, 240});
    ScreenRecordingAreaWindow* area = nullptr;
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget))
            area = candidate;
    require(area && triggerWindowCloseShortcut(area), "recording area registers standard Close");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(!controller.isOpen() && recordingWindowCount() == 0,
            "standard Close on the area retires both recording windows");
}
#endif

void closeAndStopHaveIndependentUiLifetimes() {
    ErrorObserver errors;
    qApp->installEventFilter(&errors);
    for (const bool close : {false, true}) {
        for (const bool failure : {false, true}) {
            ScreenRecordingController controller(testEffectsSource);
            QVector<bool> captureActivity;
            QObject::connect(&controller, &ScreenRecordingController::captureActivityChanged,
                             &controller, [&](bool active) { captureActivity.push_back(active); });
            require(recordingWindowCount() == 0,
                    "constructing a controller must not create windows");
            controller.open({40, 40, 320, 240});
            auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window());
            QPointer<ScreenRecordingToolbarWindow> previousToolbar(toolbar);
            std::promise<void> release;
            std::promise<void> entered;
            auto enteredFuture = entered.get_future();
            exportGate = release.get_future().share();
            exportEntered = &entered;
            failExport = failure;
            const int previousDestroyed = destroyedSessions;
            const int previousErrors = errors.shown;
            controller.startRecording();
            waitForRecording(controller);
            require(captureActivity == QVector<bool>{true},
                    "setup and recording share uninterrupted desktop capture suppression");
            require(lastDirectConfig.audio_mode == SNOW_CAPTURE_RECORDING_AUDIO_SEPARATE,
                    "subsequent recordings snapshot separate audio tracks");
            require(lastDirectConfig.loop_animated_images == 0,
                    "subsequent recordings must snapshot disabled looping");
            if (close) {
                // Exercise the native close path as well as the toolbar command.
#ifdef Q_OS_MACOS
                require(triggerWindowCloseShortcut(toolbar),
                        "recording toolbar registers standard Close");
#else
                toolbar->close();
#endif
            } else {
                palette()->recordingStopRequested();
            }
            require(enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                    "stop must reach the controlled backend");
            require(captureActivity == QVector<bool>({true, false}),
                    "stopping capture restores desktop surfaces before asynchronous export");
            require(controller.isOpen() != close, "only Close must detach the UI during export");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            require(previousToolbar.isNull() == close && recordingWindowCount() == (close ? 0 : 2),
                    "Close must destroy UI before backend finalization, while Stop retains it");
            controller.open({80, 80, 320, 240});
            require(recordingWindowCount() == (close ? 0 : 2),
                    "busy finalization must reject reopen");
            release.set_value();
            waitForIdle(controller);
            require(captureActivity == QVector<bool>({true, false}),
                    "export success and failure must not reacquire capture suppression");
            require(destroyedSessions == previousDestroyed + 1,
                    "backend must be destroyed exactly once");
            require(errors.shown == previousErrors + (failure ? 1 : 0),
                    "export failure must be reported even after Close");
            if (failure) {
                require(
                    errors.lastText.contains(QStringLiteral("Keep this folder")) &&
                        errors.lastText.contains(QStringLiteral("D:/recordings/recovery")),
                    "failed recovery must show an actionable translated message and media path");
            }
            require(recordingWindowCount() == (close ? 0 : 2),
                    "completion must not recreate closed UI");
            if (!close) {
                require(previousToolbar && previousToolbar->isVisible(),
                        "Stop must keep the original UI usable");
                palette()->recordingCloseRequested();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            exportEntered = nullptr;
            exportGate = {};
            failExport = false;
        }
    }
    qApp->removeEventFilter(&errors);
}

void requireToolbarAboveArea(ScreenRecordingAreaWindow* area) {
    auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window());
    require(toolbar != nullptr, "recording palette must belong to the toolbar window");
    const auto previousInputMode = area->inputMode();
    area->setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    toolbar->move(area->geometry().topLeft());
    area->raise();
    area->activateWindow();
    QCoreApplication::processEvents();
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const HWND areaHandle = reinterpret_cast<HWND>(area->winId());
        const HWND toolbarHandle = reinterpret_cast<HWND>(toolbar->winId());
        SetActiveWindow(areaHandle);
        QCoreApplication::processEvents();
        require(GetActiveWindow() == areaHandle,
                "the recording area must retain focus while the toolbar stays above it");
        require(GetWindow(toolbarHandle, GW_OWNER) == areaHandle,
                "the native recording toolbar must be owned by the area");
        bool toolbarAboveArea = false;
        for (HWND candidate = GetWindow(areaHandle, GW_HWNDPREV); candidate != nullptr;
             candidate = GetWindow(candidate, GW_HWNDPREV)) {
            toolbarAboveArea |= candidate == toolbarHandle;
        }
        require(toolbarAboveArea,
                "activating the overlapping recording area must keep the toolbar above it");
    }
#endif
    require(toolbar->windowHandle()->transientParent() == area->windowHandle(),
            "recording toolbar must retain the area as its transient owner");
    area->setInputMode(previousInputMode);
}

void recordingToolbarOwnershipSurvivesSurfaceRecreation() {
    QWidget firstOwner;
    QWidget secondOwner;
    ScreenRecordingToolbarWindow toolbar;
    const auto setOwner = [&](QWidget* owner) {
        toolbar.setTransientOwnerWindow(owner);
        toolbar.setAutoHideControlOwner(owner);
        QWidget* edge = toolbar.autoHideControlWindow();
        require(!edge->isVisible() && !edge->parentWidget() &&
                    edge->windowHandle()->transientParent() ==
                        (owner ? owner->windowHandle() : nullptr),
                "the hidden auto-hide control must follow owner changes and detachment");
    };
    const auto recreate = [&](QWidget* expectedOwner) {
        const QPointer<QWindow> previousSurface(toolbar.windowHandle());
        toolbar.releaseNativeSurface();
        require(previousSurface.isNull() && toolbar.internalWinId() == 0,
                "surface retirement must not recreate a toolbar while restoring ownership");
        // Qt's composition transition creates the replacement directly, without
        // calling the toolbar's explicit restoreNativeSurface() entry point.
        static_cast<void>(toolbar.winId());
        require(!toolbar.isVisible() && toolbar.parentWidget() == nullptr &&
                    toolbar.windowHandle()->transientParent() ==
                        (expectedOwner ? expectedOwner->windowHandle() : nullptr),
                "implicit surface recreation must preserve the current transient owner");
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            require(GetWindow(reinterpret_cast<HWND>(toolbar.winId()), GW_OWNER) ==
                        (expectedOwner ? reinterpret_cast<HWND>(expectedOwner->winId()) : nullptr),
                    "implicit surface recreation must preserve the current native owner");
        }
#endif
    };
    setOwner(&firstOwner);
    recreate(&firstOwner);
    setOwner(&secondOwner);
    recreate(&secondOwner);
    setOwner(nullptr);
    recreate(nullptr);
    auto owner = std::make_unique<QWidget>();
    setOwner(owner.get());
    owner.reset();
    recreate(nullptr);
}

#ifdef Q_OS_WIN
void recordingCaptureExclusionPreservesToolbarStacking() {
    namespace composition = snow_shot::platform::windows::detail;
    require(QGuiApplication::platformName() == QStringLiteral("windows"),
            "toolbar stacking regression requires native Windows surfaces");
    const snow_shot::storage::RecordingSettings settings;
    require(settings.setAutoHideToolbar(false) && settings.setStartDelaySeconds(0),
            "keep the recording toolbar visible and start without a countdown");
    const QRect screen =
        ScreenshotGeometryMapper::physicalRectForScreen(*QGuiApplication::primaryScreen());
    for (const bool captureToolbar : {true, false}) {
        require(settings.setCaptureToolbarInRecording(captureToolbar), "set toolbar capture");
        ScreenRecordingController controller(testEffectsSource);
        controller.open(QRect(screen.topLeft() + QPoint(80, 80), QSize(640, 360)));
        auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window());
        ScreenRecordingAreaWindow* area = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget))
                area = candidate;
        }
        require(toolbar && area && !toolbar->parentWidget(),
                "recording must use its real parentless toolbar and area");
        controller.startRecording();
        waitForRecording(controller);
        const bool nativeFallback = composition::usesCaptureExclusionComposition(toolbar);
        std::cout << "capture_toolbar=" << captureToolbar
                  << " native_composition_fallback=" << nativeFallback << std::endl;
        if (!captureToolbar) {
            if (QCoreApplication::arguments().contains(
                    QStringLiteral("--require-native-composition-fallback"))) {
                require(nativeFallback, "this run must exercise the OS-triggered fallback");
            }
            // Newer Windows versions can accept affinity on raster surfaces.
            // Exercise the same production transition on those hosts as well.
            if (!nativeFallback) {
                require(composition::applyCaptureExclusionCompositionFallback(toolbar),
                        "exercise the production composition transition");
                require(snow_shot::platform::setWindowExcludedFromCapture(toolbar, true),
                        "replacement surface must retain capture exclusion");
            }
            QWidget* edge = toolbar->autoHideControlWindow();
            if (!composition::usesCaptureExclusionComposition(edge)) {
                require(composition::applyCaptureExclusionCompositionFallback(edge),
                        "exercise the hidden auto-hide control's composition transition");
                require(snow_shot::platform::setWindowExcludedFromCapture(edge, true),
                        "replacement auto-hide control must retain capture exclusion");
            }
            require(!edge->isVisible() &&
                        edge->windowHandle()->transientParent() == area->windowHandle() &&
                        GetWindow(reinterpret_cast<HWND>(edge->winId()), GW_OWNER) ==
                            reinterpret_cast<HWND>(area->winId()),
                    "composition must preserve the hidden auto-hide control's owner");
        }
        DWORD affinity = WDA_NONE;
        const DWORD expectedAffinity = captureToolbar ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE;
        require(GetWindowDisplayAffinity(reinterpret_cast<HWND>(toolbar->winId()), &affinity) &&
                    affinity == expectedAffinity,
                "toolbar stacking must preserve the requested capture policy");
        for (int presentation = 0; presentation < 2; ++presentation) {
            if (presentation != 0) {
                toolbar->hide();
                toolbar->showWithoutActivating();
            }
            requireToolbarAboveArea(area);
            area->setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
            auto* button = toolbar->findChild<adqt::widgets::AdButton*>(
                QStringLiteral("screenRecordingPause"));
            require(button && button->isVisible(), "recording pause button must be available");
            const QPoint target = area->mapToGlobal(area->rect().center());
            toolbar->move(toolbar->pos() + target - button->mapToGlobal(button->rect().center()));
            toolbar->repaint();
            area->raise();
            area->activateInput();
            QCoreApplication::processEvents();
            const HWND toolbarHandle = reinterpret_cast<HWND>(toolbar->winId());
            const QPoint local = button->mapTo(toolbar, button->rect().center());
            POINT point{qRound(local.x() * toolbar->devicePixelRatioF()),
                        qRound(local.y() * toolbar->devicePixelRatioF())};
            require(ClientToScreen(toolbarHandle, &point), "map the overlapping button to desktop");
            require(GetAncestor(WindowFromPoint(point), GA_ROOT) == toolbarHandle,
                    "the overlapping toolbar button must receive native mouse input");
        }
        palette()->recordingStopRequested();
        waitForIdle(controller);
        requireToolbarAboveArea(area);
        require(GetWindowDisplayAffinity(reinterpret_cast<HWND>(toolbar->winId()), &affinity) &&
                    affinity == WDA_NONE,
                "stopping must restore capture visibility without losing toolbar ownership");
        palette()->recordingCloseRequested();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    std::cout << "recording toolbar capture-exclusion stacking passed\n";
}
#endif

void recordingExpandsSmallSelectionsOnOpenAndReopen() {
    ScreenRecordingController controller(testEffectsSource);
    for (const QSize size :
         {QSize(1, 1), QSize(1, 100), QSize(9, 9), QSize(10, 10), QSize(320, 240)}) {
        controller.open(QRect(QPoint(40, 40), size));
        QCoreApplication::processEvents();
        ScreenRecordingAreaWindow* area = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget))
                area = candidate;
        }
        require(area != nullptr, "even a one-pixel selection must open recording");
#ifdef Q_OS_MACOS
        const int minimum = qCeil(10.0 / area->devicePixelRatioF());
#else
        const int minimum = 10;
#endif
        const QSize expected = size.expandedTo(QSize(minimum, minimum));
        require(area->recordingRegion().topLeft() == QPoint(40, 40) &&
                    area->recordingRegion().size() == expected,
                "open and reopen must expand only the undersized dimensions");
        const QJsonArray state =
            controller.automationState().value(QStringLiteral("region")).toArray();
        require(state == QJsonArray{40, 40, expected.width(), expected.height()},
                "controller and visible area must agree on the expanded region");
    }
    palette()->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void recordingInputOwnerKeepsFocusAcrossPresentation() {
    ScreenRecordingController controller(testEffectsSource);
    const QRect region(40, 40, 320, 240);
    controller.open(region);
    auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window());
    ScreenRecordingAreaWindow* area = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget)) {
            area = candidate;
        }
    }
    require(area != nullptr && toolbar != nullptr, "recording windows must exist");
    QCoreApplication::processEvents();
    const auto inputOwner = [&]() -> QWidget* {
        return area->inputMode() == ScreenRecordingAreaWindow::InputMode::Drawing
                   ? static_cast<QWidget*>(area->canvas())
                   : static_cast<QWidget*>(area);
    };
    require(inputOwner() && inputOwner()->window()->isActiveWindow() && inputOwner()->hasFocus(),
            "opening an editable recording region must focus its effective input surface");
    for (const auto mode : {ScreenRecordingAreaWindow::InputMode::Drawing,
                            ScreenRecordingAreaWindow::InputMode::RegionEditing}) {
        area->setInputMode(mode);
        QCoreApplication::processEvents();
        require(inputOwner()->window()->isActiveWindow() && inputOwner()->hasFocus(),
                "editable modes must focus the drawing canvas or region controls");
        area->regionInteractionStarted();
        area->move(area->pos() + QPoint(20, 10));
        area->resize(area->size() + QSize(10, 10));
        area->regionInteractionFinished();
        QCoreApplication::processEvents();
        require(toolbar->isVisible() && inputOwner()->window()->isActiveWindow(),
                "restoring the aligned toolbar must preserve the region input owner's activation");
        controller.open(region);
        QCoreApplication::processEvents();
        require(inputOwner()->window()->isActiveWindow(),
                "reopening an editable region must prioritize its input owner");
    }
    palette()->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void recordingToolbarReconcilesFrameBeforeShowing() {
    ScreenRecordingToolbarWindow toolbar;
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "placement test requires a screen");
    const QRect region = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    toolbar.placeForRecordingRegion(region);
    toolbar.showAndActivate();
    QCoreApplication::processEvents();
#if defined(Q_OS_MACOS)
    require(toolbar.findChild<adqt::widgets::AdDpiStableWindowController*>() == nullptr &&
                qFuzzyCompare(toolbar.paletteHost()->physicalScale(), 1.0),
            "macOS recording toolbar must use native logical sizing");
    require(toolbar.testAttribute(Qt::WA_MacAlwaysShowToolWindow) &&
                !toolbar.windowHandle()->flags().testFlag(Qt::WindowDoesNotAcceptFocus),
            "macOS recording toolbar must remain visible and allow activation");
    for (const QString& size : {QStringLiteral("small"), QStringLiteral("normal")}) {
        require(snow_shot::storage::ScreenshotUiSettings().setToolbarSize(size),
                "recording toolbar size setting must be writable");
        toolbar.prepareForDisplay();
        const qreal scale = size == QStringLiteral("small") ? 0.8 : 1.0;
        require(qFuzzyCompare(toolbar.paletteHost()->physicalScale(), scale),
                "recording toolbar must apply only the configured size multiplier");
        const QRegion panels = toolbar.paletteHost()->interactiveHostRegion();
        require(!toolbar.mask().isEmpty() && (panels - toolbar.mask()).isEmpty(),
                "recording export settings and drawing rows must be included in the mask");
    }
#endif
    toolbar.hide();
    const QSize expected = toolbar.windowSizeHint();
    const QPoint anchor = toolbar.contentPosition();
    // Model Windows retaining the old physical frame after a DPI transition,
    // while the host already contains the destination display's logical layout.
    toolbar.resize(expected.width() / 2, expected.height());
    toolbar.showAndActivate();
    require(toolbar.size() == expected &&
                toolbar.rect().contains(toolbar.paletteHost()->geometry()),
            "showing recording controls must reconcile the frame before painting");
    require(toolbar.contentPosition() == anchor,
            "reconciling the recording frame must preserve its content anchor");
    toolbar.beginRegionInteraction();
    toolbar.resize(expected.width() / 2, expected.height());
    toolbar.endRegionInteraction(region);
    require(toolbar.size() == expected &&
                toolbar.rect().contains(toolbar.paletteHost()->geometry()),
            "restoring recording controls after area interaction must reconcile the frame");
}

void recordingToolbarPlacementAcrossDisplays() {
    ScreenRecordingAreaWindow area;
    ScreenRecordingToolbarWindow toolbar;
    toolbar.setTransientOwnerWindow(&area);
    QRegion desktop;
    for (QScreen* screen : QGuiApplication::screens()) {
        desktop += ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    }
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
        for (int offset : {-bounds.width() / 3, 0, bounds.width() / 3}) {
            for (int height : {100, bounds.height() - 80}) {
                const QRect region(bounds.left() + offset, bounds.top() + 40, bounds.width() / 2,
                                   height);
                // Only selections within the captured desktop can originate in the UI.
                if (!QRegion(region).subtracted(desktop).isEmpty()) {
                    continue;
                }
                area.setRecordingRegion(region);
                toolbar.placeForRecordingRegion(region);
                area.show();
                toolbar.showAndActivate();
                for (int pass = 0; pass < 5; ++pass) {
                    QCoreApplication::processEvents();
                }
                require(toolbar.size() == toolbar.windowSizeHint() &&
                            toolbar.rect().contains(toolbar.paletteHost()->geometry()),
                        "cross-display placement must reconcile the frame before opening panels");
                toolbar.palette()->setActiveTool(ScreenshotToolPalette::Tool::Shape);
                QCoreApplication::processEvents();
                const QPoint position = toolbar.contentPosition();
                const QRect content = toolbar.occupiedContentRect();
                const QRect visible = content.translated(position);
                QScreen* target = ScreenshotGeometryMapper::screenForPhysicalRect(region);
                const QRect logicalBounds = target->geometry();
                require(
                    visible.top() >= logicalBounds.top() &&
                        visible.bottom() <= logicalBounds.bottom(),
                    "recording rows must fit the selected display after cross-display placement");
                if (visible.width() <= logicalBounds.width()) {
                    require(logicalBounds.contains(visible),
                            "recording rows must remain horizontally inside the selected display");
                }
                require(toolbar.rect().contains(
                            content.translated(toolbar.contentPosition() - toolbar.pos())),
                        "recording rows must fit the native frame after cross-display placement");
                toolbar.placeForRecordingRegion(region);
                QCoreApplication::processEvents();
                require(
                    toolbar.contentPosition() == position &&
                        toolbar.occupiedContentRect() == content,
                    "repeated cross-display placement must keep the committed layout and anchor");
                toolbar.palette()->clearActiveTool();
            }
        }
    }
}

void recordingSecondaryPanelsStayOnScreen() {
    ScreenRecordingToolbarWindow toolbar;
    auto* exportButton = toolbar.palette()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenRecordingExportSettings"));
    require(exportButton != nullptr, "recording toolbar must expose export settings");
    if (toolbar.palette()->recordingExportSettingsVisible()) {
        exportButton->click();
    }
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "placement test requires a screen");
    const QRect bounds = screen->geometry();
    const QRect physicalBounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const qreal dpr = screen->devicePixelRatio();
    const int mainHeight = toolbar.occupiedContentRect().height();
    const QRect region(physicalBounds.left() + qRound(40 * dpr),
                       physicalBounds.top() + qRound(40 * dpr), qRound((bounds.width() - 80) * dpr),
                       qRound((bounds.height() - mainHeight - 60) * dpr));
    toolbar.placeForRecordingRegion(region);
    toolbar.show();
    QCoreApplication::processEvents();
    const auto requireFits = [&](const char* message) {
        const QRect occupied = toolbar.occupiedContentRect().translated(toolbar.contentPosition());
        require(occupied.top() >= bounds.top() && occupied.bottom() <= bounds.bottom(), message);
        // The default offscreen screen is narrower than the recording main row.
        if (occupied.width() <= bounds.width()) {
            require(bounds.contains(occupied), message);
        }
#if defined(Q_OS_MACOS)
        require((toolbar.paletteHost()->interactiveHostRegion() - toolbar.mask()).isEmpty(),
                "changing recording panels must update the macOS window mask");
#endif
    };
    const auto requireAnchored = [&]() {
        const QPoint position = toolbar.contentPosition();
        const QRect occupied = toolbar.occupiedContentRect();
        toolbar.placeForRecordingRegion(region);
        require(
            toolbar.contentPosition() == position && toolbar.occupiedContentRect() == occupied,
            "content changes before dragging must match a fresh placement of the whole toolbar");
    };
    requireFits("the collapsed recording toolbar must initially fit on screen");
    exportButton->click();
    QCoreApplication::processEvents();
    requireFits("opening recording export settings must keep all rows on screen");
    requireAnchored();
    exportButton->click();
    requireAnchored();
    toolbar.palette()->setActiveTool(ScreenshotToolPalette::Tool::Shape);
    QCoreApplication::processEvents();
    requireFits("opening recording drawing styles must keep all rows on screen");
    requireAnchored();

    toolbar.palette()->clearActiveTool();
    requireAnchored();
    toolbar.setStyleToolbarAboveMain(false);
    const QPoint interiorPosition = toolbar.constrainedContentPosition(
        QPoint(bounds.left(), bounds.top() + bounds.height() / 3));
    toolbar.moveContentTo(interiorPosition);
    toolbar.paletteHost()->dragStarted(interiorPosition);
    toolbar.paletteHost()->dragFinished(interiorPosition);
    exportButton->click();
    QCoreApplication::processEvents();
    requireFits("recording export settings must fit at an interior position");
    require(toolbar.contentPosition() == interiorPosition,
            "opening a panel after dragging must preserve the user's toolbar position");
    toolbar.placeForRecordingRegion(region);
    exportButton->click();
    requireAnchored();
    exportButton->click();
    requireAnchored();
}
} // namespace

namespace {
void pumpPreview() {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
    }
}
QImage previewImage(QWidget& widget) {
    QImage image(widget.size(), QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    return image;
}
void requireRecordingRegionHandle(ScreenRecordingAreaWindow& area) {
    auto* handle = area.findChild<QWidget*>(QStringLiteral("screenRecordingRegionDragHandle"));
    require(handle && handle->parentWidget() == &area && handle->isVisible() &&
                handle->isEnabled() && !handle->testAttribute(Qt::WA_TransparentForMouseEvents),
            "region editing must retain its interactive move handle");
    require(previewImage(area).pixelColor(handle->geometry().center()).alpha() > 0,
            "region editing must retain its painted interactive move handle");
}
QImage previewTrimImage(QWidget& widget, const QFont& visualFont) {
    struct SavedFont {
        QPointer<QWidget> widget;
        QFont font;
    };
    auto widgets = widget.findChildren<QWidget*>();
    widgets.prepend(&widget);
    QVector<SavedFont> saved;
    for (auto* control : widgets)
        saved.append({control, control->font()});
    const auto apply = [](QWidget* control, const QFont& font) {
        if (auto* select = qobject_cast<adqt::widgets::AdSelect*>(control))
            select->setReferenceFont(font);
        else if (auto* button = qobject_cast<adqt::widgets::AdButton*>(control))
            button->setReferenceFont(font);
        else
            control->setFont(font);
    };
    for (const auto& previous : saved) {
        QFont font = previous.font;
        font.setFamilies(visualFont.families());
        apply(previous.widget, font);
    }
    QCoreApplication::processEvents();
    const QImage image = previewImage(widget);
    for (auto it = saved.crbegin(); it != saved.crend(); ++it)
        if (it->widget)
            apply(it->widget, it->font);
    return image;
}
class PreviewTestTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QByteArray(context) == "CanvasStatusReadout" &&
            QByteArray(source) == "Motion Preview in Progress") {
            return QStringLiteral("Preview translated");
        }
        return {};
    }
};

#ifdef Q_OS_MACOS
void effectsPreviewPhysicalPixels() {
    auto state = std::make_shared<RecordingEffectTestState>();
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(QRect(40, 40, 640, 480));
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    RecordingEffectPreview preview(area, std::make_unique<RecordingEffectTestSource>(state));
    area.show();
    pumpPreview();
    const qreal dpr = area.devicePixelRatioF();
    const QSize canvasSize(qRound(640 * dpr), qRound(480 * dpr));
    for (const QSize exportSize : {QSize(320, 240), QSize(1920, 1080)}) {
        preview.configure(area.recordingRegion(), exportSize, Qt::red, Qt::transparent, true);
        preview.setEligible(true);
        pumpPreview();
        require(state->output == canvasSize,
                "macOS preview must rasterize desktop points into physical display pixels");
        state->publish(false);
        QImage keycap(64, 64, QImage::Format_RGBA8888_Premultiplied);
        keycap.fill(Qt::blue);
        state->frame->tiles.push_back({QRect(128, 128, 64, 64), keycap, canvasSize});
        QImage mouse(82, 82, QImage::Format_RGBA8888_Premultiplied);
        mouse.fill(Qt::red);
        const QPoint center(canvasSize.width() / 2, canvasSize.height() / 2);
        state->frame->tiles.push_back({QRect(center - QPoint(41, 41), mouse.size()), mouse, {}});
        state->notify();
        pumpPreview();
        QImage rendered(
            QSize(qRound(area.canvas()->width() * dpr), qRound(area.canvas()->height() * dpr)),
            QImage::Format_RGBA8888_Premultiplied);
        rendered.setDevicePixelRatio(dpr);
        rendered.fill(Qt::transparent);
        QPainter painter(&rendered);
        area.canvas()->render(&painter);
        painter.end();
        QRect keyboardPixels;
        QRect mousePixels;
        for (int y = 0; y < rendered.height(); ++y) {
            for (int x = 0; x < rendered.width(); ++x) {
                const QColor color = rendered.pixelColor(x, y);
                if (color.blue() > 128)
                    keyboardPixels |= QRect(x, y, 1, 1);
                if (color.red() > 128)
                    mousePixels |= QRect(x, y, 1, 1);
            }
        }
        require(keyboardPixels == QRect(128, 128, 64, 64),
                "macOS keycap size and placement must use physical pixels at every DPI");
        require(mousePixels == QRect(center - QPoint(41, 41), QSize(82, 82)),
                "macOS mouse effects must keep their pixel size and align with the pointer");
        state->publish(false);
        pumpPreview();
        const QImage cleared = previewImage(*area.canvas());
        require(
            cleared.pixelColor(qRound(128 / dpr), qRound(128 / dpr)).alpha() == 0 &&
                cleared.pixelColor(qRound(center.x() / dpr), qRound(center.y() / dpr)).alpha() == 0,
            "expired effects must clear their display-scaled canvas regions");
        for (const auto eventType :
             {QEvent::DevicePixelRatioChange, QEvent::ScreenChangeInternal}) {
            const int startsBeforeScaleChange = state->starts;
            const quint64 generationBeforeScaleChange = preview.generation();
            QEvent scaleChange(eventType);
            QCoreApplication::sendEvent(&area, &scaleChange);
            pumpPreview();
            require(preview.generation() > generationBeforeScaleChange &&
                        state->starts == startsBeforeScaleChange && state->active,
                    "display scale changes must reconfigure the active preview canvas");
        }
    }
}
#endif

void playbackPreviewAtZero() {
#ifdef Q_OS_WIN
    // The offscreen QPA does not initialize OLE like Qt's Windows GUI thread does.
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
            "playback preview regression must run in the GUI thread's STA apartment");
    const auto apartment = qScopeGuard([] { CoUninitialize(); });
#endif
    auto state = std::make_shared<RecordingEffectTestState>();
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(QRect(40, 40, 640, 480));
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    RecordingEffectPreview preview(area, std::make_unique<RecordingEffectTestSource>(state));
    QString previewError;
    preview.reportError = [&](const QString& error) { previewError = error; };
    area.show();
    pumpPreview();
    const auto configure = [&](uint32_t overlay, const QSize& output) {
        preview.configure(area.recordingRegion(), output, Qt::transparent, Qt::transparent, false,
                          500, QColor(0, 0, 0, 204), Qt::white, 64, Qt::transparent, false, 41,
                          overlay);
        preview.setEligible(true);
        pumpPreview();
        require(previewError.isEmpty(),
                "zero-second playback preview must initialize without errors");
    };
    const auto render = [&] {
        const qreal dpr = area.devicePixelRatioF();
        QImage image(
            QSize(qRound(area.canvas()->width() * dpr), qRound(area.canvas()->height() * dpr)),
            QImage::Format_RGBA8888_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        preview.renderAfterCanvas(painter, {});
        return image;
    };
    const auto visibleBounds = [](const QImage& image) {
        QRect bounds;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixelColor(x, y).alpha() != 0)
                    bounds |= QRect(x, y, 1, 1);
        return bounds;
    };
    QImage referenceProgress;
    QImage referenceTime;
    for (const QSize output :
         {QSize(640, 480), QSize(320, 240), QSize(1920, 1080), QSize(3840, 2160)}) {
        configure(SNOW_RECORDING_PLAYBACK_OVERLAY_PROGRESS_BAR, output);
        const QImage progress = render();
        const QRect track = visibleBounds(progress);
        // Fractional DPI rounds the child canvas outward beyond the exact selection.
        const QRectF selection = area.selectionRect().translated(-area.canvasGeometry().topLeft());
        const int selectionBottom = qRound(selection.bottom() * area.devicePixelRatioF()) - 1;
        const QString trackDescription =
            QStringLiteral("zero-second progress track: DPR=%1, bounds=%2,%3 %4x%5, canvas=%6x%7, "
                           "center alpha=%8")
                .arg(area.devicePixelRatioF())
                .arg(track.x())
                .arg(track.y())
                .arg(track.width())
                .arg(track.height())
                .arg(progress.width())
                .arg(progress.height())
                .arg(progress.pixelColor(track.center()).alpha());
        require(!track.isEmpty() && qAbs(track.height() - 12) <= 1 &&
                    qAbs(track.bottom() - selectionBottom) <= 1 &&
                    progress.pixelColor(track.center()) == QColor(0, 0, 0, 64),
                qPrintable(trackDescription));
        if (referenceProgress.isNull())
            referenceProgress = progress;
        require(progress == referenceProgress,
                "progress preview pixels must not depend on export resolution");
        configure(SNOW_RECORDING_PLAYBACK_OVERLAY_PLAYBACK_TIME, output);
        const QImage timer = render();
        const QRect time = visibleBounds(timer);
        require(!time.isEmpty() && time.bottom() < track.top(),
                "switching to zero-second play time must replace the track with a timer badge");
        if (referenceTime.isNull())
            referenceTime = timer;
        require(timer == referenceTime,
                "playback time preview pixels must not depend on export resolution");
        require(state->starts == 0 && !state->active,
                "static playback preview must not start native input observers");
        preview.setEligible(false);
        require(visibleBounds(render()).isEmpty(),
                "starting recording must clear the playback preview");
        preview.setEligible(true);
        pumpPreview();
        require(visibleBounds(render()) == time, "returning to idle must restore zero time");
        area.hide();
        require(visibleBounds(render()).isEmpty(), "hiding the area must clear the overlay");
        area.show();
        pumpPreview();
        require(visibleBounds(render()) == time, "showing the area must restore zero time");
        QDialog modal(&area);
        modal.setModal(true);
        modal.show();
        pumpPreview();
        require(visibleBounds(render()).isEmpty(), "a modal must suspend the playback preview");
        modal.close();
        pumpPreview();
        require(visibleBounds(render()) == time, "closing a modal must restore zero time");
        configure(SNOW_RECORDING_PLAYBACK_OVERLAY_NONE, output);
        require(visibleBounds(render()).isEmpty(), "disabling playback preview must erase it");
    }
    preview.configure(area.recordingRegion(), QSize(320, 240), Qt::red, Qt::transparent, false, 500,
                      QColor(0, 0, 0, 204), Qt::white, 64, Qt::transparent, false, 41,
                      SNOW_RECORDING_PLAYBACK_OVERLAY_PROGRESS_BAR);
    pumpPreview();
    require(state->active && render() != referenceProgress,
            "mouse effects and playback must share the preview canvas");
    state->publish(false);
    pumpPreview();
    require(render() == referenceProgress,
            "expiring mouse effects must retain the same fixed-pixel progress track");
}

void effectsPreviewLifecycle() {
    for (const qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
        const QRect selected(-2001, -1103, 641, 479);
        const QRect capture = selected.adjusted(-1, -1, 0, 0);
        const QRectF local(3.25, 3.75, 641 / dpr, 479 / dpr);
        const QPoint canvas = local.toAlignedRect().topLeft();
        for (const QSize output : {QSize(642, 480), QSize(428, 320)}) {
            const auto transform =
                recordingEffectsOutputTransform(capture, selected, local, canvas, dpr, output);
            // The selected first pixel follows a one-physical-pixel encoder expansion.
            const QPointF selectedOrigin(output.width() / 642.0, output.height() / 480.0);
            require(
                QLineF(transform.map(selectedOrigin), QPointF(.25, .75)).length() < 0.00001,
                "negative coordinates, encoder padding and fractional canvas insets must align");
            const QPointF selectedEnd(output.width(), output.height());
            require(QLineF(transform.map(selectedEnd), QPointF(.25 + 641 / dpr, .75 + 479 / dpr))
                            .length() < 0.00001,
                    "export scaling must map precisely to the selection at every DPI");
            for (const QSize captureSize : {capture.size(), QSize(1920, 1080), QSize(3840, 2160)}) {
                const auto keyboardTransform =
                    recordingEffectsOutputTransform(QRect(capture.topLeft(), captureSize), selected,
                                                    local, canvas, dpr, captureSize);
                const QSizeF keycap = keyboardTransform.mapRect(QRectF(0, 0, 64, 64)).size() * dpr;
                require(
                    qAbs(keycap.width() - 64) < 0.00001 && qAbs(keycap.height() - 64) < 0.00001,
                    "keyboard preview must stay 64 physical pixels at every capture size and DPI");
            }
        }
    }
    auto state = std::make_shared<RecordingEffectTestState>();
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(QRect(40, 40, 640, 480));
    RecordingEffectPreview preview(area, std::make_unique<RecordingEffectTestSource>(state));
    preview.configure(area.recordingRegion(), QSize(640, 480), QColor(255, 0, 0, 128),
                      Qt::transparent, false);
    preview.setEligible(true);
    require(!state->active, "hidden window must not observe input");
    area.show();
    pumpPreview();
    auto* label = area.findChild<QLabel*>(QStringLiteral("screenRecordingMotionPreviewLabel"));
    require(state->active && preview.hasFrame() && label && label->isVisible(),
            "visible idle preview must show effects and readout");
    auto* readout = qobject_cast<CanvasStatusReadout*>(label);
    require(readout && readout->message() == CanvasStatusReadout::Message::MotionPreview &&
                !readout->dismissalTimer()->isActive(),
            "recording motion status must use the shared persistent hint component");
    require(label->text() == QStringLiteral("Motion Preview in Progress"),
            "preview label must use requested text");
    require(label->testAttribute(Qt::WA_TransparentForMouseEvents),
            "readout must not intercept drawing");
    for (const auto mode : {ScreenRecordingAreaWindow::InputMode::PassThrough,
                            ScreenRecordingAreaWindow::InputMode::Drawing,
                            ScreenRecordingAreaWindow::InputMode::RegionEditing}) {
        area.setInputMode(mode);
        pumpPreview();
        require(state->active, "all idle input modes must preview");
        // Region controls own hit testing; only drawing covers the annotation interior.
        const int backgroundAlpha = mode == ScreenRecordingAreaWindow::InputMode::Drawing ? 2 : 0;
        require(previewImage(*area.canvas()).pixelColor(100, 100).alpha() == backgroundAlpha,
                "preview clearing must preserve the input surface in editing and drawing modes");
        state->publish(false);
        pumpPreview();
        require(previewImage(*area.canvas()).pixelColor(28, 28).alpha() == backgroundAlpha,
                "expired effects must restore hit-test coverage without leaving effect pixels");
        preview.setEligible(false);
        require(previewImage(*area.canvas()).pixelColor(28, 28).alpha() == backgroundAlpha,
                "stopped preview must retain the input surface in interactive modes");
        preview.setEligible(true);
        pumpPreview();
        if (mode == ScreenRecordingAreaWindow::InputMode::RegionEditing)
            requireRecordingRegionHandle(area);
    }
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    // Exercise both capture-coordinate layers through the renderer at different export sizes.
    for (const QSize captureSize : {QSize(640, 480), QSize(960, 720)}) {
        area.setRecordingRegion(QRect(QPoint(40, 40), captureSize));
        for (const QSize exportSize : {QSize(320, 240), QSize(1920, 1080)}) {
            preview.configure(area.recordingRegion(), exportSize, Qt::red, Qt::transparent, true);
            pumpPreview();
            state->publish(false);
            QImage keycap(64, 64, QImage::Format_RGBA8888_Premultiplied);
            keycap.fill(Qt::blue);
            state->frame->tiles.push_back({QRect(128, 128, 64, 64), keycap, captureSize});
            QImage mouse(82, 82, QImage::Format_RGBA8888_Premultiplied);
            mouse.fill(Qt::red);
            state->frame->tiles.push_back({QRect(240, 128, 82, 82), mouse, {}});
            state->notify();
            pumpPreview();
            const QImage rendered = previewImage(*area.canvas());
            QRect pixels;
            QRect mousePixels;
            for (int y = 0; y < rendered.height(); ++y) {
                for (int x = 0; x < rendered.width(); ++x) {
                    if (rendered.pixelColor(x, y).blue() > 128) {
                        pixels |= QRect(x, y, 1, 1);
                    }
                    if (rendered.pixelColor(x, y).red() > 128) {
                        mousePixels |= QRect(x, y, 1, 1);
                    }
                }
            }
            require(pixels.size() == QSize(64, 64),
                    "keyboard tiles must not grow with capture area or export scale");
            require(mousePixels.size() == QSize(82, 82),
                    "mouse tiles must not grow with capture area or export scale");
        }
    }
    area.setRecordingRegion(QRect(40, 40, 640, 480));
    preview.configure(area.recordingRegion(), QSize(640, 480), Qt::red, Qt::transparent, false);
    pumpPreview();
    const auto history = area.canvas()->canvasHistoryState();
    const QImage visible = previewImage(*area.canvas());
    require(visible.pixelColor(28, 28).alpha() > 0, "preview tiles must appear on canvas");
    state->publish(false);
    pumpPreview();
    require(previewImage(*area.canvas()).pixelColor(28, 28).alpha() == 0,
            "final empty snapshot must remove expired pixels");
    require(area.canvas()->canvasHistoryState().canUndo == history.canUndo,
            "preview must not change undo history");
    state->publish();
    const auto stale = state->frame;
    preview.configure(area.recordingRegion(), QSize(640, 480), Qt::transparent, Qt::transparent,
                      false);
    require(!state->active && !preview.hasFrame() && !label->isVisible(),
            "disabling all effects must clear synchronously");
    pumpPreview();
    preview.configure(area.recordingRegion(), QSize(640, 480), Qt::red, Qt::transparent, true);
    pumpPreview();
    require(state->active, "enabling an effect must restart preview");
    state->frame = stale;
    state->notify();
    pumpPreview();
    require(preview.generation() != stale->generation,
            "configuration must invalidate stale generations");
    QDialog modal;
    modal.setModal(true);
    modal.show();
    pumpPreview();
    require(!state->active && !label->isVisible(),
            "modal operations must suspend input observation");
    modal.hide();
    pumpPreview();
    require(state->active, "preview must return when the modal operation ends");
    PreviewTestTranslator translator;
    qApp->installTranslator(&translator);
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&area, &languageChange);
    pumpPreview();
    require(label->accessibleName() == QStringLiteral("Preview translated"),
            "visible readout must retranslate");
    qApp->removeTranslator(&translator);
    QCoreApplication::sendEvent(&area, &languageChange);
    pumpPreview();
    CanvasStatusReadout narrow(&area);
    const QString complete = QStringLiteral("Motion Preview in Progress");
    narrow.setMessage(CanvasStatusReadout::Message::MotionPreview);
    narrow.layoutIn(QRect(0, 0, 80, 28));
    require(narrow.width() <= 64 && narrow.toolTip() == complete &&
                narrow.accessibleName() == complete,
            "narrow readout must elide without losing accessible copy");
    const auto generation = preview.generation();
    preview.setEligible(false);
    preview.stopAndClear(true);
    require(!state->active && preview.generation() > generation && !preview.hasFrame() &&
                !label->isVisible(),
            "startup barrier must stop and clear preview synchronously");
    pumpPreview();
    require(!preview.hasFrame(), "queued notifications must not resurrect stopped effects");
    preview.setEligible(true);
    pumpPreview();
    area.hide();
    require(!state->active && !preview.hasFrame(), "hiding must stop observation immediately");
    int failures = 0;
    preview.reportError = [&](const QString&) { ++failures; };
    state->fail = true;
    area.show();
    pumpPreview();
    require(failures == 1 && !state->active && !preview.hasFrame() && !label->isVisible(),
            "failed initialization must clear the source and report one nonmodal error");
    pumpPreview();
    require(failures == 1, "failed activation must not retry or report repeatedly");
    preview.setEligible(false);
    state->fail = false;
    preview.setEligible(true);
    pumpPreview();
    require(state->active, "a fresh activation must recover after preview failure");
    const int startsBeforeTermination = state->starts;
    const int stopsBeforeTermination = state->stops;
    preview.prepareForSessionEnd();
    state->frame->error = QStringLiteral("late native failure");
    state->notify();
    preview.setEligible(false);
    preview.configure(area.recordingRegion(), QSize(320, 240), Qt::transparent, Qt::transparent,
                      false);
    area.hide();
    area.show();
    pumpPreview();
    require(state->active && state->starts == startsBeforeTermination &&
                state->stops == stopsBeforeTermination && failures == 1,
            "terminal preview retains native ownership without joins, restarts, or late UI errors");
}

void recordingKeyboardFontFollowsApplication() {
    const QFont original = QApplication::font();
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(QRect(32, 32, 320, 240));
    auto state = std::make_shared<RecordingEffectTestState>();
    RecordingEffectPreview preview(area, std::make_unique<RecordingEffectTestSource>(state));
    preview.configure(area.recordingRegion(), QSize(320, 240), Qt::transparent, Qt::transparent,
                      true);
    preview.setEligible(true);
    area.show();
    pumpPreview();
    require(state->keyboardFontFamily == QFontInfo(original).family().toUtf8() &&
                state->keyboardFontWeight == static_cast<uint32_t>(original.weight()) &&
                state->keyboardCjkFontFamily == QByteArray("Snow Recording Test Han"),
            "preview must receive the application's resolved UI font and Chinese fallback");
    const quint64 generation = state->generation;
    QFont changed = original;
    changed.setFamily(QStringLiteral("Snow Recording Test Mono"));
    changed.setWeight(QFont::Bold);
    QApplication::setFont(changed);
    pumpPreview();
    require(state->generation > generation &&
                state->keyboardFontFamily == QFontInfo(changed).family().toUtf8() &&
                state->keyboardCjkFontFamily == QByteArray("Snow Recording Test Han") &&
                state->keyboardFontWeight == static_cast<uint32_t>(QFont::Bold),
            "an open preview must follow application font changes");
    QApplication::setFont(original);
    pumpPreview();
}

// The window clears itself fully before painting the border, so a zero-alpha
// input surface needs no second fill. Skipping it must not change the pixels
// the compositor sees in any input mode.
void areaWindowPaintsInputSurfaceOnlyWhenItIsVisible() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(QRect(32, 32, 320, 240));
    area.show();
    QCoreApplication::processEvents();
    for (const auto mode : {ScreenRecordingAreaWindow::InputMode::PassThrough,
                            ScreenRecordingAreaWindow::InputMode::Drawing,
                            ScreenRecordingAreaWindow::InputMode::RegionEditing}) {
        area.setInputMode(mode);
        area.repaint();
        QCoreApplication::processEvents();
        const int expectedAlpha = area.inputSurfaceColor().alpha();
        const QRect selection = area.selectionRect().toAlignedRect();
        const QImage painted = area.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        require(!painted.isNull() && selection.isValid(), "the area window must paint a selection");
        require(painted.pixelColor(selection.center()).alpha() == expectedAlpha,
                "the painted selection must match the input surface colour in every mode");
    }
}

// The preview filters the whole application, so it must not cast on every event.
void previewIgnoresEventsOtherThanDialogVisibility() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(QRect(32, 32, 320, 240));
    auto state = std::make_shared<RecordingEffectTestState>();
    RecordingEffectPreview preview(area, std::make_unique<RecordingEffectTestSource>(state));
    preview.configure(area.recordingRegion(), QSize(320, 240), Qt::red, Qt::transparent, false);
    preview.setEligible(true);
    area.show();
    pumpPreview();
    require(state->active, "the preview must be running before the filter is exercised");
    const int startsBefore = state->starts;
    QWidget unrelated;
    for (const auto type :
         {QEvent::Enter, QEvent::Leave, QEvent::FocusIn, QEvent::WindowActivate}) {
        QEvent event(type);
        QCoreApplication::sendEvent(&unrelated, &event);
    }
    pumpPreview();
    require(state->active && state->starts == startsBefore,
            "events other than dialog show and hide must not reconfigure the preview");
}

void recordingKeyboardColorsFollowBackground() {
    const RecordingKeyboardTheme defaults;
    require(defaults.background == QColor(0, 0, 0, 204) && defaults.text == QColor(Qt::white),
            "keyboard defaults must be 80% black with white text");
    for (const auto& pair : {std::pair{QColor(0, 0, 0, 204), QColor(64, 64, 64, 204)},
                             std::pair{QColor(255, 255, 255), QColor(191, 191, 191)},
                             std::pair{QColor(40, 80, 120, 128), QColor(94, 124, 154, 128)},
                             std::pair{QColor(0, 0, 0, 0), QColor(64, 64, 64, 0)}}) {
        require(RecordingKeyboardTheme(pair.first, Qt::red).border == pair.second &&
                    RecordingKeyboardTheme(pair.first, Qt::green).border == pair.second,
                "border must blend background toward contrast and preserve its alpha");
    }
}

void recordingStatusPrompts() {
    using snow_shot::storage::RecordingSettings;
    const RecordingSettings settings;
    const QString previousFormat = settings.outputFormat();
    const bool previousMicrophone = settings.microphoneEnabled();
    const bool previousSpeaker = settings.systemAudioEnabled();
    const int previousDelay = settings.startDelaySeconds();
    require(settings.showStatusPrompts(), "status prompts are enabled by default");
    require(settings.setOutputFormat(QStringLiteral("mp4")) &&
                settings.setMicrophoneEnabled(false) && settings.setSystemAudioEnabled(false) &&
                settings.setStartDelaySeconds(0),
            "initialize recording prompt fixture");
    ScreenRecordingController controller(testEffectsSource);
    controller.open({40, 40, 640, 480});
    auto* area = recordingAreaWindow();
    auto* prompt =
        area ? area->findChild<QWidget*>(QStringLiteral("screenRecordingStatusPrompt")) : nullptr;
    require(prompt != nullptr, "recording area must own the status prompt");
    QElapsedTimer ready;
    ready.start();
    while (!prompt->isVisible() && ready.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(prompt->isVisible() && !effectSources.back().lock()->active,
            "status prompts must appear even when all motion effects are disabled");
    const auto checkCentered = [&] {
        require(area->selectionRect().contains(prompt->geometry()) &&
                    QLineF(QRectF(prompt->geometry()).center(), area->selectionRect().center())
                            .length() <= 1,
                "status prompts must remain centered inside the recording selection");
    };
    checkCentered();
    require(prompt->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                prompt->focusPolicy() == Qt::NoFocus,
            "status prompts must not intercept drawing, region edits or keyboard input");
    const auto renderPrompt = [&] {
        QImage image(prompt->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        prompt->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
        return image;
    };
    const QImage disabled = renderPrompt();
    require(disabled.pixelColor(8, disabled.height() / 2) == QColor(0, 0, 0, 150),
            "status prompt background must be semi-transparent black");
    int whitePixels = 0;
    for (int y = 0; y < disabled.height(); ++y)
        for (int x = 0; x < disabled.width(); ++x) {
            const QColor color = disabled.pixelColor(x, y);
            whitePixels += color.red() > 240 && color.green() > 240 && color.blue() > 240;
        }
    require(whitePixels > 20, "format text must remain white beside colored status icons");
    const auto checkStatusColors = [&](bool success, bool danger) {
        const auto colors =
            snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme().map;
        const QImage image = renderPrompt();
        const auto countColor = [&](const QColor& expected) {
            int pixels = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const QColor actual = image.pixelColor(x, y);
                    pixels += qAbs(actual.red() - expected.red()) < 8 &&
                              qAbs(actual.green() - expected.green()) < 8 &&
                              qAbs(actual.blue() - expected.blue()) < 8;
                }
            return pixels;
        };
        require((countColor(colors.colorSuccess) > 5) == success &&
                    (countColor(colors.colorError) > 5) == danger,
                "audio icons must use success for enabled and danger for disabled status");
    };
    checkStatusColors(false, true);
    palette()->recordingMicrophoneToggled(true);
    pumpPreview();
    const QImage microphone = renderPrompt();
    require(prompt->accessibleName() ==
                    QStringLiteral("Format: MP4; Microphone enabled; Speaker disabled") &&
                microphone != disabled,
            "microphone toggles must immediately update the prompt text alternative and icon");
    checkStatusColors(true, true);
    palette()->recordingSystemAudioToggled(true);
    pumpPreview();
    require(prompt->accessibleName() ==
                    QStringLiteral("Format: MP4; Microphone enabled; Speaker enabled") &&
                renderPrompt() != microphone,
            "speaker toggles must immediately update the prompt text alternative and icon");
    checkStatusColors(true, false);
    palette()->recordingMicrophoneToggled(false);
    pumpPreview();
    checkStatusColors(true, true);
    using namespace snow_shot::presentation::styles;
    const auto previousTheme = ThemeManager::instance().themeMode();
    for (auto theme : {ThemeMode::Light, ThemeMode::Dark}) {
        ThemeManager::instance().setThemeMode(theme);
        pumpPreview();
        checkStatusColors(true, true);
    }
    ThemeManager::instance().setThemeMode(previousTheme);
    palette()->recordingMicrophoneToggled(true);
    for (const auto& format : {QStringLiteral("gif"), QStringLiteral("webp")}) {
        palette()->recordingOutputFormatChanged(format);
        pumpPreview();
        require(prompt->accessibleName() ==
                    QStringLiteral("Format: %1; Microphone disabled; Speaker disabled")
                        .arg(format == QStringLiteral("gif") ? QStringLiteral("GIF")
                                                             : QStringLiteral("WebP")),
                "animated formats must show concise names and disabled audio icons");
        checkStatusColors(false, true);
        checkCentered();
    }
    palette()->recordingOutputFormatChanged(QStringLiteral("mp4"));
    pumpPreview();
    require(prompt->accessibleName() ==
                QStringLiteral("Format: MP4; Microphone enabled; Speaker enabled"),
            "switching back to video must restore the selected audio status");
    require(settings.setShowStatusPrompts(false), "disable status prompts in storage");
    pumpPreview();
    require(!prompt->isVisible(), "disabling prompts must hide an existing idle prompt");
    require(settings.setShowStatusPrompts(true), "restore status prompts");
    pumpPreview();
    require(prompt->isVisible(), "enabling prompts must restore an existing idle prompt");
    area->setRecordingRegion({70, 70, 480, 320});
    pumpPreview();
    checkCentered();
    QFont larger = prompt->font();
    larger.setPixelSize(24);
    prompt->setFont(larger);
    checkCentered();
    class PromptTranslator final : public QTranslator {
      public:
        bool isEmpty() const override {
            return false;
        }
        QString translate(const char* context, const char* source, const char*,
                          int) const override {
            return QByteArray(context) == "RecordingStatusPrompt"
                       ? QStringLiteral("Translated: ") + QString::fromUtf8(source)
                       : QString();
        }
    } translator;
    require(qApp->installTranslator(&translator), "install prompt translation fixture");
    pumpPreview();
    require(prompt->accessibleName().startsWith(QStringLiteral("Translated: Format:")),
            "visible prompts must retranslate on language change");
    checkCentered();
    qApp->removeTranslator(&translator);
    prompt->setFont(area->font());
    pumpPreview();
    if (const QString path = qEnvironmentVariable("SNOW_TEST_RECORDING_STATUS_PREVIEW");
        !path.isEmpty()) {
        QImage snapshot(area->size(), QImage::Format_ARGB32_Premultiplied);
        snapshot.fill(QColor(233, 237, 242));
        {
            QPainter painter(&snapshot);
            painter.drawImage(QPoint(), previewImage(*area));
        }
        require(snapshot.save(path), "save recording status prompt preview");
    }
    controller.startRecording();
    require(!prompt->isVisible(), "prompts must clear synchronously before recording begins");
    waitForRecording(controller);
    palette()->recordingPauseRequested();
    pumpPreview();
    require(!prompt->isVisible(), "paused recordings must keep prompts hidden");
    palette()->recordingResumeRequested();
    pumpPreview();
    require(!prompt->isVisible(), "resumed recordings must keep prompts hidden");
    palette()->recordingStopRequested();
    waitForIdle(controller);
    pumpPreview();
    require(prompt->isVisible(), "returning to idle must restore status prompts");
    palette()->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(settings.setOutputFormat(previousFormat) &&
                settings.setMicrophoneEnabled(previousMicrophone) &&
                settings.setSystemAudioEnabled(previousSpeaker) &&
                settings.setStartDelaySeconds(previousDelay),
            "restore recording prompt fixture preferences");
}

void controllerPreviewTransitions() {
    using snow_shot::storage::RecordingSettings;
    RecordingSettings().setMouseTrailColor(Qt::red);
    RecordingSettings().setShowKeyboard(true);
    require(RecordingSettings().keyboardSize() == 64 &&
                RecordingSettings().mouseTrailDurationMs() == 500 &&
                RecordingSettings().keyboardBackgroundColor() == QColor(0, 0, 0, 204) &&
                RecordingSettings().keyboardForegroundColor() == QColor(Qt::white),
            "new effect settings must have stable defaults");
    require(!RecordingSettings().setKeyboardSize(31) && !RecordingSettings().setKeyboardSize(129) &&
                !RecordingSettings().setMouseTrailDurationMs(99) &&
                !RecordingSettings().setMouseTrailDurationMs(2001),
            "duration settings must reject out-of-range values");
    ScreenRecordingController controller(testEffectsSource);
    controller.open({40, 40, 320, 240});
    pumpPreview();
    auto state = effectSources.back().lock();
    QElapsedTimer sizing;
    sizing.start();
    while (state && !state->active && sizing.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    require(state && state->active,
            "idle controller must enable preview after asynchronous sizing");
    require(state->keyboardSize == 64 && state->trailDurationMs == 500 &&
                state->keyboardBackground == 0x000000cc &&
                state->keyboardForeground == 0xffffffff && state->keyboardBorder == 0x404040cc,
            "preview must receive default effect settings");
    palette()->recordingKeyboardSizeChanged(96);
    palette()->recordingMouseTrailDurationMsChanged(2000);
    palette()->recordingKeyboardBackgroundColorChanged(QColor(40, 80, 120, 128));
    palette()->recordingKeyboardForegroundColorChanged(QColor(240, 230, 220, 200));
    pumpPreview();
    require(state->keyboardSize == 96 && state->trailDurationMs == 2000 &&
                state->keyboardBackground == 0x28507880 &&
                state->keyboardForeground == 0xf0e6dcc8 && state->keyboardBorder == 0x5e7c9a80,
            "controller edits must immediately configure preview colors and duration");
    require(RecordingSettings().keyboardSize() == 96 &&
                RecordingSettings().mouseTrailDurationMs() == 2000 &&
                RecordingSettings().keyboardBackgroundColor() == QColor(40, 80, 120, 128) &&
                RecordingSettings().keyboardForegroundColor() == QColor(240, 230, 220, 200),
            "controller edits must persist with color alpha");
    palette()->recordingMouseHighlightEnabledChanged(true);
    palette()->recordingMouseHighlightColorChanged(QColor(255, 255, 0, 128));
    require(state->highlightRadius == 41, "highlight preview retains its original default radius");
    palette()->recordingMouseHighlightRadiusChanged(96);
    palette()->recordingRecordMouseClicksChanged(true);
    palette()->recordingKeyboardVisibleChanged(false);
    palette()->recordingCursorVisibleChanged(true);
    pumpPreview();
    require(state->highlight == 0xffff0080 && state->highlightRadius == 96 &&
                RecordingSettings().mouseHighlightRadius() == 96 && state->recordMouseClicks &&
                !state->showKeyboard,
            "highlight and click-only recording must reach preview independently");
    palette()->recordingCursorVisibleChanged(false);
    pumpPreview();
    require(
        state->highlight == 0 && state->recordMouseClicks &&
            RecordingSettings().mouseHighlightEnabled(),
        "hiding cursor suppresses highlight without forgetting preference or hiding click keycaps");
    palette()->recordingMouseHighlightEnabledChanged(false);
    palette()->recordingRecordMouseClicksChanged(false);
    palette()->recordingRecordMouseScrollingChanged(true);
    pumpPreview();
    require(state->recordMouseScrolling && !state->recordMouseClicks && !state->showKeyboard &&
                RecordingSettings().recordMouseScrolling(),
            "scroll-only preview and persistence are independent");
    palette()->recordingRecordMouseScrollingChanged(false);
    palette()->recordingCursorVisibleChanged(true);
    pumpPreview();
    ScreenRecordingAreaWindow* area = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget);
            candidate && candidate->isVisible()) {
            area = candidate;
            break;
        }
    }
    auto* exportButton = palette()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenRecordingExportSettings"));
    require(area && exportButton, "controller must expose the recording area and export settings");
    if (!palette()->recordingExportSettingsVisible()) {
        exportButton->click();
    }
    pumpPreview();
    require(area->inputMode() == ScreenRecordingAreaWindow::InputMode::RegionEditing &&
                !area->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                previewImage(*area->canvas()).pixelColor(100, 100).alpha() == 0,
            "idle export settings must keep the annotation interior transparent");
    requireRecordingRegionHandle(*area);
    require(palette()->activateDrawingShortcut(QStringLiteral("shape")),
            "a drawing tool must replace export settings");
    pumpPreview();
    require(area->inputMode() == ScreenRecordingAreaWindow::InputMode::Drawing &&
                !palette()->recordingExportSettingsVisible() &&
                previewImage(*area->canvas()).pixelColor(100, 100).alpha() == 2,
            "switching from export settings to drawing must retain hit-test coverage");
    exportButton->click();
    pumpPreview();
    ErrorObserver errors;
    qApp->installEventFilter(&errors);
    failStart = true;
    controller.startRecording();
    {
        // The failed start is reported through the asynchronous start path.
        QElapsedTimer failureWait;
        failureWait.start();
        while (errors.shown == 0 && failureWait.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents,
                                            100);
        }
    }
    pumpPreview();
    require(!controller.isRecording() && state->active && errors.shown == 1,
            "startup failure must restore a fresh preview after dismissing its modal error");
    failStart = false;
    qApp->removeEventFilter(&errors);
    controller.startRecording();
    require(!state->active, "accepted start must stop preview before its queued callback");
    waitForRecording(controller);
    pumpPreview();
    require(controller.isRecording() && !state->active, "recording must keep preview stopped");
    auto* previewLabel =
        area->findChild<QLabel*>(QStringLiteral("screenRecordingMotionPreviewLabel"));
    require(previewLabel == nullptr || !previewLabel->isVisible(),
            "native capture must never see the preview label");
    require(previewImage(*area->canvas()).pixelColor(100, 100).alpha() == 0,
            "recording with export settings selected must not retain the idle input surface");
    palette()->recordingPauseRequested();
    require(!state->active, "paused session must keep preview stopped");
    palette()->recordingResumeRequested();
    require(!state->active, "resume must not restore preview");
    palette()->recordingStopRequested();
    waitForIdle(controller);
    pumpPreview();
    require(state->active, "completed finalization must restore fresh preview");
    require(area->inputMode() == ScreenRecordingAreaWindow::InputMode::RegionEditing &&
                previewImage(*area->canvas()).pixelColor(100, 100).alpha() == 0,
            "returning to idle export settings must restore the transparent annotation interior");
    requireRecordingRegionHandle(*area);
    palette()->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(!state->active, "closing must join the preview source");
    RecordingSettings().setMouseTrailColor(Qt::transparent);
    RecordingSettings().setShowKeyboard(false);
    RecordingSettings().setMouseTrailDurationMs(500);
    RecordingSettings().setKeyboardBackgroundColor(QColor(0, 0, 0, 204));
    RecordingSettings().setKeyboardForegroundColor(Qt::white);
}

std::vector<uint32_t> prepareExpectedRecordingExclusions(ScreenshotToolPalette* toolbarPalette,
                                                         bool captureToolbar) {
    std::vector<uint32_t> expected;
    if (captureToolbar)
        return expected;
    if (auto id = snow_shot::platform::captureWindowId(toolbarPalette->window()))
        expected.push_back(*id);
    auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(toolbarPalette->window());
    require(toolbar != nullptr, "recording exclusion fixture needs the recording toolbar");
    if (auto id = snow_shot::platform::captureWindowId(toolbar->autoHideControlWindow()))
        expected.push_back(*id);
    return expected;
}

void recordingApiModeWiring() {
    using snow_shot::storage::RecordingSettings;
    const RecordingSettings settings;
    require(settings.apiMode() == QStringLiteral("dxgi"), "recording must prefer DXGI by default");
    require(settings.setStartDelaySeconds(0), "disable countdown");
    for (bool deferred : {false, true}) {
        require(settings.setPostProcessingEnabled(deferred), "set recording pipeline");
        for (const auto& mode :
             {QStringLiteral("dxgi"), QStringLiteral("wgc"), QStringLiteral("gdi")}) {
            ScreenRecordingController controller(testEffectsSource);
            controller.open({40, 40, 320, 240});
            // Change after opening to verify each start reads the latest preference.
            require(settings.setApiMode(mode), "set recording API preference");
            const int beforeDeferred = deferredCreates;
            controller.startRecording();
            waitForRecording(controller);
#ifdef Q_OS_MACOS
            const uint32_t expected = SNOW_CAPTURE_BACKEND_AUTO;
#else
            const uint32_t expected = mode == QStringLiteral("wgc")   ? SNOW_CAPTURE_BACKEND_WGC
                                      : mode == QStringLiteral("gdi") ? SNOW_CAPTURE_BACKEND_GDI
                                                                      : SNOW_CAPTURE_BACKEND_DXGI;
#endif
            require(lastDirectConfig.capture_backend == expected,
                    "each recording pipeline must receive the selected API preference");
            require(deferredCreates == beforeDeferred + (deferred ? 1 : 0),
                    "test must exercise both direct and deferred recording");
            const int beforeRenderPolls = renderPolls;
            palette()->recordingStopRequested();
            if (deferred) {
                QElapsedTimer deadline;
                deadline.start();
                while (renderPolls == beforeRenderPolls && deadline.elapsed() < 3000) {
                    QCoreApplication::processEvents(
                        QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 100);
                }
                require(renderPolls > beforeRenderPolls,
                        "deferred finalization must reach rendering");
                renderState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
            }
            waitForIdle(controller);
            palette()->recordingCloseRequested();
        }
    }
    require(settings.setApiMode(QStringLiteral("dxgi")) && settings.setPostProcessingEnabled(false),
            "restore recording defaults");
}

void recordingStartupFailureAllowsRetry() {
    using snow_shot::storage::RecordingSettings;
    const RecordingSettings settings;
    require(settings.setStartDelaySeconds(0), "disable countdown");
    for (bool deferred : {false, true}) {
        require(settings.setPostProcessingEnabled(deferred), "select recording pipeline");
        for (int failure : {1, 2}) {
            require(settings.setApiMode(QStringLiteral("wgc")), "set initial recording preference");
            ErrorObserver observer;
            qApp->installEventFilter(&observer);
            ScreenRecordingController controller(testEffectsSource);
            controller.open(QRect(40, 40, 320, 240));
            auto* startButton = palette()->findChild<adqt::widgets::AdButton*>(
                QStringLiteral("screenRecordingStart"));
            require(startButton && startButton->isEnabled(), "Start is initially enabled");
            failStart = failure == 1;
            failStartOperation = failure == 2;
            startButton->click();
            QElapsedTimer deadline;
            deadline.start();
            while (!observer.shown && deadline.elapsed() < 3000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QThread::msleep(5);
            }
            require(observer.shown == 1, "failed startup reports exactly one error");
            require(!controller.isRecording() && !palette()->recordingBusy() &&
                        startButton->isEnabled(),
                    "failed startup restores an enabled Start");
            failStart = false;
            failStartOperation = false;
            require(settings.setApiMode(QStringLiteral("gdi")), "change preference before retry");
            startButton->click();
            waitForRecording(controller);
#ifdef Q_OS_MACOS
            const uint32_t expected = SNOW_CAPTURE_BACKEND_AUTO;
#else
            const uint32_t expected = SNOW_CAPTURE_BACKEND_GDI;
#endif
            require(lastDirectConfig.capture_backend == expected,
                    "retry must use the current backend preference");
            const int previousRenderPolls = renderPolls;
            palette()->recordingStopRequested();
            if (deferred) {
                deadline.restart();
                while (renderPolls == previousRenderPolls && deadline.elapsed() < 3000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                    QThread::msleep(5);
                }
                require(renderPolls > previousRenderPolls, "deferred export begins rendering");
                renderState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
            }
            waitForIdle(controller);
            palette()->recordingCloseRequested();
            qApp->removeEventFilter(&observer);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }
    require(settings.setApiMode(QStringLiteral("dxgi")) && settings.setPostProcessingEnabled(false),
            "restore recording defaults");
}

void recordingCaptureExclusionWiring() {
    using snow_shot::storage::RecordingSettings;
    const RecordingSettings settings;
    const bool savedAutoHide = settings.autoHideToolbar();
    const auto restoreAutoHide = qScopeGuard([&] {
        require(settings.setAutoHideToolbar(savedAutoHide),
                "restore capture-exclusion auto-hide preference");
    });
    require(settings.setAutoHideToolbar(false), "disable auto-hide before exclusion fixtures");
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    require(RecordingSettings().setStartDelaySeconds(0), "disable countdown");
    for (bool captureToolbar : {false, true}) {
        require(RecordingSettings().setCaptureToolbarInRecording(captureToolbar),
                "set toolbar capture preference");
        for (int failure : {0, 1, 2}) {
            const bool fail = failure != 0;
            ErrorObserver observer;
            qApp->installEventFilter(&observer);
            ScreenRecordingController controller(testEffectsSource);
            controller.open({40, 40, 320, 240});
            auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window());
            require(toolbar != nullptr, "recording capture has a toolbar window");
            QWidget* edgeControl = toolbar->autoHideControlWindow();
            require(edgeControl != nullptr && edgeControl->parentWidget() == nullptr &&
                        !edgeControl->isVisible() && edgeControl->windowHandle() != nullptr,
                    "edge control owns a retained native surface before recording startup");
            const auto expected = prepareExpectedRecordingExclusions(palette(), captureToolbar);
#ifdef Q_OS_MACOS
            const auto sharingMatches = macosCaptureSharingProbe(toolbar);
            const auto edgeSharingMatches = macosCaptureSharingProbe(edgeControl);
#endif
            failStart = failure == 1;
            failStartOperation = failure == 2;
            const bool holdCollapsedStart = !captureToolbar && !fail;
            std::promise<void> releaseStart;
            if (holdCollapsedStart) {
                require(settings.setAutoHideToolbar(true), "enable the collapsed-start fixture");
                const QRect monitor = toolbar->screen()->geometry();
                const QRect row = toolbar->paletteHost()->mainToolbarContentRect();
                toolbar->moveContentTo(
                    QPoint(monitor.left() - row.left(), toolbar->contentPosition().y()));
                setRecordingToolbarAutoHideTestCursor(*toolbar,
                                                      monitor.bottomRight() + QPoint(100, 100));
                auto* retreat =
                    toolbar->findChild<QTimer*>(QStringLiteral("recordingToolbarRetreatTimer"));
                require(retreat != nullptr && retreat->isActive(),
                        "collapsed-start fixture schedules retreat at the monitor edge");
                retreat->stop();
                require(QMetaObject::invokeMethod(retreat, "timeout", Qt::DirectConnection) &&
                            !toolbar->isVisible() && edgeControl->isVisible(),
                        "recording can start while its toolbar is already collapsed");
                sessionCreateEntered = false;
                sessionCreateGate = releaseStart.get_future().share();
            }
            controller.startRecording();
            if (holdCollapsedStart) {
                QElapsedTimer deadline;
                deadline.start();
                while (!sessionCreateEntered && deadline.elapsed() < 3000)
                    QCoreApplication::processEvents(
                        QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 25);
                require(sessionCreateEntered && toolbar->isVisible() && !edgeControl->isVisible(),
                        "capture policy transition reveals a collapsed toolbar before native "
                        "startup, including when native exclusion is unavailable");
                releaseStart.set_value();
                sessionCreateGate = {};
                require(settings.setAutoHideToolbar(false),
                        "restore auto-hide after the guarded startup transition");
            }
            if (fail) {
                QElapsedTimer deadline;
                deadline.start();
                while (observer.shown == 0 && deadline.elapsed() < 3000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                    QThread::msleep(5);
                }
                require(observer.shown > 0, "failed start is reported after cleanup");
            } else {
                waitForRecording(controller);
            }
            auto* surfaceGuard = toolbar->findChild<adqt::widgets::AdPopupSurfaceGuard*>();
            require((surfaceGuard != nullptr) == (!captureToolbar && !fail),
                    "recording owns one toolbar-wide popup guard only while exclusion is active");
            if (!fail) {
                int requestedSurfaces = 0;
                QMetaObject::Connection requestedConnection;
                if (surfaceGuard) {
                    requestedConnection = QObject::connect(
                        surfaceGuard, &adqt::widgets::AdPopupSurfaceGuard::surfaceRequested,
                        &controller, [&](QWidget*) { ++requestedSurfaces; });
                }
                // A drawing popover is first materialized after recording has started.
                auto* drawingTrigger = palette()->findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotArrowLineButton"));
                auto* drawingPopover = drawingTrigger
                                           ? drawingTrigger->findChild<adqt::widgets::AdPopover*>()
                                           : nullptr;
                require(drawingPopover != nullptr, "recording toolbar has drawing popover");
                drawingPopover->show();
                require(drawingPopover->surfaceWidget() &&
                            requestedSurfaces == (captureToolbar ? 0 : 1),
                        "late drawing popover inherits the recording toolbar capture policy");
                if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
                    require(drawingPopover->surfaceWidget()->isVisible() == captureToolbar,
                            "unavailable native exclusion keeps drawing popup hidden; capture-on "
                            "shows it");
                    if (!captureToolbar) {
                        auto* area = recordingAreaWindow();
                        auto* messages = area->findChild<adqt::widgets::AdMessage*>();
                        auto* handle = messages
                                           ? messages->findChild<adqt::widgets::AdMessageHandle*>()
                                           : nullptr;
                        require(messages && messages->count() == 1 && handle &&
                                    handle->key() ==
                                        QStringLiteral("recording-toolbar-exclusion-error"),
                                "capture exclusion warnings must appear in the recording area");
                        require(toolbar->findChild<adqt::widgets::AdMessage*>() == nullptr,
                                "capture exclusion warnings must not create toolbar messages");
                    }
                }
#ifdef Q_OS_WIN
                if (QGuiApplication::platformName() == QStringLiteral("windows")) {
                    DWORD affinity = WDA_NONE;
                    const auto expectedAffinity =
                        static_cast<DWORD>(captureToolbar ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
                    require(GetWindowDisplayAffinity(
                                reinterpret_cast<HWND>(drawingPopover->surfaceWidget()->winId()),
                                &affinity) != 0 &&
                                affinity == expectedAffinity,
                            "drawing popup native capture affinity follows the toolbar setting");
                    require(drawingPopover->surfaceWidget()->isVisible(),
                            "excluded drawing popup remains usable on the native desktop");
                }
#endif
                drawingPopover->hide();
                QObject::disconnect(requestedConnection);
            }
            require(lastExcludedWindows == expected,
                    "recording creation excludes the toolbar and retained edge control; popup "
                    "identities are handled lazily");
            require(toolbar->isVisible(), "toolbar remains visible on success and failure");
#ifdef Q_OS_MACOS
            require(sharingMatches(!captureToolbar && !fail),
                    "recording start applies sharing policy and failure restores it");
            require(
                edgeSharingMatches(!captureToolbar && !fail),
                "recording start applies and restores the retained edge control sharing policy");
#endif
            if (!fail) {
                palette()->recordingStopRequested();
                waitForIdle(controller);
                require(!toolbar->findChild<adqt::widgets::AdPopupSurfaceGuard*>(),
                        "stopping restores the capture policy for all toolbar popovers");
            }
#ifdef Q_OS_MACOS
            require(sharingMatches(false), "recording stop restores the original sharing policy");
            require(edgeSharingMatches(false),
                    "recording stop restores the edge control's original sharing policy");
#endif
            palette()->recordingCloseRequested();
            failStart = false;
            failStartOperation = false;
            qApp->removeEventFilter(&observer);
        }
    }
#ifdef Q_OS_MACOS
    require(RecordingSettings().setCaptureToolbarInRecording(false), "exclude toolbar");
    std::function<bool(bool)> sharingMatches;
    std::function<bool(bool)> edgeSharingMatches;
    {
        ScreenRecordingController controller(testEffectsSource);
        controller.open({40, 40, 320, 240});
        sharingMatches = macosCaptureSharingProbe(palette()->window());
        edgeSharingMatches = macosCaptureSharingProbe(
            qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window())
                ->autoHideControlWindow());
        controller.startRecording();
        waitForRecording(controller);
        require(sharingMatches(true), "active toolbar is excluded before destruction");
        require(edgeSharingMatches(true), "active edge control is excluded before destruction");
    }
    require(sharingMatches(false), "controller destruction restores native sharing");
    require(edgeSharingMatches(false), "controller destruction restores edge control sharing");
#endif
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

#ifdef Q_OS_MACOS
void destructionDoesNotBlockNativeWorkers() {
    std::promise<void> releaseDimensions;
    dimensionsGate = releaseDimensions.get_future().share();
    dimensionsEntered = false;
    holdDimensions = true;
    const int completed = dimensionsCompleted;
    auto controller = std::make_unique<ScreenRecordingController>(testEffectsSource);
    controller->open(QRect(40, 40, 321, 239));
    QElapsedTimer deadline;
    deadline.start();
    while (!dimensionsEntered && deadline.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(dimensionsEntered, "the sizing worker must reach the controlled native query");
    controller.reset();
    // Releasing only after destruction proves the GUI thread did not join it.
    releaseDimensions.set_value();
    while (dimensionsCompleted == completed && deadline.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(dimensionsCompleted == completed + 1, "retired sizing must complete independently");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    controller = std::make_unique<ScreenRecordingController>(testEffectsSource);
    controller->open(QRect(40, 40, 321, 239));
    controller->startRecording();
    waitForRecording(*controller);
    std::promise<void> releaseExport;
    std::promise<void> enteredPromise;
    auto entered = enteredPromise.get_future();
    exportGate = releaseExport.get_future().share();
    exportEntered = &enteredPromise;
    const int destroyed = destroyedSessions;
    palette()->recordingStopRequested();
    require(entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "finalization must reach the controlled worker");
    controller.reset();
    require(destroyedSessions == destroyed,
            "the live session must outlive its pending finalization");
    releaseExport.set_value();
    exportEntered = nullptr;
    exportGate = {};
    deadline.restart();
    while (destroyedSessions == destroyed && deadline.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(destroyedSessions == destroyed + 1,
            "retired finalization must release the native session exactly once");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void staleRetinaSizingCannotConfigureAnotherRegion() {
    snow_shot::storage::RecordingSettings().setMouseTrailColor(Qt::red);
    std::promise<void> release;
    dimensionsGate = release.get_future().share();
    dimensionsEntered = false;
    holdDimensions = true;
    dimensionsScale = 2;
    ScreenRecordingController controller(testEffectsSource);
    controller.open(QRect(-300, -100, 321, 239));
    QElapsedTimer deadline;
    deadline.start();
    while (!dimensionsEntered && deadline.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(dimensionsEntered, "Retina sizing must be dispatched off the GUI thread");
    auto state = effectSources.back().lock();
    controller.open(QRect(-280, -80, 401, 301));
    require(state && !state->active,
            "changing the region must not reuse unresolved preview dimensions");
    release.set_value();
    deadline.restart();
    while (!state->active && deadline.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(state->active && state->output == QSize(802, 602),
            "only the latest region's native Retina dimensions may configure preview");
    dimensionsScale = 1;
    palette()->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    snow_shot::storage::RecordingSettings().setMouseTrailColor(Qt::transparent);
}
#endif

void permissionsAndExactLogicalRegion() {
    const snow_shot::storage::RecordingSettings settings;
    ScreenRecordingController controller(testEffectsSource);
    QVector<bool> captureActivity;
    QObject::connect(&controller, &ScreenRecordingController::captureActivityChanged, &controller,
                     [&](bool active) { captureActivity.push_back(active); });
    bool granted = false;
    int requests = 0;
    controller.setPermissionCheck([&](bool microphone, bool input, bool notify) {
        require(microphone == (settings.outputFormat() == QStringLiteral("mp4") &&
                               settings.microphoneEnabled()),
                "start permission check must reflect the current microphone and output settings");
        require(input == (settings.showKeyboard() || settings.recordMouseClicks() ||
                          settings.recordMouseScrolling() ||
                          settings.mouseTrailColor().alpha() != 0 ||
                          settings.mouseClickColor().alpha() != 0),
                "only selected input effects require monitoring permission");
        requests += notify ? 1 : 0;
        return granted;
    });
    controller.open(QRect(-231, -119, 321, 239));
    const int initialStarts = starts.load();
    controller.startRecording();
    QCoreApplication::processEvents();
    require(requests == 1 && starts == initialStarts && !controller.isRecording(),
            "denied permissions must keep recording idle without creating a session");
    require(captureActivity == QVector<bool>({true, false}),
            "permission failure releases setup suppression");
    granted = true;
    controller.startRecording();
    waitForRecording(controller);
    require(captureActivity == QVector<bool>({true, false, true}),
            "retry after permission grant reacquires suppression");
#ifdef Q_OS_MACOS
    require(lastDirectConfig.x == -231 && lastDirectConfig.y == -119 &&
                lastDirectConfig.width == 321 && lastDirectConfig.height == 239,
            "macOS must forward odd logical regions without DPI conversion or encoder expansion");
#endif
    controller.stopRecordingAndCopy();
    waitForIdle(controller);
    palette()->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void delayCountdownBlocksTheStartUntilItElapses() {
    using snow_shot::storage::RecordingSettings;
    require(RecordingSettings().startDelaySeconds() == 0,
            "the recording delay must default to zero seconds");
    require(!RecordingSettings().setStartDelaySeconds(-1) &&
                !RecordingSettings().setStartDelaySeconds(11),
            "out-of-range delays must be rejected");
    {
        ScreenRecordingController controller(testEffectsSource);
        QVector<bool> captureActivity;
        QObject::connect(&controller, &ScreenRecordingController::captureActivityChanged,
                         &controller, [&](bool active) { captureActivity.push_back(active); });
        controller.open({40, 40, 320, 240});
        auto* exportButton = palette()->findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenRecordingExportSettings"));
        require(exportButton != nullptr, "export settings control must exist");
        if (!palette()->recordingExportSettingsVisible()) {
            exportButton->click();
        }
        auto* delayButton = palette()->findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenRecordingStartDelaySeconds"));
        require(delayButton != nullptr, "export settings must expose the delay editor");

        const auto spinWheel = [&delayButton](int delta) {
            const QPoint global = delayButton->mapToGlobal(delayButton->rect().center());
            QWheelEvent wheel(QPointF(global), QPointF(global), QPoint(), QPoint(0, delta),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(delayButton, &wheel);
        };
        spinWheel(120);
        require(palette()->recordingStartDelaySeconds() == 1 &&
                    RecordingSettings().startDelaySeconds() == 1,
                "a wheel step over the delay editor must raise and persist the delay");
        palette()->setRecordingStartDelaySeconds(10);
        spinWheel(120);
        require(palette()->recordingStartDelaySeconds() == 10,
                "the delay must clamp at ten seconds");
        spinWheel(-120);
        require(palette()->recordingStartDelaySeconds() == 9, "the delay must step back down");
        delayButton->click();
        require(palette()->recordingStartDelaySeconds() == 0 &&
                    RecordingSettings().startDelaySeconds() == 0,
                "clicking the delay editor must restore the zero default");

        const int startsBefore = starts.load();
        palette()->setRecordingStartDelaySeconds(1);
        palette()->recordingStartDelaySecondsChanged(1);
        auto* startButton = recordingToolbarButton("Start recording");
        auto* closeButton = recordingToolbarButton("Close recording");
        require(startButton != nullptr && closeButton != nullptr,
                "recording start and close controls must exist");
        controller.startRecording();
        QCoreApplication::processEvents();
        require(!controller.isRecording() && starts.load() == startsBefore,
                "a delayed start must wait for the countdown");
        require(captureActivity == QVector<bool>{true},
                "setup suppression remains active throughout the countdown");
        require(palette()->recordingBusyOperation() ==
                    ScreenshotToolPalette::RecordingBusyOperation::CountingDown,
                "the countdown must be published as a busy operation");
        require(!startButton->isEnabled() && startButton->busy(),
                "the countdown must disable Start and show its loading spinner");
        require(closeButton->isEnabled(), "the countdown must stay cancellable through Close");
        ScreenRecordingAreaWindow* area = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget);
                candidate && candidate->isVisible()) {
                area = candidate;
            }
        }
        require(area != nullptr && area->countdownActive(),
                "the recording area must show the countdown indicator");
        palette()->recordingCloseRequested();
        require(!controller.isOpen(), "Close must detach the UI during the countdown");
        require(captureActivity == QVector<bool>({true, false}),
                "countdown cancellation restores floating surfaces");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(starts.load() == startsBefore,
                "cancelling the countdown must not reach the capture backend");
        controller.open({40, 40, 320, 240});
        require(controller.isOpen() && palette()->recordingBusyOperation() ==
                                           ScreenshotToolPalette::RecordingBusyOperation::None,
                "closing a countdown must leave the controller immediately reopenable");
        palette()->recordingCloseRequested();
    }
    {
        const int startsBefore = starts.load();
        ScreenRecordingController controller(testEffectsSource);
        controller.open({40, 40, 320, 240});
        palette()->recordingStartDelaySecondsChanged(1);
        controller.startRecording();
        waitForRecording(controller);
        require(starts.load() == startsBefore + 1,
                "recording must start automatically once the countdown elapses");
        ScreenRecordingAreaWindow* area = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget);
                candidate && candidate->isVisible()) {
                area = candidate;
            }
        }
        require(area != nullptr && !area->countdownActive(),
                "the countdown indicator must disappear once recording starts");
        palette()->recordingStopRequested();
        waitForIdle(controller);
    }
    require(RecordingSettings().setStartDelaySeconds(0), "the delay must reset to the default");
}
#ifdef Q_OS_WIN
int nativeEffectsPreviewCapture() {
    const auto checkNative = [](bool success, const char* message) {
        if (!success) {
            throw std::runtime_error(message);
        }
    };
    const auto waitUntil = [](auto condition) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        return condition();
    };
    struct RestoreInput {
        POINT point{};
        HWND foreground = GetForegroundWindow();
        RestoreInput() {
            GetCursorPos(&point);
        }
        ~RestoreInput() {
            INPUT key{};
            key.type = INPUT_KEYBOARD;
            key.ki.wVk = 'A';
            key.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &key, sizeof(INPUT));
            SetCursorPos(point.x, point.y);
            if (foreground != nullptr) {
                SetForegroundWindow(foreground);
            }
        }
    } restore;
    QScreen* screen = QGuiApplication::primaryScreen();
    const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const QRect region(bounds.topLeft() + QPoint(100, 100), QSize(640, 480));
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(region);
    QWidget background(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    background.setStyleSheet(QStringLiteral("background-color: rgb(38, 48, 63);"));
    background.setGeometry(area.geometry());
    background.show();
    RecordingEffectPreview preview(area);
    preview.configure(region, region.size(), Qt::red, Qt::cyan, true);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    area.show();
    area.raise();
    area.activateWindow();
    area.canvas()->setFocus();
    pumpPreview();
    const QString output = QDir::current().absoluteFilePath(QStringLiteral("effects-native-qa"));
    QDir().mkpath(output);
    for (QScreen* target : QGuiApplication::screens()) {
        const QRect targetBounds = ScreenshotGeometryMapper::physicalRectForScreen(*target);
        const QRect selected(targetBounds.topLeft() + QPoint(50, 50), QSize(641, 479));
        area.setRecordingRegion(selected);
        background.setGeometry(area.geometry());
        preview.configure(selected.adjusted(0, 0, 1, 1), QSize(642, 480), Qt::red, Qt::cyan, true);
        for (const auto mode : {ScreenRecordingAreaWindow::InputMode::PassThrough,
                                ScreenRecordingAreaWindow::InputMode::Drawing,
                                ScreenRecordingAreaWindow::InputMode::RegionEditing}) {
            preview.setEligible(false);
            area.setInputMode(mode);
            area.show();
            area.raise();
            preview.setEligible(true);
            auto* ready =
                area.findChild<QLabel*>(QStringLiteral("screenRecordingMotionPreviewLabel"));
            checkNative(waitUntil([&]() { return ready && ready->isVisible(); }),
                        "preview must initialize on every display and in every idle input mode");
            area.repaint();
            area.canvas()->repaint();
            static_cast<void>(DwmFlush());
            const POINT gap{selected.x() + 300, selected.y() + 200};
            const HWND targetWindow = GetAncestor(WindowFromPoint(gap), GA_ROOT);
            const HWND areaHandle = reinterpret_cast<HWND>(area.winId());
            checkNative(
                (targetWindow == areaHandle) ==
                    (mode == ScreenRecordingAreaWindow::InputMode::Drawing),
                "drawing owns preview input while region-editing interiors stay click-through");
            SetCursorPos(selected.x() + 120, selected.y() + 100);
            INPUT inputs[3]{};
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki.wVk = 'A';
            inputs[1].type = inputs[2].type = INPUT_MOUSE;
            inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            checkNative(SendInput(3, inputs, sizeof(INPUT)) == 3,
                        "native input must reach the fixture");
            checkNative(waitUntil([&]() {
                            if (!preview.hasFrame()) {
                                return false;
                            }
                            const QImage image = previewImage(*area.canvas());
                            const qreal scale = area.devicePixelRatioF();
                            const QRect mouseArea(qRound(88 / scale), qRound(68 / scale),
                                                  qRound(64 / scale), qRound(64 / scale));
                            for (int y = mouseArea.top(); y <= mouseArea.bottom(); ++y) {
                                for (int x = mouseArea.left(); x <= mouseArea.right(); ++x) {
                                    if (image.pixelColor(x, y).alpha() > 2) {
                                        return true;
                                    }
                                }
                            }
                            return false;
                        }),
                        "native click effects must reach the canvas in every idle mode");
            inputs[0].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, inputs, sizeof(INPUT));
            std::cout << "native mode=" << static_cast<int>(mode)
                      << " display=" << target->name().toStdString()
                      << " dpr=" << area.devicePixelRatioF() << '\n';
        }
        for (const QSize captureSize : {QSize(641, 479), QSize(961, 719)}) {
            for (const QSize exportSize : {QSize(320, 240), QSize(1280, 960)}) {
                preview.setEligible(false);
                const QRect capture(selected.topLeft(), captureSize);
                area.setRecordingRegion(capture);
                background.setGeometry(area.geometry());
                area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
                area.raise();
                area.activateWindow();
                preview.configure(capture, exportSize, Qt::transparent, Qt::transparent, true);
                preview.setEligible(true);
                checkNative(waitUntil([&]() {
                                auto* ready = area.findChild<QLabel*>(
                                    QStringLiteral("screenRecordingMotionPreviewLabel"));
                                return ready && ready->isVisible();
                            }),
                            "keyboard-only preview must initialize");
                INPUT keyInput{};
                keyInput.type = INPUT_KEYBOARD;
                keyInput.ki.wVk = 'A';
                checkNative(SendInput(1, &keyInput, sizeof(INPUT)) == 1,
                            "native key input must reach the preview");
                QSize observedKeycap;
                const bool keyboardFrame = waitUntil([&]() {
                    if (!preview.hasFrame()) {
                        return false;
                    }
                    const qreal dpr = area.devicePixelRatioF();
                    QImage image(area.canvas()->size() * dpr,
                                 QImage::Format_RGBA8888_Premultiplied);
                    image.setDevicePixelRatio(dpr);
                    image.fill(Qt::transparent);
                    {
                        QPainter painter(&image);
                        area.canvas()->render(&painter);
                    }
                    QRect keyPixels;
                    for (int y = 0; y < image.height(); ++y) {
                        for (int x = 0; x < image.width(); ++x) {
                            if (image.pixelColor(x, y).alpha() > 16) {
                                keyPixels |= QRect(x, y, 1, 1);
                            }
                        }
                    }
                    observedKeycap = keyPixels.size();
                    // The antialiased outer edge can fall below the
                    // opacity threshold on one physical pixel.
                    return keyPixels.width() >= 63 && keyPixels.width() <= 64 &&
                           keyPixels.height() >= 63 && keyPixels.height() <= 64;
                });
                if (!keyboardFrame)
                    std::cerr << "native keyboard preview bounds=" << observedKeycap.width() << 'x'
                              << observedKeycap.height() << " dpr=" << area.devicePixelRatioF()
                              << " capture=" << captureSize.width() << 'x' << captureSize.height()
                              << " export=" << exportSize.width() << 'x' << exportSize.height()
                              << '\n';
                checkNative(keyboardFrame,
                            "native keyboard preview must remain exactly 64 physical pixels");
                keyInput.ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &keyInput, sizeof(INPUT));
                std::cout << "native keycap=64x64 capture=" << captureSize.width() << 'x'
                          << captureSize.height() << " export=" << exportSize.width() << 'x'
                          << exportSize.height() << " dpr=" << area.devicePixelRatioF() << '\n';
            }
        }
    }
    preview.setEligible(false);
    area.setRecordingRegion(region);
    background.setGeometry(area.geometry());
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    preview.configure(region, region.size(), Qt::red, Qt::cyan, true);
    // Keep an annotation in the capture baseline; shutdown must preserve it exactly.
    checkNative(area.canvas()->setCanvasTool(SnowCanvasTool::Shape),
                "native annotation tool must activate");
    for (const auto type :
         {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
        const QPointF point = type == QEvent::MouseButtonPress ? QPointF(30, 30) : QPointF(70, 70);
        QMouseEvent event(
            type, point, point, point, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(area.canvas(), &event);
    }
    checkNative(area.canvas()->canvasHistoryState().canUndo,
                "native capture fixture must retain its annotation");
    checkNative(area.canvas()->setCanvasTool(SnowCanvasTool::Select),
                "native annotation tool must release");
    pumpPreview();
    for (const auto backend :
         {SNOW_CAPTURE_BACKEND_WGC, SNOW_CAPTURE_BACKEND_DXGI, SNOW_CAPTURE_BACKEND_GDI}) {
        preview.setEligible(false);
        preview.stopAndClear(true);
        const auto capture = [&]() {
            const SnowCaptureRegionSessionConfig config{region.x(),
                                                        region.y(),
                                                        640,
                                                        480,
                                                        1,
                                                        SNOW_CAPTURE_WGC_UPDATE_MODE_COMPLETE_ONLY,
                                                        static_cast<uint8_t>(backend),
                                                        SNOW_CAPTURE_PIXEL_FORMAT_RGBA8,
                                                        {}};
            std::unique_ptr<SnowCaptureRegionSession,
                            decltype(&snow_capture_region_session_destroy)>
                captureSession(snow_capture_region_session_create(&config),
                               snow_capture_region_session_destroy);
            checkNative(captureSession != nullptr, nativeCaptureError());
            SnowCaptureRegionFrameInfo info{};
            QElapsedTimer firstFrame;
            firstFrame.start();
            while (snow_capture_region_session_capture(captureSession.get(), &info) == 0) {
                const QString error = QString::fromUtf8(nativeCaptureError());
                // DXGI can time out before producing any frame on a static desktop. Keep
                // waiting for that first frame; never discard a captured frame to pass QA.
                if (!error.contains(QStringLiteral("within timeout")) ||
                    firstFrame.elapsed() >= 3000) {
                    throw std::runtime_error(error.toStdString());
                }
                QCoreApplication::processEvents();
            }
            QImage image(info.rgba_bytes, static_cast<int>(info.width),
                         static_cast<int>(info.height), static_cast<qsizetype>(info.stride_bytes),
                         QImage::Format_RGBA8888);
            const QImage copied = image.copy();
            return copied;
        };
        const QImage clean = capture();
        preview.setEligible(true);
        auto* label = area.findChild<QLabel*>(QStringLiteral("screenRecordingMotionPreviewLabel"));
        checkNative(waitUntil([&]() { return label && label->isVisible(); }),
                    "native effect observers must initialize");
        SetCursorPos(region.x() + 100, region.y() + 100);
        SetCursorPos(region.x() + 180, region.y() + 140);
        INPUT key{};
        key.type = INPUT_KEYBOARD;
        key.ki.wVk = 'A';
        checkNative(SendInput(1, &key, sizeof(INPUT)) == 1, "native key must reach the fixture");
        checkNative(waitUntil([&]() {
                        if (!preview.hasFrame()) {
                            return false;
                        }
                        const QImage image = previewImage(*area.canvas());
                        for (int y = image.height() / 2; y < image.height(); ++y) {
                            for (int x = image.width() / 2; x < image.width(); ++x) {
                                if (image.pixelColor(x, y).alpha() != 0) {
                                    return true;
                                }
                            }
                        }
                        return false;
                    }),
                    "native keyboard input must reach the canvas");
        area.repaint();
        const QImage visible = capture();
        key.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &key, sizeof(INPUT));
        preview.setEligible(false);
        preview.stopAndClear(true);
        const QImage after = capture();
        int effectPixels = 0;
        int remaining = 0;
        for (int y = 0; y < clean.height(); ++y) {
            for (int x = 0; x < clean.width(); ++x) {
                const auto different = [&](const QImage& image) {
                    const QColor a = clean.pixelColor(x, y), b = image.pixelColor(x, y);
                    return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) +
                               qAbs(a.blue() - b.blue()) >
                           12;
                };
                effectPixels += different(visible);
                remaining += different(after);
            }
        }
        checkNative(effectPixels > 100, "native capture must observe visible preview effects");
        checkNative(remaining == 0,
                    "the first captured frame after shutdown must contain no preview pixels");
        visible.save(output +
                     QStringLiteral("/backend-%1-preview.png").arg(static_cast<int>(backend)));
        after.save(output +
                   QStringLiteral("/backend-%1-cleared.png").arg(static_cast<int>(backend)));
        std::cout << "backend=" << static_cast<int>(backend) << " preview_pixels=" << effectPixels
                  << " remaining=" << remaining << '\n';
    }
    area.hide();
    background.hide();
    return 0;
}
#endif
} // namespace

namespace snow_shot::platform {
storage::ScreenshotWindowNames
screenshotWindowNamesAtPoint(const QPointF& point, std::span<const std::uintptr_t> excludedWindows,
                             storage::ScreenshotWindowNameFields fields) {
    require(QThread::currentThread() == qApp->thread(),
            "recording names must be resolved on the GUI thread");
    ++windowNameLookups;
    windowNameLookupPoint = point;
    windowNameLookupFields = fields;
    windowNameLookupExclusions.assign(excludedWindows.begin(), excludedWindows.end());
    return {fields.applicationName ? desktopWindowNames.applicationName : QString(),
            fields.windowName ? desktopWindowNames.windowName : QString()};
}
} // namespace snow_shot::platform

extern "C" {
SnowWebcam* snow_webcam_create(uint32_t size, uint32_t fps) {
    ++webcamCreates;
    if (failWebcamCreation)
        return nullptr;
    return new SnowWebcam{size, fps, false};
}
uint8_t snow_webcam_configure(const SnowWebcam* source, uint32_t size, uint32_t fps) {
    auto* camera = const_cast<SnowWebcam*>(source);
    camera->size = size;
    camera->fps = fps;
    ++webcamConfigures;
    return 1;
}
void snow_webcam_set_active(const SnowWebcam* source, uint8_t active) {
    if (source)
        const_cast<SnowWebcam*>(source)->active = active != 0;
}
void snow_webcam_retry(const SnowWebcam*) {}
void snow_webcam_destroy(SnowWebcam* source) {
    delete source;
    ++webcamDestroys;
}
size_t snow_webcam_error(const SnowWebcam*, char* buffer, size_t capacity) {
    if (buffer && capacity)
        qstrncpy(buffer, webcamError.constData(), capacity);
    return static_cast<size_t>(webcamError.size());
}
SnowRecordingResult
snow_recording_session_create_direct(const SnowCaptureDirectRecordingConfig* config,
                                     SnowRecordingSession** result) {
    // Session creation runs on the controller's worker thread: only plain data
    // may be touched here. The preview label invariant is asserted on the GUI
    // thread by controllerPreviewTransitions instead.
    require(audioMonitorsActive == 0,
            "recording native initialization follows audio preview retirement");
    if (const auto gate = sessionCreateGate; gate.valid()) {
        sessionCreateEntered = true;
        gate.wait();
    }
    lastDirectConfig = *config;
    // The FFI receives native paths; compare captured destinations using Qt's path spelling.
    recordingOutputPath = QDir::fromNativeSeparators(QString::fromUtf8(config->output_file_utf8));
    liveSystemGain = config->system_audio_gain_db;
    liveMicrophoneGain = config->microphone_gain_db;
    recordingStopRequested = false;
    lastKeyboardFontFamily = config->keyboard_font_family_utf8;
    lastKeyboardCjkFontFamily = config->keyboard_cjk_font_family_utf8;
    lastExcludedWindows.clear();
    if (config->exclusions.window_count != 0) {
        lastExcludedWindows.assign(config->exclusions.windows,
                                   config->exclusions.windows + config->exclusions.window_count);
    }
    for (const auto& weak : effectSources) {
        if (const auto source = weak.lock()) {
            require(!source->active, "native creation must follow preview observer shutdown");
        }
    }
    if (failStart) {
        *result = nullptr;
        return SNOW_RECORDING_RESULT_INVALID_ARGUMENT;
    }
    auto* created = new SnowRecordingSession;
    created->destroyCompleted = std::exchange(nextSessionDestroyCompleted, nullptr);
    if (recordingFileFixture) {
        created->outputFile = QString::fromUtf8(config->output_file_utf8);
        created->stagingFile = created->outputFile + QStringLiteral(".test-staging");
        created->temporaryDirectory = created->outputFile + QStringLiteral(".test-temporary");
        QFile staging(created->stagingFile);
        require(staging.open(QIODevice::WriteOnly) && staging.write("captured media") > 0 &&
                    QDir().mkpath(created->temporaryDirectory),
                "fake capture creates session-owned recording artifacts");
    }
    lastCreatedSession = created;
    *result = created;
    return SNOW_RECORDING_RESULT_OK;
}
SnowRecordingResult
snow_recording_session_create_deferred(const SnowCaptureDirectRecordingConfig* config,
                                       const SnowRecordingDeferredOptions* options,
                                       SnowRecordingSession** result) {
    ++deferredCreates;
    lastDeferredOptions = *options;
    return snow_recording_session_create_direct(config, result);
}
SnowRecordingResult snow_recording_session_finalize_deferred(SnowRecordingSession* recording,
                                                             SnowRecordingSource** source) {
    const auto result = snow_recording_session_stop(recording);
    *source = result == SNOW_RECORDING_RESULT_OK ? new SnowRecordingSource : nullptr;
    return result;
}
SnowRecordingResult snow_recording_source_render_start(SnowRecordingSource*,
                                                       SnowRecordingRenderTask** task) {
    ++renderStarts;
    const auto gate = renderStartGate;
    renderStartEntered = true;
    if (gate.valid())
        gate.wait();
    ++activeRenderTasks;
    renderState = SNOW_RECORDING_RENDER_STATE_RUNNING;
    renderPercent = 0;
    *task = new SnowRecordingRenderTask;
    return SNOW_RECORDING_RESULT_OK;
}
size_t snow_recording_source_path(const SnowRecordingSource*, char* buffer, size_t capacity) {
    const QByteArray& value = renderSourcePath;
    if (buffer && capacity) {
        const size_t count = std::min(capacity - 1, static_cast<size_t>(value.size()));
        std::copy_n(value.constData(), count, buffer);
        buffer[count] = '\0';
    }
    return static_cast<size_t>(value.size()) + 1;
}
SnowRecordingResult snow_recording_source_discard(SnowRecordingSource*) {
    require(activeRenderTasks == 0, "source discard must follow task disposal");
    if (failSourceDiscard)
        return SNOW_RECORDING_RESULT_IO_ERROR;
    ++sourceDiscards;
    return SNOW_RECORDING_RESULT_OK;
}
void snow_recording_source_destroy(SnowRecordingSource* source) {
    require(activeRenderTasks == 0, "source must outlive every render attempt");
    ++sourceDestroys;
    delete source;
}
SnowRecordingResult snow_recording_render_task_poll(const SnowRecordingRenderTask*,
                                                    SnowRecordingRenderProgress* progress) {
    ++renderPolls;
    require(progress->version == SNOW_RECORDING_RENDER_PROGRESS_VERSION &&
                progress->struct_size == sizeof(*progress),
            "render poll must initialize the versioned progress header");
    if (failRenderPoll)
        return SNOW_RECORDING_RESULT_INTERNAL_ERROR;
    progress->state = renderState;
    progress->stage = SNOW_RECORDING_RENDER_STAGE_RENDER;
    progress->percent = renderPercent;
    progress->duration_ms = 2300;
    return SNOW_RECORDING_RESULT_OK;
}
SnowRecordingResult snow_recording_render_task_cancel(SnowRecordingRenderTask*) {
    const auto gate = renderCancelGate;
    renderCancelEntered = true;
    if (gate.valid())
        gate.wait();
    uint32_t running = SNOW_RECORDING_RENDER_STATE_RUNNING;
    renderState.compare_exchange_strong(running, SNOW_RECORDING_RENDER_STATE_CANCELED);
    return SNOW_RECORDING_RESULT_OK;
}
size_t snow_recording_render_task_error(const SnowRecordingRenderTask*, char* buffer,
                                        size_t capacity) {
    const QByteArray value("test rendering failure");
    if (buffer && capacity) {
        const size_t count = std::min(capacity - 1, static_cast<size_t>(value.size()));
        std::copy_n(value.constData(), count, buffer);
        buffer[count] = '\0';
    }
    return static_cast<size_t>(value.size()) + 1;
}
void snow_recording_render_task_destroy(SnowRecordingRenderTask* task) {
    ++taskDestroys;
    --activeRenderTasks;
    delete task;
}
void snow_recording_session_destroy(SnowRecordingSession* recording) {
    auto* completion = recording->destroyCompleted;
    if (const auto gate = sessionDestroyGate; gate.valid()) {
        sessionDestroyEntered = true;
        gate.wait();
    }
    if (!recording->stagingFile.isEmpty()) {
        require(QFile::remove(recording->stagingFile) &&
                    QDir(recording->temporaryDirectory).removeRecursively(),
                "native session destruction removes only its owned staging artifacts");
        recording->stagingFile.clear();
        recording->temporaryDirectory.clear();
        recording->outputFile.clear();
    }
    delete recording;
    ++destroyedSessions;
    if (completion)
        completion->set_value();
}
uint8_t snow_recording_session_start(SnowRecordingSession*) {
    ++starts;
    return failStartOperation ? 0 : 1;
}
uint8_t snow_recording_session_pause(SnowRecordingSession*) {
    return 1;
}
uint8_t snow_recording_session_resume(SnowRecordingSession*) {
    return 1;
}
SnowRecordingClip* snow_recording_clip_open(const char* path,
                                            const SnowRecordingClipOptions* options) {
    clipPath = QString::fromUtf8(path);
    clipDeferred = options == nullptr;
    ++clipOpens;
    clipPreviewSpeed = 100;
    return new SnowRecordingClip;
}
void snow_recording_clip_destroy(SnowRecordingClip* clip) {
    if (const auto gate = clipDestroyGate; gate.valid()) {
        clipDestroyEntered = true;
        gate.wait();
    }
    delete clip;
    ++clipDestroys;
}
uint8_t snow_recording_clip_info(const SnowRecordingClip*, SnowRecordingClipInfo* info) {
    *info = {32, 24, 1000000, 30};
    return 1;
}
uint64_t snow_recording_clip_boundary(const SnowRecordingClip*, uint64_t frame) {
    if (!clipBoundaries.isEmpty())
        return clipBoundaries.at(static_cast<qsizetype>(frame));
    return frame * 1000000 / 30;
}
uint64_t snow_recording_clip_seek(SnowRecordingClip* clip, uint64_t frame, uint64_t end,
                                  uint8_t play) {
    ++clipSeekCalls;
    if (failClipSeek)
        return 0;
    clipFirst = frame;
    clipEnd = end;
    clip->position = snow_recording_clip_boundary(clip, frame);
    clip->playing = play != 0;
    clipAutoplays += play != 0;
    return ++clip->revision;
}
uint64_t snow_recording_clip_set_speed(SnowRecordingClip* clip, uint32_t percent) {
    ++clipSpeedChanges;
    if (failClipSpeed)
        return 0;
    clip->speedPercent = percent;
    clipPreviewSpeed = percent;
    return ++clip->revision;
}
SnowRecordingClipFrame* snow_recording_clip_acquire(const SnowRecordingClip* clip,
                                                    SnowRecordingClipPreview* preview) {
    if (!clipPreviewEnabled)
        return nullptr;
    auto* frame = new SnowRecordingClipFrame;
    *preview = {clip->revision, clip->position, clip->playing ? 1u : 0u,
                reinterpret_cast<const uint8_t*>(frame->rgba.constData()),
                static_cast<size_t>(frame->rgba.size())};
    if (clipPreviewOverride)
        clipPreviewOverride(*preview);
    ++clipPreviewReads;
    return frame;
}
void snow_recording_clip_frame_destroy(SnowRecordingClipFrame* frame) {
    delete frame;
}
size_t snow_recording_clip_error(const SnowRecordingClip*, char* buffer, size_t capacity) {
    if (clipPreviewError.isEmpty())
        return 0;
    if (buffer && capacity > 0)
        qstrncpy(buffer, clipPreviewError.constData(), capacity);
    return static_cast<size_t>(clipPreviewError.size()) + 1;
}
SnowRecordingClipExport* snow_recording_clip_export_start(const SnowRecordingClip*, uint64_t first,
                                                          uint64_t end, const char* path) {
    clipFirst = first;
    clipEnd = end;
    ++clipExports;
    QFile file(QString::fromUtf8(path));
    require(file.open(QIODevice::WriteOnly), "trim export creates durable output");
    file.write("trim fixture");
    return new SnowRecordingClipExport;
}
SnowRecordingClipExport* snow_recording_clip_export_start_with_speed(const SnowRecordingClip* clip,
                                                                     uint64_t first, uint64_t end,
                                                                     const char* path,
                                                                     uint32_t percent) {
    clipExportSpeed = percent;
    return snow_recording_clip_export_start(clip, first, end, path);
}
uint32_t snow_recording_clip_export_poll(const SnowRecordingClipExport*, float* percent) {
    *percent = clipExportState == SNOW_RECORDING_RENDER_STATE_RUNNING ? 41.75f : 100;
    return clipExportState;
}
size_t snow_recording_clip_export_error(const SnowRecordingClipExport*, char* buffer,
                                        size_t capacity) {
    if (clipExportState != SNOW_RECORDING_RENDER_STATE_FAILED || clipExportError.isEmpty())
        return 0;
    if (buffer && capacity)
        qstrncpy(buffer, clipExportError.constData(), capacity);
    return static_cast<size_t>(clipExportError.size()) + 1;
}
void snow_recording_clip_export_cancel(SnowRecordingClipExport*) {
    clipExportState = SNOW_RECORDING_RENDER_STATE_CANCELED;
}
void snow_recording_clip_export_destroy(SnowRecordingClipExport* task) {
    delete task;
}
SnowRecordingResult snow_recording_session_stop(SnowRecordingSession* recording) {
    require(recording->stopRequested,
            "Stop must freeze its endpoint before asynchronous finalization");
    // Snapshot the gate before publishing entry. The UI may release and clear
    // the global shared_future as soon as exportEntered becomes ready.
    const auto gate = exportGate;
    const bool failure = failExport.load();
    if (exportEntered != nullptr) {
        exportEntered->set_value();
    }
    if (gate.valid()) {
        gate.wait();
    }
    if (!failure && !recording->outputFile.isEmpty()) {
        QFile output(recording->outputFile);
        require(output.open(QIODevice::WriteOnly) && output.write("finalized recording") > 0,
                "only finalization publishes the recording output");
    }
    ++exports;
    return failure ? SNOW_RECORDING_RESULT_INVALID_ARGUMENT : SNOW_RECORDING_RESULT_OK;
}
SnowRecordingResult snow_recording_session_request_stop(SnowRecordingSession* recording) {
    recording->stopRequested = true;
    recordingStopRequested = true;
    return SNOW_RECORDING_RESULT_OK;
}
uint8_t snow_recording_session_state(const SnowRecordingSession*, SnowRecordingState* state) {
    *state = SNOW_RECORDING_STATE_RUNNING;
    return 1;
}
uint8_t snow_recording_session_set_audio_gain(SnowRecordingSession*, uint32_t source,
                                              int32_t gain) {
    if (source > 1 || gain < -24 || gain > 24)
        return 0;
    (source == 0 ? liveSystemGain : liveMicrophoneGain) = gain;
    return 1;
}
uint8_t snow_recording_session_set_audio_metering(SnowRecordingSession*, uint32_t mask) {
    audioMeterMask = mask;
    return 1;
}
uint8_t snow_recording_session_take_audio_levels(const SnowRecordingSession*,
                                                 SnowRecordingAudioLevels* levels) {
    ++audioLevelReads;
    *levels = {{0.42f, 0, SNOW_RECORDING_AUDIO_READY, 0},
               {0.25f, 0, SNOW_RECORDING_AUDIO_READY, 0}};
    return 1;
}
uint8_t snow_recording_session_request_exclusions(SnowRecordingSession*,
                                                  const SnowCaptureExclusions*, const uint32_t*,
                                                  uint32_t, uint64_t* generation) {
    *generation = 1;
    return 1;
}
uint8_t snow_recording_session_exclusion_status(const SnowRecordingSession*,
                                                SnowRecordingExclusionStatus* status) {
    *status = {1, 1, 0};
    return 1;
}
SnowRecordingResult snow_recording_audio_monitor_create(uint32_t source, int32_t gain,
                                                        SnowRecordingAudioMonitor** monitor) {
    *monitor = new SnowRecordingAudioMonitorImpl{source, gain};
    ++audioMonitorCreates;
    ++audioMonitorsActive;
    return SNOW_RECORDING_RESULT_OK;
}
void snow_recording_audio_monitor_cancel(SnowRecordingAudioMonitor*) {}
void snow_recording_audio_monitor_destroy(SnowRecordingAudioMonitor* monitor) {
    while (holdAudioMonitorDestroy)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    delete monitor;
    --audioMonitorsActive;
    ++audioMonitorDestroys;
}
uint8_t snow_recording_audio_monitor_set_gain(SnowRecordingAudioMonitor* monitor, int32_t gain) {
    monitor->gain = gain;
    return 1;
}
uint8_t snow_recording_audio_monitor_set_metering(SnowRecordingAudioMonitor*, uint8_t) {
    return 1;
}
uint8_t snow_recording_audio_monitor_take_levels(const SnowRecordingAudioMonitor* monitor,
                                                 SnowRecordingAudioLevels* levels) {
    *levels = {};
    auto& source = monitor->source == 0 ? levels->system_audio : levels->microphone;
    source = {0.42f, 0, SNOW_RECORDING_AUDIO_READY, 0};
    return 1;
}
const char* snow_recording_last_error_message() {
    if (failExport) {
        return "recording recovery: finalization failed; recoverable media is retained in "
               "D:/recordings/recovery";
    }
    return "test backend";
}
}

void waitForWebcam(const std::function<bool()>& condition, const char* message) {
    QElapsedTimer deadline;
    deadline.start();
    while (!condition() && deadline.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
    require(condition(), message);
}

void prepareWebcamSettings(bool enabled) {
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    require(
        configuration.setValues(
            {{QStringLiteral("screen_recording/enable_webcam"), enabled},
             {QStringLiteral("screen_recording/webcam_size"), 150},
             {QStringLiteral("screen_recording/webcam_orientation"), 0},
             {QStringLiteral("screen_recording/webcam_offset_left"), 48},
             {QStringLiteral("screen_recording/webcam_offset_bottom"), 48},
             {QStringLiteral("screen_recording/webcam_shape"), QStringLiteral("square")},
             {QStringLiteral("screen_recording/show_cursor"), false},
             {QStringLiteral("screen_recording/show_keyboard"), false},
             {QStringLiteral("screen_recording/record_mouse_clicks"), false},
             {QStringLiteral("screen_recording/record_mouse_scrolling"), false},
             {QStringLiteral("screen_recording/mouse_highlight_enabled"), false},
             {QStringLiteral("screen_recording/mouse_trail_color"), QStringLiteral("#00000000")},
             {QStringLiteral("screen_recording/mouse_click_color"), QStringLiteral("#00000000")},
             {QStringLiteral("screen_recording/enable_microphone"), false},
             {QStringLiteral("screen_recording/enable_system_audio"), false},
             {QStringLiteral("screen_recording/post_processing_enabled"), false},
             {QStringLiteral("screen_recording/start_delay_seconds"), 0},
             {QStringLiteral("screen_recording/auto_exit_after_recording_ends"), false}}),
        "prepare an isolated webcam-only recording");
}

void webcamPreviewRetainsFramesAcrossLayoutUpdates() {
    auto state = std::make_shared<RecordingEffectTestState>();
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(QRect(40, 40, 640, 480));
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    RecordingEffectPreview preview(area, std::make_unique<RecordingEffectTestSource>(state));
    SnowWebcam camera;
    const auto configure = [&](int size, const QPoint& offset, uint32_t shape, SnowWebcam* source) {
        preview.configure(area.recordingRegion(), QSize(640, 480), Qt::transparent, Qt::transparent,
                          false, 500, QColor(0, 0, 0, 204), Qt::white, 64, Qt::transparent, false,
                          41, 0, QColor(22, 119, 255), source, size, shape, offset);
    };
    area.show();
    configure(150, QPoint(48, 48), 0, &camera);
    preview.setEligible(true);
    pumpPreview();
    require(preview.hasFrame(), "camera preview starts with a visible frame");
    const quint64 generation = preview.generation();
    const QImage initial = previewImage(*area.canvas());
    state->autoPublish = false;
    for (int step = 0; step < 40; ++step) {
        configure(150 + step, QPoint(48 + step, 48 + step), step % 2, &camera);
        require(preview.hasFrame(),
                "layout updates retain the camera until its replacement arrives");
        pumpPreview();
        require(preview.hasFrame() && preview.generation() == generation &&
                    previewImage(*area.canvas()) == initial && state->starts == 1 &&
                    state->stops == 0,
                "rapid webcam movement and resizing must not blank or restart the preview");
    }
    state->publish();
    state->frame->tiles.front().rect.translate(100, 0);
    pumpPreview();
    const QImage moved = previewImage(*area.canvas());
    require(preview.hasFrame() && moved != initial,
            "a replacement frame must swap in while layout updates continue");
    for (int y = 0; y < initial.height(); ++y) {
        for (int x = 0; x < initial.width(); ++x) {
            if (initial.pixelColor(x, y).alpha() != 0)
                require(
                    moved.pixelColor(x, y).alpha() == 0,
                    "replacing the camera frame clears its old footprint without a ghost image");
        }
    }
    const auto late = state->frame;
    configure(150, QPoint(48, 48), 0, nullptr);
    require(!preview.hasFrame() && !state->active, "disabling the camera still clears immediately");
    state->frame = late;
    pumpPreview();
    require(!preview.hasFrame(), "retained frames cannot resurrect a disabled camera");
}

void recordingWebcamPreviewLifecycle() {
    prepareWebcamSettings(true);
    const snow_shot::storage::RecordingSettings settings;
    const int created = webcamCreates;
    const int destroyed = webcamDestroys;
    {
        ScreenRecordingController controller(testEffectsSource);
        controller.open({80, 80, 320, 240});
        const auto preview = effectSources.back().lock();
        waitForWebcam([&] { return preview && preview->active && preview->webcam; },
                      "saved webcam enablement must start preview without a later UI edit");
        const auto* camera = preview->webcam;
        waitForWebcam([&] { return camera->active; }, "showing the selection activates the camera");
        require(webcamCreates == created + 1 && preview->webcamSize == 150 &&
                    preview->webcamShape == 0,
                "preview must share one camera with the saved style");
        require(settings.setWebcamLayout(300, QPoint(80, 60)) &&
                    settings.setWebcamShape(QStringLiteral("circle")),
                "change idle webcam style");
        waitForWebcam(
            [&] {
                return preview->webcamSize == 300 && preview->webcamShape == 1 &&
                       preview->webcamOffset == QPoint(80, 60);
            },
            "idle saved styles must update the existing preview");
        require(preview->webcam == camera && camera->size == 300 && webcamCreates == created + 1,
                "style changes reuse the shared camera source");
        require(settings.setWebcamEnabled(false), "disable idle webcam");
        require(!preview->active && !camera->active,
                "disabling webcam retires preview and capture");
        require(settings.setWebcamEnabled(true), "re-enable idle webcam");
        waitForWebcam(
            [&] { return preview->active && preview->webcam == camera && camera->active; },
            "re-enabling webcam restores preview on the same source");
        palette()->recordingCloseRequested();
        require(!controller.isOpen() && !camera->active, "closing the selection stops capture");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        controller.open({80, 80, 320, 240});
        const auto reopened = effectSources.back().lock();
        waitForWebcam([&] { return reopened && reopened->active && reopened->webcam == camera; },
                      "reopening configures the replacement preview with the retained source");
        require(webcamCreates == created + 1, "reopening must not create competing camera sources");
        palette()->recordingCloseRequested();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(webcamDestroys == destroyed + 1, "controller teardown releases its camera source");
}

void recordingWebcamGesturesPersistAndFreezeAtStartup() {
    prepareWebcamSettings(true);
    const snow_shot::storage::RecordingSettings settings;
    require(settings.setWebcamLayout(150, QPoint(120, 90)), "save an initial webcam layout");
    ScreenRecordingController controller(testEffectsSource);
    controller.open({80, 80, 640, 480});
    auto* area = recordingAreaWindow();
    require(area != nullptr, "webcam editing needs a recording preview");
    area->setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    RecordingWebcamControl* control = nullptr;
    waitForWebcam(
        [&] {
            control = area->findChild<RecordingWebcamControl*>();
            return control && control->isVisible();
        },
        "idle webcam preview exposes its input surface");
    const auto preview = effectSources.back().lock();
    const auto* camera = preview->webcam;
    const std::array<const char*, 4> orientationNames{
        "screenRecordingWebcamRotateClockwise", "screenRecordingWebcamRotateCounterclockwise",
        "screenRecordingWebcamFlipHorizontal", "screenRecordingWebcamFlipVertical"};
    std::array<QAbstractButton*, 4> orientationButtons{};
    for (std::size_t i = 0; i < orientationButtons.size(); ++i) {
        orientationButtons[i] =
            control->findChild<QAbstractButton*>(QString::fromLatin1(orientationNames[i]));
        require(orientationButtons[i] != nullptr, "webcam orientation action exists");
    }
    // Explicit transition table: every action is relative to the displayed image.
    const std::array<std::array<int, 4>, 8> transitions{{{1, 3, 4, 6},
                                                         {2, 0, 7, 5},
                                                         {3, 1, 6, 4},
                                                         {0, 2, 5, 7},
                                                         {5, 7, 0, 2},
                                                         {6, 4, 3, 1},
                                                         {7, 5, 2, 0},
                                                         {4, 6, 1, 3}}};
    const auto generation = preview->generation;
    const int previewStarts = preview->starts;
    for (int orientation = 0; orientation < 8; ++orientation) {
        for (std::size_t action = 0; action < orientationButtons.size(); ++action) {
            require(settings.setWebcamOrientation(orientation), "prepare webcam orientation");
            waitForWebcam(
                [&] { return preview->webcamOrientation == static_cast<uint32_t>(orientation); },
                "saved orientation reaches preview");
            orientationButtons[action]->click();
            const int expected = transitions[orientation][action];
            waitForWebcam(
                [&] { return preview->webcamOrientation == static_cast<uint32_t>(expected); },
                "orientation action updates the current camera frame");
            require(settings.webcamOrientation() == expected && preview->webcam == camera &&
                        preview->generation == generation && preview->starts == previewStarts,
                    "orientation persists without restarting capture or preview");
        }
    }
    require(settings.setWebcamOrientation(5), "save the recording orientation");
    waitForWebcam([&] { return preview->webcamOrientation == 5; },
                  "final orientation reaches preview");
    const QSize initialWindowSize = area->size();
    const QRect initialControl = control->geometry();
    area->resize(90, 70);
    waitForWebcam([&] { return preview->output.width() < 150 && preview->output.height() < 150; },
                  "shrinking the recording region refits the webcam preview");
    require(area->selectionRect().adjusted(-1, -1, 1, 1).contains(control->geometry()) &&
                settings.webcamSize() == 150 && settings.webcamOffsetLeft() == 120 &&
                settings.webcamOffsetBottom() == 90,
            "temporary fitting keeps the webcam inside without overwriting its preferences");
    area->resize(initialWindowSize);
    waitForWebcam([&] { return control->geometry() == initialControl; },
                  "growing the recording region restores the preferred webcam layout");
    require(preview->webcam == camera, "region changes keep the same camera source");
    const auto send = [](QWidget& target, QEvent::Type type, QPointF global,
                         Qt::MouseButton changed, Qt::MouseButtons held) {
        const QPointF local = target.mapFromGlobal(global);
        QMouseEvent event(type, local, local, global, changed, held, Qt::NoModifier);
        QCoreApplication::sendEvent(&target, &event);
    };
    QPointF origin = control->mapToGlobal(QPointF(control->rect().center()));
    const QPointF delta(40 / area->devicePixelRatioF(), -20 / area->devicePixelRatioF());
    send(*control, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
    send(*control, QEvent::MouseMove, origin + delta, Qt::NoButton, Qt::LeftButton);
    require(settings.webcamOffsetLeft() == 120 && settings.webcamOffsetBottom() == 90,
            "in-progress webcam edits must not write preferences");
    send(*control, QEvent::MouseButtonRelease, origin + delta, Qt::LeftButton, Qt::NoButton);
    require(settings.webcamOffsetLeft() == 160 && settings.webcamOffsetBottom() == 110,
            "finished webcam gestures save the bottom-left offsets");
    waitForWebcam([&] { return preview->webcamOffset == QPoint(160, 110); },
                  "move reaches the native preview");
    require(preview->webcam == camera, "moving reuses the camera source");
    auto* button = control->findChild<QToolButton*>(QStringLiteral("screenRecordingWebcamResize"));
    origin = button->mapToGlobal(QPointF(button->rect().center()));
    send(*button, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
    send(*control, QEvent::MouseMove, origin + delta, Qt::NoButton, Qt::LeftButton);
    require(palette()->recordingWebcamSize() == 180 && settings.webcamSize() == 150,
            "resize previews synchronize the size control without saving");
    controller.startRecording();
    waitForWebcam([&] { return controller.isRecording(); },
                  "start recording during an unfinished resize");
    require(lastDirectConfig.webcam_size == 150 && lastDirectConfig.webcam_offset_left == 160 &&
                lastDirectConfig.webcam_offset_bottom == 110 && !control->isVisible() &&
                lastDirectConfig.webcam_orientation == 5,
            "startup cancels an unfinished resize before freezing the webcam layout");
    QMetaObject::invokeMethod(orientationButtons[0], "clicked", Qt::DirectConnection,
                              Q_ARG(bool, false));
    emit area->webcamOrientationRequested(0);
    require(settings.webcamOrientation() == 5 && lastDirectConfig.webcam_orientation == 5,
            "late orientation clicks cannot change the accepted recording snapshot");
    palette()->recordingCloseRequested();
    waitForIdle(controller);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void recordingWarningsUseAreaWindow() {
    using namespace adqt::widgets;
    enum class Failure { Initialization, Recording, Paused, Preview };
    for (const auto failure :
         {Failure::Initialization, Failure::Recording, Failure::Paused, Failure::Preview}) {
        prepareWebcamSettings(failure != Failure::Preview);
        const snow_shot::storage::RecordingSettings settings;
        require(settings.setAutoHideToolbar(false), "keep warning fixture controls visible");
        if (failure == Failure::Preview)
            require(settings.setShowKeyboard(true), "enable the failing motion preview");
        failWebcamCreation = failure == Failure::Initialization;
        const auto resetFailure = qScopeGuard([] {
            failWebcamCreation = false;
            webcamError.clear();
        });
        ScreenRecordingController controller([failure] {
            auto source = testEffectsSource();
            if (failure == Failure::Preview)
                effectSources.back().lock()->fail = true;
            return source;
        });
        controller.open({80, 80, 320, 240});
        auto* controls = palette();
        auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(controls->window());
        QPointer<ScreenRecordingAreaWindow> area = recordingAreaWindow();
        require(area && area->isVisible(), "warning fixture has a visible recording area");
        auto* messages = AdMessageService::instance(area);
        if (failure == Failure::Recording || failure == Failure::Paused) {
            controller.startRecording();
            waitForRecording(controller);
            if (failure == Failure::Paused)
                controls->recordingPauseRequested();
            toolbar->hide();
            webcamError = "test camera disconnected";
        }
        waitForWebcam([&] { return messages->count() == 1; },
                      "recording warnings must appear in the recording area");
        QPointer<AdMessageHandle> handle = messages->findChild<AdMessageHandle*>();
        const bool preview = failure == Failure::Preview;
        require(handle &&
                    handle->key() == (preview ? QStringLiteral("screen-recording-preview-error")
                                              : QStringLiteral("screen-recording-webcam-error")),
                "the recording area must receive the original warning");
        QPointer<QWidget> notice = handle->noticeWidget();
        require(notice && notice->window() == area,
                "warning notices must belong to the recording area window");
        waitForWebcam(
            [&] {
                return notice && notice->isVisible() &&
                       area->rect().contains(QRect(notice->mapTo(area, QPoint()), notice->size()));
            },
            "warning notices must settle inside the recording area bounds");
        require(toolbar->findChild<AdMessage*>() == nullptr,
                "recording warnings must not create a toolbar message host");
        toolbar->showWithoutActivating();
        toolbar->hide();
        require(handle && handle->isOpen() && notice->isVisible(),
                "hiding recording controls must preserve recording area warnings");
        controls->recordingCloseRequested();
        waitForIdle(controller);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(!area && !handle, "closing the recording UI must retire its warning notices");
    }
}

void recordingWebcamPreferencesPreserveSessionSnapshots() {
    enum class Phase { Scheduled, Countdown, Starting, Recording, Paused, Finalizing };
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    for (const bool initiallyEnabled : {false, true}) {
        for (const auto phase : {Phase::Scheduled, Phase::Countdown, Phase::Starting,
                                 Phase::Recording, Phase::Paused, Phase::Finalizing}) {
            prepareWebcamSettings(initiallyEnabled);
            const snow_shot::storage::RecordingSettings settings;
            if (phase == Phase::Countdown)
                require(settings.setStartDelaySeconds(1), "prepare a controlled countdown");
            ScreenRecordingController controller(testEffectsSource);
            controller.open({80, 80, 320, 240});
            std::promise<void> releaseStart;
            if (phase == Phase::Starting) {
                sessionCreateEntered = false;
                sessionCreateGate = releaseStart.get_future().share();
            }
            controller.startRecording();
            const auto recording = [&] {
                return controller.automationState().value(QStringLiteral("state")) ==
                       QStringLiteral("recording");
            };
            if (phase == Phase::Starting)
                waitForWebcam([] { return sessionCreateEntered.load(); }, "hold native startup");
            else if (phase != Phase::Scheduled && phase != Phase::Countdown)
                waitForWebcam(recording, "start the webcam snapshot fixture");
            if (phase == Phase::Paused)
                palette()->recordingPauseRequested();
            std::promise<void> releaseExport;
            std::promise<void> enteredExport;
            auto exportReady = enteredExport.get_future();
            if (phase == Phase::Finalizing) {
                exportGate = releaseExport.get_future().share();
                exportEntered = &enteredExport;
                palette()->recordingStopRequested();
                waitForWebcam(
                    [&] {
                        return exportReady.wait_for(std::chrono::seconds(0)) ==
                               std::future_status::ready;
                    },
                    "hold recording finalization");
            }
            const int configured = webcamConfigures;
            require(
                configuration.setValues(
                    {{QStringLiteral("screen_recording/enable_webcam"), !initiallyEnabled},
                     {QStringLiteral("screen_recording/webcam_size"), 300},
                     {QStringLiteral("screen_recording/webcam_offset_left"), 80},
                     {QStringLiteral("screen_recording/webcam_offset_bottom"), 60},
                     {QStringLiteral("screen_recording/webcam_orientation"), 7},
                     {QStringLiteral("screen_recording/webcam_shape"), QStringLiteral("circle")}}),
                "save the next recording's webcam preferences during an active session");
            const auto options =
                controller.automationState().value(QStringLiteral("options")).toObject();
            require(options.value(QStringLiteral("webcam")) == initiallyEnabled &&
                        options.value(QStringLiteral("webcam_size")) == 150 &&
                        options.value(QStringLiteral("webcam_shape")) == QStringLiteral("square") &&
                        options.value(QStringLiteral("webcam_orientation")) == 0 &&
                        webcamConfigures == configured,
                    "saving preferences must preserve the accepted session's webcam snapshot");
            if (phase == Phase::Starting) {
                releaseStart.set_value();
                sessionCreateGate = {};
            }
            if (phase == Phase::Scheduled || phase == Phase::Countdown || phase == Phase::Starting)
                waitForWebcam(recording, "accepted startup retains its original webcam options");
            require((lastDirectConfig.webcam_source != nullptr) == initiallyEnabled &&
                        lastDirectConfig.webcam_size == 150 && lastDirectConfig.webcam_shape == 0 &&
                        lastDirectConfig.webcam_offset_left == 48 &&
                        lastDirectConfig.webcam_offset_bottom == 48 &&
                        lastDirectConfig.webcam_orientation == 0,
                    "native recording receives the accepted webcam snapshot");
            if (phase == Phase::Paused)
                palette()->recordingResumeRequested();
            if (phase == Phase::Finalizing) {
                releaseExport.set_value();
                exportGate = {};
                exportEntered = nullptr;
            } else {
                palette()->recordingStopRequested();
            }
            waitForWebcam(
                [&] {
                    return !controller.isRecording() &&
                           !controller.automationState().value(QStringLiteral("busy")).toBool();
                },
                "finalization returns to editable settings");
            require(palette()->recordingWebcamEnabled() == !initiallyEnabled &&
                        palette()->recordingWebcamSize() == 300 &&
                        palette()->recordingWebcamShape() == QStringLiteral("circle"),
                    "returning to idle applies saved webcam preferences to the toolbar");
            if (!initiallyEnabled) {
                const auto preview = effectSources.back().lock();
                waitForWebcam(
                    [&] {
                        return preview && preview->active && preview->webcam &&
                               preview->webcamSize == 300 && preview->webcamShape == 1;
                    },
                    "deferred enablement configures preview before the next recording");
            }
            palette()->recordingStartDelaySecondsChanged(0);
            controller.open({80, 80, 320, 240});
            controller.startRecording();
            waitForWebcam(recording, "start the next manual recording");
            require(
                (lastDirectConfig.webcam_source != nullptr) == !initiallyEnabled &&
                    lastDirectConfig.webcam_size == 300 && lastDirectConfig.webcam_shape == 1 &&
                    lastDirectConfig.webcam_offset_left == 80 &&
                    lastDirectConfig.webcam_offset_bottom == 60 &&
                    lastDirectConfig.webcam_orientation == 7,
                "the next manual recording receives all preferences saved during its predecessor");
            palette()->recordingCloseRequested();
            waitForIdle(controller);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }
}

void recordingWebcamAutomationOptionsStayIsolated() {
    prepareWebcamSettings(false);
    require(snow_shot::storage::RecordingSettings().setWebcamLayout(150, QPoint(90, 70)),
            "save non-default offsets for automation to inherit");
    require(snow_shot::storage::RecordingSettings().setWebcamOrientation(5),
            "save an orientation for automation to inherit");
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    ScreenRecordingController controller(testEffectsSource);
    QString error;
    require(controller.startAutomation({80, 80, 320, 240},
                                       {{QStringLiteral("webcam"), true},
                                        {QStringLiteral("webcam_size"), 220},
                                        {QStringLiteral("webcam_shape"), QStringLiteral("circle")}},
                                       &error),
            "start with isolated automation webcam options");
    waitForWebcam([&] { return controller.isRecording(); }, "start automated webcam fixture");
    require(lastDirectConfig.webcam_offset_left == 90 &&
                lastDirectConfig.webcam_offset_bottom == 70 &&
                lastDirectConfig.webcam_orientation == 5,
            "automation inherits saved webcam offsets");
    require(configuration.setValues(
                {{QStringLiteral("screen_recording/enable_webcam"), true},
                 {QStringLiteral("screen_recording/webcam_size"), 300},
                 {QStringLiteral("screen_recording/webcam_offset_left"), 80},
                 {QStringLiteral("screen_recording/webcam_offset_bottom"), 60},
                 {QStringLiteral("screen_recording/webcam_orientation"), 3},
                 {QStringLiteral("screen_recording/webcam_shape"), QStringLiteral("square")}}),
            "save manual preferences while automation owns recording");
    const auto saved = configuration.snapshot();
    require(controller.controlAutomation(QStringLiteral("stop"), {}, &error),
            "stop the automated recording");
    waitForIdle(controller);
    require(palette()->recordingWebcamSize() == 220 &&
                palette()->recordingWebcamShape() == QStringLiteral("circle") &&
                controller.automationState()
                        .value(QStringLiteral("options"))
                        .toObject()
                        .value(QStringLiteral("webcam_offset_left"))
                        .toInt() == 90 &&
                controller.automationState()
                        .value(QStringLiteral("options"))
                        .toObject()
                        .value(QStringLiteral("webcam_offset_bottom"))
                        .toInt() == 70,
            "automation retains its isolated webcam options through finalization");
    controller.startRecording();
    waitForWebcam([&] { return controller.isRecording(); },
                  "start a manual session after automation");
    require(lastDirectConfig.webcam_source && lastDirectConfig.webcam_size == 300 &&
                lastDirectConfig.webcam_shape == 0 && lastDirectConfig.webcam_offset_left == 80 &&
                lastDirectConfig.webcam_offset_bottom == 60 &&
                lastDirectConfig.webcam_orientation == 3 && configuration.snapshot() == saved,
            "manual startup restores current saved options without overwriting preferences");
    palette()->recordingCloseRequested();
    waitForIdle(controller);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void recordingWebcamSettingsSurviveInterruptedStartup() {
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    for (const bool cancelCountdown : {true, false}) {
        prepareWebcamSettings(true);
        ScreenRecordingController controller(testEffectsSource);
        controller.open({80, 80, 320, 240});
        std::promise<void> release;
        if (cancelCountdown) {
            palette()->recordingStartDelaySecondsChanged(10);
        } else {
            sessionCreateEntered = false;
            sessionCreateGate = release.get_future().share();
        }
        controller.startRecording();
        if (!cancelCountdown)
            waitForWebcam([] { return sessionCreateEntered.load(); }, "hold the failing startup");
        require(configuration.setValues(
                    {{QStringLiteral("screen_recording/enable_webcam"), false},
                     {QStringLiteral("screen_recording/webcam_size"), 280},
                     {QStringLiteral("screen_recording/webcam_shape"), QStringLiteral("circle")}}),
                "save webcam preferences before interrupted startup retires");
        ErrorObserver errors;
        if (cancelCountdown) {
            palette()->recordingCloseRequested();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            controller.open({80, 80, 320, 240});
        } else {
            qApp->installEventFilter(&errors);
            failStart = true;
            release.set_value();
            sessionCreateGate = {};
            waitForWebcam(
                [&] {
                    return !controller.automationState().value(QStringLiteral("busy")).toBool();
                },
                "failed startup returns to editable webcam settings");
            qApp->removeEventFilter(&errors);
            failStart = false;
            require(errors.shown == 1, "the controlled startup failure is reported once");
        }
        require(!palette()->recordingWebcamEnabled() && palette()->recordingWebcamSize() == 280 &&
                    palette()->recordingWebcamShape() == QStringLiteral("circle"),
                "reopening and startup failure restore all deferred webcam preferences");
        palette()->recordingStartDelaySecondsChanged(0);
        controller.startRecording();
        waitForWebcam([&] { return controller.isRecording(); }, "retry the interrupted recording");
        require(!lastDirectConfig.webcam_source && lastDirectConfig.webcam_size == 280 &&
                    lastDirectConfig.webcam_shape == 1,
                "retry uses preferences saved during the interrupted startup");
        palette()->recordingCloseRequested();
        waitForIdle(controller);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

void recordingSettingsDialog() {
    using namespace adqt::widgets;
    namespace settings = snow_shot::presentation::settings;
    ScreenRecordingController controller(testEffectsSource);
    controller.open(QRect(80, 80, 320, 240));
    auto* toolbarPalette = palette();
    auto* button = toolbarPalette->findChild<AdButton*>(QStringLiteral("screenRecordingSettings"));
    require(button != nullptr, "recording settings button must exist");
    require(toolbarPalette->findChild<AdModal*>(QStringLiteral("screenRecordingSettingsModal")) ==
                nullptr,
            "recording preferences must stay lazy when opening the toolbar");
    button->click();
    QCoreApplication::processEvents();
    auto* modal = controller.findChild<AdModal*>(QStringLiteral("screenRecordingSettingsModal"));
    require(modal != nullptr && modal->isOpen(), "settings button must open a popup window");
    require(modal->mode() == AdModal::Mode::Window && modal->ownerWindow() != nullptr,
            "recording settings must be a standalone owned popup");
    require(qobject_cast<ScreenRecordingAreaWindow*>(modal->ownerWindow()) != nullptr,
            "recording settings must use the recording area as their placement anchor");
    auto* recordingArea = qobject_cast<ScreenRecordingAreaWindow*>(modal->ownerWindow());
    auto* moveHandle =
        recordingArea->findChild<QWidget*>(QStringLiteral("screenRecordingRegionDragHandle"));
    require(recordingArea->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !recordingArea->canvas()->interactionEnabled() && moveHandle &&
                !moveHandle->isVisible(),
            "recording preferences must suspend region controls and drawing input");
    requireModalCenteredOnArea(modal, modal->ownerWindow()->frameGeometry(),
                               modal->ownerWindow()->screen());
    auto* form =
        modal->contentWidget()->findChild<AdForm*>(QStringLiteral("screenRecordingSettingsForm"));
    require(form != nullptr, "recording preferences must use Ant Design Qt Form");
    require(form->formLayout() == AdForm::FormLayout::Vertical,
            "recording preferences must place ordinary form labels above their controls");
    const auto& registry = settings::builtInSettingsRegistry();
    int expectedCount = 0;
    for (const auto& descriptor : registry.fields()) {
        if (descriptor.pageId != QStringLiteral("screen-recording"))
            continue;
        if (!settings::SettingsFormField::supports(descriptor)) {
            require(form->field(descriptor.id) == nullptr,
                    "toolbar customization must stay on the full settings page");
            continue;
        }
        ++expectedCount;
        require(form->field(descriptor.id) != nullptr,
                "every ordinary feature and system recording preference must appear in the form");
        require(form->field(descriptor.id)->label() == descriptor.definition->title.translated(),
                "recording preferences must share their labels with the main settings page");
        require(form->field(descriptor.id)->extraText().isEmpty(),
                "recording preferences must keep descriptions out of the compact form rows");
    }
    const int apiModeCount = form->field(QStringLiteral("screen-recording.api-mode")) ? 1 : 0;
    // Video/audio (4), animation (3), encoding (2), capture (1), interaction (5).
    require(form->items().size() == expectedCount && expectedCount == 15 + apiModeCount,
            "recording popup must contain exactly the requested settings categories");
    auto* autoHide =
        form->findChild<AdSwitch*>(QStringLiteral("screen-recording.auto-hide-toolbar"));
    require(autoHide && !autoHide->isChecked(),
            "recording settings include auto-hide toolbar disabled by default");
    auto* recordingToolbar = qobject_cast<ScreenRecordingToolbarWindow*>(toolbarPalette->window());
    require(recordingToolbar != nullptr, "recording settings have their recording toolbar");
    const QRect toolbarMonitor = recordingToolbar->screen()->geometry();
    const QRect toolbarRow = recordingToolbar->paletteHost()->mainToolbarContentRect();
    recordingToolbar->moveContentTo(
        QPoint(toolbarMonitor.left() - toolbarRow.left(), recordingToolbar->contentPosition().y()));
    auto* retreat =
        recordingToolbar->findChild<QTimer*>(QStringLiteral("recordingToolbarRetreatTimer"));
    require(retreat != nullptr, "recording settings fixture has an auto-hide retreat timer");
    const QPoint outside = toolbarMonitor.bottomRight() + QPoint(100, 100);
    require(snow_shot::storage::RecordingSettings().setAutoHideToolbar(true),
            "enable auto-hide while real recording preferences are open");
    setRecordingToolbarAutoHideTestCursor(*recordingToolbar, outside);
    require(recordingToolbar->isVisible() && !retreat->isActive(),
            "real recording preferences inhibit auto-hide even outside the toolbar");
    require(snow_shot::storage::RecordingSettings().setAutoHideToolbar(false),
            "restore auto-hide before checking default recording preferences");
    auto* notifyExport = form->findChild<AdSwitch*>(
        QStringLiteral("screen-recording.notify-after-export-completes"));
    require(notifyExport && !notifyExport->isChecked(),
            "recording settings include export notifications disabled by default");
    auto* deleteOnClose = form->findChild<AdSwitch*>(
        QStringLiteral("screen-recording.delete-files-on-recording-close"));
    require(deleteOnClose && !deleteOnClose->isChecked(),
            "recording settings include deleting files on close disabled by default");
    auto* statusPrompts =
        form->findChild<AdSwitch*>(QStringLiteral("screen-recording.show-status-prompts"));
    require(statusPrompts && statusPrompts->isChecked(),
            "recording settings include status prompts enabled by default");
    class SettingsTranslator final : public QTranslator {
      public:
        bool isEmpty() const override {
            return false;
        }
        QString translate(const char* context, const char* source, const char*,
                          int) const override {
            if (QByteArray(context) == "ScreenRecordingSettingsDialog" ||
                QByteArray(context) == "SettingsCatalog")
                return QStringLiteral("Translated: ") + QString::fromUtf8(source);
            return {};
        }
    } translator;
    require(QCoreApplication::installTranslator(&translator),
            "install recording settings translations");
    QCoreApplication::processEvents();
    require(modal->windowTitle() == QStringLiteral("Translated: Recording settings") &&
                form->field(QStringLiteral("screen-recording.auto-hide-toolbar"))->label() ==
                    QStringLiteral("Translated: Auto-hide toolbar") &&
                form->field(QStringLiteral("screen-recording.auto-hide-toolbar"))->tooltipText() ==
                    registry.field(QStringLiteral("screen-recording.auto-hide-toolbar"))
                        ->definition->description.translated() &&
                form->field(QStringLiteral("screen-recording.notify-after-export-completes"))
                        ->label() == QStringLiteral("Translated: Notify after export completes") &&
                form->field(QStringLiteral("screen-recording.delete-files-on-recording-close"))
                        ->label() ==
                    QStringLiteral("Translated: Delete files on recording close") &&
                form->field(QStringLiteral("screen-recording.frame-rate"))->label() ==
                    QStringLiteral("Translated: Frame rate") &&
                form->findChild<AdSelect*>(QStringLiteral("screen-recording.encoder"))
                        ->options()
                        .first()
                        .label == QStringLiteral("Translated: H.264 (Hardware)") &&
                form->findChild<AdSelect*>(QStringLiteral("screen-recording.encoder"))
                        ->options()
                        .at(1)
                        .label == QStringLiteral("Translated: H.265 (Hardware)"),
            "open recording preferences must retranslate their title, labels and options");
    QCoreApplication::removeTranslator(&translator);
    QCoreApplication::processEvents();
    statusPrompts->setChecked(false);
    require(!snow_shot::storage::RecordingSettings().showStatusPrompts(),
            "recording settings prompt switch persists immediately");
    statusPrompts->setChecked(true);
    autoHide->setChecked(true);
    require(snow_shot::storage::RecordingSettings().autoHideToolbar(),
            "recording settings auto-hide switch persists immediately");
    autoHide->setChecked(false);
    notifyExport->setChecked(true);
    require(snow_shot::storage::RecordingSettings().notifyAfterExportCompletes(),
            "recording settings notification switch persists immediately");
    notifyExport->setChecked(false);
    deleteOnClose->setChecked(true);
    require(snow_shot::storage::RecordingSettings().deleteFilesOnRecordingClose(),
            "recording settings delete-on-close switch persists immediately");
    deleteOnClose->setChecked(false);
    if (const QString preview = qEnvironmentVariable("SNOW_TEST_RECORDING_SETTINGS_PREVIEW");
        !preview.isEmpty()) {
        require(modal->contentWidget()->window()->grab().save(preview),
                "save recording settings popup preview");
    }
    const auto items = form->items();
    for (int index = 0; index < items.size(); index += 2) {
        const auto left = items[index]->geometry();
        require(form->rect().contains(left) &&
                    (index == 0 || items[index - 2]->geometry().bottom() < left.top()),
                "all recording preference rows must fit without overlap or scrolling");
        if (index + 1 < items.size()) {
            const auto right = items[index + 1]->geometry();
            require(left.top() == right.top() && left.right() < right.left() &&
                        qAbs(left.width() - right.width()) <= 1 && form->rect().contains(right),
                    "paired recording preferences must use two equally sized columns");
        }
    }
    auto* captureToolbar =
        form->findChild<AdSwitch*>(QStringLiteral("screen-recording.capture-toolbar"));
    require(captureToolbar != nullptr &&
                modal->contentWidget()->rect().contains(captureToolbar->mapTo(
                    modal->contentWidget(), captureToolbar->rect().center())) &&
                captureToolbar->toolTip() ==
                    registry.field(QStringLiteral("screen-recording.capture-toolbar"))
                        ->definition->description.translated(),
            "the system recording preference and its description must remain reachable");
    require(modal->contentWidget()->rect().contains(
                autoHide->mapTo(modal->contentWidget(), autoHide->rect().center())) &&
                form->field(QStringLiteral("screen-recording.auto-hide-toolbar"))->label() ==
                    registry.field(QStringLiteral("screen-recording.auto-hide-toolbar"))
                        ->definition->title.translated() &&
                form->field(QStringLiteral("screen-recording.auto-hide-toolbar"))->tooltipText() ==
                    registry.field(QStringLiteral("screen-recording.auto-hide-toolbar"))
                        ->definition->description.translated(),
            "auto-hide toolbar and its description remain reachable in the compact settings form");
    const auto select = [form](const char* id, const QVariant& value) {
        auto* control = form->findChild<AdSelect*>(QString::fromLatin1(id));
        require(control != nullptr, "recording select must exist");
        control->setCurrentValue(value);
    };
    select("screen-recording.clarity", QStringLiteral("4k"));
    select("screen-recording.frame-rate", 60);
    select("screen-recording.animated-image-clarity", QStringLiteral("1080p"));
    select("screen-recording.animated-image-frame-rate", 15);
    select("screen-recording.encoder", QStringLiteral("h265_hw"));
    select("screen-recording.encoding-preset", QStringLiteral("medium"));
    auto* quality = form->findChild<AdSlider*>(QStringLiteral("screen-recording.video-quality"));
    require(quality != nullptr && quality->minimum() == 0 && quality->maximum() == 100,
            "recording quality must use the shared schema range");
    const int originalQuality = snow_shot::storage::RecordingSettings().videoQuality();
    quality->setValue(65);
    require(snow_shot::storage::RecordingSettings().videoQuality() == originalQuality,
            "recording quality must stay local until the adjustment finishes");
    require(QMetaObject::invokeMethod(quality, "editingFinished", Qt::DirectConnection),
            "complete the recording quality adjustment");
    const auto toggle = [form](const char* id, bool value) {
        auto* control = form->findChild<AdSwitch*>(QString::fromLatin1(id));
        require(control != nullptr, "recording toggle must exist");
        control->setChecked(value);
    };
    toggle("screen-recording.loop-animated-images", false);
    toggle("screen-recording.separate-audio-tracks", true);
    toggle("screen-recording.capture-toolbar", false);
    toggle("screen-recording.auto-hide-toolbar", true);
    const snow_shot::storage::RecordingSettings saved;
    require(saved.screenRecordingClarity() == QStringLiteral("4k") && saved.frameRate() == 60 &&
                saved.animatedImageClarity() == QStringLiteral("1080p") &&
                saved.animatedImageFrameRate() == 15 &&
                saved.encoder() == QStringLiteral("h265_hw") &&
                saved.encodingPreset() == QStringLiteral("medium") && saved.videoQuality() == 65 &&
                !saved.loopAnimatedImages() && saved.separateAudioTracks() &&
                !saved.captureToolbarInRecording() && saved.autoHideToolbar(),
            "every recording popup editor must persist through the shared settings backend");
    require(saved.setFrameRate(30) && saved.setAutoHideToolbar(false),
            "external settings write must succeed");
    QCoreApplication::processEvents();
    require(form->findChild<AdSelect*>(QStringLiteral("screen-recording.frame-rate"))
                        ->currentValue()
                        .toInt() == 30 &&
                !autoHide->isChecked(),
            "an open recording form must follow settings changed elsewhere");
    QPointer<AdModal> retired(modal);
    require(saved.setAutoHideToolbar(true),
            "enable auto-hide before closing recording preferences");
    modal->accept();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(retired.isNull(), "closing recording settings must release its form and popup windows");
    require(!recordingArea->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                moveHandle->isVisible(),
            "closing recording preferences must restore region controls");
    setRecordingToolbarAutoHideTestCursor(*recordingToolbar, outside);
    require(retreat->isActive(),
            "closing real recording preferences releases auto-hide protection");
    require(saved.setAutoHideToolbar(false), "restore auto-hide before opening effect settings");
    toolbarPalette->findChild<AdButton*>(QStringLiteral("screenRecordingEffectSettings"))->click();
    QCoreApplication::processEvents();
    auto* effects =
        toolbarPalette->findChild<AdModal*>(QStringLiteral("screenRecordingEffectSettingsModal"));
    require(effects && effects->isOpen() && effects->ownerWindow() == recordingArea &&
                recordingArea->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !moveHandle->isVisible(),
            "effect settings must suspend the same recording input and region controls");
    require(saved.setAutoHideToolbar(true), "enable auto-hide while effect settings are open");
    setRecordingToolbarAutoHideTestCursor(*recordingToolbar, outside);
    require(recordingToolbar->isVisible() && !retreat->isActive(),
            "real recording effect settings inhibit auto-hide outside the toolbar");
    effects->accept();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(!recordingArea->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                moveHandle->isVisible(),
            "closing effect settings must restore region controls");
    setRecordingToolbarAutoHideTestCursor(*recordingToolbar, outside);
    require(retreat->isActive(), "closing recording effect settings releases auto-hide protection");
    require(saved.setAutoHideToolbar(false), "restore auto-hide after modal protection checks");
    controller.open(QRect(200, 150, 320, 240));
    button->click();
    modal = controller.findChild<AdModal*>(QStringLiteral("screenRecordingSettingsModal"));
    require(modal != nullptr && modal->isOpen(), "recording settings must reopen after dismissal");
    requireModalCenteredOnArea(modal, modal->ownerWindow()->frameGeometry(),
                               modal->ownerWindow()->screen());
    require(modal->contentWidget()
                    ->findChild<AdSelect*>(QStringLiteral("screen-recording.frame-rate"))
                    ->currentValue()
                    .toInt() == 30,
            "reopening recording settings must restore current saved preferences");
    toolbarPalette->setActiveTool(ScreenshotToolPalette::Tool::Shape);
    require(!modal->isOpen(), "switching to drawing must dismiss recording preferences");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    toolbarPalette->findChild<AdButton*>(QStringLiteral("screenRecordingExportSettings"))->click();
    button->click();
    modal = controller.findChild<AdModal*>(QStringLiteral("screenRecordingSettingsModal"));
    retired = modal;
    const auto expectedExclusions = prepareExpectedRecordingExclusions(
        toolbarPalette, snow_shot::storage::RecordingSettings().captureToolbarInRecording());
    toolbarPalette->recordingMouseHighlightRadiusChanged(72);
    controller.startRecording();
    waitForRecording(controller);
    require(!button->isEnabled() && (retired.isNull() || !retired->isOpen()),
            "recording must disable and dismiss the preferences window");
    require(lastDirectConfig.capture_fps == 30 && lastDirectConfig.maximum_width == 3840 &&
                lastDirectConfig.maximum_height == 2160 &&
                lastDirectConfig.codec == SNOW_CAPTURE_VIDEO_CODEC_H265 &&
                lastDirectConfig.encoder_preference == SNOW_CAPTURE_ENCODER_PREFERENCE_HARDWARE &&
                lastDirectConfig.preset == SNOW_CAPTURE_VIDEO_ENCODING_PRESET_MEDIUM &&
                lastDirectConfig.quality == 65 && lastDirectConfig.loop_animated_images == 0 &&
                lastDirectConfig.audio_mode == SNOW_CAPTURE_RECORDING_AUDIO_SEPARATE &&
                lastDirectConfig.mouse_highlight_radius == 72 &&
                lastExcludedWindows == expectedExclusions,
            "the next recording must use preferences saved in the popup");
    toolbarPalette->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void recordingActionToolbarPreferenceUpdatesOpenWindow() {
    namespace layout = snow_shot::presentation::toolbar_layout;
    namespace storage = snow_shot::storage;
    const auto kind = storage::ScreenshotToolbarLayoutKind::RecordingActionTools;
    const storage::ScreenshotToolbarSettings toolbarSettings;
    const auto original = toolbarSettings.layout(kind);
    const auto drawing = toolbarSettings.layout(storage::ScreenshotToolbarLayoutKind::DrawingTools);
    require(toolbarSettings.setLayout(kind, {}), "prepare the default recording action layout");
    ScreenRecordingToolbarWindow toolbar;
    toolbar.placeForRecordingRegion(QRect(80, 80, 320, 240));
    toolbar.showWithoutActivating();
    auto* controls = toolbar.palette();
    QCoreApplication::processEvents();
    auto* duration = controls->findChild<QLabel*>(QStringLiteral("screenRecordingDuration"));
    require(duration && duration->isVisible(),
            "a new recording window reads default toolbar settings");
    const QString start = QStringLiteral("start-stop");
    const QString copy = QStringLiteral("copy");
    const QString pause = QStringLiteral("pause-resume");
    auto hidden = layout::defaultOrder(kind);
    for (const auto& id : {start, copy, pause})
        hidden.removeAll(id);
    require(toolbarSettings.setLayout(kind, {{{copy, start}, {pause}}, hidden}),
            "commit a recording layout while its toolbar is open");
    QCoreApplication::processEvents();
    auto* group = controls->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotActionToolGroupButton0"));
    require(group && group->isVisible() && group->property("screenshotToolbarItemId") == start &&
                duration->isHidden() &&
                toolbarSettings.layout(storage::ScreenshotToolbarLayoutKind::DrawingTools) ==
                    drawing,
            "an open recording window applies grouped and hidden action preferences immediately");
    controls->setRecordingSession(ScreenshotToolPalette::RecordingSessionStatus::recording());
    require(group->accessibleName() == QStringLiteral("Stop recording") && duration->isHidden(),
            "live preferences survive recording state transitions");
    require(toolbarSettings.setLayout(kind, {{}, layout::defaultOrder(kind)}),
            "hide every recording action while its toolbar is open");
    QCoreApplication::processEvents();
    require(!controls->findChild<adqt::widgets::AdButton*>(
                QStringLiteral("screenshotActionToolGroupButton0")) &&
                controls->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingStop"))
                    ->isHidden() &&
                controls->canActivateRecordingShortcut(QStringLiteral("export")),
            "live hiding removes recording actions while preserving applicable shortcuts");
    require(toolbarSettings.setLayout(kind, {}), "reset the live recording action layout");
    QCoreApplication::processEvents();
    require(duration->isVisible() &&
                controls->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingStop"))
                    ->isVisible(),
            "resetting preferences restores the active phase's default controls immediately");
    require(toolbarSettings.setLayout(kind, original), "restore the recording action preferences");
}

void recordingModalStacking() {
    using namespace adqt::widgets;
    const auto wait = [](const std::function<bool()>& condition, const char* message) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents,
                                            100);
        require(condition(), message);
    };
    ScreenRecordingController controller(testEffectsSource);
    controller.open(QRect(80, 80, 320, 240));
    auto* controls = palette();
    auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(controls->window());
    controls->findChild<AdButton*>(QStringLiteral("screenRecordingSettings"))->click();
    auto* settings = controller.findChild<AdModal*>(QStringLiteral("screenRecordingSettingsModal"));
    require(settings && settings->isOpen(), "recording settings must open before rendering");
    QPointer<QWidget> area = settings->ownerWindow();
    require(area, "recording modals must have a recording area to cover");
    const QRect anchor = area->frameGeometry();
    QScreen* screen = area->screen();
    const auto verify = [&](AdModal* modal) {
        auto* surface = modal->contentWidget()->window();
        area->raise();
        toolbar->raise();
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() == QStringLiteral("cocoa"))
            wait(
                [&] {
                    // Drive Cocoa's native modal session through its idle boundary.
                    QEventLoop loop;
                    QObject::connect(QAbstractEventDispatcher::instance(),
                                     &QAbstractEventDispatcher::aboutToBlock, &loop,
                                     &QEventLoop::quit, Qt::QueuedConnection);
                    loop.exec();
                    return macosRecordingModalAboveControls(surface, area, toolbar);
                },
                modal == settings
                    ? "native recording settings must stay above the area and toolbar after raises"
                    : "native render progress must stay above the area and toolbar after raises");
#endif
        require(modal->ownerWindow() == area && !modal->windowModeDetached() &&
                    modal->mode() == AdModal::Mode::Window &&
                    modal->windowModality() == Qt::ApplicationModal &&
                    surface->windowHandle()->transientParent() == area->windowHandle(),
                "settings and render progress must share recording area ownership and modality");
        require(QApplication::activeModalWidget() == surface,
                "the visible recording modal must block recording controls");
        requireModalCenteredOnArea(modal, anchor, screen);
    };
    verify(settings);
    settings->accept();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    controls->recordingPostProcessingEnabledChanged(true);
    controller.startRecording();
    waitForRecording(controller);
    controls->recordingStopRequested();
    QPointer<AdModal> modal;
    wait(
        [&] {
            modal = controller.findChild<AdModal*>(QStringLiteral("screenRecordingRenderModal"));
            return modal && modal->isOpen() && renderPolls > 0;
        },
        "deferred finalization must open render progress");
    verify(modal);
    modal->footerWidget()
        ->findChild<AdButton*>(QStringLiteral("screenRecordingRenderCancel"))
        ->click();
    wait(
        [&] {
            return controller.automationState().value(QStringLiteral("source_retained")).toBool();
        },
        "canceling rendering must retain the source");
    verify(modal);
    const int previousStarts = renderStarts;
    modal->footerWidget()
        ->findChild<AdButton*>(QStringLiteral("screenRecordingRenderRetry"))
        ->click();
    wait([&] { return renderStarts > previousStarts; }, "retry must start another render attempt");
    verify(modal);
    controls->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(!controller.isOpen() && !area && modal && modal->isOpen() &&
                modal->windowModeDetached() && !modal->ownerWindow() &&
                !modal->contentWidget()->window()->windowHandle()->transientParent(),
            "rendering must detach before recording area destruction and remain usable");
    requireModalCenteredOnArea(modal, anchor, screen);
    renderPercent = 53;
    auto* progress = modal->contentWidget()->findChild<AdProgress*>(
        QStringLiteral("screenRecordingRenderProgress"));
    wait([&] { return progress->percent() == 53; },
         "detached progress must continue updating after the area closes");
    renderState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
    renderPercent = 100;
    waitForIdle(controller);
    require(!modal || !modal->isOpen(), "successful rendering must close the detached modal");
}

void recordingRenderLayout() {
    using namespace adqt::widgets;
    const auto wait = [](const std::function<bool()>& condition, const char* message) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents,
                                            100);
        require(condition(), message);
    };
    const QFont previousFont = QApplication::font();
    QApplication::setFont(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
    const QByteArray previousPath = renderSourcePath;
    for (const auto& path :
         {QByteArray("/Users/chao/Movies/b6180d62-5b37-4800-8883-0b5d83aa4c1d.snowrec"),
          QByteArray("/Users/chao/Movies/Recordings saved for later/Project recordings/"
                     "b6180d62-5b37-4800-8883-0b5d83aa4c1d.snowrec")}) {
        renderSourcePath = path;
        RecordingRenderJob job(new SnowRecordingSource, true, nullptr, nullptr);
        job.start();
        auto* modal = job.findChild<AdModal*>(QStringLiteral("screenRecordingRenderModal"));
        require(modal && modal->isOpen(), "render layout test must open its modal");
        auto* progress = modal->contentWidget()->findChild<AdProgress*>(
            QStringLiteral("screenRecordingRenderProgress"));
        wait([&] { return renderState == SNOW_RECORDING_RENDER_STATE_RUNNING && renderPolls > 0; },
             "render layout test must start its task");
        renderPercent = 45;
        wait([&] { return progress->percent() == 45; }, "render progress must update");
        modal->footerWidget()
            ->findChild<AdButton*>(QStringLiteral("screenRecordingRenderCancel"))
            ->click();
        const auto retained = [&] {
            return job.state().value(QStringLiteral("source_retained")).toBool();
        };
        wait(retained, "canceling must show the retained source layout");

        const auto verifyLayout = [&] {
            QCoreApplication::processEvents();
            auto* surface = modal->contentWidget()->window();
            auto* panel = surface->findChild<QWidget*>(QStringLiteral("ad-modal-panel"));
            require(panel && surface->rect().contains(panel->geometry()),
                    "the render panel must fit entirely within its modal window");
            auto* footer = modal->footerWidget()->parentWidget();
            const int bottomInset = footer->layout()->contentsMargins().bottom();
            const int footerBottom = modal->footerWidget()->mapTo(surface, QPoint(0, 0)).y() +
                                     modal->footerWidget()->height();
            require(bottomInset > 0 && surface->height() - footerBottom >= bottomInset,
                    "render actions must preserve the modal bottom padding");
            for (auto* label : modal->contentWidget()->findChildren<QLabel*>()) {
                require(!label->isVisible() ||
                            label->height() >= label->heightForWidth(label->width()),
                        "render labels must have enough height for their wrapped text");
            }
            for (auto* button : modal->footerWidget()->findChildren<AdButton*>()) {
                require(!button->isVisible() ||
                            modal->footerWidget()->rect().contains(button->geometry()),
                        "every render action must fit within the footer");
            }
        };
        const auto verifyLanguages = [&](const QString& phase) {
            for (const auto* locale : {"en_US", "zh_CN", "zh_TW"}) {
                QTranslator translator;
                require(translator.load(QDir(QStringLiteral(SNOW_SHOT_TEST_TRANSLATIONS_DIR))
                                            .filePath(QStringLiteral("snow_shot_%1.qm")
                                                          .arg(QString::fromLatin1(locale)))),
                        "load the complete render dialog catalog");
                QCoreApplication::installTranslator(&translator);
                verifyLayout();
                const QSize size = modal->contentWidget()->window()->size();
                modal->open();
                modal->open();
                verifyLayout();
                require(modal->contentWidget()->window()->size() == size,
                        "repeated render dialog refreshes must preserve its fitted size");
                if (const QString snapshots =
                        qEnvironmentVariable("SNOW_RECORDING_RENDER_SNAPSHOT_DIR");
                    !snapshots.isEmpty()) {
                    QDir().mkpath(snapshots);
                    const QString name = QStringLiteral("%1-%2-%3.png")
                                             .arg(phase, QString::fromLatin1(locale))
                                             .arg(path.contains("Project") ? 1 : 0);
                    require(modal->contentWidget()->window()->grab().save(
                                QDir(snapshots).filePath(name)),
                            "render dialog layout snapshot must save");
                }
                QCoreApplication::removeTranslator(&translator);
            }
        };
        verifyLanguages(QStringLiteral("canceled"));
        const int retainedHeight = modal->contentWidget()->window()->height();
        modal->footerWidget()
            ->findChild<AdButton*>(QStringLiteral("screenRecordingRenderRetry"))
            ->click();
        wait([&] { return renderState == SNOW_RECORDING_RENDER_STATE_RUNNING; },
             "retry must start a fresh render task");
        verifyLayout();
        require(modal->contentWidget()->window()->height() < retainedHeight,
                "retry must shrink the dialog after hiding source details");
        renderState = SNOW_RECORDING_RENDER_STATE_FAILED;
        wait(retained, "render failure must show source details again");
        verifyLanguages(QStringLiteral("failed"));
        require(job.release(false), "layout test must keep its source");
        wait([&] { return !modal->isOpen(); }, "layout test must finish source cleanup");
    }
    renderSourcePath = previousPath;
    QApplication::setFont(previousFont);
}

void recordingPostProcessingLifecycle() {
    using namespace adqt::widgets;
    using snow_shot::storage::RecordingSettings;
    const RecordingSettings settings;
    require(!settings.postProcessingEnabled() &&
                settings.postProcessingEffect() == QStringLiteral("progress_bar") &&
                settings.progressBarColor() == QColor(22, 119, 255),
            "post processing defaults preserve real-time recording");
    const auto wait = [](const std::function<bool()>& condition, const char* message) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents,
                                            100);
        require(condition(), message);
    };
    {
        ScreenRecordingController controller(testEffectsSource);
        int finalizedCount = 0;
        QObject::connect(&controller, &ScreenRecordingController::finalized, &controller,
                         [&] { ++finalizedCount; });
        controller.open(QRect(80, 80, 320, 240));
        auto* controls = palette();
        auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(controls->window());
        ScreenRecordingAreaWindow* area = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget))
                area = candidate;
        }
        require(area != nullptr, "rendering must retain the recording area");
        WindowInputBlockObserver areaInput;
        WindowInputBlockObserver toolbarInput;
        area->installEventFilter(&areaInput);
        toolbar->installEventFilter(&toolbarInput);
        pumpPreview();
        const QImage withoutPlayback = previewImage(*area->canvas());
        const QPoint trackPoint(withoutPlayback.width() / 2, withoutPlayback.height() - 2);
        controls->recordingPostProcessingEnabledChanged(true);
        // macOS resolves the initial native output dimensions asynchronously.
        // Four zero-duration event pumps need not reach that completion timer.
        wait(
            [&] {
                return previewImage(*area->canvas()).pixelColor(trackPoint) !=
                       withoutPlayback.pixelColor(trackPoint);
            },
            "enabling progress bar must display its zero-second track after preview sizing");
        controls->recordingPostProcessingEffectChanged(QStringLiteral("playback_time"));
        pumpPreview();
        const QImage withTime = previewImage(*area->canvas());
        require(withTime != withoutPlayback &&
                    withTime.pixelColor(trackPoint) == withoutPlayback.pixelColor(trackPoint),
                "selecting play time must immediately replace the track with the zero timer");
        controls->recordingProgressBarColorChanged(QColor(20, 100, 200, 128));
        require(settings.postProcessingEnabled() &&
                    settings.postProcessingEffect() == QStringLiteral("playback_time") &&
                    settings.progressBarColor() == QColor(20, 100, 200, 128),
                "controller must persist every post-processing control");
        require(settings.setPostProcessingEnabled(false) &&
                    settings.setPostProcessingEffect(QStringLiteral("progress_bar")) &&
                    settings.setProgressBarColor(QColor(22, 119, 255)) &&
                    !controls->recordingPostProcessingEnabled() &&
                    controls->recordingPostProcessingEffect() == QStringLiteral("progress_bar") &&
                    controls->recordingProgressBarColor() == QColor(22, 119, 255),
                "an open toolbar follows externally reset post-processing preferences");
        pumpPreview();
        require(previewImage(*area->canvas()) == withoutPlayback,
                "disabling post processing through settings must clear the preview overlay");
        controls->recordingPostProcessingEnabledChanged(true);
        controls->recordingPostProcessingEffectChanged(QStringLiteral("playback_time"));
        controls->recordingProgressBarColorChanged(QColor(20, 100, 200, 128));
        const int oldStarts = renderStarts.load();
        controller.startRecording();
        waitForRecording(controller);
        require(lastDeferredOptions.overlay == SNOW_RECORDING_PLAYBACK_OVERLAY_PLAYBACK_TIME &&
                    lastDeferredOptions.progress_bar_rgba == 0x1464c880 &&
                    !controls->recordingBusy(),
                "automatic mode snapshots playback effect and RGBA color");
        controller.stopRecordingAndCopy();
        wait([&] { return renderStarts > oldStarts; }, "stopping must start deferred rendering");
        QPointer<AdModal> modal =
            controller.findChild<AdModal*>(QStringLiteral("screenRecordingRenderModal"));
        require(modal && modal->isOpen() && !modal->windowModeDetached() &&
                    modal->windowModality() == Qt::ApplicationModal && modal->ownerWindow() == area,
                "rendering must open an application modal anchored to the recording area");
        require(areaInput.blocked && toolbarInput.blocked,
                "render progress must block clicks on the recording area and toolbar");
        auto* renderWindow = modal->contentWidget()->window();
        const QRect recordingAreaGeometry = area->frameGeometry();
        QScreen* recordingScreen = area->screen();
        requireModalCenteredOnArea(modal, recordingAreaGeometry, recordingScreen);
        require(renderWindow->windowFlags().testFlag(Qt::WindowStaysOnTopHint),
                "render progress must stay above the recording area and toolbar");
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            const HWND renderHandle = reinterpret_cast<HWND>(renderWindow->winId());
            for (auto* window : {static_cast<QWidget*>(area), static_cast<QWidget*>(toolbar)}) {
                const HWND handle = reinterpret_cast<HWND>(window->winId());
                require(!IsWindowEnabled(handle),
                        "render progress must disable native recording window input");
                bool renderAboveWindow = false;
                for (HWND candidate = GetWindow(handle, GW_HWNDPREV); candidate;
                     candidate = GetWindow(candidate, GW_HWNDPREV))
                    renderAboveWindow |= candidate == renderHandle;
                require(renderAboveWindow,
                        "native render progress must stay above both recording windows");
            }
        }
#endif
        auto* progress = modal->contentWidget()->findChild<AdProgress*>(
            QStringLiteral("screenRecordingRenderProgress"));
        require(progress && controls->recordingBusyOperation() ==
                                ScreenshotToolPalette::RecordingBusyOperation::Copying,
                "rendering uses AdProgress and preserves copy intent");
        require(progress->formattedText() == QStringLiteral("0%"),
                "render modal must start with a whole percentage");
        renderPercent = 41.75f;
        wait([&] { return progress->percent() == 41; }, "native progress must reach AdProgress");
        require(progress->formattedText() == QStringLiteral("41%") &&
                    controller.automationState().value(QStringLiteral("render_progress")) == 41.75,
                "render modal must show whole percentages while retaining precise native progress");
        class RenderTranslator final : public QTranslator {
          public:
            bool isEmpty() const override {
                return false;
            }
            QString translate(const char* context, const char* source, const char*,
                              int) const override {
                return QByteArray(context) == "RecordingRenderDialog"
                           ? QStringLiteral("Translated: ") + QString::fromUtf8(source)
                           : QString();
            }
        } translator;
        QCoreApplication::installTranslator(&translator);
        QCoreApplication::processEvents();
        require(modal->windowTitle() == QStringLiteral("Translated: Rendering recording") &&
                    progress->accessibleName() == QStringLiteral("Translated: Rendering progress"),
                "open render dialog must retranslate without replacing its progress");
        QCoreApplication::removeTranslator(&translator);
        QCoreApplication::processEvents();
        const QString snapshots = qEnvironmentVariable("SNOW_RECORDING_RENDER_SNAPSHOT_DIR");
        if (!snapshots.isEmpty()) {
            QDir().mkpath(snapshots);
            require(modal->contentWidget()->window()->grab().save(
                        QDir(snapshots).filePath(QStringLiteral("rendering.png"))),
                    "rendering dialog snapshot must save");
        }
        renderPercent = 20;
        QElapsedTimer monotonic;
        monotonic.start();
        while (monotonic.elapsed() < 250)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        require(progress->percent() == 41, "render progress must remain monotonic");
        renderPercent = 99.75f;
        wait([&] { return progress->percent() == 99; },
             "incomplete native progress must not display 100 percent");
        require(progress->formattedText() == QStringLiteral("99%"),
                "render modal must omit decimals near completion");
        controls->recordingCloseRequested();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(!controller.isOpen() && modal->isOpen() && modal->windowModeDetached() &&
                    !modal->ownerWindow(),
                "closing recording selection must detach and preserve the render modal");
        auto* cancel = modal->footerWidget()->findChild<AdButton*>(
            QStringLiteral("screenRecordingRenderCancel"));
        cancel->click();
        wait(
            [&] {
                return controller.automationState()
                    .value(QStringLiteral("source_retained"))
                    .toBool();
            },
            "canceling waits for task disposal and retains source");
        require(controller.isRecording() &&
                    !controller.automationState().value(QStringLiteral("finalized")).toBool(),
                "cancelled output cannot be finalized or copied");
        requireModalCenteredOnArea(modal, recordingAreaGeometry, recordingScreen);
        if (!snapshots.isEmpty())
            require(modal->contentWidget()->window()->grab().save(
                        QDir(snapshots).filePath(QStringLiteral("canceled.png"))),
                    "retained source dialog snapshot must save");
        const int discardedBefore = sourceDiscards.load();
        const int retryBefore = renderStarts.load();
        modal->footerWidget()
            ->findChild<AdButton*>(QStringLiteral("screenRecordingRenderRetry"))
            ->click();
        wait([&] { return renderStarts > retryBefore; },
             "retry must render the same retained source");
        requireModalCenteredOnArea(modal, recordingAreaGeometry, recordingScreen);
        require(deferredCreates == 1 && sourceDiscards == discardedBefore &&
                    lastDeferredOptions.overlay == SNOW_RECORDING_PLAYBACK_OVERLAY_PLAYBACK_TIME,
                "retry must retain immutable options and avoid recapturing or deleting source");
        renderState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
        renderPercent = 100;
        waitForIdle(controller);
        require(controller.automationState().value(QStringLiteral("finalized")).toBool() &&
                    controller.automationState().value(QStringLiteral("duration_ms")).toInteger() ==
                        2300 &&
                    sourceDiscards == discardedBefore && (!modal || !modal->isOpen()),
                "successful publication completes the job without repeating backend cleanup");
        require(finalizedCount == 1 && !QApplication::clipboard()->mimeData()->urls().isEmpty(),
                "successful retry must preserve the original copy request");
        controller.open(QRect(80, 80, 320, 240));
        controls = palette();
        controls->recordingStartDelaySecondsChanged(1);
        controller.startRecording();
        QString error;
        require(controller.automationState().value(QStringLiteral("operation")).toString() ==
                        QStringLiteral("counting_down") &&
                    !controller.automationState().value(QStringLiteral("finalized")).toBool() &&
                    !controller.controlAutomation(QStringLiteral("copy"), {}, &error),
                "accepting a new countdown clears previous output copy eligibility");
        controls->recordingCloseRequested();
        settings.setStartDelaySeconds(0);
    }
    settings.setPostProcessingEnabled(false);
    settings.setPostProcessingEffect(QStringLiteral("progress_bar"));
    settings.setProgressBarColor(QColor(22, 119, 255));
    for (const bool deferred : {false, true}) {
        QTemporaryDir destination;
        require(destination.isValid(), "completion observer output directory must exist");
        const QString expectedPath = destination.filePath(QStringLiteral("completed.mp4"));
        QApplication::clipboard()->clear();
        QPointer<ScreenRecordingController> controller =
            new ScreenRecordingController(testEffectsSource);
        QObject::connect(controller, &ScreenRecordingController::finalized, [&] {
            require(controller->automationState().value(QStringLiteral("path")).toString() ==
                        expectedPath,
                    "finalized observers must see the successfully published output path");
            delete controller.data();
        });
        QString error;
        require(controller->startAutomation(QRect(80, 80, 320, 240),
                                            {{QStringLiteral("post_processing"), deferred},
                                             {QStringLiteral("microphone"), true},
                                             {QStringLiteral("microphone_gain_db"), -6},
                                             {QStringLiteral("system_audio_gain_db"), 9},
                                             {QStringLiteral("format"), QStringLiteral("mp4")},
                                             {QStringLiteral("path"), expectedPath}},
                                            &error),
                "prepare a completion observer that synchronously destroys its controller");
        waitForRecording(*controller);
        require(lastDirectConfig.microphone_gain_db == -6 &&
                    lastDirectConfig.system_audio_gain_db == 9,
                "both recording workflows receive independent initial audio gains");
        auto* microphone = palette()->recordingAudioGainPopover(true);
        microphone->openAndFocus();
        wait([] { return audioMeterMask == 2; },
             "both recording workflows meter the selected audio source");
        auto* slider = microphone->popover()->contentWidget()->findChild<adqt::widgets::AdSlider*>(
            QStringLiteral("recordingAudioGainSlider"));
        require(slider != nullptr, "live recording audio slider exists");
        slider->setValue(12);
        require(liveMicrophoneGain == 12 && liveSystemGain == 9,
                "live gain changes reach both workflows without altering the other source");
        require(controller->controlAutomation(QStringLiteral("copy"), {}, &error),
                "Copy must survive finalized observer destruction for both export workflows");
        if (deferred) {
            wait(
                [&] {
                    return controller && controller->automationState()
                                                 .value(QStringLiteral("render_duration_ms"))
                                                 .toInteger() == 2300;
                },
                "deferred completion must adopt its render task before the terminal snapshot");
            renderState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
            renderPercent = 100;
        }
        wait(
            [&] {
                return !controller && QApplication::clipboard()->mimeData()->urls() ==
                                          QList<QUrl>{QUrl::fromLocalFile(expectedPath)};
            },
            "completed Copy uses its immutable path after a finalized observer deletes the owner");
    }
    {
        wait(
            [] {
                return snow_shot::runtime::RuntimeActivityTracker::shared()
                           .snapshot()
                           .activeCount == 0;
            },
            "earlier recording cleanup retires before the native session lifetime barrier");
        auto controller = std::make_unique<ScreenRecordingController>(testEffectsSource);
        QString error;
        require(controller->startAutomation(QRect(80, 80, 320, 240),
                                            {{QStringLiteral("post_processing"), true}}, &error),
                "prepare a deferred source finalization barrier");
        waitForRecording(*controller);
        require(controller->blocksMemoryTrimming(), "an active native recording blocks trimming");
        requireMemoryAdmissionBlocked(
            "the live native recording session retains admission after its startup worker retires");
        std::promise<void> releaseExport;
        std::promise<void> enteredPromise;
        std::promise<void> releaseSessionDestroy;
        sessionDestroyGate = releaseSessionDestroy.get_future().share();
        sessionDestroyEntered = false;
        auto entered = enteredPromise.get_future();
        exportGate = releaseExport.get_future().share();
        exportEntered = &enteredPromise;
        const int oldSessions = destroyedSessions.load();
        const int oldSources = sourceDestroys.load();
        const int oldDiscards = sourceDiscards.load();
        require(controller->controlAutomation(QStringLiteral("stop"), {}, &error),
                "deferred stop must enter the controlled finalization worker");
        require(entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "the deferred finalization must reach its barrier");
        controller.reset();
        require(exportGate.wait_for(std::chrono::seconds(0)) == std::future_status::timeout &&
                    destroyedSessions == oldSessions,
                "controller destruction never joins or releases a pending source finalization");
        requireMemoryAdmissionBlocked("detached native session finalization retains memory "
                                      "admission after owner destruction");
        releaseExport.set_value();
        exportEntered = nullptr;
        exportGate = {};
        wait([] { return sessionDestroyEntered.load(); },
             "retired native session reaches its actual destruction barrier");
        requireMemoryAdmissionBlocked(
            "native session destruction retains memory admission after finalization finishes");
        releaseSessionDestroy.set_value();
        wait([&] { return sourceDestroys > oldSources && destroyedSessions > oldSessions; },
             "retired finalization eventually releases the session and preserves source media");
        sessionDestroyGate = {};
        wait(
            [] {
                return snow_shot::runtime::RuntimeActivityTracker::shared()
                           .snapshot()
                           .activeCount == 0;
            },
            "native session cleanup releases its last memory reservation after destruction");
        require(sourceDiscards == oldDiscards,
                "destroying a controller never implicitly discards its finalized source");
    }
    for (const bool discard : {false, true}) {
        ScreenRecordingController controller(testEffectsSource);
        QString error;
        const int oldStarts = renderStarts.load();
        const int oldDiscards = sourceDiscards.load();
        require(controller.startAutomation(
                    QRect(80, 80, 320, 240),
                    {{QStringLiteral("post_processing"), true},
                     {QStringLiteral("post_processing_effect"), QStringLiteral("progress_bar")},
                     {QStringLiteral("progress_bar_color"), QStringLiteral("#1464C880")}},
                    &error),
                "automation supports isolated post-processing options");
        waitForRecording(controller);
        require(lastDeferredOptions.progress_bar_rgba == 0x1464c880 &&
                    controller.automationState()
                            .value(QStringLiteral("options"))
                            .toObject()
                            .value(QStringLiteral("progress_bar_color")) ==
                        QStringLiteral("#1464C880"),
                "automation progress bar colors use round-trippable RGBA alpha ordering");
        require(controller.controlAutomation(QStringLiteral("stop"), {}, &error),
                "automated recording must stop");
        wait([&] { return renderStarts > oldStarts; }, "automated stop must render");
        require(!controller.findChild<AdModal*>(QStringLiteral("screenRecordingRenderModal")),
                "automated rendering suppresses all dialogs");
        if (!discard) {
            require(controller.controlAutomation(QStringLiteral("cancel"), {}, &error) &&
                        controller.isRecording(),
                    "legacy cancel closes recording UI without canceling a render job");
            require(controller.controlAutomation(QStringLiteral("cancel_render"), {}, &error),
                    "explicit cancel_render cancels the current render attempt");
            wait(
                [&] {
                    return controller.automationState()
                        .value(QStringLiteral("source_retained"))
                        .toBool();
                },
                "cancel_render must retain source after worker teardown");
            const int beforeRetry = renderStarts.load();
            require(controller.controlAutomation(QStringLiteral("retry"), {}, &error),
                    "automation can retry its canceled render");
            wait([&] { return renderStarts > beforeRetry; },
                 "automation retry must start a new attempt");
        }
        renderState = SNOW_RECORDING_RENDER_STATE_FAILED;
        wait(
            [&] {
                return controller.automationState()
                    .value(QStringLiteral("source_retained"))
                    .toBool();
            },
            "render error must retain source and expose failure");
        require(controller.automationState().value(QStringLiteral("error")).toString() ==
                    QStringLiteral("test rendering failure"),
                "owned task error must be exposed without a message box");
        require(!controller.controlAutomation(QStringLiteral("pause"), {}, &error) &&
                    controller.automationState().value(QStringLiteral("error")).toString() ==
                        QStringLiteral("test rendering failure"),
                "an unavailable render action must preserve the retained failure diagnostics");
        if (discard) {
            failSourceDiscard = true;
            require(controller.controlAutomation(QStringLiteral("discard"), {}, &error),
                    "explicit discard begins asynchronously");
            wait(
                [&] {
                    return controller.automationState().value(QStringLiteral("error")).toString() ==
                               QStringLiteral("test backend") &&
                           controller.automationState()
                               .value(QStringLiteral("source_retained"))
                               .toBool();
                },
                "failed source deletion must restore retained ownership");
            require(controller.isRecording(),
                    "failed source deletion must retain the original busy operation");
            failSourceDiscard = false;
        }
        require(controller.controlAutomation(discard ? QStringLiteral("discard")
                                                     : QStringLiteral("keep_source"),
                                             {}, &error),
                "retained source supports explicit keep and discard actions");
        waitForIdle(controller);
        require(sourceDiscards == oldDiscards + (discard ? 1 : 0) &&
                    controller.automationState()
                            .value(QStringLiteral("source_path"))
                            .toString()
                            .isEmpty() == discard &&
                    !settings.postProcessingEnabled(),
                "Keep Source preserves files and unlocks UI without mutating preferences");
        require(!controller.automationState().value(QStringLiteral("finalized")).toBool() &&
                    !controller.controlAutomation(QStringLiteral("copy"), {}, &error),
                "kept or discarded source is never eligible as a completed output");
        require(controller.controlAutomation(QStringLiteral("close"), {}, &error) &&
                    controller.startAutomation(QRect(80, 80, 320, 240), {}, &error),
                "releasing a retained job allows a new recording in the same session");
        waitForRecording(controller);
        require(controller.automationState()
                        .value(QStringLiteral("source_path"))
                        .toString()
                        .isEmpty() &&
                    controller.automationState().value(QStringLiteral("render_mode")).toString() ==
                        QStringLiteral("realtime"),
                "a new recording clears retained UI state and uses saved automatic preferences");
        require(controller.controlAutomation(QStringLiteral("stop"), {}, &error),
                "the next recording stops through the real-time path");
        waitForIdle(controller);
    }
    {
        std::promise<void> releaseStart;
        renderStartGate = releaseStart.get_future().share();
        renderStartEntered = false;
        const int oldTasks = taskDestroys.load();
        const int oldSources = sourceDestroys.load();
        RecordingRenderJob job(new SnowRecordingSource, false, nullptr, nullptr);
        int finished = 0;
        job.finished = [&](RecordingRenderJob::Outcome outcome) {
            require(outcome == RecordingRenderJob::Outcome::Succeeded,
                    "completion notification preserves successful publication");
            ++finished;
        };
        job.start();
        auto* timer = job.findChild<QTimer*>(QStringLiteral("recordingRenderProgressPollTimer"));
        require(timer, "render progress owns its polling timer");
        timer->setInterval(60000);
        wait([] { return renderStartEntered.load(); }, "render start reaches its barrier");
        releaseStart.set_value();
        renderStartGate = {};
        wait([&] { return job.state().value(QStringLiteral("render_duration_ms")) == 2300; },
             "completed render creation is adopted without waiting for a progress timer");
        renderState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
        require(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
                "one progress observation sees completed publication");
        wait([&] { return finished == 1; },
             "task disposal and source release finish without additional progress timer ticks");
        require(taskDestroys == oldTasks + 1 && sourceDestroys == oldSources + 1,
                "completion notifications release native ownership exactly once");
    }
    {
        std::promise<void> releaseStart;
        renderStartGate = releaseStart.get_future().share();
        renderStartEntered = false;
        const int oldSources = sourceDestroys.load();
        const int oldPolls = renderPolls.load();
        auto* job = new RecordingRenderJob(new SnowRecordingSource, false, nullptr, nullptr);
        job->start();
        wait([] { return renderStartEntered.load(); }, "shutdown test holds render creation");
        job->prepareForSessionEnd();
        releaseStart.set_value();
        renderStartGate = {};
        wait([] { return activeRenderTasks > 0; }, "native creation completes during shutdown");
        QCoreApplication::processEvents();
        require(renderPolls == oldPolls && sourceDestroys == oldSources,
                "late completion notifications cannot resume UI work during session shutdown");
        delete job;
        wait([&] { return sourceDestroys > oldSources; },
             "explicit teardown still retires native work after notifications are disabled");
    }
    for (const int mode : {0, 1, 2, 3}) {
        wait(
            [] {
                return snow_shot::runtime::RuntimeActivityTracker::shared()
                           .snapshot()
                           .activeCount == 0;
            },
            "earlier work retires before the render cancellation lifetime barrier");
        auto releaseCancel = std::make_shared<std::promise<void>>();
        renderCancelGate = releaseCancel->get_future().share();
        renderCancelEntered = false;
        renderStartEntered = false;
        const int oldTasks = taskDestroys.load();
        const int oldSources = sourceDestroys.load();
        const int oldDiscards = sourceDiscards.load();
        // Only the test releases this native barrier. Returning while it remains pending proves
        // that cancellation and destruction do not join, independent of machine scheduling.
        // CTest's process timeout detects a regression that blocks before reaching the assertion.
        auto* job = new RecordingRenderJob(new SnowRecordingSource, false, nullptr, nullptr);
        int published = 0;
        job->finished = [&published](RecordingRenderJob::Outcome outcome) {
            published += outcome == RecordingRenderJob::Outcome::Succeeded;
        };
        failRenderPoll = mode == 2;
        job->start();
        if (mode != 2) {
            wait([] { return renderStartEntered.load(); }, "blocked-cancel render must start");
            wait([&] { return job->state().value(QStringLiteral("render_duration_ms")) == 2300; },
                 "the GUI must adopt the completed start before testing native cancellation");
            renderPercent = 41;
            wait([&] { return job->state().value(QStringLiteral("render_progress")) == 41; },
                 "the adopted task must report its updated rendering progress");
        }
        if (mode != 2)
            require(job->cancel() && renderCancelGate.wait_for(std::chrono::seconds(0)) ==
                                         std::future_status::timeout,
                    "Cancel acceptance must never wait on native publication synchronization");
        wait([] { return renderCancelEntered.load(); },
             "native cancellation must reach its barrier");
        bool heartbeat = false;
        QTimer::singleShot(0, [&heartbeat] { heartbeat = true; });
        QCoreApplication::processEvents();
        require(heartbeat &&
                    renderCancelGate.wait_for(std::chrono::seconds(0)) ==
                        std::future_status::timeout &&
                    taskDestroys == oldTasks,
                "blocked cancellation preserves responsive GUI events and a live task lease");
        if (mode == 1) {
            delete job;
            job = nullptr;
            require(
                renderCancelGate.wait_for(std::chrono::seconds(0)) == std::future_status::timeout &&
                    sourceDestroys == oldSources,
                "destruction transfers pending cancellation without joining or releasing source");
            requireMemoryAdmissionBlocked(
                "detached rendering retains memory admission while native cancellation is blocked");
        } else {
            const int previousPolls = renderPolls.load();
            renderState = mode == 3 ? SNOW_RECORDING_RENDER_STATE_SUCCEEDED
                                    : SNOW_RECORDING_RENDER_STATE_CANCELED;
            if (mode != 2)
                wait([&] { return renderPolls > previousPolls; },
                     "terminal polling must run while native cancellation remains blocked");
            require(taskDestroys == oldTasks && sourceDestroys == oldSources,
                    "terminal observation never disposes a task borrowed by pending cancellation");
        }
        releaseCancel->set_value();
        renderCancelGate = {};
        failRenderPoll = false;
        if (mode == 3) {
            wait([&] { return published == 1 && sourceDestroys > oldSources; },
                 "publication that wins cancellation must complete as a successful output");
            require(job->state().value(QStringLiteral("render_progress")) == 100 && !job->retry() &&
                        !job->release(false),
                    "late cancellation cannot expose a published output as retained source");
            delete job;
        } else if (job) {
            wait([&] { return job->state().value(QStringLiteral("source_retained")).toBool(); },
                 "completed cancellation joins its task before exposing retained source actions");
            require(job->release(false), "Keep Source must work after blocked native cancellation");
            wait([&] { return sourceDestroys > oldSources; },
                 "Keep Source eventually releases the preserved source handle");
            delete job;
        } else {
            wait([&] { return sourceDestroys > oldSources; },
                 "detached destruction waits for cancellation before releasing task and source");
        }
        require(taskDestroys == oldTasks + 1 && sourceDiscards == oldDiscards,
                "blocked cancellation disposes exactly once and never deletes source files");
    }
    {
        std::promise<void> releaseStart;
        renderStartGate = releaseStart.get_future().share();
        renderStartEntered = false;
        const int oldDestroyed = sourceDestroys.load();
        auto* job = new RecordingRenderJob(new SnowRecordingSource, false, nullptr, nullptr);
        job->start();
        wait([] { return renderStartEntered.load(); }, "render worker must reach controlled start");
        delete job;
        require(renderStartGate.wait_for(std::chrono::seconds(0)) == std::future_status::timeout &&
                    sourceDestroys == oldDestroyed,
                "destroying the UI must never join a pending render start");
        requireMemoryAdmissionBlocked(
            "detached render start cleanup retains memory admission after UI destruction");
        releaseStart.set_value();
        renderStartGate = {};
        wait([&] { return sourceDestroys > oldDestroyed; },
             "detached cleanup must release task before its preserved source");
    }
    {
        RecordingRenderJob job(new SnowRecordingSource, true, nullptr, nullptr);
        failRenderPoll = true;
        job.start();
        wait([&] { return job.state().value(QStringLiteral("source_retained")).toBool(); },
             "native poll failures must cancel and retain rather than wait forever");
        failRenderPoll = false;
        int kept = 0;
        job.finished = [&kept](RecordingRenderJob::Outcome outcome) {
            kept += outcome == RecordingRenderJob::Outcome::Kept;
        };
        auto* modal = job.findChild<AdModal*>(QStringLiteral("screenRecordingRenderModal"));
        require(!job.error().isEmpty() && modal && modal->isOpen(),
                "poll errors preserve source and show a retained source view");
        if (const QString snapshots = qEnvironmentVariable("SNOW_RECORDING_RENDER_SNAPSHOT_DIR");
            !snapshots.isEmpty())
            require(modal->contentWidget()->window()->grab().save(
                        QDir(snapshots).filePath(QStringLiteral("failed.png"))),
                    "failed rendering dialog snapshot must save");
        modal->closeRequested(AdModal::CloseReason::Keyboard);
        wait([&] { return kept == 1; }, "Escape in the retained view must keep source files");
        modal->closeRequested(AdModal::CloseReason::Keyboard);
        require(!job.release(false) && !job.release(true) && !job.retry() && kept == 1,
                "completed modal callbacks cannot release or retry its source again");
    }
}

void recordingColorSamplerInteractions();

void recordingColorSamplingIsConnected(bool nativeDesktop = false) {
    QWidget background;
    QRect region(10, 10, 640, 480);
    const QColor desktopColor(35, 153, 76);
    if (nativeDesktop) {
        QScreen* screen = QGuiApplication::primaryScreen();
        require(screen != nullptr, "native color sampling needs a screen");
        background.setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        const QRect logicalRegion(screen->geometry().center() - QPoint(180, 140), QSize(360, 280));
        background.setGeometry(logicalRegion);
        QPalette colors = background.palette();
        colors.setColor(QPalette::Window, desktopColor);
        background.setPalette(colors);
        background.setAutoFillBackground(true);
        background.show();
#ifdef Q_OS_MACOS
        region = logicalRegion;
#else
        region = ScreenshotGeometryMapper::nativeRectForLogicalRect(
            logicalRegion, screen->geometry(),
            ScreenshotGeometryMapper::physicalRectForScreen(*screen));
#endif
    }
    ScreenRecordingController controller(testEffectsSource);
    controller.open(region);
    controller.startRecording();
    waitForRecording(controller);
    auto* tools = palette();
    require(tools->activateDrawingShortcut(QStringLiteral("shape")), "activate shape for sampling");
    adqt::widgets::AdColorPicker* picker = nullptr;
    for (auto* candidate : tools->findChildren<adqt::widgets::AdColorPicker*>()) {
        if (candidate->accessibleName() == QStringLiteral("Stroke color"))
            picker = candidate;
    }
    require(picker != nullptr, "recording shape stroke picker exists");
    picker->setPopupVisible(true);
    QCoreApplication::processEvents();
    auto* sampler = qobject_cast<QAbstractButton*>(picker->previewContent());
    require(sampler != nullptr, "recording stroke picker exposes its eyedropper");
    sampler->click();
    require(!picker->popupVisible() && QApplication::overrideCursor() != nullptr,
            "recording eyedropper must enter sampling mode instead of dropping the request");
    QWidget* toolbar = tools->window();
    if (nativeDesktop) {
        ScreenRecordingAreaWindow* area = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget))
                area = candidate;
        }
        require(area != nullptr, "recording sampling area exists");
        QElapsedTimer repaint;
        repaint.start();
        while (repaint.elapsed() < 200) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(1);
        }
        auto* canvas = area->canvas();
        const QPointF point(100.25, 80.5);
        const QPointF global = canvas->mapToGlobal(point);
        for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, point, global, Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                              Qt::NoModifier);
            QApplication::sendEvent(canvas, &event);
        }
        const QColor picked = picker->value().solidColor;
        qInfo() << "Sampled desktop color" << picked << "expected" << desktopColor;
        require(qAbs(picked.red() - desktopColor.red()) <= 2 &&
                    qAbs(picked.green() - desktopColor.green()) <= 2 &&
                    qAbs(picked.blue() - desktopColor.blue()) <= 2,
                "recording eyedropper must sample the composited desktop beneath its transparent "
                "canvas");
        require(!canvas->canvasHistoryState().canUndo,
                "native sampling must not add an annotation");
    } else {
        for (const auto type : {QEvent::ShortcutOverride, QEvent::KeyPress, QEvent::KeyRelease}) {
            QKeyEvent escape(type, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(toolbar, &escape);
        }
    }
    require(QApplication::overrideCursor() == nullptr && controller.isOpen() &&
                controller.isRecording(),
            "finishing sampling must leave the recording running and restore its cursor");
    if (!nativeDesktop) {
        sampler->click();
        require(QApplication::overrideCursor() != nullptr,
                "sampling can restart before closing the recording");
    }
    tools->recordingCloseRequested();
    require(QApplication::overrideCursor() == nullptr, "closing recording must clean up sampling");
    waitForIdle(controller);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void recordingMagnifierUnavailableWithoutImageSources() {
    using namespace snow_shot::storage;
    const ScreenshotToolbarSettings settings;
    const DrawingSettings drawingSettings;
    const DrawingShortcutSettings shortcuts;
    const auto kind = ScreenshotToolbarLayoutKind::DrawingTools;
    const auto originalLayout = settings.layout(kind);
    const auto originalShortcut = shortcuts.shortcuts(QStringLiteral("magnifier"));
    const auto originalLastTool = settings.lastDrawingTool();
    const bool originalRemember = drawingSettings.rememberLastUsedTool();
    const auto restore = qScopeGuard([&] {
        static_cast<void>(settings.setLayout(kind, originalLayout));
        static_cast<void>(shortcuts.setShortcuts(QStringLiteral("magnifier"), originalShortcut));
        static_cast<void>(settings.setLastDrawingTool(originalLastTool));
        static_cast<void>(drawingSettings.setRememberLastUsedTool(originalRemember));
    });
    auto savedLayout = originalLayout;
    for (auto& position : savedLayout.positions)
        position.removeAll(QStringLiteral("magnifier"));
    savedLayout.positions.prepend({QStringLiteral("magnifier")});
    savedLayout.hidden.removeAll(QStringLiteral("magnifier"));
    require(
        settings.setLayout(kind, savedLayout) &&
            shortcuts.setShortcuts(QStringLiteral("magnifier"), {QStringLiteral("Ctrl+Alt+F12")}) &&
            drawingSettings.setRememberLastUsedTool(true),
        "save screenshot magnifier preferences before opening recording annotations");
    savedLayout = settings.layout(kind);
    ScreenRecordingController controller(testEffectsSource);
    for (int sessionIndex = 0; sessionIndex < 2; ++sessionIndex) {
        require(settings.setLastDrawingTool(QStringLiteral("magnifier")),
                "remember an image-backed tool before opening recording annotations");
        controller.open(QRect(10, 10, 640, 480));
        QCoreApplication::processEvents();
        ScreenRecordingAreaWindow* area = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget);
                candidate && candidate->isVisible())
                area = candidate;
        }
        require(area != nullptr, "recording image-source availability fixture opens a canvas");
        auto* tools = palette();
        auto* canvas = area->canvas();
        require(!tools->findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotMagnifierButton")) &&
                    !tools->canActivateDrawingShortcut(QStringLiteral("magnifier")) &&
                    !tools->activateDrawingShortcut(QStringLiteral("magnifier")) &&
                    !tools->activateToolShortcut(ScreenshotToolPalette::Tool::Magnifier) &&
                    !tools->activateRememberedDrawingTool(),
                "recording cannot expose or activate a tool requiring captured pixels");
        require(tools->activateDrawingShortcut(QStringLiteral("shape")),
                "supported recording tools still activate with a stored magnifier layout");
        QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_F12,
                           Qt::ControlModifier | Qt::AltModifier);
        override.ignore();
        QApplication::sendEvent(canvas, &override);
        require(!override.isAccepted(), "unavailable recording shortcuts must not claim keys");
        QKeyEvent press(QEvent::KeyPress, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
        QApplication::sendEvent(canvas, &press);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
        QApplication::sendEvent(canvas, &release);
        tools->magnifierRequested();
        require(canvas->canvasTool() == SnowCanvasTool::Shape &&
                    settings.layout(kind) == savedLayout,
                "recording capability filtering must preserve screenshot preferences");
        auto groupedLayout = savedLayout;
        for (auto& position : groupedLayout.positions) {
            for (const auto* id : {"magnifier", "spotlight", "highlighter"})
                position.removeAll(QString::fromLatin1(id));
        }
        groupedLayout.positions.prepend({QStringLiteral("magnifier"), QStringLiteral("spotlight"),
                                         QStringLiteral("highlighter")});
        groupedLayout.hidden.removeAll(QStringLiteral("spotlight"));
        groupedLayout.hidden.removeAll(QStringLiteral("highlighter"));
        require(settings.setLayout(kind, groupedLayout), "save an image-backed tool in a stack");
        savedLayout = settings.layout(kind);
        tools->setToolbarLayout(savedLayout);
        require(!tools->canActivateDrawingShortcut(QStringLiteral("magnifier")),
                "a layout rebuild cannot enable unsupported recording tools");
        tools->recordingCloseRequested();
        waitForIdle(controller);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

void recordingActiveToolsReturnToSelect() {
    using Tool = ScreenshotToolPalette::Tool;
    using State = ScreenshotToolPalette::RecordingState;
    using InputMode = ScreenRecordingAreaWindow::InputMode;
    const snow_shot::storage::ScreenshotToolbarSettings settings;
    const auto originalHighlight = settings.lastHighlightTool();
    const auto originalFilter = settings.lastFilterTool();
    const auto originalEraser = settings.lastEraserTool();
    const auto cleanup = qScopeGuard([&] {
        static_cast<void>(settings.setLastHighlightTool(originalHighlight));
        static_cast<void>(settings.setLastFilterTool(originalFilter));
        static_cast<void>(settings.setLastEraserTool(originalEraser));
    });
    require(settings.setLastHighlightTool(QStringLiteral("pen-highlight")) &&
                settings.setLastFilterTool(QStringLiteral("pen-filter")) &&
                settings.setLastEraserTool(QStringLiteral("eraser")),
            "recording toggle fixture seeds remembered drawing modes");
    struct ToolCase {
        const char* shortcut;
        const char* itemId;
        Tool tool;
        SnowCanvasTool canvasTool;
    };
    const ToolCase cases[] = {
        {"shape", "shape", Tool::Shape, SnowCanvasTool::Shape},
        {"arrow", "arrow", Tool::Arrow, SnowCanvasTool::Arrow},
        {"line", "line", Tool::Line, SnowCanvasTool::Line},
        {"brush", "free-draw", Tool::FreeDraw, SnowCanvasTool::FreeDraw},
        {"highlight", "highlighter", Tool::PenHighlight, SnowCanvasTool::PenHighlight},
        {"spotlight", "spotlight", Tool::Spotlight, SnowCanvasTool::Spotlight},
        {"filter", "filter", Tool::PenFilter, SnowCanvasTool::PenFilter},
        {"eraser", "eraser", Tool::Eraser, SnowCanvasTool::Eraser},
        {"watermark", "watermark", Tool::Watermark, SnowCanvasTool::Watermark},
        {"text", "text", Tool::Text, SnowCanvasTool::Text},
        {"serial_number", "serial-number", Tool::SerialNumber, SnowCanvasTool::SerialNumber},
    };
    ScreenRecordingController controller(testEffectsSource);
    controller.open(QRect(10, 10, 640, 480));
    ScreenRecordingAreaWindow* area = nullptr;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget);
            candidate && candidate->isVisible())
            area = candidate;
    }
    require(area != nullptr, "recording toggle fixture opens an annotation canvas");
    auto* tools = palette();
    auto* canvas = area->canvas();
    auto* exportButton =
        tools->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingExportSettings"));
    require(exportButton != nullptr, "recording toggle fixture exposes export settings");
    adqt::widgets::AdButton* selectButton = nullptr;
    for (auto* button : tools->findChildren<adqt::widgets::AdButton*>()) {
        if (button->toolTip().startsWith(QStringLiteral("Select elements"))) {
            selectButton = button;
            break;
        }
    }
    require(selectButton != nullptr, "recording toggle fixture exposes Select");
    int selectRequests = 0;
    QObject::connect(tools, &ScreenshotToolPalette::selectRequested, tools,
                     [&] { ++selectRequests; });
    const auto requireSelect = [&] {
        require(
            tools->activeTool() == Tool::Select && canvas->canvasTool() == SnowCanvasTool::Select &&
                area->inputMode() == InputMode::Drawing && !tools->recordingExportSettingsVisible(),
            "repeated recording tool activation selects canvas elements without export settings");
    };
    for (const auto state : {State::Idle, State::Recording, State::Paused}) {
        if (state == State::Recording) {
            controller.startRecording();
            waitForRecording(controller);
        } else if (state == State::Paused) {
            tools->recordingPauseRequested();
        }
        require(!tools->canActivateDrawingShortcut(QStringLiteral("magnifier")) &&
                    !tools->activateDrawingShortcut(QStringLiteral("magnifier")),
                "magnifiers remain unavailable while recording is idle, active, or paused");
        for (const auto& test : cases) {
            const auto shortcut = QString::fromLatin1(test.shortcut);
            require(tools->activateDrawingShortcut(shortcut) && tools->activeTool() == test.tool &&
                        canvas->canvasTool() == test.canvasTool,
                    "recording tool activation reaches its canvas tool");
            adqt::widgets::AdButton* trigger = nullptr;
            for (auto* button : tools->findChildren<adqt::widgets::AdButton*>()) {
                if (button->isVisibleTo(tools) &&
                    button->property("screenshotToolbarItemId").toString() ==
                        QString::fromLatin1(test.itemId)) {
                    trigger = button;
                    break;
                }
            }
            require(trigger != nullptr, "active recording tool has a visible toolbar trigger");
            const int previousSelectRequests = selectRequests;
            trigger->click();
            requireSelect();
            require(selectRequests == previousSelectRequests + 1,
                    "repeated toolbar click issues exactly one selection command");
            trigger->click();
            require(tools->activeTool() == test.tool && canvas->canvasTool() == test.canvasTool,
                    "clicking the deactivated trigger restores its remembered tool");
            require(tools->activateDrawingShortcut(shortcut), "repeat the active tool shortcut");
            requireSelect();
            require(tools->activateToolShortcut(Tool::Select), "repeat the Select shortcut");
            requireSelect();
            selectButton->click();
            requireSelect();
            require(tools->recordingSession().state() == state &&
                        controller.isRecording() == (state != State::Idle),
                    "tool toggles preserve the active recording state");
            exportButton->click();
            exportButton->click();
            require(tools->recordingExportSettingsVisible() && !tools->activeTool().has_value() &&
                        area->inputMode() == InputMode::RegionEditing,
                    "explicit export settings activation remains idempotent and edits the region");
        }
    }
    tools->recordingCloseRequested();
    waitForIdle(controller);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void recordingEraserTools() {
    ScreenRecordingController controller(testEffectsSource);
    const auto areaWindow = [] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* area = qobject_cast<ScreenRecordingAreaWindow*>(widget);
                area && area->isVisible())
                return area;
        }
        return static_cast<ScreenRecordingAreaWindow*>(nullptr);
    };
    controller.open(QRect(10, 10, 640, 480));
    QCoreApplication::processEvents();
    auto* area = areaWindow();
    require(area != nullptr, "recording eraser fixture opens an annotation canvas");
    verifyEraserToolbarHost(*palette(), *area->canvas(), require);
    require(area->inputMode() == ScreenRecordingAreaWindow::InputMode::Drawing,
            "recording eraser modes enable canvas drawing input");
    palette()->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    controller.open(QRect(10, 10, 640, 480));
    QCoreApplication::processEvents();
    area = areaWindow();
    require(area && palette()->activateDrawingShortcut(QStringLiteral("eraser")) &&
                area->canvas()->canvasTool() == SnowCanvasTool::BrushEraser &&
                area->canvas()->canvasStyleToolbarState().brushEraserStyle.strokeWidth == 31,
            "new recording sessions restore the eraser variant and independent width");
    palette()->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void recordingExportNamingTests(const QString& directory) {
    // These synchronous assertions unwind the fixture before storage and QApplication retire.
    // Timer callbacks below only collect observations; exceptions must not cross Qt dispatch.
    const auto requireNaming = [](bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    const snow_shot::storage::RecordingSettings settings;
    const QString previousFilenameFormat = settings.videoFilenameFormat();
    const QString previousDirectory = settings.videoSaveDirectory();
    const bool previousNotifications = settings.notifyAfterExportCompletes();
    const QByteArray previousSourcePath = renderSourcePath;
    const auto restoreFilenameFormat = qScopeGuard([&] {
        ::require(settings.setVideoFilenameFormat(previousFilenameFormat),
                  "restore video filename format");
        ::require(settings.setVideoSaveDirectory(previousDirectory), "restore video directory");
        ::require(settings.setNotifyAfterExportCompletes(previousNotifications),
                  "restore recording export notifications");
        desktopWindowNames = {};
        renderSourcePath = previousSourcePath;
    });
    requireNaming(settings.setNotifyAfterExportCompletes(true),
                  "observe the completion of each named recording export");
    for (const QString& format : {QStringLiteral("mp4"), QStringLiteral("gif"),
                                  QStringLiteral("webp"), QStringLiteral("apng")}) {
        for (bool deferred : {false, true}) {
            const QString mode = deferred ? QStringLiteral("deferred") : QStringLiteral("realtime");
            desktopWindowNames = {QStringLiteral("Browser/Docs"), QStringLiteral("{YYYY}: A/B")};
            const QString directoryTemplate =
                QDir(directory).filePath(QStringLiteral("{AN}/{WN}/%1/%2").arg(mode, format));
            const QString expandedDirectory = QDir(directory).filePath(
                QStringLiteral("Browser_Docs/{YYYY}_ A_B/%1/%2").arg(mode, format));
            requireNaming(
                settings.setVideoSaveDirectory(directoryTemplate) &&
                    settings.setVideoFilenameFormat(
                        QStringLiteral("Video_{'clip.name'}_{AN}_{WN}_%1_%2").arg(mode, format)) &&
                    QDir().mkpath(expandedDirectory),
                "configure recording directory and filename variables with collision fixtures");
            const QString basePath =
                QDir(expandedDirectory)
                    .filePath(QStringLiteral("Video_clip.name_Browser_Docs_{YYYY}_ A_B_%1_%2")
                                  .arg(mode, format));
            const auto path = [&](int suffix) {
                return basePath + (suffix ? QStringLiteral("_%1").arg(suffix) : QString()) +
                       QStringLiteral(".") + format;
            };
            for (int suffix : {0, 1}) {
                QFile existing(path(suffix));
                requireNaming(existing.open(QIODevice::WriteOnly) && existing.write("existing") > 0,
                              "create colliding normal video exports");
            }
            if (deferred) {
                const QString source = basePath + QStringLiteral(".source");
                requireNaming(QDir().mkpath(source),
                              "materialize the retained deferred source bundle");
                renderSourcePath = source.toUtf8();
            }
            ScreenRecordingController controller(testEffectsSource);
            // Disk publication has no three-second performance contract. Wait for the real
            // completion notification; CTest's existing process timeout still detects hangs.
            const auto exportAndWait = [&](auto action, const QString& expectedPath,
                                           const char* stage) {
                std::cerr << mode.toStdString() << ' ' << format.toStdString() << ": " << stage
                          << '\n';
                QEventLoop loop;
                QString completedPath;
                bool completed = false;
                QObject::connect(&controller,
                                 &ScreenRecordingController::exportNotificationRequested, &loop,
                                 [&](const QString& path) {
                                     completed = true;
                                     completedPath = path;
                                     loop.quit();
                                 });
                action();
                if (!completed)
                    loop.exec();
                requireNaming(completed && completedPath == expectedPath,
                              "export completion reports the expected numbered video filename");
            };
            QString error;
            const int beforeLookups = windowNameLookups;
            requireNaming(
                controller.startAutomation(QRect(80, 80, 320, 240),
                                           {{QStringLiteral("post_processing"), deferred},
                                            {QStringLiteral("format"), format},
                                            {QStringLiteral("capture_toolbar"), !deferred},
                                            {QStringLiteral("start_delay_seconds"), 0}},
                                           &error),
                "start export naming fixture");
            auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window());
            requireNaming(toolbar != nullptr, "recording naming fixture has a toolbar");
            auto* edge = toolbar->autoHideControlWindow();
            QWidget popup(toolbar, Qt::Tool);
            popup.show();
            popup.windowHandle()->setTransientParent(toolbar->windowHandle());
            QWidget unrelated(nullptr, Qt::Tool);
            unrelated.show();
            const auto nativeId = [](QWidget* widget) -> std::optional<std::uintptr_t> {
#ifdef Q_OS_MACOS
                return snow_shot::platform::captureWindowId(widget);
#else
                return static_cast<std::uintptr_t>(widget->internalWinId());
#endif
            };
            std::vector<std::uintptr_t> expectedExclusions;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (qobject_cast<ScreenRecordingAreaWindow*>(widget) || widget == toolbar ||
                    widget == edge || widget == &popup) {
                    if (const auto id = nativeId(widget))
                        expectedExclusions.push_back(*id);
                }
            }
            waitForRecording(controller);
            requireNaming(
                windowNameLookups == beforeLookups + 1 &&
                    windowNameLookupPoint == QPointF(240, 200) &&
                    windowNameLookupFields ==
                        snow_shot::storage::ScreenshotWindowNameFields{true, true} &&
                    recordingOutputPath == path(2),
                "one lookup at the selected region center names both templates before capture");
            for (const auto id : expectedExclusions)
                requireNaming(std::find(windowNameLookupExclusions.begin(),
                                        windowNameLookupExclusions.end(),
                                        id) != windowNameLookupExclusions.end(),
                              "recording surfaces are ignored even when the toolbar is captured");
            if (const auto id = nativeId(&unrelated))
                requireNaming(std::find(windowNameLookupExclusions.begin(),
                                        windowNameLookupExclusions.end(),
                                        *id) == windowNameLookupExclusions.end(),
                              "unrelated windows must remain eligible for naming");
            popup.hide();
            popup.setParent(nullptr);
            desktopWindowNames = {QStringLiteral("Other App"), QStringLiteral("Changed title")};
            recordingToolbarButton("Trim Video")->click();
            auto* copy = recordingToolbarButton("Copy recording");
            {
                QEventLoop ready;
                const auto isReady = [&] {
                    return controller.automationState().value(QStringLiteral("state")) ==
                               QStringLiteral("trimming") &&
                           copy->isEnabled();
                };
                QObject::connect(QAbstractEventDispatcher::instance(),
                                 &QAbstractEventDispatcher::aboutToBlock, &ready, [&] {
                                     if (isReady())
                                         ready.quit();
                                 });
                if (!isReady())
                    ready.exec();
                requireNaming(isReady(), "the named recording opens an editable trim session");
            }
            requireNaming(deferred || clipPath == path(2),
                          "normal recording uses the configured format and numbered collisions");
            QFile original(path(2));
            requireNaming(original.open(QIODevice::WriteOnly) && original.write("original") > 0,
                          "retain the original video next to the cropped exports");
            original.close();
            auto* first =
                palette()->findChild<QSlider*>(QStringLiteral("screenRecordingTrimStart"));
            requireNaming(first != nullptr, "crop timeline exists");
            first->setValue(1);
            const int beforeExports = clipExports;
            exportAndWait([&] { copy->click(); }, path(3), "first cropped Copy");
            requireNaming(copy->isEnabled() && clipExports == beforeExports + 1,
                          "the first crop completes exactly one export and restores Copy");
            requireNaming(QApplication::clipboard()->mimeData()->urls() ==
                              QList<QUrl>{QUrl::fromLocalFile(path(3))},
                          "cropped Copy follows normal export naming and preserves the extension");
            copy->click();
            requireNaming(clipExports == beforeExports + 1 &&
                              QApplication::clipboard()->mimeData()->urls() ==
                                  QList<QUrl>{QUrl::fromLocalFile(path(3))},
                          "repeated Copy keeps the cached export and filename");
            bool saveShown = false, saveSuggestionMatches = false;
            QTimer acceptSave;
            QObject::connect(&acceptSave, &QTimer::timeout, [&] {
                for (auto* widget : QApplication::topLevelWidgets()) {
                    if (auto* dialog = qobject_cast<QFileDialog*>(widget)) {
                        saveSuggestionMatches = dialog->selectedFiles() == QStringList{path(4)} &&
                                                dialog->defaultSuffix() == format;
                        saveShown = true;
                        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
                    }
                }
            });
            acceptSave.start(10);
            exportAndWait([&] { recordingToolbarButton("Save to File")->click(); }, path(4),
                          "cached cropped Save");
            acceptSave.stop();
            requireNaming(copy->isEnabled(), "Save completion restores Copy");
            QFile saved(path(4));
            requireNaming(saveShown, "cropped Save opens its filename dialog");
            requireNaming(saveSuggestionMatches,
                          "cropped Save suggests the next normal export filename");
            requireNaming(saved.open(QIODevice::ReadOnly),
                          "Save publishes the crop under the suggested video filename");
            requireNaming(saved.readAll() == "trim fixture",
                          "Save preserves the cached crop bytes");
            requireNaming(clipExports == beforeExports + 1, "Save reuses the cached crop");
            first->setValue(2);
            exportAndWait([&] { copy->click(); }, path(5), "changed cropped Copy");
            requireNaming(copy->isEnabled() && clipExports == beforeExports + 2,
                          "changing the crop completes exactly one new export and restores Copy");
            requireNaming(QApplication::clipboard()->mimeData()->urls() ==
                              QList<QUrl>{QUrl::fromLocalFile(path(5))},
                          "a new crop skips previously copied and saved video filenames");
            for (int suffix : {0, 1, 2}) {
                QFile retained(path(suffix));
                requireNaming(retained.open(QIODevice::ReadOnly) &&
                                  retained.readAll() == (suffix == 2 ? "original" : "existing"),
                              "cropped exports never replace existing videos or the original");
            }
            requireNaming(windowNameLookups == beforeLookups + 1,
                          "trimmed Copy and Save retain the names captured at start");
            recordingToolbarButton("Close recording")->click();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }
}

void recordingWindowNameFallbackTests(const QString& directory) {
    const snow_shot::storage::RecordingSettings settings;
    const QString previousFilename = settings.videoFilenameFormat();
    const QString previousDirectory = settings.videoSaveDirectory();
    const auto restore = qScopeGuard([&] {
        require(settings.setVideoFilenameFormat(previousFilename) &&
                    settings.setVideoSaveDirectory(previousDirectory),
                "restore recording naming settings");
        desktopWindowNames = {};
    });
    const QRect region(-501, -301, 321, 241);
    ScreenRecordingController controller(testEffectsSource);
    const auto start = [&](const QJsonObject& options,
                           snow_shot::storage::ScreenshotWindowNameFields expectedFields,
                           const QString& expectedPath) {
        const int expectedLookups = expectedFields.any() ? 1 : 0;
        const int before = windowNameLookups;
        QString error;
        require(controller.startAutomation(region, options, &error), "start window naming case");
        waitForRecording(controller);
        require(windowNameLookups == before + expectedLookups &&
                    recordingOutputPath == expectedPath,
                "window naming must preserve fallbacks, literal output paths and lookup gating");
        if (expectedLookups)
            require(windowNameLookupPoint == QPointF(-340.5, -180.5) &&
                        windowNameLookupFields == expectedFields,
                    "recording naming preserves its center and requests only required fields");
        controller.stopRecordingAndCopy();
        waitForIdle(controller);
        require(controller.automationState().value(QStringLiteral("path")).toString() ==
                    expectedPath,
                "finalized exports retain the destination resolved at capture start");
        require(controller.controlAutomation(QStringLiteral("close"), {}, &error),
                "retire the previous automation session before starting another");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };
    const QJsonObject options{{QStringLiteral("post_processing"), false},
                              {QStringLiteral("format"), QStringLiteral("mp4")},
                              {QStringLiteral("start_delay_seconds"), 0}};
    require(settings.setVideoSaveDirectory(directory) &&
                settings.setVideoFilenameFormat(QStringLiteral("Date_{'only'}_{wn}")),
            "set a template without exact window variables");
    start(options, {}, QDir(directory).filePath(QStringLiteral("Date_only_wn.mp4")));
    require(settings.setVideoFilenameFormat(QStringLiteral("{AN}_{WN}")),
            "set filename-only window variables");
    desktopWindowNames = {QStringLiteral("Known/App"), {}};
    start(options, {true, true}, QDir(directory).filePath(QStringLiteral("Known_App_Unknown.mp4")));
    require(settings.setVideoFilenameFormat(QStringLiteral("{AN}")),
            "set application-only filename variables");
    desktopWindowNames = {QStringLiteral("Known/App"), QStringLiteral("Unused title")};
    start(options, {true, false}, QDir(directory).filePath(QStringLiteral("Known_App.mp4")));
    require(settings.setVideoSaveDirectory(QDir(directory).filePath(QStringLiteral("{WN}"))) &&
                settings.setVideoFilenameFormat(QStringLiteral("Video")),
            "set title-only directory variables");
    desktopWindowNames = {QStringLiteral("Unused application"), QStringLiteral("Known/title")};
    start(options, {false, true},
          QDir(directory).filePath(QStringLiteral("Known_title/Video.mp4")));
    require(settings.setVideoSaveDirectory(QDir(directory).filePath(QStringLiteral("{AN}/{WN}"))) &&
                settings.setVideoFilenameFormat(QStringLiteral("Video")),
            "set directory-only window variables");
    desktopWindowNames = {};
    start(options, {true, true},
          QDir(directory).filePath(QStringLiteral("Unknown/Unknown/Video.mp4")));
    const QString literal = QDir(directory).filePath(QStringLiteral("literal_{AN}_{WN}.mp4"));
    auto explicitPath = options;
    explicitPath.insert(QStringLiteral("path"), literal);
    start(explicitPath, {true, true}, literal);

    require(settings.setVideoSaveDirectory(directory) &&
                settings.setVideoFilenameFormat(QStringLiteral("{AN}_{WN}")),
            "prepare countdown naming case");
    auto delayed = options;
    delayed.insert(QStringLiteral("start_delay_seconds"), 1);
    desktopWindowNames = {QStringLiteral("Before"), QStringLiteral("Countdown")};
    const int before = windowNameLookups;
    QString error;
    require(controller.startAutomation(region, delayed, &error) && windowNameLookups == before,
            "countdown must not resolve names before capture starts");
    desktopWindowNames = {QStringLiteral("After"), QStringLiteral("Countdown")};
    waitForRecording(controller);
    require(windowNameLookups == before + 1 &&
                recordingOutputPath ==
                    QDir(directory).filePath(QStringLiteral("After_Countdown.mp4")),
            "new sessions replace old names using the window present after countdown");
    controller.stopRecordingAndCopy();
    waitForIdle(controller);
    require(controller.controlAutomation(QStringLiteral("close"), {}, &error),
            "close window naming fixture");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void recordingTrimPlaybackTests(const QString& directory) {
    clipPreviewEnabled = true;
    // Uneven presentation boundaries catch attempts to derive a frame from nominal FPS.
    for (quint64 frame = 0; frame <= 30; ++frame)
        clipBoundaries.append(frame * frame * 1000000 / 900);
    const auto restorePreview = qScopeGuard([] {
        clipPreviewEnabled = false;
        clipPreviewOverride = {};
        clipBoundaries.clear();
        clipPreviewError.clear();
        failClipSeek = false;
        clipExportState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
    });
    const auto wait = [](auto predicate) {
        QElapsedTimer deadline;
        deadline.start();
        while (!predicate() && deadline.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(predicate(), "playback fixture receives the requested state");
    };
    const auto publish = [&](quint64 position, bool playing, bool stale = false) {
        const int reads = clipPreviewReads;
        clipPreviewOverride = [=](SnowRecordingClipPreview& preview) {
            preview.position_us = position;
            preview.playing = playing ? 1u : 0u;
            if (stale)
                preview.revision = 0;
        };
        wait([&] { return clipPreviewReads > reads; });
        clipPreviewOverride = {};
    };
    for (bool deferred : {false, true}) {
        ScreenRecordingController controller(testEffectsSource);
        QString error;
        require(controller.startAutomation(
                    QRect(80, 80, 320, 240),
                    {{QStringLiteral("post_processing"), deferred},
                     {QStringLiteral("path"),
                      directory + QStringLiteral("/playback-%1.mp4").arg(deferred)}},
                    &error),
                "start play/pause fixture");
        waitForRecording(controller);
        recordingToolbarButton("Trim Video")->click();
        auto* copy = recordingToolbarButton("Copy recording");
        wait([&] { return copy->isEnabled(); });
        auto* panel = static_cast<RecordingTrimToolbar*>(palette()->recordingTrimPanel());
        auto* toggle = panel->findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenRecordingTrimPlayPause"));
        auto* replay =
            panel->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingReplay"));
        auto* speed =
            panel->findChild<adqt::widgets::AdSelect*>(QStringLiteral("screenRecordingTrimSpeed"));
        auto* first = panel->findChild<QSlider*>(QStringLiteral("screenRecordingTrimStart"));
        auto* end = panel->findChild<QSlider*>(QStringLiteral("screenRecordingTrimEnd"));
        require(toggle->isEnabled() && toggle->toolTip() == QStringLiteral("Pause"),
                "opening trim autoplays and immediately offers Pause");
        publish(clipBoundaries[7] + 100, true);
        toggle->click();
        require(clipFirst == 7 && toggle->toolTip() == QStringLiteral("Play"),
                "Pause freezes the displayed variable-duration frame");
        const int seeksBeforeSpeed = clipSeekCalls;
        speed->setCurrentValue(125u);
        require(clipSeekCalls == seeksBeforeSpeed && toggle->toolTip() == QStringLiteral("Play"),
                "changing speed preserves paused playback without seeking");
        toggle->click();
        require(clipFirst == 7 && clipPreviewSpeed == 125 &&
                    toggle->toolTip() == QStringLiteral("Pause"),
                "Play resumes at the paused frame and retains speed");
        speed->setCurrentValue(150u);
        require(toggle->toolTip() == QStringLiteral("Pause"),
                "changing speed also preserves playing state");
        failClipSeek = true;
        toggle->click();
        failClipSeek = false;
        require(toggle->toolTip() == QStringLiteral("Pause"),
                "rejected playback requests do not change the accepted state");
        const int beforeRapid = clipSeekCalls;
        toggle->click();
        toggle->click();
        require(clipSeekCalls == beforeRapid + 2 && clipFirst == 7 &&
                    toggle->toolTip() == QStringLiteral("Pause"),
                "rapid clicks alternate immediately before another preview arrives");
        publish(clipBoundaries[25], false, true);
        require(toggle->toolTip() == QStringLiteral("Pause"),
                "stale preview cannot undo a newer playback intent");
        toggle->click();
        require(clipFirst == 7, "stale preview cannot replace the accepted playback position");
        panel->seekRequested(9);
        require(clipFirst == 9 && toggle->toolTip() == QStringLiteral("Play"),
                "timeline scrubbing pauses and updates the next resume position");
        first->setValue(3);
        end->setValue(20);
        require(clipFirst == 19 && toggle->toolTip() == QStringLiteral("Play"),
                "moving the trim end pauses at the final included frame");
        toggle->click();
        require(clipFirst == 19 && clipEnd == 20,
                "a scrubbed final frame resumes without being mistaken for completion");
        publish(clipBoundaries[20], false);
        require(toggle->toolTip() == QStringLiteral("Play"),
                "natural completion changes the action to Play");
        toggle->click();
        require(clipFirst == 3 && clipEnd == 20,
                "Play after completion restarts at the selected beginning");
        publish(clipBoundaries[12] + 1, true);
        replay->click();
        require(clipFirst == 3 && clipPreviewSpeed == 150 &&
                    toggle->toolTip() == QStringLiteral("Pause"),
                "Replay restarts the selection and retains speed");
        first->setValue(19);
        publish(clipBoundaries[20], false);
        toggle->click();
        require(clipFirst == 19 && clipEnd == 20,
                "a completed one-frame selection restarts within its boundaries");
        clipExportState = SNOW_RECORDING_RENDER_STATE_RUNNING;
        copy->click();
        const int exportingSeeks = clipSeekCalls;
        toggle->click();
        panel->playbackToggleRequested();
        require(!toggle->isEnabled() && clipSeekCalls == exportingSeeks,
                "export disables playback and guards direct requests");
        clipExportState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
        wait([&] { return copy->isEnabled(); });
        require(toggle->isEnabled() && toggle->toolTip() == QStringLiteral("Play"),
                "export completion restores paused playback controls");
        toggle->click();
        clipPreviewError = "fixture preview failure";
        wait([&] { return !toggle->isEnabled(); });
        const int failedSeeks = clipSeekCalls;
        panel->playbackToggleRequested();
        require(toggle->toolTip() == QStringLiteral("Play") && copy->isEnabled() &&
                    clipSeekCalls == failedSeeks,
                "preview failure disables playback while leaving export available");
        // A successful export must not re-enable a failed preview.
        copy->click();
        wait([&] { return copy->isEnabled(); });
        require(!toggle->isEnabled(), "busy-state changes preserve playback failure availability");
        QPointer<RecordingTrimToolbar> retired = panel;
        recordingToolbarButton("Close recording")->click();
        if (retired)
            retired->playbackToggleRequested();
        require(!controller.isOpen() && clipSeekCalls == failedSeeks,
                "detaching the editor prevents further playback requests");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        clipPreviewError.clear();
    }
}

void recordingTrimmingTests(const QString& directory, bool uiOnly = false) {
    auto& diagnostics = snow_shot::diagnostics::DiagnosticsService::instance();
    snow_shot::diagnostics::DiagnosticsOptions diagnosticOptions;
    diagnosticOptions.directories = {directory + QStringLiteral("/trim-logs")};
    diagnosticOptions.enableCrashCapture = false;
    diagnosticOptions.installMessageHandler = false;
    diagnosticOptions.mirrorToConsole = false;
    require(diagnostics.initialize(diagnosticOptions), "initialize trim diagnostic log");
    const auto stopDiagnostics = qScopeGuard([&] { diagnostics.shutdown(); });
    const QString renders = qEnvironmentVariable("SNOW_TRIM_VISUAL_OUTPUT");
    QFont visualFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
#ifdef Q_OS_WIN
    if (!renders.isEmpty()) {
        const QString font =
            QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/segoeui.ttf"));
        require(QFontDatabase::addApplicationFont(font) >= 0,
                "load the installed UI font only for readable visual fixtures");
        visualFont.setFamily(QStringLiteral("Segoe UI"));
    }
#endif
    const snow_shot::storage::RecordingSettings settings;
    require(settings.setNotifyAfterExportCompletes(true), "enable trimmed export notifications");
    const auto restoreNotifications = qScopeGuard([&] {
        require(settings.setNotifyAfterExportCompletes(false),
                "restore export notification preference");
    });
    {
        ScreenRecordingToolbarWindow toolbar;
        auto* panel = new RecordingTrimToolbar;
        panel->setTimeline(30, 1000000,
                           [](int frame) { return static_cast<quint64>(frame) * 1000000 / 30; });
        toolbar.palette()->setRecordingTrimPanel(panel, true);
        toolbar.show();
        for (bool above : {false, true}) {
            toolbar.setStyleToolbarAboveMain(above);
            for (bool busy : {true, false}) {
                toolbar.palette()->setRecordingTrimPanel(panel, busy);
                toolbar.prepareForDisplay();
                QCoreApplication::processEvents();
                auto* host = toolbar.paletteHost();
                QList<QPoint> points{panel->mapToGlobal(panel->rect().center())};
                for (const char* name : {"screenRecordingReplay", "screenRecordingTrimPlayPause",
                                         "screenRecordingTrimSpeed", "screenRecordingTrimStart",
                                         "screenRecordingTrimEnd"}) {
                    auto* control = panel->findChild<QWidget*>(QString::fromLatin1(name));
                    require(control != nullptr, "trim input target exists");
                    require(control->isEnabled() != busy, "busy trim disables every input");
                    points.append(control->mapToGlobal(control->rect().center()));
                }
                for (const auto& point : points) {
                    require(toolbar.containsInteractiveGlobalPoint(point),
                            "trim controls and track must receive input through the floating host");
                    require(host->surfaceHostRegion().contains(host->mapFromGlobal(point)),
                            "trim controls and track must belong to the native surface");
                }
                const QRect main = toolbar.palette()->mainPanel()->geometry();
                const QRect secondary = panel->geometry();
                const int gapY = above ? (secondary.bottom() + main.top()) / 2
                                       : (main.bottom() + secondary.top()) / 2;
                require(!toolbar.containsInteractiveGlobalPoint(
                            toolbar.palette()->mapToGlobal(QPoint(secondary.center().x(), gapY))),
                        "the gap between toolbar rows must remain click-through");
                if (!busy) {
                    auto* speedSelect = panel->findChild<adqt::widgets::AdSelect*>(
                        QStringLiteral("screenRecordingTrimSpeed"));
                    speedSelect->showPopup();
                    QCoreApplication::processEvents();
                    require(
                        speedSelect->popupVisible() &&
                            toolbar.containsInteractiveGlobalPoint(speedSelect->view()->mapToGlobal(
                                speedSelect->view()->rect().center())),
                        "speed popup receives input outside the floating toolbar surface");
                    speedSelect->hidePopup();
                }
            }
        }
        toolbar.palette()->setRecordingTrimPanel(nullptr, false);
        toolbar.prepareForDisplay();
        require(!panel->isVisible(), "detaching trim retires its input row");
    }
    const auto wait = [](auto predicate) {
        QElapsedTimer deadline;
        deadline.start();
        while (!predicate() && deadline.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(predicate(), "trim operation completes asynchronously");
    };
    QWidget visualHost;
    visualHost.setAttribute(Qt::WA_TranslucentBackground);
    RecordingTrimToolbar range(&visualHost);
    range.setTimeline(30, 1000000,
                      [](int frame) { return static_cast<quint64>(frame) * 1000000 / 30; });
    require(range.size() == QSize(480, 40) && range.firstFrame() == 0 && range.endFrame() == 30,
            "trim row starts at Select height with the entire clip selected");
    auto* playPause =
        range.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingTrimPlayPause"));
    require(playPause && playPause->iconRef() == adqt::icons::antd::outlined::PlayCircle() &&
                playPause->toolTip() == QStringLiteral("Play") &&
                playPause->accessibleName() == playPause->toolTip(),
            "paused trim offers the Ant Design Play Circle action");
    range.setPlaying(true);
    require(playPause->iconRef() == adqt::icons::antd::outlined::PauseCircle() &&
                playPause->toolTip() == QStringLiteral("Pause") &&
                playPause->accessibleName() == playPause->toolTip(),
            "playing trim offers the matching Pause Circle action");
    int toggleRequests = 0;
    range.playbackToggleRequested = [&] { ++toggleRequests; };
    for (int key : {Qt::Key_Space, Qt::Key_Return}) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QApplication::sendEvent(playPause, &press);
        QApplication::sendEvent(playPause, &release);
    }
    require(toggleRequests == 2, "Space and Enter each activate the toggle once");
    range.setPlaybackAvailable(false);
    playPause->click();
    range.setEnabled(false);
    range.setEnabled(true);
    require(!playPause->isEnabled() && toggleRequests == 2,
            "unavailable playback remains disabled after the parent becomes ready");
    range.setPlaybackAvailable(true);
    range.setPlaying(false);
    auto* speed =
        range.findChild<adqt::widgets::AdSelect*>(QStringLiteral("screenRecordingTrimSpeed"));
    const QVector<quint32> percents{25, 50, 75, 100, 125, 150, 175, 200, 300};
    const QStringList labels{
        QStringLiteral("0.25x"), QStringLiteral("0.5x"),  QStringLiteral("0.75x"),
        QStringLiteral("1.0x"),  QStringLiteral("1.25x"), QStringLiteral("1.5x"),
        QStringLiteral("1.75x"), QStringLiteral("2.0x"),  QStringLiteral("3.0x")};
    require(speed && speed->options().size() == percents.size() && range.speedPercent() == 100 &&
                speed->currentText() == QStringLiteral("1.0x") && !speed->searchEnabled() &&
                !speed->allowClear(),
            "speed has exactly nine fixed options and defaults to 1.0x");
    for (qsizetype index = 0; index < percents.size(); ++index)
        require(speed->options()[index].value.toUInt() == percents[index] &&
                    speed->options()[index].label == labels[index],
                "speed choices preserve the requested order, labels, and numeric values");
    quint32 changedSpeed = 0;
    range.speedChanged = [&](quint32 percent) { changedSpeed = percent; };
    speed->setCurrentValue(175u);
    require(changedSpeed == 175 && range.speedPercent() == 175,
            "selecting speed reports its exact numeric percentage");
    changedSpeed = 0;
    range.setSpeedPercent(100);
    range.setSpeedPercent(90);
    require(changedSpeed == 0 && range.speedPercent() == 100,
            "programmatic speed updates are silent and unsupported values are rejected");
    auto* first = range.findChild<QSlider*>(QStringLiteral("screenRecordingTrimStart"));
    auto* end = range.findChild<QSlider*>(QStringLiteral("screenRecordingTrimEnd"));
    int preview = -1;
    range.rangeChanged = [&](int, int, int frame) { preview = frame; };
    first->setValue(28);
    end->setValue(1);
    require(first->value() == 28 && end->value() == 29 && preview == 28,
            "handles never cross and end scrubbing previews the last included frame");
    QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    QApplication::sendEvent(first, &left);
    require(first->value() == 27 && preview == 27, "keyboard trim steps are exactly one frame");
    end->setValue(30);
    first->setValue(0);
    range.seekRequested = [&](int frame) { preview = frame; };
    const QPointF trackCenter((first->x() + first->width() + end->x()) / 2.0, range.height() / 2.0);
    QMouseEvent click(QEvent::MouseButtonPress, trackCenter, range.mapToGlobal(trackCenter),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&range, &click);
    require(preview == 15, "track scrubbing snaps to the nearest presentation frame");
    first->setValue(6);
    end->setValue(24);
    const QPointF pressGlobal = first->mapToGlobal(QPoint(2, first->height() / 2));
    for (auto type : {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
        const QPointF global =
            pressGlobal + (type == QEvent::MouseButtonPress ? QPointF() : QPointF(10, 0));
        QMouseEvent drag(type, first->mapFromGlobal(global), global,
                         type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                         type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                         Qt::NoModifier);
        QApplication::sendEvent(first, &drag);
        require(preview == (type == QEvent::MouseButtonPress ? 6 : 7),
                "dragging pauses at the boundary and preserves the initial grip offset");
    }
    require(first->toolTip().endsWith(QStringLiteral("00:00.233")) &&
                end->toolTip().endsWith(QStringLiteral("00:00.800")),
            "trim tooltips expose frame boundary timecodes after dragging");
    range.setEnabled(false);
    playPause->click();
    require(toggleRequests == 2, "busy playback cannot emit toggle requests");
    preview = -1;
    QApplication::sendEvent(&range, &click);
    require(preview == -1, "disabled timeline cannot request a seek");
    range.setEnabled(true);
    end->setValue(8);
    require(!first->geometry().intersects(end->geometry()),
            "one-frame selections keep both handles independently reachable");
    visualHost.resize(range.size() + QSize(96, 96));
    visualHost.show();
    range.show();
    speed->setFocus(Qt::TabFocusReason);
    QKeyEvent openSpeed(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(speed->lineEdit(), &openSpeed);
    require(speed->popupVisible(), "keyboard opens the speed menu");
    QKeyEvent lastSpeed(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
    QApplication::sendEvent(speed->view(), &lastSpeed);
    QKeyEvent chooseSpeed(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(speed->view(), &chooseSpeed);
    require(!speed->popupVisible() && range.speedPercent() == 300 && changedSpeed == 300,
            "keyboard confirms the highlighted speed and closes its menu");
    speed->showPopup();
    QKeyEvent dismissSpeed(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(speed->view(), &dismissSpeed);
    require(!speed->popupVisible() && range.speedPercent() == 300,
            "Escape dismisses the speed menu without changing the selection");
    speed->showPopup();
    range.setEnabled(false);
    require(!speed->isEnabled() && !speed->popupVisible() && range.speedPercent() == 300,
            "busy trim closes the speed popup and retains the selection");
    range.setEnabled(true);
    speed->showPopup();
    range.hide();
    require(!speed->popupVisible() && range.speedPercent() == 300,
            "hiding trim retires its popup without resetting speed");
    range.show();
    range.setSpeedPercent(175);
    // Render the actual widgets for repeatable light/dark and scale inspection.
    using namespace snow_shot::presentation::styles;
    const auto previousTheme = ThemeManager::instance().themeMode();
    for (auto theme : {ThemeMode::Light, ThemeMode::Dark}) {
        ThemeManager::instance().setThemeMode(theme);
        for (qreal scale : {1.0, 1.25, 1.5, 2.0}) {
            adqt::widgets::AdControlScaleContext context;
            context.logicalScale = scale;
            range.commitControlScale(context);
            require(range.height() == qRound(40 * scale), "trim row shares Select's scaled height");
            auto* replay = range.findChild<adqt::widgets::AdButton*>();
            require(replay->size() == QSize(qRound(32 * scale), qRound(32 * scale)) &&
                        replay->iconSize() == QSize(qRound(24 * scale), qRound(24 * scale)),
                    "Replay uses Selection Tool control and icon dimensions at every scale");
            require(playPause->size() == replay->size() &&
                        playPause->iconSize() == replay->iconSize() &&
                        replay->geometry().right() < playPause->geometry().left() &&
                        playPause->geometry().right() < speed->geometry().left(),
                    "playback matches Replay dimensions and sits before speed at every scale");
            require(speed->size() == QSize(qRound(72 * scale), qRound(32 * scale)) &&
                        speed->popupWidth() == qRound(96 * scale) &&
                        replay->geometry().right() < speed->geometry().left(),
                    "speed shares the control height and preserves its fixed width at every scale");
            for (auto* handle : {first, end}) {
                require(handle->size() == QSize(qRound(16 * scale), qRound(32 * scale)),
                        "trim handles provide enlarged scaled drag targets");
                require(range.rect().contains(handle->geometry()) &&
                            !replay->geometry().intersects(handle->geometry()) &&
                            !playPause->geometry().intersects(handle->geometry()) &&
                            !speed->geometry().intersects(handle->geometry()),
                        "trim targets stay inside the row and clear of Replay and speed");
            }
            first->setValue(6);
            end->setValue(24);
            range.setPosition(500000);
            range.move(48, 48);
            visualHost.resize(range.size() + QSize(96, 96));
            visualHost.show();
            range.show();
            QCoreApplication::processEvents();
            const int labelWidth =
                QFontMetrics(speed->lineEdit()->font()).horizontalAdvance(speed->currentText());
            const QByteArray fitMessage =
                QStringLiteral("speed label fits at scale %1: text %2 px, available %3 px")
                    .arg(scale)
                    .arg(labelWidth)
                    .arg(speed->lineEdit()->width())
                    .toUtf8();
            require(labelWidth <= speed->lineEdit()->width(), fitMessage.constData());
            const QImage progress = previewImage(range);
            range.setPosition(600000);
            const QImage advanced = previewImage(range);
            const int railTop = qRound(range.height() / 2.0 - 2 * scale);
            const int railBottom = qRound(range.height() / 2.0 + 2 * scale);
            for (int y = 0; y < progress.height(); ++y) {
                if (y >= railTop - 1 && y <= railBottom)
                    continue;
                for (int x = qRound(270 * scale); x < qRound(370 * scale); ++x)
                    require(progress.pixelColor(x, y) == advanced.pixelColor(x, y),
                            "playback progress must not draw a marker outside the rail");
            }
            const qreal trackWidth = range.width() - 216 * scale;
            const qreal firstX = 188 * scale + trackWidth * 0.2;
            const qreal endX = 188 * scale + trackWidth * 0.8;
            for (int y : {railTop, railBottom - 1}) {
                require(progress.pixelColor(qCeil(firstX), y) ==
                            progress.pixelColor(qRound(270 * scale), y),
                        "progress rail meets the start handle with square corners");
                require(progress.pixelColor(qFloor(endX) - 1, y) ==
                            progress.pixelColor(qRound(370 * scale), y),
                        "selected rail meets the end handle with square corners");
            }
            range.setPosition(500000);
            if (!renders.isEmpty()) {
                QDir().mkpath(renders);
                const QFont previousFont = speed->font();
                const auto restoreFont =
                    qScopeGuard([&] { speed->setReferenceFont(previousFont); });
                speed->setReferenceFont(visualFont);
                QCoreApplication::processEvents();
                require(QFontMetrics(speed->lineEdit()->font())
                                .horizontalAdvance(speed->currentText()) <=
                            speed->lineEdit()->width(),
                        "the widest speed label also fits the real UI font at every scale");
                require(
                    previewTrimImage(visualHost, visualFont)
                        .save(renders + QStringLiteral("/trim-%1-%2.png")
                                            .arg(theme == ThemeMode::Dark ? QStringLiteral("dark")
                                                                          : QStringLiteral("light"))
                                            .arg(qRound(scale * 100))),
                    "save trim visual fixture");
                for (bool playing : {false, true}) {
                    range.setPlaying(playing);
                    require(previewTrimImage(visualHost, visualFont)
                                .save(renders + QStringLiteral("/trim-%1-%2-%3.png")
                                                    .arg(theme == ThemeMode::Dark
                                                             ? QStringLiteral("dark")
                                                             : QStringLiteral("light"))
                                                    .arg(qRound(scale * 100))
                                                    .arg(playing ? QStringLiteral("pause")
                                                                 : QStringLiteral("play"))),
                            "save both playback icons at every theme and scale");
                }
                range.setPlaying(false);
                speed->showPopup();
                QCoreApplication::processEvents();
                require(
                    previewTrimImage(*speed->view()->window(), visualFont)
                        .save(renders + QStringLiteral("/trim-speed-%1-%2.png")
                                            .arg(theme == ThemeMode::Dark ? QStringLiteral("dark")
                                                                          : QStringLiteral("light"))
                                            .arg(qRound(scale * 100))),
                    "save speed popup visual fixture");
                speed->hidePopup();
                if (scale == 2.0) {
                    const auto saveState = [&](const QString& state) {
                        require(previewTrimImage(visualHost, visualFont)
                                    .save(renders + QStringLiteral("/trim-%1-%2.png")
                                                        .arg(theme == ThemeMode::Dark
                                                                 ? QStringLiteral("dark")
                                                                 : QStringLiteral("light"),
                                                             state)),
                                "save trim interaction state fixture");
                    };
                    first->clearFocus();
                    end->clearFocus();
                    saveState(QStringLiteral("rest"));
                    QEnterEvent enter{QPointF(), QPointF(), QPointF()};
                    QApplication::sendEvent(end, &enter);
                    saveState(QStringLiteral("hover"));
                    QEvent leave(QEvent::Leave);
                    QApplication::sendEvent(end, &leave);
                    end->setFocus(Qt::TabFocusReason);
                    saveState(QStringLiteral("focus"));
                    range.setEnabled(false);
                    saveState(QStringLiteral("disabled"));
                    range.setEnabled(true);
                    end->setValue(first->value() + 1);
                    saveState(QStringLiteral("one-frame"));
                }
            }
        }
    }
    visualHost.hide();
    range.commitControlScale({});
    ThemeManager::instance().setThemeMode(previousTheme);
    class TrimTranslator final : public QTranslator {
      public:
        QString translate(const char* context, const char* source, const char*,
                          int) const override {
            if (QByteArray(context) == "RecordingTrimToolbar")
                return QStringLiteral("Translated ") + QString::fromUtf8(source);
            return {};
        }
    } translator;
    QCoreApplication::installTranslator(&translator);
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(&range, &languageChange);
    require(playPause->toolTip() == QStringLiteral("Translated Play") &&
                playPause->accessibleName() == playPause->toolTip(),
            "Play tooltip and accessible name retranslate live");
    range.setPlaying(true);
    require(playPause->toolTip() == QStringLiteral("Translated Pause") &&
                playPause->accessibleName() == playPause->toolTip(),
            "Pause tooltip and accessible name use the active language");
    require(first->accessibleName() == QStringLiteral("Translated Trim start") &&
                range.findChild<adqt::widgets::AdButton*>()->toolTip() ==
                    QStringLiteral("Translated Replay"),
            "trim accessibility and Replay retranslate live");
    require(first->toolTip().startsWith(QStringLiteral("Translated Trim start\n")),
            "timecode tooltips retranslate alongside the trim handle names");
    require(speed->accessibleName() == QStringLiteral("Translated Speed") &&
                speed->toolTip() == QStringLiteral("Translated Playback and export speed") &&
                speed->accessibleDescription() == speed->toolTip() && range.speedPercent() == 175 &&
                speed->currentText() == QStringLiteral("Translated 1.75x"),
            "speed accessibility and labels retranslate without changing the rate");
    QCoreApplication::removeTranslator(&translator);
    recordingTrimPlaybackTests(directory);
    for (bool deferred : {false, true}) {
        ScreenRecordingController controller(testEffectsSource);
        QString error;
        const QString original = directory + (deferred ? QStringLiteral("/speed-deferred.mp4")
                                                       : QStringLiteral("/speed-real.mp4"));
        require(controller.startAutomation(QRect(80, 80, 320, 240),
                                           {{QStringLiteral("post_processing"), deferred},
                                            {QStringLiteral("path"), original}},
                                           &error),
                "start variable-speed trim fixture");
        waitForRecording(controller);
        recordingToolbarButton("Trim Video")->click();
        auto* copy = recordingToolbarButton("Copy recording");
        auto* close = recordingToolbarButton("Close recording");
        wait([&] { return copy->isEnabled(); });
        auto* speedSelect = palette()->findChild<adqt::widgets::AdSelect*>(
            QStringLiteral("screenRecordingTrimSpeed"));
        require(speedSelect && speedSelect->currentValue().toUInt() == 100 &&
                    clipPreviewSpeed == 100,
                "every new trim session starts at normal playback speed");
        const auto closeWithSpeedEscapes = [&] {
            speedSelect->setFocus(Qt::TabFocusReason);
            speedSelect->showPopup();
            PhysicalKeyEvent dismissPress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(speedSelect->view(), &dismissPress);
            PhysicalKeyEvent dismissRelease(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(speedSelect->lineEdit(), &dismissRelease);
            require(controller.isOpen() && !speedSelect->popupVisible(),
                    "the first Escape dismisses speed while keeping the editor open");
            PhysicalKeyEvent closePress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(speedSelect->lineEdit(), &closePress);
            PhysicalKeyEvent closeRelease(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(speedSelect->lineEdit(), &closeRelease);
            require(!controller.isOpen(),
                    "a second Escape on the closed speed selector closes recording normally");
        };
        if (!deferred) {
            QFile source(original);
            require(source.open(QIODevice::WriteOnly) && source.write("original fixture") > 0,
                    "materialize the finalized speed source");
        }
        const int oldChanges = clipSpeedChanges;
        speedSelect->setCurrentValue(125u);
        require(clipSpeedChanges == oldChanges + 1 && clipPreviewSpeed == 125,
                "selecting speed updates preview independently of seeking");
        failClipSpeed = true;
        speedSelect->setCurrentValue(175u);
        failClipSpeed = false;
        require(speedSelect->currentValue().toUInt() == 125 && clipPreviewSpeed == 125,
                "a rejected native speed change restores the accepted selection");
        const int oldAutoplays = clipAutoplays;
        palette()
            ->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingReplay"))
            ->click();
        require(clipFirst == 0 && clipEnd == 30 && clipAutoplays == oldAutoplays + 1 &&
                    clipPreviewSpeed == 125,
                "Replay retains speed and the source frame range");
        speedSelect->showPopup();
        QKeyEvent escapePress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(speedSelect->view(), &escapePress);
        QKeyEvent escapeRelease(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(speedSelect->view(), &escapeRelease);
        require(controller.isOpen() && !speedSelect->popupVisible() &&
                    speedSelect->currentValue().toUInt() == 125,
                "Escape closes the speed menu before any recording shortcut");
        speedSelect->clearFocus();
        speedSelect->lineEdit()->clearFocus();
        if (uiOnly) {
            palette()->findChild<QSlider*>(QStringLiteral("screenRecordingTrimStart"))->setValue(5);
            palette()->findChild<QSlider*>(QStringLiteral("screenRecordingTrimEnd"))->setValue(20);
            const int beforeRateChange = clipAutoplays;
            speedSelect->setCurrentValue(50u);
            require(clipPreviewSpeed == 50 && clipAutoplays == beforeRateChange,
                    "changing speed while scrubbing keeps preview paused");
            palette()
                ->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingReplay"))
                ->click();
            require(clipFirst == 5 && clipEnd == 20 && clipPreviewSpeed == 50,
                    "native Replay retains speed and selected source boundaries");
            speedSelect->showPopup();
            QKeyEvent fastest(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
            QApplication::sendEvent(speedSelect->view(), &fastest);
            QKeyEvent confirm(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(speedSelect->view(), &confirm);
            require(!speedSelect->popupVisible() && clipPreviewSpeed == 300 &&
                        speedSelect->currentValue().toUInt() == 300,
                    "native keyboard selection updates preview and closes the speed popup");
            closeWithSpeedEscapes();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            continue;
        }
        const int oldExports = clipExports;
        copy->click();
        wait([&] { return copy->isEnabled() && clipExports == oldExports + 1; });
        require(clipExportSpeed == 125 && speedSelect->currentValue().toUInt() == 125,
                "full-range variable-speed export encodes at the selected rate");
        copy->click();
        require(clipExports == oldExports + 1, "unchanged range and speed reuse the export cache");
        speedSelect->setCurrentValue(200u);
        copy->click();
        wait([&] { return copy->isEnabled() && clipExports == oldExports + 2; });
        require(clipExportSpeed == 200,
                "changing only speed invalidates the previous successful export");
        speedSelect->setCurrentValue(100u);
        copy->click();
        const int unityExports = oldExports + (deferred ? 3 : 2);
        wait([&] { return copy->isEnabled() && clipExports == unityExports; });
        require(deferred ||
                    QApplication::clipboard()->mimeData()->urls().first().toLocalFile() == original,
                "returning to 1.0x permits original finalized file reuse");
        speedSelect->setCurrentValue(75u);
        clipExportState = SNOW_RECORDING_RENDER_STATE_FAILED;
        copy->click();
        wait([&] { return copy->isEnabled() && clipExports == unityExports + 1; });
        require(speedSelect->currentValue().toUInt() == 75 && clipPreviewSpeed == 75,
                "export failure retains the selected speed for retry");
        clipExportState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
        copy->click();
        wait([&] { return copy->isEnabled() && clipExports == unityExports + 2; });
        require(clipExportSpeed == 75, "retry uses the retained speed");
        speedSelect->setCurrentValue(25u);
        clipExportState = SNOW_RECORDING_RENDER_STATE_RUNNING;
        copy->click();
        adqt::widgets::AdModal* cancelModal = nullptr;
        wait([&] {
            cancelModal = controller.findChild<adqt::widgets::AdModal*>(
                QStringLiteral("screenRecordingRenderModal"));
            return cancelModal && cancelModal->isOpen();
        });
        cancelModal->footerWidget()
            ->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingRenderCancel"))
            ->click();
        wait([&] { return copy->isEnabled() && clipExports == unityExports + 3; });
        require(speedSelect->currentValue().toUInt() == 25 && clipPreviewSpeed == 25,
                "canceling export preserves the slow-motion selection");
        clipExportState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
        copy->click();
        wait([&] { return copy->isEnabled() && clipExports == unityExports + 4; });
        require(clipExportSpeed == 25, "retry after cancellation exports the retained speed");
        speedSelect->setCurrentValue(50u);
        speedSelect->showPopup();
        clipExportState = SNOW_RECORDING_RENDER_STATE_RUNNING;
        copy->click();
        require(!speedSelect->isEnabled() && !speedSelect->popupVisible() && close->isEnabled(),
                "export locks speed and closes its popup while Close stays available");
        // Programmatic child updates cannot change an in-flight request or its cache identity.
        speedSelect->setCurrentValue(300u);
        clipExportState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
        wait([&] { return copy->isEnabled() && clipExports == unityExports + 5; });
        require(clipExportSpeed == 50,
                "export snapshots speed even if a disabled control is changed programmatically");
        speedSelect->setCurrentValue(50u);
        copy->click();
        require(clipExports == unityExports + 5,
                "successful output is cached under the submitted speed");
        QPointer<adqt::widgets::AdSelect> retiredSpeed = speedSelect;
        if (deferred) {
            closeWithSpeedEscapes();
        } else {
            speedSelect->showPopup();
            close->click();
        }
        require(!retiredSpeed || !retiredSpeed->popupVisible(),
                "closing the editor immediately retires its speed popup");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    if (uiOnly)
        return;
    for (bool deferred : {false, true}) {
        for (bool paused : {false, true}) {
            ScreenRecordingController controller(testEffectsSource);
            QStringList notificationPaths;
            QObject::connect(&controller, &ScreenRecordingController::exportNotificationRequested,
                             &controller,
                             [&](const QString& path) { notificationPaths.append(path); });
            QVector<bool> captureActivity;
            QObject::connect(&controller, &ScreenRecordingController::captureActivityChanged,
                             &controller, [&](bool active) { captureActivity.push_back(active); });
            QString error;
            const QString original =
                directory + (deferred ? QStringLiteral("/deferred") : QStringLiteral("/real")) +
                (paused ? QStringLiteral("-paused.mp4") : QStringLiteral(".mp4"));
            const int oldRenders = renderStarts, oldExports = clipExports,
                      oldAutoplays = clipAutoplays;
            require(controller.startAutomation(QRect(80, 80, 320, 240),
                                               {{QStringLiteral("post_processing"), deferred},
                                                {QStringLiteral("path"), original}},
                                               &error),
                    "start trim fixture");
            waitForRecording(controller);
            require(captureActivity == QVector<bool>{true},
                    "recording suppresses floating surfaces before entering trim");
            auto* close = recordingToolbarButton("Close recording");
            auto* trim = recordingToolbarButton("Trim Video");
            auto* save = recordingToolbarButton("Save to File");
            auto* copy = recordingToolbarButton("Copy recording");
            require(close && trim && save && copy && close->x() < trim->x() &&
                        trim->x() < save->x() && save->x() < copy->x(),
                    "main toolbar ends with Close, Trim, Save, Copy");
            if (paused)
                require(controller.controlAutomation(QStringLiteral("pause"), {}, &error),
                        "pause before trim");
            trim->click();
            require(recordingStopRequested && close->isEnabled(),
                    "trim freezes capture immediately and keeps Close enabled");
            require(captureActivity == QVector<bool>({true, false}),
                    "entering trim restores floating surfaces before source finalization");
            wait([&] {
                return controller.automationState().value(QStringLiteral("state")) ==
                           QStringLiteral("trimming") &&
                       copy->isEnabled();
            });
            require(clipDeferred == deferred &&
                        clipPath == (deferred ? QString::fromUtf8(renderSourcePath) : original),
                    "preview opens clean deferred source or finalized real-time output");
            require(renderStarts == oldRenders && clipAutoplays == oldAutoplays + 1,
                    "trim suppresses deferred rendering and autoplays exactly once");
            if (!renders.isEmpty() && !deferred && !paused) {
                for (auto theme : {ThemeMode::Light, ThemeMode::Dark}) {
                    ThemeManager::instance().setThemeMode(theme);
                    QCoreApplication::processEvents();
                    require(previewTrimImage(*palette()->window(), visualFont)
                                .save(renders + (theme == ThemeMode::Dark
                                                     ? QStringLiteral("/toolbar-dark.png")
                                                     : QStringLiteral("/toolbar-light.png"))),
                            "render the complete recording toolbar and selected Trim icon");
                }
                ThemeManager::instance().setThemeMode(previousTheme);
            }
            if (!deferred) {
                QFile originalFile(original);
                require(originalFile.open(QIODevice::WriteOnly), "create finalized source fixture");
                originalFile.write("original fixture");
                originalFile.close();
                copy->click();
                require(copy->isEnabled() && clipExports == oldExports &&
                            QApplication::clipboard()->mimeData()->urls().first().toLocalFile() ==
                                original,
                        "unchanged full-range Copy reuses the original finalized file");
            }
            require(!palette()->activateDrawingShortcut(QStringLiteral("shape")),
                    "drawing shortcuts stay unavailable in trim mode");
            auto* start =
                palette()->findChild<QSlider*>(QStringLiteral("screenRecordingTrimStart"));
            auto* finish = palette()->findChild<QSlider*>(QStringLiteral("screenRecordingTrimEnd"));
            start->setValue(5);
            finish->setValue(20);
            require(clipFirst == 19 && clipEnd == 20,
                    "end scrubbing pauses on its last included frame");
            palette()
                ->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenRecordingReplay"))
                ->click();
            require(clipFirst == 5 && clipEnd == 20 && clipAutoplays == oldAutoplays + 2,
                    "Replay uses the selected half-open range");
            const auto captureRegion = controller.automationState().value(QStringLiteral("region"));
            ScreenRecordingAreaWindow* area = nullptr;
            for (auto* widget : QApplication::topLevelWidgets())
                if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget);
                    candidate && candidate->isVisible())
                    area = candidate;
            require(area != nullptr, "trim area exists");
            const QSize size = area->size();
            area->move(area->pos() + QPoint(10, 10));
            QCoreApplication::processEvents();
            require(area->size() == size && controller.automationState().value(
                                                QStringLiteral("region")) == captureRegion,
                    "moving the preview never changes the immutable export geometry");
            controller.open(QRect(0, 0, 600, 600));
            require(area->size() == size && controller.automationState().value(
                                                QStringLiteral("region")) == captureRegion,
                    "reopening cannot replace the geometry of an active trim editor");
            copy->click();
            wait([&] { return copy->isEnabled() && clipExports == oldExports + 1; });
            require(controller.isOpen() && QApplication::clipboard()->mimeData()->hasUrls(),
                    "Copy keeps the trim editor open");
            require(QApplication::clipboard()->mimeData()->urls() ==
                        QList<QUrl>{QUrl::fromLocalFile(notificationPaths.constLast())},
                    "trimmed Copy notification shows the exported clip path");
            copy->click();
            require(clipExports == oldExports + 1,
                    "unchanged trim exports reuse the durable cache");
            if (!deferred && !paused) {
                QFile originalFile(original);
                require(originalFile.open(QIODevice::WriteOnly),
                        "materialize the retained source fixture");
                originalFile.write("original fixture");
                originalFile.close();
                const QString destination = directory + QStringLiteral("/saved-trim");
                QTimer choose;
                QObject::connect(&choose, &QTimer::timeout, [&] {
                    for (auto* widget : QApplication::topLevelWidgets()) {
                        if (auto* dialog = qobject_cast<QFileDialog*>(widget)) {
                            require(
                                dialog->defaultSuffix() == QStringLiteral("mp4") &&
                                    !dialog->testOption(QFileDialog::DontConfirmOverwrite),
                                "Save resolves extensions before native replacement confirmation");
                            dialog->selectFile(destination);
                            QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
                        }
                    }
                });
                choose.start(10);
                save->click();
                choose.stop();
                wait([&] { return copy->isEnabled(); });
                QFile saved(destination + QStringLiteral(".mp4"));
                require(saved.open(QIODevice::ReadOnly) &&
                            saved.readAll() == QByteArray("trim fixture") &&
                            clipExports == oldExports + 1 && controller.isOpen(),
                        "Save atomically publishes the cached trim and keeps the editor open");
                require(
                    notificationPaths.constLast() == destination + QStringLiteral(".mp4"),
                    "trimmed Save notification shows the chosen destination with its extension");
                require(originalFile.open(QIODevice::ReadOnly) &&
                            originalFile.readAll() == QByteArray("original fixture"),
                        "Save never replaces the original source");
            }
            start->setValue(7);
            const auto notificationsBeforeFailure = notificationPaths.size();
            clipExportError = "encode error: video packet mux: Invalid argument";
            clipExportState = SNOW_RECORDING_RENDER_STATE_FAILED;
            copy->click();
            wait([&] { return copy->isEnabled() && clipExports == oldExports + 2; });
            clipExportError.clear();
            require(start->value() == 7 && finish->value() == 20 && controller.isOpen(),
                    "export failure preserves the source session and selected range");
            require(notificationPaths.size() == notificationsBeforeFailure,
                    "failed trimmed exports do not notify");
            require(diagnostics.flush(), "flush the failed trim's diagnostic evidence");
            QFile diagnosticFile(diagnostics.status().currentFile);
            require(diagnosticFile.open(QIODevice::ReadOnly), "read trim diagnostic log");
            QJsonObject lastFailure;
            bool loggedControllerError = false;
            bool loggedUnexplainedFailure = false;
            for (const auto& line : diagnosticFile.readAll().split('\n')) {
                const auto record = QJsonDocument::fromJson(line).object();
                if (record.value(QStringLiteral("event")) == QStringLiteral("recording.failed") &&
                    record.value(QStringLiteral("message"))
                        .toString()
                        .contains(QStringLiteral("video packet mux: Invalid argument")))
                    loggedControllerError = true;
                if (record.value(QStringLiteral("event")) !=
                    QStringLiteral("recording.trim_export_failed"))
                    continue;
                if (record.value(QStringLiteral("message")) ==
                    QStringLiteral("Recording export failed without a native error message"))
                    loggedUnexplainedFailure = true;
                const auto fields = record.value(QStringLiteral("fields")).toObject();
                if (fields.value(QStringLiteral("deferred")).toBool() != deferred)
                    continue;
                lastFailure = record;
            }
            const auto fields = lastFailure.value(QStringLiteral("fields")).toObject();
            require(fields.value(QStringLiteral("first_frame")).toInt() == 7 &&
                        fields.value(QStringLiteral("end_frame")).toInt() == 20 &&
                        fields.value(QStringLiteral("speed_percent")).toInt() == 100 &&
                        fields.value(QStringLiteral("action")) == QStringLiteral("copy") &&
                        !fields.value(QStringLiteral("cache_hit")).toBool() &&
                        !fields.value(QStringLiteral("operation")).toString().isEmpty() &&
                        lastFailure.value(QStringLiteral("message"))
                            .toString()
                            .contains(QStringLiteral("video packet mux: Invalid argument")),
                    "failed trim logs the submitted range, rate, action, and native error");
            require(loggedControllerError, "controller failure logs retain the displayed error");
            require(
                loggedUnexplainedFailure,
                "failed native tasks without error text are logged as failures, not cancellation");
            clipExportState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
            copy->click();
            wait([&] { return copy->isEnabled() && clipExports == oldExports + 3; });
            start->setValue(6);
            clipExportState = SNOW_RECORDING_RENDER_STATE_RUNNING;
            copy->click();
            require(!start->isEnabled() && close->isEnabled(),
                    "export locks range editing while Close stays available");
            adqt::widgets::AdModal* cropModal = nullptr;
            wait([&] {
                cropModal = controller.findChild<adqt::widgets::AdModal*>(
                    QStringLiteral("screenRecordingRenderModal"));
                return cropModal && cropModal->isOpen();
            });
            auto* cropProgress = cropModal->contentWidget()->findChild<adqt::widgets::AdProgress*>(
                QStringLiteral("screenRecordingRenderProgress"));
            auto* cancel = cropModal->footerWidget()->findChild<adqt::widgets::AdButton*>(
                QStringLiteral("screenRecordingRenderCancel"));
            require(cropProgress && cancel &&
                        cropModal->windowTitle() == QStringLiteral("Rendering recording") &&
                        cropProgress->accessibleName() == QStringLiteral("Rendering progress"),
                    "crop export uses the shared rendering modal and accessible copy");
            wait([&] { return cropProgress->percent() == 41; });
            class CropRenderTranslator final : public QTranslator {
              public:
                bool isEmpty() const override {
                    return false;
                }
                QString translate(const char* context, const char* source, const char*,
                                  int) const override {
                    return QByteArray(context) == "RecordingRenderDialog"
                               ? QStringLiteral("Translated ") + QString::fromUtf8(source)
                               : QString();
                }
            } cropTranslator;
            QCoreApplication::installTranslator(&cropTranslator);
            QEvent cropLanguageChange(QEvent::LanguageChange);
            QApplication::sendEvent(cropModal->contentWidget(), &cropLanguageChange);
            require(cropModal->windowTitle() == QStringLiteral("Translated Rendering recording") &&
                        cancel->text() == QStringLiteral("Translated Cancel") &&
                        cropProgress->accessibleName() ==
                            QStringLiteral("Translated Rendering progress"),
                    "crop export retranslates from the shared rendering context");
            QCoreApplication::removeTranslator(&cropTranslator);
            cancel->click();
            require(!cancel->isEnabled(), "cancel disables repeated cancellation immediately");
            wait([&] { return copy->isEnabled(); });
            require(!controller.findChild<adqt::widgets::AdModal*>(
                        QStringLiteral("screenRecordingRenderModal")) &&
                        controller.isOpen() && start->value() == 6 && finish->value() == 20,
                    "cancel closes the shared modal and preserves the crop selection");
            clipExportState = SNOW_RECORDING_RENDER_STATE_RUNNING;
            copy->click();
            wait([&] {
                return controller.findChild<adqt::widgets::AdModal*>(
                           QStringLiteral("screenRecordingRenderModal")) != nullptr;
            });
            close->click();
            require(!controller.findChild<adqt::widgets::AdModal*>(
                        QStringLiteral("screenRecordingRenderModal")),
                    "detaching the crop editor removes its rendering modal");
            clipExportState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
            wait([&] { return !controller.isOpen(); });
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            require(recordingWindowCount() == 0 && captureActivity == QVector<bool>({true, false}),
                    "closing trim retires its windows without reacquiring capture suppression");
        }
    }
    for (bool deferred : {false, true}) {
        for (bool restart : {false, true}) {
            ScreenRecordingController controller(testEffectsSource);
            QVector<bool> captureActivity;
            QObject::connect(&controller, &ScreenRecordingController::captureActivityChanged,
                             &controller, [&](bool active) { captureActivity.push_back(active); });
            QString error;
            require(controller.startAutomation(QRect(80, 80, 320, 240),
                                               {{QStringLiteral("post_processing"), deferred}},
                                               &error),
                    "start crop exit fixture");
            waitForRecording(controller);
            auto* trim = recordingToolbarButton("Trim Video");
            trim->click();
            wait([&] { return trim->isEnabled(); });
            require(captureActivity == QVector<bool>({true, false}),
                    "trim playback keeps floating surfaces available");
            auto* start = recordingToolbarButton("Start recording");
            QPointer<QSlider> timeline =
                palette()->findChild<QSlider*>(QStringLiteral("screenRecordingTrimStart"));
            require(start && start->isEnabled(), "crop editor allows starting a new recording");
            if (restart) {
                start->click();
                waitForRecording(controller);
                require(captureActivity == QVector<bool>({true, false, true}),
                        "restarting from trim reacquires capture suppression");
            } else {
                trim->click();
                require(controller.automationState().value(QStringLiteral("state")) ==
                                QStringLiteral("idle") &&
                            palette()->recordingExportSettingsVisible(),
                        "active crop button returns to idle export settings");
                require(palette()->activateDrawingShortcut(QStringLiteral("shape")),
                        "leaving crop restores drawing controls");
            }
            require(!timeline || !timeline->isVisible(), "leaving crop hides the timeline");
            recordingToolbarButton("Close recording")->click();
            wait([&] { return !controller.isRecording(); });
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            require(captureActivity == (restart ? QVector<bool>({true, false, true, false})
                                                : QVector<bool>({true, false})),
                    "closing after trim releases only an active capture suppression");
        }
    }
    for (bool deferred : {false, true}) {
        ScreenRecordingController controller(testEffectsSource);
        QStringList notificationPaths;
        wait([] {
            return snow_shot::runtime::RuntimeActivityTracker::shared().snapshot().activeCount == 0;
        });
        QObject::connect(&controller, &ScreenRecordingController::exportNotificationRequested,
                         &controller, [&](const QString& path) { notificationPaths.append(path); });
        QString error;
        require(controller.startAutomation(QRect(80, 80, 320, 240),
                                           {{QStringLiteral("post_processing"), deferred}}, &error),
                "start detached trim export fixture");
        waitForRecording(controller);
        recordingToolbarButton("Trim Video")->click();
        wait([&] {
            return controller.automationState().value(QStringLiteral("state")) ==
                       QStringLiteral("trimming") &&
                   !controller.automationState().value(QStringLiteral("busy")).toBool();
        });
        palette()->findChild<QSlider*>(QStringLiteral("screenRecordingTrimStart"))->setValue(5);
        clipExportState = SNOW_RECORDING_RENDER_STATE_RUNNING;
        recordingToolbarButton("Copy recording")->click();
        require(notificationPaths.isEmpty(),
                "pending detached export has no completion notification");
        recordingToolbarButton("Close recording")->click();
        require(!controller.isOpen(), "recording windows close while trimmed export continues");
        requireMemoryAdmissionBlocked(
            "detached trim export retains memory admission after recording windows close");
        std::promise<void> releaseClipDestroy;
        clipDestroyGate = releaseClipDestroy.get_future().share();
        clipDestroyEntered = false;
        const int oldClips = clipDestroys.load();
        clipExportState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
        wait([&] { return notificationPaths.size() == 1; });
        require(!controller.isOpen() &&
                    QApplication::clipboard()->mimeData()->urls() ==
                        QList<QUrl>{QUrl::fromLocalFile(notificationPaths.first())},
                "detached trim export notifies with its completed path without reopening windows");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        wait([] { return clipDestroyEntered.load(); });
        requireMemoryAdmissionBlocked(
            "detached trim media destruction retains memory admission after publication");
        releaseClipDestroy.set_value();
        wait([&] { return clipDestroys > oldClips; });
        clipDestroyGate = {};
    }
    for (bool deferred : {false, true}) {
        ScreenRecordingController controller(testEffectsSource);
        QVector<bool> captureActivity;
        QObject::connect(&controller, &ScreenRecordingController::captureActivityChanged,
                         &controller, [&](bool active) { captureActivity.push_back(active); });
        QString error;
        require(controller.startAutomation(QRect(80, 80, 320, 240),
                                           {{QStringLiteral("post_processing"), deferred}}, &error),
                "start closing-during-preparation fixture");
        waitForRecording(controller);
        std::promise<void> release, entered;
        auto reached = entered.get_future();
        exportGate = release.get_future().share();
        exportEntered = &entered;
        const int beforeRenders = renderStarts, beforeOpens = clipOpens,
                  beforeDiscards = sourceDiscards, beforeSessions = destroyedSessions;
        recordingToolbarButton("Trim Video")->click();
        require(reached.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "trim finalization reaches its worker barrier");
        recordingToolbarButton("Close recording")->click();
        require(!controller.isOpen(), "Close stays responsive during trim preparation");
        require(captureActivity == QVector<bool>({true, false}),
                "closing during trim preparation leaves floating surfaces available");
        release.set_value();
        exportEntered = nullptr;
        exportGate = {};
        wait([&] { return destroyedSessions > beforeSessions; });
        require(renderStarts == beforeRenders && clipOpens == beforeOpens &&
                    sourceDiscards == beforeDiscards,
                "closing preparation retains the source without rendering effects or starting "
                "playback");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(recordingWindowCount() == 0 && captureActivity == QVector<bool>({true, false}),
                "detached trim completion cannot reopen windows or suppress floating surfaces");
    }
    ScreenRecordingController controller(testEffectsSource);
    QString error;
    require(controller.startAutomation(QRect(80, 80, 320, 240), {}, &error), "start Save fixture");
    waitForRecording(controller);
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* dialog = qobject_cast<QFileDialog*>(widget))
                dialog->reject();
    });
    dismiss.start(10);
    recordingToolbarButton("Save to File")->click();
    wait([&] {
        return controller.automationState().value(QStringLiteral("state")) ==
               QStringLiteral("trimming");
    });
    QCoreApplication::processEvents();
    require(controller.isOpen(), "canceling Save preserves a stopped recording in the editor");
    palette()->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void recordingSessionEndPreservationCase(const QString& mode, const QString& directory) {
    using namespace snow_shot::presentation::recording;
    using snow_shot::storage::RecordingSettings;
    const bool deferred = mode == u"deferred";
    const bool failed = mode == u"failed";
    const bool timedOut = mode == u"timeout";
    require(deferred || failed || timedOut || mode == u"direct",
            "terminal recording case must be recognized");
    qputenv("LOCALAPPDATA", directory.toUtf8());
    const RecordingSettings settings;
    require(settings.setStartDelaySeconds(0) && settings.setPostProcessingEnabled(deferred) &&
                settings.setDeleteFilesOnRecordingClose(false),
            "terminal recording fixture must configure its pipeline without a countdown");
    recordingFileFixture = true;
    failExport = failed;
    const QString sourcePath = QDir(directory).filePath(QStringLiteral("retained.snowrec"));
    renderSourcePath = sourcePath.toUtf8();
    if (deferred) {
        QFile source(sourcePath);
        require(source.open(QIODevice::WriteOnly) && source.write("deferred media") == 14,
                "deferred fixture needs a durable source file");
    }
    auto controller = std::make_unique<ScreenRecordingController>(testEffectsSource);
    controller->open(QRect(40, 40, 320, 240));
    controller->startRecording();
    waitForRecording(*controller);
    const QString output = QDir::fromNativeSeparators(recordingOutputPath);
    const QString staging = lastCreatedSession->stagingFile;
    require(starts == 1 && !output.isEmpty() && QFile::exists(staging),
            "terminal preservation must begin with a live fake native session");
    const int beforeDestroyed = destroyedSessions.load();
    const int beforeSources = sourceDestroys.load();
    std::promise<void> release;
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    if (timedOut) {
        exportGate = release.get_future().share();
        exportEntered = &entered;
    }
    controller->prepareForSessionEnd(timedOut ? 0 : 2000);
    if (timedOut) {
        require(enteredFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready &&
                    destroyedSessions == beforeDestroyed && exports == 0 && QFile::exists(staging),
                "expired terminal budget must retain a native owner while finalization is blocked");
    }
    const auto markerDirectory = snow_shot::app::distribution::writableDataDirectory();
    const QString marker =
        QDir(markerDirectory).filePath(QStringLiteral("recording-recovery.json"));
    if (snow_shot::app::distribution::isStoreManaged()) {
        const auto record = loadRecovery(marker);
        require(record && record->outputPath == output &&
                    record->sourcePath == (deferred ? sourcePath : QString()),
                "terminal controller must publish its actual output and deferred recovery paths");
    }
    if (timedOut) {
        release.set_value();
        QElapsedTimer wait;
        wait.start();
        while (destroyedSessions == beforeDestroyed && wait.elapsed() < 3000)
            QThread::msleep(1);
        require(destroyedSessions == beforeDestroyed + 1,
                "terminal worker must eventually release a successfully finalized native owner");
    }
    require(recordingStopRequested && exports == 1 && renderStarts == 0 && sourceDiscards == 0,
            "terminal preparation must freeze and finalize once without rendering or discarding");
    require(
        destroyedSessions == beforeDestroyed + (failed ? 0 : 1) &&
            QFile::exists(staging) == failed && QFile::exists(output) == !failed,
        "successful stop publishes output while failed stop retains native staging and ownership");
    require(sourceDestroys == beforeSources + (deferred ? 1 : 0) &&
                (!deferred || QFileInfo::exists(sourcePath)),
            "deferred finalization must release its source handle while retaining durable media");
    controller->prepareForSessionEnd(0);
    controller.reset();
    require(
        exports == 1 && destroyedSessions == beforeDestroyed + (failed ? 0 : 1),
        "repeated terminal preparation and controller destruction cannot stop or destroy twice");
}

void recordingSessionEndPreservation() {
    QTemporaryDir fixture;
    require(fixture.isValid(), "terminal subprocesses need parent-owned temporary storage");
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto* key : {"TEMP", "TMP", "TMPDIR"})
        environment.insert(QString::fromLatin1(key), fixture.path());
    for (const auto* mode : {"direct", "failed", "deferred", "timeout"}) {
        QProcess child;
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--store-preservation-case=") + QString::fromLatin1(mode),
                     QStringLiteral("-platform"), QStringLiteral("offscreen")});
        const bool passed = child.waitForStarted() && child.waitForFinished(15000) &&
                            child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0;
        if (!passed) {
            std::cerr << mode << ": process state=" << static_cast<int>(child.state())
                      << " exit status=" << static_cast<int>(child.exitStatus())
                      << " exit code=" << child.exitCode()
                      << " error=" << child.errorString().toStdString() << '\n'
                      << child.readAllStandardOutput().constData()
                      << child.readAllStandardError().constData();
            if (child.state() != QProcess::NotRunning) {
                child.kill();
                child.waitForFinished();
            }
        }
        require(passed, "terminal recording integration subprocess must pass");
    }
}

void recordingAreaHistoryTests() {
    using namespace snow_shot::storage;
    auto& configuration = ApplicationStorage::instance().configuration();
    const auto historyKey = QStringLiteral("screen_recording/area_history");
    const RecordingAreaHistorySettings history;
    const RecordingSettings settings;
    require(settings.setStartDelaySeconds(0) && settings.setPostProcessingEnabled(false) &&
                settings.setDeleteFilesOnRecordingClose(false) &&
                settings.setAutoExitAfterRecordingEnds(false),
            "configure recording area tests");
    const QRect first(20, 20, 301, 201);
    const QRect second(60, 60, 321, 241);
    const QRect current(100, 100, 351, 251);
    const auto space = recordingAreaCoordinateSpace();
    const auto otherSpace = space == QStringLiteral("desktop_points")
                                ? QStringLiteral("physical_pixels")
                                : QStringLiteral("desktop_points");
    require(configuration.setValue(
                historyKey,
                QJsonArray{recordingAreaToJson({first, space}), recordingAreaToJson({first, space}),
                           recordingAreaToJson({second, space}),
                           recordingAreaToJson({current, otherSpace}),
                           recordingAreaToJson({QRect(100000, 100000, 300, 200), space})}),
            "seed chronological history with duplicate and unavailable areas");
    const auto key = [](ScreenRecordingAreaWindow* area, Qt::Key value,
                        Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        area->activateInput();
        QCoreApplication::processEvents();
        PhysicalKeyEvent down(QEvent::KeyPress, value, modifiers);
        PhysicalKeyEvent up(QEvent::KeyRelease, value, modifiers);
        QApplication::sendEvent(area, &down);
        QApplication::sendEvent(area, &up);
    };
    const auto wait = [](const auto& condition) {
        QElapsedTimer deadline;
        deadline.start();
        while (!condition() && deadline.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        require(condition(), "recording area fixture must complete its asynchronous operation");
    };
    {
        ScreenRecordingController controller(testEffectsSource);
        controller.open(current);
        auto* area = recordingAreaWindow();
        auto* controls = palette();
        require(history.entries().size() == 5, "opening a recording window does not save an area");
        key(area, Qt::Key_R, Qt::ShiftModifier);
        require(area->recordingRegion() == current, "Next does nothing before browsing");
        key(area, Qt::Key_R);
        require(area->recordingRegion() == second, "Previous selects newest usable entry, skipping "
                                                   "unavailable coordinate spaces and displays");
        key(area, Qt::Key_R);
        require(area->recordingRegion() == first, "Previous moves toward older starts");
        key(area, Qt::Key_R);
        key(area, Qt::Key_R);
        require(area->recordingRegion() == first, "Previous stops at the oldest entry");
        key(area, Qt::Key_R, Qt::ShiftModifier);
        require(area->recordingRegion() == first, "duplicates remain separate navigation stops");
        key(area, Qt::Key_R, Qt::ShiftModifier);
        require(area->recordingRegion() == second, "Next moves toward newer starts");
        key(area, Qt::Key_R, Qt::ShiftModifier);
        require(area->recordingRegion() == second, "Next stops at the newest usable entry");
        require(area->restoreRecordingRegion(current), "simulate an idle region edit");
        key(area, Qt::Key_R, Qt::ShiftModifier);
        require(area->recordingRegion() == current,
                "manual region changes reset the history cursor");
        key(area, Qt::Key_R);
        require(area->recordingRegion() == second && history.entries().size() == 5,
                "browsing restarts at the newest entry without appending history");
        controller.startRecording();
        key(area, Qt::Key_R, Qt::ShiftModifier);
        waitForRecording(controller);
        require(history.entries().size() == 6 && history.entries().last().rectangle == second,
                "successful startup saves the selected area exactly once");
#ifndef Q_OS_MACOS
        require(lastDirectConfig.width == 322 && lastDirectConfig.height == 242,
                "recording uses compatible even capture dimensions while history preserves odd "
                "selection dimensions");
#endif
        controls->recordingPauseRequested();
        key(area, Qt::Key_R, Qt::ShiftModifier);
        require(area->recordingRegion() == second && history.entries().size() == 6,
                "paused recording cannot navigate or append history");
        controls->recordingResumeRequested();
        require(history.entries().size() == 6, "resume does not append another start");
        controls->recordingCloseRequested();
        wait([&] { return !controller.isRecording(); });
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        ScreenRecordingController controller(testEffectsSource);
        controller.open(current);
        auto* area = recordingAreaWindow();
        key(area, Qt::Key_R);
        require(area->recordingRegion() == second, "reopened windows browse saved recording areas");
        key(area, Qt::Key_R);
        key(area, Qt::Key_R);
        require(area->recordingRegion() == first, "browse older history before a failed start");
        ErrorObserver errors;
        qApp->installEventFilter(&errors);
        for (int failure = 0; failure < 2; ++failure) {
            failStart = failure == 0;
            failStartOperation = failure == 1;
            controller.startRecording();
            wait([&] { return errors.shown == failure + 1 && !palette()->recordingBusy(); });
            require(history.entries().size() == 6,
                    "failed backend creation or start does not save an area");
            if (failure == 0) {
                key(area, Qt::Key_R, Qt::ShiftModifier);
                require(area->recordingRegion() == second,
                        "failed startup preserves the existing history navigation cursor");
            }
        }
        failStart = false;
        failStartOperation = false;
        qApp->removeEventFilter(&errors);
        require(settings.setStartDelaySeconds(10), "enable countdown");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        ScreenRecordingController controller(testEffectsSource);
        controller.open(current);
        controller.startRecording();
        auto* area = recordingAreaWindow();
        key(area, Qt::Key_R);
        require(area->recordingRegion() == current && history.entries().size() == 6,
                "countdown blocks navigation and does not save an area");
        palette()->recordingCloseRequested();
    }
    require(settings.setStartDelaySeconds(0), "disable countdown");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        std::promise<void> releaseStart;
        sessionCreateGate = releaseStart.get_future().share();
        sessionCreateEntered = false;
        auto controller = std::make_unique<ScreenRecordingController>(testEffectsSource);
        controller->open(current);
        controller->startRecording();
        wait([] { return sessionCreateEntered.load(); });
        auto* area = recordingAreaWindow();
        key(area, Qt::Key_R);
        require(area->recordingRegion() == current && history.entries().size() == 6,
                "pending backend startup freezes navigation and has not saved history yet");
        controller.reset();
        releaseStart.set_value();
        wait([&] { return history.entries().size() == 7; });
        require(history.entries().last().rectangle == current,
                "a successful start after controller destruction still commits its selected area");
        sessionCreateGate = {};
    }
    {
        ScreenRecordingController controller(testEffectsSource);
        controller.open(first);
        controller.startRecording();
        waitForRecording(controller);
        require(history.entries().size() == 8, "every successful recording appends a new entry");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        require(configuration.setValue(historyKey, QJsonArray{}), "clear fixture history");
        ScreenRecordingController controller(testEffectsSource);
        controller.open(current);
        auto* area = recordingAreaWindow();
        key(area, Qt::Key_R);
        key(area, Qt::Key_R, Qt::ShiftModifier);
        require(area->recordingRegion() == current && palette()->recordingExportSettingsVisible(),
                "empty history leaves both geometry and the selected tool unchanged");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        ScreenRecordingController controller(testEffectsSource);
        controller.open(current);
        configuration.suspendWrites(true);
        const auto resumeWrites = qScopeGuard([&] { configuration.suspendWrites(false); });
        controller.startRecording();
        waitForRecording(controller);
        require(history.entries().isEmpty(),
                "a history persistence failure does not interrupt successful recording");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        std::promise<void> releaseStart;
        sessionCreateGate = releaseStart.get_future().share();
        sessionCreateEntered = false;
        ScreenRecordingController controller(testEffectsSource);
        controller.open(first);
        controller.startRecording();
        wait([] { return sessionCreateEntered.load(); });
        releaseStart.set_value();
        controller.prepareForSessionEnd(3000);
        require(history.entries().size() == 1 && history.entries().last().rectangle == first,
                "terminal preservation commits successful startup without processing GUI events");
        QCoreApplication::processEvents();
        require(history.entries().size() == 1,
                "queued and terminal startup completion cannot save the same start twice");
        sessionCreateGate = {};
    }
}

template <typename Application> int runRecordingControllerTests(Application& app) {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary storage must exist");
    using namespace snow_shot::storage;
    const auto shutdownStorage = qScopeGuard([] { ApplicationStorage::instance().shutdown(); });
    require(ApplicationStorage::instance()
                .initialize({temporary.filePath("bin"), temporary.filePath("settings"), 0})
                .success,
            "isolated storage must initialize");
    require(RecordingSettings().setVideoSaveDirectory(temporary.path()),
            "test output directory must be set");
    require(RecordingSettings().setCaptureToolbarInRecording(true),
            "capture exclusion must be disabled for fake backend");
    if (app.arguments().contains(QStringLiteral("--toolbar-ownership-only"))) {
        recordingToolbarOwnershipSurvivesSurfaceRecreation();
        return 0;
    }
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--toolbar-exclusion-stacking-only"))) {
        recordingCaptureExclusionPreservesToolbarStacking();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--native-capture-exclusion-only")) ||
        app.arguments().contains(QStringLiteral("--native-capture-exclusion-affinity-only"))) {
        ScreenRecordingToolbarWindow toolbar;
        int result = verifyToolbarCaptureExclusionAffinity(*toolbar.autoHideControlWindow(),
                                                           "recording-edge", false);
        result |=
            app.arguments().contains(QStringLiteral("--native-capture-exclusion-affinity-only"))
                ? verifyToolbarCaptureExclusionAffinity(toolbar, "recording")
                : verifyToolbarCaptureExclusion(toolbar, "recording");
        return result;
    }
    if (app.arguments().contains(QStringLiteral("--native-toolbar-ready-display-only"))) {
        const int result = recordingToolbarAcrossNativeDisplays();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return result;
    }
#endif
#ifdef SNOW_RECORDING_WINDOW_STARTUP_BENCHMARK
    if (app.arguments().contains(QStringLiteral("--recording-window-startup-performance"))) {
        const int result = runRecordingWindowStartupPerformanceBenchmark(app);
        return result;
    }
#endif
#ifdef Q_OS_WIN
#ifdef SNOW_RECORDING_EFFECTS_BENCHMARK
    if (app.arguments().contains(QStringLiteral("--effects-preview-performance"))) {
        const int result = runRecordingEffectsPerformanceBenchmark(app);
        return result;
    }
#endif
    if (app.arguments().contains(QStringLiteral("--effects-preview-native"))) {
        int result = 0;
        try {
            result = nativeEffectsPreviewCapture();
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            result = 1;
        }
        return result;
    }
#endif
    // The offscreen plugin may have no system font database. These original
    // fixtures exercise real font resolution without requiring language packs.
    for (const auto* name : {"SnowRecordingTestSans-Regular.ttf", "SnowRecordingTestSans-Bold.ttf",
                             "SnowRecordingTestMono-Regular.ttf", "SnowRecordingTestMono-Bold.ttf",
                             "SnowRecordingTestHan-Regular.ttf", "SnowRecordingTestHan-Bold.ttf"}) {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/recording-test-fonts/") +
                                                  QString::fromLatin1(name)) >= 0,
                "load offscreen recording font");
    }
    QFont testFont(QStringLiteral("Snow Recording Test Sans"));
    testFont.setWeight(QFont::Normal);
    QApplication::setFont(testFont);
    QFontDatabase::setApplicationFallbackFontFamilies(QChar::Script_Han,
                                                      {QStringLiteral("Snow Recording Test Han")});
    if (app.arguments().contains(QStringLiteral("--area-history-only"))) {
        recordingAreaHistoryTests();
        return 0;
    }
    const QString preservationPrefix = QStringLiteral("--store-preservation-case=");
    for (const auto& argument : app.arguments()) {
        if (argument.startsWith(preservationPrefix)) {
            recordingSessionEndPreservationCase(argument.sliced(preservationPrefix.size()),
                                                temporary.path());
            // The terminal controller intentionally retains its Impl/UI owners. Each case owns
            // one process, matching the native end-session callback's termination semantics.
            std::_Exit(EXIT_SUCCESS);
        }
    }
    if (app.arguments().contains(QStringLiteral("--store-preservation-only"))) {
        recordingSessionEndPreservation();
        return 0;
    }
    const bool trimUiOnly = app.arguments().contains(QStringLiteral("--trim-ui-only"));
    if (app.arguments().contains(QStringLiteral("--export-naming-only"))) {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        recordingExportNamingTests(temporary.path());
        recordingWindowNameFallbackTests(temporary.path());
        return 0;
    }
    if (trimUiOnly || app.arguments().contains(QStringLiteral("--trim-only"))) {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        recordingTrimmingTests(temporary.path(), trimUiOnly);
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--finalization-close-only"))) {
        renderJobDestructionKeepsStateAvailableToModalObservers();
        destructionKeepsControllerAvailableToActivityObservers();
        recordingCanCloseDuringAndAfterFinalization();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--delete-on-close-only"))) {
        recordingDeleteOnCloseTests(temporary.path());
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--auto-exit-only"))) {
        recordingAutoExitAfterSuccessfulFinalization();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--export-notifications-only"))) {
        recordingAutoExitAfterSuccessfulFinalization();
        recordingAutoExitAfterSuccessfulFinalization(true);
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--audio-gain-only"))) {
        const auto waitFor = [](auto predicate) {
            QElapsedTimer deadline;
            deadline.start();
            while (!predicate() && deadline.elapsed() < 3000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            require(predicate(), "audio controller operation completes without blocking the GUI");
        };
        require(RecordingSettings().setMicrophoneEnabled(true) &&
                    RecordingSettings().setSystemAudioEnabled(true) &&
                    RecordingSettings().setMicrophoneGainDb(-6) &&
                    RecordingSettings().setSystemAudioGainDb(9),
                "recording audio fixture preferences save");
        {
            ScreenRecordingController controller(testEffectsSource);
            controller.open(QRect(10, 10, 320, 240));
            auto* controls = palette();
            auto* microphone = controls->recordingAudioGainPopover(true);
            auto* system = controls->recordingAudioGainPopover(false);
            // This fixture controls audio-device ownership. Native tool-window activation is
            // covered by the gain-popover tests; keep rapid source changes in one focus owner.
            for (auto* popup : {microphone, system}) {
                popup->popover()->setPopupLayerMode(
                    adqt::widgets::AdPopover::PopupLayerMode::InWindow);
            }
            require(microphone->gainDb() == -6 && system->gainDb() == 9,
                    "recording UI reloads independent gain preferences");
            microphone->openAndFocus();
            waitFor([] { return audioMonitorCreates == 1; });
            auto* slider =
                microphone->popover()->contentWidget()->findChild<adqt::widgets::AdSlider*>(
                    QStringLiteral("recordingAudioGainSlider"));
            require(slider != nullptr, "microphone gain slider exists");
            slider->setValue(12);
            require(RecordingSettings().microphoneGainDb() == 12 && system->gainDb() == 9,
                    "gain changes save independently before recording");
            holdAudioMonitorDestroy = true;
            system->openAndFocus();
            microphone->openAndFocus();
            QCoreApplication::processEvents();
            if (!microphone->popover()->isVisible() || system->popover()->isVisible() ||
                audioMonitorCreates != 1 || audioMonitorsActive != 1) {
                std::cerr << "rapid audio switch: microphone visible="
                          << microphone->popover()->isVisible()
                          << " system visible=" << system->popover()->isVisible()
                          << " created=" << audioMonitorCreates.load()
                          << " active=" << audioMonitorsActive.load() << '\n';
            }
            require(microphone->popover()->isVisible() && !system->popover()->isVisible() &&
                        audioMonitorCreates == 1 && audioMonitorsActive == 1,
                    "rapid source switching rejects a stale preview before retirement completes");
            holdAudioMonitorDestroy = false;
            waitFor([] { return audioMonitorCreates == 2; });
            microphone->trigger()->click();
            waitFor([] { return audioMonitorsActive == 0; });
            const int offMonitorCreates = audioMonitorCreates;
            slider->setValue(11);
            QCoreApplication::processEvents();
            require(audioMonitorCreates == offMonitorCreates &&
                        RecordingSettings().microphoneGainDb() == 11,
                    "off source gain editing persists without acquiring an audio device");
            microphone->trigger()->click();
            slider->setValue(12);
            waitFor([offMonitorCreates] { return audioMonitorCreates == offMonitorCreates + 1; });
            holdAudioMonitorDestroy = true;
            const int oldStarts = starts;
            controller.startRecording();
            QCoreApplication::processEvents();
            require(starts == oldStarts,
                    "recording initialization waits for audio preview teardown asynchronously");
            holdAudioMonitorDestroy = false;
            waitForRecording(controller);
            require(lastDirectConfig.system_audio_gain_db == 9 &&
                        lastDirectConfig.microphone_gain_db == 12 && audioMonitorsActive == 0,
                    "native configuration captures gains after preview retirement");
            microphone->openAndFocus();
            waitFor([] { return audioMeterMask == 2; });
            QKeyEvent escapePress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(slider, &escapePress);
            QKeyEvent escapeRelease(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(slider, &escapeRelease);
            require(!microphone->popover()->isVisible() && !controls->recordingBusy() &&
                        controller.isRecording(),
                    "Escape closes audio control without recording shortcut activation");
            microphone->openAndFocus();
            waitFor([] { return audioMeterMask == 2; });
            slider->setValue(-12);
            require(liveMicrophoneGain == -12 && liveSystemGain == 9 &&
                        RecordingSettings().microphoneGainDb() == -12,
                    "gain updates reach active source without changing the other source");
            controls->recordingPauseRequested();
            require(audioMeterMask == 2, "paused recording reuses selected source metering");
            slider->setValue(0);
            require(liveMicrophoneGain == 0, "paused gain changes reach recording control state");
            require(ApplicationStorage::instance().configuration().setValues(
                        {{QStringLiteral("screen_recording/microphone_gain_db"), -3},
                         {QStringLiteral("screen_recording/system_audio_gain_db"), 7}}),
                    "external gain preference updates succeed");
            waitFor([&] { return liveMicrophoneGain == -3 && liveSystemGain == 7; });
            require(microphone->gainDb() == -3 && system->gainDb() == 7,
                    "reset or import refreshes paused gains and independent controls");
            microphone->close();
            require(audioMeterMask == 0, "closing audio controls disables worker metering");
            const int oldReads = audioLevelReads;
            QElapsedTimer closed;
            closed.start();
            while (closed.elapsed() < 90) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            require(audioLevelReads == oldReads,
                    "closed gain controls have no meter polling timer");
            controls->recordingResumeRequested();
            controller.stopRecordingAndCopy();
            waitForIdle(controller);
            controls->recordingCloseRequested();
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto saved = ApplicationStorage::instance().configuration().snapshot();
        {
            ScreenRecordingController controller(testEffectsSource);
            QString error;
            require(!controller.startAutomation(QRect(10, 10, 320, 240),
                                                {{QStringLiteral("microphone_gain_db"), 25}},
                                                &error) &&
                        error == QStringLiteral("invalid_parameters"),
                    "automation rejects invalid gain before acquiring devices");
            require(controller.startAutomation(QRect(10, 10, 320, 240),
                                               {{QStringLiteral("microphone_gain_db"), -24},
                                                {QStringLiteral("system_audio_gain_db"), 24}},
                                               &error),
                    "automation accepts independent signed gain overrides");
            waitForRecording(controller);
            require(lastDirectConfig.microphone_gain_db == -24 &&
                        lastDirectConfig.system_audio_gain_db == 24 &&
                        ApplicationStorage::instance().configuration().snapshot() == saved,
                    "automation gain overrides are session-local");
            require(controller.automationState()
                            .value(QStringLiteral("options"))
                            .toObject()
                            .value(QStringLiteral("microphone_gain_db"))
                            .toInt() == -24,
                    "automation state exposes current gain");
            controller.stopRecordingAndCopy();
            waitForIdle(controller);
            controller.detachAutomation();
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return 0;
    }
    if (const QString previewFont =
            qEnvironmentVariable("SNOW_TEST_RECORDING_SETTINGS_PREVIEW_FONT");
        !previewFont.isEmpty()) {
        const int fontId = QFontDatabase::addApplicationFont(previewFont);
        require(fontId >= 0, "load recording settings preview font");
        QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).first()));
    }
    if (app.arguments().contains(QStringLiteral("--render-layout-only"))) {
        recordingRenderLayout();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--modal-stacking-only"))) {
        recordingModalStacking();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--post-processing-only"))) {
        recordingPostProcessingLifecycle();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--settings-dialog-only"))) {
        recordingSettingsDialog();
        recordingActionToolbarPreferenceUpdatesOpenWindow();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--color-sampling-only"))) {
        recordingColorSamplingIsConnected();
        recordingColorSamplerInteractions();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--color-sampling-native-only"))) {
        recordingColorSamplingIsConnected(true);
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--tool-toggle-only"))) {
        recordingActiveToolsReturnToSelect();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--magnifier-availability-only"))) {
        recordingMagnifierUnavailableWithoutImageSources();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--eraser-only"))) {
        recordingEraserTools();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--style-wheel-only"))) {
        ScreenRecordingController controller(testEffectsSource);
        controller.open(QRect(10, 10, 640, 480));
        QCoreApplication::processEvents();
        ScreenRecordingAreaWindow* area = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget))
                area = candidate;
        }
        require(area != nullptr, "recording area exists");
        auto* toolbar = palette();
        auto* canvas = area->canvas();
        for (const auto& tool : {QStringLiteral("text"), QStringLiteral("serial_number")}) {
            require(toolbar->activateDrawingShortcut(tool), "activate recording font tool");
            const bool text = tool == QStringLiteral("text");
            const auto before = canvas->canvasStyleToolbarState();
            const double size =
                text ? before.textStyle.fontSize : before.serialNumberStyle.fontSize;
            const QPointF point(100, 100);
            QWheelEvent event(point, canvas->mapToGlobal(point.toPoint()), QPoint(), QPoint(0, 120),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(canvas, &event);
            const auto saved = snow_shot::presentation::screenshotCanvasToolStyleDefaults();
            require((text ? saved.text.fontSize : saved.serialNumber.fontSize) == size + 1,
                    "recording canvas wheel applies and persists the font style");
        }
        toolbar->recordingCloseRequested();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--webcam-preview-only"))) {
        webcamPreviewRetainsFramesAcrossLayoutUpdates();
        recordingWebcamPreviewLifecycle();
        recordingWebcamGesturesPersistAndFreezeAtStartup();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--warnings-only"))) {
        recordingWarningsUseAreaWindow();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--webcam-settings-only"))) {
        recordingWebcamPreferencesPreserveSessionSnapshots();
        recordingWebcamAutomationOptionsStayIsolated();
        recordingWebcamSettingsSurviveInterruptedStartup();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--automation-only"))) {
        {
            ScreenRecordingController manual(testEffectsSource);
            const QRect region(10, 10, 320, 240);
            manual.open(region);
            const auto checkOption = [&](const QString& key, const QJsonValue& value,
                                         const std::function<void(const QJsonValue&)>& change) {
                const auto before = manual.automationState();
                const auto original = before.value(QStringLiteral("options")).toObject().value(key);
                require(original != value, "option fixture must change the value");
                change(value);
                const auto changed = manual.automationState();
                require(changed.value(QStringLiteral("revision")).toInteger() >
                                before.value(QStringLiteral("revision")).toInteger() &&
                            changed.value(QStringLiteral("options")).toObject().value(key) == value,
                        "manual options must be observable immediately without a timer tick");
                change(value);
                require(manual.automationState().value(QStringLiteral("revision")) ==
                            changed.value(QStringLiteral("revision")),
                        "repeated option values must not invalidate a revision");
                change(original);
                const auto restored = manual.automationState();
                require(restored.value(QStringLiteral("revision")).toInteger() >
                                changed.value(QStringLiteral("revision")).toInteger() &&
                            restored.value(QStringLiteral("options")).toObject().value(key) ==
                                original,
                        "change then restore must invalidate stale commands between timer ticks");
                require(manual.automationState() == restored,
                        "reading automation state must not mutate its revision");
            };
            const auto options =
                manual.automationState().value(QStringLiteral("options")).toObject();
            checkOption(QStringLiteral("microphone"),
                        !options.value(QStringLiteral("microphone")).toBool(),
                        [](const QJsonValue& value) {
                            palette()->recordingMicrophoneToggled(value.toBool());
                        });
            checkOption(QStringLiteral("system_audio"),
                        !options.value(QStringLiteral("system_audio")).toBool(),
                        [](const QJsonValue& value) {
                            palette()->recordingSystemAudioToggled(value.toBool());
                        });
            checkOption(QStringLiteral("start_delay_seconds"), 7, [](const QJsonValue& value) {
                palette()->recordingStartDelaySecondsChanged(value.toInt());
            });
            checkOption(QStringLiteral("keyboard_size"), 96, [](const QJsonValue& value) {
                palette()->recordingKeyboardSizeChanged(value.toInt());
            });
            checkOption(QStringLiteral("mouse_highlight_radius"), 72, [](const QJsonValue& value) {
                palette()->recordingMouseHighlightRadiusChanged(value.toInt());
            });
            checkOption(QStringLiteral("mouse_trail"), QStringLiteral("#ff123456"),
                        [](const QJsonValue& value) {
                            palette()->recordingMouseTrailColorChanged(QColor(value.toString()));
                        });
            const auto beforeRegion =
                manual.automationState().value(QStringLiteral("revision")).toInteger();
            manual.open(region.translated(20, 20));
            manual.open(region);
            require(manual.automationState().value(QStringLiteral("revision")).toInteger() >
                            beforeRegion &&
                        manual.automationState()
                                .value(QStringLiteral("options"))
                                .toObject()
                                .value(QStringLiteral("region")) == QJsonArray{10, 10, 320, 240},
                    "reopening an existing UI tracks region changes and restoration");
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto saved = ApplicationStorage::instance().configuration().snapshot();
        ScreenRecordingController controller(testEffectsSource);
        QString error;
        QTemporaryDir contractOutputDirectory;
        require(contractOutputDirectory.isValid(), "automation contract output must be isolated");
        QJsonObject supportedOptions;
        const auto& contracts = snow_shot::app::mcp::recordingAutomationOptionContracts();
        for (auto field = contracts.begin(); field != contracts.end(); ++field) {
            const QJsonObject contract = field->toObject();
            const QString type = contract.value(QStringLiteral("type")).toString();
            const QString format = contract.value(QStringLiteral("format")).toString();
            const auto rejects = [&](const QJsonValue& value) {
                const int previousStarts = starts.load();
                error.clear();
                require(
                    !controller.startAutomation(QRect(10, 10, 320, 240), {{field.key(), value}},
                                                &error) &&
                        error == (format == QStringLiteral("output-path")
                                      ? QStringLiteral("output_path_unavailable")
                                      : QStringLiteral("invalid_parameters")) &&
                        !controller.isOpen() && starts == previousStarts,
                    qPrintable(QStringLiteral("invalid %1 must reject before opening or capture")
                                   .arg(field.key())));
            };
            rejects(QJsonValue(QJsonValue::Null));
            if (type == QStringLiteral("boolean"))
                rejects(QStringLiteral("true"));
            else if (type == QStringLiteral("string"))
                rejects(true);
            else {
                rejects(8.5);
                rejects(QStringLiteral("24"));
            }
            const QJsonValue choices = contract.value(QStringLiteral("enum"));
            if (choices.isArray())
                rejects(type == QStringLiteral("integer") ? QJsonValue(0)
                                                          : QJsonValue(QStringLiteral("unknown")));
            const QJsonValue minimum = contract.value(QStringLiteral("minimum"));
            const QJsonValue maximum = contract.value(QStringLiteral("maximum"));
            if (minimum.isDouble())
                rejects(minimum.toDouble() - 1);
            if (maximum.isDouble())
                rejects(maximum.toDouble() + 1);
            if (format == QStringLiteral("qt-color"))
                rejects(QStringLiteral("invalid-recording-color"));
            if (format == QStringLiteral("output-path"))
                supportedOptions.insert(field.key(),
                                        contractOutputDirectory.filePath("output.gif"));
            else if (choices.isArray())
                supportedOptions.insert(field.key(), choices.toArray().first());
            else if (type == QStringLiteral("boolean"))
                supportedOptions.insert(field.key(), false);
            else if (type == QStringLiteral("integer"))
                supportedOptions.insert(field.key(), minimum);
            else
                supportedOptions.insert(field.key(), QStringLiteral("#ff123456"));
        }
        require(!controller.startAutomation(QRect(10, 10, 320, 240),
                                            {{QStringLiteral("unknown_option"), true}}, &error) &&
                    error == QStringLiteral("invalid_parameters") && !controller.isOpen(),
                "automation must reject unknown options before opening recording UI");
        require(!controller.startAutomation(QRect(10, 10, 320, 240),
                                            {{QStringLiteral("frame_rate"), 37}}, &error) &&
                    error == QStringLiteral("invalid_parameters") && !controller.isOpen(),
                "invalid automation options must reject before opening recording UI");
        require(!controller.startAutomation(QRect(10, 10, 320, 240),
                                            {{QStringLiteral("separate_audio_tracks"), 1}},
                                            &error) &&
                    error == QStringLiteral("invalid_parameters") && !controller.isOpen(),
                "separate audio override must be a boolean");
        require(!controller.startAutomation(QRect(10, 10, 320, 240),
                                            {{QStringLiteral("quality"), 101}}, &error) &&
                    error == QStringLiteral("invalid_parameters") && !controller.isOpen(),
                "out-of-range video quality must reject before opening recording UI");
        for (const auto value : {49.0, 501.0, 150.5}) {
            require(!controller.startAutomation(QRect(10, 10, 320, 240),
                                                {{QStringLiteral("webcam_size"), value}}, &error) &&
                        !controller.isOpen(),
                    "automation rejects invalid webcam sizes before opening recording UI");
        }
        require(!controller.startAutomation(
                    QRect(10, 10, 320, 240),
                    {{QStringLiteral("webcam_shape"), QStringLiteral("oval")}}, &error) &&
                    !controller.isOpen(),
                "automation rejects unsupported webcam shapes");
        require(!controller.startAutomation(QRect(10, 10, 320, 240),
                                            {{QStringLiteral("webcam"), 1}}, &error) &&
                    !controller.isOpen(),
                "automation webcam toggle must be boolean");
        for (const auto radius : {7.0, 129.0, 41.5}) {
            require(!controller.startAutomation(
                        QRect(10, 10, 320, 240),
                        {{QStringLiteral("mouse_highlight_radius"), radius}}, &error) &&
                        !controller.isOpen(),
                    "automation rejects invalid highlight radii before opening recording UI");
        }
        supportedOptions.insert(QStringLiteral("format"), QStringLiteral("gif"));
        supportedOptions.insert(QStringLiteral("start_delay_seconds"), 10);
        supportedOptions.insert(QStringLiteral("mouse_highlight_radius"), 72);
        supportedOptions.insert(QStringLiteral("animated_frame_rate"), 15);
        require(controller.startAutomation(QRect(10, 10, 320, 240), supportedOptions, &error),
                "automation must accept every declared option without changing preferences");
        require(controller.automationState().value(QStringLiteral("operation")).toString() ==
                    QStringLiteral("counting_down"),
                "automation state must expose countdown");
        require(controller.automationState()
                        .value(QStringLiteral("options"))
                        .toObject()
                        .value(QStringLiteral("mouse_highlight_radius")) == 72,
                "automation exposes the session-specific highlight radius");
        const int previousStarts = starts.load();
        controller.detachAutomation();
        QCoreApplication::processEvents();
        require(!controller.isOpen() && starts == previousStarts,
                "disconnect must cancel countdown without starting capture");
        require(ApplicationStorage::instance().configuration().snapshot() == saved,
                "automation options must not change persistent preferences");
        QTemporaryDir outputDirectory;
        require(outputDirectory.isValid(), "recording output directory must be isolated");
        const QString outputPath = outputDirectory.filePath(QStringLiteral("recording.mp4"));
        require(controller.startAutomation(QRect(10, 10, 320, 240),
                                           {{QStringLiteral("format"), QStringLiteral("mp4")},
                                            {QStringLiteral("path"), outputPath},
                                            {QStringLiteral("start_delay_seconds"), 0},
                                            {QStringLiteral("frame_rate"), 24},
                                            {QStringLiteral("quality"), 63},
                                            {QStringLiteral("encoder"), QStringLiteral("h265_hw")},
                                            {QStringLiteral("separate_audio_tracks"), true}},
                                           &error),
                "automation start must succeed");
        waitForRecording(controller);
        require(!QFileInfo::exists(outputPath),
                "the controller must leave output publication to the recording exporter");
        require(lastDirectConfig.codec == SNOW_CAPTURE_VIDEO_CODEC_H265 &&
                    lastDirectConfig.encoder_preference == SNOW_CAPTURE_ENCODER_PREFERENCE_HARDWARE,
                "automation HEVC hardware preference must reach capture without changing settings");
        require(lastDirectConfig.capture_fps == 24,
                "automation frame rate must reach the native capture configuration");
        require(lastDirectConfig.audio_mode == SNOW_CAPTURE_RECORDING_AUDIO_SEPARATE &&
                    ApplicationStorage::instance().configuration().snapshot() == saved,
                "separate audio override reaches capture without changing preferences");
        require(lastDirectConfig.quality == 63,
                "automation quality must reach the native capture configuration");
        const auto runningRevision =
            controller.automationState().value(QStringLiteral("revision")).toInteger();
        require(controller.controlAutomation(QStringLiteral("pause"), {}, &error),
                "automation pause must succeed");
        require(controller.automationState().value(QStringLiteral("state")).toString() ==
                    QStringLiteral("paused"),
                "automation must expose paused state");
        require(controller.controlAutomation(QStringLiteral("resume"), {}, &error),
                "automation resume must succeed");
        require(controller.automationState().value(QStringLiteral("revision")).toInteger() >
                    runningRevision,
                "pause then resume must invalidate a stale revision despite restoring state");
        const int previousExports = exports.load();
        controller.detachAutomation();
        waitForIdle(controller);
        require(exports == previousExports + 1 &&
                    controller.automationState().value(QStringLiteral("finalized")).toBool(),
                "disconnect must finalize exactly once and retain the output artifact");
        require(ApplicationStorage::instance().configuration().snapshot() == saved,
                "recording lifecycle must preserve persistent preferences");
        return 0;
    }
#ifdef Q_OS_MACOS
    if (app.arguments().contains(QStringLiteral("--effects-preview-dpi-only"))) {
        effectsPreviewPhysicalPixels();
        return 0;
    }
#endif
    if (app.arguments().contains(QStringLiteral("--playback-preview-only"))) {
        playbackPreviewAtZero();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--effects-preview-only"))) {
        class KeyTranslator : public QTranslator {
          public:
            bool isEmpty() const override {
                return false;
            }
            QString translate(const char* context, const char*, const char*, int) const override {
                return QByteArray(context) == "RecordingKeyboard" ? QStringLiteral("translated")
                                                                  : QString();
            }
        } translator;
        const RecordingKeyboardLabels original(true);
        for (const auto& entry : original.entries) {
            if (entry.key_code >= 0x200 && entry.key_code <= 0x204) {
                const char* expected[] = {"Left", "Right", "Middle", "Button 4", "Button 5"};
                require(QByteArray(reinterpret_cast<const char*>(entry.utf8), entry.utf8_len) ==
                            expected[entry.key_code - 0x200],
                        "mouse icon labels must use compact button names");
            }
        }
        require(app.installTranslator(&translator), "key translator must install");
        const RecordingKeyboardLabels localized(true);
        require(localized.text == original.text,
                "all key legends must ignore application language");
        require(localized.text.contains(QByteArray("Backspace")) &&
                    localized.text.contains(QByteArray("Num 0")),
                "key legends must use English names");
        for (const auto& entry : localized.entries) {
            require(entry.key_code != 0x5B && entry.key_code != 0x5C &&
                        (entry.key_code < 0xA0 || entry.key_code > 0xA5),
                    "physical modifier legends must be resolved from native side and AltGr state");
        }
        app.removeTranslator(&translator);
        recordingKeyboardFontFollowsApplication();
        playbackPreviewAtZero();
        effectsPreviewLifecycle();
        areaWindowPaintsInputSurfaceOnlyWhenItIsVisible();
        previewIgnoresEventsOtherThanDialogVisibility();
        recordingKeyboardColorsFollowBackground();
        recordingStatusPrompts();
        controllerPreviewTransitions();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--capture-exclusion-only"))) {
        recordingPopupExclusionTests();
        recordingCaptureExclusionWiring();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--toolbar-auto-hide-only"))) {
        runScreenRecordingToolbarAutoHideTests();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--toolbar-auto-hide-preview-only"))) {
        renderScreenRecordingToolbarAutoHidePreviews();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--api-mode-only"))) {
        recordingApiModeWiring();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--startup-retry-only"))) {
        recordingStartupFailureAllowsRetry();
        return 0;
    }
    recordingExpandsSmallSelectionsOnOpenAndReopen();
    recordingInputOwnerKeepsFocusAcrossPresentation();
    recordingToolbarReconcilesFrameBeforeShowing();
    recordingToolbarPlacementAcrossDisplays();
    if (app.arguments().contains(QStringLiteral("--placement-only"))) {
        return 0;
    }
    recordingSecondaryPanelsStayOnScreen();
    recordingMagnifierUnavailableWithoutImageSources();
    {
        ScreenRecordingController controller(testEffectsSource);
        const QRect region(40, 40, 320, 240);
        controller.open(region);
        ScreenRecordingAreaWindow* area = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget)) {
                area = candidate;
            }
        }
        require(area != nullptr, "recording area must exist");
        requireToolbarAboveArea(area);
        auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window());
        auto* picker = toolbar->palette()->findChild<adqt::widgets::AdColorPicker*>(
            QStringLiteral("screenRecordingMouseTrailColor"));
        require(picker != nullptr, "export settings must expose the trail color popup");
        picker->setPopupVisible(true);
        QCoreApplication::processEvents();
        require(picker->popupVisible(), "test popup must open");
        const QPoint toolbarPosition = toolbar->pos();
        area->regionInteractionStarted();
        require(!picker->popupVisible(), "geometry interaction must dismiss export popups");
        require(!toolbar->isVisible(), "native interaction must hide the toolbar");
        area->move(area->pos() + QPoint(20, 10));
        QCoreApplication::processEvents();
        require(!toolbar->isVisible() && toolbar->pos() == toolbarPosition,
                "intermediate geometry must not show or reposition the toolbar");
        area->regionInteractionFinished();
        require(toolbar->isVisible(), "finishing interaction must restore the toolbar");
        const QPoint finalPosition = toolbar->contentPosition();
        toolbar->placeForRecordingRegion(area->recordingRegion());
        require(toolbar->contentPosition() == finalPosition,
                "restored toolbar must use final geometry");
        controller.open(region);
        auto* canvas = area->canvas();
        require(palette()->activateDrawingShortcut(QStringLiteral("shape")),
                "drawing shortcut must activate the shape tool");
        require(canvas->setCanvasTool(SnowCanvasTool::Shape), "shape must activate");
        const auto mouse = [canvas](QEvent::Type type, QPointF position, Qt::MouseButton button,
                                    Qt::MouseButtons buttons) {
            QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(canvas, &event);
        };
        mouse(QEvent::MouseButtonPress, {30, 30}, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, {100, 80}, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, {100, 80}, Qt::LeftButton, Qt::NoButton);
        require(canvas->canvasHistoryState().canUndo, "drawing must create history");
        // Recording effects operate on the transparent annotation canvas.
        using Tool = ScreenshotToolPalette::Tool;
        int expectedHistoryEntries = 1;
        for (const auto& [tool, canvasTool] :
             {std::pair{Tool::RectangleHighlight, SnowCanvasTool::RectangleHighlight},
              std::pair{Tool::PenHighlight, SnowCanvasTool::PenHighlight},
              std::pair{Tool::RectangleFilter, SnowCanvasTool::RectangleFilter},
              std::pair{Tool::PenFilter, SnowCanvasTool::PenFilter},
              std::pair{Tool::AutoFilter, SnowCanvasTool::AutoFilter}}) {
            const snow_shot::storage::ScreenshotToolbarSettings settings;
            if (tool == Tool::RectangleHighlight || tool == Tool::PenHighlight) {
                settings.setLastHighlightTool(tool == Tool::RectangleHighlight
                                                  ? QStringLiteral("rectangle-highlight")
                                                  : QStringLiteral("pen-highlight"));
            } else {
                settings.setLastFilterTool(
                    tool == Tool::RectangleFilter ? QStringLiteral("rectangle-filter")
                    : tool == Tool::PenFilter     ? QStringLiteral("pen-filter")
                                                  : QStringLiteral("auto-filter"));
            }
            const QString shortcut = tool == Tool::RectangleHighlight || tool == Tool::PenHighlight
                                         ? QStringLiteral("highlight")
                                         : QStringLiteral("filter");
            auto* exportSettings = palette()->findChild<adqt::widgets::AdButton*>(
                QStringLiteral("screenRecordingExportSettings"));
            require(exportSettings != nullptr, "recording export settings button must exist");
            exportSettings->click();
            require(palette()->activateDrawingShortcut(shortcut),
                    "recording effect shortcut must activate");
            require(canvas->canvasTool() == canvasTool &&
                        area->inputMode() == ScreenRecordingAreaWindow::InputMode::Drawing &&
                        !palette()->recordingExportSettingsVisible(),
                    "recording effect must select its canvas tool and enable drawing");
            if (tool != Tool::AutoFilter) {
                mouse(QEvent::MouseButtonPress, {35, 35}, Qt::LeftButton, Qt::LeftButton);
                mouse(QEvent::MouseMove, {90, 70}, Qt::NoButton, Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, {90, 70}, Qt::LeftButton, Qt::NoButton);
                ++expectedHistoryEntries;
                int historyEntries = 0;
                while (canvas->canvasHistoryState().canUndo) {
                    require(canvas->undo(), "recording effects must remain undoable canvas edits");
                    ++historyEntries;
                }
                require(historyEntries == expectedHistoryEntries,
                        "each recording effect gesture must add one canvas history entry");
                while (historyEntries-- > 0) {
                    require(canvas->redo(), "recording effects must remain redoable canvas edits");
                }
            }
        }
        controller.open(region);
        require(canvas->canvasHistoryState().canUndo,
                "opening an already visible region must preserve its drawing");
        QPointer<ScreenRecordingAreaWindow> closedArea(area);
        QPointer<ScreenRecordingToolbarWindow> closedToolbar(
            qobject_cast<ScreenRecordingToolbarWindow*>(palette()->window()));
        palette()->recordingCloseRequested();
        require(!controller.isOpen(), "Close must detach the session immediately");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(closedArea.isNull() && closedToolbar.isNull(), "Close must destroy both windows");
        controller.open(region);
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget)) {
                area = candidate;
            }
        }
        canvas = area->canvas();
        require(area->isVisible(), "reopening must create a visible fresh session");
        requireToolbarAboveArea(area);
        require(!canvas->canvasHistoryState().canUndo && !canvas->canvasHistoryState().canRedo,
                "a new session at the same rectangle must clear drawing and history");
        require(canvas->canvasTool() == SnowCanvasTool::Select &&
                    area->inputMode() != ScreenRecordingAreaWindow::InputMode::Drawing &&
                    !palette()->activeTool().has_value(),
                "a new session must reset transient drawing tools");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        ScreenRecordingController controller(testEffectsSource);
        controller.startRecording();
        controller.open(QRect(40, 40, 320, 240));
        QCoreApplication::processEvents();
        require(starts == 0, "a start requested while closed must not affect a later window");
        controller.startRecording();
        palette()->recordingCloseRequested();
        controller.open(QRect(80, 80, 320, 240));
        QCoreApplication::processEvents();
        require(starts == 0 && !controller.isRecording(),
                "closing a queued start must not start a later recording window");
        for (int iteration = 0; iteration < 8; ++iteration) {
            controller.startRecording();
            palette()->recordingCloseRequested();
            controller.open(QRect(80, 80, 320, 240));
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        int toolbarCount = 0;
        for (auto* widget : QApplication::topLevelWidgets()) {
            toolbarCount += qobject_cast<ScreenRecordingToolbarWindow*>(widget) != nullptr;
        }
        require(toolbarCount == 1, "reopening must leave only the current toolbar alive");
        controller.startRecording();
        controller.open(QRect(120, 80, 320, 240));
        QCoreApplication::processEvents();
        require(starts == 0, "replacing the selected region must invalidate pending starts");
        controller.startRecording();
        palette()->recordingCloseRequested();
        controller.open(QRect(80, 80, 320, 240));
        palette()->recordingKeyboardSizeChanged(96);
        palette()->recordingMouseTrailDurationMsChanged(2000);
        palette()->recordingKeyboardBackgroundColorChanged(QColor(40, 80, 120, 128));
        palette()->recordingKeyboardForegroundColorChanged(QColor(240, 230, 220, 200));
        controller.startRecording();
        controller.startRecording();
        waitForRecording(controller);
        require(starts == 1 && controller.isRecording(), "a fresh request must start exactly once");
        const RecordingKeyboardFont expectedFont;
        require(lastKeyboardFontFamily == expectedFont.family &&
                    lastKeyboardCjkFontFamily == expectedFont.cjkFamily &&
                    lastDirectConfig.keyboard_font_weight == expectedFont.weight,
                "saved recordings must receive the same application font as the preview");
        require(lastDirectConfig.audio_mode == SNOW_CAPTURE_RECORDING_AUDIO_MIXED,
                "recordings default to mixed audio");
        require(snow_shot::storage::RecordingSettings().setSeparateAudioTracks(true),
                "enable separate tracks for subsequent recordings");
        require(lastDirectConfig.audio_mode == SNOW_CAPTURE_RECORDING_AUDIO_MIXED,
                "active recording retains its audio mode snapshot");
        require(lastDirectConfig.loop_animated_images == 1, "recordings must default to looping");
        require(snow_shot::storage::RecordingSettings().setLoopAnimatedImages(false),
                "disable looping for subsequent recordings");
        require(lastDirectConfig.loop_animated_images == 1,
                "active recording must retain its loop preference snapshot");
        require(lastDirectConfig.mouse_trail_duration_ms == 2000 &&
                    lastDirectConfig.keyboard_background_rgba == 0x28507880 &&
                    lastDirectConfig.keyboard_text_rgba == 0xf0e6dcc8 &&
                    lastDirectConfig.keyboard_border_rgba == 0x5e7c9a80,
                "recording must snapshot the same duration and keyboard style as preview");
        palette()->recordingPauseRequested();
        require(controller.isRecording(),
                "paused recording must remain stoppable by the global toggle");
        controller.stopRecordingAndCopy();
        controller.stopRecordingAndCopy();
        controller.startRecording();
        // Do not pump the UI export-completion timer: this test must not alter
        // the desktop clipboard. Destruction joins the fake export worker.
        QElapsedTimer elapsed;
        elapsed.start();
        while (exports.load() == 0 && elapsed.elapsed() < 2000) {
            QThread::msleep(1);
        }
        require(exports.load() == 1 && starts == 1,
                "repeated shortcut calls during export must not duplicate stop or restart");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        ScreenRecordingController controller(testEffectsSource);
        controller.open(QRect(40, 40, 320, 240));
        controller.startRecording();
    }
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(starts == 1, "destroying the controller must cancel a queued recording start");
#ifdef Q_OS_MACOS
    standardCloseFromRecordingArea();
#endif
    closeAndStopHaveIndependentUiLifetimes();
    require(snow_shot::storage::RecordingSettings().setLoopAnimatedImages(true),
            "restore recording loop preference");
#ifdef Q_OS_MACOS
    destructionDoesNotBlockNativeWorkers();
    staleRetinaSizingCannotConfigureAnotherRegion();
#endif
    require(snow_shot::storage::RecordingSettings().setSeparateAudioTracks(false),
            "restore mixed recording audio");
    permissionsAndExactLogicalRegion();
    stopAndCopyBusyIndicatorsStayOnTheInitiatingControl();
    renderJobDestructionKeepsStateAvailableToModalObservers();
    destructionKeepsControllerAvailableToActivityObservers();
    recordingCanCloseDuringAndAfterFinalization();
    delayCountdownBlocksTheStartUntilItElapses();
    return 0;
}

int main(int argc, char** argv) {
#ifdef SNOW_RECORDING_EFFECTS_BENCHMARK
    RecordingEffectsBenchmarkApplication app(argc, argv);
#else
    QApplication app(argc, argv);
#endif
    try {
        return runRecordingControllerTests(app);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

extern "C" int32_t snow_recording_region_output_dimensions(int32_t, int32_t, uint32_t width,
                                                           uint32_t height, uint32_t maximumWidth,
                                                           uint32_t maximumHeight, uint32_t format,
                                                           uint32_t* outputWidth,
                                                           uint32_t* outputHeight) {
    const auto scale = dimensionsScale.load();
    if (holdDimensions.exchange(false)) {
        dimensionsEntered = true;
        dimensionsGate.wait();
    }
    ++dimensionsCompleted;
    return snow_recording_output_dimensions(width * scale, height * scale, maximumWidth,
                                            maximumHeight, format, outputWidth, outputHeight);
}

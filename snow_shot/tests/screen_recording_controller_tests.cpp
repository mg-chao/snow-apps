#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "eraser_toolbar_test_support.h"
#include "window_close_shortcut_test_support.h"
#include "../src/presentation/recording/recordingrenderjob.h"
#include <QFontDatabase>
#include "recording_effect_test_source.h"
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
#include <QPainter>
#include <QLineF>
#include <QTranslator>
#include "snow_shot/presentation/screenrecordingcontroller.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_capture.h"
#include "snow_shot/platform/windowcaptureexclusion.h"
#include "snow_recording.h"
#include "widgets/button.h"
#include "../src/presentation/recording/recordingaudiogainpopover.h"
#include "widgets/popover.h"
#include "widgets/dpi_stable_window_controller.h"

#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QClipboard>
#include <QMimeData>
#include <QtMath>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWindow>
#include <QScreen>
#include <QPointer>
#include <QTimer>
#include <QJsonArray>
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
#include <future>
#ifdef Q_OS_MACOS
#include "macos_capture_exclusion_probe.h"
#include "macos_recording_modal_probe.h"
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
int recordingToolbarAcrossNativeDisplays(bool startCapture);
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

struct SnowRecordingClipImpl {
    quint64 revision = 0;
};
struct SnowRecordingClipFrameImpl {};
struct SnowRecordingClipExportImpl {};
struct SnowRecordingSessionImpl {};
struct SnowRecordingAudioMonitorImpl {
    uint32_t source = 0;
    int gain = 0;
};
struct SnowRecordingSourceImpl {};
struct SnowRecordingRenderTaskImpl {};
namespace {
std::vector<std::weak_ptr<RecordingEffectTestState>> effectSources;
std::unique_ptr<RecordingEffectsSource> testEffectsSource() {
    auto state = std::make_shared<RecordingEffectTestState>();
    effectSources.push_back(state);
    return std::make_unique<RecordingEffectTestSource>(std::move(state));
}

SnowRecordingSession session;
std::atomic<int> clipOpens = 0, clipDestroys = 0, clipExports = 0;
std::atomic<uint32_t> clipExportState = SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
std::atomic<int> clipAutoplays = 0;
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
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
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

void recordingCanCloseDuringAndAfterFinalization() {
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
            require(controller.isOpen() != close, "only Close must detach the UI during export");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            require(previousToolbar.isNull() == close && recordingWindowCount() == (close ? 0 : 2),
                    "Close must destroy UI before backend finalization, while Stop retains it");
            controller.open({80, 80, 320, 240});
            require(recordingWindowCount() == (close ? 0 : 2),
                    "busy finalization must reject reopen");
            release.set_value();
            waitForIdle(controller);
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
class PreviewTestTranslator final : public QTranslator {
  public:
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QByteArray(context) == "RecordingEffectPreview" &&
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
        const int backgroundAlpha =
            mode == ScreenRecordingAreaWindow::InputMode::PassThrough ? 0 : 2;
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
    narrow.setText(complete);
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
    palette()->recordingRecordMouseClicksChanged(true);
    palette()->recordingKeyboardVisibleChanged(false);
    palette()->recordingCursorVisibleChanged(true);
    pumpPreview();
    require(state->highlight == 0xffff0080 && state->recordMouseClicks && !state->showKeyboard,
            "highlight and click-only recording must reach preview independently");
    palette()->recordingCursorVisibleChanged(false);
    pumpPreview();
    require(
        state->highlight == 0 && state->recordMouseClicks &&
            RecordingSettings().mouseHighlightEnabled(),
        "hiding cursor suppresses highlight without forgetting preference or hiding click keycaps");
    palette()->recordingMouseHighlightEnabledChanged(false);
    palette()->recordingRecordMouseClicksChanged(false);
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
                previewImage(*area->canvas()).pixelColor(100, 100).alpha() == 2,
            "idle export settings must keep the recording area clickable through preview clears");
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
    require(previewImage(*area->canvas()).pixelColor(100, 100).alpha() == 2,
            "returning to idle export settings must restore hit-test coverage");
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
    const snow_shot::storage::RecordingSettings settings;
    if (settings.outputFormat() != QStringLiteral("mp4"))
        return expected;
    for (bool microphone : {false, true}) {
        if (!(microphone ? settings.microphoneEnabled() : settings.systemAudioEnabled()))
            continue;
        auto* popup = toolbarPalette->recordingAudioGainPopover(microphone);
        popup->setRetainNativeSurfaceOnHide(true);
        if (auto id = snow_shot::platform::captureWindowId(popup->prepareSurface()))
            expected.push_back(*id);
    }
    return expected;
}

void recordingCaptureExclusionWiring() {
    using snow_shot::storage::RecordingSettings;
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
            QWidget* toolbar = palette()->window();
            const auto expected = prepareExpectedRecordingExclusions(palette(), captureToolbar);
#ifdef Q_OS_MACOS
            const auto sharingMatches = macosCaptureSharingProbe(toolbar);
#endif
            failStart = failure == 1;
            failStartOperation = failure == 2;
            controller.startRecording();
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
            require(lastExcludedWindows == expected,
                    "recording creation owns the toolbar and prepared audio surface exclusions");
            require(toolbar->isVisible(), "toolbar remains visible on success and failure");
#ifdef Q_OS_MACOS
            require(sharingMatches(!captureToolbar && !fail),
                    "recording start applies sharing policy and failure restores it");
#endif
            if (!fail) {
                palette()->recordingStopRequested();
                waitForIdle(controller);
            }
#ifdef Q_OS_MACOS
            require(sharingMatches(false), "recording stop restores the original sharing policy");
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
    {
        ScreenRecordingController controller(testEffectsSource);
        controller.open({40, 40, 320, 240});
        sharingMatches = macosCaptureSharingProbe(palette()->window());
        controller.startRecording();
        waitForRecording(controller);
        require(sharingMatches(true), "active toolbar is excluded before destruction");
    }
    require(sharingMatches(false), "controller destruction restores native sharing");
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
    bool granted = false;
    int requests = 0;
    controller.setPermissionCheck([&](bool microphone, bool input, bool notify) {
        require(microphone == (settings.outputFormat() == QStringLiteral("mp4") &&
                               settings.microphoneEnabled()),
                "start permission check must reflect the current microphone and output settings");
        require(input == (settings.showKeyboard() || settings.recordMouseClicks() ||
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
    granted = true;
    controller.startRecording();
    waitForRecording(controller);
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
            checkNative((targetWindow == areaHandle) ==
                            (mode != ScreenRecordingAreaWindow::InputMode::PassThrough),
                        "empty preview pixels must receive native clicks while editing or drawing");
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

extern "C" {
SnowRecordingResult
snow_recording_session_create_direct(const SnowCaptureDirectRecordingConfig* config,
                                     SnowRecordingSession** result) {
    // Session creation runs on the controller's worker thread: only plain data
    // may be touched here. The preview label invariant is asserted on the GUI
    // thread by controllerPreviewTransitions instead.
    require(audioMonitorsActive == 0,
            "recording native initialization follows audio preview retirement");
    lastDirectConfig = *config;
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
    *result = &session;
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
void snow_recording_session_destroy(SnowRecordingSession*) {
    ++destroyedSessions;
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
    return new SnowRecordingClip;
}
void snow_recording_clip_destroy(SnowRecordingClip* clip) {
    delete clip;
    ++clipDestroys;
}
uint8_t snow_recording_clip_info(const SnowRecordingClip*, SnowRecordingClipInfo* info) {
    *info = {32, 24, 1000000, 30};
    return 1;
}
uint64_t snow_recording_clip_boundary(const SnowRecordingClip*, uint64_t frame) {
    return frame * 1000000 / 30;
}
uint64_t snow_recording_clip_seek(SnowRecordingClip* clip, uint64_t frame, uint64_t end,
                                  uint8_t play) {
    clipFirst = frame;
    clipEnd = end;
    clipAutoplays += play != 0;
    return ++clip->revision;
}
SnowRecordingClipFrame* snow_recording_clip_acquire(const SnowRecordingClip*,
                                                    SnowRecordingClipPreview*) {
    return nullptr;
}
void snow_recording_clip_frame_destroy(SnowRecordingClipFrame* frame) {
    delete frame;
}
size_t snow_recording_clip_error(const SnowRecordingClip*, char*, size_t) {
    return 0;
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
uint32_t snow_recording_clip_export_poll(const SnowRecordingClipExport*, float* percent) {
    *percent = clipExportState == SNOW_RECORDING_RENDER_STATE_RUNNING ? 41.75f : 100;
    return clipExportState;
}
size_t snow_recording_clip_export_error(const SnowRecordingClipExport*, char*, size_t) {
    return 0;
}
void snow_recording_clip_export_cancel(SnowRecordingClipExport*) {
    clipExportState = SNOW_RECORDING_RENDER_STATE_CANCELED;
}
void snow_recording_clip_export_destroy(SnowRecordingClipExport* task) {
    delete task;
}
SnowRecordingResult snow_recording_session_stop(SnowRecordingSession*) {
    require(recordingStopRequested,
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
    ++exports;
    return failure ? SNOW_RECORDING_RESULT_INVALID_ARGUMENT : SNOW_RECORDING_RESULT_OK;
}
SnowRecordingResult snow_recording_session_request_stop(SnowRecordingSession*) {
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
        if (descriptor.reset != settings::SettingsSectionReset::ScreenRecording &&
            descriptor.reset != settings::SettingsSectionReset::ScreenRecordingCapture)
            continue;
        ++expectedCount;
        require(form->field(descriptor.id) != nullptr,
                "every feature and system recording preference must appear in the form");
        require(form->field(descriptor.id)->label() == descriptor.definition->title.translated(),
                "recording preferences must share their labels with the main settings page");
        require(form->field(descriptor.id)->extraText().isEmpty(),
                "recording preferences must keep descriptions out of the compact form rows");
    }
    require(form->items().size() == expectedCount && expectedCount == 10,
            "recording popup must contain exactly the requested settings categories");
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
                form->field(QStringLiteral("screen-recording.frame-rate"))->label() ==
                    QStringLiteral("Translated: Frame rate") &&
                form->findChild<AdSelect*>(QStringLiteral("screen-recording.encoder"))
                        ->options()
                        .first()
                        .label == QStringLiteral("Translated: H.264 (Hardware)"),
            "open recording preferences must retranslate their title, labels and options");
    QCoreApplication::removeTranslator(&translator);
    QCoreApplication::processEvents();
    if (const QString preview = qEnvironmentVariable("SNOW_TEST_RECORDING_SETTINGS_PREVIEW");
        !preview.isEmpty()) {
        require(modal->contentWidget()->window()->grab().save(preview),
                "save recording settings popup preview");
    }
    const auto items = form->items();
    for (int index = 0; index < items.size(); index += 2) {
        const auto left = items[index]->geometry();
        const auto right = items[index + 1]->geometry();
        require(left.top() == right.top() && left.right() < right.left() &&
                    qAbs(left.width() - right.width()) <= 1,
                "recording preferences must use two equally sized columns in each row");
        require(form->rect().contains(left) && form->rect().contains(right) &&
                    (index == 0 || items[index - 2]->geometry().bottom() < left.top()),
                "all recording preferences must fit in five rows without scrolling");
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
    const auto select = [form](const char* id, const QVariant& value) {
        auto* control = form->findChild<AdSelect*>(QString::fromLatin1(id));
        require(control != nullptr, "recording select must exist");
        control->setCurrentValue(value);
    };
    select("screen-recording.clarity", QStringLiteral("4k"));
    select("screen-recording.frame-rate", 60);
    select("screen-recording.animated-image-clarity", QStringLiteral("1080p"));
    select("screen-recording.animated-image-frame-rate", 15);
    select("screen-recording.encoder", QStringLiteral("h265"));
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
    const snow_shot::storage::RecordingSettings saved;
    require(saved.screenRecordingClarity() == QStringLiteral("4k") && saved.frameRate() == 60 &&
                saved.animatedImageClarity() == QStringLiteral("1080p") &&
                saved.animatedImageFrameRate() == 15 && saved.encoder() == QStringLiteral("h265") &&
                saved.encodingPreset() == QStringLiteral("medium") && saved.videoQuality() == 65 &&
                !saved.loopAnimatedImages() && saved.separateAudioTracks() &&
                !saved.captureToolbarInRecording(),
            "every recording popup editor must persist through the shared settings backend");
    require(saved.setFrameRate(30), "external settings write must succeed");
    QCoreApplication::processEvents();
    require(form->findChild<AdSelect*>(QStringLiteral("screen-recording.frame-rate"))
                    ->currentValue()
                    .toInt() == 30,
            "an open recording form must follow settings changed elsewhere");
    QPointer<AdModal> retired(modal);
    modal->accept();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(retired.isNull(), "closing recording settings must release its form and popup windows");
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
    controller.startRecording();
    waitForRecording(controller);
    require(!button->isEnabled() && (retired.isNull() || !retired->isOpen()),
            "recording must disable and dismiss the preferences window");
    require(lastDirectConfig.capture_fps == 30 && lastDirectConfig.maximum_width == 3840 &&
                lastDirectConfig.maximum_height == 2160 &&
                lastDirectConfig.codec == SNOW_CAPTURE_VIDEO_CODEC_H265 &&
                lastDirectConfig.preset == SNOW_CAPTURE_VIDEO_ENCODING_PRESET_MEDIUM &&
                lastDirectConfig.quality == 65 && lastDirectConfig.loop_animated_images == 0 &&
                lastDirectConfig.audio_mode == SNOW_CAPTURE_RECORDING_AUDIO_SEPARATE &&
                lastExcludedWindows == expectedExclusions,
            "the next recording must use preferences saved in the popup");
    toolbarPalette->recordingCloseRequested();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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
        controls->recordingPostProcessingEnabledChanged(true);
        controls->recordingPostProcessingEffectChanged(QStringLiteral("playback_time"));
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
        auto controller = std::make_unique<ScreenRecordingController>(testEffectsSource);
        QString error;
        require(controller->startAutomation(QRect(80, 80, 320, 240),
                                            {{QStringLiteral("post_processing"), true}}, &error),
                "prepare a deferred source finalization barrier");
        waitForRecording(*controller);
        std::promise<void> releaseExport;
        std::promise<void> enteredPromise;
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
        QElapsedTimer destruction;
        destruction.start();
        controller.reset();
        require(destruction.elapsed() < 500 && destroyedSessions == oldSessions,
                "controller destruction never joins or releases a pending source finalization");
        releaseExport.set_value();
        exportEntered = nullptr;
        exportGate = {};
        wait([&] { return sourceDestroys > oldSources && destroyedSessions > oldSessions; },
             "retired finalization eventually releases the session and preserves source media");
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
    for (const int mode : {0, 1, 2, 3}) {
        auto releaseCancel = std::make_shared<std::promise<void>>();
        renderCancelGate = releaseCancel->get_future().share();
        renderCancelEntered = false;
        renderStartEntered = false;
        const int oldTasks = taskDestroys.load();
        const int oldSources = sourceDestroys.load();
        const int oldDiscards = sourceDiscards.load();
        // A watchdog bounds the regression's synchronous-cancel failure without
        // using a GUI timer, which cannot run while that failure blocks the GUI.
        std::promise<void> watchdogDone;
        auto watchdog = std::async(
            std::launch::async, [releaseCancel, done = watchdogDone.get_future()]() mutable {
                if (done.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
                    releaseCancel->set_value();
            });
        auto* job = new RecordingRenderJob(new SnowRecordingSource, false, nullptr, nullptr);
        int published = 0;
        job->finished = [&published](RecordingRenderJob::Outcome outcome) {
            published += outcome == RecordingRenderJob::Outcome::Succeeded;
        };
        failRenderPoll = mode == 2;
        QElapsedTimer responsiveness;
        job->start();
        if (mode != 2) {
            wait([] { return renderStartEntered.load(); }, "blocked-cancel render must start");
            wait([&] { return job->state().value(QStringLiteral("render_duration_ms")) == 2300; },
                 "the GUI must adopt the completed start before testing native cancellation");
            renderPercent = 41;
            wait([&] { return job->state().value(QStringLiteral("render_progress")) == 41; },
                 "the adopted task must report its updated rendering progress");
        }
        responsiveness.start();
        if (mode != 2)
            require(job->cancel() && responsiveness.elapsed() < 500,
                    "Cancel acceptance must never wait on native publication synchronization");
        wait([] { return renderCancelEntered.load(); },
             "native cancellation must reach its barrier");
        bool heartbeat = false;
        QTimer::singleShot(0, [&heartbeat] { heartbeat = true; });
        QCoreApplication::processEvents();
        require(heartbeat && responsiveness.elapsed() < 500 && taskDestroys == oldTasks,
                "blocked cancellation preserves responsive GUI events and a live task lease");
        if (mode == 1) {
            responsiveness.restart();
            delete job;
            job = nullptr;
            require(
                responsiveness.elapsed() < 500 && sourceDestroys == oldSources,
                "destruction transfers pending cancellation without joining or releasing source");
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
        watchdogDone.set_value();
        watchdog.get();
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
        QElapsedTimer destruction;
        destruction.start();
        delete job;
        require(destruction.elapsed() < 500,
                "destroying the UI must never join a pending render start");
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

void recordingTrimmingTests(const QString& directory) {
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
                for (const char* name : {"screenRecordingReplay", "screenRecordingTrimStart",
                                         "screenRecordingTrimEnd"}) {
                    auto* control = panel->findChild<QWidget*>(QString::fromLatin1(name));
                    require(control != nullptr, "trim input target exists");
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
    require(range.height() == 40 && range.firstFrame() == 0 && range.endFrame() == 30,
            "trim row starts at Select height with the entire clip selected");
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
    QMouseEvent click(QEvent::MouseButtonPress, QPointF(200, 20), QPointF(200, 20), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
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
    preview = -1;
    QApplication::sendEvent(&range, &click);
    require(preview == -1, "disabled timeline cannot request a seek");
    range.setEnabled(true);
    end->setValue(8);
    require(!first->geometry().intersects(end->geometry()),
            "one-frame selections keep both handles independently reachable");
    // Render the actual widgets for repeatable light/dark and scale inspection.
    const QString renders = qEnvironmentVariable("SNOW_TRIM_VISUAL_OUTPUT");
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
            for (auto* handle : {first, end}) {
                require(handle->size() == QSize(qRound(16 * scale), qRound(32 * scale)),
                        "trim handles provide enlarged scaled drag targets");
                require(range.rect().contains(handle->geometry()) &&
                            !replay->geometry().intersects(handle->geometry()),
                        "trim targets stay inside the row and clear of Replay");
            }
            first->setValue(6);
            end->setValue(24);
            range.setPosition(500000);
            range.move(48, 48);
            visualHost.resize(range.size() + QSize(96, 96));
            visualHost.show();
            range.show();
            QCoreApplication::processEvents();
            const QImage progress = previewImage(range);
            range.setPosition(600000);
            const QImage advanced = previewImage(range);
            const int railTop = qRound(range.height() / 2.0 - 2 * scale);
            const int railBottom = qRound(range.height() / 2.0 + 2 * scale);
            for (int y = 0; y < progress.height(); ++y) {
                if (y >= railTop - 1 && y <= railBottom)
                    continue;
                for (int x = qRound(150 * scale); x < qRound(250 * scale); ++x)
                    require(progress.pixelColor(x, y) == advanced.pixelColor(x, y),
                            "playback progress must not draw a marker outside the rail");
            }
            const qreal trackWidth = range.width() - 96 * scale;
            const qreal firstX = 68 * scale + trackWidth * 0.2;
            const qreal endX = 68 * scale + trackWidth * 0.8;
            for (int y : {railTop, railBottom - 1}) {
                require(progress.pixelColor(qCeil(firstX), y) ==
                            progress.pixelColor(qRound(150 * scale), y),
                        "progress rail meets the start handle with square corners");
                require(progress.pixelColor(qFloor(endX) - 1, y) ==
                            progress.pixelColor(qRound(250 * scale), y),
                        "selected rail meets the end handle with square corners");
            }
            range.setPosition(500000);
            if (!renders.isEmpty()) {
                QDir().mkpath(renders);
                require(
                    previewImage(visualHost)
                        .save(renders + QStringLiteral("/trim-%1-%2.png")
                                            .arg(theme == ThemeMode::Dark ? QStringLiteral("dark")
                                                                          : QStringLiteral("light"))
                                            .arg(qRound(scale * 100))),
                    "save trim visual fixture");
                if (scale == 2.0) {
                    const auto saveState = [&](const QString& state) {
                        require(previewImage(visualHost)
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
    require(first->accessibleName() == QStringLiteral("Translated Trim start") &&
                range.findChild<adqt::widgets::AdButton*>()->toolTip() ==
                    QStringLiteral("Translated Replay"),
            "trim accessibility and Replay retranslate live");
    require(first->toolTip().startsWith(QStringLiteral("Translated Trim start\n")),
            "timecode tooltips retranslate alongside the trim handle names");
    QCoreApplication::removeTranslator(&translator);
    for (bool deferred : {false, true}) {
        for (bool paused : {false, true}) {
            ScreenRecordingController controller(testEffectsSource);
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
                    require(previewImage(*palette()->window())
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
                require(originalFile.open(QIODevice::ReadOnly) &&
                            originalFile.readAll() == QByteArray("original fixture"),
                        "Save never replaces the original source");
            }
            start->setValue(7);
            clipExportState = SNOW_RECORDING_RENDER_STATE_FAILED;
            copy->click();
            wait([&] { return copy->isEnabled() && clipExports == oldExports + 2; });
            require(start->value() == 7 && finish->value() == 20 && controller.isOpen(),
                    "export failure preserves the source session and selected range");
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
        }
    }
    for (bool deferred : {false, true}) {
        for (bool restart : {false, true}) {
            ScreenRecordingController controller(testEffectsSource);
            QString error;
            require(controller.startAutomation(QRect(80, 80, 320, 240),
                                               {{QStringLiteral("post_processing"), deferred}},
                                               &error),
                    "start crop exit fixture");
            waitForRecording(controller);
            auto* trim = recordingToolbarButton("Trim Video");
            trim->click();
            wait([&] { return trim->isEnabled(); });
            auto* start = recordingToolbarButton("Start recording");
            QPointer<QSlider> timeline =
                palette()->findChild<QSlider*>(QStringLiteral("screenRecordingTrimStart"));
            require(start && start->isEnabled(), "crop editor allows starting a new recording");
            if (restart) {
                start->click();
                waitForRecording(controller);
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
        }
    }
    for (bool deferred : {false, true}) {
        ScreenRecordingController controller(testEffectsSource);
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
        release.set_value();
        exportEntered = nullptr;
        exportGate = {};
        wait([&] { return destroyedSessions > beforeSessions; });
        require(renderStarts == beforeRenders && clipOpens == beforeOpens &&
                    sourceDiscards == beforeDiscards,
                "closing preparation retains the source without rendering effects or starting "
                "playback");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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

int main(int argc, char** argv) {
#ifdef SNOW_RECORDING_EFFECTS_BENCHMARK
    RecordingEffectsBenchmarkApplication app(argc, argv);
#else
    QApplication app(argc, argv);

#endif
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary storage must exist");
    using namespace snow_shot::storage;
    require(ApplicationStorage::instance()
                .initialize({temporary.filePath("bin"), temporary.filePath("settings"), 0})
                .success,
            "isolated storage must initialize");
    require(RecordingSettings().setVideoSaveDirectory(temporary.path()),
            "test output directory must be set");
    require(RecordingSettings().setCaptureToolbarInRecording(true),
            "capture exclusion must be disabled for fake backend");
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--native-toolbar-display-only")) ||
        app.arguments().contains(QStringLiteral("--native-toolbar-ready-display-only"))) {
        const int result = recordingToolbarAcrossNativeDisplays(
            app.arguments().contains(QStringLiteral("--native-toolbar-display-only")));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ApplicationStorage::instance().shutdown();
        return result;
    }
#endif
#ifdef SNOW_RECORDING_WINDOW_STARTUP_BENCHMARK
    if (app.arguments().contains(QStringLiteral("--recording-window-startup-performance"))) {
        const int result = runRecordingWindowStartupPerformanceBenchmark(app);
        ApplicationStorage::instance().shutdown();
        return result;
    }
#endif
#ifdef Q_OS_WIN
#ifdef SNOW_RECORDING_EFFECTS_BENCHMARK
    if (app.arguments().contains(QStringLiteral("--effects-preview-performance"))) {
        const int result = runRecordingEffectsPerformanceBenchmark(app);
        ApplicationStorage::instance().shutdown();
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
        ApplicationStorage::instance().shutdown();
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
    if (app.arguments().contains(QStringLiteral("--trim-only"))) {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        recordingTrimmingTests(temporary.path());
        ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--finalization-close-only"))) {
        recordingCanCloseDuringAndAfterFinalization();
        ApplicationStorage::instance().shutdown();
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
        ApplicationStorage::instance().shutdown();
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
        ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--modal-stacking-only"))) {
        recordingModalStacking();
        ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--post-processing-only"))) {
        recordingPostProcessingLifecycle();
        ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--settings-dialog-only"))) {
        recordingSettingsDialog();
        ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--color-sampling-only"))) {
        recordingColorSamplingIsConnected();
        recordingColorSamplerInteractions();
        ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--color-sampling-native-only"))) {
        recordingColorSamplingIsConnected(true);
        ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--eraser-only"))) {
        recordingEraserTools();
        ApplicationStorage::instance().shutdown();
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
        ApplicationStorage::instance().shutdown();
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
        require(controller.startAutomation(QRect(10, 10, 320, 240),
                                           {{QStringLiteral("format"), QStringLiteral("gif")},
                                            {QStringLiteral("start_delay_seconds"), 10},
                                            {QStringLiteral("animated_frame_rate"), 15}},
                                           &error),
                "automation must support isolated recording options");
        require(controller.automationState().value(QStringLiteral("operation")).toString() ==
                    QStringLiteral("counting_down"),
                "automation state must expose countdown");
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
                                            {QStringLiteral("separate_audio_tracks"), true}},
                                           &error),
                "automation start must succeed");
        waitForRecording(controller);
        require(!QFileInfo::exists(outputPath),
                "the controller must leave output publication to the recording exporter");
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
        ApplicationStorage::instance().shutdown();
        return 0;
    }
#ifdef Q_OS_MACOS
    if (app.arguments().contains(QStringLiteral("--effects-preview-dpi-only"))) {
        effectsPreviewPhysicalPixels();
        ApplicationStorage::instance().shutdown();
        return 0;
    }
#endif
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
        require(app.installTranslator(&translator), "key translator must install");
        const RecordingKeyboardLabels localized(true);
        require(localized.text == original.text,
                "all key legends must ignore application language");
        require(localized.text.contains(QByteArray("Backspace")) &&
                    localized.text.contains(QByteArray("Num 0")),
                "key legends must use English names");
        app.removeTranslator(&translator);
        recordingKeyboardFontFollowsApplication();
        effectsPreviewLifecycle();
        areaWindowPaintsInputSurfaceOnlyWhenItIsVisible();
        previewIgnoresEventsOtherThanDialogVisibility();
        recordingKeyboardColorsFollowBackground();
        controllerPreviewTransitions();
        ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--capture-exclusion-only"))) {
        recordingCaptureExclusionWiring();
        ApplicationStorage::instance().shutdown();
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
    recordingCanCloseDuringAndAfterFinalization();
    delayCountdownBlocksTheStartUntilItElapses();
    ApplicationStorage::instance().shutdown();
    return 0;
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

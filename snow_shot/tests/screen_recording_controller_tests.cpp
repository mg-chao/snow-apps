#include "snow_shot/presentation/screenrecordingcontroller.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_capture.h"
#include "widgets/button.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QMouseEvent>
#include <QWindow>
#include <QScreen>
#include <QPointer>
#include <QTimer>
#include <QMessageBox>
#include "widgets/color_picker.h"
#include <future>
#ifdef Q_OS_WIN
#include <qt_windows.h>
int recordingToolbarAcrossNativeDisplays(bool startCapture);
#endif
#include <atomic>
#include <cstdlib>
#include <iostream>

struct SnowCaptureRecordingSessionImpl {};
namespace {
SnowCaptureRecordingSession session;
int starts = 0;
std::atomic<int> exports = 0;
std::shared_future<void> exportGate;
std::promise<void>* exportEntered = nullptr;
std::atomic<bool> failExport = false;
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

class ErrorObserver final : public QObject {
  public:
    int shown = 0;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Show) {
            if (auto* dialog = qobject_cast<QMessageBox*>(watched)) {
                ++shown;
                QTimer::singleShot(0, dialog, &QMessageBox::accept);
            }
        }
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

int recordingWindowCount() {
    int count = 0;
    for (auto* widget : QApplication::topLevelWidgets()) {
        count += qobject_cast<ScreenRecordingAreaWindow*>(widget) != nullptr ||
                 qobject_cast<ScreenRecordingToolbarWindow*>(widget) != nullptr;
    }
    return count;
}

void closeAndStopHaveIndependentUiLifetimes() {
    ErrorObserver errors;
    qApp->installEventFilter(&errors);
    for (const bool close : {false, true}) {
        for (const bool failure : {false, true}) {
            ScreenRecordingController controller;
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
            QCoreApplication::processEvents();
            require(controller.isRecording(), "fake backend must start");
            if (close) {
                // Exercise the native close path as well as the toolbar command.
                toolbar->close();
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

void recordingToolbarReconcilesFrameBeforeShowing() {
    ScreenRecordingToolbarWindow toolbar;
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "placement test requires a screen");
    const QRect region = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    toolbar.placeForPhysicalRegion(region);
    toolbar.showAndActivate();
    QCoreApplication::processEvents();
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
                area.setPhysicalRegion(region);
                toolbar.placeForPhysicalRegion(region);
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
                toolbar.placeForPhysicalRegion(region);
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
    toolbar.placeForPhysicalRegion(region);
    toolbar.show();
    QCoreApplication::processEvents();
    const auto requireFits = [&](const char* message) {
        const QRect occupied = toolbar.occupiedContentRect().translated(toolbar.contentPosition());
        require(occupied.top() >= bounds.top() && occupied.bottom() <= bounds.bottom(), message);
        // The default offscreen screen is narrower than the recording main row.
        if (occupied.width() <= bounds.width()) {
            require(bounds.contains(occupied), message);
        }
    };
    const auto requireAnchored = [&]() {
        const QPoint position = toolbar.contentPosition();
        const QRect occupied = toolbar.occupiedContentRect();
        toolbar.placeForPhysicalRegion(region);
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
    toolbar.placeForPhysicalRegion(region);
    exportButton->click();
    requireAnchored();
    exportButton->click();
    requireAnchored();
}
} // namespace

extern "C" {
SnowCaptureResult
snow_capture_recording_session_create_direct(const SnowCaptureDirectRecordingConfig*,
                                             SnowCaptureRecordingSession** result) {
    *result = &session;
    return SNOW_CAPTURE_RESULT_OK;
}
void snow_capture_recording_session_destroy(SnowCaptureRecordingSession*) {
    ++destroyedSessions;
}
uint8_t snow_capture_recording_session_start(SnowCaptureRecordingSession*) {
    ++starts;
    return 1;
}
uint8_t snow_capture_recording_session_pause(SnowCaptureRecordingSession*) {
    return 1;
}
uint8_t snow_capture_recording_session_resume(SnowCaptureRecordingSession*) {
    return 1;
}
SnowCaptureResult snow_capture_recording_session_stop(SnowCaptureRecordingSession*) {
    ++exports;
    if (exportEntered != nullptr) {
        exportEntered->set_value();
    }
    if (exportGate.valid()) {
        exportGate.wait();
    }
    return failExport ? SNOW_CAPTURE_RESULT_INVALID_ARGUMENT : SNOW_CAPTURE_RESULT_OK;
}
uint8_t snow_capture_recording_session_state(const SnowCaptureRecordingSession*,
                                             SnowCaptureRecordingState* state) {
    *state = SNOW_CAPTURE_RECORDING_STATE_RUNNING;
    return 1;
}
const char* snow_capture_last_error_message() {
    return "test backend";
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary storage must exist");
    using namespace snow_shot::storage;
    require(ApplicationStorage::instance()
                .initialize({temporary.filePath("bin"), temporary.filePath("settings"), 0})
                .success,
            "isolated storage must initialize");
    require(RecordingSettings().setVideoSaveDirectory(temporary.path()),
            "test output directory must be set");
    require(RecordingSettings().setHideToolbarInRecording(false),
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
    recordingToolbarReconcilesFrameBeforeShowing();
    recordingToolbarPlacementAcrossDisplays();
    if (app.arguments().contains(QStringLiteral("--placement-only"))) {
        return 0;
    }
    recordingSecondaryPanelsStayOnScreen();
    {
        ScreenRecordingController controller;
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
        toolbar->placeForPhysicalRegion(area->physicalRegion());
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
        ScreenRecordingController controller;
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
        controller.startRecording();
        controller.startRecording();
        QCoreApplication::processEvents();
        require(starts == 1 && controller.isRecording(), "a fresh request must start exactly once");
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
        ScreenRecordingController controller;
        controller.open(QRect(40, 40, 320, 240));
        controller.startRecording();
    }
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(starts == 1, "destroying the controller must cancel a queued recording start");
    closeAndStopHaveIndependentUiLifetimes();
    ApplicationStorage::instance().shutdown();
    return 0;
}

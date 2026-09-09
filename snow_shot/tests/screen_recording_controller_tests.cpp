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
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <atomic>
#include <cstdlib>
#include <iostream>

struct SnowCaptureRecordingSessionImpl {};
namespace {
SnowCaptureRecordingSession session;
int starts = 0;
std::atomic<int> exports = 0;
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
void snow_capture_recording_session_destroy(SnowCaptureRecordingSession*) {}
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
    return SNOW_CAPTURE_RESULT_OK;
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
        palette()->recordingCloseRequested();
        controller.open(region);
        require(area->isVisible() && area->canvas() == canvas,
                "a new recording session must reuse the area and canvas");
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
        int toolbarCount = 0;
        for (auto* widget : QApplication::topLevelWidgets()) {
            toolbarCount += qobject_cast<ScreenRecordingToolbarWindow*>(widget) != nullptr;
        }
        require(toolbarCount == 1, "reopening must reuse the controller's existing toolbar");
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
    ApplicationStorage::instance().shutdown();
    return 0;
}

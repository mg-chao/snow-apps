#include "snow_shot/presentation/screenrecordingareawindow.h"

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "../src/presentation/recording/screenrecordinggeometry.h"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWheelEvent>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void sendMouseEvent(SnowCanvasWidget& canvas, QEvent::Type type, const QPointF& position,
                    Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &event);
}

void drawRectangle(SnowCanvasWidget& canvas, const QPointF& start, const QPointF& end) {
    require(canvas.setCanvasTool(SnowCanvasTool::Shape),
            "recording canvas should accept the shape tool");
    sendMouseEvent(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
}

bool imageHasVisiblePixel(const QImage& image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() != 0) {
                return true;
            }
        }
    }
    return false;
}

QImage renderWidget(QWidget& widget) {
    QImage image(widget.size(), QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    return image;
}

QRect testPhysicalRegion() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "recording area tests require an offscreen primary screen");
    const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const int width = qMin(320, qMax(80, bounds.width() / 2));
    const int height = qMin(240, qMax(60, bounds.height() / 2));
    return QRect(bounds.left() + qMax(0, (bounds.width() - width) / 4),
                 bounds.top() + qMax(0, (bounds.height() - height) / 4), width, height);
}

void geometryAndTransparentCanvasFollowThePhysicalSelection() {
    ScreenRecordingAreaWindow area;
    const QRect physical = testPhysicalRegion();
    area.setPhysicalRegion(physical);
    area.show();
    QCoreApplication::processEvents();

    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(physical);
    const QRectF logical = ScreenshotGeometryMapper::logicalRectFForPhysicalRect(physical, screen);
    const auto expected = snow_shot::presentation::recording::screenRecordingAreaFrameGeometry(
        logical, screen != nullptr ? screen->devicePixelRatio() : 1.0);
    require(area.geometry() == expected.windowGeometry &&
                area.canvasGeometry() == expected.selectionRect.toAlignedRect(),
            "recording canvas should exactly cover the logical capture selection");
    require(area.canvas() != nullptr && !area.canvas()->clearBackgroundEnabled() &&
                !area.canvas()->wheelZoomEnabled() && area.canvas()->canvasContentVisible(),
            "recording canvas should render transparently without viewport wheel transforms");
    require(!imageHasVisiblePixel(renderWidget(*area.canvas())),
            "an empty recording canvas should remain fully transparent");
}

void inputModesOwnOnlyDrawingInputAndRestoreRequestedState() {
    ScreenRecordingAreaWindow area;
    area.setPhysicalRegion(testPhysicalRegion());
    area.show();
    QCoreApplication::processEvents();
    SnowCanvasWidget* canvas = area.canvas();
    require(canvas != nullptr &&
                area.inputMode() == ScreenRecordingAreaWindow::InputMode::PassThrough &&
                area.testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !canvas->interactionEnabled(),
            "recording area should start in pass-through mode");

    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    require(!area.testAttribute(Qt::WA_TransparentForMouseEvents) && canvas->interactionEnabled(),
            "drawing mode should enable canvas input without replacing the window");
    const WId nativeId = area.winId();
    area.setDrawingBlocked(true);
    require(area.inputMode() == ScreenRecordingAreaWindow::InputMode::Drawing &&
                area.drawingBlocked() && area.testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !canvas->interactionEnabled(),
            "busy transitions should block input while retaining the requested drawing mode");
    area.setDrawingBlocked(false);
    require(area.winId() == nativeId && canvas->interactionEnabled() &&
                !area.testAttribute(Qt::WA_TransparentForMouseEvents),
            "leaving a busy transition should restore drawing without recreating the window");

    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    require(area.winId() == nativeId && area.testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !canvas->interactionEnabled(),
            "deactivation should restore pass-through on the existing native window");
}

void wheelAndEscapeRespectDrawingOwnership() {
    ScreenRecordingAreaWindow area;
    area.setPhysicalRegion(testPhysicalRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    SnowCanvasWidget* canvas = area.canvas();
    require(canvas != nullptr, "recording area should own a canvas");

    int wheelRequests = 0;
    int wheelDirection = 0;
    int deactivationRequests = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::drawingWheelRequested, &area,
                     [&](int direction) {
                         ++wheelRequests;
                         wheelDirection = direction;
                     });
    QObject::connect(&area, &ScreenRecordingAreaWindow::drawingDeactivationRequested, &area,
                     [&]() { ++deactivationRequests; });

    QWheelEvent wheel(QPointF(20, 20), QPointF(20, 20), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas, &wheel);
    require(wheel.isAccepted() && wheelRequests == 1 && wheelDirection == 1,
            "drawing mode should consume the wheel and request a positive tool adjustment");

    sendMouseEvent(*canvas, QEvent::MouseButtonPress, QPointF(16, 16), Qt::LeftButton,
                   Qt::LeftButton);
    QKeyEvent cancelGesture(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &cancelGesture);
    require(deactivationRequests == 0,
            "the first Escape should cancel an in-progress gesture before deactivating drawing");
    QKeyEvent deactivate(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &deactivate);
    require(deactivationRequests == 1 && deactivate.isAccepted(),
            "Escape should request pass-through after transient canvas work is canceled");

    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    QWheelEvent passThroughWheel(QPointF(20, 20), QPointF(20, 20), QPoint(), QPoint(0, -120),
                                 Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas, &passThroughWheel);
    require(wheelRequests == 1,
            "pass-through mode should not claim wheel input for drawing adjustments");
}

void annotationsPersistAcrossStatesAndClearOnlyForANewRegion() {
    ScreenRecordingAreaWindow area;
    const QRect firstRegion = testPhysicalRegion();
    area.setPhysicalRegion(firstRegion);
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    SnowCanvasWidget* canvas = area.canvas();
    drawRectangle(*canvas, QPointF(20, 20), QPointF(70, 55));
    require(canvas->canvasHistoryState().canUndo && imageHasVisiblePixel(renderWidget(*canvas)),
            "drawing while idle should create visible recording annotations");

    for (const auto state : {ScreenshotToolPalette::RecordingState::Recording,
                             ScreenshotToolPalette::RecordingState::Paused,
                             ScreenshotToolPalette::RecordingState::Idle}) {
        area.setRecordingState(state);
        QCoreApplication::processEvents();
        require(canvas->canvasHistoryState().canUndo,
                "annotations should survive recording, pause, resume, stop, and idle states");
    }
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    require(canvas->canvasHistoryState().canUndo && imageHasVisiblePixel(renderWidget(*canvas)),
            "completed annotations should remain visible while input passes through");

    area.setPhysicalRegion(firstRegion);
    require(canvas->canvasHistoryState().canUndo,
            "reapplying the same recording region should preserve annotations");
    const QRect secondRegion = firstRegion.translated(8, 6);
    area.setPhysicalRegion(secondRegion);
    QCoreApplication::processEvents();
    require(!canvas->canvasHistoryState().canUndo,
            "opening a different recording region should clear the previous annotations once");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    geometryAndTransparentCanvasFollowThePhysicalSelection();
    inputModesOwnOnlyDrawingInputAndRestoreRequestedState();
    wheelAndEscapeRespectDrawingOwnership();
    annotationsPersistAcrossStatesAndClearOnlyForANewRegion();
    return 0;
}

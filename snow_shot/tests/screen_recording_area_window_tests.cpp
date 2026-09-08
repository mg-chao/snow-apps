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
                !area.testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !canvas->interactionEnabled(),
            "idle pass-through should retain resize input without enabling drawing");

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
    require(area.winId() == nativeId && !area.testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !canvas->interactionEnabled(),
            "deactivation should restore idle resize input on the existing native window");
    area.setRecordingState(ScreenshotToolPalette::RecordingState::Recording);
    require(area.testAttribute(Qt::WA_TransparentForMouseEvents),
            "recording pass-through should release the whole window's input");
}

void drawingSurfaceFollowsEffectiveInputMode() {
    ScreenRecordingAreaWindow area;
    area.setPhysicalRegion(testPhysicalRegion());
    area.show();

    const auto requireSurface = [&](bool drawing) {
        QCoreApplication::processEvents();
        const QImage image = renderWidget(area);
        const QRect interior = area.canvasGeometry().adjusted(8, 8, -8, -8);
        for (int y = interior.top(); y <= interior.bottom(); ++y) {
            for (int x = interior.left(); x <= interior.right(); ++x) {
                const int alpha = image.pixelColor(x, y).alpha();
                if (drawing ? alpha <= 0 || alpha > 2 : alpha != 0) {
                    std::cerr << "drawing=" << drawing << " pixel=" << x << ',' << y
                              << " alpha=" << alpha << '\n';
                }
                require(drawing ? alpha > 0 && alpha <= 2 : alpha == 0,
                        "empty drawing pixels must receive layered-window hits only while drawing");
            }
        }
        require(image.pixelColor(0, 0).alpha() == 0,
                "drawing input must not fill the padding outside the recording region");
    };

    requireSurface(false);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    for (const auto state : {ScreenshotToolPalette::RecordingState::Idle,
                             ScreenshotToolPalette::RecordingState::Recording,
                             ScreenshotToolPalette::RecordingState::Paused}) {
        area.setRecordingState(state);
        requireSurface(true);
        area.setDrawingBlocked(true);
        requireSurface(false);
        area.setDrawingBlocked(false);
        requireSurface(true);
    }
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    requireSurface(false);
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

void sendRegionMouseEvent(QWidget& target, QEvent::Type type, const QPointF& global,
                          Qt::MouseButton button, Qt::MouseButtons buttons) {
    const QPointF local = global - QPointF(target.mapToGlobal(QPoint()));
    QMouseEvent event(type, local, local, global, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&target, &event);
}

void dragRegion(ScreenRecordingAreaWindow& area, const QPointF& local, const QPointF& delta,
                bool throughCanvas = false) {
    QWidget& target = throughCanvas ? *static_cast<QWidget*>(area.canvas()) : area;
    const QPointF start = QPointF(area.pos()) + local;
    sendRegionMouseEvent(target, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendRegionMouseEvent(target, QEvent::MouseMove, start + delta, Qt::NoButton, Qt::LeftButton);
    sendRegionMouseEvent(target, QEvent::MouseButtonRelease, start + delta, Qt::LeftButton,
                         Qt::NoButton);
}

void idleEdgesAndCornersResizePhysicalRegion() {
    ScreenRecordingAreaWindow area;
    const QRect initial = testPhysicalRegion();
    const qreal scale =
        ScreenshotGeometryMapper::screenForPhysicalRect(initial)->devicePixelRatio();
    const QPointF delta(13.0, 9.0);
    const QPoint physicalDelta(qRound(delta.x() * scale), qRound(delta.y() * scale));
    int changes = 0;
    QObject::connect(
        &area, &ScreenRecordingAreaWindow::physicalRegionChanged, &area, [&](const QRect& region) {
            require(region == area.physicalRegion(),
                    "region notification should contain the current physical selection");
            ++changes;
        });
    for (const auto mode : {ScreenRecordingAreaWindow::InputMode::PassThrough,
                            ScreenRecordingAreaWindow::InputMode::Drawing,
                            ScreenRecordingAreaWindow::InputMode::RegionEditing}) {
        area.setInputMode(mode);
        for (const Qt::Edges edges :
             {Qt::Edges(Qt::LeftEdge), Qt::Edges(Qt::RightEdge), Qt::Edges(Qt::TopEdge),
              Qt::Edges(Qt::BottomEdge), Qt::LeftEdge | Qt::TopEdge, Qt::RightEdge | Qt::TopEdge,
              Qt::LeftEdge | Qt::BottomEdge, Qt::RightEdge | Qt::BottomEdge}) {
            area.setPhysicalRegion(initial);
            area.show();
            QCoreApplication::processEvents();
            const QRect selection = area.canvasGeometry();
            QPointF start = selection.center();
            QRect expected = initial;
            if (edges.testFlag(Qt::LeftEdge)) {
                start.setX(selection.left() + 1);
                expected.setLeft(initial.left() + physicalDelta.x());
            }
            if (edges.testFlag(Qt::RightEdge)) {
                start.setX(selection.right() - 1);
                expected.setRight(initial.right() + physicalDelta.x());
            }
            if (edges.testFlag(Qt::TopEdge)) {
                start.setY(selection.top() + 1);
                expected.setTop(initial.top() + physicalDelta.y());
            }
            if (edges.testFlag(Qt::BottomEdge)) {
                start.setY(selection.bottom() - 1);
                expected.setBottom(initial.bottom() + physicalDelta.y());
            }
            const int previousChanges = changes;
            dragRegion(area, start, delta, true);
            require(area.physicalRegion() == expected && changes == previousChanges + 1,
                    "idle edges and corners should resize once using physical pixel deltas");
            require(!area.canvas()->canvasHistoryState().canUndo,
                    "resize gestures must not also create canvas annotations");
        }
    }
}

void exportSettingsMoveAndClampTheRegion() {
    ScreenRecordingAreaWindow area;
    const QRect initial = testPhysicalRegion();
    area.setPhysicalRegion(initial);
    area.show();
    QCoreApplication::processEvents();
    dragRegion(area, area.canvasGeometry().center(), QPointF(20, 10), true);
    require(area.physicalRegion() == initial,
            "pass-through interior should not move the recording selection");
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    const qreal scale =
        ScreenshotGeometryMapper::screenForPhysicalRect(initial)->devicePixelRatio();
    dragRegion(area, area.canvasGeometry().center(), QPointF(13, 9), true);
    require(area.physicalRegion() == initial.translated(qRound(13 * scale), qRound(9 * scale)),
            "export settings interior drag should move the region without changing its size");
    require(!area.canvas()->interactionEnabled() && !area.canvas()->canvasHistoryState().canUndo,
            "region movement should not enable or create drawing");
    const QImage surface = renderWidget(area);
    require(surface.pixelColor(area.canvasGeometry().center()).alpha() > 0,
            "export settings should provide a native hit surface throughout the region");
    const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(
        *ScreenshotGeometryMapper::screenForPhysicalRect(initial));
    dragRegion(area, area.canvasGeometry().center(), QPointF(-10000, -10000), true);
    require(area.physicalRegion().topLeft() == bounds.topLeft() &&
                area.physicalRegion().size() == initial.size(),
            "moving beyond the screen origin should clamp while preserving size");
    dragRegion(area, area.canvasGeometry().center(), QPointF(10000, 10000), true);
    require(area.physicalRegion().bottomRight() == bounds.bottomRight(),
            "moving beyond the screen end should clamp while preserving size");

    area.setPhysicalRegion(initial);
    dragRegion(area, area.canvasGeometry().topLeft(), QPointF(10000, 10000));
    require(area.physicalRegion().size() == QSize(2, 2) &&
                area.physicalRegion().bottomRight() == initial.bottomRight(),
            "resize should stop at two physical pixels without moving the opposite corner");
    dragRegion(area, QPointF(area.width() - 1, area.height() - 1), QPointF(20, 20));
    require(area.physicalRegion().width() > 2 && area.physicalRegion().height() > 2,
            "a minimum-size region must still allow resizing from its bottom-right corner");

    area.setPhysicalRegion(initial);
    dragRegion(area, area.canvasGeometry().topLeft(), QPointF(-10000, -10000));
    require(area.physicalRegion().topLeft() == bounds.topLeft() &&
                area.physicalRegion().bottomRight() == initial.bottomRight(),
            "resize should clamp to the screen while preserving the opposite corner");
}

void recordingAndBusyTransitionsCancelRegionGestures() {
    for (int transition = 0; transition < 4; ++transition) {
        ScreenRecordingAreaWindow area;
        const QRect initial = testPhysicalRegion();
        area.setPhysicalRegion(initial);
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
        area.show();
        QCoreApplication::processEvents();
        const QPointF start = QPointF(area.pos()) + area.canvasGeometry().center();
        sendRegionMouseEvent(area, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        if (transition == 0) {
            area.setRecordingState(ScreenshotToolPalette::RecordingState::Recording);
        } else if (transition == 1) {
            area.setRecordingState(ScreenshotToolPalette::RecordingState::Paused);
        } else if (transition == 2) {
            area.setDrawingBlocked(true);
        } else {
            area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
        }
        sendRegionMouseEvent(area, QEvent::MouseMove, start + QPointF(30, 20), Qt::NoButton,
                             Qt::LeftButton);
        sendRegionMouseEvent(area, QEvent::MouseButtonRelease, start + QPointF(30, 20),
                             Qt::LeftButton, Qt::NoButton);
        require(area.physicalRegion() == initial && QWidget::mouseGrabber() != &area,
                "state, busy, and tool transitions should cancel a region gesture");
        if (transition < 3) {
            dragRegion(area, area.canvasGeometry().bottomRight(), QPointF(20, 20));
            require(area.physicalRegion() == initial &&
                        area.testAttribute(Qt::WA_TransparentForMouseEvents),
                    "recording, paused, and busy states must lock region resizing and movement");
        }
        if (transition < 3) {
            require(area.inputMode() == ScreenRecordingAreaWindow::InputMode::RegionEditing &&
                        !area.canvas()->interactionEnabled(),
                    "locked export settings must retain region editing mode without drawing input");
        }
        area.setRecordingState(ScreenshotToolPalette::RecordingState::Idle);
        area.setDrawingBlocked(false);
        dragRegion(area, area.canvasGeometry().bottomRight(), QPointF(20, 20));
        require(area.physicalRegion() != initial, "returning to idle should restore edge resizing");
    }
}

void existingSelectionsBeyondOneScreenRemainEditable() {
    ScreenRecordingAreaWindow area;
    const QRect bounds =
        ScreenshotGeometryMapper::physicalRectForScreen(*QGuiApplication::primaryScreen());
    const QRect initial(bounds.right() - 80, bounds.top() + 20, 160, 100);
    area.setPhysicalRegion(initial);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    area.show();
    QCoreApplication::processEvents();
    dragRegion(area, area.canvasGeometry().center(), QPointF(-20, 0), true);
    require(area.physicalRegion().left() < initial.left() &&
                area.physicalRegion().size() == initial.size(),
            "an existing selection spanning screen bounds should still be movable");
    const QRect moved = area.physicalRegion();
    dragRegion(area, area.canvasGeometry().bottomRight(), QPointF(-10, -10), true);
    require(area.physicalRegion().width() < moved.width() &&
                area.physicalRegion().height() < moved.height(),
            "an existing selection spanning screen bounds should still be resizable");
}

void drawingAcrossResizeEdgesKeepsDrawingOwnership() {
    ScreenRecordingAreaWindow area;
    const QRect initial = testPhysicalRegion();
    area.setPhysicalRegion(initial);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    area.show();
    QCoreApplication::processEvents();
    drawRectangle(*area.canvas(), QPointF(20, 20),
                  QPointF(area.canvas()->width() - 2, area.canvas()->height() - 2));
    require(area.physicalRegion() == initial && area.canvas()->canvasHistoryState().canUndo,
            "drawing into a resize edge should finish the annotation without editing the region");
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    const QImage surface = renderWidget(area);
    const QPoint edge = area.canvasGeometry().topLeft() + QPoint(1, 1);
    require(surface.pixelColor(edge).alpha() > 0,
            "idle resize edges must have a nonzero-alpha native hit surface");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    geometryAndTransparentCanvasFollowThePhysicalSelection();
    inputModesOwnOnlyDrawingInputAndRestoreRequestedState();
    drawingSurfaceFollowsEffectiveInputMode();
    wheelAndEscapeRespectDrawingOwnership();
    annotationsPersistAcrossStatesAndClearOnlyForANewRegion();
    idleEdgesAndCornersResizePhysicalRegion();
    exportSettingsMoveAndClampTheRegion();
    recordingAndBusyTransitionsCancelRegionGestures();
    existingSelectionsBeyondOneScreenRemainEditable();
    drawingAcrossResizeEdgesKeepsDrawingOwnership();
    return 0;
}

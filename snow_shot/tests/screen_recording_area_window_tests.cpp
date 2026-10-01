#include "physical_key_test_support.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "../src/presentation/recording/recordingcountdownoverlay.h"
#include "../src/presentation/recording/screenrecordinggeometry.h"

#include <QApplication>
#include <QtMath>
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QTimer>
#include <QWindow>
#include <QDebug>
#ifdef Q_OS_MACOS
#include "macos_native_input.h"
#include <QElapsedTimer>
#include <QThread>
#include <stdexcept>
#endif
#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <cstdlib>
#include <iostream>
#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

class ScreenRecordingAreaWindowTestAccess {
  public:
    static QMargins insets(const ScreenRecordingAreaWindow& area) {
        return area.m_physicalInsets.toMargins();
    }
    static bool editable(const ScreenRecordingAreaWindow& area) {
        return area.regionEditingEnabled();
    }
    static Qt::Edges edges(const ScreenRecordingAreaWindow& area, QPointF position) {
        return area.resizeEdgesAt(position);
    }
    static void beginDrag(ScreenRecordingAreaWindow& area, QPoint point, Qt::Edges edges) {
        area.beginRegionDrag(point, edges);
    }
    static void drag(ScreenRecordingAreaWindow& area, QPoint point) {
        area.updateRegionDrag(point);
    }
    static void cancel(ScreenRecordingAreaWindow& area) {
        area.cancelRegionInteraction();
    }
    static bool dragging(const ScreenRecordingAreaWindow& area) {
        return area.m_controlledRegionDrag;
    }
    static void begin(ScreenRecordingAreaWindow& area) {
        area.beginRegionInteraction();
    }
    static void finish(ScreenRecordingAreaWindow& area) {
        area.finishRegionInteraction();
    }
    static QByteArray history(const ScreenRecordingAreaWindow& area) {
        return area.m_canvasRuntime->serializeDocumentHistory();
    }
};

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

QRect testRecordingRegion() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "recording area tests require an offscreen primary screen");
#ifdef Q_OS_MACOS
    const QRect bounds = screen->geometry();
#else
    const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
#endif
    const int width = qMin(320, qMax(80, bounds.width() / 2));
    const int height = qMin(240, qMax(60, bounds.height() / 2));
    return QRect(bounds.left() + qMax(0, (bounds.width() - width) / 4),
                 bounds.top() + qMax(0, (bounds.height() - height) / 4), width, height);
}

#ifdef Q_OS_MACOS
void recordingBorderInput(bool native) {
    ScreenRecordingAreaWindow area;
    const QRect initial(QGuiApplication::primaryScreen()->availableGeometry().center() -
                            QPoint(120, 80),
                        QSize(240, 160));
    area.setRecordingRegion(initial);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    area.show();
    QCoreApplication::processEvents();
    if (native)
        macActivateApplication();
    int starts = 0, finishes = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionStarted,
                     [&] { ++starts; });
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionFinished,
                     [&] { ++finishes; });
    const auto check = [](bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    for (const QPoint direction : {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1),
                                   QPoint(-1, -1), QPoint(1, -1), QPoint(-1, 1), QPoint(1, 1)}) {
        area.setRecordingRegion(initial);
        QCoreApplication::processEvents();
        // Hit the painted outer border, where AppKit can intercept input before Qt.
        const QPoint start(direction.x() < 0   ? initial.x() - 2
                           : direction.x() > 0 ? initial.x() + initial.width() + 1
                                               : initial.center().x(),
                           direction.y() < 0   ? initial.y() - 2
                           : direction.y() > 0 ? initial.y() + initial.height() + 1
                                               : initial.center().y());
        const auto send = [&](QEvent::Type type, QPoint global) {
            const QPoint local = area.mapFromGlobal(global);
            QMouseEvent event(type, local, local, global,
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(&area, &event);
            QCoreApplication::processEvents();
        };
        std::unique_ptr<MacMouseDrag> drag;
        if (native) {
            // WindowServer commits the preceding frame change asynchronously.
            QElapsedTimer timer;
            timer.start();
            while (!macWindowReceivesPoint(&area, start) && timer.elapsed() < 1000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            check(macWindowReceivesPoint(&area, start), "recording border must receive input");
            drag = std::make_unique<MacMouseDrag>(start);
        } else {
            send(QEvent::MouseButtonPress, start);
        }
        check(ScreenRecordingAreaWindowTestAccess::dragging(area),
              "border input must begin the controlled resize");
        for (int extent : {1, -40, 1, 60}) {
            const QPoint end = start - QPoint(direction.x() * (initial.width() - extent),
                                              direction.y() * (initial.height() - extent));
            if (native)
                drag->moveTo(end);
            else
                send(QEvent::MouseMove, end);
            const int minimum = snow_shot::presentation::recording::screenRecordingMinimumExtent(
                area.devicePixelRatioF());
            const int size = qMax(minimum, qAbs(extent));
            QRect expected = initial;
            if (direction.x()) {
                const int anchor = direction.x() > 0 ? initial.x() : initial.x() + initial.width();
                expected.setRect(anchor - (direction.x() * extent < 0 ? size : 0), expected.y(),
                                 size, expected.height());
            }
            if (direction.y()) {
                const int anchor = direction.y() > 0 ? initial.y() : initial.y() + initial.height();
                expected.setRect(expected.x(), anchor - (direction.y() * extent < 0 ? size : 0),
                                 expected.width(), size);
            }
            check(area.recordingRegion() == expected,
                  "each border must retain its anchor, enforce the minimum and flip both ways");
            check(area.selectionRect().size().toSize() == expected.size(),
                  "visible selection must match the committed recording region");
        }
        if (native)
            drag->finish();
        else
            send(QEvent::MouseButtonRelease,
                 start - QPoint(direction.x() * (initial.width() - 60),
                                direction.y() * (initial.height() - 60)));
        check(!ScreenRecordingAreaWindowTestAccess::dragging(area) && starts == finishes,
              "border release must finish exactly one controlled interaction");
    }
    check(starts == 8 && finishes == 8, "every edge and corner must complete one resize");
}

void nativeLogicalDrag() {
    ScreenRecordingAreaWindow area;
    QScreen* primary = QGuiApplication::primaryScreen();
    area.setRecordingRegion(
        QRect(primary->availableGeometry().topLeft() + QPoint(200, 200), QSize(321, 241)));
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    area.show();
    QCoreApplication::processEvents();
    macActivateApplication();
    for (QScreen* destination : QGuiApplication::screens()) {
        if (destination == primary)
            continue;
        for (QScreen* target : {destination, primary}) {
            const QRect origin = area.recordingRegion();
            const QPoint start = origin.center();
            const QPoint delta = target->availableGeometry().center() - start;
            const int steps = qMax(1, qMax(qAbs(delta.x()), qAbs(delta.y())) / 40);
            MacMouseDrag drag(start);
            for (int step = 1; step <= steps; ++step) {
                const QPoint offset = (QPointF(delta) * step / steps).toPoint();
                drag.moveTo(start + offset);
                if (area.recordingRegion() != origin.translated(offset)) {
                    qWarning() << "Recording drag" << "expected" << origin.translated(offset)
                               << "actual" << area.recordingRegion() << "pointer" << start + offset;
                    throw std::runtime_error("recording drag must preserve the cursor anchor and "
                                             "logical size across displays");
                }
            }
            drag.finish();
            require(area.recordingRegion() == origin.translated(delta) && area.screen() == target,
                    "recording region must remain on the destination display after release");
        }
    }
}

void logicalRegionDragAndResize() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(QRect(-231, -119, 321, 241));
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    area.show();
    QCoreApplication::processEvents();
    const QRect initial = area.recordingRegion();
    require(initial == QRect(-231, -119, 321, 241),
            "odd logical region must survive native layout unchanged");
    const auto send = [&area](QEvent::Type type, QPoint global, Qt::MouseButton button,
                              Qt::MouseButtons buttons) {
        const QPoint local = area.mapFromGlobal(global);
        QMouseEvent event(type, local, local, global, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(&area, &event);
        QCoreApplication::processEvents();
    };
    // A retained drawing-tool cursor must not mask the host's region cursor.
    area.canvas()->setCursorForLayer(SnowCanvasCursorLayer::CanvasTool, QCursor(Qt::CrossCursor));
    const auto hover = [&](QPoint global, Qt::CursorShape expected) {
        const QPoint local = area.canvas()->mapFromGlobal(global);
        QMouseEvent event(QEvent::MouseMove, local, local, global, Qt::NoButton, Qt::NoButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(area.canvas(), &event);
        require(area.cursor().shape() == expected && area.canvas()->cursor().shape() == expected &&
                    area.windowHandle()->cursor().shape() == expected,
                "region hover must own the widget, canvas and native window cursors");
    };
    hover(initial.center(), Qt::SizeAllCursor);
    hover(initial.topLeft(), Qt::SizeFDiagCursor);
    hover(QPoint(initial.left(), initial.center().y()), Qt::SizeHorCursor);
    const QPoint center = initial.center();
    send(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, center + QPoint(43, 27), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, center + QPoint(43, 27), Qt::LeftButton, Qt::NoButton);
    require(area.recordingRegion() == initial.translated(43, 27),
            "drag deltas must be logical pixels");
    const QRect moved = area.recordingRegion();
    const QPoint corner = moved.bottomRight();
    send(QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, corner + QPoint(19, 13), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, corner + QPoint(19, 13), Qt::LeftButton, Qt::NoButton);
    require(area.recordingRegion() == QRect(moved.topLeft(), moved.size() + QSize(19, 13)),
            "corner resize must preserve the logical origin and exact dimensions");
    hover(area.recordingRegion().center(), Qt::SizeAllCursor);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    require(area.canvas()->cursor().shape() == Qt::CrossCursor,
            "leaving region editing must release the host cursor to the drawing tool");
}
#endif

void geometryAndTransparentCanvasFollowThePhysicalSelection() {
    ScreenRecordingAreaWindow area;
    const QRect physical = testRecordingRegion();
    area.setRecordingRegion(physical);
    area.show();
    QCoreApplication::processEvents();

#ifdef Q_OS_MACOS
    const auto expected =
        snow_shot::presentation::recording::screenRecordingAreaFrameGeometry(QRectF(physical), 1.0);
#else
    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(physical);
    const QRectF logical = ScreenshotGeometryMapper::logicalRectFForPhysicalRect(physical, screen);
    const auto expected = snow_shot::presentation::recording::screenRecordingAreaFrameGeometry(
        logical, screen != nullptr ? screen->devicePixelRatio() : 1.0);
#endif
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
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    QCoreApplication::processEvents();
    SnowCanvasWidget* canvas = area.canvas();
    require(canvas != nullptr &&
                area.inputMode() == ScreenRecordingAreaWindow::InputMode::PassThrough &&
                area.testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !canvas->interactionEnabled(),
            "pass-through must disable both movement and resizing");

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
            "deactivation must release both movement and resize input");
    area.setRecordingState(ScreenshotToolPalette::RecordingState::Recording);
    require(area.testAttribute(Qt::WA_TransparentForMouseEvents),
            "recording pass-through should release the whole window's input");
}

void drawingSurfaceFollowsEffectiveInputMode() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
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
#ifdef Q_OS_MACOS
        // The point-based frame begins at the window origin. Sample its inner gap,
        // not the visible outer border.
        const QPoint padding(qFloor(area.selectionRect().left() - 0.5),
                             qFloor(area.selectionRect().center().y()));
#else
        const QPoint padding(0, 0);
#endif
        require(image.pixelColor(padding).alpha() == 0,
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
    area.setRecordingRegion(testRecordingRegion());
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
    PhysicalKeyEvent cancelGesture(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &cancelGesture);
    require(deactivationRequests == 0,
            "the first Escape should cancel an in-progress gesture before deactivating drawing");
    PhysicalKeyEvent deactivate(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &deactivate);
    require(deactivationRequests == 1 && deactivate.isAccepted(),
            "Escape should request pass-through after transient canvas work is canceled");

    require(canvas->setCanvasTool(SnowCanvasTool::Text), "activate recording text tool");
    sendMouseEvent(*canvas, QEvent::MouseButtonPress, QPointF(40, 40), Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(*canvas, QEvent::MouseButtonRelease, QPointF(40, 40), Qt::LeftButton,
                   Qt::NoButton);
    PhysicalKeyEvent insert(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier,
                            QStringLiteral("Preserved text"));
    QCoreApplication::sendEvent(canvas, &insert);
    require(canvas->hasActiveTextEditing(), "recording text draft should be active");
    PhysicalKeyEvent preserveText(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &preserveText);
    require(preserveText.isAccepted() && !canvas->hasActiveTextEditing() &&
                !canvas->testAttribute(Qt::WA_InputMethodEnabled) && deactivationRequests == 1 &&
                canvas->canvasHistoryState().canUndo,
            "Escape must commit recording text and end editing while preserving drawing mode");
    const QByteArray committedHistory = ScreenRecordingAreaWindowTestAccess::history(area);
    require(committedHistory.contains("Preserved text"), "Escape must retain recording text");
    PhysicalKeyEvent releaseText(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &releaseText);
    require(ScreenRecordingAreaWindowTestAccess::history(area) == committedHistory &&
                deactivationRequests == 1,
            "releasing Escape must preserve committed text and drawing mode");

    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    QWheelEvent passThroughWheel(QPointF(20, 20), QPointF(20, 20), QPoint(), QPoint(0, -120),
                                 Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas, &passThroughWheel);
    require(wheelRequests == 1,
            "pass-through mode should not claim wheel input for drawing adjustments");
}

void annotationsPersistAcrossStatesAndClearOnlyForANewRegion() {
    ScreenRecordingAreaWindow area;
    const QRect firstRegion = testRecordingRegion();
    area.setRecordingRegion(firstRegion);
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

    area.setRecordingRegion(firstRegion);
    require(canvas->canvasHistoryState().canUndo,
            "reapplying the same recording region should preserve annotations");
    const QRect secondRegion = firstRegion.translated(8, 6);
    area.setRecordingRegion(secondRegion);
    QCoreApplication::processEvents();
    require(!canvas->canvasHistoryState().canUndo,
            "opening a different recording region should clear the previous annotations once");
}

void geometryEditingIsOneCapability() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    for (const auto mode : {ScreenRecordingAreaWindow::InputMode::PassThrough,
                            ScreenRecordingAreaWindow::InputMode::Drawing,
                            ScreenRecordingAreaWindow::InputMode::RegionEditing}) {
        for (const auto state : {ScreenshotToolPalette::RecordingState::Idle,
                                 ScreenshotToolPalette::RecordingState::Recording,
                                 ScreenshotToolPalette::RecordingState::Paused}) {
            for (const bool busy : {false, true}) {
                area.setInputMode(mode);
                area.setRecordingState(state);
                area.setDrawingBlocked(busy);
                const bool editable = mode == ScreenRecordingAreaWindow::InputMode::RegionEditing &&
                                      state == ScreenshotToolPalette::RecordingState::Idle && !busy;
                require(ScreenRecordingAreaWindowTestAccess::editable(area) == editable,
                        "moving and resizing must share Export Settings idle eligibility");
                require((ScreenRecordingAreaWindowTestAccess::edges(area, {1, 1}) != Qt::Edges()) ==
                            editable,
                        "resize hit targets must never outlive movement eligibility");
            }
        }
    }
}

void interactionBoundariesAreIdempotentAndCancelOnStateChanges() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    int starts = 0;
    int finishes = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionStarted, &area,
                     [&]() { ++starts; });
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionFinished, &area,
                     [&]() { ++finishes; });
    ScreenRecordingAreaWindowTestAccess::begin(area);
    ScreenRecordingAreaWindowTestAccess::begin(area);
    require(starts == 1 && finishes == 0, "native entry must emit one start");
    area.setDrawingBlocked(true);
    require(finishes == 1, "busy transition must end an active operation");
    ScreenRecordingAreaWindowTestAccess::begin(area);
    require(starts == 1, "blocked input must not start an operation");
    area.setDrawingBlocked(false);
    ScreenRecordingAreaWindowTestAccess::begin(area);
    area.hide();
    ScreenRecordingAreaWindowTestAccess::finish(area);
    require(starts == 2 && finishes == 2, "hide and duplicate native exit must finish only once");
    area.show();
    area.setRecordingRegion({40, 40, 2, 2});
    require(ScreenRecordingAreaWindowTestAccess::edges(
                area, QRectF(area.canvasGeometry()).center()) == Qt::Edges(),
            "even a minimum-size region must retain an interior drag target");
}

void controlledBordersCrossAndCancelWithoutClearingAnnotations() {
    ScreenRecordingAreaWindow area;
    const QRect initial = testRecordingRegion();
    area.setRecordingRegion(initial);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    area.show();
    QCoreApplication::processEvents();
    drawRectangle(*area.canvas(), {20, 20}, {60, 50});
    const QByteArray history = ScreenRecordingAreaWindowTestAccess::history(area);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    int starts = 0, finishes = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionStarted,
                     [&] { ++starts; });
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionFinished,
                     [&] { ++finishes; });
    const QPoint pressed = initial.topLeft() + QPoint(initial.width(), initial.height());
    ScreenRecordingAreaWindowTestAccess::beginDrag(area, pressed, Qt::RightEdge | Qt::BottomEdge);
    ScreenRecordingAreaWindowTestAccess::drag(area, initial.topLeft() - QPoint(80, 60));
    require(area.recordingRegion().width() >= 80 && area.recordingRegion().height() >= 60 &&
                area.recordingRegion().x() < initial.x() &&
                area.recordingRegion().y() < initial.y(),
            "recording corner must cross both fixed boundaries");
    require(ScreenRecordingAreaWindowTestAccess::history(area) == history,
            "crossing borders must retain annotation history");
    ScreenRecordingAreaWindowTestAccess::drag(area, initial.topLeft() - QPoint(1, 1));
#ifdef Q_OS_MACOS
    const int minimum =
        snow_shot::presentation::recording::screenRecordingMinimumExtent(area.devicePixelRatioF());
#else
    const int minimum = 10;
#endif
    require(area.recordingRegion().width() >= minimum && area.recordingRegion().height() >= minimum,
            "recording minimum must hold immediately past the fixed corner");
    ScreenRecordingAreaWindowTestAccess::cancel(area);
    require(area.recordingRegion() == initial && starts == 1 && finishes == 1 &&
                !ScreenRecordingAreaWindowTestAccess::dragging(area),
            "cancellation must restore the starting region and finish once");
    for (const auto reason : {QEvent::UngrabMouse, QEvent::WindowDeactivate, QEvent::Hide}) {
        ScreenRecordingAreaWindowTestAccess::beginDrag(area, pressed, Qt::RightEdge);
        ScreenRecordingAreaWindowTestAccess::drag(area, initial.topLeft() - QPoint(20, 0));
        QEvent interruption(reason);
        QCoreApplication::sendEvent(&area, &interruption);
        require(!ScreenRecordingAreaWindowTestAccess::dragging(area) &&
                    area.recordingRegion() == initial,
                "interruption must roll back a crossed region");
    }
    ScreenRecordingAreaWindowTestAccess::beginDrag(area, pressed, Qt::RightEdge);
    ScreenRecordingAreaWindowTestAccess::drag(area, initial.topLeft() - QPoint(20, 0));
    PhysicalKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    PhysicalKeyEvent escapeRelease(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&area, &escape);
    QCoreApplication::sendEvent(&area, &escapeRelease);
    require(area.isVisible() && area.recordingRegion() == initial &&
                !ScreenRecordingAreaWindowTestAccess::dragging(area),
            "Escape must cancel the resize without closing the recording region");
    ScreenRecordingAreaWindowTestAccess::beginDrag(area, pressed, Qt::RightEdge);
    const QPoint end = initial.topLeft() - QPoint(80, 0);
#ifdef Q_OS_MACOS
    const QPointF logical(end);
#else
    const QPointF logical = ScreenshotGeometryMapper::logicalRectFForPhysicalRect(
                                QRect(end, QSize(1, 1)), area.screen())
                                .topLeft();
#endif
    QMouseEvent release(QEvent::MouseButtonRelease, area.mapFromGlobal(logical), logical,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&area, &release);
    require(!ScreenRecordingAreaWindowTestAccess::dragging(area) &&
                area.recordingRegion().x() < initial.x() && starts == finishes,
            "release must apply the final position and commit once");
    require(ScreenRecordingAreaWindowTestAccess::history(area) == history,
            "committing a crossing must preserve annotations");
}

void drawingAcrossFrameEdgesRetainsDrawingOwnership() {
    ScreenRecordingAreaWindow area;
    const QRect initial = testRecordingRegion();
    area.setRecordingRegion(initial);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    area.show();
    QCoreApplication::processEvents();
    drawRectangle(*area.canvas(), {20, 20},
                  {static_cast<qreal>(area.canvas()->width() - 2),
                   static_cast<qreal>(area.canvas()->height() - 2)});
    require(
        area.recordingRegion() == initial && area.canvas()->canvasHistoryState().canUndo,
        "drawing through frame hit regions must complete the annotation without editing geometry");
}

void countdownHelpersFollowTheSecondBoundaries() {
    namespace recording = snow_shot::presentation::recording;
    require(recording::screenRecordingCountdownRemainingSeconds(3000) == 3 &&
                recording::screenRecordingCountdownRemainingSeconds(2999) == 3 &&
                recording::screenRecordingCountdownRemainingSeconds(2001) == 3 &&
                recording::screenRecordingCountdownRemainingSeconds(2000) == 2 &&
                recording::screenRecordingCountdownRemainingSeconds(1000) == 1 &&
                recording::screenRecordingCountdownRemainingSeconds(1) == 1 &&
                recording::screenRecordingCountdownRemainingSeconds(0) == 1,
            "remaining seconds must count whole seconds and hold the final one");
    const auto progress = recording::screenRecordingCountdownProgress;
    require(progress(3000, 3000) == 1.0 && progress(3000, 1500) == 0.5 &&
                progress(3000, 0) == 0.0 && progress(3000, 4000) == 1.0 && progress(0, 0) == 0.0,
            "the progress ring must drain linearly and stay clamped to the full circle");
    const auto entrance = recording::screenRecordingCountdownDigitEntrance;
    require(entrance(3000, 3000) == 0.0 && entrance(3000, 2000) == 0.0 &&
                entrance(3000, 3000 - recording::screenRecordingCountdownDigitPopDurationMs) ==
                    1.0 &&
                entrance(3000, 1001) == 1.0,
            "each digit must pop in at its second boundary and settle within the second");
}

void countdownUsesThePausedBorderAndCentersItsIndicator() {
    namespace recording = snow_shot::presentation::recording;
    ScreenRecordingAreaWindow area;
    const QRect physical = testRecordingRegion();
    area.setRecordingRegion(physical);
    area.show();
    QCoreApplication::processEvents();

#ifdef Q_OS_MACOS
    const QRectF logical(physical);
    const qreal scale = 1.0;
#else
    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(physical);
    const QRectF logical = ScreenshotGeometryMapper::logicalRectFForPhysicalRect(physical, screen);
    const qreal scale = screen != nullptr ? screen->devicePixelRatio() : 1.0;
#endif
    const auto frame = recording::screenRecordingAreaFrameGeometry(logical, scale);
    const auto border = recording::screenRecordingAreaBorderGeometry(
        frame.frameRect, frame.selectionRect, frame.paddingWidth);
    const auto borderColor = [&area](const QPointF& position) {
        const QImage image = renderWidget(area);
        return image.pixelColor(qRound(position.x()), qRound(position.y()));
    };
    const QColor kIdle(0x40, 0x96, 0xff);
    const QColor kPaused(0xfa, 0xad, 0x14);
    const QPointF sample = border.top.center();

    require(!area.countdownActive(), "the countdown must start inactive");
    require(borderColor(sample) == kIdle, "an idle area must paint the blue border");
    area.startCountdown(3);
    require(area.countdownActive(), "a started countdown must report active");
    QCoreApplication::processEvents();
    require(borderColor(sample) == kPaused,
            "the countdown must repaint the border in the paused colour");

    auto* overlay = area.findChild<recording::RecordingCountdownOverlay*>();
    require(overlay != nullptr && overlay->isVisible(),
            "the countdown indicator must be a visible child of the recording area");
    require(overlay->remainingSeconds() == 3,
            "the indicator must start by showing the whole delay");
    const QRectF selection = area.selectionRect();
    const QRect geometry = overlay->geometry();
    require(geometry.width() == geometry.height() &&
                geometry.width() == recording::screenRecordingCountdownIndicatorSize,
            "the countdown indicator must be a square of the documented size");
    require(qAbs(QRectF(geometry).center().x() - selection.center().x()) <= 1.0 &&
                qAbs(QRectF(geometry).center().y() - selection.center().y()) <= 1.0,
            "the countdown indicator must center on the recording selection");

    area.setRecordingRegion(physical.translated(24, 16));
    QCoreApplication::processEvents();
    const QRect moved = overlay->geometry();
    require(qAbs(QRectF(moved).center().x() - area.selectionRect().center().x()) <= 1.0,
            "the indicator must follow selection layout changes");

    overlay->setRemainingMilliseconds(2500);
    const QImage midSecond = renderWidget(*overlay);
    const QPoint backdropSample(12, midSecond.height() / 2);
    require(midSecond.pixelColor(backdropSample).alpha() > 150,
            "the backdrop must stay opaque; the indicator must never fade as a whole");
    require(midSecond.pixelColor(3, 3).alpha() == 0 &&
                midSecond.pixelColor(midSecond.width() - 4, 3).alpha() == 0 &&
                midSecond.pixelColor(3, midSecond.height() - 4).alpha() == 0 &&
                midSecond.pixelColor(midSecond.width() - 4, midSecond.height() - 4).alpha() == 0,
            "the indicator backdrop must be circular, leaving the corners transparent");
    const QColor ring = midSecond.pixelColor(midSecond.width() / 2, 5);
    require(ring.red() > 200 && ring.green() > 120 && ring.blue() < 100,
            "a draining accent ring must paint the countdown progress");
    bool digitVisible = false;
    for (int y = 24; y < 64 && !digitVisible; ++y) {
        for (int x = 24; x < 64 && !digitVisible; ++x) {
            digitVisible = midSecond.pixelColor(x, y) == QColor(Qt::white);
        }
    }
    require(digitVisible, "the countdown digit must paint solid white between second boundaries");
    overlay->setRemainingMilliseconds(2000);
    require(renderWidget(*overlay).pixelColor(backdropSample).alpha() > 150,
            "second boundaries must pop only the digit, never the backdrop");

    area.clearCountdown();
    require(!area.countdownActive() && !overlay->isVisible(),
            "clearing must hide the countdown indicator");
    QCoreApplication::processEvents();
    require(borderColor(sample) == kIdle, "clearing must restore the idle border");
}

void quickSelectionFollowsTheDrawingSetting() {
    const snow_shot::storage::DrawingSettings drawingSettings;
    const QStringList originalDisabledTools = drawingSettings.quickSelectionDisabledTools();
    require(drawingSettings.setQuickSelectionDisabledTools({QStringLiteral("free-draw")}),
            "disabling free-draw quick selection should persist");

    // The window is constructed after the setting changed, so the very first
    // click already exercises the initial application of the policy.
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    QCoreApplication::processEvents();
    SnowCanvasWidget* canvas = area.canvas();
    require(canvas != nullptr, "recording area should own a canvas");

    require(canvas->setCanvasTool(SnowCanvasTool::FreeDraw),
            "recording canvas should accept the free-draw tool");
    sendMouseEvent(*canvas, QEvent::MouseButtonPress, QPointF(20, 20), Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(*canvas, QEvent::MouseMove, QPointF(70, 20), Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(*canvas, QEvent::MouseMove, QPointF(120, 20), Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(*canvas, QEvent::MouseButtonRelease, QPointF(120, 20), Qt::LeftButton,
                   Qt::NoButton);
    QCoreApplication::processEvents();
    require(canvas->canvasHistoryState().canUndo, "the free-draw stroke should be drawn");

    const auto clickStroke = [&]() {
        require(canvas->resetEditingState() && canvas->setCanvasTool(SnowCanvasTool::FreeDraw),
                "the fixture should restart free-draw creation without a selection");
        require(canvas->canvasStyleToolbarState().source ==
                    SnowCanvasStyleToolbarSource::DefaultFreeDraw,
                "the fixture should start from the creation style state");
        sendMouseEvent(*canvas, QEvent::MouseButtonPress, QPointF(70, 20), Qt::LeftButton,
                       Qt::LeftButton);
        sendMouseEvent(*canvas, QEvent::MouseButtonRelease, QPointF(70, 20), Qt::LeftButton,
                       Qt::NoButton);
        QCoreApplication::processEvents();
    };

    clickStroke();
    require(canvas->canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::DefaultFreeDraw,
            "the recording canvas must not select free-draw elements once disabled");

    // The disabled click creates a dot. Remove it before clicking again: its
    // endpoint would intentionally start stroke continuation before selection.
    require(canvas->undo(), "the disabled-selection dot should be undone");
    require(drawingSettings.setQuickSelectionDisabledTools({}),
            "enabling free-draw quick selection should persist");
    clickStroke();
    require(canvas->canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedFreeDraw,
            "quick selection should select the stroke while the setting keeps it enabled");

    require(drawingSettings.setQuickSelectionDisabledTools({QStringLiteral("free-draw")}),
            "re-disabling free-draw quick selection should persist");
    clickStroke();
    require(canvas->canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::DefaultFreeDraw,
            "the recording canvas must follow live quick-selection changes");

    require(drawingSettings.setQuickSelectionDisabledTools(originalDisabledTools),
            "restoring the quick-selection setting should persist");
}

void ordinaryWindowGeometryPreservesAnnotations() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    drawRectangle(*area.canvas(), {20, 20}, {70, 55});
    const QByteArray history = ScreenRecordingAreaWindowTestAccess::history(area);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    const QRect before = area.recordingRegion();
    int changes = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::recordingRegionChanged, &area,
                     [&](const QRect& region) {
                         ++changes;
                         require(region == area.recordingRegion(),
                                 "notification must report committed geometry");
                     });
    // Normal window events, including positions outside the starting screen, are authoritative.
    area.move(-200, -100);
    QCoreApplication::processEvents();
    const QRect moved = area.recordingRegion();
    require(moved.topLeft() != before.topLeft() && moved.size() == before.size() && changes > 0,
            "ordinary movement must synchronize physical coordinates without clamping");
    const QSize originalSize = area.size();
    area.resize(originalSize - QSize(20, 10));
    QCoreApplication::processEvents();
    require(area.recordingRegion().width() < moved.width() &&
                area.recordingRegion().height() < moved.height(),
            "ordinary resize must synchronize physical dimensions");
    area.resize(originalSize);
    QCoreApplication::processEvents();
    require(area.recordingRegion() == moved,
            "shrinking and expanding must restore the same physical rectangle");
    require(ScreenRecordingAreaWindowTestAccess::history(area) == history,
            "window geometry changes must preserve annotation coordinates and history");
}

#if defined(Q_OS_WIN) || defined(_WIN32)
void nativeRecordingBordersCross() {
    POINT saved{};
    GetCursorPos(&saved);
    const auto setPointer = [](const QPoint& position) {
        require(SetCursorPos(position.x(), position.y()) != FALSE,
                "native recording input requires an accessible interactive desktop");
        POINT actual{};
        require(GetCursorPos(&actual) && QPoint(actual.x, actual.y) == position,
                "native recording test could not position the physical pointer");
    };
    for (QScreen* display : QGuiApplication::screens()) {
        ScreenRecordingAreaWindow area;
        const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*display);
        const QRect original(bounds.topLeft() + QPoint(250, 200), QSize(240, 120));
        area.setRecordingRegion(original);
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
        area.show();
        QCoreApplication::processEvents();
        const HWND hwnd = reinterpret_cast<HWND>(area.winId());
        const QPoint start = original.topLeft() + QPoint(original.width(), original.height());
        setPointer(start);
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTBOTTOMRIGHT, MAKELPARAM(start.x(), start.y()));
        require(ScreenRecordingAreaWindowTestAccess::dragging(area) && GetCapture() == hwnd,
                "native recording border must own mouse capture");
        const QPoint crossed = original.topLeft() - QPoint(60, 30);
        setPointer(crossed);
        QMouseEvent move(QEvent::MouseMove, QPointF(), QPointF(QCursor::pos()), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&area, &move);
        if (area.recordingRegion() != QRect(crossed, QSize(60, 30))) {
            POINT observed{};
            GetCursorPos(&observed);
            qInfo() << "Crossing" << original << "start" << start << "end" << crossed << "pointer"
                    << QPoint(observed.x, observed.y) << "actual" << area.recordingRegion()
                    << "active" << ScreenRecordingAreaWindowTestAccess::dragging(area);
        }
        require(area.recordingRegion() == QRect(crossed, QSize(60, 30)),
                "native recording crossing must preserve exact physical dimensions");
        for (QScreen* destination : QGuiApplication::screens()) {
            const QPoint end =
                ScreenshotGeometryMapper::physicalRectForScreen(*destination).center();
            setPointer(end);
            QCoreApplication::sendEvent(&area, &move);
            require(area.recordingRegion().width() >= 10 && area.recordingRegion().height() >= 10 &&
                        ScreenRecordingAreaWindowTestAccess::dragging(area),
                    "cross-display recording resize must retain capture and its minimum");
        }
        SendMessageW(hwnd, WM_CANCELMODE, 0, 0);
        require(area.recordingRegion() == original &&
                    !ScreenRecordingAreaWindowTestAccess::dragging(area),
                "native recording cancellation must restore exact geometry");
        const QPoint end = original.topLeft() - QPoint(1, 1);
        setPointer(start);
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTBOTTOMRIGHT, MAKELPARAM(start.x(), start.y()));
        setPointer(end);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), QPointF(QCursor::pos()),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&area, &release);
        QCoreApplication::processEvents();
        require(area.recordingRegion() ==
                        QRect(original.topLeft() - QPoint(10, 10), QSize(10, 10)) &&
                    !ScreenRecordingAreaWindowTestAccess::dragging(area),
                "native release must commit the crossed minimum-sized region");
    }
    SetCursorPos(saved.x, saved.y);
}

void nativeWindowsGeometryAndInteraction() {
    require(QGuiApplication::platformName() == QStringLiteral("windows"),
            "native test requires Windows QPA");
    ScreenRecordingAreaWindow area;
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    int starts = 0;
    int finishes = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionStarted, &area,
                     [&]() { ++starts; });
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionFinished, &area,
                     [&]() { ++finishes; });
    std::cout << "Native display count: " << QGuiApplication::screens().size() << '\n';
    for (QScreen* display : QGuiApplication::screens()) {
        const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*display);
        const QRect selected(bounds.topLeft() + QPoint(40, 40), QSize(321, 241));
        area.setRecordingRegion(selected);
        area.show();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        const HWND handle = reinterpret_cast<HWND>(area.winId());
        qInfo() << display->name() << display->devicePixelRatio() << "expected" << selected
                << "observed" << area.recordingRegion() << "logical" << area.geometry();
        require(area.recordingRegion() == selected,
                "opening on each display must preserve exact physical selection");
        RECT initialClient{};
        POINT initialOrigin{};
        require(GetClientRect(handle, &initialClient) && ClientToScreen(handle, &initialOrigin),
                "initial native geometry must exist");
        const QMargins initialInsets(
            selected.left() - initialOrigin.x, selected.top() - initialOrigin.y,
            initialOrigin.x + initialClient.right - selected.x() - selected.width(),
            initialOrigin.y + initialClient.bottom - selected.y() - selected.height());
        const auto verifyClient = [&]() {
            RECT client{};
            POINT origin{};
            require(GetClientRect(handle, &client) && ClientToScreen(handle, &origin),
                    "native client geometry must be readable");
            const QRect clientGeometry(origin.x, origin.y, client.right - client.left,
                                       client.bottom - client.top);
            const QMargins currentInsets = ScreenRecordingAreaWindowTestAccess::insets(area);
            if (area.recordingRegion() != clientGeometry.marginsRemoved(currentInsets)) {
                qInfo() << "Geometry mismatch" << "client" << clientGeometry << "capture"
                        << area.recordingRegion() << "insets" << currentInsets << "logical"
                        << area.geometry();
            }
            require(area.recordingRegion() == clientGeometry.marginsRemoved(currentInsets),
                    "native events must synchronize capture to actual client pixels");
        };
        verifyClient();
        const QRect next(bounds.topLeft() + QPoint(60, 50), QSize(409, 307));
        require(SetWindowPos(handle, nullptr, next.x(), next.y(), next.width(), next.height(),
                             SWP_NOZORDER | SWP_NOACTIVATE) != 0,
                "native resize must succeed");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        verifyClient();
        // Physical changes can round to the same QWidget size at fractional DPI.
        for (const int nativeWidth : {410, 411, 409}) {
            SetWindowPos(handle, nullptr, 0, 0, nativeWidth, next.height(),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            verifyClient();
        }
        MINMAXINFO limits{};
        SendMessageW(handle, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
        require(
            limits.ptMinTrackSize.x - initialInsets.left() - initialInsets.right() == 10 &&
                limits.ptMinTrackSize.y - initialInsets.top() - initialInsets.bottom() == 10,
            "native minimum tracking size must preserve ten capture pixels after frame padding");
        const QRect physical = area.recordingRegion();
        const int x = physical.center().x();
        const int y = physical.center().y();
        const auto hit = [&](QPoint position) {
            return SendMessageW(handle, WM_NCHITTEST, 0, MAKELPARAM(position.x(), position.y()));
        };
        require(hit({x, y}) == HTCAPTION, "Export Settings center must delegate native movement");
        const std::pair<QPoint, LRESULT> targets[] = {{{physical.left(), y}, HTLEFT},
                                                      {{physical.right(), y}, HTRIGHT},
                                                      {{x, physical.top()}, HTTOP},
                                                      {{x, physical.bottom()}, HTBOTTOM},
                                                      {physical.topLeft(), HTTOPLEFT},
                                                      {physical.topRight(), HTTOPRIGHT},
                                                      {physical.bottomLeft(), HTBOTTOMLEFT},
                                                      {physical.bottomRight(), HTBOTTOMRIGHT},
                                                      {{x, y}, HTCAPTION}};
        for (const auto& [point, expected] : targets) {
            require(hit(point) == expected,
                    "all native hit targets must identify the correct operation");
            const int previousStarts = starts;
            const int previousFinishes = finishes;
            const QRect beforeInteraction = area.recordingRegion();
            POINT cursor{};
            GetCursorPos(&cursor);
            if (expected == HTCAPTION) {
                // SC_DRAGMOVE requires a real held mouse button. Exercise the same
                // system move loop through its keyboard command without injecting input.
                PostMessageW(handle, WM_SYSCOMMAND, SC_MOVE, 0);
            } else {
                SendMessageW(handle, WM_NCLBUTTONDOWN, static_cast<WPARAM>(expected),
                             MAKELPARAM(point.x(), point.y()));
            }
            QTimer cancel;
            QObject::connect(&cancel, &QTimer::timeout, &area,
                             [handle]() { SendMessageW(handle, WM_CANCELMODE, 0, 0); });
            cancel.start(0);
            PostMessageW(handle, WM_CANCELMODE, 0, 0);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            cancel.stop();
            if (expected == HTCAPTION) {
                SetCursorPos(cursor.x, cursor.y);
            }
            qInfo() << "Native interaction" << expected << "starts" << starts - previousStarts
                    << "finishes" << finishes - previousFinishes;
            require(starts == previousStarts + 1 && finishes == previousFinishes + 1,
                    "system move/resize must have one balanced interaction lifecycle");
            require(area.recordingRegion() == beforeInteraction,
                    "cancelling a no-motion border drag must preserve the exact region");
            verifyClient();
        }
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
        require(hit(physical.topLeft()) == HTCLIENT && hit({x, y}) == HTCLIENT,
                "drawing must disable both native resize and move targets");
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
        require(hit(physical.topLeft()) == HTTRANSPARENT && hit({x, y}) == HTTRANSPARENT,
                "pass-through must release both native resize and move targets");
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
        drawRectangle(*area.canvas(), {20, 20}, {70, 55});
        const QByteArray history = ScreenRecordingAreaWindowTestAccess::history(area);
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
        for (QScreen* destination : QGuiApplication::screens()) {
            if (destination == display) {
                continue;
            }
            const QRect destinationBounds =
                ScreenshotGeometryMapper::physicalRectForScreen(*destination);
            SetWindowPos(handle, nullptr, destinationBounds.x() + 60, destinationBounds.y() + 60, 0,
                         0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            verifyClient();
            require(area.screen() == destination &&
                        area.recordingRegion().intersects(destinationBounds),
                    "an existing native window must move onto another display without clamping");
            require(ScreenRecordingAreaWindowTestAccess::history(area) == history,
                    "native display transitions must preserve annotations");
        }
    }
}
#endif

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "failed to create recording area test storage directory");
    const QString executableDirectory =
        QDir(storageDirectory.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executableDirectory),
            "failed to create recording area test executable directory");
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({executableDirectory, storageDirectory.path(), 60000})
                .success,
            "failed to initialize isolated recording area test storage");
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (application.arguments().contains(QStringLiteral("--native-geometry-only"))) {
        nativeRecordingBordersCross();
        nativeWindowsGeometryAndInteraction();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
#endif
#ifdef Q_OS_MACOS
    if (application.arguments().contains(QStringLiteral("--native-border-input-only"))) {
        int result = 0;
        if (!macCanPostMouseEvents()) {
            result = 77;
        } else {
            try {
                recordingBorderInput(true);
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                result = 1;
            }
        }
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return result;
    }
    if (application.arguments().contains(QStringLiteral("--native-logical-drag-only"))) {
        int result = 0;
        if (QGuiApplication::screens().size() < 2 || !macCanPostMouseEvents()) {
            result = 77;
        } else {
            try {
                nativeLogicalDrag();
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                result = 1;
            }
        }
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return result;
    }
    recordingBorderInput(false);
    logicalRegionDragAndResize();
#endif
    controlledBordersCrossAndCancelWithoutClearingAnnotations();
    geometryAndTransparentCanvasFollowThePhysicalSelection();
    inputModesOwnOnlyDrawingInputAndRestoreRequestedState();
    drawingSurfaceFollowsEffectiveInputMode();
    wheelAndEscapeRespectDrawingOwnership();
    annotationsPersistAcrossStatesAndClearOnlyForANewRegion();
    quickSelectionFollowsTheDrawingSetting();
    geometryEditingIsOneCapability();
    interactionBoundariesAreIdempotentAndCancelOnStateChanges();
    ordinaryWindowGeometryPreservesAnnotations();
    drawingAcrossFrameEdgesRetainsDrawingOwnership();
    countdownHelpersFollowTheSecondBoundaries();
    countdownUsesThePausedBorderAndCentersItsIndicator();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}

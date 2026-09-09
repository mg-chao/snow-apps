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
#include <QTimer>
#include <QDebug>
#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <cstdlib>
#include <iostream>
#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

class ScreenRecordingAreaWindowTestAccess {
  public:
    static bool editable(const ScreenRecordingAreaWindow& area) {
        return area.regionEditingEnabled();
    }
    static Qt::Edges edges(const ScreenRecordingAreaWindow& area, QPointF position) {
        return area.resizeEdgesAt(position);
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

void geometryEditingIsOneCapability() {
    ScreenRecordingAreaWindow area;
    area.setPhysicalRegion(testPhysicalRegion());
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
    area.setPhysicalRegion(testPhysicalRegion());
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
    area.setPhysicalRegion({40, 40, 2, 2});
    require(ScreenRecordingAreaWindowTestAccess::edges(
                area, QRectF(area.canvasGeometry()).center()) == Qt::Edges(),
            "even a minimum-size region must retain an interior drag target");
}

void drawingAcrossFrameEdgesRetainsDrawingOwnership() {
    ScreenRecordingAreaWindow area;
    const QRect initial = testPhysicalRegion();
    area.setPhysicalRegion(initial);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    area.show();
    QCoreApplication::processEvents();
    drawRectangle(*area.canvas(), {20, 20},
                  {static_cast<qreal>(area.canvas()->width() - 2),
                   static_cast<qreal>(area.canvas()->height() - 2)});
    require(
        area.physicalRegion() == initial && area.canvas()->canvasHistoryState().canUndo,
        "drawing through frame hit regions must complete the annotation without editing geometry");
}

void ordinaryWindowGeometryPreservesAnnotations() {
    ScreenRecordingAreaWindow area;
    area.setPhysicalRegion(testPhysicalRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    drawRectangle(*area.canvas(), {20, 20}, {70, 55});
    const QByteArray history = ScreenRecordingAreaWindowTestAccess::history(area);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    const QRect before = area.physicalRegion();
    int changes = 0;
    QObject::connect(
        &area, &ScreenRecordingAreaWindow::physicalRegionChanged, &area, [&](const QRect& region) {
            ++changes;
            require(region == area.physicalRegion(), "notification must report committed geometry");
        });
    // Normal window events, including positions outside the starting screen, are authoritative.
    area.move(-200, -100);
    QCoreApplication::processEvents();
    const QRect moved = area.physicalRegion();
    require(moved.topLeft() != before.topLeft() && moved.size() == before.size() && changes > 0,
            "ordinary movement must synchronize physical coordinates without clamping");
    const QSize originalSize = area.size();
    area.resize(originalSize - QSize(20, 10));
    QCoreApplication::processEvents();
    require(area.physicalRegion().width() < moved.width() &&
                area.physicalRegion().height() < moved.height(),
            "ordinary resize must synchronize physical dimensions");
    area.resize(originalSize);
    QCoreApplication::processEvents();
    require(area.physicalRegion() == moved,
            "shrinking and expanding must restore the same physical rectangle");
    require(ScreenRecordingAreaWindowTestAccess::history(area) == history,
            "window geometry changes must preserve annotation coordinates and history");
}

#if defined(Q_OS_WIN) || defined(_WIN32)
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
        area.setPhysicalRegion(selected);
        area.show();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        const HWND handle = reinterpret_cast<HWND>(area.winId());
        qInfo() << display->name() << display->devicePixelRatio() << "expected" << selected
                << "observed" << area.physicalRegion() << "logical" << area.geometry();
        require(area.physicalRegion() == selected,
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
            if (area.physicalRegion() != clientGeometry.marginsRemoved(initialInsets)) {
                qInfo() << "Geometry mismatch" << "client" << clientGeometry << "capture"
                        << area.physicalRegion() << "insets" << initialInsets << "logical"
                        << area.geometry();
            }
            require(area.physicalRegion() == clientGeometry.marginsRemoved(initialInsets),
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
            limits.ptMinTrackSize.x - initialInsets.left() - initialInsets.right() == 2 &&
                limits.ptMinTrackSize.y - initialInsets.top() - initialInsets.bottom() == 2,
            "native minimum tracking size must preserve two capture pixels after frame padding");
        const QRect physical = area.physicalRegion();
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
                        area.physicalRegion().intersects(destinationBounds),
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
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (application.arguments().contains(QStringLiteral("--native-geometry-only"))) {
        nativeWindowsGeometryAndInteraction();
        return 0;
    }
#endif
    geometryAndTransparentCanvasFollowThePhysicalSelection();
    inputModesOwnOnlyDrawingInputAndRestoreRequestedState();
    drawingSurfaceFollowsEffectiveInputMode();
    wheelAndEscapeRespectDrawingOwnership();
    annotationsPersistAcrossStatesAndClearOnlyForANewRegion();
    geometryEditingIsOneCapability();
    interactionBoundariesAreIdempotentAndCancelOnStateChanges();
    ordinaryWindowGeometryPreservesAnnotations();
    drawingAcrossFrameEdgesRetainsDrawingOwnership();
    return 0;
}

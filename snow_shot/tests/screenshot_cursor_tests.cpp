#include "snow_shot/presentation/screenshotcursorimagesource.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotsourceimagecomposer.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QMouseEvent>
#include <QThread>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
void requireCursor(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

CapturedDisplayModel cursorDisplay(const QSize& size) {
    CapturedDisplayModel display;
    display.active = true;
    display.physicalRect = QRect(QPoint(), size);
    display.canvasRect = display.physicalRect;
    display.image = QImage(size, QImage::Format_RGBA8888);
    display.image.fill(QColor(20, 40, 60));
    display.cursorPixelRect = QRect(size.width() / 2, size.height() / 2, 16, 24);
    display.cursorPatch = QImage(display.cursorPixelRect.size(), QImage::Format_RGBA8888);
    display.cursorPatch.fill(QColor(200, 100, 50));
    return display;
}
void rendererDetachesOnDestruction() {
    SnowCanvasWidget canvas;
    {
        ScreenshotCanvasRenderer renderer(canvas);
        canvas.setCustomRenderer(&renderer);
    }
    requireCursor(canvas.customRenderer() == nullptr,
                  "renderer destruction must remove the borrowed canvas callback");
    {
        ScreenshotCanvasRenderer replacement(canvas);
        {
            ScreenshotCanvasRenderer previous(canvas);
            canvas.setCustomRenderer(&replacement);
        }
        requireCursor(canvas.customRenderer() == &replacement,
                      "old renderer destruction must preserve its replacement");
    }
    auto destroyedCanvas = std::make_unique<SnowCanvasWidget>();
    auto renderer = std::make_unique<ScreenshotCanvasRenderer>(*destroyedCanvas);
    destroyedCanvas->setCustomRenderer(renderer.get());
    destroyedCanvas.reset();
    renderer.reset();
}
} // namespace

void cursorTogglesPreserveAnnotationsAndSmartEraseHistory();

void runScreenshotCursorTests() {
    rendererDetachesOnDestruction();
    cursorTogglesPreserveAnnotationsAndSmartEraseHistory();
    for (const qreal scale : {1.0, 1.5, 2.0}) {
        auto display = cursorDisplay({120, 90});
        display.canvasUsesPoints = scale != 1.0;
        display.backingScale = scale;
        display.canvasRect = QRect(-40, -20, qRound(120 / scale), qRound(90 / scale));
        display.imageSourceCanvasRect = display.canvasRect;
        ScreenshotDisplaySession session;
        session.appendDisplay(display);
        session.cursorAvailable = true;
        const auto cleanPointer = display.image.constBits();
        const auto patchPointer = display.cursorPatch.constBits();
        const auto rect = screenshotCursorCanvasRect(display);
        requireCursor(rect == QRectF(-40 + 60 / scale, -20 + 45 / scale, 16 / scale, 24 / scale),
                      "cursor patch must map through image pixels into desktop canvas points");
        const auto queued = screenshotDisplayImageSource(display, true);
        for (bool visible : {false, true, false, true}) {
            session.cursorVisible = visible;
            const auto source = screenshotDisplayImageSource(display, visible);
            const auto preview = materializeScreenshotImageSource(
                source, QRectF(display.canvasRect), display.image.size());
            requireCursor(
                !preview.isNull() && preview.pixelColor(62, 47) ==
                                         (visible ? QColor(200, 100, 50) : QColor(20, 40, 60)),
                "preview must show the captured patch and reversibly restore clean pixels");
            QList<CanvasExportSource> sources;
            for (const auto& layer : screenshotDisplayImageLayers(display, visible))
                sources.push_back({layer.image, layer.destinationCanvasRect});
            SnowCanvasRuntime runtime;
            const QImage exported =
                runtime.renderToImage(display.canvasRect, display.image.size(), sources);
            requireCursor(exported == preview,
                          "preview and export must use identical cursor pixels");
            const QImage recognized = composeScreenshotSourceSelection(session, display.canvasRect);
            requireCursor(!recognized.isNull() &&
                              recognized.pixelColor(62, 47) == preview.pixelColor(62, 47),
                          "recognition and color sampling sources must share cursor visibility");
            const auto frozenOutput = materializeScreenshotImageSource(
                queued, QRectF(display.canvasRect), display.image.size());
            requireCursor(frozenOutput.pixelColor(62, 47) == QColor(200, 100, 50),
                          "queued output must retain its captured visibility after later toggles");
            requireCursor(display.image.constBits() == cleanPointer &&
                              display.cursorPatch.constBits() == patchPointer,
                          "visibility toggles must not detach retained desktop or cursor images");
        }
    }
}

void cursorTogglesPreserveAnnotationsAndSmartEraseHistory() {
    const auto display = cursorDisplay({120, 90});
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(display.image.size());
    canvas.show();
    QApplication::processEvents();
    static_cast<void>(canvas.setViewportCamera(60, 45, 1));
    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    renderer.setImageSource(screenshotDisplayImageSource(display, false));
    const auto drag = [&](QPointF start, QPointF end) {
        QMouseEvent press(QEvent::MouseButtonPress, start, start, start, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove, end, end, end, Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, end, end, end, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QCoreApplication::sendEvent(&canvas, &press);
        QCoreApplication::sendEvent(&canvas, &move);
        QCoreApplication::sendEvent(&canvas, &release);
    };
    requireCursor(canvas.setCanvasTool(SnowCanvasTool::Shape),
                  "activate cursor annotation fixture");
    SnowCanvasShapeStyle shape;
    shape.fill = QColor(Qt::green);
    shape.stroke = QColor(Qt::green);
    shape.strokeWidth = 1;
    requireCursor(canvas.setCanvasShapeStylePatch(shape,
                                                  SnowCanvasShapeStylePropertyFillColor |
                                                      SnowCanvasShapeStylePropertyStrokeColor |
                                                      SnowCanvasShapeStylePropertyStrokeWidth,
                                                  SnowCanvasShapeKind::Rectangle),
                  "configure cursor annotation fixture");
    drag(QPointF(62, 47), QPointF(73, 60));
    requireCursor(canvas.setCanvasTool(SnowCanvasTool::RectangleFilter), "activate eraser fixture");
    SnowCanvasFilterStyle filter;
    filter.type = SnowCanvasFilterType::SmartErase;
    requireCursor(canvas.setCanvasFilterStyle(filter, SnowCanvasFilterStylePropertyType),
                  "configure eraser fixture");
    drag(QPointF(10, 10), QPointF(20, 20));
    const auto exportImage = [&](bool visible) {
        QList<CanvasExportSource> sources;
        for (const auto& layer : screenshotDisplayImageLayers(display, visible))
            sources.push_back({layer.image, layer.destinationCanvasRect});
        return runtime.renderToImage(display.canvasRect, display.image.size(), sources);
    };
    QElapsedTimer eraseTimer;
    eraseTimer.start();
    while (eraseTimer.elapsed() < 10000 &&
           exportImage(false).pixelColor(15, 15) != QColor(20, 40, 60)) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    requireCursor(exportImage(false).pixelColor(15, 15) == QColor(20, 40, 60),
                  "smart erase fixture must finish reconstruction before cursor toggles");
    const auto history = runtime.serializeDocumentHistory();
    const auto revision = runtime.documentRevision();
    requireCursor(runtime.canUndo() && !history.isEmpty(), "fixture must retain editable history");
    for (bool visible : {true, false, true, false}) {
        renderer.setImageSource(screenshotDisplayImageSource(display, visible),
                                screenshotCursorCanvasRect(display));
        requireCursor(
            runtime.serializeDocumentHistory() == history &&
                runtime.documentRevision() == revision && runtime.canUndo(),
            "cursor toggles must preserve annotations, eraser commands, and undo history");
        const auto exported = exportImage(visible);
        requireCursor(exported.pixelColor(66, 53) == QColor(Qt::green),
                      "cursor patches must remain below annotations");
        requireCursor(exported.pixelColor(15, 15) == QColor(20, 40, 60),
                      "cursor toggles must keep completed smart erase results ready for export");
    }
    canvas.setCustomRenderer(nullptr);
}

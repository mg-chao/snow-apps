#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_canvas_display_item.h"
#include "snow_canvas_input_adapter.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"
#include "icons/draw_engine_icons.h"
#include "icon_registry.h"

#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void requireNear(qreal actual, qreal expected, const char* message) {
    if (std::abs(actual - expected) > 0.0001) {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

class RecordingRenderer final : public SnowCanvasCustomRenderer {
  public:
    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override {
        ++beforeCalls;
        beforeContext = context;
        painter.fillRect(context.viewportRect, QColor(220, 30, 30));
        painter.setOpacity(0.0);
        painter.setTransform(QTransform::fromTranslate(500.0, 500.0));
    }

    void renderAfterCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override {
        ++afterCalls;
        afterContext = context;
        painterStateRestored =
            qFuzzyCompare(painter.opacity(), 1.0) && painter.transform().isIdentity();
        painter.fillRect(QRect(0, 0, 8, 8), QColor(20, 190, 70));
    }

    int beforeCalls = 0;
    int afterCalls = 0;
    bool painterStateRestored = false;
    SnowCanvasRenderContext beforeContext;
    SnowCanvasRenderContext afterContext;
};

QImage renderCanvas(SnowCanvasWidget& canvas) {
    QImage image(canvas.size(), QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    canvas.render(&painter);
    painter.end();
    return image;
}

struct StrokeRunStats {
    int inkedColumns = 0;
    int gapColumns = 0;
    int longestInkRun = 0;
};

StrokeRunStats renderRectangleTopEdge(SnowStrokeStyle strokeStyle,
                                      SnowCornerRadii cornerRadii = {}) {
    constexpr QSize imageSize(160, 100);
    constexpr int sampleStartX = 45;
    constexpr int sampleEndX = 115;
    constexpr int sampleStartY = 27;
    constexpr int sampleEndY = 33;

    SceneDisplayInfo sceneInfo{};
    sceneInfo.item_count = 1;
    sceneInfo.surface_width = imageSize.width();
    sceneInfo.surface_height = imageSize.height();
    sceneInfo.camera_zoom = 1.0;

    SnowCanvasSceneItem item;
    item.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    item.center_x = 0.0;
    item.center_y = 0.0;
    item.width = 100.0;
    item.height = 40.0;
    item.fill = SnowColorRgba8{0, 0, 0, 0};
    item.fill_style = SNOW_FILL_STYLE_SOLID;
    item.stroke = SnowColorRgba8{0, 0, 0, 255};
    item.stroke_width = 4.0;
    item.stroke_style = strokeStyle;
    item.corner_radii = cornerRadii;
    item.rebuildLocalRectangleGeometry();

    QImage image(imageSize, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter, &sceneInfo, &item, 1, QRegion(image.rect())});
    painter.end();

    StrokeRunStats stats;
    int currentInkRun = 0;
    for (int x = sampleStartX; x <= sampleEndX; ++x) {
        bool hasInk = false;
        for (int y = sampleStartY; y <= sampleEndY; ++y) {
            if (image.pixelColor(x, y).alpha() > 127) {
                hasInk = true;
                break;
            }
        }
        if (hasInk) {
            ++stats.inkedColumns;
            ++currentInkRun;
            stats.longestInkRun = std::max(stats.longestInkRun, currentInkRun);
        } else {
            ++stats.gapColumns;
            currentInkRun = 0;
        }
    }
    return stats;
}

QColor renderMultiplyItemAtCenter(SnowCanvasSceneItem& item) {
    constexpr QSize imageSize(100, 100);
    SceneDisplayInfo sceneInfo{};
    sceneInfo.item_count = 1;
    sceneInfo.surface_width = imageSize.width();
    sceneInfo.surface_height = imageSize.height();
    sceneInfo.camera_zoom = 1.0;
    sceneInfo.clear_color = SnowColorRgba8{100, 150, 200, 255};

    QImage image(imageSize, QImage::Format_RGBA8888);
    image.fill(QColor(100, 150, 200));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter, &sceneInfo, &item, 1, QRegion(image.rect())});
    painter.end();
    return image.pixelColor(imageSize.width() / 2, imageSize.height() / 2);
}

void highlightItemsRenderWithMultiplyBlendMode() {
    SnowCanvasSceneItem rectangle;
    rectangle.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    rectangle.blend_mode = SNOW_BLEND_MODE_MULTIPLY;
    rectangle.width = 40.0;
    rectangle.height = 40.0;
    rectangle.fill = SnowColorRgba8{200, 100, 50, 255};
    rectangle.fill_style = SNOW_FILL_STYLE_SOLID;
    rectangle.opacity = 1.0;
    rectangle.rebuildLocalRectangleGeometry();
    const QColor rectanglePixel = renderMultiplyItemAtCenter(rectangle);
    require(rectanglePixel.red() < 100 && rectanglePixel.green() < 100 &&
                rectanglePixel.blue() < 100,
            "rectangle highlight should multiply its color with the canvas");

    // The sampled center lies seven pixels beyond the endpoint, inside a
    // 15-pixel round cap but outside a flat cap.
    SnowArrowPoint points[] = {{-30.0, 0.0}, {-7.0, 0.0}};
    SnowArrowPathCommand commands[2]{};
    commands[0].kind = SNOW_ARROW_PATH_COMMAND_MOVE_TO;
    commands[0].point = points[0];
    commands[1].kind = SNOW_ARROW_PATH_COMMAND_LINE_TO;
    commands[1].point = points[1];
    SnowCanvasSceneItem pen;
    pen.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
    pen.blend_mode = SNOW_BLEND_MODE_MULTIPLY;
    pen.stroke = SnowColorRgba8{200, 100, 50, 255};
    pen.stroke_width = 30.0;
    pen.opacity = 1.0;
    pen.arrow_type = SNOW_ARROW_TYPE_STRAIGHT;
    pen.arrow_stroke_style = SNOW_ARROW_STROKE_STYLE_SOLID;
    pen.setArrowPoints(points, 2);
    pen.arrow_path_commands = commands;
    pen.arrow_path_command_count = 2;
    const QColor penPixel = renderMultiplyItemAtCenter(pen);
    require(penPixel.red() < 100 && penPixel.green() < 100 && penPixel.blue() < 100,
            "pen highlight should use multiply blending and rounded endpoints");
}

void ownedAxisAlignedFreeDrawChunksRenderWithoutRawGeometry() {
    constexpr QSize imageSize(120, 120);

    SnowArrowPathCommand commands[3]{};
    commands[0].kind = SNOW_ARROW_PATH_COMMAND_MOVE_TO;
    commands[0].point = SnowArrowPoint{-40.0, 0.0};
    commands[1].kind = SNOW_ARROW_PATH_COMMAND_LINE_TO;
    commands[1].point = SnowArrowPoint{0.0, 0.0};
    commands[2].kind = SNOW_ARROW_PATH_COMMAND_LINE_TO;
    commands[2].point = SnowArrowPoint{0.0, 35.0};

    SnowPathChunk chunks[2]{};
    chunks[0].stable_id = 1;
    chunks[0].command_start = 0;
    chunks[0].command_offset = 0;
    chunks[0].command_count = 2;
    chunks[0].start_x = -40.0;
    chunks[0].start_y = 0.0;
    chunks[0].min_x = -40.0;
    chunks[0].min_y = 0.0;
    chunks[0].max_x = 0.0;
    chunks[0].max_y = 0.0;
    chunks[1].stable_id = 2;
    chunks[1].command_start = 2;
    chunks[1].command_offset = 2;
    chunks[1].command_count = 1;
    chunks[1].start_x = 0.0;
    chunks[1].start_y = 0.0;
    chunks[1].min_x = 0.0;
    chunks[1].min_y = 0.0;
    chunks[1].max_x = 0.0;
    chunks[1].max_y = 35.0;

    SnowPathChunkRange range{};
    range.insert_chunk_count = 2;

    SnowCanvasSceneItem item;
    item.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
    item.is_free_draw = 1;
    item.stroke = SnowColorRgba8{0, 0, 0, 255};
    item.stroke_width = 6.0;
    item.arrow_stroke_style = SNOW_ARROW_STROKE_STYLE_SOLID;
    item.opacity = 1.0;
    require(item.applyPathGeometryPatch(0, 1, &range, 1, chunks, 2, commands, 3, false, true),
            "free-draw path geometry should be accepted");

    std::vector<std::uint32_t> visibleChunks;
    item.queryPathChunks(QRectF(-50.0, -10.0, 100.0, 60.0), &visibleChunks);
    require(visibleChunks == std::vector<std::uint32_t>({0, 1}),
            "axis-aligned free-draw chunks must survive spatial culling");

    SceneDisplayInfo sceneInfo{};
    sceneInfo.item_count = 1;
    sceneInfo.surface_width = imageSize.width();
    sceneInfo.surface_height = imageSize.height();
    sceneInfo.camera_zoom = 1.0;

    const QRectF bounds = snow_canvas_render_geometry::sceneItemBounds(sceneInfo, item);
    require(bounds.contains(QPointF(20.0, 60.0)) && bounds.contains(QPointF(60.0, 95.0)),
            "owned free-draw geometry must contribute to scene bounds");

    QImage image(imageSize, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter, &sceneInfo, &item, 1, QRegion(image.rect())});
    painter.end();

    require(image.pixelColor(30, 60).alpha() > 127,
            "the horizontal free-draw chunk should be rendered");
    require(image.pixelColor(60, 80).alpha() > 127,
            "the vertical free-draw chunk should be rendered");
}

void rectangleFastPathHonorsOpacity() {
    constexpr QSize imageSize(100, 100);
    SceneDisplayInfo sceneInfo{};
    sceneInfo.item_count = 1;
    sceneInfo.surface_width = imageSize.width();
    sceneInfo.surface_height = imageSize.height();
    sceneInfo.camera_zoom = 1.0;

    SnowCanvasSceneItem rectangle;
    rectangle.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    rectangle.blend_mode = SNOW_BLEND_MODE_MULTIPLY;
    rectangle.width = 40.0;
    rectangle.height = 40.0;
    rectangle.fill = SnowColorRgba8{200, 100, 50, 255};
    rectangle.fill_style = SNOW_FILL_STYLE_SOLID;
    rectangle.opacity = 0.5;
    rectangle.rebuildLocalRectangleGeometry();

    QImage image(imageSize, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter, &sceneInfo, &rectangle, 1, QRegion(image.rect())});
    painter.end();

    const QColor center = image.pixelColor(imageSize.width() / 2, imageSize.height() / 2);
    require(center.alpha() >= 127 && center.alpha() <= 128,
            "rectangle fast path should apply item opacity");
}

void sendMouseEvent(SnowCanvasWidget& canvas, QEvent::Type type, const QPointF& position,
                    Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}

void runtimeExportUsesTheRequestedCanvasOrigin() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(200, 120);
    canvas.show();
    QApplication::processEvents();

    const QRectF selection(640.0, 360.0, 200.0, 120.0);
    require(canvas.setViewportCamera(selection.center().x(), selection.center().y(), 1.0),
            "non-zero export should configure the source canvas camera");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape),
            "non-zero export should activate the shape tool");
    SnowCanvasShapeStyle style;
    style.stroke = QColor(240, 24, 24);
    style.strokeWidth = 4.0;
    require(canvas.setCanvasShapeStylePatch(style,
                                            SnowCanvasShapeStylePropertyStrokeColor |
                                                SnowCanvasShapeStylePropertyStrokeWidth,
                                            SnowCanvasShapeKind::Rectangle),
            "non-zero export should configure a detectable rectangle stroke");
    sendMouseEvent(canvas, QEvent::MouseButtonPress, QPointF(35.0, 30.0), Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseMove, QPointF(165.0, 90.0), Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, QPointF(165.0, 90.0), Qt::LeftButton,
                   Qt::NoButton);

    QImage background(selection.size().toSize(), QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(242, 244, 247));
    const QImage output = runtime.renderToImage(selection, background.size(),
                                                {CanvasExportSource{background, selection}});
    bool containsRectangle = false;
    for (int y = 0; y < output.height() && !containsRectangle; ++y) {
        for (int x = 0; x < output.width(); ++x) {
            const QColor pixel = output.pixelColor(x, y);
            if (pixel.alpha() > 0 && pixel.red() > 180 && pixel.red() > pixel.green() * 2 &&
                pixel.red() > pixel.blue() * 2) {
                containsRectangle = true;
                break;
            }
        }
    }
    require(containsRectangle,
            "runtime export should include scene items at a non-zero canvas origin");
}

void customRendererContractIsOrderedAndIsolated() {
    SnowCanvasWidget canvas;
    canvas.resize(64, 64);
    require(canvas.setViewportCamera(10.0, 20.0, 2.0), "camera should update");

    RecordingRenderer renderer;
    canvas.setCustomRenderer(&renderer);
    require(canvas.customRenderer() == &renderer, "renderer should be retained non-owningly");

    QImage image(canvas.size(), QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    canvas.render(&painter);
    painter.end();

    require(renderer.beforeCalls == 1, "before renderer should run once");
    require(renderer.afterCalls == 1, "after renderer should run once");
    require(renderer.painterStateRestored, "painter state should be isolated between passes");
    require(renderer.beforeContext.viewportRect == canvas.rect(), "viewport should match canvas");
    require(renderer.beforeContext.exposedRegion.contains(canvas.rect()),
            "render context should expose the canvas");
    require(renderer.beforeContext.canvasToViewTransform ==
                renderer.afterContext.canvasToViewTransform,
            "both passes should receive the same transform");

    const QPointF mappedCenter =
        renderer.beforeContext.canvasToViewTransform.map(QPointF(10.0, 20.0));
    requireNear(mappedCenter.x(), 32.0, "camera center x should map to view center");
    requireNear(mappedCenter.y(), 32.0, "camera center y should map to view center");
    require(image.pixelColor(20, 20) == QColor(220, 30, 30),
            "before-canvas content should survive the engine surface clear");
    require(image.pixelColor(2, 2) == QColor(20, 190, 70),
            "after-canvas content should be the final rendered layer");

    require(canvas.viewRectForCanvasRect(QRectF(10.0, 20.0, 1.0, 1.0), 3) == QRect(29, 29, 8, 8),
            "canvas invalidation should honor camera scale and padding");

    canvas.setCustomRenderer(nullptr);
    require(canvas.customRenderer() == nullptr, "renderer should detach explicitly");
}

void canvasContentVisibilityPreservesCustomRenderingAndState() {
    SnowCanvasWidget canvas;
    canvas.resize(80, 80);
    canvas.show();
    QApplication::processEvents();
    require(canvas.canvasContentVisible(), "canvas content should be visible by default");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "shape tool should activate");

    sendMouseEvent(canvas, QEvent::MouseButtonPress, QPointF(16.0, 16.0), Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseMove, QPointF(56.0, 48.0), Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, QPointF(56.0, 48.0), Qt::LeftButton,
                   Qt::NoButton);
    require(canvas.canvasHistoryState().canUndo,
            "rectangle input should create engine-owned content");

    RecordingRenderer renderer;
    canvas.setCustomRenderer(&renderer);
    const QImage visible = renderCanvas(canvas);

    const bool clearBackgroundEnabled = canvas.clearBackgroundEnabled();
    canvas.setCanvasContentVisible(false);
    canvas.setCanvasContentVisible(false);
    require(!canvas.canvasContentVisible(), "canvas content should be suppressible");
    require(canvas.clearBackgroundEnabled() == clearBackgroundEnabled,
            "content visibility should not change background clearing");
    const QImage hidden = renderCanvas(canvas);
    require(renderer.beforeCalls == 2 && renderer.afterCalls == 2,
            "custom renderer passes should run while canvas content is hidden");
    require(visible != hidden, "engine-owned scene and overlay content should be hidden");
    require(hidden.pixelColor(20, 20) == QColor(220, 30, 30),
            "before-canvas custom content should remain visible");
    require(hidden.pixelColor(2, 2) == QColor(20, 190, 70),
            "after-canvas custom content should remain visible");

    canvas.setCanvasContentVisible(true);
    require(canvas.canvasContentVisible(), "canvas content should restore");
    require(renderCanvas(canvas) == visible,
            "restoring visibility should render the retained canvas state");
    canvas.setCustomRenderer(nullptr);
}

void coalescedSceneRevisionsInvalidateEveryDirtyRegion() {
    SnowCanvasWidget canvas;
    canvas.resize(240, 120);
    require(canvas.setCanvasTool(SnowCanvasTool::Shape),
            "shape tool should activate for tile invalidation regression setup");
    sendMouseEvent(canvas, QEvent::MouseButtonPress, QPointF(20.0, 30.0), Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseMove, QPointF(60.0, 70.0), Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, QPointF(60.0, 70.0), Qt::LeftButton,
                   Qt::NoButton);

    renderCanvas(canvas);
    require(canvas.duplicateSelected(QPointF(55.0, 0.0)),
            "first duplicate should advance the scene without painting");
    require(canvas.duplicateSelected(QPointF(55.0, 0.0)),
            "second duplicate should advance the scene without painting");

    const QImage cached = renderCanvas(canvas);
    canvas.setCanvasContentVisible(false);
    canvas.setCanvasContentVisible(true);
    const QImage uncached = renderCanvas(canvas);
    require(
        cached == uncached,
        "coalesced scene revisions must not leave earlier dirty regions valid in the tile cache");
}

void rectangleStrokeStylesRenderDistinctPatterns() {
    const StrokeRunStats solid = renderRectangleTopEdge(SNOW_STROKE_STYLE_SOLID);
    const StrokeRunStats dashed = renderRectangleTopEdge(SNOW_STROKE_STYLE_DASHED);
    const StrokeRunStats dotted = renderRectangleTopEdge(SNOW_STROKE_STYLE_DOTTED);
    const StrokeRunStats roundedDashed =
        renderRectangleTopEdge(SNOW_STROKE_STYLE_DASHED, SnowCornerRadii{8.0, 8.0, 8.0, 8.0});

    require(solid.gapColumns == 0,
            "solid rectangle stroke should not contain gaps along its top edge");
    require(dashed.inkedColumns > 0 && dashed.gapColumns > 0,
            "dashed rectangle stroke should contain both ink and gaps");
    require(dotted.inkedColumns > 0 && dotted.gapColumns > 0,
            "dotted rectangle stroke should contain both ink and gaps");
    require(dotted.longestInkRun < dashed.longestInkRun,
            "dotted rectangle stroke should have shorter ink runs than dashed");
    require(roundedDashed.inkedColumns > 0 && roundedDashed.gapColumns > 0,
            "rounded dashed rectangle stroke should contain both ink and gaps");
}

void cornerRadiusCursorMatchesTheApprovedSvg() {
    constexpr int kCursorLogicalSize = 32;
    constexpr QPoint kCursorHotSpot(3, 3);

    const QCursor cursor = snow_canvas_input::cursorForSnowCursor(SNOW_CURSOR_STYLE_CORNER_RADIUS);

    const auto ref = snow::draw_engine::icons::cursor::CornerRadius();
    const auto metadata = adqt::icons::describeIcon(ref);
    require(metadata.key.pack == QStringLiteral("snow-draw-engine-qt") &&
                metadata.key.variant == QStringLiteral("cursor") &&
                metadata.key.name == QStringLiteral("corner-radius") &&
                metadata.colorModel == adqt::icons::IconColorModel::FullColor &&
                metadata.sourceHash ==
                    QByteArrayLiteral(
                        "5cef6b53d66e14f777a031bdceb885b51a56ef0d6fdfb8077d7ec69030d9f3df"),
            "corner radius cursor should retain its generated pack metadata");

    const QPixmap pixmap = cursor.pixmap();
    require(cursor.shape() == Qt::BitmapCursor, "corner radius cursor should use a custom pixmap");
    require(!pixmap.isNull(), "corner radius cursor pixmap should be available");
    require(cursor.hotSpot() == kCursorHotSpot,
            "corner radius cursor hotspot should match the scaled arrow tip");

    adqt::icons::IconRenderRequest request;
    request.logicalSize = QSize(kCursorLogicalSize, kCursorLogicalSize);
    request.devicePixelRatio = 1.0;
    const QPixmap expectedPixmap = adqt::icons::renderIconPixmap(ref, request);
    const QImage expectedPixmapImage =
        expectedPixmap.toImage().convertToFormat(QImage::Format_RGBA8888);
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGBA8888);
    require(image.size() == expectedPixmapImage.size(),
            "corner radius cursor pixmap size should match reference");
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            require(image.pixelColor(x, y) == expectedPixmapImage.pixelColor(x, y),
                    "corner radius cursor pixmap should match the reference pixel-for-pixel");
        }
    }
}

void cornerRadiusCursorUsesItsTargetDevicePixelRatio() {
    constexpr int kCursorLogicalSize = 32;
    constexpr qreal kDevicePixelRatio = 2.0;
    constexpr QPoint kCursorHotSpot(3, 3);

    const QCursor cursor =
        snow_canvas_input::cursorForSnowCursor(SNOW_CURSOR_STYLE_CORNER_RADIUS, kDevicePixelRatio);
    const QPixmap pixmap = cursor.pixmap();
    require(pixmap.size() == QSize(64, 64),
            "corner radius cursor should use physical pixels for its target device pixel ratio");
    require(qFuzzyCompare(pixmap.devicePixelRatio(), kDevicePixelRatio),
            "corner radius cursor should retain its target device pixel ratio");
    require(cursor.hotSpot() == kCursorHotSpot,
            "corner radius cursor hotspot should remain in logical pixels");

    adqt::icons::IconRenderRequest request;
    request.logicalSize = QSize(kCursorLogicalSize, kCursorLogicalSize);
    request.devicePixelRatio = kDevicePixelRatio;
    const QPixmap expectedPixmap =
        adqt::icons::renderIconPixmap(snow::draw_engine::icons::cursor::CornerRadius(), request);
    require(pixmap.toImage().convertToFormat(QImage::Format_RGBA8888) ==
                expectedPixmap.toImage().convertToFormat(QImage::Format_RGBA8888),
            "corner radius cursor should render at its physical pixel resolution");
}

QImage renderSerialToolbarIcon(const adqt::icons::IconRef& ref, const QColor& color) {
    adqt::icons::IconRenderRequest request;
    request.logicalSize = QSize(14, 14);
    request.devicePixelRatio = 1.0;
    return adqt::icons::renderIconPixmap(ref.withColors(adqt::icons::IconColors::primary(color)),
                                         request)
        .toImage();
}

void serialToolbarGlyphsMatchTheirApprovedGeometry() {
    const QColor color(29, 27, 32);
    const QImage decrease =
        renderSerialToolbarIcon(snow::draw_engine::icons::toolbar::SerialDecrease(), color);
    const QImage increase =
        renderSerialToolbarIcon(snow::draw_engine::icons::toolbar::SerialIncrease(), color);
    const QImage textFields =
        renderSerialToolbarIcon(snow::draw_engine::icons::toolbar::SerialTextFields(), color);

    require(!decrease.isNull() && decrease.pixelColor(4, 7).rgb() == color.rgb() &&
                decrease.pixelColor(7, 4).alpha() == 0,
            "serial decrease should retain the approved horizontal-minus geometry");
    require(!increase.isNull() && increase.pixelColor(4, 7).rgb() == color.rgb() &&
                increase.pixelColor(7, 4).rgb() == color.rgb(),
            "serial increase should retain the approved plus geometry");
    require(!textFields.isNull() && textFields.pixelColor(2, 3).rgb() == color.rgb() &&
                textFields.pixelColor(5, 10).rgb() == color.rgb() &&
                textFields.pixelColor(11, 7).rgb() == color.rgb() &&
                textFields.pixelColor(11, 10).rgb() == color.rgb(),
            "serial text-fields should retain the approved paired-letter geometry");

    const adqt::icons::IconPack* staticPack = snow::draw_engine::icons::pack().staticPack();
    require(staticPack != nullptr && staticPack->packName == "snow-draw-engine-qt" &&
                staticPack->entryCount == 4,
            "draw-engine pack should contain its cursor and three toolbar glyphs");
}

void rotationHandleCursorMatchesTheReferencePlatformBehavior() {
    const QCursor hoverCursor = snow_canvas_input::cursorForSnowCursor(SNOW_CURSOR_STYLE_GRAB);
    const QCursor draggingCursor =
        snow_canvas_input::cursorForSnowCursor(SNOW_CURSOR_STYLE_GRABBING);

#ifdef Q_OS_WIN
    require(hoverCursor.shape() == Qt::PointingHandCursor,
            "hovering the rotation handle should use the Windows pointing-hand cursor");
    require(draggingCursor.shape() == Qt::PointingHandCursor,
            "dragging the rotation handle should retain the Windows pointing-hand cursor");
#else
    require(hoverCursor.shape() == Qt::OpenHandCursor,
            "hovering the rotation handle should use the platform grab cursor");
    require(draggingCursor.shape() == Qt::ClosedHandCursor,
            "dragging the rotation handle should use the platform grabbing cursor");
#endif
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    rotationHandleCursorMatchesTheReferencePlatformBehavior();
    customRendererContractIsOrderedAndIsolated();
    runtimeExportUsesTheRequestedCanvasOrigin();
    canvasContentVisibilityPreservesCustomRenderingAndState();
    coalescedSceneRevisionsInvalidateEveryDirtyRegion();
    rectangleStrokeStylesRenderDistinctPatterns();
    highlightItemsRenderWithMultiplyBlendMode();
    ownedAxisAlignedFreeDrawChunksRenderWithoutRawGeometry();
    rectangleFastPathHonorsOpacity();
    cornerRadiusCursorMatchesTheApprovedSvg();
    cornerRadiusCursorUsesItsTargetDevicePixelRatio();
    serialToolbarGlyphsMatchTheirApprovedGeometry();
    return 0;
}

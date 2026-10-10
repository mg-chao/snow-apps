#include "snow_canvas_display_cache.h"
#include "snow_canvas_export.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_magnifier_renderer.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_viewport.h"
#include "snow_canvas_widget_selection_hit_testing.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QtMath>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <tuple>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

SceneDisplayInfo projection() {
    SceneDisplayInfo info{};
    info.surface_width = 200;
    info.surface_height = 140;
    info.camera_center_x = 100;
    info.camera_center_y = 70;
    info.camera_zoom = 1;
    return info;
}

SnowCanvasSceneItem magnifier(double factor = 2.0,
                              std::uint8_t shape = SNOW_DISPLAY_RECT_SHAPE_RECTANGLE) {
    SnowSceneDisplayItem item{};
    item.kind = SNOW_SCENE_DISPLAY_ITEM_MAGNIFIER;
    item.element_id = {1, 1};
    item.center_x = 130;
    item.center_y = 70;
    item.width = 30 * factor;
    item.height = 30 * factor;
    item.opacity = 1;
    item.rect_shape = shape;
    item.stroke = {30, 40, 50, 255};
    item.stroke_width = 2;
    item.magnifier = {35, 70, 30, 30, factor};
    return SnowCanvasSceneItem(item);
}

QImage sourceImage() {
    QImage image(200, 140, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(30, 90, 180));
    QPainter painter(&image);
    painter.fillRect(QRect(20, 55, 15, 15), QColor(20, 190, 80));
    painter.fillRect(QRect(35, 55, 15, 15), QColor(230, 180, 20));
    painter.fillRect(QRect(20, 70, 15, 15), QColor(210, 40, 80));
    painter.fillRect(QRect(35, 70, 15, 15), QColor(70, 30, 200));
    return image;
}

QImage render(const std::vector<SnowCanvasSceneItem>& items,
              const QList<SnowCanvasBaseImageSource>& sources, qreal dpr = 1.0,
              const SceneDisplayInfo& info = projection(), QRegion exposed = {}) {
    QImage output(QSize(qCeil(info.surface_width * dpr), qCeil(info.surface_height * dpr)),
                  QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(dpr);
    output.fill(Qt::transparent);
    if (exposed.isEmpty())
        exposed = QRegion(QRect(0, 0, qCeil(info.surface_width), qCeil(info.surface_height)));
    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRegion(exposed);
    snow_canvas_renderer::SceneRenderRequest request;
    request.painter = &painter;
    request.displayInfo = &info;
    request.sceneItems = items.data();
    request.sceneItemCount = static_cast<std::uint32_t>(items.size());
    request.exposedRegion = exposed;
    request.clearBackgroundEnabled = false;
    request.baseImageSources = &sources;
    snow_canvas_renderer::renderSceneItems(request);
    return output;
}

QColor pixel(const QImage& image, double x, double y) {
    return image.pixelColor(qFloor(x * image.devicePixelRatio()),
                            qFloor(y * image.devicePixelRatio()));
}

void samplesOnlyOriginalPixelsAtEveryFactorAndShape() {
    const QImage original = sourceImage();
    const QList<SnowCanvasBaseImageSource> sources{{original, QRectF(0, 0, 200, 140), {}}};
    for (const auto shape : {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                             SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
        for (const double factor : {1.0, 2.0, 10.0}) {
            auto lens = magnifier(factor, static_cast<std::uint8_t>(shape));
            lens.center_x = 100;
            auto annotation = lens;
            annotation.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
            annotation.element_id = {2, 1};
            annotation.center_x = 35;
            annotation.center_y = 70;
            annotation.width = 35;
            annotation.height = 35;
            annotation.fill = {250, 0, 0, 255};
            annotation.stroke.a = 0;
            annotation.rect_shape = SNOW_DISPLAY_RECT_SHAPE_RECTANGLE;
            for (const double dpr : {1.0, 1.25, 2.0}) {
                const QImage output = render({annotation, lens}, sources, dpr);
                require(pixel(output, 100 + factor * 3, 70 + factor * 3) == QColor(70, 30, 200),
                        "magnifiers must enlarge original pixels, excluding source annotations");
                require(original.cacheKey() == sources.front().image.cacheKey(),
                        "drawing a magnifier must preserve shared original image storage");
            }
        }
    }
}

void shapesAndBorderClipCleanly() {
    QImage blue(200, 140, QImage::Format_ARGB32_Premultiplied);
    blue.fill(Qt::blue);
    const QList<SnowCanvasBaseImageSource> sources{{blue, QRectF(0, 0, 200, 140), {}}};
    for (const auto shape : {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                             SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
        const auto lens = magnifier(2, static_cast<std::uint8_t>(shape));
        const QImage output = render({lens}, sources);
        require(pixel(output, 130, 70) == QColor(Qt::blue), "lens center must show sampled pixels");
        require((pixel(output, 154, 94).alpha() != 0) ==
                    (shape == SNOW_DISPLAY_RECT_SHAPE_RECTANGLE),
                "ellipse and diamond must clip their bounding rectangle corners");
        require(pixel(output, 130, 40) == QColor(30, 40, 50),
                "magnifier border must use the configured stroke color and width");
    }
    auto rotated = magnifier();
    rotated.width = 80;
    rotated.height = 20;
    rotated.rotation = std::acos(-1.0) / 2.0;
    const QImage output = render({rotated}, sources);
    require(pixel(output, 130, 95).alpha() != 0 && pixel(output, 155, 70).alpha() == 0,
            "lens clipping must rotate with its shape");
}

void roundedCornersClipSamplingAndStroke() {
    QImage blue(200, 140, QImage::Format_ARGB32_Premultiplied);
    blue.fill(Qt::blue);
    const QList<SnowCanvasBaseImageSource> sources{{blue, QRectF(0, 0, 200, 140), {}}};
    auto lens = magnifier();
    lens.corner_radii = {6, 12, 24, 0};
    for (double dpr : {1.0, 1.25, 2.0}) {
        const QImage output = render({lens}, sources, dpr);
        require(pixel(output, 100, 40).alpha() == 0 && pixel(output, 158, 41).alpha() == 0 &&
                    pixel(output, 158, 98).alpha() == 0,
                "rounded lens corners must clip sampled pixels and border at every DPR");
        require(pixel(output, 101, 98).alpha() != 0 && pixel(output, 130, 70) == QColor(Qt::blue),
                "a zero-radius corner stays square while the interior retains sampled pixels");
        require(pixel(output, 130, 40) == QColor(30, 40, 50),
                "rounding must preserve the configured lens border");
    }
    lens.rotation = std::acos(-1.0) / 2.0;
    const auto rotated = snow_canvas_magnifier_renderer::lensPath(lens);
    require(!rotated.contains(QPointF(159, 41)) && rotated.contains(QPointF(130, 70)),
            "rotation must transform the rounded clipping contour");
}

void outerBordersSurvivePartialAndOffViewportPainting() {
    const QList<SnowCanvasBaseImageSource> sources{{sourceImage(), QRectF(0, 0, 200, 140), {}}};
    for (const auto shape : {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                             SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
        for (const double zoom : {0.5, 1.0, 2.0}) {
            SceneDisplayInfo info = projection();
            info.camera_zoom = zoom;
            auto lens = magnifier(2.0, static_cast<std::uint8_t>(shape));
            lens.center_x = info.camera_center_x;
            lens.stroke_width = 10.0;
            const double top = info.surface_height / 2.0 - lens.height * zoom / 2.0;
            const QRegion exposed(QRect(95, qFloor(top - lens.stroke_width * zoom * 0.4), 10, 1));
            // Sample inside the exposed logical pixel. At fractional DPR its top
            // edge can map to a physical pixel above the rounded device clip.
            const double sampleY = exposed.boundingRect().top() + 0.5;
            for (const double dpr : {1.0, 1.25, 2.0}) {
                const QImage full = render({lens}, sources, dpr, info);
                snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
                const QImage partial = render({lens}, sources, dpr, info, exposed);
                require(pixel(full, 100, sampleY).alpha() != 0 &&
                            pixel(partial, 100, sampleY) == pixel(full, 100, sampleY),
                        "partial exposure outside the lens interior must retain its outer border");
                require(snow_canvas_magnifier_renderer::diagnosticsForCurrentThread()
                                .sourceLayerDrawCount == 0,
                        "border-only exposure must not sample the original image layers");

                auto outside = lens;
                outside.center_y =
                    info.camera_center_y -
                    (info.surface_height / 2.0 + lens.height * zoom / 2.0 + 1.0) / zoom;
                const QImage clipped = render({outside}, sources, dpr, info);
                require(pixel(clipped, 100, 0).alpha() != 0,
                        "a lens outside the viewport must paint its border extending inside it");
            }
        }
    }
}

void missingPixelsStayTransparentAndCoverageIsRespected() {
    const auto lens = magnifier();
    const QImage empty = render({lens}, {});
    require(pixel(empty, 130, 70).alpha() == 0 && pixel(empty, 130, 40).alpha() != 0,
            "without a base image the magnifier interior must remain transparent");
    QImage image(200, 140, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::green);
    const QImage partial =
        render({lens}, {{image, QRectF(0, 0, 200, 140), QRectF(20, 55, 15, 30)}});
    require(pixel(partial, 120, 70) == QColor(Qt::green) && pixel(partial, 140, 70).alpha() == 0,
            "magnifier sampling must respect layer coverage and transparent missing source pixels");
    QImage patch(15, 15, QImage::Format_ARGB32_Premultiplied);
    patch.fill(Qt::yellow);
    const QImage layered =
        render({lens}, {{image, QRectF(0, 0, 200, 140), {}}, {patch, QRectF(35, 70, 15, 15), {}}});
    require(pixel(layered, 140, 80) == QColor(Qt::yellow),
            "source layers including captured cursor patches must retain paint order");
}

void partialPaintAndProjectionRetainOriginalSampling() {
    const QImage original = sourceImage();
    const QList<SnowCanvasBaseImageSource> sources{{original, QRectF(0, 0, 200, 140), {}}};
    const auto lens = magnifier();
    const QRegion exposed(QRect(130, 70, 10, 10));
    const QImage partial = render({lens}, sources, 1, projection(), exposed);
    require(pixel(partial, 135, 75) == QColor(70, 30, 200) && pixel(partial, 125, 65).alpha() == 0,
            "partial repaint must sample the same pixels and stay within exposure");
    SceneDisplayInfo zoomed = projection();
    zoomed.camera_center_x = 130;
    zoomed.camera_zoom = 2;
    const QImage zoomedOutput = render({lens}, sources, 1.25, zoomed);
    require(pixel(zoomedOutput, 110, 80) == QColor(70, 30, 200),
            "camera zoom and DPR must transform the lens without resampling its annotation scene");
}

void leaderRenderingUsesEveryArrowheadAndSceneBounds() {
    auto lens = magnifier();
    const SnowArrowPoint points[]{{99, 70}, {35, 70}};
    lens.setArrowPoints(points, 2);
    lens.arrow_type = SNOW_ARROW_TYPE_STRAIGHT;
    lens.arrow_ratio = 1;
    lens.arrow_stroke_style = SNOW_STROKE_STYLE_SOLID;
    for (const auto head :
         {SNOW_ARROWHEAD_NONE, SNOW_ARROWHEAD_ARROW, SNOW_ARROWHEAD_BAR, SNOW_ARROWHEAD_DOT,
          SNOW_ARROWHEAD_CIRCLE, SNOW_ARROWHEAD_CIRCLE_OUTLINE, SNOW_ARROWHEAD_TRIANGLE,
          SNOW_ARROWHEAD_TRIANGLE_OUTLINE, SNOW_ARROWHEAD_DIAMOND, SNOW_ARROWHEAD_DIAMOND_OUTLINE,
          SNOW_ARROWHEAD_CROWFOOT_ONE, SNOW_ARROWHEAD_CROWFOOT_MANY,
          SNOW_ARROWHEAD_CROWFOOT_ONE_OR_MANY, SNOW_ARROWHEAD_INDENTED_TRIANGLE}) {
        lens.arrow_end_head = head;
        const QImage output = render({lens}, {});
        require(pixel(output, 70, 70).alpha() != 0,
                "each arrowhead must keep one straight magnifier leader");
    }
    const QImage croppedLeader =
        render({lens}, {}, 1, projection(), QRegion(QRect(65, 65, 10, 10)));
    require(pixel(croppedLeader, 70, 70).alpha() != 0,
            "magnifier culling bounds must include the leader outside its lens");
}

void samplingStaysPristineDuringFilterReplay() {
    auto annotation = magnifier();
    annotation.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    annotation.center_x = 35;
    annotation.width = 30;
    annotation.height = 30;
    annotation.fill = {255, 0, 0, 255};
    annotation.stroke.a = 0;
    auto filter = annotation;
    filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    filter.filter = snow_filter_render_spec_resolve(2, 1);
    const auto lens = magnifier();
    const QList<SnowCanvasBaseImageSource> sources{{sourceImage(), QRectF(0, 0, 200, 140), {}}};
    const QImage before = render({annotation, filter, lens}, sources);
    require(pixel(before, 140, 80) == QColor(70, 30, 200),
            "a magnifier replayed after filters must still sample pristine image layers");
    filter.center_x = 130;
    filter.width = 60;
    filter.height = 60;
    const QImage after = render({lens, filter}, sources);
    const QColor filtered = pixel(after, 140, 80);
    require(filtered.alpha() == 255 && filtered.red() == filtered.green() &&
                filtered.green() == filtered.blue(),
            "a later filter must process magnifier pixels in document paint order");
}

class OriginalImageRenderer final : public SnowCanvasCustomRenderer {
  public:
    QList<SnowCanvasBaseImageSource> baseImageSources() const override {
        return {{image, QRectF(0, 0, 200, 140), {}}};
    }
    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override {
        painter.drawImage(context.canvasToViewTransform.mapRect(QRectF(0, 0, 200, 140)), image);
    }
    QImage image = sourceImage();
};

void liveWidgetRetainsOriginalSourcesWithoutFilters() {
    SnowCanvasWidget canvas;
    canvas.resize(200, 140);
    OriginalImageRenderer renderer;
    canvas.setCustomRenderer(&renderer);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(100, 70, 1) && canvas.setCanvasTool(SnowCanvasTool::Magnifier),
            "configure live magnifier widget");
    for (const auto& [type, position, button, buttons] :
         {std::tuple{QEvent::MouseButtonPress, QPointF(20, 55), Qt::LeftButton,
                     Qt::MouseButtons(Qt::LeftButton)},
          std::tuple{QEvent::MouseMove, QPointF(50, 85), Qt::NoButton,
                     Qt::MouseButtons(Qt::LeftButton)},
          std::tuple{QEvent::MouseButtonRelease, QPointF(50, 85), Qt::LeftButton,
                     Qt::MouseButtons(Qt::NoButton)}}) {
        QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    }
    QImage output(200, 140, QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    canvas.render(&painter);
    painter.end();
    require(output.pixelColor(15, 50) == QColor(20, 190, 80),
            "the widget no-filter fast path must retain original image access for magnifiers");
    canvas.setCustomRenderer(nullptr);
}

void pointer(SnowRuntime runtime, SnowViewport viewport, SnowPointerEventType type,
             const QPointF& position) {
    SnowInputEvent event{};
    event.kind = SNOW_INPUT_EVENT_POINTER;
    event.pointer.pointer_id = 1;
    event.pointer.event_type = type;
    event.pointer.device = SNOW_POINTER_DEVICE_MOUSE;
    event.pointer.position_x = position.x();
    event.pointer.position_y = position.y();
    event.pointer.button =
        type == SNOW_POINTER_EVENT_MOVE ? SNOW_POINTER_BUTTON_NONE : SNOW_POINTER_BUTTON_PRIMARY;
    event.pointer.buttons = type == SNOW_POINTER_EVENT_UP ? 0 : 1;
    SnowInteractionOutput output{};
    ScopedChangedViewportList changed;
    require(snow_viewport_process_input_ex(runtime, viewport, &event, &output,
                                           changed.outParam()) == SNOW_OK,
            "magnifier export fixture input failed");
}

QImage renderGuide(const SnowOverlayDisplayItem& item, const OverlayDisplayInfo& info, qreal dpr,
                   bool cached, const QRegion& exposed = QRegion(QRect(0, 0, 200, 140))) {
    QImage output(QSize(qCeil(info.surface_width * dpr), qCeil(info.surface_height * dpr)),
                  QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(dpr);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRegion(exposed);
    if (cached) {
        const SnowCanvasOverlayItem owned(item);
        snow_canvas_renderer::renderOverlayItems(painter, info, &owned, 1, exposed);
    } else {
        snow_canvas_renderer::renderOverlayItems(painter, info, &item, 1, exposed);
    }
    return output;
}

double alphaWeight(const QImage& image) {
    double weight = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            weight += image.pixelColor(x, y).alphaF();
    return weight;
}

void creationAndSelectionGuidesAreDashedAtEveryShapeZoomAndDpr() {
    for (const auto shape : {SNOW_RECTANGLE_SHAPE_RECTANGLE, SNOW_RECTANGLE_SHAPE_ELLIPSE,
                             SNOW_RECTANGLE_SHAPE_DIAMOND}) {
        for (double zoom : {0.5, 1.0, 2.0}) {
            SnowStyleDefaults defaults{};
            require(snow_runtime_style_defaults_default(&defaults) == SNOW_OK, "load defaults");
            defaults.magnifier.shape = shape;
            defaults.magnifier.factor = 2;
            ScopedRuntimeHandle runtime;
            SnowRuntimeConfig config{&defaults};
            require(snow_runtime_create_with_config(&config, runtime.outParam()) == SNOW_OK,
                    "create guide runtime");
            SnowCanvasViewport viewport;
            require(viewport.create(runtime.get(), snow_canvas_viewport::defaultEngineConfig()) &&
                        snow_viewport_set_surface_size(runtime.get(), viewport.get(), 200, 140) ==
                            SNOW_OK &&
                        snow_viewport_set_camera(runtime.get(), viewport.get(), 100, 70, zoom) ==
                            SNOW_OK,
                    "configure guide viewport");
            ScopedChangedViewportList changed;
            require(snow_viewport_set_active_tool_ex(runtime.get(), viewport.get(),
                                                     SNOW_ACTIVE_TOOL_MAGNIFIER,
                                                     changed.outParam()) == SNOW_OK,
                    "activate guide fixture magnifier");
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_DOWN, {70, 55});
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_MOVE, {130, 85});
            SnowCanvasDisplayCache cache;
            require(cache.sync(runtime.get(), viewport.get()) && cache.sceneItemCount() == 1 &&
                        cache.overlayItemCount() == 1,
                    "creation must publish one lens and one source guide");
            const SnowCanvasOverlayItem creationGuide(cache.overlayItems()[0]);
            require(creationGuide.kind == SNOW_OVERLAY_DISPLAY_ITEM_FOCUS_CONNECTION &&
                        creationGuide.arrow_stroke_style == SNOW_STROKE_STYLE_DASHED &&
                        std::abs(creationGuide.stroke_width * zoom - 1) < 0.001,
                    "source guide must use a constant screen-width dashed contour");
            for (double dpr : {1.0, 1.25, 2.0}) {
                const QImage dashed = renderGuide(creationGuide, cache.overlayInfo(), dpr, true);
                auto solidGuide = static_cast<const SnowOverlayDisplayItem&>(creationGuide);
                solidGuide.arrow_stroke_style = SNOW_STROKE_STYLE_SOLID;
                const double solidWeight =
                    alphaWeight(renderGuide(solidGuide, cache.overlayInfo(), dpr, true));
                require(alphaWeight(dashed) > solidWeight * 0.35 &&
                            alphaWeight(dashed) < solidWeight * 0.9,
                        "source contours must have visible dashes and gaps at every shape and DPR");
            }
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_UP, {130, 85});
            require(cache.sync(runtime.get(), viewport.get()), "sync committed magnifier");
            const auto id = cache.sceneItems()[0].element_id;
            require(snow_viewport_set_active_tool_ex(runtime.get(), viewport.get(),
                                                     SNOW_ACTIVE_TOOL_SELECT,
                                                     changed.outParam()) == SNOW_OK &&
                        snow_viewport_select_element_ex(runtime.get(), viewport.get(), id,
                                                        changed.outParam()) == SNOW_OK &&
                        cache.sync(runtime.get(), viewport.get()),
                    "select guide fixture magnifier");
            const SnowCanvasOverlayItem* sourceGuide = nullptr;
            const SnowCanvasOverlayItem* frame = nullptr;
            for (std::uint32_t i = 0; i < cache.overlayItemCount(); ++i) {
                const auto& item = cache.overlayItems()[i];
                if (item.kind == SNOW_OVERLAY_DISPLAY_ITEM_FOCUS_CONNECTION)
                    sourceGuide = &item;
                if (item.kind == SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT &&
                    item.rect_kind == SNOW_OVERLAY_RECT_MAGNIFIER_SELECTION_FRAME)
                    frame = &item;
            }
            require(sourceGuide != nullptr && frame != nullptr,
                    "selection must publish both the source contour and dashed selectable frame");
            for (double rotation : {0.0, 0.4}) {
                auto rotatedFrame = static_cast<const SnowOverlayDisplayItem&>(*frame);
                rotatedFrame.rotation = rotation;
                auto standardDash = rotatedFrame;
                standardDash.rect_kind = SNOW_OVERLAY_RECT_SELECTION_MULTI_FRAME;
                auto solidFrame = rotatedFrame;
                solidFrame.rect_kind = SNOW_OVERLAY_RECT_SELECTION_FRAME;
                for (double dpr : {1.0, 1.25, 2.0}) {
                    for (bool cached : {false, true}) {
                        const QImage dashed =
                            renderGuide(rotatedFrame, cache.overlayInfo(), dpr, cached);
                        require(dashed ==
                                    renderGuide(standardDash, cache.overlayInfo(), dpr, cached),
                                "magnifier frames must match the existing canvas selection dashes");
                        require(alphaWeight(dashed) <
                                    alphaWeight(
                                        renderGuide(solidFrame, cache.overlayInfo(), dpr, cached)) *
                                        0.85,
                                "cached and direct frame rendering must retain visible dash gaps");
                        require(renderGuide(*sourceGuide, cache.overlayInfo(), dpr, cached) ==
                                    renderGuide(creationGuide, cache.overlayInfo(), dpr, cached),
                                "creation and selection must render the same source shape");
                        const QRegion exposed(QRect(80, 45, 40, 40));
                        const QImage partial =
                            renderGuide(rotatedFrame, cache.overlayInfo(), dpr, cached, exposed);
                        require(alphaWeight(partial) > 0 &&
                                    alphaWeight(partial) < alphaWeight(dashed) &&
                                    pixel(partial, 65, 50).alpha() == 0,
                                "partial frame repaints must respect the exposed region");
                        for (int y = 46; y < 84; ++y)
                            for (int x = 81; x < 119; ++x)
                                require(pixel(partial, x, y) == pixel(dashed, x, y),
                                        "exposed guide pixels must match a complete repaint");
                    }
                }
                const auto projection =
                    snow_canvas_render_geometry::overlayProjection(cache.overlayInfo());
                const QPointF center = snow_canvas_render_geometry::canvasToView(
                    projection, rotatedFrame.center_x, rotatedFrame.center_y);
                const double halfHeight = rotatedFrame.height * zoom / 2;
                const QPointF edge = center + QPointF(std::sin(rotation) * halfHeight,
                                                      -std::cos(rotation) * halfHeight);
                require(snow_canvas_widget_selection_hit_testing::selectionInteractionAtItems(
                            &rotatedFrame, 1, projection, edge) ==
                            snow_canvas_widget_selection_hit_testing::SelectionInteractionTarget::
                                Handle,
                        "dashed selection frames must retain their complete interactive border");
            }
            require(snow_viewport_reset_editing_state_ex(runtime.get(), viewport.get(),
                                                         changed.outParam()) == SNOW_OK &&
                        cache.sync(runtime.get(), viewport.get()) && cache.overlayItemCount() == 0,
                    "deselecting must remove magnifier guides from the overlay");
        }
    }
}

void exportUsesDirectOriginalSourcesIncludingOutsideCrop() {
    SnowStyleDefaults defaults{};
    require(snow_runtime_style_defaults_default(&defaults) == SNOW_OK, "load defaults");
    defaults.magnifier.factor = 2;
    defaults.magnifier.stroke_width = 2;
    defaults.magnifier.show_leader = 1;
    ScopedRuntimeHandle runtime;
    SnowRuntimeConfig config{&defaults};
    require(snow_runtime_create_with_config(&config, runtime.outParam()) == SNOW_OK,
            "create magnifier runtime");
    SnowCanvasViewport viewport;
    require(viewport.create(runtime.get(), snow_canvas_viewport::defaultEngineConfig()) &&
                snow_viewport_set_surface_size(runtime.get(), viewport.get(), 200, 140) ==
                    SNOW_OK &&
                snow_viewport_set_camera(runtime.get(), viewport.get(), 100, 70, 1) == SNOW_OK,
            "configure magnifier viewport");
    ScopedChangedViewportList changed;
    require(snow_viewport_set_active_tool_ex(runtime.get(), viewport.get(),
                                             SNOW_ACTIVE_TOOL_MAGNIFIER,
                                             changed.outParam()) == SNOW_OK,
            "activate magnifier");
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_DOWN, {20, 55});
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_MOVE, {50, 85});
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_UP, {50, 85});
    SnowCanvasDisplayCache cache;
    require(cache.sync(runtime.get(), viewport.get()) && cache.sceneItemCount() == 1,
            "creation must publish one magnifier scene item");
    const auto initial = cache.sceneItems()[0];
    require(initial.arrow_point_count == 0,
            "a concentric lens must omit its leader while containing the source center");
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_DOWN,
            {initial.center_x, initial.center_y});
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_UP,
            {initial.center_x, initial.center_y});
    const QPointF grip(initial.center_x, initial.center_y + initial.height / 2 + 24);
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_DOWN, grip);
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_MOVE, grip + QPointF(95, 0));
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_UP, grip + QPointF(95, 0));
    require(cache.sync(runtime.get(), viewport.get()), "sync independent lens drag");
    const auto moved = cache.sceneItems()[0];
    require(std::abs(moved.center_x - initial.center_x - 95) < 0.001 &&
                moved.magnifier.source_center_x == initial.magnifier.source_center_x,
            "destination grip must move only the magnified region");
    require(moved.arrow_point_count == 2,
            "a separated magnifier must publish one straight leader toward its source center");
    for (const double dpr : {1.0, 1.25, 2.0}) {
        const QImage output = render({moved}, {}, dpr);
        require(pixel(output, 95, 70).alpha() == 0 && pixel(output, 85, 70).alpha() != 0 &&
                    pixel(output, 100, 70).alpha() != 0,
                "magnifier leaders must leave a visible gap before the lens border at every DPR");
    }
    const QImage original = sourceImage();
    const QList<CanvasExportSource> sources{{original, QRectF(0, 0, 200, 140)}};
    snow_canvas_export::resetDiagnosticsForCurrentThread();
    const QImage exported = snow_canvas_export::renderToImage(
        runtime.get(), QRectF(100, 40, 60, 60), QSize(120, 120), sources);
    require(exported.pixelColor(70, 70) == QColor(70, 30, 200),
            "a cropped export must retain original sources outside its output crop");
    const auto diagnostics = snow_canvas_export::diagnosticsForCurrentThread();
    require(diagnostics.directSourceFastPathCount == 1 && diagnostics.fullCompositorPathCount == 0,
            "magnifier-only exports must avoid full background compositor allocations");
    defaults.magnifier.show_leader = 0;
    require(snow_viewport_set_magnifier_style_patch_ex(
                runtime.get(), viewport.get(), &defaults.magnifier,
                SNOW_MAGNIFIER_STYLE_PROPERTY_SHOW_LEADER, 0, changed.outParam()) == SNOW_OK &&
                cache.sync(runtime.get(), viewport.get()) &&
                cache.sceneItems()[0].arrow_point_count == 0,
            "disabling the leader must remove its scene geometry");
    const QImage noBase = snow_canvas_export::renderToImage(runtime.get(), QRectF(100, 40, 60, 60),
                                                            QSize(60, 60), {});
    require(noBase.pixelColor(30, 30).alpha() == 0,
            "export without base sources must preserve transparent lens content");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    creationAndSelectionGuidesAreDashedAtEveryShapeZoomAndDpr();
    samplesOnlyOriginalPixelsAtEveryFactorAndShape();
    shapesAndBorderClipCleanly();
    roundedCornersClipSamplingAndStroke();
    outerBordersSurvivePartialAndOffViewportPainting();
    missingPixelsStayTransparentAndCoverageIsRespected();
    partialPaintAndProjectionRetainOriginalSampling();
    leaderRenderingUsesEveryArrowheadAndSceneBounds();
    samplingStaysPristineDuringFilterReplay();
    liveWidgetRetainsOriginalSourcesWithoutFilters();
    exportUsesDirectOriginalSourcesIncludingOutsideCrop();
    return 0;
}

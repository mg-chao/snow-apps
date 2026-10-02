#pragma once

#include "snow_canvas_renderer.h"
#include "snow_canvas_pen_mask_atlas.h"
#include "snow_canvas_filter_tile_cache.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"

#include <QImage>
#include <QPainter>
#include <QtMath>
#include <cstdlib>
#include <iostream>

namespace {

inline void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

inline QImage pattern(const QSize& size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const int alpha = (x * 11 + y * 17) % 256;
            row[x] = qRgba((x * 7 + y) % (alpha + 1), (x + y * 5) % (alpha + 1),
                           (x * 13 + y * 3) % (alpha + 1), alpha);
        }
    }
    return image;
}

inline SnowCanvasSceneItem rectangle(const QRectF& bounds, std::uint32_t id,
                                     std::uint32_t filterType = 7) {
    SnowSceneDisplayItem item{};
    item.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    item.element_id = {id, 1};
    item.center_x = bounds.center().x();
    item.center_y = bounds.center().y();
    item.width = bounds.width();
    item.height = bounds.height();
    item.opacity = 1;
    item.filter = snow_filter_render_spec_resolve(filterType, 1.0);
    return SnowCanvasSceneItem(item);
}

inline SnowCanvasSceneItem annotation(const QRectF& bounds, std::uint32_t id,
                                      SnowColorRgba8 fill = {220, 30, 90, 255}) {
    SnowCanvasSceneItem item = rectangle(bounds, id);
    item.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    item.fill = fill;
    item.fill_style = SNOW_FILL_STYLE_SOLID;
    item.blend_mode = SNOW_BLEND_MODE_NORMAL;
    return item;
}

inline SnowCanvasSceneItem brush(const std::vector<SnowArrowPoint>& points, double width,
                                 std::uint32_t id) {
    auto item = rectangle({0, 0, 1, 1}, id);
    item.is_free_draw = 1;
    item.stroke_width = width;
    item.setArrowPoints(points.data(), static_cast<std::uint32_t>(points.size()));
    QRectF bounds;
    for (const auto& chunk : item.penSegmentChunks()) {
        bounds = bounds.isNull() ? chunk.canvasBounds : bounds.united(chunk.canvasBounds);
    }
    item.center_x = bounds.center().x();
    item.center_y = bounds.center().y();
    item.width = bounds.width();
    item.height = bounds.height();
    return item;
}

struct RenderFixture {
    QSize size;
    SceneDisplayInfo info;
    QImage background;
    snow_canvas_filter_render::RenderWorkspace workspace;
    snow_canvas_pen_mask::PenMaskAtlas atlas;
    snow_canvas_renderer::SceneExecutionPlan plan;
    std::vector<SnowCanvasSceneItem> items;
    SnowCanvasCustomRenderer* renderer = nullptr;
    bool clearBackground = false;
    std::uint64_t revision = 0;

    explicit RenderFixture(QSize logicalSize = {160, 120}, qreal dpr = 1.0)
        : size(logicalSize),
          background(pattern({qCeil(size.width() * dpr), qCeil(size.height() * dpr)})) {
        info.surface_width = size.width();
        info.surface_height = size.height();
        info.camera_center_x = size.width() / 2.0;
        info.camera_center_y = size.height() / 2.0;
        info.camera_zoom = 1;
        items.push_back(annotation(QRectF(QPointF(), QSizeF(size)), 1));
    }
    ~RenderFixture() {
        snow_canvas_filter_tile_cache::invalidateNamespace(this);
    }

    QImage render(qreal dpr = 1, bool tiled = false, QRegion exposed = {}, bool keepPlan = false) {
        QImage image({qCeil(size.width() * dpr), qCeil(size.height() * dpr)},
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(QColor(7, 11, 19));
        if (exposed.isEmpty()) {
            exposed = QRegion(QRect(QPoint(), size));
        }
        if (!keepPlan || plan.restorationForItem.size() != items.size() || plan.dpr != dpr) {
            snow_canvas_renderer::prepareExecutionPlan(
                plan, items.data(), static_cast<std::uint32_t>(items.size()), info, dpr,
                snow_canvas_renderer::buildUnculledRenderPlan(
                    items.data(), static_cast<std::uint32_t>(items.size()), info, dpr),
                ++revision);
        }
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setClipRegion(exposed);
        SnowCanvasRenderContext context{
            QRect(QPoint(), size), exposed,
            QTransform(info.camera_zoom, 0, 0, info.camera_zoom,
                       info.surface_width / 2.0 - info.camera_center_x * info.camera_zoom,
                       info.surface_height / 2.0 - info.camera_center_y * info.camera_zoom),
            dpr};
        snow_canvas_renderer::SceneRenderRequest request;
        request.painter = &painter;
        request.displayInfo = &info;
        request.sceneItems = items.data();
        request.sceneItemCount = static_cast<std::uint32_t>(items.size());
        request.exposedRegion = exposed;
        request.backgroundImage = background.isNull() ? nullptr : &background;
        request.backgroundRenderer = renderer;
        request.backgroundContext = &context;
        request.workspace = &workspace;
        request.penMaskAtlas = &atlas;
        request.cacheNamespace = this;
        request.clearBackgroundEnabled = clearBackground;
        request.executionPlan = &plan;
        if (tiled) {
            snow_canvas_renderer::renderSceneItemsTiled(request);
        } else {
            snow_canvas_renderer::renderSceneItems(request);
        }
        painter.end();
        return image;
    }
};

inline QRgb interpolate(QRgb current, QRgb original, int coverage) {
    const auto channel = [coverage](int a, int b) {
        return (a * (255 - coverage) + b * coverage + 127) / 255;
    };
    return qRgba(channel(qRed(current), qRed(original)), channel(qGreen(current), qGreen(original)),
                 channel(qBlue(current), qBlue(original)),
                 channel(qAlpha(current), qAlpha(original)));
}

} // namespace

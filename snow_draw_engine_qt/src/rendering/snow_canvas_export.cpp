#include "snow_canvas_export.h"

#include "snow_canvas_display_cache.h"
#include "snow_canvas_lifecycle.h"
#include "snow_canvas_pen_mask_atlas.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_spotlight_renderer.h"
#include "snow_canvas_state.h"
#include "snow_canvas_viewport.h"
#include "snow_draw_engine_qt/snow_transient_image.h"

#include <QPainter>
#include <QtMath>

#include <cstdint>
#include <cmath>

namespace snow_canvas_export {
namespace {

thread_local RenderDiagnostics g_renderDiagnostics;

struct ExportProjection {
    QRectF canvasRect;
    QSize outputSize;

    bool isValid() const {
        return canvasRect.isValid() && !canvasRect.isEmpty() && outputSize.width() > 0 &&
               outputSize.height() > 0;
    }

    double scaleX() const {
        return outputSize.width() / canvasRect.width();
    }

    double scaleY() const {
        return outputSize.height() / canvasRect.height();
    }

    QRectF mapCanvasRect(const QRectF& rect) const {
        const QRectF normalized = rect.normalized();
        return QRectF((normalized.left() - canvasRect.left()) * scaleX(),
                      (normalized.top() - canvasRect.top()) * scaleY(),
                      normalized.width() * scaleX(), normalized.height() * scaleY());
    }
};

std::uint32_t positiveCeil(double value) {
    return static_cast<std::uint32_t>(qMax(1, qCeil(value)));
}

SceneDisplayInfo sceneInfoForExport(const SceneDisplayInfo& base, const QRectF& canvasSourceRect) {
    SceneDisplayInfo sceneInfo = base;
    sceneInfo.surface_width = canvasSourceRect.width();
    sceneInfo.surface_height = canvasSourceRect.height();
    sceneInfo.camera_center_x = canvasSourceRect.center().x();
    sceneInfo.camera_center_y = canvasSourceRect.center().y();
    sceneInfo.camera_zoom = 1.0;
    return sceneInfo;
}

WatermarkDisplayInfo watermarkInfoForExport(const WatermarkDisplayInfo& base,
                                            const QRectF& canvasSourceRect) {
    WatermarkDisplayInfo watermarkInfo = base;
    watermarkInfo.surface_width = canvasSourceRect.width();
    watermarkInfo.surface_height = canvasSourceRect.height();
    watermarkInfo.camera_center_x = canvasSourceRect.center().x();
    watermarkInfo.camera_center_y = canvasSourceRect.center().y();
    watermarkInfo.camera_zoom = 1.0;
    return watermarkInfo;
}

void renderSources(QPainter& painter, const ExportProjection& projection,
                   const QList<CanvasExportSource>& sources) {
    for (const CanvasExportSource& source : sources) {
        if (source.image.isNull() || !source.canvasRect.isValid() || source.canvasRect.isEmpty()) {
            continue;
        }
        painter.drawImage(projection.mapCanvasRect(source.canvasRect), source.image);
    }
}

bool synchronizeRuntimeScene(SnowRuntime runtime, const ExportProjection& projection,
                             SnowCanvasViewport& viewport,
                             SnowCanvasDisplayCache& displayCache,
                             snow_canvas_state::Store& state) {
    if (runtime == nullptr) {
        return false;
    }
    if (!snow_canvas_lifecycle::initializeEngine(runtime, viewport, displayCache, state)
             .hasViewport) {
        return false;
    }

    const QSize surfaceSize(static_cast<int>(positiveCeil(projection.canvasRect.width())),
                            static_cast<int>(positiveCeil(projection.canvasRect.height())));
    if (!snow_canvas_lifecycle::setSurfaceSize(runtime, viewport, surfaceSize) ||
        !snow_canvas_lifecycle::setCamera(runtime, viewport, projection.canvasRect.center().x(),
                                          projection.canvasRect.center().y(), 1.0)) {
        return false;
    }
    if (!displayCache.sync(runtime, viewport.get())) {
        return false;
    }
    return true;
}

bool requiresFullCompositor(const SnowCanvasDisplayCache& displayCache) {
    const WatermarkDisplayInfo& watermark = displayCache.watermarkInfo();
    const bool visibleWatermark =
        watermark.watermark_text_len != 0 && watermark.watermark_color.a != 0 &&
        std::isfinite(watermark.watermark_opacity) && watermark.watermark_opacity > 0.0;
    return displayCache.sceneItemCount() != 0 || displayCache.overlayItemCount() != 0 ||
           displayCache.spotlightInfo().active || visibleWatermark;
}

void renderRuntimeScene(QPainter& painter, const ExportProjection& projection,
                        const QImage& background, SnowCanvasDisplayCache& displayCache) {
    const SceneDisplayInfo sceneInfo =
        sceneInfoForExport(displayCache.sceneInfo(), projection.canvasRect);
    const WatermarkDisplayInfo watermarkInfo =
        watermarkInfoForExport(displayCache.watermarkInfo(), projection.canvasRect);
    snow_canvas_filter_render::RenderWorkspace workspace(0);
    snow_canvas_pen_mask::PenMaskAtlas penMaskAtlas(0);
    painter.save();
    painter.scale(projection.scaleX(), projection.scaleY());
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter,
        &sceneInfo,
        displayCache.sceneItems(),
        displayCache.sceneItemCount(),
        QRegion(QRect(0, 0, static_cast<int>(positiveCeil(sceneInfo.surface_width)),
                      static_cast<int>(positiveCeil(sceneInfo.surface_height)))),
        nullptr,
        0,
        &background,
        nullptr,
        nullptr,
        &displayCache,
        &workspace,
        {},
        nullptr,
        &displayCache,
        &penMaskAtlas,
    });
    snow_canvas_spotlight_renderer::render(
        painter, sceneInfo, displayCache.spotlightInfo(), displayCache.spotlightCutouts(),
        displayCache.spotlightCutoutCount(), displayCache.spotlightGeometryGeneration(),
        &displayCache, QRectF(0.0, 0.0, sceneInfo.surface_width, sceneInfo.surface_height),
        painter.hasClipping()
            ? painter.clipRegion()
            : QRegion(QRect(0, 0, static_cast<int>(positiveCeil(sceneInfo.surface_width)),
                            static_cast<int>(positiveCeil(sceneInfo.surface_height)))),
        snow_canvas_spotlight_renderer::RenderPolicy::Direct);
    snow_canvas_renderer::WatermarkPatternRenderer::render(
        painter,
        snow_canvas_renderer::WatermarkRenderRequest{
            watermarkInfo,
            QRectF(0.0, 0.0, watermarkInfo.surface_width, watermarkInfo.surface_height),
            QRectF(0.0, 0.0, watermarkInfo.surface_width, watermarkInfo.surface_height),
            painter.hasClipping()
                ? painter.clipRegion()
                : QRegion(QRect(0, 0, static_cast<int>(positiveCeil(watermarkInfo.surface_width)),
                                static_cast<int>(positiveCeil(watermarkInfo.surface_height)))),
            painter.deviceTransform(),
            snow_canvas_renderer::WatermarkRenderPurpose::ImageExport,
        });
    painter.restore();
    workspace.finishFrame(true);
}

} // namespace

RenderDiagnostics diagnosticsForCurrentThread() {
    return g_renderDiagnostics;
}

void resetDiagnosticsForCurrentThread() {
    g_renderDiagnostics = {};
}

QImage renderToImage(SnowRuntime runtime, const QRectF& virtualSelectionRect,
                     const QSize& outputSize, const QList<CanvasExportSource>& sources) {
    const ExportProjection projection{virtualSelectionRect, outputSize};
    if (outputSize.width() <= 0 || outputSize.height() <= 0) {
        return {};
    }

    QImage output = snow_draw_engine_qt::allocateTransientImage(
        outputSize, QImage::Format_ARGB32_Premultiplied);
    if (output.isNull()) {
        return {};
    }
    output.fill(Qt::transparent);
    if (!projection.isValid()) {
        return output;
    }

    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    SnowCanvasViewport viewport;
    SnowCanvasDisplayCache displayCache;
    snow_canvas_state::Store state;
    const bool synchronized =
        synchronizeRuntimeScene(runtime, projection, viewport, displayCache, state);
    if (synchronized && !requiresFullCompositor(displayCache)) {
        ++g_renderDiagnostics.directSourceFastPathCount;
        renderSources(painter, projection, sources);
        return output;
    }

    const QSize sceneSize(static_cast<int>(positiveCeil(virtualSelectionRect.width())),
                          static_cast<int>(positiveCeil(virtualSelectionRect.height())));
    QImage background = snow_draw_engine_qt::allocateTransientImage(
        sceneSize, QImage::Format_ARGB32_Premultiplied);
    if (background.isNull()) {
        return {};
    }
    background.fill(Qt::transparent);
    {
        QPainter backgroundPainter(&background);
        backgroundPainter.setRenderHint(QPainter::Antialiasing, true);
        backgroundPainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderSources(backgroundPainter, ExportProjection{virtualSelectionRect, sceneSize},
                      sources);
    }
    if (synchronized) {
        ++g_renderDiagnostics.fullCompositorPathCount;
        renderRuntimeScene(painter, projection, background, displayCache);
    } else {
        ++g_renderDiagnostics.unsynchronizedFallbackCount;
        painter.drawImage(QRect(QPoint(0, 0), outputSize), background);
    }
    return output;
}

} // namespace snow_canvas_export

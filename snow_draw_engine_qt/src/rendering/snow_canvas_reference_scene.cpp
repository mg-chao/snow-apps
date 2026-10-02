#include "snow_canvas_reference_scene.h"

#include "snow_canvas_pen_mask_atlas.h"
#include "snow_canvas_filter_tile_cache.h"

#include <cmath>
#include <limits>

SnowCanvasReferenceScene::~SnowCanvasReferenceScene() {
    snow_canvas_filter_tile_cache::invalidateNamespace(this);
}

void SnowCanvasReferenceScene::clearRenderState() {
    m_image = {};
    m_backgroundRenderer = nullptr;
    m_displayCache.clearRenderState();
    m_penMasks.clear();
    snow_canvas_filter_tile_cache::invalidateNamespace(this);
}

void SnowCanvasReferenceScene::reset() {
    if (!m_viewport.isValid() && m_image.isNull()) {
        return;
    }
    clearRenderState();
    m_viewport.reset();
    m_displayCache.reset({255, 255, 255, 255});
    m_reference = {};
}

bool SnowCanvasReferenceScene::render(SnowRuntime runtime,
                                      const SnowCanvasFilterRenderReference& reference,
                                      const snow_canvas_renderer::SceneRenderRequest& request,
                                      const QTransform& canvasToView) {
    const QRectF bounds = reference.canvasRect;
    const qreal scale = reference.pixelsPerCanvasUnit;
    const double width = std::ceil(bounds.width() * scale);
    const double height = std::ceil(bounds.height() * scale);
    if (runtime == nullptr || request.painter == nullptr || !bounds.isValid() ||
        !std::isfinite(bounds.x()) || !std::isfinite(bounds.y()) || !std::isfinite(scale) ||
        scale <= 0 || !std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1 ||
        width >= std::numeric_limits<int>::max() || height >= std::numeric_limits<int>::max()) {
        reset();
        return false;
    }
    const QSize pixelSize(static_cast<int>(width), static_cast<int>(height));
    if (!m_viewport.isValid() || m_viewport.runtime() != runtime ||
        m_reference.canvasRect != bounds || m_reference.pixelsPerCanvasUnit != scale) {
        reset();
        auto config = snow_canvas_viewport::defaultEngineConfig();
        config.min_zoom = qMin(config.min_zoom, scale);
        config.max_zoom = qMax(config.max_zoom, scale);
        if (!m_viewport.create(runtime, config) ||
            snow_viewport_set_surface_size(
                runtime, m_viewport.get(), static_cast<std::uint32_t>(pixelSize.width()),
                static_cast<std::uint32_t>(pixelSize.height())) != SNOW_OK ||
            snow_viewport_set_camera(runtime, m_viewport.get(), bounds.x() + width / (2 * scale),
                                     bounds.y() + height / (2 * scale), scale) != SNOW_OK) {
            reset();
            return false;
        }
        m_reference = reference;
    }
    if (!m_displayCache.sync(runtime, m_viewport.get())) {
        return false;
    }
    if (m_displayCache.filterIndices().empty()) {
        clearRenderState();
        return false;
    }

    const auto sceneRevision = m_displayCache.patchCursor().scene_revision;
    const auto backgroundRevision =
        request.backgroundRenderer != nullptr ? request.backgroundRenderer->contentRevision() : 0;
    const bool rebuild = m_image.isNull() || m_sceneRevision != sceneRevision ||
                         m_backgroundRenderer != request.backgroundRenderer ||
                         m_backgroundRevision != backgroundRevision ||
                         m_clearBackgroundEnabled != request.clearBackgroundEnabled ||
                         m_font != request.painter->font() ||
                         m_renderHints != request.painter->renderHints();
    if (rebuild) {
        // Replace the retained surface while immutable stroke masks survive content edits.
        m_image = {};
        QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) {
            return false;
        }
        image.fill(request.clearBackgroundEnabled
                       ? snow_canvas_renderer::toQColor(m_displayCache.sceneInfo().clear_color)
                       : Qt::transparent);
        QPainter painter(&image);
        painter.setFont(request.painter->font());
        painter.setRenderHints(request.painter->renderHints());
        const QTransform sourceTransform(scale, 0, 0, scale, -bounds.x() * scale,
                                         -bounds.y() * scale);
        const SnowCanvasRenderContext context{image.rect(), QRegion(image.rect()), sourceTransform,
                                              1.0};
        snow_canvas_filter_render::RenderWorkspace workspace;
        auto sourceRequest = request;
        sourceRequest.painter = &painter;
        sourceRequest.displayInfo = &m_displayCache.sceneInfo();
        sourceRequest.sceneItems = m_displayCache.sceneItems();
        sourceRequest.sceneItemCount = m_displayCache.sceneItemCount();
        sourceRequest.exposedRegion = context.exposedRegion;
        sourceRequest.candidateIndices = nullptr;
        sourceRequest.candidateCount = 0;
        sourceRequest.backgroundContext = &context;
        sourceRequest.displayCache = &m_displayCache;
        sourceRequest.workspace = &workspace;
        sourceRequest.penMaskAtlas =
            request.penMaskAtlas != nullptr ? request.penMaskAtlas : &m_penMasks;
        sourceRequest.enableFilterTileCache = false;
        sourceRequest.enableOriginalBackgroundCache = true;
        sourceRequest.cacheNamespace = this;
        sourceRequest.executionPlan = nullptr;
        sourceRequest.renderPlan = nullptr;
        sourceRequest.diagnostics = nullptr;
        snow_canvas_renderer::renderSceneItems(sourceRequest);
        painter.end();
        m_displayCache.clearRenderState();
        m_image = std::move(image);
        m_sceneRevision = sceneRevision;
        m_backgroundRevision = backgroundRevision;
        m_backgroundRenderer = request.backgroundRenderer;
        m_clearBackgroundEnabled = request.clearBackgroundEnabled;
        m_font = request.painter->font();
        m_renderHints = request.painter->renderHints();
    }

    const QRectF rasterBounds(bounds.topLeft(), QSizeF(pixelSize) / scale);
    const QRectF target = canvasToView.mapRect(rasterBounds);
    QPainter& painter = *request.painter;
    painter.save();
    painter.setClipRegion(request.exposedRegion, Qt::IntersectClip);
    // Match pinned image sampling: pixel-exact at 1:1, linear at every other scale.
    const QRectF deviceTarget = painter.deviceTransform().mapRect(target);
    const QSize deviceSize(qRound(deviceTarget.width()), qRound(deviceTarget.height()));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, deviceSize != pixelSize);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    if (request.backgroundContext != nullptr) {
        painter.fillRect(
            request.backgroundContext->viewportRect,
            request.clearBackgroundEnabled
                ? snow_canvas_renderer::toQColor(m_displayCache.sceneInfo().clear_color)
                : Qt::transparent);
    }
    painter.drawImage(target, m_image);
    painter.restore();
    snow_canvas_renderer::recordReferenceScenePresentation(
        rebuild, static_cast<std::size_t>(m_image.sizeInBytes()));
    if (request.diagnostics != nullptr) {
        *request.diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    }
    return true;
}

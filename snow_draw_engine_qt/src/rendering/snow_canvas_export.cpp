#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_canvas_smart_erase.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "snow_canvas_export.h"

#include "snow_canvas_display_cache.h"
#include "snow_canvas_lifecycle.h"
#include "snow_canvas_pen_mask_atlas.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_compositor.h"
#include "snow_canvas_spotlight_renderer.h"
#include "snow_canvas_state.h"
#include "snow_canvas_viewport.h"

#include <QPainter>
#include <QtMath>

#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
#include <optional>

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

qreal rasterScaleForProjection(const ExportProjection& projection) {
    return qMax(1.0, qMin(projection.scaleX(), projection.scaleY()));
}

void configureExportPainter(QPainter& painter, qreal rasterScale = 1.0) {
    painter.scale(1.0 / rasterScale, 1.0 / rasterScale);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
}

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
                   const QList<CanvasExportSource>& sources, const QRect* exposedPixels = nullptr) {
    for (const CanvasExportSource& source : sources) {
        if (source.image.isNull() || !source.canvasRect.isValid() || source.canvasRect.isEmpty()) {
            continue;
        }
        const auto target = projection.mapCanvasRect(source.canvasRect);
        if (!exposedPixels || target.intersects(QRectF(*exposedPixels))) {
            painter.save();
            if (!source.coverage.isEmpty())
                painter.setClipRect(projection.mapCanvasRect(source.coverage), Qt::IntersectClip);
            painter.drawImage(target, source.image);
            painter.restore();
        }
    }
}

bool synchronizeRuntimeScene(SnowRuntime runtime, const ExportProjection& projection,
                             SnowCanvasViewport& viewport, SnowCanvasDisplayCache& displayCache,
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

bool requiresBackgroundRaster(const SnowCanvasDisplayCache& displayCache) {
    // Filters and restorations sample the original background. Ordinary scene
    // items and document decorations only paint over it and need no extra raster.
    for (std::uint32_t index = 0; index < displayCache.sceneItemCount(); ++index) {
        if (displayCache.sceneItems()[index].kind == SNOW_SCENE_DISPLAY_ITEM_FILTER)
            return true;
    }
    return false;
}

bool hasDocumentDrawing(const SnowCanvasDisplayCache& displayCache) {
    const WatermarkDisplayInfo& watermark = displayCache.watermarkInfo();
    return displayCache.sceneItemCount() != 0 ||
           snow_canvas_spotlight_renderer::isVisible(displayCache.spotlightInfo()) ||
           (watermark.watermark_text_len != 0 && watermark.watermark_color.a != 0 &&
            std::isfinite(watermark.watermark_opacity) && watermark.watermark_opacity > 0.0);
}

struct ExportScene {
    SceneDisplayInfo sceneInfo;
    WatermarkDisplayInfo watermarkInfo;
    snow_canvas_filter_render::RenderWorkspace workspace{0};
    snow_canvas_pen_mask::PenMaskAtlas penMaskAtlas{0};
    std::vector<SnowCanvasSceneItem> items;
    snow_canvas_renderer::SceneExecutionPlan executionPlan;

    ExportScene(const ExportProjection& projection, SnowCanvasDisplayCache& displayCache,
                const SnowCanvasSmartEraseSnapshot& smartErase, qreal devicePixelRatio)
        : sceneInfo(sceneInfoForExport(displayCache.sceneInfo(), projection.canvasRect)),
          watermarkInfo(
              watermarkInfoForExport(displayCache.watermarkInfo(), projection.canvasRect)) {
        items.reserve(displayCache.sceneItemCount());
        for (std::uint32_t i = 0; i < displayCache.sceneItemCount(); ++i)
            items.push_back(displayCache.sceneItems()[i]);
        snow_canvas_smart_erase::applySnapshot(items, smartErase);
        snow_canvas_renderer::prepareExecutionPlan(
            executionPlan, items.data(), static_cast<std::uint32_t>(items.size()), sceneInfo,
            devicePixelRatio, displayCache.renderPlan(), displayCache.patchCursor().scene_revision);
    }
};

void renderExportDecorations(QPainter& painter, const ExportScene& scene) {
    snow_canvas_compositor::Frame decorations;
    decorations.sceneInfo = &scene.sceneInfo;
    decorations.watermarkInfo = &scene.watermarkInfo;
    snow_canvas_compositor::renderDocumentDecorations(
        painter, decorations, snow_canvas_renderer::WatermarkRenderPurpose::ImageExport);
}

void renderRuntimeScene(QPainter& painter, const ExportProjection& projection,
                        const QImage& background, SnowCanvasDisplayCache& displayCache,
                        const SnowCanvasSmartEraseSnapshot& smartErase,
                        const QList<SnowCanvasBaseImageSource>& baseImageSources,
                        const QRegion* exposed = nullptr,
                        SnowCanvasCustomRenderer* backgroundRenderer = nullptr,
                        const SnowCanvasRenderContext* backgroundContext = nullptr,
                        ExportScene* preparedScene = nullptr) {
    std::optional<ExportScene> localScene;
    if (!preparedScene) {
        localScene.emplace(projection, displayCache, smartErase,
                           painter.device() ? qMax(1.0, painter.device()->devicePixelRatioF())
                                            : 1.0);
        preparedScene = &*localScene;
    }
    auto& sceneInfo = preparedScene->sceneInfo;
    auto& items = preparedScene->items;
    auto& workspace = preparedScene->workspace;
    auto& penMaskAtlas = preparedScene->penMaskAtlas;
    painter.save();
    painter.scale(projection.scaleX(), projection.scaleY());
    snow_canvas_renderer::SceneRenderRequest request{
        &painter,
        &sceneInfo,
        items.data(),
        static_cast<std::uint32_t>(items.size()),
        QRegion(QRect(0, 0, static_cast<int>(positiveCeil(sceneInfo.surface_width)),
                      static_cast<int>(positiveCeil(sceneInfo.surface_height)))),
        nullptr,
        0,
        &background,
        nullptr,
        nullptr,
        nullptr,
        &workspace,
        {},
        nullptr,
        &displayCache,
        &penMaskAtlas,
    };
    // Export sources are composed over transparency, including pixels restored by erasers.
    request.clearBackgroundEnabled = false;
    if (exposed)
        request.exposedRegion = *exposed;
    request.forceBackgroundRaster = backgroundRenderer && requiresBackgroundRaster(displayCache);
    request.backgroundRenderer = backgroundRenderer;
    request.backgroundContext = backgroundContext;
    request.smartErase = smartErase;
    request.executionPlan = &preparedScene->executionPlan;
    request.spotlightInfo = &displayCache.spotlightInfo();
    request.spotlightCutouts = displayCache.spotlightCutouts();
    request.spotlightCutoutCount = displayCache.spotlightCutoutCount();
    request.baseImageSources = &baseImageSources;
    snow_canvas_renderer::renderSceneItems(request);
    if (!exposed)
        renderExportDecorations(painter, *preparedScene);
    painter.restore();
    g_renderDiagnostics.peakScratchPoolBytes =
        std::max(g_renderDiagnostics.peakScratchPoolBytes, workspace.diagnostics().peakPoolBytes);
    workspace.finishFrame(true);
}

} // namespace

RenderDiagnostics diagnosticsForCurrentThread() {
    return g_renderDiagnostics;
}

void resetDiagnosticsForCurrentThread() {
    g_renderDiagnostics = {};
}

QImage renderImageSourcesToImage(const QRectF& canvasRect, const QSize& outputSize,
                                 const QList<CanvasExportSource>& sources) {
    const ExportProjection projection{canvasRect, outputSize};
    if (!projection.isValid())
        return {};
    QImage output = snowCanvasAllocateImage(outputSize, QImage::Format_ARGB32_Premultiplied);
    if (output.isNull())
        return {};
    output.fill(Qt::transparent);
    const qreal rasterScale = rasterScaleForProjection(projection);
    output.setDevicePixelRatio(rasterScale);
    QPainter painter(&output);
    configureExportPainter(painter, rasterScale);
    renderSources(painter, projection, sources);
    painter.end();
    output.setDevicePixelRatio(1.0);
    return output;
}

QImage renderImageSourcesToImage(const QRectF& canvasRect, const QSize& outputSize,
                                 const QList<CanvasExportSource>& sources,
                                 const QRect& outputPixelRect) {
    const ExportProjection projection{canvasRect, outputSize};
    if (!projection.isValid() || outputPixelRect.isEmpty())
        return {};
    QImage output =
        snowCanvasAllocateImage(outputPixelRect.size(), QImage::Format_ARGB32_Premultiplied);
    if (output.isNull())
        return {};
    output.fill(Qt::transparent);
    const QRect visible = outputPixelRect.intersected(QRect(QPoint{}, outputSize));
    // Keep the original horizontal span origin: clipping it can alter Qt's fixed-point
    // texture stepping. Integer vertical translation preserves the full-render sample phase.
    const int bandHeight = static_cast<int>(
        std::max(qint64(1), qint64(8) * 1024 * 1024 / (qint64(outputSize.width()) * 4)));
    for (int top = visible.top(); top <= visible.bottom(); top += bandHeight) {
        const int height = std::min(bandHeight, visible.bottom() - top + 1);
        QImage band = snowCanvasAllocateImage(QSize(outputSize.width(), height),
                                              QImage::Format_ARGB32_Premultiplied);
        if (band.isNull())
            return {};
        band.fill(Qt::transparent);
        QPainter painter(&band);
        configureExportPainter(painter);
        painter.translate(0, -top);
        renderSources(painter, projection, sources);
        painter.end();
        for (int y = 0; y < height; ++y)
            std::memcpy(output.scanLine(top + y - outputPixelRect.y()) +
                            (visible.x() - outputPixelRect.x()) * 4,
                        band.constScanLine(y) + visible.x() * 4,
                        static_cast<std::size_t>(visible.width()) * 4);
    }
    return output;
}

QImage renderToImage(SnowRuntime runtime, const QRectF& virtualSelectionRect,
                     const QSize& outputSize, const QList<CanvasExportSource>& sources,
                     const SnowCanvasSmartEraseSnapshot& smartErase,
                     const QImage& preparedBackground,
                     const SnowCanvasExportBackgroundOverlay& backgroundOverlay) {
    const ExportProjection projection{virtualSelectionRect, outputSize};
    if (outputSize.width() <= 0 || outputSize.height() <= 0 ||
        (!preparedBackground.isNull() && preparedBackground.size() != outputSize)) {
        return {};
    }

    QImage output = snowCanvasAllocateImage(outputSize, QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    if (!projection.isValid()) {
        return output;
    }

    // Keep compositor scratch surfaces at export resolution. The temporary DPR
    // describes rasterization only; exported images always carry DPR 1.
    const qreal rasterScale = rasterScaleForProjection(projection);
    output.setDevicePixelRatio(rasterScale);
    QPainter painter(&output);
    configureExportPainter(painter, rasterScale);
    SnowCanvasViewport viewport;
    SnowCanvasDisplayCache displayCache;
    snow_canvas_state::Store state;
    QList<SnowCanvasBaseImageSource> baseImageSources;
    baseImageSources.reserve(sources.size());
    for (const auto& source : sources)
        baseImageSources.append({source.image, source.canvasRect, source.coverage});
    const bool synchronized =
        synchronizeRuntimeScene(runtime, projection, viewport, displayCache, state);
    const auto paintOverlay = [&](QPainter& target) {
        if (!backgroundOverlay)
            return true;
        target.save();
        target.setClipRect(QRect(QPoint(), outputSize), Qt::IntersectClip);
        const QTransform canvasToPixels(projection.scaleX(), 0, 0, projection.scaleY(),
                                        -virtualSelectionRect.x() * projection.scaleX(),
                                        -virtualSelectionRect.y() * projection.scaleY());
        const bool painted = backgroundOverlay(target, canvasToPixels);
        target.restore();
        return painted;
    };
    if (!synchronized || (!requiresBackgroundRaster(displayCache) &&
                          !snow_canvas_smart_erase::hasItems(smartErase))) {
        if (preparedBackground.isNull())
            renderSources(painter, projection, sources);
        else
            painter.drawImage(QRect(QPoint(), outputSize), preparedBackground);
        if (!paintOverlay(painter))
            return {};
        if (backgroundOverlay)
            ++g_renderDiagnostics.directBackgroundOverlayCount;
        if (synchronized) {
            ++g_renderDiagnostics.directSourceFastPathCount;
            if (hasDocumentDrawing(displayCache))
                renderRuntimeScene(painter, projection, {}, displayCache, smartErase,
                                   baseImageSources);
        } else {
            ++g_renderDiagnostics.unsynchronizedFallbackCount;
        }
        painter.end();
        output.setDevicePixelRatio(1.0);
        return output;
    }

    const QSize sceneSize = outputSize;
    QImage background = preparedBackground;
    if (background.isNull()) {
        background = renderImageSourcesToImage(virtualSelectionRect, sceneSize, sources);
    }
    if (backgroundOverlay) {
        // Filters need a complete composed backdrop; ordinary drawing uses the output
        // directly above. Sampling sources remain pristine in both paths.
        background.setDevicePixelRatio(1.0);
        QPainter backgroundPainter(&background);
        if (!backgroundPainter.isActive() || !paintOverlay(backgroundPainter))
            return {};
        g_renderDiagnostics.backgroundOverlayScratchBytes =
            std::max(g_renderDiagnostics.backgroundOverlayScratchBytes,
                     static_cast<std::size_t>(background.sizeInBytes()));
    }
    if (synchronized) {
        ++g_renderDiagnostics.fullCompositorPathCount;
        renderRuntimeScene(painter, projection, background, displayCache, smartErase,
                           baseImageSources);
    } else {
        ++g_renderDiagnostics.unsynchronizedFallbackCount;
        painter.drawImage(QRect(QPoint(0, 0), outputSize), background);
    }
    painter.end();
    output.setDevicePixelRatio(1.0);
    return output;
}

// Keep filters' source replay in the complete source frame. The renderer requests
// its own bounded sampling halo, so no raster of the discarded gaps is needed.
class ExportSourceRenderer final : public SnowCanvasCustomRenderer {
  public:
    ExportSourceRenderer(const QList<CanvasExportSource>& sources,
                         const ExportProjection& projection, bool sampled, const QImage& background,
                         const QList<CanvasExportTile>& tiles)
        : m_sources(sources), m_projection(projection), m_sampled(sampled),
          m_background(background), m_tiles(tiles) {}
    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override {
        if (!m_background.isNull()) {
            // Project the compact backdrop back into the original scene frame,
            // including neighboring retained tiles needed by filter sampling halos.
            for (const auto& tile : m_tiles) {
                const QRectF target(tile.sourcePixels.x() / m_projection.scaleX(),
                                    tile.sourcePixels.y() / m_projection.scaleY(),
                                    tile.sourcePixels.width() / m_projection.scaleX(),
                                    tile.sourcePixels.height() / m_projection.scaleY());
                if (context.exposedRegion.intersects(target.toAlignedRect()))
                    painter.drawImage(target, m_background,
                                      QRect(tile.destination, tile.sourcePixels.size()));
            }
            return;
        }
        if (m_sampled) {
            // Match the normal export's pre-scaled background, including its
            // transparent frame boundary. Render only the requested sampling halo.
            const QRectF exposed =
                QRectF(context.exposedRegion.boundingRect())
                    .intersected(QRectF(QPointF(), m_projection.canvasRect.size()));
            if (exposed.isEmpty())
                return;
            const QRect pixels =
                QRectF(exposed.x() * m_projection.scaleX(), exposed.y() * m_projection.scaleY(),
                       exposed.width() * m_projection.scaleX(),
                       exposed.height() * m_projection.scaleY())
                    .toAlignedRect()
                    .adjusted(-1, -1, 1, 1)
                    .intersected(QRect(QPoint(), m_projection.outputSize));
            if (pixels.isEmpty())
                return;
            if (qint64(pixels.width()) * pixels.height() > 100000000) {
                failed = true;
                return;
            }
            QImage raster =
                snowCanvasAllocateZeroedImage(pixels.size(), QImage::Format_ARGB32_Premultiplied);
            if (raster.isNull()) {
                failed = true;
                return;
            }
            g_renderDiagnostics.peakSourceTileBytes =
                std::max(g_renderDiagnostics.peakSourceTileBytes,
                         static_cast<std::size_t>(raster.sizeInBytes()));
            QPainter sourcePainter(&raster);
            configureExportPainter(sourcePainter);
            sourcePainter.translate(-pixels.topLeft());
            renderSources(sourcePainter, m_projection, m_sources, &pixels);
            sourcePainter.end();
            painter.drawImage(QRectF(pixels.x() / m_projection.scaleX(),
                                     pixels.y() / m_projection.scaleY(),
                                     pixels.width() / m_projection.scaleX(),
                                     pixels.height() / m_projection.scaleY()),
                              raster);
            return;
        }
        for (const auto& source : m_sources) {
            const auto target = source.canvasRect.translated(-m_projection.canvasRect.topLeft());
            if (!source.image.isNull() &&
                context.exposedRegion.intersects(target.toAlignedRect())) {
                painter.save();
                if (!source.coverage.isEmpty())
                    painter.setClipRect(
                        source.coverage.translated(-m_projection.canvasRect.topLeft()),
                        Qt::IntersectClip);
                painter.drawImage(target, source.image);
                painter.restore();
            }
        }
    }
    bool failed = false;

  private:
    const QList<CanvasExportSource>& m_sources;
    const ExportProjection& m_projection;
    bool m_sampled;
    const QImage& m_background;
    const QList<CanvasExportTile>& m_tiles;
};

QImage renderTilesToImage(SnowRuntime runtime, const QRectF& sourceFrame,
                          const QSize& sourcePixelSize, const QSize& outputSize,
                          const QList<CanvasExportTile>& tiles,
                          const QList<CanvasExportSource>& sources,
                          const SnowCanvasSmartEraseSnapshot& smartErase,
                          const QImage& preparedBackground) {
    const ExportProjection projection{sourceFrame, sourcePixelSize};
    if (!projection.isValid() || outputSize.isEmpty() || tiles.isEmpty() ||
        (!preparedBackground.isNull() && preparedBackground.size() != outputSize) ||
        !std::isfinite(sourceFrame.x()) || !std::isfinite(sourceFrame.y()) ||
        !std::isfinite(sourceFrame.width()) || !std::isfinite(sourceFrame.height()) ||
        sourceFrame.width() > std::numeric_limits<int>::max() ||
        sourceFrame.height() > std::numeric_limits<int>::max() ||
        !std::isfinite(projection.scaleX()) || !std::isfinite(projection.scaleY()) ||
        projection.scaleX() > 256 || projection.scaleY() > 256 ||
        qint64(outputSize.width()) * outputSize.height() > std::numeric_limits<int>::max() / 4)
        return {};
    for (const auto& tile : tiles) {
        const auto size = tile.sourcePixels.size();
        // Subtract dimensions before adding coordinates, avoiding QRect endpoint overflow.
        if (size.isEmpty() || tile.sourcePixels.x() < 0 || tile.sourcePixels.y() < 0 ||
            tile.sourcePixels.x() > sourcePixelSize.width() - size.width() ||
            tile.sourcePixels.y() > sourcePixelSize.height() - size.height() ||
            tile.destination.x() < 0 || tile.destination.y() < 0 ||
            tile.destination.x() > outputSize.width() - size.width() ||
            tile.destination.y() > outputSize.height() - size.height())
            return {};
    }
    QImage output = snowCanvasAllocateZeroedImage(outputSize, QImage::Format_ARGB32_Premultiplied);
    if (output.isNull())
        return {};
    // Spacing has no source tile, but still retains its prepared background.
    if (!preparedBackground.isNull()) {
        QPainter backgroundPainter(&output);
        backgroundPainter.drawImage(output.rect(), preparedBackground);
    }
    const qreal rasterScale = rasterScaleForProjection(projection);
    output.setDevicePixelRatio(rasterScale);
    SnowCanvasViewport viewport;
    SnowCanvasDisplayCache displayCache;
    snow_canvas_state::Store state;
    const bool hasContent = snow_runtime_has_document_content(runtime) != 0 ||
                            snow_canvas_smart_erase::hasItems(smartErase);
    // Empty documents need no viewport or scene plan. In particular, sparse
    // source positions must not expand the engine's visible spatial query.
    if (hasContent && !synchronizeRuntimeScene(runtime, projection, viewport, displayCache, state))
        return {};
    QList<SnowCanvasBaseImageSource> baseSources;
    for (const auto& source : sources)
        baseSources.append({source.image, source.canvasRect, source.coverage});
    const bool boundedScratch =
        requiresBackgroundRaster(displayCache) || snow_canvas_smart_erase::hasItems(smartErase);
    ExportSourceRenderer background(sources, projection, boundedScratch, preparedBackground, tiles);
    SnowCanvasRenderContext context;
    context.viewportRect = QRect(QPoint(), sourceFrame.size().toSize());
    context.canvasToViewTransform.translate(-sourceFrame.x(), -sourceFrame.y());
    context.devicePixelRatio = rasterScale;
    QPainter painter(&output);
    configureExportPainter(painter, rasterScale);
    const bool drawing =
        hasDocumentDrawing(displayCache) || snow_canvas_smart_erase::hasItems(smartErase);
    std::optional<ExportScene> scene;
    if (drawing)
        scene.emplace(projection, displayCache, smartErase, rasterScale);
    g_renderDiagnostics.outputRasterBytes = static_cast<std::size_t>(output.sizeInBytes());
    for (const auto& tile : tiles) {
        // Bound a single exposed region even when one occupied band is very large.
        const int edge =
            boundedScratch ? 512 : std::max(tile.sourcePixels.width(), tile.sourcePixels.height());
        for (int y = 0; y < tile.sourcePixels.height(); y += edge) {
            for (int x = 0; x < tile.sourcePixels.width(); x += edge) {
                const QRect source(tile.sourcePixels.topLeft() + QPoint(x, y),
                                   QSize(qMin(edge, tile.sourcePixels.width() - x),
                                         qMin(edge, tile.sourcePixels.height() - y)));
                const QPoint destination = tile.destination + QPoint(x, y);
                painter.save();
                painter.setClipRect(QRect(destination, source.size()));
                painter.translate(destination - source.topLeft());
                if (drawing) {
                    if (!preparedBackground.isNull()) {
                        // renderRuntimeScene paints the background itself. Clear this
                        // tile first so translucent backdrop pixels are not doubled.
                        painter.save();
                        painter.setCompositionMode(QPainter::CompositionMode_Source);
                        painter.fillRect(source, Qt::transparent);
                        painter.restore();
                    }
                    const QRectF logical(source.x() / projection.scaleX(),
                                         source.y() / projection.scaleY(),
                                         source.width() / projection.scaleX(),
                                         source.height() / projection.scaleY());
                    context.exposedRegion = QRegion(logical.toAlignedRect());
                    renderRuntimeScene(painter, projection, {}, displayCache, smartErase,
                                       baseSources, &context.exposedRegion, &background, &context,
                                       &*scene);
                } else if (preparedBackground.isNull()) {
                    renderSources(painter, projection, sources, &source);
                }
                painter.restore();
            }
        }
        if (drawing) {
            // Decorations retain the complete retained span. Subdividing a
            // rotated watermark introduces extra SIMD interpolation tails.
            painter.save();
            painter.setClipRect(QRect(tile.destination, tile.sourcePixels.size()));
            painter.translate(tile.destination - tile.sourcePixels.topLeft());
            painter.scale(projection.scaleX(), projection.scaleY());
            renderExportDecorations(painter, *scene);
            painter.restore();
        }
    }
    painter.end();
    output.setDevicePixelRatio(1.0);
    return background.failed ? QImage{} : output;
}

} // namespace snow_canvas_export

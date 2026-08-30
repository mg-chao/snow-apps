#pragma once

#include "snow_canvas_display_cache.h"
#include "snow_canvas_filter_render.h"
#include "snow_canvas_watermark_renderer.h"
#include "snow_draw_engine.h"

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRegion>

#include <cstdint>
#include <cstddef>

class QPainter;
class SnowCanvasCustomRenderer;
struct SnowCanvasRenderContext;
namespace snow_canvas_pen_mask {
class PenMaskAtlas;
}

namespace snow_canvas_renderer {

struct FilterRenderDiagnostics {
    bool usedFilterPath = false;
    std::size_t exposedPixelCount = 0;
    std::size_t totalWorkingPixelCount = 0;
    std::size_t peakWorkingPixelCount = 0;
    std::size_t surfaceComponentCount = 0;
    std::size_t spatialCandidateCount = 0;
    std::size_t replayedItemCount = 0;
    std::size_t filterLayerCount = 0;
    std::size_t originalFilterCount = 0;
    std::size_t effectDispatchCount = 0;
    std::size_t batchedFilterCount = 0;
    std::size_t maskPixelCount = 0;
    std::size_t maskBoundingPixelCount = 0;
    std::size_t maskCoveredPixelCount = 0;
    std::size_t sparseDispatchCount = 0;
    std::size_t denseDispatchCount = 0;
    std::size_t spatialEffectGroupCount = 0;
    std::size_t penGeometryChunkBuildCount = 0;
    std::size_t penGeometryChunkReuseCount = 0;
    std::size_t penQueriedChunkCount = 0;
    std::size_t penCulledChunkCount = 0;
    std::size_t penRasterizedTileCount = 0;
    std::size_t penRasterizedPixelCount = 0;
    std::size_t penAtlasHits = 0;
    std::size_t penAtlasMisses = 0;
    std::size_t penAtlasEvictions = 0;
    std::size_t penAtlasReusedAfterPatch = 0;
    std::size_t penSimdRasterExecutions = 0;
    std::size_t retainedPenAtlasBytes = 0;
    std::size_t allocatedBytes = 0;
    std::size_t copiedBytes = 0;
    std::size_t scratchReuseCount = 0;
    std::size_t tileHits = 0;
    std::size_t tileMisses = 0;
    std::size_t tileEvictions = 0;
    std::size_t tileCandidates = 0;
    std::size_t tileVisits = 0;
    std::size_t sourceTileHits = 0;
    std::size_t sourceTileMisses = 0;
    std::size_t sourceTileEvictions = 0;
    std::size_t sourceTileCandidates = 0;
    std::size_t sourceTileVisits = 0;
    std::size_t sourceDependencyInvalidations = 0;
    std::size_t sourceMergedNodes = 0;
    std::size_t sourceOverlappingNodes = 0;
    std::size_t retainedSourceBytes = 0;
    std::size_t maskCacheHits = 0;
    std::size_t maskCacheMisses = 0;
    std::size_t maskCacheEvictions = 0;
    std::size_t retainedMaskBytes = 0;
    std::size_t parallelJobs = 0;
    std::size_t retainedWorkspaceBytes = 0;
    std::size_t gaussianPasses = 0;
    std::size_t gaussianDownsampleAvx2Executions = 0;
    std::size_t gaussianReconstructionAvx2Executions = 0;
    std::size_t opaqueRectDispatchCount = 0;
    std::size_t constantOpacityRectDispatchCount = 0;
    std::uint64_t sceneReplayNanoseconds = 0;
    std::uint64_t maskConstructionNanoseconds = 0;
    std::uint64_t pathConstructionNanoseconds = 0;
    std::uint64_t maskScanNanoseconds = 0;
    std::uint64_t downsampleNanoseconds = 0;
    std::uint64_t reducedBlurNanoseconds = 0;
    std::uint64_t reconstructionNanoseconds = 0;
    std::uint64_t presentationNanoseconds = 0;
    const char* simdBackend = "scalar";
    // Retained aliases for existing diagnostics consumers.
    std::size_t workingSurfacePixelCount = 0;
    std::size_t peakEffectPixelCount = 0;
    std::size_t recorderCount = 0;
    std::size_t layerCount = 0;
    std::size_t filterPassCount = 0;
    std::size_t batchCount = 0;
    std::size_t batchCacheHits = 0;
    std::size_t batchCacheMisses = 0;
};

struct SceneRenderRequest {
    QPainter* painter = nullptr;
    const SceneDisplayInfo* displayInfo = nullptr;
    const SnowCanvasSceneItem* sceneItems = nullptr;
    std::uint32_t sceneItemCount = 0;
    QRegion exposedRegion;
    const std::uint32_t* candidateIndices = nullptr;
    std::uint32_t candidateCount = 0;
    const QImage* backgroundImage = nullptr;
    SnowCanvasCustomRenderer* backgroundRenderer = nullptr;
    const SnowCanvasRenderContext* backgroundContext = nullptr;
    const SnowCanvasDisplayCache* displayCache = nullptr;
    snow_canvas_filter_render::RenderWorkspace* workspace = nullptr;
    snow_canvas_filter_render::ExecutionOptions execution;
    FilterRenderDiagnostics* diagnostics = nullptr;
    const void* cacheNamespace = nullptr;
    snow_canvas_pen_mask::PenMaskAtlas* penMaskAtlas = nullptr;
    // The widget uses the tiled entry point to enable filter-source snapshots.
    // Direct renderer callers keep the uncached path by leaving this disabled.
    bool enableFilterTileCache = false;
    std::uint64_t filterTileContentKey = 0;
    QPoint filterTileCoordinate;
    bool clearBackgroundEnabled = true;
};

QColor toQColor(const SnowColorRgba8& color);
std::size_t hatchTextureCacheEntryCountForCurrentThread();
FilterRenderDiagnostics filterRenderDiagnosticsForCurrentThread();
void resetFilterRenderDiagnosticsForCurrentThread();
void accumulateFilterRenderDiagnostics(FilterRenderDiagnostics& target,
                                       const FilterRenderDiagnostics& source);
void setFilterRenderDiagnosticsForCurrentThread(const FilterRenderDiagnostics& diagnostics);
void recordTileCacheDiagnosticsForCurrentThread(std::size_t hits, std::size_t misses,
                                                std::size_t evictions, std::size_t candidates = 0,
                                                std::size_t visits = 0);
std::size_t watermarkLayoutCacheBuildCountForCurrentThread();
std::size_t watermarkDirectFallbackCountForCurrentThread();
WatermarkRenderDiagnostics watermarkRenderDiagnosticsForCurrentThread();
void resetWatermarkRenderDiagnosticsForCurrentThread();
void resetWatermarkRenderCacheForCurrentThread();
void renderSceneItems(const SceneRenderRequest& request);
void renderSceneItemsTiled(const SceneRenderRequest& request);

void renderOverlayItems(QPainter& painter, const OverlayDisplayInfo& displayInfo,
                        const SnowCanvasOverlayItem* overlayItems, std::uint32_t overlayItemCount,
                        const QRegion& exposedRegion,
                        const SceneDisplayInfo* sceneDisplayInfo = nullptr,
                        const SnowCanvasSceneItem* sceneItems = nullptr,
                        std::uint32_t sceneItemCount = 0);

void renderOverlayItems(QPainter& painter, const OverlayDisplayInfo& displayInfo,
                        const SnowOverlayDisplayItem* overlayItems, std::uint32_t overlayItemCount,
                        const QRegion& exposedRegion);

void drawDirtyRectOverlay(QPainter& painter, const SnowDirtyRect* dirtyRects,
                          std::uint32_t dirtyRectCount, const QColor& stroke, const QColor& fill,
                          int paddingPx = 1);

} // namespace snow_canvas_renderer

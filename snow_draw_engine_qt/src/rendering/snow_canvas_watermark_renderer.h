#pragma once

#include "snow_canvas_display_cache.h"

#include <QPainter>
#include <QRectF>
#include <QRegion>
#include <QTransform>

#include <cstddef>

namespace snow_canvas_renderer {

enum class WatermarkRenderPurpose {
    Widget,
    ImageExport,
};

enum class WatermarkRenderStrategy {
    None,
    SparsePixmap,
    SparseImage,
    DenseCell,
    SegmentedSparse,
    GlyphFallback,
};

struct WatermarkRenderRequest {
    WatermarkDisplayInfo configuration;
    QRectF surfaceBounds;
    QRectF anchorRenderArea;
    QRegion exposedRegion;
    QTransform effectiveDeviceTransform;
    WatermarkRenderPurpose purpose = WatermarkRenderPurpose::ImageExport;
};

struct WatermarkRenderDiagnostics {
    std::size_t renderCallCount = 0;
    std::size_t earlyExitCount = 0;
    std::size_t cacheEvictionCount = 0;
    std::size_t shapeHitCount = 0;
    std::size_t shapeMissCount = 0;
    std::size_t unitHitCount = 0;
    std::size_t unitMissCount = 0;
    std::size_t tintBuildCount = 0;
    std::size_t repeatCellBuildCount = 0;
    std::size_t sparseBatchCount = 0;
    std::size_t submittedFragmentCount = 0;
    std::size_t culledFragmentCount = 0;
    std::size_t segmentedChunkCount = 0;
    std::size_t denseFillCount = 0;
    std::size_t fallbackGlyphDrawCount = 0;
    std::size_t cacheBytes = 0;
    double fragmentCoverage = 0.0;
    double shapeMilliseconds = 0.0;
    double rasterMilliseconds = 0.0;
    double tintMilliseconds = 0.0;
    double placementMilliseconds = 0.0;
    double compositionMilliseconds = 0.0;
    WatermarkRenderStrategy selectedStrategy = WatermarkRenderStrategy::None;
    QRectF renderedLogicalBounds;
    QRect renderedDeviceBounds;
};

class WatermarkPatternRenderer final {
  public:
    static void render(QPainter& painter, const WatermarkRenderRequest& request);
};

void renderWatermark(QPainter& painter, const WatermarkDisplayInfo& displayInfo);
void renderWatermark(QPainter& painter, const WatermarkDisplayInfo& displayInfo,
                     const QRectF& renderArea);

std::size_t watermarkLayoutCacheBuildCountForCurrentThread();
std::size_t watermarkDirectFallbackCountForCurrentThread();
std::size_t watermarkPatternCacheEntryCountForCurrentThread();
std::size_t watermarkPatternCacheBytesForCurrentThread();
std::size_t watermarkPlacementWorkspaceBytesForCurrentThread();
WatermarkRenderDiagnostics watermarkRenderDiagnosticsForCurrentThread();
void resetWatermarkRenderDiagnosticsForCurrentThread();
void resetWatermarkRenderCacheForCurrentThread();

} // namespace snow_canvas_renderer

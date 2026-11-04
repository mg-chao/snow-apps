#pragma once

#include "snow_draw_engine.h"
#include "snow_draw_engine_qt/snow_canvas_export_types.h"
#include "snow_draw_engine_qt/snow_canvas_smart_erase.h"

#include <QImage>
#include <QList>
#include <QRectF>
#include <QSize>

#include <cstddef>

namespace snow_canvas_export {

struct RenderDiagnostics {
    std::size_t directSourceFastPathCount = 0;
    std::size_t fullCompositorPathCount = 0;
    std::size_t unsynchronizedFallbackCount = 0;
    std::size_t outputRasterBytes = 0;
    std::size_t peakScratchPoolBytes = 0;
    std::size_t peakSourceTileBytes = 0;
    std::size_t directBackgroundOverlayCount = 0;
    std::size_t backgroundOverlayScratchBytes = 0;
};

RenderDiagnostics diagnosticsForCurrentThread();
void resetDiagnosticsForCurrentThread();

QImage renderImageSourcesToImage(const QRectF& canvasRect, const QSize& outputSize,
                                 const QList<CanvasExportSource>& sources);

QImage renderToImage(SnowRuntime runtime, const QRectF& virtualSelectionRect,
                     const QSize& outputSize, const QList<CanvasExportSource>& sources,
                     const SnowCanvasSmartEraseSnapshot& smartErase = {},
                     const QImage& background = {},
                     const SnowCanvasExportBackgroundOverlay& backgroundOverlay = {});

QImage renderTilesToImage(SnowRuntime runtime, const QRectF& sourceFrame,
                          const QSize& sourcePixelSize, const QSize& outputSize,
                          const QList<CanvasExportTile>& tiles,
                          const QList<CanvasExportSource>& sources,
                          const SnowCanvasSmartEraseSnapshot& smartErase = {},
                          const QImage& background = {});

} // namespace snow_canvas_export

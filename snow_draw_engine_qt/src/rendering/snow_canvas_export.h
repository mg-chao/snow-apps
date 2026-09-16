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
};

RenderDiagnostics diagnosticsForCurrentThread();
void resetDiagnosticsForCurrentThread();

QImage renderToImage(SnowRuntime runtime, const QRectF& virtualSelectionRect,
                     const QSize& outputSize, const QList<CanvasExportSource>& sources,
                     const SnowCanvasSmartEraseSnapshot& smartErase = {});

} // namespace snow_canvas_export

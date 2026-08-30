#pragma once

#include "snow_canvas_display_cache.h"

#include <QRectF>
#include <QRegion>

#include <cstddef>
#include <cstdint>

class QPainter;

namespace snow_canvas_spotlight_renderer {

struct RenderDiagnostics {
    std::size_t processedCutoutCount = 0;
    std::size_t locallyCulledCutoutCount = 0;
    std::size_t earlyExitCount = 0;
    std::size_t zeroCutoutFastPathCount = 0;
    std::size_t renderedPixelCount = 0;
    std::size_t renderedRegionCount = 0;
};

RenderDiagnostics diagnosticsForCurrentThread();
void resetDiagnosticsForCurrentThread();
void render(QPainter& painter, const SceneDisplayInfo& sceneInfo,
            const SpotlightDisplayInfo& spotlightInfo, const SnowSpotlightCutout* cutouts,
            std::uint32_t cutoutCount, const QRectF& renderArea, const QRegion& exposedRegion);

} // namespace snow_canvas_spotlight_renderer

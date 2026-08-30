#include "snow_draw_engine_qt/snow_canvas_region_filter.h"

#include "snow_canvas_filter_render.h"

#include <cmath>
#include <cstdint>

bool applySnowCanvasRegionFilter(const QImage& source, QImage& destination,
                                 const QRegion& destinationPixels,
                                 const SnowCanvasRegionFilterParameters& parameters) {
    if (!std::isfinite(parameters.strength) || !std::isfinite(parameters.logicalSigma) ||
        !std::isfinite(parameters.logicalSamplingRadius) ||
        !std::isfinite(parameters.devicePixelRatio) || parameters.devicePixelRatio <= 0.0) {
        return false;
    }
    snow_canvas_filter_render::Parameters internal;
    internal.type = static_cast<std::uint32_t>(parameters.type);
    internal.strength = parameters.strength;
    internal.logicalSigma = parameters.logicalSigma;
    internal.logicalSamplingRadius = parameters.logicalSamplingRadius;
    internal.devicePixelRatio = parameters.devicePixelRatio;
    return snow_canvas_filter_render::applyRegion(source, destination, destinationPixels, internal);
}

#include "snow_draw_engine_qt/snow_canvas_region_filter.h"

#include "snow_canvas_filter_render.h"

#include <cmath>
#include <cstdint>

namespace {
snow_canvas_filter_render::Parameters internalParameters(
    const SnowCanvasRegionFilterParameters& parameters) {
    snow_canvas_filter_render::Parameters internal;
    internal.type = static_cast<std::uint32_t>(parameters.type);
    internal.strength = parameters.strength;
    internal.logicalSigma = parameters.logicalSigma;
    internal.logicalSamplingRadius = parameters.logicalSamplingRadius;
    internal.devicePixelRatio = parameters.devicePixelRatio;
    internal.gridOriginInImage = parameters.gridOriginInImage;
    return internal;
}
} // namespace

struct SnowCanvasRegionFilterScratch::Impl {
    explicit Impl(std::size_t retainedByteLimit)
        : workspace(retainedByteLimit) {}

    snow_canvas_filter_render::RenderWorkspace workspace;
};

struct SnowCanvasRegionFilterScratchAccess {
    static snow_canvas_filter_render::RenderWorkspace& workspace(
        SnowCanvasRegionFilterScratch& scratch) {
        return scratch.m_impl->workspace;
    }
};

SnowCanvasRegionFilterScratch::SnowCanvasRegionFilterScratch(std::size_t retainedByteLimit)
    : m_impl(std::make_unique<Impl>(retainedByteLimit)) {}

SnowCanvasRegionFilterScratch::~SnowCanvasRegionFilterScratch() = default;

void SnowCanvasRegionFilterScratch::finishFrame() {
    m_impl->workspace.finishFrame(false);
}

int snowCanvasRegionFilterSupportPixels(const SnowCanvasRegionFilterParameters& parameters) {
    return snow_canvas_filter_render::samplingRadiusPixels(internalParameters(parameters));
}

bool applySnowCanvasRegionFilter(const QImage& source, QImage& destination,
                                 const QRegion& destinationPixels,
                                 const SnowCanvasRegionFilterParameters& parameters,
                                 SnowCanvasRegionFilterScratch* scratch) {
    if (!std::isfinite(parameters.strength) || !std::isfinite(parameters.logicalSigma) ||
        !std::isfinite(parameters.logicalSamplingRadius) ||
        !std::isfinite(parameters.devicePixelRatio) || parameters.devicePixelRatio <= 0.0) {
        return false;
    }
    snow_canvas_filter_render::RenderWorkspace* workspace =
        scratch != nullptr ? &SnowCanvasRegionFilterScratchAccess::workspace(*scratch) : nullptr;
    return snow_canvas_filter_render::applyRegion(source, destination, destinationPixels,
                                                  internalParameters(parameters), workspace);
}

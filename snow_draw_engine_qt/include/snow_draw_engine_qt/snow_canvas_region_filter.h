#pragma once

#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QImage>
#include <QRegion>

struct SnowCanvasRegionFilterParameters {
    SnowCanvasFilterType type = SnowCanvasFilterType::GaussianBlur;
    double strength = 1.0;
    double logicalSigma = 0.0;
    double logicalSamplingRadius = 0.0;
    qreal devicePixelRatio = 1.0;
};

// Applies a filter only to destinationPixels. The caller must provide same-size
// ARGB32 premultiplied images and initialize destination (normally as a copy of
// source); pixels outside destinationPixels are preserved.
[[nodiscard]] bool applySnowCanvasRegionFilter(
    const QImage& source, QImage& destination, const QRegion& destinationPixels,
    const SnowCanvasRegionFilterParameters& parameters);

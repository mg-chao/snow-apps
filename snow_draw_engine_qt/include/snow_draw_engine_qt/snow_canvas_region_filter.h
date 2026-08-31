#pragma once

#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QImage>
#include <QPointF>
#include <QRegion>

#include <cstddef>
#include <memory>

struct SnowCanvasRegionFilterParameters {
    SnowCanvasFilterType type = SnowCanvasFilterType::GaussianBlur;
    double strength = 1.0;
    double logicalSigma = 0.0;
    double logicalSamplingRadius = 0.0;
    qreal devicePixelRatio = 1.0;
    // Pixel-aligned origin the reduced sampling grid is anchored to. Callers that
    // filter a cropped sub-image pass the negated crop offset so the sampling
    // phase matches filtering the full image.
    QPointF gridOriginInImage;
};

// Opaque reusable scratch storage (pooled reduced-resolution buffers). Sharing one
// instance across filter invocations avoids reallocating scratch per call. Not
// thread-safe; each worker thread owns its own instance.
class SnowCanvasRegionFilterScratch {
  public:
    explicit SnowCanvasRegionFilterScratch(std::size_t retainedByteLimit = 16u * 1024u * 1024u);
    ~SnowCanvasRegionFilterScratch();

    SnowCanvasRegionFilterScratch(const SnowCanvasRegionFilterScratch&) = delete;
    SnowCanvasRegionFilterScratch& operator=(const SnowCanvasRegionFilterScratch&) = delete;

    // Releases leased scratch while retaining pooled buffers within the byte limit.
    void finishFrame();

  private:
    friend struct SnowCanvasRegionFilterScratchAccess;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Returns the blur's physical sampling radius in image pixels: filtered output at
// a pixel depends on source pixels at most this far away. Callers use it to size
// crops or masks around a region of interest.
[[nodiscard]] int snowCanvasRegionFilterSupportPixels(
    const SnowCanvasRegionFilterParameters& parameters);

// Applies a filter only to destinationPixels. The caller must provide same-size
// ARGB32 premultiplied images and initialize destination (normally as a copy of
// source); pixels outside destinationPixels are preserved.
[[nodiscard]] bool applySnowCanvasRegionFilter(
    const QImage& source, QImage& destination, const QRegion& destinationPixels,
    const SnowCanvasRegionFilterParameters& parameters,
    SnowCanvasRegionFilterScratch* scratch = nullptr);

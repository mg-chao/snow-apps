#ifndef SNOW_SHOT_PRESENTATION_PINNEDGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_PINNEDGEOMETRY_H

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/storage/pinnedwindowplacement.h"
#include <QImage>
#include <QScreen>
#include <algorithm>

namespace snow_shot::presentation {
using storage::kPinnedGeometryUnits;
using storage::pinnedGeometryScale;
using storage::PinnedGeometryUnits;

inline QRect pinnedScreenGeometry(const QScreen& screen) {
    return kPinnedGeometryUnits == PinnedGeometryUnits::LogicalPixels
               ? screen.geometry()
               : ScreenshotGeometryMapper::physicalRectForScreen(screen);
}
inline QRect pinnedLogicalRect(const QRect& rect, const QScreen* screen) {
    return kPinnedGeometryUnits == PinnedGeometryUnits::LogicalPixels
               ? rect
               : ScreenshotGeometryMapper::logicalRectForPhysicalRect(rect, screen);
}
// Imported raster pixels map to backing pixels on the target display. Callers
// rendering formatted text supply its rendering scale instead. Image DPR metadata
// must not determine the initial size of a file or clipboard pin.
inline QSize pinnedImageWindowSize(const QImage& image, qreal rasterScale) {
    const qreal scale = kPinnedGeometryUnits == PinnedGeometryUnits::LogicalPixels
                            ? std::max<qreal>(1.0, rasterScale)
                            : 1.0;
    return image.isNull() ? QSize()
                          : QSize(std::max(1, qRound(image.width() / scale)),
                                  std::max(1, qRound(image.height() / scale)));
}
} // namespace snow_shot::presentation
#endif

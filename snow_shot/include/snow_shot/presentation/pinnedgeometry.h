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

// One placement policy for every pin: fit into the work area, or center at full size.
inline ScreenshotPinnedImageFit
fitPinnedImageOnScreen(const QScreen& screen, const QSize& windowSize, bool autoResizeWindow) {
    const QRect available = screen.availableGeometry();
    const QRect logical = screen.geometry();
    const QRect native = pinnedScreenGeometry(screen);
    return autoResizeWindow
               ? ScreenshotGeometryMapper::fitImageToAvailableGeometry(windowSize, available,
                                                                       logical, native, 16)
               : ScreenshotGeometryMapper::centerImageAtFullResolution(windowSize, available,
                                                                       logical, native);
}
} // namespace snow_shot::presentation
#endif

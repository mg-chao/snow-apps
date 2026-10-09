#ifndef SNOW_SHOT_PRESENTATION_PINNEDGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_PINNEDGEOMETRY_H

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/storage/pinnedwindowplacement.h"
#include <QImage>
#include <QScreen>
#include <algorithm>
#include <limits>

namespace snow_shot::presentation {
using storage::kPinnedGeometryUnits;
using storage::pinnedGeometryScale;
using storage::PinnedGeometryUnits;

inline constexpr int pinnedShadowMargin(PinnedGeometryUnits units = kPinnedGeometryUnits) {
    return units == PinnedGeometryUnits::LogicalPixels ? 12 : 16;
}
inline QSize pinnedOuterSize(const QSize& content,
                             PinnedGeometryUnits units = kPinnedGeometryUnits) {
    const int padding = 2 * pinnedShadowMargin(units);
    const int maximumContent = std::numeric_limits<int>::max() - padding;
    return content.width() > 0 && content.height() > 0 && content.width() <= maximumContent &&
                   content.height() <= maximumContent
               ? QSize(content.width() + padding, content.height() + padding)
               : QSize();
}
inline QRect pinnedOuterRect(const QRect& content,
                             PinnedGeometryUnits units = kPinnedGeometryUnits) {
    if (!content.isValid())
        return {};
    const qint64 margin = pinnedShadowMargin(units);
    const qint64 left = qint64(content.left()) - margin;
    const qint64 top = qint64(content.top()) - margin;
    const qint64 right = qint64(content.right()) + margin;
    const qint64 bottom = qint64(content.bottom()) + margin;
    constexpr int minimum = std::numeric_limits<int>::min();
    constexpr int maximum = std::numeric_limits<int>::max();
    if (left < minimum || top < minimum || right > maximum || bottom > maximum ||
        right - left + 1 > maximum || bottom - top + 1 > maximum)
        return {};
    return QRect(QPoint(static_cast<int>(left), static_cast<int>(top)),
                 QPoint(static_cast<int>(right), static_cast<int>(bottom)));
}
inline QRect pinnedContentRect(const QRect& outer,
                               PinnedGeometryUnits units = kPinnedGeometryUnits) {
    if (!outer.isValid())
        return {};
    const qint64 margin = pinnedShadowMargin(units);
    const qint64 width = qint64(outer.right()) - outer.left() + 1;
    const qint64 height = qint64(outer.bottom()) - outer.top() + 1;
    constexpr int maximum = std::numeric_limits<int>::max();
    if (width <= 2 * margin || height <= 2 * margin || width > maximum || height > maximum)
        return {};
    return QRect(
        QPoint(static_cast<int>(outer.left() + margin), static_cast<int>(outer.top() + margin)),
        QPoint(static_cast<int>(outer.right() - margin),
               static_cast<int>(outer.bottom() - margin)));
}

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

#ifndef SNOW_SHOT_STORAGE_PINNEDWINDOWPLACEMENT_H
#define SNOW_SHOT_STORAGE_PINNEDWINDOWPLACEMENT_H

#include <QPointF>
#include <QSize>
#include <QString>
#include <cmath>

namespace snow_shot::storage {
enum class PinnedGeometryUnits { LogicalPixels, PhysicalPixels };
#if defined(Q_OS_MACOS)
inline constexpr auto kPinnedGeometryUnits = PinnedGeometryUnits::LogicalPixels;
#else
inline constexpr auto kPinnedGeometryUnits = PinnedGeometryUnits::PhysicalPixels;
#endif
inline qreal pinnedGeometryScale(qreal backingScale,
                                 PinnedGeometryUnits units = kPinnedGeometryUnits) {
    return units == PinnedGeometryUnits::LogicalPixels ? 1.0 : backingScale;
}
// Positions are display-local logical coordinates. Window extents use the
// explicit platform geometry unit, independently of the source raster size.
struct PinnedWindowPlacement {
    QString displayName;
    QString displaySerial;
    QPointF position;
    QSize windowSize;
    PinnedGeometryUnits units = kPinnedGeometryUnits;

    [[nodiscard]] bool isValid() const {
        return std::isfinite(position.x()) && std::isfinite(position.y()) &&
               windowSize.width() > 0 && windowSize.height() > 0;
    }
    friend bool operator==(const PinnedWindowPlacement&, const PinnedWindowPlacement&) = default;
};
} // namespace snow_shot::storage
#endif

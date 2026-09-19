#ifndef SNOW_SHOT_CAPTUREFRAMEGEOMETRY_H
#define SNOW_SHOT_CAPTUREFRAMEGEOMETRY_H
#include "snow_capture.h"
#include <QRect>
#include <cmath>
#include <limits>
#include <optional>
namespace snow_shot::presentation::capture {
inline std::optional<QRect> logicalFrameRect(const SnowCaptureFrameGeometry& geometry) {
    const double minimum = std::numeric_limits<int>::min();
    const double maximum = std::numeric_limits<int>::max();
    if (geometry.coordinate_space != 1 || !std::isfinite(geometry.x) ||
        !std::isfinite(geometry.y) || !std::isfinite(geometry.width) ||
        !std::isfinite(geometry.height) || !std::isfinite(geometry.backing_scale) ||
        geometry.backing_scale <= 0 || geometry.width < 1 || geometry.height < 1 ||
        geometry.width > maximum || geometry.height > maximum || geometry.x < minimum ||
        geometry.y < minimum || geometry.x + geometry.width > maximum ||
        geometry.y + geometry.height > maximum)
        return std::nullopt;
    return QRect(qRound(geometry.x), qRound(geometry.y), qRound(geometry.width),
                 qRound(geometry.height));
}
} // namespace snow_shot::presentation::capture
#endif

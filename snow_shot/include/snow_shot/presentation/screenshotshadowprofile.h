#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSHADOWPROFILE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSHADOWPROFILE_H

#include <QtGlobal>

#include <algorithm>

namespace snow_shot::presentation::shadow_profile {
inline constexpr qreal peakAlphaScale = 0.36;

// Squaring the cubic fade roughly halves the dense inner band: half-peak opacity now
// reaches 11% of the margin instead of 21%. The tail still reaches zero with zero slope.
// Both cached bitmap assets and exterior vector spans use this allocation-free profile.
inline qreal falloff(qreal distance, qreal width) {
    const qreal remaining = std::clamp(1.0 - distance / width, 0.0, 1.0);
    const qreal cubic = remaining * remaining * remaining;
    return cubic * cubic;
}
} // namespace snow_shot::presentation::shadow_profile

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSHADOWPROFILE_H

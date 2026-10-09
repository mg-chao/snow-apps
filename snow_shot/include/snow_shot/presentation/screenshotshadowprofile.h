#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSHADOWPROFILE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSHADOWPROFILE_H

#include <QtGlobal>

#include <algorithm>

namespace snow_shot::presentation::shadow_profile {
inline constexpr qreal peakAlphaScale = 0.36;

// Half-peak opacity reaches 16% of the margin, midway between the broader cubic fade
// (21%) and the narrower sixth-power fade (11%). The tail reaches zero with zero slope.
// Both cached bitmap assets and exterior vector spans use this allocation-free profile.
inline qreal falloff(qreal distance, qreal width) {
    const qreal remaining = std::clamp(1.0 - distance / width, 0.0, 1.0);
    const qreal squared = remaining * remaining;
    return squared * squared;
}
} // namespace snow_shot::presentation::shadow_profile

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSHADOWPROFILE_H

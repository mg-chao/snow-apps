#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCLICKTHROUGHGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCLICKTHROUGHGEOMETRY_H

#include <QRect>

namespace screenshot_pinned_click_through {
[[nodiscard]] QRect exitButtonGeometry(const QRect& pinnedNativeGeometry,
                                       const QRect& screenPhysicalBounds, qreal devicePixelRatio);
} // namespace screenshot_pinned_click_through

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCLICKTHROUGHGEOMETRY_H

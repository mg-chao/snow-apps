#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCLICKTHROUGHGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCLICKTHROUGHGEOMETRY_H

#include <QRect>

namespace screenshot_pinned_click_through {
inline constexpr int kControlHeight = 32;
inline constexpr int kOpacityEditorWidth = 152;
inline constexpr int kControlSpacing = 8;
struct ControlsGeometry {
    QRect opacityEditor;
    QRect moveButton;
    QRect exitButton;
};
[[nodiscard]] ControlsGeometry controlsGeometry(const QRect& pinnedNativeGeometry,
                                                const QRect& screenPhysicalBounds,
                                                qreal devicePixelRatio);
[[nodiscard]] QRect exitButtonGeometry(const QRect& pinnedNativeGeometry,
                                       const QRect& screenPhysicalBounds, qreal devicePixelRatio);
} // namespace screenshot_pinned_click_through

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCLICKTHROUGHGEOMETRY_H

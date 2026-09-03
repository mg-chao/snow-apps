#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESTOREGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESTOREGEOMETRY_H

#include <QList>
#include <QRect>

namespace screenshot_pinned_restore_geometry {

struct ScreenGeometry {
    QRect physicalBounds;
    QRect availableBounds;
    qreal devicePixelRatio = 1.0;
};

// The subset of a persisted pinned-window record whose values are expressed
// in physical pixels of the monitor that saved them. The scale percent is
// derived from those pixels (window width over the full-resolution basis), so
// it moves with them: translating one field without the others yields a
// window whose menu, wheel stepping and geometry disagree.
struct SavedState {
    QRect nativeGeometry;
    // Empty when the window was not in thumbnail mode.
    QRect preThumbnailNativeGeometry;
    double scalePercent = 100.0;
    // Empty for legacy records that did not capture the saving monitor; such
    // records are restored without any DPI translation.
    QRect screenPhysicalBounds;
    qreal screenDevicePixelRatio = 1.0;
};

struct RestoredState {
    QRect nativeGeometry;
    QRect preThumbnailNativeGeometry;
    double scalePercent = 100.0;
};

// Translates every DPI-dependent field of a saved record from the saving
// monitor to the target monitor in one step and keeps the geometries
// reachable on the current screen layout. This is the only entry point on
// purpose: callers cannot obtain a translated geometry without the matching
// translated scale percent. Empty geometries stay empty so optional rects
// round-trip as absent; the percent is scaled by exactly the same ratio as
// the rects, which leaves it bit-identical when the DPI did not change.
[[nodiscard]] RestoredState reconcileSavedState(const SavedState& saved,
                                                const ScreenGeometry& targetScreen,
                                                const QList<ScreenGeometry>& screens);

} // namespace screenshot_pinned_restore_geometry

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESTOREGEOMETRY_H

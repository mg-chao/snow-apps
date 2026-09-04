#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESTOREGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESTOREGEOMETRY_H

#include <QList>
#include <QRect>

namespace screenshot_pinned_restore_geometry {

struct ScreenGeometry {
    QRect physicalBounds;
    QRect availableBounds;
};

// The subset of a persisted pinned-window record whose values are expressed
// in physical pixels of the monitor that saved them. Physical pixels are the
// only unit: a restored window is recreated at exactly its saved physical
// size, and its scale value derives from that size, so the saving monitor's
// DPI never participates in a restore.
struct SavedState {
    QRect nativeGeometry;
    // Empty when the window was not in thumbnail mode.
    QRect preThumbnailNativeGeometry;
    // Empty for legacy records that did not capture the saving monitor; such
    // records are restored at their saved coordinates without re-basing.
    QRect screenPhysicalBounds;
};

struct RestoredState {
    QRect nativeGeometry;
    QRect preThumbnailNativeGeometry;
};

// Re-bases every persisted geometry from the saving monitor onto the target
// monitor of the current screen layout while keeping the saved physical pixel
// sizes, and keeps the geometries reachable on screen. Monitor DPI is
// deliberately not consulted: the scale value is a function of the physical
// sizes alone. This is the only entry point on purpose: callers cannot obtain
// a re-based geometry without its matching pre-thumbnail geometry. Empty
// geometries stay empty so optional rects round-trip as absent.
[[nodiscard]] RestoredState reconcileSavedState(const SavedState& saved,
                                                const ScreenGeometry& targetScreen,
                                                const QList<ScreenGeometry>& screens);

} // namespace screenshot_pinned_restore_geometry

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESTOREGEOMETRY_H

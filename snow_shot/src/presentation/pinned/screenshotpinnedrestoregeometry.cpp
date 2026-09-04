#include "screenshotpinnedrestoregeometry.h"

#include <QtGlobal>

#include <algorithm>

namespace screenshot_pinned_restore_geometry {
namespace {

QRect translateGeometry(const QRect& savedGeometry, const SavedState& saved,
                        const ScreenGeometry& targetScreen, const QList<ScreenGeometry>& screens) {
    if (savedGeometry.isEmpty()) {
        return savedGeometry;
    }
    QRect geometry = savedGeometry;
    if (!saved.screenPhysicalBounds.isEmpty()) {
        // A restored window keeps its saved physical pixel size; only its
        // anchor onto the monitor layout is re-based when the saving monitor
        // is gone or moved.
        const QPoint relative = geometry.topLeft() - saved.screenPhysicalBounds.topLeft();
        geometry.moveTopLeft(targetScreen.physicalBounds.topLeft() + relative);
    }
    bool visible = false;
    for (const ScreenGeometry& screen : screens) {
        if (screen.availableBounds.intersects(geometry)) {
            visible = true;
            break;
        }
    }
    if (!visible) {
        const QRect& available = targetScreen.availableBounds;
        const int left = geometry.width() >= available.width()
                             ? available.left()
                             : qBound(available.left(), geometry.left(),
                                      available.right() - geometry.width() + 1);
        const int top = geometry.height() >= available.height()
                            ? available.top()
                            : qBound(available.top(), geometry.top(),
                                     available.bottom() - geometry.height() + 1);
        geometry.moveLeft(left);
        geometry.moveTop(top);
    }
    return geometry;
}

} // namespace

RestoredState reconcileSavedState(const SavedState& saved, const ScreenGeometry& targetScreen,
                                  const QList<ScreenGeometry>& screens) {
    RestoredState restored;
    restored.nativeGeometry = translateGeometry(saved.nativeGeometry, saved, targetScreen, screens);
    restored.preThumbnailNativeGeometry =
        translateGeometry(saved.preThumbnailNativeGeometry, saved, targetScreen, screens);
    return restored;
}

} // namespace screenshot_pinned_restore_geometry

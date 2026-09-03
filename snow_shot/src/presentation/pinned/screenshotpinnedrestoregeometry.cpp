#include "screenshotpinnedrestoregeometry.h"

#include <QtGlobal>

#include <algorithm>

namespace screenshot_pinned_restore_geometry {
namespace {

// Legacy records without the saving monitor's bounds cannot be positioned
// relative to it, so they are also left at their saved scale: applying a
// DPI ratio to only some of the fields is exactly the inconsistency this
// module exists to prevent.
qreal dpiRatio(const SavedState& saved, const ScreenGeometry& targetScreen) {
    if (saved.screenPhysicalBounds.isEmpty()) {
        return 1.0;
    }
    const qreal savedDpi = saved.screenDevicePixelRatio > 0.0 ? saved.screenDevicePixelRatio : 1.0;
    const qreal targetDpi =
        targetScreen.devicePixelRatio > 0.0 ? targetScreen.devicePixelRatio : 1.0;
    return targetDpi / savedDpi;
}

QRect translateGeometry(const QRect& savedGeometry, const SavedState& saved,
                        const ScreenGeometry& targetScreen, qreal ratio,
                        const QList<ScreenGeometry>& screens) {
    if (savedGeometry.isEmpty()) {
        return savedGeometry;
    }
    QRect geometry = savedGeometry;
    if (!saved.screenPhysicalBounds.isEmpty()) {
        const QPoint relative = geometry.topLeft() - saved.screenPhysicalBounds.topLeft();
        geometry.moveTopLeft(targetScreen.physicalBounds.topLeft() +
                             QPoint(qRound(relative.x() * ratio), qRound(relative.y() * ratio)));
        geometry.setSize(QSize(std::max(1, qRound(geometry.width() * ratio)),
                               std::max(1, qRound(geometry.height() * ratio))));
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
    const qreal ratio = dpiRatio(saved, targetScreen);
    RestoredState restored;
    restored.nativeGeometry =
        translateGeometry(saved.nativeGeometry, saved, targetScreen, ratio, screens);
    restored.preThumbnailNativeGeometry =
        translateGeometry(saved.preThumbnailNativeGeometry, saved, targetScreen, ratio, screens);
    restored.scalePercent = saved.scalePercent * ratio;
    return restored;
}

} // namespace screenshot_pinned_restore_geometry

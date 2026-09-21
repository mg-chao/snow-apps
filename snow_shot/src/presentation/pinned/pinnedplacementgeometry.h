#ifndef SNOW_SHOT_PRESENTATION_PINNEDPLACEMENTGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_PINNEDPLACEMENTGEOMETRY_H
#include "snow_shot/storage/pinnedwindowplacement.h"
#include <QRectF>
#include <algorithm>

namespace snow_shot::presentation {
struct PinnedDisplayGeometry {
    QString name;
    QString serial;
    QRectF desktopBounds;
    QRectF usableBounds;
    qreal backingScale = 1;
};
inline bool pinnedDisplayContains(const PinnedDisplayGeometry& display, const QPointF& point) {
    const QRectF& bounds = display.desktopBounds;
    return point.x() >= bounds.left() && point.x() < bounds.left() + bounds.width() &&
           point.y() >= bounds.top() && point.y() < bounds.top() + bounds.height();
}
inline QRectF pinnedDesktopRect(const storage::PinnedWindowPlacement& placement,
                                const PinnedDisplayGeometry& display) {
    return {display.desktopBounds.topLeft() + placement.position,
            QSizeF(placement.windowSize) /
                storage::pinnedGeometryScale(display.backingScale, placement.units)};
}
inline storage::PinnedWindowPlacement
pinnedPlacementAtPointer(storage::PinnedWindowPlacement placement,
                         const PinnedDisplayGeometry& display, const QPointF& pointer,
                         const QPointF& anchorPixels) {
    placement.displayName = display.name;
    placement.displaySerial = display.serial;
    placement.position =
        pointer - display.desktopBounds.topLeft() -
        anchorPixels / storage::pinnedGeometryScale(display.backingScale, placement.units);
    return placement;
}
inline storage::PinnedWindowPlacement
recoverPinnedPlacement(storage::PinnedWindowPlacement placement,
                       const PinnedDisplayGeometry& display) {
    placement.displayName = display.name;
    placement.displaySerial = display.serial;
    const QRectF available = display.usableBounds.translated(-display.desktopBounds.topLeft());
    const QSizeF size = QSizeF(placement.windowSize) /
                        storage::pinnedGeometryScale(display.backingScale, placement.units);
    placement.position.setX(
        std::clamp(placement.position.x(), available.left(),
                   std::max(available.left(), available.right() - size.width())));
    placement.position.setY(
        std::clamp(placement.position.y(), available.top(),
                   std::max(available.top(), available.bottom() - size.height())));
    return placement;
}
} // namespace snow_shot::presentation
#endif

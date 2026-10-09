#ifndef SNOW_SHOT_PRESENTATION_PINNEDPLACEMENTGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_PINNEDPLACEMENTGEOMETRY_H
#include "snow_shot/storage/pinnedwindowplacement.h"
#include <QRectF>
#include <algorithm>
#include <utility>

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
inline QRectF pinnedDesktopContentRect(const storage::PinnedWindowPlacement& placement,
                                       const PinnedDisplayGeometry& display, int frameMargin) {
    const qreal inset =
        frameMargin / storage::pinnedGeometryScale(display.backingScale, placement.units);
    return pinnedDesktopRect(placement, display).adjusted(inset, inset, -inset, -inset);
}
inline storage::PinnedWindowPlacement
pinnedPlacementAtContentOrigin(storage::PinnedWindowPlacement placement,
                               const PinnedDisplayGeometry& display, const QPointF& contentOrigin,
                               int frameMargin) {
    return pinnedPlacementAtPointer(std::move(placement), display, contentOrigin,
                                    QPointF(frameMargin, frameMargin));
}
template <typename Display, typename Describe, typename Apply, typename Read, typename Resolve>
bool applyExactPinnedPlacement(storage::PinnedWindowPlacement requested, Display display,
                               int frameMargin, Describe describe, Apply apply, Read read,
                               Resolve resolve) {
    if (!display || !requested.isValid() || frameMargin < 0 ||
        requested.windowSize.width() <= qint64(frameMargin) * 2 ||
        requested.windowSize.height() <= qint64(frameMargin) * 2)
        return false;
    const QPointF origin =
        pinnedDesktopContentRect(requested, describe(display), frameMargin).topLeft();
    const QSize windowSize = requested.windowSize;
    const int attempts = requested.units == storage::PinnedGeometryUnits::LogicalPixels ? 1 : 3;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (!display || !apply(requested, display) || !display)
            return false;
        const auto actual = read();
        if (!actual || !display)
            return false;
        display = resolve(*actual, display);
        if (!display)
            return false;
        const auto descriptor = describe(display);
        if (actual->windowSize == windowSize) {
            const QPointF difference =
                pinnedDesktopContentRect(*actual, descriptor, frameMargin).topLeft() - origin;
            const qreal tolerance =
                .51 / storage::pinnedGeometryScale(descriptor.backingScale, actual->units);
            if (qAbs(difference.x()) <= tolerance && qAbs(difference.y()) <= tolerance)
                return true;
        }
        requested =
            pinnedPlacementAtContentOrigin(std::move(requested), descriptor, origin, frameMargin);
    }
    return false;
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

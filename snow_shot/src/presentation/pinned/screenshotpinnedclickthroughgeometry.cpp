#include "screenshotpinnedclickthroughgeometry.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

QRect screenshot_pinned_click_through::exitButtonGeometry(const QRect& pinnedNativeGeometry,
                                                          const QRect& screenPhysicalBounds,
                                                          qreal devicePixelRatio) {
    if (!pinnedNativeGeometry.isValid() || pinnedNativeGeometry.isEmpty() ||
        !screenPhysicalBounds.isValid() || screenPhysicalBounds.isEmpty() ||
        !std::isfinite(devicePixelRatio) || devicePixelRatio <= 0.0) {
        return {};
    }

    const int size = std::max(1, qRound(32.0 * devicePixelRatio));
    const int inset = std::max(1, qRound(16.0 * devicePixelRatio));
    if (screenPhysicalBounds.width() < size || screenPhysicalBounds.height() < size) {
        return {};
    }

    const int preferredLeft =
        pinnedNativeGeometry.x() + pinnedNativeGeometry.width() - inset - size;
    const int preferredTop = pinnedNativeGeometry.y() - inset - size;
    const int maximumLeft = screenPhysicalBounds.x() + screenPhysicalBounds.width() - size;
    const int maximumTop = screenPhysicalBounds.y() + screenPhysicalBounds.height() - size;
    return QRect(QPoint(qBound(screenPhysicalBounds.x(), preferredLeft, maximumLeft),
                        qBound(screenPhysicalBounds.y(), preferredTop, maximumTop)),
                 QSize(size, size));
}

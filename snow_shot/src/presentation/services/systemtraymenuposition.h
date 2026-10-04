#pragma once

#include <QPoint>
#include <QRect>

namespace snow_shot::presentation {

inline QPoint systemTrayMenuPosition(const QRect& iconGeometry, const QPoint& pointerPosition) {
    // Keyboard activation leaves the pointer anywhere on the desktop. Anchor to the icon
    // in that case, while keeping the mouse position for requests made inside its bounds.
    if (iconGeometry.isValid() && !iconGeometry.contains(pointerPosition)) {
        return iconGeometry.center();
    }
    return pointerPosition;
}

} // namespace snow_shot::presentation

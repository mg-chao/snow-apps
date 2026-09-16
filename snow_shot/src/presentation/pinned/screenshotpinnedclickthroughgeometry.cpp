#include "screenshotpinnedclickthroughgeometry.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

screenshot_pinned_click_through::ControlsGeometry screenshot_pinned_click_through::controlsGeometry(
    const QRect& pinnedNativeGeometry, const QRect& screenPhysicalBounds, qreal devicePixelRatio) {
    if (!pinnedNativeGeometry.isValid() || pinnedNativeGeometry.isEmpty() ||
        !screenPhysicalBounds.isValid() || screenPhysicalBounds.isEmpty() ||
        !std::isfinite(devicePixelRatio) || devicePixelRatio <= 0.0) {
        return {};
    }

    const int size = std::max(1, qRound(kControlHeight * devicePixelRatio));
    const int inset = std::max(1, qRound(16.0 * devicePixelRatio));
    const int editorWidth = std::max(1, qRound(kOpacityEditorWidth * devicePixelRatio));
    const int spacing = std::max(1, qRound(kControlSpacing * devicePixelRatio));
    const int totalWidth = editorWidth + 2 * spacing + 2 * size;
    if (screenPhysicalBounds.width() < totalWidth || screenPhysicalBounds.height() < size) {
        return {};
    }

    const int preferredLeft =
        pinnedNativeGeometry.x() + pinnedNativeGeometry.width() - inset - totalWidth;
    const int preferredTop = pinnedNativeGeometry.y() - inset - size;
    const int maximumLeft = screenPhysicalBounds.x() + screenPhysicalBounds.width() - totalWidth;
    const int maximumTop = screenPhysicalBounds.y() + screenPhysicalBounds.height() - size;
    const QPoint origin(qBound(screenPhysicalBounds.x(), preferredLeft, maximumLeft),
                        qBound(screenPhysicalBounds.y(), preferredTop, maximumTop));
    return {QRect(origin, QSize(editorWidth, size)),
            QRect(origin + QPoint(editorWidth + spacing, 0), QSize(size, size)),
            QRect(origin + QPoint(editorWidth + 2 * spacing + size, 0), QSize(size, size))};
}

QRect screenshot_pinned_click_through::exitButtonGeometry(const QRect& pinnedNativeGeometry,
                                                          const QRect& screenPhysicalBounds,
                                                          qreal devicePixelRatio) {
    return controlsGeometry(pinnedNativeGeometry, screenPhysicalBounds, devicePixelRatio)
        .exitButton;
}

#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEFFECTGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEFFECTGEOMETRY_H

#include "snow_shot/presentation/screenshotselectionlimits.h"

#include <QLineF>
#include <QRectF>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>

inline constexpr qreal kScreenshotSelectionShadowControlScale = 1.5;
inline constexpr qreal kScreenshotSelectionShadowControlHitRadius =
    8.0 * kScreenshotSelectionShadowControlScale;

enum class ScreenshotSelectionEffectHandle {
    None,
    TopLeft,
    TopRight,
    BottomRight,
    BottomLeft,
    Shadow
};

inline bool screenshotSelectionRadiusHandle(ScreenshotSelectionEffectHandle handle) {
    return handle != ScreenshotSelectionEffectHandle::None &&
           handle != ScreenshotSelectionEffectHandle::Shadow;
}

struct ScreenshotSelectionEffectLayout {
    bool available = false;
    std::array<QPointF, 4> corners;
    QPointF shadow;
    QPointF shadowAnchor;
    qreal shadowDragDirection = 1.0;
    qreal maximumRadius = 0.0;
    qreal radiusPerCanvasUnit = 0.0;

    QPointF position(ScreenshotSelectionEffectHandle handle) const {
        if (screenshotSelectionRadiusHandle(handle))
            return corners[static_cast<std::size_t>(handle) - 1];
        return shadow;
    }

    ScreenshotSelectionEffectHandle nearestCorner(const QPointF& point, qreal distance) const {
        auto nearest = ScreenshotSelectionEffectHandle::None;
        for (std::size_t index = 0; index < corners.size(); ++index) {
            const qreal candidate = QLineF(point, corners[index]).length();
            if (candidate <= distance) {
                distance = candidate;
                nearest = static_cast<ScreenshotSelectionEffectHandle>(index + 1);
            }
        }
        return nearest;
    }
};

// All decoration distances are logical viewport pixels; effect values remain canvas units.
inline ScreenshotSelectionEffectLayout
screenshotSelectionEffectLayout(const QRectF& selection, int radius, const QTransform& canvasToView,
                                const QRectF& viewport) {
    ScreenshotSelectionEffectLayout layout;
    const QRectF view = canvasToView.mapRect(selection.normalized());
    const qreal side = std::min(view.width(), view.height());
    if (!selection.isValid() || side <= 32.0 || !canvasToView.isInvertible())
        return layout;
    layout.available = true;
    layout.maximumRadius =
        std::min<qreal>(snow_shot::presentation::kScreenshotSelectionCornerRadiusMax,
                        std::min(selection.width(), selection.height()) / 2.0);
    const qreal minimumInset = 12.0;
    const qreal maximumInset = side / 2.0 - 4.0;
    const qreal inset = minimumInset + (maximumInset - minimumInset) *
                                           std::clamp(radius / layout.maximumRadius, 0.0, 1.0);
    layout.corners = {
        view.topLeft() + QPointF(inset, inset), view.topRight() + QPointF(-inset, inset),
        view.bottomRight() - QPointF(inset, inset), view.bottomLeft() + QPointF(inset, -inset)};
    const qreal scale = std::min(std::hypot(canvasToView.m11(), canvasToView.m12()),
                                 std::hypot(canvasToView.m21(), canvasToView.m22()));
    layout.radiusPerCanvasUnit = layout.maximumRadius * scale / (maximumInset - minimumInset);
    const qreal shadowOffset = 16.0 * kScreenshotSelectionShadowControlScale;
    layout.shadowAnchor = QPointF(view.right(), view.center().y());
    layout.shadow = layout.shadowAnchor + QPointF(shadowOffset, 0);
    if (!viewport.isEmpty() &&
        layout.shadow.x() + kScreenshotSelectionShadowControlHitRadius > viewport.right()) {
        const QPointF leftShadow(view.left() - shadowOffset, view.center().y());
        if (leftShadow.x() - kScreenshotSelectionShadowControlHitRadius >= viewport.left() &&
            leftShadow.x() + kScreenshotSelectionShadowControlHitRadius <= viewport.right()) {
            layout.shadowAnchor = QPointF(view.left(), view.center().y());
            layout.shadow = leftShadow;
            layout.shadowDragDirection = -1.0;
        } else {
            layout.shadow.rx() = view.right() - shadowOffset;
        }
    }
    // A clipped/multi-display selection must never move its control to an unrelated viewport.
    layout.available =
        viewport.isEmpty() || viewport.contains(view.center()) || viewport.intersects(view);
    return layout;
}

inline QPointF screenshotSelectionRadiusInwardDirection(ScreenshotSelectionEffectHandle handle) {
    switch (handle) {
    case ScreenshotSelectionEffectHandle::TopLeft:
        return {1, 1};
    case ScreenshotSelectionEffectHandle::TopRight:
        return {-1, 1};
    case ScreenshotSelectionEffectHandle::BottomRight:
        return {-1, -1};
    case ScreenshotSelectionEffectHandle::BottomLeft:
        return {1, -1};
    default:
        return {};
    }
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEFFECTGEOMETRY_H

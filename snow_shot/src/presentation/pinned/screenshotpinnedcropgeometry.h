// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "screenshotpinnedresizegeometry.h"
#include <QImage>
#include <QRegion>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <optional>

namespace screenshot_pinned_crop_geometry {
inline bool valid(const QRectF& rect) {
    return rect.width() > 0 && rect.height() > 0 && std::isfinite(rect.x()) &&
           std::isfinite(rect.y()) && std::isfinite(rect.right()) && std::isfinite(rect.bottom());
}

inline QRegion selectionRepaintRegion(const QRectF& previous, const QRectF& current,
                                      const QRect& viewport) {
    if (previous == current)
        return {};
    QRegion damage;
    for (const QRectF& rect : {previous, current}) {
        // Include the centered stroke and its antialiasing fringe in logical view pixels.
        if (valid(rect))
            damage += rect.adjusted(-2, -2, 2, 2).toAlignedRect();
    }
    return damage.intersected(viewport);
}

inline QTransform sourceToCanvas(const QSize& pixels, const QTransform& orientation,
                                 const QRectF& canvas) {
    if (pixels.isEmpty() || !valid(canvas))
        return {};
    const auto rotation = QImage::trueMatrix(orientation, pixels.width(), pixels.height());
    const auto bounds = rotation.mapRect(QRectF(QPointF(), pixels));
    QTransform projection;
    projection.translate(canvas.x(), canvas.y());
    projection.scale(canvas.width() / bounds.width(), canvas.height() / bounds.height());
    return rotation * projection;
}

inline QRect pixelBounds(const QRectF& rect, const QSize& size) {
    return valid(rect) ? rect.toAlignedRect().intersected(QRect(QPoint(), size)) : QRect();
}

struct Adjustment {
    QRectF rect;
    screenshot_pinned_resize_geometry::DragHandle handle;
};

// Work in one fixed source-to-screen coordinate system for the entire drag. Signed
// endpoints, rather than accumulated window deltas, make crossing reversible.
inline Adjustment adjust(const QRectF& start, const QRectF& bounds, const QPointF& delta,
                         screenshot_pinned_resize_geometry::DragHandle pressed,
                         screenshot_pinned_resize_geometry::DragHandle previous,
                         const QSizeF& minimum) {
    using H = screenshot_pinned_resize_geometry::DragHandle;
    constexpr Qt::Edges edges[] = {Qt::LeftEdge | Qt::TopEdge,     Qt::TopEdge,
                                   Qt::RightEdge | Qt::TopEdge,    Qt::RightEdge,
                                   Qt::RightEdge | Qt::BottomEdge, Qt::BottomEdge,
                                   Qt::LeftEdge | Qt::BottomEdge,  Qt::LeftEdge};
    if (!valid(start) || !valid(bounds))
        return {start, previous};
    const auto initial = edges[int(pressed)];
    Qt::Edges effective;
    auto axis = [&](qreal low, qreal high, qreal boundLow, qreal boundHigh, qreal movement,
                    qreal min, Qt::Edge lowEdge, Qt::Edge highEdge) {
        const bool movingLow = initial.testFlag(lowEdge);
        const bool movingHigh = initial.testFlag(highEdge);
        qreal first = low + (movingLow ? movement : 0);
        qreal last = high + (movingHigh ? movement : 0);
        const bool reversed =
            first > last ||
            (first == last && edges[int(previous)].testFlag(movingLow ? highEdge : lowEdge));
        if (movingLow || movingHigh)
            effective |= (movingLow != reversed) ? lowEdge : highEdge;
        if (first > last)
            std::swap(first, last);
        min = std::clamp(min, qreal(0), boundHigh - boundLow);
        first = std::clamp(first, boundLow, boundHigh);
        last = std::clamp(last, boundLow, boundHigh);
        if (last - first < min) {
            if (effective.testFlag(lowEdge))
                first = last - min;
            else if (effective.testFlag(highEdge))
                last = first + min;
            else {
                first = (first + last - min) / 2;
                last = first + min;
            }
            if (first < boundLow) {
                last += boundLow - first;
                first = boundLow;
            }
            if (last > boundHigh) {
                first -= last - boundHigh;
                last = boundHigh;
            }
        }
        return std::pair(first, last);
    };
    const auto x = axis(start.left(), start.right(), bounds.left(), bounds.right(), delta.x(),
                        minimum.width(), Qt::LeftEdge, Qt::RightEdge);
    const auto y = axis(start.top(), start.bottom(), bounds.top(), bounds.bottom(), delta.y(),
                        minimum.height(), Qt::TopEdge, Qt::BottomEdge);
    H handle = previous;
    for (int i = 0; i != 8; ++i)
        if (edges[i] == effective)
            handle = H(i);
    return {QRectF(QPointF(x.first, y.first), QPointF(x.second, y.second)), handle};
}
} // namespace screenshot_pinned_crop_geometry

// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGEIMAGEEDITING_P_H
#define SNOW_SHOT_PRESENTATION_COLLAGEIMAGEEDITING_P_H

#include "snow_draw_engine_qt/snow_canvas_overlay_style.h"
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace snow_shot::presentation::collage_detail {
inline void paintPreviewBorder(QPainter& painter, const QRectF& bounds, const QColor& color) {
    // Quantize the one-logical-pixel guide and its edges in device space, including
    // backing-store translations, so every side covers the same whole pixel count.
    const auto deviceTransform = painter.deviceTransform();
    const auto pixels = deviceTransform.mapRect(bounds);
    const qreal stroke = std::max(1, qRound(deviceTransform.m11()));
    const qreal halfStroke = stroke / 2;
    const auto snapEdge = [halfStroke](qreal coordinate) {
        const qreal edge = coordinate - halfStroke;
        // Shared cell edges can differ by floating-point roundoff at a half-pixel tie.
        const qreal tie = std::floor(edge) + .5;
        return qFuzzyCompare(edge, tie) ? std::ceil(edge) : std::round(edge);
    };
    const QRectF outer(
        QPointF(snapEdge(pixels.left()), snapEdge(pixels.top())),
        QPointF(snapEdge(pixels.right()) + stroke, snapEdge(pixels.bottom()) + stroke));
    QPainterPath border;
    border.addRect(outer);
    const auto inner = outer.adjusted(stroke, stroke, -stroke, -stroke);
    if (!inner.isEmpty())
        border.addRect(inner);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillPath(deviceTransform.inverted().map(border), color);
    painter.restore();
}

inline Qt::CursorShape resizeCursor(SnowCanvasSelectionHandle handle) {
    switch (handle) {
    case SnowCanvasSelectionHandle::TopLeft:
    case SnowCanvasSelectionHandle::BottomRight:
        return Qt::SizeFDiagCursor;
    case SnowCanvasSelectionHandle::TopRight:
    case SnowCanvasSelectionHandle::BottomLeft:
        return Qt::SizeBDiagCursor;
    case SnowCanvasSelectionHandle::Top:
    case SnowCanvasSelectionHandle::Bottom:
        return Qt::SizeVerCursor;
    case SnowCanvasSelectionHandle::Left:
    case SnowCanvasSelectionHandle::Right:
        return Qt::SizeHorCursor;
    case SnowCanvasSelectionHandle::None:
        return Qt::ArrowCursor;
    }
    return Qt::ArrowCursor;
}

inline QRectF centeredResize(const QRectF& initialImage, SnowCanvasSelectionHandle handle,
                             QPointF delta, qreal viewScale) {
    const auto half = initialImage.size() / 2;
    QPointF vector;
    switch (handle) {
    case SnowCanvasSelectionHandle::TopLeft:
        vector = {-half.width(), -half.height()};
        break;
    case SnowCanvasSelectionHandle::TopRight:
        vector = {half.width(), -half.height()};
        break;
    case SnowCanvasSelectionHandle::BottomRight:
        vector = {half.width(), half.height()};
        break;
    case SnowCanvasSelectionHandle::BottomLeft:
        vector = {-half.width(), half.height()};
        break;
    case SnowCanvasSelectionHandle::Top:
        vector = {0, -half.height()};
        break;
    case SnowCanvasSelectionHandle::Right:
        vector = {half.width(), 0};
        break;
    case SnowCanvasSelectionHandle::Bottom:
        vector = {0, half.height()};
        break;
    case SnowCanvasSelectionHandle::Left:
        vector = {-half.width(), 0};
        break;
    case SnowCanvasSelectionHandle::None:
        break;
    }
    const qreal denominator = QPointF::dotProduct(vector, vector);
    if (denominator <= 0 || viewScale <= 0)
        return initialImage;
    const qreal maximumEdge = std::max(initialImage.width(), initialImage.height()) * viewScale;
    const qreal scale = std::clamp(1 + QPointF::dotProduct(delta, vector) / denominator,
                                   2.0 / maximumEdge, 1000000.0 / maximumEdge);
    const auto size = initialImage.size() * scale;
    return {initialImage.center() - QPointF(size.width() / 2, size.height() / 2), size};
}
} // namespace snow_shot::presentation::collage_detail
#endif

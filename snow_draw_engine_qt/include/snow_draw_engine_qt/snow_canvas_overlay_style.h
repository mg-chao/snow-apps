// SPDX-License-Identifier: Apache-2.0
#ifndef SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_OVERLAY_STYLE_H
#define SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_OVERLAY_STYLE_H

#include <QPen>
#include <QPainter>
#include <array>

// Host-owned image editors use the same view-space decoration geometry as the engine.
// These values mirror geometry/handles.rs and selection_visuals.rs. They are logical
// pixels: neither the canvas zoom nor the device pixel ratio changes their hit areas.
enum class SnowCanvasSelectionHandle {
    None,
    TopLeft,
    TopRight,
    BottomRight,
    BottomLeft,
    Top,
    Right,
    Bottom,
    Left
};

inline QRectF snowCanvasImageSelectionFrame(const QRectF& imageBounds) {
    return imageBounds.adjusted(-4.0, -4.0, 4.0, 4.0);
}

inline std::array<QPointF, 4> snowCanvasImageSelectionCorners(const QRectF& imageBounds) {
    const auto frame = snowCanvasImageSelectionFrame(imageBounds);
    return {frame.topLeft(), frame.topRight(), frame.bottomRight(), frame.bottomLeft()};
}

inline SnowCanvasSelectionHandle snowCanvasImageSelectionHandleAt(const QRectF& imageBounds,
                                                                  const QPointF& point) {
    if (imageBounds.isEmpty())
        return SnowCanvasSelectionHandle::None;
    const auto corners = snowCanvasImageSelectionCorners(imageBounds);
    constexpr std::array handles{
        SnowCanvasSelectionHandle::TopLeft, SnowCanvasSelectionHandle::TopRight,
        SnowCanvasSelectionHandle::BottomRight, SnowCanvasSelectionHandle::BottomLeft};
    for (std::size_t index = 0; index < corners.size(); ++index)
        if (QRectF(corners[index] - QPointF(6, 6), QSizeF(12, 12)).contains(point))
            return handles[index];
    const auto frame = snowCanvasImageSelectionFrame(imageBounds);
    if (QRectF(frame.left() - 6, frame.top() - 6, frame.width() + 12, 12).contains(point))
        return SnowCanvasSelectionHandle::Top;
    if (QRectF(frame.right() - 6, frame.top() - 6, 12, frame.height() + 12).contains(point))
        return SnowCanvasSelectionHandle::Right;
    if (QRectF(frame.left() - 6, frame.bottom() - 6, frame.width() + 12, 12).contains(point))
        return SnowCanvasSelectionHandle::Bottom;
    if (QRectF(frame.left() - 6, frame.top() - 6, 12, frame.height() + 12).contains(point))
        return SnowCanvasSelectionHandle::Left;
    return SnowCanvasSelectionHandle::None;
}

inline void snowCanvasPaintImageSelection(QPainter& painter, const QRectF& imageBounds) {
    if (imageBounds.isEmpty())
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0x40, 0x96, 0xff), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(snowCanvasImageSelectionFrame(imageBounds));
    painter.setBrush(Qt::white);
    for (const auto& center : snowCanvasImageSelectionCorners(imageBounds))
        painter.drawRoundedRect(QRectF(center - QPointF(4, 4), QSizeF(8, 8)), 2, 2);
    painter.restore();
}

// Selection bounds describe the stroke centerline in logical view coordinates.
// Keep fractional coordinates and antialias the stroke instead of snapping or
// insetting it: a logical pixel need not cover an integer number of device pixels.
inline void snowCanvasPaintSelectionMarquee(QPainter& painter, const QRectF& bounds,
                                            const QColor& stroke = QColor(0x40, 0x96, 0xff),
                                            const QColor& fill = QColor(0x40, 0x96, 0xff, 0x33),
                                            qreal strokeWidth = 1.0) {
    if (bounds.isEmpty())
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(stroke, strokeWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
    if (stroke.alpha() == 0 || strokeWidth <= 0)
        pen.setStyle(Qt::NoPen);
    painter.setPen(pen);
    painter.setBrush(fill);
    painter.drawRect(bounds);
    painter.restore();
}

// Shared by the engine's selection frames and host-owned canvas guides. Widths are
// logical view pixels; antialiasing preserves their coverage at fractional DPI.
inline void snowCanvasApplySelectionDashStyle(QPen& pen) {
    pen.setStyle(Qt::CustomDashLine);
    pen.setCapStyle(Qt::FlatCap);
    pen.setDashPattern({5.0, 3.0});
}

#endif // SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_OVERLAY_STYLE_H

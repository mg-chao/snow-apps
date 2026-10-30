// SPDX-License-Identifier: Apache-2.0
#ifndef SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_OVERLAY_STYLE_H
#define SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_OVERLAY_STYLE_H

#include <QPen>
#include <QPainter>

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

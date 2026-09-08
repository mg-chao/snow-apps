#include "snow_shot/presentation/styles/buttonborder.h"

#include <algorithm>
#include <QPainter>
#include <QPen>
#include <QRectF>

namespace snow_shot::presentation::styles {
void drawButtonBorder(QPainter* painter, const QSize& logicalSize, const ButtonBorderSpec& spec) {
    if (painter == nullptr || logicalSize.isEmpty() || spec.width <= 0 || !spec.color.isValid()) {
        return;
    }

    // Paint in logical coordinates so Qt applies the backing store's DPR, child origin and
    // clip together. A separately rounded device-pixel image can put its last row/column
    // outside that clip at fractional scale factors.
    const qreal width = std::min({spec.width, logicalSize.width(), logicalSize.height()});
    const qreal inset = width / 2.0;
    const QRectF bounds =
        QRectF(QPointF(), QSizeF(logicalSize)).adjusted(inset, inset, -inset, -inset);
    const qreal outerRadius = std::clamp<qreal>(
        spec.radius, 0.0, std::min(logicalSize.width(), logicalSize.height()) / 2.0);
    const qreal radius = std::max(0.0, outerRadius - inset);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setBrush(Qt::NoBrush);
    QPen pen(spec.color);
    pen.setWidthF(width);
    pen.setJoinStyle(Qt::RoundJoin);
    if (spec.pattern == BorderPattern::Dashed) {
        pen.setCapStyle(Qt::FlatCap);
        pen.setDashPattern({3.0, 2.0});
    }
    painter->setPen(pen);
    // QPainter centers the stroke on the path; the half-width inset keeps it inside bounds.
    painter->drawRoundedRect(bounds, radius, radius);
    painter->restore();
}
} // namespace snow_shot::presentation::styles

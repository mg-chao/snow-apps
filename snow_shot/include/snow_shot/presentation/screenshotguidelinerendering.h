#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTGUIDELINERENDERING_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTGUIDELINERENDERING_H

#include <QColor>
#include <QLineF>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QRegion>
#include <QVector>

#include <cmath>

[[nodiscard]] inline qreal screenshotGuideLinePixelCenter(qreal coordinate) {
    return std::floor(coordinate) + 0.5;
}

namespace screenshot_guide_line_rendering {
struct CrosshairGeometry {
    QLineF vertical;
    QLineF horizontal;
};

[[nodiscard]] inline CrosshairGeometry crosshairGeometry(const QRectF& bounds,
                                                         const QPointF& center) {
    const qreal left = screenshotGuideLinePixelCenter(bounds.left());
    const qreal top = screenshotGuideLinePixelCenter(bounds.top());
    const qreal right = std::ceil(bounds.right()) - 0.5;
    const qreal bottom = std::ceil(bounds.bottom()) - 0.5;
    const qreal x = screenshotGuideLinePixelCenter(center.x());
    const qreal y = screenshotGuideLinePixelCenter(center.y());
    return {
        QLineF(QPointF(x, top), QPointF(x, bottom)),
        QLineF(QPointF(left, y), QPointF(right, y)),
    };
}

[[nodiscard]] inline bool lineIntersectsExposure(const QLineF& line, const QRegion* exposedRegion) {
    if (exposedRegion == nullptr) {
        return true;
    }
    const QRect damageBounds =
        QRectF(line.p1(), line.p2()).normalized().adjusted(-1.0, -1.0, 1.0, 1.0).toAlignedRect();
    return exposedRegion->intersects(damageBounds);
}

inline void drawCrosshair(QPainter& painter, const CrosshairGeometry& geometry, const QColor& color,
                          bool dashed, const QRegion* exposedRegion = nullptr) {
    if (!color.isValid() || color.alpha() == 0) {
        return;
    }

    QPen pen(color, 1.0);
    pen.setCosmetic(true);
    if (dashed) {
        static const QVector<qreal> dashPattern{10.0, 3.0};
        pen.setDashPattern(dashPattern);
    }
    painter.setPen(pen);
    if (lineIntersectsExposure(geometry.vertical, exposedRegion)) {
        painter.drawLine(geometry.vertical);
    }
    if (lineIntersectsExposure(geometry.horizontal, exposedRegion)) {
        painter.drawLine(geometry.horizontal);
    }
}
} // namespace screenshot_guide_line_rendering

inline void paintScreenshotGuideLineCrosshair(QPainter& painter, const QRectF& bounds,
                                              const QPointF& center, const QColor& color,
                                              bool dashed) {
    if (!color.isValid() || color.alpha() == 0 || bounds.isEmpty()) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);
    screenshot_guide_line_rendering::drawCrosshair(
        painter, screenshot_guide_line_rendering::crosshairGeometry(bounds, center), color, dashed);
    painter.restore();
}

inline void paintScreenshotGuideLines(QPainter& painter, const QRectF& bounds,
                                      const QPointF& cursorCenter, const QColor& cursorColor,
                                      const QColor& monitorCenterColor,
                                      const QRegion* exposedRegion = nullptr) {
    if (bounds.isEmpty() || ((!cursorColor.isValid() || cursorColor.alpha() == 0) &&
                             (!monitorCenterColor.isValid() || monitorCenterColor.alpha() == 0))) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);
    screenshot_guide_line_rendering::drawCrosshair(
        painter, screenshot_guide_line_rendering::crosshairGeometry(bounds, cursorCenter),
        cursorColor, true, exposedRegion);
    screenshot_guide_line_rendering::drawCrosshair(
        painter, screenshot_guide_line_rendering::crosshairGeometry(bounds, bounds.center()),
        monitorCenterColor, false, exposedRegion);
    painter.restore();
}

inline void paintScreenshotColorPickerCenterGuideLines(QPainter& painter, const QRectF& preview,
                                                       const QRectF& centerSample,
                                                       const QColor& color) {
    if (!color.isValid() || color.alpha() == 0 || preview.isEmpty() || centerSample.isEmpty()) {
        return;
    }

    QPen pen(color, 1.0);
    pen.setCosmetic(true);
    const qreal left = screenshotGuideLinePixelCenter(preview.left());
    const qreal top = screenshotGuideLinePixelCenter(preview.top());
    const qreal right = std::ceil(preview.right()) - 0.5;
    const qreal bottom = std::ceil(preview.bottom()) - 0.5;
    const qreal x = screenshotGuideLinePixelCenter(centerSample.center().x());
    const qreal y = screenshotGuideLinePixelCenter(centerSample.center().y());
    const qreal centerTop = centerSample.top() - 0.5;
    const qreal centerBottom = centerSample.bottom() + 0.5;
    const qreal centerLeft = centerSample.left() - 0.5;
    const qreal centerRight = centerSample.right() + 0.5;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    if (top <= centerTop) {
        painter.drawLine(QPointF(x, top), QPointF(x, centerTop));
    }
    if (centerBottom <= bottom) {
        painter.drawLine(QPointF(x, centerBottom), QPointF(x, bottom));
    }
    if (left <= centerLeft) {
        painter.drawLine(QPointF(left, y), QPointF(centerLeft, y));
    }
    if (centerRight <= right) {
        painter.drawLine(QPointF(centerRight, y), QPointF(right, y));
    }
    painter.restore();
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTGUIDELINERENDERING_H

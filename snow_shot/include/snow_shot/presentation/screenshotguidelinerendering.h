#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTGUIDELINERENDERING_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTGUIDELINERENDERING_H

#include <QColor>
#include <QLineF>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QRegion>
#include <QTransform>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

[[nodiscard]] inline qreal screenshotGuideLinePixelCenter(qreal coordinate) {
    return std::floor(coordinate) + 0.5;
}

namespace screenshot_guide_line_rendering {
constexpr qreal kDashLength = 10.0;
constexpr qreal kDashGap = 3.0;

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

[[nodiscard]] inline bool exposedLine(const QLineF& line, const QTransform& deviceTransform,
                                      bool dashed, const QRegion* exposedRegion, QLineF& result) {
    result = line;
    if (exposedRegion == nullptr) {
        return true;
    }
    const QRect damageBounds =
        QRectF(line.p1(), line.p2()).normalized().adjusted(-1.0, -1.0, 1.0, 1.0).toAlignedRect();
    if (!exposedRegion->intersects(damageBounds)) {
        return false;
    }
    // Screen-space guides use positive axis-aligned transforms. Preserve full-line
    // rendering for other painter transforms rather than altering their dash geometry.
    if (deviceTransform.type() > QTransform::TxScale || deviceTransform.m11() <= 0.0 ||
        deviceTransform.m22() <= 0.0) {
        return true;
    }

    const bool vertical = line.x1() == line.x2();
    qreal exposedStart = std::numeric_limits<qreal>::max();
    qreal exposedEnd = std::numeric_limits<qreal>::lowest();
    for (const QRect& rectangle : *exposedRegion) {
        const QRect intersection = rectangle.intersected(damageBounds);
        if (intersection.isEmpty()) {
            continue;
        }
        exposedStart =
            std::min(exposedStart, qreal(vertical ? intersection.top() : intersection.left()));
        exposedEnd = std::max(
            exposedEnd, qreal(vertical ? intersection.bottom() + 1 : intersection.right() + 1));
    }
    const qreal scale = vertical ? deviceTransform.m22() : deviceTransform.m11();
    const qreal offset = vertical ? deviceTransform.dy() : deviceTransform.dx();
    const qreal originalStart = (vertical ? line.y1() : line.x1()) * scale + offset;
    const qreal originalEnd = (vertical ? line.y2() : line.x2()) * scale + offset;
    // Qt clips lines extending beyond the device before initializing its cosmetic
    // dash phase. Keep that behavior for a start outside the device or reversed axes.
    if (originalStart < 0.0 || originalEnd <= originalStart) {
        return true;
    }
    exposedStart = exposedStart * scale + offset;
    exposedEnd = exposedEnd * scale + offset;

    // Cosmetic dash lengths are physical pixels. Moving the start by whole periods
    // keeps the phase anchored to the original viewport, including fractional DPRs.
    // Padding puts synthetic square caps outside the painter's existing exposure clip.
    const qreal period = dashed ? kDashLength + kDashGap : 1.0;
    const qreal start = std::max(
        originalStart,
        originalStart + std::floor((exposedStart - originalStart - 1.0) / period) * period);
    const qreal end =
        std::min(originalEnd,
                 originalStart + std::ceil((exposedEnd - originalStart + 1.0) / period) * period);
    if (start > end) {
        return false;
    }
    if (vertical) {
        if (start > originalStart) {
            result.setP1(QPointF(line.x1(), line.y1() + (start - originalStart) / scale));
        }
        if (end < originalEnd) {
            result.setP2(QPointF(line.x2(), line.y2() - (originalEnd - end) / scale));
        }
    } else {
        if (start > originalStart) {
            result.setP1(QPointF(line.x1() + (start - originalStart) / scale, line.y1()));
        }
        if (end < originalEnd) {
            result.setP2(QPointF(line.x2() - (originalEnd - end) / scale, line.y2()));
        }
    }
    return true;
}

inline void drawCrosshair(QPainter& painter, const CrosshairGeometry& geometry, const QColor& color,
                          bool dashed, const QRegion* exposedRegion = nullptr) {
    if (!color.isValid() || color.alpha() == 0) {
        return;
    }

    QLineF lines[2];
    int lineCount = 0;
    const QTransform deviceTransform = painter.deviceTransform();
    if (exposedLine(geometry.vertical, deviceTransform, dashed, exposedRegion, lines[lineCount])) {
        ++lineCount;
    }
    if (exposedLine(geometry.horizontal, deviceTransform, dashed, exposedRegion,
                    lines[lineCount])) {
        ++lineCount;
    }
    if (lineCount == 0) {
        return;
    }
    QPen pen(color, 1.0);
    pen.setCosmetic(true);
    if (dashed) {
        static const QVector<qreal> dashPattern{kDashLength, kDashGap};
        pen.setDashPattern(dashPattern);
    }
    painter.setPen(pen);
    painter.drawLines(lines, lineCount);
}
} // namespace screenshot_guide_line_rendering

inline void paintScreenshotGuideLineCrosshair(QPainter& painter, const QRectF& bounds,
                                              const QPointF& center, const QColor& color,
                                              bool dashed, const QRegion* exposedRegion = nullptr) {
    if (!color.isValid() || color.alpha() == 0 || bounds.isEmpty()) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);
    screenshot_guide_line_rendering::drawCrosshair(
        painter, screenshot_guide_line_rendering::crosshairGeometry(bounds, center), color, dashed,
        exposedRegion);
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

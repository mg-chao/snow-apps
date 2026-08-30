#include "snow_shot/presentation/screenshotocrvisuals.h"

#include "snow_draw_engine_qt/snow_canvas_region_filter.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"

#include <QPainter>
#include <QPolygon>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace {
constexpr qreal kOcrRegionExpansionFraction = 0.08;
constexpr qreal kOcrRegionExpansionMinimum = 1.0;
constexpr qreal kOcrRegionExpansionMaximum = 4.0;
constexpr qreal kBackgroundBlendAmount = 0.5;

qreal edgeLength(const QPointF& first, const QPointF& second) {
    return std::hypot(second.x() - first.x(), second.y() - first.y());
}

QPolygonF expandedQuad(const QPolygonF& quad) {
    if (quad.size() != 4) {
        return quad;
    }
    const qreal width = std::max(edgeLength(quad.at(0), quad.at(1)),
                                 edgeLength(quad.at(3), quad.at(2)));
    const qreal height = std::max(edgeLength(quad.at(0), quad.at(3)),
                                  edgeLength(quad.at(1), quad.at(2)));
    const qreal margin = std::clamp(std::min(width, height) * kOcrRegionExpansionFraction,
                                    kOcrRegionExpansionMinimum, kOcrRegionExpansionMaximum);
    const QPointF center = quad.boundingRect().center();
    QPolygonF expanded;
    expanded.reserve(quad.size());
    for (const QPointF& point : quad) {
        const QPointF delta = point - center;
        const qreal length = std::hypot(delta.x(), delta.y());
        expanded.push_back(length > 0.0 ? point + delta * (margin / length)
                                       : point + QPointF(margin, margin));
    }
    return expanded;
}

QPointF imagePointForCanvasPoint(const QPointF& point, const QRectF& canvasRect,
                                 const QSize& pixelSize) {
    return QPointF((point.x() - canvasRect.left()) * pixelSize.width() / canvasRect.width(),
                   (point.y() - canvasRect.top()) * pixelSize.height() / canvasRect.height());
}

QPolygon imagePolygonForQuad(const QPolygonF& quad, const QRectF& canvasRect,
                             const QSize& pixelSize, bool expand) {
    const QPolygonF source = expand ? expandedQuad(quad) : quad;
    QPolygon polygon;
    polygon.reserve(source.size());
    for (const QPointF& point : source) {
        polygon.push_back(imagePointForCanvasPoint(point, canvasRect, pixelSize).toPoint());
    }
    return polygon;
}

} // namespace

QRegion screenshotOcrFilterRegion(const ScreenshotOcrPresentation& presentation,
                                  const QRectF& canvasRect, const QSize& pixelSize) {
    const QRectF normalized = canvasRect.normalized();
    if (!normalized.isValid() || normalized.isEmpty() || !pixelSize.isValid() ||
        pixelSize.isEmpty()) {
        return {};
    }
    QRegion region;
    for (const ScreenshotOcrLine& line : presentation.lines) {
        if (line.quad.size() >= 3) {
            region += QRegion(imagePolygonForQuad(line.quad, normalized, pixelSize, true));
        }
    }
    return region.intersected(QRect(QPoint(), pixelSize));
}

QImage renderScreenshotOcrFilteredImage(const QImage& source, const QRectF& canvasRect,
                                        const ScreenshotOcrPresentation& presentation,
                                        const QColor& backgroundColor,
                                        qreal devicePixelRatio) {
    if (source.isNull()) {
        return {};
    }
    QImage filtered = source.format() == QImage::Format_ARGB32_Premultiplied
                          ? source.copy()
                          : source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QRegion region = screenshotOcrFilterRegion(presentation, canvasRect, filtered.size());
    if (region.isEmpty()) {
        return filtered;
    }
    SnowCanvasRegionFilterParameters parameters;
    parameters.type = SnowCanvasFilterType::GaussianBlur;
    parameters.strength = 1.0;
    parameters.logicalSigma = 8.0;
    parameters.devicePixelRatio = std::max<qreal>(1.0, devicePixelRatio);
    const QImage original = filtered.copy();
    if (!applySnowCanvasRegionFilter(original, filtered, region, parameters)) {
        return {};
    }
    QPainter painter(&filtered);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setClipRegion(region);
    QColor blendColor = backgroundColor.isValid() ? backgroundColor : QColor(Qt::white);
    blendColor.setAlpha(qBound(0, qRound(kBackgroundBlendAmount * 255.0), 255));
    painter.fillRect(filtered.rect(), blendColor);
    return filtered;
}

QImage renderScreenshotOcrFilteredSource(const ScreenshotImageSource& source,
                                         const QRectF& canvasRect, const QSize& pixelSize,
                                         const ScreenshotOcrPresentation& presentation,
                                         const QColor& backgroundColor) {
    if (!canvasRect.isValid() || canvasRect.isEmpty()) {
        return {};
    }
    const QImage materialized = materializeScreenshotImageSource(source, canvasRect, pixelSize);
    return renderScreenshotOcrFilteredImage(materialized, canvasRect, presentation, backgroundColor,
                                            pixelSize.width() / canvasRect.width());
}

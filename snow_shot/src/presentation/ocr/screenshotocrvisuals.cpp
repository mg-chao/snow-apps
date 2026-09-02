#include "snow_shot/presentation/screenshotocrvisuals.h"

#include "snow_draw_engine_qt/snow_canvas_region_filter.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"

#include <QPainter>
#include <QPolygon>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <vector>

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

QImage materializeScreenshotImageSource(const ScreenshotImageSource& source,
                                        const QRectF& canvasRect, const QSize& pixelSize) {
    const QRectF normalized = canvasRect.normalized();
    if (!source.isValid() || !normalized.isValid() || normalized.isEmpty() ||
        !pixelSize.isValid() || pixelSize.isEmpty()) {
        return {};
    }
    if (source.isMaterialized() && source.materializedCanvasRect == normalized &&
        source.materializedImage.size() == pixelSize) {
        QImage image = source.materializedImage;
        if (image.format() != QImage::Format_ARGB32_Premultiplied) {
            image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
        image.setDevicePixelRatio(1.0);
        return image;
    }
    QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(1.0);
    image.fill(Qt::transparent);
    const qreal scaleX = pixelSize.width() / normalized.width();
    const qreal scaleY = pixelSize.height() / normalized.height();
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setTransform(QTransform(scaleX, 0.0, 0.0, scaleY,
                                    -normalized.left() * scaleX,
                                    -normalized.top() * scaleY));
    const auto drawLayer = [&painter](const ScreenshotImageLayer& layer) {
        if (!layer.isValid()) {
            return;
        }
        const qreal sourceScaleX = layer.image.width() / layer.imageCanvasRect.width();
        const qreal sourceScaleY = layer.image.height() / layer.imageCanvasRect.height();
        const QRectF sourceRect(
            (layer.destinationCanvasRect.left() - layer.imageCanvasRect.left()) * sourceScaleX,
            (layer.destinationCanvasRect.top() - layer.imageCanvasRect.top()) * sourceScaleY,
            layer.destinationCanvasRect.width() * sourceScaleX,
            layer.destinationCanvasRect.height() * sourceScaleY);
        painter.drawImage(layer.destinationCanvasRect, layer.image, sourceRect);
    };
    if (source.isMaterialized()) {
        drawLayer(ScreenshotImageLayer{source.materializedImage, source.materializedCanvasRect,
                                       source.materializedCanvasRect.intersected(normalized)});
    } else {
        for (const ScreenshotImageLayer& layer : source.layers) {
            ScreenshotImageLayer clipped = layer;
            clipped.destinationCanvasRect = clipped.destinationCanvasRect.intersected(normalized);
            drawLayer(clipped);
        }
    }
    return image;
}

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

QRectF screenshotOcrFilteredImageCanvasRect(const QRectF& canvasRect, const QSize& pixelSize,
                                            const QRect& filteredPixels) {
    const QRectF normalized = canvasRect.normalized();
    if (!normalized.isValid() || normalized.isEmpty() || !pixelSize.isValid() ||
        pixelSize.width() < 1 || pixelSize.height() < 1 || filteredPixels.isEmpty()) {
        return {};
    }
    const qreal scaleX = normalized.width() / pixelSize.width();
    const qreal scaleY = normalized.height() / pixelSize.height();
    return QRectF(normalized.left() + filteredPixels.left() * scaleX,
                  normalized.top() + filteredPixels.top() * scaleY,
                  filteredPixels.width() * scaleX, filteredPixels.height() * scaleY);
}

QImage renderScreenshotOcrFilteredImage(const QImage& source, const QRectF& canvasRect,
                                        const ScreenshotOcrPresentation& presentation,
                                        const QColor& backgroundColor,
                                        qreal devicePixelRatio, QRect* filteredPixels,
                                        SnowCanvasRegionFilterScratch* scratch) {
    if (filteredPixels != nullptr) {
        *filteredPixels = {};
    }
    const QRectF normalized = canvasRect.normalized();
    if (source.isNull() || !normalized.isValid() || normalized.isEmpty()) {
        return {};
    }
    const QRect imageRect(QPoint(0, 0), source.size());

    SnowCanvasRegionFilterParameters parameters;
    parameters.type = SnowCanvasFilterType::GaussianBlur;
    parameters.strength = 1.0;
    parameters.logicalSigma = 8.0;
    parameters.devicePixelRatio = std::max<qreal>(1.0, devicePixelRatio);
    // Filtered output only depends on source pixels within this radius, so it
    // sizes both the cluster merge margin and the crop margin.
    const int support = snowCanvasRegionFilterSupportPixels(parameters);

    struct Cluster {
        QRegion region;
        QRect expandedBounds;
    };
    std::vector<Cluster> clusters;
    for (const ScreenshotOcrLine& line : presentation.lines) {
        if (line.quad.size() < 3) {
            continue;
        }
        const QPolygon polygon =
            imagePolygonForQuad(line.quad, normalized, source.size(), true);
        if (polygon.isEmpty()) {
            continue;
        }
        // Quads whose support-expanded bounds never meet are filtered
        // independently; merging only spatially close quads keeps each blur
        // pass proportional to the area it actually covers.
        Cluster next{QRegion(polygon),
                     polygon.boundingRect().adjusted(-support, -support, support, support)};
        for (std::size_t index = 0; index < clusters.size();) {
            if (!clusters[index].expandedBounds.intersects(next.expandedBounds)) {
                ++index;
                continue;
            }
            next.region += clusters[index].region;
            next.expandedBounds =
                next.expandedBounds.united(clusters[index].expandedBounds);
            clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(index));
            index = 0;
        }
        clusters.push_back(std::move(next));
    }
    if (clusters.empty()) {
        if (filteredPixels != nullptr) {
            *filteredPixels = imageRect;
        }
        return source.format() == QImage::Format_ARGB32_Premultiplied
                   ? source
                   : source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    QRect crop = clusters.front().expandedBounds;
    for (const Cluster& cluster : clusters) {
        crop = crop.united(cluster.expandedBounds);
    }
    crop = crop.intersected(imageRect);
    if (crop.isEmpty()) {
        if (filteredPixels != nullptr) {
            *filteredPixels = imageRect;
        }
        return source.format() == QImage::Format_ARGB32_Premultiplied
                   ? source
                   : source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    QImage blurInput = source.copy(crop);
    if (blurInput.format() != QImage::Format_ARGB32_Premultiplied) {
        blurInput = blurInput.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    blurInput.setDevicePixelRatio(1.0);
    // Sharing with blurInput is safe: the engine detaches destination before its
    // first write, leaving blurInput as the pristine read-side buffer.
    QImage filtered = blurInput;
    // Anchoring the reduced sampling grid to the absolute image origin keeps the
    // cropped render pixel-identical to filtering the full image.
    parameters.gridOriginInImage = QPointF(-crop.left(), -crop.top());
    QRegion fillRegion;
    for (Cluster& cluster : clusters) {
        const QRegion localRegion = cluster.region.translated(-crop.topLeft());
        if (!applySnowCanvasRegionFilter(blurInput, filtered, localRegion, parameters,
                                         scratch)) {
            return {};
        }
        fillRegion += localRegion;
    }

    QPainter painter(&filtered);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setClipRegion(fillRegion);
    QColor blendColor = backgroundColor.isValid() ? backgroundColor : QColor(Qt::white);
    blendColor.setAlpha(qBound(0, qRound(kBackgroundBlendAmount * 255.0), 255));
    painter.fillRect(QRect(QPoint(0, 0), crop.size()), blendColor);
    if (filteredPixels != nullptr) {
        *filteredPixels = crop;
    }
    return filtered;
}


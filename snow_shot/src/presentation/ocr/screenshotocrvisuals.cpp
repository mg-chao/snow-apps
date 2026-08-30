#include "snow_shot/presentation/screenshotocrvisuals.h"

#include "snow_draw_engine_qt/snow_canvas_region_filter.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"

#include <QPainter>
#include <QPolygon>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {
constexpr qreal kOcrRegionExpansionFraction = 0.08;
constexpr qreal kOcrRegionExpansionMinimum = 1.0;
constexpr qreal kOcrRegionExpansionMaximum = 4.0;
constexpr qreal kWhiteBlendAmount = 0.5;

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

qreal linearColor(int value) {
    const qreal normalized = value / 255.0;
    return normalized <= 0.04045 ? normalized / 12.92
                                 : std::pow((normalized + 0.055) / 1.055, 2.4);
}

qreal relativeLuminance(const QColor& color) {
    return 0.2126 * linearColor(color.red()) + 0.7152 * linearColor(color.green()) +
           0.0722 * linearColor(color.blue());
}

qreal contrastRatio(const QColor& first, const QColor& second) {
    const qreal firstLuminance = relativeLuminance(first);
    const qreal secondLuminance = relativeLuminance(second);
    const qreal light = std::max(firstLuminance, secondLuminance);
    const qreal dark = std::min(firstLuminance, secondLuminance);
    return (light + 0.05) / (dark + 0.05);
}

int median(std::vector<int>& values, int fallback) {
    if (values.empty()) {
        return fallback;
    }
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

qreal quadShortSide(const QPolygonF& quad) {
    if (quad.size() != 4) {
        return 1.0;
    }
    return std::min(edgeLength(quad.at(0), quad.at(3)), edgeLength(quad.at(1), quad.at(2)));
}

QColor robustExteriorColor(const QImage& image, const QPolygonF& lineQuad,
                           const QPolygon& expandedPolygon, const QRegion& allRegions) {
    const qreal radius = std::clamp(quadShortSide(lineQuad) * 0.45, 3.0, 18.0);
    const QRect bounds = expandedPolygon.boundingRect().adjusted(-qRound(radius), -qRound(radius),
                                                                  qRound(radius), qRound(radius));
    std::vector<int> red;
    std::vector<int> green;
    std::vector<int> blue;
    const QRect clipped = bounds.intersected(image.rect());
    const int sampleStride = std::max(1, std::max(clipped.width(), clipped.height()) / 96);
    for (int y = clipped.top(); y <= clipped.bottom(); y += sampleStride) {
        for (int x = clipped.left(); x <= clipped.right(); x += sampleStride) {
            const QPoint point(x, y);
            if (expandedPolygon.containsPoint(point, Qt::OddEvenFill) ||
                allRegions.contains(point)) {
                continue;
            }
            const QColor color = image.pixelColor(point);
            red.push_back(color.red());
            green.push_back(color.green());
            blue.push_back(color.blue());
        }
    }
    if (!red.empty()) {
        return QColor(median(red, 255), median(green, 255), median(blue, 255));
    }
    return QColor(Qt::white);
}

QColor estimateForeground(const QImage& image, const QPolygon& linePolygon,
                          const QColor& background) {
    struct Candidate {
        qreal distance = 0.0;
        QColor color;
    };
    std::vector<Candidate> candidates;
    const QRect clipped = linePolygon.boundingRect().intersected(image.rect());
    const int sampleStride = std::max(1, std::max(clipped.width(), clipped.height()) / 96);
    for (int y = clipped.top(); y <= clipped.bottom(); y += sampleStride) {
        for (int x = clipped.left(); x <= clipped.right(); x += sampleStride) {
            const QPoint point(x, y);
            if (!linePolygon.containsPoint(point, Qt::OddEvenFill)) {
                continue;
            }
            const QColor color = image.pixelColor(point);
            const qreal red = color.red() - background.red();
            const qreal green = color.green() - background.green();
            const qreal blue = color.blue() - background.blue();
            candidates.push_back({red * red + green * green + blue * blue, color});
        }
    }
    if (candidates.empty()) {
        return Qt::black;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& first, const Candidate& second) {
                  return first.distance > second.distance;
              });
    const std::size_t keep = std::max<std::size_t>(1, candidates.size() / 3);
    std::vector<int> red;
    std::vector<int> green;
    std::vector<int> blue;
    red.reserve(keep);
    green.reserve(keep);
    blue.reserve(keep);
    for (std::size_t index = 0; index < keep; ++index) {
        red.push_back(candidates[index].color.red());
        green.push_back(candidates[index].color.green());
        blue.push_back(candidates[index].color.blue());
    }
    const QColor filteredBackground(
        qRound(background.red() * (1.0 - kWhiteBlendAmount) + 255.0 * kWhiteBlendAmount),
        qRound(background.green() * (1.0 - kWhiteBlendAmount) + 255.0 * kWhiteBlendAmount),
        qRound(background.blue() * (1.0 - kWhiteBlendAmount) + 255.0 * kWhiteBlendAmount));
    const QColor estimated(median(red, 0), median(green, 0), median(blue, 0));
    if (contrastRatio(estimated, filteredBackground) >= 4.5) {
        return estimated;
    }

    // Text is drawn over the filtered background, so retain the sampled color only when it
    // remains readable. This keeps the foreground calculation local to snow_shot while
    // preserving the renderer's former contrast guarantee.
    const QColor black(Qt::black);
    const QColor white(Qt::white);
    return contrastRatio(black, filteredBackground) >= contrastRatio(white, filteredBackground)
               ? black
               : white;
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

ScreenshotOcrForegrounds resolveScreenshotOcrForegrounds(
    const QImage& source, const QRectF& canvasRect,
    const ScreenshotOcrPresentation& presentation) {
    ScreenshotOcrForegrounds foregrounds(presentation.lines.size(), Qt::black);
    const QRectF normalized = canvasRect.normalized();
    if (source.isNull() || !normalized.isValid() || normalized.isEmpty()) {
        return foregrounds;
    }
    const QImage image = source.format() == QImage::Format_ARGB32_Premultiplied
                             ? source
                             : source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QRegion allRegions = screenshotOcrFilterRegion(presentation, normalized, image.size());
    for (int index = 0; index < presentation.lines.size(); ++index) {
        const ScreenshotOcrLine& line = presentation.lines.at(index);
        if (line.quad.size() < 3) {
            continue;
        }
        const QPolygon textPolygon = imagePolygonForQuad(line.quad, normalized, image.size(), false);
        const QPolygon expandedPolygon = imagePolygonForQuad(line.quad, normalized, image.size(), true);
        const QColor background =
            robustExteriorColor(image, line.quad, expandedPolygon, allRegions);
        foregrounds[index] = estimateForeground(image, textPolygon, background);
    }
    return foregrounds;
}

QImage renderScreenshotOcrFilteredImage(const QImage& source, const QRectF& canvasRect,
                                        const ScreenshotOcrPresentation& presentation,
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
    const int whiteAlpha = qBound(0, qRound(kWhiteBlendAmount * 255.0), 255);
    painter.fillRect(filtered.rect(), QColor(255, 255, 255, whiteAlpha));
    return filtered;
}

QImage renderScreenshotOcrFilteredSource(const ScreenshotImageSource& source,
                                         const QRectF& canvasRect, const QSize& pixelSize,
                                         const ScreenshotOcrPresentation& presentation) {
    if (!canvasRect.isValid() || canvasRect.isEmpty()) {
        return {};
    }
    const QImage materialized = materializeScreenshotImageSource(source, canvasRect, pixelSize);
    return renderScreenshotOcrFilteredImage(materialized, canvasRect, presentation,
                                            pixelSize.width() / canvasRect.width());
}

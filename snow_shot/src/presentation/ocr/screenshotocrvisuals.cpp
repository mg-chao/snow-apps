#include "snow_shot/presentation/screenshotocrvisuals.h"

#include "snow_draw_engine_qt/snow_canvas_region_filter.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"

#include <QPainter>
#include <QPolygon>
#include <QPolygonF>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {
constexpr qreal kOcrRegionExpansionFraction = 0.08;
constexpr qreal kOcrRegionExpansionMinimum = 1.0;
constexpr qreal kOcrRegionExpansionMaximum = 4.0;
constexpr qreal kBackgroundBlendAmount = 0.5;

// Work is independent of the image/paragraph area: 4 sides * 3 bands * 24 positions,
// plus an 8 * 8 interior grid. Only four histogram seeds are refined, never all pairs.
constexpr int kSamplesPerSide = 24;
constexpr int kInteriorGrid = 8;
constexpr float kColorRadiusSquared = 0.025f * 0.025f;

struct BackgroundSample {
    QRgb rgb;
    std::array<float, 3> lab;
    int weight;
};

std::array<float, 3> perceptualColor(QRgb rgb) {
    static const auto linear = [] {
        std::array<float, 256> values{};
        for (int i = 0; i < 256; ++i) {
            const float value = static_cast<float>(i) / 255.0f;
            values[i] =
                value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
        }
        return values;
    }();
    const float r = linear[qRed(rgb)], g = linear[qGreen(rgb)], b = linear[qBlue(rgb)];
    // OKLab separates perceptual lightness from chroma, including dark and saturated panels.
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

float colorDistanceSquared(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float l = a[0] - b[0], u = a[1] - b[1], v = a[2] - b[2];
    return l * l + u * u + v * v;
}

bool finitePoint(const QPointF& point) {
    // Keep rounding and QRegion arithmetic away from integer overflow as well as NaNs.
    constexpr qreal limit = std::numeric_limits<int>::max() / 4.0;
    return std::isfinite(point.x()) && std::isfinite(point.y()) && std::abs(point.x()) < limit &&
           std::abs(point.y()) < limit;
}

bool finitePolygon(const QPolygonF& polygon) {
    return std::all_of(polygon.cbegin(), polygon.cend(), finitePoint);
}

QPolygon clippedPolygon(const QPolygonF& polygon, const QSize& size) {
    const QRectF bounds{QPointF(), QSizeF(size)};
    // Clip vectors before scan conversion: a huge slanted OCR quad must never allocate a
    // QRegion proportional to its off-image extent. Ordinary in-image geometry stays identical.
    return bounds.contains(polygon.boundingRect())
               ? polygon.toPolygon()
               : polygon.intersected(QPolygonF(bounds)).toPolygon();
}

bool convexQuad(const QPolygonF& quad) {
    if (quad.size() != 4 || !finitePolygon(quad)) {
        return false;
    }
    qreal winding = 0;
    for (int i = 0; i < 4; ++i) {
        const QPointF a = quad[(i + 1) % 4] - quad[i];
        const QPointF b = quad[(i + 2) % 4] - quad[(i + 1) % 4];
        const qreal cross = a.x() * b.y() - a.y() * b.x();
        if (QPointF::dotProduct(a, a) < 1e-12 || std::abs(cross) < 1e-8 ||
            (i > 0 && cross * winding <= 0)) {
            return false;
        }
        winding = cross;
    }
    return true;
}

bool quadIntersectsImage(const QPolygonF& quad, const QSize& size) {
    if (!convexQuad(quad)) {
        return false;
    }
    const QRectF imageRect{QPointF(), QSizeF(size)};
    const QRectF bounds = quad.boundingRect();
    return bounds.intersects(imageRect) &&
           (imageRect.contains(bounds) || !quad.intersected(QPolygonF(imageRect)).isEmpty());
}

// QRegion::contains scans banded raster rectangles; on skewed pages its cost grows with the
// entire page for every sample. A balanced bounding-volume tree keeps storage proportional to
// OCR block count and tests the four original edges only for nearby candidates.
class TextRegionIndex {
  public:
    TextRegionIndex(const QVector<QPolygonF>& quads, const QRect& imageRect) : m_quads(quads) {
        m_entries.reserve(quads.size());
        for (qsizetype i = 0; i < quads.size(); ++i) {
            const QRectF bounds = quads[i].boundingRect().intersected(QRectF(imageRect));
            if (!bounds.isEmpty()) {
                m_entries.push_back({bounds, i});
            }
        }
        m_nodes.reserve(m_entries.size() * 2);
        if (!m_entries.empty()) {
            build(0, m_entries.size());
        }
    }

    bool contains(const QPoint& pixel) const {
        return !m_nodes.empty() && contains(0, QPointF(pixel) + QPointF(0.5, 0.5));
    }

  private:
    struct Entry {
        QRectF bounds;
        qsizetype quad;
    };
    struct Node {
        QRectF bounds;
        qsizetype quad = -1;
        std::size_t left = 0;
        std::size_t right = 0;
    };

    std::size_t build(std::size_t begin, std::size_t end) {
        QRectF bounds = m_entries[begin].bounds;
        for (std::size_t i = begin + 1; i < end; ++i) {
            bounds = bounds.united(m_entries[i].bounds);
        }
        const std::size_t index = m_nodes.size();
        m_nodes.push_back({bounds});
        if (end - begin == 1) {
            m_nodes[index].quad = m_entries[begin].quad;
            return index;
        }
        const std::size_t middle = begin + (end - begin) / 2;
        const bool horizontal = bounds.width() >= bounds.height();
        std::nth_element(m_entries.begin() + static_cast<std::ptrdiff_t>(begin),
                         m_entries.begin() + static_cast<std::ptrdiff_t>(middle),
                         m_entries.begin() + static_cast<std::ptrdiff_t>(end),
                         [horizontal](const Entry& a, const Entry& b) {
                             const qreal first =
                                 horizontal ? a.bounds.center().x() : a.bounds.center().y();
                             const qreal second =
                                 horizontal ? b.bounds.center().x() : b.bounds.center().y();
                             return first == second ? a.quad < b.quad : first < second;
                         });
        m_nodes[index].left = build(begin, middle);
        m_nodes[index].right = build(middle, end);
        return index;
    }

    bool contains(std::size_t index, const QPointF& point) const {
        const Node& node = m_nodes[index];
        if (!node.bounds.contains(point)) {
            return false;
        }
        return node.quad >= 0 ? m_quads[node.quad].containsPoint(point, Qt::OddEvenFill)
                              : contains(node.left, point) || contains(node.right, point);
    }

    const QVector<QPolygonF>& m_quads;
    std::vector<Entry> m_entries;
    std::vector<Node> m_nodes;
};

// Read only the few sampled pixels; converting the entire screenshot would dominate latency.
QRgb opaquePixel(const QImage& image, const QPoint& point) {
    switch (image.format()) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
        return reinterpret_cast<const QRgb*>(image.constScanLine(point.y()))[point.x()];
    default:
        return image.pixelColor(point).rgba();
    }
}

bool flatPixel(const QImage& image, const QPoint& point, QRgb& rgb) {
    rgb = opaquePixel(image, point);
    // Partially transparent premultiplied pixels cannot identify an opaque background.
    if (qAlpha(rgb) != 255) {
        return false;
    }
    const std::array<QPoint, 4> offsets{QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1)};
    return std::all_of(offsets.begin(), offsets.end(), [&](const QPoint& offset) {
        const QPoint neighbor = point + offset;
        if (!image.rect().contains(neighbor)) {
            return true;
        }
        const QRgb other = opaquePixel(image, neighbor);
        return qAlpha(other) == 255 && std::abs(qRed(rgb) - qRed(other)) <= 18 &&
               std::abs(qGreen(rgb) - qGreen(other)) <= 18 &&
               std::abs(qBlue(rgb) - qBlue(other)) <= 18;
    });
}

QRgb dominantColor(const BackgroundSample* samples, int count, const BackgroundSample* exterior,
                   int exteriorCount) {
    const QRgb first = samples[0].rgb;
    if (std::all_of(samples, samples + count,
                    [first](const BackgroundSample& entry) { return entry.rgb == first; })) {
        return first;
    }
    struct Bin {
        int weight = 0;
        int r = 0, g = 0, b = 0;
    };
    std::array<Bin, 4096> histogram{};
    for (int i = 0; i < count; ++i) {
        const auto& entry = samples[i];
        const QRgb rgb = entry.rgb;
        auto& bin =
            histogram[((qRed(rgb) >> 4) << 8) | ((qGreen(rgb) >> 4) << 4) | (qBlue(rgb) >> 4)];
        bin.weight += entry.weight;
        bin.r += qRed(rgb) * entry.weight;
        bin.g += qGreen(rgb) * entry.weight;
        bin.b += qBlue(rgb) * entry.weight;
    }
    std::array<int, 4> seeds{-1, -1, -1, -1};
    for (int i = 0; i < static_cast<int>(histogram.size()); ++i) {
        if (histogram[i].weight == 0) {
            continue;
        }
        for (int rank = 0; rank < 4; ++rank) {
            if (seeds[rank] < 0 || histogram[i].weight > histogram[seeds[rank]].weight) {
                for (int move = 3; move > rank; --move) {
                    seeds[move] = seeds[move - 1];
                }
                seeds[rank] = i;
                break;
            }
        }
    }
    int bestSupport = -1, bestExteriorSupport = -1;
    QRgb bestRgb = first;
    for (const int seed : seeds) {
        if (seed < 0) {
            break;
        }
        const auto& bin = histogram[seed];
        auto lab =
            perceptualColor(qRgb(bin.r / bin.weight, bin.g / bin.weight, bin.b / bin.weight));
        // Fixed mean-shift steps merge compression noise across quantization boundaries.
        for (int step = 0; step < 2; ++step) {
            int weight = 0, r = 0, g = 0, b = 0;
            for (int i = 0; i < count; ++i) {
                if (colorDistanceSquared(samples[i].lab, lab) <= kColorRadiusSquared) {
                    const auto& entry = samples[i];
                    weight += entry.weight;
                    r += qRed(entry.rgb) * entry.weight;
                    g += qGreen(entry.rgb) * entry.weight;
                    b += qBlue(entry.rgb) * entry.weight;
                }
            }
            if (weight == 0) {
                break;
            }
            lab = perceptualColor(qRgb((r + weight / 2) / weight, (g + weight / 2) / weight,
                                       (b + weight / 2) / weight));
        }
        int support = 0, exteriorSupport = 0;
        float nearestDistance = std::numeric_limits<float>::max();
        QRgb representative = first;
        for (int i = 0; i < count; ++i) {
            const float distance = colorDistanceSquared(samples[i].lab, lab);
            if (distance <= kColorRadiusSquared) {
                support += samples[i].weight;
                // Return an observed color, not the mean of distinct background blocks.
                if (distance < nearestDistance ||
                    (distance == nearestDistance && samples[i].rgb < representative)) {
                    nearestDistance = distance;
                    representative = samples[i].rgb;
                }
            }
        }
        for (int i = 0; i < exteriorCount; ++i) {
            if (colorDistanceSquared(exterior[i].lab, lab) <= kColorRadiusSquared) {
                exteriorSupport += exterior[i].weight;
            }
        }
        // Exterior evidence only breaks exact interior ties; it cannot override a majority or
        // introduce a color absent from the interior. The final RGB ordering is deterministic.
        if (support > bestSupport ||
            (support == bestSupport &&
             (exteriorSupport > bestExteriorSupport ||
              (exteriorSupport == bestExteriorSupport && representative < bestRgb)))) {
            bestSupport = support;
            bestExteriorSupport = exteriorSupport;
            bestRgb = representative;
        }
    }
    return bestRgb;
}

QColor estimateBackground(const QImage& source, const QPolygonF& quad,
                          const TextRegionIndex& textRegions) {
    if (!quadIntersectsImage(quad, source.size())) {
        return {};
    }
    std::array<BackgroundSample, 4 * 3 * kSamplesPerSide + kInteriorGrid * kInteriorGrid> samples{};
    int count = 0;
    QRgb previousRgb = 0;
    std::array<float, 3> previousLab{};
    QSet<QPoint> visited;
    visited.reserve(static_cast<qsizetype>(samples.size()));
    const auto sample = [&](const QPointF& point, bool exterior) {
        if (point.x() < 0 || point.y() < 0 || point.x() >= source.width() ||
            point.y() >= source.height()) {
            return;
        }
        const QPoint pixel(static_cast<int>(point.x()), static_cast<int>(point.y()));
        if ((exterior && textRegions.contains(pixel)) || visited.contains(pixel)) {
            return;
        }
        visited.insert(pixel);
        QRgb rgb;
        const bool flat = flatPixel(source, pixel, rgb);
        if (qAlpha(rgb) == 0) {
            return;
        }
        if (qAlpha(rgb) != 255) {
            // QImage handles premultiplied and uncommon formats without converting the frame.
            rgb = source.pixelColor(pixel).rgb();
        }
        if (rgb != previousRgb) {
            previousRgb = rgb;
            previousLab = perceptualColor(rgb);
        }
        samples[count++] = {rgb, previousLab, flat ? 4 : 1};
    };
    const QPointF center = (quad[0] + quad[1] + quad[2] + quad[3]) / 4.0;
    qreal shortEdge = std::numeric_limits<qreal>::max();
    for (int side = 0; side < 4; ++side) {
        shortEdge = std::min(shortEdge, std::hypot((quad[(side + 1) % 4] - quad[side]).x(),
                                                   (quad[(side + 1) % 4] - quad[side]).y()));
    }
    const qreal bandWidth = std::clamp(shortEdge * 0.12, 2.0, 6.0);
    for (int side = 0; side < 4; ++side) {
        const QPointF edge = quad[(side + 1) % 4] - quad[side];
        const qreal length = std::hypot(edge.x(), edge.y());
        QPointF normal(-edge.y() / length, edge.x() / length);
        if (QPointF::dotProduct(normal, center - quad[side]) > 0) {
            normal = -normal;
        }
        const int positions =
            static_cast<int>(std::clamp(length / 2.0, 4.0, double(kSamplesPerSide)));
        for (int band = 0; band < 3; ++band) {
            const qreal distance = 1.5 + band * bandWidth / 2.0;
            for (int i = 0; i < positions; ++i) {
                sample(quad[side] + edge * ((i + 0.5) / positions) + normal * distance, true);
            }
        }
    }
    const int exteriorCount = count;
    const bool clipped = !QRectF(source.rect()).contains(quad.boundingRect());
    const QRectF interiorBounds =
        clipped ? quad.intersected(QPolygonF(QRectF(source.rect()))).boundingRect()
                : quad.boundingRect();
    for (int y = 0; y < kInteriorGrid; ++y) {
        for (int x = 0; x < kInteriorGrid; ++x) {
            // One sample per area cell, with row/column-dependent offsets, avoids lining every
            // observation up with periodic glyph strokes. Mirrored offsets preserve winding ties.
            const qreal xPhase = (std::min(y, kInteriorGrid - 1 - y) + 0.5) / (kInteriorGrid / 2.0);
            const qreal yPhase = (std::min(x, kInteriorGrid - 1 - x) + 0.5) / (kInteriorGrid / 2.0);
            const qreal u = (x + (x < kInteriorGrid / 2 ? xPhase : 1 - xPhase)) / kInteriorGrid;
            const qreal v = (y + (y < kInteriorGrid / 2 ? yPhase : 1 - yPhase)) / kInteriorGrid;
            const QPointF left = quad[0] * (1 - v) + quad[3] * v;
            const QPointF right = quad[1] * (1 - v) + quad[2] * v;
            const QPointF point =
                clipped ? interiorBounds.topLeft() +
                              QPointF(u * interiorBounds.width(), v * interiorBounds.height())
                        : left + (right - left) * u;
            if (!clipped || quad.containsPoint(point, Qt::OddEvenFill)) {
                sample(point, false);
            }
        }
    }
    if (count > exteriorCount) {
        return QColor::fromRgb(dominantColor(samples.data() + exteriorCount, count - exteriorCount,
                                             samples.data(), exteriorCount));
    }
    if (exteriorCount > 0) {
        return QColor::fromRgb(dominantColor(samples.data(), exteriorCount, nullptr, 0));
    }
    // No usable color (for example fully transparent input) must still produce a solid fill.
    return QColor(Qt::white);
}

qreal edgeLength(const QPointF& first, const QPointF& second) {
    return std::hypot(second.x() - first.x(), second.y() - first.y());
}

QPolygonF expandedQuad(const QPolygonF& quad) {
    if (quad.size() != 4) {
        return quad;
    }
    const qreal width =
        std::max(edgeLength(quad.at(0), quad.at(1)), edgeLength(quad.at(3), quad.at(2)));
    const qreal height =
        std::max(edgeLength(quad.at(0), quad.at(3)), edgeLength(quad.at(1), quad.at(2)));
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
                             const QSize& pixelSize, bool expand,
                             bool requireIntersection = false) {
    if (!finitePolygon(quad)) {
        return {};
    }
    if (requireIntersection) {
        QPolygonF original;
        original.reserve(quad.size());
        for (const QPointF& point : quad) {
            original.push_back(imagePointForCanvasPoint(point, canvasRect, pixelSize));
        }
        if (!quadIntersectsImage(original, pixelSize)) {
            return {};
        }
    }
    const QPolygonF source = expand ? expandedQuad(quad) : quad;
    QPolygonF polygon;
    polygon.reserve(source.size());
    for (const QPointF& point : source) {
        const QPointF pixel = imagePointForCanvasPoint(point, canvasRect, pixelSize);
        if (!finitePoint(pixel)) {
            return {};
        }
        polygon.push_back(pixel);
    }
    return clippedPolygon(polygon, pixelSize);
}

} // namespace

void prepareScreenshotOcrFillColors(ScreenshotOcrPresentation& presentation, const QImage& source,
                                    const QRectF& canvasRect, bool backgroundFill) {
    const QRectF normalized = canvasRect.normalized();
    presentation.solidBackgroundFill = backgroundFill;
    for (ScreenshotOcrLine& line : presentation.lines) {
        line.backgroundFillColor = QColor();
    }
    if (!backgroundFill || source.isNull() || !normalized.isValid() || normalized.isEmpty()) {
        return;
    }
    QVector<QPolygonF> quads;
    quads.reserve(presentation.lines.size());
    for (const ScreenshotOcrLine& line : presentation.lines) {
        QPolygonF quad;
        for (const QPointF& point : line.quad) {
            quad.push_back(imagePointForCanvasPoint(point, normalized, source.size()));
        }
        if (!convexQuad(quad)) {
            quad.clear();
        }
        quads.push_back(std::move(quad));
    }
    const TextRegionIndex textRegions(quads, source.rect());
    for (qsizetype i = 0; i < presentation.lines.size(); ++i) {
        presentation.lines[i].backgroundFillColor =
            estimateBackground(source, quads[i], textRegions);
    }
}

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
    painter.setTransform(QTransform(scaleX, 0.0, 0.0, scaleY, -normalized.left() * scaleX,
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
        // The displayed block is authoritative after layout processing, including its gaps.
        if (line.quad.size() >= 3) {
            region += QRegion(imagePolygonForQuad(line.quad, normalized, pixelSize, true,
                                                  presentation.solidBackgroundFill));
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
                  normalized.top() + filteredPixels.top() * scaleY, filteredPixels.width() * scaleX,
                  filteredPixels.height() * scaleY);
}

QImage renderScreenshotOcrFilteredImage(const QImage& source, const QRectF& canvasRect,
                                        const ScreenshotOcrPresentation& presentation,
                                        const QColor& backgroundColor, qreal devicePixelRatio,
                                        QRect* filteredPixels,
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
    struct SolidRegion {
        QRegion region;
        QColor color;
    };
    std::vector<SolidRegion> solids;
    std::vector<Cluster> clusters;
    QRect crop;
    for (const ScreenshotOcrLine& line : presentation.lines) {
        if (line.quad.size() < 3) {
            continue;
        }
        const QPolygon polygon = imagePolygonForQuad(line.quad, normalized, source.size(), true,
                                                     presentation.solidBackgroundFill);
        if (polygon.isEmpty()) {
            continue;
        }
        if (presentation.solidBackgroundFill || line.backgroundFillColor.isValid()) {
            QRegion region = QRegion(polygon).intersected(imageRect);
            if (!region.isEmpty()) {
                crop = crop.united(region.boundingRect());
                QColor color = line.backgroundFillColor.isValid() ? line.backgroundFillColor
                               : backgroundColor.isValid()        ? backgroundColor
                                                                  : QColor(Qt::white);
                color.setAlpha(255);
                solids.push_back({std::move(region), color});
            }
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
            next.expandedBounds = next.expandedBounds.united(clusters[index].expandedBounds);
            clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(index));
            index = 0;
        }
        clusters.push_back(std::move(next));
    }
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

    QImage filtered = source.copy(crop);
    if (filtered.format() != QImage::Format_ARGB32_Premultiplied) {
        filtered = filtered.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    filtered.setDevicePixelRatio(1.0);
    // Sharing with blurInput is safe: the engine detaches destination before its
    // first write, leaving blurInput as the pristine read-side buffer.
    const QImage blurInput = clusters.empty() ? QImage() : filtered;
    // Anchoring the reduced sampling grid to the absolute image origin keeps the
    // cropped render pixel-identical to filtering the full image.
    parameters.gridOriginInImage = QPointF(-crop.left(), -crop.top());
    QRegion fillRegion;
    for (Cluster& cluster : clusters) {
        const QRegion localRegion = cluster.region.translated(-crop.topLeft());
        if (!applySnowCanvasRegionFilter(blurInput, filtered, localRegion, parameters, scratch)) {
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
    // Blur always reads the immutable source. Solid regions win overlaps, and retain line order.
    for (const SolidRegion& solid : solids) {
        painter.setClipRegion(solid.region.translated(-crop.topLeft()));
        painter.fillRect(filtered.rect(), solid.color);
    }
    if (filteredPixels != nullptr) {
        *filteredPixels = crop;
    }
    return filtered;
}

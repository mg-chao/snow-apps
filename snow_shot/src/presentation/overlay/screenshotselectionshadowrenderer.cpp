#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"
#include "widgets/checkerboard.h"

#include <QPainter>
#include <QPainterPath>
#include <QRegion>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <list>
#include <utility>
#include <vector>

namespace {
constexpr qreal kPeakAlphaScale = 0.36;
constexpr int kCacheEntryLimit = 8;
constexpr std::size_t kCacheByteLimit = 16u * 1024u * 1024u;
constexpr int kDprQuantization = 64;

struct ShadowKey {
    int physicalRadius = 0;
    int physicalShadowWidth = 0;
    QRgb color = 0;
    int quantizedDpr = kDprQuantization;

    bool operator==(const ShadowKey& other) const {
        return physicalRadius == other.physicalRadius &&
               physicalShadowWidth == other.physicalShadowWidth && color == other.color &&
               quantizedDpr == other.quantizedDpr;
    }
};

QImage applyOutputOpacity(QImage image, qreal opacity) {
    if (image.isNull()) {
        return {};
    }
    const qreal normalizedOpacity = std::isfinite(opacity) ? std::clamp(opacity, 0.0, 1.0) : 1.0;
    if (normalizedOpacity >= 1.0) {
        return image;
    }

    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.fillRect(image.rect(), QColor(0, 0, 0, qRound(normalizedOpacity * 255.0)));
    painter.end();
    return image;
}

struct ShadowCacheEntry {
    ShadowKey key;
    QImage image;
    std::size_t bytes = 0;
    std::uint64_t lastUsed = 0;
};

struct ShadowCache {
    std::vector<ShadowCacheEntry> entries;
    std::uint64_t use = 0;
    std::size_t bytes = 0;
};

thread_local ShadowCache g_cache;
thread_local ScreenshotSelectionShadowDiagnostics g_diagnostics;

struct RegionPathCache {
    QRegion region;
    qreal radius = -1;
    QPainterPath path;
};

struct RegionCompositionCache {
    QPainterPath path;
    QSize size;
    int width = -1;
    QColor color;
    QImage mask;
    QImage shadow;
};

thread_local RegionPathCache g_regionPathCache;
thread_local RegionCompositionCache g_regionCompositionCache;

int quantizedDpr(qreal dpr) {
    return std::max(1, qRound(std::max<qreal>(1.0, dpr) * kDprQuantization));
}

qreal roundedRectangleDistance(qreal x, qreal y, qreal halfWidth, qreal halfHeight, qreal radius) {
    const qreal qx = std::abs(x) - (halfWidth - radius);
    const qreal qy = std::abs(y) - (halfHeight - radius);
    const qreal outsideX = std::max<qreal>(qx, 0.0);
    const qreal outsideY = std::max<qreal>(qy, 0.0);
    return std::hypot(outsideX, outsideY) + std::min<qreal>(std::max(qx, qy), 0.0) - radius;
}

QImage buildShadowAsset(const ShadowKey& key) {
    const int radius = std::max(0, key.physicalRadius);
    const int shadow = std::max(1, key.physicalShadowWidth);
    const int cornerSpan = radius + shadow;
    const int size = std::max(3, cornerSpan * 2 + 1);
    QImage asset(QSize(size, size), QImage::Format_ARGB32);
    asset.fill(Qt::transparent);

    QColor color = QColor::fromRgba(key.color);
    const qreal peakAlpha = kPeakAlphaScale * static_cast<qreal>(color.alphaF());
    // Pixel centers range from 0.5 to size - 0.5, so the asset's geometric
    // center is size / 2. Keeping the one-pixel center slice inside the shape
    // is especially important for radius 0: that slice is stretched across
    // the selection by the nine-slice renderer and must remain transparent.
    const qreal center = size / 2.0;
    const qreal halfWidth = center - shadow;
    const qreal halfHeight = center - shadow;
    for (int y = 0; y < size; ++y) {
        QRgb* scanLine = reinterpret_cast<QRgb*>(asset.scanLine(y));
        for (int x = 0; x < size; ++x) {
            const qreal distance =
                roundedRectangleDistance(x + 0.5 - center, y + 0.5 - center, halfWidth, halfHeight,
                                         static_cast<qreal>(radius));
            if (distance < 0.0 || distance >= shadow || peakAlpha <= 0.0) {
                continue;
            }
            const qreal progress =
                std::clamp(1.0 - distance / static_cast<qreal>(shadow), 0.0, 1.0);
            const qreal smooth = progress * progress * (3.0 - 2.0 * progress);
            color.setAlphaF(static_cast<float>(peakAlpha * smooth));
            scanLine[x] = color.rgba();
        }
    }
    return asset;
}

QImage shadowAsset(int physicalRadius, int physicalShadowWidth, const QColor& color, qreal dpr) {
    const ShadowKey key{
        std::max(0, physicalRadius),
        std::max(1, physicalShadowWidth),
        color.rgba(),
        quantizedDpr(dpr),
    };
    ++g_cache.use;
    for (ShadowCacheEntry& entry : g_cache.entries) {
        if (!(entry.key == key)) {
            continue;
        }
        entry.lastUsed = g_cache.use;
        ++g_diagnostics.cacheHits;
        return entry.image;
    }

    QImage asset = buildShadowAsset(key);
    const std::size_t assetBytes = static_cast<std::size_t>(asset.sizeInBytes());
    ++g_diagnostics.cacheBuilds;

    // Keep an individual style asset out of the cache when it cannot fit. The
    // persisted settings are small, but this preserves the byte cap for API
    // callers that provide unusually large radii or shadow widths.
    if (assetBytes > kCacheByteLimit) {
        return asset;
    }

    ShadowCacheEntry entry;
    entry.key = key;
    entry.image = std::move(asset);
    entry.bytes = assetBytes;
    entry.lastUsed = g_cache.use;

    while ((static_cast<int>(g_cache.entries.size()) >= kCacheEntryLimit ||
            entry.bytes > kCacheByteLimit - g_cache.bytes) &&
           !g_cache.entries.empty()) {
        const auto leastUsed =
            std::min_element(g_cache.entries.begin(), g_cache.entries.end(),
                             [](const ShadowCacheEntry& left, const ShadowCacheEntry& right) {
                                 return left.lastUsed < right.lastUsed;
                             });
        g_cache.bytes -= leastUsed->bytes;
        g_cache.entries.erase(leastUsed);
    }
    g_cache.bytes += entry.bytes;
    g_cache.entries.push_back(std::move(entry));
    return g_cache.entries.back().image;
}

QPainterPath roundedHole(const QRectF& selectionBounds, qreal cornerRadius) {
    QPainterPath path;
    const qreal radius = std::clamp(
        cornerRadius, 0.0, std::min(selectionBounds.width(), selectionBounds.height()) / 2.0);
    if (radius <= 0.0) {
        path.addRect(selectionBounds);
    } else {
        path.addRoundedRect(selectionBounds, radius, radius, Qt::AbsoluteSize);
    }
    return path;
}

void paintNineSlice(QPainter& painter, const QRectF& selectionBounds, qreal cornerRadius,
                    qreal shadowWidth, const QImage& asset) {
    const qreal radius = std::clamp(
        cornerRadius, 0.0, std::min(selectionBounds.width(), selectionBounds.height()) / 2.0);
    const qreal shadow = std::max<qreal>(0.0, shadowWidth);
    const QRectF outer = selectionBounds.adjusted(-shadow, -shadow, shadow, shadow);
    const int physicalSpan = (asset.width() - 1) / 2;

    const qreal x[4] = {
        outer.left(),
        selectionBounds.left() + radius,
        selectionBounds.right() - radius,
        outer.right(),
    };
    const qreal y[4] = {
        outer.top(),
        selectionBounds.top() + radius,
        selectionBounds.bottom() - radius,
        outer.bottom(),
    };
    const int sourceX[4] = {0, physicalSpan, asset.width() - physicalSpan, asset.width()};
    const int sourceY[4] = {0, physicalSpan, asset.height() - physicalSpan, asset.height()};
    const QRectF targetRects[9] = {
        QRectF(QPointF(x[0], y[0]), QPointF(x[1], y[1])),
        QRectF(QPointF(x[1], y[0]), QPointF(x[2], y[1])),
        QRectF(QPointF(x[2], y[0]), QPointF(x[3], y[1])),
        QRectF(QPointF(x[0], y[1]), QPointF(x[1], y[2])),
        QRectF(QPointF(x[1], y[1]), QPointF(x[2], y[2])),
        QRectF(QPointF(x[2], y[1]), QPointF(x[3], y[2])),
        QRectF(QPointF(x[0], y[2]), QPointF(x[1], y[3])),
        QRectF(QPointF(x[1], y[2]), QPointF(x[2], y[3])),
        QRectF(QPointF(x[2], y[2]), QPointF(x[3], y[3])),
    };
    int targetIndex = 0;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column, ++targetIndex) {
            const QRectF& target = targetRects[targetIndex];
            if (target.width() <= 0.0 || target.height() <= 0.0) {
                continue;
            }
            painter.drawImage(target, asset,
                              QRect(sourceX[column], sourceY[row],
                                    sourceX[column + 1] - sourceX[column],
                                    sourceY[row + 1] - sourceY[row]));
        }
    }
}

void paintCheckerboardPerimeter(QPainter& painter, const QRectF& selectionBounds,
                                qreal cornerRadius, qreal shadowWidth, const QWidget* widget) {
    if (shadowWidth <= 0.0) {
        return;
    }
    const QRectF outer =
        selectionBounds.adjusted(-shadowWidth, -shadowWidth, shadowWidth, shadowWidth);
    painter.save();
    painter.setClipRegion(QRegion(outer.toAlignedRect()), Qt::IntersectClip);
    QPainterPath perimeter;
    perimeter.setFillRule(Qt::OddEvenFill);
    perimeter.addRect(outer);
    perimeter.addPath(roundedHole(selectionBounds, cornerRadius));
    painter.fillPath(perimeter, adqt::widgets::themedCheckerboardBrush(widget));
    painter.restore();
}

void renderShadow(QPainter& painter, const QRectF& selectionBounds, qreal cornerRadius,
                  qreal shadowWidth, const QColor& shadowColor, qreal dpr,
                  bool retainAsset = true) {
    if (selectionBounds.isEmpty() || shadowWidth <= 0.0) {
        return;
    }
    const qreal effectiveDevicePixelRatio = std::max<qreal>(1.0, dpr);
    const int physicalRadius =
        qRound(std::max<qreal>(0.0, cornerRadius) * effectiveDevicePixelRatio);
    const int physicalShadowWidth = std::max(1, qRound(shadowWidth * effectiveDevicePixelRatio));
    const QColor color = shadowColor.isValid() ? shadowColor : QColor(0x33, 0x33, 0x33);
    const QImage asset =
        retainAsset
            ? shadowAsset(physicalRadius, physicalShadowWidth, color, effectiveDevicePixelRatio)
            : buildShadowAsset(ShadowKey{physicalRadius, physicalShadowWidth, color.rgba(),
                                         quantizedDpr(effectiveDevicePixelRatio)});
    paintNineSlice(painter, selectionBounds, cornerRadius, shadowWidth, asset);
}
} // namespace

void ScreenshotSelectionShadowRenderer::renderPreview(
    QPainter& painter, const QRectF& selectionBounds, qreal cornerRadius, qreal shadowWidth,
    const QColor& shadowColor, qreal devicePixelRatio, const QWidget* widget) {
    paintCheckerboardPerimeter(painter, selectionBounds, cornerRadius, shadowWidth, widget);
    renderShadow(painter, selectionBounds, cornerRadius, shadowWidth, shadowColor,
                 devicePixelRatio);
}

QImage ScreenshotSelectionShadowRenderer::composeExport(const QImage& content, int cornerRadius,
                                                        int shadowWidth,
                                                        const QColor& shadowColor) {
    return ScreenshotResultCompositor::compose(
        content, ScreenshotResultStyle{cornerRadius, shadowWidth, shadowColor, {}, 1.0},
        std::max<qreal>(1.0, content.devicePixelRatio()));
}

void ScreenshotSelectionShadowRenderer::renderResultShadow(QPainter& painter,
                                                           const QRectF& contentBounds,
                                                           qreal cornerRadius, qreal shadowWidth,
                                                           const QColor& shadowColor,
                                                           qreal devicePixelRatio) {
    renderShadow(painter, contentBounds, cornerRadius, shadowWidth, shadowColor, devicePixelRatio);
}

QPainterPath screenshotRegionPath(const ScreenshotRegionGeometry& geometry, qreal radius) {
    if (geometry.custom())
        return geometry.path();
    const QRegion& region = geometry.rectangles();
    // The region's scanline rectangles are a storage decomposition, not contours.
    // Simplify their union before rounding so shared edges never become visible.
    auto& cache = g_regionPathCache;
    if (cache.region == region && cache.radius == radius)
        return cache.path;
    if (radius > 0 && region.rectCount() > 1 && region.rectCount() <= 512) {
        std::vector<QRect> rectangles(region.begin(), region.end());
        const int clearance = qCeil(radius * 2);
        bool isolated = true;
        for (std::size_t i = 0; isolated && i < rectangles.size(); ++i) {
            const QRect nearby =
                rectangles[i].adjusted(-clearance, -clearance, clearance, clearance);
            for (std::size_t j = i + 1; j < rectangles.size(); ++j) {
                if (nearby.intersects(rectangles[j])) {
                    isolated = false;
                    break;
                }
            }
        }
        if (isolated) {
            QPainterPath rounded;
            for (const QRect& rectangle : rectangles) {
                const qreal effectiveRadius =
                    std::min<qreal>(radius, std::min(rectangle.width(), rectangle.height()) / 2.0);
                rounded.addRoundedRect(QRectF(rectangle), effectiveRadius, effectiveRadius,
                                       Qt::AbsoluteSize);
            }
            cache = {region, radius, rounded};
            return rounded;
        }
    }
    QPainterPath joined;
    joined.addRegion(region);
    joined = joined.simplified();
    if (radius <= 0 || region.isEmpty()) {
        cache = {region, radius, joined};
        return joined;
    }
    const auto polygons = joined.toSubpathPolygons();
    struct Edge {
        QPointF a;
        QPointF b;
    };
    std::vector<Edge> edges;
    for (const auto& contour : polygons) {
        for (qsizetype j = 1; j < contour.size(); ++j)
            edges.push_back({contour[j - 1], contour[j]});
    }
    // Only edges within twice the requested radius can constrain a corner.
    // Index them once instead of testing every edge at every corner.
    const QRectF bounds = joined.boundingRect();
    const qreal cellSize = std::max<qreal>(
        {16.0, radius * 2.0, std::max(bounds.width(), bounds.height()) / 128.0,
         std::sqrt(std::max<qreal>(1.0, bounds.width() * bounds.height()) / 4096.0)});
    const int columns = std::max(1, qCeil(bounds.width() / cellSize) + 1);
    const int rows = std::max(1, qCeil(bounds.height() / cellSize) + 1);
    const auto columnCount = static_cast<std::size_t>(columns);
    std::vector<std::vector<std::size_t>> edgeCells(columnCount *
                                                    static_cast<std::size_t>(rows));
    const auto columnFor = [&](qreal x) {
        return std::clamp(qFloor((x - bounds.left()) / cellSize), 0, columns - 1);
    };
    const auto rowFor = [&](qreal y) {
        return std::clamp(qFloor((y - bounds.top()) / cellSize), 0, rows - 1);
    };
    const auto cellAt = [&](int column, int row) -> std::vector<std::size_t>& {
        return edgeCells[static_cast<std::size_t>(row) * columnCount +
                         static_cast<std::size_t>(column)];
    };
    const qreal reach = radius * 2.0 + 1.0e-6;
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const auto& edge = edges[index];
        const int firstColumn = columnFor(std::min(edge.a.x(), edge.b.x()) - reach);
        const int lastColumn = columnFor(std::max(edge.a.x(), edge.b.x()) + reach);
        const int firstRow = rowFor(std::min(edge.a.y(), edge.b.y()) - reach);
        const int lastRow = rowFor(std::max(edge.a.y(), edge.b.y()) + reach);
        for (int row = firstRow; row <= lastRow; ++row)
            for (int column = firstColumn; column <= lastColumn; ++column)
                cellAt(column, row).push_back(index);
    }
    QPainterPath rounded;
    rounded.setFillRule(Qt::OddEvenFill);
    for (auto polygon : polygons) {
        if (polygon.size() > 1 && polygon.first() == polygon.last())
            polygon.removeLast();
        const qsizetype count = polygon.size();
        if (count < 3)
            continue;
        for (qsizetype i = 0; i < count; ++i) {
            const QPointF corner = polygon[i];
            const QPointF previous = polygon[(i + count - 1) % count];
            const QPointF next = polygon[(i + 1) % count];
            const qreal before = QLineF(corner, previous).length();
            const qreal after = QLineF(corner, next).length();
            if (before <= 0 || after <= 0)
                continue;
            qreal r = std::min({radius, before / 2, after / 2});
            // Bound curvature by all nonincident edges, including other contours.
            // This keeps thin bridges and nearby hole boundaries from crossing.
            for (const std::size_t index : cellAt(columnFor(corner.x()), rowFor(corner.y()))) {
                const QPointF a = edges[index].a, b = edges[index].b;
                if (a == corner || b == corner)
                    continue;
                const QPointF ab = b - a;
                const qreal lengthSquared = QPointF::dotProduct(ab, ab);
                if (lengthSquared <= 0)
                    continue;
                const qreal t = std::clamp(QPointF::dotProduct(corner - a, ab) / lengthSquared,
                                           qreal(0), qreal(1));
                r = std::min(r, QLineF(corner, a + ab * t).length() / 2);
            }
            const QPointF entry = corner + (previous - corner) * (r / before);
            const QPointF leave = corner + (next - corner) * (r / after);
            if (i == 0)
                rounded.moveTo(entry);
            else
                rounded.lineTo(entry);
            constexpr qreal kCircle = 0.5522847498307936;
            rounded.cubicTo(entry + (corner - entry) * kCircle, leave + (corner - leave) * kCircle,
                            leave);
        }
        rounded.closeSubpath();
    }
    cache = {region, radius, rounded};
    return rounded;
}

namespace {
QImage regionMask(const QSize& size, const QPainterPath& path) {
    QImage mask(size, QImage::Format_Alpha8);
    ++g_diagnostics.regionMaskBuilds;
    g_diagnostics.regionScratchPeakBytes = std::max(g_diagnostics.regionScratchPeakBytes,
                                                    static_cast<std::size_t>(mask.sizeInBytes()));
    mask.fill(0);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillPath(path, Qt::white);
    return mask;
}

QImage regionShadow(const QImage& mask, int width, const QColor& color) {
    // Finite-support separable blur, O(pixel count) regardless of shadow width.
    const int w = mask.width(), h = mask.height();
    const auto rowWidth = static_cast<std::size_t>(w);
    const auto pixelCount = rowWidth * static_cast<std::size_t>(h);
    ++g_diagnostics.regionShadowBuilds;
    // Mask + output shadow + two float planes + the vertical running sums.
    g_diagnostics.regionScratchPeakBytes =
        std::max(g_diagnostics.regionScratchPeakBytes,
                 static_cast<std::size_t>(mask.sizeInBytes()) +
                     pixelCount * (sizeof(QRgb) + 2 * sizeof(float)) +
                     rowWidth * sizeof(float));
    std::vector<float> alpha(pixelCount);
    std::vector<float> scratch(alpha.size());
    for (int y = 0; y < h; ++y) {
        const auto* row = mask.constScanLine(y);
        for (int x = 0; x < w; ++x)
            alpha[static_cast<std::size_t>(y) * rowWidth + static_cast<std::size_t>(x)] =
                static_cast<float>(row[x]);
    }
    std::vector<float> columns(rowWidth);
    for (int pass = 0; pass < 3; ++pass) {
        const int radius = width / 3 + (pass < width % 3 ? 1 : 0);
        if (radius == 0)
            continue;
        const float divisor = static_cast<float>(2 * radius + 1);
        for (int y = 0; y < h; ++y) {
            const auto offset = static_cast<std::size_t>(y) * rowWidth;
            float sum = 0;
            for (int x = 0; x <= radius && x < w; ++x)
                sum += alpha[offset + static_cast<std::size_t>(x)];
            for (int x = 0; x < w; ++x) {
                const auto column = static_cast<std::size_t>(x);
                scratch[offset + column] = sum / divisor;
                if (x - radius >= 0)
                    sum -= alpha[offset + static_cast<std::size_t>(x - radius)];
                if (x + radius + 1 < w)
                    sum += alpha[offset + static_cast<std::size_t>(x + radius + 1)];
            }
        }
        // Keep each column's running sum, but visit memory in row order.
        // Column-major traversal of 4K/8K float buffers defeats CPU caches.
        std::fill(columns.begin(), columns.end(), 0.0f);
        for (int y = 0; y <= radius && y < h; ++y)
            for (int x = 0; x < w; ++x)
                columns[static_cast<std::size_t>(x)] +=
                    scratch[static_cast<std::size_t>(y) * rowWidth +
                            static_cast<std::size_t>(x)];
        for (int y = 0; y < h; ++y) {
            const auto offset = static_cast<std::size_t>(y) * rowWidth;
            for (int x = 0; x < w; ++x)
                alpha[offset + static_cast<std::size_t>(x)] =
                    columns[static_cast<std::size_t>(x)] / divisor;
            if (y - radius >= 0)
                for (int x = 0; x < w; ++x)
                    columns[static_cast<std::size_t>(x)] -=
                        scratch[static_cast<std::size_t>(y - radius) * rowWidth +
                                static_cast<std::size_t>(x)];
            if (y + radius + 1 < h)
                for (int x = 0; x < w; ++x)
                    columns[static_cast<std::size_t>(x)] +=
                        scratch[static_cast<std::size_t>(y + radius + 1) * rowWidth +
                                static_cast<std::size_t>(x)];
        }
    }
    QImage shadow(mask.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<QRgb*>(shadow.scanLine(y));
        const auto* maskRow = mask.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const int a =
                std::clamp(qRound(static_cast<qreal>(
                                      alpha[static_cast<std::size_t>(y) * rowWidth +
                                            static_cast<std::size_t>(x)]) *
                                  static_cast<qreal>(color.alphaF()) *
                                  kPeakAlphaScale * (255 - maskRow[x]) / 255.0),
                           0, 255);
            row[x] = qPremultiply(qRgba(color.red(), color.green(), color.blue(), a));
        }
    }
    return shadow;
}

struct RegionTile {
    QRect bounds;
    QPainterPath path;
};

std::vector<RegionTile> sparseRegionTiles(const QPainterPath& path, const QRect& outputBounds,
                                          int shadowWidth) {
    std::vector<RegionTile> tiles;
    QPainterPath subpath;
    subpath.setFillRule(path.fillRule());
    const auto appendSubpath = [&] {
        if (subpath.isEmpty())
            return;
        const QRect bounds =
            subpath.boundingRect()
                .toAlignedRect()
                .adjusted(-shadowWidth - 2, -shadowWidth - 2, shadowWidth + 2, shadowWidth + 2)
                .intersected(outputBounds);
        if (bounds.isEmpty())
            return;
        RegionTile tile{bounds, subpath};
        for (std::size_t index = 0; index < tiles.size();) {
            if (!tile.bounds.intersects(tiles[index].bounds)) {
                ++index;
                continue;
            }
            tile.bounds = tile.bounds.united(tiles[index].bounds);
            tile.path.addPath(tiles[index].path);
            tiles.erase(tiles.begin() + static_cast<std::ptrdiff_t>(index));
            index = 0;
        }
        tiles.push_back(std::move(tile));
    };
    for (int index = 0; index < path.elementCount(); ++index) {
        const auto element = path.elementAt(index);
        if (element.type == QPainterPath::MoveToElement) {
            appendSubpath();
            subpath = QPainterPath();
            subpath.setFillRule(path.fillRule());
            subpath.moveTo(element.x, element.y);
        } else if (element.type == QPainterPath::LineToElement) {
            subpath.lineTo(element.x, element.y);
        } else if (element.type == QPainterPath::CurveToElement &&
                   index + 2 < path.elementCount()) {
            const auto control = path.elementAt(++index);
            const auto end = path.elementAt(++index);
            subpath.cubicTo(QPointF(element.x, element.y), QPointF(control.x, control.y),
                            QPointF(end.x, end.y));
        }
    }
    appendSubpath();
    if (tiles.size() < 2)
        return {};
    qint64 area = 0;
    for (const auto& tile : tiles)
        area += qint64(tile.bounds.width()) * tile.bounds.height();
    if (area * 2 >= qint64(outputBounds.width()) * outputBounds.height())
        return {};
    return tiles;
}

void paintTiledRegion(QPainter& painter, const QImage& content,
                      const ScreenshotResultLayout& layout, const ScreenshotResultStyle& style,
                      const QPainterPath& path, const QRect& bounds);

QImage composeSparseRegion(const QImage& content, const ScreenshotResultLayout& layout,
                           const ScreenshotResultStyle& style, const std::vector<RegionTile>& tiles,
                           qreal opacity) {
    QImage output(layout.outputRect.size(), QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    QPainter outputPainter(&output);
    for (const auto& region : tiles) {
        if (layout.effectInsets.left() > 0 &&
            qint64(region.bounds.width()) * region.bounds.height() > 1024 * 1024) {
            paintTiledRegion(outputPainter, content, layout, style, region.path, region.bounds);
            continue;
        }
        QPainterPath localPath = region.path.translated(-region.bounds.topLeft());
        const QImage mask = regionMask(region.bounds.size(), localPath);
        QImage tile(region.bounds.size(), QImage::Format_ARGB32_Premultiplied);
        tile.fill(Qt::transparent);
        QPainter painter(&tile);
        painter.drawImage(layout.contentRect.topLeft() - region.bounds.topLeft(), content);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.drawImage(QPoint(), mask);
        if (layout.effectInsets.left() > 0) {
            const QImage shadow = regionShadow(mask, layout.effectInsets.left(), style.shadowColor);
            painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
            painter.drawImage(QPoint(), shadow);
        }
        painter.end();
        outputPainter.drawImage(region.bounds.topLeft(), tile);
    }
    outputPainter.end();
    return applyOutputOpacity(std::move(output), opacity);
}

void paintTiledRegion(QPainter& painter, const QImage& content,
                      const ScreenshotResultLayout& layout, const ScreenshotResultStyle& style,
                      const QPainterPath& path, const QRect& bounds) {
    const int width = layout.effectInsets.left();
    // Each core owns its pixels; the halo supplies the complete finite blur
    // support. Grow cores with the support so wide shadows do not multiply work.
    const int side = std::max(512, width * 8);
    for (int y = bounds.top(); y <= bounds.bottom(); y += side) {
        for (int x = bounds.left(); x <= bounds.right(); x += side) {
            const QRect core = QRect(x, y, side, side).intersected(bounds);
            const QRect halo = core.adjusted(-width - 2, -width - 2, width + 2, width + 2)
                                   .intersected(layout.outputRect);
            if (!path.intersects(QRectF(halo)))
                continue;
            if (path.contains(QRectF(halo))) {
                painter.drawImage(core, content, core.translated(-layout.contentRect.topLeft()));
                continue;
            }
            const auto localPath = path.translated(-halo.topLeft());
            const auto mask = regionMask(halo.size(), localPath);
            QImage tile(halo.size(), QImage::Format_ARGB32_Premultiplied);
            tile.fill(Qt::transparent);
            {
                QPainter tilePainter(&tile);
                tilePainter.drawImage(layout.contentRect.topLeft() - halo.topLeft(), content);
                tilePainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                tilePainter.drawImage(QPoint(), mask);
                tilePainter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
                tilePainter.drawImage(QPoint(), regionShadow(mask, width, style.shadowColor));
            }
            painter.drawImage(core, tile, core.translated(-halo.topLeft()));
        }
    }
}

QImage composeTiledRegion(const QImage& content, const ScreenshotResultLayout& layout,
                          const ScreenshotResultStyle& style, const QPainterPath& path,
                          qreal opacity) {
    QImage output(layout.outputRect.size(), QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    paintTiledRegion(painter, content, layout, style, path, layout.outputRect);
    painter.end();
    return applyOutputOpacity(std::move(output), opacity);
}

QImage composeRegion(const QImage& content, const ScreenshotResultStyle& style,
                     qreal devicePixelRatio, qreal opacity) {
    const auto layout =
        ScreenshotResultCompositor::layoutForContent(content.size(), style, devicePixelRatio);
    const qreal scale = style.regionScale;
    QTransform transform;
    transform.translate(layout.contentRect.x(), layout.contentRect.y());
    transform.scale(scale, scale);
    const QPainterPath path = transform.map(
        (style.region->custom()
             ? style.region->path(scale)
             : screenshotRegionPath(*style.region,
                                    style.cornerRadius * layout.devicePixelRatio / scale)));
    const auto sparseTiles = sparseRegionTiles(path, layout.outputRect, layout.effectInsets.left());
    if (!sparseTiles.empty())
        return composeSparseRegion(content, layout, style, sparseTiles, opacity);
    // Bound temporary blur storage independently of export dimensions. Interior
    // and exterior tiles need no blur; only boundary tiles allocate scratch.
    if (layout.effectInsets.left() > 0 &&
        qint64(layout.outputRect.width()) * layout.outputRect.height() > 1024 * 1024)
        return composeTiledRegion(content, layout, style, path, opacity);
    auto& cache = g_regionCompositionCache;
    QImage mask;
    QImage shadow;
    if (cache.path != path || cache.size != layout.outputRect.size()) {
        cache = {};
        cache.path = path;
        cache.size = layout.outputRect.size();
    }
    mask = cache.mask;
    if (mask.isNull()) {
        mask = regionMask(cache.size, path);
        if (mask.sizeInBytes() <= qsizetype(kCacheByteLimit))
            cache.mask = mask;
    }
    if (cache.width == layout.effectInsets.left() && cache.color == style.shadowColor)
        shadow = cache.shadow;
    else
        cache.shadow = {};
    cache.width = layout.effectInsets.left();
    cache.color = style.shadowColor;
    if (shadow.isNull() && cache.width > 0) {
        shadow = regionShadow(mask, cache.width, cache.color);
        if (cache.mask.sizeInBytes() + shadow.sizeInBytes() <= qsizetype(kCacheByteLimit))
            cache.shadow = shadow;
    }
    QImage output(layout.outputRect.size(), QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    painter.drawImage(layout.contentRect.topLeft(), content);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.drawImage(QPoint(), mask);
    if (!shadow.isNull()) {
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
        painter.drawImage(QPoint(), shadow);
    }
    painter.end();
    return applyOutputOpacity(std::move(output), opacity);
}
} // namespace

ScreenshotResultStyle
ScreenshotResultCompositor::normalizedStyle(const ScreenshotResultStyle& style) {
    ScreenshotResultStyle normalized = style;
    normalized.cornerRadius = std::clamp(
        normalized.cornerRadius, 0, snow_shot::presentation::kScreenshotSelectionCornerRadiusMax);
    if (normalized.region && normalized.region->custom())
        normalized.cornerRadius = 0;
    normalized.shadowWidth = std::clamp(
        normalized.shadowWidth, 0, snow_shot::presentation::kScreenshotSelectionShadowWidthMax);
    if (!normalized.shadowColor.isValid()) {
        normalized.shadowColor = QColor(0x33, 0x33, 0x33);
    }
    return normalized;
}

ScreenshotResultLayout ScreenshotResultCompositor::layoutForContent(
    const QSize& contentPixelSize, const ScreenshotResultStyle& style, qreal devicePixelRatio) {
    if (!contentPixelSize.isValid() || contentPixelSize.isEmpty()) {
        return {};
    }
    const ScreenshotResultStyle normalized = normalizedStyle(style);
    const qreal dpr = std::max<qreal>(1.0, devicePixelRatio);
    const int effect = std::max(0, qRound(normalized.shadowWidth * dpr));
    ScreenshotResultLayout layout;
    layout.effectInsets = QMargins(effect, effect, effect, effect);
    layout.outputRect = QRect(QPoint(), contentPixelSize + QSize(effect * 2, effect * 2));
    layout.contentRect = QRect(QPoint(effect, effect), contentPixelSize);
    layout.devicePixelRatio = dpr;
    return layout;
}

QImage ScreenshotResultCompositor::normalizeImage(const QImage& image) {
    if (image.isNull()) {
        return {};
    }
    QImage normalized = image.format() == QImage::Format_ARGB32_Premultiplied
                            ? image
                            : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    normalized.setDevicePixelRatio(1.0);
    return normalized;
}

QImage ScreenshotResultCompositor::compose(const QImage& content,
                                           const ScreenshotResultStyle& style,
                                           qreal devicePixelRatio, qreal outputOpacity) {
    const QImage normalizedContent = normalizeImage(content);
    if (normalizedContent.isNull()) {
        return {};
    }
    const ScreenshotResultStyle normalized = normalizedStyle(style);
    if (normalized.region) {
        return composeRegion(normalizedContent, normalized, devicePixelRatio, outputOpacity);
    }
    if (normalized.cornerRadius == 0 && normalized.shadowWidth == 0) {
        return applyOutputOpacity(normalizedContent, outputOpacity);
    }
    const ScreenshotResultLayout layout =
        layoutForContent(normalizedContent.size(), normalized, devicePixelRatio);
    if (!layout.isValid()) {
        return {};
    }

    QImage output(layout.outputRect.size(), QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(1.0);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.drawImage(layout.contentRect, normalizedContent);

    const qreal physicalRadius = normalized.cornerRadius * layout.devicePixelRatio;
    if (normalized.cornerRadius > 0) {
        QImage mask(output.size(), QImage::Format_ARGB32_Premultiplied);
        mask.fill(Qt::transparent);
        {
            QPainter maskPainter(&mask);
            maskPainter.setRenderHint(QPainter::Antialiasing, true);
            maskPainter.setPen(Qt::NoPen);
            maskPainter.setBrush(Qt::white);
            maskPainter.drawPath(roundedHole(layout.contentRect, physicalRadius));
        }
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.drawImage(QPoint(), mask);
    }
    if (normalized.shadowWidth > 0) {
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
        // Composition draws this shadow only once; retain assets for repainted live surfaces.
        renderShadow(painter, layout.contentRect, physicalRadius, layout.effectInsets.left(),
                     normalized.shadowColor, 1.0, false);
    }
    painter.end();
    return applyOutputOpacity(std::move(output), outputOpacity);
}

void ScreenshotResultCompositor::finishLiveSurface(QPainter& painter, const QRectF& viewportBounds,
                                                   const QRectF& contentBounds,
                                                   const ScreenshotResultStyle& style,
                                                   qreal devicePixelRatio,
                                                   qreal canvasToViewScale) {
    if (viewportBounds.isEmpty() || contentBounds.isEmpty()) {
        return;
    }
    const ScreenshotResultStyle normalized = normalizedStyle(style);
    const qreal viewScale = std::max<qreal>(0.0, canvasToViewScale);
    const qreal viewRadius = normalized.cornerRadius * viewScale;
    const qreal viewShadow = normalized.shadowWidth * viewScale;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    // The widget's integer-DIP bounds need not contain the physical content.
    // Use a containing outer rectangle so odd-even fill is a difference, never
    // an XOR of intersecting shapes. Clip that difference to the viewport.
    // Avoid QPainterPath::subtracted: it flattens curves and can misclassify
    // nearly coincident edges produced by fractional camera roundoff.
    QPainterPath outside;
    outside.setFillRule(Qt::OddEvenFill);
    outside.addRect(viewportBounds.united(contentBounds));
    outside.addPath(roundedHole(contentBounds, viewRadius));
    painter.save();
    painter.setClipRect(viewportBounds, Qt::IntersectClip);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    painter.fillPath(outside, Qt::black);
    painter.restore();
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
    ScreenshotSelectionShadowRenderer::renderResultShadow(
        painter, contentBounds, viewRadius, viewShadow, normalized.shadowColor, devicePixelRatio);
    painter.restore();
}

ScreenshotSelectionShadowDiagnostics
ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread() {
    ScreenshotSelectionShadowDiagnostics result = g_diagnostics;
    result.retainedBytes = g_cache.bytes;
    result.retainedEntries = g_cache.entries.size();
    result.regionCacheRetainedBytes =
        static_cast<std::size_t>(g_regionCompositionCache.mask.sizeInBytes() +
                                 g_regionCompositionCache.shadow.sizeInBytes());
    result.regionPathCacheElements = static_cast<std::size_t>(
        g_regionPathCache.path.elementCount() + g_regionCompositionCache.path.elementCount());
    return result;
}

void ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread() {
    g_diagnostics = ScreenshotSelectionShadowDiagnostics{};
}

void ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread() {
    g_cache = ShadowCache{};
    g_regionPathCache = {};
    g_regionCompositionCache = {};
}

void ScreenshotResultCompositor::restoreBakedExterior(QImage& image, const QImage& background,
                                                      const QPainterPath& path) {
    if (image.isNull() || background.isNull() || path.isEmpty())
        return;
    const QImage mask = regionMask(image.size(), path);
    QImage outside = normalizeImage(
        background.size() == image.size()
            ? background
            : background.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    {
        QPainter painter(&outside);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        painter.drawImage(QPoint(), mask);
    }
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.drawImage(QPoint(), mask);
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.drawImage(QPoint(), outside);
}

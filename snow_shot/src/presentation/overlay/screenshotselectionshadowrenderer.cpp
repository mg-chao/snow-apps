#include "snow_draw_engine_qt/snow_canvas_image.h"
#include <snow/memory/pixel_array.h>
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"
#include "snow_shot/presentation/screenshotshadowprofile.h"

#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/image/screenshotdisplayimage.h"
#include "widgets/checkerboard.h"

#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QRegion>
#include <QColorSpace>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <list>
#include <limits>
#include <utility>
#include <vector>

namespace {
using namespace snow_shot::presentation::shadow_profile;
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

    if (!snowCanvasDetachImage(image))
        return {};
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

ShadowCache& shadowCacheForCurrentThread() {
    // Debug iterator tracking can allocate even for an empty vector. Defer
    // construction until use, after loader TLS initialization has completed.
    thread_local ShadowCache cache;
    return cache;
}

thread_local ScreenshotSelectionShadowDiagnostics g_diagnostics;
thread_local std::size_t g_liveRegionCacheBytes = 0;

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
    QImage asset = snowCanvasAllocateImage(QSize(size, size), QImage::Format_ARGB32_Premultiplied);
    asset.fill(Qt::transparent);

    QColor color = QColor::fromRgba(key.color);
    const qreal peakAlpha = peakAlphaScale * static_cast<qreal>(color.alphaF());
    // Pixel centers range from 0.5 to size - 0.5, so the asset's geometric
    // center is size / 2. Keep the central texel inside the shape even for
    // radius 0: the slice renderer omits this entirely transparent interior.
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
            color.setAlphaF(static_cast<float>(peakAlpha * falloff(distance, shadow)));
            scanLine[x] = qPremultiply(color.rgba());
        }
    }
    return asset;
}

QImage shadowAsset(int physicalRadius, int physicalShadowWidth, const QColor& color, qreal dpr) {
    auto& cache = shadowCacheForCurrentThread();
    const ShadowKey key{
        std::max(0, physicalRadius),
        std::max(1, physicalShadowWidth),
        color.rgba(),
        quantizedDpr(dpr),
    };
    ++cache.use;
    for (ShadowCacheEntry& entry : cache.entries) {
        if (!(entry.key == key)) {
            continue;
        }
        entry.lastUsed = cache.use;
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
    entry.lastUsed = cache.use;

    while ((static_cast<int>(cache.entries.size()) >= kCacheEntryLimit ||
            entry.bytes > kCacheByteLimit - cache.bytes) &&
           !cache.entries.empty()) {
        const auto leastUsed =
            std::min_element(cache.entries.begin(), cache.entries.end(),
                             [](const ShadowCacheEntry& left, const ShadowCacheEntry& right) {
                                 return left.lastUsed < right.lastUsed;
                             });
        cache.bytes -= leastUsed->bytes;
        cache.entries.erase(leastUsed);
    }
    cache.bytes += entry.bytes;
    cache.entries.push_back(std::move(entry));
    return cache.entries.back().image;
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
            // The center is entirely inside the content and always transparent. Drawing it
            // would still make Qt visit a selection-sized surface on every shadow repaint.
            if (row == 1 && column == 1)
                continue;
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
    const qreal radius = std::clamp(
        cornerRadius, 0.0, std::min(selectionBounds.width(), selectionBounds.height()) / 2.0);
    const int physicalRadius = qRound(radius * effectiveDevicePixelRatio);
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

void ScreenshotSelectionShadowRenderer::renderBorder(QPainter& painter,
                                                     const QPainterPath& selectionPath, qreal width,
                                                     const QColor& color) {
    if (width <= 0 || selectionPath.isEmpty())
        return;
    QPainterPathStroker stroker;
    stroker.setWidth(2 * width);
    // A constant-width exterior must fit the fixed effect insets used by every
    // output and damage calculation. Miter tips can exceed those insets at acute
    // corners; round joins stay within one width of the selection.
    stroker.setJoinStyle(Qt::RoundJoin);
    const QPainterPath exterior = stroker.createStroke(selectionPath).subtracted(selectionPath);
    QColor opaque = color.isValid() ? color : QColor(Qt::white);
    opaque.setAlpha(255);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillPath(exterior, opaque);
    painter.restore();
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
    std::vector<std::vector<std::size_t>> edgeCells(columnCount * static_cast<std::size_t>(rows));
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
    QImage mask = snowCanvasAllocateZeroedImage(size, QImage::Format_Alpha8);
    ++g_diagnostics.regionMaskBuilds;
    g_diagnostics.regionScratchPeakBytes = std::max(g_diagnostics.regionScratchPeakBytes,
                                                    static_cast<std::size_t>(mask.sizeInBytes()));
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillPath(path, Qt::white);
    return mask;
}

snow::memory::PixelArray<float> regionBlur(const QImage& mask, int width) {
    // Finite-support separable blur, O(pixel count) regardless of shadow width.
    const int w = mask.width(), h = mask.height();
    const auto rowWidth = static_cast<std::size_t>(w);
    const auto pixelCount = rowWidth * static_cast<std::size_t>(h);
    ++g_diagnostics.regionShadowBuilds;
    // Mask + output shadow + two float planes + the vertical running sums.
    g_diagnostics.regionScratchPeakBytes =
        std::max(g_diagnostics.regionScratchPeakBytes,
                 static_cast<std::size_t>(mask.sizeInBytes()) +
                     pixelCount * (sizeof(QRgb) + 2 * sizeof(float)) + rowWidth * sizeof(float));
    snow::memory::PixelArray<float> alpha(pixelCount);
    snow::memory::PixelArray<float> scratch(alpha.size());
    for (int y = 0; y < h; ++y) {
        const auto* row = mask.constScanLine(y);
        for (int x = 0; x < w; ++x)
            alpha[static_cast<std::size_t>(y) * rowWidth + static_cast<std::size_t>(x)] =
                static_cast<float>(row[x]);
    }
    snow::memory::PixelArray<float> columns(rowWidth);
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
                    scratch[static_cast<std::size_t>(y) * rowWidth + static_cast<std::size_t>(x)];
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
    return alpha;
}

QRgb regionShadowPixel(float alpha, uchar mask, const QColor& color) {
    const int a = std::clamp(qRound(static_cast<qreal>(alpha) * static_cast<qreal>(color.alphaF()) *
                                    peakAlphaScale * (255 - mask) / 255.0),
                             0, 255);
    return qPremultiply(qRgba(color.red(), color.green(), color.blue(), a));
}

QImage tintRegionShadow(const QImage& mask, const snow::memory::PixelArray<float>& alpha,
                        const QColor& color) {
    const int w = mask.width(), h = mask.height();
    const auto rowWidth = static_cast<std::size_t>(w);
    QImage shadow = snowCanvasAllocateImage(mask.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<QRgb*>(shadow.scanLine(y));
        const auto* maskRow = mask.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            row[x] = regionShadowPixel(
                alpha[static_cast<std::size_t>(y) * rowWidth + static_cast<std::size_t>(x)],
                maskRow[x], color);
        }
    }
    return shadow;
}

QImage regionShadow(const QImage& mask, int width, const QColor& color) {
    return tintRegionShadow(mask, regionBlur(mask, width), color);
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
    ++g_diagnostics.selectionSizedTransientAllocations;
    QImage output = snowCanvasAllocateZeroedImage(layout.outputRect.size(),
                                                  QImage::Format_ARGB32_Premultiplied);
    output.setColorSpace(content.colorSpace());
    QPainter outputPainter(&output);
    for (const auto& region : tiles) {
        if (layout.effectInsets.left() > 0 &&
            qint64(region.bounds.width()) * region.bounds.height() > 1024 * 1024) {
            paintTiledRegion(outputPainter, content, layout, style, region.path, region.bounds);
            continue;
        }
        QPainterPath localPath = region.path.translated(-region.bounds.topLeft());
        const QImage mask = regionMask(region.bounds.size(), localPath);
        QImage tile =
            snowCanvasAllocateImage(region.bounds.size(), QImage::Format_ARGB32_Premultiplied);
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
            QImage tile = snowCanvasAllocateImage(halo.size(), QImage::Format_ARGB32_Premultiplied);
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
    ++g_diagnostics.selectionSizedTransientAllocations;
    QImage output = snowCanvasAllocateZeroedImage(layout.outputRect.size(),
                                                  QImage::Format_ARGB32_Premultiplied);
    output.setColorSpace(content.colorSpace());
    QPainter painter(&output);
    paintTiledRegion(painter, content, layout, style, path, layout.outputRect);
    painter.end();
    return applyOutputOpacity(std::move(output), opacity);
}

QImage composeRegion(const QImage& content, const ScreenshotResultStyle& style,
                     qreal devicePixelRatio, qreal opacity) {
    const auto layout =
        ScreenshotResultCompositor::layoutForContent(content.size(), style, devicePixelRatio);
    const auto path =
        screenshotSelectionPath(layout.contentRect, style.region,
                                style.cornerRadius * layout.devicePixelRatio, style.regionScale);
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
    ++g_diagnostics.selectionSizedTransientAllocations;
    QImage output = snowCanvasAllocateZeroedImage(layout.outputRect.size(),
                                                  QImage::Format_ARGB32_Premultiplied);
    output.setColorSpace(content.colorSpace());
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

// Destination-over naturally fills the uncovered part of an opaque selection.
// For translucent source pixels it would also fill *inside* the selection. Reduce
// the background there by geometric coverage, keeping antialiased edges intact.
// The usual opaque-capture path neither copies the background nor builds a mask.
QImage backgroundOutsideSelection(QImage background, const QImage& content,
                                  const QPainterPath& path, const QPoint& contentOffset) {
    constexpr int side = 512;
    bool detached = false;
    for (int top = 0; top < content.height(); top += side) {
        for (int left = 0; left < content.width(); left += side) {
            const QRect tile = QRect(left, top, side, side).intersected(content.rect());
            const QRect outputTile = tile.translated(contentOffset);
            if (!path.intersects(QRectF(outputTile)))
                continue;
            const bool interior = path.contains(QRectF(outputTile));
            QImage mask;
            QPoint maskOffset;
            for (int y = tile.top(); y <= tile.bottom(); ++y) {
                const auto* source = reinterpret_cast<const QRgb*>(content.constScanLine(y));
                const int outputY = y + contentOffset.y();
                auto* row = const_cast<QRgb*>(
                    reinterpret_cast<const QRgb*>(background.constScanLine(outputY)));
                for (int x = tile.left(); x <= tile.right(); ++x) {
                    const int alpha = qAlpha(source[x]);
                    const int outputX = x + contentOffset.x();
                    if (alpha == 255 || qAlpha(row[outputX]) == 0)
                        continue;
                    int coverage = 255;
                    if (!interior) {
                        if (mask.isNull()) {
                            const auto& cache = g_regionCompositionCache;
                            if (cache.path == path && cache.size == background.size() &&
                                !cache.mask.isNull()) {
                                mask = cache.mask;
                            } else {
                                mask = regionMask(outputTile.size(),
                                                  path.translated(-outputTile.topLeft()));
                                maskOffset = outputTile.topLeft();
                            }
                            if (mask.isNull())
                                return {};
                        }
                        coverage =
                            mask.constScanLine(outputY - maskOffset.y())[outputX - maskOffset.x()];
                    }
                    if (coverage == 0)
                        continue;
                    if (!detached) {
                        if (!snowCanvasDetachImage(background))
                            return {};
                        row = reinterpret_cast<QRgb*>(background.scanLine(outputY));
                        detached = true;
                    }
                    const int denominator = 255 - (alpha * coverage + 127) / 255;
                    const int numerator = 255 - coverage;
                    const QRgb pixel = row[outputX];
                    const auto scale = [=](int component) {
                        return (component * numerator + denominator / 2) / denominator;
                    };
                    row[outputX] = qRgba(scale(qRed(pixel)), scale(qGreen(pixel)),
                                         scale(qBlue(pixel)), scale(qAlpha(pixel)));
                }
            }
        }
    }
    return background;
}
} // namespace

namespace {
constexpr std::size_t kLiveRegionCacheByteLimit = 64u * 1024u * 1024u;

// Keep the unrounded blur only where it can contribute shadow pixels. Interior
// pixels have full coverage and distant exterior pixels have zero blur. This
// keeps color-independent coverage proportional to the boundary, while retaining
// the exact floating-point inputs used by the export compositor's tint rounding.
struct RegionShadowCoverage {
    struct Span {
        int x = 0;
        int y = 0;
        int count = 0;
        std::size_t offset = 0;
    };
    std::vector<Span> spans;
    snow::memory::PixelArray<float> alpha;
    snow::memory::PixelArray<uchar> mask;
    QRect bounds;

    std::size_t bytes() const {
        return spans.capacity() * sizeof(Span) + alpha.capacity() * sizeof(float) + mask.capacity();
    }

    static RegionShadowCoverage fromBlur(const QImage& maskImage,
                                         const snow::memory::PixelArray<float>& blurred) {
        RegionShadowCoverage result;
        const auto rowWidth = static_cast<std::size_t>(maskImage.width());
        std::size_t samples = 0;
        std::size_t spanCount = 0;
        int left = maskImage.width(), top = maskImage.height(), right = -1, bottom = -1;
        const auto contributes = [](float a, uchar coverage) {
            // No permitted tint can reveal a sample that is transparent even
            // at full opacity. This also removes floating running-sum residue.
            return coverage != 255 &&
                   qRound(static_cast<qreal>(a) * peakAlphaScale * (255 - coverage) / 255.0) > 0;
        };
        for (int y = 0; y < maskImage.height(); ++y) {
            const auto* row = maskImage.constScanLine(y);
            bool inside = false;
            for (int x = 0; x < maskImage.width(); ++x) {
                const bool present = contributes(
                    blurred[static_cast<std::size_t>(y) * rowWidth + static_cast<std::size_t>(x)],
                    row[x]);
                samples += present ? 1u : 0u;
                spanCount += present && !inside ? 1u : 0u;
                if (present) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = y;
                }
                inside = present;
            }
        }
        result.spans.reserve(spanCount);
        result.alpha.reserve(samples);
        result.mask.reserve(samples);
        if (samples > 0)
            result.bounds = QRect(QPoint(left, top), QPoint(right, bottom));
        for (int y = 0; y < maskImage.height(); ++y) {
            const auto* row = maskImage.constScanLine(y);
            bool inside = false;
            for (int x = 0; x < maskImage.width(); ++x) {
                const float a =
                    blurred[static_cast<std::size_t>(y) * rowWidth + static_cast<std::size_t>(x)];
                const bool present = contributes(a, row[x]);
                if (present) {
                    if (!inside)
                        result.spans.push_back({x, y, 0, result.alpha.size()});
                    ++result.spans.back().count;
                    result.alpha.push_back(a);
                    result.mask.push_back(row[x]);
                }
                inside = present;
            }
        }
        return result;
    }

    QImage tinted(const QRect& rasterBounds, const QColor& color) const {
        QImage image =
            snowCanvasAllocateZeroedImage(rasterBounds.size(), QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            return image;
        for (const auto& span : spans) {
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(span.y - rasterBounds.y())) +
                        span.x - rasterBounds.x();
            for (int x = 0; x < span.count; ++x) {
                const auto index = span.offset + static_cast<std::size_t>(x);
                row[x] = regionShadowPixel(alpha[index], mask[index], color);
            }
        }
        return image;
    }
};
} // namespace

struct ScreenshotRegionShadowRenderer::Cache {
    struct Tile {
        QRect core;
        QRect halo;
        QPainterPath path;
        bool admitted = false;
        bool hasCoverage = false;
        QRect rasterBounds;
        QImage shadow;
        QColor color;
        RegionShadowCoverage coverage;
    };
    struct Strip {
        QRect core;
        QRect halo;
        QImage image;
    };
    std::optional<ScreenshotRegionGeometry> region;
    int radius = -1;
    int width = -1;
    std::vector<Tile> tiles;
    std::vector<Strip> strips;
    std::vector<std::size_t> stripVisibleTiles;
    int stripHeight = 0;
    int stripHaloRows = 0;
    QColor stripColor;
    std::size_t coverageBytes = 0;
    std::size_t publishedBytes = 0;

    ~Cache() {
        g_liveRegionCacheBytes -= publishedBytes;
    }

    std::size_t bytes() const {
        std::size_t result = coverageBytes;
        for (const auto& tile : tiles)
            result += static_cast<std::size_t>(tile.shadow.sizeInBytes());
        for (const auto& strip : strips)
            result += static_cast<std::size_t>(strip.image.sizeInBytes());
        return result;
    }

    void clearStrips() {
        strips.clear();
        stripVisibleTiles.clear();
        stripHeight = 0;
        stripHaloRows = 0;
        stripColor = {};
    }

    void publishBytes() {
        g_liveRegionCacheBytes -= publishedBytes;
        publishedBytes = bytes();
        g_liveRegionCacheBytes += publishedBytes;
    }

    void prepare(const ScreenshotRegionGeometry& localRegion, int cornerRadius, int shadowWidth) {
        if (region == localRegion && radius == cornerRadius && width == shadowWidth)
            return;
        tiles.clear();
        clearStrips();
        coverageBytes = 0;
        region = localRegion;
        radius = cornerRadius;
        width = shadowWidth;
        const QSize contentSize = localRegion.boundingRect().size();
        const QRect output(QPoint(), contentSize + QSize(width * 2, width * 2));
        const QPainterPath path =
            screenshotRegionPath(localRegion, radius).translated(width, width);
        const auto append = [&](const QPainterPath& tilePath, const QRect& core,
                                const QRect& halo) {
            if (!tilePath.intersects(QRectF(halo)) || tilePath.contains(QRectF(halo)))
                return;
            Tile tile;
            tile.core = core;
            tile.halo = halo;
            tile.path = tilePath;
            tiles.push_back(std::move(tile));
        };
        const auto appendRegion = [&](const QPainterPath& tilePath, const QRect& bounds,
                                      bool tiled) {
            if (!tiled) {
                append(tilePath, bounds, bounds);
                return;
            }
            // Match export's grid and halo exactly: changing their origin can
            // change the last bit of the finite-support float running sums.
            const int side = std::max(512, width * 8);
            for (int y = bounds.top(); y <= bounds.bottom(); y += side) {
                for (int x = bounds.left(); x <= bounds.right(); x += side) {
                    const QRect core = QRect(x, y, side, side).intersected(bounds);
                    const QRect halo = core.adjusted(-width - 2, -width - 2, width + 2, width + 2)
                                           .intersected(output);
                    append(tilePath, core, halo);
                }
            }
        };
        const auto sparse = sparseRegionTiles(path, output, width);
        if (!sparse.empty()) {
            for (const auto& tile : sparse)
                appendRegion(tile.path, tile.bounds,
                             qint64(tile.bounds.width()) * tile.bounds.height() > 1024 * 1024);
        } else {
            appendRegion(path, output, qint64(output.width()) * output.height() > 1024 * 1024);
        }
        publishBytes();
    }
};

ScreenshotRegionShadowRenderer::ScreenshotRegionShadowRenderer()
    : m_cache(std::make_unique<Cache>()) {}

ScreenshotRegionShadowRenderer::~ScreenshotRegionShadowRenderer() = default;

void ScreenshotRegionShadowRenderer::reset() {
    m_cache = std::make_unique<Cache>();
}

std::size_t ScreenshotRegionShadowRenderer::retainedBytes() const {
    return m_cache->bytes();
}

void ScreenshotRegionShadowRenderer::render(
    QPainter& painter, const ScreenshotRegionGeometry& region, int cornerRadius, int shadowWidth,
    const QColor& shadowColor, const QTransform& canvasToView, const QRegion& exposedView) {
    const QRect bounds = region.boundingRect();
    shadowWidth = std::max(0, shadowWidth);
    if (bounds.isEmpty() || shadowWidth == 0 || exposedView.isEmpty())
        return;
    const QColor color = shadowColor.isValid() ? shadowColor : QColor(0x33, 0x33, 0x33);
    const int radius = region.custom() ? 0 : std::max(0, cornerRadius);
    auto& cache = *m_cache;
    cache.prepare(region.translated(-bounds.topLeft()), radius, shadowWidth);
    const QRectF outputBounds(
        bounds.adjusted(-shadowWidth, -shadowWidth, shadowWidth, shadowWidth));
    const QRectF fullTarget = canvasToView.mapRect(outputBounds);
    QTransform imageToView;
    imageToView.translate(fullTarget.left(), fullTarget.top());
    imageToView.scale(fullTarget.width() / outputBounds.width(),
                      fullTarget.height() / outputBounds.height());
    painter.save();
    painter.setWorldTransform(imageToView, true);
    const auto imageForTile = [&](Cache::Tile& tile) {
        QImage shadow = tile.shadow;
        if (!tile.admitted || tile.color != color) {
            if (tile.hasCoverage) {
                shadow = tile.coverage.tinted(tile.rasterBounds, color);
            } else {
                const auto mask =
                    regionMask(tile.halo.size(), tile.path.translated(-tile.halo.topLeft()));
                if (mask.isNull())
                    return QImage{};
                const auto blurred = regionBlur(mask, shadowWidth);
                auto coverage = RegionShadowCoverage::fromBlur(mask, blurred);
                tile.rasterBounds =
                    coverage.bounds.isEmpty()
                        ? QRect()
                        : coverage.bounds.adjusted(-1, -1, 1, 1).intersected(mask.rect());
                if (cache.tiles.size() == 1 && tile.core == tile.halo &&
                    tile.halo.topLeft().isNull())
                    tile.rasterBounds = mask.rect();
                shadow = coverage.tinted(tile.rasterBounds, color);
                if (shadow.isNull() && !tile.rasterBounds.isEmpty())
                    return shadow;
                g_diagnostics.regionScratchPeakBytes =
                    std::max(g_diagnostics.regionScratchPeakBytes,
                             static_cast<std::size_t>(mask.sizeInBytes() + shadow.sizeInBytes()) +
                                 blurred.capacity() * sizeof(float) + coverage.bytes());
                // Admit complete color-independent assets by their actual
                // boundary size. No entry is evicted while scanning a frame,
                // so a larger working set cannot cause scan-order LRU churn.
                if (coverage.bytes() + static_cast<std::size_t>(shadow.sizeInBytes()) <=
                    kLiveRegionCacheByteLimit - cache.bytes()) {
                    cache.coverageBytes += coverage.bytes();
                    tile.coverage = std::move(coverage);
                    tile.hasCoverage = true;
                    tile.admitted = true;
                }
            }
            if (shadow.isNull() && !tile.rasterBounds.isEmpty())
                return shadow;
            if (tile.admitted) {
                tile.shadow = shadow;
                tile.color = color;
            }
        }
        return shadow;
    };
    const QTransform deviceToImage = painter.deviceTransform().inverted();
    const qreal edgeGuardX =
        std::max<qreal>(2, std::abs(deviceToImage.m11()) + std::abs(deviceToImage.m21()) + 1);
    const qreal edgeGuardY =
        std::max<qreal>(2, std::abs(deviceToImage.m12()) + std::abs(deviceToImage.m22()) + 1);
    const qreal fixedStep = deviceToImage.m11() * 65536.0;
    const bool exactStep = std::abs(fixedStep - std::round(fixedStep)) < 1.0e-8;
    const bool singleFullTile = cache.tiles.size() == 1 &&
                                cache.tiles.front().core == cache.tiles.front().halo &&
                                cache.tiles.front().halo.topLeft().isNull();
    const bool downsamples =
        std::abs(deviceToImage.m11()) > 1.0 || std::abs(deviceToImage.m22()) > 1.0;
    if ((!exactStep || downsamples) && !singleFullTile) {
        // Qt's nearest texture fetch advances a truncated 16.16 horizontal
        // step from each span's start. Independent tile clips restart that
        // accumulation, which can select a neighboring pixel after a long
        // transparent gap. Horizontal strips keep the original full-image span
        // origins, while bounding temporary storage independently of its area.
        constexpr int kStripByteLimit = 4 * 1024 * 1024;
        const int outputWidth = qRound(outputBounds.width());
        const int outputHeight = qRound(outputBounds.height());
        const int haloRows = std::min(outputHeight, qCeil(edgeGuardY));
        const int stripHeight =
            std::max(1, std::min(512, kStripByteLimit / (outputWidth * 4) - haloRows * 2));
        std::vector<std::size_t> visibleTiles;
        for (std::size_t index = 0; index < cache.tiles.size(); ++index) {
            if (exposedView.intersects(
                    imageToView.mapRect(QRectF(cache.tiles[index].core)).toAlignedRect()))
                visibleTiles.push_back(index);
        }
        if (cache.stripHeight != stripHeight || cache.stripHaloRows != haloRows ||
            cache.stripColor != color || cache.stripVisibleTiles != visibleTiles) {
            // A cached strip contains complete cores for this visible tile set.
            // New exposure must not reuse a strip assembled without those cores.
            cache.clearStrips();
            cache.stripHeight = stripHeight;
            cache.stripHaloRows = haloRows;
            cache.stripColor = color;
            cache.stripVisibleTiles = visibleTiles;
            for (int y = 0; y < outputHeight; y += stripHeight) {
                const QRect core(0, y, outputWidth, std::min(stripHeight, outputHeight - y));
                const QRect halo = core.adjusted(0, -haloRows, 0, haloRows)
                                       .intersected(QRect(0, 0, outputWidth, outputHeight));
                cache.strips.push_back({core, halo, {}});
            }
        }
        // Primary boundary assets get first admission to the shared budget.
        // Otherwise the first large strips can crowd out later visible tiles,
        // causing mask rasterization and blur on every subsequent repaint.
        for (const auto index : visibleTiles)
            imageForTile(cache.tiles[index]);
        for (auto& cachedStrip : cache.strips) {
            const auto& core = cachedStrip.core;
            const auto& halo = cachedStrip.halo;
            if (!exposedView.intersects(imageToView.mapRect(QRectF(core)).toAlignedRect()))
                continue;
            QImage strip = cachedStrip.image;
            if (strip.isNull()) {
                const bool hasBoundary =
                    std::any_of(visibleTiles.begin(), visibleTiles.end(), [&](const auto index) {
                        return cache.tiles[index].core.intersects(halo);
                    });
                if (!hasBoundary)
                    continue;
                strip =
                    snowCanvasAllocateZeroedImage(halo.size(), QImage::Format_ARGB32_Premultiplied);
                if (strip.isNull())
                    continue;
                ++g_diagnostics.regionStripBuilds;
                g_diagnostics.regionScratchPeakBytes =
                    std::max(g_diagnostics.regionScratchPeakBytes,
                             static_cast<std::size_t>(strip.sizeInBytes()));
                bool complete = true;
                {
                    QPainter stripPainter(&strip);
                    for (const auto index : visibleTiles) {
                        auto& tile = cache.tiles[index];
                        if (!tile.core.intersects(halo))
                            continue;
                        const QImage shadow = imageForTile(tile);
                        if (shadow.isNull() && (!tile.admitted || !tile.rasterBounds.isEmpty())) {
                            complete = false;
                            continue;
                        }
                        stripPainter.save();
                        stripPainter.setClipRect(tile.core.translated(-halo.topLeft()));
                        stripPainter.drawImage(tile.halo.topLeft() + tile.rasterBounds.topLeft() -
                                                   halo.topLeft(),
                                               shadow);
                        stripPainter.restore();
                    }
                }
                if (complete && static_cast<std::size_t>(strip.sizeInBytes()) <=
                                    kLiveRegionCacheByteLimit - cache.bytes())
                    cachedStrip.image = strip;
            }
            QRectF clip(core);
            if (core.top() == 0)
                clip.setTop(-edgeGuardY);
            if (core.bottom() == outputHeight - 1)
                clip.setBottom(outputHeight + edgeGuardY);
            clip.setLeft(-edgeGuardX);
            clip.setRight(outputWidth + edgeGuardX);
            painter.save();
            painter.setClipRect(clip, Qt::IntersectClip);
            painter.drawImage(halo.topLeft(), strip);
            painter.restore();
        }
        painter.restore();
        cache.publishBytes();
        return;
    }
    cache.clearStrips();
    for (auto& tile : cache.tiles) {
        const QRectF target = imageToView.mapRect(QRectF(tile.core));
        if (!exposedView.intersects(target.toAlignedRect()))
            continue;
        const QImage shadow = imageForTile(tile);
        // Share the full output's transform and clip cores instead of drawing
        // separate target rectangles. Image-edge antialiasing and independent
        // source-scale rounding would otherwise expose seams at fractional DPR.
        painter.save();
        if (tile.core != tile.halo) {
            QRectF clip(tile.core);
            // The original full image supplies antialiased perimeter coverage.
            // An integer QRegion clip at that same fractional edge would cut it
            // off, so clip only internal core edges.
            if (tile.core.left() == 0)
                clip.setLeft(-edgeGuardX);
            if (tile.core.top() == 0)
                clip.setTop(-edgeGuardY);
            if (tile.core.right() == outputBounds.width() - 1)
                clip.setRight(outputBounds.width() + edgeGuardX);
            if (tile.core.bottom() == outputBounds.height() - 1)
                clip.setBottom(outputBounds.height() + edgeGuardY);
            painter.setClipRect(clip, Qt::IntersectClip);
        }
        painter.drawImage(tile.halo.topLeft() + tile.rasterBounds.topLeft(), shadow);
        painter.restore();
    }
    painter.restore();
    cache.publishBytes();
}

QPainterPath screenshotSelectionPath(const QRectF& contentBounds,
                                     const std::optional<ScreenshotRegionGeometry>& region,
                                     qreal cornerRadius, qreal regionScale) {
    if (!region)
        return roundedHole(contentBounds, cornerRadius);
    if (!std::isfinite(regionScale) || regionScale <= 0)
        return {};
    QTransform transform;
    transform.translate(contentBounds.x(), contentBounds.y());
    transform.scale(regionScale, regionScale);
    return transform.map(region->custom()
                             ? region->path(regionScale)
                             : screenshotRegionPath(*region, cornerRadius / regionScale));
}

ScreenshotResultStyle ScreenshotResultCompositor::scaledStyle(const ScreenshotResultStyle& style,
                                                              qreal scale) {
    auto result = normalizedStyle(style);
    result.regionScale *= scale;
    result.cornerRadius = qRound(result.cornerRadius * scale);
    result.shadowWidth = qRound(result.shadowWidth * scale);
    result.borderWidth = qRound(result.borderWidth * scale);
    return result;
}

std::shared_ptr<const ScreenshotLayoutCompactionPlan>
ScreenshotResultCompositor::layoutCompactionForContent(const QSize& contentPixelSize,
                                                       const ScreenshotResultStyle& style,
                                                       ScreenshotLayoutCompaction options,
                                                       qreal scale) {
    if (!options.enabled || !style.region)
        return {};
    const auto output = scaledStyle(style, scale);
    return screenshotLayoutCompactionPlan(*output.region, contentPixelSize, output.regionScale,
                                          options, output.effectWidth());
}

ScreenshotResultStyle
ScreenshotResultCompositor::normalizedStyle(const ScreenshotResultStyle& style) {
    ScreenshotResultStyle normalized = style;
    normalized.backgroundOpacityPercent = std::clamp(normalized.backgroundOpacityPercent, 0, 100);
    normalized.cornerRadius = std::max(normalized.cornerRadius, 0);
    if (normalized.region && normalized.region->custom())
        normalized.cornerRadius = 0;
    normalized.shadowWidth = std::max(normalized.shadowWidth, 0);
    normalized.borderWidth = std::max(normalized.borderWidth, 0);
    if (!normalized.borderColor.isValid())
        normalized.borderColor = QColor(Qt::white);
    normalized.borderColor.setAlpha(255);
    if (!normalized.shadowColor.isValid()) {
        normalized.shadowColor = QColor(0x33, 0x33, 0x33);
    }
    return normalized;
}

ScreenshotResultLayout ScreenshotResultCompositor::layoutForContent(
    const QSize& contentPixelSize, const ScreenshotResultStyle& style, qreal devicePixelRatio) {
    if (!contentPixelSize.isValid() || contentPixelSize.isEmpty() ||
        !std::isfinite(devicePixelRatio) || devicePixelRatio <= 0 ||
        !std::isfinite(style.regionScale) || style.regionScale <= 0) {
        return {};
    }
    const ScreenshotResultStyle normalized = normalizedStyle(style);
    const qreal dpr = std::max<qreal>(1.0, devicePixelRatio);
    const qreal physicalEffect = std::round(normalized.effectWidth() * dpr);
    const qreal width = contentPixelSize.width() + 2 * physicalEffect;
    const qreal height = contentPixelSize.height() + 2 * physicalEffect;
    // Expansion must remain representable before converting the padding to int.
    // Each export entry point retains its own existing raster allocation budget.
    if (!std::isfinite(width) || !std::isfinite(height) ||
        width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max())
        return {};
    const int effect = static_cast<int>(physicalEffect);
    ScreenshotResultLayout layout;
    layout.effectInsets = QMargins(effect, effect, effect, effect);
    layout.outputRect = QRect(QPoint(), contentPixelSize + QSize(effect * 2, effect * 2));
    layout.contentRect = QRect(QPoint(effect, effect), contentPixelSize);
    layout.devicePixelRatio = dpr;
    return layout;
}

QImage ScreenshotResultCompositor::normalizeImage(const QImage& image) {
    return snow_shot::image::normalizeScreenshotDisplayImage(image);
}

QImage ScreenshotResultCompositor::maskBackgroundForSelection(QImage background,
                                                              const QImage& content,
                                                              const QPainterPath& selectionPath,
                                                              const QPoint& contentOffset) {
    if (background.format() != QImage::Format_ARGB32_Premultiplied ||
        content.format() != QImage::Format_ARGB32_Premultiplied ||
        !background.rect().contains(QRect(contentOffset, content.size())))
        return {};
    return backgroundOutsideSelection(std::move(background), content, selectionPath, contentOffset);
}

QImage ScreenshotResultCompositor::compose(const QImage& content,
                                           const ScreenshotResultStyle& style,
                                           qreal devicePixelRatio, qreal outputOpacity,
                                           const QImage& background) {
    QImage normalizedContent = normalizeImage(content);
    if (normalizedContent.isNull()) {
        return {};
    }
    const ScreenshotResultStyle normalized = normalizedStyle(style);
    const auto layout = layoutForContent(normalizedContent.size(), normalized, devicePixelRatio);
    if (!layout.isValid())
        return {};
    if (normalized.backgroundOpacityPercent > 0 && !background.isNull()) {
        auto selectedStyle = normalized;
        selectedStyle.backgroundOpacityPercent = 0;
        QImage result = compose(normalizedContent, selectedStyle, devicePixelRatio);
        if (result.isNull() || result.size() != background.size())
            return {};
        const auto path = screenshotSelectionPath(layout.contentRect, normalized.region,
                                                  normalized.cornerRadius * layout.devicePixelRatio,
                                                  normalized.regionScale);
        const auto retained = maskBackgroundForSelection(
            normalizeImage(background), normalizedContent, path, layout.contentRect.topLeft());
        if (retained.isNull())
            return {};
        QPainter painter(&result);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
        painter.setOpacity(normalized.backgroundOpacityPercent / 100.0);
        painter.drawImage(QPoint(), retained);
        painter.end();
        return applyOutputOpacity(std::move(result), outputOpacity);
    }
    if (normalized.region && normalized.borderType == ScreenshotSelectionBorderType::Shadow)
        return composeRegion(normalizedContent, normalized, devicePixelRatio, outputOpacity);
    if (!normalized.region && normalized.cornerRadius == 0 && normalized.effectWidth() == 0)
        return applyOutputOpacity(std::move(normalizedContent), outputOpacity);
    ++g_diagnostics.selectionSizedTransientAllocations;
    QImage output =
        snowCanvasAllocateImage(layout.outputRect.size(), QImage::Format_ARGB32_Premultiplied);
    if (output.isNull())
        return {};
    output.setColorSpace(normalizedContent.colorSpace());
    output.setDevicePixelRatio(1.0);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.drawImage(layout.contentRect, normalizedContent);

    const qreal physicalRadius = normalized.cornerRadius * layout.devicePixelRatio;
    const auto path = screenshotSelectionPath(layout.contentRect, normalized.region, physicalRadius,
                                              normalized.regionScale);
    if (normalized.cornerRadius > 0 || normalized.region) {
        const auto mask = regionMask(output.size(), path);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.drawImage(QPoint(), mask);
    }
    if (normalized.effectWidth() > 0) {
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
        if (normalized.borderType == ScreenshotSelectionBorderType::Border) {
            ScreenshotSelectionShadowRenderer::renderBorder(
                painter, path, layout.effectInsets.left(), normalized.borderColor);
        } else {
            // Composition draws this shadow once; retain assets only for live surfaces.
            renderShadow(painter, layout.contentRect, physicalRadius, layout.effectInsets.left(),
                         normalized.shadowColor, 1.0, false);
        }
    }
    painter.end();
    return applyOutputOpacity(std::move(output), outputOpacity);
}

namespace {
// SourceOver and DestinationOut leave the destination unchanged at zero source
// alpha, so inverse coverage is valid for both painting and erasing an exterior.
void paintSelectionExterior(QPainter& painter, const QRectF& viewportBounds,
                            const QPainterPath& selectionPath, const QColor& color,
                            QPainter::CompositionMode composition) {
    if (viewportBounds.isEmpty())
        return;
    painter.save();
    painter.setCompositionMode(composition);
    painter.setClipRect(viewportBounds, Qt::IntersectClip);
    if (selectionPath.fillRule() == Qt::OddEvenFill) {
        // Adding one enclosing contour inverts parity without flattening curves.
        // It must contain the entire path, including content beyond the viewport.
        QPainterPath outside;
        outside.addRect(viewportBounds.united(selectionPath.boundingRect()));
        outside.addPath(selectionPath);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillPath(outside, color);
    } else {
        // Winding coverage cannot be inverted by adding a contour: winding
        // counts can have either sign or magnitude. Invert raster coverage instead,
        // preserving the original curves and fill rule at the actual device phase.
        const auto device = painter.deviceTransform();
        bool invertible = false;
        const auto inverse = device.inverted(&invertible);
        if (!invertible) {
            painter.restore();
            return;
        }
        const auto path = device.map(selectionPath);
        const QRect pixels = device.mapRect(painter.clipBoundingRect()).toAlignedRect();
        constexpr int side = 256;
        QImage mask;
        painter.setWorldTransform(inverse, true);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        for (int y = pixels.top(); y <= pixels.bottom(); y += side) {
            for (int x = pixels.left(); x <= pixels.right(); x += side) {
                const QRect tile = QRect(x, y, side, side).intersected(pixels);
                const QRectF neighborhood = QRectF(tile).adjusted(-1, -1, 1, 1);
                if (path.contains(neighborhood))
                    continue;
                if (!path.intersects(neighborhood)) {
                    painter.fillRect(tile, color);
                    continue;
                }
                if (mask.isNull()) {
                    mask = snowCanvasAllocateImage(
                        QSize(std::min(side, pixels.width()), std::min(side, pixels.height())),
                        QImage::Format_ARGB32_Premultiplied);
                    if (mask.isNull()) {
                        painter.restore();
                        return;
                    }
                    g_diagnostics.regionScratchPeakBytes =
                        std::max(g_diagnostics.regionScratchPeakBytes,
                                 static_cast<std::size_t>(mask.sizeInBytes()));
                }
                mask.fill(color);
                {
                    QPainter raster(&mask);
                    raster.setRenderHint(QPainter::Antialiasing);
                    raster.translate(-tile.topLeft());
                    raster.setCompositionMode(QPainter::CompositionMode_DestinationOut);
                    raster.fillPath(path, Qt::black);
                }
                ++g_diagnostics.regionMaskBuilds;
                painter.drawImage(tile.topLeft(), mask, QRect(QPoint(), tile.size()));
            }
        }
    }
    painter.restore();
}
} // namespace

void ScreenshotResultCompositor::fillOutsideSelection(QPainter& painter,
                                                      const QRectF& viewportBounds,
                                                      const QPainterPath& selectionPath,
                                                      const QColor& color) {
    paintSelectionExterior(painter, viewportBounds, selectionPath, color,
                           QPainter::CompositionMode_SourceOver);
}

void ScreenshotResultCompositor::maskLiveSurface(QPainter& painter, const QRectF& viewportBounds,
                                                 const QPainterPath& selectionPath) {
    paintSelectionExterior(painter, viewportBounds, selectionPath, Qt::black,
                           QPainter::CompositionMode_DestinationOut);
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
    const qreal viewShadow = normalized.effectWidth() * viewScale;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto contentPath = screenshotSelectionPath(
        contentBounds,
        normalized.borderType == ScreenshotSelectionBorderType::Border ? normalized.region
                                                                       : std::nullopt,
        viewRadius, normalized.regionScale * viewScale);
    maskLiveSurface(painter, viewportBounds, contentPath);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
    if (normalized.borderType == ScreenshotSelectionBorderType::Border) {
        ScreenshotSelectionShadowRenderer::renderBorder(painter, contentPath, viewShadow,
                                                        normalized.borderColor);
    } else {
        ScreenshotSelectionShadowRenderer::renderResultShadow(painter, contentBounds, viewRadius,
                                                              viewShadow, normalized.shadowColor,
                                                              devicePixelRatio);
    }
    painter.restore();
}

ScreenshotSelectionShadowDiagnostics
ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread() {
    const auto& cache = shadowCacheForCurrentThread();
    ScreenshotSelectionShadowDiagnostics result = g_diagnostics;
    result.retainedBytes = cache.bytes;
    result.retainedEntries = cache.entries.size();
    result.regionCacheRetainedBytes =
        static_cast<std::size_t>(g_regionCompositionCache.mask.sizeInBytes() +
                                 g_regionCompositionCache.shadow.sizeInBytes()) +
        g_liveRegionCacheBytes;
    result.regionPathCacheElements = static_cast<std::size_t>(
        g_regionPathCache.path.elementCount() + g_regionCompositionCache.path.elementCount());
    return result;
}

void ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread() {
    g_diagnostics = ScreenshotSelectionShadowDiagnostics{};
}

void ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread() {
    shadowCacheForCurrentThread() = ShadowCache{};
    g_regionPathCache = {};
    g_regionCompositionCache = {};
}

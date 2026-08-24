#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"
#include "snow_draw_engine_qt/snow_transient_image.h"

#include <QBrush>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <vector>

namespace {
constexpr qreal kPeakAlphaScale = 0.36;
constexpr int kCacheEntryLimit = 8;
constexpr std::size_t kCacheByteLimit = 16u * 1024u * 1024u;
constexpr int kDprQuantization = 64;
constexpr int kCheckerTileSize = 6;

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

int quantizedDpr(qreal dpr) {
    return std::max(1, qRound(std::max<qreal>(1.0, dpr) * kDprQuantization));
}

QImage checkerboard() {
    static const QImage image = [] {
        QImage result(QSize(kCheckerTileSize * 2, kCheckerTileSize * 2),
                      QImage::Format_ARGB32_Premultiplied);
        result.fill(QColor(QStringLiteral("#ffffff")));
        QPainter painter(&result);
        painter.fillRect(QRect(0, 0, kCheckerTileSize, kCheckerTileSize),
                         QColor(QStringLiteral("#f0f0f0")));
        painter.fillRect(
            QRect(kCheckerTileSize, kCheckerTileSize, kCheckerTileSize, kCheckerTileSize),
            QColor(QStringLiteral("#f0f0f0")));
        return result;
    }();
    return image;
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
    const qreal peakAlpha = kPeakAlphaScale * color.alphaF();
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
                                qreal cornerRadius, qreal shadowWidth) {
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
    painter.fillPath(perimeter, QBrush(checkerboard()));
    painter.restore();
}

void renderShadow(QPainter& painter, const QRectF& selectionBounds, qreal cornerRadius,
                  qreal shadowWidth, const QColor& shadowColor, qreal dpr) {
    if (selectionBounds.isEmpty() || shadowWidth <= 0.0) {
        return;
    }
    const qreal effectiveDevicePixelRatio = std::max<qreal>(1.0, dpr);
    const int physicalRadius =
        qRound(std::max<qreal>(0.0, cornerRadius) * effectiveDevicePixelRatio);
    const int physicalShadowWidth = std::max(1, qRound(shadowWidth * effectiveDevicePixelRatio));
    const QColor color = shadowColor.isValid() ? shadowColor : QColor(0x33, 0x33, 0x33);
    const QImage asset =
        shadowAsset(physicalRadius, physicalShadowWidth, color, effectiveDevicePixelRatio);
    paintNineSlice(painter, selectionBounds, cornerRadius, shadowWidth, asset);
}
} // namespace

void ScreenshotSelectionShadowRenderer::renderPreview(QPainter& painter,
                                                      const QRectF& selectionBounds,
                                                      qreal cornerRadius, qreal shadowWidth,
                                                      const QColor& shadowColor,
                                                      qreal devicePixelRatio) {
    paintCheckerboardPerimeter(painter, selectionBounds, cornerRadius, shadowWidth);
    renderShadow(painter, selectionBounds, cornerRadius, shadowWidth, shadowColor,
                 devicePixelRatio);
}

QImage ScreenshotSelectionShadowRenderer::composeExport(const QImage& content, int cornerRadius,
                                                        int shadowWidth,
                                                        const QColor& shadowColor) {
    return ScreenshotResultCompositor::compose(
        content, ScreenshotResultStyle{cornerRadius, shadowWidth, shadowColor},
        std::max<qreal>(1.0, content.devicePixelRatio()));
}

void ScreenshotSelectionShadowRenderer::renderResultShadow(
    QPainter& painter, const QRectF& contentBounds, qreal cornerRadius, qreal shadowWidth,
    const QColor& shadowColor, qreal devicePixelRatio) {
    renderShadow(painter, contentBounds, cornerRadius, shadowWidth, shadowColor,
                 devicePixelRatio);
}

ScreenshotResultStyle ScreenshotResultCompositor::normalizedStyle(
    const ScreenshotResultStyle& style) {
    ScreenshotResultStyle normalized = style;
    normalized.cornerRadius = std::clamp(
        normalized.cornerRadius, 0,
        snow_shot::presentation::kScreenshotSelectionCornerRadiusMax);
    normalized.shadowWidth = std::clamp(
        normalized.shadowWidth, 0,
        snow_shot::presentation::kScreenshotSelectionShadowWidthMax);
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
                                           qreal devicePixelRatio) {
    const QImage normalizedContent = normalizeImage(content);
    if (normalizedContent.isNull()) {
        return {};
    }
    const ScreenshotResultStyle normalized = normalizedStyle(style);
    if (normalized.cornerRadius == 0 && normalized.shadowWidth == 0) {
        return normalizedContent;
    }
    const ScreenshotResultLayout layout =
        layoutForContent(normalizedContent.size(), normalized, devicePixelRatio);
    if (!layout.isValid()) {
        return {};
    }

    QImage output = snow_draw_engine_qt::allocateTransientImage(
        layout.outputRect.size(), QImage::Format_ARGB32_Premultiplied);
    if (output.isNull()) {
        return {};
    }
    output.setDevicePixelRatio(1.0);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.drawImage(layout.contentRect, normalizedContent);

    const qreal physicalRadius = normalized.cornerRadius * layout.devicePixelRatio;
    if (normalized.cornerRadius > 0) {
        QImage mask = snow_draw_engine_qt::allocateTransientImage(
            output.size(), QImage::Format_ARGB32_Premultiplied);
        if (mask.isNull()) {
            return {};
        }
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
        ScreenshotSelectionShadowRenderer::renderResultShadow(
            painter, layout.contentRect, physicalRadius, layout.effectInsets.left(),
            normalized.shadowColor, 1.0);
    }
    painter.end();
    return output;
}

void ScreenshotResultCompositor::finishLiveSurface(
    QPainter& painter, const QRectF& viewportBounds, const QRectF& contentBounds,
    const ScreenshotResultStyle& style, qreal devicePixelRatio, qreal canvasToViewScale) {
    if (viewportBounds.isEmpty() || contentBounds.isEmpty()) {
        return;
    }
    const ScreenshotResultStyle normalized = normalizedStyle(style);
    const qreal viewScale = std::max<qreal>(0.0, canvasToViewScale);
    const qreal viewRadius = normalized.cornerRadius * viewScale;
    const qreal viewShadow = normalized.shadowWidth * viewScale;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath outside;
    outside.setFillRule(Qt::OddEvenFill);
    outside.addRect(viewportBounds);
    outside.addPath(roundedHole(contentBounds, viewRadius));
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    painter.fillPath(outside, Qt::black);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
    ScreenshotSelectionShadowRenderer::renderResultShadow(
        painter, contentBounds, viewRadius, viewShadow, normalized.shadowColor,
        devicePixelRatio);
    painter.restore();
}

ScreenshotSelectionShadowDiagnostics
ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread() {
    ScreenshotSelectionShadowDiagnostics result = g_diagnostics;
    result.retainedBytes = g_cache.bytes;
    result.retainedEntries = g_cache.entries.size();
    return result;
}

void ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread() {
    g_diagnostics = ScreenshotSelectionShadowDiagnostics{};
}

void ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread() {
    g_cache = ShadowCache{};
}

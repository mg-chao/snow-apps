#include "snow_canvas_fill_render.h"

#include <QBrush>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace snow_canvas_fill_render {
namespace {

constexpr int kHatchTextureSupersampling = 4;
constexpr qsizetype kHatchTextureCacheCapacity = 128;
constexpr double kTextFillReferenceFontSize = 24.0;

struct HatchTextureKey {
    QRgb rgba = 0;
    quint64 lineWidthBits = 0;

    bool operator==(const HatchTextureKey& other) const {
        return rgba == other.rgba && lineWidthBits == other.lineWidthBits;
    }
};

size_t qHash(const HatchTextureKey& key, size_t seed = 0) {
    return qHashMulti(seed, key.rgba, key.lineWidthBits);
}

struct HatchTexture {
    QImage image;
    double brushScale = 1.0;
    quint64 lastUse = 0;
};

struct HatchTextureCache {
    QHash<HatchTextureKey, HatchTexture> entries;
    quint64 clock = 0;
};

HatchTextureCache& currentHatchTextureCache() {
    thread_local HatchTextureCache cache;
    return cache;
}

quint64 doubleBits(double value) {
    quint64 bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double resolvedHatchLineWidth(double referenceStrokeWidth) {
    return std::clamp(1.0 + (referenceStrokeWidth - 1.0) * 0.6, 0.5, 6.0);
}

HatchTexture createHatchTexture(const QColor& color, double lineWidth) {
    const double spacing = std::clamp(lineWidth * 4.0, 3.0, 21.0);
    const double desiredTileSize = spacing * std::sqrt(2.0);
    const int physicalSize =
        qMax(1, static_cast<int>(std::ceil(desiredTileSize * kHatchTextureSupersampling)));
    const double sourceTileSize = static_cast<double>(physicalSize) / kHatchTextureSupersampling;
    const double brushScale = desiredTileSize / sourceTileSize;

    QImage image(physicalSize, physicalSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(kHatchTextureSupersampling);
    image.fill(Qt::transparent);

    QPainter texturePainter(&image);
    texturePainter.setRenderHint(QPainter::Antialiasing, true);
    texturePainter.setPen(QPen(color, lineWidth / brushScale, Qt::SolidLine, Qt::FlatCap));
    for (int diagonal = -1; diagonal <= 2; ++diagonal) {
        texturePainter.drawLine(QPointF(0.0, diagonal * sourceTileSize),
                                QPointF(sourceTileSize, (diagonal - 1) * sourceTileSize));
    }

    return HatchTexture{std::move(image), brushScale, 0};
}

const HatchTexture& hatchTexture(const QColor& color, double referenceStrokeWidth) {
    HatchTextureCache& cache = currentHatchTextureCache();
    const double lineWidth = resolvedHatchLineWidth(referenceStrokeWidth);
    const HatchTextureKey key{color.rgba(), doubleBits(lineWidth)};
    ++cache.clock;
    auto found = cache.entries.find(key);
    if (found == cache.entries.end()) {
        if (cache.entries.size() >= kHatchTextureCacheCapacity) {
            auto oldest = cache.entries.begin();
            for (auto candidate = cache.entries.begin(); candidate != cache.entries.end();
                 ++candidate) {
                if (candidate->lastUse < oldest->lastUse) {
                    oldest = candidate;
                }
            }
            cache.entries.erase(oldest);
        }
        found = cache.entries.insert(key, createHatchTexture(color, lineWidth));
    }
    found->lastUse = cache.clock;
    return found.value();
}

QBrush hatchBrush(const QColor& color, double referenceStrokeWidth, bool mirrored) {
    const HatchTexture& texture = hatchTexture(color, referenceStrokeWidth);
    QBrush brush(texture.image);
    QTransform transform;
    transform.scale(texture.brushScale, mirrored ? -texture.brushScale : texture.brushScale);
    brush.setTransform(transform);
    return brush;
}

QColor toQColor(const SnowColorRgba8& color) {
    return QColor(color.r, color.g, color.b, color.a);
}

} // namespace

void drawStyledFill(QPainter& painter, const QPainterPath& path, const SnowColorRgba8& fill,
                    SnowFillStyle style, double referenceStrokeWidth, double coordinateScale) {
    if (fill.a == 0) {
        return;
    }

    const QColor color = toQColor(fill);
    if (style == SNOW_FILL_STYLE_SOLID) {
        painter.fillPath(path, color);
        return;
    }
    if (!std::isfinite(referenceStrokeWidth) || !std::isfinite(coordinateScale) ||
        !(coordinateScale > 0.0)) {
        return;
    }

    QBrush brush = hatchBrush(color, referenceStrokeWidth, false);
    QTransform scaled = brush.transform();
    scaled.scale(coordinateScale, coordinateScale);
    brush.setTransform(scaled);
    painter.fillPath(path, brush);
    if (style == SNOW_FILL_STYLE_CROSS_LINE) {
        brush = hatchBrush(color, referenceStrokeWidth, true);
        scaled = brush.transform();
        scaled.scale(coordinateScale, coordinateScale);
        brush.setTransform(scaled);
        painter.fillPath(path, brush);
    }
}

void drawTextBackgroundFill(QPainter& painter, const QPainterPath& path, const SnowColorRgba8& fill,
                            SnowFillStyle style, double fontSize, double coordinateScale) {
    drawStyledFill(painter, path, fill, style, fontSize / kTextFillReferenceFontSize,
                   coordinateScale);
}

std::size_t hatchTextureCacheEntryCountForCurrentThread() {
    return static_cast<std::size_t>(currentHatchTextureCache().entries.size());
}

void resetHatchTextureCacheForCurrentThread() {
    currentHatchTextureCache() = {};
}

} // namespace snow_canvas_fill_render

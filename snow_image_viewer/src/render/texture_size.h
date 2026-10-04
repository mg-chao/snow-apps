#pragma once

#include <QRect>
#include <QSize>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace snow::image_viewer {

inline std::vector<QRect> textureTilesForLimit(const QSize& imageSize, int maximumTextureSize) {
    std::vector<QRect> tiles;
    if (!imageSize.isValid() || maximumTextureSize <= 0) {
        return tiles;
    }

    for (int y = 0; y < imageSize.height();) {
        const int height = std::min(maximumTextureSize, imageSize.height() - y);
        for (int x = 0; x < imageSize.width();) {
            const int width = std::min(maximumTextureSize, imageSize.width() - x);
            tiles.emplace_back(x, y, width, height);
            if (width == imageSize.width() - x)
                break;
            x += width;
        }
        if (height == imageSize.height() - y)
            break;
        y += height;
    }
    return tiles;
}

struct TextureArrayTilePlan final {
    QSize layerSize;
    int columns = 0;
    int rows = 0;
    std::vector<QRect> tiles;

    [[nodiscard]] bool isValid() const {
        return layerSize.isValid() && columns > 0 && rows > 0 &&
               tiles.size() == static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows);
    }

    [[nodiscard]] std::uint64_t allocatedPixels() const {
        return isValid() ? static_cast<std::uint64_t>(layerSize.width()) *
                               static_cast<std::uint64_t>(layerSize.height()) * tiles.size()
                         : 0;
    }
};

inline TextureArrayTilePlan textureArrayTilePlan(const QSize& imageSize, int maximumTextureSize,
                                                 int maximumLayers) {
    TextureArrayTilePlan best;
    if (!imageSize.isValid() || maximumTextureSize <= 0 || maximumLayers <= 0)
        return best;

    std::vector<int> dimensions;
    for (int value = 1; value <= maximumTextureSize;) {
        dimensions.push_back(value);
        if (value > maximumTextureSize / 2)
            break;
        value *= 2;
    }

    constexpr std::uint64_t kLayerPenaltyPixels = 16U * 1024U;
    std::uint64_t bestCost = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t bestLayers = std::numeric_limits<std::uint64_t>::max();
    for (const int tileHeight : dimensions) {
        const std::uint64_t rows = (static_cast<std::uint64_t>(imageSize.height()) +
                                    static_cast<std::uint64_t>(tileHeight) - 1U) /
                                   static_cast<std::uint64_t>(tileHeight);
        if (rows > static_cast<std::uint64_t>(maximumLayers))
            continue;
        for (const int tileWidth : dimensions) {
            const std::uint64_t columns = (static_cast<std::uint64_t>(imageSize.width()) +
                                           static_cast<std::uint64_t>(tileWidth) - 1U) /
                                          static_cast<std::uint64_t>(tileWidth);
            if (columns > static_cast<std::uint64_t>(maximumLayers) / rows)
                continue;
            const std::uint64_t layers = columns * rows;
            const std::uint64_t layerPixels =
                static_cast<std::uint64_t>(tileWidth) * static_cast<std::uint64_t>(tileHeight);
            if (layerPixels >
                (std::numeric_limits<std::uint64_t>::max() - layers * kLayerPenaltyPixels) /
                    layers) {
                continue;
            }
            const std::uint64_t cost = layerPixels * layers + layers * kLayerPenaltyPixels;
            if (cost < bestCost || (cost == bestCost && layers < bestLayers)) {
                bestCost = cost;
                bestLayers = layers;
                best.layerSize = QSize(tileWidth, tileHeight);
                best.columns = static_cast<int>(columns);
                best.rows = static_cast<int>(rows);
            }
        }
    }
    if (!best.layerSize.isValid())
        return {};

    best.tiles.reserve(static_cast<std::size_t>(best.columns) *
                       static_cast<std::size_t>(best.rows));
    for (int row = 0; row < best.rows; ++row) {
        const int y = row * best.layerSize.height();
        const int height = std::min(best.layerSize.height(), imageSize.height() - y);
        for (int column = 0; column < best.columns; ++column) {
            const int x = column * best.layerSize.width();
            const int width = std::min(best.layerSize.width(), imageSize.width() - x);
            best.tiles.emplace_back(x, y, width, height);
        }
    }
    return best;
}

} // namespace snow::image_viewer

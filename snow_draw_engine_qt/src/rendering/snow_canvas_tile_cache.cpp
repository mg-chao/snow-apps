#include "snow_canvas_tile_cache.h"

#include <QImage>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snow_canvas_tile_cache {
namespace {

constexpr std::size_t kTemporaryStripByteLimit = 8u * 1024u * 1024u;
constexpr std::size_t kBytesPerPixel = 4;

std::size_t layerIndex(Layer layer) {
    return layer == Layer::SpotlightCoverage ? 1u : 0u;
}

std::uint64_t realBits(qreal value) {
    const double converted = static_cast<double>(value);
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(converted));
    std::memcpy(&bits, &converted, sizeof(bits));
    return bits;
}

struct TileKey {
    const void* canvasNamespace = nullptr;
    Layer layer = Layer::Scene;
    int x = 0;
    int y = 0;
    QSize logicalSize;
    std::uint64_t dprBits = 0;
    std::uint64_t contentKey = 0;
    std::uint64_t revision = 0;

    bool operator==(const TileKey& other) const {
        return canvasNamespace == other.canvasNamespace && layer == other.layer && x == other.x &&
               y == other.y && logicalSize == other.logicalSize && dprBits == other.dprBits &&
               contentKey == other.contentKey && revision == other.revision;
    }
};

struct TileKeyHash {
    std::size_t operator()(const TileKey& key) const {
        std::size_t hash = std::hash<const void*>{}(key.canvasNamespace);
        hash ^= static_cast<std::size_t>(key.layer) * 0x9e3779b1u;
        hash ^= static_cast<std::size_t>(key.x) * 0x85ebca77u;
        hash ^= static_cast<std::size_t>(key.y) * 0xc2b2ae3du;
        hash ^= static_cast<std::size_t>(key.logicalSize.width()) * 0x27d4eb2fu;
        hash ^= static_cast<std::size_t>(key.logicalSize.height()) * 0x165667b1u;
        hash ^= static_cast<std::size_t>(key.dprBits ^ (key.dprBits >> 32));
        hash ^= static_cast<std::size_t>(key.contentKey ^ (key.contentKey >> 32));
        hash ^= static_cast<std::size_t>(key.revision ^ (key.revision >> 32));
        return hash;
    }
};

struct MaskKey {
    const void* canvasNamespace = nullptr;
    std::uint64_t value = 0;

    bool operator==(const MaskKey& other) const {
        return canvasNamespace == other.canvasNamespace && value == other.value;
    }
};

struct MaskKeyHash {
    std::size_t operator()(const MaskKey& key) const {
        const std::uint64_t folded = key.value ^ (key.value >> 32);
        return std::hash<const void*>{}(key.canvasNamespace) ^ static_cast<std::size_t>(folded);
    }
};

struct Tile {
    TileKey key;
    QRect physicalRect;
    QImage image;
    QRegion valid;
    bool fullyOpaque = false;
    std::list<TileKey>::iterator lru;
};

struct LayerSignature {
    bool initialized = false;
    QSize logicalSize;
    std::uint64_t dprBits = 0;
    std::uint64_t contentKey = 0;
    std::uint64_t revision = 0;
};

struct NamespaceState {
    std::array<LayerSignature, 2> layers;
};

struct RetainedMask {
    std::shared_ptr<MaskTile> tile;
    std::size_t bytes = 0;
    std::list<MaskKey>::iterator lru;
};

struct Cache {
    std::mutex mutex;
    std::unordered_map<TileKey, std::shared_ptr<Tile>, TileKeyHash> tiles;
    std::unordered_map<MaskKey, RetainedMask, MaskKeyHash> masks;
    std::unordered_map<const void*, NamespaceState> namespaces;
    std::array<std::list<TileKey>, 2> lru;
    std::list<MaskKey> maskLru;
    std::array<std::size_t, 2> layerBytes{};
    std::size_t maskBytes = 0;
    std::array<std::size_t, 2> pendingEvictions{};
    std::size_t pendingMaskEvictions = 0;
};

Cache& cache() {
    static Cache instance;
    return instance;
}

std::size_t layerByteLimit(Layer layer) {
    return layer == Layer::SpotlightCoverage ? kSpotlightByteLimit : kSceneByteLimit;
}

std::size_t imageBytes(const QImage& image) {
    return image.isNull() ? 0u : static_cast<std::size_t>(image.sizeInBytes());
}

QRegion logicalToPhysical(const QRegion& region, qreal dpr, int physicalPadding = 0) {
    QRegion physical;
    const qreal safeDpr = std::max<qreal>(0.01, dpr);
    for (const QRect& rect : region) {
        const int left = static_cast<int>(std::floor(rect.left() * safeDpr)) - physicalPadding;
        const int top = static_cast<int>(std::floor(rect.top() * safeDpr)) - physicalPadding;
        const int right =
            static_cast<int>(std::ceil((rect.right() + 1) * safeDpr)) + physicalPadding;
        const int bottom =
            static_cast<int>(std::ceil((rect.bottom() + 1) * safeDpr)) + physicalPadding;
        if (right > left && bottom > top) {
            physical += QRect(left, top, right - left, bottom - top);
        }
    }
    return physical;
}

QRegion physicalToLogical(const QRegion& region, qreal dpr) {
    QRegion logical;
    const qreal safeDpr = std::max<qreal>(0.01, dpr);
    for (const QRect& rect : region) {
        const int left = static_cast<int>(std::floor(rect.left() / safeDpr));
        const int top = static_cast<int>(std::floor(rect.top() / safeDpr));
        const int right = static_cast<int>(std::ceil((rect.right() + 1) / safeDpr));
        const int bottom = static_cast<int>(std::ceil((rect.bottom() + 1) / safeDpr));
        if (right > left && bottom > top) {
            logical += QRect(left, top, right - left, bottom - top);
        }
    }
    return logical;
}

QSize physicalSizeFor(const QSize& logicalSize, qreal dpr) {
    return QSize(qCeil(static_cast<qreal>(logicalSize.width()) * dpr),
                 qCeil(static_cast<qreal>(logicalSize.height()) * dpr));
}

void touchLocked(Cache& state, const std::shared_ptr<Tile>& tile) {
    const std::size_t index = layerIndex(tile->key.layer);
    state.lru[index].erase(tile->lru);
    state.lru[index].push_front(tile->key);
    tile->lru = state.lru[index].begin();
}

void eraseMasksForNamespaceLocked(Cache& state, const void* canvasNamespace) {
    for (auto iterator = state.masks.begin(); iterator != state.masks.end();) {
        if (iterator->first.canvasNamespace != canvasNamespace) {
            ++iterator;
            continue;
        }
        state.layerBytes[layerIndex(Layer::Scene)] -= iterator->second.bytes;
        state.maskBytes -= iterator->second.bytes;
        state.maskLru.erase(iterator->second.lru);
        iterator = state.masks.erase(iterator);
    }
}

void eraseLayerLocked(Cache& state, const void* canvasNamespace, Layer layer) {
    const std::size_t index = layerIndex(layer);
    for (auto iterator = state.tiles.begin(); iterator != state.tiles.end();) {
        if (iterator->first.canvasNamespace != canvasNamespace || iterator->first.layer != layer) {
            ++iterator;
            continue;
        }
        state.layerBytes[index] -= imageBytes(iterator->second->image);
        state.lru[index].erase(iterator->second->lru);
        iterator = state.tiles.erase(iterator);
    }
    if (layer == Layer::Scene) {
        eraseMasksForNamespaceLocked(state, canvasNamespace);
    }
}

void eraseNamespaceLocked(Cache& state, const void* canvasNamespace) {
    eraseLayerLocked(state, canvasNamespace, Layer::Scene);
    eraseLayerLocked(state, canvasNamespace, Layer::SpotlightCoverage);
    state.namespaces.erase(canvasNamespace);
}

void evictLayerLocked(Cache& state, Layer layer) {
    const std::size_t index = layerIndex(layer);
    const std::size_t limit = layerByteLimit(layer);
    while (state.layerBytes[index] > limit) {
        bool removed = false;
        if (layer == Layer::Scene && !state.maskLru.empty()) {
            const MaskKey key = state.maskLru.back();
            state.maskLru.pop_back();
            const auto found = state.masks.find(key);
            if (found != state.masks.end()) {
                state.layerBytes[index] -= found->second.bytes;
                state.maskBytes -= found->second.bytes;
                state.masks.erase(found);
                ++state.pendingMaskEvictions;
                removed = true;
            }
        }
        if (removed) {
            continue;
        }
        if (!state.lru[index].empty()) {
            const TileKey key = state.lru[index].back();
            state.lru[index].pop_back();
            const auto found = state.tiles.find(key);
            if (found != state.tiles.end()) {
                state.layerBytes[index] -= imageBytes(found->second->image);
                state.tiles.erase(found);
                ++state.pendingEvictions[index];
                removed = true;
            }
        }
        if (!removed) {
            break;
        }
    }
}

void evictLocked(Cache& state) {
    evictLayerLocked(state, Layer::Scene);
    evictLayerLocked(state, Layer::SpotlightCoverage);
}

void invalidateTilesLocked(Cache& state, const void* canvasNamespace, Layer layer,
                           const QRegion& logicalRegion, qreal dpr, const QSize& logicalSize) {
    const QSize physicalSize = physicalSizeFor(logicalSize, dpr);
    QRegion physical = logicalRegion.isEmpty() ? QRegion(QRect(QPoint(), physicalSize))
                                               : logicalToPhysical(logicalRegion, dpr, 1);
    for (auto& [key, tile] : state.tiles) {
        if (key.canvasNamespace != canvasNamespace || key.layer != layer) {
            continue;
        }
        tile->valid -= physical.translated(-tile->physicalRect.left(), -tile->physicalRect.top());
        tile->fullyOpaque = false;
    }
}

void rekeyRevisionLocked(Cache& state, const void* canvasNamespace, Layer layer,
                         LayerSignature& signature, std::uint64_t revision,
                         const QRegion& logicalDirtyRegion, qreal dpr) {
    if (signature.revision == revision) {
        if (!logicalDirtyRegion.isEmpty()) {
            invalidateTilesLocked(state, canvasNamespace, layer, logicalDirtyRegion, dpr,
                                  signature.logicalSize);
        }
        return;
    }

    const QRegion physicalDirty =
        logicalDirtyRegion.isEmpty()
            ? QRegion(QRect(QPoint(), physicalSizeFor(signature.logicalSize, dpr)))
            : logicalToPhysical(logicalDirtyRegion, dpr, 1);
    const std::size_t index = layerIndex(layer);
    std::vector<std::shared_ptr<Tile>> retained;
    retained.reserve(state.tiles.size());
    for (auto iterator = state.tiles.begin(); iterator != state.tiles.end();) {
        if (iterator->first.canvasNamespace != canvasNamespace || iterator->first.layer != layer ||
            iterator->first.contentKey != signature.contentKey ||
            iterator->first.revision != signature.revision) {
            ++iterator;
            continue;
        }
        retained.push_back(iterator->second);
        state.lru[index].erase(iterator->second->lru);
        iterator = state.tiles.erase(iterator);
    }
    for (const std::shared_ptr<Tile>& tile : retained) {
        tile->key.revision = revision;
        tile->valid -=
            physicalDirty.translated(-tile->physicalRect.left(), -tile->physicalRect.top());
        tile->fullyOpaque = false;
        state.lru[index].push_front(tile->key);
        tile->lru = state.lru[index].begin();
        state.tiles.emplace(tile->key, tile);
    }
    signature.revision = revision;
}

void prepareSignatureLocked(Cache& state, const RenderRequest& request) {
    NamespaceState& namespaceState = state.namespaces[request.canvasNamespace];
    LayerSignature& signature = namespaceState.layers[layerIndex(request.layer)];
    const std::uint64_t dprBits = realBits(request.devicePixelRatio);
    const bool contentChanged =
        !signature.initialized || signature.logicalSize != request.logicalSize ||
        signature.dprBits != dprBits || signature.contentKey != request.contentKey;
    if (contentChanged) {
        eraseLayerLocked(state, request.canvasNamespace, request.layer);
        signature = LayerSignature{
            true, request.logicalSize, dprBits, request.contentKey, request.sceneRevision,
        };
        return;
    }
    rekeyRevisionLocked(state, request.canvasNamespace, request.layer, signature,
                        request.sceneRevision, request.dirtyRegion, request.devicePixelRatio);
}

std::shared_ptr<Tile> acquireTile(Cache& state, const RenderRequest& request, int tileX, int tileY,
                                  const QSize& physicalSize) {
    const TileKey key{
        request.canvasNamespace,
        request.layer,
        tileX,
        tileY,
        request.logicalSize,
        realBits(request.devicePixelRatio),
        request.contentKey,
        request.sceneRevision,
    };
    const auto found = state.tiles.find(key);
    if (found != state.tiles.end()) {
        touchLocked(state, found->second);
        return found->second;
    }

    auto tile = std::make_shared<Tile>();
    tile->key = key;
    tile->physicalRect =
        QRect(tileX * kTilePhysicalSize, tileY * kTilePhysicalSize,
              std::min(kTilePhysicalSize, physicalSize.width() - tileX * kTilePhysicalSize),
              std::min(kTilePhysicalSize, physicalSize.height() - tileY * kTilePhysicalSize));
    if (tile->physicalRect.isEmpty()) {
        return {};
    }
    try {
        tile->image = QImage(tile->physicalRect.size(), QImage::Format_ARGB32_Premultiplied);
    } catch (const std::bad_alloc&) {
        return {};
    }
    if (tile->image.isNull()) {
        return {};
    }
    tile->image.setDevicePixelRatio(request.devicePixelRatio);
    tile->image.fill(Qt::transparent);
    const std::size_t index = layerIndex(request.layer);
    state.lru[index].push_front(key);
    tile->lru = state.lru[index].begin();
    state.layerBytes[index] += imageBytes(tile->image);
    state.tiles.emplace(key, tile);
    evictLayerLocked(state, request.layer);
    return tile;
}

bool copyImageRect(QImage& destination, const QRect& destinationRect, const QImage& source,
                   const QRect& sourceRect) {
    if (destination.format() != QImage::Format_ARGB32_Premultiplied ||
        source.format() != QImage::Format_ARGB32_Premultiplied ||
        destinationRect.size() != sourceRect.size() || destinationRect.isEmpty()) {
        return false;
    }
    for (int row = 0; row < sourceRect.height(); ++row) {
        std::memcpy(destination.scanLine(destinationRect.top() + row) +
                        destinationRect.left() * kBytesPerPixel,
                    source.constScanLine(sourceRect.top() + row) +
                        sourceRect.left() * kBytesPerPixel,
                    static_cast<std::size_t>(sourceRect.width()) * kBytesPerPixel);
    }
    return true;
}

bool tileHasFullValidity(const Tile& tile) {
    return tile.valid == QRegion(QRect(QPoint(), tile.image.size()));
}

bool imageIsFullyOpaque(const QImage& image) {
    if (image.format() != QImage::Format_ARGB32_Premultiplied || image.isNull()) {
        return false;
    }
    for (int y = 0; y < image.height(); ++y) {
        const QRgb* pixels = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(pixels[x]) != 255) {
                return false;
            }
        }
    }
    return true;
}

void updateTileOpacity(Tile& tile) {
    tile.fullyOpaque = tileHasFullValidity(tile) && imageIsFullyOpaque(tile.image);
}

struct MissingTile {
    std::shared_ptr<Tile> tile;
    QRegion tileWanted;
    QRegion missing;
};

bool renderIntoTile(const RenderRequest& request, const MissingTile& work, qreal dpr,
                    QPainter::RenderHints renderHints, std::size_t* allocationFailures) {
    if (!work.tile || work.missing.isEmpty()) {
        return true;
    }
    try {
        QPainter tilePainter(&work.tile->image);
        tilePainter.setRenderHints(renderHints);
        const QRegion globalMissing =
            work.missing.translated(work.tile->physicalRect.left(), work.tile->physicalRect.top());
        const QRegion logicalMissing = physicalToLogical(globalMissing, dpr);
        tilePainter.translate(-work.tile->physicalRect.left() / dpr,
                              -work.tile->physicalRect.top() / dpr);
        tilePainter.setClipRegion(logicalMissing);
        tilePainter.setCompositionMode(QPainter::CompositionMode_Source);
        tilePainter.fillRect(logicalMissing.boundingRect(), Qt::transparent);
        tilePainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        request.renderMissing(tilePainter, logicalMissing);
        tilePainter.end();
        work.tile->valid += work.tileWanted;
        if (request.layer == Layer::SpotlightCoverage) {
            updateTileOpacity(*work.tile);
        }
        return true;
    } catch (const std::bad_alloc&) {
        if (allocationFailures != nullptr) {
            ++*allocationFailures;
        }
        return false;
    }
}

bool renderStrip(const RenderRequest& request, const std::vector<MissingTile>& works,
                 std::size_t first, std::size_t last, qreal dpr, QPainter::RenderHints renderHints,
                 std::size_t* allocationFailures, std::size_t* stripCount,
                 std::size_t* rasterizedPixels) {
    if (first >= last) {
        return true;
    }
    const QRect firstRect = works[first].tile->physicalRect;
    const QRect lastRect = works[last - 1].tile->physicalRect;
    const QRect stripRect(firstRect.left(), firstRect.top(),
                          lastRect.right() - firstRect.left() + 1, firstRect.height());
    if (stripRect.width() <= 0 || stripRect.height() <= 0) {
        return true;
    }
    if (static_cast<std::size_t>(stripRect.width()) * static_cast<std::size_t>(stripRect.height()) *
            kBytesPerPixel >
        kTemporaryStripByteLimit) {
        const std::size_t midpoint = first + (last - first) / 2;
        return renderStrip(request, works, first, midpoint, dpr, renderHints, allocationFailures,
                           stripCount, rasterizedPixels) &&
               renderStrip(request, works, midpoint, last, dpr, renderHints, allocationFailures,
                           stripCount, rasterizedPixels);
    }

    QImage staging;
    try {
        staging = QImage(stripRect.size(), QImage::Format_ARGB32_Premultiplied);
    } catch (const std::bad_alloc&) {
        if (allocationFailures != nullptr) {
            ++*allocationFailures;
        }
        return false;
    }
    if (staging.isNull()) {
        if (allocationFailures != nullptr) {
            ++*allocationFailures;
        }
        return false;
    }
    staging.setDevicePixelRatio(dpr);
    staging.fill(Qt::transparent);

    QRegion globalMissing;
    for (std::size_t index = first; index < last; ++index) {
        globalMissing += works[index].missing.translated(works[index].tile->physicalRect.left(),
                                                         works[index].tile->physicalRect.top());
    }
    const QRegion logicalMissing = physicalToLogical(globalMissing, dpr);
    try {
        QPainter stripPainter(&staging);
        stripPainter.setRenderHints(renderHints);
        stripPainter.translate(-stripRect.left() / dpr, -stripRect.top() / dpr);
        request.renderMissing(stripPainter, physicalToLogical(QRegion(stripRect), dpr));
        stripPainter.end();
    } catch (const std::bad_alloc&) {
        if (allocationFailures != nullptr) {
            ++*allocationFailures;
        }
        return false;
    }

    for (std::size_t index = first; index < last; ++index) {
        const MissingTile& work = works[index];
        for (const QRect& rect : work.missing) {
            const QRect globalRect = rect.translated(work.tile->physicalRect.topLeft());
            const QRect sourceRect = globalRect.translated(-stripRect.topLeft());
            const QRect destinationRect = globalRect.translated(-work.tile->physicalRect.topLeft());
            if (!copyImageRect(work.tile->image, destinationRect, staging, sourceRect)) {
                if (allocationFailures != nullptr) {
                    ++*allocationFailures;
                }
                return false;
            }
        }
        work.tile->valid += work.tileWanted;
        if (request.layer == Layer::SpotlightCoverage) {
            updateTileOpacity(*work.tile);
        }
    }
    if (stripCount != nullptr) {
        ++*stripCount;
    }
    if (rasterizedPixels != nullptr) {
        *rasterizedPixels += static_cast<std::size_t>(stripRect.width()) *
                             static_cast<std::size_t>(stripRect.height());
    }
    return true;
}

bool colorizeTile(const std::shared_ptr<Tile>& tile, qreal dpr, const QColor& presentationColor,
                  QImage* out) {
    if (!tile || out == nullptr) {
        return false;
    }
    try {
        QImage colored(tile->image.size(), QImage::Format_ARGB32_Premultiplied);
        if (colored.isNull()) {
            return false;
        }
        colored.setDevicePixelRatio(dpr);
        colored.fill(presentationColor);
        QPainter colorPainter(&colored);
        colorPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        colorPainter.drawImage(QPointF(), tile->image);
        colorPainter.end();
        *out = std::move(colored);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool drawBatchedPresentation(
    QPainter& destination,
    const std::vector<std::pair<std::shared_ptr<Tile>, QRegion>>& presentedTiles, qreal dpr,
    qreal opacity, const QColor& presentationColor, bool* used, bool* allocationFailure) {
    if (used != nullptr) {
        *used = false;
    }
    if (allocationFailure != nullptr) {
        *allocationFailure = false;
    }
    if (presentedTiles.empty() || !presentationColor.isValid()) {
        return true;
    }

    for (const auto& presented : presentedTiles) {
        if (!presented.first ||
            presented.second != QRegion(QRect(QPoint(), presented.first->image.size()))) {
            return true;
        }
    }

    struct PresentationGroup {
        std::size_t first = 0;
        std::size_t last = 0;
        bool opaque = false;
    };
    std::vector<PresentationGroup> groups;
    try {
        std::size_t groupStart = 0;
        while (groupStart < presentedTiles.size()) {
            std::size_t groupEnd = groupStart + 1;
            const QRect firstRect = presentedTiles[groupStart].first->physicalRect;
            const bool opaque = presentedTiles[groupStart].first->fullyOpaque;
            int right = firstRect.right();
            while (groupEnd < presentedTiles.size()) {
                const QRect candidate = presentedTiles[groupEnd].first->physicalRect;
                const std::size_t width = static_cast<std::size_t>(candidate.right()) -
                                          static_cast<std::size_t>(firstRect.left()) + 1U;
                const std::size_t bytes =
                    width * static_cast<std::size_t>(firstRect.height()) * kBytesPerPixel;
                if (candidate.top() != firstRect.top() ||
                    candidate.height() != firstRect.height() || candidate.left() != right + 1 ||
                    presentedTiles[groupEnd].first->fullyOpaque != opaque ||
                    bytes > kTemporaryStripByteLimit) {
                    break;
                }
                right = candidate.right();
                ++groupEnd;
            }
            groups.push_back(PresentationGroup{groupStart, groupEnd, opaque});
            groupStart = groupEnd;
        }
    } catch (const std::bad_alloc&) {
        if (allocationFailure != nullptr) {
            *allocationFailure = true;
        }
        return false;
    }

    int maximumWidth = 0;
    int maximumHeight = 0;
    for (const PresentationGroup& group : groups) {
        if (group.opaque) {
            continue;
        }
        const QRect firstRect = presentedTiles[group.first].first->physicalRect;
        const QRect lastRect = presentedTiles[group.last - 1].first->physicalRect;
        maximumWidth = std::max(maximumWidth, lastRect.right() - firstRect.left() + 1);
        maximumHeight = std::max(maximumHeight, firstRect.height());
    }

    QImage staging;
    if (maximumWidth > 0 && maximumHeight > 0) {
        if (static_cast<std::size_t>(maximumWidth) * static_cast<std::size_t>(maximumHeight) *
                kBytesPerPixel >
            kTemporaryStripByteLimit) {
            return true;
        }
        try {
            staging =
                QImage(QSize(maximumWidth, maximumHeight), QImage::Format_ARGB32_Premultiplied);
        } catch (const std::bad_alloc&) {
            if (allocationFailure != nullptr) {
                *allocationFailure = true;
            }
            return false;
        }
        if (staging.isNull()) {
            if (allocationFailure != nullptr) {
                *allocationFailure = true;
            }
            return false;
        }
        staging.setDevicePixelRatio(dpr);
    }

    try {
        destination.save();
        destination.setOpacity(destination.opacity() * std::clamp(opacity, qreal(0.0), qreal(1.0)));
        for (const PresentationGroup& group : groups) {
            const QRect firstRect = presentedTiles[group.first].first->physicalRect;
            const QRect lastRect = presentedTiles[group.last - 1].first->physicalRect;
            if (group.opaque) {
                destination.fillRect(QRectF(firstRect.left() / dpr, firstRect.top() / dpr,
                                            (lastRect.right() - firstRect.left() + 1) / dpr,
                                            firstRect.height() / dpr),
                                     presentationColor);
                continue;
            }
            const int width = lastRect.right() - firstRect.left() + 1;
            const int height = firstRect.height();
            staging.fill(presentationColor);
            QPainter maskPainter(&staging);
            maskPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            for (std::size_t index = group.first; index < group.last; ++index) {
                const QRect tileRect = presentedTiles[index].first->physicalRect;
                maskPainter.save();
                maskPainter.setClipRect(QRectF((tileRect.left() - firstRect.left()) / dpr,
                                               (tileRect.top() - firstRect.top()) / dpr,
                                               tileRect.width() / dpr, tileRect.height() / dpr));
                maskPainter.drawImage(QPointF((tileRect.left() - firstRect.left()) / dpr,
                                              (tileRect.top() - firstRect.top()) / dpr),
                                      presentedTiles[index].first->image);
                maskPainter.restore();
            }
            maskPainter.end();
            destination.drawImage(QPointF(firstRect.left() / dpr, firstRect.top() / dpr), staging,
                                  QRect(0, 0, width, height));
        }
        destination.restore();
    } catch (const std::bad_alloc&) {
        destination.restore();
        if (allocationFailure != nullptr) {
            *allocationFailure = true;
        }
        return false;
    }
    if (used != nullptr) {
        *used = true;
    }
    return true;
}

void drawTile(QPainter& destination, const std::shared_ptr<Tile>& tile, qreal dpr, qreal opacity,
              const QImage* preparedImage) {
    if (!tile) {
        return;
    }
    destination.save();
    destination.setOpacity(destination.opacity() * std::clamp(opacity, qreal(0.0), qreal(1.0)));
    if (preparedImage != nullptr) {
        destination.drawImage(
            QPointF(tile->physicalRect.left() / dpr, tile->physicalRect.top() / dpr),
            *preparedImage);
    } else {
        destination.drawImage(
            QPointF(tile->physicalRect.left() / dpr, tile->physicalRect.top() / dpr), tile->image);
    }
    destination.restore();
}

std::size_t maskTileBytes(const MaskTile& tile) {
    return sizeof(MaskTile) + imageBytes(tile.image) +
           tile.spans.capacity() * sizeof(snow_canvas_filter_render::MaskSpan) +
           tile.occupiedBlocks.capacity() * sizeof(QRect);
}

void consumePendingLocked(Cache& state, Layer layer, Diagnostics* diagnostics) {
    if (diagnostics == nullptr) {
        return;
    }
    const std::size_t index = layerIndex(layer);
    diagnostics->evictions += state.pendingEvictions[index];
    state.pendingEvictions[index] = 0;
}

} // namespace

Diagnostics render(QPainter& destination, const RenderRequest& request) {
    Diagnostics diagnostics;
    if (request.canvasNamespace == nullptr || request.logicalSize.isEmpty() ||
        request.exposedRegion.isEmpty() || !request.renderMissing) {
        return diagnostics;
    }
    const qreal dpr = std::max<qreal>(0.01, request.devicePixelRatio);
    const QPainter::RenderHints renderHints = destination.renderHints();
    const QSize physicalSize = physicalSizeFor(request.logicalSize, dpr);
    if (physicalSize.isEmpty()) {
        return diagnostics;
    }
    const QRegion physicalViewport(QRect(QPoint(), physicalSize));
    const QRegion wanted =
        logicalToPhysical(request.exposedRegion, dpr).intersected(physicalViewport);
    if (wanted.isEmpty()) {
        return diagnostics;
    }

    struct TileCoordinate {
        int x = 0;
        int y = 0;
    };
    std::vector<TileCoordinate> candidates;
    if (wanted.rectCount() == 1) {
        const QRect rect = wanted.boundingRect();
        const int firstTileX = rect.left() / kTilePhysicalSize;
        const int firstTileY = rect.top() / kTilePhysicalSize;
        const int lastTileX = rect.right() / kTilePhysicalSize;
        const int lastTileY = rect.bottom() / kTilePhysicalSize;
        candidates.reserve(static_cast<std::size_t>(lastTileX - firstTileX + 1) *
                           static_cast<std::size_t>(lastTileY - firstTileY + 1));
        for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
            for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
                candidates.push_back(TileCoordinate{tileX, tileY});
            }
        }
    } else {
        std::unordered_set<std::uint64_t> seen;
        for (const QRect& rect : wanted) {
            const int firstTileX = rect.left() / kTilePhysicalSize;
            const int firstTileY = rect.top() / kTilePhysicalSize;
            const int lastTileX = rect.right() / kTilePhysicalSize;
            const int lastTileY = rect.bottom() / kTilePhysicalSize;
            for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
                for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
                    const std::uint64_t coordinate =
                        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tileX)) << 32) |
                        static_cast<std::uint32_t>(tileY);
                    if (seen.insert(coordinate).second) {
                        candidates.push_back(TileCoordinate{tileX, tileY});
                    }
                }
            }
        }
    }
    diagnostics.candidateTileCount = candidates.size();
    const bool fullViewport = wanted == physicalViewport;

    std::vector<std::vector<MissingTile>> rowMissing;
    if (request.mode == RenderMode::HorizontalStrips && !candidates.empty()) {
        int firstTileY = candidates.front().y;
        int lastTileY = candidates.front().y;
        for (const TileCoordinate& candidate : candidates) {
            firstTileY = std::min(firstTileY, candidate.y);
            lastTileY = std::max(lastTileY, candidate.y);
        }
        rowMissing.resize(static_cast<std::size_t>(lastTileY) -
                          static_cast<std::size_t>(firstTileY) + 1U);
    }
    std::vector<std::pair<std::shared_ptr<Tile>, QRegion>> presentedTiles;
    if (!request.skipPresentation) {
        presentedTiles.reserve(candidates.size());
    }
    std::vector<MissingTile> perTileMissing;
    if (request.mode == RenderMode::PerTile) {
        perTileMissing.reserve(candidates.size());
    }

    bool allocationFailure = false;
    Cache& state = cache();
    int firstRow = 0;
    if (request.mode == RenderMode::HorizontalStrips && !candidates.empty()) {
        firstRow = candidates.front().y;
        for (const TileCoordinate& candidate : candidates) {
            firstRow = std::min(firstRow, candidate.y);
        }
    }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        prepareSignatureLocked(state, request);
        consumePendingLocked(state, request.layer, &diagnostics);
        for (const TileCoordinate& candidate : candidates) {
            ++diagnostics.visitedTileCount;
            std::shared_ptr<Tile> tile =
                acquireTile(state, request, candidate.x, candidate.y, physicalSize);
            if (!tile) {
                ++diagnostics.allocationFailures;
                allocationFailure = true;
                break;
            }
            if (fullViewport && request.skipPresentation && tileHasFullValidity(*tile)) {
                ++diagnostics.hits;
                continue;
            }
            const QRegion tileWanted = fullViewport ? QRegion(QRect(QPoint(), tile->image.size()))
                                                    : wanted.intersected(tile->physicalRect)
                                                          .translated(-tile->physicalRect.left(),
                                                                      -tile->physicalRect.top());
            if (tileWanted.isEmpty()) {
                continue;
            }
            const QRegion missing = fullViewport && tileHasFullValidity(*tile)
                                        ? QRegion()
                                        : tileWanted.subtracted(tile->valid);
            if (missing.isEmpty()) {
                ++diagnostics.hits;
                presentedTiles.emplace_back(tile, tileWanted);
                continue;
            }
            ++diagnostics.misses;
            MissingTile work{tile, tileWanted, missing};
            if (request.mode == RenderMode::HorizontalStrips) {
                rowMissing[static_cast<std::size_t>(candidate.y - firstRow)].push_back(
                    std::move(work));
            } else {
                perTileMissing.push_back(std::move(work));
            }
        }
    }

    if (!allocationFailure && request.mode == RenderMode::PerTile) {
        for (const MissingTile& work : perTileMissing) {
            if (!renderIntoTile(request, work, dpr, renderHints, &diagnostics.allocationFailures)) {
                allocationFailure = true;
                break;
            }
            presentedTiles.emplace_back(work.tile, work.tileWanted);
        }
    }

    if (!allocationFailure && request.mode == RenderMode::HorizontalStrips) {
        for (const std::vector<MissingTile>& works : rowMissing) {
            std::size_t first = 0;
            while (first < works.size()) {
                std::size_t last = first + 1;
                while (last < works.size()) {
                    const QRect& firstRect = works[first].tile->physicalRect;
                    const QRect& candidateRect = works[last].tile->physicalRect;
                    const std::size_t width = static_cast<std::size_t>(candidateRect.right()) -
                                              static_cast<std::size_t>(firstRect.left()) + 1U;
                    const std::size_t bytes =
                        width * static_cast<std::size_t>(firstRect.height()) * kBytesPerPixel;
                    if (bytes > kTemporaryStripByteLimit) {
                        break;
                    }
                    ++last;
                }
                if (!renderStrip(request, works, first, last, dpr, renderHints,
                                 &diagnostics.allocationFailures, &diagnostics.coverageStrips,
                                 &diagnostics.rasterizedPhysicalPixels)) {
                    allocationFailure = true;
                    break;
                }
                for (std::size_t index = first; index < last; ++index) {
                    presentedTiles.emplace_back(works[index].tile, works[index].tileWanted);
                }
                first = last;
            }
            if (allocationFailure) {
                break;
            }
        }
    }

    if (!allocationFailure && !request.skipPresentation) {
        bool batched = false;
        bool presentationAllocationFailure = false;
        if (request.presentationColor.isValid()) {
            destination.save();
            destination.setClipRegion(request.exposedRegion, Qt::IntersectClip);
            drawBatchedPresentation(destination, presentedTiles, dpr, request.destinationOpacity,
                                    request.presentationColor, &batched,
                                    &presentationAllocationFailure);
            destination.restore();
        }
        if (presentationAllocationFailure) {
            ++diagnostics.allocationFailures;
        } else if (!batched) {
            destination.save();
            destination.setClipRegion(request.exposedRegion, Qt::IntersectClip);
            for (const auto& presented : presentedTiles) {
                QImage prepared;
                if (request.presentationColor.isValid() &&
                    !colorizeTile(presented.first, dpr, request.presentationColor, &prepared)) {
                    ++diagnostics.allocationFailures;
                    break;
                }
                drawTile(destination, presented.first, dpr, request.destinationOpacity,
                         request.presentationColor.isValid() ? &prepared : nullptr);
            }
            destination.restore();
        }
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        consumePendingLocked(state, request.layer, &diagnostics);
        diagnostics.retainedLayerBytes = state.layerBytes[layerIndex(request.layer)];
        diagnostics.retainedAggregateBytes = state.layerBytes[layerIndex(Layer::Scene)] +
                                             state.layerBytes[layerIndex(Layer::SpotlightCoverage)];
        diagnostics.retainedBytes = diagnostics.retainedAggregateBytes;
    }
    return diagnostics;
}

std::shared_ptr<const MaskTile> findMask(const void* canvasNamespace, std::uint64_t key) {
    if (canvasNamespace == nullptr) {
        return {};
    }
    Cache& state = cache();
    const MaskKey maskKey{canvasNamespace, key};
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.masks.find(maskKey);
    if (found == state.masks.end()) {
        return {};
    }
    state.maskLru.erase(found->second.lru);
    state.maskLru.push_front(maskKey);
    found->second.lru = state.maskLru.begin();
    return found->second.tile;
}

void storeMask(const void* canvasNamespace, std::uint64_t key, MaskTile tile) {
    if (canvasNamespace == nullptr || tile.image.isNull()) {
        return;
    }
    Cache& state = cache();
    const MaskKey maskKey{canvasNamespace, key};
    auto retained = std::make_shared<MaskTile>(std::move(tile));
    const std::size_t bytes = maskTileBytes(*retained);
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto existing = state.masks.find(maskKey);
    if (existing != state.masks.end()) {
        state.layerBytes[layerIndex(Layer::Scene)] -= existing->second.bytes;
        state.maskBytes -= existing->second.bytes;
        state.maskLru.erase(existing->second.lru);
        state.masks.erase(existing);
    }
    state.maskLru.push_front(maskKey);
    state.masks.emplace(maskKey, RetainedMask{std::move(retained), bytes, state.maskLru.begin()});
    state.layerBytes[layerIndex(Layer::Scene)] += bytes;
    state.maskBytes += bytes;
    evictLayerLocked(state, Layer::Scene);
}

std::size_t consumeMaskEvictions() {
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    const std::size_t evictions = state.pendingMaskEvictions;
    state.pendingMaskEvictions = 0;
    return evictions;
}

std::size_t maskRetainedBytes() {
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.maskBytes;
}

void invalidate(const void* canvasNamespace, const QRegion& logicalRegion, qreal dpr) {
    invalidate(canvasNamespace, logicalRegion, dpr, Layer::Scene);
}

void invalidate(const void* canvasNamespace, const QRegion& logicalRegion, qreal dpr, Layer layer) {
    if (canvasNamespace == nullptr) {
        return;
    }
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.namespaces.find(canvasNamespace);
    if (found == state.namespaces.end()) {
        return;
    }
    const LayerSignature& signature = found->second.layers[layerIndex(layer)];
    if (!signature.initialized) {
        return;
    }
    invalidateTilesLocked(state, canvasNamespace, layer, logicalRegion, dpr, signature.logicalSize);
}

void invalidateForRevision(const void* canvasNamespace, const QRegion& logicalRegion, qreal dpr,
                           std::uint64_t sceneRevision) {
    invalidateForRevision(canvasNamespace, logicalRegion, dpr, sceneRevision, Layer::Scene);
}

void invalidateForRevision(const void* canvasNamespace, const QRegion& logicalRegion, qreal dpr,
                           std::uint64_t layerRevision, Layer layer) {
    if (canvasNamespace == nullptr) {
        return;
    }
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.namespaces.find(canvasNamespace);
    if (found == state.namespaces.end()) {
        return;
    }
    LayerSignature& signature = found->second.layers[layerIndex(layer)];
    if (!signature.initialized) {
        return;
    }
    rekeyRevisionLocked(state, canvasNamespace, layer, signature, layerRevision, logicalRegion,
                        dpr);
}

void invalidateNamespace(const void* canvasNamespace) {
    if (canvasNamespace == nullptr) {
        return;
    }
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    eraseNamespaceLocked(state, canvasNamespace);
}

void clear() {
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    decltype(state.tiles){}.swap(state.tiles);
    decltype(state.masks){}.swap(state.masks);
    decltype(state.namespaces){}.swap(state.namespaces);
    std::list<TileKey>{}.swap(state.lru[0]);
    std::list<TileKey>{}.swap(state.lru[1]);
    std::list<MaskKey>{}.swap(state.maskLru);
    state.layerBytes = {};
    state.maskBytes = 0;
    state.pendingEvictions = {};
    state.pendingMaskEvictions = 0;
}

std::size_t retainedBytes() {
    return retainedAggregateBytes();
}

std::size_t retainedBytes(Layer layer) {
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.layerBytes[layerIndex(layer)];
}

std::size_t retainedAggregateBytes() {
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.layerBytes[0] + state.layerBytes[1];
}

} // namespace snow_canvas_tile_cache

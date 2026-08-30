#include "snow_canvas_pen_mask_atlas.h"
#include "snow_canvas_pen_mask_avx2.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace snow_canvas_pen_mask {
namespace {

void hashValue(std::uint64_t& hash, std::uint64_t value) {
    hash ^= value;
    hash *= 0x100000001b3ULL;
}

void hashDouble(std::uint64_t& hash, double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    hashValue(hash, bits);
}

int floorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    const int remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

double pointSegmentDistanceSquared(double px, double py, double ax, double ay, double bx,
                                   double by) {
    const double dx = bx - ax;
    const double dy = by - ay;
    const double lengthSquared = dx * dx + dy * dy;
    if (!(lengthSquared > 0.0)) {
        const double x = px - ax;
        const double y = py - ay;
        return x * x + y * y;
    }
    const double projection = qBound(0.0, ((px - ax) * dx + (py - ay) * dy) / lengthSquared, 1.0);
    const double x = px - (ax + projection * dx);
    const double y = py - (ay + projection * dy);
    return x * x + y * y;
}

QRectF physicalTileCanvasBounds(int tileX, int tileY, const SceneDisplayInfo& displayInfo,
                                qreal dpr) {
    const double zoom = displayInfo.camera_zoom;
    if (!(zoom > 0.0) || !(dpr > 0.0)) {
        return {};
    }
    const double scale = zoom * dpr;
    const double offsetX =
        displayInfo.surface_width * 0.5 * dpr - displayInfo.camera_center_x * scale;
    const double offsetY =
        displayInfo.surface_height * 0.5 * dpr - displayInfo.camera_center_y * scale;
    const double left = (tileX * kTileSize - offsetX) / scale;
    const double top = (tileY * kTileSize - offsetY) / scale;
    const double right = ((tileX + 1) * kTileSize - offsetX) / scale;
    const double bottom = ((tileY + 1) * kTileSize - offsetY) / scale;
    const double aaOutset = 0.5 / scale;
    return QRectF(QPointF(left - aaOutset, top - aaOutset),
                  QPointF(right + aaOutset, bottom + aaOutset));
}

} // namespace

bool PenMaskAtlas::Key::operator==(const Key& other) const {
    return elementIndex == other.elementIndex && elementGeneration == other.elementGeneration &&
           styleRevision == other.styleRevision && tileX == other.tileX && tileY == other.tileY;
}

std::size_t PenMaskAtlas::KeyHash::operator()(const Key& key) const {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    hashValue(hash, key.elementIndex);
    hashValue(hash, key.elementGeneration);
    hashValue(hash, key.styleRevision);
    hashValue(hash, static_cast<std::uint32_t>(key.tileX));
    hashValue(hash, static_cast<std::uint32_t>(key.tileY));
    return static_cast<std::size_t>(hash);
}

PenMaskAtlas::PenMaskAtlas(std::size_t byteBudget) : m_byteBudget(byteBudget) {}

PenMaskAtlas::~PenMaskAtlas() = default;

std::uint64_t PenMaskAtlas::styleRevision(const SnowCanvasSceneItem& item) const {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    hashDouble(hash, item.stroke_width);
    hashDouble(hash, item.opacity);
    hashDouble(hash, item.rotation);
    return hash;
}

std::uint64_t PenMaskAtlas::viewSignature(const void* canvasOwner,
                                          const SceneDisplayInfo& displayInfo,
                                          qreal devicePixelRatio) const {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    hashValue(hash, reinterpret_cast<std::uintptr_t>(canvasOwner));
    hashDouble(hash, displayInfo.surface_width);
    hashDouble(hash, displayInfo.surface_height);
    hashDouble(hash, displayInfo.camera_center_x);
    hashDouble(hash, displayInfo.camera_center_y);
    hashDouble(hash, displayInfo.camera_zoom);
    hashDouble(hash, devicePixelRatio);
    return hash;
}

void PenMaskAtlas::beginFrame(const void* canvasOwner, const SceneDisplayInfo& displayInfo,
                              qreal devicePixelRatio, const SnowCanvasDisplayCache* displayCache) {
    const std::uint64_t signature = viewSignature(canvasOwner, displayInfo, devicePixelRatio);
    if (!m_hasViewSignature || signature != m_viewSignature) {
        clear();
        m_viewSignature = signature;
        m_hasViewSignature = true;
        m_appliedSceneRevision = 0;
    }
    m_displayInfo = displayInfo;
    m_devicePixelRatio = devicePixelRatio;
    if (displayCache == nullptr) {
        return;
    }
    const std::uint64_t revision = displayCache->patchCursor().scene_revision;
    if (revision == m_appliedSceneRevision) {
        return;
    }
    if (m_appliedSceneRevision != 0 && revision != m_appliedSceneRevision + 1) {
        // Intermediate deltas are unavailable after multiple cache syncs.
        clear();
    }
    const std::size_t before = m_entries.size();
    for (const AppliedPenFilterGeometryDelta& delta :
         displayCache->appliedPenFilterGeometryDeltas()) {
        if (delta.elementRemoved) {
            removeElement(delta.elementId);
        } else {
            invalidate(delta);
        }
    }
    const std::size_t invalidated = before - std::min(before, m_entries.size());
    m_diagnostics.reusedAfterPatch += m_entries.size();
    (void)invalidated;
    m_appliedSceneRevision = revision;
}

void PenMaskAtlas::touch(Entry& entry) {
    m_lru.splice(m_lru.begin(), m_lru, entry.lru);
    entry.lru = m_lru.begin();
}

std::shared_ptr<const Tile>
PenMaskAtlas::tile(const SnowCanvasSceneItem& item, int physicalTileX, int physicalTileY,
                   const SceneDisplayInfo& displayInfo, qreal devicePixelRatio,
                   const snow_canvas_filter_render::ExecutionOptions& execution) {
    const Key key{
        item.element_id.index, item.element_id.generation, styleRevision(item), physicalTileX,
        physicalTileY,
    };
    auto found = m_entries.find(key);
    if (found != m_entries.end()) {
        touch(found->second);
        ++m_diagnostics.hits;
        return found->second.tile;
    }
    ++m_diagnostics.misses;
    const bool useAvx2 =
        !execution.forceScalar && snow_canvas_filter_render::selectedSimdBackend() ==
                                      snow_canvas_filter_render::SimdBackend::Avx2;
    std::shared_ptr<Tile> generated;
    try {
        generated = rasterizeTile(item, physicalTileX, physicalTileY, displayInfo, devicePixelRatio,
                                  useAvx2);
    } catch (const std::bad_alloc&) {
        return {};
    }
    if (!generated) {
        return {};
    }
    if (m_byteBudget == 0 || !generated->occupied) {
        return generated;
    }
    const std::size_t bytes = static_cast<std::size_t>(generated->alpha.sizeInBytes()) +
                              generated->spans.capacity() * sizeof(RowSpan) + sizeof(Entry);
    try {
        m_lru.push_front(key);
        auto [inserted, ok] = m_entries.emplace(key, Entry{generated, bytes, m_lru.begin()});
        if (!ok) {
            m_lru.pop_front();
            return inserted->second.tile;
        }
        m_retainedBytes += bytes;
        evictToBudget();
    } catch (const std::bad_alloc&) {
        if (!m_lru.empty() && m_lru.front() == key) {
            m_lru.pop_front();
        }
        return generated;
    }
    return generated;
}

std::shared_ptr<Tile> PenMaskAtlas::rasterizeTile(const SnowCanvasSceneItem& item,
                                                  int physicalTileX, int physicalTileY,
                                                  const SceneDisplayInfo& displayInfo,
                                                  qreal devicePixelRatio, bool useAvx2) {
    if (!(displayInfo.camera_zoom > 0.0) || !(devicePixelRatio > 0.0)) {
        return {};
    }
    auto result = std::make_shared<Tile>();
    result->alpha = QImage(kTileSize, kTileSize, QImage::Format_Alpha8);
    if (result->alpha.isNull()) {
        return {};
    }
    result->alpha.fill(0);
    result->physicalOrigin = QPoint(physicalTileX * kTileSize, physicalTileY * kTileSize);

    const QRectF canvasBounds =
        physicalTileCanvasBounds(physicalTileX, physicalTileY, displayInfo, devicePixelRatio);
    std::vector<std::uint32_t> chunks;
    std::size_t candidates = 0;
    item.queryPenSegmentChunks(canvasBounds, &chunks, &candidates);
    m_diagnostics.queriedChunks += candidates;
    m_diagnostics.culledChunks += candidates >= chunks.size() ? candidates - chunks.size() : 0;
    const auto& allChunks = item.penSegmentChunks();
    const auto& points = item.penFilterPoints();
    const double scale = displayInfo.camera_zoom * devicePixelRatio;
    const double viewOffsetX =
        displayInfo.surface_width * 0.5 * devicePixelRatio - displayInfo.camera_center_x * scale;
    const double viewOffsetY =
        displayInfo.surface_height * 0.5 * devicePixelRatio - displayInfo.camera_center_y * scale;
    const double radius = item.stroke_width * 0.5 * scale;
    const double transitionOuter = radius + 0.5;
    const int tileLeft = result->physicalOrigin.x();
    const int tileTop = result->physicalOrigin.y();
    bool executedAvx2 = false;

    for (std::uint32_t chunkIndex : chunks) {
        if (chunkIndex >= allChunks.size()) {
            continue;
        }
        const auto& chunk = allChunks[chunkIndex];
        for (std::uint32_t offset = 0; offset + 1 < chunk.pointCount; ++offset) {
            const SnowArrowPoint& first = points[chunk.firstPoint + offset];
            const SnowArrowPoint& second = points[chunk.firstPoint + offset + 1];
            const double ax = first.x * scale + viewOffsetX;
            const double ay = first.y * scale + viewOffsetY;
            const double bx = second.x * scale + viewOffsetX;
            const double by = second.y * scale + viewOffsetY;
            const int beginX = qBound(
                0, static_cast<int>(std::floor(std::min(ax, bx) - transitionOuter)) - tileLeft,
                kTileSize);
            const int endX = qBound(
                0, static_cast<int>(std::ceil(std::max(ax, bx) + transitionOuter)) - tileLeft,
                kTileSize);
            const int beginY = qBound(
                0, static_cast<int>(std::floor(std::min(ay, by) - transitionOuter)) - tileTop,
                kTileSize);
            const int endY =
                qBound(0, static_cast<int>(std::ceil(std::max(ay, by) + transitionOuter)) - tileTop,
                       kTileSize);
            if (useAvx2 &&
                snow_canvas_pen_mask_avx2::rasterizeCapsuleSegment(
                    result->alpha.bits(), result->alpha.bytesPerLine(), tileLeft, tileTop, beginX,
                    endX, beginY, endY, ax, ay, bx, by, transitionOuter)) {
                executedAvx2 = true;
                continue;
            }
            for (int y = beginY; y < endY; ++y) {
                auto* alpha = result->alpha.scanLine(y);
                const double py = tileTop + y + 0.5;
                for (int x = beginX; x < endX; ++x) {
                    const double px = tileLeft + x + 0.5;
                    const double distance =
                        std::sqrt(pointSegmentDistanceSquared(px, py, ax, ay, bx, by));
                    const double coverage = qBound(0.0, transitionOuter - distance, 1.0);
                    const int quantized = qRound(coverage * 255.0);
                    alpha[x] = static_cast<std::uint8_t>(std::max<int>(alpha[x], quantized));
                }
            }
        }
    }

    const double opacity = qBound(0.0, item.opacity, 1.0);
    for (int y = 0; y < kTileSize; ++y) {
        auto* alpha = result->alpha.scanLine(y);
        int x = 0;
        while (x < kTileSize) {
            alpha[x] = static_cast<std::uint8_t>(qRound(alpha[x] * opacity));
            if (alpha[x] == 0) {
                ++x;
                continue;
            }
            const int begin = x++;
            while (x < kTileSize) {
                alpha[x] = static_cast<std::uint8_t>(qRound(alpha[x] * opacity));
                if (alpha[x] == 0) {
                    break;
                }
                ++x;
            }
            result->spans.push_back(RowSpan{
                static_cast<std::uint8_t>(y),
                static_cast<std::uint8_t>(begin),
                static_cast<std::uint8_t>(x),
            });
            result->coveredPixelCount += static_cast<std::size_t>(x - begin);
        }
    }
    result->occupied = result->coveredPixelCount != 0;
    ++m_diagnostics.rasterizedTiles;
    m_diagnostics.rasterizedPixels += kTileSize * kTileSize;
    if (executedAvx2) {
        ++m_diagnostics.simdRasterExecutions;
    }
    return result;
}

void PenMaskAtlas::invalidate(const AppliedPenFilterGeometryDelta& delta) {
    const QRectF changed = delta.oldChangedCanvasBounds.united(delta.newChangedCanvasBounds);
    if (changed.isEmpty()) {
        return;
    }
    for (auto iterator = m_entries.begin(); iterator != m_entries.end();) {
        const Key& key = iterator->first;
        if (key.elementIndex != delta.elementId.index ||
            key.elementGeneration != delta.elementId.generation ||
            !physicalTileCanvasBounds(key.tileX, key.tileY, m_displayInfo, m_devicePixelRatio)
                 .intersects(changed)) {
            ++iterator;
            continue;
        }
        m_retainedBytes -= iterator->second.bytes;
        m_lru.erase(iterator->second.lru);
        iterator = m_entries.erase(iterator);
    }
}

void PenMaskAtlas::removeElement(const SnowElementId& id) {
    for (auto iterator = m_entries.begin(); iterator != m_entries.end();) {
        if (iterator->first.elementIndex != id.index ||
            iterator->first.elementGeneration != id.generation) {
            ++iterator;
            continue;
        }
        m_retainedBytes -= iterator->second.bytes;
        m_lru.erase(iterator->second.lru);
        iterator = m_entries.erase(iterator);
    }
}

void PenMaskAtlas::evictToBudget() {
    while (m_retainedBytes > m_byteBudget && !m_lru.empty()) {
        const Key key = m_lru.back();
        m_lru.pop_back();
        const auto found = m_entries.find(key);
        if (found == m_entries.end()) {
            continue;
        }
        m_retainedBytes -= found->second.bytes;
        m_entries.erase(found);
        ++m_diagnostics.evictions;
    }
}

void PenMaskAtlas::clear() {
    m_entries.clear();
    m_lru.clear();
    m_retainedBytes = 0;
    m_viewSignature = 0;
    m_appliedSceneRevision = 0;
    m_hasViewSignature = false;
    m_displayInfo = {};
    m_devicePixelRatio = 1.0;
}

std::size_t PenMaskAtlas::retainedBytes() const {
    return m_retainedBytes;
}
std::size_t PenMaskAtlas::entryCount() const {
    return m_entries.size();
}
std::size_t PenMaskAtlas::byteBudget() const {
    return m_byteBudget;
}

Diagnostics PenMaskAtlas::takeDiagnostics() {
    m_diagnostics.retainedBytes = m_retainedBytes;
    Diagnostics result = m_diagnostics;
    m_diagnostics = {};
    return result;
}

} // namespace snow_canvas_pen_mask

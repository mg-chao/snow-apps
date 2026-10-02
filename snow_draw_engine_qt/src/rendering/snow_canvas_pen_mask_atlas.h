#pragma once

#include "snow_canvas_display_cache.h"
#include "snow_canvas_filter_render.h"

#include <QImage>
#include <QPoint>

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

namespace snow_canvas_pen_mask {

constexpr int kTileSize = 64;
constexpr std::size_t kDefaultBudgetBytes = 16u * 1024u * 1024u;

struct RowSpan {
    std::uint8_t y = 0;
    std::uint8_t beginX = 0;
    std::uint8_t endX = 0;
};

struct Tile {
    QImage alpha;
    QPoint physicalOrigin;
    std::vector<RowSpan> spans;
    std::size_t coveredPixelCount = 0;
    bool occupied = false;
};

struct Diagnostics {
    std::size_t queriedChunks = 0;
    std::size_t culledChunks = 0;
    std::size_t rasterizedTiles = 0;
    std::size_t rasterizedPixels = 0;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
    std::size_t reusedAfterPatch = 0;
    std::size_t simdRasterExecutions = 0;
    std::size_t retainedBytes = 0;
};

class PenMaskAtlas {
  public:
    explicit PenMaskAtlas(std::size_t byteBudget = kDefaultBudgetBytes);
    ~PenMaskAtlas();

    PenMaskAtlas(const PenMaskAtlas&) = delete;
    PenMaskAtlas& operator=(const PenMaskAtlas&) = delete;

    void beginFrame(const void* canvasOwner, const SceneDisplayInfo& displayInfo,
                    qreal devicePixelRatio, const SnowCanvasDisplayCache* displayCache = nullptr);
    std::shared_ptr<const Tile>
    tile(const SnowCanvasSceneItem& item, int physicalTileX, int physicalTileY,
         const SceneDisplayInfo& displayInfo, qreal devicePixelRatio,
         const snow_canvas_filter_render::ExecutionOptions& execution = {});
    void invalidate(const AppliedPenFilterGeometryDelta& delta);
    void removeElement(const SnowElementId& id);
    void clear();

    std::size_t retainedBytes() const;
    std::size_t entryCount() const;
    std::size_t byteBudget() const;
    Diagnostics takeDiagnostics();

  private:
    struct Key {
        std::uint32_t elementIndex = 0;
        std::uint32_t elementGeneration = 0;
        std::uint64_t styleRevision = 0;
        int tileX = 0;
        int tileY = 0;

        bool operator==(const Key& other) const;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const;
    };
    struct Entry {
        std::shared_ptr<Tile> tile;
        std::size_t bytes = 0;
        std::list<Key>::iterator lru;
        std::uint64_t restorationGeometryIdentity = 0;
        bool immutableRestoration = false;
    };

    std::uint64_t styleRevision(const SnowCanvasSceneItem& item) const;
    std::uint64_t restorationGeometryIdentity(const SnowCanvasSceneItem& item) const;
    std::uint64_t viewSignature(const void* canvasOwner, const SceneDisplayInfo& displayInfo,
                                qreal devicePixelRatio) const;
    std::shared_ptr<Tile> rasterizeTile(const SnowCanvasSceneItem& item, int physicalTileX,
                                        int physicalTileY, const SceneDisplayInfo& displayInfo,
                                        qreal devicePixelRatio, bool useAvx2);
    void touch(Entry& entry);
    void evictToBudget();

    std::size_t m_byteBudget;
    std::size_t m_retainedBytes = 0;
    std::uint64_t m_viewSignature = 0;
    std::uint64_t m_appliedSceneRevision = 0;
    bool m_hasViewSignature = false;
    SceneDisplayInfo m_displayInfo{};
    qreal m_devicePixelRatio = 1.0;
    std::unordered_map<Key, Entry, KeyHash> m_entries;
    std::list<Key> m_lru;
    Diagnostics m_diagnostics;
};

} // namespace snow_canvas_pen_mask

#include "snow_canvas_display_cache.h"

#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_render_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <numeric>

namespace {

constexpr SnowColorRgba8 kDefaultClearColor{255, 255, 255, 255};
constexpr int kSceneSpatialCellSize = 128;
constexpr std::int64_t kMaxCellsPerItem = 4096;

static_assert(sizeof(SnowPatchCursor) == 24);
static_assert(offsetof(SnowPatchCursor, decoration_revision) == 8);
static_assert(sizeof(SnowPatchInfo) == 584);
static_assert(offsetof(SnowPatchInfo, decoration_dirty_rect_count) == 84);

struct PatchApplyResult {
    bool structural = false;
    std::vector<std::uint32_t> changedIndices;
};

std::int64_t spatialCellKey(int x, int y) {
    const std::uint64_t high = static_cast<std::uint32_t>(x);
    const std::uint64_t low = static_cast<std::uint32_t>(y);
    return static_cast<std::int64_t>((high << 32) | low);
}

template <typename T> const T* storageDataOrNull(const std::vector<T>& storage) {
    return storage.empty() ? nullptr : storage.data();
}

template <typename T>
void assignStorage(std::vector<T>& storage, const T* items, std::uint32_t itemCount) {
    if (items == nullptr || itemCount == 0) {
        storage.clear();
        return;
    }
    storage.assign(items, items + itemCount);
}

template <typename Owned, typename Borrowed>
PatchApplyResult applyPatchOps(std::vector<Owned>& storage, bool reset, const SnowPatchOp* ops,
                               std::uint32_t opCount, const Borrowed* insertItems,
                               std::uint32_t insertItemCount) {
    PatchApplyResult result;
    if (reset) {
        std::vector<Owned>().swap(storage);
        result.structural = true;
    }
    if (ops == nullptr || opCount == 0) {
        return result;
    }

    using Difference = typename std::vector<Owned>::difference_type;

    std::size_t requiredCapacity = storage.size();
    for (std::uint32_t index = 0; index < opCount; ++index) {
        requiredCapacity = requiredCapacity -
                           std::min<std::size_t>(requiredCapacity, ops[index].delete_count) +
                           ops[index].insert_count;
    }
    storage.reserve(requiredCapacity);

    for (std::uint32_t index = 0; index < opCount; ++index) {
        const SnowPatchOp& op = ops[index];
        const std::size_t start = std::min<std::size_t>(storage.size(), op.start);
        const std::size_t deleteCount =
            std::min<std::size_t>(storage.size() - start, op.delete_count);
        const std::size_t insertOffset = std::min<std::size_t>(insertItemCount, op.insert_offset);
        const std::size_t insertCount =
            std::min<std::size_t>(insertItemCount - insertOffset, op.insert_count);
        const std::size_t commonCount = std::min(deleteCount, insertCount);
        if (deleteCount != insertCount) {
            result.structural = true;
        }
        for (std::size_t itemIndex = 0; itemIndex < commonCount; ++itemIndex) {
            storage[start + itemIndex] = insertItems[insertOffset + itemIndex];
            result.changedIndices.push_back(static_cast<std::uint32_t>(start + itemIndex));
        }

        if (deleteCount > commonCount) {
            storage.erase(storage.begin() + static_cast<Difference>(start + commonCount),
                          storage.begin() + static_cast<Difference>(start + deleteCount));
        } else if (insertCount > commonCount && insertItems != nullptr) {
            std::vector<Owned> additions;
            additions.reserve(insertCount - commonCount);
            for (std::size_t itemIndex = commonCount; itemIndex < insertCount; ++itemIndex) {
                additions.emplace_back(insertItems[insertOffset + itemIndex]);
            }
            storage.insert(storage.begin() + static_cast<Difference>(start + commonCount),
                           std::make_move_iterator(additions.begin()),
                           std::make_move_iterator(additions.end()));
        }
    }
    return result;
}

struct PatchPayloadView {
    const SnowPatchOp* sceneOps = nullptr;
    const SnowPenFilterGeometryPatch* penFilterGeometryOps = nullptr;
    const SnowArrowPoint* penFilterGeometryPoints = nullptr;
    const SnowPathGeometryPatch* pathGeometryOps = nullptr;
    const SnowPathChunkRange* pathGeometryRanges = nullptr;
    const SnowPathChunk* pathGeometryChunks = nullptr;
    const SnowArrowPathCommand* pathGeometryCommands = nullptr;
    const SnowPatchOp* overlayOps = nullptr;
    const SnowPatchOp* spotlightOps = nullptr;
    const SnowSceneDisplayItem* sceneItems = nullptr;
    const SnowOverlayDisplayItem* overlayItems = nullptr;
    const SnowSpotlightCutout* spotlightCutouts = nullptr;
    const SnowDirtyRect* sceneDirtyRects = nullptr;
    const SnowDirtyRect* decorationDirtyRects = nullptr;
    const SnowDirtyRect* overlayDirtyRects = nullptr;
    std::uint32_t sceneOpCount = 0;
    std::uint32_t penFilterGeometryOpCount = 0;
    std::uint32_t penFilterGeometryPointCount = 0;
    std::uint32_t pathGeometryOpCount = 0;
    std::uint32_t pathGeometryRangeCount = 0;
    std::uint32_t pathGeometryChunkCount = 0;
    std::uint32_t pathGeometryCommandCount = 0;
    std::uint32_t overlayOpCount = 0;
    std::uint32_t spotlightOpCount = 0;
    std::uint32_t sceneItemCount = 0;
    std::uint32_t overlayItemCount = 0;
    std::uint32_t spotlightCutoutCount = 0;
    std::uint32_t sceneDirtyRectCount = 0;
    std::uint32_t decorationDirtyRectCount = 0;
    std::uint32_t overlayDirtyRectCount = 0;
};

bool readPatchPayload(SnowPatchHandle patch, PatchPayloadView* outPayload) {
    if (outPayload == nullptr) {
        return false;
    }

    return snow_patch_get_scene_ops(patch, &outPayload->sceneOps, &outPayload->sceneOpCount) ==
               SNOW_OK &&
           snow_patch_get_pen_filter_geometry_ops(patch, &outPayload->penFilterGeometryOps,
                                                  &outPayload->penFilterGeometryOpCount) ==
               SNOW_OK &&
           snow_patch_get_pen_filter_geometry_points(patch, &outPayload->penFilterGeometryPoints,
                                                     &outPayload->penFilterGeometryPointCount) ==
               SNOW_OK &&
           snow_patch_get_path_geometry_ops(patch, &outPayload->pathGeometryOps,
                                            &outPayload->pathGeometryOpCount) == SNOW_OK &&
           snow_patch_get_path_geometry_ranges(patch, &outPayload->pathGeometryRanges,
                                               &outPayload->pathGeometryRangeCount) == SNOW_OK &&
           snow_patch_get_path_geometry_chunks(patch, &outPayload->pathGeometryChunks,
                                               &outPayload->pathGeometryChunkCount) == SNOW_OK &&
           snow_patch_get_path_geometry_commands(patch, &outPayload->pathGeometryCommands,
                                                 &outPayload->pathGeometryCommandCount) ==
               SNOW_OK &&
           snow_patch_get_overlay_ops(patch, &outPayload->overlayOps,
                                      &outPayload->overlayOpCount) == SNOW_OK &&
           snow_patch_get_spotlight_ops(patch, &outPayload->spotlightOps,
                                        &outPayload->spotlightOpCount) == SNOW_OK &&
           snow_patch_get_scene_items(patch, &outPayload->sceneItems,
                                      &outPayload->sceneItemCount) == SNOW_OK &&
           snow_patch_get_overlay_items(patch, &outPayload->overlayItems,
                                        &outPayload->overlayItemCount) == SNOW_OK &&
           snow_patch_get_spotlight_cutouts(patch, &outPayload->spotlightCutouts,
                                            &outPayload->spotlightCutoutCount) == SNOW_OK &&
           snow_patch_get_scene_dirty_rects(patch, &outPayload->sceneDirtyRects,
                                            &outPayload->sceneDirtyRectCount) == SNOW_OK &&
           snow_patch_get_decoration_dirty_rects(patch, &outPayload->decorationDirtyRects,
                                                 &outPayload->decorationDirtyRectCount) ==
               SNOW_OK &&
           snow_patch_get_overlay_dirty_rects(patch, &outPayload->overlayDirtyRects,
                                              &outPayload->overlayDirtyRectCount) == SNOW_OK;
}

} // namespace

SnowCanvasDisplayCache::SnowCanvasDisplayCache() {
    reset(kDefaultClearColor);
}

void SnowCanvasDisplayCache::reset(const SnowColorRgba8& clearColor) {
    m_patchCursor = SnowPatchCursor{};
    std::vector<SnowCanvasSceneItem>().swap(m_sceneStorage);
    std::vector<SnowCanvasOverlayItem>().swap(m_overlayStorage);
    std::vector<SnowSpotlightCutout>().swap(m_spotlightStorage);
    m_lastPenFilterGeometryOpCount = 0;
    m_lastPenFilterGeometryPointCount = 0;
    std::vector<AppliedPenFilterGeometryDelta>().swap(m_appliedPenFilterGeometryDeltas);
    std::vector<SnowDirtyRect>().swap(m_sceneDirtyStorage);
    std::vector<SnowDirtyRect>().swap(m_decorationDirtyStorage);
    std::vector<SnowDirtyRect>().swap(m_overlayDirtyStorage);
    decltype(m_sceneSpatialCells)().swap(m_sceneSpatialCells);
    std::vector<std::vector<std::int64_t>>().swap(m_sceneItemSpatialCells);
    std::vector<std::uint32_t>().swap(m_sceneGlobalItems);
    std::vector<std::uint32_t>().swap(m_filterIndices);
    std::vector<std::uint32_t>().swap(m_sceneQueryMarks);
    m_sceneQueryStamp = 0;
    refreshViews();
    m_sceneDisplayInfo = SceneDisplayInfo{};
    m_sceneDisplayInfo.clear_color = clearColor;
    m_watermarkDisplayInfo = WatermarkDisplayInfo{};
    m_spotlightDisplayInfo = SpotlightDisplayInfo{};
    m_overlayDisplayInfo = OverlayDisplayInfo{};
}

void SnowCanvasDisplayCache::setClearColor(const SnowColorRgba8& clearColor) {
    m_sceneDisplayInfo.clear_color = clearColor;
}

bool SnowCanvasDisplayCache::sync(SnowRuntime runtime, SnowViewport viewport) {
    if (runtime == nullptr || viewport == nullptr) {
        return false;
    }

    ScopedPatchHandle patch;
    SnowPatchInfo patchInfo{};
    if (snow_viewport_acquire_patch(runtime, viewport, &m_patchCursor, patch.outParam()) !=
            SNOW_OK ||
        snow_patch_get_info(patch.get(), &patchInfo) != SNOW_OK) {
        return false;
    }

    PatchPayloadView payload;
    if (!readPatchPayload(patch.get(), &payload)) {
        return false;
    }
    m_lastPenFilterGeometryOpCount = payload.penFilterGeometryOpCount;
    m_lastPenFilterGeometryPointCount = payload.penFilterGeometryPointCount;
    if (patchInfo.scene_reset != 0) {
        std::vector<AppliedPenFilterGeometryDelta>().swap(m_appliedPenFilterGeometryDeltas);
    } else {
        m_appliedPenFilterGeometryDeltas.clear();
    }
    m_appliedPenFilterGeometryDeltas.reserve(payload.penFilterGeometryOpCount);

    const bool viewChanged = m_sceneDisplayInfo.surface_width != patchInfo.surface_width ||
                             m_sceneDisplayInfo.surface_height != patchInfo.surface_height ||
                             m_sceneDisplayInfo.camera_center_x != patchInfo.camera_center_x ||
                             m_sceneDisplayInfo.camera_center_y != patchInfo.camera_center_y ||
                             m_sceneDisplayInfo.camera_zoom != patchInfo.camera_zoom;
    PatchApplyResult sceneApply =
        applyPatchOps(m_sceneStorage, patchInfo.scene_reset != 0, payload.sceneOps,
                      payload.sceneOpCount, payload.sceneItems, payload.sceneItemCount);
    for (std::uint32_t opIndex = 0; opIndex < payload.pathGeometryOpCount; ++opIndex) {
        const SnowPathGeometryPatch& op = payload.pathGeometryOps[opIndex];
        if (op.range_offset > payload.pathGeometryRangeCount ||
            op.range_count > payload.pathGeometryRangeCount - op.range_offset) {
            reset(patchInfo.clear_color);
            return false;
        }
        if (op.element_removed != 0) {
            continue;
        }
        const auto found = std::find_if(
            m_sceneStorage.begin(), m_sceneStorage.end(), [&op](const SnowCanvasSceneItem& item) {
                return item.element_id.index == op.element_id.index &&
                       item.element_id.generation == op.element_id.generation;
            });
        if (found == m_sceneStorage.end() ||
            !found->applyPathGeometryPatch(
                op.expected_geometry_revision, op.resulting_geometry_revision,
                op.range_count == 0 ? nullptr : payload.pathGeometryRanges + op.range_offset,
                op.range_count, payload.pathGeometryChunks, payload.pathGeometryChunkCount,
                payload.pathGeometryCommands, payload.pathGeometryCommandCount, op.closed != 0,
                op.full_reset != 0)) {
            reset(patchInfo.clear_color);
            return false;
        }
        sceneApply.changedIndices.push_back(
            static_cast<std::uint32_t>(std::distance(m_sceneStorage.begin(), found)));
    }
    for (std::uint32_t opIndex = 0; opIndex < payload.penFilterGeometryOpCount; ++opIndex) {
        const SnowPenFilterGeometryPatch& op = payload.penFilterGeometryOps[opIndex];
        if (op.append_offset > payload.penFilterGeometryPointCount ||
            op.append_count > payload.penFilterGeometryPointCount - op.append_offset) {
            reset(patchInfo.clear_color);
            return false;
        }
        const auto boundsFromPatch = [](double minX, double minY, double maxX, double maxY) {
            return maxX > minX && maxY > minY ? QRectF(QPointF(minX, minY), QPointF(maxX, maxY))
                                              : QRectF{};
        };
        m_appliedPenFilterGeometryDeltas.push_back(AppliedPenFilterGeometryDelta{
            op.element_id,
            op.expected_geometry_revision,
            op.resulting_geometry_revision,
            boundsFromPatch(op.old_changed_min_x, op.old_changed_min_y, op.old_changed_max_x,
                            op.old_changed_max_y),
            boundsFromPatch(op.new_changed_min_x, op.new_changed_min_y, op.new_changed_max_x,
                            op.new_changed_max_y),
            op.full_reset != 0,
            op.element_removed != 0,
        });
        if (op.element_removed != 0) {
            continue;
        }
        const auto found = std::find_if(
            m_sceneStorage.begin(), m_sceneStorage.end(), [&op](const SnowCanvasSceneItem& item) {
                return item.element_id.index == op.element_id.index &&
                       item.element_id.generation == op.element_id.generation;
            });
        if (found == m_sceneStorage.end()) {
            reset(patchInfo.clear_color);
            return false;
        }
        const SnowArrowPoint* appendedPoints =
            op.append_count == 0 ? nullptr : payload.penFilterGeometryPoints + op.append_offset;
        if (!found->applyPenFilterGeometryPatch(
                op.expected_geometry_revision, op.resulting_geometry_revision,
                op.retain_prefix_count, appendedPoints, op.append_count, op.full_reset != 0)) {
            reset(patchInfo.clear_color);
            return false;
        }
        sceneApply.changedIndices.push_back(
            static_cast<std::uint32_t>(std::distance(m_sceneStorage.begin(), found)));
    }
    const PatchApplyResult overlayApply =
        applyPatchOps(m_overlayStorage, patchInfo.overlay_reset != 0, payload.overlayOps,
                      payload.overlayOpCount, payload.overlayItems, payload.overlayItemCount);
    applyPatchOps(
        m_spotlightStorage, patchInfo.decoration_reset != 0, payload.spotlightOps,
        payload.spotlightOpCount, payload.spotlightCutouts, payload.spotlightCutoutCount);
    if (patchInfo.scene_reset != 0) {
        decltype(m_sceneSpatialCells)().swap(m_sceneSpatialCells);
        std::vector<std::vector<std::int64_t>>().swap(m_sceneItemSpatialCells);
        std::vector<std::uint32_t>().swap(m_sceneGlobalItems);
        std::vector<std::uint32_t>().swap(m_filterIndices);
        std::vector<std::uint32_t>().swap(m_sceneQueryMarks);
        m_sceneQueryStamp = 0;
        std::vector<SnowDirtyRect>().swap(m_sceneDirtyStorage);
    }
    if (patchInfo.overlay_reset != 0) {
        std::vector<SnowDirtyRect>().swap(m_overlayDirtyStorage);
    }
    if (patchInfo.decoration_reset != 0) {
        std::vector<SnowDirtyRect>().swap(m_decorationDirtyStorage);
    }
    assignStorage(m_sceneDirtyStorage, payload.sceneDirtyRects, payload.sceneDirtyRectCount);
    assignStorage(m_decorationDirtyStorage, payload.decorationDirtyRects,
                  payload.decorationDirtyRectCount);
    assignStorage(m_overlayDirtyStorage, payload.overlayDirtyRects, payload.overlayDirtyRectCount);
    refreshViews();
    applyPatchInfo(patchInfo);
    if (patchInfo.scene_reset != 0 || sceneApply.structural) {
        rebuildFilterIndices();
    } else {
        updateFilterIndices(sceneApply.changedIndices);
    }
    if (viewChanged || sceneApply.structural) {
        rebuildViewBoundsAndSpatialIndex();
    } else {
        for (std::uint32_t index : sceneApply.changedIndices) {
            if (index >= m_sceneStorage.size()) {
                continue;
            }
            m_sceneStorage[index].viewBounds = snow_canvas_render_geometry::sceneItemBounds(
                m_sceneDisplayInfo, m_sceneStorage[index]);
            updateSceneSpatialItem(index);
        }
        if (overlayApply.structural) {
            for (SnowCanvasOverlayItem& item : m_overlayStorage) {
                item.viewBounds =
                    snow_canvas_render_geometry::overlayItemBounds(m_overlayDisplayInfo, item);
            }
        } else {
            for (std::uint32_t index : overlayApply.changedIndices) {
                if (index < m_overlayStorage.size()) {
                    m_overlayStorage[index].viewBounds =
                        snow_canvas_render_geometry::overlayItemBounds(m_overlayDisplayInfo,
                                                                       m_overlayStorage[index]);
                }
            }
        }
    }
    return true;
}

const SceneDisplayInfo& SnowCanvasDisplayCache::sceneInfo() const {
    return m_sceneDisplayInfo;
}

const WatermarkDisplayInfo& SnowCanvasDisplayCache::watermarkInfo() const {
    return m_watermarkDisplayInfo;
}

const SpotlightDisplayInfo& SnowCanvasDisplayCache::spotlightInfo() const {
    return m_spotlightDisplayInfo;
}

const OverlayDisplayInfo& SnowCanvasDisplayCache::overlayInfo() const {
    return m_overlayDisplayInfo;
}

const SnowPatchCursor& SnowCanvasDisplayCache::patchCursor() const {
    return m_patchCursor;
}

const SnowCanvasSceneItem* SnowCanvasDisplayCache::sceneItems() const {
    return m_sceneItems;
}

const SnowCanvasOverlayItem* SnowCanvasDisplayCache::overlayItems() const {
    return m_overlayItems;
}

const SnowSpotlightCutout* SnowCanvasDisplayCache::spotlightCutouts() const {
    return m_spotlightCutouts;
}

std::uint32_t SnowCanvasDisplayCache::sceneItemCount() const {
    return m_sceneItemCount;
}

std::uint32_t SnowCanvasDisplayCache::overlayItemCount() const {
    return m_overlayItemCount;
}

std::uint32_t SnowCanvasDisplayCache::spotlightCutoutCount() const {
    return m_spotlightCutoutCount;
}

std::uint32_t SnowCanvasDisplayCache::lastPenFilterGeometryOpCount() const {
    return m_lastPenFilterGeometryOpCount;
}

std::uint32_t SnowCanvasDisplayCache::lastPenFilterGeometryPointCount() const {
    return m_lastPenFilterGeometryPointCount;
}

const std::vector<AppliedPenFilterGeometryDelta>&
SnowCanvasDisplayCache::appliedPenFilterGeometryDeltas() const {
    return m_appliedPenFilterGeometryDeltas;
}

const std::vector<std::uint32_t>& SnowCanvasDisplayCache::filterIndices() const {
    return m_filterIndices;
}

const SnowDirtyRect* SnowCanvasDisplayCache::sceneDirtyRects() const {
    return m_sceneDirtyRects;
}

const SnowDirtyRect* SnowCanvasDisplayCache::overlayDirtyRects() const {
    return m_overlayDirtyRects;
}

const SnowDirtyRect* SnowCanvasDisplayCache::decorationDirtyRects() const {
    return m_decorationDirtyRects;
}

std::uint32_t SnowCanvasDisplayCache::sceneDirtyRectCount() const {
    return m_sceneDirtyRectCount;
}

std::uint32_t SnowCanvasDisplayCache::overlayDirtyRectCount() const {
    return m_overlayDirtyRectCount;
}

std::uint32_t SnowCanvasDisplayCache::decorationDirtyRectCount() const {
    return m_decorationDirtyRectCount;
}

void SnowCanvasDisplayCache::rebuildViewBoundsAndSpatialIndex() {
    for (SnowCanvasSceneItem& item : m_sceneStorage) {
        item.viewBounds = snow_canvas_render_geometry::sceneItemBounds(m_sceneDisplayInfo, item);
    }
    for (SnowCanvasOverlayItem& item : m_overlayStorage) {
        item.viewBounds =
            snow_canvas_render_geometry::overlayItemBounds(m_overlayDisplayInfo, item);
    }
    rebuildSceneSpatialIndex();
}

void SnowCanvasDisplayCache::rebuildSceneSpatialIndex() {
    m_sceneSpatialCells.clear();
    m_sceneGlobalItems.clear();
    m_sceneItemSpatialCells.clear();
    m_sceneItemSpatialCells.resize(m_sceneStorage.size());
    m_sceneQueryMarks.resize(m_sceneStorage.size(), 0);
    for (std::uint32_t index = 0; index < m_sceneStorage.size(); ++index) {
        updateSceneSpatialItem(index);
    }
}

void SnowCanvasDisplayCache::rebuildFilterIndices() {
    m_filterIndices.clear();
    for (std::uint32_t index = 0; index < m_sceneStorage.size(); ++index) {
        if (m_sceneStorage[index].kind == SNOW_SCENE_DISPLAY_ITEM_FILTER) {
            m_filterIndices.push_back(index);
        }
    }
}

void SnowCanvasDisplayCache::removeSceneSpatialItem(std::uint32_t index) {
    if (index >= m_sceneItemSpatialCells.size()) {
        return;
    }
    for (std::int64_t key : m_sceneItemSpatialCells[index]) {
        auto found = m_sceneSpatialCells.find(key);
        if (found == m_sceneSpatialCells.end()) {
            continue;
        }
        std::vector<std::uint32_t>& indices = found->second;
        indices.erase(std::remove(indices.begin(), indices.end(), index), indices.end());
        if (indices.empty()) {
            m_sceneSpatialCells.erase(found);
        }
    }
    m_sceneItemSpatialCells[index].clear();
    m_sceneGlobalItems.erase(
        std::remove(m_sceneGlobalItems.begin(), m_sceneGlobalItems.end(), index),
        m_sceneGlobalItems.end());
}

void SnowCanvasDisplayCache::updateSceneSpatialItem(std::uint32_t index) {
    if (index >= m_sceneStorage.size()) {
        return;
    }
    if (m_sceneItemSpatialCells.size() < m_sceneStorage.size()) {
        m_sceneItemSpatialCells.resize(m_sceneStorage.size());
    }
    if (m_sceneQueryMarks.size() < m_sceneStorage.size()) {
        m_sceneQueryMarks.resize(m_sceneStorage.size(), 0);
    }
    removeSceneSpatialItem(index);

    const QRectF bounds = m_sceneStorage[index].viewBounds;
    if (!bounds.isValid() || bounds.isEmpty()) {
        return;
    }
    const int minX = static_cast<int>(std::floor(bounds.left() / kSceneSpatialCellSize));
    const int minY = static_cast<int>(std::floor(bounds.top() / kSceneSpatialCellSize));
    const int maxX = static_cast<int>(std::floor(bounds.right() / kSceneSpatialCellSize));
    const int maxY = static_cast<int>(std::floor(bounds.bottom() / kSceneSpatialCellSize));
    const std::int64_t cellCount =
        static_cast<std::int64_t>(maxX - minX + 1) * static_cast<std::int64_t>(maxY - minY + 1);
    if (cellCount <= 0 || cellCount > kMaxCellsPerItem) {
        m_sceneGlobalItems.push_back(index);
        return;
    }
    std::vector<std::int64_t>& itemCells = m_sceneItemSpatialCells[index];
    itemCells.reserve(static_cast<std::size_t>(cellCount));
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const std::int64_t key = spatialCellKey(x, y);
            itemCells.push_back(key);
            m_sceneSpatialCells[key].push_back(index);
        }
    }
}

void SnowCanvasDisplayCache::sceneCandidateIndices(const QRegion& exposedRegion,
                                                   std::vector<std::uint32_t>* outIndices) const {
    if (outIndices == nullptr) {
        return;
    }
    outIndices->clear();
    const std::size_t itemCount = m_sceneStorage.size();
    if (itemCount == 0 || exposedRegion.isEmpty()) {
        return;
    }

    const auto useAllItems = [&]() {
        outIndices->resize(itemCount);
        std::iota(outIndices->begin(), outIndices->end(), 0u);
    };
    m_sceneQueryStamp = m_sceneQueryStamp + 1;
    if (m_sceneQueryStamp == 0) {
        std::fill(m_sceneQueryMarks.begin(), m_sceneQueryMarks.end(), 0);
        m_sceneQueryStamp = 1;
    }
    const auto markCandidate = [&](std::uint32_t index) {
        if (index >= itemCount || m_sceneQueryMarks[index] == m_sceneQueryStamp) {
            return;
        }
        m_sceneQueryMarks[index] = m_sceneQueryStamp;
        if (exposedRegion.intersects(m_sceneStorage[index].viewBounds.toAlignedRect())) {
            outIndices->push_back(index);
        }
    };
    for (std::uint32_t index : m_sceneGlobalItems) {
        markCandidate(index);
    }
    for (const QRect& rect : exposedRegion) {
        const int minX =
            static_cast<int>(std::floor(rect.left() / static_cast<double>(kSceneSpatialCellSize)));
        const int minY =
            static_cast<int>(std::floor(rect.top() / static_cast<double>(kSceneSpatialCellSize)));
        const int maxX =
            static_cast<int>(std::floor(rect.right() / static_cast<double>(kSceneSpatialCellSize)));
        const int maxY = static_cast<int>(
            std::floor(rect.bottom() / static_cast<double>(kSceneSpatialCellSize)));
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const auto found = m_sceneSpatialCells.find(spatialCellKey(x, y));
                if (found == m_sceneSpatialCells.end()) {
                    continue;
                }
                for (std::uint32_t index : found->second) {
                    markCandidate(index);
                }
            }
        }
    }
    const std::size_t denseCandidateThreshold =
        (itemCount / 5U) * 3U + ((itemCount % 5U) * 3U + 4U) / 5U;
    if (outIndices->size() >= denseCandidateThreshold) {
        useAllItems();
        return;
    }
    std::sort(outIndices->begin(), outIndices->end());
}

void SnowCanvasDisplayCache::refreshViews() {
    m_sceneItems = storageDataOrNull(m_sceneStorage);
    m_overlayItems = storageDataOrNull(m_overlayStorage);
    m_spotlightCutouts = storageDataOrNull(m_spotlightStorage);
    m_sceneDirtyRects = storageDataOrNull(m_sceneDirtyStorage);
    m_decorationDirtyRects = storageDataOrNull(m_decorationDirtyStorage);
    m_overlayDirtyRects = storageDataOrNull(m_overlayDirtyStorage);
    m_sceneItemCount = static_cast<std::uint32_t>(m_sceneStorage.size());
    m_overlayItemCount = static_cast<std::uint32_t>(m_overlayStorage.size());
    m_spotlightCutoutCount = static_cast<std::uint32_t>(m_spotlightStorage.size());
    m_sceneDirtyRectCount = static_cast<std::uint32_t>(m_sceneDirtyStorage.size());
    m_decorationDirtyRectCount = static_cast<std::uint32_t>(m_decorationDirtyStorage.size());
    m_overlayDirtyRectCount = static_cast<std::uint32_t>(m_overlayDirtyStorage.size());
}

void SnowCanvasDisplayCache::updateFilterIndices(const std::vector<std::uint32_t>& changedIndices) {
    for (std::uint32_t index : changedIndices) {
        if (index >= m_sceneStorage.size()) {
            continue;
        }
        const auto position =
            std::lower_bound(m_filterIndices.begin(), m_filterIndices.end(), index);
        const bool present = position != m_filterIndices.end() && *position == index;
        const bool shouldBePresent = m_sceneStorage[index].kind == SNOW_SCENE_DISPLAY_ITEM_FILTER;
        if (shouldBePresent && !present) {
            m_filterIndices.insert(position, index);
        } else if (!shouldBePresent && present) {
            m_filterIndices.erase(position);
        }
    }
}

void SnowCanvasDisplayCache::applyPatchInfo(const SnowPatchInfo& patchInfo) {
    SceneDisplayInfo sceneDisplayInfo{};
    sceneDisplayInfo.item_count = m_sceneItemCount;
    sceneDisplayInfo.dirty_rect_count = m_sceneDirtyRectCount;
    sceneDisplayInfo.surface_width = patchInfo.surface_width;
    sceneDisplayInfo.surface_height = patchInfo.surface_height;
    sceneDisplayInfo.camera_center_x = patchInfo.camera_center_x;
    sceneDisplayInfo.camera_center_y = patchInfo.camera_center_y;
    sceneDisplayInfo.camera_zoom = patchInfo.camera_zoom;
    sceneDisplayInfo.clear_color = patchInfo.clear_color;
    m_sceneDisplayInfo = sceneDisplayInfo;

    WatermarkDisplayInfo watermarkDisplayInfo{};
    watermarkDisplayInfo.surface_width = patchInfo.surface_width;
    watermarkDisplayInfo.surface_height = patchInfo.surface_height;
    watermarkDisplayInfo.camera_center_x = patchInfo.camera_center_x;
    watermarkDisplayInfo.camera_center_y = patchInfo.camera_center_y;
    watermarkDisplayInfo.camera_zoom = patchInfo.camera_zoom;
    watermarkDisplayInfo.watermark_color = patchInfo.watermark_color;
    std::copy_n(reinterpret_cast<const char*>(patchInfo.watermark_text),
                SNOW_WATERMARK_TEXT_CAPACITY, watermarkDisplayInfo.watermark_text.data());
    watermarkDisplayInfo.watermark_text_len = patchInfo.watermark_text_len;
    watermarkDisplayInfo.watermark_font_size = patchInfo.watermark_font_size;
    std::copy_n(reinterpret_cast<const char*>(patchInfo.watermark_font_family),
                SNOW_WATERMARK_FONT_FAMILY_CAPACITY,
                watermarkDisplayInfo.watermark_font_family.data());
    watermarkDisplayInfo.watermark_font_family_len = patchInfo.watermark_font_family_len;
    watermarkDisplayInfo.watermark_angle = patchInfo.watermark_angle;
    watermarkDisplayInfo.watermark_gap = patchInfo.watermark_gap;
    watermarkDisplayInfo.watermark_opacity = patchInfo.watermark_opacity;
    m_watermarkDisplayInfo = watermarkDisplayInfo;

    m_spotlightDisplayInfo = SpotlightDisplayInfo{
        patchInfo.spotlight_color,
        patchInfo.spotlight_opacity,
        patchInfo.spotlight_active != 0,
    };

    OverlayDisplayInfo overlayDisplayInfo{};
    overlayDisplayInfo.item_count = m_overlayItemCount;
    overlayDisplayInfo.dirty_rect_count = m_overlayDirtyRectCount;
    overlayDisplayInfo.surface_width = patchInfo.surface_width;
    overlayDisplayInfo.surface_height = patchInfo.surface_height;
    overlayDisplayInfo.camera_center_x = patchInfo.camera_center_x;
    overlayDisplayInfo.camera_center_y = patchInfo.camera_center_y;
    overlayDisplayInfo.camera_zoom = patchInfo.camera_zoom;
    m_overlayDisplayInfo = overlayDisplayInfo;
    m_patchCursor = SnowPatchCursor{
        patchInfo.scene_revision,
        patchInfo.decoration_revision,
        patchInfo.overlay_revision,
    };
}

namespace snow_canvas_display {

QRect dirtyRectToUpdateRect(const SnowDirtyRect& dirtyRect, int paddingPx) {
    return snow_canvas_render_geometry::alignedRectForBounds(
        QRectF(QPointF(dirtyRect.min_x, dirtyRect.min_y),
               QPointF(dirtyRect.max_x, dirtyRect.max_y)),
        paddingPx);
}

QRegion dirtyRectsToRegion(const SnowDirtyRect* dirtyRects, std::uint32_t dirtyRectCount,
                           const QRect& clip, int paddingPx) {
    QRegion region;
    if (dirtyRects == nullptr) {
        return region;
    }
    for (std::uint32_t index = 0; index < dirtyRectCount; ++index) {
        const SnowDirtyRect& dirtyRect = dirtyRects[index];
        const QRect updateRect = dirtyRectToUpdateRect(dirtyRect, paddingPx).intersected(clip);
        if (!updateRect.isEmpty()) {
            region += updateRect;
        }
    }
    return region;
}

QRegion dirtyVisualizationRegion(const SnowCanvasDisplayCache& cache, const QRect& clip,
                                 int paddingPx) {
    return dirtyRectsToRegion(cache.sceneDirtyRects(), cache.sceneDirtyRectCount(), clip,
                              paddingPx) +
           dirtyRectsToRegion(cache.decorationDirtyRects(), cache.decorationDirtyRectCount(), clip,
                              paddingPx) +
           dirtyRectsToRegion(cache.overlayDirtyRects(), cache.overlayDirtyRectCount(), clip,
                              paddingPx);
}

} // namespace snow_canvas_display

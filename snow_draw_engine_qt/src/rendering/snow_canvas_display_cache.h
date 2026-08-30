#pragma once

#include <QRect>
#include <QRegion>

#include <cstdint>
#include <array>
#include <unordered_map>
#include <vector>

#include "snow_canvas_display_item.h"
#include "snow_draw_engine.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

struct SceneDisplayInfo {
    std::uint32_t item_count = 0;
    std::uint32_t dirty_rect_count = 0;
    double surface_width = 0.0;
    double surface_height = 0.0;
    double camera_center_x = 0.0;
    double camera_center_y = 0.0;
    double camera_zoom = 1.0;
    SnowColorRgba8 clear_color{};
};

struct WatermarkDisplayInfo {
    double surface_width = 0.0;
    double surface_height = 0.0;
    double camera_center_x = 0.0;
    double camera_center_y = 0.0;
    double camera_zoom = 1.0;
    SnowColorRgba8 watermark_color{};
    std::array<char, SNOW_WATERMARK_TEXT_CAPACITY> watermark_text{};
    std::uint16_t watermark_text_len = 0;
    double watermark_font_size = 12.0;
    std::array<char, SNOW_WATERMARK_FONT_FAMILY_CAPACITY> watermark_font_family{};
    std::uint16_t watermark_font_family_len = 0;
    double watermark_angle = 30.0;
    double watermark_gap = 56.0;
    double watermark_opacity = 0.16;
};

struct SpotlightDisplayInfo {
    SnowColorRgba8 color{0, 0, 0, 255};
    double opacity = 0.64;
    bool active = false;
};

struct OverlayDisplayInfo {
    std::uint32_t item_count = 0;
    std::uint32_t dirty_rect_count = 0;
    double surface_width = 0.0;
    double surface_height = 0.0;
    double camera_center_x = 0.0;
    double camera_center_y = 0.0;
    double camera_zoom = 1.0;
};

struct AppliedPenFilterGeometryDelta {
    SnowElementId elementId{};
    std::uint64_t expectedRevision = 0;
    std::uint64_t resultingRevision = 0;
    QRectF oldChangedCanvasBounds;
    QRectF newChangedCanvasBounds;
    bool fullReset = false;
    bool elementRemoved = false;
};

class SnowCanvasDisplayCache {
  public:
    SnowCanvasDisplayCache();

    void reset(const SnowColorRgba8& clearColor);
    void setClearColor(const SnowColorRgba8& clearColor);
    bool sync(SnowRuntime runtime, SnowViewport viewport);

    const SceneDisplayInfo& sceneInfo() const;
    const WatermarkDisplayInfo& watermarkInfo() const;
    const SpotlightDisplayInfo& spotlightInfo() const;
    const OverlayDisplayInfo& overlayInfo() const;
    const SnowPatchCursor& patchCursor() const;

    const SnowCanvasSceneItem* sceneItems() const;
    const SnowCanvasOverlayItem* overlayItems() const;
    const SnowSpotlightCutout* spotlightCutouts() const;
    std::uint32_t sceneItemCount() const;
    std::uint32_t overlayItemCount() const;
    std::uint32_t spotlightCutoutCount() const;
    std::uint32_t lastPenFilterGeometryOpCount() const;
    std::uint32_t lastPenFilterGeometryPointCount() const;
    const std::vector<AppliedPenFilterGeometryDelta>& appliedPenFilterGeometryDeltas() const;
    const std::vector<std::uint32_t>& filterIndices() const;

    const SnowDirtyRect* sceneDirtyRects() const;
    const SnowDirtyRect* overlayDirtyRects() const;
    const SnowDirtyRect* decorationDirtyRects() const;
    std::uint32_t sceneDirtyRectCount() const;
    std::uint32_t overlayDirtyRectCount() const;
    std::uint32_t decorationDirtyRectCount() const;
    void sceneCandidateIndices(const QRegion& exposedRegion,
                               std::vector<std::uint32_t>* outIndices) const;

  private:
    void refreshViews();
    void applyPatchInfo(const SnowPatchInfo& patchInfo);
    void rebuildViewBoundsAndSpatialIndex();
    void rebuildSceneSpatialIndex();
    void updateSceneSpatialItem(std::uint32_t index);
    void removeSceneSpatialItem(std::uint32_t index);
    void rebuildFilterIndices();
    void updateFilterIndices(const std::vector<std::uint32_t>& changedIndices);

    SnowPatchCursor m_patchCursor{};
    SceneDisplayInfo m_sceneDisplayInfo{};
    WatermarkDisplayInfo m_watermarkDisplayInfo{};
    SpotlightDisplayInfo m_spotlightDisplayInfo{};
    OverlayDisplayInfo m_overlayDisplayInfo{};
    std::vector<SnowCanvasSceneItem> m_sceneStorage;
    std::vector<SnowCanvasOverlayItem> m_overlayStorage;
    std::vector<SnowSpotlightCutout> m_spotlightStorage;
    std::vector<SnowDirtyRect> m_sceneDirtyStorage;
    std::vector<SnowDirtyRect> m_overlayDirtyStorage;
    std::vector<SnowDirtyRect> m_decorationDirtyStorage;
    const SnowCanvasSceneItem* m_sceneItems = nullptr;
    const SnowCanvasOverlayItem* m_overlayItems = nullptr;
    const SnowSpotlightCutout* m_spotlightCutouts = nullptr;
    const SnowDirtyRect* m_sceneDirtyRects = nullptr;
    const SnowDirtyRect* m_overlayDirtyRects = nullptr;
    const SnowDirtyRect* m_decorationDirtyRects = nullptr;
    std::uint32_t m_sceneItemCount = 0;
    std::uint32_t m_overlayItemCount = 0;
    std::uint32_t m_spotlightCutoutCount = 0;
    std::uint32_t m_lastPenFilterGeometryOpCount = 0;
    std::uint32_t m_lastPenFilterGeometryPointCount = 0;
    std::vector<AppliedPenFilterGeometryDelta> m_appliedPenFilterGeometryDeltas;
    std::uint32_t m_sceneDirtyRectCount = 0;
    std::uint32_t m_overlayDirtyRectCount = 0;
    std::uint32_t m_decorationDirtyRectCount = 0;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> m_sceneSpatialCells;
    std::vector<std::vector<std::int64_t>> m_sceneItemSpatialCells;
    std::vector<std::uint32_t> m_sceneGlobalItems;
    std::vector<std::uint32_t> m_filterIndices;
    mutable std::vector<std::uint32_t> m_sceneQueryMarks;
    mutable std::uint32_t m_sceneQueryStamp = 0;
};

namespace snow_canvas_display {

QRect dirtyRectToUpdateRect(const SnowDirtyRect& dirtyRect, int paddingPx = 1);

QRegion dirtyRectsToRegion(const SnowDirtyRect* dirtyRects, std::uint32_t dirtyRectCount,
                           const QRect& clip, int paddingPx = 1);

QRegion dirtyVisualizationRegion(const SnowCanvasDisplayCache& cache, const QRect& clip,
                                 int paddingPx = 1);

} // namespace snow_canvas_display

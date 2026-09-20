#pragma once

#include <QPointF>

#include <cstdint>

class SnowCanvasDisplayCache;
struct SnowOverlayDisplayItem;

namespace snow_canvas_render_geometry {
struct ViewProjection;
}

namespace snow_canvas_widget_selection_hit_testing {

enum class SelectionInteractionTarget { None, Move, Handle };

SelectionInteractionTarget
selectionInteractionAtItems(const SnowOverlayDisplayItem* items, std::uint32_t itemCount,
                            const snow_canvas_render_geometry::ViewProjection& projection,
                            const QPointF& viewPosition);
SelectionInteractionTarget selectionInteractionAt(const SnowCanvasDisplayCache& displayCache,
                                                  const QPointF& viewPosition);

bool pointerHitsSelectionInteractionItems(
    const SnowOverlayDisplayItem* items, std::uint32_t itemCount,
    const snow_canvas_render_geometry::ViewProjection& projection, const QPointF& viewPosition);
bool pointerHitsSelectionInteraction(const SnowCanvasDisplayCache& displayCache,
                                     const QPointF& viewPosition);

} // namespace snow_canvas_widget_selection_hit_testing

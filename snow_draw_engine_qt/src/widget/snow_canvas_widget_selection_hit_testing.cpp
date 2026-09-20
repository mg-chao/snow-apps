#include "snow_canvas_widget_selection_hit_testing.h"

#include "snow_canvas_display_cache.h"
#include "snow_canvas_render_geometry.h"

#include <algorithm>
#include <cmath>

namespace snow_canvas_widget_selection_hit_testing {
namespace {

bool isSelectionInteractionRectKind(SnowOverlayRectKind kind) {
    switch (kind) {
    case SNOW_OVERLAY_RECT_SELECTION_RESIZE_HANDLE:
    case SNOW_OVERLAY_RECT_SELECTION_ROTATION_HANDLE:
    case SNOW_OVERLAY_RECT_SELECTION_CORNER_RADIUS_HANDLE:
    case SNOW_OVERLAY_RECT_ARROW_ENDPOINT_HANDLE:
    case SNOW_OVERLAY_RECT_ARROW_FOCUS_HANDLE:
    case SNOW_OVERLAY_RECT_ARROW_SEGMENT_HANDLE:
        return true;
    default:
        return false;
    }
}

bool isSelectionFrameRectKind(SnowOverlayRectKind kind) {
    return kind == SNOW_OVERLAY_RECT_SELECTION_FRAME ||
           kind == SNOW_OVERLAY_RECT_SELECTION_MULTI_FRAME;
}

QPointF overlayRectLocalViewPoint(const snow_canvas_render_geometry::ViewProjection& projection,
                                  const SnowOverlayDisplayItem& item, const QPointF& viewPosition) {
    const QPointF center =
        snow_canvas_render_geometry::canvasToView(projection, item.center_x, item.center_y);
    const QPointF delta = viewPosition - center;
    const double cosTheta = std::cos(-item.rotation);
    const double sinTheta = std::sin(-item.rotation);
    return QPointF(delta.x() * cosTheta - delta.y() * sinTheta,
                   delta.x() * sinTheta + delta.y() * cosTheta);
}

bool overlayRectContainsViewPoint(const snow_canvas_render_geometry::ViewProjection& projection,
                                  const SnowOverlayDisplayItem& item, const QPointF& viewPosition,
                                  double extraPx) {
    const QPointF local = overlayRectLocalViewPoint(projection, item, viewPosition);
    const double halfWidth = std::max(0.0, item.width * projection.cameraZoom) / 2.0 + extraPx;
    const double halfHeight = std::max(0.0, item.height * projection.cameraZoom) / 2.0 + extraPx;
    return std::abs(local.x()) <= halfWidth && std::abs(local.y()) <= halfHeight;
}

bool overlayRectStrictlyContainsViewPoint(
    const snow_canvas_render_geometry::ViewProjection& projection,
    const SnowOverlayDisplayItem& item, const QPointF& viewPosition) {
    const QPointF local = overlayRectLocalViewPoint(projection, item, viewPosition);
    const double halfWidth = std::max(0.0, item.width * projection.cameraZoom) / 2.0;
    const double halfHeight = std::max(0.0, item.height * projection.cameraZoom) / 2.0;
    return halfWidth > 0.0 && halfHeight > 0.0 && std::abs(local.x()) < halfWidth &&
           std::abs(local.y()) < halfHeight;
}

bool overlayTextSelectionMoveRingContainsViewPoint(
    const snow_canvas_render_geometry::ViewProjection& projection,
    const SnowOverlayDisplayItem& selectionFrame, const SnowOverlayDisplayItem& textActualFrame,
    const QPointF& viewPosition) {
    return overlayRectStrictlyContainsViewPoint(projection, selectionFrame, viewPosition) &&
           !overlayRectContainsViewPoint(projection, textActualFrame, viewPosition, 0.0);
}

bool nearSelectionEdge(double value, double edge, double inwardSign, double outerTolerance,
                       double innerTolerance) {
    const double distance = (value - edge) * inwardSign;
    return distance >= -outerTolerance && distance <= innerTolerance;
}

bool insideSelectionEdgeSpan(double value, double min, double max, double tolerance) {
    return value > min + tolerance && value < max - tolerance;
}

bool overlayFrameEdgeContainsViewPoint(
    const snow_canvas_render_geometry::ViewProjection& projection,
    const SnowOverlayDisplayItem& item, const QPointF& viewPosition) {
    constexpr double kOuterTolerancePx = 6.0;
    constexpr double kInnerTolerancePx = 6.0;
    const QPointF local = overlayRectLocalViewPoint(projection, item, viewPosition);
    const double halfWidth = std::max(0.0, item.width * projection.cameraZoom) / 2.0;
    const double halfHeight = std::max(0.0, item.height * projection.cameraZoom) / 2.0;
    const double spanTolerance = std::max(kOuterTolerancePx, kInnerTolerancePx);
    if (halfWidth <= 0.0 || halfHeight <= 0.0) {
        return false;
    }

    const double minX = -halfWidth;
    const double maxX = halfWidth;
    const double minY = -halfHeight;
    const double maxY = halfHeight;
    return (nearSelectionEdge(local.y(), minY, 1.0, kOuterTolerancePx, kInnerTolerancePx) &&
            insideSelectionEdgeSpan(local.x(), minX, maxX, spanTolerance)) ||
           (nearSelectionEdge(local.x(), maxX, -1.0, kOuterTolerancePx, kInnerTolerancePx) &&
            insideSelectionEdgeSpan(local.y(), minY, maxY, spanTolerance)) ||
           (nearSelectionEdge(local.y(), maxY, -1.0, kOuterTolerancePx, kInnerTolerancePx) &&
            insideSelectionEdgeSpan(local.x(), minX, maxX, spanTolerance)) ||
           (nearSelectionEdge(local.x(), minX, 1.0, kOuterTolerancePx, kInnerTolerancePx) &&
            insideSelectionEdgeSpan(local.y(), minY, maxY, spanTolerance));
}

template <typename Item>
SelectionInteractionTarget
selectionInteractionAtItemRange(const Item* items, std::uint32_t itemCount,
                                const snow_canvas_render_geometry::ViewProjection& projection,
                                const QPointF& viewPosition) {
    if (items == nullptr || itemCount == 0) {
        return SelectionInteractionTarget::None;
    }

    constexpr double kHandleHitSizePx = 12.0;
    const SnowOverlayDisplayItem* textActualFrame = nullptr;
    for (std::uint32_t offset = 0; offset < itemCount; ++offset) {
        const SnowOverlayDisplayItem& item = items[itemCount - 1 - offset];
        if (item.kind != SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT) {
            continue;
        }

        if (item.rect_kind == SNOW_OVERLAY_RECT_TEXT_ACTUAL_FRAME) {
            textActualFrame = &item;
            continue;
        }

        if (isSelectionInteractionRectKind(item.rect_kind)) {
            const double handleWidth = std::max(0.0, item.width * projection.cameraZoom);
            const double handleHeight = std::max(0.0, item.height * projection.cameraZoom);
            const double extraPx =
                std::max(0.0, kHandleHitSizePx - std::min(handleWidth, handleHeight)) / 2.0;
            if (overlayRectContainsViewPoint(projection, item, viewPosition, extraPx)) {
                return SelectionInteractionTarget::Handle;
            }
            continue;
        }

        if (item.rect_kind == SNOW_OVERLAY_RECT_SELECTION_FRAME && textActualFrame != nullptr &&
            overlayTextSelectionMoveRingContainsViewPoint(projection, item, *textActualFrame,
                                                          viewPosition)) {
            return SelectionInteractionTarget::Move;
        }

        if (isSelectionFrameRectKind(item.rect_kind) &&
            overlayFrameEdgeContainsViewPoint(projection, item, viewPosition)) {
            return SelectionInteractionTarget::Handle;
        }
    }
    return SelectionInteractionTarget::None;
}

} // namespace

bool pointerHitsSelectionInteractionItems(
    const SnowOverlayDisplayItem* items, std::uint32_t itemCount,
    const snow_canvas_render_geometry::ViewProjection& projection, const QPointF& viewPosition) {
    return selectionInteractionAtItems(items, itemCount, projection, viewPosition) !=
           SelectionInteractionTarget::None;
}

bool pointerHitsSelectionInteraction(const SnowCanvasDisplayCache& displayCache,
                                     const QPointF& viewPosition) {
    return selectionInteractionAt(displayCache, viewPosition) != SelectionInteractionTarget::None;
}

SelectionInteractionTarget
selectionInteractionAtItems(const SnowOverlayDisplayItem* items, std::uint32_t itemCount,
                            const snow_canvas_render_geometry::ViewProjection& projection,
                            const QPointF& viewPosition) {
    return selectionInteractionAtItemRange(items, itemCount, projection, viewPosition);
}

SelectionInteractionTarget selectionInteractionAt(const SnowCanvasDisplayCache& displayCache,
                                                  const QPointF& viewPosition) {
    const snow_canvas_render_geometry::ViewProjection projection =
        snow_canvas_render_geometry::overlayProjection(displayCache.overlayInfo());
    return selectionInteractionAtItemRange(
        displayCache.overlayItems(), displayCache.overlayItemCount(), projection, viewPosition);
}

} // namespace snow_canvas_widget_selection_hit_testing

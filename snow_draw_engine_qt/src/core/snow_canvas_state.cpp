#include "snow_canvas_state.h"

#include "snow_canvas_utf8.h"

#include <cstdint>

namespace snow_canvas_state {

bool Changes::any() const {
    return activeToolChanged || styleToolbarChanged || historyChanged || snapConfigChanged ||
           gridConfigChanged;
}

bool colorsEqual(const SnowColorRgba8& lhs, const SnowColorRgba8& rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}

bool cornerRadiiEqual(const SnowCornerRadii& lhs, const SnowCornerRadii& rhs) {
    return lhs.top_left == rhs.top_left && lhs.top_right == rhs.top_right &&
           lhs.bottom_right == rhs.bottom_right && lhs.bottom_left == rhs.bottom_left;
}

bool shapeStylesEqual(const SnowShapeStyle& lhs, const SnowShapeStyle& rhs) {
    return colorsEqual(lhs.fill, rhs.fill) && lhs.fill_style == rhs.fill_style &&
           colorsEqual(lhs.stroke, rhs.stroke) && lhs.stroke_width == rhs.stroke_width &&
           cornerRadiiEqual(lhs.corner_radii, rhs.corner_radii) &&
           lhs.start_arrowhead == rhs.start_arrowhead && lhs.end_arrowhead == rhs.end_arrowhead &&
           lhs.stroke_style == rhs.stroke_style && lhs.arrow_type == rhs.arrow_type &&
           lhs.opacity == rhs.opacity && lhs.highlight_shape == rhs.highlight_shape &&
           lhs.shape == rhs.shape;
}

bool fontFamiliesEqual(const char* lhs, std::uint32_t lhsLength, const char* rhs,
                       std::uint32_t rhsLength) {
    return snow_canvas_utf8::fieldsEqual(lhs, lhsLength, rhs, rhsLength,
                                         SNOW_FONT_FAMILY_UTF8_CAPACITY);
}

bool textStylesEqual(const SnowTextStyle& lhs, const SnowTextStyle& rhs) {
    return colorsEqual(lhs.color, rhs.color) && lhs.font_size == rhs.font_size &&
           colorsEqual(lhs.fill, rhs.fill) && lhs.fill_style == rhs.fill_style &&
           colorsEqual(lhs.stroke, rhs.stroke) && lhs.stroke_width == rhs.stroke_width &&
           cornerRadiiEqual(lhs.corner_radii, rhs.corner_radii) &&
           lhs.horizontal_align == rhs.horizontal_align &&
           lhs.vertical_align == rhs.vertical_align && lhs.opacity == rhs.opacity &&
           lhs.font_family_truncated == rhs.font_family_truncated &&
           fontFamiliesEqual(lhs.font_family_utf8, lhs.font_family_utf8_len, rhs.font_family_utf8,
                             rhs.font_family_utf8_len);
}

bool serialNumberStylesEqual(const SnowSerialNumberStyle& lhs, const SnowSerialNumberStyle& rhs) {
    return lhs.number == rhs.number && colorsEqual(lhs.color, rhs.color) &&
           colorsEqual(lhs.fill, rhs.fill) && lhs.fill_style == rhs.fill_style &&
           lhs.font_size == rhs.font_size && lhs.stroke_width == rhs.stroke_width &&
           lhs.stroke_style == rhs.stroke_style && lhs.opacity == rhs.opacity &&
           lhs.font_family_truncated == rhs.font_family_truncated &&
           fontFamiliesEqual(lhs.font_family_utf8, lhs.font_family_utf8_len, rhs.font_family_utf8,
                             rhs.font_family_utf8_len);
}

bool filterStylesEqual(const SnowFilterStyle& lhs, const SnowFilterStyle& rhs) {
    return lhs.filter_type == rhs.filter_type && lhs.strength == rhs.strength &&
           lhs.opacity == rhs.opacity && lhs.stroke_width == rhs.stroke_width;
}

bool styleToolbarStatesEqual(const SnowStyleToolbarState& lhs, const SnowStyleToolbarState& rhs) {
    return lhs.source == rhs.source && textStylesEqual(lhs.text_style, rhs.text_style) &&
           serialNumberStylesEqual(lhs.serial_number_style, rhs.serial_number_style) &&
           shapeStylesEqual(lhs.shape_style, rhs.shape_style) &&
           filterStylesEqual(lhs.filter_style, rhs.filter_style) &&
           lhs.text_style_mixed == rhs.text_style_mixed &&
           lhs.serial_number_style_mixed == rhs.serial_number_style_mixed &&
           lhs.shape_style_mixed == rhs.shape_style_mixed &&
           lhs.filter_style_mixed == rhs.filter_style_mixed;
}

bool watermarkConfigsEqual(const SnowWatermarkConfig& lhs, const SnowWatermarkConfig& rhs) {
    return colorsEqual(lhs.color, rhs.color) && lhs.font_size == rhs.font_size &&
           lhs.angle == rhs.angle && lhs.gap == rhs.gap && lhs.opacity == rhs.opacity &&
           snow_canvas_utf8::fieldsEqual(lhs.text_utf8, lhs.text_utf8_len, rhs.text_utf8,
                                         rhs.text_utf8_len, SNOW_WATERMARK_TEXT_CAPACITY) &&
           snow_canvas_utf8::fieldsEqual(lhs.font_family_utf8, lhs.font_family_utf8_len,
                                         rhs.font_family_utf8, rhs.font_family_utf8_len,
                                         SNOW_WATERMARK_FONT_FAMILY_CAPACITY);
}

bool spotlightConfigsEqual(const SnowSpotlightConfig& lhs, const SnowSpotlightConfig& rhs) {
    return colorsEqual(lhs.color, rhs.color) && lhs.opacity == rhs.opacity;
}

bool historyStatesEqual(const SnowHistoryState& lhs, const SnowHistoryState& rhs) {
    return lhs.can_undo == rhs.can_undo && lhs.can_redo == rhs.can_redo;
}

bool snapConfigsEqual(const SnowSnapConfig& lhs, const SnowSnapConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.enable_point_snaps == rhs.enable_point_snaps &&
           lhs.enable_gap_snaps == rhs.enable_gap_snaps && lhs.show_guides == rhs.show_guides &&
           lhs.show_gap_size == rhs.show_gap_size && lhs.distance == rhs.distance &&
           colorsEqual(lhs.line_color, rhs.line_color) && lhs.line_width == rhs.line_width &&
           lhs.marker_size == rhs.marker_size && lhs.gap_dash_length == rhs.gap_dash_length &&
           lhs.gap_dash_gap == rhs.gap_dash_gap;
}

bool gridConfigsEqual(const SnowGridConfig& lhs, const SnowGridConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.size == rhs.size;
}

Changes diffSnapshots(const Snapshot& previous, const Snapshot& next) {
    return Changes{
        previous.activeTool != next.activeTool,
        !styleToolbarStatesEqual(previous.styleToolbarState, next.styleToolbarState) ||
            !watermarkConfigsEqual(previous.watermarkConfig, next.watermarkConfig) ||
            !spotlightConfigsEqual(previous.spotlightConfig, next.spotlightConfig),
        !historyStatesEqual(previous.historyState, next.historyState),
        !snapConfigsEqual(previous.snapConfig, next.snapConfig),
        !gridConfigsEqual(previous.gridConfig, next.gridConfig),
    };
}

bool readSnapshot(SnowRuntime runtime, SnowViewport viewport, Snapshot* outSnapshot) {
    if (runtime == nullptr || viewport == nullptr || outSnapshot == nullptr) {
        return false;
    }

    Snapshot snapshot;
    if (snow_viewport_get_active_tool(runtime, viewport, &snapshot.activeTool) != SNOW_OK) {
        return false;
    }
    if (snow_viewport_get_style_toolbar_state(runtime, viewport, &snapshot.styleToolbarState) !=
        SNOW_OK) {
        return false;
    }
    if (snow_viewport_get_watermark_config(runtime, viewport, &snapshot.watermarkConfig) !=
        SNOW_OK) {
        return false;
    }
    if (snow_viewport_get_spotlight_config(runtime, viewport, &snapshot.spotlightConfig) !=
        SNOW_OK) {
        return false;
    }
    if (snow_runtime_get_history_state(runtime, &snapshot.historyState) != SNOW_OK) {
        return false;
    }
    if (snow_viewport_get_snap_config(runtime, viewport, &snapshot.snapConfig) != SNOW_OK) {
        return false;
    }
    if (snow_viewport_get_grid_config(runtime, viewport, &snapshot.gridConfig) != SNOW_OK) {
        return false;
    }

    *outSnapshot = snapshot;
    return true;
}

const Snapshot& Store::snapshot() const {
    return m_snapshot;
}

void Store::reset() {
    m_snapshot = Snapshot{};
}

void Store::applyEngineConfigDefaults(const SnowEngineConfig& config) {
    m_snapshot.snapConfig = config.snap;
    m_snapshot.gridConfig = config.grid;
}

bool Store::refresh(SnowRuntime runtime, SnowViewport viewport, Changes* outChanges) {
    Snapshot next;
    if (!readSnapshot(runtime, viewport, &next)) {
        return false;
    }

    if (outChanges != nullptr) {
        *outChanges = diffSnapshots(m_snapshot, next);
    }
    m_snapshot = next;
    return true;
}

} // namespace snow_canvas_state

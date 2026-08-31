#include "snow_canvas_commands.h"

namespace snow_canvas_commands {
namespace {

bool hasViewport(SnowRuntime runtime, SnowViewport viewport) {
    return runtime != nullptr && viewport != nullptr;
}

bool setSnapConfigRaw(SnowRuntime runtime, SnowViewport viewport, SnowSnapConfig* config,
                      SnowChangedViewportList* changedViewports) {
    return snow_viewport_set_snap_config_ex(runtime, viewport, config, changedViewports) == SNOW_OK;
}

bool getSnapConfigRaw(SnowRuntime runtime, SnowViewport viewport, SnowSnapConfig* config) {
    return snow_viewport_get_snap_config(runtime, viewport, config) == SNOW_OK;
}

bool setGridConfigRaw(SnowRuntime runtime, SnowViewport viewport, SnowGridConfig* config,
                      SnowChangedViewportList* changedViewports) {
    return snow_viewport_set_grid_config_ex(runtime, viewport, config, changedViewports) == SNOW_OK;
}

bool getGridConfigRaw(SnowRuntime runtime, SnowViewport viewport, SnowGridConfig* config) {
    return snow_viewport_get_grid_config(runtime, viewport, config) == SNOW_OK;
}

} // namespace

SnowTextCommitDraft CommitTextRequest::toAbi() const {
    SnowTextCommitDraft draft{};
    draft.element_id = elementId;
    draft.has_existing_element = hasExistingElement ? 1 : 0;
    draft.auto_resize = autoResize ? 1 : 0;
    draft.update_default_style = updateDefaultStyle ? 1 : 0;
    draft.center_x = centerX;
    draft.center_y = centerY;
    draft.text_utf8 = utf8Data;
    draft.text_utf8_len = utf8Len;
    draft.measured_layout = measuredLayout;
    draft.style = style;
    return draft;
}

SnowActiveTextDraftPresentation ActiveTextDraftPresentationRequest::toAbi() const {
    SnowActiveTextDraftPresentation draft{};
    draft.element_id = elementId;
    draft.has_existing_element = hasExistingElement ? 1 : 0;
    draft.auto_resize = autoResize ? 1 : 0;
    draft.center_x = centerX;
    draft.center_y = centerY;
    draft.width = width;
    draft.height = height;
    draft.rotation = rotation;
    draft.text_utf8 = utf8Data;
    draft.text_utf8_len = utf8Len;
    draft.style = style;
    return draft;
}

MutationResult setActiveTool(SnowRuntime runtime, SnowViewport viewport, SnowActiveTool tool) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_set_active_tool_ex(
                         runtime, viewport, tool, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult undo(SnowRuntime runtime) {
    MutationResult result;
    if (runtime == nullptr) {
        return result;
    }
    result.success = snow_runtime_undo_ex(runtime, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult redo(SnowRuntime runtime) {
    MutationResult result;
    if (runtime == nullptr) {
        return result;
    }
    result.success = snow_runtime_redo_ex(runtime, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult resetEditingState(SnowRuntime runtime, SnowViewport viewport) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_reset_editing_state_ex(
                         runtime, viewport, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult selectElement(SnowRuntime runtime, SnowViewport viewport, SnowElementId id) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_select_element_ex(runtime, viewport, id,
                                                     result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult deleteSelected(SnowRuntime runtime, SnowViewport viewport) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_delete_selected_ex(
                         runtime, viewport, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult duplicateSelected(SnowRuntime runtime, SnowViewport viewport, double offsetX,
                                 double offsetY) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success =
        snow_viewport_duplicate_selected_ex(runtime, viewport, offsetX, offsetY,
                                            result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult reorderSelected(SnowRuntime runtime, SnowViewport viewport, std::uint32_t action) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_reorder_selected_ex(
                         runtime, viewport, action, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult setSelectedOpacity(SnowRuntime runtime, SnowViewport viewport, double opacity) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_set_selected_opacity_ex(
                         runtime, viewport, opacity, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult adjustSelectedSerialNumbers(SnowRuntime runtime, SnowViewport viewport,
                                           std::int64_t delta) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_adjust_selected_serial_numbers_ex(
                         runtime, viewport, delta, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

CreateSerialNumberTextResult createSerialNumberText(SnowRuntime runtime, SnowViewport viewport,
                                                    const SnowTextLayoutSize& layout) {
    CreateSerialNumberTextResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }

    std::uint8_t hasTextId = 0;
    result.success = snow_viewport_create_serial_number_text_ex(
                         runtime, viewport, layout.width, layout.height, &result.textId, &hasTextId,
                         result.changedViewports.outParam()) == SNOW_OK;
    result.hasTextId = hasTextId != 0;
    return result;
}

MutationResult commitText(SnowRuntime runtime, SnowViewport viewport,
                          const CommitTextRequest& request) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    if (request.utf8Data == nullptr && request.utf8Len > 0) {
        return result;
    }

    const SnowTextCommitDraft draft = request.toAbi();
    result.success = snow_viewport_commit_text_draft_payload_ex(
                         runtime, viewport, &draft, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

ProcessInputResult processInput(SnowRuntime runtime, SnowViewport viewport,
                                const SnowInputEvent& event) {
    ProcessInputResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }

    result.success = snow_viewport_process_input_ex(runtime, viewport, &event, &result.output,
                                                    result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

ProcessInputResult processPointerMoveBatch(SnowRuntime runtime, SnowViewport viewport,
                                           const std::vector<SnowInputEvent>& events) {
    ProcessInputResult result;
    if (!hasViewport(runtime, viewport) || events.empty()) {
        return result;
    }
    result.success =
        snow_viewport_process_pointer_move_batch_ex(
            runtime, viewport, events.data(), static_cast<std::uint32_t>(events.size()),
            &result.output, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

TextResizeMeasurementResult activeTextResizeMeasurement(SnowRuntime runtime,
                                                        SnowViewport viewport) {
    TextResizeMeasurementResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    std::uint8_t active = 0;
    result.success = snow_viewport_get_active_text_resize_measurement(
                         runtime, viewport, &result.info, &active) == SNOW_OK;
    result.active = active != 0;
    return result;
}

MutationResult applyActiveTextResizeMeasurement(SnowRuntime runtime, SnowViewport viewport,
                                                const SnowTextLayoutSize& layout) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_apply_active_text_resize_measurement_ex(
                         runtime, viewport, &layout, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult setActiveTextDraftPresentation(SnowRuntime runtime, SnowViewport viewport,
                                              const ActiveTextDraftPresentationRequest& request) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    if (request.utf8Data == nullptr && request.utf8Len > 0) {
        return result;
    }

    const SnowActiveTextDraftPresentation draft = request.toAbi();
    result.success = snow_viewport_set_active_text_draft_presentation_ex(
                         runtime, viewport, &draft, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult clearActiveTextDraftPresentation(SnowRuntime runtime, SnowViewport viewport) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_clear_active_text_draft_presentation_ex(
                         runtime, viewport, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult setShapeStylePatch(SnowRuntime runtime, SnowViewport viewport,
                                  const SnowShapeStyle& style, std::uint32_t properties,
                                  SnowShapeKind kind) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success =
        snow_viewport_set_shape_style_patch_ex(runtime, viewport, &style, properties, kind,
                                               result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult setWatermarkConfig(SnowRuntime runtime, SnowViewport viewport,
                                  const SnowWatermarkConfig& config) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_set_watermark_config_ex(
                         runtime, viewport, &config, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult setSpotlightConfig(SnowRuntime runtime, SnowViewport viewport,
                                  const SnowSpotlightConfig& config) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_set_spotlight_config_ex(
                         runtime, viewport, &config, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult setFilterStyle(SnowRuntime runtime, SnowViewport viewport,
                              const SnowFilterStyle& style, std::uint32_t properties) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success =
        snow_viewport_set_filter_style_ex(runtime, viewport, &style, properties,
                                          result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult setTextStyle(SnowRuntime runtime, SnowViewport viewport, const SnowTextStyle& style,
                            const std::vector<SnowTextLayoutOverride>& layouts) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_set_text_style_ex(runtime, viewport, &style,
                                                     layouts.empty() ? nullptr : layouts.data(),
                                                     static_cast<std::uint32_t>(layouts.size()),
                                                     result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

MutationResult setSerialNumberStyle(SnowRuntime runtime, SnowViewport viewport,
                                    const SnowSerialNumberStyle& style) {
    MutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }
    result.success = snow_viewport_set_serial_number_style_ex(
                         runtime, viewport, &style, result.changedViewports.outParam()) == SNOW_OK;
    return result;
}

PairedMutationResult setSnapConfig(SnowRuntime runtime, SnowViewport viewport,
                                   SnowSnapConfig config) {
    PairedMutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }

    if (!setSnapConfigRaw(runtime, viewport, &config, result.firstChangedViewports.outParam())) {
        return result;
    }

    if (config.enabled == 0) {
        result.success = true;
        return result;
    }

    SnowGridConfig gridConfig{};
    if (!getGridConfigRaw(runtime, viewport, &gridConfig)) {
        return result;
    }
    if (gridConfig.enabled != 0) {
        gridConfig.enabled = 0;
        if (!setGridConfigRaw(runtime, viewport, &gridConfig,
                              result.secondChangedViewports.outParam())) {
            return result;
        }
    }

    result.success = true;
    return result;
}

PairedMutationResult setGridConfig(SnowRuntime runtime, SnowViewport viewport,
                                   SnowGridConfig config) {
    PairedMutationResult result;
    if (!hasViewport(runtime, viewport)) {
        return result;
    }

    if (!setGridConfigRaw(runtime, viewport, &config, result.firstChangedViewports.outParam())) {
        return result;
    }

    if (config.enabled == 0) {
        result.success = true;
        return result;
    }

    SnowSnapConfig snapConfig{};
    if (!getSnapConfigRaw(runtime, viewport, &snapConfig)) {
        return result;
    }
    if (snapConfig.enabled != 0) {
        snapConfig.enabled = 0;
        if (!setSnapConfigRaw(runtime, viewport, &snapConfig,
                              result.secondChangedViewports.outParam())) {
            return result;
        }
    }

    result.success = true;
    return result;
}

} // namespace snow_canvas_commands

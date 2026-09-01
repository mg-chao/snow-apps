#pragma once

#include "snow_canvas_ffi_handles.h"
#include "snow_draw_engine.h"

#include <cstdint>
#include <vector>

namespace snow_canvas_commands {

struct MutationResult {
    bool success = false;
    ScopedChangedViewportList changedViewports;
};

struct PairedMutationResult {
    bool success = false;
    ScopedChangedViewportList firstChangedViewports;
    ScopedChangedViewportList secondChangedViewports;
};

struct CreateSerialNumberTextResult {
    bool success = false;
    ScopedChangedViewportList changedViewports;
    SnowElementId textId{};
    bool hasTextId = false;
};

struct CommitTextRequest {
    SnowElementId elementId{};
    bool hasExistingElement = false;
    double centerX = 0.0;
    double centerY = 0.0;
    const char* utf8Data = nullptr;
    std::uint32_t utf8Len = 0;
    SnowTextLayoutSize measuredLayout{1.0, 1.0};
    SnowTextStyle style{};
    bool autoResize = false;
    bool updateDefaultStyle = false;

    SnowTextCommitDraft toAbi() const;
};

struct ActiveTextDraftPresentationRequest {
    SnowElementId elementId{};
    bool hasExistingElement = false;
    double centerX = 0.0;
    double centerY = 0.0;
    double width = 1.0;
    double height = 1.0;
    double rotation = 0.0;
    const char* utf8Data = nullptr;
    std::uint32_t utf8Len = 0;
    SnowTextStyle style{};
    bool autoResize = false;

    SnowActiveTextDraftPresentation toAbi() const;
};

struct ProcessInputResult {
    bool success = false;
    ScopedChangedViewportList changedViewports;
    SnowInteractionOutput output{};
};

struct TextResizeMeasurementResult {
    bool success = false;
    bool active = false;
    SnowTextElementInfo info{};
};

MutationResult setActiveTool(SnowRuntime runtime, SnowViewport viewport, SnowActiveTool tool);
MutationResult undo(SnowRuntime runtime);
MutationResult redo(SnowRuntime runtime);
MutationResult resetEditingState(SnowRuntime runtime, SnowViewport viewport);
MutationResult selectElement(SnowRuntime runtime, SnowViewport viewport, SnowElementId id);
MutationResult deleteSelected(SnowRuntime runtime, SnowViewport viewport);
MutationResult duplicateSelected(SnowRuntime runtime, SnowViewport viewport, double offsetX,
                                 double offsetY);
MutationResult reorderSelected(SnowRuntime runtime, SnowViewport viewport, std::uint32_t action);
MutationResult setSelectedOpacity(SnowRuntime runtime, SnowViewport viewport, double opacity);
MutationResult adjustSelectedSerialNumbers(SnowRuntime runtime, SnowViewport viewport,
                                           std::int64_t delta);
CreateSerialNumberTextResult createSerialNumberText(SnowRuntime runtime, SnowViewport viewport,
                                                    const SnowTextLayoutSize& layout);
MutationResult commitText(SnowRuntime runtime, SnowViewport viewport,
                          const CommitTextRequest& request);
ProcessInputResult processInput(SnowRuntime runtime, SnowViewport viewport,
                                const SnowInputEvent& event);
ProcessInputResult processPointerMoveBatch(SnowRuntime runtime, SnowViewport viewport,
                                           const std::vector<SnowInputEvent>& events);
TextResizeMeasurementResult activeTextResizeMeasurement(SnowRuntime runtime, SnowViewport viewport);
MutationResult applyActiveTextResizeMeasurement(SnowRuntime runtime, SnowViewport viewport,
                                                const SnowTextLayoutSize& layout);
MutationResult setActiveTextDraftPresentation(SnowRuntime runtime, SnowViewport viewport,
                                              const ActiveTextDraftPresentationRequest& request);
MutationResult clearActiveTextDraftPresentation(SnowRuntime runtime, SnowViewport viewport);
MutationResult setShapeStylePatch(SnowRuntime runtime, SnowViewport viewport,
                                  const SnowShapeStyle& style, std::uint32_t properties,
                                  SnowShapeKind kind);
MutationResult setWatermarkConfig(SnowRuntime runtime, SnowViewport viewport,
                                  const SnowWatermarkConfig& config);
MutationResult setSpotlightConfig(SnowRuntime runtime, SnowViewport viewport,
                                  const SnowSpotlightConfig& config);
MutationResult setFilterStyle(SnowRuntime runtime, SnowViewport viewport,
                              const SnowFilterStyle& style, std::uint32_t properties);
MutationResult setTextStyle(SnowRuntime runtime, SnowViewport viewport, const SnowTextStyle& style,
                            const std::vector<SnowTextLayoutOverride>& layouts = {});
MutationResult setSerialNumberStyle(SnowRuntime runtime, SnowViewport viewport,
                                    const SnowSerialNumberStyle& style);
PairedMutationResult setSnapConfig(SnowRuntime runtime, SnowViewport viewport,
                                   SnowSnapConfig config);
PairedMutationResult setGridConfig(SnowRuntime runtime, SnowViewport viewport,
                                   SnowGridConfig config);

} // namespace snow_canvas_commands

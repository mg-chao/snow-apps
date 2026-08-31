#pragma once

#include "snow_draw_engine.h"

namespace snow_canvas_state {

struct Snapshot {
    SnowActiveTool activeTool = SNOW_ACTIVE_TOOL_SHAPE;
    SnowStyleToolbarState styleToolbarState{};
    SnowWatermarkConfig watermarkConfig{};
    SnowSpotlightConfig spotlightConfig{};
    SnowHistoryState historyState{};
    SnowSnapConfig snapConfig{};
    SnowGridConfig gridConfig{};
};

struct Changes {
    bool activeToolChanged = false;
    bool styleToolbarChanged = false;
    bool historyChanged = false;
    bool snapConfigChanged = false;
    bool gridConfigChanged = false;

    bool any() const;
};

bool colorsEqual(const SnowColorRgba8& lhs, const SnowColorRgba8& rhs);
bool cornerRadiiEqual(const SnowCornerRadii& lhs, const SnowCornerRadii& rhs);
bool shapeStylesEqual(const SnowShapeStyle& lhs, const SnowShapeStyle& rhs);
bool textStylesEqual(const SnowTextStyle& lhs, const SnowTextStyle& rhs);
bool serialNumberStylesEqual(const SnowSerialNumberStyle& lhs, const SnowSerialNumberStyle& rhs);
bool filterStylesEqual(const SnowFilterStyle& lhs, const SnowFilterStyle& rhs);
bool styleToolbarStatesEqual(const SnowStyleToolbarState& lhs, const SnowStyleToolbarState& rhs);
bool watermarkConfigsEqual(const SnowWatermarkConfig& lhs, const SnowWatermarkConfig& rhs);
bool spotlightConfigsEqual(const SnowSpotlightConfig& lhs, const SnowSpotlightConfig& rhs);
bool historyStatesEqual(const SnowHistoryState& lhs, const SnowHistoryState& rhs);
bool snapConfigsEqual(const SnowSnapConfig& lhs, const SnowSnapConfig& rhs);
bool gridConfigsEqual(const SnowGridConfig& lhs, const SnowGridConfig& rhs);

Changes diffSnapshots(const Snapshot& previous, const Snapshot& next);
bool readSnapshot(SnowRuntime runtime, SnowViewport viewport, Snapshot* outSnapshot);

class Store final {
  public:
    const Snapshot& snapshot() const;
    void reset();
    void applyEngineConfigDefaults(const SnowEngineConfig& config);
    bool refresh(SnowRuntime runtime, SnowViewport viewport, Changes* outChanges = nullptr);

  private:
    Snapshot m_snapshot;
};

} // namespace snow_canvas_state

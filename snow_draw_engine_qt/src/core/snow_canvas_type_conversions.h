#pragma once

#include "snow_draw_engine.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

namespace snow_canvas_types {

QColor toQColor(const SnowColorRgba8& color);
SnowColorRgba8 toEngineColor(const QColor& color);

SnowCanvasTool toCanvasTool(SnowActiveTool tool);
SnowActiveTool toEngineTool(SnowCanvasTool tool);

SnowCanvasStyleToolbarSource toCanvasStyleToolbarSource(SnowStyleToolbarSource source);

SnowShapeKind toEngineShapeKind(SnowCanvasShapeKind kind);

SnowCanvasArrowhead toCanvasArrowhead(SnowArrowhead arrowhead);
SnowArrowhead toEngineArrowhead(SnowCanvasArrowhead arrowhead);

SnowCanvasArrowType toCanvasArrowType(SnowArrowType arrowType);
SnowArrowType toEngineArrowType(SnowCanvasArrowType arrowType);

SnowCanvasTextHorizontalAlign toCanvasTextHorizontalAlign(SnowTextHorizontalAlign align);
SnowTextHorizontalAlign toEngineTextHorizontalAlign(SnowCanvasTextHorizontalAlign align);

SnowCanvasTextVerticalAlign toCanvasTextVerticalAlign(SnowTextVerticalAlign align);
SnowTextVerticalAlign toEngineTextVerticalAlign(SnowCanvasTextVerticalAlign align);

SnowCanvasFillStyle toCanvasFillStyle(SnowFillStyle fillStyle);
SnowFillStyle toEngineFillStyle(SnowCanvasFillStyle fillStyle);

SnowCanvasStrokeStyle toCanvasStrokeStyle(SnowStrokeStyle strokeStyle);
SnowStrokeStyle toEngineStrokeStyle(SnowCanvasStrokeStyle strokeStyle);

SnowCanvasCornerRadii toCanvasCornerRadii(const SnowCornerRadii& cornerRadii);
SnowCornerRadii toEngineCornerRadii(const SnowCanvasCornerRadii& cornerRadii);

SnowCanvasShapeStyle toCanvasShapeStyle(const SnowShapeStyle& style);
SnowShapeStyle toEngineShapeStyle(const SnowCanvasShapeStyle& style);

SnowCanvasTextStyle toCanvasTextStyle(const SnowTextStyle& style);
SnowTextStyle toEngineTextStyle(const SnowCanvasTextStyle& style);

SnowCanvasSerialNumberStyle toCanvasSerialNumberStyle(const SnowSerialNumberStyle& style);
SnowSerialNumberStyle toEngineSerialNumberStyle(const SnowCanvasSerialNumberStyle& style);

SnowCanvasStyleToolbarState toCanvasStyleToolbarState(const SnowStyleToolbarState& state);

SnowCanvasHistoryState toCanvasHistoryState(const SnowHistoryState& state);

SnowCanvasSnapConfig toCanvasSnapConfig(const SnowSnapConfig& config);
SnowSnapConfig toEngineSnapConfig(const SnowCanvasSnapConfig& config);

SnowCanvasGridConfig toCanvasGridConfig(const SnowGridConfig& config);
SnowGridConfig toEngineGridConfig(const SnowCanvasGridConfig& config);

SnowCanvasSpotlightConfig toCanvasSpotlightConfig(const SnowSpotlightConfig& config);
SnowSpotlightConfig toEngineSpotlightConfig(const SnowCanvasSpotlightConfig& config);

bool toEngineStyleDefaults(const SnowCanvasStyleDefaults& defaults,
                           SnowStyleDefaults& engineDefaults);

} // namespace snow_canvas_types

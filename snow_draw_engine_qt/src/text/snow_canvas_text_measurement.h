#pragma once

#include "snow_draw_engine.h"

#include <QFont>

#include <cstdint>
#include <vector>

namespace snow_canvas_text_measurement {

struct TextLayoutOverrideMeasurement {
    bool success = true;
    std::vector<SnowTextLayoutOverride> layouts;
};

struct SelectedTextLayoutMeasurementRequest {
    SnowRuntime runtime = nullptr;
    SnowViewport viewport = nullptr;
    SnowTextStyle style{};
    QFont baseFont;
};

struct ResizeLayoutMeasurementRequest {
    SnowTextElementInfo info{};
    QFont baseFont;
    double zoom = 1.0;
};

TextLayoutOverrideMeasurement measureAutoResizeLayoutOverrides(const SnowTextElementInfo* infos,
                                                               std::uint32_t infoCount,
                                                               const SnowTextStyle& style,
                                                               const QFont& baseFont);
TextLayoutOverrideMeasurement
measureSelectedAutoResizeLayoutOverrides(const SelectedTextLayoutMeasurementRequest& request);
SnowTextLayoutSize measureEmptyDraftLayout(const SnowTextStyle& style, const QFont& baseFont);
SnowTextLayoutSize measureSerialLabelLayout(const SnowSerialLabelLayoutRequest& request,
                                            const QFont& baseFont);
SnowTextLayoutSize
measureSerialNumberBoundTextLayout(const SnowTextStyle& textStyle,
                                   const SnowSerialNumberStyle& serialNumberStyle,
                                   const QFont& baseFont);
SnowTextLayoutSize measureResizeLayout(const ResizeLayoutMeasurementRequest& request);
double steppedFontSize(double current, bool increase);

} // namespace snow_canvas_text_measurement

#pragma once

#include "snow_draw_engine.h"
#include "snow_canvas_text_layout.h"

#include <QFont>
#include <QByteArray>
#include <QCache>

#include <cstdint>
#include <vector>

namespace snow_canvas_text_measurement {

// Natural metrics depend on typography, not on the arrow's current wrap width.
// Costs include the key's text/font bytes so very large labels cannot grow the
// cache without bound. Wrapped layouts remain measured at the exact live width.
class NaturalTextLayoutCache {
  public:
    explicit NaturalTextLayoutCache(int maximumBytes = 1024 * 1024);
    snow_canvas_text_layout::TextMeasuredLayout measure(const QString& text, const QFont& baseFont,
                                                        const SnowSceneDisplayItem& item);
    void clear();
    std::uint64_t measurementCount() const;

  private:
    QCache<QByteArray, snow_canvas_text_layout::TextMeasuredLayout> m_layouts;
    std::uint64_t m_measurementCount = 0;
};

struct TextLayoutOverrideMeasurement {
    bool success = true;
    std::vector<SnowTextLayoutOverride> layouts;
};

struct SelectedTextLayoutMeasurementRequest {
    SnowRuntime runtime = nullptr;
    SnowViewport viewport = nullptr;
    SnowTextStyle style{};
    QFont baseFont;
    std::uint32_t properties = SNOW_TEXT_STYLE_ALL_PROPERTIES;
};

struct ResizeLayoutMeasurementRequest {
    SnowTextElementInfo info{};
    QFont baseFont;
    double zoom = 1.0;
};

TextLayoutOverrideMeasurement
measureAutoResizeLayoutOverrides(const SnowTextElementInfo* infos, std::uint32_t infoCount,
                                 const SnowTextStyle& style, const QFont& baseFont,
                                 std::uint32_t properties = SNOW_TEXT_STYLE_ALL_PROPERTIES);
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

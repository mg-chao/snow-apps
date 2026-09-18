#include "snow_canvas_text_measurement.h"

#include "snow_canvas_text.h"
#include "snow_canvas_text_layout.h"
#include "snow_canvas_utf8.h"

#include <QSizeF>
#include <QString>

#include <algorithm>
#include <cstdint>

namespace snow_canvas_text_measurement {
namespace {

SnowCanvasSceneItem previewItemForStyle(const SnowTextElementInfo& info,
                                        const SnowTextStyle& style) {
    SnowCanvasSceneItem item = snow_canvas_text::defaultPreviewItem(info);
    snow_canvas_text::applyTextStyleToSceneItem(item, style);
    return item;
}

} // namespace

TextLayoutOverrideMeasurement
measureSelectedAutoResizeLayoutOverrides(const SelectedTextLayoutMeasurementRequest& request) {
    TextLayoutOverrideMeasurement result;
    if (request.runtime == nullptr || request.viewport == nullptr) {
        result.success = false;
        return result;
    }

    std::uint32_t count = 0;
    if (snow_viewport_get_selected_text_elements(request.runtime, request.viewport, nullptr, 0,
                                                 &count) != SNOW_OK) {
        result.success = false;
        return result;
    }
    if (count == 0) {
        return result;
    }

    std::vector<SnowTextElementInfo> infos(count);
    std::uint32_t writtenCount = 0;
    if (snow_viewport_get_selected_text_elements(request.runtime, request.viewport, infos.data(),
                                                 count, &writtenCount) != SNOW_OK) {
        result.success = false;
        return result;
    }

    return measureAutoResizeLayoutOverrides(infos.data(), qMin(count, writtenCount), request.style,
                                            request.baseFont);
}

TextLayoutOverrideMeasurement measureAutoResizeLayoutOverrides(const SnowTextElementInfo* infos,
                                                               std::uint32_t infoCount,
                                                               const SnowTextStyle& style,
                                                               const QFont& baseFont) {
    TextLayoutOverrideMeasurement result;
    if (infos == nullptr && infoCount != 0) {
        result.success = false;
        return result;
    }

    result.layouts.reserve(infoCount);
    for (std::uint32_t index = 0; index < infoCount; ++index) {
        const SnowTextElementInfo& info = infos[index];
        if (info.auto_resize == 0) {
            continue;
        }

        SnowCanvasSceneItem item = previewItemForStyle(info, style);
        const QSizeF size = snow_canvas_text_layout::measureNaturalText(
            snow_canvas_text::textFromSceneItem(item), baseFont, item);
        result.layouts.push_back(SnowTextLayoutOverride{
            info.id,
            SnowTextLayoutSize{size.width(), size.height()},
        });
    }
    return result;
}

SnowTextLayoutSize measureEmptyDraftLayout(const SnowTextStyle& style, const QFont& baseFont) {
    SnowTextElementInfo info{};
    info.font_size = snow_canvas_text::resolvedTextFontSize(style.font_size);
    info.auto_resize = 1;
    SnowCanvasSceneItem item = previewItemForStyle(info, style);
    const QSizeF size = snow_canvas_text_layout::measureNaturalText(QString(), baseFont, item);
    return SnowTextLayoutSize{size.width(), size.height()};
}

SnowTextLayoutSize measureSerialLabelLayout(const SnowSerialLabelLayoutRequest& request,
                                            const QFont& baseFont) {
    SnowTextStyle style{};
    style.font_size = request.font_size;
    const QString fontFamily = snow_canvas_utf8::stringFromField(
        request.font_family_utf8, request.font_family_utf8_len, SNOW_FONT_FAMILY_UTF8_CAPACITY);
    snow_canvas_utf8::copyStringToField(fontFamily.trimmed(), style.font_family_utf8,
                                        style.font_family_utf8_len, style.font_family_truncated,
                                        SNOW_FONT_FAMILY_UTF8_CAPACITY);
    return measureEmptyDraftLayout(style, baseFont);
}

SnowTextLayoutSize
measureSerialNumberBoundTextLayout(const SnowTextStyle& textStyle,
                                   const SnowSerialNumberStyle& serialNumberStyle,
                                   const QFont& baseFont) {
    SnowTextStyle boundTextStyle = textStyle;
    boundTextStyle.font_size = serialNumberStyle.font_size;
    return measureEmptyDraftLayout(boundTextStyle, baseFont);
}

SnowTextLayoutSize measureResizeLayout(const ResizeLayoutMeasurementRequest& request) {
    SnowCanvasSceneItem item = snow_canvas_text::defaultPreviewItem(request.info);
    const QString text = snow_canvas_text::textFromSceneItem(item);
    const double zoom = qMax(0.0001, request.zoom);
    if (request.info.measure_natural_width != 0) {
        const QSizeF size =
            snow_canvas_text_layout::measureNaturalText(text, request.baseFont, item, zoom);
        return SnowTextLayoutSize{
            qMax(1.0, size.width()),
            qMax(1.0, size.height()),
        };
    }

    const double measuredWidth =
        qMax(request.info.width,
             snow_canvas_text_layout::measureMinimumWrappedWidth(request.baseFont, item, zoom));
    const QSizeF size = snow_canvas_text_layout::measureWrappedText(text, request.baseFont, item,
                                                                    measuredWidth, zoom);
    return SnowTextLayoutSize{
        qMax(1.0, measuredWidth),
        qMax(1.0, size.height()),
    };
}

double steppedFontSize(double current, bool increase) {
    constexpr double kMaximumTextFontSize = 256.0;
    const double resolvedCurrent = snow_canvas_text::resolvedTextFontSize(current);
    return std::clamp(resolvedCurrent + (increase ? 1.0 : -1.0),
                      snow_canvas_text::minimumTextFontSize(), kMaximumTextFontSize);
}

} // namespace snow_canvas_text_measurement

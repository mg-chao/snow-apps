#pragma once

#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <variant>
#include <type_traits>

namespace snow_canvas_style_limits {
inline constexpr double minimumFontSize = 6.0;
inline constexpr double maximumTextFontSize = 256.0;
inline constexpr double maximumBadgeFontSize = 512.0;
inline constexpr double maximumWatermarkFontSize = 512.0;
} // namespace snow_canvas_style_limits

// Explicit user intent. Display synchronization and programmatic setters do not create edits.
enum SnowCanvasWatermarkProperty : quint32 {
    SnowCanvasWatermarkColor = 1u << 0,
    SnowCanvasWatermarkText = 1u << 1,
    SnowCanvasWatermarkTemplate = 1u << 2,
    SnowCanvasWatermarkFontSize = 1u << 3,
    SnowCanvasWatermarkFontFamily = 1u << 4,
    SnowCanvasWatermarkAngle = 1u << 5,
    SnowCanvasWatermarkGap = 1u << 6,
    SnowCanvasWatermarkOpacity = 1u << 7,
};
enum SnowCanvasSpotlightProperty : quint32 {
    SnowCanvasSpotlightColor = 1u << 0,
    SnowCanvasSpotlightOpacity = 1u << 1,
};

struct SnowCanvasShapeEdit {
    SnowCanvasShapeStyle style;
    quint32 properties = 0;
    SnowCanvasShapeKind kind = SnowCanvasShapeKind::Rectangle;
};
struct SnowCanvasTextEdit {
    SnowCanvasTextStyle style;
    quint32 properties = 0;
};
struct SnowCanvasSerialNumberEdit {
    SnowCanvasSerialNumberStyle style;
    quint32 properties = 0;
};
struct SnowCanvasFilterEdit {
    SnowCanvasFilterStyle style;
    quint32 properties = 0;
    bool pen = false;
};
struct SnowCanvasWatermarkEdit {
    SnowCanvasWatermarkConfig style;
    quint32 properties = 0;
};
struct SnowCanvasSpotlightEdit {
    SnowCanvasSpotlightConfig style;
    quint32 properties = 0;
};
struct SnowCanvasBrushEraserEdit {
    SnowCanvasBrushEraserStyle style;
    quint32 properties = SnowCanvasBrushEraserStylePropertyStrokeWidth;
};
struct SnowCanvasDistanceEdit {
    SnowCanvasDistanceStyle style;
    quint32 properties = 0;
};
using SnowCanvasStyleEdit =
    std::variant<SnowCanvasShapeEdit, SnowCanvasTextEdit, SnowCanvasSerialNumberEdit,
                 SnowCanvasFilterEdit, SnowCanvasWatermarkEdit, SnowCanvasSpotlightEdit,
                 SnowCanvasBrushEraserEdit, SnowCanvasDistanceEdit>;

inline void snowCanvasMergeStyle(SnowCanvasDistanceStyle& target,
                                 const SnowCanvasDistanceStyle& value, quint32 properties) {
    if (properties & SnowCanvasDistanceStylePropertyStrokeColor)
        target.stroke = value.stroke;
    if (properties & SnowCanvasDistanceStylePropertyStrokeWidth)
        target.strokeWidth = value.strokeWidth;
    if (properties & SnowCanvasDistanceStylePropertyFactor)
        target.factor = value.factor;
    if (properties & SnowCanvasDistanceStylePropertyUnit)
        target.unit = value.unit;
    if (properties & SnowCanvasDistanceStylePropertyDecimalPlaces)
        target.decimalPlaces = value.decimalPlaces;
    if (properties & SnowCanvasDistanceStylePropertyEndpointScale)
        target.endpointScale = value.endpointScale;
    if (properties & SnowCanvasDistanceStylePropertyEndpointStyle)
        target.endpointStyle = value.endpointStyle;
}

inline void snowCanvasMergeStyle(SnowCanvasShapeStyle& target, const SnowCanvasShapeStyle& value,
                                 quint32 properties) {
    if ((properties & SnowCanvasShapeStylePropertyFillColor) != 0)
        target.fill = value.fill;
    if ((properties & SnowCanvasShapeStylePropertyFillStyle) != 0)
        target.fillStyle = value.fillStyle;
    if ((properties & SnowCanvasShapeStylePropertyStrokeColor) != 0)
        target.stroke = value.stroke;
    if ((properties & SnowCanvasShapeStylePropertyStrokeWidth) != 0)
        target.strokeWidth = value.strokeWidth;
    if ((properties & SnowCanvasShapeStylePropertyCornerRadius) != 0)
        target.cornerRadii = value.cornerRadii;
    if ((properties & SnowCanvasShapeStylePropertyStartArrowhead) != 0)
        target.startArrowhead = value.startArrowhead;
    if ((properties & SnowCanvasShapeStylePropertyEndArrowhead) != 0)
        target.endArrowhead = value.endArrowhead;
    if ((properties & SnowCanvasShapeStylePropertyStrokeStyle) != 0)
        target.strokeStyle = value.strokeStyle;
    if ((properties & SnowCanvasShapeStylePropertyArrowShaftType) != 0)
        target.arrowShaftType = value.arrowShaftType;
    if ((properties & SnowCanvasShapeStylePropertyArrowRatio) != 0)
        target.arrowRatio = value.arrowRatio;
    if ((properties & SnowCanvasShapeStylePropertyArrowType) != 0)
        target.arrowType = value.arrowType;
    if ((properties & SnowCanvasShapeStylePropertyOpacity) != 0)
        target.opacity = value.opacity;
    if ((properties & SnowCanvasShapeStylePropertyShape) != 0)
        target.shape = value.shape;
}

inline void snowCanvasMergeStyle(SnowCanvasTextStyle& target, const SnowCanvasTextStyle& value,
                                 quint32 properties) {
    if ((properties & SnowCanvasTextStyleMixedColor) != 0)
        target.color = value.color;
    if ((properties & SnowCanvasTextStyleMixedFontSize) != 0)
        target.fontSize = value.fontSize;
    if ((properties & SnowCanvasTextStyleMixedFontFamily) != 0)
        target.fontFamily = value.fontFamily;
    if ((properties & SnowCanvasTextStyleMixedFill) != 0)
        target.fill = value.fill;
    if ((properties & SnowCanvasTextStyleMixedFillStyle) != 0)
        target.fillStyle = value.fillStyle;
    if ((properties & SnowCanvasTextStyleMixedStroke) != 0)
        target.stroke = value.stroke;
    if ((properties & SnowCanvasTextStyleMixedStrokeWidth) != 0)
        target.strokeWidth = value.strokeWidth;
    if ((properties & SnowCanvasTextStyleMixedCornerRadii) != 0)
        target.cornerRadii = value.cornerRadii;
    if ((properties & SnowCanvasTextStyleMixedHorizontalAlign) != 0)
        target.horizontalAlign = value.horizontalAlign;
    if ((properties & SnowCanvasTextStyleMixedVerticalAlign) != 0)
        target.verticalAlign = value.verticalAlign;
    if ((properties & SnowCanvasTextStyleMixedOpacity) != 0)
        target.opacity = value.opacity;
}

inline void snowCanvasMergeStyle(SnowCanvasSerialNumberStyle& target,
                                 const SnowCanvasSerialNumberStyle& value, quint32 properties) {
    if ((properties & SnowCanvasSerialNumberStyleMixedNumber) != 0)
        target.number = value.number;
    if ((properties & SnowCanvasSerialNumberStyleMixedColor) != 0)
        target.color = value.color;
    if ((properties & SnowCanvasSerialNumberStyleMixedFill) != 0)
        target.fill = value.fill;
    if ((properties & SnowCanvasSerialNumberStyleMixedFillStyle) != 0)
        target.fillStyle = value.fillStyle;
    if ((properties & SnowCanvasSerialNumberStyleMixedFontSize) != 0)
        target.fontSize = value.fontSize;
    if ((properties & SnowCanvasSerialNumberStyleMixedFontFamily) != 0)
        target.fontFamily = value.fontFamily;
    if ((properties & SnowCanvasSerialNumberStyleMixedStrokeWidth) != 0)
        target.strokeWidth = value.strokeWidth;
    if ((properties & SnowCanvasSerialNumberStyleMixedStrokeStyle) != 0)
        target.strokeStyle = value.strokeStyle;
    if ((properties & SnowCanvasSerialNumberStyleMixedOpacity) != 0)
        target.opacity = value.opacity;
    if ((properties & SnowCanvasSerialNumberStyleMixedNumericType) != 0)
        target.numericType = value.numericType;
    if ((properties & SnowCanvasSerialNumberStyleMixedType) != 0)
        target.type = value.type;
}

inline void snowCanvasMergeStyle(SnowCanvasFilterStyle& target, const SnowCanvasFilterStyle& value,
                                 quint32 properties) {
    if ((properties & SnowCanvasFilterStylePropertyType) != 0)
        target.type = value.type;
    if ((properties & SnowCanvasFilterStylePropertyStrength) != 0)
        target.strength = value.strength;
    if ((properties & SnowCanvasFilterStylePropertyOpacity) != 0)
        target.opacity = value.opacity;
    if ((properties & SnowCanvasFilterStylePropertyStrokeWidth) != 0)
        target.strokeWidth = value.strokeWidth;
}

inline void snowCanvasMergeStyle(SnowCanvasWatermarkConfig& target,
                                 const SnowCanvasWatermarkConfig& value, quint32 properties) {
    if ((properties & SnowCanvasWatermarkColor) != 0)
        target.color = value.color;
    if ((properties & SnowCanvasWatermarkText) != 0)
        target.text = value.text;
    if ((properties & SnowCanvasWatermarkTemplate) != 0)
        target.templateValue = value.templateValue;
    if ((properties & SnowCanvasWatermarkTemplate) != 0)
        target.templateApplicationTime = value.templateApplicationTime;
    if ((properties & SnowCanvasWatermarkFontSize) != 0)
        target.fontSize = value.fontSize;
    if ((properties & SnowCanvasWatermarkFontFamily) != 0)
        target.fontFamily = value.fontFamily;
    if ((properties & SnowCanvasWatermarkAngle) != 0)
        target.angle = value.angle;
    if ((properties & SnowCanvasWatermarkGap) != 0)
        target.gap = value.gap;
    if ((properties & SnowCanvasWatermarkOpacity) != 0)
        target.opacity = value.opacity;
}

inline void snowCanvasMergeStyle(SnowCanvasSpotlightConfig& target,
                                 const SnowCanvasSpotlightConfig& value, quint32 properties) {
    if ((properties & SnowCanvasSpotlightColor) != 0)
        target.color = value.color;
    if ((properties & SnowCanvasSpotlightOpacity) != 0)
        target.opacity = value.opacity;
}

inline void snowCanvasMergeStyle(SnowCanvasBrushEraserStyle& target,
                                 const SnowCanvasBrushEraserStyle& value, quint32 properties) {
    if ((properties & SnowCanvasBrushEraserStylePropertyStrokeWidth) != 0)
        target.strokeWidth = value.strokeWidth;
}

inline void snowCanvasMergeStyleEdit(SnowCanvasStyleDefaults& defaults,
                                     const SnowCanvasStyleEdit& edit) {
    std::visit(
        [&](const auto& patch) {
            using T = std::decay_t<decltype(patch)>;
            if constexpr (std::is_same_v<T, SnowCanvasShapeEdit>) {
                SnowCanvasShapeStyle* target = nullptr;
                switch (patch.kind) {
                case SnowCanvasShapeKind::Rectangle:
                    target = &defaults.rectangle;
                    break;
                case SnowCanvasShapeKind::Arrow:
                    target = &defaults.arrow;
                    break;
                case SnowCanvasShapeKind::Line:
                    target = &defaults.line;
                    break;
                case SnowCanvasShapeKind::FreeDraw:
                    target = &defaults.freeDraw;
                    break;
                case SnowCanvasShapeKind::RectangleHighlight:
                    target = &defaults.rectangleHighlight;
                    break;
                case SnowCanvasShapeKind::PenHighlight:
                    target = &defaults.penHighlight;
                    break;
                case SnowCanvasShapeKind::Spotlight:
                    if ((patch.properties & SnowCanvasShapeStylePropertyShape) != 0)
                        defaults.spotlightShape = patch.style.shape;
                    break;
                }
                if (target != nullptr)
                    snowCanvasMergeStyle(*target, patch.style, patch.properties);
            } else if constexpr (std::is_same_v<T, SnowCanvasDistanceEdit>) {
                snowCanvasMergeStyle(defaults.distance, patch.style, patch.properties);
            } else if constexpr (std::is_same_v<T, SnowCanvasTextEdit>) {
                snowCanvasMergeStyle(defaults.text, patch.style, patch.properties);
            } else if constexpr (std::is_same_v<T, SnowCanvasSerialNumberEdit>) {
                snowCanvasMergeStyle(defaults.serialNumber, patch.style, patch.properties);
            } else if constexpr (std::is_same_v<T, SnowCanvasFilterEdit>) {
                snowCanvasMergeStyle(patch.pen ? defaults.penFilter : defaults.rectangleFilter,
                                     patch.style, patch.properties);
                if ((patch.properties & SnowCanvasFilterStylePropertyStrength) != 0) {
                    defaults.penFilter.strength = patch.style.strength;
                    defaults.rectangleFilter.strength = patch.style.strength;
                }
            } else if constexpr (std::is_same_v<T, SnowCanvasWatermarkEdit>) {
                snowCanvasMergeStyle(defaults.watermark, patch.style, patch.properties);
            } else if constexpr (std::is_same_v<T, SnowCanvasSpotlightEdit>) {
                snowCanvasMergeStyle(defaults.spotlight, patch.style, patch.properties);
            } else if constexpr (std::is_same_v<T, SnowCanvasBrushEraserEdit>) {
                snowCanvasMergeStyle(defaults.brushEraser, patch.style, patch.properties);
            }
        },
        edit);
}

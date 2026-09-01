#include "snow_canvas_type_conversions.h"

#include "snow_canvas_utf8.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <type_traits>

namespace snow_canvas_types {
namespace {

std::uint8_t toFlag(bool value) {
    return value ? 1 : 0;
}

bool fromFlag(std::uint8_t value) {
    return value != 0;
}

QString fontFamilyFromEngine(const char* utf8, std::uint32_t length, std::uint32_t capacity) {
    return snow_canvas_utf8::stringFromField(utf8, length, capacity);
}

void copyFontFamilyToEngine(const QString& fontFamily, char* target, std::uint32_t& length,
                            std::uint8_t& truncated, std::uint32_t capacity) {
    snow_canvas_utf8::copyStringToField(fontFamily.trimmed(), target, length, truncated, capacity);
}

template <typename Enum> bool enumInRange(Enum value, Enum first, Enum last) {
    using Underlying = std::underlying_type_t<Enum>;
    const Underlying raw = static_cast<Underlying>(value);
    return raw >= static_cast<Underlying>(first) && raw <= static_cast<Underlying>(last);
}

bool validShapeStyleEnums(const SnowCanvasShapeStyle& style) {
    return enumInRange(style.fillStyle, SnowCanvasFillStyle::Line, SnowCanvasFillStyle::Solid) &&
           enumInRange(style.startArrowhead, SnowCanvasArrowhead::None,
                       SnowCanvasArrowhead::CrowfootOneOrMany) &&
           enumInRange(style.endArrowhead, SnowCanvasArrowhead::None,
                       SnowCanvasArrowhead::CrowfootOneOrMany) &&
           enumInRange(style.strokeStyle, SnowCanvasStrokeStyle::Solid,
                       SnowCanvasStrokeStyle::Dotted) &&
           enumInRange(style.arrowType, SnowCanvasArrowType::Straight,
                       SnowCanvasArrowType::Elbow) &&
           enumInRange(style.highlightShape, SnowCanvasHighlightShape::Rectangle,
                       SnowCanvasHighlightShape::Ellipse) &&
           enumInRange(style.shape, SnowCanvasRectangleShape::Rectangle,
                       SnowCanvasRectangleShape::Diamond);
}

} // namespace

QColor toQColor(const SnowColorRgba8& color) {
    return QColor(color.r, color.g, color.b, color.a);
}

SnowColorRgba8 toEngineColor(const QColor& color) {
    const QColor rgba = color.isValid() ? color.toRgb() : QColor(0, 0, 0, 0);
    return SnowColorRgba8{
        static_cast<std::uint8_t>(rgba.red()),
        static_cast<std::uint8_t>(rgba.green()),
        static_cast<std::uint8_t>(rgba.blue()),
        static_cast<std::uint8_t>(rgba.alpha()),
    };
}

SnowCanvasTool toCanvasTool(SnowActiveTool tool) {
    switch (tool) {
    case SNOW_ACTIVE_TOOL_SELECT:
        return SnowCanvasTool::Select;
    case SNOW_ACTIVE_TOOL_SHAPE:
        return SnowCanvasTool::Shape;
    case SNOW_ACTIVE_TOOL_ARROW:
        return SnowCanvasTool::Arrow;
    case SNOW_ACTIVE_TOOL_LINE:
        return SnowCanvasTool::Line;
    case SNOW_ACTIVE_TOOL_FREE_DRAW:
        return SnowCanvasTool::FreeDraw;
    case SNOW_ACTIVE_TOOL_RECTANGLE_HIGHLIGHT:
        return SnowCanvasTool::RectangleHighlight;
    case SNOW_ACTIVE_TOOL_PEN_HIGHLIGHT:
        return SnowCanvasTool::PenHighlight;
    case SNOW_ACTIVE_TOOL_ERASER:
        return SnowCanvasTool::Eraser;
    case SNOW_ACTIVE_TOOL_RECTANGLE_FILTER:
        return SnowCanvasTool::RectangleFilter;
    case SNOW_ACTIVE_TOOL_PEN_FILTER:
        return SnowCanvasTool::PenFilter;
    case SNOW_ACTIVE_TOOL_WATERMARK:
        return SnowCanvasTool::Watermark;
    case SNOW_ACTIVE_TOOL_TEXT:
        return SnowCanvasTool::Text;
    case SNOW_ACTIVE_TOOL_SERIAL_NUMBER:
        return SnowCanvasTool::SerialNumber;
    case SNOW_ACTIVE_TOOL_SPOTLIGHT:
        return SnowCanvasTool::Spotlight;
    }
    return SnowCanvasTool::Select;
}

SnowActiveTool toEngineTool(SnowCanvasTool tool) {
    switch (tool) {
    case SnowCanvasTool::Select:
        return SNOW_ACTIVE_TOOL_SELECT;
    case SnowCanvasTool::Shape:
        return SNOW_ACTIVE_TOOL_SHAPE;
    case SnowCanvasTool::Arrow:
        return SNOW_ACTIVE_TOOL_ARROW;
    case SnowCanvasTool::Line:
        return SNOW_ACTIVE_TOOL_LINE;
    case SnowCanvasTool::FreeDraw:
        return SNOW_ACTIVE_TOOL_FREE_DRAW;
    case SnowCanvasTool::RectangleHighlight:
        return SNOW_ACTIVE_TOOL_RECTANGLE_HIGHLIGHT;
    case SnowCanvasTool::PenHighlight:
        return SNOW_ACTIVE_TOOL_PEN_HIGHLIGHT;
    case SnowCanvasTool::Eraser:
        return SNOW_ACTIVE_TOOL_ERASER;
    case SnowCanvasTool::RectangleFilter:
        return SNOW_ACTIVE_TOOL_RECTANGLE_FILTER;
    case SnowCanvasTool::PenFilter:
        return SNOW_ACTIVE_TOOL_PEN_FILTER;
    case SnowCanvasTool::Watermark:
        return SNOW_ACTIVE_TOOL_WATERMARK;
    case SnowCanvasTool::Text:
        return SNOW_ACTIVE_TOOL_TEXT;
    case SnowCanvasTool::SerialNumber:
        return SNOW_ACTIVE_TOOL_SERIAL_NUMBER;
    case SnowCanvasTool::Spotlight:
        return SNOW_ACTIVE_TOOL_SPOTLIGHT;
    }
    return SNOW_ACTIVE_TOOL_SELECT;
}

SnowCanvasStyleToolbarSource toCanvasStyleToolbarSource(SnowStyleToolbarSource source) {
    switch (source) {
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_RECTANGLE:
        return SnowCanvasStyleToolbarSource::DefaultRectangle;
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_RECTANGLE:
        return SnowCanvasStyleToolbarSource::SelectedRectangle;
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_ARROW:
        return SnowCanvasStyleToolbarSource::DefaultArrow;
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_ARROW:
        return SnowCanvasStyleToolbarSource::SelectedArrow;
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_LINE:
        return SnowCanvasStyleToolbarSource::DefaultLine;
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_LINE:
        return SnowCanvasStyleToolbarSource::SelectedLine;
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_FREE_DRAW:
        return SnowCanvasStyleToolbarSource::DefaultFreeDraw;
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_FREE_DRAW:
        return SnowCanvasStyleToolbarSource::SelectedFreeDraw;
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_RECTANGLE_HIGHLIGHT:
        return SnowCanvasStyleToolbarSource::DefaultRectangleHighlight;
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_RECTANGLE_HIGHLIGHT:
        return SnowCanvasStyleToolbarSource::SelectedRectangleHighlight;
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_PEN_HIGHLIGHT:
        return SnowCanvasStyleToolbarSource::DefaultPenHighlight;
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_PEN_HIGHLIGHT:
        return SnowCanvasStyleToolbarSource::SelectedPenHighlight;
    case SNOW_STYLE_TOOLBAR_SOURCE_ERASER:
        return SnowCanvasStyleToolbarSource::Eraser;
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_RECTANGLE_FILTER:
        return SnowCanvasStyleToolbarSource::DefaultRectangleFilter;
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_RECTANGLE_FILTER:
        return SnowCanvasStyleToolbarSource::SelectedRectangleFilter;
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_PEN_FILTER:
        return SnowCanvasStyleToolbarSource::DefaultPenFilter;
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_PEN_FILTER:
        return SnowCanvasStyleToolbarSource::SelectedPenFilter;
    case SNOW_STYLE_TOOLBAR_SOURCE_WATERMARK:
        return SnowCanvasStyleToolbarSource::Watermark;
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_TEXT:
        return SnowCanvasStyleToolbarSource::DefaultText;
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_TEXT:
        return SnowCanvasStyleToolbarSource::SelectedText;
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_SERIAL_NUMBER:
        return SnowCanvasStyleToolbarSource::DefaultSerialNumber;
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_SERIAL_NUMBER:
        return SnowCanvasStyleToolbarSource::SelectedSerialNumber;
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_SPOTLIGHT:
        return SnowCanvasStyleToolbarSource::DefaultSpotlight;
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_SPOTLIGHT:
        return SnowCanvasStyleToolbarSource::SelectedSpotlight;
    }
    return SnowCanvasStyleToolbarSource::DefaultRectangle;
}

SnowShapeKind toEngineShapeKind(SnowCanvasShapeKind kind) {
    switch (kind) {
    case SnowCanvasShapeKind::Rectangle:
        return SNOW_SHAPE_KIND_RECTANGLE;
    case SnowCanvasShapeKind::Arrow:
        return SNOW_SHAPE_KIND_ARROW;
    case SnowCanvasShapeKind::Line:
        return SNOW_SHAPE_KIND_LINE;
    case SnowCanvasShapeKind::FreeDraw:
        return SNOW_SHAPE_KIND_FREE_DRAW;
    case SnowCanvasShapeKind::RectangleHighlight:
        return SNOW_SHAPE_KIND_RECTANGLE_HIGHLIGHT;
    case SnowCanvasShapeKind::PenHighlight:
        return SNOW_SHAPE_KIND_PEN_HIGHLIGHT;
    case SnowCanvasShapeKind::Spotlight:
        return SNOW_SHAPE_KIND_SPOTLIGHT;
    }
    return SNOW_SHAPE_KIND_RECTANGLE;
}

SnowCanvasArrowhead toCanvasArrowhead(SnowArrowhead arrowhead) {
    switch (arrowhead) {
    case SNOW_ARROWHEAD_NONE:
        return SnowCanvasArrowhead::None;
    case SNOW_ARROWHEAD_ARROW:
        return SnowCanvasArrowhead::Arrow;
    case SNOW_ARROWHEAD_BAR:
        return SnowCanvasArrowhead::Bar;
    case SNOW_ARROWHEAD_DOT:
        return SnowCanvasArrowhead::Dot;
    case SNOW_ARROWHEAD_CIRCLE:
        return SnowCanvasArrowhead::Circle;
    case SNOW_ARROWHEAD_CIRCLE_OUTLINE:
        return SnowCanvasArrowhead::CircleOutline;
    case SNOW_ARROWHEAD_TRIANGLE:
        return SnowCanvasArrowhead::Triangle;
    case SNOW_ARROWHEAD_TRIANGLE_OUTLINE:
        return SnowCanvasArrowhead::TriangleOutline;
    case SNOW_ARROWHEAD_DIAMOND:
        return SnowCanvasArrowhead::Diamond;
    case SNOW_ARROWHEAD_DIAMOND_OUTLINE:
        return SnowCanvasArrowhead::DiamondOutline;
    case SNOW_ARROWHEAD_CROWFOOT_ONE:
        return SnowCanvasArrowhead::CrowfootOne;
    case SNOW_ARROWHEAD_CROWFOOT_MANY:
        return SnowCanvasArrowhead::CrowfootMany;
    case SNOW_ARROWHEAD_CROWFOOT_ONE_OR_MANY:
        return SnowCanvasArrowhead::CrowfootOneOrMany;
    }
    return SnowCanvasArrowhead::None;
}

SnowArrowhead toEngineArrowhead(SnowCanvasArrowhead arrowhead) {
    switch (arrowhead) {
    case SnowCanvasArrowhead::None:
        return SNOW_ARROWHEAD_NONE;
    case SnowCanvasArrowhead::Arrow:
        return SNOW_ARROWHEAD_ARROW;
    case SnowCanvasArrowhead::Bar:
        return SNOW_ARROWHEAD_BAR;
    case SnowCanvasArrowhead::Dot:
        return SNOW_ARROWHEAD_DOT;
    case SnowCanvasArrowhead::Circle:
        return SNOW_ARROWHEAD_CIRCLE;
    case SnowCanvasArrowhead::CircleOutline:
        return SNOW_ARROWHEAD_CIRCLE_OUTLINE;
    case SnowCanvasArrowhead::Triangle:
        return SNOW_ARROWHEAD_TRIANGLE;
    case SnowCanvasArrowhead::TriangleOutline:
        return SNOW_ARROWHEAD_TRIANGLE_OUTLINE;
    case SnowCanvasArrowhead::Diamond:
        return SNOW_ARROWHEAD_DIAMOND;
    case SnowCanvasArrowhead::DiamondOutline:
        return SNOW_ARROWHEAD_DIAMOND_OUTLINE;
    case SnowCanvasArrowhead::CrowfootOne:
        return SNOW_ARROWHEAD_CROWFOOT_ONE;
    case SnowCanvasArrowhead::CrowfootMany:
        return SNOW_ARROWHEAD_CROWFOOT_MANY;
    case SnowCanvasArrowhead::CrowfootOneOrMany:
        return SNOW_ARROWHEAD_CROWFOOT_ONE_OR_MANY;
    }
    return SNOW_ARROWHEAD_NONE;
}

SnowCanvasArrowType toCanvasArrowType(SnowArrowType arrowType) {
    switch (arrowType) {
    case SNOW_ARROW_TYPE_STRAIGHT:
        return SnowCanvasArrowType::Straight;
    case SNOW_ARROW_TYPE_CURVE:
        return SnowCanvasArrowType::Curve;
    case SNOW_ARROW_TYPE_ELBOW:
        return SnowCanvasArrowType::Elbow;
    }
    return SnowCanvasArrowType::Straight;
}

SnowArrowType toEngineArrowType(SnowCanvasArrowType arrowType) {
    switch (arrowType) {
    case SnowCanvasArrowType::Straight:
        return SNOW_ARROW_TYPE_STRAIGHT;
    case SnowCanvasArrowType::Curve:
        return SNOW_ARROW_TYPE_CURVE;
    case SnowCanvasArrowType::Elbow:
        return SNOW_ARROW_TYPE_ELBOW;
    }
    return SNOW_ARROW_TYPE_STRAIGHT;
}

SnowCanvasTextHorizontalAlign toCanvasTextHorizontalAlign(SnowTextHorizontalAlign align) {
    switch (align) {
    case SNOW_TEXT_HORIZONTAL_ALIGN_LEFT:
        return SnowCanvasTextHorizontalAlign::Left;
    case SNOW_TEXT_HORIZONTAL_ALIGN_CENTER:
        return SnowCanvasTextHorizontalAlign::Center;
    case SNOW_TEXT_HORIZONTAL_ALIGN_RIGHT:
        return SnowCanvasTextHorizontalAlign::Right;
    }
    return SnowCanvasTextHorizontalAlign::Left;
}

SnowTextHorizontalAlign toEngineTextHorizontalAlign(SnowCanvasTextHorizontalAlign align) {
    switch (align) {
    case SnowCanvasTextHorizontalAlign::Left:
        return SNOW_TEXT_HORIZONTAL_ALIGN_LEFT;
    case SnowCanvasTextHorizontalAlign::Center:
        return SNOW_TEXT_HORIZONTAL_ALIGN_CENTER;
    case SnowCanvasTextHorizontalAlign::Right:
        return SNOW_TEXT_HORIZONTAL_ALIGN_RIGHT;
    }
    return SNOW_TEXT_HORIZONTAL_ALIGN_LEFT;
}

SnowCanvasTextVerticalAlign toCanvasTextVerticalAlign(SnowTextVerticalAlign align) {
    switch (align) {
    case SNOW_TEXT_VERTICAL_ALIGN_TOP:
        return SnowCanvasTextVerticalAlign::Top;
    case SNOW_TEXT_VERTICAL_ALIGN_CENTER:
        return SnowCanvasTextVerticalAlign::Center;
    case SNOW_TEXT_VERTICAL_ALIGN_BOTTOM:
        return SnowCanvasTextVerticalAlign::Bottom;
    }
    return SnowCanvasTextVerticalAlign::Center;
}

SnowTextVerticalAlign toEngineTextVerticalAlign(SnowCanvasTextVerticalAlign align) {
    switch (align) {
    case SnowCanvasTextVerticalAlign::Top:
        return SNOW_TEXT_VERTICAL_ALIGN_TOP;
    case SnowCanvasTextVerticalAlign::Center:
        return SNOW_TEXT_VERTICAL_ALIGN_CENTER;
    case SnowCanvasTextVerticalAlign::Bottom:
        return SNOW_TEXT_VERTICAL_ALIGN_BOTTOM;
    }
    return SNOW_TEXT_VERTICAL_ALIGN_CENTER;
}

SnowCanvasFillStyle toCanvasFillStyle(SnowFillStyle fillStyle) {
    switch (fillStyle) {
    case SNOW_FILL_STYLE_LINE:
        return SnowCanvasFillStyle::Line;
    case SNOW_FILL_STYLE_CROSS_LINE:
        return SnowCanvasFillStyle::CrossLine;
    case SNOW_FILL_STYLE_SOLID:
        return SnowCanvasFillStyle::Solid;
    }
    return SnowCanvasFillStyle::Solid;
}

SnowFillStyle toEngineFillStyle(SnowCanvasFillStyle fillStyle) {
    switch (fillStyle) {
    case SnowCanvasFillStyle::Line:
        return SNOW_FILL_STYLE_LINE;
    case SnowCanvasFillStyle::CrossLine:
        return SNOW_FILL_STYLE_CROSS_LINE;
    case SnowCanvasFillStyle::Solid:
        return SNOW_FILL_STYLE_SOLID;
    }
    return SNOW_FILL_STYLE_SOLID;
}

SnowCanvasStrokeStyle toCanvasStrokeStyle(SnowStrokeStyle strokeStyle) {
    switch (strokeStyle) {
    case SNOW_STROKE_STYLE_SOLID:
        return SnowCanvasStrokeStyle::Solid;
    case SNOW_STROKE_STYLE_DASHED:
        return SnowCanvasStrokeStyle::Dashed;
    case SNOW_STROKE_STYLE_DOTTED:
        return SnowCanvasStrokeStyle::Dotted;
    }
    return SnowCanvasStrokeStyle::Solid;
}

SnowStrokeStyle toEngineStrokeStyle(SnowCanvasStrokeStyle strokeStyle) {
    switch (strokeStyle) {
    case SnowCanvasStrokeStyle::Solid:
        return SNOW_STROKE_STYLE_SOLID;
    case SnowCanvasStrokeStyle::Dashed:
        return SNOW_STROKE_STYLE_DASHED;
    case SnowCanvasStrokeStyle::Dotted:
        return SNOW_STROKE_STYLE_DOTTED;
    }
    return SNOW_STROKE_STYLE_SOLID;
}

SnowCanvasCornerRadii toCanvasCornerRadii(const SnowCornerRadii& cornerRadii) {
    return SnowCanvasCornerRadii{
        cornerRadii.top_left,
        cornerRadii.top_right,
        cornerRadii.bottom_right,
        cornerRadii.bottom_left,
    };
}

SnowCornerRadii toEngineCornerRadii(const SnowCanvasCornerRadii& cornerRadii) {
    return SnowCornerRadii{
        cornerRadii.topLeft,
        cornerRadii.topRight,
        cornerRadii.bottomRight,
        cornerRadii.bottomLeft,
    };
}

SnowCanvasShapeStyle toCanvasShapeStyle(const SnowShapeStyle& style) {
    return SnowCanvasShapeStyle{
        toQColor(style.fill),
        toCanvasFillStyle(style.fill_style),
        toQColor(style.stroke),
        style.stroke_width,
        toCanvasCornerRadii(style.corner_radii),
        toCanvasArrowhead(style.start_arrowhead),
        toCanvasArrowhead(style.end_arrowhead),
        toCanvasStrokeStyle(style.stroke_style),
        toCanvasArrowType(style.arrow_type),
        style.opacity,
        style.highlight_shape == SNOW_HIGHLIGHT_SHAPE_ELLIPSE ? SnowCanvasHighlightShape::Ellipse
                                                              : SnowCanvasHighlightShape::Rectangle,
        style.shape == SNOW_RECTANGLE_SHAPE_ELLIPSE   ? SnowCanvasRectangleShape::Ellipse
        : style.shape == SNOW_RECTANGLE_SHAPE_DIAMOND ? SnowCanvasRectangleShape::Diamond
                                                      : SnowCanvasRectangleShape::Rectangle,
    };
}

SnowShapeStyle toEngineShapeStyle(const SnowCanvasShapeStyle& style) {
    SnowShapeStyle engineStyle{};
    engineStyle.fill = toEngineColor(style.fill);
    engineStyle.fill_style = toEngineFillStyle(style.fillStyle);
    engineStyle.stroke = toEngineColor(style.stroke);
    engineStyle.stroke_width = style.strokeWidth;
    engineStyle.corner_radii = toEngineCornerRadii(style.cornerRadii);
    engineStyle.start_arrowhead = toEngineArrowhead(style.startArrowhead);
    engineStyle.end_arrowhead = toEngineArrowhead(style.endArrowhead);
    engineStyle.stroke_style = toEngineStrokeStyle(style.strokeStyle);
    engineStyle.arrow_type = toEngineArrowType(style.arrowType);
    engineStyle.opacity = style.opacity;
    engineStyle.highlight_shape = style.highlightShape == SnowCanvasHighlightShape::Ellipse
                                      ? SNOW_HIGHLIGHT_SHAPE_ELLIPSE
                                      : SNOW_HIGHLIGHT_SHAPE_RECTANGLE;
    engineStyle.shape =
        style.shape == SnowCanvasRectangleShape::Ellipse   ? SNOW_RECTANGLE_SHAPE_ELLIPSE
        : style.shape == SnowCanvasRectangleShape::Diamond ? SNOW_RECTANGLE_SHAPE_DIAMOND
                                                           : SNOW_RECTANGLE_SHAPE_RECTANGLE;
    return engineStyle;
}

SnowCanvasTextStyle toCanvasTextStyle(const SnowTextStyle& style) {
    return SnowCanvasTextStyle{
        toQColor(style.color),
        style.font_size,
        fontFamilyFromEngine(style.font_family_utf8, style.font_family_utf8_len,
                             SNOW_FONT_FAMILY_UTF8_CAPACITY),
        toQColor(style.fill),
        toCanvasFillStyle(style.fill_style),
        toQColor(style.stroke),
        style.stroke_width,
        toCanvasCornerRadii(style.corner_radii),
        toCanvasTextHorizontalAlign(style.horizontal_align),
        toCanvasTextVerticalAlign(style.vertical_align),
        style.opacity,
    };
}

SnowTextStyle toEngineTextStyle(const SnowCanvasTextStyle& style) {
    SnowTextStyle engineStyle{};
    engineStyle.color = toEngineColor(style.color);
    engineStyle.font_size = style.fontSize;
    copyFontFamilyToEngine(style.fontFamily, engineStyle.font_family_utf8,
                           engineStyle.font_family_utf8_len, engineStyle.font_family_truncated,
                           SNOW_FONT_FAMILY_UTF8_CAPACITY);
    engineStyle.fill = toEngineColor(style.fill);
    engineStyle.fill_style = toEngineFillStyle(style.fillStyle);
    engineStyle.stroke = toEngineColor(style.stroke);
    engineStyle.stroke_width = style.strokeWidth;
    engineStyle.corner_radii = toEngineCornerRadii(style.cornerRadii);
    engineStyle.horizontal_align = toEngineTextHorizontalAlign(style.horizontalAlign);
    engineStyle.vertical_align = toEngineTextVerticalAlign(style.verticalAlign);
    engineStyle.opacity = style.opacity;
    return engineStyle;
}

SnowCanvasSerialNumberStyle toCanvasSerialNumberStyle(const SnowSerialNumberStyle& style) {
    return SnowCanvasSerialNumberStyle{
        static_cast<qint64>(style.number),
        toQColor(style.color),
        toQColor(style.fill),
        toCanvasFillStyle(style.fill_style),
        style.font_size,
        fontFamilyFromEngine(style.font_family_utf8, style.font_family_utf8_len,
                             SNOW_FONT_FAMILY_UTF8_CAPACITY),
        style.stroke_width,
        toCanvasStrokeStyle(style.stroke_style),
        style.opacity,
    };
}

SnowSerialNumberStyle toEngineSerialNumberStyle(const SnowCanvasSerialNumberStyle& style) {
    SnowSerialNumberStyle engineStyle{};
    engineStyle.number = static_cast<std::int64_t>(style.number);
    engineStyle.color = toEngineColor(style.color);
    engineStyle.fill = toEngineColor(style.fill);
    engineStyle.fill_style = toEngineFillStyle(style.fillStyle);
    engineStyle.font_size = style.fontSize;
    copyFontFamilyToEngine(style.fontFamily, engineStyle.font_family_utf8,
                           engineStyle.font_family_utf8_len, engineStyle.font_family_truncated,
                           SNOW_FONT_FAMILY_UTF8_CAPACITY);
    engineStyle.stroke_width = style.strokeWidth;
    engineStyle.stroke_style = toEngineStrokeStyle(style.strokeStyle);
    engineStyle.opacity = style.opacity;
    return engineStyle;
}

SnowCanvasStyleToolbarState toCanvasStyleToolbarState(const SnowStyleToolbarState& state) {
    return SnowCanvasStyleToolbarState{
        toCanvasStyleToolbarSource(state.source),
        toCanvasShapeStyle(state.shape_style),
        toCanvasTextStyle(state.text_style),
        toCanvasSerialNumberStyle(state.serial_number_style),
        state.text_style_mixed,
        state.serial_number_style_mixed,
        state.shape_style_mixed,
        SnowCanvasFilterStyle{
            static_cast<SnowCanvasFilterType>(state.filter_style.filter_type),
            state.filter_style.strength,
            state.filter_style.opacity,
            state.filter_style.stroke_width,
        },
        state.filter_style_mixed,
    };
}

SnowCanvasHistoryState toCanvasHistoryState(const SnowHistoryState& state) {
    return SnowCanvasHistoryState{
        fromFlag(state.can_undo),
        fromFlag(state.can_redo),
    };
}

SnowCanvasSnapConfig toCanvasSnapConfig(const SnowSnapConfig& config) {
    return SnowCanvasSnapConfig{
        fromFlag(config.enabled),
        fromFlag(config.enable_point_snaps),
        fromFlag(config.enable_gap_snaps),
        fromFlag(config.show_guides),
        fromFlag(config.show_gap_size),
        config.distance,
        toQColor(config.line_color),
        config.line_width,
        config.marker_size,
        config.gap_dash_length,
        config.gap_dash_gap,
    };
}

SnowSnapConfig toEngineSnapConfig(const SnowCanvasSnapConfig& config) {
    SnowSnapConfig engineConfig{};
    engineConfig.enabled = toFlag(config.enabled);
    engineConfig.enable_point_snaps = toFlag(config.enablePointSnaps);
    engineConfig.enable_gap_snaps = toFlag(config.enableGapSnaps);
    engineConfig.show_guides = toFlag(config.showGuides);
    engineConfig.show_gap_size = toFlag(config.showGapSize);
    engineConfig.distance = config.distance;
    engineConfig.line_color = toEngineColor(config.lineColor);
    engineConfig.line_width = config.lineWidth;
    engineConfig.marker_size = config.markerSize;
    engineConfig.gap_dash_length = config.gapDashLength;
    engineConfig.gap_dash_gap = config.gapDashGap;
    return engineConfig;
}

SnowCanvasGridConfig toCanvasGridConfig(const SnowGridConfig& config) {
    return SnowCanvasGridConfig{
        fromFlag(config.enabled),
        config.size,
    };
}

SnowGridConfig toEngineGridConfig(const SnowCanvasGridConfig& config) {
    SnowGridConfig engineConfig{};
    engineConfig.enabled = toFlag(config.enabled);
    engineConfig.size = config.size;
    return engineConfig;
}

SnowCanvasSpotlightConfig toCanvasSpotlightConfig(const SnowSpotlightConfig& config) {
    return SnowCanvasSpotlightConfig{toQColor(config.color), config.opacity};
}

SnowSpotlightConfig toEngineSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
    return SnowSpotlightConfig{toEngineColor(config.color), config.opacity};
}

bool toEngineStyleDefaults(const SnowCanvasStyleDefaults& defaults,
                           SnowStyleDefaults& engineDefaults) {
    const SnowCanvasShapeStyle* shapes[] = {
        &defaults.rectangle,          &defaults.arrow,        &defaults.line, &defaults.freeDraw,
        &defaults.rectangleHighlight, &defaults.penHighlight,
    };
    if (std::any_of(
            std::begin(shapes), std::end(shapes),
            [](const SnowCanvasShapeStyle* style) { return !validShapeStyleEnums(*style); }) ||
        !enumInRange(defaults.rectangleFilter.type, SnowCanvasFilterType::Mosaic,
                     SnowCanvasFilterType::Inversion) ||
        !enumInRange(defaults.penFilter.type, SnowCanvasFilterType::Mosaic,
                     SnowCanvasFilterType::Inversion) ||
        !enumInRange(defaults.text.fillStyle, SnowCanvasFillStyle::Line,
                     SnowCanvasFillStyle::Solid) ||
        !enumInRange(defaults.text.horizontalAlign, SnowCanvasTextHorizontalAlign::Left,
                     SnowCanvasTextHorizontalAlign::Right) ||
        !enumInRange(defaults.text.verticalAlign, SnowCanvasTextVerticalAlign::Top,
                     SnowCanvasTextVerticalAlign::Bottom) ||
        !enumInRange(defaults.serialNumber.fillStyle, SnowCanvasFillStyle::Line,
                     SnowCanvasFillStyle::Solid) ||
        !enumInRange(defaults.serialNumber.strokeStyle, SnowCanvasStrokeStyle::Solid,
                     SnowCanvasStrokeStyle::Dotted)) {
        return false;
    }

    const QColor colors[] = {
        defaults.rectangle.fill,
        defaults.rectangle.stroke,
        defaults.arrow.fill,
        defaults.arrow.stroke,
        defaults.line.fill,
        defaults.line.stroke,
        defaults.freeDraw.fill,
        defaults.freeDraw.stroke,
        defaults.rectangleHighlight.fill,
        defaults.rectangleHighlight.stroke,
        defaults.penHighlight.fill,
        defaults.penHighlight.stroke,
        defaults.text.color,
        defaults.text.fill,
        defaults.text.stroke,
        defaults.serialNumber.color,
        defaults.serialNumber.fill,
        defaults.watermark.color,
        defaults.spotlight.color,
    };
    for (const QColor& color : colors) {
        if (!color.isValid()) {
            return false;
        }
    }

    engineDefaults = SnowStyleDefaults{};
    engineDefaults.rectangle = toEngineShapeStyle(defaults.rectangle);
    engineDefaults.arrow = toEngineShapeStyle(defaults.arrow);
    engineDefaults.line = toEngineShapeStyle(defaults.line);
    engineDefaults.free_draw = toEngineShapeStyle(defaults.freeDraw);
    engineDefaults.rectangle_highlight = toEngineShapeStyle(defaults.rectangleHighlight);
    engineDefaults.pen_highlight = toEngineShapeStyle(defaults.penHighlight);
    engineDefaults.rectangle_filter = SnowFilterStyle{
        static_cast<SnowFilterType>(defaults.rectangleFilter.type),
        defaults.rectangleFilter.strength,
        defaults.rectangleFilter.opacity,
        defaults.rectangleFilter.strokeWidth,
    };
    engineDefaults.pen_filter = SnowFilterStyle{
        static_cast<SnowFilterType>(defaults.penFilter.type),
        defaults.penFilter.strength,
        defaults.penFilter.opacity,
        defaults.penFilter.strokeWidth,
    };
    engineDefaults.text = toEngineTextStyle(defaults.text);
    engineDefaults.serial_number = toEngineSerialNumberStyle(defaults.serialNumber);
    engineDefaults.watermark.color = toEngineColor(defaults.watermark.color);
    const QByteArray watermarkText = defaults.watermark.text.toUtf8();
    engineDefaults.watermark.text_utf8_len = static_cast<std::uint32_t>(watermarkText.size());
    std::copy_n(
        watermarkText.constData(),
        std::min<std::size_t>(watermarkText.size(), sizeof(engineDefaults.watermark.text_utf8)),
        engineDefaults.watermark.text_utf8);
    engineDefaults.watermark.font_size = defaults.watermark.fontSize;
    const QByteArray watermarkFamily = defaults.watermark.fontFamily.toUtf8();
    engineDefaults.watermark.font_family_utf8_len =
        static_cast<std::uint32_t>(watermarkFamily.size());
    std::copy_n(watermarkFamily.constData(),
                std::min<std::size_t>(watermarkFamily.size(),
                                      sizeof(engineDefaults.watermark.font_family_utf8)),
                engineDefaults.watermark.font_family_utf8);
    engineDefaults.watermark.angle = defaults.watermark.angle;
    engineDefaults.watermark.gap = defaults.watermark.gap;
    engineDefaults.watermark.opacity = defaults.watermark.opacity;
    engineDefaults.spotlight = toEngineSpotlightConfig(defaults.spotlight);
    return true;
}

} // namespace snow_canvas_types

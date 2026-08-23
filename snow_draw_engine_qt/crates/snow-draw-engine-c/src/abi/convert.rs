mod patch_payloads;

pub(crate) use patch_payloads::*;

use snow_draw_engine::{
    ActiveTool, ArrowPathCommand, ArrowStrokeStyle, ArrowType, Arrowhead, ArrowheadDisplayDashMode,
    ArrowheadDisplayFillMode, ArrowheadDisplayPrimitive, ArrowheadDisplayPrimitiveKind,
    CanvasFilterType, ColorRgba8, CornerRadii, CursorCommand, CursorStyle, DisplayFillStyle,
    DisplayStrokeStyle, DisplayTextHorizontalAlign, DisplayTextVerticalAlign, ElementId,
    EngineConfig, FillStyle, FilterStyle, GridConfig, HistoryState, InputEvent, InteractionOutput,
    KeyCode, KeyEvent, KeyEventType, Modifiers, Point, PointerButton, PointerButtons,
    PointerCaptureCommand, PointerDevice, PointerEvent, PointerEventType, RectangleShapeStyle,
    RuntimeConfig, SerialNumberStyle, ShapeKind, ShapeStyle, SnapConfig, SpotlightConfig,
    StrokeStyle, StyleDefaults, StyleToolbarSource, TextElementInfo, TextHorizontalAlign,
    TextLayoutOverride, TextLayoutSize, TextStyle, TextVerticalAlign, Vector2, WatermarkConfig,
    WheelDeltaKind, WheelEvent, ZoomFocus, normalize_font_family,
};

use crate::abi::text::{
    copy_optional_str_to_c_char_field, copy_str_to_c_char_field, string_from_c_char_field,
};
use crate::abi::types::*;

impl From<SnowColorRgba8> for ColorRgba8 {
    fn from(value: SnowColorRgba8) -> Self {
        Self {
            r: value.r,
            g: value.g,
            b: value.b,
            a: value.a,
        }
    }
}

impl From<SnowWatermarkConfig> for WatermarkConfig {
    fn from(value: SnowWatermarkConfig) -> Self {
        Self {
            color: value.color.into(),
            text: string_from_c_char_field(&value.text_utf8, value.text_utf8_len)
                .unwrap_or_default(),
            font_size: value.font_size,
            font_family: string_from_c_char_field(
                &value.font_family_utf8,
                value.font_family_utf8_len,
            )
            .unwrap_or_default(),
            angle: value.angle,
            gap: value.gap,
            opacity: value.opacity,
        }
        .normalized()
    }
}

impl From<WatermarkConfig> for SnowWatermarkConfig {
    fn from(value: WatermarkConfig) -> Self {
        let mut out = Self {
            color: value.color.into(),
            font_size: value.font_size,
            angle: value.angle,
            gap: value.gap,
            opacity: value.opacity,
            ..Self::default()
        };
        let mut truncated = 0;
        copy_str_to_c_char_field(
            &mut out.text_utf8,
            &mut out.text_utf8_len,
            &mut truncated,
            &value.text,
        );
        copy_str_to_c_char_field(
            &mut out.font_family_utf8,
            &mut out.font_family_utf8_len,
            &mut truncated,
            &value.font_family,
        );
        out
    }
}

impl From<SnowSpotlightConfig> for SpotlightConfig {
    fn from(value: SnowSpotlightConfig) -> Self {
        Self {
            color: value.color.into(),
            opacity: value.opacity,
        }
        .normalized()
    }
}

impl From<SpotlightConfig> for SnowSpotlightConfig {
    fn from(value: SpotlightConfig) -> Self {
        Self {
            color: value.color.into(),
            opacity: value.opacity,
        }
    }
}

impl From<ColorRgba8> for SnowColorRgba8 {
    fn from(value: ColorRgba8) -> Self {
        Self {
            r: value.r,
            g: value.g,
            b: value.b,
            a: value.a,
        }
    }
}

pub(crate) fn snow_arrowhead_from_rust(value: Option<Arrowhead>) -> SnowArrowhead {
    match value {
        None => SnowArrowhead::None,
        Some(Arrowhead::Arrow) => SnowArrowhead::Arrow,
        Some(Arrowhead::Bar) => SnowArrowhead::Bar,
        Some(Arrowhead::Dot) => SnowArrowhead::Dot,
        Some(Arrowhead::Circle) => SnowArrowhead::Circle,
        Some(Arrowhead::CircleOutline) => SnowArrowhead::CircleOutline,
        Some(Arrowhead::Triangle) => SnowArrowhead::Triangle,
        Some(Arrowhead::TriangleOutline) => SnowArrowhead::TriangleOutline,
        Some(Arrowhead::Diamond) => SnowArrowhead::Diamond,
        Some(Arrowhead::DiamondOutline) => SnowArrowhead::DiamondOutline,
        Some(Arrowhead::CrowfootOne) => SnowArrowhead::CrowfootOne,
        Some(Arrowhead::CrowfootMany) => SnowArrowhead::CrowfootMany,
        Some(Arrowhead::CrowfootOneOrMany) => SnowArrowhead::CrowfootOneOrMany,
        Some(Arrowhead::Square) => SnowArrowhead::Square,
        Some(Arrowhead::InvertedTriangle) => SnowArrowhead::InvertedTriangle,
    }
}

pub(crate) fn snow_arrowhead_to_rust(value: SnowArrowhead) -> Option<Arrowhead> {
    match value {
        SnowArrowhead::None => None,
        SnowArrowhead::Arrow => Some(Arrowhead::Arrow),
        SnowArrowhead::Bar => Some(Arrowhead::Bar),
        SnowArrowhead::Dot => Some(Arrowhead::Dot),
        SnowArrowhead::Circle => Some(Arrowhead::Circle),
        SnowArrowhead::CircleOutline => Some(Arrowhead::CircleOutline),
        SnowArrowhead::Triangle => Some(Arrowhead::Triangle),
        SnowArrowhead::TriangleOutline => Some(Arrowhead::TriangleOutline),
        SnowArrowhead::Diamond => Some(Arrowhead::Diamond),
        SnowArrowhead::DiamondOutline => Some(Arrowhead::DiamondOutline),
        SnowArrowhead::CrowfootOne => Some(Arrowhead::CrowfootOne),
        SnowArrowhead::CrowfootMany => Some(Arrowhead::CrowfootMany),
        SnowArrowhead::CrowfootOneOrMany => Some(Arrowhead::CrowfootOneOrMany),
        SnowArrowhead::Square => Some(Arrowhead::Square),
        SnowArrowhead::InvertedTriangle => Some(Arrowhead::InvertedTriangle),
    }
}

pub(crate) fn snow_arrow_stroke_style_from_rust(value: ArrowStrokeStyle) -> SnowArrowStrokeStyle {
    match value {
        ArrowStrokeStyle::Solid => SnowArrowStrokeStyle::Solid,
        ArrowStrokeStyle::Dashed => SnowArrowStrokeStyle::Dashed,
        ArrowStrokeStyle::Dotted => SnowArrowStrokeStyle::Dotted,
    }
}

pub(crate) fn snow_arrow_stroke_style_to_rust(value: SnowArrowStrokeStyle) -> ArrowStrokeStyle {
    match value {
        SnowArrowStrokeStyle::Solid => ArrowStrokeStyle::Solid,
        SnowArrowStrokeStyle::Dashed => ArrowStrokeStyle::Dashed,
        SnowArrowStrokeStyle::Dotted => ArrowStrokeStyle::Dotted,
    }
}

pub(crate) fn snow_arrow_type_from_rust(value: ArrowType) -> SnowArrowType {
    match value {
        ArrowType::Straight => SnowArrowType::Straight,
        ArrowType::Curve => SnowArrowType::Curve,
        ArrowType::Elbow => SnowArrowType::Elbow,
    }
}

pub(crate) fn snow_arrow_type_to_rust(value: SnowArrowType) -> ArrowType {
    match value {
        SnowArrowType::Straight => ArrowType::Straight,
        SnowArrowType::Curve => ArrowType::Curve,
        SnowArrowType::Elbow => ArrowType::Elbow,
    }
}

pub(crate) fn snow_shape_kind_to_rust(value: SnowShapeKind) -> ShapeKind {
    match value {
        SnowShapeKind::Rectangle => ShapeKind::Rectangle,
        SnowShapeKind::Arrow => ShapeKind::Arrow,
        SnowShapeKind::Line => ShapeKind::Line,
        SnowShapeKind::FreeDraw => ShapeKind::FreeDraw,
        SnowShapeKind::RectangleHighlight => ShapeKind::RectangleHighlight,
        SnowShapeKind::PenHighlight => ShapeKind::PenHighlight,
        SnowShapeKind::Spotlight => ShapeKind::Spotlight,
    }
}

pub(crate) fn snow_arrow_points_from_rust(points: &[[f64; 2]]) -> Vec<SnowArrowPoint> {
    points
        .iter()
        .map(|point| SnowArrowPoint {
            x: point[0],
            y: point[1],
        })
        .collect()
}

fn snow_arrow_point_from_path_point(point: [f64; 2]) -> SnowArrowPoint {
    SnowArrowPoint {
        x: point[0],
        y: point[1],
    }
}

pub(crate) fn snow_arrow_path_commands_from_rust(
    commands: &[ArrowPathCommand],
) -> Vec<SnowArrowPathCommand> {
    commands
        .iter()
        .map(|command| match *command {
            ArrowPathCommand::MoveTo { point } => SnowArrowPathCommand {
                kind: SnowArrowPathCommandKind::MoveTo,
                point: snow_arrow_point_from_path_point(point),
                ..SnowArrowPathCommand::default()
            },
            ArrowPathCommand::LineTo { point } => SnowArrowPathCommand {
                kind: SnowArrowPathCommandKind::LineTo,
                point: snow_arrow_point_from_path_point(point),
                ..SnowArrowPathCommand::default()
            },
            ArrowPathCommand::QuadTo { control, end } => SnowArrowPathCommand {
                kind: SnowArrowPathCommandKind::QuadTo,
                point: snow_arrow_point_from_path_point(end),
                control1: snow_arrow_point_from_path_point(control),
                ..SnowArrowPathCommand::default()
            },
            ArrowPathCommand::CubicTo {
                control_1,
                control_2,
                end,
            } => SnowArrowPathCommand {
                kind: SnowArrowPathCommandKind::CubicTo,
                point: snow_arrow_point_from_path_point(end),
                control1: snow_arrow_point_from_path_point(control_1),
                control2: snow_arrow_point_from_path_point(control_2),
                ..SnowArrowPathCommand::default()
            },
        })
        .collect()
}

fn snow_arrowhead_primitive_kind_from_rust(
    value: ArrowheadDisplayPrimitiveKind,
) -> SnowArrowheadPrimitiveKind {
    match value {
        ArrowheadDisplayPrimitiveKind::Line => SnowArrowheadPrimitiveKind::Line,
        ArrowheadDisplayPrimitiveKind::Polygon => SnowArrowheadPrimitiveKind::Polygon,
        ArrowheadDisplayPrimitiveKind::Circle => SnowArrowheadPrimitiveKind::Circle,
    }
}

fn snow_arrowhead_fill_mode_from_rust(value: ArrowheadDisplayFillMode) -> SnowArrowheadFillMode {
    match value {
        ArrowheadDisplayFillMode::Stroke => SnowArrowheadFillMode::Stroke,
        ArrowheadDisplayFillMode::Background => SnowArrowheadFillMode::Background,
    }
}

fn snow_arrowhead_dash_mode_from_rust(value: ArrowheadDisplayDashMode) -> SnowArrowheadDashMode {
    match value {
        ArrowheadDisplayDashMode::Inherit => SnowArrowheadDashMode::Inherit,
        ArrowheadDisplayDashMode::Solid => SnowArrowheadDashMode::Solid,
        ArrowheadDisplayDashMode::DottedCap => SnowArrowheadDashMode::DottedCap,
    }
}

fn snow_arrowhead_primitives_from_rust(
    primitives: &[ArrowheadDisplayPrimitive],
) -> Vec<SnowArrowheadPrimitive> {
    primitives
        .iter()
        .map(|primitive| {
            let mut points = [SnowArrowPoint::default(); SNOW_ARROWHEAD_PRIMITIVE_POINT_CAPACITY];
            for (point_index, point) in primitive
                .points
                .iter()
                .take(SNOW_ARROWHEAD_PRIMITIVE_POINT_CAPACITY)
                .enumerate()
            {
                points[point_index] = SnowArrowPoint {
                    x: point[0],
                    y: point[1],
                };
            }
            SnowArrowheadPrimitive {
                kind: snow_arrowhead_primitive_kind_from_rust(primitive.kind),
                fill_mode: snow_arrowhead_fill_mode_from_rust(primitive.fill_mode),
                dash_mode: snow_arrowhead_dash_mode_from_rust(primitive.dash_mode),
                point_count: primitive
                    .points
                    .len()
                    .min(SNOW_ARROWHEAD_PRIMITIVE_POINT_CAPACITY)
                    as u32,
                points,
                center: SnowArrowPoint {
                    x: primitive.center[0],
                    y: primitive.center[1],
                },
                diameter: primitive.diameter,
            }
        })
        .collect()
}

pub(crate) fn snow_text_horizontal_align_from_rust(
    value: DisplayTextHorizontalAlign,
) -> SnowTextHorizontalAlign {
    match value {
        DisplayTextHorizontalAlign::Left => SnowTextHorizontalAlign::Left,
        DisplayTextHorizontalAlign::Center => SnowTextHorizontalAlign::Center,
        DisplayTextHorizontalAlign::Right => SnowTextHorizontalAlign::Right,
    }
}

pub(crate) fn snow_text_vertical_align_from_rust(
    value: DisplayTextVerticalAlign,
) -> SnowTextVerticalAlign {
    match value {
        DisplayTextVerticalAlign::Top => SnowTextVerticalAlign::Top,
        DisplayTextVerticalAlign::Center => SnowTextVerticalAlign::Center,
        DisplayTextVerticalAlign::Bottom => SnowTextVerticalAlign::Bottom,
    }
}

pub(crate) fn snow_fill_style_from_rust(value: DisplayFillStyle) -> SnowFillStyle {
    match value {
        DisplayFillStyle::Line => SnowFillStyle::Line,
        DisplayFillStyle::CrossLine => SnowFillStyle::CrossLine,
        DisplayFillStyle::Solid => SnowFillStyle::Solid,
    }
}

pub(crate) fn snow_fill_style_to_rust(value: SnowFillStyle) -> snow_draw_engine::FillStyle {
    match value {
        SnowFillStyle::Line => snow_draw_engine::FillStyle::Line,
        SnowFillStyle::CrossLine => snow_draw_engine::FillStyle::CrossLine,
        SnowFillStyle::Solid => snow_draw_engine::FillStyle::Solid,
    }
}

pub(crate) fn snow_document_fill_style_from_rust(value: FillStyle) -> SnowFillStyle {
    match value {
        FillStyle::Line => SnowFillStyle::Line,
        FillStyle::CrossLine => SnowFillStyle::CrossLine,
        FillStyle::Solid => SnowFillStyle::Solid,
    }
}

pub(crate) fn snow_stroke_style_from_rust(value: DisplayStrokeStyle) -> SnowStrokeStyle {
    match value {
        DisplayStrokeStyle::Solid => SnowStrokeStyle::Solid,
        DisplayStrokeStyle::Dashed => SnowStrokeStyle::Dashed,
        DisplayStrokeStyle::Dotted => SnowStrokeStyle::Dotted,
    }
}

pub(crate) fn snow_document_stroke_style_from_rust(value: StrokeStyle) -> SnowStrokeStyle {
    match value {
        StrokeStyle::Solid => SnowStrokeStyle::Solid,
        StrokeStyle::Dashed => SnowStrokeStyle::Dashed,
        StrokeStyle::Dotted => SnowStrokeStyle::Dotted,
    }
}

pub(crate) fn snow_stroke_style_to_rust(value: SnowStrokeStyle) -> StrokeStyle {
    match value {
        SnowStrokeStyle::Solid => StrokeStyle::Solid,
        SnowStrokeStyle::Dashed => StrokeStyle::Dashed,
        SnowStrokeStyle::Dotted => StrokeStyle::Dotted,
    }
}

pub(crate) fn snow_text_horizontal_align_to_rust(
    value: SnowTextHorizontalAlign,
) -> TextHorizontalAlign {
    match value {
        SnowTextHorizontalAlign::Left => TextHorizontalAlign::Left,
        SnowTextHorizontalAlign::Center => TextHorizontalAlign::Center,
        SnowTextHorizontalAlign::Right => TextHorizontalAlign::Right,
    }
}

pub(crate) fn snow_document_text_horizontal_align_from_rust(
    value: TextHorizontalAlign,
) -> SnowTextHorizontalAlign {
    match value {
        TextHorizontalAlign::Left => SnowTextHorizontalAlign::Left,
        TextHorizontalAlign::Center => SnowTextHorizontalAlign::Center,
        TextHorizontalAlign::Right => SnowTextHorizontalAlign::Right,
    }
}

pub(crate) fn snow_text_vertical_align_to_rust(value: SnowTextVerticalAlign) -> TextVerticalAlign {
    match value {
        SnowTextVerticalAlign::Top => TextVerticalAlign::Top,
        SnowTextVerticalAlign::Center => TextVerticalAlign::Center,
        SnowTextVerticalAlign::Bottom => TextVerticalAlign::Bottom,
    }
}

pub(crate) fn snow_document_text_vertical_align_from_rust(
    value: TextVerticalAlign,
) -> SnowTextVerticalAlign {
    match value {
        TextVerticalAlign::Top => SnowTextVerticalAlign::Top,
        TextVerticalAlign::Center => SnowTextVerticalAlign::Center,
        TextVerticalAlign::Bottom => SnowTextVerticalAlign::Bottom,
    }
}

pub(crate) fn snow_element_id_from_rust(id: ElementId) -> SnowElementId {
    SnowElementId {
        index: id.index,
        generation: id.generation,
    }
}

pub(crate) fn snow_element_id_to_rust(id: SnowElementId) -> ElementId {
    ElementId {
        index: id.index,
        generation: id.generation,
    }
}

impl From<SnowTextLayoutSize> for TextLayoutSize {
    fn from(value: SnowTextLayoutSize) -> Self {
        Self {
            width: value.width,
            height: value.height,
        }
    }
}

impl From<SnowTextLayoutOverride> for TextLayoutOverride {
    fn from(value: SnowTextLayoutOverride) -> Self {
        Self {
            id: snow_element_id_to_rust(value.id),
            size: value.size.into(),
        }
    }
}

fn font_family_from_c<const N: usize>(bytes: &[std::ffi::c_char; N], len: u32) -> Option<String> {
    string_from_c_char_field(bytes, len).and_then(|value| normalize_font_family(Some(value)))
}

pub(crate) fn snow_text_element_info_from_rust(value: TextElementInfo) -> SnowTextElementInfo {
    let mut out = SnowTextElementInfo {
        id: snow_element_id_from_rust(value.id),
        center_x: value.center.x,
        center_y: value.center.y,
        width: value.width,
        height: value.height,
        rotation: value.rotation,
        font_size: value.font_size,
        auto_resize: u8::from(value.auto_resize),
        measure_natural_width: u8::from(value.measure_natural_width),
        ..SnowTextElementInfo::default()
    };
    copy_str_to_c_char_field(
        &mut out.text_utf8,
        &mut out.text_utf8_len,
        &mut out.text_truncated,
        &value.text,
    );
    copy_optional_str_to_c_char_field(
        &mut out.font_family_utf8,
        &mut out.font_family_utf8_len,
        &mut out.font_family_truncated,
        value.font_family.as_deref(),
    );
    out
}

impl From<SnowCornerRadii> for CornerRadii {
    fn from(value: SnowCornerRadii) -> Self {
        Self {
            top_left: value.top_left,
            top_right: value.top_right,
            bottom_right: value.bottom_right,
            bottom_left: value.bottom_left,
        }
    }
}

impl From<SnowSnapConfig> for SnapConfig {
    fn from(value: SnowSnapConfig) -> Self {
        Self {
            enabled: value.enabled != 0,
            distance: value.distance,
            enable_point_snaps: value.enable_point_snaps != 0,
            enable_gap_snaps: value.enable_gap_snaps != 0,
            show_guides: value.show_guides != 0,
            show_gap_size: value.show_gap_size != 0,
            line_color: value.line_color.into(),
            line_width: value.line_width,
            marker_size: value.marker_size,
            gap_dash_length: value.gap_dash_length,
            gap_dash_gap: value.gap_dash_gap,
        }
    }
}

impl From<SnapConfig> for SnowSnapConfig {
    fn from(value: SnapConfig) -> Self {
        Self {
            enabled: u8::from(value.enabled),
            enable_point_snaps: u8::from(value.enable_point_snaps),
            enable_gap_snaps: u8::from(value.enable_gap_snaps),
            show_guides: u8::from(value.show_guides),
            show_gap_size: u8::from(value.show_gap_size),
            reserved0: [0; 3],
            distance: value.distance,
            line_color: value.line_color.into(),
            line_width: value.line_width,
            marker_size: value.marker_size,
            gap_dash_length: value.gap_dash_length,
            gap_dash_gap: value.gap_dash_gap,
        }
    }
}

impl From<SnowGridConfig> for GridConfig {
    fn from(value: SnowGridConfig) -> Self {
        Self {
            enabled: value.enabled != 0,
            size: value.size,
        }
    }
}

impl From<GridConfig> for SnowGridConfig {
    fn from(value: GridConfig) -> Self {
        Self {
            enabled: u8::from(value.enabled),
            reserved0: [0; 7],
            size: value.size,
        }
    }
}

impl From<CornerRadii> for SnowCornerRadii {
    fn from(value: CornerRadii) -> Self {
        Self {
            top_left: value.top_left,
            top_right: value.top_right,
            bottom_right: value.bottom_right,
            bottom_left: value.bottom_left,
        }
    }
}

impl From<SnowShapeStyle> for ShapeStyle {
    fn from(value: SnowShapeStyle) -> Self {
        Self {
            fill: value.fill.into(),
            fill_style: snow_fill_style_to_rust(value.fill_style),
            stroke: value.stroke.into(),
            stroke_width: value.stroke_width,
            corner_radii: value.corner_radii.into(),
            start_arrowhead: snow_arrowhead_to_rust(value.start_arrowhead),
            end_arrowhead: snow_arrowhead_to_rust(value.end_arrowhead),
            stroke_style: snow_arrow_stroke_style_to_rust(value.stroke_style),
            arrow_type: snow_arrow_type_to_rust(value.arrow_type),
            opacity: value.opacity,
            highlight_shape: match value.highlight_shape {
                SnowHighlightShape::Rectangle => snow_draw_engine::HighlightShape::Rectangle,
                SnowHighlightShape::Ellipse => snow_draw_engine::HighlightShape::Ellipse,
            },
            shape: match value.shape {
                SnowRectangleShape::Rectangle => snow_draw_engine::HighlightShape::Rectangle,
                SnowRectangleShape::Ellipse => snow_draw_engine::HighlightShape::Ellipse,
                SnowRectangleShape::Diamond => snow_draw_engine::HighlightShape::Diamond,
            },
        }
    }
}

impl From<SnowFilterStyle> for FilterStyle {
    fn from(value: SnowFilterStyle) -> Self {
        Self {
            filter_type: match value.filter_type {
                SnowFilterType::Mosaic => CanvasFilterType::Mosaic,
                SnowFilterType::GaussianBlur => CanvasFilterType::GaussianBlur,
                SnowFilterType::Grayscale => CanvasFilterType::Grayscale,
                SnowFilterType::Inversion => CanvasFilterType::Inversion,
            },
            strength: value.strength,
            opacity: value.opacity,
            stroke_width: value.stroke_width,
        }
    }
}

impl From<FilterStyle> for SnowFilterStyle {
    fn from(value: FilterStyle) -> Self {
        Self {
            filter_type: match value.filter_type {
                CanvasFilterType::Mosaic => SnowFilterType::Mosaic,
                CanvasFilterType::GaussianBlur => SnowFilterType::GaussianBlur,
                CanvasFilterType::Grayscale => SnowFilterType::Grayscale,
                CanvasFilterType::Inversion => SnowFilterType::Inversion,
            },
            strength: value.strength,
            opacity: value.opacity,
            stroke_width: value.stroke_width,
        }
    }
}

impl From<SnowRectangleShapeStyle> for RectangleShapeStyle {
    fn from(value: SnowRectangleShapeStyle) -> Self {
        Self {
            fill: value.fill.into(),
            fill_style: snow_fill_style_to_rust(value.fill_style),
            stroke: value.stroke.into(),
            stroke_width: value.stroke_width,
            stroke_style: snow_stroke_style_to_rust(value.stroke_style),
            corner_radii: value.corner_radii.into(),
            shape: snow_draw_engine::HighlightShape::Rectangle,
        }
    }
}

impl From<RectangleShapeStyle> for SnowRectangleShapeStyle {
    fn from(value: RectangleShapeStyle) -> Self {
        Self {
            fill: value.fill.into(),
            fill_style: snow_document_fill_style_from_rust(value.fill_style),
            stroke: value.stroke.into(),
            stroke_width: value.stroke_width,
            stroke_style: snow_document_stroke_style_from_rust(value.stroke_style),
            corner_radii: value.corner_radii.into(),
        }
    }
}

impl From<SnowArrowStyle> for snow_draw_engine::ArrowStyle {
    fn from(value: SnowArrowStyle) -> Self {
        Self {
            stroke: value.stroke.into(),
            stroke_width: value.stroke_width,
            start_arrowhead: snow_arrowhead_to_rust(value.start_arrowhead),
            end_arrowhead: snow_arrowhead_to_rust(value.end_arrowhead),
            stroke_style: snow_arrow_stroke_style_to_rust(value.stroke_style),
            arrow_type: snow_arrow_type_to_rust(value.arrow_type),
        }
    }
}

impl From<snow_draw_engine::ArrowStyle> for SnowArrowStyle {
    fn from(value: snow_draw_engine::ArrowStyle) -> Self {
        Self {
            stroke: value.stroke.into(),
            stroke_width: value.stroke_width,
            start_arrowhead: snow_arrowhead_from_rust(value.start_arrowhead),
            end_arrowhead: snow_arrowhead_from_rust(value.end_arrowhead),
            stroke_style: snow_arrow_stroke_style_from_rust(value.stroke_style),
            arrow_type: snow_arrow_type_from_rust(value.arrow_type),
            reserved0: [0; 4],
        }
    }
}

impl From<SnowTextStyle> for TextStyle {
    fn from(value: SnowTextStyle) -> Self {
        Self {
            color: value.color.into(),
            font_size: value.font_size,
            font_family: font_family_from_c(&value.font_family_utf8, value.font_family_utf8_len),
            fill: value.fill.into(),
            fill_style: snow_fill_style_to_rust(value.fill_style),
            stroke: value.stroke.into(),
            stroke_width: value.stroke_width,
            corner_radii: value.corner_radii.into(),
            horizontal_align: snow_text_horizontal_align_to_rust(value.horizontal_align),
            vertical_align: snow_text_vertical_align_to_rust(value.vertical_align),
            opacity: value.opacity,
        }
    }
}

impl From<TextStyle> for SnowTextStyle {
    fn from(value: TextStyle) -> Self {
        let mut out = Self {
            color: value.color.into(),
            font_size: value.font_size,
            fill: value.fill.into(),
            fill_style: match value.fill_style {
                snow_draw_engine::FillStyle::Line => SnowFillStyle::Line,
                snow_draw_engine::FillStyle::CrossLine => SnowFillStyle::CrossLine,
                snow_draw_engine::FillStyle::Solid => SnowFillStyle::Solid,
            },
            stroke: value.stroke.into(),
            stroke_width: value.stroke_width,
            corner_radii: value.corner_radii.into(),
            horizontal_align: snow_document_text_horizontal_align_from_rust(value.horizontal_align),
            vertical_align: snow_document_text_vertical_align_from_rust(value.vertical_align),
            opacity: value.opacity,
            reserved0: [0; 4],
            font_family_utf8_len: 0,
            font_family_truncated: 0,
            reserved1: [0; 3],
            font_family_utf8: [0; SNOW_FONT_FAMILY_UTF8_CAPACITY],
        };
        copy_optional_str_to_c_char_field(
            &mut out.font_family_utf8,
            &mut out.font_family_utf8_len,
            &mut out.font_family_truncated,
            value.font_family.as_deref(),
        );
        out
    }
}

impl From<SnowSerialNumberStyle> for SerialNumberStyle {
    fn from(value: SnowSerialNumberStyle) -> Self {
        Self {
            number: value.number.max(0),
            color: value.color.into(),
            fill: value.fill.into(),
            fill_style: snow_fill_style_to_rust(value.fill_style),
            font_size: value.font_size,
            font_family: font_family_from_c(&value.font_family_utf8, value.font_family_utf8_len),
            stroke_width: value.stroke_width,
            stroke_style: snow_stroke_style_to_rust(value.stroke_style),
            opacity: value.opacity,
        }
    }
}

impl From<SerialNumberStyle> for SnowSerialNumberStyle {
    fn from(value: SerialNumberStyle) -> Self {
        let mut out = Self {
            number: value.number.max(0),
            color: value.color.into(),
            fill: value.fill.into(),
            fill_style: match value.fill_style {
                snow_draw_engine::FillStyle::Line => SnowFillStyle::Line,
                snow_draw_engine::FillStyle::CrossLine => SnowFillStyle::CrossLine,
                snow_draw_engine::FillStyle::Solid => SnowFillStyle::Solid,
            },
            font_size: value.font_size,
            stroke_width: value.stroke_width,
            stroke_style: snow_document_stroke_style_from_rust(value.stroke_style),
            opacity: value.opacity,
            reserved0: [0; 4],
            font_family_utf8_len: 0,
            font_family_truncated: 0,
            reserved1: [0; 3],
            font_family_utf8: [0; SNOW_FONT_FAMILY_UTF8_CAPACITY],
        };
        copy_optional_str_to_c_char_field(
            &mut out.font_family_utf8,
            &mut out.font_family_utf8_len,
            &mut out.font_family_truncated,
            value.font_family.as_deref(),
        );
        out
    }
}

impl From<ShapeStyle> for SnowShapeStyle {
    fn from(value: ShapeStyle) -> Self {
        Self {
            fill: value.fill.into(),
            fill_style: snow_document_fill_style_from_rust(value.fill_style),
            stroke: value.stroke.into(),
            stroke_width: value.stroke_width,
            corner_radii: value.corner_radii.into(),
            start_arrowhead: snow_arrowhead_from_rust(value.start_arrowhead),
            end_arrowhead: snow_arrowhead_from_rust(value.end_arrowhead),
            stroke_style: snow_arrow_stroke_style_from_rust(value.stroke_style),
            arrow_type: snow_arrow_type_from_rust(value.arrow_type),
            opacity: value.opacity,
            highlight_shape: match value.highlight_shape {
                snow_draw_engine::HighlightShape::Rectangle => SnowHighlightShape::Rectangle,
                snow_draw_engine::HighlightShape::Ellipse => SnowHighlightShape::Ellipse,
                snow_draw_engine::HighlightShape::Diamond => SnowHighlightShape::Rectangle,
            },
            shape: match value.shape {
                snow_draw_engine::HighlightShape::Rectangle => SnowRectangleShape::Rectangle,
                snow_draw_engine::HighlightShape::Ellipse => SnowRectangleShape::Ellipse,
                snow_draw_engine::HighlightShape::Diamond => SnowRectangleShape::Diamond,
            },
        }
    }
}

fn strict_string_from_c_char_field<const N: usize>(
    bytes: &[std::ffi::c_char; N],
    len: u32,
) -> Result<String, SnowError> {
    if len as usize > N {
        return Err(SnowError::InvalidArgument);
    }
    string_from_c_char_field(bytes, len).ok_or(SnowError::InvalidArgument)
}

unsafe fn raw_c_enum_in_range<T>(value: *const T, first: i32, last: i32) -> bool {
    let raw = unsafe { std::ptr::read_unaligned(value.cast::<i32>()) };
    (first..=last).contains(&raw)
}

unsafe fn runtime_style_default_enums_are_valid(defaults: *const SnowStyleDefaults) -> bool {
    let shapes = [
        unsafe { std::ptr::addr_of!((*defaults).rectangle) },
        unsafe { std::ptr::addr_of!((*defaults).arrow) },
        unsafe { std::ptr::addr_of!((*defaults).line) },
        unsafe { std::ptr::addr_of!((*defaults).free_draw) },
        unsafe { std::ptr::addr_of!((*defaults).rectangle_highlight) },
        unsafe { std::ptr::addr_of!((*defaults).pen_highlight) },
    ];
    for shape in shapes {
        if !unsafe {
            raw_c_enum_in_range(std::ptr::addr_of!((*shape).fill_style), 0, 2)
                && raw_c_enum_in_range(std::ptr::addr_of!((*shape).start_arrowhead), 0, 14)
                && raw_c_enum_in_range(std::ptr::addr_of!((*shape).end_arrowhead), 0, 14)
                && raw_c_enum_in_range(std::ptr::addr_of!((*shape).stroke_style), 0, 2)
                && raw_c_enum_in_range(std::ptr::addr_of!((*shape).arrow_type), 0, 2)
                && raw_c_enum_in_range(std::ptr::addr_of!((*shape).highlight_shape), 0, 1)
                && raw_c_enum_in_range(std::ptr::addr_of!((*shape).shape), 0, 2)
        } {
            return false;
        }
    }

    unsafe {
        raw_c_enum_in_range(
            std::ptr::addr_of!((*defaults).rectangle_filter.filter_type),
            0,
            3,
        ) && raw_c_enum_in_range(std::ptr::addr_of!((*defaults).pen_filter.filter_type), 0, 3)
            && raw_c_enum_in_range(std::ptr::addr_of!((*defaults).text.fill_style), 0, 2)
            && raw_c_enum_in_range(std::ptr::addr_of!((*defaults).text.horizontal_align), 0, 2)
            && raw_c_enum_in_range(std::ptr::addr_of!((*defaults).text.vertical_align), 0, 2)
            && raw_c_enum_in_range(
                std::ptr::addr_of!((*defaults).serial_number.fill_style),
                0,
                2,
            )
            && raw_c_enum_in_range(
                std::ptr::addr_of!((*defaults).serial_number.stroke_style),
                0,
                2,
            )
    }
}

pub(crate) fn runtime_config_from_c(
    config: Option<&SnowRuntimeConfig>,
) -> Result<RuntimeConfig, SnowError> {
    let Some(config) = config else {
        return Ok(RuntimeConfig::default());
    };
    if config.style_defaults.is_null() {
        return Ok(RuntimeConfig::default());
    }
    if !unsafe { runtime_style_default_enums_are_valid(config.style_defaults) } {
        return Err(SnowError::InvalidArgument);
    }
    let defaults = unsafe { &*config.style_defaults };
    let rectangle: ShapeStyle = defaults.rectangle.into();
    let arrow: ShapeStyle = defaults.arrow.into();
    if defaults.text.font_family_truncated != 0 || defaults.serial_number.font_family_truncated != 0
    {
        return Err(SnowError::InvalidArgument);
    }
    let text_font_family = strict_string_from_c_char_field(
        &defaults.text.font_family_utf8,
        defaults.text.font_family_utf8_len,
    )?;
    let serial_font_family = strict_string_from_c_char_field(
        &defaults.serial_number.font_family_utf8,
        defaults.serial_number.font_family_utf8_len,
    )?;
    let watermark_text = strict_string_from_c_char_field(
        &defaults.watermark.text_utf8,
        defaults.watermark.text_utf8_len,
    )?;
    let watermark_font_family = strict_string_from_c_char_field(
        &defaults.watermark.font_family_utf8,
        defaults.watermark.font_family_utf8_len,
    )?;
    Ok(RuntimeConfig {
        style_defaults: StyleDefaults {
            editor: snow_draw_engine::EditorStyleDefaults {
                rectangle: rectangle.rectangle_shape_style(),
                arrow: arrow.arrow_style(),
                line: defaults.line.into(),
                free_draw: defaults.free_draw.into(),
                rectangle_highlight: defaults.rectangle_highlight.into(),
                pen_highlight: defaults.pen_highlight.into(),
                rectangle_filter: defaults.rectangle_filter.into(),
                pen_filter: defaults.pen_filter.into(),
                text: TextStyle {
                    color: defaults.text.color.into(),
                    font_size: defaults.text.font_size,
                    font_family: normalize_font_family(Some(text_font_family)),
                    fill: defaults.text.fill.into(),
                    fill_style: snow_fill_style_to_rust(defaults.text.fill_style),
                    stroke: defaults.text.stroke.into(),
                    stroke_width: defaults.text.stroke_width,
                    corner_radii: defaults.text.corner_radii.into(),
                    horizontal_align: snow_text_horizontal_align_to_rust(
                        defaults.text.horizontal_align,
                    ),
                    vertical_align: snow_text_vertical_align_to_rust(defaults.text.vertical_align),
                    opacity: defaults.text.opacity,
                },
                serial_number: SerialNumberStyle {
                    number: defaults.serial_number.number,
                    color: defaults.serial_number.color.into(),
                    fill: defaults.serial_number.fill.into(),
                    fill_style: snow_fill_style_to_rust(defaults.serial_number.fill_style),
                    font_size: defaults.serial_number.font_size,
                    font_family: normalize_font_family(Some(serial_font_family)),
                    stroke_width: defaults.serial_number.stroke_width,
                    stroke_style: snow_stroke_style_to_rust(defaults.serial_number.stroke_style),
                    opacity: defaults.serial_number.opacity,
                },
            },
            watermark: WatermarkConfig {
                color: defaults.watermark.color.into(),
                text: watermark_text,
                font_size: defaults.watermark.font_size,
                font_family: watermark_font_family,
                angle: defaults.watermark.angle,
                gap: defaults.watermark.gap,
                opacity: defaults.watermark.opacity,
            },
            spotlight: SpotlightConfig {
                color: defaults.spotlight.color.into(),
                opacity: defaults.spotlight.opacity,
            },
        },
    })
}

impl From<StyleDefaults> for SnowStyleDefaults {
    fn from(value: StyleDefaults) -> Self {
        let rectangle = value.editor.rectangle;
        let arrow = value.editor.arrow;
        let rectangle_shape = SnowShapeStyle {
            fill: rectangle.fill.into(),
            fill_style: snow_document_fill_style_from_rust(rectangle.fill_style),
            stroke: rectangle.stroke.into(),
            stroke_width: rectangle.stroke_width,
            stroke_style: snow_arrow_stroke_style_from_rust(match rectangle.stroke_style {
                StrokeStyle::Solid => ArrowStrokeStyle::Solid,
                StrokeStyle::Dashed => ArrowStrokeStyle::Dashed,
                StrokeStyle::Dotted => ArrowStrokeStyle::Dotted,
            }),
            corner_radii: rectangle.corner_radii.into(),
            shape: match rectangle.shape {
                snow_draw_engine::HighlightShape::Rectangle => SnowRectangleShape::Rectangle,
                snow_draw_engine::HighlightShape::Ellipse => SnowRectangleShape::Ellipse,
                snow_draw_engine::HighlightShape::Diamond => SnowRectangleShape::Diamond,
            },
            ..SnowShapeStyle::default()
        };
        let arrow_shape = SnowShapeStyle {
            stroke: arrow.stroke.into(),
            stroke_width: arrow.stroke_width,
            start_arrowhead: snow_arrowhead_from_rust(arrow.start_arrowhead),
            end_arrowhead: snow_arrowhead_from_rust(arrow.end_arrowhead),
            stroke_style: snow_arrow_stroke_style_from_rust(arrow.stroke_style),
            arrow_type: snow_arrow_type_from_rust(arrow.arrow_type),
            ..SnowShapeStyle::default()
        };
        Self {
            rectangle: rectangle_shape,
            arrow: arrow_shape,
            line: value.editor.line.into(),
            free_draw: value.editor.free_draw.into(),
            rectangle_highlight: value.editor.rectangle_highlight.into(),
            pen_highlight: value.editor.pen_highlight.into(),
            rectangle_filter: value.editor.rectangle_filter.into(),
            pen_filter: value.editor.pen_filter.into(),
            text: value.editor.text.into(),
            serial_number: value.editor.serial_number.into(),
            watermark: value.watermark.into(),
            spotlight: value.spotlight.into(),
        }
    }
}

impl Default for SnowStyleToolbarState {
    fn default() -> Self {
        Self {
            source: SnowStyleToolbarSource::DefaultRectangle,
            reserved0: 0,
            shape_style: SnowShapeStyle::default(),
            text_style: SnowTextStyle::default(),
            serial_number_style: SnowSerialNumberStyle::default(),
            text_style_mixed: 0,
            serial_number_style_mixed: 0,
            shape_style_mixed: 0,
            filter_style: SnowFilterStyle::default(),
            filter_style_mixed: 0,
        }
    }
}

impl From<HistoryState> for SnowHistoryState {
    fn from(value: HistoryState) -> Self {
        Self {
            can_undo: u8::from(value.can_undo),
            can_redo: u8::from(value.can_redo),
            reserved0: [0; 6],
        }
    }
}

pub(crate) fn snow_engine_config_to_rust(value: &SnowEngineConfig) -> EngineConfig {
    let zoom_focus = match value.zoom_focus {
        SnowZoomFocus::Pointer => ZoomFocus::Pointer,
        SnowZoomFocus::Center => ZoomFocus::Center,
    };
    EngineConfig {
        min_zoom: value.min_zoom,
        max_zoom: value.max_zoom,
        zoom_focus,
        wheel_zoom_sensitivity: value.wheel_zoom_sensitivity,
        clear_color: value.clear_color.into(),
        snap: value.snap.into(),
        grid: value.grid.into(),
        enable_pointer_capture: value.enable_pointer_capture != 0,
    }
}

pub(crate) fn snow_active_tool_to_rust(value: SnowActiveTool) -> ActiveTool {
    match value {
        SnowActiveTool::Select => ActiveTool::Select,
        SnowActiveTool::Shape => ActiveTool::Shape,
        SnowActiveTool::Arrow => ActiveTool::Arrow,
        SnowActiveTool::Line => ActiveTool::Line,
        SnowActiveTool::FreeDraw => ActiveTool::FreeDraw,
        SnowActiveTool::RectangleHighlight => ActiveTool::RectangleHighlight,
        SnowActiveTool::PenHighlight => ActiveTool::PenHighlight,
        SnowActiveTool::Eraser => ActiveTool::Eraser,
        SnowActiveTool::RectangleFilter => ActiveTool::RectangleFilter,
        SnowActiveTool::PenFilter => ActiveTool::PenFilter,
        SnowActiveTool::Spotlight => ActiveTool::Spotlight,
        SnowActiveTool::Watermark => ActiveTool::Watermark,
        SnowActiveTool::Text => ActiveTool::Text,
        SnowActiveTool::SerialNumber => ActiveTool::SerialNumber,
    }
}

pub(crate) fn snow_active_tool_from_rust(value: ActiveTool) -> SnowActiveTool {
    match value {
        ActiveTool::Select => SnowActiveTool::Select,
        ActiveTool::Shape => SnowActiveTool::Shape,
        ActiveTool::Arrow => SnowActiveTool::Arrow,
        ActiveTool::Line => SnowActiveTool::Line,
        ActiveTool::FreeDraw => SnowActiveTool::FreeDraw,
        ActiveTool::RectangleHighlight => SnowActiveTool::RectangleHighlight,
        ActiveTool::PenHighlight => SnowActiveTool::PenHighlight,
        ActiveTool::Eraser => SnowActiveTool::Eraser,
        ActiveTool::RectangleFilter => SnowActiveTool::RectangleFilter,
        ActiveTool::PenFilter => SnowActiveTool::PenFilter,
        ActiveTool::Spotlight => SnowActiveTool::Spotlight,
        ActiveTool::Watermark => SnowActiveTool::Watermark,
        ActiveTool::Text => SnowActiveTool::Text,
        ActiveTool::SerialNumber => SnowActiveTool::SerialNumber,
    }
}

pub(crate) fn snow_active_tool_mask_to_rust(value: u64) -> u64 {
    const TOOLS: [SnowActiveTool; 14] = [
        SnowActiveTool::Select,
        SnowActiveTool::Shape,
        SnowActiveTool::Arrow,
        SnowActiveTool::Text,
        SnowActiveTool::SerialNumber,
        SnowActiveTool::Line,
        SnowActiveTool::FreeDraw,
        SnowActiveTool::RectangleHighlight,
        SnowActiveTool::Eraser,
        SnowActiveTool::RectangleFilter,
        SnowActiveTool::Watermark,
        SnowActiveTool::PenHighlight,
        SnowActiveTool::PenFilter,
        SnowActiveTool::Spotlight,
    ];

    TOOLS.into_iter().fold(0, |mask, tool| {
        if value & (1_u64 << tool as u32) == 0 {
            mask
        } else {
            mask | snow_active_tool_to_rust(tool).policy_bit()
        }
    })
}

pub(crate) fn snow_style_toolbar_source_from_rust(
    value: StyleToolbarSource,
) -> SnowStyleToolbarSource {
    match value {
        StyleToolbarSource::DefaultRectangle => SnowStyleToolbarSource::DefaultRectangle,
        StyleToolbarSource::SelectedRectangle => SnowStyleToolbarSource::SelectedRectangle,
        StyleToolbarSource::DefaultArrow => SnowStyleToolbarSource::DefaultArrow,
        StyleToolbarSource::SelectedArrow => SnowStyleToolbarSource::SelectedArrow,
        StyleToolbarSource::DefaultLine => SnowStyleToolbarSource::DefaultLine,
        StyleToolbarSource::SelectedLine => SnowStyleToolbarSource::SelectedLine,
        StyleToolbarSource::DefaultFreeDraw => SnowStyleToolbarSource::DefaultFreeDraw,
        StyleToolbarSource::SelectedFreeDraw => SnowStyleToolbarSource::SelectedFreeDraw,
        StyleToolbarSource::DefaultRectangleHighlight => {
            SnowStyleToolbarSource::DefaultRectangleHighlight
        }
        StyleToolbarSource::SelectedRectangleHighlight => {
            SnowStyleToolbarSource::SelectedRectangleHighlight
        }
        StyleToolbarSource::DefaultPenHighlight => SnowStyleToolbarSource::DefaultPenHighlight,
        StyleToolbarSource::SelectedPenHighlight => SnowStyleToolbarSource::SelectedPenHighlight,
        StyleToolbarSource::Eraser => SnowStyleToolbarSource::Eraser,
        StyleToolbarSource::DefaultRectangleFilter => {
            SnowStyleToolbarSource::DefaultRectangleFilter
        }
        StyleToolbarSource::SelectedRectangleFilter => {
            SnowStyleToolbarSource::SelectedRectangleFilter
        }
        StyleToolbarSource::DefaultPenFilter => SnowStyleToolbarSource::DefaultPenFilter,
        StyleToolbarSource::SelectedPenFilter => SnowStyleToolbarSource::SelectedPenFilter,
        StyleToolbarSource::DefaultSpotlight => SnowStyleToolbarSource::DefaultSpotlight,
        StyleToolbarSource::SelectedSpotlight => SnowStyleToolbarSource::SelectedSpotlight,
        StyleToolbarSource::Watermark => SnowStyleToolbarSource::Watermark,
        StyleToolbarSource::DefaultText => SnowStyleToolbarSource::DefaultText,
        StyleToolbarSource::SelectedText => SnowStyleToolbarSource::SelectedText,
        StyleToolbarSource::DefaultSerialNumber => SnowStyleToolbarSource::DefaultSerialNumber,
        StyleToolbarSource::SelectedSerialNumber => SnowStyleToolbarSource::SelectedSerialNumber,
    }
}

pub(crate) fn snow_pointer_event_to_rust(value: &SnowPointerEvent) -> PointerEvent {
    let event_type = match value.event_type {
        SnowPointerEventType::Down => PointerEventType::Down,
        SnowPointerEventType::Move => PointerEventType::Move,
        SnowPointerEventType::Up => PointerEventType::Up,
        SnowPointerEventType::Cancel => PointerEventType::Cancel,
        SnowPointerEventType::Enter => PointerEventType::Enter,
        SnowPointerEventType::Leave => PointerEventType::Leave,
        SnowPointerEventType::DoubleClick => PointerEventType::DoubleClick,
    };
    let device = match value.device {
        SnowPointerDevice::Mouse => PointerDevice::Mouse,
        SnowPointerDevice::Touch => PointerDevice::Touch,
        SnowPointerDevice::Pen => PointerDevice::Pen,
        SnowPointerDevice::Unknown => PointerDevice::Unknown,
    };
    let button = match value.button {
        SnowPointerButton::None => None,
        SnowPointerButton::Primary => Some(PointerButton::Primary),
        SnowPointerButton::Secondary => Some(PointerButton::Secondary),
        SnowPointerButton::Middle => Some(PointerButton::Middle),
    };

    PointerEvent {
        pointer_id: value.pointer_id,
        event_type,
        device,
        position: Point {
            x: value.position_x,
            y: value.position_y,
        },
        button,
        buttons: PointerButtons(value.buttons),
        modifiers: snow_modifiers_to_rust(value.modifiers),
    }
}

pub(crate) fn snow_input_event_to_rust(value: &SnowInputEvent) -> Result<InputEvent, SnowError> {
    match value.kind {
        SnowInputEventKind::Pointer => Ok(InputEvent::Pointer(snow_pointer_event_to_rust(
            &value.pointer,
        ))),
        SnowInputEventKind::Wheel => Ok(InputEvent::Wheel(snow_wheel_event_to_rust(&value.wheel))),
        SnowInputEventKind::Key => snow_key_event_to_rust(&value.key).map(InputEvent::Key),
        SnowInputEventKind::FocusLost => Ok(InputEvent::FocusLost),
    }
}

pub(crate) fn snow_wheel_event_to_rust(value: &SnowWheelEvent) -> WheelEvent {
    let delta_kind = match value.delta_kind {
        SnowWheelDeltaKind::Pixel => WheelDeltaKind::Pixel,
        SnowWheelDeltaKind::Angle => WheelDeltaKind::Angle,
    };

    WheelEvent {
        position: Point {
            x: value.position_x,
            y: value.position_y,
        },
        delta: Vector2 {
            x: value.delta_x,
            y: value.delta_y,
        },
        delta_kind,
        modifiers: snow_modifiers_to_rust(value.modifiers),
    }
}

pub(crate) fn snow_key_event_to_rust(value: &SnowKeyEvent) -> Result<KeyEvent, SnowError> {
    let event_type = match value.event_type {
        SnowKeyEventType::KeyDown => KeyEventType::KeyDown,
        SnowKeyEventType::KeyUp => KeyEventType::KeyUp,
    };
    let key_code = match value.key_code {
        SnowKeyCode::Unknown => KeyCode::Unknown,
        SnowKeyCode::Space => KeyCode::Space,
        SnowKeyCode::Escape => KeyCode::Escape,
        SnowKeyCode::ArrowUp => KeyCode::ArrowUp,
        SnowKeyCode::ArrowDown => KeyCode::ArrowDown,
        SnowKeyCode::ArrowLeft => KeyCode::ArrowLeft,
        SnowKeyCode::ArrowRight => KeyCode::ArrowRight,
        SnowKeyCode::Backspace => KeyCode::Backspace,
        SnowKeyCode::Delete => KeyCode::Delete,
        SnowKeyCode::Character => {
            let ch = char::from_u32(value.codepoint).ok_or(SnowError::InvalidArgument)?;
            KeyCode::Character(ch)
        }
    };

    Ok(KeyEvent {
        event_type,
        key_code,
        modifiers: snow_modifiers_to_rust(value.modifiers),
        repeat: value.repeat != 0,
    })
}

pub(crate) fn snow_modifiers_to_rust(value: SnowModifiers) -> Modifiers {
    Modifiers {
        ctrl: value.ctrl != 0,
        shift: value.shift != 0,
        alt: value.alt != 0,
        meta: value.meta != 0,
    }
}

pub(crate) fn snow_interaction_output_from_rust(value: InteractionOutput) -> SnowInteractionOutput {
    let (capture_kind, capture_pointer_id) = match value.capture {
        PointerCaptureCommand::NoChange => (SnowPointerCaptureCommandKind::NoChange, 0),
        PointerCaptureCommand::Capture(pointer_id) => {
            (SnowPointerCaptureCommandKind::Capture, pointer_id)
        }
        PointerCaptureCommand::Release => (SnowPointerCaptureCommandKind::Release, 0),
    };
    let (cursor_kind, cursor_style) = match value.cursor {
        CursorCommand::NoChange => (SnowCursorCommandKind::NoChange, SnowCursorStyle::Default),
        CursorCommand::Set(style) => (
            SnowCursorCommandKind::Set,
            snow_cursor_style_from_rust(style),
        ),
    };

    SnowInteractionOutput {
        consumed: u8::from(value.consumed),
        reserved0: [0; 3],
        capture_kind,
        capture_pointer_id,
        cursor_kind,
        cursor_style,
    }
}

pub(crate) fn snow_cursor_style_from_rust(value: CursorStyle) -> SnowCursorStyle {
    match value {
        CursorStyle::Default => SnowCursorStyle::Default,
        CursorStyle::Crosshair => SnowCursorStyle::Crosshair,
        CursorStyle::Grab => SnowCursorStyle::Grab,
        CursorStyle::Grabbing => SnowCursorStyle::Grabbing,
        CursorStyle::Move => SnowCursorStyle::Move,
        CursorStyle::ResizeHorizontal => SnowCursorStyle::ResizeHorizontal,
        CursorStyle::ResizeVertical => SnowCursorStyle::ResizeVertical,
        CursorStyle::ResizeNwSe => SnowCursorStyle::ResizeNwSe,
        CursorStyle::ResizeNeSw => SnowCursorStyle::ResizeNeSw,
        CursorStyle::Text => SnowCursorStyle::Text,
        CursorStyle::NotAllowed => SnowCursorStyle::NotAllowed,
        CursorStyle::CornerRadius => SnowCursorStyle::CornerRadius,
        CursorStyle::Hidden => SnowCursorStyle::Hidden,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine::{FillStyle, StyleDefaults};

    fn boundary_test_string(prefix_len: usize) -> (String, String) {
        let prefix = "a".repeat(prefix_len);
        let value = format!("{prefix}😀");
        (prefix, value)
    }

    #[test]
    fn rectangle_style_fill_style_round_trips_through_c_abi() {
        let c_style = SnowShapeStyle {
            fill_style: SnowFillStyle::CrossLine,
            ..SnowShapeStyle::default()
        };

        let rust_style: ShapeStyle = c_style.into();
        assert_eq!(rust_style.fill_style, FillStyle::CrossLine);

        let round_trip: SnowShapeStyle = rust_style.into();
        assert_eq!(round_trip.fill_style, SnowFillStyle::CrossLine);
    }

    #[test]
    fn reference_arrowheads_round_trip_through_c_abi() {
        for (rust, c) in [
            (Arrowhead::Square, SnowArrowhead::Square),
            (Arrowhead::InvertedTriangle, SnowArrowhead::InvertedTriangle),
        ] {
            assert_eq!(snow_arrowhead_from_rust(Some(rust)), c);
            assert_eq!(snow_arrowhead_to_rust(c), Some(rust));
        }
    }

    #[test]
    fn corner_radius_cursor_is_exposed_through_the_c_abi() {
        assert_eq!(
            snow_cursor_style_from_rust(CursorStyle::CornerRadius),
            SnowCursorStyle::CornerRadius
        );
    }

    #[test]
    fn hidden_cursor_is_exposed_through_the_c_abi() {
        assert_eq!(
            snow_cursor_style_from_rust(CursorStyle::Hidden),
            SnowCursorStyle::Hidden
        );
    }

    #[test]
    fn text_element_info_conversion_truncates_text_and_font_family_at_utf8_boundaries() {
        let (text_prefix, text) = boundary_test_string(SNOW_TEXT_UTF8_CAPACITY - 1);
        let (family_prefix, family) = boundary_test_string(SNOW_FONT_FAMILY_UTF8_CAPACITY - 1);

        let info = snow_text_element_info_from_rust(TextElementInfo {
            id: ElementId {
                index: 7,
                generation: 3,
            },
            center: Point { x: 1.0, y: 2.0 },
            width: 100.0,
            height: 30.0,
            rotation: 0.0,
            text,
            font_size: 21.0,
            font_family: Some(family),
            auto_resize: true,
            measure_natural_width: false,
        });

        assert_eq!(info.text_utf8_len, (SNOW_TEXT_UTF8_CAPACITY - 1) as u32);
        assert_eq!(info.text_truncated, 1);
        assert_eq!(
            string_from_c_char_field(&info.text_utf8, info.text_utf8_len),
            Some(text_prefix)
        );
        assert_eq!(
            info.font_family_utf8_len,
            (SNOW_FONT_FAMILY_UTF8_CAPACITY - 1) as u32
        );
        assert_eq!(info.font_family_truncated, 1);
        assert_eq!(
            string_from_c_char_field(&info.font_family_utf8, info.font_family_utf8_len),
            Some(family_prefix)
        );
    }

    #[test]
    fn style_conversion_truncates_font_family_at_utf8_boundaries() {
        let (family_prefix, family) = boundary_test_string(SNOW_FONT_FAMILY_UTF8_CAPACITY - 1);

        let text_style: SnowTextStyle = TextStyle {
            color: ColorRgba8::default(),
            font_size: 21.0,
            font_family: Some(family.clone()),
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            corner_radii: CornerRadii::default(),
            horizontal_align: TextHorizontalAlign::Left,
            vertical_align: TextVerticalAlign::Center,
            opacity: 1.0,
        }
        .into();

        assert_eq!(
            text_style.font_family_utf8_len,
            (SNOW_FONT_FAMILY_UTF8_CAPACITY - 1) as u32
        );
        assert_eq!(text_style.font_family_truncated, 1);
        assert_eq!(
            string_from_c_char_field(
                &text_style.font_family_utf8,
                text_style.font_family_utf8_len
            ),
            Some(family_prefix.clone())
        );

        let serial_style: SnowSerialNumberStyle = SerialNumberStyle {
            number: 1,
            color: ColorRgba8::default(),
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            font_size: 16.0,
            font_family: Some(family),
            stroke_width: 2.0,
            stroke_style: StrokeStyle::Solid,
            opacity: 1.0,
        }
        .into();

        assert_eq!(
            serial_style.font_family_utf8_len,
            (SNOW_FONT_FAMILY_UTF8_CAPACITY - 1) as u32
        );
        assert_eq!(serial_style.font_family_truncated, 1);
        assert_eq!(
            string_from_c_char_field(
                &serial_style.font_family_utf8,
                serial_style.font_family_utf8_len
            ),
            Some(family_prefix)
        );
    }

    #[test]
    fn text_style_from_c_bounds_reported_font_family_length() {
        let mut style = SnowTextStyle::default();
        style.font_family_utf8.fill(b'a' as std::ffi::c_char);
        style.font_family_utf8_len = (SNOW_FONT_FAMILY_UTF8_CAPACITY + 10) as u32;

        let converted: TextStyle = style.into();

        assert_eq!(
            converted.font_family,
            Some("a".repeat(SNOW_FONT_FAMILY_UTF8_CAPACITY))
        );
    }

    #[test]
    fn complete_runtime_style_defaults_round_trip_through_c_abi() {
        let mut expected = StyleDefaults::default();
        expected.editor.rectangle.fill = ColorRgba8 {
            r: 1,
            g: 2,
            b: 3,
            a: 0,
        };
        expected.editor.rectangle.stroke_width = 3.0;
        expected.editor.arrow.stroke_width = 4.0;
        expected.editor.line.stroke_width = 5.0;
        expected.editor.free_draw.stroke_width = 6.0;
        expected.editor.rectangle_highlight.stroke_width = 7.0;
        expected.editor.pen_highlight.stroke_width = 8.0;
        expected.editor.rectangle_filter.strength = 0.41;
        expected.editor.rectangle_filter.stroke_width = 9.0;
        expected.editor.pen_filter.strength = 0.42;
        expected.editor.pen_filter.stroke_width = 10.0;
        expected.editor.text.font_family = Some("C Text Font".to_owned());
        expected.editor.serial_number.font_family = Some("C Serial Font".to_owned());
        expected.watermark.text = "C watermark".to_owned();
        expected.watermark.font_family = "C Watermark Font".to_owned();
        expected.watermark.opacity = 0.24;
        expected.spotlight.opacity = 0.62;

        let c_defaults: SnowStyleDefaults = expected.clone().into();
        assert_eq!(c_defaults.rectangle.fill.a, 0);
        assert_eq!(c_defaults.text.font_family_truncated, 0);
        assert_eq!(c_defaults.serial_number.font_family_truncated, 0);
        let c_config = SnowRuntimeConfig {
            style_defaults: &c_defaults,
        };
        let converted = runtime_config_from_c(Some(&c_config)).unwrap();

        assert_eq!(converted.style_defaults, expected);
    }

    #[test]
    fn runtime_config_rejects_truncated_profile_strings() {
        let mut defaults: SnowStyleDefaults = StyleDefaults::default().into();
        defaults.text.font_family_truncated = 1;
        let config = SnowRuntimeConfig {
            style_defaults: &defaults,
        };

        assert_eq!(
            runtime_config_from_c(Some(&config)),
            Err(SnowError::InvalidArgument)
        );
    }
}

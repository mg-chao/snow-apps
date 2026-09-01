use super::*;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowZoomFocus {
    Pointer = 0,
    Center = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowActiveTool {
    Select = 0,
    Shape = 1,
    Arrow = 2,
    Text = 3,
    SerialNumber = 4,
    Line = 5,
    FreeDraw = 6,
    RectangleHighlight = 7,
    Eraser = 8,
    RectangleFilter = 9,
    Watermark = 10,
    PenHighlight = 11,
    PenFilter = 12,
    Spotlight = 13,
}

impl SnowActiveTool {
    #[allow(non_upper_case_globals)]
    pub const Filter: Self = Self::RectangleFilter;
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowStyleToolbarSource {
    DefaultRectangle = 0,
    SelectedRectangle = 1,
    DefaultArrow = 2,
    SelectedArrow = 3,
    DefaultText = 4,
    SelectedText = 5,
    DefaultSerialNumber = 6,
    SelectedSerialNumber = 7,
    DefaultLine = 8,
    SelectedLine = 9,
    DefaultFreeDraw = 10,
    SelectedFreeDraw = 11,
    DefaultRectangleHighlight = 12,
    SelectedRectangleHighlight = 13,
    Eraser = 14,
    DefaultRectangleFilter = 15,
    SelectedRectangleFilter = 16,
    Watermark = 17,
    DefaultPenHighlight = 18,
    SelectedPenHighlight = 19,
    DefaultPenFilter = 20,
    SelectedPenFilter = 21,
    DefaultSpotlight = 22,
    SelectedSpotlight = 23,
}

impl SnowStyleToolbarSource {
    #[allow(non_upper_case_globals)]
    pub const DefaultFilter: Self = Self::DefaultRectangleFilter;
    #[allow(non_upper_case_globals)]
    pub const SelectedFilter: Self = Self::SelectedRectangleFilter;
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SnowFilterType {
    #[default]
    Mosaic = 0,
    GaussianBlur = 1,
    Grayscale = 2,
    Inversion = 3,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowFilterStyle {
    pub filter_type: SnowFilterType,
    pub strength: f64,
    pub opacity: f64,
    pub stroke_width: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SnowColorRgba8 {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowCornerRadii {
    pub top_left: f64,
    pub top_right: f64,
    pub bottom_right: f64,
    pub bottom_left: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowShapeStyle {
    pub fill: SnowColorRgba8,
    pub stroke: SnowColorRgba8,
    pub stroke_width: f64,
    pub corner_radii: SnowCornerRadii,
    pub start_arrowhead: SnowArrowhead,
    pub end_arrowhead: SnowArrowhead,
    pub stroke_style: SnowStrokeStyle,
    pub arrow_type: SnowArrowType,
    pub fill_style: SnowFillStyle,
    pub opacity: f64,
    pub highlight_shape: SnowHighlightShape,
    pub shape: SnowRectangleShape,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowWatermarkConfig {
    pub color: SnowColorRgba8,
    pub text_utf8_len: u32,
    pub text_utf8: [std::ffi::c_char; SNOW_WATERMARK_TEXT_UTF8_CAPACITY],
    pub font_size: f64,
    pub font_family_utf8_len: u32,
    pub font_family_utf8: [std::ffi::c_char; SNOW_FONT_FAMILY_UTF8_CAPACITY],
    pub angle: f64,
    pub gap: f64,
    pub opacity: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowSpotlightConfig {
    pub color: SnowColorRgba8,
    pub opacity: f64,
}

impl Default for SnowWatermarkConfig {
    fn default() -> Self {
        Self {
            color: SnowColorRgba8::default(),
            text_utf8_len: 0,
            text_utf8: [0; SNOW_WATERMARK_TEXT_UTF8_CAPACITY],
            font_size: 12.0,
            font_family_utf8_len: 0,
            font_family_utf8: [0; SNOW_FONT_FAMILY_UTF8_CAPACITY],
            angle: 30.0,
            gap: 56.0,
            opacity: 0.16,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SnowHighlightShape {
    #[default]
    Rectangle = 0,
    Ellipse = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SnowRectangleShape {
    #[default]
    Rectangle = 0,
    Ellipse = 1,
    Diamond = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowRectangleShapeStyle {
    pub fill: SnowColorRgba8,
    pub fill_style: SnowFillStyle,
    pub stroke: SnowColorRgba8,
    pub stroke_width: f64,
    pub stroke_style: SnowStrokeStyle,
    pub corner_radii: SnowCornerRadii,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowShapeKind {
    Rectangle = 0,
    Arrow = 1,
    Line = 2,
    FreeDraw = 3,
    RectangleHighlight = 4,
    PenHighlight = 5,
    Spotlight = 6,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowArrowStyle {
    pub stroke: SnowColorRgba8,
    pub stroke_width: f64,
    pub start_arrowhead: SnowArrowhead,
    pub end_arrowhead: SnowArrowhead,
    pub stroke_style: SnowStrokeStyle,
    pub arrow_type: SnowArrowType,
    pub reserved0: [u8; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowTextStyle {
    pub color: SnowColorRgba8,
    pub font_size: f64,
    pub fill: SnowColorRgba8,
    pub fill_style: SnowFillStyle,
    pub stroke: SnowColorRgba8,
    pub stroke_width: f64,
    pub corner_radii: SnowCornerRadii,
    pub horizontal_align: SnowTextHorizontalAlign,
    pub vertical_align: SnowTextVerticalAlign,
    pub opacity: f64,
    pub reserved0: [u8; 4],
    pub font_family_utf8_len: u32,
    pub font_family_truncated: u8,
    pub reserved1: [u8; 3],
    pub font_family_utf8: [std::ffi::c_char; SNOW_FONT_FAMILY_UTF8_CAPACITY],
}

#[cfg(test)]
mod spotlight_abi_tests {
    use super::*;

    #[test]
    fn spotlight_values_are_append_only_and_config_layout_is_stable() {
        assert_eq!(SnowActiveTool::PenFilter as i32, 12);
        assert_eq!(SnowActiveTool::Spotlight as i32, 13);
        assert_eq!(SnowStyleToolbarSource::SelectedPenFilter as i32, 21);
        assert_eq!(SnowStyleToolbarSource::DefaultSpotlight as i32, 22);
        assert_eq!(SnowStyleToolbarSource::SelectedSpotlight as i32, 23);
        assert_eq!(SnowShapeKind::PenHighlight as i32, 5);
        assert_eq!(SnowShapeKind::Spotlight as i32, 6);
        assert_eq!(std::mem::size_of::<SnowSpotlightConfig>(), 16);
        assert_eq!(std::mem::offset_of!(SnowSpotlightConfig, color), 0);
        assert_eq!(std::mem::offset_of!(SnowSpotlightConfig, opacity), 8);
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowSerialNumberStyle {
    pub number: i64,
    pub color: SnowColorRgba8,
    pub fill: SnowColorRgba8,
    pub fill_style: SnowFillStyle,
    pub font_size: f64,
    pub stroke_width: f64,
    pub stroke_style: SnowStrokeStyle,
    pub opacity: f64,
    pub reserved0: [u8; 4],
    pub font_family_utf8_len: u32,
    pub font_family_truncated: u8,
    pub reserved1: [u8; 3],
    pub font_family_utf8: [std::ffi::c_char; SNOW_FONT_FAMILY_UTF8_CAPACITY],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowStyleToolbarState {
    pub source: SnowStyleToolbarSource,
    pub reserved0: u32,
    pub shape_style: SnowShapeStyle,
    pub text_style: SnowTextStyle,
    pub serial_number_style: SnowSerialNumberStyle,
    pub text_style_mixed: u32,
    pub serial_number_style_mixed: u32,
    pub shape_style_mixed: u32,
    pub filter_style: SnowFilterStyle,
    pub filter_style_mixed: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowStyleDefaults {
    pub rectangle: SnowShapeStyle,
    pub arrow: SnowShapeStyle,
    pub line: SnowShapeStyle,
    pub free_draw: SnowShapeStyle,
    pub rectangle_highlight: SnowShapeStyle,
    pub pen_highlight: SnowShapeStyle,
    pub rectangle_filter: SnowFilterStyle,
    pub pen_filter: SnowFilterStyle,
    pub text: SnowTextStyle,
    pub serial_number: SnowSerialNumberStyle,
    pub watermark: SnowWatermarkConfig,
    pub spotlight: SnowSpotlightConfig,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowRuntimeConfig {
    pub style_defaults: *const SnowStyleDefaults,
}

impl Default for SnowRuntimeConfig {
    fn default() -> Self {
        Self {
            style_defaults: std::ptr::null(),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowSerialNumberToolbarState {
    pub visible: u8,
    pub can_decrease: u8,
    pub can_increase: u8,
    pub can_create_text: u8,
    pub reserved0: [u8; 4],
    pub left: f64,
    pub top: f64,
    pub width: f64,
    pub height: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SnowHistoryState {
    pub can_undo: u8,
    pub can_redo: u8,
    pub reserved0: [u8; 6],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SnowModifiers {
    pub ctrl: u8,
    pub shift: u8,
    pub alt: u8,
    pub meta: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowSnapConfig {
    pub enabled: u8,
    pub enable_point_snaps: u8,
    pub enable_gap_snaps: u8,
    pub show_guides: u8,
    pub show_gap_size: u8,
    pub reserved0: [u8; 3],
    pub distance: f64,
    pub line_color: SnowColorRgba8,
    pub line_width: f64,
    pub marker_size: f64,
    pub gap_dash_length: f64,
    pub gap_dash_gap: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowGridConfig {
    pub enabled: u8,
    pub reserved0: [u8; 7],
    pub size: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowEngineConfig {
    pub min_zoom: f64,
    pub max_zoom: f64,
    pub zoom_focus: SnowZoomFocus,
    pub wheel_zoom_sensitivity: f64,
    pub clear_color: SnowColorRgba8,
    pub snap: SnowSnapConfig,
    pub grid: SnowGridConfig,
    pub enable_pointer_capture: u8,
    pub reserved: [u8; 7],
}

impl Default for SnowSnapConfig {
    fn default() -> Self {
        Self {
            enabled: 0,
            enable_point_snaps: 1,
            enable_gap_snaps: 1,
            show_guides: 1,
            show_gap_size: 0,
            reserved0: [0; 3],
            distance: 8.0,
            line_color: SnowColorRgba8 {
                r: 255,
                g: 107,
                b: 107,
                a: 255,
            },
            line_width: 1.0,
            marker_size: 8.0,
            gap_dash_length: 4.0,
            gap_dash_gap: 4.0,
        }
    }
}

impl Default for SnowShapeStyle {
    fn default() -> Self {
        Self {
            fill: SnowColorRgba8::default(),
            stroke: SnowColorRgba8::default(),
            stroke_width: 0.0,
            corner_radii: SnowCornerRadii::default(),
            start_arrowhead: SnowArrowhead::None,
            end_arrowhead: SnowArrowhead::None,
            stroke_style: SnowStrokeStyle::Solid,
            arrow_type: SnowArrowType::Straight,
            fill_style: SnowFillStyle::Solid,
            opacity: 1.0,
            highlight_shape: SnowHighlightShape::Rectangle,
            shape: SnowRectangleShape::Rectangle,
        }
    }
}

impl Default for SnowRectangleShapeStyle {
    fn default() -> Self {
        Self {
            fill: SnowColorRgba8::default(),
            fill_style: SnowFillStyle::Solid,
            stroke: SnowColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: SnowStrokeStyle::Solid,
            corner_radii: SnowCornerRadii::default(),
        }
    }
}

impl Default for SnowArrowStyle {
    fn default() -> Self {
        Self {
            stroke: SnowColorRgba8::default(),
            stroke_width: 0.0,
            start_arrowhead: SnowArrowhead::None,
            end_arrowhead: SnowArrowhead::None,
            stroke_style: SnowStrokeStyle::Solid,
            arrow_type: SnowArrowType::Straight,
            reserved0: [0; 4],
        }
    }
}

impl Default for SnowTextStyle {
    fn default() -> Self {
        Self {
            color: SnowColorRgba8::default(),
            font_size: 0.0,
            fill: SnowColorRgba8::default(),
            fill_style: SnowFillStyle::Solid,
            stroke: SnowColorRgba8::default(),
            stroke_width: 0.0,
            corner_radii: SnowCornerRadii::default(),
            horizontal_align: SnowTextHorizontalAlign::Left,
            vertical_align: SnowTextVerticalAlign::Center,
            opacity: 1.0,
            reserved0: [0; 4],
            font_family_utf8_len: 0,
            font_family_truncated: 0,
            reserved1: [0; 3],
            font_family_utf8: [0; SNOW_FONT_FAMILY_UTF8_CAPACITY],
        }
    }
}

impl Default for SnowSerialNumberStyle {
    fn default() -> Self {
        Self {
            number: 0,
            color: SnowColorRgba8::default(),
            fill: SnowColorRgba8::default(),
            fill_style: SnowFillStyle::Solid,
            font_size: 0.0,
            stroke_width: 0.0,
            stroke_style: SnowStrokeStyle::Solid,
            opacity: 1.0,
            reserved0: [0; 4],
            font_family_utf8_len: 0,
            font_family_truncated: 0,
            reserved1: [0; 3],
            font_family_utf8: [0; SNOW_FONT_FAMILY_UTF8_CAPACITY],
        }
    }
}

impl Default for SnowGridConfig {
    fn default() -> Self {
        Self {
            enabled: 0,
            reserved0: [0; 7],
            size: 20.0,
        }
    }
}

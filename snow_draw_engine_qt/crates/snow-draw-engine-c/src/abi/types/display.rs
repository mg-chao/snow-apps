use super::*;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowSceneDisplayItemKind {
    Unknown = 0,
    DrawRect = 1,
    Stroke = 2,
    Text = 3,
    Image = 4,
    Arrow = 5,
    SerialNumber = 6,
    SerialNumberConnector = 7,
    Filter = 8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowTextHorizontalAlign {
    Left = 0,
    Center = 1,
    Right = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowTextVerticalAlign {
    Top = 0,
    Center = 1,
    Bottom = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowFillStyle {
    Line = 0,
    CrossLine = 1,
    Solid = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowStrokeStyle {
    Solid = 0,
    Dashed = 1,
    Dotted = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SnowDisplayRectShape {
    #[default]
    Rectangle = 0,
    Ellipse = 1,
    Diamond = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SnowBlendMode {
    #[default]
    Normal = 0,
    Multiply = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowFilterRenderSpec {
    pub filter_type: u32,
    pub reserved0: u32,
    pub strength: f64,
    pub mosaic_block_size: f64,
    pub blur_sigma: f64,
    pub sampling_radius: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowSceneDisplayItem {
    pub kind: SnowSceneDisplayItemKind,
    pub blend_mode: SnowBlendMode,
    pub element_id: SnowElementId,
    pub center_x: f64,
    pub center_y: f64,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub fill: SnowColorRgba8,
    pub stroke: SnowColorRgba8,
    pub text_color: SnowColorRgba8,
    pub stroke_width: f64,
    pub corner_radii: SnowCornerRadii,
    pub arrow_point_count: u32,
    pub arrow_type: SnowArrowType,
    pub is_free_draw: u8,
    pub reserved1: [u8; 2],
    pub arrow_start_head: SnowArrowhead,
    pub arrow_end_head: SnowArrowhead,
    pub arrow_stroke_style: SnowStrokeStyle,
    pub bound_text_element_index: u32,
    pub arrow_points: *const SnowArrowPoint,
    pub arrow_path_command_count: u32,
    pub arrowhead_primitive_count: u32,
    pub arrow_path_commands: *const SnowArrowPathCommand,
    pub arrowhead_primitives: *const SnowArrowheadPrimitive,
    pub font_size: f64,
    pub opacity: f64,
    pub serial_number: i64,
    pub text_utf8_len: u32,
    pub text_horizontal_align: SnowTextHorizontalAlign,
    pub text_vertical_align: SnowTextVerticalAlign,
    pub fill_style: SnowFillStyle,
    pub stroke_style: SnowStrokeStyle,
    pub has_bound_text_element: u8,
    pub rect_shape: u8,
    pub reserved2: [u8; 2],
    pub bound_text_element_generation: u32,
    pub filter: SnowFilterRenderSpec,
    pub text_utf8: *const std::ffi::c_char,
    pub font_family_utf8_len: u32,
    pub font_family_utf8: *const std::ffi::c_char,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowOverlayDisplayItemKind {
    DrawRect = 1,
    SnapGuide = 2,
    FocusConnection = 3,
    PenFilterContour = 4,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowOverlayRectKind {
    Unspecified = 0,
    SelectionMarquee = 1,
    SelectionCandidateFrame = 2,
    SelectionFrame = 3,
    SelectionMultiFrame = 4,
    SelectionResizeHandle = 5,
    SelectionRotationHandle = 6,
    SelectionCornerRadiusHandle = 7,
    ArrowEndpointHandle = 8,
    ArrowFocusHandle = 9,
    ArrowSegmentHandle = 10,
    TextActualFrame = 11,
    TextHoverUnderline = 12,
    EraserCursor = 13,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowSnapGuideKind {
    Point = 0,
    Gap = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowSnapGuideAxis {
    Horizontal = 0,
    Vertical = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowOverlayDisplayItem {
    pub kind: SnowOverlayDisplayItemKind,
    pub rect_kind: SnowOverlayRectKind,
    pub snap_guide_kind: SnowSnapGuideKind,
    pub snap_guide_axis: SnowSnapGuideAxis,
    pub snap_marker_count: u8,
    pub snap_has_label: u8,
    pub reserved0: u16,
    pub center_x: f64,
    pub center_y: f64,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub fill: SnowColorRgba8,
    pub stroke: SnowColorRgba8,
    pub stroke_width: f64,
    pub corner_radii: SnowCornerRadii,
    pub arrow_point_count: u32,
    pub arrow_type: SnowArrowType,
    pub reserved1: [u8; 3],
    pub arrow_start_head: SnowArrowhead,
    pub arrow_end_head: SnowArrowhead,
    pub arrow_stroke_style: SnowStrokeStyle,
    pub fill_style: SnowFillStyle,
    pub arrow_points: *const SnowArrowPoint,
    pub arrow_path_command_count: u32,
    pub arrowhead_primitive_count: u32,
    pub arrow_path_commands: *const SnowArrowPathCommand,
    pub arrowhead_primitives: *const SnowArrowheadPrimitive,
    pub snap_start_x: f64,
    pub snap_start_y: f64,
    pub snap_end_x: f64,
    pub snap_end_y: f64,
    pub snap_marker0_x: f64,
    pub snap_marker0_y: f64,
    pub snap_marker1_x: f64,
    pub snap_marker1_y: f64,
    pub snap_label: f64,
    pub snap_color: SnowColorRgba8,
    pub snap_line_width: f64,
    pub snap_marker_size: f64,
    pub snap_gap_dash_length: f64,
    pub snap_gap_dash_gap: f64,
}

impl Default for SnowInteractionOutput {
    fn default() -> Self {
        Self {
            consumed: 0,
            reserved0: [0; 3],
            capture_kind: SnowPointerCaptureCommandKind::NoChange,
            capture_pointer_id: 0,
            cursor_kind: SnowCursorCommandKind::NoChange,
            cursor_style: SnowCursorStyle::Default,
        }
    }
}

impl Default for SnowSceneDisplayItem {
    fn default() -> Self {
        Self {
            kind: SnowSceneDisplayItemKind::Unknown,
            blend_mode: SnowBlendMode::Normal,
            element_id: SnowElementId::default(),
            center_x: 0.0,
            center_y: 0.0,
            width: 0.0,
            height: 0.0,
            rotation: 0.0,
            fill: SnowColorRgba8::default(),
            stroke: SnowColorRgba8::default(),
            text_color: SnowColorRgba8::default(),
            stroke_width: 0.0,
            corner_radii: SnowCornerRadii::default(),
            arrow_point_count: 0,
            arrow_type: SnowArrowType::Straight,
            is_free_draw: 0,
            reserved1: [0; 2],
            arrow_start_head: SnowArrowhead::None,
            arrow_end_head: SnowArrowhead::None,
            arrow_stroke_style: SnowStrokeStyle::Solid,
            bound_text_element_index: 0,
            arrow_points: std::ptr::null(),
            arrow_path_command_count: 0,
            arrowhead_primitive_count: 0,
            arrow_path_commands: std::ptr::null(),
            arrowhead_primitives: std::ptr::null(),
            font_size: 0.0,
            opacity: 1.0,
            serial_number: 0,
            text_utf8_len: 0,
            text_horizontal_align: SnowTextHorizontalAlign::Left,
            text_vertical_align: SnowTextVerticalAlign::Center,
            fill_style: SnowFillStyle::Solid,
            stroke_style: SnowStrokeStyle::Solid,
            has_bound_text_element: 0,
            rect_shape: SnowDisplayRectShape::Rectangle as u8,
            reserved2: [0; 2],
            bound_text_element_generation: 0,
            filter: SnowFilterRenderSpec::default(),
            text_utf8: std::ptr::null(),
            font_family_utf8_len: 0,
            font_family_utf8: std::ptr::null(),
        }
    }
}

impl Default for SnowOverlayDisplayItem {
    fn default() -> Self {
        Self {
            kind: SnowOverlayDisplayItemKind::DrawRect,
            rect_kind: SnowOverlayRectKind::Unspecified,
            snap_guide_kind: SnowSnapGuideKind::Point,
            snap_guide_axis: SnowSnapGuideAxis::Horizontal,
            snap_marker_count: 0,
            snap_has_label: 0,
            reserved0: 0,
            center_x: 0.0,
            center_y: 0.0,
            width: 0.0,
            height: 0.0,
            rotation: 0.0,
            fill: SnowColorRgba8::default(),
            stroke: SnowColorRgba8::default(),
            stroke_width: 0.0,
            corner_radii: SnowCornerRadii::default(),
            arrow_point_count: 0,
            arrow_type: SnowArrowType::Straight,
            reserved1: [0; 3],
            arrow_start_head: SnowArrowhead::None,
            arrow_end_head: SnowArrowhead::None,
            arrow_stroke_style: SnowStrokeStyle::Solid,
            fill_style: SnowFillStyle::Solid,
            arrow_points: std::ptr::null(),
            arrow_path_command_count: 0,
            arrowhead_primitive_count: 0,
            arrow_path_commands: std::ptr::null(),
            arrowhead_primitives: std::ptr::null(),
            snap_start_x: 0.0,
            snap_start_y: 0.0,
            snap_end_x: 0.0,
            snap_end_y: 0.0,
            snap_marker0_x: 0.0,
            snap_marker0_y: 0.0,
            snap_marker1_x: 0.0,
            snap_marker1_y: 0.0,
            snap_label: 0.0,
            snap_color: SnowColorRgba8::default(),
            snap_line_width: 0.0,
            snap_marker_size: 0.0,
            snap_gap_dash_length: 0.0,
            snap_gap_dash_gap: 0.0,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn display_views_are_compact() {
        let scene_size = std::mem::size_of::<SnowSceneDisplayItem>();
        let overlay_size = std::mem::size_of::<SnowOverlayDisplayItem>();
        assert!(
            scene_size <= 320,
            "scene display view is {scene_size} bytes"
        );
        assert!(
            overlay_size <= 384,
            "overlay display view is {overlay_size} bytes"
        );
    }
}

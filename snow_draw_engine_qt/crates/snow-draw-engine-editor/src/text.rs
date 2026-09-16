mod commit;
mod resize;
mod serial;

use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{ColorRgba8, CornerRadii, ErrorCode};
use snow_draw_engine_document::{
    ElementId, FillStyle, SerialNumberType, StrokeStyle, TextData, TextHorizontalAlign,
    TextLayoutSize, TextVerticalAlign, validate_text_layout_size,
};

pub use commit::{TextCommitTarget, TextDraftCommit};
pub use resize::TextResizeMeasurementRequest;

pub(crate) use commit::{text_with_committed_draft, text_with_style_attributes};
pub(crate) use resize::{
    TextResizeLayoutOverride, TextSelectionResizeHandle, text_resize_layout_override_matches_rect,
    text_resize_measurement_font_size, text_resize_measurement_requested_values,
    text_with_selection_rect,
};
pub(crate) use serial::{SerialNumberTextCreationRequest, create_serial_number_text_creation_plan};

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct TextStyle {
    pub color: ColorRgba8,
    pub font_size: f64,
    pub font_family: Option<String>,
    pub fill: ColorRgba8,
    pub fill_style: FillStyle,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub corner_radii: CornerRadii,
    pub horizontal_align: TextHorizontalAlign,
    pub vertical_align: TextVerticalAlign,
    pub opacity: f64,
}

impl TextStyle {
    pub(crate) fn from_text(text: &TextData) -> Self {
        Self {
            color: text.color,
            font_size: text.font_size,
            font_family: text.font_family.clone(),
            fill: text.fill,
            fill_style: text.fill_style,
            stroke: text.stroke,
            stroke_width: text.stroke_width,
            corner_radii: text.corner_radii,
            horizontal_align: text.horizontal_align,
            vertical_align: text.vertical_align,
            opacity: text.opacity,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TextLayoutOverride {
    /// Text element whose layout was measured by the host renderer.
    pub id: ElementId,
    /// Exact host-renderer measurement to apply while changing text style.
    pub size: TextLayoutSize,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TextPreviewFontSize {
    pub id: ElementId,
    pub font_size: f64,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct SerialNumberStyle {
    pub number: i64,
    #[serde(default, rename = "type")]
    pub serial_number_type: SerialNumberType,
    pub color: ColorRgba8,
    pub fill: ColorRgba8,
    pub fill_style: FillStyle,
    pub font_size: f64,
    pub font_family: Option<String>,
    pub stroke_width: f64,
    pub stroke_style: StrokeStyle,
    pub opacity: f64,
}

pub(crate) fn text_layout_override_size(
    layouts: &[TextLayoutOverride],
    id: ElementId,
) -> Result<TextLayoutSize, ErrorCode> {
    layouts
        .iter()
        .find(|layout| layout.id == id)
        .map(|layout| validate_text_layout_size(layout.size))
        .transpose()?
        .ok_or(ErrorCode::InvalidArgument)
}

use snow_draw_engine_core::Point;
use snow_draw_engine_document::{
    ElementId, InkBox, MIN_TEXT_FONT_SIZE, RectangleData, TextData, TextLayoutSize,
};

#[derive(Clone, Debug, PartialEq)]
pub struct TextResizeMeasurementRequest {
    /// Active text element that needs an exact host-renderer measurement.
    pub id: ElementId,
    pub center: Point<f64>,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub text: String,
    pub font_size: f64,
    pub font_family: Option<String>,
    pub auto_resize: bool,
    /// When true, the host should measure natural unwrapped width; otherwise it
    /// should measure wrapped height for `width`.
    pub measure_natural_width: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct TextResizeLayoutOverride {
    pub(crate) requested_width: f64,
    pub(crate) requested_height: f64,
    pub(crate) requested_font_size: f64,
    pub(crate) layout: TextLayoutSize,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct TextSelectionResizeHandle {
    pub(crate) x_sign: f64,
    pub(crate) y_sign: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct TextResizeMeasurementRequestedValues {
    pub(crate) width: f64,
    pub(crate) height: f64,
    pub(crate) font_size: f64,
}

/// Host-measured outcome of a single-text resize drag for the preview
/// rectangle: the requested font and the measured ink box. The commit stores
/// these so the committed element describes exactly what the frame paints.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct MeasuredTextResize {
    pub(crate) font_size: f64,
    pub(crate) ink: Option<InkBox>,
}

impl MeasuredTextResize {
    pub(crate) fn from_layout_override(layout_override: TextResizeLayoutOverride) -> Self {
        Self {
            font_size: layout_override.requested_font_size,
            ink: layout_override.layout.ink(),
        }
    }
}

pub(crate) fn text_resize_measurement_requested_values(
    requested_rect: RectangleData,
    requested_font_size: f64,
    existing_layout_override: Option<TextResizeLayoutOverride>,
) -> TextResizeMeasurementRequestedValues {
    if let Some(layout_override) = existing_layout_override
        && text_resize_layout_override_matches_rect(layout_override, requested_rect)
    {
        return TextResizeMeasurementRequestedValues {
            width: layout_override.requested_width,
            height: layout_override.requested_height,
            font_size: layout_override.requested_font_size.max(MIN_TEXT_FONT_SIZE),
        };
    }

    TextResizeMeasurementRequestedValues {
        width: requested_rect.width,
        height: requested_rect.height,
        font_size: requested_font_size.max(MIN_TEXT_FONT_SIZE),
    }
}

pub(crate) fn text_resize_measurement_font_size(
    original_font_size: f64,
    original_rect: RectangleData,
    preview_rect: RectangleData,
    changes_width_only: bool,
    layout_override: Option<TextResizeLayoutOverride>,
) -> Option<f64> {
    if changes_width_only {
        return Some(original_font_size.max(MIN_TEXT_FONT_SIZE));
    }
    if let Some(layout_override) = layout_override
        && text_resize_layout_override_matches_rect(layout_override, preview_rect)
    {
        return Some(layout_override.requested_font_size.max(MIN_TEXT_FONT_SIZE));
    }
    text_resize_font_size_from_height(
        original_font_size,
        original_rect.height,
        preview_rect.height,
    )
}

pub(crate) fn text_resize_layout_override_matches_rect(
    layout_override: TextResizeLayoutOverride,
    rect: RectangleData,
) -> bool {
    (layout_override.layout.width() - rect.width).abs() <= 1e-3
        && (layout_override.layout.height() - rect.height).abs() <= 1e-3
}

fn text_resize_font_size_from_height(
    original_font_size: f64,
    original_height: f64,
    preview_height: f64,
) -> Option<f64> {
    if original_height.is_finite()
        && original_height > f64::EPSILON
        && preview_height.is_finite()
        && preview_height > f64::EPSILON
    {
        let scale = preview_height / original_height;
        if scale.is_finite() && scale > f64::EPSILON {
            return Some((original_font_size * scale).max(MIN_TEXT_FONT_SIZE));
        }
    }
    None
}

pub(crate) fn text_with_selection_rect(
    text: &TextData,
    rect: RectangleData,
    resize_handle: Option<TextSelectionResizeHandle>,
    single_text_resize: bool,
    measured_resize: Option<MeasuredTextResize>,
) -> TextData {
    let single_text_resize_font_size = measured_resize.map(|measured| measured.font_size);
    let mut updated = text.clone();
    let size_changed = (updated.width() - rect.width).abs() > f64::EPSILON
        || (updated.height() - rect.height).abs() > f64::EPSILON;
    updated.center = rect.center;
    updated.layout = updated
        .layout
        .with_wrap(rect.width.max(0.0), rect.height.max(0.0));
    updated.rotation = rect.rotation;
    updated.corner_radii = rect.corner_radii;
    if size_changed {
        match (resize_handle, single_text_resize) {
            (Some(handle), true)
                if handle.x_sign.abs() > f64::EPSILON && handle.y_sign.abs() <= f64::EPSILON =>
            {
                updated.auto_resize = false;
            }
            (Some(_), false) => {
                scale_text_font_size_from_width(text, &mut updated);
            }
            (Some(handle), true) if handle.y_sign.abs() > f64::EPSILON => {
                if let Some(font_size) = single_text_resize_font_size
                    && font_size.is_finite()
                    && font_size > 0.0
                {
                    updated.font_size = font_size;
                } else {
                    scale_text_font_size_from_height(text, &mut updated);
                }
            }
            _ => {
                scale_text_font_size_from_height(text, &mut updated);
                updated.auto_resize = false;
            }
        }
        scale_text_content_from_font(text, &mut updated);
        if let Some(ink) = measured_resize.and_then(|measured| measured.ink) {
            // Keep the font-scaled approximation until the host's exact
            // re-measurement lands.
            updated.layout = updated.layout.with_ink(Some(ink));
        }
    }
    updated.font_size = updated.font_size.max(MIN_TEXT_FONT_SIZE);
    updated
}

/// The painted ink scales with the font: keep the measured content box
/// proportional through resize previews and commits until the host's exact
/// re-measurement lands. Width-only resizes keep the font and therefore the
/// ink; the rewrapped measurement replaces it asynchronously.
fn scale_text_content_from_font(original: &TextData, updated: &mut TextData) {
    if original.font_size.is_finite()
        && original.font_size > f64::EPSILON
        && updated.font_size.is_finite()
        && updated.font_size > f64::EPSILON
    {
        let scale = updated.font_size / original.font_size;
        if scale.is_finite() && scale > f64::EPSILON {
            updated.layout = updated.layout.scaled_ink(scale);
        }
    }
}

fn scale_text_font_size_from_width(original: &TextData, updated: &mut TextData) {
    if original.width().is_finite()
        && original.width() > f64::EPSILON
        && updated.width().is_finite()
        && updated.width() > f64::EPSILON
    {
        let scale = updated.width() / original.width();
        if scale.is_finite() && scale > f64::EPSILON {
            updated.font_size *= scale;
        }
    }
}

fn scale_text_font_size_from_height(original: &TextData, updated: &mut TextData) {
    if original.height().is_finite()
        && original.height() > f64::EPSILON
        && updated.height().is_finite()
        && updated.height() > f64::EPSILON
    {
        let scale = updated.height() / original.height();
        if scale.is_finite() && scale > f64::EPSILON {
            updated.font_size *= scale;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rect(width: f64, height: f64) -> RectangleData {
        RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Default::default(),
            width,
            height,
            rotation: 0.0,
            fill: Default::default(),
            fill_style: snow_draw_engine_document::FillStyle::Solid,
            stroke: Default::default(),
            stroke_width: 0.0,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: Default::default(),
            opacity: 1.0,
        }
    }

    fn text() -> TextData {
        TextData {
            font_size: 20.0,
            auto_resize: true,
            layout: TextLayoutSize::with_content(100.0, 40.0, 96.0, 40.0),
            ..TextData::default()
        }
    }

    #[test]
    fn width_only_single_text_resize_disables_auto_resize() {
        let updated = text_with_selection_rect(
            &text(),
            rect(120.0, 40.0),
            Some(TextSelectionResizeHandle {
                x_sign: 1.0,
                y_sign: 0.0,
            }),
            true,
            None,
        );

        assert!(!updated.auto_resize);
        assert_eq!(updated.font_size, 20.0);
        assert_eq!(updated.width(), 120.0);
    }

    #[test]
    fn height_single_text_resize_uses_measured_font_size() {
        let updated = text_with_selection_rect(
            &text(),
            rect(100.0, 80.0),
            Some(TextSelectionResizeHandle {
                x_sign: 0.0,
                y_sign: 1.0,
            }),
            true,
            Some(MeasuredTextResize {
                font_size: 30.0,
                ink: InkBox::new(144.0, 60.0),
            }),
        );

        assert_eq!(updated.font_size, 30.0);
        assert!(updated.auto_resize);
        assert_eq!(updated.height(), 80.0);
        assert_eq!(updated.layout.ink(), InkBox::new(144.0, 60.0));
    }

    #[test]
    fn height_resize_without_measurement_scales_content_with_font() {
        let updated = text_with_selection_rect(
            &text(),
            rect(100.0, 80.0),
            Some(TextSelectionResizeHandle {
                x_sign: 0.0,
                y_sign: 1.0,
            }),
            true,
            None,
        );

        // Font scales by the height ratio 80/40; the ink box follows until the
        // host's exact measurement lands.
        assert_eq!(updated.font_size, 40.0);
        assert_eq!(updated.layout.ink(), InkBox::new(192.0, 80.0));
    }

    #[test]
    fn move_keeps_measured_content_untouched() {
        let updated = text_with_selection_rect(&text(), rect(100.0, 40.0), None, false, None);

        assert_eq!(updated.layout.ink(), InkBox::new(96.0, 40.0));
    }
}

use crate::{
    ElementData, ElementRecord, FillStyle, FilterData, PenFilterData, RectangleData,
    SerialNumberData, SerialNumberTextConnection, StrokeStyle, TextData, TextHorizontalAlign,
    TextLayoutRect, TextLayoutSize, TextVerticalAlign, arrow_bounds, arrow_is_degenerate,
    validate_arrow, validate_free_draw,
};
use snow_draw_engine_core::{
    ColorRgba8, CornerRadii, DrawRect, ErrorCode, Point, rotated_rect_extents,
};

pub const MIN_TEXT_FONT_SIZE: f64 = 6.0;
pub const MIN_SERIAL_NUMBER_FONT_SIZE: f64 = MIN_TEXT_FONT_SIZE;
const MIN_SERIAL_NUMBER_BOUND_TEXT_GAP: f64 = 18.0;
const SERIAL_NUMBER_BOUND_TEXT_GAP_PER_FONT_SIZE: f64 = MIN_SERIAL_NUMBER_BOUND_TEXT_GAP / 21.0;
const TEXT_BACKGROUND_HORIZONTAL_PADDING_PER_LINE_HEIGHT: f64 = 0.32;
const TEXT_BACKGROUND_VERTICAL_PADDING_PER_LINE_HEIGHT: f64 = 0.1;

pub fn validate_rectangle(rect: &RectangleData) -> Result<(), ErrorCode> {
    let scalar_fields = [
        rect.center.x,
        rect.center.y,
        rect.width,
        rect.height,
        rect.rotation,
        rect.stroke_width,
        rect.corner_radii.top_left,
        rect.corner_radii.top_right,
        rect.corner_radii.bottom_right,
        rect.corner_radii.bottom_left,
        rect.opacity,
    ];
    if scalar_fields.iter().any(|value| !value.is_finite()) {
        return Err(ErrorCode::InvalidArgument);
    }
    if rect.width < 0.0
        || rect.height < 0.0
        || rect.stroke_width < 0.0
        || rect.corner_radii.top_left < 0.0
        || rect.corner_radii.top_right < 0.0
        || rect.corner_radii.bottom_right < 0.0
        || rect.corner_radii.bottom_left < 0.0
        || rect.opacity < 0.0
        || rect.opacity > 1.0
    {
        return Err(ErrorCode::InvalidArgument);
    }
    if rect.is_spotlight()
        && (rect.opacity != 1.0
            || rect.fill.a != 0
            || rect.stroke.a != 0
            || rect.stroke_width != 0.0
            || rect.corner_radii != CornerRadii::default())
    {
        return Err(ErrorCode::InvalidArgument);
    }
    if !corner_radii_fit_within_rect(rect.width, rect.height, rect.corner_radii) {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

pub fn validate_element_data(data: &ElementData) -> Result<(), ErrorCode> {
    match data {
        ElementData::Rectangle(rect) => validate_rectangle(rect),
        ElementData::Filter(filter) => validate_filter(filter),
        ElementData::PenFilter(filter) => validate_pen_filter(filter),
        ElementData::Arrow(arrow) => validate_arrow(arrow),
        ElementData::FreeDraw(free_draw) => validate_free_draw(free_draw),
        ElementData::Text(text) => validate_text(text),
        ElementData::SerialNumber(serial) => validate_serial_number(serial),
    }
}

pub fn normalize_corner_radii(width: f64, height: f64, radii: CornerRadii) -> CornerRadii {
    if width <= 0.0 || height <= 0.0 {
        return CornerRadii::default();
    }

    let constraints = [
        scale_constraint(width, radii.top_left + radii.top_right),
        scale_constraint(width, radii.bottom_left + radii.bottom_right),
        scale_constraint(height, radii.top_left + radii.bottom_left),
        scale_constraint(height, radii.top_right + radii.bottom_right),
    ];
    let scale = constraints.into_iter().fold(1.0, f64::min).clamp(0.0, 1.0);
    radii.scaled(scale)
}

pub fn corner_radii_fit_within_rect(width: f64, height: f64, radii: CornerRadii) -> bool {
    let epsilon = 1e-9;
    radii.top_left + radii.top_right <= width + epsilon
        && radii.bottom_left + radii.bottom_right <= width + epsilon
        && radii.top_left + radii.bottom_left <= height + epsilon
        && radii.top_right + radii.bottom_right <= height + epsilon
}

fn scale_constraint(limit: f64, sum: f64) -> f64 {
    if sum <= 0.0 {
        1.0
    } else {
        (limit.max(0.0) / sum).min(1.0)
    }
}

pub fn rect_bounds(rect: &RectangleData) -> DrawRect {
    let (extent_x, extent_y) =
        rotated_rect_extents(rect.width, rect.height, rect.rotation, rect.stroke_width);
    DrawRect::new(
        rect.center.x - extent_x,
        rect.center.y - extent_y,
        rect.center.x + extent_x,
        rect.center.y + extent_y,
    )
}

impl Default for TextData {
    fn default() -> Self {
        Self {
            center: Point::default(),
            width: 1.0,
            height: 36.0,
            rotation: 0.0,
            text: String::new(),
            color: ColorRgba8 {
                r: 0xf4,
                g: 0x21,
                b: 0x2c,
                a: 0xff,
            },
            font_size: 30.0,
            font_family: None,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8 {
                r: 0xff,
                g: 0xcc,
                b: 0xc7,
                a: 0xff,
            },
            stroke_width: 0.0,
            corner_radii: CornerRadii::splat(6.0),
            horizontal_align: TextHorizontalAlign::Left,
            vertical_align: TextVerticalAlign::Center,
            auto_resize: true,
            opacity: 1.0,
        }
    }
}

impl Default for SerialNumberData {
    fn default() -> Self {
        Self {
            center: Point::default(),
            diameter: 21.0,
            rotation: 0.0,
            number: 1,
            color: ColorRgba8 {
                r: 0xf4,
                g: 0x21,
                b: 0x2c,
                a: 0xff,
            },
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            font_size: 24.0,
            font_family: None,
            stroke_width: 2.0,
            stroke_style: StrokeStyle::Solid,
            opacity: 1.0,
            text_element_id: None,
        }
    }
}

pub fn normalize_font_family(font_family: Option<String>) -> Option<String> {
    font_family
        .map(|value| value.trim().to_owned())
        .filter(|value| !value.is_empty())
}

pub fn text_line_height(font_size: f64) -> f64 {
    sanitize_non_negative(font_size).max(1.0) * 1.2
}

pub fn validate_text_layout_size(layout: TextLayoutSize) -> Result<TextLayoutSize, ErrorCode> {
    if !layout.width.is_finite()
        || !layout.height.is_finite()
        || layout.width <= 0.0
        || layout.height <= 0.0
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(layout)
}

pub fn resolve_text_layout_rect(text: &TextData) -> TextLayoutRect {
    TextLayoutRect {
        center: text.center,
        width: sanitize_positive(text.width, 1.0),
        height: sanitize_positive(text.height, text_line_height(text.font_size)),
        rotation: text.rotation,
    }
}

pub fn text_with_auto_resize_layout(
    text: &TextData,
    layout: TextLayoutSize,
) -> Result<TextData, ErrorCode> {
    let layout = validate_text_layout_size(layout)?;
    let mut updated = text.clone();
    if updated.auto_resize {
        let delta_x = match updated.horizontal_align {
            TextHorizontalAlign::Left => (layout.width - updated.width) / 2.0,
            TextHorizontalAlign::Center => 0.0,
            TextHorizontalAlign::Right => -(layout.width - updated.width) / 2.0,
        };
        let delta_y = (layout.height - updated.height) / 2.0;
        let cos = updated.rotation.cos();
        let sin = updated.rotation.sin();
        updated.center.x += cos * delta_x - sin * delta_y;
        updated.center.y += sin * delta_x + cos * delta_y;
        updated.width = layout.width;
        updated.height = layout.height;
    }
    Ok(updated)
}

pub fn text_with_content_and_layout(
    text: &TextData,
    content: impl Into<String>,
    layout: TextLayoutSize,
) -> Result<TextData, ErrorCode> {
    let layout = validate_text_layout_size(layout)?;
    let mut updated = if text.auto_resize {
        text_with_auto_resize_layout(text, layout)?
    } else {
        let mut updated = text.clone();
        let delta_y = (layout.height - updated.height) / 2.0;
        updated.center.x -= updated.rotation.sin() * delta_y;
        updated.center.y += updated.rotation.cos() * delta_y;
        updated.height = layout.height;
        updated
    };
    updated.text = content.into();
    Ok(updated)
}

pub fn serial_number_bound_text_rect(
    serial: &SerialNumberData,
    text: &TextData,
    layout: TextLayoutSize,
) -> Result<TextLayoutRect, ErrorCode> {
    let layout = validate_text_layout_size(layout)?;
    let gap = (text.font_size.max(0.0) * SERIAL_NUMBER_BOUND_TEXT_GAP_PER_FONT_SIZE)
        .max(MIN_SERIAL_NUMBER_BOUND_TEXT_GAP)
        .max(resolve_serial_number_stroke_width(serial) * 2.0);
    Ok(TextLayoutRect {
        center: Point {
            x: serial.center.x + serial.diameter.max(0.0) / 2.0 + gap + layout.width / 2.0,
            y: serial.center.y,
        },
        width: layout.width,
        height: layout.height,
        rotation: 0.0,
    })
}

pub fn resolve_serial_number_text_connection(
    serial: &SerialNumberData,
    text: &TextData,
) -> Option<SerialNumberTextConnection> {
    let line_width = resolve_serial_number_stroke_width(serial);
    if line_width <= 0.0 || serial.diameter <= 0.0 || text.width <= 0.0 || text.height <= 0.0 {
        return None;
    }

    let serial_radius = serial.diameter.max(0.0) / 2.0;
    let serial_bounds = serial_number_bounds(serial);
    let text_bounds = text_bounds(text);
    if draw_rect_width(text_bounds) <= 0.0 || draw_rect_height(text_bounds) <= 0.0 {
        return None;
    }

    let center = serial.center;
    let attachment = serial_text_attachment(serial_bounds, text_bounds, center.x);
    let dx = attachment.anchor.x - center.x;
    let dy = attachment.anchor.y - center.y;
    let distance = (dx * dx + dy * dy).sqrt();
    let start_offset = serial_radius + 8.0;
    let half_line_width = line_width / 2.0;
    if distance <= start_offset + half_line_width {
        return None;
    }

    let ux = dx / distance;
    let uy = dy / distance;
    Some(SerialNumberTextConnection {
        start: Point {
            x: center.x + ux * start_offset,
            y: center.y + uy * start_offset,
        },
        end: Point {
            x: attachment.anchor.x - ux * half_line_width,
            y: attachment.anchor.y - uy * half_line_width,
        },
        text_baseline_start: attachment.text_baseline_start,
        text_baseline_end: attachment.text_baseline_end,
    })
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct SerialTextAttachment {
    anchor: Point<f64>,
    text_baseline_start: Option<Point<f64>>,
    text_baseline_end: Option<Point<f64>>,
}

fn serial_text_attachment(
    serial_bounds: DrawRect,
    text_bounds: DrawRect,
    serial_center_x: f64,
) -> SerialTextAttachment {
    let is_above = text_bounds.max_y < serial_bounds.min_y;
    let is_below = text_bounds.min_y > serial_bounds.max_y;
    let centered_horizontally =
        serial_center_x >= text_bounds.min_x && serial_center_x <= text_bounds.max_x;
    if centered_horizontally && is_above {
        return SerialTextAttachment {
            anchor: Point {
                x: serial_center_x,
                y: text_bounds.max_y,
            },
            text_baseline_start: None,
            text_baseline_end: None,
        };
    }
    if centered_horizontally && is_below {
        return SerialTextAttachment {
            anchor: Point {
                x: serial_center_x,
                y: text_bounds.min_y,
            },
            text_baseline_start: None,
            text_baseline_end: None,
        };
    }

    let anchor_x = serial_center_x.clamp(text_bounds.min_x, text_bounds.max_x);
    let baseline_y = text_bounds.max_y;
    SerialTextAttachment {
        anchor: Point {
            x: anchor_x,
            y: baseline_y,
        },
        text_baseline_start: Some(Point {
            x: text_bounds.min_x,
            y: baseline_y,
        }),
        text_baseline_end: Some(Point {
            x: text_bounds.max_x,
            y: baseline_y,
        }),
    }
}

pub fn text_bounds(text: &TextData) -> DrawRect {
    let can_paint_text = !text.text.is_empty() && text.font_size > 0.0;
    let stroke_outset = if can_paint_text && text.stroke.a != 0 {
        text.stroke_width.max(0.0) / 2.0
    } else {
        0.0
    };
    let (fill_outset_x, fill_outset_y) = if text.fill.a != 0 && text.font_size > 0.0 {
        let line_height = text_line_height(text.font_size);
        (
            line_height * TEXT_BACKGROUND_HORIZONTAL_PADDING_PER_LINE_HEIGHT,
            line_height * TEXT_BACKGROUND_VERTICAL_PADDING_PER_LINE_HEIGHT,
        )
    } else {
        (0.0, 0.0)
    };
    let paint_outset_x = stroke_outset.max(fill_outset_x);
    let paint_outset_y = stroke_outset.max(fill_outset_y);
    let (extent_x, extent_y) = rotated_rect_extents(
        text.width + paint_outset_x * 2.0,
        text.height + paint_outset_y * 2.0,
        text.rotation,
        0.0,
    );
    DrawRect::new(
        text.center.x - extent_x,
        text.center.y - extent_y,
        text.center.x + extent_x,
        text.center.y + extent_y,
    )
}

pub fn serial_number_bounds(serial: &SerialNumberData) -> DrawRect {
    let diameter = sanitize_non_negative(serial.diameter);
    let stroke_outset = resolve_serial_number_stroke_width(serial);
    let (extent_x, extent_y) =
        rotated_rect_extents(diameter, diameter, serial.rotation, stroke_outset);
    DrawRect::new(
        serial.center.x - extent_x,
        serial.center.y - extent_y,
        serial.center.x + extent_x,
        serial.center.y + extent_y,
    )
}

pub fn serial_number_rect_proxy(serial: &SerialNumberData) -> RectangleData {
    RectangleData {
        rectangle_kind: crate::RectangleElementKind::Rectangle,
        highlight_shape: crate::HighlightShape::Rectangle,
        center: serial.center,
        width: serial.diameter,
        height: serial.diameter,
        rotation: serial.rotation,
        fill: serial.fill,
        fill_style: serial.fill_style,
        stroke: serial.color,
        stroke_width: resolve_serial_number_stroke_width(serial),
        stroke_style: serial.stroke_style,
        corner_radii: CornerRadii::splat(serial.diameter.max(0.0) / 2.0),
        opacity: serial.opacity,
    }
}

pub fn serial_number_minimum_selection_scale(serial: &SerialNumberData) -> f64 {
    if serial.font_size.is_finite() && serial.font_size > f64::EPSILON {
        MIN_SERIAL_NUMBER_FONT_SIZE / serial.font_size
    } else {
        1.0
    }
}

pub fn serial_number_with_selection_rect(
    serial: &SerialNumberData,
    rect: RectangleData,
) -> SerialNumberData {
    let mut updated = serial.clone();
    updated.center = rect.center;
    let requested_diameter = rect.width.min(rect.height).max(0.0);
    let mut next_diameter = requested_diameter;
    if serial.diameter.is_finite()
        && serial.diameter > f64::EPSILON
        && requested_diameter.is_finite()
        && requested_diameter > f64::EPSILON
    {
        let scale = (requested_diameter / serial.diameter)
            .max(serial_number_minimum_selection_scale(serial));
        if scale.is_finite() && scale > f64::EPSILON {
            updated.font_size = serial.font_size * scale;
            next_diameter = serial.diameter * scale;
        }
    }
    updated.diameter = next_diameter;
    updated.rotation = rect.rotation;
    updated
}

pub fn resolve_serial_number_stroke_width(serial: &SerialNumberData) -> f64 {
    let scale = sanitize_non_negative(serial.font_size) / 16.0;
    sanitize_non_negative(serial.stroke_width * scale)
}

pub fn resolve_serial_number_style_diameter(number: i64, font_size: f64) -> f64 {
    resolve_serial_number_diameter(number, font_size, SerialNumberData::default().diameter)
}

pub fn serial_number_with_label_style(
    serial: &SerialNumberData,
    number: i64,
    font_size: f64,
) -> SerialNumberData {
    let mut updated = serial.clone();
    updated.number = number.max(0);
    updated.font_size = font_size;
    updated.diameter = resolve_serial_number_style_diameter(updated.number, updated.font_size);
    updated
}

pub fn resolve_serial_number_diameter(number: i64, font_size: f64, min_diameter: f64) -> f64 {
    let canonical_font_size = 16.0;
    let (width, height) = serial_number_label_size(number.max(0), canonical_font_size);
    let line_height = text_line_height(canonical_font_size);
    let base = width.max(height.max(line_height));
    let padding = line_height * 0.26;
    let scale = sanitize_non_negative(font_size).max(1.0) / canonical_font_size;
    sanitize_positive((base + padding * 2.0) * scale, min_diameter.max(0.0))
        .max(min_diameter.max(0.0))
}

fn serial_number_label_size(number: i64, font_size: f64) -> (f64, f64) {
    let digit_count = number.to_string().chars().count().max(1) as f64;
    let line_height = text_line_height(font_size);
    (digit_count * font_size.max(1.0) * 0.6, line_height)
}

pub fn text_hit_test(text: &TextData, point: Point<f64>, hit_tolerance: f64) -> bool {
    let width = sanitize_non_negative(text.width);
    let height = sanitize_non_negative(text.height);
    if width <= 0.0 || height <= 0.0 {
        return false;
    }
    let tolerance = hit_tolerance.max(0.0);
    let local = canvas_to_rect_local(text.center, text.rotation, point);
    local.x >= -width / 2.0 - tolerance
        && local.x <= width / 2.0 + tolerance
        && local.y >= -height / 2.0 - tolerance
        && local.y <= height / 2.0 + tolerance
}

pub fn serial_number_hit_test(
    serial: &SerialNumberData,
    point: Point<f64>,
    hit_tolerance: f64,
) -> bool {
    let radius = sanitize_non_negative(serial.diameter) / 2.0;
    if radius <= 0.0 {
        return false;
    }
    let effective_radius =
        radius + resolve_serial_number_stroke_width(serial) / 2.0 + hit_tolerance.max(0.0);
    if effective_radius <= 0.0 {
        return false;
    }
    let local = canvas_to_rect_local(serial.center, serial.rotation, point);
    local.x * local.x + local.y * local.y <= effective_radius * effective_radius + 1e-9
}

fn draw_rect_width(rect: DrawRect) -> f64 {
    rect.max_x - rect.min_x
}

fn draw_rect_height(rect: DrawRect) -> f64 {
    rect.max_y - rect.min_y
}

pub fn validate_text(text: &TextData) -> Result<(), ErrorCode> {
    let scalar_fields = [
        text.center.x,
        text.center.y,
        text.width,
        text.height,
        text.rotation,
        text.font_size,
        text.stroke_width,
        text.corner_radii.top_left,
        text.corner_radii.top_right,
        text.corner_radii.bottom_right,
        text.corner_radii.bottom_left,
        text.opacity,
    ];
    if scalar_fields.iter().any(|value| !value.is_finite()) {
        return Err(ErrorCode::InvalidArgument);
    }
    if text.width < 0.0
        || text.height < 0.0
        || text.font_size < MIN_TEXT_FONT_SIZE
        || text.stroke_width < 0.0
        || text.corner_radii.top_left < 0.0
        || text.corner_radii.top_right < 0.0
        || text.corner_radii.bottom_right < 0.0
        || text.corner_radii.bottom_left < 0.0
        || text.opacity < 0.0
        || text.opacity > 1.0
    {
        return Err(ErrorCode::InvalidArgument);
    }
    if normalize_font_family(text.font_family.clone()) != text.font_family {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

pub fn validate_serial_number(serial: &SerialNumberData) -> Result<(), ErrorCode> {
    let scalar_fields = [
        serial.center.x,
        serial.center.y,
        serial.diameter,
        serial.rotation,
        serial.font_size,
        serial.stroke_width,
        serial.opacity,
    ];
    if scalar_fields.iter().any(|value| !value.is_finite()) {
        return Err(ErrorCode::InvalidArgument);
    }
    if serial.diameter < 0.0
        || serial.number < 0
        || serial.font_size < MIN_SERIAL_NUMBER_FONT_SIZE
        || serial.stroke_width < 0.0
        || serial.opacity < 0.0
        || serial.opacity > 1.0
    {
        return Err(ErrorCode::InvalidArgument);
    }
    if normalize_font_family(serial.font_family.clone()) != serial.font_family {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

fn sanitize_non_negative(value: f64) -> f64 {
    if value.is_finite() {
        value.max(0.0)
    } else {
        0.0
    }
}

fn sanitize_positive(value: f64, fallback: f64) -> f64 {
    if value.is_finite() && value > 0.0 {
        value
    } else {
        fallback.max(0.0)
    }
}

pub fn rectangle_hit_test(rect: &RectangleData, point: Point<f64>, hit_tolerance: f64) -> bool {
    if rect.is_spotlight() {
        let local = canvas_to_rect_local(rect.center, rect.rotation, point);
        return local.x.abs() <= rect.width / 2.0 + hit_tolerance.max(0.0)
            && local.y.abs() <= rect.height / 2.0 + hit_tolerance.max(0.0);
    }
    if rect.is_highlight() {
        return highlight_hit_test(rect, point, hit_tolerance);
    }
    let local = canvas_to_rect_local(rect.center, rect.rotation, point);
    let hit_tolerance = hit_tolerance.max(0.0);
    if rect.highlight_shape != crate::HighlightShape::Rectangle {
        let half_width = rect.width / 2.0;
        let half_height = rect.height / 2.0;
        let contains = |outset: f64| {
            let x = half_width + outset;
            let y = half_height + outset;
            if x <= 0.0 || y <= 0.0 {
                return false;
            }
            match rect.highlight_shape {
                crate::HighlightShape::Ellipse => {
                    local.x * local.x / (x * x) + local.y * local.y / (y * y) <= 1.0
                }
                crate::HighlightShape::Diamond => local.x.abs() / x + local.y.abs() / y <= 1.0,
                crate::HighlightShape::Rectangle => unreachable!(),
            }
        };
        if rect.fill.a != 0 && contains(0.0) {
            return true;
        }
        if rect.stroke.a == 0 || rect.stroke_width <= 0.0 {
            return false;
        }
        let band = rect.stroke_width / 2.0 + hit_tolerance;
        return contains(band) && !contains(-band);
    }
    let fill_hit = rect.fill.a != 0
        && rounded_rect_contains_local_point(rect.width, rect.height, rect.corner_radii, local);
    fill_hit || rectangle_stroke_hit_test(rect, local, hit_tolerance)
}

pub fn filter_rect_proxy(filter: &FilterData) -> RectangleData {
    RectangleData {
        rectangle_kind: crate::RectangleElementKind::Rectangle,
        highlight_shape: crate::HighlightShape::Rectangle,
        center: filter.center,
        width: filter.width,
        height: filter.height,
        rotation: filter.rotation,
        fill: ColorRgba8::default(),
        fill_style: FillStyle::Solid,
        stroke: ColorRgba8::default(),
        stroke_width: 0.0,
        stroke_style: StrokeStyle::Solid,
        corner_radii: CornerRadii::default(),
        opacity: filter.opacity,
    }
}

pub fn filter_bounds(filter: &FilterData) -> DrawRect {
    rect_bounds(&filter_rect_proxy(filter))
}

pub fn filter_hit_test(filter: &FilterData, point: Point<f64>, hit_tolerance: f64) -> bool {
    let local = canvas_to_rect_local(filter.center, filter.rotation, point);
    let hit_tolerance = hit_tolerance.max(0.0);
    local.x.abs() <= filter.width / 2.0 + hit_tolerance
        && local.y.abs() <= filter.height / 2.0 + hit_tolerance
}

pub fn validate_filter(filter: &FilterData) -> Result<(), ErrorCode> {
    let scalar_fields = [
        filter.center.x,
        filter.center.y,
        filter.width,
        filter.height,
        filter.rotation,
        filter.opacity,
    ];
    if scalar_fields.iter().any(|value| !value.is_finite())
        || filter.width < 0.0
        || filter.height < 0.0
        || !(0.0..=1.0).contains(&filter.opacity)
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

pub fn pen_filter_bounds(filter: &PenFilterData) -> DrawRect {
    rect_bounds(&pen_filter_rect_proxy(filter))
}

/// Returns the rectangle used to represent a pen filter in selection and
/// interaction geometry. Unlike the persisted filter rectangle, this proxy is
/// the boundary of the painted outer contour, including the stroke width.
pub fn pen_filter_rect_proxy(filter: &PenFilterData) -> RectangleData {
    RectangleData {
        rectangle_kind: crate::RectangleElementKind::Rectangle,
        highlight_shape: crate::HighlightShape::Rectangle,
        center: filter.center(),
        width: filter.width + filter.stroke_width.max(0.0),
        height: filter.height + filter.stroke_width.max(0.0),
        rotation: filter.rotation,
        fill: ColorRgba8::default(),
        fill_style: FillStyle::Solid,
        stroke: ColorRgba8::default(),
        stroke_width: 0.0,
        stroke_style: StrokeStyle::Solid,
        corner_radii: CornerRadii::default(),
        opacity: filter.opacity,
    }
}

pub fn pen_filter_hit_test(filter: &PenFilterData, point: Point<f64>, hit_tolerance: f64) -> bool {
    let center = filter.center();
    let cosine = (-filter.rotation).cos();
    let sine = (-filter.rotation).sin();
    let dx = point.x - center.x;
    let dy = point.y - center.y;
    let local = Point::new(
        center.x + dx * cosine - dy * sine,
        center.y + dx * sine + dy * cosine,
    );
    let threshold = filter.stroke_width / 2.0 + hit_tolerance.max(0.0);
    filter.points.windows(2).any(|segment| {
        let start = Point::new(
            filter.x + segment[0][0] * filter.width,
            filter.y + segment[0][1] * filter.height,
        );
        let end = Point::new(
            filter.x + segment[1][0] * filter.width,
            filter.y + segment[1][1] * filter.height,
        );
        let vx = end.x - start.x;
        let vy = end.y - start.y;
        let length_squared = vx * vx + vy * vy;
        let t = if length_squared > 0.0 {
            (((local.x - start.x) * vx + (local.y - start.y) * vy) / length_squared).clamp(0.0, 1.0)
        } else {
            0.0
        };
        let nearest_x = start.x + vx * t;
        let nearest_y = start.y + vy * t;
        (local.x - nearest_x).hypot(local.y - nearest_y) <= threshold
    })
}

pub fn validate_pen_filter(filter: &PenFilterData) -> Result<(), ErrorCode> {
    let scalars = [
        filter.x,
        filter.y,
        filter.width,
        filter.height,
        filter.rotation,
        filter.strength,
        filter.stroke_width,
        filter.opacity,
    ];
    if scalars.into_iter().any(|value| !value.is_finite())
        || filter.width < 0.0
        || filter.height < 0.0
        || !(0.0..=1.0).contains(&filter.strength)
        || !(1.0..=72.0).contains(&filter.stroke_width)
        || !(0.0..=1.0).contains(&filter.opacity)
        || filter.points.len() < 2
        || filter.points.iter().any(|point| {
            point
                .iter()
                .any(|value| !value.is_finite() || !(0.0..=1.0).contains(value))
        })
        || !filter
            .global_points()
            .windows(2)
            .any(|segment| (segment[1].x - segment[0].x).hypot(segment[1].y - segment[0].y) > 0.0)
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

pub fn highlight_hit_test(rect: &RectangleData, point: Point<f64>, hit_tolerance: f64) -> bool {
    if !rect.is_highlight() {
        return false;
    }
    let local = canvas_to_rect_local(rect.center, rect.rotation, point);
    let dx = local.x.abs();
    let dy = local.y.abs();
    let half_width = rect.width / 2.0;
    let half_height = rect.height / 2.0;
    match rect.highlight_shape {
        crate::HighlightShape::Rectangle => {
            dx <= half_width + rect.stroke_width / 2.0 + hit_tolerance
                && dy <= half_height + rect.stroke_width / 2.0 + hit_tolerance
        }
        crate::HighlightShape::Ellipse => {
            if half_width <= 0.0 || half_height <= 0.0 {
                return false;
            }
            let expanded_x = half_width + rect.stroke_width / 2.0 + hit_tolerance;
            let expanded_y = half_height + rect.stroke_width / 2.0 + hit_tolerance;
            dx * dx / (expanded_x * expanded_x) + dy * dy / (expanded_y * expanded_y) <= 1.0
        }
        crate::HighlightShape::Diamond => false,
    }
}

fn rectangle_stroke_hit_test(
    rect: &RectangleData,
    local_point: Point<f64>,
    hit_tolerance: f64,
) -> bool {
    if rect.stroke.a == 0 || rect.stroke_width <= 0.0 {
        return false;
    }

    // Expand the selectable band around the visible stroke without changing
    // the rendered geometry. This keeps hairline outlines selectable.
    let hit_outset = rect.stroke_width / 2.0 + hit_tolerance;
    let outer_width = rect.width + hit_outset * 2.0;
    let outer_height = rect.height + hit_outset * 2.0;
    let outer_radii = normalize_corner_radii(
        outer_width,
        outer_height,
        CornerRadii {
            top_left: rect.corner_radii.top_left + hit_outset,
            top_right: rect.corner_radii.top_right + hit_outset,
            bottom_right: rect.corner_radii.bottom_right + hit_outset,
            bottom_left: rect.corner_radii.bottom_left + hit_outset,
        },
    );
    if !rounded_rect_contains_local_point(outer_width, outer_height, outer_radii, local_point) {
        return false;
    }

    let inner_width = (rect.width - hit_outset * 2.0).max(0.0);
    let inner_height = (rect.height - hit_outset * 2.0).max(0.0);
    if inner_width <= 0.0 || inner_height <= 0.0 {
        return true;
    }

    let inner_radii = normalize_corner_radii(
        inner_width,
        inner_height,
        CornerRadii {
            top_left: (rect.corner_radii.top_left - hit_outset).max(0.0),
            top_right: (rect.corner_radii.top_right - hit_outset).max(0.0),
            bottom_right: (rect.corner_radii.bottom_right - hit_outset).max(0.0),
            bottom_left: (rect.corner_radii.bottom_left - hit_outset).max(0.0),
        },
    );
    !rounded_rect_contains_local_point(inner_width, inner_height, inner_radii, local_point)
}

fn rounded_rect_contains_local_point(
    width: f64,
    height: f64,
    radii: CornerRadii,
    local_point: Point<f64>,
) -> bool {
    if width <= 0.0 || height <= 0.0 {
        return false;
    }

    let half_width = width / 2.0;
    let half_height = height / 2.0;
    if local_point.x.abs() > half_width || local_point.y.abs() > half_height {
        return false;
    }

    let (radius, corner_center) = if local_point.x <= 0.0 && local_point.y <= 0.0 {
        (
            radii.top_left,
            Point {
                x: -half_width + radii.top_left,
                y: -half_height + radii.top_left,
            },
        )
    } else if local_point.x >= 0.0 && local_point.y <= 0.0 {
        (
            radii.top_right,
            Point {
                x: half_width - radii.top_right,
                y: -half_height + radii.top_right,
            },
        )
    } else if local_point.x >= 0.0 && local_point.y >= 0.0 {
        (
            radii.bottom_right,
            Point {
                x: half_width - radii.bottom_right,
                y: half_height - radii.bottom_right,
            },
        )
    } else {
        (
            radii.bottom_left,
            Point {
                x: -half_width + radii.bottom_left,
                y: half_height - radii.bottom_left,
            },
        )
    };

    let in_corner_region_x = if local_point.x < 0.0 {
        local_point.x < -half_width + radius
    } else {
        local_point.x > half_width - radius
    };
    let in_corner_region_y = if local_point.y < 0.0 {
        local_point.y < -half_height + radius
    } else {
        local_point.y > half_height - radius
    };
    if !in_corner_region_x || !in_corner_region_y || radius <= 0.0 {
        return true;
    }

    let dx = local_point.x - corner_center.x;
    let dy = local_point.y - corner_center.y;
    dx * dx + dy * dy <= radius * radius + 1e-9
}

fn canvas_to_rect_local(center: Point<f64>, rotation: f64, point: Point<f64>) -> Point<f64> {
    let theta = -rotation;
    let dx = point.x - center.x;
    let dy = point.y - center.y;
    Point {
        x: dx * theta.cos() - dy * theta.sin(),
        y: dx * theta.sin() + dy * theta.cos(),
    }
}

pub(crate) fn element_visible_bounds(element: &ElementRecord) -> Option<DrawRect> {
    if !element.meta.visible {
        return None;
    }

    match &element.data {
        ElementData::Rectangle(rect) if rect.width > 0.0 && rect.height > 0.0 => {
            Some(rect_bounds(rect))
        }
        ElementData::Arrow(arrow) if !arrow_is_degenerate(arrow) => Some(arrow_bounds(arrow)),
        ElementData::FreeDraw(free_draw) => Some(crate::free_draw_bounds(free_draw)),
        ElementData::Text(text) if text.width > 0.0 && text.height > 0.0 => Some(text_bounds(text)),
        ElementData::SerialNumber(serial) if serial.diameter > 0.0 => {
            Some(serial_number_bounds(serial))
        }
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn text_defaults_use_product_color_and_corner_radius() {
        let text = TextData::default();

        assert_eq!(
            text.color,
            ColorRgba8 {
                r: 0xf4,
                g: 0x21,
                b: 0x2c,
                a: 0xff,
            }
        );
        assert_eq!(
            text.stroke,
            ColorRgba8 {
                r: 0xff,
                g: 0xcc,
                b: 0xc7,
                a: 0xff,
            }
        );
        assert_eq!(text.corner_radii, CornerRadii::splat(6.0));
    }

    #[test]
    fn serial_number_defaults_use_product_color_and_font_size() {
        assert_eq!(SerialNumberData::default().font_size, 24.0);
        assert_eq!(
            SerialNumberData::default().color,
            ColorRgba8 {
                r: 0xf4,
                g: 0x21,
                b: 0x2c,
                a: 0xff,
            }
        );
    }

    #[test]
    fn pen_filter_rect_proxy_includes_stroke_in_outer_contour_dimensions() {
        let filter = PenFilterData {
            x: 10.0,
            y: 20.0,
            width: 100.0,
            height: 40.0,
            stroke_width: 12.0,
            ..PenFilterData::default()
        };

        let proxy = pen_filter_rect_proxy(&filter);
        assert_eq!(proxy.center, Point::new(60.0, 40.0));
        assert_eq!(proxy.width, 112.0);
        assert_eq!(proxy.height, 52.0);
        assert_eq!(
            pen_filter_bounds(&filter),
            DrawRect::new(4.0, 14.0, 116.0, 66.0)
        );
    }

    #[test]
    fn text_font_size_accepts_minimum() {
        let text = TextData {
            font_size: MIN_TEXT_FONT_SIZE,
            ..TextData::default()
        };

        assert_eq!(validate_text(&text), Ok(()));
    }

    #[test]
    fn text_bounds_include_only_visible_stroke_paint() {
        let visible_stroke = TextData {
            center: Point::new(10.0, 20.0),
            width: 100.0,
            height: 40.0,
            text: "outlined".to_owned(),
            fill: ColorRgba8::default(),
            stroke: ColorRgba8 {
                r: 0,
                g: 0,
                b: 0,
                a: 0xff,
            },
            stroke_width: 10.0,
            ..TextData::default()
        };

        assert_eq!(
            text_bounds(&visible_stroke),
            DrawRect::new(-45.0, -5.0, 65.0, 45.0)
        );

        let transparent_stroke = TextData {
            stroke: ColorRgba8::default(),
            ..visible_stroke
        };
        assert_eq!(
            text_bounds(&transparent_stroke),
            DrawRect::new(-40.0, 0.0, 60.0, 40.0)
        );
    }

    #[test]
    fn text_bounds_include_background_paint_padding() {
        let text = TextData {
            center: Point::new(0.0, 0.0),
            width: 100.0,
            height: 40.0,
            text: "filled".to_owned(),
            font_size: 20.0,
            fill: ColorRgba8 {
                r: 0xff,
                g: 0xff,
                b: 0xff,
                a: 0xff,
            },
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            ..TextData::default()
        };

        let bounds = text_bounds(&text);
        assert!((bounds.min_x + 57.68).abs() < 1e-9);
        assert!((bounds.max_x - 57.68).abs() < 1e-9);
        assert!((bounds.min_y + 22.4).abs() < 1e-9);
        assert!((bounds.max_y - 22.4).abs() < 1e-9);
    }

    #[test]
    fn text_font_size_rejects_below_minimum() {
        let text = TextData {
            font_size: MIN_TEXT_FONT_SIZE - 0.1,
            ..TextData::default()
        };

        assert_eq!(validate_text(&text), Err(ErrorCode::InvalidArgument));
    }

    #[test]
    fn serial_number_font_size_accepts_minimum() {
        let serial = SerialNumberData {
            font_size: MIN_SERIAL_NUMBER_FONT_SIZE,
            ..SerialNumberData::default()
        };

        assert_eq!(validate_serial_number(&serial), Ok(()));
    }

    #[test]
    fn serial_number_font_size_rejects_below_minimum() {
        let serial = SerialNumberData {
            font_size: MIN_SERIAL_NUMBER_FONT_SIZE - 0.1,
            ..SerialNumberData::default()
        };

        assert_eq!(
            validate_serial_number(&serial),
            Err(ErrorCode::InvalidArgument)
        );
    }

    #[test]
    fn serial_number_minimum_selection_scale_matches_font_size_limit() {
        let serial = SerialNumberData {
            font_size: 16.0,
            ..SerialNumberData::default()
        };

        assert_eq!(serial_number_minimum_selection_scale(&serial), 0.375);
    }

    #[test]
    fn serial_number_selection_rect_scales_font_from_diameter() {
        let updated = serial_number_with_selection_rect(
            &SerialNumberData {
                diameter: 40.0,
                font_size: 16.0,
                ..SerialNumberData::default()
            },
            RectangleData {
                rectangle_kind: crate::RectangleElementKind::Rectangle,
                highlight_shape: crate::HighlightShape::Rectangle,
                center: Point::default(),
                width: 80.0,
                height: 100.0,
                rotation: 0.0,
                fill: ColorRgba8::default(),
                fill_style: FillStyle::Solid,
                stroke: ColorRgba8::default(),
                stroke_width: 0.0,
                stroke_style: StrokeStyle::Solid,
                corner_radii: CornerRadii::default(),
                opacity: 1.0,
            },
        );

        assert_eq!(updated.diameter, 80.0);
        assert_eq!(updated.font_size, 32.0);
    }

    #[test]
    fn serial_number_selection_rect_clamps_font_size_at_minimum() {
        let updated = serial_number_with_selection_rect(
            &SerialNumberData {
                diameter: 40.0,
                font_size: 16.0,
                ..SerialNumberData::default()
            },
            RectangleData {
                rectangle_kind: crate::RectangleElementKind::Rectangle,
                highlight_shape: crate::HighlightShape::Rectangle,
                center: Point::default(),
                width: 2.0,
                height: 2.0,
                rotation: 0.0,
                fill: ColorRgba8::default(),
                fill_style: FillStyle::Solid,
                stroke: ColorRgba8::default(),
                stroke_width: 0.0,
                stroke_style: StrokeStyle::Solid,
                corner_radii: CornerRadii::default(),
                opacity: 1.0,
            },
        );

        assert_eq!(updated.font_size, MIN_SERIAL_NUMBER_FONT_SIZE);
        assert_eq!(updated.diameter, 15.0);
    }

    #[test]
    fn transparent_rotated_highlight_interior_is_hittable_for_both_shapes() {
        let base = RectangleData {
            rectangle_kind: crate::RectangleElementKind::RectangleHighlight,
            highlight_shape: crate::HighlightShape::Rectangle,
            center: Point::new(20.0, 30.0),
            width: 100.0,
            height: 40.0,
            rotation: std::f64::consts::FRAC_PI_2,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 0.0,
        };
        assert!(highlight_hit_test(&base, Point::new(20.0, 70.0), 0.0));

        let ellipse = RectangleData {
            highlight_shape: crate::HighlightShape::Ellipse,
            ..base
        };
        assert!(highlight_hit_test(&ellipse, Point::new(20.0, 30.0), 0.0));
        assert!(!highlight_hit_test(&ellipse, Point::new(38.0, 75.0), 0.0));
    }

    #[test]
    fn rotated_filter_hit_testing_uses_its_local_rectangle() {
        let filter = FilterData {
            center: Point::new(10.0, 20.0),
            width: 100.0,
            height: 20.0,
            rotation: std::f64::consts::FRAC_PI_4,
            ..FilterData::default()
        };
        assert!(filter_hit_test(&filter, filter.center, 0.0));
        assert!(!filter_hit_test(&filter, Point::new(60.0, 20.0), 0.0));
        assert!(filter_hit_test(&filter, Point::new(60.0, 20.0), 30.0));
    }
}

use super::*;
use snow_draw_engine_document::StrokeStyle;

pub(crate) fn clamp_resize_scale(scale: f64, min_abs: f64, allow_flip: bool) -> f64 {
    let min_abs = min_abs.max(0.0);
    if !allow_flip {
        return scale.max(min_abs);
    }

    let sign = if scale.abs() <= f64::EPSILON {
        1.0
    } else {
        scale.signum()
    };
    sign * scale.abs().max(min_abs)
}

pub(crate) fn scaled_point_from_anchor(
    point_local: Point<f64>,
    anchor_local: Point<f64>,
    scale_x: f64,
    scale_y: f64,
) -> Point<f64> {
    Point {
        x: anchor_local.x + (point_local.x - anchor_local.x) * scale_x,
        y: anchor_local.y + (point_local.y - anchor_local.y) * scale_y,
    }
}

pub(crate) fn resized_dimensions(
    width: f64,
    height: f64,
    scale_x: f64,
    scale_y: f64,
) -> (f64, f64) {
    (
        (width * scale_x.abs()).max(MIN_RECT_SIZE),
        (height * scale_y.abs()).max(MIN_RECT_SIZE),
    )
}

pub(crate) fn resize_axis_reference_length(size: f64, scale_from_center: bool) -> f64 {
    if scale_from_center { size / 2.0 } else { size }
}

pub(crate) fn resize_scale_for_axis(
    anchor_value: f64,
    handle_value: f64,
    axis_sign: f64,
    size: f64,
    scale_from_center: bool,
) -> f64 {
    let reference_length = resize_axis_reference_length(size, scale_from_center);
    if axis_sign.abs() <= f64::EPSILON || reference_length <= f64::EPSILON {
        1.0
    } else {
        (handle_value - anchor_value) / (axis_sign * reference_length)
    }
}

pub(crate) fn resize_scale_sign(scale: f64) -> f64 {
    if scale.abs() <= f64::EPSILON {
        1.0
    } else {
        scale.signum()
    }
}

pub(crate) fn aspect_locked_resize_scales(
    request: AspectLockedResizeRequest,
    raw_scale_x: f64,
    raw_scale_y: f64,
    min_scale_x: f64,
    min_scale_y: f64,
    allow_flip: bool,
) -> (f64, f64) {
    let uniform_min_abs = min_scale_x.max(min_scale_y);
    let has_x = request.handle.x_sign().abs() > f64::EPSILON;
    let has_y = request.handle.y_sign().abs() > f64::EPSILON;
    let scale_magnitude = |scale: f64| {
        if allow_flip {
            scale.abs()
        } else {
            scale.max(0.0)
        }
    };
    let abs_scale = match (has_x, has_y) {
        (true, true) => scale_magnitude(raw_scale_x).max(scale_magnitude(raw_scale_y)),
        (true, false) => scale_magnitude(raw_scale_x),
        (false, true) => scale_magnitude(raw_scale_y),
        (false, false) => 1.0,
    }
    .max(uniform_min_abs);
    let scale_x = if has_x {
        if allow_flip {
            resize_scale_sign(raw_scale_x) * abs_scale
        } else {
            abs_scale
        }
    } else {
        abs_scale
    };
    let scale_y = if has_y {
        if allow_flip {
            resize_scale_sign(raw_scale_y) * abs_scale
        } else {
            abs_scale
        }
    } else {
        abs_scale
    };
    (scale_x, scale_y)
}

pub(crate) struct ResizeDragGeometryRequest<'a> {
    pub(crate) original_elements: &'a [SelectionRectState],
    pub(crate) original_bounds: &'a SelectionBounds,
    pub(crate) handle: ResizeHandle,
    pub(crate) handle_offset_canvas: Point<f64>,
    pub(crate) frame_padding: f64,
    pub(crate) corner_handle_outset: f64,
    pub(crate) canvas_point: Point<f64>,
    pub(crate) modifiers: Modifiers,
    pub(crate) force_aspect_lock: bool,
    pub(crate) allow_flip: bool,
    pub(crate) minimum_scale: f64,
}

pub(crate) fn resize_drag_geometry(request: ResizeDragGeometryRequest<'_>) -> ResizeDragGeometry {
    let ResizeDragGeometryRequest {
        original_elements,
        original_bounds,
        handle,
        handle_offset_canvas,
        frame_padding,
        corner_handle_outset,
        canvas_point,
        modifiers,
        force_aspect_lock,
        allow_flip,
        minimum_scale,
    } = request;
    let scale_from_center = modifiers.alt;
    let anchor_local = handle.anchor_local_point(
        original_bounds.width,
        original_bounds.height,
        scale_from_center,
    );
    let handle_local = selection_handle_local_from_pointer_for_handle(
        original_bounds,
        handle,
        handle_offset_canvas,
        frame_padding,
        corner_handle_outset,
        canvas_point,
    );
    let raw_scale_x = resize_scale_for_axis(
        anchor_local.x,
        handle_local.x,
        handle.x_sign(),
        original_bounds.width,
        scale_from_center,
    );
    let raw_scale_y = resize_scale_for_axis(
        anchor_local.y,
        handle_local.y,
        handle.y_sign(),
        original_bounds.height,
        scale_from_center,
    );
    let minimum_scale = minimum_scale.max(0.0);
    let min_scale_x = original_elements
        .iter()
        .fold(minimum_scale, |current, element| {
            current.max(MIN_RECT_SIZE / element.rect.width.max(MIN_RECT_SIZE))
        });
    let min_scale_y = original_elements
        .iter()
        .fold(minimum_scale, |current, element| {
            current.max(MIN_RECT_SIZE / element.rect.height.max(MIN_RECT_SIZE))
        });
    let (scale_x, scale_y) = if modifiers.shift || force_aspect_lock {
        aspect_locked_resize_scales(
            AspectLockedResizeRequest { handle },
            raw_scale_x,
            raw_scale_y,
            min_scale_x,
            min_scale_y,
            allow_flip,
        )
    } else {
        let scale_x = if handle.x_sign().abs() <= f64::EPSILON {
            1.0
        } else {
            clamp_resize_scale(raw_scale_x, min_scale_x, allow_flip)
        };
        let scale_y = if handle.y_sign().abs() <= f64::EPSILON {
            1.0
        } else {
            clamp_resize_scale(raw_scale_y, min_scale_y, allow_flip)
        };
        (scale_x, scale_y)
    };

    ResizeDragGeometry {
        anchor_local,
        handle_local,
        scale_x,
        scale_y,
    }
}

pub(crate) fn resized_selection_rect(
    original_rect: &RectangleData,
    original_bounds: &SelectionBounds,
    anchor_local: Point<f64>,
    scale_x: f64,
    scale_y: f64,
) -> RectangleData {
    let center_local = canvas_to_rect_local(
        original_bounds.center,
        original_bounds.rotation,
        original_rect.center,
    );
    let next_center_local = scaled_point_from_anchor(center_local, anchor_local, scale_x, scale_y);
    let (width, height) =
        resized_dimensions(original_rect.width, original_rect.height, scale_x, scale_y);
    RectangleData {
        rectangle_kind: original_rect.rectangle_kind,
        highlight_shape: original_rect.highlight_shape,
        center: rect_local_to_canvas(
            original_bounds.center,
            original_bounds.rotation,
            next_center_local,
        ),
        width,
        height,
        rotation: original_rect.rotation,
        fill: original_rect.fill,
        fill_style: original_rect.fill_style,
        stroke: original_rect.stroke,
        stroke_width: original_rect.stroke_width,
        stroke_style: original_rect.stroke_style,
        corner_radii: normalize_corner_radii(
            width,
            height,
            flipped_corner_radii(
                original_rect.corner_radii,
                scale_x < -f64::EPSILON,
                scale_y < -f64::EPSILON,
            ),
        ),
        opacity: original_rect.opacity,
    }
}

fn flipped_corner_radii(radii: CornerRadii, flip_x: bool, flip_y: bool) -> CornerRadii {
    let mut flipped = radii;
    if flip_x {
        flipped = CornerRadii {
            top_left: flipped.top_right,
            top_right: flipped.top_left,
            bottom_right: flipped.bottom_left,
            bottom_left: flipped.bottom_right,
        };
    }
    if flip_y {
        flipped = CornerRadii {
            top_left: flipped.bottom_left,
            top_right: flipped.bottom_right,
            bottom_right: flipped.top_right,
            bottom_left: flipped.top_left,
        };
    }
    flipped
}

const ESTIMATED_TEXT_MIN_WIDTH_EM: f64 = 0.6;
const ESTIMATED_TEXT_LATIN_WIDTH_EM: f64 = 0.56;
const ESTIMATED_TEXT_SPACE_WIDTH_EM: f64 = 0.33;
const ESTIMATED_TEXT_CJK_WIDTH_EM: f64 = 1.0;

pub(crate) fn text_resize_preview_rect(
    text: &TextData,
    original_bounds: &SelectionBounds,
    handle: ResizeHandle,
    geometry: ResizeDragGeometry,
    scale_from_center: bool,
    layout_override: Option<TextResizeLayoutOverride>,
) -> Option<RectangleData> {
    let original_rect = RectangleData {
        rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
        highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
        center: text.center,
        width: text.width(),
        height: text.height(),
        rotation: text.rotation,
        fill: text.fill,
        fill_style: text.fill_style,
        stroke: text.stroke,
        stroke_width: text.stroke_width,
        stroke_style: StrokeStyle::Solid,
        corner_radii: text.corner_radii,
        opacity: text.opacity,
    };

    if text_resize_changes_width_only(handle) {
        let scale_x = geometry.scale_x;
        if scale_x <= f64::EPSILON || !scale_x.is_finite() {
            let next_width = MIN_RECT_SIZE;
            if let Some(layout) = text_resize_layout_for_width(next_width, layout_override) {
                return Some(resized_text_rect_from_size(
                    &original_rect,
                    original_bounds,
                    text_resize_anchor(handle, scale_from_center),
                    layout.width(),
                    layout.height(),
                ));
            }
            return Some(resized_text_rect_from_size(
                &original_rect,
                original_bounds,
                text_resize_anchor(handle, scale_from_center),
                next_width,
                text_resize_height(text, next_width, layout_override),
            ));
        }
        let next_width = (text.width() * scale_x).max(MIN_RECT_SIZE);
        if let Some(layout) = text_resize_layout_for_width(next_width, layout_override) {
            return Some(resized_text_rect_from_size(
                &original_rect,
                original_bounds,
                text_resize_anchor(handle, scale_from_center),
                layout.width(),
                layout.height(),
            ));
        }
        let next_height = text_resize_height(text, next_width, layout_override);
        return Some(resized_text_rect_from_size(
            &original_rect,
            original_bounds,
            text_resize_anchor(handle, scale_from_center),
            next_width,
            next_height,
        ));
    }

    let scale = text_uniform_resize_scale(handle, geometry)?;
    let min_font_scale = if text.font_size.is_finite() && text.font_size > f64::EPSILON {
        MIN_TEXT_FONT_SIZE / text.font_size
    } else {
        1.0
    };
    let scale = scale.max(min_font_scale);
    let next_height = text.height() * scale;
    if next_height <= f64::EPSILON || !next_height.is_finite() {
        return None;
    }
    let next_font_size = text.font_size * scale;
    if !scale.is_finite() || !next_font_size.is_finite() || next_font_size < MIN_TEXT_FONT_SIZE {
        return None;
    }
    if let Some(layout) = text_resize_layout_for_height(next_height, layout_override) {
        return Some(resized_text_rect_from_size(
            &original_rect,
            original_bounds,
            text_resize_anchor(handle, scale_from_center),
            layout.width(),
            layout.height(),
        ));
    }
    let next_width = (text.width() * scale).max(MIN_RECT_SIZE);
    Some(resized_text_rect_from_size(
        &original_rect,
        original_bounds,
        text_resize_anchor(handle, scale_from_center),
        next_width,
        next_height,
    ))
}

pub(crate) fn text_resize_changes_width_only(handle: ResizeHandle) -> bool {
    handle.x_sign().abs() > f64::EPSILON && handle.y_sign().abs() <= f64::EPSILON
}

fn text_uniform_resize_scale(handle: ResizeHandle, geometry: ResizeDragGeometry) -> Option<f64> {
    let has_x = handle.x_sign().abs() > f64::EPSILON;
    let has_y = handle.y_sign().abs() > f64::EPSILON;
    let scale = match (has_x, has_y) {
        (true, true) => text_corner_uniform_resize_scale(geometry.scale_x, geometry.scale_y)?,
        (false, true) => geometry.scale_y,
        (true, false) => geometry.scale_x,
        (false, false) => 1.0,
    };
    if scale.is_finite() && scale > f64::EPSILON {
        Some(scale)
    } else {
        None
    }
}

fn text_corner_uniform_resize_scale(scale_x: f64, scale_y: f64) -> Option<f64> {
    let valid_x = scale_x.is_finite() && scale_x > f64::EPSILON;
    let valid_y = scale_y.is_finite() && scale_y > f64::EPSILON;
    match (valid_x, valid_y) {
        (true, true) => {
            let x_delta = (scale_x - 1.0).abs();
            let y_delta = (scale_y - 1.0).abs();
            Some(if x_delta >= y_delta { scale_x } else { scale_y })
        }
        (true, false) => Some(scale_x),
        (false, true) => Some(scale_y),
        (false, false) => None,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum TextResizeAnchor {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Left,
    Right,
    Center,
}

pub(crate) fn text_resize_anchor(
    handle: ResizeHandle,
    scale_from_center: bool,
) -> TextResizeAnchor {
    if scale_from_center {
        return TextResizeAnchor::Center;
    }

    match handle {
        ResizeHandle::Bottom | ResizeHandle::BottomRight => TextResizeAnchor::TopLeft,
        ResizeHandle::BottomLeft => TextResizeAnchor::TopRight,
        ResizeHandle::Left => TextResizeAnchor::Right,
        ResizeHandle::Right => TextResizeAnchor::Left,
        ResizeHandle::Top | ResizeHandle::TopLeft => TextResizeAnchor::BottomRight,
        ResizeHandle::TopRight => TextResizeAnchor::BottomLeft,
    }
}

pub(crate) fn resized_text_rect_from_size(
    original_rect: &RectangleData,
    original_bounds: &SelectionBounds,
    anchor: TextResizeAnchor,
    next_width: f64,
    next_height: f64,
) -> RectangleData {
    let anchor_local = text_anchor_local(anchor, original_rect.width, original_rect.height);
    let anchor_offset = text_anchor_offset(anchor, next_width, next_height);
    let original_center_local = canvas_to_rect_local(
        original_bounds.center,
        original_bounds.rotation,
        original_rect.center,
    );
    let resolved_anchor_local = Point {
        x: original_center_local.x + anchor_local.x,
        y: original_center_local.y + anchor_local.y,
    };
    let next_center_local = Point {
        x: resolved_anchor_local.x - anchor_offset.x,
        y: resolved_anchor_local.y - anchor_offset.y,
    };

    RectangleData {
        rectangle_kind: original_rect.rectangle_kind,
        highlight_shape: original_rect.highlight_shape,
        center: rect_local_to_canvas(
            original_bounds.center,
            original_bounds.rotation,
            next_center_local,
        ),
        width: next_width,
        height: next_height,
        rotation: original_rect.rotation,
        fill: original_rect.fill,
        fill_style: original_rect.fill_style,
        stroke: original_rect.stroke,
        stroke_width: original_rect.stroke_width,
        stroke_style: original_rect.stroke_style,
        corner_radii: normalize_corner_radii(next_width, next_height, original_rect.corner_radii),
        opacity: original_rect.opacity,
    }
}

fn text_anchor_local(anchor: TextResizeAnchor, width: f64, height: f64) -> Point<f64> {
    match anchor {
        TextResizeAnchor::TopLeft => Point::new(-width / 2.0, -height / 2.0),
        TextResizeAnchor::TopRight => Point::new(width / 2.0, -height / 2.0),
        TextResizeAnchor::BottomLeft => Point::new(-width / 2.0, height / 2.0),
        TextResizeAnchor::BottomRight => Point::new(width / 2.0, height / 2.0),
        TextResizeAnchor::Left => Point::new(-width / 2.0, 0.0),
        TextResizeAnchor::Right => Point::new(width / 2.0, 0.0),
        TextResizeAnchor::Center => Point::new(0.0, 0.0),
    }
}

fn text_anchor_offset(anchor: TextResizeAnchor, width: f64, height: f64) -> Point<f64> {
    match anchor {
        TextResizeAnchor::TopLeft => Point::new(-width / 2.0, -height / 2.0),
        TextResizeAnchor::TopRight => Point::new(width / 2.0, -height / 2.0),
        TextResizeAnchor::BottomLeft => Point::new(-width / 2.0, height / 2.0),
        TextResizeAnchor::BottomRight => Point::new(width / 2.0, height / 2.0),
        TextResizeAnchor::Left => Point::new(-width / 2.0, 0.0),
        TextResizeAnchor::Right => Point::new(width / 2.0, 0.0),
        TextResizeAnchor::Center => Point::new(0.0, 0.0),
    }
}

/// Provisional Rust-only estimate for text resize previews before Qt returns
/// the exact layout. Persisted text sizes must come from `TextLayoutSize`
/// values measured by the host renderer.
pub(crate) fn estimated_wrapped_text_height(text: &TextData, max_width: f64) -> f64 {
    let max_width = max_width.max(estimated_min_wrapped_text_width(text));
    let line_height = text_line_height(text.font_size);
    let line_count = normalized_text_lines(&text.text)
        .iter()
        .map(|line| estimated_wrapped_line_count(line, text.font_size, max_width))
        .sum::<usize>()
        .max(1);
    (line_count as f64 * line_height).max(line_height)
}

pub(crate) fn estimated_min_wrapped_text_width(text: &TextData) -> f64 {
    (text.font_size.max(MIN_TEXT_FONT_SIZE) * ESTIMATED_TEXT_MIN_WIDTH_EM).max(MIN_RECT_SIZE)
}

fn text_resize_height(
    text: &TextData,
    width: f64,
    layout_override: Option<TextResizeLayoutOverride>,
) -> f64 {
    if let Some(layout) = text_resize_layout_for_width(width, layout_override) {
        return layout.height();
    }
    estimated_wrapped_text_height(text, width)
}

fn text_resize_layout_for_width(
    width: f64,
    layout_override: Option<TextResizeLayoutOverride>,
) -> Option<TextLayoutSize> {
    let layout_override = layout_override?;
    let layout = layout_override.layout;
    if !layout.width().is_finite()
        || layout.width() <= 0.0
        || !layout.height().is_finite()
        || layout.height() <= 0.0
    {
        return None;
    }
    if (layout.width() - width).abs() <= 1e-3 {
        return Some(layout);
    }
    let is_min_width_clamp = layout_override.requested_width + 1e-3 < layout.width();
    if is_min_width_clamp && width <= layout.width() + 1e-3 {
        return Some(layout);
    }
    None
}

fn text_resize_layout_for_height(
    height: f64,
    layout_override: Option<TextResizeLayoutOverride>,
) -> Option<TextLayoutSize> {
    let layout_override = layout_override?;
    let layout = layout_override.layout;
    if !layout.width().is_finite()
        || layout.width() <= 0.0
        || !layout.height().is_finite()
        || layout.height() <= 0.0
    {
        return None;
    }
    if (layout_override.requested_height - height).abs() <= 1e-3 {
        return Some(layout);
    }
    None
}

fn normalized_text_lines(text: &str) -> Vec<&str> {
    if text.is_empty() {
        vec![" "]
    } else {
        text.split('\n')
            .map(|line| if line.is_empty() { " " } else { line })
            .collect()
    }
}

fn estimated_wrapped_line_count(line: &str, font_size: f64, max_width: f64) -> usize {
    let mut line_count = 1usize;
    let mut current_width = 0.0;
    for ch in line.chars() {
        let char_width = estimated_char_width(ch, font_size);
        if current_width > 0.0 && current_width + char_width > max_width {
            line_count += 1;
            current_width = char_width;
        } else {
            current_width += char_width;
        }
    }
    line_count
}

fn estimated_char_width(ch: char, font_size: f64) -> f64 {
    let em = if ch.is_whitespace() {
        ESTIMATED_TEXT_SPACE_WIDTH_EM
    } else if ch.len_utf8() > 1 {
        ESTIMATED_TEXT_CJK_WIDTH_EM
    } else {
        ESTIMATED_TEXT_LATIN_WIDTH_EM
    };
    font_size.max(MIN_TEXT_FONT_SIZE) * em
}

pub(crate) fn resized_selection_bounds_from_drag_geometry(
    original_bounds: &SelectionBounds,
    geometry: ResizeDragGeometry,
) -> SelectionBounds {
    let center_local = scaled_point_from_anchor(
        Point { x: 0.0, y: 0.0 },
        geometry.anchor_local,
        geometry.scale_x,
        geometry.scale_y,
    );
    let (width, height) = resized_dimensions(
        original_bounds.width,
        original_bounds.height,
        geometry.scale_x,
        geometry.scale_y,
    );
    SelectionBounds {
        center: rect_local_to_canvas(
            original_bounds.center,
            original_bounds.rotation,
            center_local,
        ),
        width,
        height,
        rotation: original_bounds.rotation,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn assert_close(left: f64, right: f64) {
        assert!(
            (left - right).abs() <= 1e-9,
            "expected {left} to be close to {right}"
        );
    }

    fn selection_bounds() -> SelectionBounds {
        SelectionBounds {
            center: Point::new(50.0, 50.0),
            width: 100.0,
            height: 40.0,
            rotation: 0.0,
        }
    }

    fn selection_element(bounds: SelectionBounds) -> SelectionRectState {
        SelectionRectState {
            id: ElementId {
                index: 1,
                generation: 1,
            },
            rect: RectangleData {
                rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
                highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
                center: bounds.center,
                width: bounds.width,
                height: bounds.height,
                rotation: bounds.rotation,
                fill: ColorRgba8::default(),
                fill_style: snow_draw_engine_document::FillStyle::Solid,
                stroke: ColorRgba8::default(),
                stroke_width: 0.0,
                stroke_style: StrokeStyle::Solid,
                corner_radii: CornerRadii::default(),
                opacity: 1.0,
            },
        }
    }

    #[test]
    fn right_resize_clamps_before_flipping_past_left_edge() {
        let bounds = selection_bounds();
        let elements = [selection_element(bounds)];
        let geometry = resize_drag_geometry(ResizeDragGeometryRequest {
            original_elements: &elements,
            original_bounds: &bounds,
            handle: ResizeHandle::Right,
            handle_offset_canvas: Point::default(),
            frame_padding: 0.0,
            corner_handle_outset: 0.0,
            canvas_point: Point::new(-10.0, 50.0),
            modifiers: Modifiers::default(),
            force_aspect_lock: false,
            allow_flip: false,
            minimum_scale: 0.0,
        });
        let resized = resized_selection_bounds_from_drag_geometry(&bounds, geometry);

        assert_close(geometry.scale_x, MIN_RECT_SIZE / bounds.width);
        assert!(geometry.scale_x > 0.0);
        assert_close(resized.width, MIN_RECT_SIZE);
    }

    #[test]
    fn aspect_locked_resize_clamps_before_flipping_past_anchor() {
        let bounds = selection_bounds();
        let elements = [selection_element(bounds)];
        let geometry = resize_drag_geometry(ResizeDragGeometryRequest {
            original_elements: &elements,
            original_bounds: &bounds,
            handle: ResizeHandle::Right,
            handle_offset_canvas: Point::default(),
            frame_padding: 0.0,
            corner_handle_outset: 0.0,
            canvas_point: Point::new(-10.0, 50.0),
            modifiers: Modifiers {
                shift: true,
                ..Modifiers::default()
            },
            force_aspect_lock: false,
            allow_flip: false,
            minimum_scale: 0.0,
        });

        assert_close(geometry.scale_x, MIN_RECT_SIZE / bounds.height);
        assert_close(geometry.scale_y, MIN_RECT_SIZE / bounds.height);
        assert!(geometry.scale_x > 0.0);
        assert!(geometry.scale_y > 0.0);
    }

    #[test]
    fn right_resize_flips_past_left_edge_when_enabled() {
        let bounds = selection_bounds();
        let elements = [selection_element(bounds)];
        let geometry = resize_drag_geometry(ResizeDragGeometryRequest {
            original_elements: &elements,
            original_bounds: &bounds,
            handle: ResizeHandle::Right,
            handle_offset_canvas: Point::default(),
            frame_padding: 0.0,
            corner_handle_outset: 0.0,
            canvas_point: Point::new(-10.0, 50.0),
            modifiers: Modifiers::default(),
            force_aspect_lock: false,
            allow_flip: true,
            minimum_scale: 0.0,
        });
        let resized = resized_selection_bounds_from_drag_geometry(&bounds, geometry);

        assert_close(geometry.scale_x, -0.1);
        assert_close(resized.center.x, -5.0);
        assert_close(resized.width, 10.0);
    }

    #[test]
    fn flipped_selection_rect_mirrors_corner_radii() {
        let bounds = selection_bounds();
        let rect = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: bounds.center,
            width: bounds.width,
            height: bounds.height,
            rotation: bounds.rotation,
            fill: ColorRgba8::default(),
            fill_style: snow_draw_engine_document::FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii {
                top_left: 1.0,
                top_right: 2.0,
                bottom_right: 3.0,
                bottom_left: 4.0,
            },
            opacity: 1.0,
        };

        let flipped = resized_selection_rect(
            &rect,
            &bounds,
            Point::new(-bounds.width / 2.0, 0.0),
            -1.0,
            1.0,
        );

        assert_eq!(
            flipped.corner_radii,
            CornerRadii {
                top_left: 2.0,
                top_right: 1.0,
                bottom_right: 4.0,
                bottom_left: 3.0,
            }
        );
    }
}

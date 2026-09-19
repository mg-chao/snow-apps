use super::*;

pub(crate) const SELECTION_FRAME_PADDING_PX: f64 = 4.0;
pub(crate) const TEXT_SELECTION_FRAME_PADDING_PX: f64 = 14.0;
pub(crate) const SELECTION_HANDLE_SIZE_PX: f64 = 8.0;
pub(crate) const SELECTION_HANDLE_HIT_SIZE_PX: f64 = 12.0;
pub(crate) const SELECTION_ARROW_CORNER_HANDLE_OUTSET_PX: f64 = 8.0;
pub(crate) const SELECTION_TIGHT_EDGE_INNER_HIT_PX: f64 = 2.0;
pub(crate) const ELEMENT_HIT_TOLERANCE_PX: f64 = 4.0;
pub(crate) const POINTER_DRAG_THRESHOLD: f64 = 4.0;
pub(crate) const ARROW_SEGMENT_HANDLE_MIN_VISIBLE_PX: f64 = 5.0;
pub(crate) const SELECTION_ROTATION_OFFSET_PX: f64 = 20.0;
pub(crate) const SELECTION_CORNER_HANDLE_MIN_INSET_PX: f64 = 12.0;
pub(crate) const MIN_RECT_SIZE: f64 = 1.0;
pub(crate) const ALL_CORNERS: [RectCorner; 4] = [
    RectCorner::TopLeft,
    RectCorner::TopRight,
    RectCorner::BottomRight,
    RectCorner::BottomLeft,
];
pub(crate) const SELECTION_BLUE: ColorRgba8 = ColorRgba8 {
    r: 0x40,
    g: 0x96,
    b: 0xff,
    a: 0xff,
};
pub(crate) const SELECTION_MARQUEE_FILL: ColorRgba8 = ColorRgba8 {
    r: 0x40,
    g: 0x96,
    b: 0xff,
    a: 0x33,
};

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct ResizeDragGeometry {
    pub(crate) anchor_local: Point<f64>,
    pub(crate) handle_local: Point<f64>,
    pub(crate) scale_x: f64,
    pub(crate) scale_y: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct SelectionEditPreview {
    pub(crate) elements: Vec<SelectionRectState>,
    pub(crate) arrows: Vec<SelectionArrowState>,
    pub(crate) bounds: SelectionBounds,
    pub(crate) snap_guides: Vec<SnapGuide>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ArrowEditPreview {
    pub(crate) arrow: ArrowData,
    pub(crate) reorder_targets: Vec<ElementId>,
    pub(crate) next_mode: Option<ArrowEditMode>,
    pub(crate) suggested_binding: Option<ArrowSuggestedBinding>,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct ResizeSelectionContext<'a> {
    pub(crate) original_elements: &'a [SelectionRectState],
    pub(crate) original_arrows: &'a [SelectionArrowState],
    pub(crate) original_bounds: &'a SelectionBounds,
    pub(crate) handle: ResizeHandle,
    pub(crate) handle_offset_canvas: Point<f64>,
    pub(crate) frame_padding: f64,
    pub(crate) corner_handle_outset: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct AspectLockedResizeRequest {
    pub(crate) handle: ResizeHandle,
}

pub(crate) fn selection_frame_padding(zoom: f64) -> f64 {
    SELECTION_FRAME_PADDING_PX / zoom.max(0.0001)
}

pub(crate) fn text_selection_frame_padding(zoom: f64) -> f64 {
    TEXT_SELECTION_FRAME_PADDING_PX / zoom.max(0.0001)
}

pub(crate) fn selection_frame_padding_for_members(
    zoom: f64,
    selected_rect_count: usize,
    selected_arrow_count: usize,
) -> f64 {
    if selected_rect_count == 0 && selected_arrow_count > 0 {
        0.0
    } else {
        selection_frame_padding(zoom)
    }
}

pub(crate) fn selection_corner_handle_outset_for_members(
    zoom: f64,
    selected_rect_count: usize,
    selected_arrow_count: usize,
) -> f64 {
    if selected_rect_count == 0 && selected_arrow_count > 0 {
        SELECTION_ARROW_CORNER_HANDLE_OUTSET_PX / zoom.max(0.0001)
    } else {
        0.0
    }
}

pub(crate) fn selection_handle_size(zoom: f64) -> f64 {
    SELECTION_HANDLE_SIZE_PX / zoom.max(0.0001)
}

pub(crate) fn selection_handle_hit_size(zoom: f64) -> f64 {
    SELECTION_HANDLE_HIT_SIZE_PX / zoom.max(0.0001)
}

pub(crate) fn element_hit_tolerance(zoom: f64) -> f64 {
    ELEMENT_HIT_TOLERANCE_PX / zoom.max(0.0001)
}

pub(crate) fn selection_rotation_offset(zoom: f64) -> f64 {
    SELECTION_ROTATION_OFFSET_PX / zoom.max(0.0001)
}

pub(crate) fn selection_corner_handle_min_inset(zoom: f64) -> f64 {
    SELECTION_CORNER_HANDLE_MIN_INSET_PX / zoom.max(0.0001)
}

pub(crate) fn selection_resize_handle_center(
    bounds: &SelectionBounds,
    frame_padding: f64,
    corner_handle_outset: f64,
    corner: RectCorner,
) -> Point<f64> {
    selection_resize_handle_center_for_handle(
        bounds,
        frame_padding,
        corner_handle_outset,
        ResizeHandle::from_corner(corner),
    )
}

pub(crate) fn selection_resize_handle_local_offset(
    handle: ResizeHandle,
    frame_padding: f64,
    corner_handle_outset: f64,
) -> Point<f64> {
    let corner_outset = if handle.is_corner() {
        corner_handle_outset
    } else {
        0.0
    };
    Point {
        x: handle.x_sign() * (frame_padding + corner_outset),
        y: handle.y_sign() * (frame_padding + corner_outset),
    }
}

pub(crate) fn selection_resize_handle_center_for_handle(
    bounds: &SelectionBounds,
    frame_padding: f64,
    corner_handle_outset: f64,
    handle: ResizeHandle,
) -> Point<f64> {
    let local_handle = handle.local_point(bounds.width, bounds.height);
    let local_offset =
        selection_resize_handle_local_offset(handle, frame_padding, corner_handle_outset);
    rect_local_to_canvas(
        bounds.center,
        bounds.rotation,
        Point {
            x: local_handle.x + local_offset.x,
            y: local_handle.y + local_offset.y,
        },
    )
}

pub(crate) fn selection_rotation_handle_center(
    bounds: &SelectionBounds,
    frame_padding: f64,
    zoom: f64,
) -> Point<f64> {
    rect_local_to_canvas(
        bounds.center,
        bounds.rotation,
        Point {
            x: 0.0,
            y: -(bounds.height / 2.0 + frame_padding + selection_rotation_offset(zoom)),
        },
    )
}

#[cfg(any())]
pub(crate) fn selection_handle_local_from_pointer(
    bounds: &SelectionBounds,
    corner: RectCorner,
    handle_offset_canvas: Point<f64>,
    frame_padding: f64,
    corner_handle_outset: f64,
    canvas_point: Point<f64>,
) -> Point<f64> {
    selection_handle_local_from_pointer_for_handle(
        bounds,
        ResizeHandle::from_corner(corner),
        handle_offset_canvas,
        frame_padding,
        corner_handle_outset,
        canvas_point,
    )
}

pub(crate) fn selection_handle_local_from_pointer_for_handle(
    bounds: &SelectionBounds,
    handle: ResizeHandle,
    handle_offset_canvas: Point<f64>,
    frame_padding: f64,
    corner_handle_outset: f64,
    canvas_point: Point<f64>,
) -> Point<f64> {
    let adjusted_handle_canvas = Point {
        x: canvas_point.x - handle_offset_canvas.x,
        y: canvas_point.y - handle_offset_canvas.y,
    };
    let visual_handle_local =
        canvas_to_rect_local(bounds.center, bounds.rotation, adjusted_handle_canvas);
    let local_offset =
        selection_resize_handle_local_offset(handle, frame_padding, corner_handle_outset);
    Point {
        x: visual_handle_local.x - local_offset.x,
        y: visual_handle_local.y - local_offset.y,
    }
}

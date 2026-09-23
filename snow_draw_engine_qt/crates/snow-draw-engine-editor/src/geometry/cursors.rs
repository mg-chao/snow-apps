use super::*;

pub(crate) fn selection_edit_label(mode: SelectionEditMode) -> &'static str {
    match mode {
        SelectionEditMode::Move { .. } => "move selection",
        SelectionEditMode::Resize { .. } => "resize selection",
        SelectionEditMode::Rotate { .. } => "rotate selection",
        SelectionEditMode::CornerRadius { .. } => "adjust rectangle corner radius",
    }
}

pub(crate) fn arrow_edit_label(mode: ArrowEditMode) -> &'static str {
    match mode {
        ArrowEditMode::Move => "move arrow",
        ArrowEditMode::Label => "move arrow text",
        ArrowEditMode::Endpoint(_) => "drag arrow endpoint",
        ArrowEditMode::Point(_) => "drag arrow point",
        ArrowEditMode::FocusPoint(_) => "drag arrow focus point",
        ArrowEditMode::Segment(_) => "drag arrow segment",
    }
}

pub(crate) fn hover_cursor_for_selection_target(
    target: SelectionHitTarget,
    rotation: f64,
) -> CursorStyle {
    match target {
        SelectionHitTarget::Move => CursorStyle::Move,
        SelectionHitTarget::Resize(handle) => resize_cursor_for_handle(handle, rotation),
        SelectionHitTarget::Rotate => CursorStyle::Grab,
        SelectionHitTarget::CornerRadius(_) => CursorStyle::CornerRadius,
    }
}

pub(crate) fn active_cursor_for_selection_target(
    target: SelectionHitTarget,
    rotation: f64,
) -> CursorStyle {
    match target {
        SelectionHitTarget::Move => CursorStyle::Move,
        SelectionHitTarget::Resize(handle) => resize_cursor_for_handle(handle, rotation),
        SelectionHitTarget::Rotate => CursorStyle::Grabbing,
        SelectionHitTarget::CornerRadius(_) => CursorStyle::CornerRadius,
    }
}

pub(crate) fn hover_cursor_for_arrow_target(target: ArrowHitTarget) -> CursorStyle {
    match target {
        ArrowHitTarget::Move | ArrowHitTarget::Label => CursorStyle::Move,
        ArrowHitTarget::Endpoint(_) | ArrowHitTarget::Point(_) | ArrowHitTarget::FocusPoint(_) => {
            CursorStyle::Grab
        }
        ArrowHitTarget::Segment(_) => CursorStyle::Move,
    }
}

pub(crate) fn active_cursor_for_arrow_target(target: ArrowHitTarget) -> CursorStyle {
    match target {
        ArrowHitTarget::Move | ArrowHitTarget::Label => CursorStyle::Move,
        ArrowHitTarget::Endpoint(_) | ArrowHitTarget::Point(_) | ArrowHitTarget::FocusPoint(_) => {
            CursorStyle::Crosshair
        }
        ArrowHitTarget::Segment(_) => CursorStyle::Move,
    }
}

pub(crate) fn active_cursor_for_arrow_mode(mode: ArrowEditMode) -> CursorStyle {
    match mode {
        ArrowEditMode::Move | ArrowEditMode::Label => CursorStyle::Move,
        ArrowEditMode::Endpoint(_) | ArrowEditMode::Point(_) | ArrowEditMode::FocusPoint(_) => {
            CursorStyle::Crosshair
        }
        ArrowEditMode::Segment(_) => CursorStyle::Move,
    }
}

pub(crate) fn resize_cursor_for_handle(handle: ResizeHandle, rotation: f64) -> CursorStyle {
    let rotated = rotate_vector(handle.local_point(2.0, 2.0), rotation);
    let angle = rotated.y.atan2(rotated.x).to_degrees().rem_euclid(180.0);
    if !(22.5..157.5).contains(&angle) {
        CursorStyle::ResizeHorizontal
    } else if angle < 67.5 {
        CursorStyle::ResizeNwSe
    } else if angle < 112.5 {
        CursorStyle::ResizeVertical
    } else {
        CursorStyle::ResizeNeSw
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::arrow::ArrowEndpointEdge;
    use std::f64::consts::FRAC_PI_4;

    #[test]
    fn corner_radius_handles_use_the_corner_radius_cursor_when_hovered_and_dragged() {
        const TARGET: SelectionHitTarget = SelectionHitTarget::CornerRadius(RectCorner::TopLeft);

        assert_eq!(
            hover_cursor_for_selection_target(TARGET, 0.0),
            CursorStyle::CornerRadius
        );
        assert_eq!(
            active_cursor_for_selection_target(TARGET, 0.0),
            CursorStyle::CornerRadius
        );
    }

    #[test]
    fn arrow_control_points_use_grab_cursor_when_hovered() {
        let targets = [
            ArrowHitTarget::Endpoint(ArrowEndpointEdge::Start),
            ArrowHitTarget::Point(1),
            ArrowHitTarget::FocusPoint(ArrowEndpointEdge::End),
        ];

        for target in targets {
            assert_eq!(hover_cursor_for_arrow_target(target), CursorStyle::Grab);
            assert_eq!(
                active_cursor_for_arrow_target(target),
                CursorStyle::Crosshair
            );
        }
    }

    #[test]
    fn resize_handle_cursors_follow_each_unrotated_control_point() {
        let cases = [
            (ResizeHandle::TopLeft, CursorStyle::ResizeNwSe),
            (ResizeHandle::Top, CursorStyle::ResizeVertical),
            (ResizeHandle::TopRight, CursorStyle::ResizeNeSw),
            (ResizeHandle::Right, CursorStyle::ResizeHorizontal),
            (ResizeHandle::BottomRight, CursorStyle::ResizeNwSe),
            (ResizeHandle::Bottom, CursorStyle::ResizeVertical),
            (ResizeHandle::BottomLeft, CursorStyle::ResizeNeSw),
            (ResizeHandle::Left, CursorStyle::ResizeHorizontal),
        ];

        for (handle, expected) in cases {
            let target = SelectionHitTarget::Resize(handle);
            assert_eq!(hover_cursor_for_selection_target(target, 0.0), expected);
            assert_eq!(active_cursor_for_selection_target(target, 0.0), expected);
        }
    }

    #[test]
    fn resize_handle_cursors_rotate_with_the_element() {
        let cases = [
            (ResizeHandle::TopLeft, CursorStyle::ResizeVertical),
            (ResizeHandle::Top, CursorStyle::ResizeNeSw),
            (ResizeHandle::TopRight, CursorStyle::ResizeHorizontal),
            (ResizeHandle::Right, CursorStyle::ResizeNwSe),
            (ResizeHandle::BottomRight, CursorStyle::ResizeVertical),
            (ResizeHandle::Bottom, CursorStyle::ResizeNeSw),
            (ResizeHandle::BottomLeft, CursorStyle::ResizeHorizontal),
            (ResizeHandle::Left, CursorStyle::ResizeNwSe),
        ];

        for (handle, expected) in cases {
            let target = SelectionHitTarget::Resize(handle);
            assert_eq!(
                hover_cursor_for_selection_target(target, FRAC_PI_4),
                expected
            );
            assert_eq!(
                active_cursor_for_selection_target(target, FRAC_PI_4),
                expected
            );
        }
    }
}

#[cfg(any())]
pub(crate) fn resize_cursor_for_corner(corner: RectCorner, rotation: f64) -> CursorStyle {
    resize_cursor_for_handle(ResizeHandle::from_corner(corner), rotation)
}

use super::*;
use snow_draw_engine_core::arrow::{ArrowEndpointEdge, EngineContext};
pub(crate) use snow_draw_engine_document::{
    ArrowEndpointDragOptions, ArrowFocusDragOptions, compute_arrow_endpoint_drag,
    compute_arrow_focus_drag, drag_elbow_arrow_segment, preview_elbow_arrow_endpoint_binding,
    recompute_arrow_after_bindable_change, visible_arrow_focus_points,
};

// Keep these interaction constants in sync with Excalidraw's common constants.
pub(crate) const MINIMUM_ARROW_SIZE_PX: f64 = 20.0;
pub(crate) const LINE_CONFIRM_THRESHOLD_PX: f64 = 8.0;
pub(crate) const DRAGGING_THRESHOLD_PX: f64 = 10.0;
pub(crate) const MIN_COMMITTED_ARROW_LENGTH: f64 = 10.0;
pub(crate) const MIN_COMMITTED_LINE_LENGTH: f64 = 8.0;
const SHIFT_LOCKING_ANGLE: f64 = std::f64::consts::PI / 12.0;

/// Quantize an angle to the same 15-degree increments used by the
/// Shift-constrained linear drawing interaction.
pub(crate) fn lock_rotation_to_discrete_angle(rotation: f64) -> f64 {
    (rotation / SHIFT_LOCKING_ANGLE).round() * SHIFT_LOCKING_ANGLE
}

pub(crate) fn arrow_with_style(
    arrow_id: ElementId,
    arrow: &ArrowData,
    style: ArrowStyle,
    bindables: &[BindableElementState],
    context: EngineContext,
) -> ArrowData {
    let mut next = arrow.clone();
    next.stroke = style.stroke;
    next.stroke_width = style.stroke_width;
    next.start_arrowhead = style.start_arrowhead;
    next.end_arrowhead = style.end_arrowhead;
    next.stroke_style = style.stroke_style;
    next.arrow_type = style.arrow_type;

    if arrow.arrow_type == style.arrow_type {
        return next;
    }

    let points = if style.arrow_type.is_elbow() {
        default_elbow_arrow_points(arrow.start(), arrow.end())
    } else if arrow.is_elbow() {
        vec![arrow.start(), arrow.end()]
    } else {
        arrow.global_points()
    };
    let Some(mut updated) = ArrowData::from_global_points(
        &points,
        style.stroke,
        style.stroke_width,
        style.stroke_style,
        style.arrow_type,
        style.start_arrowhead,
        style.end_arrowhead,
    ) else {
        return next;
    };
    updated.inherit_linear_metadata_from(arrow);
    updated.rotation = arrow.rotation;
    updated.start_binding = arrow.start_binding.clone();
    updated.end_binding = arrow.end_binding.clone();

    recompute_arrow_after_bindable_change(arrow_id, &updated, bindables, &[], context).arrow
}

pub(crate) fn preview_arrow_from_points(
    points: &[Point<f64>],
    style: ArrowStyle,
) -> Option<ArrowData> {
    let points = match points {
        [] | [_] => return None,
        _ => points.to_vec(),
    };
    let arrow = ArrowData::from_global_points(
        &points,
        style.stroke,
        style.stroke_width,
        style.stroke_style,
        style.arrow_type,
        style.start_arrowhead,
        style.end_arrowhead,
    )?;
    (arrow_length(&arrow) > 1e-6).then_some(arrow)
}

pub(crate) fn visible_arrow_point_indices(arrow: &ArrowData) -> Vec<usize> {
    if arrow.points.is_empty() {
        return Vec::new();
    }
    if arrow.is_elbow() || arrow.points.len() == 1 {
        return vec![0, arrow.points.len().saturating_sub(1)]
            .into_iter()
            .collect::<std::collections::BTreeSet<_>>()
            .into_iter()
            .collect();
    }
    (0..arrow.points.len()).collect()
}

pub(crate) fn move_linear_arrow_point(
    arrow: &ArrowData,
    point_index: usize,
    point: Point<f64>,
) -> Option<ArrowData> {
    if arrow.is_elbow() || point_index == 0 || point_index + 1 >= arrow.points.len() {
        return None;
    }

    let mut global_points = arrow.global_points();
    *global_points.get_mut(point_index)? = point;

    let mut next = ArrowData::from_global_points(
        &global_points,
        arrow.stroke,
        arrow.stroke_width,
        arrow.stroke_style,
        arrow.arrow_type,
        arrow.start_arrowhead,
        arrow.end_arrowhead,
    )?;
    next.inherit_linear_metadata_from(arrow);
    next.rotation = arrow.rotation;
    next.start_binding = arrow.start_binding.clone();
    next.end_binding = arrow.end_binding.clone();
    next.start_is_special = arrow.start_is_special;
    next.end_is_special = arrow.end_is_special;
    Some(next)
}

pub(crate) fn insert_linear_arrow_segment_point(
    arrow: &ArrowData,
    segment_index: usize,
    point: Point<f64>,
) -> Option<ArrowData> {
    if arrow.is_elbow() || segment_index == 0 || segment_index >= arrow.points.len() {
        return None;
    }

    let mut global_points = arrow.global_points();
    global_points.insert(segment_index, point);

    let mut next = ArrowData::from_global_points(
        &global_points,
        arrow.stroke,
        arrow.stroke_width,
        arrow.stroke_style,
        arrow.arrow_type,
        arrow.start_arrowhead,
        arrow.end_arrowhead,
    )?;
    next.inherit_linear_metadata_from(arrow);
    next.rotation = arrow.rotation;
    next.start_binding = arrow.start_binding.clone();
    next.end_binding = arrow.end_binding.clone();
    next.start_is_special = arrow.start_is_special;
    next.end_is_special = arrow.end_is_special;
    Some(next)
}

pub(crate) fn remove_linear_arrow_point(
    arrow: &ArrowData,
    point_index: usize,
) -> Option<ArrowData> {
    if arrow.is_elbow()
        || point_index == 0
        || point_index + 1 >= arrow.points.len()
        || arrow.points.len() <= 2
    {
        return None;
    }

    let mut global_points = arrow.global_points();
    global_points.remove(point_index);
    let mut next = ArrowData::from_global_points(
        &global_points,
        arrow.stroke,
        arrow.stroke_width,
        arrow.stroke_style,
        arrow.arrow_type,
        arrow.start_arrowhead,
        arrow.end_arrowhead,
    )?;
    next.inherit_linear_metadata_from(arrow);
    next.rotation = arrow.rotation;
    next.start_binding = arrow.start_binding.clone();
    next.end_binding = arrow.end_binding.clone();
    next.start_is_special = arrow.start_is_special;
    next.end_is_special = arrow.end_is_special;
    Some(next)
}

pub(crate) fn default_elbow_arrow_points(start: Point<f64>, end: Point<f64>) -> Vec<Point<f64>> {
    let dx = end.x - start.x;
    let dy = end.y - start.y;
    if dx.abs() <= 1e-6 || dy.abs() <= 1e-6 {
        return vec![start, end];
    }

    if dx.abs() >= dy.abs() {
        let mid_x = f64::midpoint(start.x, end.x);
        vec![
            start,
            Point::new(mid_x, start.y),
            Point::new(mid_x, end.y),
            end,
        ]
    } else {
        let mid_y = f64::midpoint(start.y, end.y);
        vec![
            start,
            Point::new(start.x, mid_y),
            Point::new(end.x, mid_y),
            end,
        ]
    }
}

pub(crate) fn arrow_length(arrow: &ArrowData) -> f64 {
    document_arrow_length(arrow)
}

pub(crate) fn arrow_endpoint_index(point_count: usize, edge: ArrowEndpointEdge) -> usize {
    match edge {
        ArrowEndpointEdge::Start => 0,
        ArrowEndpointEdge::End => point_count.saturating_sub(1),
    }
}

pub(crate) fn arrow_target_position(
    arrow: &ArrowData,
    target: ArrowHitTarget,
) -> Option<Point<f64>> {
    let points = arrow.global_points();
    match target {
        ArrowHitTarget::Move => None,
        ArrowHitTarget::Endpoint(edge) => points
            .get(arrow_endpoint_index(points.len(), edge))
            .copied(),
        ArrowHitTarget::FocusPoint(_) => None,
        ArrowHitTarget::Point(index) => points.get(index).copied(),
        ArrowHitTarget::Segment(index) => snow_draw_engine_document::arrow_segment_midpoints(arrow)
            .into_iter()
            .find_map(|(candidate, midpoint)| (candidate == index).then_some(midpoint)),
    }
}

pub(crate) fn snap_linear_arrow_loop_endpoint(
    arrow: &ArrowData,
    edge: ArrowEndpointEdge,
    threshold: f64,
) -> ArrowData {
    // Excalidraw only closes paths with at least three points, and closure is
    // a line/polygon behavior. Arrow endpoints must remain independent.
    if !arrow.is_line() || arrow.is_elbow() || arrow.points.len() < 3 {
        return arrow.clone();
    }
    let mut points = arrow.global_points();
    let endpoint_index = arrow_endpoint_index(points.len(), edge);
    let opposite_index = arrow_endpoint_index(
        points.len(),
        match edge {
            ArrowEndpointEdge::Start => ArrowEndpointEdge::End,
            ArrowEndpointEdge::End => ArrowEndpointEdge::Start,
        },
    );
    if point_distance(points[endpoint_index], points[opposite_index]) > threshold {
        return arrow.clone();
    }
    points[endpoint_index] = points[opposite_index];
    rebuild_linear_arrow_from_points(arrow, &points).unwrap_or_else(|| arrow.clone())
}

pub(crate) fn remove_linear_arrow_point_near_neighbor(
    arrow: &ArrowData,
    point_index: usize,
    threshold: f64,
) -> ArrowData {
    if point_index == 0 || point_index + 1 >= arrow.points.len() {
        return arrow.clone();
    }
    let points = arrow.global_points();
    let point = points[point_index];
    if point_distance(point, points[point_index - 1]) > threshold
        && point_distance(point, points[point_index + 1]) > threshold
    {
        return arrow.clone();
    }
    remove_linear_arrow_point(arrow, point_index).unwrap_or_else(|| arrow.clone())
}

fn rebuild_linear_arrow_from_points(arrow: &ArrowData, points: &[Point<f64>]) -> Option<ArrowData> {
    let mut next = ArrowData::from_global_points(
        points,
        arrow.stroke,
        arrow.stroke_width,
        arrow.stroke_style,
        arrow.arrow_type,
        arrow.start_arrowhead,
        arrow.end_arrowhead,
    )?;
    next.inherit_linear_metadata_from(arrow);
    next.rotation = arrow.rotation;
    next.start_binding = arrow.start_binding.clone();
    next.end_binding = arrow.end_binding.clone();
    next.start_is_special = arrow.start_is_special;
    next.end_is_special = arrow.end_is_special;
    Some(next)
}

pub(crate) fn point_distance(a: Point<f64>, b: Point<f64>) -> f64 {
    (a.x - b.x).hypot(a.y - b.y)
}

pub(crate) fn lock_linear_point_to_discrete_angle(
    origin: Point<f64>,
    target: Point<f64>,
) -> Point<f64> {
    let delta = Point::new(target.x - origin.x, target.y - origin.y);
    let angle = delta.y.atan2(delta.x);
    let locked_angle = (angle / SHIFT_LOCKING_ANGLE).round() * SHIFT_LOCKING_ANGLE;
    let projected_length = delta.x * locked_angle.cos() + delta.y * locked_angle.sin();
    Point::new(
        origin.x + projected_length * locked_angle.cos(),
        origin.y + projected_length * locked_angle.sin(),
    )
}

pub(crate) fn arrow_loop_active(arrow: &ArrowData, threshold: f64) -> bool {
    let points = arrow.global_points();
    arrow.is_line()
        && !arrow.is_elbow()
        && points.len() >= 3
        && point_distance(points[0], points[points.len() - 1]) <= threshold
}

pub(crate) fn segment_is_fixed(arrow: &ArrowData, segment_index: usize) -> bool {
    arrow.fixed_segments.as_ref().is_some_and(|segments| {
        segments
            .iter()
            .any(|segment| segment.index == segment_index)
    })
}

pub(crate) fn segment_handle_visible(arrow: &ArrowData, segment_index: usize, zoom: f64) -> bool {
    let points = arrow.global_points();
    let Some(start) = points.get(segment_index.saturating_sub(1)).copied() else {
        return false;
    };
    let Some(end) = points.get(segment_index).copied() else {
        return false;
    };
    point_distance(start, end) * zoom >= ARROW_SEGMENT_HANDLE_MIN_VISIBLE_PX
}

pub(crate) fn translated_arrow_for_move(arrow: &ArrowData, delta: Point<f64>) -> ArrowData {
    let mut next = arrow.clone();
    next.x += delta.x;
    next.y += delta.y;
    if delta.x.abs() > f64::EPSILON || delta.y.abs() > f64::EPSILON {
        next.start_binding = None;
        next.end_binding = None;
        next.start_is_special = None;
        next.end_is_special = None;
    }
    next
}

fn transformed_arrow_from_points(
    arrow: &ArrowData,
    transformed_points: Vec<Point<f64>>,
) -> Option<ArrowData> {
    if transformed_points == arrow.global_points() {
        return Some(arrow.clone());
    }

    let mut next = ArrowData::from_global_points(
        &transformed_points,
        arrow.stroke,
        arrow.stroke_width,
        arrow.stroke_style,
        arrow.arrow_type,
        arrow.start_arrowhead,
        arrow.end_arrowhead,
    )?;
    next.inherit_linear_metadata_from(arrow);
    next.rotation = arrow.rotation;
    next.start_binding = None;
    next.end_binding = None;
    next.start_is_special = None;
    next.end_is_special = None;
    Some(next)
}

pub(crate) fn resized_arrow_for_selection(
    arrow: &ArrowData,
    original_bounds: &SelectionBounds,
    anchor_local: Point<f64>,
    scale_x: f64,
    scale_y: f64,
) -> Option<ArrowData> {
    let transformed_points = arrow
        .global_points()
        .into_iter()
        .map(|point| {
            let local =
                canvas_to_rect_local(original_bounds.center, original_bounds.rotation, point);
            rect_local_to_canvas(
                original_bounds.center,
                original_bounds.rotation,
                scaled_point_from_anchor(local, anchor_local, scale_x, scale_y),
            )
        })
        .collect::<Vec<_>>();
    transformed_arrow_from_points(arrow, transformed_points)
}

pub(crate) fn rotated_arrow_for_selection(
    arrow: &ArrowData,
    center: Point<f64>,
    rotation_delta: f64,
) -> Option<ArrowData> {
    let transformed_points = arrow
        .global_points()
        .into_iter()
        .map(|point| rotate_point_around(point, center, rotation_delta))
        .collect::<Vec<_>>();
    let mut next = transformed_arrow_from_points(arrow, transformed_points)?;
    next.rotation = normalize_rotation(arrow.rotation + rotation_delta);
    Some(next)
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{
        ColorRgba8,
        arrow::{ArrowStrokeStyle, ArrowType},
    };

    #[test]
    fn rotating_diagonal_arrow_keeps_a_rotation_aligned_selection_frame() {
        let arrow = ArrowData::from_global_points(
            &[Point::new(0.0, 0.0), Point::new(100.0, 100.0)],
            ColorRgba8::default(),
            2.0,
            ArrowStrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .expect("two distinct points should create an arrow");
        let rotation = std::f64::consts::FRAC_PI_4;
        let rotated = rotated_arrow_for_selection(&arrow, Point::new(50.0, 50.0), rotation)
            .expect("rotating an arrow should produce a preview");

        let bounds = selection_bounds_from_selection(
            &[],
            &[SelectionArrowState {
                id: ElementId::default(),
                arrow: rotated.clone(),
            }],
        )
        .expect("a non-degenerate arrow should have selection bounds");

        assert!((rotated.rotation - rotation).abs() < 1e-9);
        assert!((bounds.rotation - rotation).abs() < 1e-9);
        assert!((bounds.center.x - 50.0).abs() < 1e-9);
        assert!((bounds.center.y - 50.0).abs() < 1e-9);
        assert!((bounds.width - 100.0).abs() < 1e-9);
        assert!((bounds.height - 100.0).abs() < 1e-9);
    }

    #[test]
    fn line_resize_and_rotation_preserve_identity_fill_and_canonical_route() {
        let fill = ColorRgba8 {
            r: 10,
            g: 20,
            b: 30,
            a: 128,
        };
        let line = ArrowData::from_global_points(
            &[
                Point::new(0.0, 0.0),
                Point::new(50.0, 100.0),
                Point::new(100.0, 0.0),
            ],
            ColorRgba8::default(),
            2.0,
            ArrowStrokeStyle::Dotted,
            ArrowType::Curve,
            None,
            None,
        )
        .unwrap()
        .into_line(fill, snow_draw_engine_document::FillStyle::Line);
        let original_bounds = selection_bounds_from_selection(
            &[],
            &[SelectionArrowState {
                id: ElementId::default(),
                arrow: line.clone(),
            }],
        )
        .unwrap();
        let resized = resized_arrow_for_selection(
            &line,
            &original_bounds,
            Point::new(-original_bounds.width / 2.0, 0.0),
            2.0,
            0.5,
        )
        .unwrap();
        let rotated = rotated_arrow_for_selection(
            &resized,
            original_bounds.center,
            std::f64::consts::FRAC_PI_4,
        )
        .unwrap();

        assert!(rotated.is_line());
        assert_eq!(rotated.fill, fill);
        assert_eq!(
            rotated.fill_style,
            snow_draw_engine_document::FillStyle::Line
        );
        assert_eq!(rotated.arrow_type, ArrowType::Curve);
        assert_eq!(rotated.start_arrowhead, None);
        assert_eq!(rotated.end_arrowhead, None);
    }

    #[test]
    fn line_endpoint_snaps_closed_within_reference_loop_threshold() {
        let line = ArrowData::from_global_points(
            &[
                Point::new(0.0, 0.0),
                Point::new(50.0, 100.0),
                Point::new(8.0, 0.0),
            ],
            ColorRgba8::default(),
            2.0,
            ArrowStrokeStyle::Solid,
            ArrowType::Curve,
            None,
            None,
        )
        .unwrap()
        .into_line(
            ColorRgba8::default(),
            snow_draw_engine_document::FillStyle::Solid,
        );

        let closed = snap_linear_arrow_loop_endpoint(&line, ArrowEndpointEdge::End, 9.0);

        assert_eq!(closed.start(), closed.end());
        assert!(closed.is_line());
    }

    #[test]
    fn arrow_endpoints_do_not_snap_together() {
        let arrow = ArrowData::from_global_points(
            &[
                Point::new(0.0, 0.0),
                Point::new(50.0, 100.0),
                Point::new(8.0, 0.0),
            ],
            ColorRgba8::default(),
            2.0,
            ArrowStrokeStyle::Solid,
            ArrowType::Curve,
            None,
            Some(snow_draw_engine_core::arrow::Arrowhead::Arrow),
        )
        .unwrap();

        let end_drag = snap_linear_arrow_loop_endpoint(&arrow, ArrowEndpointEdge::End, 9.0);
        let start_drag = snap_linear_arrow_loop_endpoint(&arrow, ArrowEndpointEdge::Start, 9.0);

        assert_eq!(end_drag, arrow);
        assert_eq!(start_drag, arrow);
        assert_ne!(end_drag.start(), end_drag.end());
        assert!(!arrow_loop_active(&end_drag, 9.0));
    }

    #[test]
    fn two_point_line_does_not_collapse_into_a_loop() {
        let line = ArrowData::from_global_points(
            &[Point::new(0.0, 0.0), Point::new(8.0, 0.0)],
            ColorRgba8::default(),
            2.0,
            ArrowStrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap()
        .into_line(
            ColorRgba8::default(),
            snow_draw_engine_document::FillStyle::Solid,
        );

        let result = snap_linear_arrow_loop_endpoint(&line, ArrowEndpointEdge::End, 9.0);

        assert_eq!(result, line);
        assert_ne!(result.start(), result.end());
        assert!(!arrow_loop_active(&result, 9.0));
    }

    #[test]
    fn interior_line_point_is_deleted_when_released_near_neighbor() {
        let line = ArrowData::from_global_points(
            &[
                Point::new(0.0, 0.0),
                Point::new(5.0, 1.0),
                Point::new(100.0, 0.0),
            ],
            ColorRgba8::default(),
            2.0,
            ArrowStrokeStyle::Solid,
            ArrowType::Curve,
            None,
            None,
        )
        .unwrap()
        .into_line(
            ColorRgba8::default(),
            snow_draw_engine_document::FillStyle::Solid,
        );

        let simplified = remove_linear_arrow_point_near_neighbor(&line, 1, 6.0);

        assert_eq!(simplified.global_points().len(), 2);
        assert!(simplified.is_line());
    }

    #[test]
    fn shift_lock_projects_to_excalidraw_fifteen_degree_increments() {
        let origin = Point::new(10.0, 20.0);
        let locked = lock_linear_point_to_discrete_angle(origin, Point::new(110.0, 25.0));

        assert!((locked.y - origin.y).abs() < 1e-9);
        assert!((locked.x - 110.0).abs() < 1e-9);

        let diagonal = lock_linear_point_to_discrete_angle(origin, Point::new(90.0, 100.0));
        assert!((diagonal.x - 90.0).abs() < 1e-9);
        assert!((diagonal.y - 100.0).abs() < 1e-9);
    }

    #[test]
    fn removing_an_interior_line_point_preserves_line_metadata() {
        let line = ArrowData::from_global_points(
            &[
                Point::new(0.0, 0.0),
                Point::new(30.0, 40.0),
                Point::new(60.0, -40.0),
                Point::new(100.0, 0.0),
            ],
            ColorRgba8::default(),
            3.0,
            ArrowStrokeStyle::Dashed,
            ArrowType::Curve,
            None,
            None,
        )
        .unwrap()
        .into_line(
            ColorRgba8 {
                r: 1,
                g: 2,
                b: 3,
                a: 90,
            },
            snow_draw_engine_document::FillStyle::CrossLine,
        );

        let updated = remove_linear_arrow_point(&line, 1).unwrap();

        assert!(updated.is_line());
        assert_eq!(updated.global_points().len(), 3);
        assert_eq!(updated.global_points()[1], Point::new(60.0, -40.0));
        assert_eq!(updated.fill, line.fill);
        assert_eq!(updated.fill_style, line.fill_style);
        assert_eq!(updated.stroke_style, line.stroke_style);
        assert!(snow_draw_engine_document::validate_arrow(&updated).is_ok());

        let inserted =
            insert_linear_arrow_segment_point(&updated, 1, Point::new(20.0, 15.0)).unwrap();
        assert!(snow_draw_engine_document::validate_arrow(&inserted).is_ok());
    }
}

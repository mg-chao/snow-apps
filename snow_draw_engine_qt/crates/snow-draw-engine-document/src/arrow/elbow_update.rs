#[path = "elbow_update_endpoints.rs"]
mod endpoints;
#[path = "elbow_update_segments.rs"]
mod segments;

use std::collections::BTreeMap;

use endpoints::{EndpointDragRouteInput, handle_endpoint_drag};
use segments::{handle_segment_move, handle_segment_renormalization};

use crate::arrow_binding_core::{
    bind_point_to_outline, get_heading_for_elbow_snap as resolve_heading_for_elbow_snap,
};
use crate::arrow_elbow_router::{
    BASE_BINDING_GAP_ELBOW, DEDUP_THRESHOLD, EndpointRoute, ensure_orthogonal, offset_from_heading,
    point_bounds, remove_short_segments, route_between_points,
};
use crate::arrow_geom::{
    Heading, apply_fixed_segments_to_global_points, clamp, dedupe_collinear_points,
    get_global_fixed_point, get_point_at_index_global, heading_from_bindable,
    is_horizontal_heading, normalize_arrow_from_global_points, rotated_bindable_bounds,
    vector_to_heading,
};
use crate::arrow_hit_test::{distance_to_bindable_outline, get_hovered_bindable};
use crate::{
    ArrowEndpointEdge, ArrowPatch, ArrowState, BindableState, Bounds, ElbowUpdatePatch, ElementId,
    FixedSegment, PatchValue, Point,
};

pub use crate::arrow_elbow_router::BASE_PADDING;

const MAX_POS: f64 = 1_000_000.0;

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct UpdateElbowArrowPointsOptions {
    pub is_dragging: Option<bool>,
    pub zoom: Option<f64>,
    pub validate_invariants: Option<bool>,
    pub max_coordinate: Option<f64>,
    pub midpoint_snapping_enabled: Option<bool>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum EndpointSide {
    Start,
    End,
}

fn patch_value_or_current<T: Clone>(patch: &PatchValue<T>, current: &Option<T>) -> Option<T> {
    match patch {
        Some(value) => value.clone(),
        None => current.clone(),
    }
}

fn apply_point_update(points: &[Point], updated_points: Option<&[Point]>) -> Vec<Point> {
    let Some(updated_points) = updated_points else {
        return points.to_vec();
    };

    if updated_points.len() == 2 && points.len() > 2 {
        return points
            .iter()
            .enumerate()
            .map(|(index, point)| {
                if index == 0 {
                    updated_points[0]
                } else if index + 1 == points.len() {
                    updated_points[1]
                } else {
                    *point
                }
            })
            .collect();
    }

    updated_points.to_vec()
}

fn resolve_max_coordinate(max_coordinate: Option<f64>) -> f64 {
    let Some(max_coordinate) = max_coordinate else {
        return MAX_POS;
    };
    if !max_coordinate.is_finite() || max_coordinate <= 0.0 {
        return MAX_POS;
    }
    max_coordinate
}

pub fn get_binding_gap(bindable: &BindableState, elbowed: bool) -> f64 {
    let base_gap = if elbowed { BASE_BINDING_GAP_ELBOW } else { 5.0 };
    base_gap + bindable.stroke_width / 2.0
}

pub fn max_binding_distance(zoom: f64) -> f64 {
    let base_distance = BASE_BINDING_GAP_ELBOW.max(15.0);
    let safe_zoom = if zoom < 1.0 { zoom.max(1e-6) } else { 1.0 };
    clamp(
        base_distance / (safe_zoom * 1.5),
        base_distance,
        base_distance * 2.0,
    )
}

fn aabb_for_bindable(bindable: &BindableState, offset: Option<[f64; 4]>) -> Bounds {
    rotated_bindable_bounds(bindable, offset.unwrap_or([0.0; 4]))
}

fn obstacle_for_bindable(bindable: &BindableState, gap: f64) -> Bounds {
    aabb_for_bindable(bindable, Some([gap, gap, gap, gap]))
}

pub fn validate_elbow_points(points: &[Point], tolerance: Option<f64>) -> bool {
    let tolerance = tolerance.unwrap_or(DEDUP_THRESHOLD);
    points.windows(2).all(|segment| {
        let start = segment[0];
        let end = segment[1];
        (start[0] - end[0]).abs() < tolerance || (start[1] - end[1]).abs() < tolerance
    })
}

fn sanitize_fixed_segments(
    fixed_segments: Option<Vec<FixedSegment>>,
    point_count: usize,
) -> Option<Vec<FixedSegment>> {
    let fixed_segments = fixed_segments?;

    let next = fixed_segments
        .into_iter()
        .filter(|segment| segment.index > 0 && segment.index < point_count)
        .collect::<Vec<_>>();
    if next.is_empty() { None } else { Some(next) }
}

pub fn get_heading_for_elbow_snap(
    point: Point,
    other_point: Point,
    bindable: Option<&BindableState>,
    zoom: f64,
) -> Heading {
    let fallback = vector_to_heading(point, other_point);
    let Some(bindable) = bindable else {
        return fallback;
    };

    let bind_distance = max_binding_distance(zoom);
    let distance = distance_to_bindable_outline(point, bindable);
    if distance > bind_distance {
        return fallback;
    }

    heading_from_bindable(point, bindable)
}

fn hovered_bindable_for_point(
    point: Point,
    bindables_by_id: &BTreeMap<ElementId, BindableState>,
    zoom: f64,
) -> Option<BindableState> {
    let bindables = bindables_by_id.values().cloned().collect::<Vec<_>>();
    get_hovered_bindable(point, &bindables, max_binding_distance(zoom))
}

fn resolve_endpoint_bindable(
    arrow: &ArrowState,
    bindables_by_id: &BTreeMap<ElementId, BindableState>,
    side: EndpointSide,
    is_dragging: bool,
    zoom: f64,
) -> Option<BindableState> {
    let point = match side {
        EndpointSide::Start => get_point_at_index_global(arrow, 0),
        EndpointSide::End => get_point_at_index_global(arrow, -1),
    };
    if is_dragging {
        return hovered_bindable_for_point(point, bindables_by_id, zoom);
    }

    let binding = match side {
        EndpointSide::Start => arrow.start_binding.as_ref(),
        EndpointSide::End => arrow.end_binding.as_ref(),
    };
    binding.and_then(|binding| bindables_by_id.get(&binding.element_id).cloned())
}

fn resolve_endpoint_point(
    arrow: &ArrowState,
    side: EndpointSide,
    binding: Option<&crate::FixedPointBinding>,
    bindable: Option<&BindableState>,
    initial_point: Point,
    is_dragging: bool,
    midpoint_snapping_enabled: bool,
) -> Point {
    if is_dragging {
        let Some(bindable) = bindable else {
            return initial_point;
        };
        let edge = match side {
            EndpointSide::Start => ArrowEndpointEdge::Start,
            EndpointSide::End => ArrowEndpointEdge::End,
        };
        return bind_point_to_outline(arrow, bindable, edge, None, midpoint_snapping_enabled);
    }

    match (binding, bindable) {
        (Some(binding), Some(bindable)) => get_global_fixed_point(binding, bindable),
        _ => initial_point,
    }
}

fn resolve_endpoint_heading(
    point: Point,
    other_point: Point,
    bindable: Option<&BindableState>,
    origin_point: Point,
    zoom: f64,
) -> Heading {
    let Some(bindable) = bindable else {
        return vector_to_heading(point, other_point);
    };

    let distance_at_bind_point = distance_to_bindable_outline(point, bindable);
    let aabb = obstacle_for_bindable(bindable, distance_at_bind_point);
    resolve_heading_for_elbow_snap(
        point,
        other_point,
        Some(bindable),
        Some(aabb),
        Some(origin_point),
        Some(zoom),
    )
}

fn resolve_endpoint(
    arrow: &ArrowState,
    bindables_by_id: &BTreeMap<ElementId, BindableState>,
    side: EndpointSide,
    other_point: Point,
    is_dragging: bool,
    zoom: f64,
    midpoint_snapping_enabled: bool,
) -> EndpointRoute {
    let point = match side {
        EndpointSide::Start => get_point_at_index_global(arrow, 0),
        EndpointSide::End => get_point_at_index_global(arrow, -1),
    };
    let binding = match side {
        EndpointSide::Start => arrow.start_binding.as_ref(),
        EndpointSide::End => arrow.end_binding.as_ref(),
    };
    let bindable = resolve_endpoint_bindable(arrow, bindables_by_id, side, is_dragging, zoom);
    let resolved_point = resolve_endpoint_point(
        arrow,
        side,
        binding,
        bindable.as_ref(),
        point,
        is_dragging,
        midpoint_snapping_enabled,
    );
    let heading =
        resolve_endpoint_heading(resolved_point, other_point, bindable.as_ref(), point, zoom);
    let has_arrowhead = match side {
        EndpointSide::Start => arrow.start_arrowhead.is_some(),
        EndpointSide::End => arrow.end_arrowhead.is_some(),
    };
    let bindable_bounds = bindable
        .as_ref()
        .map(|bindable| aabb_for_bindable(bindable, None));
    let element_bounds = if let Some(bindable) = bindable.as_ref() {
        let head = get_binding_gap(bindable, true) * if has_arrowhead { 6.0 } else { 2.0 };
        aabb_for_bindable(bindable, Some(offset_from_heading(heading, head, 1.0)))
    } else {
        point_bounds(resolved_point)
    };
    let overlap_bounds = if let Some(bindable) = bindable.as_ref() {
        aabb_for_bindable(
            bindable,
            Some(offset_from_heading(heading, BASE_PADDING, BASE_PADDING)),
        )
    } else {
        point_bounds(resolved_point)
    };

    EndpointRoute {
        point: resolved_point,
        heading,
        element_bounds,
        overlap_bounds,
        bindable_bounds,
        has_arrowhead,
    }
}

fn build_working_arrow(
    arrow: &ArrowState,
    updates: &ElbowUpdatePatch,
) -> (ArrowState, Option<Vec<FixedSegment>>) {
    let points = apply_point_update(&arrow.points, updates.points.as_deref());
    let fixed_segments = patch_value_or_current(&updates.fixed_segments, &arrow.fixed_segments);
    let start_binding = patch_value_or_current(&updates.start_binding, &arrow.start_binding);
    let end_binding = patch_value_or_current(&updates.end_binding, &arrow.end_binding);
    let fixed_segments = sanitize_fixed_segments(fixed_segments, points.len());

    (
        ArrowState {
            id: arrow.id,
            x: arrow.x,
            y: arrow.y,
            width: arrow.width,
            height: arrow.height,
            points,
            start_binding,
            end_binding,
            start_arrowhead: arrow.start_arrowhead,
            end_arrowhead: arrow.end_arrowhead,
            elbowed: arrow.elbowed,
            fixed_segments: fixed_segments.clone(),
            start_is_special: arrow.start_is_special,
            end_is_special: arrow.end_is_special,
        },
        fixed_segments,
    )
}

pub(super) fn to_global_point(origin: Point, local: Point) -> Point {
    [origin[0] + local[0], origin[1] + local[1]]
}

pub(super) fn to_local_point(origin: Point, global: Point) -> Point {
    [global[0] - origin[0], global[1] - origin[1]]
}

pub(super) fn points_equal(left: Point, right: Point) -> bool {
    (left[0] - right[0]).abs() <= 1e-6 && (left[1] - right[1]).abs() <= 1e-6
}

pub(super) fn heading_for_point_is_horizontal(point: Point, origin: Point) -> bool {
    is_horizontal_heading(vector_to_heading(origin, point))
}

pub(super) fn global_points_for_arrow(arrow: &ArrowState) -> Vec<Point> {
    arrow
        .points
        .iter()
        .map(|point| to_global_point([arrow.x, arrow.y], *point))
        .collect()
}

fn fixed_segments_patch_value(
    fixed_segments: Option<Vec<FixedSegment>>,
) -> PatchValue<Vec<FixedSegment>> {
    Some(fixed_segments.filter(|segments| !segments.is_empty()))
}

pub(super) fn normalize_arrow_element_update(
    global_points: &[Point],
    fixed_segments: Option<Vec<FixedSegment>>,
    start_is_special: PatchValue<bool>,
    end_is_special: PatchValue<bool>,
    max_coordinate: f64,
) -> ArrowPatch {
    let normalized = normalize_arrow_from_global_points(global_points, max_coordinate);
    ArrowPatch {
        x: Some(normalized.x),
        y: Some(normalized.y),
        width: Some(normalized.width),
        height: Some(normalized.height),
        points: Some(normalized.points),
        fixed_segments: fixed_segments_patch_value(fixed_segments),
        start_is_special,
        end_is_special,
        ..ArrowPatch::default()
    }
}

fn has_elbow_updates(updates: &ElbowUpdatePatch) -> bool {
    updates.points.is_some()
        || updates.fixed_segments.is_some()
        || updates.start_binding.is_some()
        || updates.end_binding.is_some()
}

pub(super) fn compute_default_route_patch(
    working_arrow: &ArrowState,
    bindables_by_id: &BTreeMap<ElementId, BindableState>,
    zoom: f64,
    is_dragging: bool,
    max_coordinate: f64,
    midpoint_snapping_enabled: bool,
) -> ArrowPatch {
    let start_point = resolve_endpoint_point(
        working_arrow,
        EndpointSide::Start,
        working_arrow.start_binding.as_ref(),
        resolve_endpoint_bindable(
            working_arrow,
            bindables_by_id,
            EndpointSide::Start,
            is_dragging,
            zoom,
        )
        .as_ref(),
        get_point_at_index_global(working_arrow, 0),
        is_dragging,
        midpoint_snapping_enabled,
    );
    let end_point = resolve_endpoint_point(
        working_arrow,
        EndpointSide::End,
        working_arrow.end_binding.as_ref(),
        resolve_endpoint_bindable(
            working_arrow,
            bindables_by_id,
            EndpointSide::End,
            is_dragging,
            zoom,
        )
        .as_ref(),
        get_point_at_index_global(working_arrow, -1),
        is_dragging,
        midpoint_snapping_enabled,
    );
    let start = resolve_endpoint(
        working_arrow,
        bindables_by_id,
        EndpointSide::Start,
        end_point,
        is_dragging,
        zoom,
        midpoint_snapping_enabled,
    );
    let end = resolve_endpoint(
        working_arrow,
        bindables_by_id,
        EndpointSide::End,
        start_point,
        is_dragging,
        zoom,
        midpoint_snapping_enabled,
    );

    let obstacles = bindables_by_id
        .values()
        .map(|bindable| obstacle_for_bindable(bindable, get_binding_gap(bindable, true)))
        .collect::<Vec<_>>();

    let mut global_points = route_between_points(start, end, &obstacles)
        .unwrap_or_else(|| ensure_orthogonal(&[start.point, end.point]));
    global_points = apply_fixed_segments_to_global_points(global_points, working_arrow);
    global_points =
        dedupe_collinear_points(&remove_short_segments(&ensure_orthogonal(&global_points)));

    if !validate_elbow_points(&global_points, None) {
        global_points = ensure_orthogonal(&[start.point, end.point]);
    }

    normalize_patch_from_global_points(&global_points, max_coordinate)
}

fn normalize_patch_from_global_points(points: &[Point], max_coordinate: f64) -> ArrowPatch {
    let normalized = normalize_arrow_from_global_points(points, max_coordinate);
    ArrowPatch {
        x: Some(normalized.x),
        y: Some(normalized.y),
        width: Some(normalized.width),
        height: Some(normalized.height),
        points: Some(normalized.points),
        ..ArrowPatch::default()
    }
}

pub fn update_elbow_arrow_points(
    arrow: &ArrowState,
    bindables_by_id: &BTreeMap<ElementId, BindableState>,
    updates: &ElbowUpdatePatch,
    options: Option<UpdateElbowArrowPointsOptions>,
) -> ArrowPatch {
    let max_coordinate =
        resolve_max_coordinate(options.as_ref().and_then(|value| value.max_coordinate));
    if arrow.points.len() < 2 {
        return ArrowPatch {
            points: updates
                .points
                .clone()
                .or_else(|| Some(arrow.points.clone())),
            ..ArrowPatch::default()
        };
    }

    if options
        .as_ref()
        .and_then(|value| value.validate_invariants)
        .unwrap_or(false)
        && let Some(points) = updates.points.as_ref()
    {
        let valid_count = points.len() == 2 || points.len() == arrow.points.len();
        assert!(
            valid_count,
            "updated elbow point count must match or provide only endpoints"
        );
    }

    let (working_arrow, sanitized_fixed_segments) = build_working_arrow(arrow, updates);
    if working_arrow.points.len() < 2 {
        return ArrowPatch {
            points: Some(working_arrow.points),
            ..ArrowPatch::default()
        };
    }

    let zoom = options.as_ref().and_then(|value| value.zoom).unwrap_or(1.0);
    let is_dragging = options
        .as_ref()
        .and_then(|value| value.is_dragging)
        .unwrap_or(false);
    let midpoint_snapping_enabled = options
        .as_ref()
        .and_then(|value| value.midpoint_snapping_enabled)
        .unwrap_or(true);
    if !has_elbow_updates(updates) && arrow.fixed_segments.is_some() {
        return handle_segment_renormalization(
            arrow,
            bindables_by_id,
            zoom,
            max_coordinate,
            midpoint_snapping_enabled,
        );
    }

    let fixed_segments = sanitized_fixed_segments.clone().unwrap_or_default();
    if fixed_segments.is_empty() {
        let mut patch = compute_default_route_patch(
            &working_arrow,
            bindables_by_id,
            zoom,
            is_dragging,
            max_coordinate,
            midpoint_snapping_enabled,
        );
        if sanitized_fixed_segments != arrow.fixed_segments || updates.fixed_segments.is_some() {
            patch.fixed_segments = Some(sanitized_fixed_segments);
        }
        return patch;
    }

    if arrow
        .fixed_segments
        .as_ref()
        .map_or(0, |segments| segments.len())
        > fixed_segments.len()
    {
        let mut reroute_arrow = working_arrow.clone();
        reroute_arrow.fixed_segments = Some(fixed_segments.clone());
        let mut patch = compute_default_route_patch(
            &reroute_arrow,
            bindables_by_id,
            zoom,
            is_dragging,
            max_coordinate,
            midpoint_snapping_enabled,
        );
        patch.fixed_segments = fixed_segments_patch_value(Some(fixed_segments));
        return patch;
    }

    let start_bindable = resolve_endpoint_bindable(
        &working_arrow,
        bindables_by_id,
        EndpointSide::Start,
        is_dragging,
        zoom,
    );
    let end_bindable = resolve_endpoint_bindable(
        &working_arrow,
        bindables_by_id,
        EndpointSide::End,
        is_dragging,
        zoom,
    );
    let start_point = resolve_endpoint_point(
        &working_arrow,
        EndpointSide::Start,
        working_arrow.start_binding.as_ref(),
        start_bindable.as_ref(),
        get_point_at_index_global(&working_arrow, 0),
        is_dragging,
        midpoint_snapping_enabled,
    );
    let end_point = resolve_endpoint_point(
        &working_arrow,
        EndpointSide::End,
        working_arrow.end_binding.as_ref(),
        end_bindable.as_ref(),
        get_point_at_index_global(&working_arrow, -1),
        is_dragging,
        midpoint_snapping_enabled,
    );
    let start_heading = resolve_endpoint_heading(
        start_point,
        end_point,
        start_bindable.as_ref(),
        get_point_at_index_global(&working_arrow, 0),
        zoom,
    );
    let end_heading = resolve_endpoint_heading(
        end_point,
        start_point,
        end_bindable.as_ref(),
        get_point_at_index_global(&working_arrow, -1),
        zoom,
    );

    if updates.points.is_none() {
        return handle_segment_move(
            arrow,
            fixed_segments,
            start_heading,
            end_heading,
            start_bindable.as_ref(),
            end_bindable.as_ref(),
            max_coordinate,
        );
    }

    if updates.points.is_some() && updates.fixed_segments.is_some() {
        return ArrowPatch {
            points: updates.points.clone(),
            fixed_segments: updates.fixed_segments.clone(),
            ..ArrowPatch::default()
        };
    }

    handle_endpoint_drag(EndpointDragRouteInput {
        arrow,
        updated_points: &working_arrow.points,
        fixed_segments: &fixed_segments,
        start_heading,
        end_heading,
        start_global_point: start_point,
        end_global_point: end_point,
        hovered_start_bindable: start_bindable.as_ref(),
        hovered_end_bindable: end_bindable.as_ref(),
        max_coordinate,
    })
}

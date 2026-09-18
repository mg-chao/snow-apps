use std::collections::BTreeMap;

use crate::arrow_elbow_update::{
    UpdateElbowArrowPointsOptions, get_binding_gap, get_heading_for_elbow_snap,
    update_elbow_arrow_points as update_elbow_points_impl,
    validate_elbow_points as validate_elbow_points_impl,
};
use crate::arrow_geom::{
    Heading, apply_fixed_segments_to_global_points, dedupe_collinear_points,
    get_global_fixed_point, get_point_at_index_global, is_horizontal_heading, is_orthogonal_path,
    normalize_arrow_from_global_points, rotated_bindable_bounds, vector_to_heading,
};
use crate::{
    ArrowPatch, ArrowState, BindableState, Bounds, ElbowUpdatePatch, ElementId, FixedSegment,
    MoveFixedSegmentToPointResult, Point, RecomputeElbowInput, UpdateElbowArrowInput,
};

const DEDUP_THRESHOLD: f64 = 1.0;

fn obstacle_for_bindable(bindable: &BindableState, gap: f64) -> Bounds {
    rotated_bindable_bounds(bindable, [gap, gap, gap, gap])
}

fn ensure_orthogonal(points: &[Point]) -> Vec<Point> {
    if points.len() < 2 {
        return points.to_vec();
    }

    let mut result = vec![points[0]];
    for point in points.iter().skip(1) {
        let previous = *result.last().unwrap();
        let same_x = (previous[0] - point[0]).abs() <= 1e-6;
        let same_y = (previous[1] - point[1]).abs() <= 1e-6;
        if same_x || same_y {
            result.push(*point);
            continue;
        }
        result.push([point[0], previous[1]]);
        result.push(*point);
    }

    dedupe_collinear_points(&result)
}

fn compute_endpoint_and_heading(
    arrow: &ArrowState,
    bindables_by_id: &BTreeMap<ElementId, BindableState>,
    is_start: bool,
    zoom: f64,
) -> (Point, Heading, Option<Bounds>) {
    let binding = if is_start {
        arrow.start_binding.as_ref()
    } else {
        arrow.end_binding.as_ref()
    };
    let point = get_point_at_index_global(arrow, if is_start { 0 } else { -1 });
    let other_point = get_point_at_index_global(arrow, if is_start { -1 } else { 0 });

    let Some(binding) = binding else {
        return (
            point,
            get_heading_for_elbow_snap(point, other_point, None, zoom),
            None,
        );
    };
    let Some(bindable) = bindables_by_id.get(&binding.element_id) else {
        return (
            point,
            get_heading_for_elbow_snap(point, other_point, None, zoom),
            None,
        );
    };

    let fixed_point = get_global_fixed_point(binding, bindable);
    (
        fixed_point,
        get_heading_for_elbow_snap(fixed_point, other_point, Some(bindable), zoom),
        Some(obstacle_for_bindable(
            bindable,
            get_binding_gap(bindable, true),
        )),
    )
}

fn recompute_elbow_fallback_patch(input: &RecomputeElbowInput) -> ArrowPatch {
    let bindables_by_id = input
        .bindables
        .iter()
        .cloned()
        .map(|bindable| (bindable.id, bindable))
        .collect::<BTreeMap<_, _>>();

    let (start_point, _, _) =
        compute_endpoint_and_heading(&input.arrow, &bindables_by_id, true, input.context.zoom);
    let (end_point, _, _) =
        compute_endpoint_and_heading(&input.arrow, &bindables_by_id, false, input.context.zoom);

    let points = apply_fixed_segments_to_global_points(
        ensure_orthogonal(&[start_point, end_point]),
        &input.arrow,
    );
    let normalized = normalize_arrow_from_global_points(&points, input.context.max_coordinate);

    ArrowPatch {
        x: Some(normalized.x),
        y: Some(normalized.y),
        width: Some(normalized.width),
        height: Some(normalized.height),
        points: Some(normalized.points),
        ..ArrowPatch::default()
    }
}

pub fn update_elbow_arrow_patch(input: UpdateElbowArrowInput) -> ArrowPatch {
    let bindables_by_id = input
        .bindables
        .iter()
        .cloned()
        .map(|bindable| (bindable.id, bindable))
        .collect::<BTreeMap<_, _>>();

    let mut patch = update_elbow_points_impl(
        &input.arrow,
        &bindables_by_id,
        &input.updates,
        Some(UpdateElbowArrowPointsOptions {
            is_dragging: input
                .options
                .as_ref()
                .and_then(|options| options.is_dragging),
            zoom: Some(input.context.zoom),
            validate_invariants: input
                .options
                .as_ref()
                .and_then(|options| options.validate_invariants),
            max_coordinate: Some(input.context.max_coordinate),
            midpoint_snapping_enabled: input
                .options
                .as_ref()
                .and_then(|options| options.midpoint_snapping_enabled),
        }),
    );

    if patch.fixed_segments.is_none() && input.updates.fixed_segments.is_some() {
        patch.fixed_segments = input.updates.fixed_segments.clone();
    }
    if input.updates.start_binding.is_some() {
        patch.start_binding = input.updates.start_binding;
    }
    if input.updates.end_binding.is_some() {
        patch.end_binding = input.updates.end_binding;
    }

    patch
}

pub fn recompute_elbow_patch(input: RecomputeElbowInput) -> ArrowPatch {
    let bindables_by_id = input
        .bindables
        .iter()
        .cloned()
        .map(|bindable| (bindable.id, bindable))
        .collect::<BTreeMap<_, _>>();

    let patch = update_elbow_points_impl(
        &input.arrow,
        &bindables_by_id,
        &ElbowUpdatePatch::default(),
        Some(UpdateElbowArrowPointsOptions {
            is_dragging: Some(false),
            zoom: Some(input.context.zoom),
            validate_invariants: None,
            max_coordinate: Some(input.context.max_coordinate),
            midpoint_snapping_enabled: None,
        }),
    );

    let resolved_x = patch.x.unwrap_or(input.arrow.x);
    let resolved_y = patch.y.unwrap_or(input.arrow.y);
    let resolved_points = patch
        .points
        .as_ref()
        .cloned()
        .unwrap_or_else(|| input.arrow.points.clone());

    let is_out_of_bounds = resolved_x.abs() > input.context.max_coordinate
        || resolved_y.abs() > input.context.max_coordinate;
    let has_invalid_path =
        resolved_points.len() >= 2 && !validate_elbow_points_impl(&resolved_points, None);

    if is_out_of_bounds || has_invalid_path {
        return recompute_elbow_fallback_patch(&input);
    }

    patch
}

pub fn move_fixed_segment_to_point(
    arrow: &ArrowState,
    segment_index: usize,
    pointer: Point,
) -> MoveFixedSegmentToPointResult {
    if segment_index == 0 || segment_index >= arrow.points.len() {
        return MoveFixedSegmentToPointResult {
            patch: ArrowPatch::default(),
            active_segment_index: None,
            active_segment_mid_point: None,
        };
    }

    let is_horizontal = is_horizontal_heading(vector_to_heading(
        arrow.points[segment_index - 1],
        arrow.points[segment_index],
    ));
    let local_pointer = [pointer[0] - arrow.x, pointer[1] - arrow.y];

    let mut segments_by_index = BTreeMap::new();
    for segment in arrow.fixed_segments.clone().unwrap_or_default() {
        segments_by_index.insert(segment.index, segment);
    }

    segments_by_index.insert(
        segment_index,
        FixedSegment {
            index: segment_index,
            start: [
                if is_horizontal {
                    arrow.points[segment_index - 1][0]
                } else {
                    local_pointer[0]
                },
                if is_horizontal {
                    local_pointer[1]
                } else {
                    arrow.points[segment_index - 1][1]
                },
            ],
            end: [
                if is_horizontal {
                    arrow.points[segment_index][0]
                } else {
                    local_pointer[0]
                },
                if is_horizontal {
                    local_pointer[1]
                } else {
                    arrow.points[segment_index][1]
                },
            ],
        },
    );

    let fixed_segments = segments_by_index.into_values().collect::<Vec<_>>();
    let active_segment = fixed_segments
        .iter()
        .find(|segment| segment.index == segment_index)
        .cloned();

    MoveFixedSegmentToPointResult {
        patch: ArrowPatch {
            fixed_segments: Some(if fixed_segments.is_empty() {
                None
            } else {
                Some(fixed_segments.clone())
            }),
            ..ArrowPatch::default()
        },
        active_segment_index: active_segment.as_ref().map(|segment| segment.index),
        active_segment_mid_point: active_segment.map(|segment| {
            [
                arrow.x + (segment.start[0] + segment.end[0]) / 2.0,
                arrow.y + (segment.start[1] + segment.end[1]) / 2.0,
            ]
        }),
    }
}

pub fn validate_elbow_points(points: &[Point], tolerance: Option<f64>) -> bool {
    validate_elbow_points_impl(points, tolerance)
}

pub fn validate_elbow_invariant(arrow: &ArrowState) -> Vec<String> {
    let mut issues = Vec::new();
    if !arrow.elbowed {
        return issues;
    }
    if arrow.points.len() < 2 {
        issues.push("elbow arrow must contain at least two points".to_owned());
    }
    if !is_orthogonal_path(&arrow.points, DEDUP_THRESHOLD) {
        issues.push("elbow arrow path must be orthogonal".to_owned());
    }
    if let Some(fixed_segments) = arrow.fixed_segments.as_ref() {
        for fixed in fixed_segments {
            if fixed.index == 0 || fixed.index >= arrow.points.len() {
                issues.push(format!(
                    "fixed segment index {} is outside points range",
                    fixed.index
                ));
            }
        }
    }
    issues
}

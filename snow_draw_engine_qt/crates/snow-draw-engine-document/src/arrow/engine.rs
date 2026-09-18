use std::collections::BTreeMap;

use crate::arrow_binding_core::{
    calculate_fixed_point_for_binding, calculate_fixed_point_for_elbow_binding, update_bound_point,
};
use crate::arrow_focus_core::compute_focus_point_drag;
use crate::arrow_geom::points_equal;
use crate::arrow_state_core::apply_arrow_patch_internal;
use crate::{
    ArrowEndpointEdge, ArrowEngineEvent, ArrowPatch, ArrowState, BindablePatch, BindableState,
    ComputeEndpointDragInput, ComputeFocusPointDragInput, ElbowUpdatePatch, ElementId,
    EngineResult, FixedPointBinding, Point, RecomputeAfterBindableChangeInput, RecomputeElbowInput,
    UpdateElbowArrowInput, UpdateElbowArrowOptions, ValidationReport,
};
use snow_draw_engine_core::arrow::BindMode;

fn merge_arrow_patches(primary: ArrowPatch, secondary: ArrowPatch) -> ArrowPatch {
    ArrowPatch {
        x: secondary.x.or(primary.x),
        y: secondary.y.or(primary.y),
        width: secondary.width.or(primary.width),
        height: secondary.height.or(primary.height),
        points: secondary.points.or(primary.points),
        start_binding: secondary.start_binding.or(primary.start_binding),
        end_binding: secondary.end_binding.or(primary.end_binding),
        fixed_segments: secondary.fixed_segments.or(primary.fixed_segments),
        start_is_special: secondary.start_is_special.or(primary.start_is_special),
        end_is_special: secondary.end_is_special.or(primary.end_is_special),
    }
}

fn bindables_by_id(bindables: &[BindableState]) -> BTreeMap<ElementId, BindableState> {
    bindables
        .iter()
        .cloned()
        .map(|bindable| (bindable.id, bindable))
        .collect()
}

fn recompute_endpoint_from_binding(
    arrow: &ArrowState,
    edge: ArrowEndpointEdge,
    binding: &FixedPointBinding,
    bindables_by_id: &BTreeMap<ElementId, BindableState>,
) -> Option<Point> {
    let bindable = bindables_by_id.get(&binding.element_id)?;
    let selector = match edge {
        ArrowEndpointEdge::Start => crate::ArrowEndpointSelector::StartBinding,
        ArrowEndpointEdge::End => crate::ArrowEndpointSelector::EndBinding,
    };
    update_bound_point(
        arrow,
        selector,
        Some(binding),
        bindable,
        bindables_by_id,
        false,
    )
}

fn recompute_elbow_patch_internal(input: &RecomputeElbowInput) -> ArrowPatch {
    crate::arrow_elbow_core::recompute_elbow_patch(input.clone())
}

fn has_elbow_updates(updates: &ElbowUpdatePatch) -> bool {
    updates.points.is_some()
        || updates.fixed_segments.is_some()
        || updates.start_binding.is_some()
        || updates.end_binding.is_some()
}

fn update_elbow_arrow_patch_internal(input: &UpdateElbowArrowInput) -> ArrowPatch {
    if !has_elbow_updates(&input.updates) {
        return ArrowPatch::default();
    }

    crate::arrow_elbow_core::update_elbow_arrow_patch(input.clone())
}

fn validate_elbow_points(points: &[Point], tolerance: f64) -> bool {
    crate::arrow_elbow_core::validate_elbow_points(points, Some(tolerance))
}

fn validate_elbow_invariant(arrow: &ArrowState) -> Vec<String> {
    crate::arrow_elbow_core::validate_elbow_invariant(arrow)
}

pub fn compute_endpoint_drag(input: &ComputeEndpointDragInput) -> EngineResult {
    let base = crate::arrow_binding_core::compute_simple_binding_patch(input);
    if !input.arrow.elbowed {
        return base;
    }
    compute_elbow_endpoint_drag(input, base)
}

fn dragged_endpoint_edge(input: &ComputeEndpointDragInput) -> Option<ArrowEndpointEdge> {
    let end_index = input.arrow.points.len().saturating_sub(1);
    let mut start = false;
    let mut end = false;
    for update in &input.dragged_points {
        start |= update.index == 0;
        end |= update.index == end_index;
    }
    match (start, end) {
        (true, false) => Some(ArrowEndpointEdge::Start),
        (false, true) => Some(ArrowEndpointEdge::End),
        _ => None,
    }
}

/// Route options for a live elbow endpoint drag: the drag is a preview unless
/// finalizing, and midpoint snapping is gated off by angle-locked drags
/// (Excalidraw's `isMidpointSnappingEnabled`).
fn elbow_drag_options(input: &ComputeEndpointDragInput) -> UpdateElbowArrowOptions {
    let options = input.options.as_ref();
    UpdateElbowArrowOptions {
        is_dragging: Some(
            !options
                .and_then(|options| options.finalize)
                .unwrap_or(false),
        ),
        validate_invariants: None,
        midpoint_snapping_enabled: Some(
            !options
                .and_then(|options| options.angle_locked)
                .unwrap_or(false),
        ),
    }
}

/// Live elbow endpoint drag: bind from the shared decision patch, then route
/// with the pointer as the moving tip and re-anchor on the routed outline.
/// `recompute_elbow` is for bindable motion, not pointer-following preview.
fn compute_elbow_endpoint_drag(
    input: &ComputeEndpointDragInput,
    base: EngineResult,
) -> EngineResult {
    let Some(edge) = dragged_endpoint_edge(input) else {
        let next_arrow = apply_arrow_patch_internal(&input.arrow, &base.arrow_patch);
        let elbow_patch = recompute_elbow_patch_internal(&RecomputeElbowInput {
            arrow: next_arrow,
            bindables: input.bindables.clone(),
            context: input.context,
        });
        return EngineResult {
            arrow_patch: merge_arrow_patches(base.arrow_patch, elbow_patch),
            bindable_patches: base.bindable_patches,
            suggested_binding: base.suggested_binding,
            events: base.events,
        };
    };

    let route_options = elbow_drag_options(input);
    let midpoint_snapping_enabled = route_options.midpoint_snapping_enabled.unwrap_or(true);
    let next_binding = match edge {
        ArrowEndpointEdge::Start => base
            .arrow_patch
            .start_binding
            .unwrap_or(input.arrow.start_binding),
        ArrowEndpointEdge::End => base
            .arrow_patch
            .end_binding
            .unwrap_or(input.arrow.end_binding),
    };
    let hovered = next_binding.and_then(|binding| {
        input
            .bindables
            .iter()
            .find(|bindable| bindable.id == binding.element_id)
            .cloned()
    });

    let mut working = input.arrow.clone();
    let tentative = hovered.as_ref().map(|bindable| FixedPointBinding {
        element_id: bindable.id,
        fixed_point: calculate_fixed_point_for_binding(bindable, input.pointer),
        mode: BindMode::Orbit,
    });
    match edge {
        ArrowEndpointEdge::Start => working.start_binding = tentative,
        ArrowEndpointEdge::End => working.end_binding = tentative,
    }

    let pointer_local = [input.pointer[0] - working.x, input.pointer[1] - working.y];
    let start_local = match edge {
        ArrowEndpointEdge::Start => pointer_local,
        ArrowEndpointEdge::End => working.points.first().copied().unwrap_or([0.0, 0.0]),
    };
    let end_local = match edge {
        ArrowEndpointEdge::Start => working.points.last().copied().unwrap_or([0.0, 0.0]),
        ArrowEndpointEdge::End => pointer_local,
    };
    let route_patch = update_elbow_arrow_patch_internal(&UpdateElbowArrowInput {
        arrow: working.clone(),
        updates: ElbowUpdatePatch {
            points: Some(vec![start_local, end_local]),
            ..ElbowUpdatePatch::default()
        },
        bindables: input.bindables.clone(),
        context: input.context,
        options: Some(route_options.clone()),
    });
    working = apply_arrow_patch_internal(&working, &route_patch);

    let binding = hovered.as_ref().map(|bindable| FixedPointBinding {
        element_id: bindable.id,
        fixed_point: calculate_fixed_point_for_elbow_binding(
            &working,
            bindable,
            edge,
            midpoint_snapping_enabled,
        ),
        mode: BindMode::Orbit,
    });
    let updates = match edge {
        ArrowEndpointEdge::Start => ElbowUpdatePatch {
            start_binding: Some(binding),
            ..ElbowUpdatePatch::default()
        },
        ArrowEndpointEdge::End => ElbowUpdatePatch {
            end_binding: Some(binding),
            ..ElbowUpdatePatch::default()
        },
    };
    let binding_patch = update_elbow_arrow_patch_internal(&UpdateElbowArrowInput {
        arrow: working,
        updates,
        bindables: input.bindables.clone(),
        context: input.context,
        options: Some(route_options),
    });

    EngineResult {
        arrow_patch: merge_arrow_patches(route_patch, binding_patch),
        bindable_patches: base.bindable_patches,
        suggested_binding: base.suggested_binding,
        events: base.events,
    }
}

pub fn finalize_endpoint_drag(input: &ComputeEndpointDragInput) -> EngineResult {
    let mut next_input = input.clone();
    let mut options = next_input.options.unwrap_or_default();
    options.finalize = Some(true);
    next_input.options = Some(options);
    compute_endpoint_drag(&next_input)
}

pub fn recompute_after_bindable_change(input: &RecomputeAfterBindableChangeInput) -> EngineResult {
    let bindables_by_id = bindables_by_id(&input.bindables);
    let mut start_binding = input.arrow.start_binding;
    let mut end_binding = input.arrow.end_binding;
    let mut bindable_patches = Vec::new();
    let mut events = Vec::new();

    if let Some(binding) = start_binding.as_ref()
        && !bindables_by_id.contains_key(&binding.element_id)
    {
        bindable_patches.push(BindablePatch {
            id: binding.element_id,
            add_bound_arrow_id: None,
            remove_bound_arrow_id: Some(input.arrow.id),
        });
        events.push(ArrowEngineEvent::BindingBroken {
            arrow_id: input.arrow.id,
            edge: crate::ArrowEndpointEdge::Start,
        });
        start_binding = None;
    }
    if let Some(binding) = end_binding.as_ref()
        && !bindables_by_id.contains_key(&binding.element_id)
    {
        bindable_patches.push(BindablePatch {
            id: binding.element_id,
            add_bound_arrow_id: None,
            remove_bound_arrow_id: Some(input.arrow.id),
        });
        events.push(ArrowEngineEvent::BindingBroken {
            arrow_id: input.arrow.id,
            edge: crate::ArrowEndpointEdge::End,
        });
        end_binding = None;
    }

    let mut next_points = input.arrow.points.clone();
    let should_update_start = start_binding.as_ref().is_some_and(|binding| {
        input
            .changed_bindable_ids
            .as_ref()
            .is_none_or(|ids| ids.is_empty() || ids.iter().any(|id| id == &binding.element_id))
    });
    let should_update_end = end_binding.as_ref().is_some_and(|binding| {
        input
            .changed_bindable_ids
            .as_ref()
            .is_none_or(|ids| ids.is_empty() || ids.iter().any(|id| id == &binding.element_id))
    });

    let simulated_arrow = ArrowState {
        points: next_points.clone(),
        start_binding,
        end_binding,
        ..input.arrow.clone()
    };

    if should_update_start
        && let Some(binding) = start_binding.as_ref()
        && let Some(updated) = recompute_endpoint_from_binding(
            &simulated_arrow,
            crate::ArrowEndpointEdge::Start,
            binding,
            &bindables_by_id,
        )
    {
        next_points[0] = updated;
    }

    let simulated_arrow = ArrowState {
        points: next_points.clone(),
        start_binding,
        end_binding,
        ..input.arrow.clone()
    };

    if should_update_end
        && let Some(binding) = end_binding.as_ref()
        && let Some(updated) = recompute_endpoint_from_binding(
            &simulated_arrow,
            crate::ArrowEndpointEdge::End,
            binding,
            &bindables_by_id,
        )
    {
        let end_index = next_points.len().saturating_sub(1);
        next_points[end_index] = updated;
    }

    let origin = next_points.first().copied().unwrap_or([0.0, 0.0]);
    let move_mid_points_with_element = input
        .options
        .as_ref()
        .and_then(|options| options.move_mid_points_with_element)
        .unwrap_or(false);
    let last_index = next_points.len().saturating_sub(1);
    let normalized_points = next_points
        .iter()
        .enumerate()
        .map(|(index, point)| {
            if move_mid_points_with_element && index != 0 && index != last_index {
                *point
            } else {
                [point[0] - origin[0], point[1] - origin[1]]
            }
        })
        .collect::<Vec<_>>();
    let (mut min_x, mut max_x): (f64, f64) = (0.0, 0.0);
    let (mut min_y, mut max_y): (f64, f64) = (0.0, 0.0);
    for point in &normalized_points {
        min_x = min_x.min(point[0]);
        max_x = max_x.max(point[0]);
        min_y = min_y.min(point[1]);
        max_y = max_y.max(point[1]);
    }

    let base_patch = ArrowPatch {
        x: Some(input.arrow.x + origin[0]),
        y: Some(input.arrow.y + origin[1]),
        width: Some(max_x - min_x),
        height: Some(max_y - min_y),
        points: Some(normalized_points),
        start_binding: Some(start_binding),
        end_binding: Some(end_binding),
        fixed_segments: None,
        start_is_special: None,
        end_is_special: None,
    };

    let arrow_with_base = apply_arrow_patch_internal(&input.arrow, &base_patch);
    if !arrow_with_base.elbowed {
        return EngineResult {
            arrow_patch: base_patch,
            bindable_patches,
            suggested_binding: None,
            events,
        };
    }

    let elbow_patch = if input
        .arrow
        .fixed_segments
        .as_ref()
        .is_some_and(|segments| !segments.is_empty())
    {
        let end_index = next_points.len().saturating_sub(1);
        update_elbow_arrow_patch_internal(&UpdateElbowArrowInput {
            arrow: input.arrow.clone(),
            updates: ElbowUpdatePatch {
                points: Some(vec![next_points[0], next_points[end_index]]),
                start_binding: Some(start_binding),
                end_binding: Some(end_binding),
                ..ElbowUpdatePatch::default()
            },
            bindables: input.bindables.clone(),
            context: input.context,
            options: Some(UpdateElbowArrowOptions {
                is_dragging: Some(false),
                validate_invariants: None,
                midpoint_snapping_enabled: None,
            }),
        })
    } else {
        recompute_elbow_patch_internal(&RecomputeElbowInput {
            arrow: arrow_with_base,
            bindables: input.bindables.clone(),
            context: input.context,
        })
    };

    EngineResult {
        arrow_patch: merge_arrow_patches(base_patch, elbow_patch),
        bindable_patches,
        suggested_binding: None,
        events,
    }
}

pub fn compute_focus_drag(input: &ComputeFocusPointDragInput) -> EngineResult {
    compute_focus_point_drag(input)
}

pub fn validate_arrow_invariant(arrow: &ArrowState) -> ValidationReport {
    let mut violations = Vec::new();
    if arrow.points.len() < 2 {
        violations.push("arrow must contain at least two points".to_owned());
    }

    if let Some(first_point) = arrow.points.first().copied()
        && !points_equal(first_point, [0.0, 0.0], 1e-6)
    {
        violations.push("arrow points must be normalized with [0,0] as first point".to_owned());
    }

    if arrow.elbowed {
        if !validate_elbow_points(&arrow.points, 1.0) {
            violations.push("elbow arrow must keep orthogonal segments".to_owned());
        }
        violations.extend(validate_elbow_invariant(arrow));
    }

    ValidationReport {
        valid: violations.is_empty(),
        violations,
    }
}

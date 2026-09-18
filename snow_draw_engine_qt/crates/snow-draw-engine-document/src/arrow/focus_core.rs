use std::collections::BTreeMap;

use crate::arrow_binding_geometry::{OutlineMidPointMode, outline_mid_point_suggestion};
use crate::arrow_geom::{
    center, clamp, distance, get_global_fixed_point, get_point_at_index_global,
    normalize_arrow_from_global_points, normalize_fixed_point, to_global_point, to_local_point,
    unrotate_point,
};
use crate::arrow_hit_test::{
    distance_to_bindable_outline, get_hovered_bindable, is_bindable_visible_at_point,
    is_point_in_bindable,
};
use crate::{
    ArrowEndpointEdge, ArrowEndpointSelector, ArrowEngineEvent, ArrowPatch, ArrowState, BindMode,
    BindablePatch, BindableState, ComputeFocusPointDragInput, ElementId, EngineResult,
    FixedPointBinding, FocusPointContext, FocusPointDescriptor, ListVisibleFocusPointsInput, Point,
    SuggestedBinding,
};

const BASE_BINDING_GAP: f64 = 5.0;

pub const FOCUS_POINT_SIZE: f64 = 10.0 / 1.5;

fn focus_hit_threshold(zoom: f64) -> f64 {
    (FOCUS_POINT_SIZE * 1.5) / zoom.max(1e-6)
}

fn get_binding_gap(bindable: &BindableState, _elbowed: bool) -> f64 {
    BASE_BINDING_GAP + bindable.stroke_width / 2.0
}

fn max_binding_distance(zoom: f64) -> f64 {
    let base_distance = BASE_BINDING_GAP.max(15.0);
    let safe_zoom = if zoom < 1.0 { zoom } else { 1.0 };
    clamp(
        base_distance / (safe_zoom * 1.5),
        base_distance,
        base_distance * 2.0,
    )
}

fn calculate_fixed_point_for_binding(bindable: &BindableState, global_point: Point) -> Point {
    let bindable_center = center(bindable.x, bindable.y, bindable.width, bindable.height);
    let local = unrotate_point(global_point, bindable_center, bindable.angle);
    let fixed_x = (local[0] - bindable.x) / bindable.width.max(1e-6);
    let fixed_y = (local[1] - bindable.y) / bindable.height.max(1e-6);
    normalize_fixed_point([fixed_x, fixed_y])
}

fn pick_hovered_bindable(
    point: Point,
    bindables: &[BindableState],
    zoom: f64,
) -> Option<BindableState> {
    get_hovered_bindable(point, bindables, max_binding_distance(zoom))
}

fn edge_index(arrow: &ArrowState, edge: ArrowEndpointEdge) -> usize {
    match edge {
        ArrowEndpointEdge::Start => 0,
        ArrowEndpointEdge::End => arrow.points.len().saturating_sub(1),
    }
}

fn binding_for_edge(arrow: &ArrowState, edge: ArrowEndpointEdge) -> Option<&FixedPointBinding> {
    match edge {
        ArrowEndpointEdge::Start => arrow.start_binding.as_ref(),
        ArrowEndpointEdge::End => arrow.end_binding.as_ref(),
    }
}

fn with_edge_binding(
    start_binding: Option<FixedPointBinding>,
    end_binding: Option<FixedPointBinding>,
    edge: ArrowEndpointEdge,
    binding: Option<FixedPointBinding>,
) -> (Option<FixedPointBinding>, Option<FixedPointBinding>) {
    match edge {
        ArrowEndpointEdge::Start => (binding, end_binding),
        ArrowEndpointEdge::End => (start_binding, binding),
    }
}

fn bindables_by_id(bindables: &[BindableState]) -> BTreeMap<ElementId, BindableState> {
    bindables
        .iter()
        .cloned()
        .map(|bindable| (bindable.id, bindable))
        .collect()
}

pub(crate) fn resolve_bound_point_local(
    arrow: &ArrowState,
    edge: ArrowEndpointEdge,
    binding: &FixedPointBinding,
    bindable: &BindableState,
    bindables_by_id: &BTreeMap<ElementId, BindableState>,
    dragging: bool,
) -> Point {
    let selector = match edge {
        ArrowEndpointEdge::Start => ArrowEndpointSelector::StartBinding,
        ArrowEndpointEdge::End => ArrowEndpointSelector::EndBinding,
    };
    crate::arrow_binding_core::update_bound_point(
        arrow,
        selector,
        Some(binding),
        bindable,
        bindables_by_id,
        dragging,
    )
    .unwrap_or_else(|| {
        let focus_point = get_global_fixed_point(binding, bindable);
        to_local_point(arrow, focus_point)
    })
}

pub fn is_focus_point_visible(
    arrow: &ArrowState,
    edge: ArrowEndpointEdge,
    binding: &FixedPointBinding,
    bindable: &BindableState,
    context: FocusPointContext,
    ignore_overlap: bool,
) -> bool {
    if arrow.elbowed || !context.is_binding_enabled || arrow.points.len() != 2 {
        return false;
    }

    let focus_point = get_global_fixed_point(binding, bindable);
    if !is_bindable_visible_at_point(focus_point, bindable) {
        return false;
    }

    if !ignore_overlap {
        let associated_index = if arrow
            .start_binding
            .as_ref()
            .is_some_and(|start_binding| start_binding.element_id == bindable.id)
        {
            0
        } else {
            arrow.points.len().saturating_sub(1)
        };
        let associated_point =
            get_point_at_index_global(arrow, if associated_index == 0 { 0 } else { -1 });
        if distance(focus_point, associated_point) < focus_hit_threshold(context.zoom) {
            return false;
        }
    }

    let endpoint = get_point_at_index_global(
        arrow,
        if edge == ArrowEndpointEdge::Start {
            0
        } else {
            -1
        },
    );
    let inside_or_near_outline = is_point_in_bindable(focus_point, bindable)
        || distance_to_bindable_outline(focus_point, bindable)
            <= get_binding_gap(bindable, arrow.elbowed);

    distance(focus_point, endpoint) >= focus_hit_threshold(context.zoom) && inside_or_near_outline
}

pub fn list_visible_focus_points(input: &ListVisibleFocusPointsInput) -> Vec<FocusPointDescriptor> {
    let ignore_overlap = input
        .options
        .as_ref()
        .and_then(|options| options.ignore_overlap)
        .unwrap_or(false);
    let bindables_by_id = bindables_by_id(&input.bindables);
    let mut out = Vec::new();

    if let Some(start_binding) = input.arrow.start_binding.as_ref()
        && let Some(bindable) = bindables_by_id.get(&start_binding.element_id)
        && is_focus_point_visible(
            &input.arrow,
            ArrowEndpointEdge::Start,
            start_binding,
            bindable,
            input.context,
            ignore_overlap,
        )
    {
        out.push(FocusPointDescriptor {
            edge: ArrowEndpointEdge::Start,
            point: get_global_fixed_point(start_binding, bindable),
            binding: *start_binding,
        });
    }

    if let Some(end_binding) = input.arrow.end_binding.as_ref()
        && let Some(bindable) = bindables_by_id.get(&end_binding.element_id)
        && is_focus_point_visible(
            &input.arrow,
            ArrowEndpointEdge::End,
            end_binding,
            bindable,
            input.context,
            ignore_overlap,
        )
    {
        out.push(FocusPointDescriptor {
            edge: ArrowEndpointEdge::End,
            point: get_global_fixed_point(end_binding, bindable),
            binding: *end_binding,
        });
    }

    out
}

fn collect_binding_transition(
    arrow_id: ElementId,
    edge: ArrowEndpointEdge,
    previous_binding: Option<&FixedPointBinding>,
    next_binding: Option<&FixedPointBinding>,
    bindable_patches: &mut Vec<BindablePatch>,
    events: &mut Vec<ArrowEngineEvent>,
    reorder_targets: &mut Vec<ElementId>,
) {
    if let Some(previous_binding) = previous_binding
        && next_binding
            .is_none_or(|next_binding| next_binding.element_id != previous_binding.element_id)
    {
        bindable_patches.push(BindablePatch {
            id: previous_binding.element_id,
            add_bound_arrow_id: None,
            remove_bound_arrow_id: Some(arrow_id),
        });
        if next_binding.is_none() {
            events.push(ArrowEngineEvent::BindingBroken { arrow_id, edge });
        }
    }

    if let Some(next_binding) = next_binding
        && previous_binding
            .is_none_or(|previous_binding| previous_binding.element_id != next_binding.element_id)
    {
        bindable_patches.push(BindablePatch {
            id: next_binding.element_id,
            add_bound_arrow_id: Some(arrow_id),
            remove_bound_arrow_id: None,
        });
        if !reorder_targets
            .iter()
            .any(|id| id == &next_binding.element_id)
        {
            reorder_targets.push(next_binding.element_id);
            events.push(ArrowEngineEvent::ReorderArrow {
                arrow_id,
                bindable_id: next_binding.element_id,
            });
        }
    }
}

fn compute_patch_from_local_points(
    arrow: &ArrowState,
    points: &[Point],
    max_coordinate: f64,
) -> ArrowPatch {
    let global_points = points
        .iter()
        .copied()
        .map(|point| to_global_point(arrow, point))
        .collect::<Vec<_>>();
    let normalized = normalize_arrow_from_global_points(&global_points, max_coordinate);
    ArrowPatch {
        x: Some(normalized.x),
        y: Some(normalized.y),
        width: Some(normalized.width),
        height: Some(normalized.height),
        points: Some(normalized.points),
        start_binding: None,
        end_binding: None,
        fixed_segments: None,
        start_is_special: None,
        end_is_special: None,
    }
}

fn snap_point_to_grid(point: Point, grid_size: Option<f64>) -> Point {
    let Some(grid_size) = grid_size else {
        return point;
    };
    if !grid_size.is_finite() || grid_size <= 0.0 {
        return point;
    }

    [
        (point[0] / grid_size).round() * grid_size,
        (point[1] / grid_size).round() * grid_size,
    ]
}

fn resolve_suggested_binding(
    hovered: Option<&BindableState>,
    pointer: Point,
    zoom: f64,
) -> Option<SuggestedBinding> {
    hovered.cloned().map(|element| {
        // Focus-point drags only apply to straight arrows; snapping is on
        // (focus drags have no angle-lock mode).
        let mid_points = outline_mid_point_suggestion(
            pointer,
            &element,
            zoom,
            OutlineMidPointMode::for_arrow(false, true),
        );
        SuggestedBinding {
            bindable_id: Some(element.id),
            mid_point: mid_points.highlighted,
            near_mid_points: mid_points.near,
            element,
        }
    })
}

pub fn compute_focus_point_drag(input: &ComputeFocusPointDragInput) -> EngineResult {
    let arrow = &input.arrow;
    if arrow.elbowed || arrow.points.len() < 2 {
        return EngineResult {
            arrow_patch: ArrowPatch::default(),
            bindable_patches: Vec::new(),
            suggested_binding: None,
            events: Vec::new(),
        };
    }

    let switch_to_inside_binding = input
        .options
        .as_ref()
        .and_then(|options| options.switch_to_inside_binding)
        .unwrap_or(false);
    let dragged_edge = input.dragged_edge;
    let other_edge = if dragged_edge == ArrowEndpointEdge::Start {
        ArrowEndpointEdge::End
    } else {
        ArrowEndpointEdge::Start
    };
    let dragged_index = edge_index(arrow, dragged_edge);
    let other_index = edge_index(arrow, other_edge);

    let bindables_by_id = bindables_by_id(&input.bindables);
    let mut start_binding = arrow.start_binding;
    let mut end_binding = arrow.end_binding;
    let mut next_points = arrow.points.clone();

    let hovered = if input.context.is_binding_enabled && !input.bindables.is_empty() {
        pick_hovered_bindable(input.pointer, &input.bindables, input.context.zoom)
    } else {
        None
    };

    let current_dragged_binding = binding_for_edge(arrow, dragged_edge).cloned();
    if let Some(hovered_bindable) = hovered.as_ref() {
        let mut mode = current_dragged_binding
            .as_ref()
            .map(|binding| binding.mode)
            .unwrap_or(BindMode::Orbit);
        if switch_to_inside_binding && mode == BindMode::Orbit {
            mode = BindMode::Inside;
        } else if !switch_to_inside_binding && mode == BindMode::Inside {
            mode = BindMode::Orbit;
        }

        let next_binding = FixedPointBinding {
            element_id: hovered_bindable.id,
            mode,
            fixed_point: calculate_fixed_point_for_binding(hovered_bindable, input.pointer),
        };
        (start_binding, end_binding) =
            with_edge_binding(start_binding, end_binding, dragged_edge, Some(next_binding));
    } else {
        (start_binding, end_binding) =
            with_edge_binding(start_binding, end_binding, dragged_edge, None);
        next_points[dragged_index] = to_local_point(
            arrow,
            snap_point_to_grid(
                input.pointer,
                input.options.as_ref().and_then(|options| options.grid_size),
            ),
        );
    }

    let mut dragged_binding = match dragged_edge {
        ArrowEndpointEdge::Start => start_binding,
        ArrowEndpointEdge::End => end_binding,
    };
    let dragged_bindable = dragged_binding
        .as_ref()
        .and_then(|binding| bindables_by_id.get(&binding.element_id))
        .cloned();

    if let (Some(binding), Some(bindable)) = (dragged_binding.as_mut(), dragged_bindable.as_ref()) {
        let opposite_binding = match other_edge {
            ArrowEndpointEdge::Start => start_binding.as_ref(),
            ArrowEndpointEdge::End => end_binding.as_ref(),
        };
        let bound_to_same_element = opposite_binding
            .is_some_and(|opposite_binding| opposite_binding.element_id == binding.element_id);
        binding.mode = if switch_to_inside_binding || bound_to_same_element {
            BindMode::Inside
        } else {
            BindMode::Orbit
        };
        next_points[dragged_index] = resolve_bound_point_local(
            &ArrowState {
                points: next_points.clone(),
                start_binding,
                end_binding,
                ..arrow.clone()
            },
            dragged_edge,
            binding,
            bindable,
            &bindables_by_id,
            true,
        );
        (start_binding, end_binding) =
            with_edge_binding(start_binding, end_binding, dragged_edge, Some(*binding));
    }

    let other_binding = match other_edge {
        ArrowEndpointEdge::Start => start_binding,
        ArrowEndpointEdge::End => end_binding,
    };
    if let Some(mut other_binding) = other_binding
        && other_binding.mode == BindMode::Orbit
        && input.context.is_binding_enabled
        && let Some(other_bindable) = bindables_by_id.get(&other_binding.element_id)
    {
        let bound_to_same_after_update = dragged_bindable
            .as_ref()
            .is_some_and(|dragged_bindable| dragged_bindable.id == other_binding.element_id);
        other_binding.mode = if switch_to_inside_binding || bound_to_same_after_update {
            BindMode::Inside
        } else {
            BindMode::Orbit
        };
        next_points[other_index] = resolve_bound_point_local(
            &ArrowState {
                points: next_points.clone(),
                start_binding,
                end_binding,
                ..arrow.clone()
            },
            other_edge,
            &other_binding,
            other_bindable,
            &bindables_by_id,
            false,
        );
        (start_binding, end_binding) =
            with_edge_binding(start_binding, end_binding, other_edge, Some(other_binding));
    }

    let mut arrow_patch =
        compute_patch_from_local_points(arrow, &next_points, input.context.max_coordinate);
    arrow_patch.start_binding = Some(start_binding);
    arrow_patch.end_binding = Some(end_binding);

    let mut bindable_patches = Vec::new();
    let mut events = Vec::new();
    let mut reorder_targets = Vec::new();
    collect_binding_transition(
        arrow.id,
        ArrowEndpointEdge::Start,
        arrow.start_binding.as_ref(),
        start_binding.as_ref(),
        &mut bindable_patches,
        &mut events,
        &mut reorder_targets,
    );
    collect_binding_transition(
        arrow.id,
        ArrowEndpointEdge::End,
        arrow.end_binding.as_ref(),
        end_binding.as_ref(),
        &mut bindable_patches,
        &mut events,
        &mut reorder_targets,
    );

    EngineResult {
        arrow_patch,
        bindable_patches,
        suggested_binding: resolve_suggested_binding(
            hovered.as_ref(),
            input.pointer,
            input.context.zoom,
        ),
        events,
    }
}

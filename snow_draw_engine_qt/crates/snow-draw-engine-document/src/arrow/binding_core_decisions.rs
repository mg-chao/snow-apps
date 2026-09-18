use super::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum EndpointBindingDisposition {
    Unchanged,
    Clear,
    Bind,
}

#[derive(Clone, Debug, PartialEq)]
pub(super) struct EndpointBindingDecision {
    pub(super) disposition: EndpointBindingDisposition,
    pub(super) strategy: EndpointBindingStrategy,
}

#[derive(Clone, Debug, PartialEq)]
pub(super) struct EndpointBindingDecisionPair {
    pub(super) start: EndpointBindingDecision,
    pub(super) end: EndpointBindingDecision,
}

fn unchanged_decision() -> EndpointBindingDecision {
    EndpointBindingDecision {
        disposition: EndpointBindingDisposition::Unchanged,
        strategy: EndpointBindingStrategy {
            mode: None,
            bindable_id: None,
            element: None,
            focus_point: None,
        },
    }
}

fn clear_decision() -> EndpointBindingDecision {
    EndpointBindingDecision {
        disposition: EndpointBindingDisposition::Clear,
        strategy: EndpointBindingStrategy {
            mode: None,
            bindable_id: None,
            element: None,
            focus_point: None,
        },
    }
}

fn bind_decision(
    mode: BindMode,
    bindable: &BindableState,
    focus_point: Point,
) -> EndpointBindingDecision {
    EndpointBindingDecision {
        disposition: EndpointBindingDisposition::Bind,
        strategy: EndpointBindingStrategy {
            mode: Some(mode),
            bindable_id: Some(bindable.id),
            element: Some(bindable.clone()),
            focus_point: Some(focus_point),
        },
    }
}

pub(super) fn to_binding(
    arrow: &ArrowState,
    edge: ArrowEndpointEdge,
    element: &BindableState,
    mode: BindMode,
    focus_point: Point,
    midpoint_snapping_enabled: bool,
) -> FixedPointBinding {
    FixedPointBinding {
        element_id: element.id,
        mode: if arrow.elbowed { BindMode::Orbit } else { mode },
        fixed_point: if arrow.elbowed {
            calculate_fixed_point_for_elbow_binding(arrow, element, edge, midpoint_snapping_enabled)
        } else {
            calculate_fixed_point_for_binding(element, focus_point)
        },
    }
}

fn pick_drag_edge_decisions(
    start_dragged: bool,
    decision: EndpointBindingDecision,
    other: EndpointBindingDecision,
) -> EndpointBindingDecisionPair {
    if start_dragged {
        EndpointBindingDecisionPair {
            start: decision,
            end: other,
        }
    } else {
        EndpointBindingDecisionPair {
            start: other,
            end: decision,
        }
    }
}

fn strategy_for_elbow_endpoint(input: &ComputeEndpointDragInput) -> EndpointBindingDecisionPair {
    let dragged = normalize_dragged_points(&input.arrow, &input.dragged_points);
    let Some((&dragged_index, _)) = dragged.iter().next() else {
        return EndpointBindingDecisionPair {
            start: unchanged_decision(),
            end: unchanged_decision(),
        };
    };

    let hit = get_hovered_bindable(
        input.pointer,
        &input.bindables,
        max_binding_distance(input.context.zoom),
    );
    let focus_point = get_point_at_index_global(&input.arrow, dragged_index as isize);
    let decision = if let Some(hit) = hit {
        bind_decision(BindMode::Orbit, &hit, focus_point)
    } else {
        clear_decision()
    };

    if dragged_index == 0 {
        EndpointBindingDecisionPair {
            start: decision,
            end: unchanged_decision(),
        }
    } else {
        EndpointBindingDecisionPair {
            start: unchanged_decision(),
            end: decision,
        }
    }
}

pub(super) fn normalize_dragged_points(
    arrow: &ArrowState,
    updates: &crate::PointUpdates,
) -> BTreeMap<usize, Point> {
    let mut pairs = point_updates_to_pairs(updates);
    pairs.retain(|(index, _)| *index < arrow.points.len());
    pairs.into_iter().collect()
}

pub(super) fn get_endpoint_binding_decisions(
    input: &ComputeEndpointDragInput,
) -> EndpointBindingDecisionPair {
    let arrow = &input.arrow;
    let bindables = &input.bindables;
    let context = &input.context;
    let options = input.options.as_ref();
    let complex_bindings = options
        .and_then(|options| options.complex_bindings)
        .unwrap_or(false);
    let dragged_points = normalize_dragged_points(arrow, &input.dragged_points);
    let start_index = 0usize;
    let end_index = arrow.points.len().saturating_sub(1);
    let start_dragged = dragged_points.contains_key(&start_index);
    let end_dragged = dragged_points.contains_key(&end_index);
    let bindables_by_id = bindables
        .iter()
        .map(|bindable| (bindable.id, bindable.clone()))
        .collect::<BTreeMap<_, _>>();

    if !start_dragged && !end_dragged {
        return EndpointBindingDecisionPair {
            start: unchanged_decision(),
            end: unchanged_decision(),
        };
    }
    if start_dragged && end_dragged {
        return EndpointBindingDecisionPair {
            start: clear_decision(),
            end: clear_decision(),
        };
    }
    if !context.is_binding_enabled {
        return pick_drag_edge_decisions(start_dragged, clear_decision(), unchanged_decision());
    }
    if arrow.elbowed {
        return strategy_for_elbow_endpoint(input);
    }

    let dragged_index = if start_dragged {
        start_index
    } else {
        end_index
    };
    let Some(&dragged_point) = dragged_points.get(&dragged_index) else {
        return EndpointBindingDecisionPair {
            start: unchanged_decision(),
            end: unchanged_decision(),
        };
    };

    let global_point = to_global_point(arrow, dragged_point);
    // While angle-locked the dragged point slides along the locked ray, so
    // hit-test the raw pointer instead (Excalidraw uses the scene pointer
    // there as well).
    let angle_locked = options
        .and_then(|options| options.angle_locked)
        .unwrap_or(false);
    let hit_target = if angle_locked {
        input.pointer
    } else {
        global_point
    };
    let hit = get_hovered_bindable(hit_target, bindables, max_binding_distance(context.zoom));
    let other_binding = if start_dragged {
        arrow.end_binding.as_ref()
    } else {
        arrow.start_binding.as_ref()
    };
    let overlapping =
        get_bindables_over_point(global_point, bindables, max_binding_distance(context.zoom));
    let other_bindable = other_binding.and_then(|binding| bindables_by_id.get(&binding.element_id));
    let is_overlapping_other = other_bindable.is_some_and(|other_bindable| {
        overlapping
            .iter()
            .any(|bindable| bindable.id == other_bindable.id)
    });
    let is_nested = hit.as_ref().is_some_and(|hit| {
        other_bindable
            .filter(|other_bindable| other_bindable.id != hit.id)
            .is_some_and(|other_bindable| is_bindable_inside_other_bindable(other_bindable, hit))
    });
    let bind_mode_forces_inside = matches!(context.bind_mode, BindMode::Inside | BindMode::Skip);
    let alt_forces_inside = options.and_then(|options| options.alt_key).unwrap_or(false);
    let point_in_hit = hit
        .as_ref()
        .is_some_and(|hit| is_point_in_bindable(hit_target, hit));
    let other_focus_point = match (other_binding, other_bindable) {
        (Some(other_binding), Some(other_bindable)) => {
            Some(get_global_fixed_point(other_binding, other_bindable))
        }
        _ => None,
    };
    let other_focus_point_is_in_element =
        other_bindable
            .zip(other_focus_point)
            .is_some_and(|(other_bindable, other_focus_point)| {
                is_point_in_bindable(other_focus_point, other_bindable)
                    || distance_to_bindable_outline(other_focus_point, other_bindable) <= 1e-4
            });
    let point_is_close_to_other_element = other_bindable.is_some_and(|other_bindable| {
        is_point_in_bindable(global_point, other_bindable)
            || distance_to_bindable_outline(global_point, other_bindable)
                <= max_binding_distance(context.zoom)
    });
    // An opposite inside binding is never overridden (Excalidraw's
    // `arrowStartIsInside` / inside-mode guard).
    let other_never_override =
        other_binding.is_some_and(|binding| binding.mode == BindMode::Inside);
    let opposite_index = if start_dragged {
        end_index
    } else {
        start_index
    };
    let opposite_point = get_point_at_index_global(arrow, opposite_index as isize);

    let midpoint_snapping_enabled = !angle_locked;

    let angle_locked_other = if let Some(other_bindable) =
        other_bindable.filter(|_| !other_never_override && angle_locked)
    {
        let edge = if start_dragged {
            ArrowEndpointEdge::End
        } else {
            ArrowEndpointEdge::Start
        };
        let projected = project_fixed_point_onto_diagonal(
            arrow,
            opposite_point,
            other_bindable,
            edge,
            bindables,
            context.zoom,
            // The opposite end keeps midpoint snapping while angle-locked:
            // Excalidraw gates only the dragged end on `angleLocked`.
            true,
        )
        .unwrap_or(opposite_point);
        bind_decision(BindMode::Orbit, other_bindable, projected)
    } else {
        unchanged_decision()
    };

    let other_decision = if let Some(other_bindable) = other_bindable {
        if let Some(opposite_orbit_focus_point) =
            options.and_then(|options| options.opposite_orbit_focus_point)
        {
            if !other_never_override
                && !other_focus_point_is_in_element
                && !point_is_close_to_other_element
            {
                bind_decision(BindMode::Orbit, other_bindable, opposite_orbit_focus_point)
            } else {
                angle_locked_other
            }
        } else {
            angle_locked_other
        }
    } else {
        unchanged_decision()
    };

    if bind_mode_forces_inside {
        let bind_target = if hit.is_some() && is_overlapping_other {
            other_bindable
                .filter(|other_bindable| is_bindable_background_opaque(other_bindable))
                .cloned()
                .or(hit.clone())
        } else {
            hit.clone()
        };
        let forced = bind_target
            .as_ref()
            .map(|bind_target| bind_decision(BindMode::Inside, bind_target, global_point))
            .unwrap_or_else(clear_decision);
        let should_break_opposite_binding = options
            .and_then(|options| options.finalize)
            .unwrap_or(false)
            && hit.is_some()
            && other_binding.is_some()
            && other_binding.is_some_and(|binding| {
                hit.as_ref().is_some_and(|hit| binding.element_id == hit.id)
            })
            && arrow.points.len() == 2;
        let other = if should_break_opposite_binding {
            clear_decision()
        } else {
            unchanged_decision()
        };
        return pick_drag_edge_decisions(start_dragged, forced, other);
    }

    if alt_forces_inside {
        let forced = hit
            .as_ref()
            .map(|hit| bind_decision(BindMode::Inside, hit, global_point))
            .unwrap_or_else(clear_decision);
        return pick_drag_edge_decisions(start_dragged, forced, unchanged_decision());
    }

    if let (Some(other_binding), Some(hit)) = (other_binding, hit.as_ref())
        && other_binding.element_id == hit.id
    {
        if !complex_bindings {
            let current = bind_decision(BindMode::Inside, hit, global_point);
            let opposite = bind_decision(BindMode::Inside, hit, opposite_point);
            return pick_drag_edge_decisions(start_dragged, current, opposite);
        }

        if other_binding.mode == BindMode::Orbit {
            let edge = if start_dragged {
                ArrowEndpointEdge::Start
            } else {
                ArrowEndpointEdge::End
            };
            let projected_focus = project_fixed_point_onto_diagonal(
                arrow,
                global_point,
                hit,
                edge,
                bindables,
                context.zoom,
                midpoint_snapping_enabled,
            )
            .unwrap_or(global_point);
            let current = bind_decision(BindMode::Orbit, hit, projected_focus);
            let other = if options
                .and_then(|options| options.finalize)
                .unwrap_or(false)
            {
                if arrow.points.len() > 2 {
                    unchanged_decision()
                } else {
                    clear_decision()
                }
            } else {
                unchanged_decision()
            };
            return pick_drag_edge_decisions(start_dragged, current, other);
        }

        let current = bind_decision(BindMode::Inside, hit, global_point);
        return pick_drag_edge_decisions(start_dragged, current, unchanged_decision());
    }

    let Some(hit) = hit else {
        return pick_drag_edge_decisions(start_dragged, clear_decision(), other_decision);
    };

    if let Some(other_bindable) = other_bindable
        && is_overlapping_other
        && is_bindable_background_opaque(other_bindable)
    {
        let current = bind_decision(BindMode::Inside, other_bindable, global_point);
        return pick_drag_edge_decisions(start_dragged, current, unchanged_decision());
    }

    // Normal case: inside when the pointer is within the hit element,
    // orbit otherwise — new arrows included (Excalidraw `_simple`).
    let mode = if complex_bindings {
        BindMode::Orbit
    } else if point_in_hit && !is_nested {
        BindMode::Inside
    } else {
        BindMode::Orbit
    };

    let projected_focus_point = if mode == BindMode::Orbit {
        project_fixed_point_onto_diagonal(
            arrow,
            global_point,
            &hit,
            if start_dragged {
                ArrowEndpointEdge::Start
            } else {
                ArrowEndpointEdge::End
            },
            bindables,
            context.zoom,
            midpoint_snapping_enabled,
        )
        .unwrap_or(global_point)
    } else {
        global_point
    };
    let current = bind_decision(mode, &hit, projected_focus_point);
    pick_drag_edge_decisions(start_dragged, current, other_decision)
}

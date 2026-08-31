use super::direction::{aabb_for_bindable, heading_for_point_from_bindable};
use super::outline::{intersect_outline, normalize_vector, point_from_vector};
use super::snap_points::{avoid_rectangular_corner, snap_to_mid};
use super::*;

pub fn bind_point_to_outline(
    arrow: &ArrowState,
    bindable: &BindableState,
    start_or_end: ArrowEndpointEdge,
    custom_intersector: Option<[Point; 2]>,
) -> Point {
    let shape = bindable.shape;
    let point = get_point_at_index_global(
        arrow,
        if start_or_end == ArrowEndpointEdge::Start {
            0
        } else {
            -1
        },
    );
    if arrow.points.len() < 2 {
        return point;
    }

    let edge_point = if shape == crate::BindableShape::Rectangle && arrow.elbowed {
        avoid_rectangular_corner(arrow.elbowed, bindable, point)
    } else {
        point
    };
    let adjacent_point = if let Some(intersector) = custom_intersector {
        if !arrow.elbowed {
            intersector[1]
        } else {
            get_point_at_index_global(
                arrow,
                if start_or_end == ArrowEndpointEdge::Start {
                    1
                } else {
                    -2
                },
            )
        }
    } else {
        get_point_at_index_global(
            arrow,
            if start_or_end == ArrowEndpointEdge::Start {
                1
            } else {
                -2
            },
        )
    };
    let binding_gap = get_binding_gap(bindable, arrow.elbowed);
    let bindable_center = center(bindable.x, bindable.y, bindable.width, bindable.height);

    let intersection = if arrow.elbowed {
        let heading =
            heading_for_point_from_bindable(point, bindable, aabb_for_bindable(bindable, None));
        let is_horizontal = matches!(heading, Heading::Left | Heading::Right);
        let resolved = snap_to_mid(bindable, edge_point, Some(0.05), Some(arrow)).unwrap_or(point);
        let other_point = if is_horizontal {
            [bindable_center[0], resolved[1]]
        } else {
            [resolved[0], bindable_center[1]]
        };
        let intersector = custom_intersector.unwrap_or_else(|| {
            let direction = normalize_vector(other_point, resolved).unwrap_or([0.0, 0.0]);
            [
                other_point,
                point_from_vector(
                    other_point,
                    direction,
                    bindable.width.max(bindable.height) * 2.0,
                ),
            ]
        });
        let mut intersections =
            intersect_outline(intersector[0], intersector[1], bindable, binding_gap);
        intersections.sort_by(|left, right| {
            distance_sq(point, *left)
                .partial_cmp(&distance_sq(point, *right))
                .unwrap_or(std::cmp::Ordering::Equal)
        });
        intersections.into_iter().next().or_else(|| {
            let another_point = if is_horizontal {
                [resolved[0], bindable_center[1]]
            } else {
                [bindable_center[0], resolved[1]]
            };
            let direction = normalize_vector(another_point, resolved).unwrap_or([0.0, 0.0]);
            let ray_end = point_from_vector(
                another_point,
                direction,
                bindable.width.max(bindable.height) * 2.0,
            );
            let mut fallback =
                intersect_outline(another_point, ray_end, bindable, BASE_BINDING_GAP_ELBOW);
            fallback.sort_by(|left, right| {
                distance_sq(point, *left)
                    .partial_cmp(&distance_sq(point, *right))
                    .unwrap_or(std::cmp::Ordering::Equal)
            });
            fallback.into_iter().next()
        })
    } else {
        let mut intersector = custom_intersector;
        if intersector.is_none()
            && let Some(direction) = normalize_vector(edge_point, adjacent_point)
        {
            let half_vector = point_from_vector(
                [0.0, 0.0],
                direction,
                distance(edge_point, adjacent_point)
                    + bindable.width.max(bindable.height)
                    + binding_gap * 2.0,
            );
            intersector = Some([
                point_from_vector(adjacent_point, half_vector, 1.0),
                point_from_vector(adjacent_point, [-half_vector[0], -half_vector[1]], 1.0),
            ]);
        }

        if distance(edge_point, adjacent_point) < 1.0 {
            Some(edge_point)
        } else if let Some(intersector) = intersector {
            let mut intersections =
                intersect_outline(intersector[0], intersector[1], bindable, binding_gap);
            intersections.sort_by(|left, right| {
                distance_sq(*left, adjacent_point)
                    .partial_cmp(&distance_sq(*right, adjacent_point))
                    .unwrap_or(std::cmp::Ordering::Equal)
            });
            intersections.into_iter().next()
        } else {
            None
        }
    };

    match intersection {
        Some(intersection) if distance_sq(edge_point, intersection) >= 1e-4 => intersection,
        _ => edge_point,
    }
}

pub fn calculate_fixed_point_for_binding(bindable: &BindableState, global_point: Point) -> Point {
    let bindable_center = center(bindable.x, bindable.y, bindable.width, bindable.height);
    let local = unrotate_point(global_point, bindable_center, bindable.angle);
    let fixed_x = (local[0] - bindable.x) / bindable.width.max(1e-6);
    let fixed_y = (local[1] - bindable.y) / bindable.height.max(1e-6);
    normalize_fixed_point([fixed_x, fixed_y])
}

pub fn calculate_fixed_point_for_elbow_binding(
    arrow: &ArrowState,
    bindable: &BindableState,
    start_or_end: ArrowEndpointEdge,
) -> Point {
    calculate_fixed_point_for_binding(
        bindable,
        bind_point_to_outline(arrow, bindable, start_or_end, None),
    )
}

pub fn update_bound_point(
    arrow: &ArrowState,
    edge: ArrowEndpointSelector,
    binding: Option<&FixedPointBinding>,
    bindable: &BindableState,
    bindables_by_id: &BindableLookupRecord,
    dragging: bool,
) -> Option<Point> {
    let binding = binding?;
    if (binding.element_id != bindable.id && arrow.points.len() > 2)
        || arrow.points.len() < 2
        || points_equal(
            arrow.points.last().copied().unwrap_or([0.0, 0.0]),
            [0.0, 0.0],
            0.0,
        )
    {
        return None;
    }

    let focus_point = get_global_fixed_point(binding, bindable);
    if binding.mode == BindMode::Inside {
        return Some(to_local_point(arrow, focus_point));
    }

    let normalized_edge = normalize_arrow_endpoint_edge(edge);
    let other_binding = if normalized_edge == ArrowEndpointEdge::Start {
        arrow.end_binding.as_ref()
    } else {
        arrow.start_binding.as_ref()
    };
    let other_point = get_point_at_index_global(
        arrow,
        if normalized_edge == ArrowEndpointEdge::Start {
            1
        } else {
            -2
        },
    );
    let other_bindable = other_binding.and_then(|binding| bindables_by_id.get(&binding.element_id));
    let other_focus = match (other_binding, other_bindable) {
        (Some(other_binding), Some(other_bindable)) => {
            Some(get_global_fixed_point(other_binding, other_bindable))
        }
        _ => None,
    };
    let other_focus_point_or_arrow_point = if arrow.points.len() == 2 {
        other_focus.unwrap_or(other_point)
    } else {
        other_point
    };

    let other_outline = if let Some(other_bindable) = other_bindable {
        let mut intersections = intersect_outline(
            focus_point,
            other_focus_point_or_arrow_point,
            other_bindable,
            get_binding_gap(other_bindable, arrow.elbowed),
        );
        intersections.sort_by(|left, right| {
            distance_sq(*left, focus_point)
                .partial_cmp(&distance_sq(*right, focus_point))
                .unwrap_or(std::cmp::Ordering::Equal)
        });
        intersections.into_iter().next()
    } else {
        None
    };

    let mut outline = intersect_outline(
        focus_point,
        other_focus_point_or_arrow_point,
        bindable,
        get_binding_gap(bindable, arrow.elbowed),
    );
    outline.sort_by(|left, right| {
        distance_sq(*left, other_focus_point_or_arrow_point)
            .partial_cmp(&distance_sq(*right, other_focus_point_or_arrow_point))
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    let outline = outline.into_iter().next();

    let start_has_arrowhead = arrow.start_arrowhead.is_some();
    let end_has_arrowhead = arrow.end_arrowhead.is_some();
    let current_has_arrowhead = if normalized_edge == ArrowEndpointEdge::Start {
        start_has_arrowhead
    } else {
        end_has_arrowhead
    };
    let resolved_target = if (!start_has_arrowhead && !end_has_arrowhead) || current_has_arrowhead {
        focus_point
    } else {
        outline.unwrap_or(focus_point)
    };

    if let (Some(other_bindable), Some(outline_point)) = (other_bindable, outline)
        && !dragging
        && other_bindable.width * other_bindable.height < bindable.width * bindable.height * 2.0
        && (is_point_in_bindable(outline_point, other_bindable)
            || distance_to_bindable_outline(outline_point, other_bindable)
                <= get_binding_gap(other_bindable, arrow.elbowed))
    {
        return Some(to_local_point(arrow, resolved_target));
    }

    let other_target_point = if other_bindable.is_some() {
        other_outline.or(other_focus).unwrap_or(other_point)
    } else {
        other_point
    };
    let target_point = outline.unwrap_or(focus_point);
    let too_short = distance(other_target_point, target_point) <= BASE_ARROW_MIN_LENGTH;

    if other_bindable.is_none() {
        return Some(to_local_point(
            arrow,
            if too_short { focus_point } else { target_point },
        ));
    }

    if too_short {
        return Some(to_local_point(arrow, resolved_target));
    }

    Some(to_local_point(arrow, target_point))
}

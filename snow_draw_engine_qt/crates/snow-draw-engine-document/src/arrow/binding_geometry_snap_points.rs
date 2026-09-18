use super::outline::{LineSegment, line_intersection, normalize_vector};
use super::*;

pub(super) fn avoid_rectangular_corner(
    elbowed: bool,
    bindable: &BindableState,
    point: Point,
) -> Point {
    let bindable_center = center(bindable.x, bindable.y, bindable.width, bindable.height);
    let local = unrotate_point(point, bindable_center, bindable.angle);
    let binding_gap = get_binding_gap(bindable, elbowed);

    if local[0] < bindable.x && local[1] < bindable.y {
        if local[1] - bindable.y > -binding_gap {
            return rotate_point(
                [bindable.x - binding_gap, bindable.y],
                bindable_center,
                bindable.angle,
            );
        }
        return rotate_point(
            [bindable.x, bindable.y - binding_gap],
            bindable_center,
            bindable.angle,
        );
    }

    if local[0] < bindable.x && local[1] > bindable.y + bindable.height {
        if local[0] - bindable.x > -binding_gap {
            return rotate_point(
                [bindable.x, bindable.y + bindable.height + binding_gap],
                bindable_center,
                bindable.angle,
            );
        }
        return rotate_point(
            [bindable.x - binding_gap, bindable.y + bindable.height],
            bindable_center,
            bindable.angle,
        );
    }

    if local[0] > bindable.x + bindable.width && local[1] > bindable.y + bindable.height {
        if local[0] - bindable.x < bindable.width + binding_gap {
            return rotate_point(
                [
                    bindable.x + bindable.width,
                    bindable.y + bindable.height + binding_gap,
                ],
                bindable_center,
                bindable.angle,
            );
        }
        return rotate_point(
            [
                bindable.x + bindable.width + binding_gap,
                bindable.y + bindable.height,
            ],
            bindable_center,
            bindable.angle,
        );
    }

    if local[0] > bindable.x + bindable.width && local[1] < bindable.y {
        if local[0] - bindable.x < bindable.width + binding_gap {
            return rotate_point(
                [bindable.x + bindable.width, bindable.y - binding_gap],
                bindable_center,
                bindable.angle,
            );
        }
        return rotate_point(
            [bindable.x + bindable.width + binding_gap, bindable.y],
            bindable_center,
            bindable.angle,
        );
    }

    point
}

pub(super) fn snap_to_mid(
    bindable: &BindableState,
    point: Point,
    tolerance: Option<f64>,
    arrow: Option<&ArrowState>,
) -> Option<Point> {
    let shape = bindable.shape;
    let tolerance = tolerance.unwrap_or(0.05);
    let raw_center = center(bindable.x, bindable.y, bindable.width, bindable.height);
    let bindable_center = [raw_center[0] - 0.1, raw_center[1] - 0.1];
    let local = unrotate_point(point, bindable_center, bindable.angle);
    let binding_gap = arrow.map_or(0.0, |arrow| get_binding_gap(bindable, arrow.elbowed));
    let vertical_threshold = clamp(tolerance * bindable.height, 5.0, 80.0);
    let horizontal_threshold = clamp(tolerance * bindable.width, 5.0, 80.0);

    if distance(bindable_center, local) < binding_gap {
        return None;
    }

    if local[0] <= bindable.x + bindable.width / 2.0
        && local[1] > bindable_center[1] - vertical_threshold
        && local[1] < bindable_center[1] + vertical_threshold
    {
        return Some(rotate_point(
            [bindable.x - binding_gap, bindable_center[1]],
            bindable_center,
            bindable.angle,
        ));
    }

    if local[1] <= bindable.y + bindable.height / 2.0
        && local[0] > bindable_center[0] - horizontal_threshold
        && local[0] < bindable_center[0] + horizontal_threshold
    {
        return Some(rotate_point(
            [bindable_center[0], bindable.y - binding_gap],
            bindable_center,
            bindable.angle,
        ));
    }

    if local[0] >= bindable.x + bindable.width / 2.0
        && local[1] > bindable_center[1] - vertical_threshold
        && local[1] < bindable_center[1] + vertical_threshold
    {
        return Some(rotate_point(
            [
                bindable.x + bindable.width + binding_gap,
                bindable_center[1],
            ],
            bindable_center,
            bindable.angle,
        ));
    }

    if local[1] >= bindable.y + bindable.height / 2.0
        && local[0] > bindable_center[0] - horizontal_threshold
        && local[0] < bindable_center[0] + horizontal_threshold
    {
        return Some(rotate_point(
            [
                bindable_center[0],
                bindable.y + bindable.height + binding_gap,
            ],
            bindable_center,
            bindable.angle,
        ));
    }

    if shape == crate::BindableShape::Diamond {
        let offset_threshold = horizontal_threshold.max(vertical_threshold);
        let qx = bindable.width / 4.0;
        let qy = bindable.height / 4.0;
        let corners = [
            [bindable.x + qx - binding_gap, bindable.y + qy - binding_gap],
            [
                bindable.x + 3.0 * qx + binding_gap,
                bindable.y + qy - binding_gap,
            ],
            [
                bindable.x + qx - binding_gap,
                bindable.y + 3.0 * qy + binding_gap,
            ],
            [
                bindable.x + 3.0 * qx + binding_gap,
                bindable.y + 3.0 * qy + binding_gap,
            ],
        ];

        for corner in corners {
            if distance(corner, local) < offset_threshold {
                return Some(rotate_point(corner, bindable_center, bindable.angle));
            }
        }
    }

    None
}

fn get_snap_outline_mid_point_candidates(bindable: &BindableState) -> [Point; 4] {
    [
        get_binding_side_mid_point([1.0, 0.5], bindable),
        get_binding_side_mid_point([0.5, 1.0], bindable),
        get_binding_side_mid_point([0.0, 0.5], bindable),
        get_binding_side_mid_point([0.5, 0.0], bindable),
    ]
}

pub fn get_snap_outline_mid_point(
    point: Point,
    bindable: &BindableState,
    zoom: f64,
) -> Option<Point> {
    let threshold = max_binding_distance(zoom) + bindable.stroke_width / 2.0;
    get_snap_outline_mid_point_candidates(bindable)
        .into_iter()
        .find(|&candidate| {
            distance(point, candidate) <= threshold && !is_point_in_bindable(point, bindable)
        })
}

/// How midpoint indicators behave for a binding suggestion, combining arrow
/// kind with the angle-lock gate so call sites cannot transpose two booleans.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OutlineMidPointMode {
    /// Angle-locked drag: no indicators at all.
    Disabled,
    /// Straight arrows: indicators hidden while the cursor is inside the
    /// bindable; only the nearest midpoint may show as a proximity hint.
    Linear,
    /// Elbow arrows: bindings keep anchoring to the outline midpoints, so all
    /// candidates stay visible even with the cursor inside the bindable.
    Elbow,
}

impl OutlineMidPointMode {
    pub fn for_arrow(elbowed: bool, midpoint_snapping_enabled: bool) -> Self {
        if !midpoint_snapping_enabled {
            Self::Disabled
        } else if elbowed {
            Self::Elbow
        } else {
            Self::Linear
        }
    }
}

/// Midpoint indicators rendered alongside a binding highlight, mirroring
/// Excalidraw's interactive-scene dots: the closest side midpoint within the
/// snap threshold is highlighted, and the remaining candidates are shown as
/// proximity hints (all of them for elbow arrows, only the closest one within
/// twice the threshold otherwise).
pub struct OutlineMidPointSuggestion {
    pub highlighted: Option<Point>,
    pub near: Vec<Point>,
}

pub fn outline_mid_point_suggestion(
    pointer: Point,
    bindable: &BindableState,
    zoom: f64,
    mode: OutlineMidPointMode,
) -> OutlineMidPointSuggestion {
    let elbowed = mode == OutlineMidPointMode::Elbow;
    let none = OutlineMidPointSuggestion {
        highlighted: None,
        near: Vec::new(),
    };
    if mode == OutlineMidPointMode::Disabled {
        return none;
    }
    // Indicators are hidden while the cursor is inside the bindable, except
    // for elbow arrows whose bindings still anchor to the outline midpoints.
    if !elbowed && is_point_in_bindable(pointer, bindable) {
        return none;
    }

    let threshold = max_binding_distance(zoom) + bindable.stroke_width / 2.0;
    let candidates = get_snap_outline_mid_point_candidates(bindable);
    let (closest_index, closest_distance) = candidates
        .iter()
        .enumerate()
        .map(|(index, candidate)| (index, distance(pointer, *candidate)))
        .min_by(|left, right| {
            left.1
                .partial_cmp(&right.1)
                .unwrap_or(std::cmp::Ordering::Equal)
        })
        .unwrap_or((0, f64::INFINITY));

    let highlighted = (closest_distance <= threshold).then(|| candidates[closest_index]);
    let near = if elbowed {
        candidates
            .iter()
            .enumerate()
            .filter(|(index, _)| highlighted.is_none() || *index != closest_index)
            .map(|(_, point)| *point)
            .collect()
    } else if highlighted.is_none() && closest_distance <= threshold * 2.0 {
        vec![candidates[closest_index]]
    } else {
        Vec::new()
    };

    OutlineMidPointSuggestion { highlighted, near }
}

fn get_diagonal_guide_segments(bindable: &BindableState) -> [LineSegment; 2] {
    let shape = bindable.shape;
    let c = center(bindable.x, bindable.y, bindable.width, bindable.height);

    if shape == crate::BindableShape::Rectangle {
        let top_left = rotate_point([bindable.x, bindable.y], c, bindable.angle);
        let top_right = rotate_point([bindable.x + bindable.width, bindable.y], c, bindable.angle);
        let bottom_right = rotate_point(
            [bindable.x + bindable.width, bindable.y + bindable.height],
            c,
            bindable.angle,
        );
        let bottom_left = rotate_point(
            [bindable.x, bindable.y + bindable.height],
            c,
            bindable.angle,
        );
        return [
            LineSegment {
                from: top_left,
                to: bottom_right,
            },
            LineSegment {
                from: top_right,
                to: bottom_left,
            },
        ];
    }

    let top_center = rotate_point(
        [bindable.x + bindable.width / 2.0, bindable.y],
        c,
        bindable.angle,
    );
    let bottom_center = rotate_point(
        [
            bindable.x + bindable.width / 2.0,
            bindable.y + bindable.height,
        ],
        c,
        bindable.angle,
    );
    let left_center = rotate_point(
        [bindable.x, bindable.y + bindable.height / 2.0],
        c,
        bindable.angle,
    );
    let right_center = rotate_point(
        [
            bindable.x + bindable.width,
            bindable.y + bindable.height / 2.0,
        ],
        c,
        bindable.angle,
    );

    [
        LineSegment {
            from: top_center,
            to: bottom_center,
        },
        LineSegment {
            from: left_center,
            to: right_center,
        },
    ]
}

pub fn project_fixed_point_onto_diagonal(
    arrow: &ArrowState,
    point: Point,
    bindable: &BindableState,
    start_or_end: ArrowEndpointEdge,
    bindables: &[BindableState],
    zoom: f64,
    midpoint_snapping_enabled: bool,
) -> Option<Point> {
    if arrow.points.len() < 2 || (arrow.width < 3.0 && arrow.height < 3.0) {
        return None;
    }

    if midpoint_snapping_enabled
        && let Some(side_mid_point) = get_snap_outline_mid_point(point, bindable, zoom)
    {
        return Some(side_mid_point);
    }

    let [diagonal_one, diagonal_two] = get_diagonal_guide_segments(bindable);
    let bindables_by_id = bindables
        .iter()
        .map(|candidate| (candidate.id, candidate.clone()))
        .collect::<BTreeMap<_, _>>();

    let mut anchor = get_point_at_index_global(
        arrow,
        if start_or_end == ArrowEndpointEdge::Start {
            1
        } else {
            -2
        },
    );
    if arrow.points.len() == 2 {
        let other_binding = if start_or_end == ArrowEndpointEdge::Start {
            arrow.end_binding.as_ref()
        } else {
            arrow.start_binding.as_ref()
        };
        let other_bindable =
            other_binding.and_then(|binding| bindables_by_id.get(&binding.element_id));
        if let (Some(other_binding), Some(other_bindable)) = (other_binding, other_bindable) {
            anchor = get_global_fixed_point(other_binding, other_bindable);
        }
    }

    let direction = normalize_vector(anchor, point)?;
    let extent = distance(diagonal_one.from, diagonal_one.to)
        .max(distance(diagonal_two.from, diagonal_two.to));
    let ray_length = 2.0 * distance(anchor, point) + extent;
    let ray_point = [
        anchor[0] + direction[0] * ray_length,
        anchor[1] + direction[1] * ray_length,
    ];

    let p1 = line_intersection(diagonal_one.from, diagonal_one.to, ray_point, anchor);
    let p2 = line_intersection(diagonal_two.from, diagonal_two.to, ray_point, anchor);
    let projection = match (p1, p2) {
        (Some(left), Some(right)) => {
            if distance(anchor, left) <= distance(anchor, right) {
                Some(left)
            } else {
                Some(right)
            }
        }
        (Some(point), None) | (None, Some(point)) => Some(point),
        (None, None) => None,
    }?;

    if is_point_in_bindable(projection, bindable) {
        Some(projection)
    } else {
        None
    }
}

use std::collections::BTreeMap;

use crate::arrow_elbow_router::{BASE_PADDING, DEDUP_THRESHOLD};
use crate::arrow_geom::{Heading, distance, is_horizontal_heading};
use crate::{ArrowPatch, ArrowState, BindableState, ElementId, FixedSegment};

use super::{
    compute_default_route_patch, global_points_for_arrow, heading_for_point_is_horizontal,
    normalize_arrow_element_update, points_equal, to_global_point, to_local_point,
};

pub(super) fn handle_segment_renormalization(
    arrow: &ArrowState,
    bindables_by_id: &BTreeMap<ElementId, BindableState>,
    zoom: f64,
    max_coordinate: f64,
    midpoint_snapping_enabled: bool,
) -> ArrowPatch {
    let Some(mut next_fixed_segments) = arrow.fixed_segments.clone() else {
        return ArrowPatch {
            x: Some(arrow.x),
            y: Some(arrow.y),
            width: Some(arrow.width),
            height: Some(arrow.height),
            points: Some(arrow.points.clone()),
            fixed_segments: Some(None),
            start_is_special: Some(arrow.start_is_special),
            end_is_special: Some(arrow.end_is_special),
            ..ArrowPatch::default()
        };
    };

    let global_points = global_points_for_arrow(arrow);
    let mut merged_points = Vec::new();
    for (index, point) in global_points.iter().copied().enumerate() {
        if index < 2 {
            merged_points.push(point);
            continue;
        }

        let current_is_horizontal =
            heading_for_point_is_horizontal(point, global_points[index - 1]);
        let previous_is_horizontal =
            heading_for_point_is_horizontal(global_points[index - 1], global_points[index - 2]);

        if current_is_horizontal == previous_is_horizontal {
            let previous_segment_idx = next_fixed_segments
                .iter()
                .position(|segment| segment.index == index - 1);
            let current_segment_idx = next_fixed_segments
                .iter()
                .position(|segment| segment.index == index);

            if let Some(current_segment_idx) = current_segment_idx {
                next_fixed_segments[current_segment_idx].start =
                    to_local_point([arrow.x, arrow.y], global_points[index - 2]);
            }

            if let Some(previous_segment_idx) = previous_segment_idx {
                next_fixed_segments.remove(previous_segment_idx);
            }

            merged_points.pop();
            for segment in &mut next_fixed_segments {
                if segment.index > index - 1 {
                    segment.index -= 1;
                }
            }
        }

        merged_points.push(point);
    }

    let mut next_points = Vec::new();
    for (index, point) in merged_points.iter().copied().enumerate() {
        if index < 3 {
            next_points.push(point);
            continue;
        }

        if distance(merged_points[index - 2], merged_points[index - 1]) < DEDUP_THRESHOLD {
            let previous_previous_segment_idx = next_fixed_segments
                .iter()
                .position(|segment| segment.index == index - 2);
            let previous_segment_idx = next_fixed_segments
                .iter()
                .position(|segment| segment.index == index - 1);

            let mut to_remove = [previous_previous_segment_idx, previous_segment_idx]
                .into_iter()
                .flatten()
                .collect::<Vec<_>>();
            to_remove.sort_unstable();
            to_remove.dedup();
            for remove_index in to_remove.into_iter().rev() {
                next_fixed_segments.remove(remove_index);
            }

            next_points.pop();
            next_points.pop();

            for segment in &mut next_fixed_segments {
                if segment.index > index - 2 {
                    segment.index -= 2;
                }
            }

            let is_horizontal = heading_for_point_is_horizontal(point, merged_points[index - 1]);
            next_points.push([
                if is_horizontal {
                    point[0]
                } else {
                    merged_points[index - 2][0]
                },
                if is_horizontal {
                    merged_points[index - 2][1]
                } else {
                    point[1]
                },
            ]);
            continue;
        }

        next_points.push(point);
    }

    let filtered_fixed_segments = next_fixed_segments
        .into_iter()
        .filter(|segment| segment.index != 1 && segment.index != next_points.len() - 1)
        .collect::<Vec<_>>();
    if filtered_fixed_segments.is_empty() {
        let mut reroute_arrow = arrow.clone();
        reroute_arrow.points = next_points
            .iter()
            .map(|point| to_local_point([arrow.x, arrow.y], *point))
            .collect();
        reroute_arrow.fixed_segments = None;
        return compute_default_route_patch(
            &reroute_arrow,
            bindables_by_id,
            zoom,
            false,
            max_coordinate,
            midpoint_snapping_enabled,
        );
    }

    normalize_arrow_element_update(
        &next_points,
        Some(filtered_fixed_segments),
        Some(arrow.start_is_special),
        Some(arrow.end_is_special),
        max_coordinate,
    )
}

pub(super) fn handle_segment_move(
    arrow: &ArrowState,
    mut fixed_segments: Vec<FixedSegment>,
    start_heading: Heading,
    end_heading: Heading,
    hovered_start_bindable: Option<&BindableState>,
    hovered_end_bindable: Option<&BindableState>,
    max_coordinate: f64,
) -> ArrowPatch {
    let actively_modified_segment_idx =
        fixed_segments
            .iter()
            .enumerate()
            .find_map(|(index, segment)| {
                let Some(existing) = arrow
                    .fixed_segments
                    .as_ref()
                    .and_then(|segments| segments.get(index))
                else {
                    return Some(index);
                };
                if existing.index != segment.index {
                    return Some(index);
                }

                let moved_x = (segment.start[0] - existing.start[0]).abs() > 1e-6
                    && (segment.end[0] - existing.end[0]).abs() > 1e-6;
                let moved_y = (segment.start[1] - existing.start[1]).abs() > 1e-6
                    && (segment.end[1] - existing.end[1]).abs() > 1e-6;
                (moved_x != moved_y).then_some(index)
            });

    let Some(actively_modified_segment_idx) = actively_modified_segment_idx else {
        return ArrowPatch {
            points: Some(arrow.points.clone()),
            ..ArrowPatch::default()
        };
    };

    let first_segment_idx = arrow
        .fixed_segments
        .as_ref()
        .and_then(|segments| segments.iter().position(|segment| segment.index == 1));
    let last_segment_idx = arrow.fixed_segments.as_ref().and_then(|segments| {
        segments
            .iter()
            .position(|segment| segment.index == arrow.points.len() - 1)
    });

    let segment_length = distance(
        fixed_segments[actively_modified_segment_idx].start,
        fixed_segments[actively_modified_segment_idx].end,
    );
    let segment_is_too_short = segment_length < BASE_PADDING + 5.0;
    if first_segment_idx.is_none()
        && fixed_segments[actively_modified_segment_idx].index == 1
        && hovered_start_bindable.is_some()
    {
        let start_is_horizontal = is_horizontal_heading(start_heading);
        let start_is_positive = if start_is_horizontal {
            start_heading == Heading::Right
        } else {
            start_heading == Heading::Down
        };
        let padding = if start_is_positive {
            if segment_is_too_short {
                segment_length / 2.0
            } else {
                BASE_PADDING
            }
        } else if segment_is_too_short {
            -segment_length / 2.0
        } else {
            -BASE_PADDING
        };
        fixed_segments[actively_modified_segment_idx].start = [
            fixed_segments[actively_modified_segment_idx].start[0]
                + if start_is_horizontal { padding } else { 0.0 },
            fixed_segments[actively_modified_segment_idx].start[1]
                + if start_is_horizontal { 0.0 } else { padding },
        ];
    }

    if last_segment_idx.is_none()
        && fixed_segments[actively_modified_segment_idx].index == arrow.points.len() - 1
        && hovered_end_bindable.is_some()
    {
        let end_is_horizontal = is_horizontal_heading(end_heading);
        let end_is_positive = if end_is_horizontal {
            end_heading == Heading::Right
        } else {
            end_heading == Heading::Down
        };
        let padding = if end_is_positive {
            if segment_is_too_short {
                segment_length / 2.0
            } else {
                BASE_PADDING
            }
        } else if segment_is_too_short {
            -segment_length / 2.0
        } else {
            -BASE_PADDING
        };
        fixed_segments[actively_modified_segment_idx].end = [
            fixed_segments[actively_modified_segment_idx].end[0]
                + if end_is_horizontal { padding } else { 0.0 },
            fixed_segments[actively_modified_segment_idx].end[1]
                + if end_is_horizontal { 0.0 } else { padding },
        ];
    }

    let origin = [arrow.x, arrow.y];
    let mut next_fixed_segments = fixed_segments
        .iter()
        .map(|segment| FixedSegment {
            index: segment.index,
            start: to_global_point(origin, segment.start),
            end: to_global_point(origin, segment.end),
        })
        .collect::<Vec<_>>();
    let mut new_points = global_points_for_arrow(arrow);

    let start_idx = next_fixed_segments[actively_modified_segment_idx].index - 1;
    let end_idx = next_fixed_segments[actively_modified_segment_idx].index;
    let start = next_fixed_segments[actively_modified_segment_idx].start;
    let end = next_fixed_segments[actively_modified_segment_idx].end;
    let previous_segment_is_horizontal =
        if start_idx > 0 && !points_equal(new_points[start_idx], new_points[start_idx - 1]) {
            Some(heading_for_point_is_horizontal(
                new_points[start_idx - 1],
                new_points[start_idx],
            ))
        } else {
            None
        };
    let next_segment_is_horizontal = if end_idx + 1 < new_points.len()
        && !points_equal(new_points[end_idx], new_points[end_idx + 1])
    {
        Some(heading_for_point_is_horizontal(
            new_points[end_idx + 1],
            new_points[end_idx],
        ))
    } else {
        None
    };

    if let Some(previous_segment_is_horizontal) = previous_segment_is_horizontal {
        let axis = if previous_segment_is_horizontal { 1 } else { 0 };
        new_points[start_idx - 1][axis] = start[axis];
    }
    new_points[start_idx] = start;
    new_points[end_idx] = end;
    if let Some(next_segment_is_horizontal) = next_segment_is_horizontal {
        let axis = if next_segment_is_horizontal { 1 } else { 0 };
        new_points[end_idx + 1][axis] = end[axis];
    }

    if let Some(previous_segment_idx) = next_fixed_segments
        .iter()
        .position(|segment| segment.index == start_idx)
    {
        let axis = if heading_for_point_is_horizontal(
            next_fixed_segments[previous_segment_idx].end,
            next_fixed_segments[previous_segment_idx].start,
        ) {
            1
        } else {
            0
        };
        next_fixed_segments[previous_segment_idx].start[axis] = start[axis];
        next_fixed_segments[previous_segment_idx].end = start;
    }

    if let Some(next_segment_idx) = next_fixed_segments
        .iter()
        .position(|segment| segment.index == end_idx + 1)
    {
        let axis = if heading_for_point_is_horizontal(
            next_fixed_segments[next_segment_idx].end,
            next_fixed_segments[next_segment_idx].start,
        ) {
            1
        } else {
            0
        };
        next_fixed_segments[next_segment_idx].end[axis] = end[axis];
        next_fixed_segments[next_segment_idx].start = end;
    }

    if first_segment_idx.is_none() && start_idx == 0 {
        let start_is_horizontal = if hovered_start_bindable.is_some() {
            is_horizontal_heading(start_heading)
        } else {
            heading_for_point_is_horizontal(new_points[1], new_points[0])
        };
        new_points.insert(
            0,
            [
                if start_is_horizontal {
                    start[0]
                } else {
                    arrow.x + arrow.points[0][0]
                },
                if start_is_horizontal {
                    arrow.y + arrow.points[0][1]
                } else {
                    start[1]
                },
            ],
        );

        if hovered_start_bindable.is_some() {
            new_points.insert(
                0,
                [arrow.x + arrow.points[0][0], arrow.y + arrow.points[0][1]],
            );
        }

        for segment in &mut next_fixed_segments {
            segment.index += if hovered_start_bindable.is_some() {
                2
            } else {
                1
            };
        }
    }

    if last_segment_idx.is_none() && end_idx == arrow.points.len() - 1 {
        let end_is_horizontal = is_horizontal_heading(end_heading);
        new_points.push([
            if end_is_horizontal {
                end[0]
            } else {
                arrow.x + arrow.points[arrow.points.len() - 1][0]
            },
            if end_is_horizontal {
                arrow.y + arrow.points[arrow.points.len() - 1][1]
            } else {
                end[1]
            },
        ]);
        if hovered_end_bindable.is_some() {
            new_points.push([
                arrow.x + arrow.points[arrow.points.len() - 1][0],
                arrow.y + arrow.points[arrow.points.len() - 1][1],
            ]);
        }
    }

    normalize_arrow_element_update(
        &new_points,
        Some(
            next_fixed_segments
                .into_iter()
                .map(|segment| FixedSegment {
                    index: segment.index,
                    start: to_local_point(origin, segment.start),
                    end: to_local_point(origin, segment.end),
                })
                .collect(),
        ),
        Some(Some(false)),
        Some(Some(false)),
        max_coordinate,
    )
}

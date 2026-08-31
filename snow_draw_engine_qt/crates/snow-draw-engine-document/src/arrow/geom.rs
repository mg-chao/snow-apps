use crate::{
    ArrowState, BindableState, Bounds, BindableShape, FixedPointBinding, Point,
};
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Heading {
    Up,
    Right,
    Down,
    Left,
}

pub const EPSILON: f64 = 1e-4;

pub fn clamp(value: f64, min: f64, max: f64) -> f64 {
    if value < min {
        return min;
    }
    if value > max {
        return max;
    }
    value
}

pub fn center(x: f64, y: f64, width: f64, height: f64) -> Point {
    [x + width / 2.0, y + height / 2.0]
}

pub fn rotate_point(point: Point, around: Point, angle: f64) -> Point {
    if angle == 0.0 {
        return point;
    }
    let dx = point[0] - around[0];
    let dy = point[1] - around[1];
    let cos = angle.cos();
    let sin = angle.sin();
    [
        around[0] + dx * cos - dy * sin,
        around[1] + dx * sin + dy * cos,
    ]
}

pub fn unrotate_point(point: Point, around: Point, angle: f64) -> Point {
    rotate_point(point, around, -angle)
}

pub fn distance_sq(a: Point, b: Point) -> f64 {
    let dx = a[0] - b[0];
    let dy = a[1] - b[1];
    dx * dx + dy * dy
}

pub fn distance(a: Point, b: Point) -> f64 {
    distance_sq(a, b).sqrt()
}

pub fn manhattan(a: Point, b: Point) -> f64 {
    (a[0] - b[0]).abs() + (a[1] - b[1]).abs()
}

pub fn to_global_point(arrow: &ArrowState, local_point: Point) -> Point {
    [arrow.x + local_point[0], arrow.y + local_point[1]]
}

pub fn to_local_point(arrow: &ArrowState, global_point: Point) -> Point {
    [global_point[0] - arrow.x, global_point[1] - arrow.y]
}

pub fn get_point_at_index(arrow: &ArrowState, index_maybe_from_end: isize) -> Point {
    let index = if index_maybe_from_end < 0 {
        arrow.points.len() as isize + index_maybe_from_end
    } else {
        index_maybe_from_end
    };
    if index < 0 {
        return [0.0, 0.0];
    }
    arrow
        .points
        .get(index as usize)
        .copied()
        .unwrap_or([0.0, 0.0])
}

pub fn get_point_at_index_global(arrow: &ArrowState, index_maybe_from_end: isize) -> Point {
    to_global_point(arrow, get_point_at_index(arrow, index_maybe_from_end))
}

pub fn normalize_fixed_point(ratio: Point) -> Point {
    let x = if ratio[0].is_finite() {
        ratio[0]
    } else {
        0.5001
    };
    let y = if ratio[1].is_finite() {
        ratio[1]
    } else {
        0.5001
    };
    let adjusted_x = if (x - 0.5).abs() < EPSILON { 0.5001 } else { x };
    let adjusted_y = if (y - 0.5).abs() < EPSILON { 0.5001 } else { y };
    [adjusted_x, adjusted_y]
}

pub fn get_global_fixed_point(binding: &FixedPointBinding, bindable: &BindableState) -> Point {
    let [fx, fy] = normalize_fixed_point(binding.fixed_point);
    let bindable_center = center(bindable.x, bindable.y, bindable.width, bindable.height);
    let point = [
        bindable.x + bindable.width * fx,
        bindable.y + bindable.height * fy,
    ];
    rotate_point(point, bindable_center, bindable.angle)
}

pub fn compute_bounds_from_points(points: &[Point]) -> (f64, f64) {
    if points.is_empty() {
        return (0.0, 0.0);
    }
    let mut min_x = points[0][0];
    let mut min_y = points[0][1];
    let mut max_x = points[0][0];
    let mut max_y = points[0][1];
    for point in points {
        min_x = min_x.min(point[0]);
        min_y = min_y.min(point[1]);
        max_x = max_x.max(point[0]);
        max_y = max_y.max(point[1]);
    }
    (max_x - min_x, max_y - min_y)
}

pub(crate) fn bounds_from_points(points: &[Point]) -> Bounds {
    if points.is_empty() {
        return [0.0, 0.0, 0.0, 0.0];
    }

    let mut min_x = points[0][0];
    let mut min_y = points[0][1];
    let mut max_x = points[0][0];
    let mut max_y = points[0][1];
    for point in points.iter().skip(1) {
        min_x = min_x.min(point[0]);
        min_y = min_y.min(point[1]);
        max_x = max_x.max(point[0]);
        max_y = max_y.max(point[1]);
    }
    [min_x, min_y, max_x, max_y]
}

pub(crate) fn inflate_bounds(bounds: Bounds, offsets: [f64; 4]) -> Bounds {
    let [top_offset, right_offset, bottom_offset, left_offset] = offsets;
    [
        bounds[0] - left_offset,
        bounds[1] - top_offset,
        bounds[2] + right_offset,
        bounds[3] + bottom_offset,
    ]
}

pub(crate) fn bindable_bounds_corners(bindable: &BindableState, offsets: [f64; 4]) -> [Point; 4] {
    let [top_offset, right_offset, bottom_offset, left_offset] = offsets;
    [
        [bindable.x - left_offset, bindable.y - top_offset],
        [
            bindable.x + bindable.width + right_offset,
            bindable.y - top_offset,
        ],
        [
            bindable.x + bindable.width + right_offset,
            bindable.y + bindable.height + bottom_offset,
        ],
        [
            bindable.x - left_offset,
            bindable.y + bindable.height + bottom_offset,
        ],
    ]
}

pub(crate) fn rotated_bindable_bounds(bindable: &BindableState, offsets: [f64; 4]) -> Bounds {
    let bindable_center = center(bindable.x, bindable.y, bindable.width, bindable.height);
    let rotated = bindable_bounds_corners(bindable, offsets)
        .map(|corner| rotate_point(corner, bindable_center, bindable.angle));
    bounds_from_points(&rotated)
}

pub(crate) fn apply_fixed_segments_to_global_points(
    points: Vec<Point>,
    arrow: &ArrowState,
) -> Vec<Point> {
    let Some(fixed_segments) = arrow.fixed_segments.as_ref() else {
        return points;
    };

    let mut out = points;
    for fixed in fixed_segments {
        if fixed.index == 0 || fixed.index >= out.len() {
            continue;
        }
        out[fixed.index - 1] = [arrow.x + fixed.start[0], arrow.y + fixed.start[1]];
        out[fixed.index] = [arrow.x + fixed.end[0], arrow.y + fixed.end[1]];
    }
    out
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NormalizedArrowFromGlobalPoints {
    pub x: f64,
    pub y: f64,
    pub points: Vec<Point>,
    pub width: f64,
    pub height: f64,
}

pub fn normalize_arrow_from_global_points(
    global_points: &[Point],
    max_coordinate: f64,
) -> NormalizedArrowFromGlobalPoints {
    if global_points.is_empty() {
        return NormalizedArrowFromGlobalPoints {
            x: 0.0,
            y: 0.0,
            points: Vec::new(),
            width: 0.0,
            height: 0.0,
        };
    }

    let origin = global_points[0];
    let points = global_points
        .iter()
        .map(|point| {
            [
                clamp(point[0] - origin[0], -max_coordinate, max_coordinate),
                clamp(point[1] - origin[1], -max_coordinate, max_coordinate),
            ]
        })
        .collect::<Vec<_>>();
    let (width, height) = compute_bounds_from_points(&points);

    NormalizedArrowFromGlobalPoints {
        x: clamp(origin[0], -max_coordinate, max_coordinate),
        y: clamp(origin[1], -max_coordinate, max_coordinate),
        points,
        width,
        height,
    }
}

pub fn vector_to_heading(from: Point, to: Point) -> Heading {
    let x = to[0] - from[0];
    let y = to[1] - from[1];
    let abs_x = x.abs();
    let abs_y = y.abs();
    if x > abs_y {
        return Heading::Right;
    }
    if x <= -abs_y {
        return Heading::Left;
    }
    if y > abs_x {
        return Heading::Down;
    }
    Heading::Up
}

pub fn is_horizontal_heading(heading: Heading) -> bool {
    matches!(heading, Heading::Left | Heading::Right)
}

pub fn reverse_heading(heading: Heading) -> Heading {
    match heading {
        Heading::Up => Heading::Down,
        Heading::Right => Heading::Left,
        Heading::Down => Heading::Up,
        Heading::Left => Heading::Right,
    }
}

pub fn heading_from_bindable(point: Point, bindable: &BindableState) -> Heading {
    let shape = bindable.shape;
    const SEARCH_CONE_MULTIPLIER: f64 = 2.0;
    let bindable_center = center(bindable.x, bindable.y, bindable.width, bindable.height);

    let scale_from_origin = |input_point: Point, origin: Point, factor: f64| -> Point {
        [
            origin[0] + (input_point[0] - origin[0]) * factor,
            origin[1] + (input_point[1] - origin[1]) * factor,
        ]
    };
    let vector_from_point = |input_point: Point, origin: Point| -> Point {
        [input_point[0] - origin[0], input_point[1] - origin[1]]
    };
    let cross = |left: Point, right: Point| -> f64 { left[0] * right[1] - left[1] * right[0] };

    let corner_points = [
        [bindable.x, bindable.y],
        [bindable.x + bindable.width, bindable.y],
        [bindable.x + bindable.width, bindable.y + bindable.height],
        [bindable.x, bindable.y + bindable.height],
    ];
    let corners = corner_points.map(|corner| rotate_point(corner, bindable_center, bindable.angle));
    let mut min_x = corners[0][0];
    let mut min_y = corners[0][1];
    let mut max_x = corners[0][0];
    let mut max_y = corners[0][1];
    for corner in corners {
        min_x = min_x.min(corner[0]);
        min_y = min_y.min(corner[1]);
        max_x = max_x.max(corner[0]);
        max_y = max_y.max(corner[1]);
    }
    let aabb = [min_x, min_y, max_x, max_y];

    let heading_for_point = |from: Point, to: Point| -> Heading { vector_to_heading(from, to) };
    let mid_point = [(aabb[0] + aabb[2]) / 2.0, (aabb[1] + aabb[3]) / 2.0];

    if shape == BindableShape::Diamond {
        const SHRINK: f64 = 0.95;
        let top = scale_from_origin(
            rotate_point(
                [bindable.x + bindable.width / 2.0, bindable.y],
                bindable_center,
                bindable.angle,
            ),
            mid_point,
            SHRINK,
        );
        let right = scale_from_origin(
            rotate_point(
                [
                    bindable.x + bindable.width,
                    bindable.y + bindable.height / 2.0,
                ],
                bindable_center,
                bindable.angle,
            ),
            mid_point,
            SHRINK,
        );
        let bottom = scale_from_origin(
            rotate_point(
                [
                    bindable.x + bindable.width / 2.0,
                    bindable.y + bindable.height,
                ],
                bindable_center,
                bindable.angle,
            ),
            mid_point,
            SHRINK,
        );
        let left = scale_from_origin(
            rotate_point(
                [bindable.x, bindable.y + bindable.height / 2.0],
                bindable_center,
                bindable.angle,
            ),
            mid_point,
            SHRINK,
        );

        if cross(vector_from_point(point, top), vector_from_point(top, right)) <= 0.0
            && cross(vector_from_point(point, top), vector_from_point(top, left)) > 0.0
        {
            return heading_for_point(top, mid_point);
        }
        if cross(
            vector_from_point(point, right),
            vector_from_point(right, bottom),
        ) <= 0.0
            && cross(
                vector_from_point(point, right),
                vector_from_point(right, top),
            ) > 0.0
        {
            return heading_for_point(right, mid_point);
        }
        if cross(
            vector_from_point(point, bottom),
            vector_from_point(bottom, left),
        ) <= 0.0
            && cross(
                vector_from_point(point, bottom),
                vector_from_point(bottom, right),
            ) > 0.0
        {
            return heading_for_point(bottom, mid_point);
        }
        if cross(vector_from_point(point, left), vector_from_point(left, top)) <= 0.0
            && cross(
                vector_from_point(point, left),
                vector_from_point(left, bottom),
            ) > 0.0
        {
            return heading_for_point(left, mid_point);
        }

        if cross(
            vector_from_point(point, mid_point),
            vector_from_point(top, mid_point),
        ) <= 0.0
            && cross(
                vector_from_point(point, mid_point),
                vector_from_point(right, mid_point),
            ) > 0.0
        {
            let reference = if bindable.width > bindable.height {
                top
            } else {
                right
            };
            return heading_for_point(reference, mid_point);
        }
        if cross(
            vector_from_point(point, mid_point),
            vector_from_point(right, mid_point),
        ) <= 0.0
            && cross(
                vector_from_point(point, mid_point),
                vector_from_point(bottom, mid_point),
            ) > 0.0
        {
            let reference = if bindable.width > bindable.height {
                bottom
            } else {
                right
            };
            return heading_for_point(reference, mid_point);
        }
        if cross(
            vector_from_point(point, mid_point),
            vector_from_point(bottom, mid_point),
        ) <= 0.0
            && cross(
                vector_from_point(point, mid_point),
                vector_from_point(left, mid_point),
            ) > 0.0
        {
            let reference = if bindable.width > bindable.height {
                bottom
            } else {
                left
            };
            return heading_for_point(reference, mid_point);
        }

        let reference = if bindable.width > bindable.height {
            top
        } else {
            left
        };
        return heading_for_point(reference, mid_point);
    }

    let top_left = scale_from_origin([aabb[0], aabb[1]], mid_point, SEARCH_CONE_MULTIPLIER);
    let top_right = scale_from_origin([aabb[2], aabb[1]], mid_point, SEARCH_CONE_MULTIPLIER);
    let bottom_left = scale_from_origin([aabb[0], aabb[3]], mid_point, SEARCH_CONE_MULTIPLIER);
    let bottom_right = scale_from_origin([aabb[2], aabb[3]], mid_point, SEARCH_CONE_MULTIPLIER);

    let sign = |a: Point, b: Point, c: Point| -> f64 {
        (a[0] - c[0]) * (b[1] - c[1]) - (b[0] - c[0]) * (a[1] - c[1])
    };
    let point_in_triangle = |p: Point, a: Point, b: Point, c: Point| -> bool {
        let d1 = sign(p, a, b);
        let d2 = sign(p, b, c);
        let d3 = sign(p, c, a);
        let has_neg = d1 < 0.0 || d2 < 0.0 || d3 < 0.0;
        let has_pos = d1 > 0.0 || d2 > 0.0 || d3 > 0.0;
        !(has_neg && has_pos)
    };

    if point_in_triangle(point, top_left, top_right, mid_point) {
        return Heading::Up;
    }
    if point_in_triangle(point, top_right, bottom_right, mid_point) {
        return Heading::Right;
    }
    if point_in_triangle(point, bottom_right, bottom_left, mid_point) {
        return Heading::Down;
    }
    Heading::Left
}

pub fn points_equal(a: Point, b: Point, tolerance: f64) -> bool {
    (a[0] - b[0]).abs() <= tolerance && (a[1] - b[1]).abs() <= tolerance
}

pub fn is_orthogonal_path(points: &[Point], tolerance: f64) -> bool {
    for index in 1..points.len() {
        let previous = points[index - 1];
        let current = points[index];
        if (current[0] - previous[0]).abs() > tolerance
            && (current[1] - previous[1]).abs() > tolerance
        {
            return false;
        }
    }
    true
}

pub fn dedupe_collinear_points(points: &[Point]) -> Vec<Point> {
    if points.len() <= 2 {
        return points.to_vec();
    }
    let mut result = vec![points[0]];
    for index in 1..points.len() - 1 {
        let previous = *result.last().unwrap();
        let current = points[index];
        let next = points[index + 1];
        let horizontal_prev = (previous[1] - current[1]).abs() <= 1e-6;
        let horizontal_next = (current[1] - next[1]).abs() <= 1e-6;
        let vertical_prev = (previous[0] - current[0]).abs() <= 1e-6;
        let vertical_next = (current[0] - next[0]).abs() <= 1e-6;
        if (horizontal_prev && horizontal_next) || (vertical_prev && vertical_next) {
            continue;
        }
        result.push(current);
    }
    result.push(points[points.len() - 1]);
    result
}

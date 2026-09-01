use super::*;

pub(crate) fn aabb_for_bindable(bindable: &BindableState, offset: Option<[f64; 4]>) -> Bounds {
    let bounds = rotated_bindable_bounds(bindable, [0.0; 4]);
    if let Some(offset) = offset {
        inflate_bounds(bounds, offset)
    } else {
        bounds
    }
}

fn scale_from_origin(point: Point, origin: Point, factor: f64) -> Point {
    [
        origin[0] + (point[0] - origin[0]) * factor,
        origin[1] + (point[1] - origin[1]) * factor,
    ]
}

fn point_in_triangle(point: Point, a: Point, b: Point, c: Point) -> bool {
    let sign = |p1: Point, p2: Point, p3: Point| -> f64 {
        (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])
    };

    let d1 = sign(point, a, b);
    let d2 = sign(point, b, c);
    let d3 = sign(point, c, a);
    let has_negative = d1 < 0.0 || d2 < 0.0 || d3 < 0.0;
    let has_positive = d1 > 0.0 || d2 > 0.0 || d3 > 0.0;
    !(has_negative && has_positive)
}

pub(crate) fn heading_for_point_from_bindable(
    point: Point,
    bindable: &BindableState,
    aabb: Bounds,
) -> Heading {
    if bindable.shape == crate::BindableShape::Diamond {
        return heading_from_bindable(point, bindable);
    }

    let mid_point = [
        aabb[0] + (aabb[2] - aabb[0]) / 2.0,
        aabb[1] + (aabb[3] - aabb[1]) / 2.0,
    ];
    let search_cone_multiplier = 2.0;
    let top_left = scale_from_origin([aabb[0], aabb[1]], mid_point, search_cone_multiplier);
    let top_right = scale_from_origin([aabb[2], aabb[1]], mid_point, search_cone_multiplier);
    let bottom_left = scale_from_origin([aabb[0], aabb[3]], mid_point, search_cone_multiplier);
    let bottom_right = scale_from_origin([aabb[2], aabb[3]], mid_point, search_cone_multiplier);

    if point_in_triangle(point, top_left, top_right, mid_point) {
        Heading::Up
    } else if point_in_triangle(point, top_right, bottom_right, mid_point) {
        Heading::Right
    } else if point_in_triangle(point, bottom_right, bottom_left, mid_point) {
        Heading::Down
    } else {
        Heading::Left
    }
}

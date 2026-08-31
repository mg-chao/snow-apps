use super::*;

fn point_segment_distance(point: Point, a: Point, b: Point) -> f64 {
    let abx = b[0] - a[0];
    let aby = b[1] - a[1];
    let apx = point[0] - a[0];
    let apy = point[1] - a[1];
    let ab_len_sq = abx * abx + aby * aby;
    if ab_len_sq <= 1e-9 {
        return distance(point, a);
    }
    let t = clamp((apx * abx + apy * aby) / ab_len_sq, 0.0, 1.0);
    let projection = [a[0] + abx * t, a[1] + aby * t];
    distance(point, projection)
}

fn unrotate_to_local(point: Point, bindable: &BindableState) -> Point {
    let bindable_center = center(bindable.x, bindable.y, bindable.width, bindable.height);
    unrotate_point(point, bindable_center, bindable.angle)
}

pub(super) fn get_diamond_vertices(bindable: &BindableState) -> [Point; 4] {
    let top_x = bindable.width.floor() / 2.0 + 1.0;
    let right_y = bindable.height.floor() / 2.0 + 1.0;
    let top = [bindable.x + top_x, bindable.y];
    let right = [bindable.x + bindable.width, bindable.y + right_y];
    let bottom = [bindable.x + top_x, bindable.y + bindable.height];
    let left = [bindable.x, bindable.y + right_y];
    [top, right, bottom, left]
}

fn is_point_in_convex_polygon(point: Point, vertices: &[Point]) -> bool {
    let mut reference_sign = 0.0;
    for index in 0..vertices.len() {
        let from = vertices[index];
        let to = vertices[(index + 1) % vertices.len()];
        let cross =
            (to[0] - from[0]) * (point[1] - from[1]) - (to[1] - from[1]) * (point[0] - from[0]);
        if cross.abs() <= 1e-9 {
            continue;
        }
        let sign = cross.signum();
        if reference_sign == 0.0 {
            reference_sign = sign;
            continue;
        }
        if sign != reference_sign {
            return false;
        }
    }
    true
}

pub fn is_point_in_bindable(point: Point, bindable: &BindableState) -> bool {
    let shape = bindable.shape;
    let local = unrotate_to_local(point, bindable);
    let x = local[0];
    let y = local[1];

    if shape == BindableShape::Rectangle {
        return x >= bindable.x
            && y >= bindable.y
            && x <= bindable.x + bindable.width
            && y <= bindable.y + bindable.height;
    }

    if shape == BindableShape::Ellipse {
        let cx = bindable.x + bindable.width / 2.0;
        let cy = bindable.y + bindable.height / 2.0;
        let rx = (bindable.width / 2.0).max(1e-6);
        let ry = (bindable.height / 2.0).max(1e-6);
        let nx = (x - cx) / rx;
        let ny = (y - cy) / ry;
        return nx * nx + ny * ny <= 1.0;
    }

    is_point_in_convex_polygon(local, &get_diamond_vertices(bindable))
}

fn distance_to_rectangle_outline(point: Point, bindable: &BindableState) -> f64 {
    let local = unrotate_to_local(point, bindable);
    let min_x = bindable.x;
    let min_y = bindable.y;
    let max_x = bindable.x + bindable.width;
    let max_y = bindable.y + bindable.height;

    let inside = local[0] >= min_x && local[0] <= max_x && local[1] >= min_y && local[1] <= max_y;

    if inside {
        return [
            (local[0] - min_x).abs(),
            (max_x - local[0]).abs(),
            (local[1] - min_y).abs(),
            (max_y - local[1]).abs(),
        ]
        .into_iter()
        .fold(f64::INFINITY, f64::min);
    }

    let clamped_x = clamp(local[0], min_x, max_x);
    let clamped_y = clamp(local[1], min_y, max_y);
    distance(local, [clamped_x, clamped_y])
}

fn distance_to_ellipse_outline(point: Point, bindable: &BindableState) -> f64 {
    let local = unrotate_to_local(point, bindable);
    let cx = bindable.x + bindable.width / 2.0;
    let cy = bindable.y + bindable.height / 2.0;
    let rx = (bindable.width / 2.0).max(1e-6);
    let ry = (bindable.height / 2.0).max(1e-6);

    let dx = local[0] - cx;
    let dy = local[1] - cy;
    if dx.abs() < 1e-9 && dy.abs() < 1e-9 {
        return rx.min(ry);
    }

    let scale = 1.0 / ((dx * dx) / (rx * rx) + (dy * dy) / (ry * ry)).sqrt();
    let projection = [cx + dx * scale, cy + dy * scale];
    distance(local, projection)
}

fn distance_to_diamond_outline(point: Point, bindable: &BindableState) -> f64 {
    let local = unrotate_to_local(point, bindable);
    let [top, right, bottom, left] = get_diamond_vertices(bindable);
    [
        point_segment_distance(local, top, right),
        point_segment_distance(local, right, bottom),
        point_segment_distance(local, bottom, left),
        point_segment_distance(local, left, top),
    ]
    .into_iter()
    .fold(f64::INFINITY, f64::min)
}

pub fn distance_to_bindable_outline(point: Point, bindable: &BindableState) -> f64 {
    match bindable.shape {
        BindableShape::Rectangle => distance_to_rectangle_outline(point, bindable),
        BindableShape::Ellipse => distance_to_ellipse_outline(point, bindable),
        BindableShape::Diamond => distance_to_diamond_outline(point, bindable),
    }
}

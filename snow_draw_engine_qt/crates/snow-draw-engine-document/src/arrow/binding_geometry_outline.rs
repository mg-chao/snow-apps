use super::*;

#[derive(Clone, Copy, Debug, PartialEq)]
pub(super) struct LineSegment {
    pub(super) from: Point,
    pub(super) to: Point,
}

pub(super) fn normalize_vector(from: Point, to: Point) -> Option<Point> {
    let dx = to[0] - from[0];
    let dy = to[1] - from[1];
    let length = dx.hypot(dy);
    if length <= 1e-6 {
        None
    } else {
        Some([dx / length, dy / length])
    }
}

pub(super) fn point_from_vector(origin: Point, direction: Point, magnitude: f64) -> Point {
    [
        origin[0] + direction[0] * magnitude,
        origin[1] + direction[1] * magnitude,
    ]
}

pub(super) fn line_intersection(a1: Point, a2: Point, b1: Point, b2: Point) -> Option<Point> {
    let dxa = a2[0] - a1[0];
    let dya = a2[1] - a1[1];
    let dxb = b2[0] - b1[0];
    let dyb = b2[1] - b1[1];
    let denominator = dxa * dyb - dya * dxb;
    if denominator.abs() < 1e-9 {
        return None;
    }

    let dx = b1[0] - a1[0];
    let dy = b1[1] - a1[1];
    let t = (dx * dyb - dy * dxb) / denominator;
    let u = (dx * dya - dy * dxa) / denominator;
    if !(0.0..=1.0).contains(&t) || !(0.0..=1.0).contains(&u) {
        return None;
    }

    Some([a1[0] + t * dxa, a1[1] + t * dya])
}

fn dedupe_points(points: Vec<Point>) -> Vec<Point> {
    let mut unique = Vec::new();
    for point in points {
        if unique
            .iter()
            .any(|candidate| points_equal(*candidate, point, 1e-6))
        {
            continue;
        }
        unique.push(point);
    }
    unique
}

fn rectangle_intersections(
    from: Point,
    to: Point,
    bindable: &BindableState,
    gap: f64,
) -> Vec<Point> {
    let min_x = bindable.x - gap;
    let min_y = bindable.y - gap;
    let max_x = bindable.x + bindable.width + gap;
    let max_y = bindable.y + bindable.height + gap;
    let corners = [
        [min_x, min_y],
        [max_x, min_y],
        [max_x, max_y],
        [min_x, max_y],
    ];
    let edges = [
        LineSegment {
            from: corners[0],
            to: corners[1],
        },
        LineSegment {
            from: corners[1],
            to: corners[2],
        },
        LineSegment {
            from: corners[2],
            to: corners[3],
        },
        LineSegment {
            from: corners[3],
            to: corners[0],
        },
    ];

    dedupe_points(
        edges
            .iter()
            .filter_map(|edge| line_intersection(from, to, edge.from, edge.to))
            .collect(),
    )
}

fn ellipse_intersections(from: Point, to: Point, bindable: &BindableState, gap: f64) -> Vec<Point> {
    let cx = bindable.x + bindable.width / 2.0;
    let cy = bindable.y + bindable.height / 2.0;
    let rx = (bindable.width / 2.0 + gap).max(1e-6);
    let ry = (bindable.height / 2.0 + gap).max(1e-6);
    let dx = to[0] - from[0];
    let dy = to[1] - from[1];
    let px = from[0] - cx;
    let py = from[1] - cy;

    let a = (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry);
    let b = 2.0 * ((px * dx) / (rx * rx) + (py * dy) / (ry * ry));
    let c = (px * px) / (rx * rx) + (py * py) / (ry * ry) - 1.0;
    let discriminant = b * b - 4.0 * a * c;
    if discriminant < -1e-9 || a.abs() < 1e-9 {
        return Vec::new();
    }

    let sqrt_discriminant = discriminant.max(0.0).sqrt();
    let roots = [
        (-b - sqrt_discriminant) / (2.0 * a),
        (-b + sqrt_discriminant) / (2.0 * a),
    ];

    dedupe_points(
        roots
            .into_iter()
            .filter(|t| (0.0..=1.0).contains(t))
            .map(|t| [from[0] + dx * t, from[1] + dy * t])
            .collect(),
    )
}

fn diamond_vertices(bindable: &BindableState, gap: f64) -> [Point; 4] {
    let cx = bindable.x + bindable.width / 2.0;
    let cy = bindable.y + bindable.height / 2.0;
    [
        [cx, bindable.y - gap],
        [bindable.x + bindable.width + gap, cy],
        [cx, bindable.y + bindable.height + gap],
        [bindable.x - gap, cy],
    ]
}

fn diamond_intersections(from: Point, to: Point, bindable: &BindableState, gap: f64) -> Vec<Point> {
    let vertices = diamond_vertices(bindable, gap);
    let edges = [
        LineSegment {
            from: vertices[0],
            to: vertices[1],
        },
        LineSegment {
            from: vertices[1],
            to: vertices[2],
        },
        LineSegment {
            from: vertices[2],
            to: vertices[3],
        },
        LineSegment {
            from: vertices[3],
            to: vertices[0],
        },
    ];

    dedupe_points(
        edges
            .iter()
            .filter_map(|edge| line_intersection(from, to, edge.from, edge.to))
            .collect(),
    )
}

pub(super) fn intersect_outline(
    from: Point,
    to: Point,
    bindable: &BindableState,
    gap: f64,
) -> Vec<Point> {
    let bindable_center = center(bindable.x, bindable.y, bindable.width, bindable.height);
    let local_from = unrotate_point(from, bindable_center, bindable.angle);
    let local_to = unrotate_point(to, bindable_center, bindable.angle);
    let mut local_bindable = bindable.clone();
    local_bindable.angle = 0.0;

    let local_points = match local_bindable.shape {
        crate::BindableShape::Rectangle => {
            rectangle_intersections(local_from, local_to, &local_bindable, gap)
        }
        crate::BindableShape::Ellipse => {
            ellipse_intersections(local_from, local_to, &local_bindable, gap)
        }
        crate::BindableShape::Diamond => {
            diamond_intersections(local_from, local_to, &local_bindable, gap)
        }
    };

    local_points
        .into_iter()
        .map(|point| rotate_point(point, bindable_center, bindable.angle))
        .collect()
}

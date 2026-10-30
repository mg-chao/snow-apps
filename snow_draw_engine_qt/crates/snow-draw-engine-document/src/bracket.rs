//! Three-point curly braces. The endpoints define a local frame, not a painted baseline.
use crate::ArrowData;
use snow_draw_engine_core::{DrawRect, Point, arrow::ArrowPathCommand};

type Cubic = [Point<f64>; 4];

/// The two endpoints and middle tip in canvas coordinates, without stroke overflow.
pub fn bracket_snap_points(arrow: &ArrowData) -> Option<[Point<f64>; 3]> {
    if !arrow.is_bracket() || arrow.points.len() != 3 {
        return None;
    }
    let points =
        [0, 1, 2].map(|i| Point::new(arrow.x + arrow.points[i][0], arrow.y + arrow.points[i][1]));
    points
        .iter()
        .all(|p| p.x.is_finite() && p.y.is_finite())
        .then_some(points)
}

/// Four cubics, with continuous shoulder tangents and a rounded cusp at the apex.
/// Fixed-size geometry keeps pointer updates independent of document population.
pub fn bracket_segments(arrow: &ArrowData) -> Option<[Cubic; 4]> {
    let [a, c, b] = bracket_snap_points(arrow)?;
    let length = (b.x - a.x).hypot(b.y - a.y);
    if !length.is_finite() || length <= 1e-6 {
        return None;
    }
    let v = Point::new((b.x - a.x) / length, (b.y - a.y) / length);
    let n = Point::new(v.y, -v.x);
    let depth = (c.x - a.x) * n.x + (c.y - a.y) * n.y;
    let tip = (c.x - a.x) * v.x + (c.y - a.y) * v.y;
    let p = |x: f64, y: f64| Point::new(a.x + n.x * x + v.x * y, a.y + n.y * x + v.y * y);
    let spine = depth * 0.5;
    let upper = p(spine, tip * 0.5);
    let lower = p(spine, f64::midpoint(tip, length));
    let segments = [
        [a, p(spine, 0.0), p(spine, tip * 0.25), upper],
        [upper, p(spine, tip * 0.75), p(spine, tip), c],
        [
            c,
            p(spine, tip),
            p(spine, tip + (length - tip) * 0.25),
            lower,
        ],
        [
            lower,
            p(spine, tip + (length - tip) * 0.75),
            p(spine, length),
            b,
        ],
    ];
    segments
        .iter()
        .flatten()
        .all(|p| p.x.is_finite() && p.y.is_finite())
        .then_some(segments)
}

pub fn bracket_path_commands(arrow: &ArrowData) -> Vec<ArrowPathCommand> {
    let Some(segments) = bracket_segments(arrow) else {
        return Vec::new();
    };
    let xy = |p: Point<f64>| [p.x, p.y];
    let mut path = Vec::with_capacity(5);
    path.push(ArrowPathCommand::MoveTo {
        point: xy(segments[0][0]),
    });
    for [_, c1, c2, end] in segments {
        path.push(ArrowPathCommand::CubicTo {
            control_1: xy(c1),
            control_2: xy(c2),
            end: xy(end),
        });
    }
    path
}

pub fn bracket_bounds(arrow: &ArrowData) -> DrawRect {
    let Some(segments) = bracket_segments(arrow) else {
        return DrawRect::default();
    };
    // Bounds need only the cubic extrema, not path chunks, arc lengths or allocation.
    let bounds = segments.map(|[a, b, c, d]| snow_draw_engine_core::cubic_bounds(a, b, c, d));
    let r = arrow.stroke_width.max(0.0) * 0.5;
    DrawRect::new(
        bounds.iter().map(|b| b.min_x).fold(f64::INFINITY, f64::min) - r,
        bounds.iter().map(|b| b.min_y).fold(f64::INFINITY, f64::min) - r,
        bounds
            .iter()
            .map(|b| b.max_x)
            .fold(f64::NEG_INFINITY, f64::max)
            + r,
        bounds
            .iter()
            .map(|b| b.max_y)
            .fold(f64::NEG_INFINITY, f64::max)
            + r,
    )
}

pub fn bracket_segment_distance(p: Point<f64>, a: Point<f64>, b: Point<f64>) -> f64 {
    let dx = b.x - a.x;
    let dy = b.y - a.y;
    let t = if dx * dx + dy * dy > 0.0 {
        ((p.x - a.x) * dx + (p.y - a.y) * dy) / (dx * dx + dy * dy)
    } else {
        0.0
    }
    .clamp(0.0, 1.0);
    (p.x - a.x - t * dx).hypot(p.y - a.y - t * dy)
}

fn cubic_hit(points: Cubic, p: Point<f64>, tolerance: f64, level: u8) -> bool {
    let min_x = points.iter().map(|p| p.x).fold(f64::INFINITY, f64::min) - tolerance;
    let max_x = points.iter().map(|p| p.x).fold(f64::NEG_INFINITY, f64::max) + tolerance;
    let min_y = points.iter().map(|p| p.y).fold(f64::INFINITY, f64::min) - tolerance;
    let max_y = points.iter().map(|p| p.y).fold(f64::NEG_INFINITY, f64::max) + tolerance;
    if p.x < min_x || p.x > max_x || p.y < min_y || p.y > max_y {
        return false;
    }
    let [a, b, c, d] = points;
    let flatness = bracket_segment_distance(b, a, d).max(bracket_segment_distance(c, a, d));
    if flatness <= (tolerance * 0.1).max(1e-4) || level == 16 {
        return bracket_segment_distance(p, a, d) <= tolerance + flatness;
    }
    let mid =
        |a: Point<f64>, b: Point<f64>| Point::new(f64::midpoint(a.x, b.x), f64::midpoint(a.y, b.y));
    let ab = mid(a, b);
    let bc = mid(b, c);
    let cd = mid(c, d);
    let abc = mid(ab, bc);
    let bcd = mid(bc, cd);
    let center = mid(abc, bcd);
    cubic_hit([a, ab, abc, center], p, tolerance, level + 1)
        || cubic_hit([center, bcd, cd, d], p, tolerance, level + 1)
}

pub fn bracket_hit_test(arrow: &ArrowData, point: Point<f64>, tolerance: f64) -> bool {
    arrow.stroke.a != 0
        && arrow.opacity > 0.0
        && bracket_segments(arrow).is_some_and(|segments| {
            segments.into_iter().any(|segment| {
                cubic_hit(
                    segment,
                    point,
                    arrow.stroke_width * 0.5 + tolerance.max(0.0),
                    0,
                )
            })
        })
}

pub fn bracket_with_moved_points(
    arrow: &ArrowData,
    indices: &[usize],
    delta: Point<f64>,
) -> Option<ArrowData> {
    if delta.x == 0.0 && delta.y == 0.0 {
        return Some(arrow.clone());
    }
    let mut points = arrow.global_points();
    for &i in indices {
        let p = points.get_mut(i)?;
        p.x += delta.x;
        p.y += delta.y;
    }
    let normalized = ArrowData::from_global_points(
        &points,
        arrow.stroke,
        arrow.stroke_width,
        arrow.stroke_style,
        arrow.arrow_type,
        None,
        None,
    )?;
    let mut next = arrow.clone();
    next.x = normalized.x;
    next.y = normalized.y;
    next.width = normalized.width;
    next.height = normalized.height;
    next.points = normalized.points;
    crate::validate_arrow(&next).ok()?;
    Some(next)
}

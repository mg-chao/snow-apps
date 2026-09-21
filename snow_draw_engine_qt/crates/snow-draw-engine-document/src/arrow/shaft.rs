//! Render geometry for tapered arrows. Editing always retains the original centerline.
use crate::{ArrowData, arrowhead_render_primitives};
use snow_draw_engine_core::{
    PathCommand,
    arrow::{
        ArrowEndpointPosition, ArrowShaftType, Arrowhead, ArrowheadRenderPrimitive, StrokeStyle,
    },
};
type P = [f64; 2];

pub struct ArrowShaftGeometry {
    pub contours: Vec<Vec<P>>,
    pub hollow: bool,
    pub destination: ArrowEndpointPosition,
}
impl ArrowShaftGeometry {
    pub fn path_commands(&self) -> Vec<PathCommand> {
        let mut result = Vec::new();
        for contour in &self.contours {
            if let Some(&point) = contour.first() {
                result.push(PathCommand::MoveTo { point });
                result.extend(
                    contour
                        .iter()
                        .skip(1)
                        .map(|&point| PathCommand::LineTo { point }),
                );
                result.push(PathCommand::LineTo { point });
            }
        }
        result
    }
}
fn add(a: P, b: P) -> P {
    [a[0] + b[0], a[1] + b[1]]
}
fn sub(a: P, b: P) -> P {
    [a[0] - b[0], a[1] - b[1]]
}
fn mul(a: P, s: f64) -> P {
    [a[0] * s, a[1] * s]
}
fn length(a: P) -> f64 {
    a[0].hypot(a[1])
}
fn mix(a: P, b: P, t: f64) -> P {
    add(a, mul(sub(b, a), t))
}
fn unit(a: P) -> P {
    mul(a, 1.0 / length(a).max(1e-12))
}
fn normal(a: P) -> P {
    [-a[1], a[0]]
}
fn flatten_curve(out: &mut Vec<P>, a: P, b: P, c: P, d: P, depth: u32) {
    let chord = length(sub(d, a));
    if depth >= 12 || length(sub(b, a)) + length(sub(c, b)) + length(sub(d, c)) - chord < 0.02 {
        out.push(d);
        return;
    }
    let ab = mix(a, b, 0.5);
    let bc = mix(b, c, 0.5);
    let cd = mix(c, d, 0.5);
    let abc = mix(ab, bc, 0.5);
    let bcd = mix(bc, cd, 0.5);
    let mid = mix(abc, bcd, 0.5);
    flatten_curve(out, a, ab, abc, mid, depth + 1);
    flatten_curve(out, mid, bcd, cd, d, depth + 1);
}
fn flatten(commands: Vec<PathCommand>) -> Vec<P> {
    let mut out = Vec::new();
    let mut at = [0.0; 2];
    for cmd in commands {
        match cmd {
            PathCommand::MoveTo { point } | PathCommand::LineTo { point } => {
                out.push(point);
                at = point;
            }
            PathCommand::QuadTo { control, end } => {
                flatten_curve(
                    &mut out,
                    at,
                    mix(at, control, 2.0 / 3.0),
                    mix(end, control, 2.0 / 3.0),
                    end,
                    0,
                );
                at = end;
            }
            PathCommand::CubicTo {
                control_1,
                control_2,
                end,
            } => {
                flatten_curve(&mut out, at, control_1, control_2, end, 0);
                at = end;
            }
        }
    }
    out.dedup_by(|a, b| length(sub(*a, *b)) < 1e-6);
    out
}
fn at_distance(points: &[P], distances: &[f64], d: f64) -> P {
    let i = distances
        .partition_point(|v| *v < d)
        .clamp(1, points.len() - 1);
    mix(
        points[i - 1],
        points[i],
        ((d - distances[i - 1]) / (distances[i] - distances[i - 1])).clamp(0.0, 1.0),
    )
}
// Limited offset joins cannot produce the unbounded spikes of an acute miter.
fn ribbon(points: &[P], distances: &[f64], total: f64, width: f64, start: f64, end: f64) -> Vec<P> {
    let mut samples = vec![(at_distance(points, distances, start), start)];
    samples.extend(
        points
            .iter()
            .zip(distances)
            .filter(|(_, d)| **d > start && **d < end)
            .map(|(&p, &d)| (p, d)),
    );
    samples.push((at_distance(points, distances, end), end));
    let mut left = Vec::new();
    let mut right = Vec::new();
    for (i, &(p, d)) in samples.iter().enumerate() {
        let before = unit(sub(p, samples[i.saturating_sub(1)].0));
        let after = unit(sub(samples[(i + 1).min(samples.len() - 1)].0, p));
        let n = if i == 0 {
            normal(after)
        } else if i + 1 == samples.len() {
            normal(before)
        } else {
            let bisector = unit(add(normal(before), normal(after)));
            let dot = bisector[0] * normal(after)[0] + bisector[1] * normal(after)[1];
            mul(bisector, 1.0 / dot.max(0.5))
        };
        let radius = width * d / total;
        let turn = before[0] * after[1] - before[1] * after[0];
        if i > 0 && i + 1 < samples.len() && turn.abs() > 1e-6 {
            let from = normal(before);
            let angle = turn.atan2(before[0] * after[0] + before[1] * after[1]);
            let steps = (angle.abs() / 0.15).ceil() as usize;
            for step in 0..=steps {
                let t = angle * step as f64 / steps as f64;
                let arc = [
                    from[0] * t.cos() - from[1] * t.sin(),
                    from[0] * t.sin() + from[1] * t.cos(),
                ];
                // The outside follows a round join; the inside uses a bounded intersection.
                left.push(add(p, mul(if turn < 0.0 { arc } else { n }, radius)));
                right.push(sub(p, mul(if turn > 0.0 { arc } else { n }, radius)));
            }
        } else {
            let offset = mul(n, radius);
            left.push(add(p, offset));
            right.push(sub(p, offset));
        }
    }
    left.extend(right.into_iter().rev());
    left
}

pub fn tapered_arrow_geometry(arrow: &ArrowData) -> Option<ArrowShaftGeometry> {
    if arrow.arrow_shaft_type != ArrowShaftType::Tapered
        || arrow.is_line()
        || arrow.is_pen_highlight()
        || arrow.stroke_width <= 0.0
    {
        return None;
    }
    let reverse = arrow.end_arrowhead.is_none() && arrow.start_arrowhead.is_some();
    let head = if reverse {
        arrow.start_arrowhead?
    } else {
        arrow.end_arrowhead?
    };
    if !matches!(
        head,
        Arrowhead::Arrow
            | Arrowhead::Triangle
            | Arrowhead::TriangleOutline
            | Arrowhead::IndentedTriangle
    ) {
        return None;
    }
    let destination = if reverse {
        ArrowEndpointPosition::Start
    } else {
        ArrowEndpointPosition::End
    };
    let primitives = arrowhead_render_primitives(arrow, destination);
    let mut vertices = Vec::new();
    for primitive in primitives {
        match primitive {
            ArrowheadRenderPrimitive::Line(p) => {
                vertices.push(p.from);
                vertices.push(p.to);
            }
            ArrowheadRenderPrimitive::Polygon(p) => vertices.extend(p.points),
            _ => {}
        }
    }
    let mut points = flatten(arrow.path_commands());
    if reverse {
        points.reverse();
    }
    if points.len() < 2 || vertices.len() < 3 {
        return None;
    }
    let tip = *points.last()?;
    vertices.retain(|p| length(sub(*p, tip)) > 1e-6);
    // The two widest vertices are the arrowhead's shoulders; the indented head also has a notch.
    let mut shoulders = None;
    let mut span = 0.0;
    for &a in &vertices {
        for &b in &vertices {
            let size = length(sub(a, b));
            if size > span {
                span = size;
                shoulders = Some((a, b));
            }
        }
    }
    let (mut a, mut b) = shoulders?;
    let back = mix(a, b, 0.5);
    let direction = unit(sub(tip, back));
    if (a[0] - back[0]) * normal(direction)[0] + (a[1] - back[1]) * normal(direction)[1] < 0.0 {
        std::mem::swap(&mut a, &mut b);
    }
    let mut distances = vec![0.0];
    for pair in points.windows(2) {
        distances.push(distances.last()? + length(sub(pair[1], pair[0])));
    }
    let full = *distances.last()?;
    // Intersect the indented head's shoulder-to-notch edges at half the head width.
    let neck = if head == Arrowhead::IndentedTriangle {
        mix(back, tip, 0.125)
    } else {
        back
    };
    let head_length = length(sub(tip, neck)).min(full * 0.8);
    let total = full - head_length;
    if total < 1e-6 || span < 1e-6 {
        return None;
    }
    let cut = at_distance(&points, &distances, total);
    let keep = distances.partition_point(|d| *d < total);
    points.truncate(keep);
    distances.truncate(keep);
    points.push(cut);
    distances.push(total);
    // Match existing shoulders exactly, including curved-head orientation.
    *points.last_mut()? = neck;
    let width = span * 0.25;
    let hollow = head == Arrowhead::TriangleOutline;
    let mut contours = Vec::new();
    let head_contour = if head == Arrowhead::IndentedTriangle {
        vec![mix(back, tip, 0.25), a, tip, b]
    } else {
        vec![back, a, tip, b]
    };
    if hollow || arrow.stroke_style == StrokeStyle::Solid {
        let mut contour = ribbon(&points, &distances, total, width, 0.0, total);
        let half = contour.len() / 2;
        contour[half - 1] = add(neck, mul(normal(direction), width));
        contour[half] = sub(neck, mul(normal(direction), width));
        contour.splice(half..half, [a, tip, b]);
        contours.push(contour);
    } else {
        let stroke = arrow.stroke_width;
        let (dash, gap) = if arrow.stroke_style == StrokeStyle::Dashed {
            (4.0 * stroke, 2.0 * stroke)
        } else {
            (stroke, 2.0 * stroke)
        };
        let mut start = 0.0;
        // Match Qt's pattern cadence, bounded for extreme imported coordinates.
        for _ in 0..100_000 {
            if start >= total {
                break;
            }
            let end = (start + dash).min(total);
            if arrow.stroke_style == StrokeStyle::Dotted {
                let d = (start + end) * 0.5;
                let center = at_distance(&points, &distances, d);
                let radius = width * d / total;
                contours.push(
                    (0..20)
                        .map(|i| {
                            // Match the clockwise shaft/head winding so overlapping dots never cut holes.
                            let angle = -(i as f64) * std::f64::consts::TAU / 20.0;
                            add(center, [radius * angle.cos(), radius * angle.sin()])
                        })
                        .collect(),
                );
            } else {
                contours.push(ribbon(&points, &distances, total, width, start, end));
            }
            start += dash + gap;
        }
        contours.push(head_contour);
    }
    Some(ArrowShaftGeometry {
        contours,
        hollow,
        destination,
    })
}

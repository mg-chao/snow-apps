use crate::arrow_geom::{distance, rotate_point};
use crate::{
    ArrowEndpointPosition, ArrowPathCommand, StrokeStyle, Arrowhead, ArrowheadCirclePrimitive,
    ArrowheadDashMode, ArrowheadFillMode, ArrowheadLinePrimitive, ArrowheadPoints,
    ArrowheadPolygonPrimitive, ArrowheadPrimitiveKind, ArrowheadRenderPrimitive, CurvePathOp,
    Point,
};

#[derive(Clone, Debug, PartialEq)]
pub struct ArrowheadPointsInput {
    pub arrow_points: Vec<Point>,
    pub stroke_width: f64,
    pub curve_ops: Vec<CurvePathOp>,
    pub position: ArrowEndpointPosition,
    pub arrowhead: Arrowhead,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ArrowheadRenderPrimitivesInput {
    pub arrow_points: Vec<Point>,
    pub stroke_width: f64,
    pub curve_ops: Vec<CurvePathOp>,
    pub position: ArrowEndpointPosition,
    pub arrowhead: Arrowhead,
    pub stroke_style: StrokeStyle,
}

pub fn get_arrowhead_size(arrowhead: Arrowhead) -> f64 {
    match arrowhead {
        Arrowhead::Arrow => 25.0,
        Arrowhead::Diamond | Arrowhead::DiamondOutline | Arrowhead::Square => 12.0,
        Arrowhead::CrowfootMany | Arrowhead::CrowfootOne | Arrowhead::CrowfootOneOrMany => 20.0,
        _ => 15.0,
    }
}

pub fn get_arrowhead_angle(arrowhead: Arrowhead) -> f64 {
    match arrowhead {
        Arrowhead::Bar => 90.0,
        Arrowhead::Arrow => 20.0,
        _ => 25.0,
    }
}

fn to_radians(degrees: f64) -> f64 {
    (degrees * std::f64::consts::PI) / 180.0
}

fn is_curve_op(op: Option<&CurvePathOp>) -> bool {
    op.is_some_and(|op| !op.data.is_empty())
}

fn get_curve_point(op: &CurvePathOp, x_index: usize, y_index: usize) -> Point {
    [op.data[x_index], op.data[y_index]]
}

fn get_curve_start_point(previous_op: Option<&CurvePathOp>) -> Option<Point> {
    let previous_op = previous_op?;
    if previous_op.op == "move" && previous_op.data.len() >= 2 {
        return Some([previous_op.data[0], previous_op.data[1]]);
    }
    if previous_op.op == "bcurveTo" && previous_op.data.len() >= 6 {
        return Some([previous_op.data[4], previous_op.data[5]]);
    }
    Some([0.0, 0.0])
}

fn normalize_direction(from: Point, to: Point) -> Option<Point> {
    let dx = to[0] - from[0];
    let dy = to[1] - from[1];
    let length = dx.hypot(dy);
    if length <= 1e-6 {
        return None;
    }
    Some([dx / length, dy / length])
}

fn point_at_bezier(t: f64, p0: Point, p1: Point, p2: Point, p3: Point) -> Point {
    let x = (1.0 - t).powi(3) * p3[0]
        + 3.0 * t * (1.0 - t).powi(2) * p2[0]
        + 3.0 * t.powi(2) * (1.0 - t) * p1[0]
        + p0[0] * t.powi(3);
    let y = (1.0 - t).powi(3) * p3[1]
        + 3.0 * t * (1.0 - t).powi(2) * p2[1]
        + 3.0 * t.powi(2) * (1.0 - t) * p1[1]
        + p0[1] * t.powi(3);
    [x, y]
}

fn get_segment_length(arrow_points: &[Point], position: ArrowEndpointPosition) -> f64 {
    let current_point = match position {
        ArrowEndpointPosition::End => arrow_points.last().copied().unwrap_or([0.0, 0.0]),
        ArrowEndpointPosition::Start => arrow_points.first().copied().unwrap_or([0.0, 0.0]),
    };
    let previous_point = if arrow_points.len() > 1 {
        match position {
            ArrowEndpointPosition::End => arrow_points[arrow_points.len() - 2],
            ArrowEndpointPosition::Start => arrow_points[1],
        }
    } else {
        [0.0, 0.0]
    };
    distance(current_point, previous_point)
}

pub fn get_arrowhead_points(input: &ArrowheadPointsInput) -> Option<ArrowheadPoints> {
    let arrow_points = &input.arrow_points;
    let curve_ops = &input.curve_ops;
    if arrow_points.is_empty() || curve_ops.len() < 2 {
        return None;
    }

    let index = match input.position {
        ArrowEndpointPosition::Start => 1,
        ArrowEndpointPosition::End => curve_ops.len() - 1,
    };
    let op = curve_ops.get(index)?;
    if !is_curve_op(Some(op)) || op.data.len() != 6 {
        return None;
    }

    let previous_op = curve_ops.get(index.saturating_sub(1));
    let p0 = get_curve_start_point(previous_op)?;
    let p1 = get_curve_point(op, 0, 1);
    let p2 = get_curve_point(op, 2, 3);
    let p3 = get_curve_point(op, 4, 5);

    let endpoint = match input.position {
        ArrowEndpointPosition::Start => p0,
        ArrowEndpointPosition::End => p3,
    };
    let sample_point = point_at_bezier(0.3, p0, p1, p2, p3);
    let direction = normalize_direction(sample_point, endpoint)?;
    let size = get_arrowhead_size(input.arrowhead);
    let length = get_segment_length(arrow_points, input.position);
    let length_multiplier = if matches!(
        input.arrowhead,
        Arrowhead::Diamond | Arrowhead::DiamondOutline | Arrowhead::Square
    ) {
        0.25
    } else {
        0.5
    };
    let min_size = size.min(length * length_multiplier);
    let x2 = endpoint[0];
    let y2 = endpoint[1];
    let xs = x2 - direction[0] * min_size;
    let ys = y2 - direction[1] * min_size;

    if matches!(
        input.arrowhead,
        Arrowhead::Dot | Arrowhead::Circle | Arrowhead::CircleOutline
    ) {
        let diameter = (ys - y2).hypot(xs - x2) + input.stroke_width - 2.0;
        return Some(vec![x2, y2, diameter]);
    }

    let angle = get_arrowhead_angle(input.arrowhead);
    if matches!(
        input.arrowhead,
        Arrowhead::CrowfootMany | Arrowhead::CrowfootOneOrMany
    ) {
        let [x3, y3] = rotate_point([x2, y2], [xs, ys], to_radians(-angle));
        let [x4, y4] = rotate_point([x2, y2], [xs, ys], to_radians(angle));
        return Some(vec![xs, ys, x3, y3, x4, y4]);
    }

    let [x3, y3] = rotate_point([xs, ys], [x2, y2], to_radians(-angle));
    let [x4, y4] = rotate_point([xs, ys], [x2, y2], to_radians(angle));

    if matches!(
        input.arrowhead,
        Arrowhead::Diamond | Arrowhead::DiamondOutline | Arrowhead::Square
    ) {
        let previous_point = match input.position {
            ArrowEndpointPosition::Start => {
                if arrow_points.len() > 1 {
                    arrow_points[1]
                } else {
                    [0.0, 0.0]
                }
            }
            ArrowEndpointPosition::End => {
                if arrow_points.len() > 1 {
                    arrow_points[arrow_points.len() - 2]
                } else {
                    [0.0, 0.0]
                }
            }
        };
        let opposite_seed = match input.position {
            ArrowEndpointPosition::Start => [x2 + min_size * 2.0, y2],
            ArrowEndpointPosition::End => [x2 - min_size * 2.0, y2],
        };
        let opposite_angle = match input.position {
            ArrowEndpointPosition::Start => (previous_point[1] - y2).atan2(previous_point[0] - x2),
            ArrowEndpointPosition::End => (y2 - previous_point[1]).atan2(x2 - previous_point[0]),
        };
        let [ox, oy] = rotate_point(opposite_seed, [x2, y2], opposite_angle);

        return Some(vec![x2, y2, x3, y3, ox, oy, x4, y4]);
    }

    Some(vec![x2, y2, x3, y3, x4, y4])
}

fn to_point(x: f64, y: f64) -> Point {
    [x, y]
}

pub fn get_arrowhead_render_primitives(
    input: &ArrowheadRenderPrimitivesInput,
) -> Vec<ArrowheadRenderPrimitive> {
    let Some(points) = get_arrowhead_points(&ArrowheadPointsInput {
        arrow_points: input.arrow_points.clone(),
        stroke_width: input.stroke_width,
        curve_ops: input.curve_ops.clone(),
        position: input.position,
        arrowhead: input.arrowhead,
    }) else {
        return Vec::new();
    };

    let line =
        |from: Point, to: Point, dash_mode: ArrowheadDashMode, roughness_cap: Option<f64>| {
            ArrowheadRenderPrimitive::Line(ArrowheadLinePrimitive {
                kind: ArrowheadPrimitiveKind::Line,
                from,
                to,
                dash_mode,
                roughness_cap,
            })
        };

    let polygon = |polygon_points: Vec<Point>, fill_mode: ArrowheadFillMode, roughness_cap: f64| {
        ArrowheadRenderPrimitive::Polygon(ArrowheadPolygonPrimitive {
            kind: ArrowheadPrimitiveKind::Polygon,
            points: polygon_points,
            fill_mode,
            dash_mode: ArrowheadDashMode::Solid,
            roughness_cap: Some(roughness_cap),
        })
    };

    let circle =
        |center: Point, diameter: f64, fill_mode: ArrowheadFillMode, roughness_cap: f64| {
            ArrowheadRenderPrimitive::Circle(ArrowheadCirclePrimitive {
                kind: ArrowheadPrimitiveKind::Circle,
                center,
                diameter,
                fill_mode,
                dash_mode: ArrowheadDashMode::Solid,
                roughness_cap: Some(roughness_cap),
            })
        };

    match input.arrowhead {
        Arrowhead::Dot | Arrowhead::Circle => {
            vec![circle(
                to_point(points[0], points[1]),
                points[2],
                ArrowheadFillMode::Stroke,
                0.5,
            )]
        }
        Arrowhead::CircleOutline => vec![circle(
            to_point(points[0], points[1]),
            points[2],
            ArrowheadFillMode::Background,
            0.5,
        )],
        Arrowhead::Triangle | Arrowhead::TriangleOutline | Arrowhead::InvertedTriangle => {
            vec![polygon(
                vec![
                    to_point(points[0], points[1]),
                    to_point(points[2], points[3]),
                    to_point(points[4], points[5]),
                    to_point(points[0], points[1]),
                ],
                if input.arrowhead == Arrowhead::TriangleOutline {
                    ArrowheadFillMode::Background
                } else {
                    ArrowheadFillMode::Stroke
                },
                1.0,
            )]
        }
        Arrowhead::Diamond | Arrowhead::DiamondOutline | Arrowhead::Square => vec![polygon(
            vec![
                to_point(points[0], points[1]),
                to_point(points[2], points[3]),
                to_point(points[4], points[5]),
                to_point(points[6], points[7]),
                to_point(points[0], points[1]),
            ],
            if input.arrowhead == Arrowhead::DiamondOutline {
                ArrowheadFillMode::Background
            } else {
                ArrowheadFillMode::Stroke
            },
            1.0,
        )],
        Arrowhead::CrowfootOne => vec![line(
            to_point(points[2], points[3]),
            to_point(points[4], points[5]),
            ArrowheadDashMode::Inherit,
            None,
        )],
        _ => {
            let dash_mode = if input.stroke_style == StrokeStyle::Dotted {
                ArrowheadDashMode::DottedCap
            } else {
                ArrowheadDashMode::Solid
            };
            let mut primitives = vec![
                line(
                    to_point(points[2], points[3]),
                    to_point(points[0], points[1]),
                    dash_mode,
                    Some(1.0),
                ),
                line(
                    to_point(points[4], points[5]),
                    to_point(points[0], points[1]),
                    dash_mode,
                    Some(1.0),
                ),
            ];

            if input.arrowhead == Arrowhead::CrowfootOneOrMany
                && let Some(crowfoot_one_points) = get_arrowhead_points(&ArrowheadPointsInput {
                    arrow_points: input.arrow_points.clone(),
                    stroke_width: input.stroke_width,
                    curve_ops: input.curve_ops.clone(),
                    position: input.position,
                    arrowhead: Arrowhead::CrowfootOne,
                })
                && crowfoot_one_points.len() == 6
            {
                primitives.push(line(
                    to_point(crowfoot_one_points[2], crowfoot_one_points[3]),
                    to_point(crowfoot_one_points[4], crowfoot_one_points[5]),
                    dash_mode,
                    Some(1.0),
                ));
            }

            primitives
        }
    }
}

fn is_horizontal(a: Point, b: Point) -> bool {
    (a[0] - b[0]).abs() > (a[1] - b[1]).abs()
}

pub fn generate_elbow_arrow_path_commands(points: &[Point], radius: f64) -> Vec<ArrowPathCommand> {
    if points.is_empty() {
        return Vec::new();
    }
    if points.len() == 1 {
        return vec![ArrowPathCommand::MoveTo { point: points[0] }];
    }

    let mut subpoints = Vec::new();
    for index in 1..points.len() - 1 {
        let prev = points[index - 1];
        let next = points[index + 1];
        let point = points[index];
        let prev_is_horizontal = is_horizontal(point, prev);
        let next_is_horizontal = is_horizontal(next, point);
        let corner = radius
            .min(distance(points[index], next) / 2.0)
            .min(distance(points[index], prev) / 2.0);

        if prev_is_horizontal {
            subpoints.push([
                if prev[0] < point[0] {
                    point[0] - corner
                } else {
                    point[0] + corner
                },
                point[1],
            ]);
        } else {
            subpoints.push([
                point[0],
                if prev[1] < point[1] {
                    point[1] - corner
                } else {
                    point[1] + corner
                },
            ]);
        }

        subpoints.push(point);

        if next_is_horizontal {
            subpoints.push([
                if next[0] < point[0] {
                    point[0] - corner
                } else {
                    point[0] + corner
                },
                point[1],
            ]);
        } else {
            subpoints.push([
                point[0],
                if next[1] < point[1] {
                    point[1] - corner
                } else {
                    point[1] + corner
                },
            ]);
        }
    }

    let mut commands = vec![ArrowPathCommand::MoveTo { point: points[0] }];
    for index in (0..subpoints.len()).step_by(3) {
        commands.push(ArrowPathCommand::LineTo {
            point: subpoints[index],
        });
        commands.push(ArrowPathCommand::QuadTo {
            control: subpoints[index + 1],
            end: subpoints[index + 2],
        });
    }
    commands.push(ArrowPathCommand::LineTo {
        point: points[points.len() - 1],
    });
    commands
}

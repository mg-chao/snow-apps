use snow_draw_engine_core::arrow::{
    ArrowEndpointPosition, ArrowType, Arrowhead, ArrowheadRenderPrimitive, StrokeStyle,
};
use snow_draw_engine_core::{ColorRgba8, Point};
use snow_draw_engine_document::{ArrowData, arrowhead_render_primitives};

const STYLES: [Arrowhead; 15] = [
    Arrowhead::Arrow,
    Arrowhead::Bar,
    Arrowhead::Dot,
    Arrowhead::Circle,
    Arrowhead::CircleOutline,
    Arrowhead::IndentedTriangle,
    Arrowhead::Triangle,
    Arrowhead::TriangleOutline,
    Arrowhead::Diamond,
    Arrowhead::DiamondOutline,
    Arrowhead::CrowfootOne,
    Arrowhead::CrowfootMany,
    Arrowhead::CrowfootOneOrMany,
    Arrowhead::Square,
    Arrowhead::InvertedTriangle,
];

fn arrow(style: Arrowhead, width: f64, arrow_type: ArrowType) -> ArrowData {
    ArrowData::from_global_points(
        &[
            Point::new(0.0, 0.0),
            Point::new(1000.0, 0.0),
            Point::new(1000.0, 1000.0),
        ],
        ColorRgba8::default(),
        width,
        StrokeStyle::Solid,
        arrow_type,
        Some(style),
        Some(style),
    )
    .unwrap()
}

fn assert_point_scaled(base: [f64; 2], thick: [f64; 2], anchor: [f64; 2], scale: f64) {
    for axis in 0..2 {
        assert!((thick[axis] - anchor[axis] - (base[axis] - anchor[axis]) * scale).abs() < 1e-8);
    }
}

#[test]
fn arrowhead_and_tail_details_grow_sublinearly_with_stroke_width() {
    for style in STYLES {
        for arrow_type in [ArrowType::Straight, ArrowType::Curve, ArrowType::Elbow] {
            for (position, anchor) in [
                (ArrowEndpointPosition::Start, [0.0, 0.0]),
                (ArrowEndpointPosition::End, [1000.0, 1000.0]),
            ] {
                let base = arrowhead_render_primitives(&arrow(style, 2.0, arrow_type), position);
                assert!(!base.is_empty());
                for (width, scale) in [(2.0, 1.0), (8.0, 2.0), (18.0, 3.0), (32.0, 4.0)] {
                    let thick =
                        arrowhead_render_primitives(&arrow(style, width, arrow_type), position);
                    assert_eq!(base.len(), thick.len());
                    for (base, thick) in base.iter().zip(&thick) {
                        match (base, thick) {
                            (
                                ArrowheadRenderPrimitive::Line(a),
                                ArrowheadRenderPrimitive::Line(b),
                            ) => {
                                assert_point_scaled(a.from, b.from, anchor, scale);
                                assert_point_scaled(a.to, b.to, anchor, scale);
                            }
                            (
                                ArrowheadRenderPrimitive::Polygon(a),
                                ArrowheadRenderPrimitive::Polygon(b),
                            ) => {
                                assert_eq!(a.points.len(), b.points.len());
                                for (a, b) in a.points.iter().zip(&b.points) {
                                    assert_point_scaled(*a, *b, anchor, scale);
                                }
                            }
                            (
                                ArrowheadRenderPrimitive::Circle(a),
                                ArrowheadRenderPrimitive::Circle(b),
                            ) => {
                                assert_eq!(a.center, b.center);
                                assert!((b.diameter - a.diameter * scale).abs() < 1e-8);
                                // A centered outline leaves an inner diameter of diameter - width.
                                assert!(b.diameter > width);
                            }
                            _ => panic!("endpoint primitive kind changed for {style:?}"),
                        }
                    }
                }
            }
        }
    }
}

#[test]
fn curved_endpoint_styles_preserve_straight_endpoint_shapes() {
    // Pairwise distances are invariant under rotation: bending the shaft may
    // orient an endpoint differently, but must not deform any part of it.
    for style in STYLES {
        for position in [ArrowEndpointPosition::Start, ArrowEndpointPosition::End] {
            for width in [2.0, 8.0, 32.0] {
                let straight = arrowhead_render_primitives(
                    &arrow(style, width, ArrowType::Straight),
                    position,
                );
                let curved =
                    arrowhead_render_primitives(&arrow(style, width, ArrowType::Curve), position);
                assert_eq!(straight.len(), curved.len());
                let mut straight_points = Vec::new();
                let mut curved_points = Vec::new();
                for (straight, curved) in straight.iter().zip(&curved) {
                    match (straight, curved) {
                        (ArrowheadRenderPrimitive::Line(a), ArrowheadRenderPrimitive::Line(b)) => {
                            straight_points.extend([a.from, a.to]);
                            curved_points.extend([b.from, b.to]);
                        }
                        (
                            ArrowheadRenderPrimitive::Polygon(a),
                            ArrowheadRenderPrimitive::Polygon(b),
                        ) => {
                            assert_eq!(a.points.len(), b.points.len());
                            straight_points.extend_from_slice(&a.points);
                            curved_points.extend_from_slice(&b.points);
                        }
                        (
                            ArrowheadRenderPrimitive::Circle(a),
                            ArrowheadRenderPrimitive::Circle(b),
                        ) => {
                            assert_eq!(a.center, b.center);
                            assert!((a.diameter - b.diameter).abs() < 1e-8);
                        }
                        _ => panic!("endpoint primitive kind changed for {style:?}"),
                    }
                }
                for i in 0..straight_points.len() {
                    for j in i + 1..straight_points.len() {
                        let distance = |points: &[[f64; 2]]| {
                            (points[i][0] - points[j][0]).hypot(points[i][1] - points[j][1])
                        };
                        assert!(
                            (distance(&straight_points) - distance(&curved_points)).abs() < 1e-8,
                            "{style:?} deformed at {position:?}, width {width}, vertices {i}/{j}"
                        );
                    }
                }
            }
        }
    }
}

#[test]
fn short_arrow_endpoint_sizes_remain_bounded() {
    for style in STYLES {
        let mut short = arrow(style, 32.0, ArrowType::Straight);
        short.points = vec![[0.0, 0.0], [20.0, 0.0]];
        short.width = 20.0;
        short.height = 0.0;
        for position in [ArrowEndpointPosition::Start, ArrowEndpointPosition::End] {
            let anchor = if position == ArrowEndpointPosition::Start {
                [0.0, 0.0]
            } else {
                [20.0, 0.0]
            };
            for primitive in arrowhead_render_primitives(&short, position) {
                let points = match primitive {
                    ArrowheadRenderPrimitive::Line(line) => vec![line.from, line.to],
                    ArrowheadRenderPrimitive::Polygon(polygon) => polygon.points,
                    ArrowheadRenderPrimitive::Circle(circle) => {
                        assert!(circle.diameter <= 10.0 + 1e-8);
                        vec![circle.center]
                    }
                };
                for point in points {
                    assert!((point[0] - anchor[0]).hypot(point[1] - anchor[1]) <= 10.0 + 1e-8);
                }
            }
        }
    }
}

#[test]
fn indented_triangle_preserves_triangle_envelope_with_quarter_depth_notch() {
    use snow_draw_engine_core::arrow::ArrowheadFillMode;
    for arrow_type in [ArrowType::Straight, ArrowType::Curve, ArrowType::Elbow] {
        for position in [ArrowEndpointPosition::Start, ArrowEndpointPosition::End] {
            for width in [2.0, 8.0, 32.0] {
                let triangle = arrowhead_render_primitives(
                    &arrow(Arrowhead::Triangle, width, arrow_type),
                    position,
                );
                let indented = arrowhead_render_primitives(
                    &arrow(Arrowhead::IndentedTriangle, width, arrow_type),
                    position,
                );
                let [ArrowheadRenderPrimitive::Polygon(base)] = triangle.as_slice() else {
                    panic!("expected one triangle polygon");
                };
                let [ArrowheadRenderPrimitive::Polygon(notched)] = indented.as_slice() else {
                    panic!("expected one indented polygon");
                };
                assert_eq!(notched.fill_mode, ArrowheadFillMode::Stroke);
                assert_eq!(notched.points.len(), 5);
                assert_eq!(notched.points[0], base.points[0]);
                assert_eq!(notched.points[1], base.points[1]);
                assert_eq!(notched.points[3], base.points[2]);
                assert_eq!(notched.points[4], base.points[0]);
                for i in 0..4 {
                    for j in i + 1..4 {
                        assert_ne!(notched.points[i], notched.points[j]);
                    }
                }
                for axis in 0..2 {
                    let midpoint = (base.points[1][axis] + base.points[2][axis]) * 0.5;
                    let expected = midpoint + 0.25 * (base.points[0][axis] - midpoint);
                    assert!((notched.points[2][axis] - expected).abs() < 1e-8);
                }
            }
        }
    }
}

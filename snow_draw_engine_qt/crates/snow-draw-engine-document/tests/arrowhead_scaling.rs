use snow_draw_engine_core::arrow::{
    ArrowEndpointPosition, ArrowType, Arrowhead, ArrowheadRenderPrimitive, StrokeStyle,
};
use snow_draw_engine_core::{ColorRgba8, Point};
use snow_draw_engine_document::{ArrowData, arrowhead_render_primitives};

const STYLES: [Arrowhead; 14] = [
    Arrowhead::Arrow,
    Arrowhead::Bar,
    Arrowhead::Dot,
    Arrowhead::Circle,
    Arrowhead::CircleOutline,
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

use snow_draw_engine_core::{
    ColorRgba8, Point,
    arrow::{
        ArrowEndpointPosition, ArrowShaftType, ArrowType, Arrowhead, ArrowheadFillMode,
        ArrowheadRenderPrimitive, StrokeStyle,
    },
};
use snow_draw_engine_document::{ArrowData, arrow_bounds, arrow_hit_test, tapered_arrow_geometry};

fn arrow(head: Option<Arrowhead>) -> ArrowData {
    let mut arrow = ArrowData::from_global_points(
        &[Point::new(0.0, 0.0), Point::new(400.0, 0.0)],
        ColorRgba8 {
            r: 255,
            g: 0,
            b: 0,
            a: 255,
        },
        2.0,
        StrokeStyle::Solid,
        ArrowType::Straight,
        None,
        head,
    )
    .unwrap();
    arrow.arrow_shaft_type = ArrowShaftType::Tapered;
    arrow
}

fn distance(a: [f64; 2], b: [f64; 2]) -> f64 {
    (a[0] - b[0]).hypot(a[1] - b[1])
}

fn joined_shaft_width(
    geometry: &snow_draw_engine_document::ArrowShaftGeometry,
    tip: [f64; 2],
) -> f64 {
    let contour = &geometry.contours[0];
    let tip_index = contour
        .iter()
        .position(|point| distance(*point, tip) < 1e-6)
        .expect("joined arrowhead tip must be part of its shaft contour");
    distance(
        contour[(tip_index + contour.len() - 2) % contour.len()],
        contour[(tip_index + 2) % contour.len()],
    )
}

#[test]
fn tapered_supported_heads_and_fallback_preserve_preference() {
    for head in [
        Arrowhead::Arrow,
        Arrowhead::Triangle,
        Arrowhead::TriangleOutline,
        Arrowhead::IndentedTriangle,
    ] {
        let a = arrow(Some(head));
        let geometry = tapered_arrow_geometry(&a).unwrap();
        assert_eq!(geometry.hollow, head == Arrowhead::TriangleOutline);
        assert_eq!(geometry.destination, ArrowEndpointPosition::End);

        let mut double_headed = arrow(Some(Arrowhead::Triangle));
        double_headed.start_arrowhead = Some(head);
        let geometry = tapered_arrow_geometry(&double_headed).unwrap();
        if head == Arrowhead::TriangleOutline {
            let [ArrowheadRenderPrimitive::Polygon(start_head)] =
                geometry.arrowhead_primitives.as_slice()
            else {
                panic!("an outlined second head must retain its own paint primitive")
            };
            assert_eq!(start_head.fill_mode, ArrowheadFillMode::Background);
        } else {
            assert!(
                geometry.arrowhead_primitives.is_empty(),
                "filled second heads must be integrated into the widening contour"
            );
            assert!(geometry.contours[0].contains(&[0.0, 0.0]));
        }
    }
    for head in [
        None,
        Some(Arrowhead::Bar),
        Some(Arrowhead::Dot),
        Some(Arrowhead::Circle),
        Some(Arrowhead::CircleOutline),
        Some(Arrowhead::Diamond),
        Some(Arrowhead::DiamondOutline),
        Some(Arrowhead::CrowfootOne),
        Some(Arrowhead::CrowfootMany),
        Some(Arrowhead::CrowfootOneOrMany),
        Some(Arrowhead::Square),
        Some(Arrowhead::InvertedTriangle),
    ] {
        let mut a = arrow(head);
        assert!(tapered_arrow_geometry(&a).is_none());
        assert_eq!(a.arrow_shaft_type, ArrowShaftType::Tapered);
        a.end_arrowhead = Some(Arrowhead::Triangle);
        assert!(tapered_arrow_geometry(&a).is_some());
    }
}
#[test]
fn tapered_direction_uses_sole_head_or_end_head() {
    let mut a = arrow(None);
    a.start_arrowhead = Some(Arrowhead::Triangle);
    assert_eq!(
        tapered_arrow_geometry(&a).unwrap().destination,
        ArrowEndpointPosition::Start
    );
    a.end_arrowhead = Some(Arrowhead::TriangleOutline);
    assert_eq!(
        tapered_arrow_geometry(&a).unwrap().destination,
        ArrowEndpointPosition::End
    );
    a.end_arrowhead = Some(Arrowhead::Circle);
    assert!(tapered_arrow_geometry(&a).is_none());
    a.start_arrowhead = Some(Arrowhead::Circle);
    a.end_arrowhead = Some(Arrowhead::Triangle);
    assert!(
        tapered_arrow_geometry(&a).is_none(),
        "an unsupported start head must also make a double-headed arrow render plain"
    );
}

#[test]
fn double_headed_tapered_shaft_uses_each_single_head_width() {
    let mut start_only = arrow(None);
    start_only.start_arrowhead = Some(Arrowhead::Arrow);
    let start_geometry = tapered_arrow_geometry(&start_only).unwrap();
    let expected_start_width = joined_shaft_width(&start_geometry, [0.0, 0.0]);

    let end_only = arrow(Some(Arrowhead::Triangle));
    let end_geometry = tapered_arrow_geometry(&end_only).unwrap();
    let expected_end_width = joined_shaft_width(&end_geometry, [400.0, 0.0]);
    assert!(
        (expected_start_width - expected_end_width).abs() > 1e-6,
        "different arrowheads should exercise independent endpoint widths"
    );

    let mut double_headed = end_only;
    double_headed.start_arrowhead = Some(Arrowhead::Arrow);
    let geometry = tapered_arrow_geometry(&double_headed).unwrap();
    assert_eq!(geometry.destination, ArrowEndpointPosition::End);
    let actual_start_width = joined_shaft_width(&geometry, [0.0, 0.0]);
    let actual_end_width = joined_shaft_width(&geometry, [400.0, 0.0]);
    assert!((actual_start_width - expected_start_width).abs() < 1e-6);
    assert!((actual_end_width - expected_end_width).abs() < 1e-6);
    assert!(geometry.arrowhead_primitives.is_empty());
    assert!(geometry.contours[0].contains(&[0.0, 0.0]));
    assert!(
        arrow_hit_test(&double_headed, Point::new(20.0, 0.0), 0.0),
        "the filled interior of the second widening head must be hit-testable"
    );
}
#[test]
fn tapered_bounds_and_hit_testing_follow_visible_shape() {
    let mut a = arrow(Some(Arrowhead::Triangle));
    assert!(arrow_hit_test(&a, Point::new(350.0, 2.5), 0.0));
    assert!(!arrow_hit_test(&a, Point::new(30.0, 2.5), 0.0));
    a.end_arrowhead = Some(Arrowhead::TriangleOutline);
    assert!(!arrow_hit_test(&a, Point::new(350.0, 0.0), 0.0));
    let geometry = tapered_arrow_geometry(&a).unwrap();
    let bounds = arrow_bounds(&a);
    for p in geometry.contours.iter().flatten() {
        assert!(
            p[0] >= bounds.min_x
                && p[0] <= bounds.max_x
                && p[1] >= bounds.min_y
                && p[1] <= bounds.max_y
        );
    }
}
#[test]
fn tapered_paths_strokes_and_degenerate_inputs_are_finite() {
    for kind in [ArrowType::Straight, ArrowType::Curve, ArrowType::Elbow] {
        for width in [0.5, 2.0, 12.0, 50.0] {
            for style in [StrokeStyle::Solid, StrokeStyle::Dashed, StrokeStyle::Dotted] {
                for double_headed in [false, true] {
                    let mut a = arrow(Some(Arrowhead::Triangle));
                    a.start_arrowhead = double_headed.then_some(Arrowhead::Arrow);
                    a.arrow_type = kind;
                    a.stroke_width = width;
                    a.stroke_style = style;
                    a.points = vec![[0.0, 0.0], [100.0, 0.0], [100.0, 100.0], [250.0, 100.0]];
                    let g = tapered_arrow_geometry(&a).unwrap();
                    assert!(!g.path_commands().is_empty());
                    for p in g.contours.iter().flatten() {
                        assert!(p[0].is_finite() && p[1].is_finite());
                    }
                    if style != StrokeStyle::Solid {
                        assert!(g.contours.len() > 1);
                    }
                    a.points = vec![[0.0, 0.0], [0.0, 0.0]];
                    assert!(tapered_arrow_geometry(&a).is_none());
                    a.points = vec![[0.0, 0.0], [0.001, 0.0]];
                    if let Some(g) = tapered_arrow_geometry(&a) {
                        assert!(
                            g.contours
                                .iter()
                                .flatten()
                                .all(|p| p[0].is_finite() && p[1].is_finite())
                        );
                    }
                }
            }
        }
    }
}
#[test]
fn tapered_serialization_defaults_and_linear_tools() {
    let a = arrow(Some(Arrowhead::Triangle));
    let mut value = serde_json::to_value(&a).unwrap();
    assert_eq!(
        serde_json::from_value::<ArrowData>(value.clone()).unwrap(),
        a
    );
    value.as_object_mut().unwrap().remove("arrow_shaft_type");
    let old: ArrowData = serde_json::from_value(value).unwrap();
    assert_eq!(old.arrow_shaft_type, ArrowShaftType::Plain);
    assert!(
        tapered_arrow_geometry(
            &a.clone()
                .into_line(ColorRgba8::default(), Default::default())
        )
        .is_none()
    );
}

#[test]
fn tapered_dots_and_heads_have_consistent_fill_winding() {
    for (start, end) in [
        (None, Some(Arrowhead::Triangle)),
        (Some(Arrowhead::Triangle), None),
        (Some(Arrowhead::Arrow), Some(Arrowhead::Triangle)),
    ] {
        let mut a = arrow(end);
        a.start_arrowhead = start;
        a.stroke_style = StrokeStyle::Dotted;
        let g = tapered_arrow_geometry(&a).unwrap();
        for contour in g.contours {
            let area: f64 = contour
                .iter()
                .zip(contour.iter().cycle().skip(1))
                .take(contour.len())
                .map(|(a, b)| a[0] * b[1] - a[1] * b[0])
                .sum();
            assert!(
                area < 0.0,
                "all filled components need matching winding when they overlap"
            );
        }
    }
}

#[test]
fn arrow_ratio_tapered_geometry_bounds_and_hit_testing() {
    for head in [
        Arrowhead::Arrow,
        Arrowhead::Triangle,
        Arrowhead::TriangleOutline,
        Arrowhead::IndentedTriangle,
    ] {
        for arrow_type in [ArrowType::Straight, ArrowType::Curve, ArrowType::Elbow] {
            let mut a = arrow(Some(head));
            a.start_arrowhead = Some(head);
            a.arrow_type = arrow_type;
            for ratio in [1.0, 2.0, 3.0] {
                a.arrow_ratio = ratio;
                let geometry = tapered_arrow_geometry(&a).unwrap();
                assert_eq!(a.stroke_width, 2.0);
                let bounds = arrow_bounds(&a);
                for contour in &geometry.contours {
                    for p in contour {
                        assert!(p[0].is_finite() && p[1].is_finite());
                        assert!(
                            p[0] >= bounds.min_x
                                && p[0] <= bounds.max_x
                                && p[1] >= bounds.min_y
                                && p[1] <= bounds.max_y
                        );
                        assert!(arrow_hit_test(&a, Point::new(p[0], p[1]), 0.01));
                    }
                }
            }
        }
    }
}

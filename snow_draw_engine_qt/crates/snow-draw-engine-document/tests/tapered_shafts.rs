use snow_draw_engine_core::{
    ColorRgba8, Point,
    arrow::{ArrowEndpointPosition, ArrowShaftType, ArrowType, Arrowhead, StrokeStyle},
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
                let mut a = arrow(Some(Arrowhead::Triangle));
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
    let mut a = arrow(Some(Arrowhead::Triangle));
    a.stroke_style = StrokeStyle::Dotted;
    for reverse in [false, true] {
        if reverse {
            a.start_arrowhead = a.end_arrowhead.take();
        }
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

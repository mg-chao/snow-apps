use snow_draw_engine_core::{
    ColorRgba8, Point,
    arrow::{ArrowPathCommand, ArrowType, StrokeStyle},
};
use snow_draw_engine_document::*;
fn brace(points: [Point<f64>; 3]) -> ArrowData {
    let mut a = ArrowData::from_global_points(
        &points,
        ColorRgba8 {
            r: 245,
            g: 34,
            b: 45,
            a: 255,
        },
        2.0,
        StrokeStyle::Solid,
        ArrowType::Straight,
        None,
        None,
    )
    .unwrap();
    a.linear_kind = LinearElementKind::Bracket;
    a.fill = Default::default();
    a
}
fn point(x: f64, y: f64) -> Point<f64> {
    Point::new(x, y)
}
#[test]
fn bracket_snap_points_preserve_exact_canvas_controls_without_stroke_or_bounds() {
    for mirror in [-1.0, 1.0] {
        let points = [
            point(120. * mirror, 20.),
            point(40. * mirror, 90.),
            point(100. * mirror, 200.),
        ];
        let mut arrow = brace(points);
        arrow.stroke_width = 72.;
        assert_eq!(bracket_snap_points(&arrow), Some(points));
        arrow.linear_kind = LinearElementKind::Line;
        assert!(bracket_snap_points(&arrow).is_none());
        arrow.linear_kind = LinearElementKind::Bracket;
        arrow.points.pop();
        assert!(bracket_snap_points(&arrow).is_none());
    }
    let mut arrow = brace([point(0., 0.), point(80., 100.), point(0., 200.)]);
    arrow.x = f64::INFINITY;
    assert!(bracket_snap_points(&arrow).is_none());
}
#[test]
fn bracket_path_has_four_cubics_exact_controls_and_tight_bounds() {
    let a = brace([point(0., 0.), point(80., 100.), point(0., 200.)]);
    validate_arrow(&a).unwrap();
    assert!(a.is_linear_annotation());
    assert!(
        !a.is_generated_annotation(),
        "brackets have no generated label"
    );
    let commands = a.path_commands();
    assert_eq!(commands.len(), 5);
    assert!(
        commands[1..]
            .iter()
            .all(|c| matches!(c, ArrowPathCommand::CubicTo { .. }))
    );
    let bounds = arrow_bounds(&a);
    assert_eq!(bounds.min_x, -1.);
    assert_eq!(bounds.max_x, 81.);
    assert_eq!(bounds.min_y, -1.);
    assert_eq!(bounds.max_y, 201.);
    assert!(arrow_hit_test(&a, point(40., 50.), 0.1));
    assert!(
        !arrow_hit_test(&a, point(0., 100.), 2.),
        "the editing guide is not a painted stroke"
    );
    assert!(
        !arrow_hit_test(&a, point(20., 25.), 0.1),
        "do not hit the control polygon"
    );
}
#[test]
fn bracket_geometry_is_covariant_under_rotation_mirroring_and_extreme_apices() {
    for depth in [-80., 0., 0.001, 80., 10000.] {
        for height in [-100., 0., 100., 200., 350.] {
            for rotation in [
                0_f64,
                0.37,
                std::f64::consts::FRAC_PI_2,
                std::f64::consts::PI,
            ] {
                let transform = |p: Point<f64>| {
                    point(
                        30. + p.x * rotation.cos() - p.y * rotation.sin(),
                        -20. + p.x * rotation.sin() + p.y * rotation.cos(),
                    )
                };
                let a = brace([point(0., 0.), point(depth, height), point(0., 200.)]);
                let b = brace(
                    a.global_points()
                        .try_into()
                        .unwrap_or([point(0., 0.); 3])
                        .map(transform),
                );
                validate_arrow(&b).unwrap();
                let sa = bracket_segments(&a).unwrap();
                let sb = bracket_segments(&b).unwrap();
                for (left, right) in sa.into_iter().flatten().zip(sb.into_iter().flatten()) {
                    let expected = transform(left);
                    assert!(
                        (right.x - expected.x).abs() < 1e-8 && (right.y - expected.y).abs() < 1e-8
                    );
                }
                let bounds = bracket_bounds(&b);
                let state = ElementData::Arrow(b.clone()).state_rect();
                assert_eq!(state.min_x, bounds.min_x + 1.0);
                assert_eq!(state.max_y, bounds.max_y - 1.0);
                for segment in sb {
                    for step in 0..=100 {
                        let t = f64::from(step) / 100.;
                        let u = 1. - t;
                        let p = point(
                            u * u * u * segment[0].x
                                + 3. * u * u * t * segment[1].x
                                + 3. * u * t * t * segment[2].x
                                + t * t * t * segment[3].x,
                            u * u * u * segment[0].y
                                + 3. * u * u * t * segment[1].y
                                + 3. * u * t * t * segment[2].y
                                + t * t * t * segment[3].y,
                        );
                        assert!(
                            p.x >= bounds.min_x
                                && p.x <= bounds.max_x
                                && p.y >= bounds.min_y
                                && p.y <= bounds.max_y
                        );
                        assert!(bracket_hit_test(&b, p, 0.1));
                    }
                }
            }
        }
    }
}
#[test]
fn bracket_edits_preserve_unmoved_points_and_reject_coincident_endpoints() {
    let a = brace([point(10., 20.), point(80., 100.), point(10., 200.)]);
    let b = bracket_with_moved_points(&a, &[0, 2], point(20., 30.)).unwrap();
    assert_eq!(
        b.global_points(),
        vec![point(30., 50.), point(80., 100.), point(30., 230.)]
    );
    assert!(bracket_with_moved_points(&a, &[0], point(0., 180.)).is_none());
    assert_eq!(
        bracket_with_moved_points(&a, &[0], point(0., 0.)).unwrap(),
        a
    );
    for width in [0., 73., f64::NAN] {
        let mut b = a.clone();
        b.stroke_width = width;
        assert!(validate_arrow(&b).is_err());
    }
    let serialized = serde_json::to_string(&b).unwrap();
    assert_eq!(serde_json::from_str::<ArrowData>(&serialized).unwrap(), b);
}

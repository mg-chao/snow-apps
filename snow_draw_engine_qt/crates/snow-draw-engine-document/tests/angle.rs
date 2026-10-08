use std::f64::consts::{FRAC_PI_2, PI, TAU};

use snow_draw_engine_core::{
    ColorRgba8, Point,
    arrow::{ArrowPathCommand, ArrowShaftType, ArrowType, Arrowhead, StrokeStyle},
};
use snow_draw_engine_document::{
    AngleAnnotation, AngleUnit, ArrowData, Document, ElementData, ElementKind, ElementMeta,
    LinearElementKind, Operation, TextLayoutSize, Transaction, angle_geometry, angle_label,
    angle_label_text, angle_value, arrow_bounds, arrow_hit_test, arrow_segment_midpoints,
    generated_annotation_label, validate_angle_annotation, validate_arrow,
};

fn angle(degrees: f64) -> ArrowData {
    let radians = degrees.to_radians();
    let mut arrow = ArrowData::from_global_points(
        &[
            Point::new(100.0, 0.0),
            Point::new(0.0, 0.0),
            Point::new(100.0 * radians.cos(), -100.0 * radians.sin()),
        ],
        ColorRgba8 {
            r: 31,
            g: 122,
            b: 204,
            a: 255,
        },
        2.0,
        StrokeStyle::Solid,
        ArrowType::Straight,
        None,
        None,
    )
    .unwrap();
    arrow.linear_kind = LinearElementKind::Angle;
    arrow.angle = Some(AngleAnnotation {
        full_turn: degrees == 360.0,
        ..AngleAnnotation::default()
    });
    arrow
}

fn close(actual: f64, expected: f64) {
    assert!((actual - expected).abs() <= 1e-9, "{actual} != {expected}");
}

#[test]
fn angle_measures_visual_counterclockwise_sweeps_and_preserves_zero_full_turn_identity() {
    for (degrees, expected) in [
        (0.0, 0.0),
        (90.0, FRAC_PI_2),
        (180.0, PI),
        (270.0, PI * 1.5),
        (360.0, TAU),
    ] {
        let arrow = angle(degrees);
        close(angle_value(&arrow).unwrap(), expected);
        assert_eq!(arrow.element_kind(), ElementKind::Angle);
        assert!(validate_arrow(&arrow).is_ok());
    }
    let mut invalid = angle(90.0);
    invalid.angle.as_mut().unwrap().full_turn = true;
    assert!(angle_value(&invalid).is_none());
    assert!(validate_arrow(&invalid).is_err());
    // Unequal ray lengths still represent a complete turn when their directions coincide.
    let mut full_turn = angle(360.0);
    // The points are relative to A; rebuild to keep the two directions aligned.
    full_turn.points[2] = [-50.0, 0.0];
    close(angle_value(&full_turn).unwrap(), TAU);
}

#[test]
fn angle_formats_degrees_and_radians_at_every_supported_precision() {
    let mut arrow = angle(90.0);
    for (precision, degrees, radians) in [
        (0, "90°", "2 rad"),
        (1, "90.0°", "1.6 rad"),
        (2, "90.00°", "1.57 rad"),
        (3, "90.000°", "1.571 rad"),
    ] {
        arrow.angle.as_mut().unwrap().decimal_places = precision;
        arrow.angle.as_mut().unwrap().unit = AngleUnit::Degrees;
        assert_eq!(angle_label_text(&arrow).as_deref(), Some(degrees));
        arrow.angle.as_mut().unwrap().unit = AngleUnit::Radians;
        assert_eq!(angle_label_text(&arrow).as_deref(), Some(radians));
    }
    assert_eq!(angle_label_text(&angle(0.0)).as_deref(), Some("0°"));
    assert_eq!(angle_label_text(&angle(360.0)).as_deref(), Some("360°"));
    assert!(
        validate_angle_annotation(AngleAnnotation {
            decimal_places: 4,
            ..AngleAnnotation::default()
        })
        .is_err()
    );
}

#[test]
fn angle_requires_exactly_three_finite_points_and_two_nonzero_rays() {
    let arrow = angle(90.0);
    for points in [
        vec![[0.0, 0.0], [-100.0, 0.0]],
        vec![[0.0, 0.0], [-100.0, 0.0], [-100.0, -100.0], [5.0, 5.0]],
        vec![[0.0, 0.0], [0.0, 0.0], [-100.0, -100.0]],
        vec![[0.0, 0.0], [-100.0, 0.0], [-100.0, 0.0]],
        vec![[0.0, 0.0], [-100.0, f64::NAN], [-100.0, -100.0]],
    ] {
        let mut invalid = arrow.clone();
        invalid.points = points;
        assert!(validate_arrow(&invalid).is_err());
        assert!(angle_geometry(&invalid).is_none());
    }
    let mut ordinary = arrow.clone();
    ordinary.linear_kind = LinearElementKind::Arrow;
    assert!(validate_arrow(&ordinary).is_err());
    let mut missing = arrow;
    missing.angle = None;
    assert!(validate_arrow(&missing).is_err());
}

#[test]
fn angle_only_accepts_solid_supported_strokes_without_heads_fill_or_taper() {
    let arrow = angle(45.0);
    for width in [1.0, 2.0, 72.0] {
        let mut changed = arrow.clone();
        changed.stroke_width = width;
        assert!(validate_arrow(&changed).is_ok());
    }
    for width in [0.0, 0.5, 72.1, f64::INFINITY, f64::NAN] {
        let mut changed = arrow.clone();
        changed.stroke_width = width;
        assert!(validate_arrow(&changed).is_err());
    }
    for field in 0..4 {
        let mut changed = arrow.clone();
        match field {
            0 => changed.start_arrowhead = Some(Arrowhead::Arrow),
            1 => changed.stroke_style = StrokeStyle::Dashed,
            2 => changed.fill.a = 255,
            _ => changed.arrow_shaft_type = ArrowShaftType::Tapered,
        }
        assert!(validate_arrow(&changed).is_err());
    }
    assert!(arrow_segment_midpoints(&arrow).is_empty());
    assert!(
        !arrow
            .clone()
            .into_line(Default::default(), Default::default())
            .is_angle()
    );
    assert!(
        arrow
            .clone()
            .into_line(Default::default(), Default::default())
            .angle
            .is_none()
    );
    assert!(arrow.into_pen_highlight().angle.is_none());
}

#[test]
fn angle_renders_separate_counterclockwise_inner_arc_with_bounded_command_count() {
    for (degrees, count) in [
        (0.0, 0),
        (45.0, 1),
        (90.0, 1),
        (180.0, 2),
        (270.0, 3),
        (360.0, 4),
    ] {
        let arrow = angle(degrees);
        let commands = arrow.path_commands();
        assert_eq!(commands.len(), 3 + if count == 0 { 0 } else { 1 + count });
        assert!(matches!(commands[0], ArrowPathCommand::MoveTo { .. }));
        assert!(matches!(commands[1], ArrowPathCommand::LineTo { .. }));
        assert!(matches!(commands[2], ArrowPathCommand::LineTo { .. }));
        if count > 0 {
            assert!(matches!(commands[3], ArrowPathCommand::MoveTo { .. }));
            let ArrowPathCommand::CubicTo { control_1, .. } = commands[4] else {
                panic!("arc must be cubic")
            };
            assert!(
                control_1[1] < 0.0,
                "initial tangent must point visually counterclockwise"
            );
            let ArrowPathCommand::CubicTo { end, .. } = commands.last().unwrap() else {
                panic!("arc must end with a cubic")
            };
            let geometry = angle_geometry(&arrow).unwrap();
            close(
                end[0],
                geometry.vertex.x + geometry.radius * geometry.end_direction.x,
            );
            close(
                end[1],
                geometry.vertex.y + geometry.radius * geometry.end_direction.y,
            );
        }
    }
}

#[test]
fn angle_arc_extrema_extend_spatial_and_selection_bounds_and_support_precise_picking() {
    let reflex = angle(270.0);
    let radius = angle_geometry(&reflex).unwrap().radius;
    let bounds = arrow_bounds(&reflex);
    assert!(bounds.min_x < -radius);
    assert!(bounds.min_y < -radius);
    let selection_bounds = ElementData::Arrow(reflex.clone()).state_rect();
    assert!(selection_bounds.min_x <= -radius);
    assert!(selection_bounds.min_y <= -radius);
    let arc_middle = Point::new(-radius / 2.0_f64.sqrt(), -radius / 2.0_f64.sqrt());
    assert!(arrow_hit_test(&reflex, arc_middle, 0.0));
    assert!(!arrow_hit_test(
        &reflex,
        Point::new(0.0, 0.0 - radius * 0.5),
        0.0
    ));
    let acute = angle(45.0);
    assert!(!arrow_hit_test(&acute, Point::new(-radius, 0.0), 0.0));
    let full_turn = angle(360.0);
    assert!(arrow_hit_test(&full_turn, Point::new(-radius, 0.0), 0.0));
    assert!(!arrow_hit_test(&angle(0.0), Point::new(-radius, 0.0), 0.0));
}

#[test]
fn angle_measured_labels_clear_the_arc_and_remain_horizontal_on_the_sweep_bisector() {
    let mut arrow = angle(90.0);
    for width in [1.0, 2.0, 18.0, 72.0] {
        arrow.stroke_width = width;
        let layout = TextLayoutSize::new(80.0, 24.0);
        let label = angle_label(&arrow, Some(layout)).unwrap();
        close(label.font_size, 20.0 * (width / 2.0).sqrt());
        close(label.center.x, -label.center.y);
        let geometry = angle_geometry(&arrow).unwrap();
        let offset = label.center.x.hypot(label.center.y);
        close(
            offset,
            geometry.radius + 80.0_f64.hypot(24.0) / 2.0 + 6.0 + width / 2.0,
        );
        assert_eq!(label.layout, layout);
        assert_eq!(label.rotation, 0.0);
        assert_eq!(label.color, arrow.stroke);
        assert_eq!(
            generated_annotation_label(&arrow, Some(layout)),
            Some(label)
        );
    }
    let short = ArrowData::from_global_points(
        &[
            Point::new(1.0, 0.0),
            Point::new(0.0, 0.0),
            Point::new(0.0, -2.0),
        ],
        arrow.stroke,
        2.0,
        StrokeStyle::Solid,
        ArrowType::Straight,
        None,
        None,
    )
    .map(|mut short| {
        short.inherit_linear_metadata_from(&arrow);
        short.stroke_width = 2.0;
        short
    })
    .unwrap();
    close(angle_geometry(&short).unwrap().radius, 0.3);
    assert!(
        angle_label(&short, Some(TextLayoutSize::new(200.0, 24.0)))
            .unwrap()
            .center
            .x
            > 50.0
    );
}

#[test]
fn angle_transforms_measure_rendered_rays_and_preserve_metadata() {
    let original = angle(60.0);
    for (sx, sy, expected) in [
        (1.0, 1.0, 60.0),
        (-1.0, 1.0, 300.0),
        (1.0, -1.0, 300.0),
        (-1.0, -1.0, 60.0),
    ] {
        let points: Vec<_> = original
            .global_points()
            .into_iter()
            .map(|p| Point::new(p.x * sx + 42.0, p.y * sy - 36.0))
            .collect();
        let mut transformed = ArrowData::from_global_points(
            &points,
            original.stroke,
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        transformed.inherit_linear_metadata_from(&original);
        close(angle_value(&transformed).unwrap().to_degrees(), expected);
        assert_eq!(transformed.angle, original.angle);
        assert!(validate_arrow(&transformed).is_ok());
    }
    let full_turn = angle(360.0);
    let mut transformed = ArrowData::from_global_points(
        &full_turn
            .global_points()
            .into_iter()
            .map(|p| Point::new(-p.x * 2.0, p.y * 3.0))
            .collect::<Vec<_>>(),
        full_turn.stroke,
        2.0,
        StrokeStyle::Solid,
        ArrowType::Straight,
        None,
        None,
    )
    .unwrap();
    transformed.inherit_linear_metadata_from(&full_turn);
    close(angle_value(&transformed).unwrap(), TAU);
}

#[test]
fn angle_document_synchronizes_owned_label_layout_style_content_and_history() {
    let mut document = Document::new();
    let owner_id = document.allocate_element_id();
    let text_id = document.allocate_element_id();
    let mut owner = angle(270.0);
    owner.text_element_id = Some(text_id);
    let measured = TextLayoutSize::new(72.0, 26.0);
    let mut text = angle_label(&owner, Some(measured)).unwrap();
    text.center = Point::new(999.0, 999.0);
    text.text = "stale".to_owned();
    let mut creation = Transaction::new("create angle");
    creation.insert_arrow(owner_id, ElementMeta::default(), owner.clone());
    creation.insert_text(text_id, ElementMeta::default(), text);
    let created = document.apply(&creation).unwrap();
    assert_eq!(
        document.text(text_id).unwrap(),
        &angle_label(&owner, Some(measured)).unwrap()
    );
    assert!(document.validate_session().is_ok());
    let saved = serde_json::to_string(&document).unwrap();
    let restored: Document = serde_json::from_str(&saved).unwrap();
    assert!(restored.validate_session().is_ok());
    let restored_owner = restored.arrow(owner_id).unwrap();
    assert_eq!(restored_owner.angle, owner.angle);
    close(
        angle_value(restored_owner).unwrap(),
        angle_value(&owner).unwrap(),
    );
    assert_eq!(
        restored.text(text_id).unwrap(),
        document.text(text_id).unwrap()
    );
    let mut update = Transaction::new("style angle");
    owner.angle.as_mut().unwrap().unit = AngleUnit::Radians;
    owner.angle.as_mut().unwrap().decimal_places = 3;
    owner.stroke_width = 18.0;
    update.push(Operation::UpdateElementData {
        id: owner_id,
        data: ElementData::Arrow(owner.clone()),
    });
    let updated = document.apply(&update).unwrap();
    assert_eq!(
        document.text(text_id).unwrap(),
        &angle_label(&owner, Some(measured)).unwrap()
    );
    document.apply(&updated.inverse).unwrap();
    assert_eq!(document.text(text_id).unwrap().text, "270°");
    document.apply(&created.inverse).unwrap();
    assert!(document.element(owner_id).is_err());
    assert!(document.element(text_id).is_err());
}

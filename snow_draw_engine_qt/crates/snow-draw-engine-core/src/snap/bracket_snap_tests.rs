use super::*;

fn request(target: DrawRect) -> ObjectSnapRectRequest<'static> {
    ObjectSnapRectRequest {
        target_rect: target,
        reference_rects: &[],
        snap_distance: 4.0,
        target_anchors_x: &ObjectSnapService::ALL_ANCHORS,
        target_anchors_y: &ObjectSnapService::ALL_ANCHORS,
        enable_point_snaps: true,
        enable_gap_snaps: true,
    }
}

#[test]
fn control_points_align_edges_and_centers_on_independent_axes() {
    let points = [
        Point::new(100., 200.),
        Point::new(180., 290.),
        Point::new(120., 440.),
    ];
    for point in points {
        for anchor in ObjectSnapService::ALL_ANCHORS {
            let offset = match anchor {
                SnapAxisAnchor::Start => 0.,
                SnapAxisAnchor::Center => 20.,
                SnapAxisAnchor::End => 40.,
            };
            let x = point.x - offset - 3.;
            let y = point.y - offset - 3.;
            let x_snap = OBJECT_SNAP_SERVICE.snap_rect_with_reference_points(
                request(DrawRect::new(x, 2000., x + 40., 2040.)),
                &[point],
            );
            assert_eq!((x_snap.dx, x_snap.dy), (3., 0.));
            assert_eq!(x_snap.guides.len(), 1);
            assert!(x_snap.guides[0].markers.contains(&point));
            let y_snap = OBJECT_SNAP_SERVICE.snap_rect_with_reference_points(
                request(DrawRect::new(2000., y, 2040., y + 40.)),
                &[point],
            );
            assert_eq!((y_snap.dx, y_snap.dy), (0., 3.));
            assert_eq!(y_snap.guides.len(), 1);
            assert!(y_snap.guides[0].markers.contains(&point));
        }
    }
}

#[test]
fn separate_points_can_supply_the_two_axes_and_markers_use_final_geometry() {
    let points = [
        Point::new(100., 200.),
        Point::new(180., 290.),
        Point::new(120., 440.),
    ];
    let mut req = request(DrawRect::new(97., 437., 137., 477.));
    req.target_anchors_x = &[SnapAxisAnchor::Start];
    req.target_anchors_y = &[SnapAxisAnchor::Start];
    let snap = OBJECT_SNAP_SERVICE.snap_rect_with_reference_points(req, &points);
    assert_eq!((snap.dx, snap.dy), (3., 3.));
    assert_eq!(snap.guides.len(), 2);
    for guide in &snap.guides {
        assert_eq!(guide.kind, SnapGuideKind::Point);
        assert!(
            guide
                .markers
                .iter()
                .all(|p| if guide.axis == SnapGuideAxis::Vertical {
                    p.x == 100.
                } else {
                    p.y == 440.
                })
        );
    }
    assert!(snap.guides.iter().any(|g| g.markers.contains(&points[0])));
    assert!(snap.guides.iter().any(|g| g.markers.contains(&points[2])));
}

#[test]
fn points_never_supply_gap_candidates_or_intermediate_coordinates() {
    let points = [
        Point::new(100., 200.),
        Point::new(180., 290.),
        Point::new(120., 440.),
    ];
    let mut req = request(DrawRect::new(97., 197., 137., 237.));
    req.enable_point_snaps = false;
    assert_eq!(
        OBJECT_SNAP_SERVICE.snap_rect_with_reference_points(req, &points),
        ObjectSnapResult::default()
    );
    req.enable_point_snaps = true;
    req.target_rect = DrawRect::new(140., 2000., 150., 2010.);
    assert_eq!(
        OBJECT_SNAP_SERVICE.snap_rect_with_reference_points(req, &points),
        ObjectSnapResult::default()
    );
    req.target_rect = DrawRect::new(97., 197., 137., 237.);
    let snap = OBJECT_SNAP_SERVICE.snap_rect_with_reference_points(req, &points);
    assert!(
        snap.guides
            .iter()
            .all(|g| g.kind == SnapGuideKind::Point && g.label.is_none())
    );
}

#[test]
fn point_references_compete_with_rectangles_and_duplicate_points_are_stable() {
    let references = [DrawRect::new(98., 2000., 138., 2040.)];
    let mut req = request(DrawRect::new(100., 2000., 140., 2040.));
    req.reference_rects = &references;
    req.target_anchors_x = &[SnapAxisAnchor::Start];
    req.target_anchors_y = &[];
    let point = Point::new(100., 2020.);
    let snap = OBJECT_SNAP_SERVICE.snap_rect_with_reference_points(req, &[point]);
    assert_eq!(snap.dx, 0.);
    assert!(snap.guides[0].markers.contains(&point));
    assert_eq!(
        snap,
        OBJECT_SNAP_SERVICE.snap_rect_with_reference_points(req, &[point, point])
    );
    assert_eq!(
        OBJECT_SNAP_SERVICE.snap_rect(req),
        OBJECT_SNAP_SERVICE.snap_rect_with_reference_points(req, &[])
    );
    assert_eq!(
        OBJECT_SNAP_SERVICE.snap_rect(req),
        OBJECT_SNAP_SERVICE.snap_rect_with_reference_points(req, &[Point::new(f64::NAN, 2000.)])
    );
}

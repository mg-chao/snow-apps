use super::*;
use crate::DocumentModel;
use snow_draw_engine_core::{
    ColorRgba8,
    arrow::{ArrowType, StrokeStyle},
};
use snow_draw_engine_document::{ArrowData, ElementMeta, LinearElementKind, Transaction};

fn fixture(mirror: f64) -> (DocumentModel, ElementId, ArrowData) {
    let mut model = DocumentModel::new();
    let id = model.allocate_element_id();
    let mut arrow = ArrowData::from_global_points(
        &[
            Point::new(100. * mirror, 200.),
            Point::new(180. * mirror, 290.),
            Point::new(120. * mirror, 440.),
        ],
        ColorRgba8 {
            r: 255,
            g: 0,
            b: 0,
            a: 255,
        },
        2.,
        StrokeStyle::Solid,
        ArrowType::Straight,
        None,
        None,
    )
    .unwrap();
    arrow.linear_kind = LinearElementKind::Bracket;
    let mut insert = Transaction::new("insert bracket reference");
    insert.insert_arrow(id, ElementMeta::default(), arrow.clone());
    model.apply_transaction(insert).unwrap();
    (model, id, arrow)
}

fn query(x: f64, y: f64) -> SnapQuery {
    SnapQuery {
        point: Point::new(x, y),
        threshold: 2.,
        include_grid: false,
        grid_size: 20.,
    }
}

#[test]
fn all_three_controls_snap_independent_axes_outside_the_bracket_spatial_cells() {
    for mirror in [-1., 1.] {
        let (model, _, arrow) = fixture(mirror);
        for point in bracket_snap_points(&arrow).unwrap() {
            let x_snap = model.snap_point(&query(point.x + 1., 2000.));
            assert_eq!(x_snap.point, Point::new(point.x, 2000.));
            assert_eq!(x_snap.guides.len(), 1);
            assert!(x_snap.guides[0].markers.contains(&point));
            let y_snap = model.snap_point(&query(2000., point.y + 1.));
            assert_eq!(y_snap.point, Point::new(2000., point.y));
            assert!(y_snap.guides[0].markers.contains(&point));
        }
        let midpoint = query(140. * mirror, 2000.);
        assert_eq!(model.snap_point(&midpoint).point, midpoint.point);
        let mut stroke_edge = query(99. * mirror, 2000.);
        stroke_edge.threshold = 0.25;
        assert!(model.snap_point(&stroke_edge).guides.is_empty());
    }
}

#[test]
fn eligibility_filter_sees_a_proxy_without_adding_its_center_as_a_target() {
    let (model, id, _) = fixture(1.);
    let source = query(101., 2000.);
    let mut visited = false;
    let excluded = model.snap_point_filtered(&source, |candidate, proxy| {
        assert_eq!(candidate, id);
        assert_eq!(proxy.center, Point::new(140., 320.));
        assert_eq!((proxy.width, proxy.height), (80., 240.));
        visited = true;
        false
    });
    assert!(visited);
    assert_eq!(excluded.point, source.point);
    assert!(excluded.guides.is_empty());
    assert_eq!(
        model.snap_point_filtered(&source, |_, _| true),
        model.snap_point(&source)
    );
}

#[test]
fn targets_refresh_after_edit_visibility_removal_undo_redo_and_rebuild() {
    let (mut model, id, mut arrow) = fixture(1.);
    let old = query(101., 2000.);
    let next = query(501., 2000.);
    arrow.x += 400.;
    let mut update = Transaction::new("move bracket");
    update.update_arrow(id, arrow);
    let moved = model.apply_transaction(update).unwrap();
    assert_eq!(model.snap_point(&old).point, old.point);
    assert_eq!(model.snap_point(&next).point.x, 500.);
    let undone = model.apply_history_transaction(&moved.inverse).unwrap();
    assert_eq!(model.snap_point(&old).point.x, 100.);
    assert_eq!(model.snap_point(&next).point, next.point);
    model.apply_history_transaction(&undone.inverse).unwrap();
    assert_eq!(model.snap_point(&next).point.x, 500.);
    model = DocumentModel::from_document(model.document().clone()).unwrap();
    assert_eq!(model.snap_point(&next).point.x, 500.);

    let mut hide = Transaction::new("hide bracket");
    hide.update_element_meta(
        id,
        ElementMeta {
            visible: false,
            locked: false,
        },
    );
    let hidden = model.apply_transaction(hide).unwrap();
    assert!(model.snap_point(&next).guides.is_empty());
    let rebuilt = DocumentModel::from_document(model.document().clone()).unwrap();
    assert!(rebuilt.snap_point(&next).guides.is_empty());
    model.apply_history_transaction(&hidden.inverse).unwrap();
    assert_eq!(model.snap_point(&next).point.x, 500.);

    let mut remove = Transaction::new("remove bracket");
    remove.remove_element(id);
    let removed = model.apply_transaction(remove).unwrap();
    assert!(model.snap_point(&next).guides.is_empty());
    let restored = model.apply_history_transaction(&removed.inverse).unwrap();
    assert_eq!(model.snap_point(&next).point.x, 500.);
    model.apply_history_transaction(&restored.inverse).unwrap();
    assert!(model.snap_point(&next).guides.is_empty());
}

#[test]
fn duplicate_coordinates_and_ties_produce_deterministic_guides() {
    let (mut model, id, mut arrow) = fixture(1.);
    arrow.points[2][0] = arrow.points[0][0];
    let mut update = Transaction::new("align endpoints");
    update.update_arrow(id, arrow);
    model.apply_transaction(update).unwrap();
    let source = query(101., 2000.);
    let first = model.snap_point(&source);
    let rebuilt = DocumentModel::from_document(model.document().clone()).unwrap();
    assert_eq!(first, rebuilt.snap_point(&source));
    assert_eq!(first.guides.len(), 1);
    assert!(first.guides[0].markers.contains(&Point::new(100., 200.)));
    let mut tie = query(140., 2000.);
    tie.threshold = 40.;
    assert_eq!(model.snap_point(&tie).point.x, 100.);
}

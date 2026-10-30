use super::*;
use snow_draw_engine_document::{ElementMeta, LinearElementKind};

fn fixture(
    mirror: f64,
) -> (
    Editor,
    DocumentModel,
    ElementId,
    [Point<f64>; 3],
    SelectionRectState,
    SelectionBounds,
) {
    let mut editor = Editor::new(EngineConfig::default()).unwrap();
    editor.config.snap.enabled = true;
    let points = [
        Point::new(100. * mirror, 200.),
        Point::new(300. * mirror, 330.),
        Point::new(160. * mirror, 500.),
    ];
    let mut arrow = ArrowData::from_global_points(
        &points,
        editor.state.default_bracket_style.stroke,
        2.,
        snow_draw_engine_core::arrow::StrokeStyle::Solid,
        snow_draw_engine_core::arrow::ArrowType::Straight,
        None,
        None,
    )
    .unwrap();
    arrow.linear_kind = LinearElementKind::Bracket;
    let mut document = DocumentModel::new();
    let bracket = document.allocate_element_id();
    let selected = document.allocate_element_id();
    let rect = preview_rectangle(
        Point::new(600., 700.),
        Point::new(640., 740.),
        editor.state.default_rectangle_shape_style,
        false,
        false,
    )
    .unwrap();
    let mut insert = Transaction::new("bracket snap fixture");
    insert.insert_arrow(bracket, ElementMeta::default(), arrow);
    insert.insert_rectangle(selected, ElementMeta::default(), rect);
    document.apply_transaction(insert).unwrap();
    let bounds = SelectionBounds {
        center: rect.center,
        width: rect.width,
        height: rect.height,
        rotation: 0.,
    };
    (
        editor,
        document,
        bracket,
        points,
        SelectionRectState { id: selected, rect },
        bounds,
    )
}

#[test]
fn creation_move_and_resize_align_to_each_control_of_skewed_and_mirrored_brackets() {
    for mirror in [-1., 1.] {
        let (editor, document, _, points, selected, bounds) = fixture(mirror);
        let elements = [selected];
        for point in points {
            let raw = Point::new(point.x + 3., point.y + 3.);
            let (created, guides) = editor.preview_rectangle_with_snapping(
                &document,
                Point::new(point.x - 100., point.y - 100.),
                raw,
                Modifiers::default(),
            );
            let rect = rectangle_to_draw_rect(&created.unwrap());
            assert_eq!((rect.max_x, rect.max_y), (point.x, point.y));
            assert!(guides.iter().any(|g| g.markers.contains(&point)));

            let moved = editor.resolve_move_snap(MoveSnapRequest {
                document: &document,
                original_bounds: &bounds,
                original_elements: &elements,
                original_arrows: &[],
                base_dx: raw.x - 600.,
                base_dy: raw.y - 700.,
                modifiers: Modifiers::default(),
            });
            assert_eq!((moved.dx, moved.dy), (point.x - 600., point.y - 700.));
            assert!(moved.guides.iter().any(|g| g.markers.contains(&point)));

            let resized = editor.resize_selection_preview(
                &document,
                ResizeSelectionContext {
                    original_elements: &elements,
                    original_arrows: &[],
                    original_bounds: &bounds,
                    handle: ResizeHandle::TopLeft,
                    handle_offset_canvas: Point::new(0., 0.),
                    frame_padding: 0.,
                    corner_handle_outset: 0.,
                },
                raw,
                Modifiers::default(),
                None,
            );
            let rect = rectangle_to_draw_rect(&resized.elements[0].rect);
            assert_eq!((rect.min_x, rect.min_y), (point.x, point.y));
            assert!(!resized.snap_guides.is_empty());

            for (position, guides) in [
                editor.snap_arrow_creation_point(
                    &document,
                    Point::new(raw.x, 2000.),
                    Modifiers::default(),
                ),
                editor.snap_serial_number_creation_center(
                    &document,
                    Point::new(raw.x, 2000.),
                    Modifiers::default(),
                ),
            ] {
                assert_eq!(position, Point::new(point.x, 2000.));
                assert!(guides[0].markers.contains(&point));
            }
        }
        let source = Point::new(200. * mirror, 2000.);
        assert_eq!(
            editor.snap_creation_point(&document, source, Modifiers::default()),
            (source, Vec::new())
        );
    }
}

#[test]
fn ctrl_point_enablement_and_guide_visibility_apply_to_bracket_targets() {
    let (mut editor, document, _, _, selected, bounds) = fixture(1.);
    let elements = [selected];
    let source = Point::new(103., 2000.);
    for persistent in [false, true] {
        editor.config.snap.enabled = persistent;
        for ctrl in [false, true] {
            let modifiers = Modifiers {
                ctrl,
                ..Default::default()
            };
            let should_snap = persistent != ctrl;
            for show_guides in [false, true] {
                editor.config.snap.show_guides = show_guides;
                let (position, guides) = editor.snap_creation_point(&document, source, modifiers);
                assert_eq!(position.x, if should_snap { 100. } else { 103. });
                assert_eq!(!guides.is_empty(), should_snap && show_guides);
                let moved = editor.resolve_move_snap(MoveSnapRequest {
                    document: &document,
                    original_bounds: &bounds,
                    original_elements: &elements,
                    original_arrows: &[],
                    base_dx: -497.,
                    base_dy: 1300.,
                    modifiers,
                });
                assert_eq!(moved.dx, if should_snap { -500. } else { -497. });
                assert_eq!(!moved.guides.is_empty(), should_snap && show_guides);
                assert_eq!(editor.config.snap.enabled, persistent);
            }
        }
    }
    editor.config.snap.enabled = true;
    editor.config.snap.enable_point_snaps = false;
    assert_eq!(
        editor
            .snap_creation_point(&document, source, Modifiers::default())
            .0,
        source
    );
    let moved = editor.resolve_move_snap(MoveSnapRequest {
        document: &document,
        original_bounds: &bounds,
        original_elements: &elements,
        original_arrows: &[],
        base_dx: -497.,
        base_dy: 1300.,
        modifiers: Modifiers::default(),
    });
    assert_eq!((moved.dx, moved.dy), (-497., 1300.));
    assert!(moved.guides.is_empty());
}

#[test]
fn zoom_grid_and_external_guides_keep_their_existing_priority() {
    let (mut editor, document, _, _, _, _) = fixture(1.);
    for (zoom, offset, snaps) in [(2., 3., true), (2., 5., false), (0.5, 12., true)] {
        editor
            .set_camera(Camera {
                center: Point::new(0., 0.),
                zoom,
            })
            .unwrap();
        let source = Point::new(100. + offset, 2000.);
        let (position, _) = editor.snap_creation_point(&document, source, Modifiers::default());
        assert_eq!(position.x, if snaps { 100. } else { source.x });
    }
    editor
        .set_camera(Camera {
            center: Point::new(0., 0.),
            zoom: 1.,
        })
        .unwrap();
    editor.config.grid.enabled = true;
    let source = Point::new(303., 333.);
    assert_eq!(
        editor
            .snap_creation_point(&document, source, Modifiers::default())
            .0,
        Point::new(300., 340.)
    );
    assert_eq!(
        editor
            .snap_creation_point(
                &document,
                source,
                Modifiers {
                    ctrl: true,
                    ..Default::default()
                }
            )
            .0,
        source
    );
    editor.config.grid.enabled = false;
    for line in [101., 100.] {
        editor.view.get_mut().snap_guide_targets.vertical_xs[0] = Some(line);
        let (point, guides) =
            editor.snap_creation_point(&document, Point::new(103., 2000.), Modifiers::default());
        assert_eq!(point.x, line);
        assert!(
            guides.is_empty(),
            "the external guide replaces bracket markers when closer or tied"
        );
        let plan = editor
            .object_snap_plan(
                &document,
                ObjectSnapActor::Creation(ActiveTool::Shape),
                &[],
                Modifiers::default(),
            )
            .unwrap();
        let snap = plan.snap_rect(ObjectSnapRectRequest {
            target_rect: DrawRect::new(103., 2000., 143., 2040.),
            reference_rects: &plan.references,
            snap_distance: plan.snap_distance,
            target_anchors_x: &[SnapAxisAnchor::Start],
            target_anchors_y: &[],
            enable_point_snaps: true,
            enable_gap_snaps: true,
        });
        assert_eq!(snap.dx, line - 103.);
        assert!(snap.guides.is_empty());
    }
}

#[test]
fn hidden_brackets_and_brackets_in_the_moving_or_resizing_selection_are_excluded() {
    let (editor, mut document, bracket, _, selected, bounds) = fixture(1.);
    let elements = [selected];
    let arrows = [SelectionArrowState {
        id: bracket,
        arrow: document.arrow(bracket).unwrap().clone(),
    }];
    let moved = editor.resolve_move_snap(MoveSnapRequest {
        document: &document,
        original_bounds: &bounds,
        original_elements: &elements,
        original_arrows: &arrows,
        base_dx: -497.,
        base_dy: 1300.,
        modifiers: Modifiers::default(),
    });
    assert_eq!(moved.dx, -497.);
    assert!(moved.guides.is_empty());
    let resized = editor.resize_selection_preview(
        &document,
        ResizeSelectionContext {
            original_elements: &elements,
            original_arrows: &arrows,
            original_bounds: &bounds,
            handle: ResizeHandle::TopLeft,
            handle_offset_canvas: Point::new(0., 0.),
            frame_padding: 0.,
            corner_handle_outset: 0.,
        },
        Point::new(103., 700.),
        Modifiers::default(),
        None,
    );
    assert_eq!(
        rectangle_to_draw_rect(&resized.elements[0].rect).min_x,
        103.
    );
    assert!(resized.snap_guides.is_empty());
    let mut hide = Transaction::new("hide bracket");
    hide.update_element_meta(
        bracket,
        ElementMeta {
            visible: false,
            locked: false,
        },
    );
    document.apply_transaction(hide).unwrap();
    let moved = editor.resolve_move_snap(MoveSnapRequest {
        document: &document,
        original_bounds: &bounds,
        original_elements: &elements,
        original_arrows: &[],
        base_dx: -497.,
        base_dy: 1300.,
        modifiers: Modifiers::default(),
    });
    assert_eq!(moved.dx, -497.);
    assert!(moved.guides.is_empty());
}

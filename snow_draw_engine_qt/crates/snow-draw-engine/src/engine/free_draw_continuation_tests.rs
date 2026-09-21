use super::*;
use snow_draw_engine_core::{ColorRgba8, PathSegmentMode};
use snow_draw_engine_display::{OverlayDisplayItem, SceneDisplayItem};
use snow_draw_engine_document::{
    ElementMeta, FillStyle, FreeDrawData, FreeDrawStyle, StrokeStyle, Transaction,
};
use snow_draw_engine_interaction::{
    PointerButton, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
};

fn setup() -> (Engine, ViewportId) {
    let mut e = Engine::default();
    let v = e.create_viewport(ViewportConfig::default()).unwrap();
    e.set_viewport_surface_size(v, 800, 600).unwrap();
    e.set_viewport_snap_config(
        v,
        SnapConfig {
            enabled: false,
            ..Default::default()
        },
    )
    .unwrap();
    e.set_viewport_active_tool(v, ActiveTool::FreeDraw).unwrap();
    (e, v)
}
fn stroke(points: &[Point]) -> FreeDrawData {
    FreeDrawData::from_global_vertices(
        points,
        vec![PathSegmentMode::Straight; points.len() - 1],
        false,
        FreeDrawStyle {
            stroke: ColorRgba8 {
                r: 200,
                g: 20,
                b: 40,
                a: 255,
            },
            stroke_width: 4.0,
            stroke_style: StrokeStyle::Solid,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            opacity: 0.5,
        },
    )
    .unwrap()
}
fn apply(e: &mut Engine, v: ViewportId, t: Transaction) {
    e.apply_editor_command(
        v,
        EditorCommand::ApplyTransaction(ApplyTransactionCommand {
            transaction: t,
            history_undo_snapshot: None,
        }),
    )
    .unwrap();
}
fn insert(e: &mut Engine, v: ViewportId, data: FreeDrawData, meta: ElementMeta) -> ElementId {
    let id = e.model.peek_next_element_id();
    let mut t = Transaction::new("fixture");
    t.insert_free_draw(id, meta, data);
    apply(e, v, t);
    id
}
fn pointer(e: &mut Engine, v: ViewportId, kind: PointerEventType, p: Point, shift: bool) {
    e.process_input(
        v,
        InputEvent::Pointer(PointerEvent {
            pointer_id: 1,
            event_type: kind,
            device: PointerDevice::Mouse,
            position: Point::new(p.x + 400.0, p.y + 300.0),
            button: Some(PointerButton::Primary),
            buttons: PointerButtons(PointerButtons::PRIMARY),
            modifiers: snow_draw_engine_interaction::Modifiers {
                shift,
                ..Default::default()
            },
        }),
    )
    .unwrap();
}
fn marker(e: &Engine, v: ViewportId) -> Option<Point> {
    e.acquire_patch(v, None)
        .unwrap()
        .overlay
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| {
            if let OverlayDisplayItem::Rectangle(rect) = item
                && rect.fill
                    == (ColorRgba8 {
                        r: 106,
                        g: 189,
                        b: 252,
                        a: 255,
                    })
            {
                Some(Point::new(rect.center_x, rect.center_y))
            } else {
                None
            }
        })
}
fn scene(e: &Engine, v: ViewportId) -> Vec<SceneDisplayItem> {
    e.acquire_patch(v, None)
        .unwrap()
        .scene
        .ops
        .iter()
        .flat_map(|op| op.insert_items.iter().cloned())
        .collect()
}
#[test]
fn free_draw_continuation_hover_radius_priority_and_cleanup() {
    let (mut e, v) = setup();
    let data = stroke(&[Point::new(-40.0, 0.0), Point::new(40.0, 0.0)]);
    insert(&mut e, v, data.clone(), ElementMeta::default());
    for (p, expected) in [
        (Point::new(48.0, 0.0), Some(Point::new(40.0, 0.0))),
        (Point::new(48.01, 0.0), None),
        (Point::new(-40.0, 7.0), Some(Point::new(-40.0, 0.0))),
        (Point::new(0.0, 0.0), None),
    ] {
        pointer(&mut e, v, PointerEventType::Move, p, false);
        assert_eq!(marker(&e, v), expected);
    }
    let top = insert(
        &mut e,
        v,
        stroke(&[Point::new(40.0, 4.0), Point::new(100.0, 4.0)]),
        ElementMeta::default(),
    );
    pointer(
        &mut e,
        v,
        PointerEventType::Move,
        Point::new(40.0, 2.0),
        false,
    );
    assert_eq!(marker(&e, v), Some(Point::new(40.0, 4.0)));
    pointer(
        &mut e,
        v,
        PointerEventType::Leave,
        Point::new(40.0, 2.0),
        false,
    );
    assert_eq!(marker(&e, v), None);
    pointer(
        &mut e,
        v,
        PointerEventType::Down,
        Point::new(40.0, 2.0),
        false,
    );
    pointer(
        &mut e,
        v,
        PointerEventType::Up,
        Point::new(40.0, 30.0),
        true,
    );
    assert_eq!(e.model.paint_order().len(), 2);
    assert_eq!(
        e.model.free_draw(top).unwrap().global_vertices().first(),
        Some(&Point::new(40.0, 30.0))
    );
    e.set_viewport_active_tool(v, ActiveTool::Select).unwrap();
    pointer(
        &mut e,
        v,
        PointerEventType::Move,
        Point::new(-40.0, 0.0),
        false,
    );
    assert_eq!(marker(&e, v), None);
}
#[test]
fn free_draw_continuation_append_prepend_preview_and_history() {
    for at_start in [false, true] {
        let (mut e, v) = setup();
        let data = stroke(&[
            Point::new(-60.0, 0.0),
            Point::new(0.0, 20.0),
            Point::new(60.0, 0.0),
        ]);
        let id = insert(&mut e, v, data.clone(), ElementMeta::default());
        let other = insert(
            &mut e,
            v,
            stroke(&[Point::new(90.0, 0.0), Point::new(150.0, 0.0)]),
            ElementMeta::default(),
        );
        let endpoint = Point::new(if at_start { -60.0 } else { 60.0 }, 0.0);
        let next = Point::new(if at_start { -100.0 } else { 100.0 }, 50.0);
        pointer(&mut e, v, PointerEventType::Down, endpoint, false);
        pointer(&mut e, v, PointerEventType::Move, next, true);
        assert_eq!(
            e.model.free_draw(id).unwrap(),
            &data,
            "preview must not mutate document"
        );
        let items = scene(&e, v);
        assert_eq!(items.len(), 2);
        let SceneDisplayItem::Arrow(preview) = &items[0] else {
            panic!("stroke preview")
        };
        assert_eq!(preview.id.index, id.index);
        assert_eq!(preview.opacity, 0.5);
        let preview_commands = preview.path_commands.clone();
        pointer(&mut e, v, PointerEventType::Up, next, true);
        let result = e.model.free_draw(id).unwrap().clone();
        assert_eq!(e.model.paint_order(), &[id, other]);
        assert_eq!(result.stroke, data.stroke);
        assert_eq!(result.stroke_width, data.stroke_width);
        assert_eq!(result.opacity, data.opacity);
        assert_eq!(preview_commands, result.path_commands());
        let points = result.global_vertices();
        if at_start {
            assert_eq!(points.first(), Some(&next));
            assert_eq!(&points[1..], data.global_vertices());
        } else {
            assert_eq!(points.last(), Some(&next));
            assert_eq!(&points[..points.len() - 1], data.global_vertices());
        }
        e.undo().unwrap();
        assert_eq!(e.model.free_draw(id).unwrap(), &data);
        e.redo().unwrap();
        assert_eq!(e.model.free_draw(id).unwrap(), &result);
        e.history.validate_session(&e.model).unwrap();
    }
}
#[test]
fn free_draw_continuation_click_cancel_and_invalidated_target() {
    for cancel in [false, true] {
        let (mut e, v) = setup();
        let data = stroke(&[Point::new(-40.0, 0.0), Point::new(40.0, 0.0)]);
        let id = insert(&mut e, v, data.clone(), ElementMeta::default());
        pointer(
            &mut e,
            v,
            PointerEventType::Down,
            Point::new(44.0, 2.0),
            true,
        );
        if cancel {
            pointer(
                &mut e,
                v,
                PointerEventType::Move,
                Point::new(90.0, 30.0),
                true,
            );
            pointer(
                &mut e,
                v,
                PointerEventType::Cancel,
                Point::new(90.0, 30.0),
                true,
            );
        } else {
            pointer(&mut e, v, PointerEventType::Up, Point::new(44.0, 2.0), true);
        }
        assert_eq!(e.model.free_draw(id).unwrap(), &data);
        e.undo().unwrap();
        assert!(e.model.paint_order().is_empty(), "no extra history entry");
    }
    let (mut e, v) = setup();
    let data = stroke(&[Point::new(-40.0, 0.0), Point::new(40.0, 0.0)]);
    let id = insert(&mut e, v, data.clone(), ElementMeta::default());
    pointer(
        &mut e,
        v,
        PointerEventType::Down,
        Point::new(40.0, 0.0),
        false,
    );
    let mut changed = data.clone();
    changed.opacity = 0.8;
    let mut t = Transaction::new("external edit");
    t.update_free_draw(id, changed.clone());
    apply(&mut e, v, t);
    pointer(
        &mut e,
        v,
        PointerEventType::Up,
        Point::new(90.0, 30.0),
        false,
    );
    assert_eq!(e.model.free_draw(id).unwrap(), &changed);
}
#[test]
fn free_draw_continuation_excludes_closed_hidden_and_locked_paths() {
    for mode in 0..3 {
        let (mut e, v) = setup();
        let mut data = stroke(&[
            Point::new(-40.0, 0.0),
            Point::new(40.0, 0.0),
            Point::new(0.0, 40.0),
        ]);
        let mut meta = ElementMeta::default();
        match mode {
            0 => {
                data.closed = true;
                data.segment_modes.push(PathSegmentMode::Straight);
            }
            1 => meta.visible = false,
            _ => meta.locked = true,
        }
        insert(&mut e, v, data, meta);
        pointer(
            &mut e,
            v,
            PointerEventType::Move,
            Point::new(-40.0, 0.0),
            false,
        );
        assert_eq!(marker(&e, v), None);
    }
}

#[test]
fn free_draw_continuation_transformed_endpoints_revalidate_with_camera_and_document() {
    let (mut e, v) = setup();
    let mut data = stroke(&[Point::new(-40.0, 0.0), Point::new(40.0, 0.0)]);
    data.rotation = std::f64::consts::FRAC_PI_2;
    data.width = 120.0;
    let original = data.global_vertices();
    let endpoint = original[1];
    let id = insert(&mut e, v, data.clone(), ElementMeta::default());
    // This view position becomes the endpoint after zoom/pan, without a new hover event.
    pointer(
        &mut e,
        v,
        PointerEventType::Move,
        Point::new(7.0, 0.0),
        false,
    );
    assert_eq!(marker(&e, v), None);
    e.set_viewport_camera(
        v,
        Camera {
            center: endpoint,
            zoom: 2.0,
        },
    )
    .unwrap();
    assert_eq!(marker(&e, v), Some(endpoint));
    pointer(
        &mut e,
        v,
        PointerEventType::Move,
        Point::new(8.01, 0.0),
        false,
    );
    assert_eq!(marker(&e, v), None);
    pointer(
        &mut e,
        v,
        PointerEventType::Down,
        Point::new(0.0, 0.0),
        false,
    );
    pointer(
        &mut e,
        v,
        PointerEventType::Up,
        Point::new(60.0, 40.0),
        true,
    );
    let points = e.model.free_draw(id).unwrap().global_vertices();
    for (actual, expected) in points.iter().zip(&original) {
        assert!((actual.x - expected.x).abs() < 1e-9 && (actual.y - expected.y).abs() < 1e-9);
    }
    assert_eq!(
        points.last(),
        Some(&Point::new(endpoint.x + 30.0, endpoint.y + 20.0))
    );
    pointer(
        &mut e,
        v,
        PointerEventType::Move,
        Point::new(60.0, 40.0),
        false,
    );
    assert!(marker(&e, v).is_some());
    let mut updated = e.model.free_draw(id).unwrap().clone();
    updated.x += 100.0;
    let mut t = Transaction::new("move target");
    t.update_free_draw(id, updated);
    apply(&mut e, v, t);
    assert_eq!(marker(&e, v), None);
}

#[test]
fn free_draw_continuation_closes_at_opposite_endpoint_and_never_merges() {
    for start in [false, true] {
        let (mut e, v) = setup();
        let data = stroke(&[
            Point::new(-60.0, 0.0),
            Point::new(0.0, 60.0),
            Point::new(60.0, 0.0),
        ]);
        let id = insert(&mut e, v, data, ElementMeta::default());
        let from = Point::new(if start { -60.0 } else { 60.0 }, 0.0);
        let to = Point::new(-from.x, 0.0);
        pointer(&mut e, v, PointerEventType::Down, from, false);
        pointer(
            &mut e,
            v,
            PointerEventType::Move,
            Point::new(0.0, -60.0),
            true,
        );
        pointer(&mut e, v, PointerEventType::Move, to, true);
        let items = scene(&e, v);
        let SceneDisplayItem::Arrow(preview) = &items[0] else {
            panic!("preview")
        };
        assert!(preview.geometry.closed);
        pointer(&mut e, v, PointerEventType::Up, to, true);
        let result = e.model.free_draw(id).unwrap();
        assert!(result.closed);
        assert_eq!(result.segment_modes.len(), result.vertices.len());
        assert_eq!(preview.path_commands, result.path_commands());
        pointer(&mut e, v, PointerEventType::Move, to, false);
        assert_eq!(marker(&e, v), None);
    }
    let (mut e, v) = setup();
    let id = insert(
        &mut e,
        v,
        stroke(&[Point::new(-60.0, 0.0), Point::new(0.0, 0.0)]),
        ElementMeta::default(),
    );
    let other_data = stroke(&[Point::new(60.0, 0.0), Point::new(120.0, 0.0)]);
    let other = insert(&mut e, v, other_data.clone(), ElementMeta::default());
    pointer(
        &mut e,
        v,
        PointerEventType::Down,
        Point::new(0.0, 0.0),
        false,
    );
    pointer(&mut e, v, PointerEventType::Up, Point::new(60.0, 0.0), true);
    assert_eq!(e.model.paint_order(), &[id, other]);
    assert_eq!(e.model.free_draw(other).unwrap(), &other_data);
}

#[test]
fn free_draw_continuation_escape_removes_preview_and_preserves_history() {
    let (mut e, v) = setup();
    let data = stroke(&[Point::new(-40.0, 0.0), Point::new(40.0, 0.0)]);
    let id = insert(&mut e, v, data.clone(), ElementMeta::default());
    let before = scene(&e, v);
    pointer(
        &mut e,
        v,
        PointerEventType::Down,
        Point::new(40.0, 0.0),
        false,
    );
    pointer(
        &mut e,
        v,
        PointerEventType::Move,
        Point::new(100.0, 40.0),
        false,
    );
    e.process_input(
        v,
        InputEvent::Key(snow_draw_engine_interaction::KeyEvent {
            event_type: snow_draw_engine_interaction::KeyEventType::KeyDown,
            key_code: snow_draw_engine_interaction::KeyCode::Escape,
            modifiers: Default::default(),
            repeat: false,
        }),
    )
    .unwrap();
    assert_eq!(scene(&e, v), before);
    assert_eq!(e.model.free_draw(id).unwrap(), &data);
    e.undo().unwrap();
    assert!(e.model.paint_order().is_empty());
}

#[test]
fn free_draw_continuation_preview_enters_view_from_culled_original() {
    let (mut e, v) = setup();
    let id = insert(
        &mut e,
        v,
        stroke(&[Point::new(-600.0, 0.0), Point::new(-500.0, 0.0)]),
        ElementMeta::default(),
    );
    insert(
        &mut e,
        v,
        stroke(&[Point::new(0.0, 100.0), Point::new(100.0, 100.0)]),
        ElementMeta::default(),
    );
    assert_eq!(scene(&e, v).len(), 1);
    pointer(
        &mut e,
        v,
        PointerEventType::Down,
        Point::new(-500.0, 0.0),
        false,
    );
    pointer(
        &mut e,
        v,
        PointerEventType::Move,
        Point::new(20.0, 0.0),
        true,
    );
    let items = scene(&e, v);
    assert_eq!(items.len(), 2);
    let SceneDisplayItem::Arrow(preview) = &items[0] else {
        panic!("replacement")
    };
    assert_eq!(preview.id.index, id.index);
    assert!(preview.geometry.canvas_bounds[2] >= 20.0);
    pointer(&mut e, v, PointerEventType::Up, Point::new(20.0, 0.0), true);
    assert_eq!(scene(&e, v).len(), 2);
}

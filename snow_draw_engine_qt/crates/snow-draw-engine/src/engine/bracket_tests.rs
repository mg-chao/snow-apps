use super::*;
use crate::SceneDisplayItem;
use snow_draw_engine_document::ElementKind;
use snow_draw_engine_interaction::{
    Modifiers, PointerButton, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
};
fn setup() -> (Engine, ViewportId) {
    let mut engine = Engine::default();
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Bracket)
        .unwrap();
    engine
        .set_viewport_snap_config(
            viewport,
            SnapConfig {
                enabled: false,
                ..Default::default()
            },
        )
        .unwrap();
    (engine, viewport)
}
fn pointer(engine: &mut Engine, viewport: ViewportId, kind: PointerEventType, point: Point<f64>) {
    pointer_modified(engine, viewport, kind, point, Modifiers::default());
}
fn pointer_modified(
    engine: &mut Engine,
    viewport: ViewportId,
    kind: PointerEventType,
    point: Point<f64>,
    modifiers: Modifiers,
) {
    engine
        .process_input(
            viewport,
            InputEvent::Pointer(PointerEvent {
                pressure: None,
                timestamp_ms: 0,
                pointer_id: 1,
                event_type: kind,
                device: PointerDevice::Mouse,
                position: Point::new(point.x + 400.0, point.y + 300.0),
                button: matches!(
                    kind,
                    PointerEventType::Down | PointerEventType::Up | PointerEventType::DoubleClick
                )
                .then_some(PointerButton::Primary),
                buttons: if matches!(kind, PointerEventType::Down | PointerEventType::DoubleClick) {
                    PointerButtons(PointerButtons::PRIMARY)
                } else {
                    PointerButtons::default()
                },
                modifiers,
            }),
        )
        .unwrap();
}
fn click(engine: &mut Engine, viewport: ViewportId, point: Point<f64>) {
    pointer(engine, viewport, PointerEventType::Down, point);
    pointer(engine, viewport, PointerEventType::Up, point);
}

fn create(engine: &mut Engine, view: ViewportId) -> ElementId {
    pointer(
        engine,
        view,
        PointerEventType::Down,
        Point::new(-100., -100.),
    );
    pointer(engine, view, PointerEventType::Move, Point::new(-20., 100.));
    pointer(engine, view, PointerEventType::Up, Point::new(-20., 100.));
    engine.model.paint_order()[0]
}
#[test]
fn bracket_drag_creates_one_element_three_handles_and_one_history_step() {
    let (mut e, v) = setup();
    let id = create(&mut e, v);
    assert_eq!(e.model.paint_order().len(), 1);
    let a = e.model.arrow(id).unwrap();
    assert_eq!(a.element_kind(), ElementKind::Bracket);
    assert_eq!(
        a.global_points(),
        vec![
            Point::new(-100., -100.),
            Point::new(-20., 0.),
            Point::new(-100., 100.)
        ]
    );
    e.select_element_with_viewport_changes(v, id).unwrap();
    let p = e
        .editor
        .presentation_state(&e.model, &e.viewport_slot(v).unwrap().view);
    assert_eq!(p.arrow_handles.len(), 3);
    assert!(!snow_draw_engine_editor::selection_box_visible_for_members(
        &p.selection_elements,
        &p.selection_arrows
    ));
    e.undo().unwrap();
    assert!(e.model.paint_order().is_empty() && !e.history_state().can_undo);
    e.redo().unwrap();
    assert!(e.model.arrow(id).unwrap().is_bracket());
}
#[test]
fn bracket_each_control_drag_is_independent_cancelable_and_one_history_step() {
    for (start, indices) in [
        (Point::new(-100., -100.), vec![0]),
        (Point::new(-20., 0.), vec![1]),
        (Point::new(-100., 100.), vec![2]),
        (Point::new(-100., 0.), vec![0, 2]),
        (Point::new(-60., -50.), vec![0, 1, 2]),
    ] {
        let (mut e, v) = setup();
        let id = create(&mut e, v);
        e.set_viewport_active_tool(v, ActiveTool::Select).unwrap();
        e.select_element_with_viewport_changes(v, id).unwrap();
        let original = e.model.arrow(id).unwrap().clone();
        let end = Point::new(start.x + 17., start.y + 23.);
        pointer(&mut e, v, PointerEventType::Down, start);
        pointer(&mut e, v, PointerEventType::Move, end);
        assert_eq!(e.model.arrow(id).unwrap(), &original);
        e.process_input(v, InputEvent::FocusLost).unwrap();
        assert_eq!(e.model.arrow(id).unwrap(), &original);
        pointer(&mut e, v, PointerEventType::Down, start);
        pointer(&mut e, v, PointerEventType::Move, end);
        pointer(&mut e, v, PointerEventType::Up, end);
        let changed = e.model.arrow(id).unwrap().clone();
        for (i, p) in changed.global_points().iter().enumerate() {
            let old = original.global_points()[i];
            let delta = if indices.contains(&i) {
                Point::new(17., 23.)
            } else {
                Point::new(0., 0.)
            };
            assert!(
                (p.x - old.x - delta.x).abs() < 1e-8 && (p.y - old.y - delta.y).abs() < 1e-8,
                "control {indices:?}: {i} {p:?}"
            );
        }
        let mut restored = Engine::from_serialized_document_session_with_config(
            &e.serialize_document_session().unwrap(),
            EngineConfig::default(),
        )
        .unwrap();
        restored.undo().unwrap();
        assert_eq!(restored.model.arrow(id).unwrap(), &original);
        restored.redo().unwrap();
        assert_eq!(restored.model.arrow(id).unwrap(), &changed);
        restored.undo().unwrap();
        restored.undo().unwrap();
        assert!(restored.model.paint_order().is_empty() && !restored.history_state().can_undo);
    }
}
#[test]
fn bracket_collapsed_creations_and_clicks_do_not_add_history() {
    let (mut e, v) = setup();
    click(&mut e, v, Point::new(0., 0.));
    assert!(e.model.paint_order().is_empty());
    let id = create(&mut e, v);
    let original = e.model.arrow(id).unwrap().clone();
    for p in [
        Point::new(-100., -100.),
        Point::new(-20., 0.),
        Point::new(-100., 0.),
    ] {
        click(&mut e, v, p);
        assert_eq!(e.model.arrow(id).unwrap(), &original);
    }
    e.undo().unwrap();
    assert!(!e.history_state().can_undo);
}

#[test]
fn bracket_edits_retain_unrelated_geometry_and_skip_unchanged_updates() {
    let (mut e, v) = setup();
    let id = create(&mut e, v);
    e.apply_annotation_json(br#"{"version":1,"operations":[{"type":"bracket","points":[[200,-100],[280,0],[200,100]]}]}"#).unwrap();
    let unrelated = e.model.paint_order()[1];
    e.select_element_with_viewport_changes(v, id).unwrap();
    let SceneDisplayItem::Arrow(cached) = e.scene_cache.entry(unrelated).unwrap() else {
        panic!("cached bracket")
    };
    let geometry = cached.geometry.clone();
    pointer(&mut e, v, PointerEventType::Down, Point::new(-20., 0.));
    for point in [Point::new(20., 10.), Point::new(-100., 250.)] {
        let cursor = e.viewport_slot(v).unwrap().composer.current_cursor();
        pointer(&mut e, v, PointerEventType::Move, point);
        let patch = e.acquire_patch(v, Some(cursor)).unwrap();
        assert_eq!(patch.path_geometry_ops.len(), 1);
        assert_eq!(patch.path_geometry_ops[0].id.index, id.index);
        let SceneDisplayItem::Arrow(cached) = e.scene_cache.entry(unrelated).unwrap() else {
            panic!("cached bracket")
        };
        assert!(std::sync::Arc::ptr_eq(&geometry, &cached.geometry));
        let cursor = e.viewport_slot(v).unwrap().composer.current_cursor();
        pointer(&mut e, v, PointerEventType::Move, point);
        let patch = e.acquire_patch(v, Some(cursor)).unwrap();
        assert!(patch.path_geometry_ops.is_empty() && patch.scene.ops.is_empty());
    }
}

#[test]
fn bracket_coincident_endpoint_keeps_last_preview_and_crossing_keeps_handle_identity() {
    let (mut e, v) = setup();
    let id = create(&mut e, v);
    e.select_element_with_viewport_changes(v, id).unwrap();
    pointer(&mut e, v, PointerEventType::Down, Point::new(-100., -100.));
    pointer(&mut e, v, PointerEventType::Move, Point::new(-100., 80.));
    pointer(&mut e, v, PointerEventType::Move, Point::new(-100., 100.));
    pointer(&mut e, v, PointerEventType::Up, Point::new(-100., 100.));
    assert_eq!(
        e.model.arrow(id).unwrap().global_points()[0],
        Point::new(-100., 80.)
    );
    pointer(&mut e, v, PointerEventType::Down, Point::new(-100., 80.));
    pointer(&mut e, v, PointerEventType::Move, Point::new(-100., 150.));
    pointer(&mut e, v, PointerEventType::Up, Point::new(-100., 150.));
    let points = e.model.arrow(id).unwrap().global_points();
    assert_eq!(points[0], Point::new(-100., 150.));
    assert_eq!(points[2], Point::new(-100., 100.));
}

#[test]
fn bracket_copy_duplicate_delete_and_session_preserve_three_point_geometry() {
    let (mut e, v) = setup();
    let id = create(&mut e, v);
    e.select_element_with_viewport_changes(v, id).unwrap();
    let template = e.serialize_selected_draw_template().unwrap();
    e.duplicate_selected_with_viewport_changes(v, Point::new(0., 300.))
        .unwrap();
    let duplicate = e.selected_ids()[0];
    let original = e.model.arrow(id).unwrap();
    let copied = e.model.arrow(duplicate).unwrap();
    assert!(copied.is_bracket() && copied.text_element_id.is_none());
    for (a, b) in original
        .global_points()
        .into_iter()
        .zip(copied.global_points())
    {
        assert_eq!(b, Point::new(a.x, a.y + 300.));
    }
    e.insert_draw_template_with_viewport_changes(v, &template, Point::new(300., 0.))
        .unwrap();
    let inserted = e.selected_ids()[0];
    assert!(e.model.arrow(inserted).unwrap().is_bracket());
    assert_eq!(e.model.paint_order().len(), 3);
    let mut restored = Engine::from_serialized_document_session_with_config(
        &e.serialize_document_session().unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
    assert_eq!(restored.model.document(), e.model.document());
    let view = restored.create_viewport(ViewportConfig::default()).unwrap();
    restored
        .select_element_with_viewport_changes(view, inserted)
        .unwrap();
    restored
        .delete_selected_with_viewport_changes(view)
        .unwrap();
    assert_eq!(restored.model.paint_order().len(), 2);
    restored.undo().unwrap();
    assert_eq!(restored.model.paint_order().len(), 3);
    restored.redo().unwrap();
    assert_eq!(restored.model.paint_order().len(), 2);
}

#[test]
fn bracket_creation_mirrors_in_all_drag_directions_and_cancels_without_history() {
    for end in [
        Point::new(80., 200.),
        Point::new(-80., 200.),
        Point::new(80., -200.),
        Point::new(-80., -200.),
    ] {
        let (mut e, v) = setup();
        pointer(&mut e, v, PointerEventType::Down, Point::new(0., 0.));
        pointer(&mut e, v, PointerEventType::Move, end);
        e.process_input(v, InputEvent::FocusLost).unwrap();
        assert!(e.model.paint_order().is_empty() && !e.history_state().can_undo);
        pointer(&mut e, v, PointerEventType::Down, Point::new(0., 0.));
        pointer(&mut e, v, PointerEventType::Move, end);
        pointer(&mut e, v, PointerEventType::Up, end);
        let a = e.model.arrow(e.model.paint_order()[0]).unwrap();
        assert_eq!(
            a.global_points(),
            vec![
                Point::new(0., 0_f64.min(end.y)),
                Point::new(end.x, end.y / 2.),
                Point::new(0., 0_f64.max(end.y))
            ]
        );
    }
}
#[test]
fn bracket_invalid_annotation_transactions_leave_document_and_history_unchanged() {
    let (mut e, _) = setup();
    for operation in [
        serde_json::json!({"type":"bracket","points":[[0,0],[80,100],[0,0]]}),
        serde_json::json!({"type":"bracket","points":[[0,0],[80,100]]}),
        serde_json::json!({"type":"bracket","points":[[0,0],[80,100],[0,200]],"style":{"stroke_width":73}}),
        serde_json::json!({"type":"bracket","points":[[0,0],[80,100],[0,200]],"style":{"unit":"degrees"}}),
    ] {
        let input=serde_json::to_vec(&serde_json::json!({"version":1,"operations":[{"type":"bracket","points":[[0,0],[80,100],[0,200]]},operation]})).unwrap();
        assert!(e.apply_annotation_json(&input).is_err());
        assert!(e.model.paint_order().is_empty() && !e.history_state().can_undo);
    }
}

#[test]
fn bracket_opacity_tracks_primary_mixed_selection_history_and_session_restore() {
    let (mut engine, viewport) = setup();
    engine
        .apply_annotation_json(br#"{"version":1,"operations":[{"type":"bracket","points":[[0,0],[80,100],[0,200]],"style":{"stroke":[20,70,190,128]}},{"type":"bracket","points":[[200,0],[280,100],[200,200]],"style":{"stroke_width":4}}]}"#)
        .unwrap();
    let ids = engine.model.paint_order().to_vec();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, ids[0])
        .unwrap();
    engine
        .set_selected_opacity_with_viewport_changes(viewport, 0.25)
        .unwrap();
    let state = engine.viewport_style_toolbar_state(viewport).unwrap();
    assert_eq!(state.source, crate::StyleToolbarSource::SelectedBracket);
    assert_eq!(state.shape_style.opacity, 0.25);
    assert_eq!(state.shape_style_mixed, 0);
    assert_eq!(engine.model.arrow(ids[0]).unwrap().stroke.a, 128);

    let selection = serde_json::from_value(serde_json::json!({
        "selectedIds": ids,
        "primaryId": ids[1],
    }))
    .unwrap();
    engine
        .editor
        .restore_history_selection(&engine.model, &selection);
    engine.refresh_all_viewports().unwrap();
    let state = engine.viewport_style_toolbar_state(viewport).unwrap();
    assert_eq!(state.shape_style.opacity, 1.0);
    assert_eq!(
        state.shape_style_mixed,
        snow_draw_engine_editor::SHAPE_STYLE_MIXED_OPACITY
    );
    assert_ne!(state.bracket_style_mixed, 0);

    engine
        .set_selected_opacity_with_viewport_changes(viewport, 0.5)
        .unwrap();
    let state = engine.viewport_style_toolbar_state(viewport).unwrap();
    assert_eq!(state.shape_style.opacity, 0.5);
    assert_eq!(state.shape_style_mixed, 0);
    for id in &ids {
        let arrow = engine.model.arrow(*id).unwrap();
        assert_eq!(arrow.opacity, 0.5);
        assert!(arrow.text_element_id.is_none());
    }
    engine.undo_with_viewport_changes().unwrap();
    let state = engine.viewport_style_toolbar_state(viewport).unwrap();
    assert_eq!(state.shape_style.opacity, 1.0);
    assert_eq!(
        state.shape_style_mixed,
        snow_draw_engine_editor::SHAPE_STYLE_MIXED_OPACITY
    );
    engine.redo_with_viewport_changes().unwrap();
    engine
        .clear_selection_with_viewport_changes(viewport)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, ids[0])
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .shape_style
            .opacity,
        0.5
    );
    let mut restored = Engine::from_serialized_document_session_with_config(
        &engine.serialize_document_session().unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
    let viewport = restored.create_viewport(ViewportConfig::default()).unwrap();
    restored
        .select_element_with_viewport_changes(viewport, ids[0])
        .unwrap();
    assert_eq!(
        restored
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .shape_style
            .opacity,
        0.5
    );
}

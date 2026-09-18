use super::*;
use snow_draw_engine_display::SceneDisplayItem;
use snow_draw_engine_interaction::{
    KeyCode, KeyEvent, KeyEventType, Modifiers, PointerButton, PointerButtons, PointerDevice,
    PointerEvent, PointerEventType,
};

pub(super) fn pointer(
    engine: &mut Engine,
    viewport: ViewportId,
    kind: PointerEventType,
    x: f64,
    y: f64,
    alt: bool,
) {
    engine
        .process_input_with_viewport_changes(
            viewport,
            InputEvent::Pointer(PointerEvent {
                pointer_id: 1,
                event_type: kind,
                device: PointerDevice::Mouse,
                position: Point::new(x, y),
                button: Some(PointerButton::Primary),
                buttons: PointerButtons(PointerButtons::PRIMARY),
                modifiers: Modifiers {
                    alt,
                    ..Modifiers::default()
                },
            }),
        )
        .unwrap();
}

fn setup() -> (Engine, ViewportId, ElementId) {
    let mut engine = Engine::default();
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        300.0,
        200.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        500.0,
        400.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        500.0,
        400.0,
        false,
    );
    let id = engine.model.paint_order()[0];
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    (engine, viewport, id)
}

fn scene(engine: &Engine, viewport: ViewportId) -> Vec<SceneDisplayItem> {
    engine
        .acquire_patch(viewport, None)
        .unwrap()
        .scene
        .ops
        .iter()
        .flat_map(|op| op.insert_items.clone())
        .collect()
}

#[test]
fn alt_drag_previews_original_and_copy_in_shared_views_and_commits_one_history_entry() {
    let (mut engine, viewport, id) = setup();
    let second = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine.set_viewport_surface_size(second, 800, 600).unwrap();
    let original = *engine.model.rectangle(id).unwrap();
    let history = engine.history_state();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        400.0,
        300.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        450.0,
        340.0,
        false,
    );
    assert_eq!(*engine.model.rectangle(id).unwrap(), original);
    assert_eq!(engine.model.paint_order().len(), 1);
    assert_eq!(engine.history_state(), history);
    for view in [viewport, second] {
        let items = scene(&engine, view);
        assert_eq!(items.len(), 2);
        let centers: Vec<_> = items
            .iter()
            .map(|item| match item {
                SceneDisplayItem::Rectangle(rect) => (rect.center_x, rect.center_y),
                _ => panic!("expected rectangle"),
            })
            .collect();
        assert_eq!(centers, vec![(0.0, 0.0), (50.0, 40.0)]);
    }
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        470.0,
        360.0,
        false,
    );
    let copy = engine.selected_ids()[0];
    assert_ne!(copy, id);
    let mut expected = original;
    expected.center = Point::new(70.0, 60.0);
    assert_eq!(*engine.model.rectangle(copy).unwrap(), expected);
    engine.undo().unwrap();
    assert_eq!(engine.model.paint_order(), &[id]);
    assert_eq!(engine.selected_ids(), vec![id]);
    assert_eq!(engine.history_state().can_undo, history.can_undo);
    engine.redo().unwrap();
    assert_eq!(engine.selected_ids(), vec![copy]);
    assert_eq!(*engine.model.rectangle(copy).unwrap(), expected);
}

#[test]
fn alt_drag_noops_and_cancellation_preserve_document_history_and_selection() {
    for mode in 0..7 {
        let (mut engine, viewport, id) = setup();
        let before = engine.serialize_document_session().unwrap();
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Down,
            400.0,
            300.0,
            true,
        );
        if mode >= 2 {
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Move,
                450.0,
                340.0,
                true,
            );
        } else if mode == 1 {
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Move,
                401.0,
                300.0,
                true,
            );
        }
        if mode == 6 {
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Move,
                400.0,
                300.0,
                true,
            );
        }
        match mode {
            3 => pointer(
                &mut engine,
                viewport,
                PointerEventType::Cancel,
                450.0,
                340.0,
                true,
            ),
            4 => {
                engine
                    .process_input_with_viewport_changes(
                        viewport,
                        InputEvent::Key(KeyEvent {
                            event_type: KeyEventType::KeyDown,
                            key_code: KeyCode::Escape,
                            modifiers: Modifiers::default(),
                            repeat: false,
                        }),
                    )
                    .unwrap();
            }
            5 => {
                engine
                    .process_input_with_viewport_changes(viewport, InputEvent::FocusLost)
                    .unwrap();
            }
            _ => pointer(
                &mut engine,
                viewport,
                PointerEventType::Up,
                400.0,
                300.0,
                true,
            ),
        }
        assert_eq!(engine.model.paint_order(), &[id], "mode {mode}");
        assert_eq!(engine.selected_ids(), vec![id]);
        assert_eq!(scene(&engine, viewport).len(), 1);
        assert_eq!(
            engine.serialize_document_session().unwrap(),
            before,
            "mode {mode}"
        );
    }
}

#[test]
fn alt_pressed_after_mouse_down_does_not_convert_a_move_to_copy() {
    let (mut engine, viewport, id) = setup();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        400.0,
        300.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        450.0,
        340.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        450.0,
        340.0,
        true,
    );
    assert_eq!(engine.model.paint_order(), &[id]);
    assert_eq!(
        engine.model.rectangle(id).unwrap().center,
        Point::new(50.0, 40.0)
    );
}

#[test]
fn alt_drag_uses_canvas_coordinates_under_zoom_and_camera_translation() {
    let (mut engine, viewport, id) = setup();
    engine
        .set_viewport_camera(
            viewport,
            Camera {
                center: Point::new(20.0, 10.0),
                zoom: 2.0,
            },
        )
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        360.0,
        280.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        460.0,
        360.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        500.0,
        400.0,
        true,
    );
    assert_eq!(
        engine.model.rectangle(id).unwrap().center,
        Point::new(0.0, 0.0)
    );
    assert_eq!(
        engine
            .model
            .rectangle(engine.selected_ids()[0])
            .unwrap()
            .center,
        Point::new(70.0, 60.0)
    );
}

#[test]
fn alt_drag_multi_selection_matches_explicit_duplicate_and_keeps_all_originals() {
    let (mut engine, viewport, id) = setup();
    engine
        .duplicate_selected_with_viewport_changes(viewport, Point::new(230.0, 0.0))
        .unwrap();
    // Marquee both rectangles, then grab the first member rather than the group center.
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        280.0,
        180.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        750.0,
        420.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        750.0,
        420.0,
        false,
    );
    assert_eq!(engine.selected_ids().len(), 2);
    let snapshot = engine.capture_session_snapshot();
    let bytes = engine.serialize_document_session().unwrap();
    let mut expected =
        Engine::from_serialized_document_session_with_config(&bytes, EngineConfig::default())
            .unwrap();
    let expected_view = expected.create_viewport(ViewportConfig::default()).unwrap();
    expected
        .editor
        .restore_history_selection(&expected.model, &snapshot);
    expected
        .duplicate_selected_with_viewport_changes(expected_view, Point::new(40.0, 50.0))
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        400.0,
        300.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        440.0,
        350.0,
        true,
    );
    assert_eq!(scene(&engine, viewport).len(), 4);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        440.0,
        350.0,
        true,
    );
    assert_eq!(engine.selected_ids(), expected.selected_ids());
    assert_eq!(engine.model.document(), expected.model.document());
    assert_eq!(
        engine.model.rectangle(id).unwrap().center,
        Point::new(0.0, 0.0)
    );
}

#[test]
fn alt_drag_serial_number_copies_bound_text_and_connector() {
    use snow_draw_engine_document::{ElementMeta, SerialNumberData, TextData, Transaction};
    let mut engine = Engine::default();
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    let text_id = ElementId {
        index: 0,
        generation: 1,
    };
    let serial_id = ElementId {
        index: 1,
        generation: 1,
    };
    let mut transaction = Transaction::new("serial with text");
    transaction.insert_text(
        text_id,
        ElementMeta::default(),
        TextData {
            center: Point::new(130.0, 0.0),
            text: "bound".to_owned(),
            width: 80.0,
            height: 24.0,
            ..TextData::default()
        },
    );
    transaction.insert_serial_number(
        serial_id,
        ElementMeta::default(),
        SerialNumberData {
            center: Point::new(0.0, 0.0),
            text_element_id: Some(text_id),
            ..SerialNumberData::default()
        },
    );
    engine
        .commit_transaction(
            viewport,
            ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            },
        )
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, serial_id)
        .unwrap();
    let original = engine.model.document().clone();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        400.0,
        300.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        450.0,
        360.0,
        true,
    );
    let preview = scene(&engine, viewport);
    assert_eq!(
        preview
            .iter()
            .filter(|i| matches!(i, SceneDisplayItem::SerialNumber(_)))
            .count(),
        2
    );
    assert_eq!(
        preview
            .iter()
            .filter(|i| matches!(i, SceneDisplayItem::Text(_)))
            .count(),
        2
    );
    assert_eq!(
        preview
            .iter()
            .filter(|i| matches!(i, SceneDisplayItem::SerialNumberConnector(_)))
            .count(),
        2
    );
    assert_eq!(engine.model.document(), &original);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        450.0,
        360.0,
        true,
    );
    let copy = engine.selected_ids()[0];
    let label = engine.model.bound_text_id_for_serial_number(copy).unwrap();
    assert_ne!(label, text_id);
    assert_eq!(
        engine.model.text(label).unwrap().center,
        Point::new(180.0, 60.0)
    );
    assert_eq!(engine.model.text(label).unwrap().text, "bound");
    assert_eq!(scene(&engine, viewport), preview);
}

#[test]
fn alt_drag_reuses_grid_snapping_and_unselected_drags_still_move() {
    let (mut engine, viewport, id) = setup();
    engine
        .set_viewport_grid_config(
            viewport,
            GridConfig {
                enabled: true,
                size: 20.0,
            },
        )
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        400.0,
        300.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        453.0,
        347.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        453.0,
        347.0,
        true,
    );
    let center = engine
        .model
        .rectangle(engine.selected_ids()[0])
        .unwrap()
        .center;
    assert_eq!(center.x % 20.0, 0.0);
    assert_eq!(center.y % 20.0, 0.0);
    engine.undo().unwrap();
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        400.0,
        200.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        460.0,
        260.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        460.0,
        260.0,
        true,
    );
    assert_eq!(engine.model.paint_order(), &[id]);
    assert_eq!(
        engine.model.rectangle(id).unwrap().center,
        Point::new(60.0, 60.0)
    );
}

#[test]
fn alt_resize_handle_keeps_center_scaling_without_copying() {
    let (mut engine, viewport, id) = setup();
    let original = *engine.model.rectangle(id).unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        300.0,
        200.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        270.0,
        170.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        270.0,
        170.0,
        true,
    );
    assert_eq!(engine.model.paint_order(), &[id]);
    let resized = engine.model.rectangle(id).unwrap();
    assert_eq!(resized.center, original.center);
    assert!(resized.width > original.width);
}

#[test]
fn alt_drag_preview_supports_strokes_filters_and_spotlights() {
    for tool in [
        ActiveTool::FreeDraw,
        ActiveTool::PenFilter,
        ActiveTool::RectangleFilter,
        ActiveTool::Spotlight,
        ActiveTool::RectangleHighlight,
        ActiveTool::PenHighlight,
    ] {
        let mut engine = Engine::default();
        let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
        engine
            .set_viewport_surface_size(viewport, 800, 600)
            .unwrap();
        engine.set_viewport_active_tool(viewport, tool).unwrap();
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Down,
            300.0,
            200.0,
            false,
        );
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            400.0,
            300.0,
            false,
        );
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Up,
            500.0,
            400.0,
            false,
        );
        let id = engine.model.paint_order()[0];
        engine
            .set_viewport_active_tool(viewport, ActiveTool::Select)
            .unwrap();
        engine
            .select_element_with_viewport_changes(viewport, id)
            .unwrap();
        let original = engine.model.element(id).unwrap().clone();
        let snapshot = engine.capture_session_snapshot();
        let bytes = engine.serialize_document_session().unwrap();
        let mut expected =
            Engine::from_serialized_document_session_with_config(&bytes, EngineConfig::default())
                .unwrap();
        let expected_view = expected.create_viewport(ViewportConfig::default()).unwrap();
        expected
            .editor
            .restore_history_selection(&expected.model, &snapshot);
        expected
            .duplicate_selected_with_viewport_changes(expected_view, Point::new(50.0, 60.0))
            .unwrap();
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Down,
            350.0,
            250.0,
            true,
        );
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            400.0,
            310.0,
            true,
        );
        assert_eq!(engine.model.element(id).unwrap(), &original, "{tool:?}");
        if tool == ActiveTool::Spotlight {
            let patch = engine.acquire_patch(viewport, None).unwrap();
            assert_eq!(patch.decoration.spotlight_ops[0].insert_items.len(), 2);
        } else {
            assert_eq!(scene(&engine, viewport).len(), 2, "{tool:?}");
        }
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Up,
            400.0,
            310.0,
            true,
        );
        assert_eq!(
            engine.model.document(),
            expected.model.document(),
            "{tool:?}"
        );
    }
}

#[test]
fn shift_alt_toggles_selection_and_empty_canvas_does_not_duplicate() {
    let (mut engine, viewport, id) = setup();
    engine
        .process_input_with_viewport_changes(
            viewport,
            InputEvent::Pointer(PointerEvent {
                pointer_id: 1,
                event_type: PointerEventType::Down,
                device: PointerDevice::Mouse,
                position: Point::new(400.0, 200.0),
                button: Some(PointerButton::Primary),
                buttons: PointerButtons(PointerButtons::PRIMARY),
                modifiers: Modifiers {
                    alt: true,
                    shift: true,
                    ..Modifiers::default()
                },
            }),
        )
        .unwrap();
    assert!(engine.selected_ids().is_empty());
    assert_eq!(engine.model.paint_order(), &[id]);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        400.0,
        200.0,
        true,
    );
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        50.0,
        50.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        100.0,
        100.0,
        true,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        100.0,
        100.0,
        true,
    );
    assert_eq!(engine.model.paint_order(), &[id]);
}

use super::duplicate_drag_tests::pointer;
use super::*;
use snow_draw_engine_display::SceneDisplayItem;
use snow_draw_engine_interaction::PointerEventType;

fn setup(zoom: f64) -> (Engine, ViewportId) {
    let mut engine = Engine::default();
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine
        .set_viewport_camera(
            viewport,
            Camera {
                center: Point::default(),
                zoom,
            },
        )
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::SerialNumber)
        .unwrap();
    (engine, viewport)
}

#[test]
fn serial_number_click_creates_on_press_and_ignores_jitter_at_any_zoom() {
    for zoom in [0.5, 1.0, 3.0] {
        let (mut engine, viewport) = setup(zoom);
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Down,
            400.0,
            300.0,
            false,
        );
        assert_eq!(engine.model.paint_order().len(), 1);
        let serial_id = engine.model.paint_order()[0];
        assert_eq!(
            engine.model.serial_number(serial_id).unwrap().center,
            Point::default()
        );
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            403.0,
            300.0,
            false,
        );
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Up,
            402.0,
            300.0,
            false,
        );
        assert_eq!(engine.model.paint_order().len(), 1);
        assert_eq!(
            engine.model.serial_number(serial_id).unwrap().center,
            Point::default()
        );
        assert_eq!(engine.take_text_edit_request(viewport).unwrap(), None);
    }
}

#[test]
fn serial_number_drag_previews_bound_text_and_edits_at_final_release_position() {
    for zoom in [0.5, 1.0, 3.0] {
        let (mut engine, viewport) = setup(zoom);
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Down,
            400.0,
            300.0,
            false,
        );
        let serial_id = engine.model.paint_order()[0];
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            404.0,
            300.0,
            false,
        );
        let text_id = engine
            .model
            .serial_number(serial_id)
            .unwrap()
            .text_element_id
            .unwrap();
        assert_eq!(engine.take_text_edit_request(viewport).unwrap(), None);
        let history = engine.history_state();
        // Cross back over the number and move in another direction after attaching.
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            320.0,
            240.0,
            false,
        );
        assert_eq!(
            engine.history_state(),
            history,
            "moves must not add undo entries"
        );
        let patch = engine.acquire_patch(viewport, None).unwrap();
        let text = patch
            .scene
            .ops
            .iter()
            .flat_map(|op| &op.insert_items)
            .find_map(|item| {
                if let SceneDisplayItem::Text(text) = item {
                    Some(text)
                } else {
                    None
                }
            })
            .unwrap();
        assert_eq!(
            Point::new(text.center_x, text.center_y),
            Point::new(-80.0 / zoom, -60.0 / zoom)
        );
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Up,
            250.0,
            220.0,
            false,
        );
        assert_eq!(
            engine.model.text(text_id).unwrap().center,
            Point::new(-150.0 / zoom, -80.0 / zoom)
        );
        assert_eq!(
            engine.model.serial_number(serial_id).unwrap().center,
            Point::default()
        );
        assert_eq!(
            engine.take_text_edit_request(viewport).unwrap(),
            Some(text_id)
        );
        assert_eq!(engine.take_text_edit_request(viewport).unwrap(), None);
    }
}

#[test]
fn serial_number_drag_applies_measured_label_layout_to_preview_and_release() {
    let (mut engine, viewport) = setup(1.0);
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
        404.0,
        300.0,
        false,
    );
    let serial_id = engine.model.paint_order()[0];
    let text_id = engine
        .model
        .serial_number(serial_id)
        .unwrap()
        .text_element_id
        .unwrap();

    let request = engine
        .serial_number_label_layout_request(viewport)
        .unwrap()
        .expect("attached label must request a measured layout");
    assert_eq!(request.text_id, text_id);
    assert_eq!(
        request.font_size,
        engine.model.serial_number(serial_id).unwrap().font_size
    );

    let history = engine.history_state();
    assert_eq!(
        engine
            .apply_serial_number_label_layout(
                viewport,
                text_id,
                snow_draw_engine_document::TextLayoutSize {
                    width: 31.0,
                    height: 36.0,
                }
            )
            .unwrap()
            .changed_viewports,
        vec![viewport]
    );
    assert_eq!(
        engine.history_state(),
        history,
        "measurement adds no undo entries"
    );
    assert_eq!(
        engine.serial_number_label_layout_request(viewport).unwrap(),
        None,
        "measurement is requested only once"
    );

    let patch = engine.acquire_patch(viewport, None).unwrap();
    let text = patch
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| {
            if let SceneDisplayItem::Text(text) = item {
                Some(text)
            } else {
                None
            }
        })
        .unwrap();
    assert_eq!(text.width, 31.0, "drag preview renders the measured layout");
    assert_eq!(text.height, 36.0);

    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        320.0,
        240.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        250.0,
        220.0,
        false,
    );
    let placed = engine.model.text(text_id).unwrap();
    assert_eq!(placed.center, Point::new(-150.0, -80.0));
    assert_eq!(placed.width, 31.0, "release persists the measured layout");
    assert_eq!(placed.height, 36.0);
}

#[test]
fn serial_number_release_without_move_still_attaches_text() {
    let (mut engine, viewport) = setup(1.0);
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
        PointerEventType::Up,
        500.0,
        400.0,
        false,
    );
    let serial = engine
        .model
        .serial_number(engine.model.paint_order()[0])
        .unwrap();
    let text_id = serial.text_element_id.unwrap();
    assert_eq!(serial.center, Point::default());
    assert_eq!(
        engine.model.text(text_id).unwrap().center,
        Point::new(100.0, 100.0)
    );
    assert_eq!(
        engine.take_text_edit_request(viewport).unwrap(),
        Some(text_id)
    );
}

#[test]
fn serial_number_cancel_and_focus_loss_do_not_request_editing() {
    for focus_lost in [false, true] {
        let (mut engine, viewport) = setup(1.0);
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
            480.0,
            300.0,
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
        if focus_lost {
            engine
                .process_input(viewport, InputEvent::FocusLost)
                .unwrap();
        } else {
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Cancel,
                500.0,
                400.0,
                false,
            );
        }
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Up,
            500.0,
            400.0,
            false,
        );
        assert_eq!(engine.take_text_edit_request(viewport).unwrap(), None);
        assert_eq!(engine.model.paint_order().len(), 2);
    }
}

#[test]
fn serial_number_existing_selection_drag_does_not_create_bound_text() {
    let (mut engine, viewport) = setup(1.0);
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
        PointerEventType::Up,
        400.0,
        300.0,
        false,
    );
    let serial_id = engine.model.paint_order()[0];
    engine
        .select_element_with_viewport_changes(viewport, serial_id)
        .unwrap();
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
        350.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        450.0,
        350.0,
        false,
    );
    assert_eq!(engine.model.paint_order().len(), 1);
    assert_eq!(
        engine.model.serial_number(serial_id).unwrap().center,
        Point::new(50.0, 50.0)
    );
    assert_eq!(engine.take_text_edit_request(viewport).unwrap(), None);
    // An empty-canvas press clears that selection and creates immediately.
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        200.0,
        150.0,
        false,
    );
    assert_eq!(engine.model.paint_order().len(), 2);
}

#[test]
fn serial_number_drag_ignores_other_pointer_and_secondary_release() {
    use snow_draw_engine_interaction::{
        Modifiers, PointerButton, PointerButtons, PointerDevice, PointerEvent,
    };
    let (mut engine, viewport) = setup(1.0);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        400.0,
        300.0,
        false,
    );
    for (pointer_id, event_type, button) in [
        (2, PointerEventType::Move, None),
        (2, PointerEventType::Up, Some(PointerButton::Primary)),
        (1, PointerEventType::Up, Some(PointerButton::Secondary)),
    ] {
        engine
            .process_input(
                viewport,
                InputEvent::Pointer(PointerEvent {
                    pointer_id,
                    event_type,
                    device: PointerDevice::Mouse,
                    position: Point::new(600.0, 450.0),
                    button,
                    buttons: PointerButtons(PointerButtons::PRIMARY),
                    modifiers: Modifiers::default(),
                }),
            )
            .unwrap();
        assert_eq!(engine.model.paint_order().len(), 1);
        assert_eq!(engine.take_text_edit_request(viewport).unwrap(), None);
    }
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        500.0,
        400.0,
        false,
    );
    assert!(engine.take_text_edit_request(viewport).unwrap().is_some());
}

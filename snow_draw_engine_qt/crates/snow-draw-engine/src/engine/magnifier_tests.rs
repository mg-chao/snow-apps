use super::duplicate_drag_tests::pointer;
use super::*;
use snow_draw_engine_core::CornerRadii;
use snow_draw_engine_display::{OverlayDisplayItem, SceneDisplayItem, UiShapeKind};
use snow_draw_engine_document::{HighlightShape, Transaction};
use snow_draw_engine_editor::{
    MAGNIFIER_STYLE_PROPERTY_ALL, MAGNIFIER_STYLE_PROPERTY_CORNER_RADII,
    MAGNIFIER_STYLE_PROPERTY_FACTOR, MagnifierStyle,
};
use snow_draw_engine_interaction::{Modifiers, PointerEventType, WheelDeltaKind, WheelEvent};

fn fixture(shape: HighlightShape, factor: f64) -> (Engine, ViewportId, ElementId) {
    let mut config = EngineConfig::default();
    config.style_defaults.editor.magnifier.shape = shape;
    config.style_defaults.editor.magnifier.factor = factor;
    let mut engine = Engine::new(config);
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Magnifier)
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        350.0,
        260.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        450.0,
        340.0,
        false,
    );
    let preview = engine.acquire_patch(viewport, None).unwrap();
    assert!(
        preview
            .scene
            .ops
            .iter()
            .flat_map(|op| &op.insert_items)
            .any(|item| matches!(item, SceneDisplayItem::Magnifier(_)))
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        450.0,
        340.0,
        false,
    );
    let id = engine.model.paint_order()[0];
    (engine, viewport, id)
}
fn select(engine: &mut Engine, viewport: ViewportId, id: ElementId) {
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
}
fn drag(engine: &mut Engine, viewport: ViewportId, from: Point<f64>, to: Point<f64>) {
    pointer(
        engine,
        viewport,
        PointerEventType::Down,
        from.x + 400.0,
        from.y + 300.0,
        false,
    );
    pointer(
        engine,
        viewport,
        PointerEventType::Move,
        to.x + 400.0,
        to.y + 300.0,
        false,
    );
    pointer(
        engine,
        viewport,
        PointerEventType::Up,
        to.x + 400.0,
        to.y + 300.0,
        false,
    );
}
fn handle(engine: &mut Engine, viewport: ViewportId, kind: UiShapeKind) -> Point<f64> {
    let patch = engine.acquire_patch(viewport, None).unwrap();
    patch
        .overlay
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| match item {
            OverlayDisplayItem::Rectangle(item) if item.kind == kind => {
                Some(Point::new(item.center_x, item.center_y))
            }
            _ => None,
        })
        .unwrap()
}

fn scene_magnifiers(
    engine: &Engine,
    viewport: ViewportId,
) -> Vec<snow_draw_engine_display::MagnifierDisplayItem> {
    engine
        .acquire_patch(viewport, None)
        .unwrap()
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .filter_map(|item| match item {
            SceneDisplayItem::Magnifier(value) => Some(value.clone()),
            _ => None,
        })
        .collect()
}

#[test]
fn magnifier_creates_all_shapes_as_one_pristine_image_item() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, id) = fixture(shape, 2.0);
        let value = *engine.model.magnifier(id).unwrap();
        assert_eq!(value.source_center, value.magnified_center);
        assert_eq!((value.width, value.height), (100.0, 80.0));
        assert_eq!(value.shape, shape);
        assert_eq!(value.corner_radii, CornerRadii::splat(6.0));
        assert!(value.show_leader && value.leader().is_none());
        let patch = engine.acquire_patch(viewport, None).unwrap();
        let items = patch
            .scene
            .ops
            .iter()
            .flat_map(|op| &op.insert_items)
            .collect::<Vec<_>>();
        assert_eq!(items.len(), 1);
        let SceneDisplayItem::Magnifier(item) = items[0] else {
            panic!("one logical lens")
        };
        assert_eq!((item.lens.width, item.lens.height), (200.0, 160.0));
        assert_eq!(item.lens.corner_radii, CornerRadii::splat(6.0));
        assert_eq!((item.source_width, item.source_height), (100.0, 80.0));
        select(&mut engine, viewport, id);
        let patch = engine.acquire_patch(viewport, None).unwrap();
        assert!(patch.overlay.ops.iter().flat_map(|op|&op.insert_items).any(|item|matches!(item,OverlayDisplayItem::FocusConnection(item) if item.stroke_style==snow_draw_engine_core::arrow::StrokeStyle::Dashed)));
    }
}

#[test]
fn magnifier_source_wins_overlap_and_lens_moves_independently_with_history() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, id) = fixture(shape, 2.0);
        select(&mut engine, viewport, id);
        let before = *engine.model.magnifier(id).unwrap();
        drag(
            &mut engine,
            viewport,
            Point::new(0.0, 0.0),
            Point::new(20.0, 15.0),
        );
        let moved = *engine.model.magnifier(id).unwrap();
        assert_eq!(moved.source_center, Point::new(20.0, 15.0));
        assert_eq!(moved.magnified_center, moved.source_center);
        assert!(engine.undo().unwrap());
        assert_eq!(*engine.model.magnifier(id).unwrap(), before);
        assert!(engine.redo().unwrap());
        // The lens's exposed horizontal interior remains outside the source.
        let lens_from = Point::new(95.0, 15.0);
        drag(&mut engine, viewport, lens_from, Point::new(185.0, 55.0));
        let lens = *engine.model.magnifier(id).unwrap();
        assert_eq!(lens.source_center, moved.source_center);
        assert_eq!(lens.magnified_center, Point::new(110.0, 55.0));
        assert!(engine.undo().unwrap());
        assert_eq!(*engine.model.magnifier(id).unwrap(), moved);
    }
}

#[test]
fn magnifier_corner_handle_edits_geometry_with_undo_and_redo() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let original = *engine.model.magnifier(id).unwrap();
    let from = handle(
        &mut engine,
        viewport,
        UiShapeKind::SelectionCornerRadiusHandle,
    );
    drag(
        &mut engine,
        viewport,
        from,
        Point::new(from.x + 15.0, from.y + 15.0),
    );
    let changed = *engine.model.magnifier(id).unwrap();
    assert_ne!(changed.corner_radii, original.corner_radii);
    assert_eq!(
        (changed.width, changed.height),
        (original.width, original.height)
    );
    assert_eq!(changed.source_center, original.source_center);
    assert_eq!(changed.magnified_center, original.magnified_center);
    assert!(engine.undo().unwrap());
    assert_eq!(*engine.model.magnifier(id).unwrap(), original);
    assert!(engine.redo().unwrap());
    assert_eq!(*engine.model.magnifier(id).unwrap(), changed);
}

#[test]
fn magnifier_mixed_corner_patch_preserves_factors_and_defaults() {
    let (mut engine, viewport, first_id) = fixture(HighlightShape::Rectangle, 2.0);
    let first = *engine.model.magnifier(first_id).unwrap();
    let second_id = engine.model.peek_next_element_id();
    let second = snow_draw_engine_document::MagnifierData {
        source_center: Point::new(400.0, 0.0),
        magnified_center: Point::new(400.0, 0.0),
        factor: 3.0,
        corner_radii: CornerRadii::splat(12.0),
        ..first
    };
    let mut transaction = Transaction::new("second magnifier");
    transaction.insert_magnifier(second_id, Default::default(), second);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    let snapshot = serde_json::from_value(serde_json::json!({
        "selectedIds": [first_id, second_id], "primaryId": first_id
    }))
    .unwrap();
    engine
        .editor
        .restore_history_selection(&engine.model, &snapshot);
    engine.refresh_all_viewports().unwrap();
    let state = engine.viewport_style_toolbar_state(viewport).unwrap();
    assert_eq!(
        state.magnifier_style_mixed,
        MAGNIFIER_STYLE_PROPERTY_CORNER_RADII | MAGNIFIER_STYLE_PROPERTY_FACTOR
    );
    let style = MagnifierStyle {
        corner_radii: CornerRadii::splat(8.0),
        factor: 9.0,
        ..Default::default()
    };
    engine
        .set_viewport_magnifier_style_patch(
            viewport,
            style,
            MAGNIFIER_STYLE_PROPERTY_CORNER_RADII,
            false,
        )
        .unwrap();
    assert_eq!(
        engine.model.magnifier(first_id).unwrap().corner_radii,
        CornerRadii::splat(8.0)
    );
    assert_eq!(
        engine.model.magnifier(second_id).unwrap().corner_radii,
        CornerRadii::splat(8.0)
    );
    assert_eq!(engine.model.magnifier(first_id).unwrap().factor, 2.0);
    assert_eq!(engine.model.magnifier(second_id).unwrap().factor, 3.0);
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style_mixed,
        MAGNIFIER_STYLE_PROPERTY_FACTOR
    );
    assert!(engine.undo().unwrap());
    assert_eq!(*engine.model.magnifier(first_id).unwrap(), first);
    assert_eq!(*engine.model.magnifier(second_id).unwrap(), second);
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style
            .corner_radii,
        CornerRadii::splat(6.0)
    );
}

#[test]
fn magnifier_factor_one_external_grip_can_separate_coincident_regions() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Ellipse, 1.0);
    select(&mut engine, viewport, id);
    let from = handle(&mut engine, viewport, UiShapeKind::MagnifierMoveHandle);
    drag(
        &mut engine,
        viewport,
        from,
        Point::new(from.x + 140.0, from.y + 20.0),
    );
    let value = engine.model.magnifier(id).unwrap();
    assert_eq!(value.source_center, Point::new(0.0, 0.0));
    assert_eq!(value.magnified_center, Point::new(140.0, 20.0));
    assert!(value.leader().is_some());
}

#[test]
fn magnifier_single_resize_and_rotation_edit_source_with_fixed_lens_center() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let grip = handle(&mut engine, viewport, UiShapeKind::MagnifierMoveHandle);
    drag(
        &mut engine,
        viewport,
        grip,
        Point::new(grip.x + 180.0, grip.y),
    );
    let before = *engine.model.magnifier(id).unwrap();
    let from = handle(&mut engine, viewport, UiShapeKind::SelectionResizeHandle);
    drag(
        &mut engine,
        viewport,
        from,
        Point::new(from.x - 30.0, from.y - 20.0),
    );
    let resized = *engine.model.magnifier(id).unwrap();
    assert_eq!(resized.magnified_center, before.magnified_center);
    assert_eq!(resized.factor, before.factor);
    assert!(resized.width > before.width && resized.height > before.height);
    let rotation = handle(&mut engine, viewport, UiShapeKind::SelectionRotationHandle);
    let relative = Point::new(
        rotation.x - resized.source_center.x,
        rotation.y - resized.source_center.y,
    );
    let to = Point::new(
        resized.source_center.x - relative.y,
        resized.source_center.y + relative.x,
    );
    drag(&mut engine, viewport, rotation, to);
    let rotated = *engine.model.magnifier(id).unwrap();
    assert_eq!(rotated.source_center, resized.source_center);
    assert_eq!(rotated.magnified_center, resized.magnified_center);
    assert!((rotated.rotation - std::f64::consts::FRAC_PI_2).abs() < 1e-9);
    assert!(engine.undo().unwrap());
    assert_eq!(*engine.model.magnifier(id).unwrap(), resized);
}

#[test]
fn magnifier_lens_cancel_does_not_commit_and_z_order_remains_intact() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let before = *engine.model.magnifier(id).unwrap();
    let from = handle(&mut engine, viewport, UiShapeKind::MagnifierMoveHandle);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        from.x + 400.0,
        from.y + 300.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        from.x + 500.0,
        from.y + 330.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Cancel,
        from.x + 500.0,
        from.y + 330.0,
        false,
    );
    assert_eq!(*engine.model.magnifier(id).unwrap(), before);
    let top = engine.model.peek_next_element_id();
    let mut rect = before.source_rect();
    rect.width = 20.0;
    rect.height = 20.0;
    let mut transaction = Transaction::new("top annotation");
    transaction.insert_rectangle(top, Default::default(), rect);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    assert_eq!(
        engine
            .hit_quick_selection_at(
                viewport,
                Point::new(0.0, 0.0),
                snow_draw_engine_interaction::PointerButton::Primary
            )
            .unwrap(),
        Some(top)
    );
}

#[test]
fn magnifier_selected_styles_defaults_and_sessions_remain_independent() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Diamond, 2.0);
    select(&mut engine, viewport, id);
    let style = MagnifierStyle {
        factor: 3.5,
        show_leader: false,
        stroke_width: 0.0,
        corner_radii: CornerRadii::splat(12.0),
        ..Default::default()
    };
    engine
        .set_viewport_magnifier_style_patch(viewport, style, MAGNIFIER_STYLE_PROPERTY_ALL, false)
        .unwrap();
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 3.5);
    let defaults = MagnifierStyle {
        factor: 8.0,
        corner_radii: CornerRadii::splat(3.0),
        ..Default::default()
    };
    engine
        .set_viewport_magnifier_style_patch(
            viewport,
            defaults,
            MAGNIFIER_STYLE_PROPERTY_FACTOR | MAGNIFIER_STYLE_PROPERTY_CORNER_RADII,
            true,
        )
        .unwrap();
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 3.5);
    assert_eq!(
        engine.model.magnifier(id).unwrap().corner_radii,
        CornerRadii::splat(12.0)
    );
    let bytes = engine.serialize_document_session().unwrap();
    assert_eq!(
        serde_json::from_slice::<serde_json::Value>(&bytes).unwrap()["schemaVersion"],
        11
    );
    let mut restored =
        Engine::from_serialized_document_session_with_config(&bytes, EngineConfig::default())
            .unwrap();
    assert_eq!(
        *restored.model.magnifier(id).unwrap(),
        *engine.model.magnifier(id).unwrap()
    );
    let rv = restored.create_viewport(ViewportConfig::default()).unwrap();
    restored
        .set_viewport_active_tool(rv, ActiveTool::Magnifier)
        .unwrap();
    assert_eq!(
        restored
            .viewport_style_toolbar_state(rv)
            .unwrap()
            .magnifier_style
            .corner_radii,
        CornerRadii::splat(3.0)
    );
    assert_eq!(
        restored
            .viewport_style_toolbar_state(rv)
            .unwrap()
            .magnifier_style
            .factor,
        8.0
    );
    let history = engine.serialize_document_history().unwrap();
    assert_eq!(
        serde_json::from_slice::<serde_json::Value>(&history).unwrap()["schemaVersion"],
        7
    );
    let restored =
        Engine::from_serialized_document_history_with_config(&history, EngineConfig::default())
            .unwrap();
    assert_eq!(
        *restored.model.magnifier(id).unwrap(),
        *engine.model.magnifier(id).unwrap()
    );
    let template = engine.serialize_selected_draw_template().unwrap();
    assert_eq!(
        serde_json::from_slice::<serde_json::Value>(&template).unwrap()["schemaVersion"],
        2
    );
    engine
        .insert_draw_template_with_viewport_changes(viewport, &template, Point::new(150.0, 120.0))
        .unwrap();
    assert_eq!(engine.model.paint_order().len(), 2);
    let copy = engine
        .model
        .magnifier(engine.model.paint_order()[1])
        .unwrap();
    assert_eq!((copy.width, copy.height, copy.factor), (100.0, 80.0, 3.5));
}

#[test]
fn magnifier_legacy_session_without_defaults_loads_and_invalid_defaults_fail() {
    let engine = Engine::default();
    let mut json: serde_json::Value =
        serde_json::from_slice(&engine.serialize_document_session().unwrap()).unwrap();
    json["schemaVersion"] = 10.into();
    json["editor"].as_object_mut().unwrap().remove("magnifier");
    let mut legacy = Engine::from_serialized_document_session_with_config(
        &serde_json::to_vec(&json).unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
    let v = legacy.create_viewport(ViewportConfig::default()).unwrap();
    assert_eq!(
        legacy
            .viewport_style_toolbar_state(v)
            .unwrap()
            .magnifier_style
            .factor,
        2.0
    );
    json["editor"]["magnifier"] = serde_json::to_value(MagnifierStyle {
        factor: 11.0,
        ..Default::default()
    })
    .unwrap();
    assert!(
        Engine::from_serialized_document_session_with_config(
            &serde_json::to_vec(&json).unwrap(),
            EngineConfig::default()
        )
        .is_err()
    );
}

#[test]
fn magnifier_mixed_selection_resizes_and_rotates_both_region_centers_coherently() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let grip = handle(&mut engine, viewport, UiShapeKind::MagnifierMoveHandle);
    drag(
        &mut engine,
        viewport,
        grip,
        Point::new(grip.x + 180.0, grip.y),
    );
    let rectangle_id = engine.model.peek_next_element_id();
    let mut rectangle = engine.model.magnifier(id).unwrap().source_rect();
    rectangle.center = Point::new(400.0, 0.0);
    rectangle.width = 20.0;
    rectangle.height = 20.0;
    let mut transaction = Transaction::new("mixed selection member");
    transaction.insert_rectangle(rectangle_id, Default::default(), rectangle);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    let snapshot = serde_json::from_value(serde_json::json!({
        "selectedIds": [id, rectangle_id], "primaryId": id
    }))
    .unwrap();
    engine
        .editor
        .restore_history_selection(&engine.model, &snapshot);
    engine.refresh_all_viewports().unwrap();
    let from = handle(&mut engine, viewport, UiShapeKind::SelectionResizeHandle);
    drag(
        &mut engine,
        viewport,
        from,
        Point::new(from.x - 100.0, from.y - 60.0),
    );
    let resized = *engine.model.magnifier(id).unwrap();
    let sx = resized.width / 100.0;
    assert!(sx > 1.0 && resized.height > 80.0);
    assert!((resized.magnified_center.x - resized.source_center.x - 180.0 * sx).abs() < 1e-8);
    assert!(
        (engine.model.rectangle(rectangle_id).unwrap().center.x
            - resized.source_center.x
            - 400.0 * sx)
            .abs()
            < 1e-8
    );
    assert_eq!(resized.factor, 2.0);
    let bounds = engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view)
        .selection_bounds
        .unwrap();
    let from = handle(&mut engine, viewport, UiShapeKind::SelectionRotationHandle);
    let dx = from.x - bounds.center.x;
    let dy = from.y - bounds.center.y;
    drag(
        &mut engine,
        viewport,
        from,
        Point::new(bounds.center.x - dy, bounds.center.y + dx),
    );
    let rotated = *engine.model.magnifier(id).unwrap();
    assert!((rotated.rotation - std::f64::consts::FRAC_PI_2).abs() < 1e-8);
    assert!((rotated.magnified_center.x - rotated.source_center.x).abs() < 1e-8);
    assert!((rotated.magnified_center.y - rotated.source_center.y - 180.0 * sx).abs() < 1e-8);
    assert_eq!(
        (rotated.width, rotated.height, rotated.factor),
        (resized.width, resized.height, 2.0)
    );
}

#[test]
fn magnifier_wheel_clamps_factor_without_changing_border_camera_or_defaults() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Ellipse, 2.0);
    select(&mut engine, viewport, id);
    let before = *engine.model.magnifier(id).unwrap();
    let camera = engine.viewport_slot(viewport).unwrap().view;
    for (delta, expected) in [(360.0, 2.3), (24000.0, 10.0), (-24000.0, 1.0)] {
        assert!(
            engine
                .process_input(
                    viewport,
                    InputEvent::Wheel(WheelEvent {
                        position: Point::new(400.0, 300.0),
                        delta: snow_draw_engine_core::Vector2 { x: 0.0, y: delta },
                        delta_kind: WheelDeltaKind::Angle,
                        modifiers: Modifiers::default(),
                    })
                )
                .unwrap()
                .consumed
        );
        assert_eq!(engine.model.magnifier(id).unwrap().factor, expected);
    }
    let after = *engine.model.magnifier(id).unwrap();
    assert_eq!(
        (after.stroke, after.stroke_width),
        (before.stroke, before.stroke_width)
    );
    assert_eq!(
        (after.source_center, after.magnified_center),
        (before.source_center, before.magnified_center)
    );
    assert_eq!(engine.viewport_slot(viewport).unwrap().view, camera);
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Magnifier)
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style
            .factor,
        2.0
    );
    assert_eq!(
        engine.set_viewport_magnifier_style_patch(
            viewport,
            MagnifierStyle {
                factor: 8.0,
                ..Default::default()
            },
            MAGNIFIER_STYLE_PROPERTY_FACTOR,
            false
        ),
        Err(ErrorCode::InvalidState)
    );
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style
            .factor,
        2.0
    );
}

#[test]
fn magnifier_selection_preview_and_commit_use_full_pointer_down_snapshot() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let original = *engine.model.magnifier(id).unwrap();
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
        420.0,
        315.0,
        false,
    );
    let mut changed = original;
    changed.factor = 6.0;
    changed.magnified_center = Point::new(200.0, 100.0);
    let mut transaction = Transaction::new("concurrent style update");
    transaction.update_magnifier(id, changed);
    engine.model.apply_transaction(transaction).unwrap();
    let patch = engine.acquire_patch(viewport, None).unwrap();
    let lens = patch
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| {
            if let SceneDisplayItem::Magnifier(value) = item {
                Some(value)
            } else {
                None
            }
        })
        .unwrap();
    assert_eq!(lens.factor, original.factor);
    assert_eq!((lens.lens.center_x, lens.lens.center_y), (20.0, 15.0));
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        420.0,
        315.0,
        false,
    );
    assert_eq!(
        *engine.model.magnifier(id).unwrap(),
        original.translated(Point::new(20.0, 15.0))
    );
}

#[test]
fn magnifier_actual_shape_source_hit_precedes_lens_inside_generic_selection_frame() {
    for (shape, point) in [
        (HighlightShape::Ellipse, Point::new(40.0, 30.0)),
        (HighlightShape::Diamond, Point::new(30.0, 25.0)),
    ] {
        let (mut engine, viewport, id) = fixture(shape, 2.0);
        select(&mut engine, viewport, id);
        let before = *engine.model.magnifier(id).unwrap();
        drag(
            &mut engine,
            viewport,
            point,
            Point::new(point.x + 100.0, point.y),
        );
        let moved = *engine.model.magnifier(id).unwrap();
        assert_eq!(moved.source_center, before.source_center);
        assert_eq!(moved.magnified_center, Point::new(100.0, 0.0));
    }
}

#[test]
fn magnifier_source_alt_drag_duplicates_both_regions_as_one_history_entry() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let original = *engine.model.magnifier(id).unwrap();
    for (kind, x, y) in [
        (PointerEventType::Down, 400.0, 300.0),
        (PointerEventType::Move, 430.0, 320.0),
        (PointerEventType::Up, 430.0, 320.0),
    ] {
        pointer(&mut engine, viewport, kind, x, y, true);
    }
    assert_eq!(engine.model.paint_order().len(), 2);
    assert_eq!(*engine.model.magnifier(id).unwrap(), original);
    let copy = engine.model.paint_order()[1];
    assert_eq!(
        *engine.model.magnifier(copy).unwrap(),
        original.translated(Point::new(30.0, 20.0))
    );
    assert!(engine.undo().unwrap());
    assert_eq!(engine.model.paint_order(), &[id]);
}

#[test]
fn magnifier_source_alt_drag_preview_keeps_original_stationary() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, id) = fixture(shape, 2.0);
        select(&mut engine, viewport, id);
        let original = scene_magnifiers(&engine, viewport).remove(0);
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
            430.0,
            320.0,
            true,
        );
        let preview = scene_magnifiers(&engine, viewport);
        assert_eq!(preview.len(), 2);
        assert_eq!(
            preview
                .iter()
                .find(|value| value.lens.id == original.lens.id),
            Some(&original),
            "copying must keep the committed original in place"
        );
        let copy = preview
            .iter()
            .find(|value| value.lens.id != original.lens.id)
            .unwrap();
        assert_eq!(copy.source_center, Point::new(30.0, 20.0));
        assert_eq!((copy.lens.center_x, copy.lens.center_y), (30.0, 20.0));
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Up,
            430.0,
            320.0,
            true,
        );
        assert_eq!(scene_magnifiers(&engine, viewport), preview);
    }
}

#[test]
fn magnifier_pending_eraser_preview_fades_lens_and_leader_without_changing_geometry() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, id) = fixture(shape, 2.0);
        select(&mut engine, viewport, id);
        let grip = handle(&mut engine, viewport, UiShapeKind::MagnifierMoveHandle);
        drag(
            &mut engine,
            viewport,
            grip,
            Point::new(grip.x + 250.0, grip.y),
        );
        engine
            .set_selected_opacity_with_viewport_changes(viewport, 0.6)
            .unwrap();
        let original = scene_magnifiers(&engine, viewport).remove(0);
        assert!(original.leader.is_some());
        engine
            .set_viewport_active_tool(viewport, ActiveTool::Eraser)
            .unwrap();
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Down,
            650.0,
            300.0,
            false,
        );
        let mut expected = original.clone();
        expected.lens.opacity *= 0.5;
        expected.leader.as_mut().unwrap().opacity *= 0.5;
        assert_eq!(scene_magnifiers(&engine, viewport), vec![expected]);
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Up,
            650.0,
            300.0,
            false,
        );
        assert!(scene_magnifiers(&engine, viewport).is_empty());
        assert!(engine.undo().unwrap());
        assert_eq!(scene_magnifiers(&engine, viewport), vec![original]);
    }
}

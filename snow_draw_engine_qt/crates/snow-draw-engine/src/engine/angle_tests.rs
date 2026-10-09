use super::*;
use crate::StyleToolbarSource;
use crate::{
    ANGLE_STYLE_PROPERTY_ALL, ANGLE_STYLE_PROPERTY_DECIMAL_PLACES, ANGLE_STYLE_PROPERTY_UNIT,
    AngleStyle, AngleUnit, SceneDisplayItem, TextLayoutSize,
};
use serde_json::json;
use snow_draw_engine_document::{ElementKind, angle_value};
use snow_draw_engine_interaction::{
    KeyCode, KeyEvent, KeyEventType, Modifiers, PointerButton, PointerButtons, PointerDevice,
    PointerEvent, PointerEventType, WheelDeltaKind, WheelEvent,
};

fn setup() -> (Engine, ViewportId) {
    let mut engine = Engine::default();
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Angle)
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
    engine
        .process_input(
            viewport,
            InputEvent::Pointer(PointerEvent {
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
                modifiers: Modifiers::default(),
            }),
        )
        .unwrap();
}
fn click(engine: &mut Engine, viewport: ViewportId, point: Point<f64>) {
    pointer(engine, viewport, PointerEventType::Down, point);
    pointer(engine, viewport, PointerEventType::Up, point);
}
fn create(engine: &mut Engine, viewport: ViewportId) -> ElementId {
    click(engine, viewport, Point::new(100.0, 0.0));
    click(engine, viewport, Point::new(0.0, 0.0));
    assert!(engine.model.paint_order().is_empty());
    click(engine, viewport, Point::new(0.0, -100.0));
    engine.model.paint_order()[0]
}
fn label(engine: &Engine, id: ElementId) -> &snow_draw_engine_document::TextData {
    engine
        .model
        .text(engine.model.bound_text_id_for_arrow(id).unwrap())
        .unwrap()
}
fn select(engine: &mut Engine, ids: &[ElementId]) {
    let snapshot =
        serde_json::from_value(json!({"selectedIds":ids,"primaryId":ids.first()})).unwrap();
    engine
        .editor
        .restore_history_selection(&engine.model, &snapshot);
    engine.refresh_all_viewports().unwrap();
}
fn apply(engine: &mut Engine, operations: serde_json::Value) -> Result<Vec<ElementId>, ErrorCode> {
    let (_, bytes) = engine.apply_annotation_json(
        &serde_json::to_vec(&json!({"version":1,"operations":operations})).unwrap(),
    )?;
    let result: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
    Ok(serde_json::from_value(result["created_element_ids"].clone()).unwrap())
}

#[test]
fn angle_three_clicks_create_an_owned_pair_and_one_history_step() {
    let (mut engine, viewport) = setup();
    let id = create(&mut engine, viewport);
    let arrow = engine.model.arrow(id).unwrap();
    assert_eq!(arrow.element_kind(), ElementKind::Angle);
    assert_eq!(arrow.points.len(), 3);
    assert_eq!(
        arrow.stroke,
        snow_draw_engine_core::ColorRgba8 {
            r: 245,
            g: 34,
            b: 45,
            a: 255
        }
    );
    assert_eq!(label(&engine, id).text, "90\u{00b0}");
    assert_eq!(engine.model.paint_order().len(), 2);
    assert!(engine.undo().unwrap());
    assert!(engine.model.paint_order().is_empty());
    assert!(!engine.history_state().can_undo);
    assert!(engine.redo().unwrap());
    assert_eq!(label(&engine, id).text, "90\u{00b0}");
}

#[test]
fn angle_short_ray_double_click_still_requires_three_clicks() {
    let (mut engine, viewport) = setup();
    click(&mut engine, viewport, Point::new(2.0, 0.0));
    pointer(
        &mut engine,
        viewport,
        PointerEventType::DoubleClick,
        Point::new(0.0, 0.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(0.0, 0.0),
    );
    assert!(engine.model.paint_order().is_empty());
    click(&mut engine, viewport, Point::new(0.0, -2.0));
    let id = engine.model.paint_order()[0];
    assert_eq!(label(&engine, id).text, "90\u{00b0}");
    assert_eq!(
        engine.model.arrow(id).unwrap().global_points(),
        vec![
            Point::new(2.0, 0.0),
            Point::new(0.0, 0.0),
            Point::new(0.0, -2.0)
        ]
    );
}

#[test]
fn angle_wheel_draft_lock_survives_same_position_move_and_final_click() {
    let (mut engine, viewport) = setup();
    click(&mut engine, viewport, Point::new(100.0, 0.0));
    click(&mut engine, viewport, Point::new(0.0, 0.0));
    let final_position = Point::new(0.0, -100.0);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        final_position,
    );
    engine
        .adjust_viewport_angle_value(viewport, 15_f64.to_radians())
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        final_position,
    );
    click(&mut engine, viewport, final_position);
    let id = engine.model.paint_order()[0];
    assert_eq!(label(&engine, id).text, "105\u{00b0}");
    let arrow = engine.model.arrow(id).unwrap();
    let points = arrow.global_points();
    assert_eq!(points[0], Point::new(100.0, 0.0));
    assert_eq!(points[1], Point::new(0.0, 0.0));
    assert!((points[2].x.hypot(points[2].y) - 100.0).abs() < 1e-9);
    assert!(engine.undo().unwrap());
    assert!(!engine.history_state().can_undo);
}

#[test]
fn angle_wheel_closed_interval_retains_full_turn_after_session_round_trip() {
    let (mut engine, viewport) = setup();
    let id = create(&mut engine, viewport);
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    select(&mut engine, &[id]);
    engine
        .adjust_viewport_angle_value(viewport, 1000_f64.to_radians())
        .unwrap();
    assert_eq!(label(&engine, id).text, "360\u{00b0}");
    assert!(engine.model.arrow(id).unwrap().angle.unwrap().full_turn);
    let restored = Engine::from_serialized_document_session_with_config(
        &engine.serialize_document_session().unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
    assert_eq!(label(&restored, id).text, "360\u{00b0}");
    engine
        .adjust_viewport_angle_value(viewport, -1000_f64.to_radians())
        .unwrap();
    assert_eq!(label(&engine, id).text, "0\u{00b0}");
    assert!(!engine.model.arrow(id).unwrap().angle.unwrap().full_turn);
}

#[test]
fn angle_selected_style_patches_preserve_creation_defaults() {
    let (mut engine, viewport) = setup();
    let defaults = engine
        .viewport_style_toolbar_state(viewport)
        .unwrap()
        .angle_style;
    let id = create(&mut engine, viewport);
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    select(&mut engine, &[id]);
    let style = AngleStyle {
        unit: AngleUnit::Radians,
        decimal_places: 3,
        stroke_width: 4.0,
        stroke: snow_draw_engine_core::ColorRgba8 {
            r: 1,
            g: 2,
            b: 3,
            a: 255,
        },
    };
    engine
        .set_viewport_angle_style_patch(viewport, style, ANGLE_STYLE_PROPERTY_ALL)
        .unwrap();
    assert_eq!(label(&engine, id).text, "1.571 rad");
    select(&mut engine, &[]);
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Angle)
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .angle_style,
        defaults
    );
}

#[test]
fn angle_homogeneous_selection_adjusts_together_and_mixed_selection_is_unchanged() {
    let (mut engine, viewport) = setup();
    let ids = apply(
        &mut engine,
        json!([
            {"type":"angle","points":[[100,0],[0,0],[0,-100]]},
            {"type":"angle","points":[[300,0],[200,0],[200,100]]},
            {"type":"arrow","points":[[400,0],[500,0]]}
        ]),
    )
    .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    select(&mut engine, &ids[..2]);
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .source,
        StyleToolbarSource::SelectedAngle
    );
    engine
        .adjust_viewport_angle_value(viewport, 10_f64.to_radians())
        .unwrap();
    assert_eq!(label(&engine, ids[0]).text, "100\u{00b0}");
    assert_eq!(label(&engine, ids[1]).text, "280\u{00b0}");
    select(&mut engine, &ids);
    let before = engine.serialize_document_session().unwrap();
    engine
        .adjust_viewport_angle_value(viewport, 10_f64.to_radians())
        .unwrap();
    assert_eq!(engine.serialize_document_session().unwrap(), before);
}

#[test]
fn angle_heterogeneous_selection_style_patch_updates_only_creation_defaults() {
    let (mut engine, viewport) = setup();
    let ids = apply(&mut engine, json!([
        {"type":"angle","points":[[100,0],[0,0],[0,-100]],"style":{"stroke_width":4,"decimal_places":1}},
        {"type":"rectangle","bounds":[200,0,100,100]}
    ])).unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    select(&mut engine, &ids);
    let owner = engine.model.arrow(ids[0]).unwrap().clone();
    let text = label(&engine, ids[0]).clone();
    let rectangle = *engine.model.rectangle(ids[1]).unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .angle_style,
        AngleStyle::default()
    );
    let defaults = AngleStyle {
        stroke_width: 7.0,
        unit: AngleUnit::Radians,
        decimal_places: 3,
        ..Default::default()
    };
    engine
        .set_viewport_angle_style_patch(viewport, defaults, ANGLE_STYLE_PROPERTY_ALL)
        .unwrap();
    let toolbar = engine.viewport_style_toolbar_state(viewport).unwrap();
    assert_ne!(toolbar.source, StyleToolbarSource::SelectedAngle);
    assert_eq!(toolbar.angle_style, defaults);
    assert_eq!(toolbar.angle_style_mixed, 0);
    assert_eq!(engine.model.arrow(ids[0]).unwrap(), &owner);
    assert_eq!(label(&engine, ids[0]), &text);
    assert_eq!(engine.model.rectangle(ids[1]).unwrap(), &rectangle);
    select(&mut engine, &[]);
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Angle)
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .angle_style,
        defaults
    );
}

#[test]
fn angle_invalid_annotations_and_style_patches_are_atomic() {
    let (mut engine, viewport) = setup();
    for operation in [
        json!({"type":"angle","points":[[0,0],[0,0],[100,0]]}),
        json!({"type":"angle","points":[[100,0],[0,0],[0,0]]}),
        json!({"type":"angle","points":[[100,0],[0,0],[0,-100]],"full_turn":true}),
        json!({"type":"angle","points":[[100,0],[0,0],[0,-100]],"style":{"decimal_places":4}}),
    ] {
        assert!(apply(&mut engine, json!([operation])).is_err());
        assert!(engine.model.paint_order().is_empty());
    }
    assert!(
        engine
            .adjust_viewport_angle_value(viewport, f64::NAN)
            .is_err()
    );
    assert!(
        engine
            .set_viewport_angle_style_patch(viewport, AngleStyle::default(), 16)
            .is_err()
    );
    engine
        .set_viewport_angle_style_patch(
            viewport,
            AngleStyle {
                unit: AngleUnit::Radians,
                ..Default::default()
            },
            ANGLE_STYLE_PROPERTY_UNIT,
        )
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .angle_style
            .unit,
        AngleUnit::Radians
    );
}

#[test]
fn angle_selected_wheel_burst_is_one_undo_step_and_reverse_restores_exact_geometry() {
    let (mut engine, viewport) = setup();
    let id = create(&mut engine, viewport);
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    select(&mut engine, &[id]);
    let original = engine.model.arrow(id).unwrap().clone();
    engine
        .adjust_viewport_angle_value(viewport, 1_f64.to_radians())
        .unwrap();
    engine
        .adjust_viewport_angle_value(viewport, 1_f64.to_radians())
        .unwrap();
    assert_eq!(label(&engine, id).text, "92\u{00b0}");
    engine.undo().unwrap();
    assert_eq!(engine.model.arrow(id).unwrap(), &original);
    engine.redo().unwrap();
    engine
        .adjust_viewport_angle_value(viewport, -2_f64.to_radians())
        .unwrap();
    assert!(
        (angle_value(engine.model.arrow(id).unwrap())
            .unwrap()
            .to_degrees()
            - 90.0)
            .abs()
            < 1e-9
    );
    Engine::from_serialized_document_session_with_config(
        &engine.serialize_document_session().unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
}

#[test]
fn angle_degenerate_clicks_do_not_advance_and_escape_cancels_the_whole_draft() {
    let (mut engine, viewport) = setup();
    click(&mut engine, viewport, Point::new(100.0, 0.0));
    click(&mut engine, viewport, Point::new(100.0, 0.0));
    click(&mut engine, viewport, Point::new(0.0, 0.0));
    click(&mut engine, viewport, Point::new(0.0, 0.0));
    assert!(engine.model.paint_order().is_empty());
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(0.0, -100.0),
    );
    engine
        .adjust_viewport_angle_value(viewport, 10_f64.to_radians())
        .unwrap();
    engine
        .process_input(
            viewport,
            InputEvent::Key(KeyEvent {
                event_type: KeyEventType::KeyDown,
                key_code: KeyCode::Escape,
                modifiers: Modifiers::default(),
                repeat: false,
            }),
        )
        .unwrap();
    assert!(engine.model.paint_order().is_empty());
    assert!(!engine.history_state().can_undo);
    let id = create(&mut engine, viewport);
    assert_eq!(label(&engine, id).text, "90\u{00b0}");
}

#[test]
fn angle_wheel_input_uses_notches_and_shift_fraction_without_changing_width_or_camera() {
    let (mut engine, viewport) = setup();
    let id = create(&mut engine, viewport);
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    select(&mut engine, &[id]);
    engine
        .set_viewport_angle_style_patch(
            viewport,
            AngleStyle {
                decimal_places: 3,
                ..Default::default()
            },
            ANGLE_STYLE_PROPERTY_DECIMAL_PLACES,
        )
        .unwrap();
    let before_view = engine.viewport_slot(viewport).unwrap().view;
    for (delta, shift) in [(360.0, false), (-240.0, true)] {
        let interaction = engine
            .process_input(
                viewport,
                InputEvent::Wheel(WheelEvent {
                    position: Point::new(400.0, 300.0),
                    delta: snow_draw_engine_core::Vector2 { x: 0.0, y: delta },
                    delta_kind: WheelDeltaKind::Angle,
                    modifiers: Modifiers {
                        shift,
                        ..Default::default()
                    },
                }),
            )
            .unwrap();
        assert!(interaction.consumed);
    }
    assert_eq!(label(&engine, id).text, "92.800\u{00b0}");
    assert_eq!(engine.model.arrow(id).unwrap().stroke_width, 2.0);
    assert_eq!(engine.viewport_slot(viewport).unwrap().view, before_view);
}

#[test]
fn angle_wheel_immediate_reverse_removes_history_and_restores_exact_owned_pair() {
    let (mut engine, viewport) = setup();
    let id = create(&mut engine, viewport);
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    select(&mut engine, &[id]);
    let original = engine.model.arrow(id).unwrap().clone();
    let original_label = label(&engine, id).clone();
    engine
        .adjust_viewport_angle_value(viewport, 1_f64.to_radians())
        .unwrap();
    engine
        .adjust_viewport_angle_value(viewport, -1_f64.to_radians())
        .unwrap();
    assert_eq!(engine.model.arrow(id).unwrap(), &original);
    assert_eq!(label(&engine, id), &original_label);
    Engine::from_serialized_document_session_with_config(
        &engine.serialize_document_session().unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
    engine.undo().unwrap();
    assert!(
        engine.model.paint_order().is_empty(),
        "only the creation step should remain"
    );
}

#[test]
fn angle_warm_metrics_immediate_reverse_restores_owned_labels_and_creation_history() {
    let (mut engine, viewport) = setup();
    let operations: Vec<_> = (0..3)
        .map(|index| {
            let x = f64::from(index) * 300.0;
            json!({"type":"angle","points":[[x+100.0,0],[x,0],[x,-100.0]]})
        })
        .collect();
    let ids = apply(&mut engine, json!(operations)).unwrap();
    let originals: Vec<_> = ids
        .iter()
        .map(|id| {
            (
                engine.model.arrow(*id).unwrap().clone(),
                label(&engine, *id).clone(),
            )
        })
        .collect();
    let measured = TextLayoutSize::new(64.0, 24.0);
    assert_ne!(originals[0].1.layout, measured);
    let metrics: Vec<_> = engine
        .arrow_text_layout_requests(viewport)
        .unwrap()
        .iter()
        .map(|request| (request.text_id, request.key, measured, 64.0))
        .collect();
    assert_eq!(metrics.len(), ids.len());
    engine
        .apply_arrow_text_measurements(viewport, &metrics)
        .unwrap();
    assert_eq!(label(&engine, ids[0]), &originals[0].1);
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    select(&mut engine, &ids[..2]);
    engine
        .adjust_viewport_angle_value(viewport, 0.1_f64.to_radians())
        .unwrap();
    assert_eq!(label(&engine, ids[0]).layout, measured);
    engine
        .adjust_viewport_angle_value(viewport, -0.1_f64.to_radians())
        .unwrap();
    for (id, (arrow, text)) in ids.iter().zip(&originals) {
        assert_eq!(engine.model.arrow(*id).unwrap(), arrow);
        assert_eq!(label(&engine, *id), text);
    }
    assert!(
        engine
            .arrow_text_layout_requests(viewport)
            .unwrap()
            .is_empty(),
        "host measurements remain reusable after restoring persisted label data"
    );
    let mut restored = Engine::from_serialized_document_session_with_config(
        &engine.serialize_document_session().unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
    for current in [&mut engine, &mut restored] {
        current.undo().unwrap();
        assert!(
            current.model.paint_order().is_empty(),
            "the net-zero burst must leave only the original import step"
        );
        current.redo().unwrap();
        for (id, (arrow, text)) in ids.iter().zip(&originals) {
            assert_eq!(current.model.arrow(*id).unwrap(), arrow);
            assert_eq!(label(current, *id), text);
        }
        Engine::from_serialized_document_session_with_config(
            &current.serialize_document_session().unwrap(),
            EngineConfig::default(),
        )
        .unwrap();
    }
}

#[test]
fn angle_wheel_selection_change_breaks_history_burst() {
    let (mut engine, viewport) = setup();
    let id = create(&mut engine, viewport);
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    engine
        .adjust_viewport_angle_value(viewport, 1_f64.to_radians())
        .unwrap();
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    engine
        .adjust_viewport_angle_value(viewport, 1_f64.to_radians())
        .unwrap();
    engine.undo().unwrap();
    assert_eq!(label(&engine, id).text, "91\u{00b0}");
    engine.undo().unwrap();
    assert_eq!(label(&engine, id).text, "90\u{00b0}");
}

#[test]
fn angle_measured_label_clearance_survives_scene_composition_commit_and_geometry_changes() {
    let (mut engine, viewport) = setup();
    let second = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine.set_viewport_surface_size(second, 800, 600).unwrap();
    click(&mut engine, viewport, Point::new(100.0, 0.0));
    click(&mut engine, viewport, Point::new(0.0, 0.0));
    let end = Point::new(0.0, -100.0);
    pointer(&mut engine, viewport, PointerEventType::Move, end);
    let request = engine
        .arrow_text_layout_requests(viewport)
        .unwrap()
        .remove(0);
    let size = TextLayoutSize::new(180.0, 24.0);
    let expected_offset = 27.0 + 180_f64.hypot(24.0) / 2.0 + 7.0;
    let cursors: Vec<_> = [viewport, second]
        .into_iter()
        .map(|id| {
            (
                id,
                engine.viewport_slot(id).unwrap().composer.current_cursor(),
            )
        })
        .collect();
    engine.begin_presentation_update().unwrap();
    engine
        .apply_arrow_text_measurements(viewport, &[(request.text_id, request.key, size, 180.0)])
        .unwrap();
    assert_eq!(
        engine.end_presentation_update().unwrap().changed_viewports,
        vec![viewport, second]
    );
    for (id, cursor) in cursors {
        let patch = engine.acquire_patch(id, Some(cursor)).unwrap();
        assert!(!patch.scene.reset);
        assert_eq!(patch.scene.revision, cursor.scene_revision.0 + 1);
        let full_patch = engine.acquire_patch(id, None).unwrap();
        let rendered = full_patch
            .scene
            .ops
            .iter()
            .flat_map(|operation| &operation.insert_items)
            .find_map(|item| {
                if let SceneDisplayItem::Text(text) = item {
                    Some(text)
                } else {
                    None
                }
            })
            .unwrap();
        assert!((rendered.center_x.hypot(rendered.center_y) - expected_offset).abs() < 1e-9);
        assert_eq!(rendered.width, 180.0);
    }
    click(&mut engine, viewport, end);
    let owner = engine.model.paint_order()[0];
    assert_eq!(label(&engine, owner).layout, size);
    assert!(
        (label(&engine, owner)
            .center
            .x
            .hypot(label(&engine, owner).center.y)
            - expected_offset)
            .abs()
            < 1e-9
    );
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, owner)
        .unwrap();
    let committed_request = engine
        .arrow_text_layout_requests(viewport)
        .unwrap()
        .remove(0);
    engine
        .apply_arrow_text_measurements(
            viewport,
            &[(
                committed_request.text_id,
                committed_request.key,
                size,
                180.0,
            )],
        )
        .unwrap();
    engine
        .adjust_viewport_angle_value(viewport, 0.1_f64.to_radians())
        .unwrap();
    assert!(
        engine
            .arrow_text_layout_requests(viewport)
            .unwrap()
            .is_empty(),
        "unchanged integer text should reuse its natural-width measurement"
    );
    assert_eq!(label(&engine, owner).layout, size);
    engine
        .set_viewport_angle_style_patch(
            viewport,
            AngleStyle {
                decimal_places: 3,
                ..Default::default()
            },
            ANGLE_STYLE_PROPERTY_DECIMAL_PLACES,
        )
        .unwrap();
    assert_eq!(
        engine.arrow_text_layout_requests(viewport).unwrap()[0]
            .text
            .text,
        "90.100\u{00b0}"
    );
    engine.undo().unwrap();
    assert_eq!(label(&engine, owner).text, "90\u{00b0}");
    Engine::from_serialized_document_session_with_config(
        &engine.serialize_document_session().unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
}

#[test]
fn angle_owned_pair_survives_duplicate_template_session_and_delete_history() {
    let (mut engine, viewport) = setup();
    let id = apply(
        &mut engine,
        json!([{
            "type":"angle", "points":[[100,0],[0,0],[100,0]], "full_turn":true,
            "style":{"unit":"radians","decimal_places":3}
        }]),
    )
    .unwrap()[0];
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    let annotation = engine.model.arrow(id).unwrap().angle;
    let template = engine.serialize_selected_draw_template().unwrap();
    engine
        .duplicate_selected_with_viewport_changes(viewport, Point::new(0.0, 100.0))
        .unwrap();
    let duplicate = engine.selected_ids()[0];
    assert_ne!(
        engine.model.bound_text_id_for_arrow(id),
        engine.model.bound_text_id_for_arrow(duplicate)
    );
    assert_eq!(engine.model.arrow(duplicate).unwrap().angle, annotation);
    assert_eq!(label(&engine, duplicate).text, "6.283 rad");
    engine
        .insert_draw_template_with_viewport_changes(viewport, &template, Point::new(100.0, -100.0))
        .unwrap();
    let inserted = engine.selected_ids()[0];
    assert_eq!(engine.model.arrow(inserted).unwrap().angle, annotation);
    assert_eq!(label(&engine, inserted).text, "6.283 rad");
    assert_eq!(engine.model.paint_order().len(), 6);
    let mut restored = Engine::from_serialized_document_session_with_config(
        &engine.serialize_document_session().unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
    assert_eq!(restored.model.document(), engine.model.document());
    let restored_viewport = restored.create_viewport(ViewportConfig::default()).unwrap();
    restored
        .select_element_with_viewport_changes(restored_viewport, inserted)
        .unwrap();
    restored
        .delete_selected_with_viewport_changes(restored_viewport)
        .unwrap();
    assert_eq!(restored.model.paint_order().len(), 4);
    restored.undo().unwrap();
    assert_eq!(restored.model.paint_order().len(), 6);
    assert_eq!(label(&restored, inserted).text, "6.283 rad");
    restored.redo().unwrap();
    assert_eq!(restored.model.paint_order().len(), 4);
}

#[test]
fn angle_style_patch_updates_first_ray_preview_and_preserves_defaults_in_selected_mixed_styles() {
    let (mut engine, viewport) = setup();
    click(&mut engine, viewport, Point::new(100.0, 0.0));
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(0.0, 0.0),
    );
    let creation = AngleStyle {
        stroke_width: 6.0,
        ..Default::default()
    };
    engine
        .set_viewport_angle_style_patch(viewport, creation, ANGLE_STYLE_PROPERTY_ALL)
        .unwrap();
    let preview = engine.acquire_patch(viewport, None).unwrap();
    assert!(
        preview
            .scene
            .ops
            .iter()
            .flat_map(|operation| &operation.insert_items)
            .any(
                |item| matches!(item, SceneDisplayItem::Arrow(arrow) if arrow.stroke_width == 6.0)
            )
    );
    engine
        .process_input(viewport, InputEvent::FocusLost)
        .unwrap();
    let ids = apply(&mut engine, json!([
        {"type":"angle","points":[[100,0],[0,0],[0,-100]],"style":{"stroke_width":2,"decimal_places":0}},
        {"type":"angle","points":[[300,0],[200,0],[200,-100]],"style":{"stroke_width":4,"decimal_places":3}}
    ])).unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    select(&mut engine, &ids);
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .angle_style_mixed,
        crate::ANGLE_STYLE_MIXED_STROKE_WIDTH | crate::ANGLE_STYLE_MIXED_DECIMAL_PLACES
    );
    engine
        .set_viewport_angle_style_patch(
            viewport,
            AngleStyle {
                unit: AngleUnit::Radians,
                ..Default::default()
            },
            ANGLE_STYLE_PROPERTY_UNIT,
        )
        .unwrap();
    assert_eq!(label(&engine, ids[0]).text, "2 rad");
    assert_eq!(label(&engine, ids[1]).text, "1.571 rad");
    assert_eq!(engine.model.arrow(ids[0]).unwrap().stroke_width, 2.0);
    assert_eq!(engine.model.arrow(ids[1]).unwrap().stroke_width, 4.0);
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Angle)
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .angle_style,
        creation
    );
}

#[test]
fn angle_wheel_target_token_changes_for_same_style_selection_swaps_and_draft_stages() {
    let (mut engine, viewport) = setup();
    let ids = apply(
        &mut engine,
        json!([
            {"type":"angle","points":[[100,0],[0,0],[0,-100]]},
            {"type":"angle","points":[[300,0],[200,0],[200,-100]]}
        ]),
    )
    .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, ids[0])
        .unwrap();
    let first = engine.angle_adjustment_target_revision();
    engine
        .adjust_viewport_angle_value(viewport, 1_f64.to_radians())
        .unwrap();
    assert_eq!(engine.angle_adjustment_target_revision(), first);
    engine
        .select_element_with_viewport_changes(viewport, ids[1])
        .unwrap();
    assert_ne!(engine.angle_adjustment_target_revision(), first);
    let second = engine.angle_adjustment_target_revision();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(-50.0, -50.0),
    );
    assert_eq!(engine.angle_adjustment_target_revision(), second);
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Angle)
        .unwrap();
    let idle = engine.angle_adjustment_target_revision();
    click(&mut engine, viewport, Point::new(-100.0, 150.0));
    let first_ray = engine.angle_adjustment_target_revision();
    assert_ne!(first_ray, idle);
    click(&mut engine, viewport, Point::new(-200.0, 150.0));
    let vertex = engine.angle_adjustment_target_revision();
    assert_ne!(vertex, first_ray);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(-200.0, 50.0),
    );
    let draft = engine.angle_adjustment_target_revision();
    assert_ne!(draft, vertex);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(-250.0, 100.0),
    );
    assert_eq!(engine.angle_adjustment_target_revision(), draft);
    engine
        .adjust_viewport_angle_value(viewport, 1_f64.to_radians())
        .unwrap();
    assert_eq!(engine.angle_adjustment_target_revision(), draft);
    engine
        .set_viewport_angle_style_patch(
            viewport,
            AngleStyle {
                decimal_places: 3,
                ..Default::default()
            },
            ANGLE_STYLE_PROPERTY_DECIMAL_PLACES,
        )
        .unwrap();
    assert_eq!(engine.angle_adjustment_target_revision(), draft);
    engine
        .process_input(viewport, InputEvent::FocusLost)
        .unwrap();
    assert_eq!(engine.angle_adjustment_target_revision(), idle);
}

#[test]
fn angle_warm_metrics_reuse_is_local_and_preserves_wheel_burst_history_isolation() {
    let (mut engine, viewport) = setup();
    let operations: Vec<_> = (0..128)
        .map(|index| {
            let x = f64::from(index) * 300.0;
            json!({"type":"angle","points":[[x+100.0,0],[x,0],[x,-100.0]]})
        })
        .collect();
    let ids = apply(&mut engine, json!(operations)).unwrap();
    let size = TextLayoutSize::new(64.0, 24.0);
    let metrics: Vec<_> = engine
        .arrow_text_layout_requests(viewport)
        .unwrap()
        .iter()
        .map(|request| (request.text_id, request.key, size, 64.0))
        .collect();
    assert_eq!(metrics.len(), 128);
    engine
        .apply_arrow_text_measurements(viewport, &metrics)
        .unwrap();
    assert!(
        engine
            .arrow_text_layout_requests(viewport)
            .unwrap()
            .is_empty()
    );
    let unrelated = label(&engine, ids[1]).clone();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, ids[0])
        .unwrap();
    let plan_builds = engine.scene_cache.order_plan_build_count();
    let request_builds = engine.editor.arrow_text_request_build_count();
    assert_eq!(engine.editor.arrow_text_cached_owner_count(), 128);
    engine
        .adjust_viewport_angle_value(viewport, 0.1_f64.to_radians())
        .unwrap();
    engine
        .adjust_viewport_angle_value(viewport, 0.1_f64.to_radians())
        .unwrap();
    assert!(
        engine
            .arrow_text_layout_requests(viewport)
            .unwrap()
            .is_empty()
    );
    assert_eq!(engine.scene_cache.order_plan_build_count(), plan_builds);
    assert_eq!(
        engine.editor.arrow_text_request_build_count(),
        request_builds + 2,
        "each wheel commit rebuilds only the changed owner's cached request"
    );
    assert_eq!(
        label(&engine, ids[1]),
        &unrelated,
        "unrelated metrics must not enter the active angle transaction"
    );
    engine.undo().unwrap();
    assert!(
        (angle_value(engine.model.arrow(ids[0]).unwrap())
            .unwrap()
            .to_degrees()
            - 90.0)
            .abs()
            < 1e-9,
        "the complete wheel burst must undo together with warm unrelated metrics"
    );
    engine
        .set_viewport_angle_style_patch(
            viewport,
            AngleStyle {
                unit: AngleUnit::Radians,
                decimal_places: 3,
                ..Default::default()
            },
            ANGLE_STYLE_PROPERTY_UNIT | ANGLE_STYLE_PROPERTY_DECIMAL_PLACES,
        )
        .unwrap();
    let pending = engine.arrow_text_layout_requests(viewport).unwrap();
    assert_eq!(pending.len(), 1);
    assert_eq!(pending[0].arrow_id, ids[0]);
    assert_eq!(pending[0].text.text, "1.571 rad");
    assert_eq!(label(&engine, ids[1]), &unrelated);
    Engine::from_serialized_document_session_with_config(
        &engine.serialize_document_session().unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
}

#[test]
fn angle_smart_erase_sync_does_not_visit_owned_labels_or_unrelated_document_ids() {
    let (mut engine, viewport) = setup();
    for base in (0..1024).step_by(200) {
        let operations: Vec<_> = (base..(base + 200).min(1024))
            .map(|index| {
                let x = 10000.0 + f64::from(index);
                json!({"type":"angle","points":[[x+80.0,10000.0],[x,10000.0],[x,9960.0]]})
            })
            .collect();
        apply(&mut engine, json!(operations)).unwrap();
    }
    let metrics: Vec<_> = engine
        .arrow_text_layout_requests(viewport)
        .unwrap()
        .iter()
        .map(|request| {
            (
                request.text_id,
                request.key,
                TextLayoutSize::new(64.0, 24.0),
                64.0,
            )
        })
        .collect();
    assert_eq!(metrics.len(), 1024);
    engine
        .apply_arrow_text_measurements(viewport, &metrics)
        .unwrap();
    click(&mut engine, viewport, Point::new(100.0, 0.0));
    click(&mut engine, viewport, Point::new(0.0, 0.0));
    let builds = engine.editor.arrow_text_request_build_count();
    let visits = engine.scene_cache.smart_erase_candidate_visit_count();
    for index in 0..16 {
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            Point::new(f64::from(index), -100.0),
        );
        let candidates = engine.editor.arrow_text_preview_candidate_count();
        assert_eq!(candidates, 0, "all committed labels are offscreen");
        assert!(engine.smart_erase_items().is_empty());
        assert_eq!(
            engine.editor.arrow_text_preview_candidate_count(),
            candidates
        );
    }
    assert_eq!(engine.editor.arrow_text_request_build_count(), builds);
    assert_eq!(
        engine.scene_cache.smart_erase_candidate_visit_count(),
        visits
    );
    assert_eq!(engine.editor.arrow_text_cached_owner_count(), 1024);
}

#[test]
fn angle_label_memo_reuses_committed_owners_and_releases_deleted_document_ids() {
    let (mut engine, viewport) = setup();
    let operations: Vec<_> = (0..64)
        .map(|index| {
            let x = f64::from(index) * 300.0;
            json!({"type":"angle","points":[[x+100.0,0],[x,0],[x,-100.0]]})
        })
        .collect();
    let ids = apply(&mut engine, json!(operations)).unwrap();
    let requests = engine.arrow_text_layout_requests(viewport).unwrap();
    let metrics: Vec<_> = requests
        .iter()
        .map(|request| {
            (
                request.text_id,
                request.key,
                TextLayoutSize::new(64.0, 24.0),
                64.0,
            )
        })
        .collect();
    engine
        .apply_arrow_text_measurements(viewport, &metrics)
        .unwrap();
    let builds = engine.editor.arrow_text_request_build_count();
    assert_eq!(engine.editor.arrow_text_cached_owner_count(), 64);
    assert!(engine.editor.arrow_text_preview_candidate_count() <= 2);
    for _ in 0..3 {
        assert!(
            engine
                .arrow_text_layout_requests(viewport)
                .unwrap()
                .is_empty()
        );
    }
    click(&mut engine, viewport, Point::new(100.0, 200.0));
    click(&mut engine, viewport, Point::new(0.0, 200.0));
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(0.0, 100.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(20.0, 100.0),
    );
    assert_eq!(
        engine.editor.arrow_text_request_build_count(),
        builds,
        "draft moves must reuse every unchanged committed label request"
    );
    assert!(
        engine.editor.arrow_text_preview_candidate_count() <= 2,
        "draft moves must query the viewport index instead of visiting all label owners"
    );
    engine
        .process_input(viewport, InputEvent::FocusLost)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, ids[0])
        .unwrap();
    engine
        .delete_selected_with_viewport_changes(viewport)
        .unwrap();
    assert_eq!(engine.editor.arrow_text_cached_owner_count(), 63);
    assert_eq!(
        engine.editor.arrow_text_request_build_count(),
        builds,
        "deletion must evict its owner without rebuilding remaining labels"
    );
    engine.undo().unwrap();
    assert_eq!(engine.editor.arrow_text_cached_owner_count(), 64);
    assert_eq!(
        engine.editor.arrow_text_request_build_count(),
        builds + 1,
        "undoing deletion rebuilds only the restored owner"
    );
    let restored_requests = engine.arrow_text_layout_requests(viewport).unwrap();
    assert_eq!(
        restored_requests.len(),
        1,
        "deleted label measurements must be evicted instead of retaining dead IDs"
    );
    assert_eq!(restored_requests[0].arrow_id, ids[0]);
    engine.clear_document_preserving_viewports().unwrap();
    assert_eq!(engine.editor.arrow_text_cached_owner_count(), 0);
    assert!(
        engine
            .arrow_text_layout_requests(viewport)
            .unwrap()
            .is_empty()
    );
}

#[test]
fn angle_ten_thousand_warm_visible_labels_publish_only_active_geometry_and_items() {
    use snow_draw_engine_core::arrow::{ArrowType, StrokeStyle};
    use snow_draw_engine_document::{
        AngleAnnotation, ArrowData, ElementMeta, LinearElementKind, Transaction, angle_label,
    };

    let (mut engine, viewport) = setup();
    let mut transaction = Transaction::new("dense visible angle fixture");
    for index in 0..10000 {
        let owner = ElementId {
            index: index * 2,
            generation: 1,
        };
        let text = ElementId {
            index: index * 2 + 1,
            generation: 1,
        };
        let mut arrow = ArrowData::from_global_points(
            &[
                Point::new(-200.0, 100.0),
                Point::new(-300.0, 100.0),
                Point::new(-300.0, 0.0),
            ],
            AngleStyle::default().stroke,
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        arrow.linear_kind = LinearElementKind::Angle;
        arrow.angle = Some(AngleAnnotation::default());
        arrow.text_element_id = Some(text);
        let label = angle_label(&arrow, None).unwrap();
        transaction.insert_arrow(owner, ElementMeta::default(), arrow);
        transaction.insert_text(text, ElementMeta::default(), label);
    }
    engine.model.apply_transaction(transaction).unwrap();
    engine.scene_cache.sync(&engine.model, None);
    engine.refresh_all_viewports().unwrap();
    let metrics: Vec<_> = engine
        .arrow_text_layout_requests(viewport)
        .unwrap()
        .iter()
        .map(|request| {
            (
                request.text_id,
                request.key,
                TextLayoutSize::new(64.0, 24.0),
                64.0,
            )
        })
        .collect();
    assert_eq!(metrics.len(), 10000);
    engine
        .apply_arrow_text_measurements(viewport, &metrics)
        .unwrap();
    assert_eq!(engine.editor.arrow_text_preview_candidate_count(), 10000);
    let owner = ElementId {
        index: 0,
        generation: 1,
    };
    let unrelated = ElementId {
        index: 2,
        generation: 1,
    };
    let SceneDisplayItem::Arrow(cached) = engine.scene_cache.entry(unrelated).unwrap() else {
        panic!("cached angle");
    };
    let original_geometry = cached.geometry.clone();
    let builds = engine.editor.arrow_text_request_build_count();
    let draft = engine.model.peek_next_element_id();
    click(&mut engine, viewport, Point::new(300.0, 200.0));
    click(&mut engine, viewport, Point::new(200.0, 200.0));
    for x in [200.0, 220.0] {
        let cursor = engine
            .viewport_slot(viewport)
            .unwrap()
            .composer
            .current_cursor();
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            Point::new(x, 100.0),
        );
        let patch = engine.acquire_patch(viewport, Some(cursor)).unwrap();
        assert!(!patch.scene.reset);
        assert_eq!(patch.path_geometry_ops.len(), 1);
        assert_eq!(patch.path_geometry_ops[0].id.index, draft.index);
        let changed: Vec<_> = patch
            .scene
            .ops
            .iter()
            .flat_map(|op| &op.insert_items)
            .collect();
        assert_eq!(changed.len(), 2, "only draft rays and text enter the patch");
        assert!(changed.iter().all(|item| match item {
            SceneDisplayItem::Arrow(arrow) => arrow.id.index == draft.index,
            SceneDisplayItem::Text(text) => text.id.index == u32::MAX - 1,
            _ => false,
        }));
        assert_eq!(engine.editor.arrow_text_request_build_count(), builds);
    }
    engine
        .process_input(viewport, InputEvent::FocusLost)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, owner)
        .unwrap();
    let cursor = engine
        .viewport_slot(viewport)
        .unwrap()
        .composer
        .current_cursor();
    engine
        .adjust_viewport_angle_value(viewport, 0.1_f64.to_radians())
        .unwrap();
    let patch = engine.acquire_patch(viewport, Some(cursor)).unwrap();
    assert_eq!(patch.path_geometry_ops.len(), 1);
    assert_eq!(patch.path_geometry_ops[0].id.index, owner.index);
    let changed: Vec<_> = patch
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .collect();
    assert_eq!(
        changed.len(),
        2,
        "only selected rays and owned text enter the patch"
    );
    assert!(changed.iter().all(|item| match item {
        SceneDisplayItem::Arrow(arrow) => arrow.id.index == owner.index,
        SceneDisplayItem::Text(text) => text.id.index == owner.index + 1,
        _ => false,
    }));
    assert_eq!(engine.editor.arrow_text_request_build_count(), builds + 1);
    let SceneDisplayItem::Arrow(cached) = engine.scene_cache.entry(unrelated).unwrap() else {
        panic!("cached angle");
    };
    assert!(std::sync::Arc::ptr_eq(&original_geometry, &cached.geometry));
    assert_eq!(original_geometry.revision, cached.geometry.revision);
}

#[test]
fn angle_measured_label_memo_culls_per_viewport_and_replaces_visible_estimates() {
    let (mut engine, viewport) = setup();
    engine
        .set_viewport_surface_size(viewport, 100, 100)
        .unwrap();
    let ids = apply(
        &mut engine,
        json!([
            {"type":"angle","points":[[-100,200],[-200,200],[-200,100]]}
        ]),
    )
    .unwrap();
    let second = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine.set_viewport_surface_size(second, 100, 100).unwrap();
    engine
        .set_viewport_camera(
            second,
            Camera {
                center: Point::new(-160.0, 160.0),
                zoom: 1.0,
            },
        )
        .unwrap();
    let request = engine
        .arrow_text_layout_requests(viewport)
        .unwrap()
        .remove(0);
    engine
        .apply_arrow_text_measurements(
            viewport,
            &[(
                request.text_id,
                request.key,
                TextLayoutSize::new(500.0, 24.0),
                500.0,
            )],
        )
        .unwrap();
    let builds = engine.editor.arrow_text_request_build_count();
    let first_patch = engine.acquire_patch(viewport, None).unwrap();
    let first_texts: Vec<_> = first_patch
        .scene
        .ops
        .iter()
        .flat_map(|operation| &operation.insert_items)
        .filter_map(|item| {
            if let SceneDisplayItem::Text(text) = item {
                Some(text)
            } else {
                None
            }
        })
        .collect();
    assert_eq!(
        first_texts.len(),
        1,
        "measured label may enter while its owner is offscreen"
    );
    assert_eq!(first_texts[0].width, 500.0);
    let second_patch = engine.acquire_patch(second, None).unwrap();
    assert!(
        second_patch
            .scene
            .ops
            .iter()
            .flat_map(|operation| &operation.insert_items)
            .all(|item| !matches!(item, SceneDisplayItem::Text(_))),
        "visible estimates must be replaced even when the measured label moved offscreen"
    );
    assert_eq!(
        engine.editor.arrow_text_request_build_count(),
        builds,
        "viewport culling must reuse cached measurement and geometry"
    );
    assert_eq!(label(&engine, ids[0]).text, "90\u{00b0}");
}

#[test]
#[ignore = "run explicitly with --release for input stage profiling"]
fn angle_input_stage_profile() {
    use std::time::Instant;
    let population = std::env::var("SNOW_ANGLE_PROFILE_COUNT")
        .ok()
        .and_then(|count| count.parse::<usize>().ok())
        .unwrap_or(10_000);
    let (mut engine, viewport) = setup();
    for chunk in (0..population).collect::<Vec<_>>().chunks(250) {
        let operations: Vec<_> = chunk
            .iter()
            .map(|index| {
                let x = 10_000.0 + (*index % 100) as f64 * 30.0;
                let y = 10_000.0 + (*index / 100) as f64 * 30.0;
                json!({"type":"angle","points":[[x+40.0,y],[x,y],[x,y-40.0]]})
            })
            .collect();
        apply(&mut engine, json!(operations)).unwrap();
    }
    let metrics: Vec<_> = engine
        .arrow_text_layout_requests(viewport)
        .unwrap()
        .iter()
        .map(|request| {
            (
                request.text_id,
                request.key,
                TextLayoutSize::new(64.0, 24.0),
                64.0,
            )
        })
        .collect();
    engine
        .apply_arrow_text_measurements(viewport, &metrics)
        .unwrap();
    let mut stages = [0.0_f64; 6];
    click(&mut engine, viewport, Point::new(100.0, 0.0));
    click(&mut engine, viewport, Point::new(0.0, 0.0));
    for index in 0..60 {
        engine.begin_presentation_update().unwrap();
        let start = Instant::now();
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            Point::new(index as f64, -100.0),
        );
        stages[0] += start.elapsed().as_secs_f64();
        let start = Instant::now();
        std::hint::black_box(engine.arrow_text_layout_requests(viewport).unwrap());
        stages[1] += start.elapsed().as_secs_f64();
        let start = Instant::now();
        engine.end_presentation_update().unwrap();
        stages[2] += start.elapsed().as_secs_f64();
        let start = Instant::now();
        std::hint::black_box(engine.viewport_style_toolbar_state(viewport).unwrap());
        stages[3] += start.elapsed().as_secs_f64();
        let start = Instant::now();
        std::hint::black_box(
            engine
                .viewport_serial_number_toolbar_state(viewport)
                .unwrap(),
        );
        stages[4] += start.elapsed().as_secs_f64();
        let start = Instant::now();
        std::hint::black_box(engine.editor.snapshot());
        stages[5] += start.elapsed().as_secs_f64();
    }
    eprintln!(
        "ANGLE_PROFILE count={population} mean_ms input/requests/refresh/style/serial/snapshot={:?}",
        stages.map(|seconds| seconds * 1000.0 / 60.0)
    );
}

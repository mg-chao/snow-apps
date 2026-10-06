use super::duplicate_drag_tests::pointer;
use super::*;
use snow_draw_engine_display::SceneDisplayItem;
use snow_draw_engine_document::{SerialNumberNumericType, SerialNumberType};
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

fn set_numeric_type(
    engine: &mut Engine,
    viewport: ViewportId,
    numeric_type: SerialNumberNumericType,
) {
    let mut style = engine.editor.serial_number_style(&engine.model);
    style.numeric_type = numeric_type;
    engine
        .set_viewport_serial_number_style_patch(
            viewport,
            style,
            snow_draw_engine_editor::SERIAL_NUMBER_STYLE_MIXED_NUMERIC_TYPE,
        )
        .unwrap();
}

#[test]
fn serial_number_selected_appearance_edits_preserve_creation_sequence() {
    use snow_draw_engine_editor::{
        SERIAL_NUMBER_STYLE_MIXED_FONT_SIZE, SERIAL_NUMBER_STYLE_MIXED_NUMBER,
    };
    for numeric_type in [
        SerialNumberNumericType::Arabic,
        SerialNumberNumericType::Roman,
        SerialNumberNumericType::LowercaseLetters,
        SerialNumberNumericType::UppercaseLetters,
        SerialNumberNumericType::Chinese,
    ] {
        for explicit_start in [false, true] {
            for use_patch in [false, true] {
                let (mut engine, viewport) = setup(1.0);
                set_numeric_type(&mut engine, viewport, numeric_type);
                let start = if explicit_start { 50 } else { 1 };
                if explicit_start {
                    let mut style = engine.editor.serial_number_style(&engine.model);
                    style.number = start;
                    engine
                        .set_viewport_serial_number_style_patch(
                            viewport,
                            style,
                            SERIAL_NUMBER_STYLE_MIXED_NUMBER,
                        )
                        .unwrap();
                }
                let first = click_serial_number(&mut engine, viewport, 100.0);
                let second = click_serial_number(&mut engine, viewport, 300.0);
                engine
                    .select_element_with_viewport_changes(viewport, first)
                    .unwrap();
                let mut style = engine.editor.serial_number_style(&engine.model);
                style.font_size = 32.0;
                if use_patch {
                    engine
                        .set_viewport_serial_number_style_patch(
                            viewport,
                            style,
                            SERIAL_NUMBER_STYLE_MIXED_FONT_SIZE,
                        )
                        .unwrap();
                } else {
                    engine
                        .set_viewport_serial_number_style(viewport, style)
                        .unwrap();
                }
                assert_eq!(engine.model.serial_number(first).unwrap().number, start);
                assert_eq!(engine.model.serial_number(first).unwrap().font_size, 32.0);
                assert_eq!(engine.model.serial_number(second).unwrap().font_size, 24.0);
                engine
                    .reset_editing_state_with_viewport_changes(viewport)
                    .unwrap();
                engine
                    .set_viewport_active_tool(viewport, ActiveTool::SerialNumber)
                    .unwrap();
                assert_eq!(
                    engine.editor.serial_number_style(&engine.model).number,
                    start + 2
                );
                let next = click_serial_number(&mut engine, viewport, 500.0);
                assert_eq!(engine.model.serial_number(next).unwrap().number, start + 2);
            }
        }
    }
}

#[test]
fn serial_number_full_format_and_appearance_edits_restore_destination_sequence() {
    use snow_draw_engine_editor::{
        SERIAL_NUMBER_STYLE_MIXED_NUMBER, SERIAL_NUMBER_STYLE_MIXED_NUMERIC_TYPE,
        SERIAL_NUMBER_STYLE_MIXED_TYPE,
    };
    let numeric_types = [
        SerialNumberNumericType::Arabic,
        SerialNumberNumericType::Roman,
        SerialNumberNumericType::LowercaseLetters,
        SerialNumberNumericType::UppercaseLetters,
        SerialNumberNumericType::Chinese,
    ];
    for serial_number_type in [SerialNumberType::OutlinedCircle, SerialNumberType::Circle] {
        let (mut engine, viewport) = setup(1.0);
        for numeric_type in numeric_types {
            let mut style = engine.editor.serial_number_style(&engine.model);
            style.numeric_type = numeric_type;
            style.number = 10 + numeric_type as i64;
            engine
                .set_viewport_serial_number_style_patch(
                    viewport,
                    style,
                    SERIAL_NUMBER_STYLE_MIXED_NUMBER | SERIAL_NUMBER_STYLE_MIXED_NUMERIC_TYPE,
                )
                .unwrap();
        }
        let mut style = engine.editor.serial_number_style(&engine.model);
        style.serial_number_type = serial_number_type;
        engine
            .set_viewport_serial_number_style_patch(viewport, style, SERIAL_NUMBER_STYLE_MIXED_TYPE)
            .unwrap();
        for numeric_type in numeric_types {
            let mut style = engine.editor.serial_number_style(&engine.model);
            style.numeric_type = numeric_type;
            style.font_size += 1.0;
            engine
                .set_viewport_serial_number_style(viewport, style)
                .unwrap();
            assert_eq!(
                engine.editor.serial_number_style(&engine.model).number,
                10 + numeric_type as i64
            );
        }
        let mut style = engine.editor.serial_number_style(&engine.model);
        style.serial_number_type = SerialNumberType::OutlinedCircle;
        engine
            .set_viewport_serial_number_style_patch(viewport, style, SERIAL_NUMBER_STYLE_MIXED_TYPE)
            .unwrap();
        let id = click_serial_number(&mut engine, viewport, 100.0);
        assert_eq!(engine.model.serial_number(id).unwrap().number, 14);
        for numeric_type in numeric_types {
            set_numeric_type(&mut engine, viewport, numeric_type);
            assert_eq!(
                engine.editor.serial_number_style(&engine.model).number,
                10 + numeric_type as i64
                    + i64::from(numeric_type == SerialNumberNumericType::Chinese)
            );
        }
    }
}

#[test]
fn serial_number_creation_keeps_the_same_size_across_values_and_formats() {
    for numeric_type in [
        SerialNumberNumericType::Arabic,
        SerialNumberNumericType::Roman,
        SerialNumberNumericType::Chinese,
        SerialNumberNumericType::LowercaseLetters,
        SerialNumberNumericType::UppercaseLetters,
    ] {
        for number in [0_i64, 9, 888, 3999, i64::MAX] {
            let (mut engine, viewport) = setup(1.0);
            let mut style = engine.editor.serial_number_style(&engine.model);
            style.numeric_type = numeric_type;
            style.number = number;
            engine
                .set_viewport_serial_number_style_patch(
                    viewport,
                    style,
                    snow_draw_engine_editor::SERIAL_NUMBER_STYLE_MIXED_NUMBER
                        | snow_draw_engine_editor::SERIAL_NUMBER_STYLE_MIXED_NUMERIC_TYPE,
                )
                .unwrap();
            for (offset, x) in [(0, 100.0), (1, 300.0)] {
                pointer(
                    &mut engine,
                    viewport,
                    PointerEventType::Down,
                    x,
                    300.0,
                    false,
                );
                pointer(&mut engine, viewport, PointerEventType::Up, x, 300.0, false);
                let serial = engine
                    .model
                    .serial_number(*engine.model.paint_order().last().unwrap())
                    .unwrap();
                assert_eq!(serial.number, number.saturating_add(offset));
                assert_eq!(serial.numeric_type, numeric_type);
                assert!((serial.diameter - 40.32).abs() < 1e-9);
                assert_eq!(serial.font_size, 24.0);
            }
        }
    }
}

#[test]
fn serial_number_content_edits_preserve_mixed_stored_sizes_through_history_and_sessions() {
    use snow_draw_engine_document::{ElementMeta, SerialNumberData, Transaction};
    let (mut engine, viewport) = setup(1.0);
    let mut transaction = Transaction::new("insert resized badges");
    let mut ids = Vec::new();
    for (number, diameter, font_size, numeric_type) in [
        (9, 97.0, 24.0, SerialNumberNumericType::Arabic),
        (3999, 147.0, 48.0, SerialNumberNumericType::Roman),
    ] {
        let id = engine.model.allocate_element_id();
        transaction.insert_serial_number(
            id,
            ElementMeta::default(),
            SerialNumberData {
                center: Point::new(diameter, 100.0),
                rotation: 0.3,
                number,
                diameter,
                font_size,
                numeric_type,
                ..SerialNumberData::default()
            },
        );
        ids.push(id);
    }
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
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        350.0,
        250.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        750.0,
        550.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        750.0,
        550.0,
        false,
    );
    assert_eq!(engine.selected_ids(), ids);
    for delta in [1, -1] {
        let before: Vec<_> = ids
            .iter()
            .map(|id| engine.model.serial_number(*id).unwrap().clone())
            .collect();
        let mut expected = before.clone();
        for serial in &mut expected {
            serial.number += delta;
        }
        engine
            .adjust_selected_serial_numbers_with_viewport_changes(viewport, delta)
            .unwrap();
        for (id, serial) in ids.iter().zip(&expected) {
            assert_eq!(engine.model.serial_number(*id).unwrap(), serial);
        }
        assert!(engine.undo().unwrap());
        for (id, serial) in ids.iter().zip(&before) {
            assert_eq!(engine.model.serial_number(*id).unwrap(), serial);
        }
        assert!(engine.redo().unwrap());
        for (id, serial) in ids.iter().zip(&expected) {
            assert_eq!(engine.model.serial_number(*id).unwrap(), serial);
        }
        // Test each direction from the seed: consecutive number steps are
        // intentionally coalesced, including cancellation back to the start.
        assert!(engine.undo().unwrap());
    }
    let before: Vec<_> = ids
        .iter()
        .map(|id| engine.model.serial_number(*id).unwrap().clone())
        .collect();
    let mut expected = before.clone();
    for serial in &mut expected {
        serial.numeric_type = SerialNumberNumericType::Chinese;
    }
    set_numeric_type(&mut engine, viewport, SerialNumberNumericType::Chinese);
    for (id, serial) in ids.iter().zip(&expected) {
        assert_eq!(engine.model.serial_number(*id).unwrap(), serial);
    }
    assert!(engine.undo().unwrap());
    for (id, serial) in ids.iter().zip(&before) {
        assert_eq!(engine.model.serial_number(*id).unwrap(), serial);
    }
    assert!(engine.redo().unwrap());
    let restored = Engine::from_serialized_document_session_with_config(
        &engine.serialize_document_session().unwrap(),
        Default::default(),
    )
    .unwrap();
    for (id, serial) in ids.iter().zip(&expected) {
        assert_eq!(engine.model.serial_number(*id).unwrap(), serial);
        assert_eq!(restored.model.serial_number(*id).unwrap(), serial);
    }
}

#[test]
fn serial_number_creation_increments_each_numeric_type_independently() {
    let (mut engine, viewport) = setup(1.0);
    let numeric_types = [
        SerialNumberNumericType::Arabic,
        SerialNumberNumericType::Roman,
        SerialNumberNumericType::LowercaseLetters,
        SerialNumberNumericType::UppercaseLetters,
        SerialNumberNumericType::Chinese,
    ];
    for number in 1..=2 {
        for (index, numeric_type) in numeric_types.into_iter().enumerate() {
            set_numeric_type(&mut engine, viewport, numeric_type);
            assert_eq!(
                engine.editor.serial_number_style(&engine.model).number,
                number
            );
            let x = 100.0 + index as f64 * 120.0;
            let y = 100.0 + number as f64 * 120.0;
            pointer(&mut engine, viewport, PointerEventType::Down, x, y, false);
            pointer(&mut engine, viewport, PointerEventType::Up, x, y, false);
            let serial = engine
                .model
                .serial_number(*engine.model.paint_order().last().unwrap())
                .unwrap();
            assert_eq!(serial.numeric_type, numeric_type);
            assert_eq!(serial.number, number);
            assert_eq!(
                engine.editor.serial_number_style(&engine.model).number,
                number + 1
            );
        }
    }
}

#[test]
fn serial_number_explicit_values_are_remembered_per_numeric_type_and_session() {
    let (mut engine, viewport) = setup(1.0);
    let values = [
        (SerialNumberNumericType::Arabic, 0),
        (SerialNumberNumericType::Roman, 10),
        (SerialNumberNumericType::LowercaseLetters, 27),
        (SerialNumberNumericType::UppercaseLetters, 99),
        (SerialNumberNumericType::Chinese, i64::MAX),
    ];
    for (numeric_type, number) in values {
        set_numeric_type(&mut engine, viewport, numeric_type);
        let mut style = engine.editor.serial_number_style(&engine.model);
        style.number = number;
        engine
            .set_viewport_serial_number_style_patch(
                viewport,
                style,
                snow_draw_engine_editor::SERIAL_NUMBER_STYLE_MIXED_NUMBER,
            )
            .unwrap();
    }
    let bytes = engine.serialize_document_session().unwrap();
    let mut restored =
        Engine::from_serialized_document_session_with_config(&bytes, Default::default()).unwrap();
    let restored_viewport = restored.create_viewport(Default::default()).unwrap();
    for (numeric_type, number) in values {
        set_numeric_type(&mut engine, viewport, numeric_type);
        set_numeric_type(&mut restored, restored_viewport, numeric_type);
        assert_eq!(
            engine.editor.serial_number_style(&engine.model).number,
            number
        );
        assert_eq!(
            restored.editor.serial_number_style(&restored.model).number,
            number
        );
    }
}

#[test]
fn serial_number_document_clear_restarts_all_sequences_and_preserves_appearance() {
    use snow_draw_engine_editor::{
        SERIAL_NUMBER_STYLE_MIXED_FONT_SIZE, SERIAL_NUMBER_STYLE_MIXED_NUMBER,
    };

    for explicit_start in [false, true] {
        let (mut engine, viewport) = setup(1.0);
        let second_viewport = engine.create_viewport(Default::default()).unwrap();
        let numeric_types = [
            SerialNumberNumericType::Arabic,
            SerialNumberNumericType::Roman,
            SerialNumberNumericType::LowercaseLetters,
            SerialNumberNumericType::UppercaseLetters,
            SerialNumberNumericType::Chinese,
        ];
        for (index, numeric_type) in numeric_types.into_iter().enumerate() {
            set_numeric_type(&mut engine, viewport, numeric_type);
            let mut style = engine.editor.serial_number_style(&engine.model);
            style.font_size = 32.0;
            style.number = 10 + index as i64;
            let start = if explicit_start { style.number } else { 1 };
            engine
                .set_viewport_serial_number_style_patch(
                    viewport,
                    style,
                    SERIAL_NUMBER_STYLE_MIXED_FONT_SIZE
                        | if explicit_start {
                            SERIAL_NUMBER_STYLE_MIXED_NUMBER
                        } else {
                            0
                        },
                )
                .unwrap();
            let id = click_serial_number(&mut engine, viewport, 100.0 + index as f64 * 120.0);
            assert_eq!(engine.model.serial_number(id).unwrap().number, start);
        }
        let mut appearance = engine.editor.serial_number_style(&engine.model);
        appearance.number = 1;
        for _ in 0..2 {
            engine.clear_document_preserving_viewports().unwrap();
            assert!(engine.model.paint_order().is_empty());
            assert!(!engine.history.can_undo());
            assert!(!engine.history.can_redo());
            assert_eq!(engine.editor.serial_number_style(&engine.model), appearance);
            engine
                .set_viewport_active_tool(viewport, ActiveTool::SerialNumber)
                .unwrap();
            for (index, numeric_type) in numeric_types.into_iter().enumerate() {
                set_numeric_type(&mut engine, viewport, numeric_type);
                for view in [viewport, second_viewport] {
                    assert_eq!(
                        engine.viewport_active_tool(view).unwrap(),
                        ActiveTool::SerialNumber
                    );
                    assert_eq!(
                        engine
                            .viewport_style_toolbar_state(view)
                            .unwrap()
                            .serial_number_style
                            .number,
                        1
                    );
                }
                let id = click_serial_number(&mut engine, viewport, 100.0 + index as f64 * 120.0);
                assert_eq!(engine.model.serial_number(id).unwrap().number, 1);
                engine.undo().unwrap();
                assert_eq!(engine.editor.serial_number_style(&engine.model).number, 1);
                engine.redo().unwrap();
                assert_eq!(engine.editor.serial_number_style(&engine.model).number, 2);
            }
        }
    }
}

#[test]
fn serial_number_document_clear_uses_configured_start_for_its_numeric_type() {
    let mut config = EngineConfig::default();
    config.style_defaults.editor.serial_number.numeric_type = SerialNumberNumericType::Roman;
    config.style_defaults.editor.serial_number.number = 7;
    let mut engine = Engine::new(config);
    let viewport = engine.create_viewport(Default::default()).unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::SerialNumber)
        .unwrap();
    for numeric_type in [
        SerialNumberNumericType::Roman,
        SerialNumberNumericType::Arabic,
    ] {
        set_numeric_type(&mut engine, viewport, numeric_type);
        let mut style = engine.editor.serial_number_style(&engine.model);
        style.number = 99;
        engine
            .set_viewport_serial_number_style_patch(
                viewport,
                style,
                snow_draw_engine_editor::SERIAL_NUMBER_STYLE_MIXED_NUMBER,
            )
            .unwrap();
    }
    engine.clear_document_preserving_viewports().unwrap();
    assert_eq!(engine.editor.serial_number_style(&engine.model).number, 1);
    set_numeric_type(&mut engine, viewport, SerialNumberNumericType::Roman);
    assert_eq!(engine.editor.serial_number_style(&engine.model).number, 7);
}

#[test]
fn serial_number_inactive_counters_follow_deletion_reset_and_history() {
    let (mut engine, viewport) = setup(1.0);
    for (numeric_type, x, y) in [
        (SerialNumberNumericType::Arabic, 100.0, 100.0),
        (SerialNumberNumericType::Roman, 300.0, 100.0),
        (SerialNumberNumericType::Roman, 300.0, 250.0),
    ] {
        set_numeric_type(&mut engine, viewport, numeric_type);
        pointer(&mut engine, viewport, PointerEventType::Down, x, y, false);
        pointer(&mut engine, viewport, PointerEventType::Up, x, y, false);
    }
    let deleted_id = *engine.model.paint_order().last().unwrap();
    set_numeric_type(&mut engine, viewport, SerialNumberNumericType::Chinese);
    let mut style = engine.editor.serial_number_style(&engine.model);
    style.number = 100;
    engine
        .set_viewport_serial_number_style(viewport, style)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, deleted_id)
        .unwrap();
    engine
        .delete_selected_with_viewport_changes(viewport)
        .unwrap();
    let check = |engine: &mut Engine, arabic: i64, roman: i64| {
        for (numeric_type, expected) in [
            (SerialNumberNumericType::Arabic, arabic),
            (SerialNumberNumericType::Roman, roman),
            (SerialNumberNumericType::Chinese, 100),
        ] {
            set_numeric_type(engine, viewport, numeric_type);
            assert_eq!(
                engine.editor.serial_number_style(&engine.model).number,
                expected
            );
        }
    };
    check(&mut engine, 2, 2);
    engine.undo().unwrap();
    check(&mut engine, 2, 3);
    engine.redo().unwrap();
    check(&mut engine, 2, 2);
    engine
        .delete_all_elements_with_viewport_changes(viewport)
        .unwrap();
    check(&mut engine, 1, 1);
    engine.undo().unwrap();
    check(&mut engine, 2, 2);
    engine.redo().unwrap();
    check(&mut engine, 1, 1);
}

#[test]
fn serial_number_values_load_legacy_sessions_and_reject_negative_counters() {
    let (mut engine, viewport) = setup(1.0);
    set_numeric_type(&mut engine, viewport, SerialNumberNumericType::Roman);
    let mut style = engine.editor.serial_number_style(&engine.model);
    style.number = 24;
    engine
        .set_viewport_serial_number_style(viewport, style)
        .unwrap();
    let mut legacy: serde_json::Value =
        serde_json::from_slice(&engine.serialize_document_session().unwrap()).unwrap();
    legacy["schemaVersion"] = serde_json::json!(7);
    legacy["editor"]
        .as_object_mut()
        .unwrap()
        .remove("serialNumberValues");
    let mut restored = Engine::from_serialized_document_session_with_config(
        &serde_json::to_vec(&legacy).unwrap(),
        Default::default(),
    )
    .unwrap();
    let restored_viewport = restored.create_viewport(Default::default()).unwrap();
    assert_eq!(
        restored.editor.serial_number_style(&restored.model).number,
        24
    );
    set_numeric_type(
        &mut restored,
        restored_viewport,
        SerialNumberNumericType::Arabic,
    );
    assert_eq!(
        restored.editor.serial_number_style(&restored.model).number,
        1
    );
    set_numeric_type(
        &mut restored,
        restored_viewport,
        SerialNumberNumericType::Roman,
    );
    assert_eq!(
        restored.editor.serial_number_style(&restored.model).number,
        24
    );
    legacy["editor"]["serialNumberValues"] = serde_json::json!([1, 24, -1, 1, 1]);
    assert!(matches!(
        Engine::from_serialized_document_session_with_config(
            &serde_json::to_vec(&legacy).unwrap(),
            Default::default(),
        ),
        Err(ErrorCode::InvalidArgument)
    ));
}

fn click_serial_number(engine: &mut Engine, viewport: ViewportId, x: f64) -> ElementId {
    pointer(engine, viewport, PointerEventType::Down, x, 300.0, false);
    let id = *engine.model.paint_order().last().unwrap();
    pointer(engine, viewport, PointerEventType::Up, x, 300.0, false);
    id
}

#[test]
fn serial_number_style_restarts_sequence_below_canvas_maximum() {
    for start in [0, 1, 3] {
        for use_patch in [false, true] {
            let (mut engine, viewport) = setup(1.0);
            let mut style = engine.editor.serial_number_style(&engine.model);
            style.number = 50;
            engine
                .set_viewport_serial_number_style(viewport, style)
                .unwrap();
            let high_id = click_serial_number(&mut engine, viewport, 100.0);
            assert_eq!(engine.model.serial_number(high_id).unwrap().number, 50);

            let mut style = engine.editor.serial_number_style(&engine.model);
            style.number = start;
            if use_patch {
                engine
                    .set_viewport_serial_number_style_patch(
                        viewport,
                        style,
                        snow_draw_engine_editor::SERIAL_NUMBER_STYLE_MIXED_NUMBER,
                    )
                    .unwrap();
            } else {
                engine
                    .set_viewport_serial_number_style(viewport, style)
                    .unwrap();
            }
            for offset in 0..3 {
                let id = click_serial_number(&mut engine, viewport, 300.0 + offset as f64 * 150.0);
                assert_eq!(
                    engine.model.serial_number(id).unwrap().number,
                    start + offset
                );
                assert_eq!(
                    engine
                        .viewport_style_toolbar_state(viewport)
                        .unwrap()
                        .serial_number_style
                        .number,
                    start + offset + 1
                );
                assert_eq!(engine.model.serial_number(high_id).unwrap().number, 50);
            }
        }
    }
}

#[test]
fn serial_number_explicit_sequence_survives_history_and_session() {
    let (mut engine, viewport) = setup(1.0);
    let first = click_serial_number(&mut engine, viewport, 100.0);
    click_serial_number(&mut engine, viewport, 300.0);
    click_serial_number(&mut engine, viewport, 500.0);
    let mut style = engine.editor.serial_number_style(&engine.model);
    style.number = 2;
    engine
        .set_viewport_serial_number_style_patch(
            viewport,
            style,
            snow_draw_engine_editor::SERIAL_NUMBER_STYLE_MIXED_NUMBER,
        )
        .unwrap();
    let duplicate = click_serial_number(&mut engine, viewport, 700.0);
    assert_eq!(engine.model.serial_number(duplicate).unwrap().number, 2);

    assert!(engine.undo().unwrap());
    assert!(engine.redo().unwrap());
    assert_eq!(engine.editor.serial_number_style(&engine.model).number, 3);
    let bytes = engine.serialize_document_session().unwrap();
    let mut restored =
        Engine::from_serialized_document_session_with_config(&bytes, Default::default()).unwrap();
    let restored_viewport = restored.create_viewport(Default::default()).unwrap();
    restored
        .set_viewport_surface_size(restored_viewport, 800, 600)
        .unwrap();
    restored
        .set_viewport_active_tool(restored_viewport, ActiveTool::SerialNumber)
        .unwrap();
    let id = click_serial_number(&mut restored, restored_viewport, 400.0);
    assert_eq!(restored.model.serial_number(id).unwrap().number, 3);
    // The explicit sequence now matches maximum + 1, but remains independent.
    restored
        .delete_all_elements_with_viewport_changes(restored_viewport)
        .unwrap();
    assert_eq!(
        restored.editor.serial_number_style(&restored.model).number,
        4
    );
    assert!(restored.undo().unwrap());
    assert_eq!(restored.model.serial_number(first).unwrap().number, 1);
    assert_eq!(
        restored.editor.serial_number_style(&restored.model).number,
        4
    );
}

#[test]
fn serial_number_creation_follows_imported_numbers_without_explicit_start() {
    use snow_draw_engine_document::{ElementMeta, SerialNumberData, Transaction};
    let (mut engine, viewport) = setup(1.0);
    let mut transaction = Transaction::new("import serial number");
    transaction.insert_serial_number(
        engine.model.peek_next_element_id(),
        ElementMeta::default(),
        SerialNumberData {
            center: Point::new(-300.0, 0.0),
            number: 50,
            ..Default::default()
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
    let mut style = engine.editor.serial_number_style(&engine.model);
    style.font_size += 1.0;
    engine
        .set_viewport_serial_number_style(viewport, style)
        .unwrap();
    let id = click_serial_number(&mut engine, viewport, 400.0);
    assert_eq!(engine.model.serial_number(id).unwrap().number, 51);
    assert_eq!(engine.editor.serial_number_style(&engine.model).number, 52);
}

#[test]
fn numberless_circle_drag_preserves_sizing_and_next_number() {
    let (mut engine, viewport) = setup(1.0);
    let mut style = engine.editor.serial_number_style(&engine.model);
    style.number = 42;
    style.font_size = 48.0;
    engine
        .set_viewport_serial_number_style(viewport, style.clone())
        .unwrap();
    style.serial_number_type = SerialNumberType::Circle;
    engine
        .set_viewport_serial_number_style(viewport, style)
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        400.0,
        300.0,
        false,
    );
    let serial_id = engine.model.paint_order()[0];
    let serial = engine.model.serial_number(serial_id).unwrap();
    assert_eq!(serial.serial_number_type, SerialNumberType::Circle);
    assert_eq!(serial.diameter, 24.0);
    assert_eq!(serial.number, 42);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        500.0,
        350.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        520.0,
        360.0,
        false,
    );
    let serial = engine.model.serial_number(serial_id).unwrap();
    assert_eq!(serial.center, Point::default());
    assert_eq!(serial.diameter, 24.0);
    let text_id = serial.text_element_id.unwrap();
    assert_eq!(
        engine.model.text(text_id).unwrap().center,
        Point::new(120.0, 60.0)
    );
    assert_eq!(
        engine.take_text_edit_request(viewport).unwrap(),
        Some(text_id)
    );
    assert_eq!(engine.editor.serial_number_style(&engine.model).number, 42);
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
                snow_draw_engine_document::TextLayoutSize::new(31.0, 36.0)
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
    assert_eq!(placed.width(), 31.0, "release persists the measured layout");
    assert_eq!(placed.height(), 36.0);
}

#[test]
fn serial_number_release_without_move_still_attaches_text() {
    // "Without move" means no intermediate Move events: the host coalesced the
    // drag, so the release position must itself attach and place the label
    // (creation_workflow.rs:962). A same-point release is a plain click and
    // intentionally attaches nothing, as the jitter test asserts.
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
fn serial_connector_tracks_ink_committed_with_edited_label() {
    use snow_draw_engine_document::{
        SerialPaintGeometry, TextPaintGeometry, resolve_serial_paint_text_connection,
    };
    use snow_draw_engine_editor::{TextCommitTarget, TextDraftCommit};

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
    let serial_id = engine.model.paint_order()[0];
    let serial = engine.model.serial_number(serial_id).unwrap().clone();
    let text_id = serial.text_element_id.unwrap();

    // The host exits label editing and commits the session with the layout it
    // re-measured for the edited text, as SnowCanvasTextEditorSession::finish
    // does: layout rectangle plus painted ink box.
    engine
        .select_element_with_viewport_changes(viewport, text_id)
        .unwrap();
    let style = engine.editor.text_style(&engine.model);
    let center = engine.model.text(text_id).unwrap().center;
    engine
        .commit_text_draft_with_viewport_changes(
            viewport,
            TextDraftCommit::new(
                TextCommitTarget::Existing(text_id),
                center,
                "grown label",
                snow_draw_engine_document::TextLayoutSize::with_content(220.0, 60.0, 208.0, 60.0),
                style,
                true,
                false,
            ),
        )
        .unwrap();

    let committed = engine.model.text(text_id).unwrap().clone();
    assert_eq!(
        committed.layout.ink(),
        snow_draw_engine_document::InkBox::new(208.0, 60.0),
        "commit must persist the measured ink, not keep the creation-time ink"
    );

    let patch = engine.acquire_patch(viewport, None).unwrap();
    let connector = patch
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| {
            if let SceneDisplayItem::SerialNumberConnector(connector) = item {
                Some(connector)
            } else {
                None
            }
        })
        .expect("committed serial label must render a connector");

    let measured_paint = TextPaintGeometry {
        center,
        width: 220.0,
        height: 60.0,
        rotation: committed.rotation,
        content_width: 208.0,
        content_height: 60.0,
        horizontal_align: committed.horizontal_align,
        vertical_align: committed.vertical_align,
        has_text: true,
        font_size: committed.font_size,
        fill: committed.fill,
        stroke: committed.stroke,
        stroke_width: committed.stroke_width,
    };
    let expected = resolve_serial_paint_text_connection(
        &SerialPaintGeometry::from_serial(&serial),
        &measured_paint,
    )
    .unwrap();
    assert!((connector.end_x - expected.end.x).abs() < 1e-9);
    assert!((connector.end_y - expected.end.y).abs() < 1e-9);
    assert_eq!(
        connector.has_baseline,
        expected.text_baseline_start.is_some()
    );
    if let (Some(expected_start), Some(expected_end)) =
        (expected.text_baseline_start, expected.text_baseline_end)
    {
        assert!((connector.baseline_start_x - expected_start.x).abs() < 1e-9);
        assert!((connector.baseline_start_y - expected_start.y).abs() < 1e-9);
        assert!((connector.baseline_end_x - expected_end.x).abs() < 1e-9);
        assert!((connector.baseline_end_y - expected_end.y).abs() < 1e-9);
    }
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
    // An empty-canvas press first spends itself on deselecting; only the next
    // press starts a new badge.
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        200.0,
        150.0,
        false,
    );
    assert!(engine.selected_ids().is_empty());
    assert_eq!(engine.model.paint_order().len(), 1);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        200.0,
        150.0,
        false,
    );
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

#[test]
fn drag_label_placeholder_follows_line_height_contract_not_stale_default() {
    // The persisted default text style keeps the wrap rectangle it was
    // initialized with (1x36 at font 30) even after the user changes the
    // default font size. The drag-attached label must size itself from the
    // published line-height contract at the serial number's font size, never
    // from that stale rectangle scaled by a font ratio (36 * 24 / 50 = 17.28).
    let (mut engine, viewport) = setup(1.0);
    let mut style = engine.editor.text_style(&engine.model);
    style.font_size = 50.0;
    engine
        .set_viewport_text_style(viewport, style, &[])
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
        460.0,
        320.0,
        false,
    );
    let serial_id = engine.model.paint_order()[0];
    let text_id = engine
        .model
        .serial_number(serial_id)
        .unwrap()
        .text_element_id
        .unwrap();
    let label = engine.model.text(text_id).unwrap();
    assert_eq!(label.font_size, 24.0);
    assert_eq!(label.width(), 1.0);
    assert_eq!(
        label.height(),
        snow_draw_engine_document::text_line_height(24.0),
        "drag label placeholder must be one contract line height tall, not the stale default rectangle"
    );
}

#[test]
fn drag_and_toolbar_bound_labels_share_styling_and_layout() {
    // The floating toolbar's Create Text button and the serial drag are the two
    // ways to attach a bound label. Given the same default style and the same
    // host measurement, both must produce the same label styling and layout;
    // only the center differs (pointer placement vs. beside the badge).
    let (mut engine, viewport) = setup(1.0);
    let mut style = engine.editor.text_style(&engine.model);
    style.font_size = 50.0;
    engine
        .set_viewport_text_style(viewport, style, &[])
        .unwrap();

    // Toolbar path: click creates the badge, then Create Text measures first.
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
    let toolbar_serial_id = engine.model.paint_order()[0];
    engine
        .select_element_with_viewport_changes(viewport, toolbar_serial_id)
        .unwrap();
    let measured = snow_draw_engine_document::TextLayoutSize::with_content(31.0, 36.0, 7.0, 36.0);
    let (_, toolbar_text_id) = engine
        .create_serial_number_text_with_viewport_changes(viewport, measured)
        .unwrap();
    let toolbar_text = engine
        .model
        .text(toolbar_text_id.expect("toolbar path creates the label"))
        .unwrap()
        .clone();

    // The toolbar path leaves the new label selected; a blank-canvas press
    // deselects it before the drag path can create the next badge.
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        100.0,
        100.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        100.0,
        100.0,
        false,
    );
    assert!(engine.selected_ids().is_empty());

    // Drag path: press, drag, host measurement lands, release.
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        100.0,
        100.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        160.0,
        120.0,
        false,
    );
    let drag_serial_id = *engine
        .model
        .paint_order()
        .iter()
        .filter(|id| engine.model.serial_number(**id).is_ok())
        .nth(1)
        .unwrap();
    let drag_text_id = engine
        .model
        .serial_number(drag_serial_id)
        .unwrap()
        .text_element_id
        .unwrap();
    engine
        .apply_serial_number_label_layout(viewport, drag_text_id, measured)
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        160.0,
        120.0,
        false,
    );
    let drag_text = engine.model.text(drag_text_id).unwrap().clone();

    assert_eq!(drag_text.font_size, toolbar_text.font_size);
    assert_eq!(drag_text.color, toolbar_text.color);
    assert_eq!(drag_text.fill, toolbar_text.fill);
    assert_eq!(drag_text.fill_style, toolbar_text.fill_style);
    assert_eq!(drag_text.stroke, toolbar_text.stroke);
    assert_eq!(drag_text.stroke_width, toolbar_text.stroke_width);
    assert_eq!(drag_text.font_family, toolbar_text.font_family);
    assert_eq!(drag_text.corner_radii, toolbar_text.corner_radii);
    assert_eq!(drag_text.horizontal_align, toolbar_text.horizontal_align);
    assert_eq!(drag_text.vertical_align, toolbar_text.vertical_align);
    assert_eq!(drag_text.opacity, toolbar_text.opacity);
    assert_eq!(drag_text.auto_resize, toolbar_text.auto_resize);
    assert_eq!(drag_text.rotation, toolbar_text.rotation);
    assert_eq!(drag_text.layout, toolbar_text.layout);
    assert_ne!(drag_text.center, toolbar_text.center);
}

#[test]
fn serial_number_numeric_type_survives_creation_history_templates_and_sessions() {
    use snow_draw_engine_document::SerialNumberNumericType as Numeric;
    let (mut engine, viewport) = setup(1.0);
    let mut style = engine.editor.serial_number_style(&engine.model);
    style.number = 27;
    style.numeric_type = Numeric::LowercaseLetters;
    engine
        .set_viewport_serial_number_style(viewport, style)
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
        PointerEventType::Up,
        400.0,
        300.0,
        false,
    );
    let id = engine.model.paint_order()[0];
    assert_eq!(
        engine.model.serial_number(id).unwrap().numeric_type,
        Numeric::LowercaseLetters
    );
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    let before = engine.model.serial_number(id).unwrap().clone();
    let mut style = engine.editor.serial_number_style(&engine.model);
    style.numeric_type = Numeric::Roman;
    engine
        .set_viewport_serial_number_style_patch(
            viewport,
            style,
            snow_draw_engine_editor::SERIAL_NUMBER_STYLE_MIXED_NUMERIC_TYPE,
        )
        .unwrap();
    assert_eq!(engine.model.serial_number(id).unwrap().number, 27);
    let patch = engine.acquire_patch(viewport, None).unwrap();
    let displayed = patch
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| {
            if let SceneDisplayItem::SerialNumber(serial) = item {
                Some(serial)
            } else {
                None
            }
        })
        .unwrap();
    assert_eq!(displayed.label, "XXVII");
    assert_eq!(
        displayed.diameter,
        engine.model.serial_number(id).unwrap().diameter
    );
    assert!(engine.undo().unwrap());
    assert_eq!(engine.model.serial_number(id).unwrap(), &before);
    assert!(engine.redo().unwrap());
    assert_eq!(
        engine.model.serial_number(id).unwrap().numeric_type,
        Numeric::Roman
    );

    let session = engine.serialize_document_session().unwrap();
    let history = engine.serialize_document_history().unwrap();
    for session_payload in [true, false] {
        let bytes = if session_payload { &session } else { &history };
        let restored = if session_payload {
            Engine::from_serialized_document_session_with_config(bytes, Default::default()).unwrap()
        } else {
            Engine::from_serialized_document_history_with_config(bytes, Default::default()).unwrap()
        };
        assert_eq!(
            restored.model.serial_number(id).unwrap().numeric_type,
            Numeric::Roman
        );
        let mut legacy: serde_json::Value = serde_json::from_slice(bytes).unwrap();
        fn remove_format(value: &mut serde_json::Value) {
            match value {
                serde_json::Value::Object(object) => {
                    object.remove("numeric_type");
                    for child in object.values_mut() {
                        remove_format(child);
                    }
                }
                serde_json::Value::Array(array) => {
                    for child in array {
                        remove_format(child);
                    }
                }
                _ => {}
            }
        }
        remove_format(&mut legacy);
        let encoded = serde_json::to_vec(&legacy).unwrap();
        let restored = if session_payload {
            Engine::from_serialized_document_session_with_config(&encoded, Default::default())
                .unwrap()
        } else {
            Engine::from_serialized_document_history_with_config(&encoded, Default::default())
                .unwrap()
        };
        assert_eq!(
            restored.model.serial_number(id).unwrap().numeric_type,
            Numeric::Arabic
        );
    }
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    let template = engine.serialize_selected_draw_template().unwrap();
    engine
        .duplicate_selected_with_viewport_changes(viewport, Point::new(100.0, 0.0))
        .unwrap();
    assert_eq!(
        engine
            .model
            .serial_number(engine.selected_ids()[0])
            .unwrap()
            .numeric_type,
        Numeric::Roman
    );
    engine
        .insert_draw_template_with_viewport_changes(viewport, &template, Point::new(200.0, 0.0))
        .unwrap();
    assert_eq!(
        engine
            .model
            .serial_number(engine.selected_ids()[0])
            .unwrap()
            .numeric_type,
        Numeric::Roman
    );
}

#[test]
fn serial_number_numeric_type_repaints_label_and_preserves_bounds_and_attached_connector() {
    use snow_draw_engine_display::{
        DecorationRevision, OverlayRevision, PatchCursor, SceneRevision,
    };
    use snow_draw_engine_document::{
        SerialNumberNumericType, resolve_serial_number_text_connection,
    };
    let (mut engine, viewport) = setup(1.0);
    let mut style = engine.editor.serial_number_style(&engine.model);
    style.number = 888;
    engine
        .set_viewport_serial_number_style(viewport, style)
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
        650.0,
        500.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        650.0,
        500.0,
        false,
    );
    let id = engine.model.paint_order()[0];
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    let original = engine.model.serial_number(id).unwrap().clone();
    let expected = resolve_serial_number_text_connection(
        &original,
        engine
            .model
            .text(original.text_element_id.unwrap())
            .unwrap(),
    )
    .unwrap();
    let before_bounds = engine
        .editor
        .presentation_state(
            &engine.model,
            &engine.viewports.get(&viewport).unwrap().view,
        )
        .selection_bounds
        .unwrap();
    let before = engine.acquire_patch(viewport, None).unwrap();
    let cursor = PatchCursor {
        scene_revision: SceneRevision(before.scene.revision),
        decoration_revision: DecorationRevision(before.decoration.revision),
        overlay_revision: OverlayRevision(before.overlay.revision),
    };
    let mut style = engine.editor.serial_number_style(&engine.model);
    style.numeric_type = SerialNumberNumericType::Chinese;
    engine
        .set_viewport_serial_number_style_patch(
            viewport,
            style,
            snow_draw_engine_editor::SERIAL_NUMBER_STYLE_MIXED_NUMERIC_TYPE,
        )
        .unwrap();
    let serial = engine.model.serial_number(id).unwrap();
    let mut expected_serial = original;
    expected_serial.numeric_type = SerialNumberNumericType::Chinese;
    assert_eq!(serial, &expected_serial);
    let after_bounds = engine
        .editor
        .presentation_state(
            &engine.model,
            &engine.viewports.get(&viewport).unwrap().view,
        )
        .selection_bounds
        .unwrap();
    assert_eq!(after_bounds, before_bounds);
    let patch = engine.acquire_patch(viewport, Some(cursor)).unwrap();
    assert!(
        !patch.scene.reset,
        "format changes support incremental scene updates"
    );
    let half = serial.diameter / 2.0;
    assert!(
        patch.scene.dirty_regions.iter().any(|region| {
            region.min_x <= 400.0 - half
                && region.max_x >= 400.0 + half
                && region.min_y <= 300.0 - half
                && region.max_y >= 300.0 + half
        }),
        "repaint covers the fixed badge area when its label changes"
    );
    assert!(
        patch
            .scene
            .ops
            .iter()
            .flat_map(|op| &op.insert_items)
            .any(|item| {
                matches!(item, SceneDisplayItem::SerialNumber(serial)
            if serial.label == snow_draw_engine_document::format_serial_number(
                888, SerialNumberNumericType::Chinese))
            })
    );
    let full = engine.acquire_patch(viewport, None).unwrap();
    let connector = full
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| {
            if let SceneDisplayItem::SerialNumberConnector(connector) = item {
                Some(connector)
            } else {
                None
            }
        })
        .expect("the attached connector remains in the scene");
    assert!((connector.start_x - expected.start.x).abs() < 1e-9);
    assert!((connector.start_y - expected.start.y).abs() < 1e-9);
    assert!((connector.end_x - expected.end.x).abs() < 1e-9);
    assert!((connector.end_y - expected.end.y).abs() < 1e-9);
}

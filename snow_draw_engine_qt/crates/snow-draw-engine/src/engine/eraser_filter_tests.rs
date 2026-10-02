use super::*;
use snow_draw_engine_document::{
    CanvasFilterType, ElementData, ElementMeta, FilterData, Operation, PenFilterData, Transaction,
};
use snow_draw_engine_editor::{BRUSH_ERASER_STYLE_PROPERTY_STROKE_WIDTH, BrushEraserStyle};
use snow_draw_engine_interaction::{
    Modifiers, PointerButton, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
};

fn fixture(tool: ActiveTool) -> (Engine, ViewportId) {
    let mut engine = Engine::default();
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine.set_viewport_active_tool(viewport, tool).unwrap();
    (engine, viewport)
}

fn pointer(kind: PointerEventType, x: f64, y: f64) -> InputEvent {
    InputEvent::Pointer(PointerEvent {
        pointer_id: 1,
        event_type: kind,
        device: PointerDevice::Mouse,
        position: Point::new(x, y),
        button: Some(PointerButton::Primary),
        buttons: PointerButtons(PointerButtons::PRIMARY),
        modifiers: Modifiers::default(),
    })
}

fn gesture(engine: &mut Engine, viewport: ViewportId, from: (f64, f64), to: (f64, f64)) {
    engine
        .process_input(viewport, pointer(PointerEventType::Down, from.0, from.1))
        .unwrap();
    engine
        .process_input(viewport, pointer(PointerEventType::Move, to.0, to.1))
        .unwrap();
    engine
        .process_input(viewport, pointer(PointerEventType::Up, to.0, to.1))
        .unwrap();
}

#[test]
fn eraser_filters_create_immutable_restore_coverage_and_undo_redo() {
    for tool in [ActiveTool::RectangleEraser, ActiveTool::BrushEraser] {
        let (mut engine, viewport) = fixture(tool);
        gesture(&mut engine, viewport, (300.0, 200.0), (400.0, 260.0));
        let id = engine.model.paint_order()[0];
        let record = engine.model.element(id).unwrap().clone();
        engine
            .set_viewport_brush_eraser_creation_style(
                viewport,
                BrushEraserStyle { stroke_width: 52.0 },
                BRUSH_ERASER_STYLE_PROPERTY_STROKE_WIDTH,
            )
            .unwrap();
        assert!(record.data.is_background_restore());
        assert!(engine.selected_ids().is_empty());
        assert_eq!(
            engine.select_element_with_viewport_changes(viewport, id),
            Err(ErrorCode::InvalidState)
        );
        for operation in [
            Operation::UpdateElementData {
                id,
                data: record.data.clone(),
            },
            Operation::UpdateElementMeta {
                id,
                meta: ElementMeta {
                    visible: false,
                    locked: false,
                },
            },
        ] {
            let mut tx = Transaction::new("attempt eraser edit");
            tx.push(operation);
            assert_eq!(
                engine.model.apply_transaction(tx),
                Err(ErrorCode::InvalidState)
            );
            assert_eq!(engine.model.element(id).unwrap(), &record);
        }
        assert!(engine.undo().unwrap());
        assert!(engine.model.paint_order().is_empty());
        assert!(engine.redo().unwrap());
        assert_eq!(engine.model.element(id).unwrap(), &record);
        engine
            .delete_all_elements_with_viewport_changes(viewport)
            .unwrap();
        assert!(engine.model.paint_order().is_empty());
        assert!(engine.undo().unwrap());
        assert_eq!(engine.model.element(id).unwrap(), &record);
    }
}

#[test]
fn eraser_filters_brush_dot_width_is_latched_and_updates_shared_future_defaults() {
    let (mut engine, viewport) = fixture(ActiveTool::BrushEraser);
    let other = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine.set_viewport_surface_size(other, 800, 600).unwrap();
    engine
        .process_input(viewport, pointer(PointerEventType::Down, 400.0, 300.0))
        .unwrap();
    engine
        .set_viewport_brush_eraser_creation_style(
            other,
            BrushEraserStyle { stroke_width: 52.0 },
            BRUSH_ERASER_STYLE_PROPERTY_STROKE_WIDTH,
        )
        .unwrap();
    let preview = engine.acquire_patch(viewport, None).unwrap();
    let preview_filter = preview
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| {
            if let snow_draw_engine_display::SceneDisplayItem::Filter(filter) = item {
                Some(filter)
            } else {
                None
            }
        })
        .unwrap();
    assert_eq!(preview_filter.stroke_width, 30.0);
    assert_eq!(
        preview_filter.filter.filter_type,
        snow_draw_engine_display::DisplayFilterType::RestoreBackground
    );
    engine
        .process_input(viewport, pointer(PointerEventType::Up, 400.0, 300.0))
        .unwrap();
    let filter = engine
        .model
        .pen_filter(engine.model.paint_order()[0])
        .unwrap();
    assert_eq!(filter.stroke_width, 30.0);
    assert_eq!(filter.points, vec![[0.0, 0.0]]);
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .brush_eraser_style
            .stroke_width,
        52.0
    );
    assert_eq!(
        engine
            .viewport_style_toolbar_state(other)
            .unwrap()
            .brush_eraser_style
            .stroke_width,
        52.0
    );
    gesture(&mut engine, viewport, (450.0, 300.0), (450.0, 300.0));
    assert_eq!(
        engine
            .model
            .pen_filter(engine.model.paint_order()[1])
            .unwrap()
            .stroke_width,
        52.0
    );
    engine
        .set_viewport_active_tool(viewport, ActiveTool::PenFilter)
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .filter_style
            .stroke_width,
        30.0
    );
}

#[test]
fn eraser_filters_cannot_be_selected_bound_or_deleted_by_legacy_eraser() {
    let (mut engine, viewport) = fixture(ActiveTool::RectangleEraser);
    gesture(&mut engine, viewport, (300.0, 200.0), (500.0, 400.0));
    engine
        .set_viewport_active_tool(viewport, ActiveTool::BrushEraser)
        .unwrap();
    gesture(&mut engine, viewport, (400.0, 300.0), (400.0, 300.0));
    let ids = engine.model.paint_order().to_vec();
    assert!(
        engine
            .model
            .bindable_element_states_with_overrides(&[])
            .is_empty()
    );
    assert!(
        engine
            .model
            .elements_at_with_tolerance(Point::default(), 8.0)
            .is_empty()
    );
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    gesture(&mut engine, viewport, (280.0, 180.0), (520.0, 420.0));
    assert!(engine.selected_ids().is_empty());
    gesture(&mut engine, viewport, (400.0, 300.0), (400.0, 300.0));
    assert!(engine.selected_ids().is_empty());
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Eraser)
        .unwrap();
    gesture(&mut engine, viewport, (400.0, 300.0), (450.0, 300.0));
    assert_eq!(engine.model.paint_order(), ids);
    let deletion = serde_json::to_vec(
        &serde_json::json!({"version":1,"operations":[{"type":"delete","ids":[ids[0]]}]}),
    )
    .unwrap();
    assert!(engine.apply_annotation_json(&deletion).is_err());
    assert_eq!(engine.model.paint_order(), ids);
}

#[test]
fn eraser_filters_do_not_allow_regular_filter_conversion_or_bad_widths() {
    let (mut engine, viewport) = fixture(ActiveTool::RectangleFilter);
    gesture(&mut engine, viewport, (300.0, 200.0), (400.0, 260.0));
    let id = engine.model.paint_order()[0];
    let mut filter = *engine.model.filter(id).unwrap();
    filter.filter_type = CanvasFilterType::RestoreBackground;
    filter.strength = 1.0;
    let mut tx = Transaction::new("convert regular filter");
    tx.update_filter(id, filter);
    assert_eq!(
        engine.model.apply_transaction(tx),
        Err(ErrorCode::InvalidState)
    );
    for width in [f64::NAN, f64::INFINITY, 0.0, 73.0] {
        assert!(
            engine
                .set_viewport_brush_eraser_creation_style(
                    viewport,
                    BrushEraserStyle {
                        stroke_width: width
                    },
                    BRUSH_ERASER_STYLE_PROPERTY_STROKE_WIDTH
                )
                .is_err()
        );
    }
    assert!(
        engine
            .set_viewport_brush_eraser_creation_style(viewport, BrushEraserStyle::default(), 2)
            .is_err()
    );
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .brush_eraser_style,
        BrushEraserStyle::default()
    );
    assert!(
        PenFilterData::from_global_points(
            &[Point::default()],
            CanvasFilterType::Mosaic,
            1.0,
            30.0,
            1.0
        )
        .is_none()
    );
}

#[test]
fn eraser_filters_cancel_and_zero_size_rectangles_commit_nothing() {
    for tool in [ActiveTool::RectangleEraser, ActiveTool::BrushEraser] {
        let (mut engine, viewport) = fixture(tool);
        engine
            .process_input(viewport, pointer(PointerEventType::Down, 300.0, 200.0))
            .unwrap();
        engine
            .process_input(viewport, pointer(PointerEventType::Move, 400.0, 260.0))
            .unwrap();
        engine
            .process_input(viewport, pointer(PointerEventType::Cancel, 400.0, 260.0))
            .unwrap();
        assert!(engine.model.paint_order().is_empty());
        assert!(!engine.history_state().can_undo);
    }
    let (mut engine, viewport) = fixture(ActiveTool::RectangleEraser);
    gesture(&mut engine, viewport, (300.0, 200.0), (300.0, 200.0));
    assert!(engine.model.paint_order().is_empty());
}

#[test]
fn eraser_filters_session_history_and_old_defaults_round_trip() {
    let (mut engine, viewport) = fixture(ActiveTool::BrushEraser);
    engine
        .set_viewport_brush_eraser_creation_style(
            viewport,
            BrushEraserStyle { stroke_width: 47.0 },
            BRUSH_ERASER_STYLE_PROPERTY_STROKE_WIDTH,
        )
        .unwrap();
    gesture(&mut engine, viewport, (400.0, 300.0), (400.0, 300.0));
    let bytes = engine.serialize_document_session().unwrap();
    let mut restored =
        Engine::from_serialized_document_session_with_config(&bytes, EngineConfig::default())
            .unwrap();
    restored.create_viewport(ViewportConfig::default()).unwrap();
    assert_eq!(restored.editor.brush_eraser_style().stroke_width, 47.0);
    assert_eq!(restored.model.document(), engine.model.document());
    assert!(restored.history_state().can_undo);
    restored.undo_with_viewport_changes().unwrap();
    assert!(restored.model.paint_order().is_empty());
    restored.redo_with_viewport_changes().unwrap();
    assert_eq!(restored.model.paint_order().len(), 1);
    let history = engine.serialize_document_history().unwrap();
    let mut restored =
        Engine::from_serialized_document_history_with_config(&history, EngineConfig::default())
            .unwrap();
    restored.create_viewport(ViewportConfig::default()).unwrap();
    assert!(restored.history_state().can_undo);
    restored.undo_with_viewport_changes().unwrap();
    assert!(restored.model.paint_order().is_empty());
    restored.redo_with_viewport_changes().unwrap();
    assert_eq!(restored.model.paint_order().len(), 1);
    engine.clear_document_preserving_viewports().unwrap();
    assert_eq!(engine.editor.brush_eraser_style().stroke_width, 47.0);
    let mut old: serde_json::Value =
        serde_json::from_slice(&engine.serialize_document_session().unwrap()).unwrap();
    old["schemaVersion"] = serde_json::json!(5);
    old["editor"].as_object_mut().unwrap().remove("brushEraser");
    let old = serde_json::to_vec(&old).unwrap();
    let restored =
        Engine::from_serialized_document_session_with_config(&old, EngineConfig::default())
            .unwrap();
    assert_eq!(
        restored.editor.brush_eraser_style(),
        BrushEraserStyle::default()
    );
}

#[test]
fn eraser_filters_batch_geometry_stays_bounded_and_preserves_shift_endpoint() {
    let (mut engine, viewport) = fixture(ActiveTool::BrushEraser);
    engine
        .process_input(viewport, pointer(PointerEventType::Down, 100.0, 100.0))
        .unwrap();
    let events = (1..=2048)
        .map(|index| pointer(PointerEventType::Move, 100.0 + index as f64 * 0.1, 100.0))
        .collect::<Vec<_>>();
    engine
        .process_pointer_move_batch_with_viewport_changes(viewport, &events)
        .unwrap();
    let mut event = pointer(PointerEventType::Move, 600.0, 400.0);
    if let InputEvent::Pointer(ref mut event) = event {
        event.modifiers.shift = true;
    }
    engine.process_input(viewport, event).unwrap();
    let mut event = pointer(PointerEventType::Up, 700.0, 400.0);
    if let InputEvent::Pointer(ref mut event) = event {
        event.modifiers.shift = true;
    }
    engine.process_input(viewport, event).unwrap();
    let filter = engine
        .model
        .pen_filter(engine.model.paint_order()[0])
        .unwrap();
    assert!(filter.points.len() < 16);
    let points = filter.global_points();
    assert_eq!(points[0], Point::new(-300.0, -200.0));
    assert_eq!(*points.last().unwrap(), Point::new(300.0, 100.0));
}

#[test]
fn eraser_filters_cannot_hide_or_change_stored_coverage() {
    let (mut engine, viewport) = fixture(ActiveTool::RectangleEraser);
    gesture(&mut engine, viewport, (300.0, 200.0), (400.0, 260.0));
    let id = engine.model.paint_order()[0];
    let mut filter = *engine.model.filter(id).unwrap();
    filter.width += 10.0;
    let mut tx = Transaction::new("modify eraser");
    tx.update_filter(id, filter);
    assert_eq!(
        engine.model.apply_transaction(tx),
        Err(ErrorCode::InvalidState)
    );
    assert!(matches!(
        engine.model.element(id).unwrap().data,
        ElementData::Filter(_)
    ));
    let invalid = FilterData {
        filter_type: CanvasFilterType::RestoreBackground,
        ..FilterData::default()
    };
    assert!(snow_draw_engine_document::validate_filter(&invalid).is_err());
}

#[test]
fn eraser_filters_restore_rejects_malformed_coverage_atomically() {
    for tool in [ActiveTool::RectangleEraser, ActiveTool::BrushEraser] {
        let (mut engine, viewport) = fixture(tool);
        gesture(&mut engine, viewport, (300.0, 200.0), (400.0, 260.0));
        let mut record = engine
            .model
            .element(engine.model.paint_order()[0])
            .unwrap()
            .clone();
        record.id = ElementId {
            index: 1,
            generation: 1,
        };
        let mut invalid_data = Vec::new();
        match &record.data {
            ElementData::Filter(filter) => {
                for invalid in [
                    FilterData {
                        opacity: 0.5,
                        ..*filter
                    },
                    FilterData {
                        strength: 0.5,
                        ..*filter
                    },
                    FilterData {
                        width: -1.0,
                        ..*filter
                    },
                ] {
                    invalid_data.push(ElementData::Filter(invalid));
                }
            }
            ElementData::PenFilter(filter) => {
                for invalid in [
                    PenFilterData {
                        opacity: 0.5,
                        ..filter.clone()
                    },
                    PenFilterData {
                        strength: 0.5,
                        ..filter.clone()
                    },
                    PenFilterData {
                        stroke_width: 0.0,
                        ..filter.clone()
                    },
                    PenFilterData {
                        points: Vec::new(),
                        ..filter.clone()
                    },
                ] {
                    invalid_data.push(ElementData::PenFilter(invalid));
                }
            }
            _ => unreachable!(),
        }
        let mut model = snow_draw_engine_model::DocumentModel::new();
        let before = model.document().clone();
        for data in invalid_data {
            let mut tx = Transaction::new("restore malformed eraser");
            tx.insert_filter(
                ElementId {
                    index: 0,
                    generation: 1,
                },
                ElementMeta::default(),
                FilterData::default(),
            );
            tx.push(Operation::RestoreElement {
                element: snow_draw_engine_document::ElementRecord {
                    data,
                    ..record.clone()
                },
                paint_index: 0,
            });
            assert_eq!(model.apply_transaction(tx), Err(ErrorCode::InvalidArgument));
            assert_eq!(model.document(), &before);
        }
    }
}

#[test]
fn eraser_filters_templates_reject_restore_records_without_mutation() {
    for tool in [ActiveTool::RectangleEraser, ActiveTool::BrushEraser] {
        let (mut engine, viewport) = fixture(tool);
        gesture(&mut engine, viewport, (300.0, 200.0), (400.0, 260.0));
        let id = engine.model.paint_order()[0];
        let restore = engine.model.element(id).unwrap().clone();
        let before = engine.serialize_document_session().unwrap();
        let mut template = snow_draw_engine_editor::DrawTemplate {
            schema_version: 1,
            source_center: Point::default(),
            selected_ids: vec![id],
            elements: vec![restore],
        };
        for include_restore_in_selection in [true, false] {
            if !include_restore_in_selection {
                let ordinary_id = ElementId {
                    index: id.index + 1,
                    generation: id.generation,
                };
                template
                    .elements
                    .push(snow_draw_engine_document::ElementRecord {
                        id: ordinary_id,
                        meta: ElementMeta::default(),
                        data: ElementData::Filter(FilterData::default()),
                    });
                template.selected_ids = vec![ordinary_id];
            }
            let payload = serde_json::to_vec(&template).unwrap();
            assert_eq!(
                engine.insert_draw_template_with_viewport_changes(
                    viewport,
                    &payload,
                    Point::new(100.0, 100.0)
                ),
                Err(ErrorCode::InvalidArgument)
            );
            assert_eq!(engine.serialize_document_session().unwrap(), before);
        }
    }
}

#[test]
fn eraser_filters_restored_selection_is_filtered_and_history_snapshots_are_rejected() {
    let (mut engine, viewport) = fixture(ActiveTool::RectangleEraser);
    gesture(&mut engine, viewport, (300.0, 200.0), (500.0, 400.0));
    let restore = engine.model.paint_order()[0];
    engine
        .set_viewport_active_tool(viewport, ActiveTool::RectangleFilter)
        .unwrap();
    gesture(&mut engine, viewport, (600.0, 200.0), (700.0, 300.0));
    let ordinary = engine.model.paint_order()[1];
    let snapshot: snow_draw_engine_editor::DocumentSyncSnapshot =
        serde_json::from_value(serde_json::json!({
            "selectedIds": [restore, ordinary],
            "primaryId": restore
        }))
        .unwrap();
    assert_eq!(
        snapshot.validate_session(&engine.model),
        Err(ErrorCode::InvalidArgument)
    );
    engine
        .editor
        .restore_history_selection(&engine.model, &snapshot);
    assert_eq!(engine.selected_ids(), vec![ordinary]);
    let history: serde_json::Value =
        serde_json::from_slice(&engine.serialize_document_history().unwrap()).unwrap();
    for snapshot_name in ["undoSnapshot", "redoSnapshot"] {
        let mut invalid = history.clone();
        let last = invalid["history"]["undoStack"]
            .as_array_mut()
            .unwrap()
            .last_mut()
            .unwrap();
        last[snapshot_name] = serde_json::json!({
            "selectedIds": [restore],
            "primaryId": restore
        });
        let invalid = serde_json::to_vec(&invalid).unwrap();
        assert!(
            Engine::from_serialized_document_history_with_config(&invalid, EngineConfig::default())
                .is_err()
        );
    }
    engine
        .delete_selected_with_viewport_changes(viewport)
        .unwrap();
    assert_eq!(engine.model.paint_order(), &[restore]);
    assert!(engine.undo().unwrap());
    assert_eq!(engine.model.paint_order(), &[restore, ordinary]);

    let (mut legacy, viewport) = fixture(ActiveTool::RectangleFilter);
    gesture(&mut legacy, viewport, (300.0, 200.0), (400.0, 260.0));
    let mut history: serde_json::Value =
        serde_json::from_slice(&legacy.serialize_document_history().unwrap()).unwrap();
    history["schemaVersion"] = serde_json::json!(5);
    let history = serde_json::to_vec(&history).unwrap();
    let restored =
        Engine::from_serialized_document_history_with_config(&history, EngineConfig::default())
            .unwrap();
    assert_eq!(restored.model.document(), legacy.model.document());
    assert_eq!(
        restored.editor.brush_eraser_style(),
        BrushEraserStyle::default()
    );
}

#[test]
fn eraser_filters_whole_canvas_and_quick_selection_only_select_ordinary_filters() {
    let (mut engine, viewport) = fixture(ActiveTool::RectangleFilter);
    engine.set_quick_selection_disabled_tools(0).unwrap();
    gesture(&mut engine, viewport, (300.0, 200.0), (500.0, 400.0));
    let ordinary_rectangle = engine.model.paint_order()[0];
    engine
        .set_viewport_active_tool(viewport, ActiveTool::PenFilter)
        .unwrap();
    gesture(&mut engine, viewport, (300.0, 300.0), (500.0, 300.0));
    let ordinary_brush = engine.model.paint_order()[1];
    engine
        .set_viewport_active_tool(viewport, ActiveTool::RectangleEraser)
        .unwrap();
    gesture(&mut engine, viewport, (300.0, 200.0), (500.0, 400.0));
    engine
        .set_viewport_active_tool(viewport, ActiveTool::BrushEraser)
        .unwrap();
    gesture(&mut engine, viewport, (300.0, 300.0), (500.0, 300.0));
    assert!(engine.selected_ids().is_empty());
    let restore_records = engine.model.paint_order()[2..]
        .iter()
        .map(|id| engine.model.element(*id).unwrap().clone())
        .collect::<Vec<_>>();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    gesture(&mut engine, viewport, (0.0, 0.0), (800.0, 600.0));
    let selected = engine.selected_ids();
    assert_eq!(selected.len(), 2);
    assert!(selected.contains(&ordinary_rectangle));
    assert!(selected.contains(&ordinary_brush));
    for (tool, expected) in [
        (ActiveTool::RectangleFilter, ordinary_rectangle),
        (ActiveTool::PenFilter, ordinary_brush),
    ] {
        engine.set_viewport_active_tool(viewport, tool).unwrap();
        gesture(&mut engine, viewport, (400.0, 300.0), (400.0, 300.0));
        assert_eq!(engine.selected_ids(), vec![expected]);
        for record in &restore_records {
            assert_eq!(engine.model.element(record.id).unwrap(), record);
        }
    }
    engine
        .duplicate_selected_with_viewport_changes(viewport, Point::new(0.0, 100.0))
        .unwrap();
    engine
        .reorder_selected_with_viewport_changes(viewport, 0)
        .unwrap();
    engine
        .delete_selected_with_viewport_changes(viewport)
        .unwrap();
    for record in &restore_records {
        assert_eq!(engine.model.element(record.id).unwrap(), record);
    }
    assert_eq!(engine.model.paint_order().len(), 4);
    Engine::from_serialized_document_session_with_config(
        &engine.serialize_document_session().unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
}

#[test]
fn eraser_filters_direct_and_imported_arrow_bindings_are_rejected_atomically() {
    use snow_draw_engine_core::arrow::{ArrowType, BindMode, StrokeStyle};
    use snow_draw_engine_document::{ArrowData, ArrowEndpointBinding};

    for tool in [ActiveTool::RectangleEraser, ActiveTool::BrushEraser] {
        let (mut engine, viewport) = fixture(tool);
        gesture(&mut engine, viewport, (300.0, 200.0), (400.0, 260.0));
        let restore_id = engine.model.paint_order()[0];
        let restore_record = engine.model.element(restore_id).unwrap().clone();
        let unbound = ArrowData::from_global_points(
            &[Point::new(0.0, 0.0), Point::new(50.0, 30.0)],
            Default::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        let mut bound = unbound.clone();
        bound.start_binding = Some(ArrowEndpointBinding {
            element_id: restore_id,
            fixed_point: [0.5, 0.5],
            mode: BindMode::Inside,
        });
        let arrow_id = engine.model.peek_next_element_id();
        let before = engine.model.document().clone();
        for operation in [
            Operation::InsertElement {
                id: arrow_id,
                meta: ElementMeta::default(),
                data: ElementData::Arrow(bound.clone()),
            },
            Operation::RestoreElement {
                element: snow_draw_engine_document::ElementRecord {
                    id: arrow_id,
                    meta: ElementMeta::default(),
                    data: ElementData::Arrow(bound.clone()),
                },
                paint_index: 0,
            },
        ] {
            let mut tx = Transaction::new("bind arrow to eraser");
            tx.push(operation);
            assert_eq!(
                engine.model.apply_transaction(tx),
                Err(ErrorCode::InvalidArgument)
            );
            assert_eq!(engine.model.document(), &before);
        }
        let mut tx = Transaction::new("insert unbound arrow");
        tx.insert_arrow(arrow_id, ElementMeta::default(), unbound.clone());
        engine.model.apply_transaction(tx).unwrap();
        let before = engine.model.document().clone();
        let mut tx = Transaction::new("bind existing arrow to eraser");
        tx.update_arrow(arrow_id, bound.clone());
        assert_eq!(
            engine.model.apply_transaction(tx),
            Err(ErrorCode::InvalidArgument)
        );
        assert_eq!(engine.model.document(), &before);

        for history_only in [false, true] {
            let bytes = if history_only {
                engine.serialize_document_history().unwrap()
            } else {
                engine.serialize_document_session().unwrap()
            };
            let mut invalid: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
            invalid["document"]["slots"][arrow_id.index as usize]["data"] =
                serde_json::to_value(ElementData::Arrow(bound.clone())).unwrap();
            let invalid = serde_json::to_vec(&invalid).unwrap();
            let result = if history_only {
                Engine::from_serialized_document_history_with_config(
                    &invalid,
                    EngineConfig::default(),
                )
            } else {
                Engine::from_serialized_document_session_with_config(
                    &invalid,
                    EngineConfig::default(),
                )
            };
            assert_eq!(result.unwrap_err(), ErrorCode::InvalidArgument);
        }

        // Check the reverse operation order: an arrow may refer to a later insert.
        let arrow_id = ElementId {
            index: 0,
            generation: 1,
        };
        let restore_id = ElementId {
            index: 1,
            generation: 1,
        };
        bound.start_binding.as_mut().unwrap().element_id = restore_id;
        for restore_operation in [
            Operation::InsertElement {
                id: restore_id,
                meta: ElementMeta::default(),
                data: restore_record.data.clone(),
            },
            Operation::RestoreElement {
                element: snow_draw_engine_document::ElementRecord {
                    id: restore_id,
                    meta: ElementMeta::default(),
                    data: restore_record.data.clone(),
                },
                paint_index: 0,
            },
        ] {
            let mut model = snow_draw_engine_model::DocumentModel::new();
            let before = model.document().clone();
            let mut tx = Transaction::new("insert arrow before restoration target");
            tx.insert_arrow(arrow_id, ElementMeta::default(), bound.clone());
            tx.push(restore_operation);
            assert_eq!(model.apply_transaction(tx), Err(ErrorCode::InvalidArgument));
            assert_eq!(model.document(), &before);
        }
    }
}

#[test]
fn eraser_filters_rectangle_alt_and_shift_match_rectangle_filter_controls() {
    for modifiers in [
        Modifiers {
            shift: true,
            ..Default::default()
        },
        Modifiers {
            alt: true,
            ..Default::default()
        },
        Modifiers {
            shift: true,
            alt: true,
            ..Default::default()
        },
    ] {
        let mut geometry = Vec::new();
        for tool in [ActiveTool::RectangleFilter, ActiveTool::RectangleEraser] {
            let (mut engine, viewport) = fixture(tool);
            for (kind, x, y) in [
                (PointerEventType::Down, 400.0, 300.0),
                (PointerEventType::Move, 520.0, 350.0),
                (PointerEventType::Up, 520.0, 350.0),
            ] {
                let mut event = pointer(kind, x, y);
                if let InputEvent::Pointer(ref mut pointer) = event {
                    pointer.modifiers = modifiers;
                }
                engine.process_input(viewport, event).unwrap();
            }
            let filter = engine.model.filter(engine.model.paint_order()[0]).unwrap();
            geometry.push((filter.center, filter.width, filter.height, filter.rotation));
            if modifiers.shift {
                assert_eq!(filter.width, filter.height);
            }
            if modifiers.alt {
                assert_eq!(filter.center, Point::default());
            }
        }
        assert_eq!(geometry[0], geometry[1]);
    }
}

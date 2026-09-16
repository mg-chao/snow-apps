use super::*;
use snow_draw_engine_core::DrawRect;
use snow_draw_engine_document::{
    AutoFilterRegion, AutoFilterRegionRecord, CanvasFilterType, ElementKind, ElementMeta,
    FilterData, Transaction,
};
use snow_draw_engine_editor::FILTER_STYLE_PROPERTY_STRENGTH;

fn record(x: f64) -> AutoFilterRegionRecord {
    AutoFilterRegionRecord {
        source_bounds: DrawRect::new(x, 0.0, x + 200.0, 200.0),
        regions: vec![
            AutoFilterRegion {
                id: 1,
                bounds: DrawRect::new(x, 0.0, x + 100.0, 100.0),
                category: "image".into(),
            },
            AutoFilterRegion {
                id: 2,
                bounds: DrawRect::new(x + 20.0, 20.0, x + 40.0, 40.0),
                category: "text".into(),
            },
        ],
    }
}
fn engine() -> (Engine, ViewportId) {
    let mut engine = Engine::new(EngineConfig::default());
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::AutoFilter)
        .unwrap();
    (engine, viewport)
}
fn apply(engine: &mut Engine, viewport: ViewportId, transaction: Transaction) {
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
}

#[test]
fn smart_erase_order_strength_history_and_session() {
    use snow_draw_engine_document::PenFilterData;
    let (mut e, v) = engine();
    e.set_auto_filter_regions(v, Some(record(0.0))).unwrap();
    e.fill_auto_filter_category(v, "text").unwrap();
    let auto = e.model.auto_filter_fill(2).unwrap().0;
    let ordinary = e.model.peek_next_element_id();
    let mut t = Transaction::new("ordinary");
    t.insert_filter(ordinary, ElementMeta::default(), FilterData::default());
    apply(&mut e, v, t);
    let first = e.model.peek_next_element_id();
    let mut t = Transaction::new("smart rectangle");
    t.insert_filter(
        first,
        ElementMeta::default(),
        FilterData {
            filter_type: CanvasFilterType::SmartErase,
            strength: 0.99,
            rotation: 0.5,
            opacity: 0.6,
            ..FilterData::default()
        },
    );
    apply(&mut e, v, t);
    let second = e.model.peek_next_element_id();
    let mut t = Transaction::new("smart pen");
    t.insert_pen_filter(
        second,
        ElementMeta::default(),
        PenFilterData {
            filter_type: CanvasFilterType::SmartErase,
            strength: 0.1,
            ..PenFilterData::default()
        },
    );
    apply(&mut e, v, t);
    assert_eq!(e.model.paint_order(), &[first, second, auto, ordinary]);
    assert_eq!(e.model.filter(first).unwrap().strength, 0.5);
    assert_eq!(e.model.pen_filter(second).unwrap().strength, 0.5);
    let mut t = Transaction::new("attempt reverse fixed layer");
    t.reorder_elements(vec![second], 0);
    apply(&mut e, v, t);
    assert_eq!(e.model.paint_order(), &[first, second, auto, ordinary]);
    e.select_element_with_viewport_changes(v, first).unwrap();
    let before = e.serialize_document_history().unwrap();
    let mut style = e.editor.filter_style(&e.model);
    style.strength = 0.1;
    let command = e
        .editor
        .set_filter_style(&e.model, style, FILTER_STYLE_PROPERTY_STRENGTH)
        .unwrap();
    assert!(command.is_none());
    assert_eq!(before, e.serialize_document_history().unwrap());
    e.delete_selected_with_viewport_changes(v).unwrap();
    e.undo().unwrap();
    assert_eq!(e.model.paint_order(), &[first, second, auto, ordinary]);
    e.redo().unwrap();
    assert_eq!(e.model.paint_order(), &[second, auto, ordinary]);
    e.undo().unwrap();
    e.model
        .document()
        .validate_session()
        .expect("current document valid");
    e.history.validate_session(&e.model).expect("history valid");
    snow_draw_engine_editor::EditorSession::from_persisted(e.editor.persisted())
        .expect("editor valid");
    let serialized = e.serialize_document_session().unwrap();
    let mut restored =
        Engine::from_serialized_document_session_with_config(&serialized, EngineConfig::default())
            .unwrap();
    assert_eq!(
        restored.model.paint_order(),
        &[first, second, auto, ordinary]
    );
    let items = restored.smart_erase_items();
    assert_eq!(
        items.len(),
        2,
        "uncropped descriptors must not depend on viewport visibility"
    );
    assert_eq!(CanvasFilterType::SmartErase as u32, 5);
    assert_eq!(
        serde_json::to_value(CanvasFilterType::SmartErase).unwrap(),
        "SmartErase"
    );
    assert_eq!(restored.model.filter(first).unwrap().rotation, 0.5);
    assert_eq!(restored.model.filter(first).unwrap().opacity, 0.6);
}

#[test]
fn smart_erase_auto_fill_is_rejected_atomically() {
    let (mut e, v) = engine();
    e.set_auto_filter_regions(v, Some(record(0.0))).unwrap();
    let before = e.serialize_document_history().unwrap();
    let t = e
        .model
        .auto_filter_fill_transaction(&[1, 2], CanvasFilterType::SmartErase, 0.5, false);
    assert!(e.model.apply_transaction(t).is_err());
    assert_eq!(before, e.serialize_document_history().unwrap());
}

#[test]
fn smart_erase_conversion_undo_restores_previous_layer_order() {
    let (mut e, v) = engine();
    let first = e.model.peek_next_element_id();
    let mut t = Transaction::new("first ordinary");
    t.insert_filter(first, ElementMeta::default(), FilterData::default());
    apply(&mut e, v, t);
    let second = e.model.peek_next_element_id();
    let mut t = Transaction::new("second ordinary");
    t.insert_filter(second, ElementMeta::default(), FilterData::default());
    apply(&mut e, v, t);
    let mut t = Transaction::new("convert to smart");
    t.update_filter(
        second,
        FilterData {
            filter_type: CanvasFilterType::SmartErase,
            ..FilterData::default()
        },
    );
    apply(&mut e, v, t);
    assert_eq!(e.model.paint_order(), &[second, first]);
    e.undo().unwrap();
    assert_eq!(e.model.paint_order(), &[first, second]);
    e.redo().unwrap();
    assert_eq!(e.model.paint_order(), &[second, first]);
    e.history.validate_session(&e.model).unwrap();
}

#[test]
fn auto_filter_empty_identification_and_generation_are_history_aware() {
    let (mut e, v) = engine();
    let generation = e.auto_filter_generation();
    let mut empty = record(0.0);
    empty.regions.clear();
    e.set_auto_filter_regions(v, Some(empty)).unwrap();
    assert!(e.auto_filter_regions().unwrap().regions.is_empty());
    assert_ne!(generation, e.auto_filter_generation());
    e.undo().unwrap();
    assert!(e.auto_filter_regions().is_none());
    assert_ne!(generation, e.auto_filter_generation());
    assert!(!e.history_state().can_undo);
    e.redo().unwrap();
    assert!(e.auto_filter_regions().is_some());
}

#[test]
fn auto_filter_reset_undo_restores_previous_regions_and_fills() {
    let (mut e, v) = engine();
    e.set_auto_filter_regions(v, Some(record(0.0))).unwrap();
    e.fill_auto_filter_category(v, "text").unwrap();
    let original = *e.model.auto_filter_fill(2).unwrap().1;
    e.set_auto_filter_regions(v, None).unwrap();
    assert!(e.model.paint_order().is_empty());
    e.set_auto_filter_regions(v, Some(record(250.0))).unwrap();
    e.undo().unwrap();
    assert!(e.auto_filter_regions().is_none());
    e.undo().unwrap();
    assert_eq!(e.auto_filter_regions(), Some(&record(0.0)));
    assert_eq!(*e.model.auto_filter_fill(2).unwrap().1, original);
    e.redo().unwrap();
    assert!(e.model.auto_filter_fill(2).is_none());
}

#[test]
fn auto_filter_toggle_compares_type_and_preserves_element_style_on_undo() {
    let (mut e, v) = engine();
    e.set_auto_filter_regions(v, Some(record(0.0))).unwrap();
    let t = e
        .model
        .auto_filter_fill_transaction(&[1, 2], CanvasFilterType::Mosaic, 0.2, true);
    apply(&mut e, v, t);
    let t = e
        .model
        .auto_filter_fill_transaction(&[1, 2], CanvasFilterType::Mosaic, 0.9, true);
    apply(&mut e, v, t);
    assert!(e.model.paint_order().is_empty());
    e.undo().unwrap();
    assert_eq!(e.model.auto_filter_fill(2).unwrap().1.strength, 0.2);
    let t = e
        .model
        .auto_filter_fill_transaction(&[2], CanvasFilterType::Inversion, 0.9, true);
    apply(&mut e, v, t);
    assert_eq!(
        e.model.auto_filter_fill(2).unwrap().1.filter_type,
        CanvasFilterType::Inversion
    );
    e.undo().unwrap();
    assert_eq!(
        e.model.auto_filter_fill(2).unwrap().1.filter_type,
        CanvasFilterType::Mosaic
    );
}

#[test]
fn auto_filter_fills_are_unselectable_bottommost_and_survive_sessions() {
    let (mut e, v) = engine();
    e.set_auto_filter_regions(v, Some(record(0.0))).unwrap();
    let generation = e.auto_filter_generation();
    let id = e.model.peek_next_element_id();
    let mut t = Transaction::new("rectangle");
    t.insert_filter(id, ElementMeta::default(), FilterData::default());
    apply(&mut e, v, t);
    assert_eq!(generation, e.auto_filter_generation());
    e.fill_auto_filter_category(v, "text").unwrap();
    let fill = e.model.auto_filter_fill(2).unwrap().0;
    assert_eq!(e.model.paint_order()[0], fill);
    assert_eq!(
        e.model.element(fill).unwrap().data.kind(),
        ElementKind::AutoFilter
    );
    e.select_element_with_viewport_changes(v, fill).unwrap();
    assert!(e.selected_ids().is_empty());
    e.select_element_with_viewport_changes(v, id).unwrap();
    let mut t = Transaction::new("send back");
    t.reorder_elements(vec![id], 0);
    apply(&mut e, v, t);
    assert_eq!(e.model.paint_order()[0], fill);
    let session = e.serialize_document_session().unwrap();
    let restored =
        Engine::from_serialized_document_session_with_config(&session, EngineConfig::default())
            .unwrap();
    assert_eq!(e.model.document(), restored.model.document());
    assert!(restored.model.auto_filter_fill(2).is_some());
    assert_eq!(record(0.0).region_at(Point::new(25.0, 25.0)).unwrap().id, 2);
}

#[test]
fn auto_filter_legacy_sessions_and_history_default_to_unidentified() {
    let (mut e, v) = engine();
    let id = e.model.peek_next_element_id();
    let mut t = Transaction::new("legacy filter");
    t.insert_filter(id, ElementMeta::default(), FilterData::default());
    apply(&mut e, v, t);
    for version in [1, 2] {
        for history_only in [false, true] {
            let bytes = if history_only {
                e.serialize_document_history().unwrap()
            } else {
                e.serialize_document_session().unwrap()
            };
            let mut json: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
            json["schemaVersion"] = version.into();
            json["document"]
                .as_object_mut()
                .unwrap()
                .remove("auto_filter_regions");
            let bytes = serde_json::to_vec(&json).unwrap();
            let mut restored = if history_only {
                Engine::from_serialized_document_history_with_config(
                    &bytes,
                    EngineConfig::default(),
                )
            } else {
                Engine::from_serialized_document_session_with_config(
                    &bytes,
                    EngineConfig::default(),
                )
            }
            .unwrap();
            assert!(restored.auto_filter_regions().is_none());
            assert!(restored.model.filter(id).is_ok());
            restored.undo().unwrap();
            assert!(restored.model.paint_order().is_empty());
            restored.redo().unwrap();
            assert!(restored.model.filter(id).is_ok());
        }
    }
}

#[test]
fn auto_filter_rejects_invalid_records_without_history_or_generation_change() {
    let (mut e, v) = engine();
    let generation = e.auto_filter_generation();
    let mut invalid = record(0.0);
    invalid.regions[1].id = invalid.regions[0].id;
    assert!(e.set_auto_filter_regions(v, Some(invalid)).is_err());
    assert_eq!(generation, e.auto_filter_generation());
    assert!(!e.history_state().can_undo);
    assert!(e.auto_filter_regions().is_none());
}

#[test]
fn auto_filter_category_actions_have_independent_history_and_skip_noops() {
    let (mut e, v) = engine();
    e.set_auto_filter_regions(v, Some(record(0.0))).unwrap();
    e.fill_auto_filter_category(v, "text").unwrap();
    e.fill_auto_filter_category(v, "text").unwrap();
    e.fill_auto_filter_category(v, "missing").unwrap();
    e.undo().unwrap();
    assert!(e.model.auto_filter_fill(2).is_none());
    assert!(e.auto_filter_regions().is_some());
    e.redo().unwrap();
    for strength in [0.2, 0.8] {
        let mut style = e.viewport_style_toolbar_state(v).unwrap().filter_style;
        style.strength = strength;
        e.set_viewport_filter_style(v, style, FILTER_STYLE_PROPERTY_STRENGTH)
            .unwrap();
        e.fill_auto_filter_category(v, "text").unwrap();
    }
    e.undo().unwrap();
    assert_eq!(e.model.auto_filter_fill(2).unwrap().1.strength, 0.2);
    e.undo().unwrap();
    assert_eq!(e.model.auto_filter_fill(2).unwrap().1.strength, 0.5);
}

#[test]
fn auto_filter_shared_default_survives_selected_filter_style_undo() {
    let (mut e, v) = engine();
    let id = e.model.peek_next_element_id();
    let mut t = Transaction::new("older filter");
    t.insert_filter(
        id,
        ElementMeta::default(),
        FilterData {
            strength: 0.2,
            ..FilterData::default()
        },
    );
    apply(&mut e, v, t);
    e.set_viewport_active_tool(v, ActiveTool::Select).unwrap();
    e.select_element_with_viewport_changes(v, id).unwrap();
    assert_eq!(
        e.viewport_style_toolbar_state(v)
            .unwrap()
            .filter_style
            .strength,
        0.2
    );
    e.set_viewport_active_tool(v, ActiveTool::AutoFilter)
        .unwrap();
    assert_eq!(
        e.viewport_style_toolbar_state(v)
            .unwrap()
            .filter_style
            .strength,
        0.5
    );
    e.set_viewport_active_tool(v, ActiveTool::Select).unwrap();
    e.select_element_with_viewport_changes(v, id).unwrap();
    let mut style = e.viewport_style_toolbar_state(v).unwrap().filter_style;
    style.strength = 0.8;
    e.set_viewport_filter_style(v, style, FILTER_STYLE_PROPERTY_STRENGTH)
        .unwrap();
    assert_eq!(e.model.filter(id).unwrap().strength, 0.8);
    e.undo().unwrap();
    assert_eq!(e.model.filter(id).unwrap().strength, 0.2);
    e.set_viewport_active_tool(v, ActiveTool::AutoFilter)
        .unwrap();
    assert_eq!(
        e.viewport_style_toolbar_state(v)
            .unwrap()
            .filter_style
            .strength,
        0.8
    );
    e.set_viewport_active_tool(v, ActiveTool::PenFilter)
        .unwrap();
    assert_eq!(
        e.viewport_style_toolbar_state(v)
            .unwrap()
            .filter_style
            .strength,
        0.8
    );
}

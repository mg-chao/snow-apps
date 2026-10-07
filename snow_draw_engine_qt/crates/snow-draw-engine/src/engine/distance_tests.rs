use super::*;
use crate::{
    Arrowhead, DISTANCE_STYLE_PROPERTY_ALL, DISTANCE_STYLE_PROPERTY_DECIMAL_PLACES,
    DISTANCE_STYLE_PROPERTY_ENDPOINT_RATIO, DISTANCE_STYLE_PROPERTY_ENDPOINT_STYLE,
    DISTANCE_STYLE_PROPERTY_FACTOR, DISTANCE_STYLE_PROPERTY_STROKE,
    DISTANCE_STYLE_PROPERTY_STROKE_WIDTH, DISTANCE_STYLE_PROPERTY_UNIT, DistanceStyle,
    DistanceUnit, SceneDisplayItem, TextLayoutSize,
};
use serde_json::json;
use snow_draw_engine_core::ColorRgba8;
use snow_draw_engine_document::{
    ElementKind, TextHorizontalAlign, TextVerticalAlign, arrow_text_anchor,
};
use snow_draw_engine_editor::{
    ArrowHandleKind, DocumentSyncSnapshot, TextCommitTarget, TextDraftCommit,
};
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
        .set_viewport_active_tool(viewport, ActiveTool::Distance)
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

#[test]
fn distance_input_and_host_metrics_publish_one_incremental_patch() {
    let (mut engine, viewport) = setup();
    let second = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine.set_viewport_surface_size(second, 800, 600).unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        Point::new(0.0, 0.0),
    );
    for x in [100.0, 125.0, 150.0] {
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
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            Point::new(x, 0.0),
        );
        let request = engine
            .arrow_text_layout_requests(viewport)
            .unwrap()
            .remove(0);
        let result = engine
            .apply_arrow_text_measurements(
                viewport,
                &[(
                    request.text_id,
                    request.key,
                    TextLayoutSize::new(64.0, 24.0),
                    64.0,
                )],
            )
            .unwrap();
        assert!(result.changed_viewports.is_empty());
        for &(id, cursor) in &cursors {
            assert_eq!(
                engine.viewport_slot(id).unwrap().composer.current_cursor(),
                cursor
            );
        }
        assert_eq!(
            engine.end_presentation_update().unwrap().changed_viewports,
            vec![viewport, second]
        );
        for (id, cursor) in cursors {
            let patch = engine.acquire_patch(id, Some(cursor)).unwrap();
            assert!(
                !patch.scene.reset,
                "host measurement must not skip a published revision"
            );
            assert_eq!(patch.scene.base_revision, cursor.scene_revision.0);
            assert_eq!(patch.scene.revision, cursor.scene_revision.0 + 1);
            assert!(
                patch
                    .scene
                    .ops
                    .iter()
                    .flat_map(|op| &op.insert_items)
                    .any(|item| matches!(item, SceneDisplayItem::Text(text) if text.width == 64.0))
            );
        }
    }
    engine.begin_presentation_update().unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(150.0, 0.0),
    );
    engine.end_presentation_update().unwrap();
    let owner = engine.model.paint_order()[0];
    assert_eq!(label(&engine, owner).layout.width(), 64.0);
    engine.undo_with_viewport_changes().unwrap();
    engine.redo_with_viewport_changes().unwrap();
    assert_eq!(label(&engine, owner).layout.width(), 64.0);
}

fn pointer(
    engine: &mut Engine,
    viewport: ViewportId,
    event_type: PointerEventType,
    point: Point<f64>,
) {
    let view = engine.viewport_slot(viewport).unwrap().view;
    engine
        .process_input(
            viewport,
            InputEvent::Pointer(PointerEvent {
                pointer_id: 1,
                event_type,
                device: PointerDevice::Mouse,
                position: snow_draw_engine_core::canvas_to_view(point, &view.camera, view.surface),
                button: Some(PointerButton::Primary),
                buttons: PointerButtons(PointerButtons::PRIMARY),
                modifiers: Modifiers::default(),
            }),
        )
        .unwrap();
}

fn gesture(
    engine: &mut Engine,
    viewport: ViewportId,
    start: Point<f64>,
    end: Point<f64>,
    click: bool,
) -> ElementId {
    let before = engine.model.paint_order().len();
    pointer(engine, viewport, PointerEventType::Down, start);
    if click {
        pointer(engine, viewport, PointerEventType::Up, start);
    }
    pointer(engine, viewport, PointerEventType::Move, end);
    assert_eq!(engine.model.paint_order().len(), before);
    if click {
        pointer(engine, viewport, PointerEventType::Down, end);
    }
    pointer(engine, viewport, PointerEventType::Up, end);
    assert_eq!(engine.model.paint_order().len(), before + 2);
    engine.model.paint_order()[before]
}

fn label(engine: &Engine, owner: ElementId) -> &snow_draw_engine_document::TextData {
    engine
        .model
        .text(engine.model.bound_text_id_for_arrow(owner).unwrap())
        .unwrap()
}

fn scene(engine: &Engine, viewport: ViewportId) -> Vec<SceneDisplayItem> {
    let patch = engine.acquire_patch(viewport, None).unwrap();
    assert!(patch.scene.reset);
    patch
        .scene
        .ops
        .iter()
        .flat_map(|op| op.insert_items.iter().cloned())
        .collect()
}

fn apply(engine: &mut Engine, operations: serde_json::Value) -> Result<Vec<ElementId>, ErrorCode> {
    let (_, result) = engine.apply_annotation_json(
        &serde_json::to_vec(&json!({"version":1,"operations":operations})).unwrap(),
    )?;
    let result: serde_json::Value = serde_json::from_slice(&result).unwrap();
    Ok(serde_json::from_value(result["created_element_ids"].clone()).unwrap())
}

fn select(engine: &mut Engine, ids: &[ElementId]) {
    let snapshot: DocumentSyncSnapshot =
        serde_json::from_value(json!({"selectedIds":ids,"primaryId":ids.first()})).unwrap();
    engine
        .editor
        .restore_history_selection(&engine.model, &snapshot);
    engine.refresh_all_viewports().unwrap();
}

#[test]
fn distance_drag_and_two_click_creation_are_one_owned_undo_step() {
    for click in [false, true] {
        let (mut engine, viewport) = setup();
        let id = gesture(
            &mut engine,
            viewport,
            Point::new(-100.0, 0.0),
            Point::new(100.0, 0.0),
            click,
        );
        let arrow = engine.model.arrow(id).unwrap().clone();
        assert_eq!(arrow.element_kind(), ElementKind::Distance);
        assert_eq!(arrow.points.len(), 2);
        assert_eq!(arrow.start_arrowhead, Some(Arrowhead::Bar));
        assert_eq!(arrow.end_arrowhead, Some(Arrowhead::Bar));
        assert_eq!(arrow.distance.unwrap().unit, DistanceUnit::Cm);
        assert_eq!(label(&engine, id).text, "200 cm");
        assert_eq!(label(&engine, id).font_size, 20.0);
        assert_eq!(label(&engine, id).center, Point::new(0.0, 0.0));
        assert_eq!(
            label(&engine, id).horizontal_align,
            TextHorizontalAlign::Center
        );
        assert_eq!(label(&engine, id).vertical_align, TextVerticalAlign::Center);
        assert_eq!(label(&engine, id).rotation, 0.0);
        let text_id = arrow.text_element_id.unwrap();
        engine.undo_with_viewport_changes().unwrap();
        assert!(engine.model.paint_order().is_empty());
        assert!(!engine.history_state().can_undo);
        engine.redo_with_viewport_changes().unwrap();
        assert_eq!(engine.model.paint_order(), &[id, text_id]);
        assert_eq!(engine.model.arrow(id).unwrap(), &arrow);
    }
}

#[test]
fn distance_font_scaling_is_consistent_in_preview_edits_imports_and_restored_sessions() {
    for (stroke_width, expected_font_size) in [
        (1.0, 14.142_135_623_730_951),
        (2.0, 20.0),
        (10.0, 44.721_359_549_995_796),
    ] {
        let (mut engine, viewport) = setup();
        let style = DistanceStyle {
            stroke_width,
            ..Default::default()
        };
        engine
            .set_viewport_distance_style_patch(
                viewport,
                style,
                DISTANCE_STYLE_PROPERTY_STROKE_WIDTH,
            )
            .unwrap();
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Down,
            Point::new(0.0, 0.0),
        );
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            Point::new(100.0, 0.0),
        );
        let request = engine
            .arrow_text_layout_requests(viewport)
            .unwrap()
            .remove(0);
        assert!((request.text.font_size - expected_font_size).abs() < 1e-9);
        assert!(scene(&engine, viewport).iter().any(|item| {
            matches!(item, SceneDisplayItem::Text(text)
                if (text.font_size - expected_font_size).abs() < 1e-9)
        }));
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Up,
            Point::new(100.0, 0.0),
        );
        let id = engine.model.paint_order()[0];
        assert!((label(&engine, id).font_size - expected_font_size).abs() < 1e-9);
        engine
            .select_element_with_viewport_changes(viewport, id)
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
                    TextLayoutSize::new(84.0, 24.0),
                    84.0,
                )],
            )
            .unwrap();
        assert!(
            engine
                .arrow_text_layout_requests(viewport)
                .unwrap()
                .is_empty()
        );

        let (edited_width, edited_font_size) = if stroke_width == 10.0 {
            (1.0, 14.142_135_623_730_951)
        } else {
            (10.0, 44.721_359_549_995_796)
        };
        engine
            .set_viewport_distance_style_patch(
                viewport,
                DistanceStyle {
                    stroke_width: edited_width,
                    ..style
                },
                DISTANCE_STYLE_PROPERTY_STROKE_WIDTH,
            )
            .unwrap();
        assert!((label(&engine, id).font_size - edited_font_size).abs() < 1e-9);
        let updated = engine
            .arrow_text_layout_requests(viewport)
            .unwrap()
            .remove(0);
        assert_ne!(
            updated.key, request.key,
            "stroke edits invalidate measured text layout"
        );
        assert!((updated.text.font_size - edited_font_size).abs() < 1e-9);
        engine.undo_with_viewport_changes().unwrap();
        assert!((label(&engine, id).font_size - expected_font_size).abs() < 1e-9);
        engine.redo_with_viewport_changes().unwrap();
        assert!((label(&engine, id).font_size - edited_font_size).abs() < 1e-9);
        let restored = Engine::from_serialized_document_session_with_config(
            &engine.serialize_document_session().unwrap(),
            EngineConfig::default(),
        )
        .unwrap();
        assert_eq!(label(&restored, id), label(&engine, id));

        let ids = apply(
            &mut engine,
            json!([{"type":"distance","points":[[0,50],[100,50]],
                "style":{"stroke_width":stroke_width}}]),
        )
        .unwrap();
        assert!((label(&engine, ids[0]).font_size - expected_font_size).abs() < 1e-9);
    }
}

#[test]
fn distance_sessions_with_legacy_linear_fonts_restore_and_keep_history() {
    let (mut engine, viewport) = setup();
    let id = apply(
        &mut engine,
        json!([{"type":"distance","points":[[0,0],[100,0]],"style":{"stroke_width":4}}]),
    )
    .unwrap()[0];
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    engine
        .set_viewport_distance_style_patch(
            viewport,
            DistanceStyle {
                stroke_width: 10.0,
                ..Default::default()
            },
            DISTANCE_STYLE_PROPERTY_STROKE_WIDTH,
        )
        .unwrap();
    for redo_pending in [false, true] {
        if redo_pending {
            engine.undo_with_viewport_changes().unwrap();
        }
        for history_only in [false, true] {
            let bytes = if history_only {
                engine.serialize_document_history().unwrap()
            } else {
                engine.serialize_document_session().unwrap()
            };
            // Recreate the fonts stored by the previous linear sizing rule in both
            // the document and history operations, retaining the measured layouts.
            let legacy = String::from_utf8(bytes)
                .unwrap()
                .replace("\"font_size\":28.284271247461902", "\"font_size\":40.0")
                .replace("\"font_size\":44.721359549995796", "\"font_size\":100.0");
            let mut restored = if history_only {
                Engine::from_serialized_document_history_with_config(
                    legacy.as_bytes(),
                    EngineConfig::default(),
                )
            } else {
                Engine::from_serialized_document_session_with_config(
                    legacy.as_bytes(),
                    EngineConfig::default(),
                )
            }
            .unwrap();
            assert_eq!(label(&restored, id), label(&engine, id));
            if redo_pending {
                restored.redo_with_viewport_changes().unwrap();
                assert!((label(&restored, id).font_size - 44.721_359_549_995_796).abs() < 1e-9);
            }
            restored.undo_with_viewport_changes().unwrap();
            assert!((label(&restored, id).font_size - 28.284_271_247_461_902).abs() < 1e-9);
            restored.redo_with_viewport_changes().unwrap();
            assert!((label(&restored, id).font_size - 44.721_359_549_995_796).abs() < 1e-9);
        }
    }
}

#[test]
fn distance_annotation_json_defaults_to_cm_and_preserves_explicit_units() {
    let (mut engine, _) = setup();
    let ids = apply(
        &mut engine,
        json!([
            {"type":"distance","points":[[0,0],[10,0]]},
            {"type":"distance","points":[[0,50],[10,50]],"style":{}},
            {"type":"distance","points":[[0,100],[10,100]],"style":{"unit":"px"}},
            {"type":"distance","points":[[0,150],[10,150]],"style":{"unit":"mm"}}
        ]),
    )
    .unwrap();
    for (id, unit) in ids.iter().zip([
        DistanceUnit::Cm,
        DistanceUnit::Cm,
        DistanceUnit::Px,
        DistanceUnit::Mm,
    ]) {
        assert_eq!(
            engine.model.arrow(*id).unwrap().distance.unwrap().unit,
            unit
        );
        assert_eq!(label(&engine, *id).text, format!("10 {}", unit.suffix()));
    }
    assert_eq!(label(&engine, ids[3]).text, "10 mm");
    let restored = Engine::from_serialized_document_session_with_config(
        &engine.serialize_document_session().unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
    for id in ids {
        assert_eq!(
            restored.model.arrow(id).unwrap(),
            engine.model.arrow(id).unwrap()
        );
        assert_eq!(label(&restored, id).text, label(&engine, id).text);
    }
}

#[test]
fn distance_live_preview_measures_an_unwrapped_short_label_without_document_elements() {
    for click in [false, true] {
        let (mut engine, viewport) = setup();
        let start = Point::new(0.0, 0.0);
        let end = Point::new(10.0, 0.0);
        let style = DistanceStyle {
            unit: DistanceUnit::Cm,
            factor: 1000.0,
            decimal_places: 3,
            ..Default::default()
        };
        engine
            .set_viewport_distance_style_patch(viewport, style, DISTANCE_STYLE_PROPERTY_ALL)
            .unwrap();
        pointer(&mut engine, viewport, PointerEventType::Down, start);
        if click {
            pointer(&mut engine, viewport, PointerEventType::Up, start);
        }
        pointer(&mut engine, viewport, PointerEventType::Move, end);
        let requests = engine.arrow_text_layout_requests(viewport).unwrap();
        assert_eq!(requests.len(), 1);
        let request = &requests[0];
        assert_eq!(request.text.text, "10000.000 cm");
        assert_eq!(request.text.font_size, 20.0);
        assert_eq!(request.arrow_width, 10.0);
        assert_eq!(request.text.center, Point::new(5.0, 0.0));
        assert!(request.max_width.is_finite() && request.max_width >= 10000.0);
        assert!(engine.model.paint_order().is_empty());
        engine
            .apply_arrow_text_measurements(
                viewport,
                &[(
                    request.text_id,
                    request.key,
                    TextLayoutSize::new(200.0, 24.0),
                    200.0,
                )],
            )
            .unwrap();
        let items = scene(&engine, viewport);
        let rendered = items
            .iter()
            .find_map(|item| {
                if let SceneDisplayItem::Text(text) = item {
                    Some(text)
                } else {
                    None
                }
            })
            .unwrap();
        assert_eq!(rendered.text, "10000.000 cm");
        assert_eq!(rendered.width, 200.0);
        assert_eq!(rendered.rotation, 0.0);
        let arrow = items
            .iter()
            .find_map(|item| {
                if let SceneDisplayItem::Arrow(arrow) = item {
                    Some(arrow)
                } else {
                    None
                }
            })
            .unwrap();
        assert_eq!(arrow.bound_text_id, Some(rendered.id));
        assert!(arrow.label_bounds.is_some());
        if click {
            pointer(&mut engine, viewport, PointerEventType::Down, end);
        }
        pointer(&mut engine, viewport, PointerEventType::Up, end);
        let id = engine.model.paint_order()[0];
        assert_eq!(label(&engine, id).layout.width(), 200.0);
        assert_eq!(label(&engine, id).text, "10000.000 cm");
        assert_ne!(
            engine.model.bound_text_id_for_arrow(id),
            Some(request.text_id)
        );
        assert_eq!(engine.arrow_text_count(), 1);
        let committed_request = engine
            .arrow_text_layout_requests(viewport)
            .unwrap()
            .remove(0);
        assert_eq!(committed_request.arrow_width, 10.0);
    }
}

#[test]
fn distance_zero_finish_cancel_and_tool_change_discard_provisional_labels() {
    for mode in [0, 1, 2] {
        let (mut engine, viewport) = setup();
        let start = Point::new(0.0, 0.0);
        pointer(&mut engine, viewport, PointerEventType::Down, start);
        pointer(&mut engine, viewport, PointerEventType::Up, start);
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            Point::new(100.0, 0.0),
        );
        let old = engine
            .arrow_text_layout_requests(viewport)
            .unwrap()
            .remove(0);
        match mode {
            0 => {
                pointer(&mut engine, viewport, PointerEventType::Down, start);
                pointer(&mut engine, viewport, PointerEventType::Up, start);
            }
            1 => pointer(&mut engine, viewport, PointerEventType::Cancel, start),
            _ => {
                engine
                    .set_viewport_active_tool(viewport, ActiveTool::Arrow)
                    .unwrap();
            }
        }
        assert!(engine.model.paint_order().is_empty());
        assert!(!engine.history_state().can_undo);
        assert!(
            engine
                .arrow_text_layout_requests(viewport)
                .unwrap()
                .is_empty()
        );
        assert!(
            scene(&engine, viewport)
                .iter()
                .all(|item| !matches!(item, SceneDisplayItem::Text(_)))
        );
        engine
            .set_viewport_active_tool(viewport, ActiveTool::Distance)
            .unwrap();
        pointer(&mut engine, viewport, PointerEventType::Down, start);
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            Point::new(100.0, 0.0),
        );
        let new = engine
            .arrow_text_layout_requests(viewport)
            .unwrap()
            .remove(0);
        assert_ne!(old.text_id, new.text_id);
        assert!(
            !engine
                .editor
                .apply_arrow_text_measurement(
                    &engine.model,
                    old.text_id,
                    old.key,
                    TextLayoutSize::new(120.0, 24.0),
                    120.0
                )
                .unwrap()
        );
    }
}

#[test]
fn distance_pixel_metric_is_captured_at_start_and_independent_of_zoom() {
    let (mut engine, viewport) = setup();
    let style = DistanceStyle {
        factor: 2.0,
        unit: DistanceUnit::Cm,
        decimal_places: 3,
        ..Default::default()
    };
    engine
        .set_viewport_distance_style_patch(viewport, style, DISTANCE_STYLE_PROPERTY_ALL)
        .unwrap();
    engine
        .set_viewport_distance_pixel_scale(viewport, Point::new(2.0, 3.0))
        .unwrap();
    let start = Point::new(0.0, 0.0);
    let end = Point::new(3.0, 4.0);
    pointer(&mut engine, viewport, PointerEventType::Down, start);
    engine
        .set_viewport_distance_pixel_scale(viewport, Point::new(10.0, 10.0))
        .unwrap();
    pointer(&mut engine, viewport, PointerEventType::Move, end);
    pointer(&mut engine, viewport, PointerEventType::Up, end);
    let id = engine.model.paint_order()[0];
    let annotation = engine.model.arrow(id).unwrap().distance.unwrap();
    assert_eq!(
        (annotation.pixel_scale_x, annotation.pixel_scale_y),
        (2.0, 3.0)
    );
    assert_eq!(label(&engine, id).text, "26.833 cm");
    for zoom in [0.5, 2.0, 4.0] {
        engine
            .set_viewport_camera(
                viewport,
                Camera {
                    center: Point::new(1.5, 2.0),
                    zoom,
                },
            )
            .unwrap();
        assert_eq!(label(&engine, id).text, "26.833 cm");
        assert!(
            scene(&engine, viewport)
                .iter()
                .any(|item| matches!(item,SceneDisplayItem::Text(text) if text.text=="26.833 cm"))
        );
    }
    let style = DistanceStyle {
        factor: 3.0,
        ..style
    };
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    engine
        .set_viewport_distance_style_patch(viewport, style, DISTANCE_STYLE_PROPERTY_FACTOR)
        .unwrap();
    assert_eq!(label(&engine, id).text, "40.249 cm");
    assert_eq!(
        engine
            .model
            .arrow(id)
            .unwrap()
            .distance
            .unwrap()
            .pixel_scale_x,
        2.0
    );
}

#[test]
fn distance_masked_multi_selection_edits_preserve_other_fields_and_regular_arrows() {
    let (mut engine, viewport) = setup();
    let ids=apply(&mut engine,json!([
        {"type":"distance","points":[[-200,-50],[-100,-50]],"pixel_scale":[2,3],"style":{"factor":2,"unit":"cm"}},
        {"type":"distance","points":[[0,-50],[100,-50]],"pixel_scale":[4,5],"style":{"factor":5,"unit":"m","stroke_width":4}},
        {"type":"arrow","points":[[-200,100],[100,100]]}
    ])).unwrap();
    let regular = engine.model.arrow(ids[2]).unwrap().clone();
    select(&mut engine, &ids);
    let state = engine.viewport_style_toolbar_state(viewport).unwrap();
    assert_eq!(state.source, crate::StyleToolbarSource::SelectedDistance);
    assert_eq!(
        state.distance_style_mixed,
        DISTANCE_STYLE_PROPERTY_FACTOR
            | DISTANCE_STYLE_PROPERTY_UNIT
            | DISTANCE_STYLE_PROPERTY_STROKE_WIDTH
    );
    let color = ColorRgba8 {
        r: 10,
        g: 20,
        b: 30,
        a: 255,
    };
    let patch = DistanceStyle {
        stroke: color,
        factor: f64::NAN,
        ..Default::default()
    };
    engine
        .set_viewport_distance_style_patch(viewport, patch, DISTANCE_STYLE_PROPERTY_STROKE)
        .unwrap();
    for (id, expected, expected_font_size) in [
        (ids[0], (2.0, DistanceUnit::Cm, 2.0, 2.0, 3.0), 20.0),
        (
            ids[1],
            (5.0, DistanceUnit::M, 4.0, 4.0, 5.0),
            28.284_271_247_461_902,
        ),
    ] {
        let arrow = engine.model.arrow(id).unwrap();
        let distance = arrow.distance.unwrap();
        assert_eq!(
            (
                distance.factor,
                distance.unit,
                arrow.stroke_width,
                distance.pixel_scale_x,
                distance.pixel_scale_y
            ),
            expected
        );
        assert_eq!(arrow.stroke, color);
        assert_eq!(label(&engine, id).color, color);
        assert!((label(&engine, id).font_size - expected_font_size).abs() < 1e-9);
    }
    assert_eq!(engine.model.arrow(ids[2]).unwrap(), &regular);
    let patch = DistanceStyle {
        stroke_width: 8.0,
        factor: 10.0,
        unit: DistanceUnit::Km,
        decimal_places: 2,
        ..Default::default()
    };
    let mask = DISTANCE_STYLE_PROPERTY_STROKE_WIDTH
        | DISTANCE_STYLE_PROPERTY_FACTOR
        | DISTANCE_STYLE_PROPERTY_UNIT
        | DISTANCE_STYLE_PROPERTY_DECIMAL_PLACES;
    engine
        .set_viewport_distance_style_patch(viewport, patch, mask)
        .unwrap();
    assert_eq!(label(&engine, ids[0]).text, "2000.00 km");
    assert_eq!(label(&engine, ids[1]).text, "4000.00 km");
    assert_eq!(label(&engine, ids[0]).font_size, 40.0);
    assert_eq!(label(&engine, ids[1]).color, color);
    assert_eq!(engine.model.arrow(ids[2]).unwrap(), &regular);
    engine.undo_with_viewport_changes().unwrap();
    assert_eq!(label(&engine, ids[0]).text, "400 cm");
    assert!((label(&engine, ids[1]).font_size - 28.284_271_247_461_902).abs() < 1e-9);
}

#[test]
fn distance_endpoints_share_all_existing_arrowhead_styles() {
    let (mut engine, viewport) = setup();
    let id = gesture(
        &mut engine,
        viewport,
        Point::new(-100.0, 0.0),
        Point::new(100.0, 0.0),
        false,
    );
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    for style in [
        None,
        Some(Arrowhead::Arrow),
        Some(Arrowhead::Bar),
        Some(Arrowhead::Dot),
        Some(Arrowhead::Circle),
        Some(Arrowhead::CircleOutline),
        Some(Arrowhead::Triangle),
        Some(Arrowhead::TriangleOutline),
        Some(Arrowhead::Diamond),
        Some(Arrowhead::DiamondOutline),
        Some(Arrowhead::CrowfootOne),
        Some(Arrowhead::CrowfootMany),
        Some(Arrowhead::CrowfootOneOrMany),
        Some(Arrowhead::Square),
        Some(Arrowhead::InvertedTriangle),
        Some(Arrowhead::IndentedTriangle),
    ] {
        engine
            .set_viewport_distance_style_patch(
                viewport,
                DistanceStyle {
                    endpoint_style: style,
                    ..Default::default()
                },
                DISTANCE_STYLE_PROPERTY_ENDPOINT_STYLE,
            )
            .unwrap();
        let arrow = engine.model.arrow(id).unwrap();
        assert_eq!(arrow.start_arrowhead, style);
        assert_eq!(arrow.end_arrowhead, style);
        assert_eq!(arrow.points.len(), 2);
        assert_eq!(label(&engine, id).text, "200 cm");
        assert!(scene(&engine,viewport).iter().any(|item| matches!(item,SceneDisplayItem::Arrow(arrow) if arrow.start_arrowhead==style && arrow.end_arrowhead==style)));
    }
}

#[test]
fn distance_label_moves_the_owner_and_endpoint_edit_updates_derived_label() {
    let (mut engine, viewport) = setup();
    let id = gesture(
        &mut engine,
        viewport,
        Point::new(-100.0, 0.0),
        Point::new(100.0, 0.0),
        false,
    );
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    let view = engine.viewport_slot(viewport).unwrap().view;
    let presentation = engine.editor.presentation_state(&engine.model, &view);
    assert_eq!(presentation.arrow_handles.len(), 2);
    assert!(
        presentation
            .arrow_handles
            .iter()
            .all(|handle| handle.kind == ArrowHandleKind::Endpoint)
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        Point::new(0.0, 0.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(30.0, 40.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(30.0, 40.0),
    );
    let arrow = engine.model.arrow(id).unwrap();
    assert_eq!(arrow.start(), Point::new(-70.0, 40.0));
    assert_eq!(arrow.end(), Point::new(130.0, 40.0));
    assert_eq!(label(&engine, id).center, Point::new(30.0, 40.0));
    assert_eq!(label(&engine, id).text, "200 cm");
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        Point::new(130.0, 40.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(230.0, 40.0),
    );
    let request = engine
        .arrow_text_layout_requests(viewport)
        .unwrap()
        .remove(0);
    assert_eq!(request.text.text, "300 cm");
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(230.0, 40.0),
    );
    assert_eq!(label(&engine, id).text, "300 cm");
    assert_eq!(
        label(&engine, id).center,
        arrow_text_anchor(engine.model.arrow(id).unwrap())
    );
    assert!(engine.model.arrow(id).unwrap().start_binding.is_none());
    assert!(engine.model.arrow(id).unwrap().end_binding.is_none());
    assert!(engine.arrow_text_target(viewport, None).unwrap().is_none());
    let text_id = engine.model.bound_text_id_for_arrow(id).unwrap();
    let commit = TextDraftCommit::new(
        TextCommitTarget::Existing(text_id),
        Point::default(),
        "edited",
        TextLayoutSize::new(60.0, 20.0),
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .text_style,
        true,
        false,
    );
    assert_eq!(
        engine
            .commit_text_draft_with_viewport_changes(viewport, commit)
            .unwrap_err(),
        ErrorCode::InvalidState
    );
}

#[test]
fn distance_owned_pair_survives_duplicate_template_session_and_delete_history() {
    let (mut engine, viewport) = setup();
    engine
        .set_viewport_distance_pixel_scale(viewport, Point::new(2.0, 3.0))
        .unwrap();
    let style = DistanceStyle {
        factor: 0.1,
        unit: DistanceUnit::M,
        decimal_places: 1,
        ..Default::default()
    };
    engine
        .set_viewport_distance_style_patch(viewport, style, DISTANCE_STYLE_PROPERTY_ALL)
        .unwrap();
    let id = gesture(
        &mut engine,
        viewport,
        Point::new(-100.0, 0.0),
        Point::new(100.0, 0.0),
        false,
    );
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    let template = engine.serialize_selected_draw_template().unwrap();
    let annotation = engine.model.arrow(id).unwrap().distance;
    engine
        .duplicate_selected_with_viewport_changes(viewport, Point::new(0.0, 100.0))
        .unwrap();
    let duplicate = engine.selected_ids()[0];
    assert_ne!(duplicate, id);
    assert_ne!(
        engine.model.bound_text_id_for_arrow(duplicate),
        engine.model.bound_text_id_for_arrow(id)
    );
    assert_eq!(engine.model.arrow(duplicate).unwrap().distance, annotation);
    assert_eq!(label(&engine, duplicate).text, "40.0 m");
    engine
        .insert_draw_template_with_viewport_changes(viewport, &template, Point::new(100.0, -100.0))
        .unwrap();
    let inserted = engine.selected_ids()[0];
    assert_ne!(inserted, duplicate);
    assert_eq!(engine.model.arrow(inserted).unwrap().distance, annotation);
    assert_eq!(label(&engine, inserted).text, "40.0 m");
    assert_eq!(engine.model.paint_order().len(), 6);
    let bytes = engine.serialize_document_session().unwrap();
    let mut restored =
        Engine::from_serialized_document_session_with_config(&bytes, EngineConfig::default())
            .unwrap();
    assert_eq!(restored.model.document(), engine.model.document());
    let view = restored.create_viewport(ViewportConfig::default()).unwrap();
    assert_eq!(
        restored
            .viewport_style_toolbar_state(view)
            .unwrap()
            .distance_style,
        style
    );
    restored
        .select_element_with_viewport_changes(view, inserted)
        .unwrap();
    restored
        .delete_selected_with_viewport_changes(view)
        .unwrap();
    assert_eq!(restored.model.paint_order().len(), 4);
    assert!(restored.model.arrow(inserted).is_err());
    restored.undo_with_viewport_changes().unwrap();
    assert_eq!(restored.model.paint_order().len(), 6);
    assert_eq!(label(&restored, inserted).text, "40.0 m");
    restored.redo_with_viewport_changes().unwrap();
    assert_eq!(restored.model.paint_order().len(), 4);
}

#[test]
fn distance_invalid_annotations_and_masked_style_patches_are_atomic() {
    let (mut engine, viewport) = setup();
    let invalid = [
        json!({"type":"distance","points":[[0,0],[0,0]]}),
        json!({"type":"distance","points":[[0,0],[10,0],[20,0]]}),
        json!({"type":"distance","points":[[0,0],[10,0]],"style":{"factor":0}}),
        json!({"type":"distance","points":[[0,0],[10,0]],"style":{"factor":1001}}),
        json!({"type":"distance","points":[[0,0],[10,0]],"style":{"decimal_places":4}}),
        json!({"type":"distance","points":[[0,0],[10,0]],"style":{"unit":"in"}}),
        json!({"type":"distance","points":[[0,0],[10,0]],"style":{"endpoint_style":"invalid"}}),
        json!({"type":"distance","points":[[0,0],[10,0]],"style":{"endpoint_scale":0.4}}),
        json!({"type":"distance","points":[[0,0],[10,0]],"pixel_scale":[0,1]}),
        json!({"type":"distance","points":[[0,0],[10,0]],"pixel_scale":[1.7976931348623157e308,1]}),
    ];
    let before = engine.serialize_document_session().unwrap();
    for operation in invalid {
        assert!(
            apply(
                &mut engine,
                json!([{"type":"rectangle","bounds":[0,0,10,10]},operation])
            )
            .is_err()
        );
        assert_eq!(engine.serialize_document_session().unwrap(), before);
    }
    for (patch, mask) in [
        (
            DistanceStyle {
                stroke_width: 0.5,
                ..Default::default()
            },
            DISTANCE_STYLE_PROPERTY_STROKE_WIDTH,
        ),
        (
            DistanceStyle {
                factor: f64::INFINITY,
                ..Default::default()
            },
            DISTANCE_STYLE_PROPERTY_FACTOR,
        ),
        (
            DistanceStyle {
                endpoint_ratio: 0.4,
                ..Default::default()
            },
            DISTANCE_STYLE_PROPERTY_ENDPOINT_RATIO,
        ),
        (DistanceStyle::default(), 128),
    ] {
        assert_eq!(
            engine
                .set_viewport_distance_style_patch(viewport, patch, mask)
                .unwrap_err(),
            ErrorCode::InvalidArgument
        );
        assert_eq!(engine.serialize_document_session().unwrap(), before);
    }
    for scale in [
        Point::new(0.0, 1.0),
        Point::new(f64::INFINITY, 1.0),
        Point::new(1.0, -1.0),
    ] {
        assert_eq!(
            engine
                .set_viewport_distance_pixel_scale(viewport, scale)
                .unwrap_err(),
            ErrorCode::InvalidArgument
        );
    }
}

#[test]
fn distance_units_are_suffixes_and_decimals_have_fixed_precision() {
    let (mut engine, viewport) = setup();
    let id = gesture(
        &mut engine,
        viewport,
        Point::new(0.0, 0.0),
        Point::new(10.0, 0.0),
        false,
    );
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    for unit in [
        DistanceUnit::Mm,
        DistanceUnit::Cm,
        DistanceUnit::M,
        DistanceUnit::Km,
        DistanceUnit::Px,
    ] {
        for decimals in 0..=3 {
            let style = DistanceStyle {
                unit,
                decimal_places: decimals,
                ..Default::default()
            };
            engine
                .set_viewport_distance_style_patch(
                    viewport,
                    style,
                    DISTANCE_STYLE_PROPERTY_UNIT | DISTANCE_STYLE_PROPERTY_DECIMAL_PLACES,
                )
                .unwrap();
            assert_eq!(
                label(&engine, id).text,
                format!("{:.*} {}", usize::from(decimals), 10.0, unit.suffix())
            );
        }
    }
}

#[test]
fn distance_legacy_sessions_keep_regular_arrows_and_default_missing_metrics() {
    let (mut engine, _) = setup();
    let id = apply(
        &mut engine,
        json!([{"type":"arrow","points":[[0,0],[10,0]]}]),
    )
    .unwrap()[0];
    let mut session: serde_json::Value =
        serde_json::from_slice(&engine.serialize_document_session().unwrap()).unwrap();
    session["editor"]
        .as_object_mut()
        .unwrap()
        .remove("distance");
    let restored = Engine::from_serialized_document_session_with_config(
        &serde_json::to_vec(&session).unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
    assert!(restored.model.arrow(id).unwrap().is_regular_arrow());
    assert!(restored.model.arrow(id).unwrap().distance.is_none());
    let annotation: snow_draw_engine_document::DistanceAnnotation =
        serde_json::from_value(json!({"factor":1,"unit":"px","decimal_places":0})).unwrap();
    assert_eq!(
        (annotation.pixel_scale_x, annotation.pixel_scale_y),
        (1.0, 1.0)
    );
}

#[test]
fn distance_common_opacity_reports_mixed_and_updates_owned_labels_with_undo() {
    let (mut engine, viewport) = setup();
    let ids = apply(
        &mut engine,
        json!([
            {"type":"distance","points":[[-200,0],[-100,0]]},
            {"type":"distance","points":[[100,0],[200,0]],"style":{"stroke_width":4}}
        ]),
    )
    .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, ids[0])
        .unwrap();
    engine
        .set_selected_opacity_with_viewport_changes(viewport, 0.25)
        .unwrap();
    let state = engine.viewport_style_toolbar_state(viewport).unwrap();
    assert_eq!(state.source, crate::StyleToolbarSource::SelectedDistance);
    assert_eq!(state.shape_style.opacity, 0.25);
    assert_eq!(state.shape_style_mixed, 0);
    assert_eq!(label(&engine, ids[0]).opacity, 0.25);
    select(&mut engine, &ids);
    let state = engine.viewport_style_toolbar_state(viewport).unwrap();
    assert_eq!(state.shape_style.opacity, 0.25);
    assert_eq!(
        state.shape_style_mixed,
        snow_draw_engine_editor::SHAPE_STYLE_MIXED_OPACITY
    );
    assert_eq!(
        state.distance_style_mixed,
        DISTANCE_STYLE_PROPERTY_STROKE_WIDTH
    );
    engine
        .set_selected_opacity_with_viewport_changes(viewport, 0.5)
        .unwrap();
    for id in &ids {
        assert_eq!(engine.model.arrow(*id).unwrap().opacity, 0.5);
        assert_eq!(label(&engine, *id).opacity, 0.5);
    }
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .shape_style_mixed,
        0
    );
    engine.undo_with_viewport_changes().unwrap();
    assert_eq!(label(&engine, ids[0]).opacity, 0.25);
    assert_eq!(label(&engine, ids[1]).opacity, 1.0);
    engine.redo_with_viewport_changes().unwrap();
    assert_eq!(label(&engine, ids[0]).opacity, 0.5);
    assert_eq!(label(&engine, ids[1]).opacity, 0.5);
}

#[test]
fn distance_final_pointer_position_invalidates_preview_metrics_before_commit() {
    let (mut engine, viewport) = setup();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        Point::new(0.0, 0.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(10.0, 0.0),
    );
    let old = engine
        .arrow_text_layout_requests(viewport)
        .unwrap()
        .remove(0);
    engine
        .apply_arrow_text_measurements(
            viewport,
            &[(old.text_id, old.key, TextLayoutSize::new(40.0, 24.0), 40.0)],
        )
        .unwrap();
    // Hosts deliver the final position as a preview move and measure it before release.
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(100.0, 0.0),
    );
    let final_request = engine
        .arrow_text_layout_requests(viewport)
        .unwrap()
        .remove(0);
    assert_eq!(final_request.text.text, "100 cm");
    assert_eq!(final_request.arrow_width, 100.0);
    assert_eq!(old.text_id, final_request.text_id);
    assert_ne!(old.key, final_request.key);
    assert!(
        !engine
            .editor
            .apply_arrow_text_measurement(
                &engine.model,
                old.text_id,
                old.key,
                TextLayoutSize::new(40.0, 24.0),
                40.0
            )
            .unwrap()
    );
    engine
        .apply_arrow_text_measurements(
            viewport,
            &[(
                final_request.text_id,
                final_request.key,
                TextLayoutSize::new(64.0, 24.0),
                64.0,
            )],
        )
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(100.0, 0.0),
    );
    let id = engine.model.paint_order()[0];
    assert_eq!(label(&engine, id).text, "100 cm");
    assert_eq!(label(&engine, id).layout.width(), 64.0);
    engine.undo_with_viewport_changes().unwrap();
    engine.redo_with_viewport_changes().unwrap();
    assert_eq!(label(&engine, id).layout.width(), 64.0);
}

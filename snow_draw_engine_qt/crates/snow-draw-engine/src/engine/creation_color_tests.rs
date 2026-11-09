use super::*;
use snow_draw_engine_core::ColorRgba8;
use snow_draw_engine_editor::{
    ElementCreationPreview, SHAPE_STYLE_PROPERTY_FILL, SHAPE_STYLE_PROPERTY_STROKE,
    SHAPE_STYLE_PROPERTY_STROKE_WIDTH, ShapeKind,
};
use snow_draw_engine_interaction::{
    KeyCode, KeyEvent, KeyEventType, PointerButton, PointerButtons, PointerDevice, PointerEvent,
    PointerEventType,
};

const ORIGINAL: ColorRgba8 = ColorRgba8 {
    r: 23,
    g: 76,
    b: 129,
    a: 117,
};
const UPDATED: ColorRgba8 = ColorRgba8 {
    r: 65,
    g: 43,
    b: 21,
    a: 0,
};
const LATEST: ColorRgba8 = ColorRgba8 {
    r: 9,
    g: 71,
    b: 37,
    a: 200,
};

fn setup(tool: ActiveTool) -> (Engine, ViewportId) {
    let mut engine = Engine::default();
    let view = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine.set_viewport_surface_size(view, 800, 600).unwrap();
    engine
        .set_viewport_snap_config(
            view,
            SnapConfig {
                enabled: false,
                ..Default::default()
            },
        )
        .unwrap();
    engine.set_viewport_active_tool(view, tool).unwrap();
    engine.set_creation_color(Some(ORIGINAL)).unwrap();
    (engine, view)
}

fn pointer(engine: &mut Engine, view: ViewportId, kind: PointerEventType, x: f64, y: f64) {
    engine
        .process_input(
            view,
            InputEvent::Pointer(PointerEvent {
                pointer_id: 1,
                event_type: kind,
                device: PointerDevice::Mouse,
                position: Point::new(x + 400.0, y + 300.0),
                button: matches!(kind, PointerEventType::Down | PointerEventType::Up)
                    .then_some(PointerButton::Primary),
                buttons: if matches!(kind, PointerEventType::Down | PointerEventType::Move) {
                    PointerButtons(PointerButtons::PRIMARY)
                } else {
                    PointerButtons::default()
                },
                modifiers: Default::default(),
                pressure: None,
                timestamp_ms: 0,
            }),
        )
        .unwrap();
}

fn click(engine: &mut Engine, view: ViewportId, x: f64, y: f64) {
    pointer(engine, view, PointerEventType::Down, x, y);
    pointer(engine, view, PointerEventType::Up, x, y);
}

fn preview(engine: &Engine, view: ViewportId) -> ElementCreationPreview {
    engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(view).unwrap().view)
        .creation_preview
        .expect("gesture has a preview")
}

fn assert_preview_colors(engine: &Engine, view: ViewportId, stroke: ColorRgba8, fill: ColorRgba8) {
    match preview(engine, view) {
        ElementCreationPreview::Rectangle(rect) => {
            assert_eq!((rect.stroke, rect.fill), (stroke, fill))
        }
        ElementCreationPreview::SpeechBubble(bubble) => {
            assert_eq!((bubble.body.stroke, bubble.body.fill), (stroke, fill));
        }
        ElementCreationPreview::Magnifier(value) => assert_eq!(value.stroke, stroke),
        ElementCreationPreview::Arrow(arrow) => {
            assert_eq!(arrow.stroke, stroke);
            if arrow.is_line() {
                assert_eq!(arrow.fill, fill);
            }
        }
        ElementCreationPreview::FreeDraw(value) => assert_eq!(value.stroke, stroke),
        other => panic!("unexpected preview: {other:?}"),
    }
}

fn assert_committed_colors(engine: &Engine, id: ElementId, stroke: ColorRgba8, fill: ColorRgba8) {
    if let Ok(rect) = engine.model.rectangle(id) {
        assert_eq!((rect.stroke, rect.fill), (stroke, fill));
    } else if let Ok(bubble) = engine.model.speech_bubble(id) {
        assert_eq!((bubble.body.stroke, bubble.body.fill), (stroke, fill));
    } else if let Ok(magnifier) = engine.model.magnifier(id) {
        assert_eq!(magnifier.stroke, stroke);
    } else if let Ok(arrow) = engine.model.arrow(id) {
        assert_eq!(arrow.stroke, stroke);
        if arrow.is_line() {
            assert_eq!(arrow.fill, fill);
        }
    } else if let Ok(free_draw) = engine.model.free_draw(id) {
        assert_eq!(free_draw.stroke, stroke);
    } else {
        panic!("unexpected element");
    }
}

#[test]
fn shared_creation_color_preserves_active_gesture_previews_and_commits() {
    for tool in [
        ActiveTool::Shape,
        ActiveTool::SpeechBubble,
        ActiveTool::RectangleHighlight,
        ActiveTool::Arrow,
        ActiveTool::Line,
        ActiveTool::PenHighlight,
        ActiveTool::Distance,
        ActiveTool::Bracket,
        ActiveTool::Magnifier,
        ActiveTool::FreeDraw,
    ] {
        for update_before_preview in [false, true] {
            let (mut engine, view) = setup(tool);
            let peer = engine.create_viewport(ViewportConfig::default()).unwrap();
            pointer(&mut engine, view, PointerEventType::Down, -100.0, -80.0);
            if !update_before_preview {
                pointer(&mut engine, view, PointerEventType::Move, 0.0, 0.0);
            }
            let document = engine.document_checkpoint().unwrap();
            let history = engine.serialize_document_history().unwrap();
            let selection = engine.selected_ids();
            engine.set_creation_color(Some(UPDATED)).unwrap();
            engine.set_creation_color(Some(LATEST)).unwrap();
            assert_eq!(engine.document_checkpoint().unwrap(), document, "{tool:?}");
            assert_eq!(
                engine.serialize_document_history().unwrap(),
                history,
                "{tool:?}"
            );
            assert_eq!(engine.selected_ids(), selection);
            // Persisted preferences already contain the latest color while the draft stays local.
            let mut restored = Engine::from_serialized_document_session_with_config(
                &engine.serialize_document_session().unwrap(),
                EngineConfig::default(),
            )
            .unwrap();
            let restored_view = restored.create_viewport(ViewportConfig::default()).unwrap();
            restored
                .set_viewport_active_tool(restored_view, ActiveTool::Shape)
                .unwrap();
            assert_eq!(
                restored
                    .viewport_style_toolbar_state(restored_view)
                    .unwrap()
                    .shape_style
                    .stroke,
                LATEST
            );
            pointer(&mut engine, view, PointerEventType::Move, 100.0, 80.0);
            assert_preview_colors(&engine, view, ORIGINAL, ORIGINAL);
            assert_preview_colors(&engine, peer, ORIGINAL, ORIGINAL);
            pointer(&mut engine, view, PointerEventType::Up, 100.0, 80.0);
            let id = engine.model.paint_order()[0];
            assert_committed_colors(&engine, id, ORIGINAL, ORIGINAL);
            let checkpoint = engine.document_checkpoint().unwrap();
            engine.undo().unwrap();
            assert!(engine.model.paint_order().is_empty());
            engine.redo().unwrap();
            assert_eq!(engine.document_checkpoint().unwrap(), checkpoint);
            engine
                .set_viewport_active_tool(view, ActiveTool::Shape)
                .unwrap();
            pointer(&mut engine, view, PointerEventType::Down, -160.0, 110.0);
            pointer(&mut engine, view, PointerEventType::Up, -120.0, 150.0);
            assert_committed_colors(
                &engine,
                *engine.model.paint_order().last().unwrap(),
                LATEST,
                LATEST,
            );
        }
    }
}

#[test]
fn shared_creation_color_preserves_multiclick_angle_and_arrow() {
    for tool in [ActiveTool::Angle, ActiveTool::Arrow, ActiveTool::Line] {
        let (mut engine, view) = setup(tool);
        click(&mut engine, view, 100.0, 0.0);
        assert!(engine.has_active_interaction());
        assert!(!engine.has_pointer_capture());
        engine.set_creation_color(Some(UPDATED)).unwrap();
        click(&mut engine, view, 0.0, 0.0);
        engine.set_creation_color(Some(LATEST)).unwrap();
        pointer(&mut engine, view, PointerEventType::Move, 0.0, -100.0);
        assert_preview_colors(&engine, view, ORIGINAL, ORIGINAL);
        click(&mut engine, view, 0.0, -100.0);
        if tool != ActiveTool::Angle {
            click(&mut engine, view, 0.0, -100.0);
        }
        assert!(!engine.has_active_interaction());
        assert_committed_colors(&engine, engine.model.paint_order()[0], ORIGINAL, ORIGINAL);
    }
}

#[test]
fn shared_creation_color_preserves_independent_stroke_and_fill_when_enabled_midgesture() {
    let (mut engine, view) = setup(ActiveTool::Shape);
    engine.set_creation_color(None).unwrap();
    let mut style = engine
        .viewport_style_toolbar_state(view)
        .unwrap()
        .shape_style;
    style.fill = UPDATED;
    engine
        .set_viewport_shape_style_patch(
            view,
            ShapeStylePatch {
                kind: ShapeKind::Rectangle,
                style,
                properties: SHAPE_STYLE_PROPERTY_FILL,
            },
        )
        .unwrap();
    pointer(&mut engine, view, PointerEventType::Down, -100.0, -80.0);
    engine.set_creation_color(Some(LATEST)).unwrap();
    engine.set_creation_color(None).unwrap();
    pointer(&mut engine, view, PointerEventType::Up, 100.0, 80.0);
    assert_committed_colors(&engine, engine.model.paint_order()[0], ORIGINAL, UPDATED);
    pointer(&mut engine, view, PointerEventType::Down, -160.0, 110.0);
    pointer(&mut engine, view, PointerEventType::Up, -120.0, 150.0);
    assert_committed_colors(
        &engine,
        *engine.model.paint_order().last().unwrap(),
        LATEST,
        LATEST,
    );
}

#[test]
fn shared_creation_color_preserves_drafts_during_explicit_style_edits() {
    for (tool, kind) in [
        (ActiveTool::Shape, ShapeKind::Rectangle),
        (ActiveTool::SpeechBubble, ShapeKind::SpeechBubble),
        (ActiveTool::Line, ShapeKind::Line),
        (ActiveTool::Arrow, ShapeKind::Arrow),
    ] {
        let (mut engine, view) = setup(tool);
        pointer(&mut engine, view, PointerEventType::Down, -100.0, -80.0);
        pointer(&mut engine, view, PointerEventType::Move, 0.0, 0.0);
        engine.set_creation_color(Some(UPDATED)).unwrap();
        let mut style = engine
            .viewport_style_toolbar_state(view)
            .unwrap()
            .shape_style;
        style.stroke_width = 7.0;
        engine
            .set_viewport_shape_style_patch(
                view,
                ShapeStylePatch {
                    kind,
                    style,
                    properties: SHAPE_STYLE_PROPERTY_STROKE_WIDTH,
                },
            )
            .unwrap();
        assert_preview_colors(&engine, view, ORIGINAL, ORIGINAL);
        style.stroke = UPDATED; // Already the future default: still an explicit draft edit.
        engine
            .set_viewport_shape_style_patch(
                view,
                ShapeStylePatch {
                    kind,
                    style,
                    properties: SHAPE_STYLE_PROPERTY_STROKE,
                },
            )
            .unwrap();
        assert_preview_colors(&engine, view, UPDATED, ORIGINAL);
        pointer(&mut engine, view, PointerEventType::Up, 100.0, 80.0);
        assert_committed_colors(&engine, engine.model.paint_order()[0], UPDATED, ORIGINAL);
    }
}

#[test]
fn shared_creation_color_snapshot_is_discarded_on_cancel_and_tool_change() {
    for finish in [0, 1, 2, 3] {
        let (mut engine, view) = setup(ActiveTool::Shape);
        pointer(&mut engine, view, PointerEventType::Down, -100.0, -80.0);
        engine.set_creation_color(Some(UPDATED)).unwrap();
        match finish {
            0 => pointer(&mut engine, view, PointerEventType::Cancel, 0.0, 0.0),
            1 => {
                engine
                    .process_input(
                        view,
                        InputEvent::Key(KeyEvent {
                            event_type: KeyEventType::KeyDown,
                            key_code: KeyCode::Escape,
                            modifiers: Default::default(),
                            repeat: false,
                        }),
                    )
                    .unwrap();
            }
            2 => {
                engine
                    .set_viewport_active_tool(view, ActiveTool::Line)
                    .unwrap();
            }
            _ => {
                engine.clear_document_preserving_viewports().unwrap();
            }
        }
        engine.set_creation_color(None).unwrap();
        engine
            .set_viewport_active_tool(view, ActiveTool::Shape)
            .unwrap();
        pointer(&mut engine, view, PointerEventType::Down, -160.0, 110.0);
        pointer(&mut engine, view, PointerEventType::Up, -120.0, 150.0);
        assert_committed_colors(&engine, engine.model.paint_order()[0], UPDATED, UPDATED);
    }
}

#[test]
fn shared_creation_color_preserves_specialized_previews_during_style_edits() {
    use snow_draw_engine_editor::*;
    for tool in [
        ActiveTool::Distance,
        ActiveTool::Angle,
        ActiveTool::Bracket,
        ActiveTool::Magnifier,
    ] {
        let (mut engine, view) = setup(tool);
        if tool == ActiveTool::Angle {
            click(&mut engine, view, 100.0, 0.0);
            click(&mut engine, view, 0.0, 0.0);
            pointer(&mut engine, view, PointerEventType::Move, 0.0, -100.0);
        } else {
            pointer(&mut engine, view, PointerEventType::Down, -100.0, -80.0);
            pointer(&mut engine, view, PointerEventType::Move, 0.0, 0.0);
        }
        engine.set_creation_color(Some(UPDATED)).unwrap();
        for edit_color in [false, true] {
            match tool {
                ActiveTool::Distance => {
                    let mut style = engine.editor.distance_style(&engine.model);
                    style.stroke_width = 7.0;
                    style.stroke = UPDATED;
                    engine
                        .set_viewport_distance_style_patch(
                            view,
                            style,
                            DISTANCE_STYLE_PROPERTY_STROKE_WIDTH
                                | if edit_color {
                                    DISTANCE_STYLE_PROPERTY_STROKE
                                } else {
                                    0
                                },
                        )
                        .unwrap();
                }
                ActiveTool::Angle => {
                    let mut style = engine.editor.angle_style(&engine.model);
                    style.stroke_width = 7.0;
                    style.stroke = UPDATED;
                    engine
                        .set_viewport_angle_style_patch(
                            view,
                            style,
                            ANGLE_STYLE_PROPERTY_STROKE_WIDTH
                                | if edit_color {
                                    ANGLE_STYLE_PROPERTY_STROKE
                                } else {
                                    0
                                },
                        )
                        .unwrap();
                }
                ActiveTool::Bracket => {
                    let mut style = engine.editor.bracket_style(&engine.model);
                    style.stroke_width = 7.0;
                    style.stroke = UPDATED;
                    engine
                        .set_viewport_bracket_style_patch(
                            view,
                            style,
                            BRACKET_STYLE_PROPERTY_STROKE_WIDTH
                                | if edit_color {
                                    BRACKET_STYLE_PROPERTY_STROKE
                                } else {
                                    0
                                },
                        )
                        .unwrap();
                }
                ActiveTool::Magnifier => {
                    let mut style = engine.editor.magnifier_style(&engine.model);
                    style.stroke_width = 7.0;
                    style.stroke = UPDATED;
                    engine
                        .set_viewport_magnifier_style_patch(
                            view,
                            style,
                            MAGNIFIER_STYLE_PROPERTY_STROKE_WIDTH
                                | if edit_color {
                                    MAGNIFIER_STYLE_PROPERTY_STROKE
                                } else {
                                    0
                                },
                            true,
                        )
                        .unwrap();
                }
                _ => unreachable!(),
            }
            assert_preview_colors(
                &engine,
                view,
                if edit_color { UPDATED } else { ORIGINAL },
                ORIGINAL,
            );
        }
        if tool == ActiveTool::Angle {
            click(&mut engine, view, 0.0, -100.0);
        } else {
            pointer(&mut engine, view, PointerEventType::Up, 100.0, 80.0);
        }
        assert_committed_colors(&engine, engine.model.paint_order()[0], UPDATED, ORIGINAL);
    }
}

#[test]
fn shared_creation_color_preserves_serial_number_drag_label_colors() {
    let (mut engine, view) = setup(ActiveTool::SerialNumber);
    pointer(&mut engine, view, PointerEventType::Down, -100.0, -80.0);
    engine.set_creation_color(Some(UPDATED)).unwrap();
    pointer(&mut engine, view, PointerEventType::Move, 100.0, 80.0);
    pointer(&mut engine, view, PointerEventType::Up, 100.0, 80.0);
    let serial = engine
        .model
        .serial_number(engine.model.paint_order()[0])
        .unwrap();
    assert_eq!((serial.color, serial.fill), (ORIGINAL, ORIGINAL));
    let label = engine.model.text(serial.text_element_id.unwrap()).unwrap();
    assert_eq!(
        (label.color, label.stroke, label.fill),
        (ORIGINAL, ORIGINAL, ORIGINAL)
    );
}

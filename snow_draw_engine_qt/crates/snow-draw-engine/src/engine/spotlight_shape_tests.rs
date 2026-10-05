use super::duplicate_drag_tests::pointer;
use super::*;
use snow_draw_engine_display::DisplayRectangleShape;
use snow_draw_engine_document::{HighlightShape, RectangleData};
use snow_draw_engine_editor::{SHAPE_STYLE_PROPERTY_SHAPE, ShapeKind, StyleToolbarSource};
use snow_draw_engine_interaction::PointerEventType;

fn fixture(shape: HighlightShape) -> (Engine, ViewportId, ElementId) {
    let mut config = EngineConfig::default();
    config.style_defaults.editor.spotlight_shape = shape;
    let mut engine = Engine::new(config);
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Spotlight)
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .shape_style
            .shape,
        shape
    );
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
    let preview = engine.acquire_patch(viewport, None).unwrap();
    let cutout = preview.decoration.spotlight_ops[0].insert_items[0];
    let display_shape = match shape {
        HighlightShape::Rectangle => DisplayRectangleShape::Rectangle,
        HighlightShape::Ellipse => DisplayRectangleShape::Ellipse,
        HighlightShape::Diamond => DisplayRectangleShape::Diamond,
    };
    assert_eq!(cutout.shape, display_shape);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        500.0,
        400.0,
        false,
    );
    let id = engine.model.paint_order()[0];
    assert_eq!(engine.model.rectangle(id).unwrap().highlight_shape, shape);
    (engine, viewport, id)
}

fn shape_patch(engine: &mut Engine, viewport: ViewportId, kind: ShapeKind, shape: HighlightShape) {
    let mut style = engine
        .viewport_style_toolbar_state(viewport)
        .unwrap()
        .shape_style;
    style.shape = shape;
    engine
        .set_viewport_shape_style_patch(
            viewport,
            ShapeStylePatch {
                kind,
                style,
                properties: SHAPE_STYLE_PROPERTY_SHAPE,
            },
        )
        .unwrap();
}

#[test]
fn spotlight_shapes_survive_edits_history_duplication_and_transforms() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, id) = fixture(shape);
        engine
            .set_viewport_active_tool(viewport, ActiveTool::Select)
            .unwrap();
        engine
            .select_element_with_viewport_changes(viewport, id)
            .unwrap();
        let before = *engine.model.rectangle(id).unwrap();
        let next_shape = match shape {
            HighlightShape::Rectangle => HighlightShape::Ellipse,
            HighlightShape::Ellipse => HighlightShape::Diamond,
            HighlightShape::Diamond => HighlightShape::Rectangle,
        };
        shape_patch(&mut engine, viewport, ShapeKind::Spotlight, next_shape);
        let state = engine.viewport_style_toolbar_state(viewport).unwrap();
        assert_eq!(state.source, StyleToolbarSource::SelectedSpotlight);
        assert_eq!(state.shape_style.shape, next_shape);
        engine.undo_with_viewport_changes().unwrap();
        assert_eq!(*engine.model.rectangle(id).unwrap(), before);
        engine.redo_with_viewport_changes().unwrap();
        let spotlight = *engine.model.rectangle(id).unwrap();
        assert_eq!(spotlight.highlight_shape, next_shape);
        shape_patch(
            &mut engine,
            viewport,
            ShapeKind::Rectangle,
            HighlightShape::Ellipse,
        );
        assert_eq!(*engine.model.rectangle(id).unwrap(), spotlight);
        for (kind, x, y) in [
            (PointerEventType::Down, 300.0, 200.0),
            (PointerEventType::Move, 270.0, 170.0),
            (PointerEventType::Up, 270.0, 170.0),
        ] {
            pointer(&mut engine, viewport, kind, x, y, true);
        }
        let resized = *engine.model.rectangle(id).unwrap();
        assert!(resized.width > spotlight.width);
        assert_eq!(resized.highlight_shape, next_shape);
        // The rotation handle is above the top-center of the selected bounds.
        let handle_y = 300.0 - resized.height / 2.0 - 20.0;
        for (kind, x, y) in [
            (PointerEventType::Down, 400.0, handle_y),
            (PointerEventType::Move, 600.0, 300.0),
            (PointerEventType::Up, 600.0, 300.0),
        ] {
            pointer(&mut engine, viewport, kind, x, y, false);
        }
        let transformed = *engine.model.rectangle(id).unwrap();
        assert!(transformed.rotation.abs() > 0.1);
        assert_eq!(transformed.highlight_shape, next_shape);
        engine
            .duplicate_selected_with_viewport_changes(viewport, Point::new(300.0, 0.0))
            .unwrap();
        let copy = engine.selected_ids()[0];
        assert_eq!(
            engine.model.rectangle(copy).unwrap().highlight_shape,
            next_shape
        );
        engine.undo_with_viewport_changes().unwrap();
        assert!(engine.model.rectangle(copy).is_err());
        engine.redo_with_viewport_changes().unwrap();
        assert_eq!(
            engine.model.rectangle(copy).unwrap().rotation,
            transformed.rotation
        );
    }
}

#[test]
fn spotlight_shape_sessions_preserve_elements_defaults_and_legacy_compatibility() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, id) = fixture(shape);
        engine
            .reset_editing_state_with_viewport_changes(viewport)
            .unwrap();
        let bytes = engine.serialize_document_session().unwrap();
        let mut restored =
            Engine::from_serialized_document_session_with_config(&bytes, EngineConfig::default())
                .unwrap();
        assert_eq!(
            *restored.model.rectangle(id).unwrap(),
            *engine.model.rectangle(id).unwrap()
        );
        let view = restored.create_viewport(ViewportConfig::default()).unwrap();
        restored
            .set_viewport_active_tool(view, ActiveTool::Spotlight)
            .unwrap();
        assert_eq!(
            restored
                .viewport_style_toolbar_state(view)
                .unwrap()
                .shape_style
                .shape,
            shape
        );
        restored
            .set_viewport_active_tool(view, ActiveTool::Shape)
            .unwrap();
        assert_eq!(
            restored
                .viewport_style_toolbar_state(view)
                .unwrap()
                .shape_style
                .shape,
            HighlightShape::Rectangle
        );
        let mut legacy: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(legacy["schemaVersion"], 7);
        legacy["schemaVersion"] = serde_json::json!(6);
        legacy["editor"]
            .as_object_mut()
            .unwrap()
            .remove("spotlightShape");
        let mut restored = Engine::from_serialized_document_session_with_config(
            &serde_json::to_vec(&legacy).unwrap(),
            EngineConfig::default(),
        )
        .unwrap();
        let view = restored.create_viewport(ViewportConfig::default()).unwrap();
        restored
            .set_viewport_active_tool(view, ActiveTool::Spotlight)
            .unwrap();
        assert_eq!(
            restored
                .viewport_style_toolbar_state(view)
                .unwrap()
                .shape_style
                .shape,
            HighlightShape::Rectangle
        );
        assert_eq!(restored.model.rectangle(id).unwrap().highlight_shape, shape);
        let history = engine.serialize_document_history().unwrap();
        let restored =
            Engine::from_serialized_document_history_with_config(&history, EngineConfig::default())
                .unwrap();
        assert_eq!(restored.model.rectangle(id).unwrap().highlight_shape, shape);
    }
}

#[test]
fn spotlight_shape_switch_updates_an_active_creation_preview() {
    let (mut engine, viewport, _) = fixture(HighlightShape::Rectangle);
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Spotlight)
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        600.0,
        200.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        700.0,
        400.0,
        false,
    );
    shape_patch(
        &mut engine,
        viewport,
        ShapeKind::Spotlight,
        HighlightShape::Ellipse,
    );
    let patch = engine.acquire_patch(viewport, None).unwrap();
    let cutouts = &patch.decoration.spotlight_ops[0].insert_items;
    assert_eq!(cutouts.len(), 2);
    assert_eq!(
        cutouts[1].shape,
        snow_draw_engine_display::DisplayRectangleShape::Ellipse
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        700.0,
        400.0,
        false,
    );
    let rect: &RectangleData = engine
        .model
        .rectangle(engine.model.paint_order()[1])
        .unwrap();
    assert_eq!(rect.highlight_shape, HighlightShape::Ellipse);
    assert_eq!(rect.fill.a, 0);
    assert_eq!(rect.stroke_width, 0.0);
}

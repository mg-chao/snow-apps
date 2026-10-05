use super::*;
use snow_draw_engine_core::{ColorRgba8, arrow::ArrowType};
use snow_draw_engine_document::StrokeStyle;
use snow_draw_engine_interaction::{
    Modifiers, PointerButton, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
};

fn setup() -> (Engine, ViewportId) {
    let mut config = EngineConfig::default();
    config.style_defaults.editor.pen_highlight.stroke = ColorRgba8 {
        r: 230,
        g: 190,
        b: 30,
        a: 180,
    };
    config.style_defaults.editor.pen_highlight.stroke_width = 18.0;
    let mut engine = Engine::new(config);
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::PenHighlight)
        .unwrap();
    engine
        .set_viewport_snap_config(
            viewport,
            SnapConfig {
                enabled: false,
                ..SnapConfig::default()
            },
        )
        .unwrap();
    (engine, viewport)
}

fn pointer(
    engine: &mut Engine,
    viewport: ViewportId,
    kind: PointerEventType,
    point: Point<f64>,
    shift: bool,
) {
    engine
        .process_input(
            viewport,
            InputEvent::Pointer(PointerEvent {
                pointer_id: 1,
                event_type: kind,
                device: PointerDevice::Mouse,
                position: Point::new(point.x + 400.0, point.y + 300.0),
                button: Some(PointerButton::Primary),
                buttons: PointerButtons(PointerButtons::PRIMARY),
                modifiers: Modifiers {
                    shift,
                    ..Modifiers::default()
                },
            }),
        )
        .unwrap();
}

fn assert_point(actual: Point<f64>, expected: Point<f64>) {
    assert!(
        (actual.x - expected.x).hypot(actual.y - expected.y) < 1e-9,
        "actual={actual:?}, expected={expected:?}"
    );
}

#[test]
fn pen_highlight_shift_creation_is_one_undo_step() {
    let (mut engine, viewport) = setup();
    let start = Point::new(-100.0, -50.0);
    let target = Point::new(100.0, -10.0);
    pointer(&mut engine, viewport, PointerEventType::Down, start, true);
    pointer(&mut engine, viewport, PointerEventType::Move, target, true);
    assert!(
        engine.model.paint_order().is_empty(),
        "the preview must not create a history entry"
    );
    pointer(&mut engine, viewport, PointerEventType::Up, target, true);
    let id = engine.model.paint_order()[0];
    let pen = engine.model.arrow(id).unwrap().clone();
    let angle = 15.0_f64.to_radians();
    let length = 200.0 * angle.cos() + 40.0 * angle.sin();
    assert_point(pen.start(), start);
    assert_point(
        pen.end(),
        Point::new(
            start.x + length * angle.cos(),
            start.y + length * angle.sin(),
        ),
    );
    assert!(pen.is_pen_highlight());
    assert_eq!(pen.stroke_width, 18.0);

    engine.undo_with_viewport_changes().unwrap();
    assert!(engine.model.paint_order().is_empty());
    engine.redo_with_viewport_changes().unwrap();
    assert_eq!(engine.model.paint_order(), &[id]);
    assert_eq!(engine.model.arrow(id).unwrap(), &pen);
}

#[test]
fn pen_highlight_shift_endpoint_rotation_preserves_style_and_history() {
    for tool in [ActiveTool::PenHighlight, ActiveTool::Select] {
        for endpoint in [0, 1] {
            let (mut engine, viewport) = setup();
            let points = [Point::new(-100.0, 0.0), Point::new(100.0, 0.0)];
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Down,
                points[0],
                false,
            );
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Up,
                points[1],
                false,
            );
            let id = engine.model.paint_order()[0];
            engine.set_viewport_active_tool(viewport, tool).unwrap();
            engine
                .select_element_with_viewport_changes(viewport, id)
                .unwrap();
            let original = engine.model.arrow(id).unwrap().clone();
            let anchor = points[1 - endpoint];
            let angle = if endpoint == 0 { 148.0_f64 } else { 32.0_f64 }.to_radians();
            let locked_angle = if endpoint == 0 { 150.0_f64 } else { 30.0_f64 }.to_radians();
            let target = Point::new(
                anchor.x + 200.0 * angle.cos(),
                anchor.y + 200.0 * angle.sin(),
            );
            let length = 200.0 * (angle - locked_angle).cos();
            let expected = Point::new(
                anchor.x + length * locked_angle.cos(),
                anchor.y + length * locked_angle.sin(),
            );
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Down,
                points[endpoint],
                false,
            );
            pointer(&mut engine, viewport, PointerEventType::Move, target, true);
            pointer(&mut engine, viewport, PointerEventType::Up, target, true);

            let rotated = engine.model.arrow(id).unwrap().clone();
            assert_point(rotated.global_points()[endpoint], expected);
            assert_point(rotated.global_points()[1 - endpoint], anchor);
            assert!(rotated.is_pen_highlight());
            assert_eq!(rotated.points.len(), 2);
            assert_eq!(rotated.stroke, original.stroke);
            assert_eq!(rotated.stroke_width, original.stroke_width);
            assert_eq!(rotated.opacity, original.opacity);
            assert_eq!(rotated.stroke_style, StrokeStyle::Solid);
            assert_eq!(rotated.arrow_type, ArrowType::Straight);
            assert_eq!(rotated.start_arrowhead, None);
            assert_eq!(rotated.end_arrowhead, None);

            engine.undo_with_viewport_changes().unwrap();
            assert_eq!(engine.model.arrow(id).unwrap(), &original);
            engine.redo_with_viewport_changes().unwrap();
            assert_eq!(engine.model.arrow(id).unwrap(), &rotated);
        }
    }
}

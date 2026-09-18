use super::*;
use snow_draw_engine_core::SnapConfig;
use snow_draw_engine_display::OverlayDisplayItem;
use snow_draw_engine_interaction::{
    PointerButton, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
};

fn pointer(engine: &mut Engine, viewport: ViewportId, kind: PointerEventType, x: f64, y: f64) {
    engine
        .process_input_with_viewport_changes(
            viewport,
            InputEvent::Pointer(PointerEvent {
                pointer_id: 1,
                event_type: kind,
                device: PointerDevice::Mouse,
                position: Point::new(x, y),
                button: Some(PointerButton::Primary),
                buttons: PointerButtons(PointerButtons::PRIMARY),
                modifiers: Default::default(),
            }),
        )
        .unwrap();
}

fn drag(engine: &mut Engine, viewport: ViewportId, from: (f64, f64), to: (f64, f64)) {
    pointer(engine, viewport, PointerEventType::Down, from.0, from.1);
    pointer(engine, viewport, PointerEventType::Move, to.0, to.1);
    pointer(engine, viewport, PointerEventType::Up, to.0, to.1);
}

fn snap_enabled_engine() -> (Engine, ViewportId) {
    let mut engine = Engine::default();
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine
        .set_viewport_snap_config(
            viewport,
            SnapConfig {
                enabled: true,
                distance: 10.0,
                ..SnapConfig::default()
            },
        )
        .unwrap();
    (engine, viewport)
}

fn snap_guide_count(engine: &Engine, viewport: ViewportId) -> usize {
    engine
        .acquire_patch(viewport, None)
        .unwrap()
        .overlay
        .ops
        .iter()
        .flat_map(|op| op.insert_items.iter())
        .filter(|item| matches!(item, OverlayDisplayItem::SnapGuide(_)))
        .count()
}

fn create_element(
    engine: &mut Engine,
    viewport: ViewportId,
    tool: ActiveTool,
    from: (f64, f64),
    to: (f64, f64),
) -> ElementId {
    engine.set_viewport_active_tool(viewport, tool).unwrap();
    drag(engine, viewport, from, to);
    *engine.model.paint_order().last().unwrap()
}

/// Rectangle A covers view x 300..500, y 200..400, i.e. canvas x -100..100,
/// y -100..100 (the camera maps the view center to the canvas origin).
/// Drags land edges within the 10 px snap distance of its borders so any
/// participation of the dragged element (or of a filter reference) would
/// visibly move the result.
fn setup_with_rectangle() -> (Engine, ViewportId, ElementId) {
    let (mut engine, viewport) = snap_enabled_engine();
    let rectangle = create_element(
        &mut engine,
        viewport,
        ActiveTool::Shape,
        (300.0, 200.0),
        (500.0, 400.0),
    );
    (engine, viewport, rectangle)
}

fn select(engine: &mut Engine, viewport: ViewportId, id: ElementId) {
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
}

fn view_from_canvas(x: f64, y: f64) -> (f64, f64) {
    (x + 400.0, y + 300.0)
}

fn proxy_min(engine: &Engine, id: ElementId) -> Point {
    let rect = engine.model.element_rect_proxy(id).unwrap();
    Point::new(
        rect.center.x - rect.width / 2.0,
        rect.center.y - rect.height / 2.0,
    )
}

#[test]
fn creating_a_filter_does_not_snap_to_nearby_elements() {
    let (mut engine, viewport, _) = setup_with_rectangle();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::RectangleFilter)
        .unwrap();
    pointer(&mut engine, viewport, PointerEventType::Down, 400.0, 450.0);
    pointer(&mut engine, viewport, PointerEventType::Move, 507.0, 550.0);
    assert_eq!(snap_guide_count(&engine, viewport), 0);
    pointer(&mut engine, viewport, PointerEventType::Up, 507.0, 550.0);

    let filter = engine.model.filter(engine.model.paint_order()[1]).unwrap();
    assert_eq!(filter.center, Point::new(53.5, 200.0));
    assert_eq!(filter.width, 107.0);
}

#[test]
fn moving_a_filter_does_not_snap_to_nearby_elements() {
    let (mut engine, viewport, _) = setup_with_rectangle();
    let filter_id = create_element(
        &mut engine,
        viewport,
        ActiveTool::RectangleFilter,
        (100.0, 450.0),
        (180.0, 530.0),
    );
    select(&mut engine, viewport, filter_id);
    // Raw move by (207, 0) puts the filter min_x at -93, seven pixels from
    // the rectangle min_x at -100; snapping would shift the center to -60.
    pointer(&mut engine, viewport, PointerEventType::Down, 140.0, 490.0);
    pointer(&mut engine, viewport, PointerEventType::Move, 347.0, 490.0);
    assert_eq!(snap_guide_count(&engine, viewport), 0);
    pointer(&mut engine, viewport, PointerEventType::Up, 347.0, 490.0);

    let filter = engine.model.filter(filter_id).unwrap();
    assert_eq!(filter.center, Point::new(-53.0, 190.0));
}

#[test]
fn resizing_a_filter_does_not_snap_to_nearby_elements() {
    let (mut engine, viewport, _) = setup_with_rectangle();
    let filter_id = create_element(
        &mut engine,
        viewport,
        ActiveTool::RectangleFilter,
        (100.0, 450.0),
        (180.0, 530.0),
    );
    select(&mut engine, viewport, filter_id);
    // Dragging the bottom-right handle to view x = 293 leaves the filter
    // max_x at -107, seven pixels from the rectangle min_x at -100;
    // snapping would produce a width of 200 centered at x = -200 instead.
    pointer(&mut engine, viewport, PointerEventType::Down, 180.0, 530.0);
    pointer(&mut engine, viewport, PointerEventType::Move, 293.0, 530.0);
    assert_eq!(snap_guide_count(&engine, viewport), 0);
    pointer(&mut engine, viewport, PointerEventType::Up, 293.0, 530.0);

    let filter = engine.model.filter(filter_id).unwrap();
    assert_eq!(filter.center, Point::new(-203.5, 190.0));
    assert_eq!(filter.width, 193.0);
}

#[test]
fn moving_a_rectangle_does_not_snap_to_filter_edges() {
    let (mut engine, viewport) = snap_enabled_engine();
    let filter_id = create_element(
        &mut engine,
        viewport,
        ActiveTool::RectangleFilter,
        (100.0, 450.0),
        (180.0, 530.0),
    );
    let rectangle_id = create_element(
        &mut engine,
        viewport,
        ActiveTool::Shape,
        (300.0, 200.0),
        (500.0, 400.0),
    );
    select(&mut engine, viewport, rectangle_id);
    // Raw move by (0, 257) puts the rectangle min_y at 157, seven pixels
    // above the filter min_y at 150; snapping would shift the center to 250.
    pointer(&mut engine, viewport, PointerEventType::Down, 400.0, 300.0);
    pointer(&mut engine, viewport, PointerEventType::Move, 400.0, 557.0);
    assert_eq!(snap_guide_count(&engine, viewport), 0);
    pointer(&mut engine, viewport, PointerEventType::Up, 400.0, 557.0);

    let rect = engine.model.rectangle(rectangle_id).unwrap();
    assert_eq!(rect.center, Point::new(0.0, 257.0));
    assert!(engine.model.filter(filter_id).is_ok());
}

#[test]
fn moving_a_rectangle_still_snaps_to_other_rectangles() {
    let (mut engine, viewport, _) = setup_with_rectangle();
    let moved_id = create_element(
        &mut engine,
        viewport,
        ActiveTool::Shape,
        (100.0, 450.0),
        (180.0, 530.0),
    );
    select(&mut engine, viewport, moved_id);
    pointer(&mut engine, viewport, PointerEventType::Down, 140.0, 490.0);
    pointer(&mut engine, viewport, PointerEventType::Move, 347.0, 490.0);
    assert!(snap_guide_count(&engine, viewport) > 0);
    pointer(&mut engine, viewport, PointerEventType::Up, 347.0, 490.0);

    let rect = engine.model.rectangle(moved_id).unwrap();
    assert_eq!(rect.center, Point::new(-60.0, 190.0));
}

#[test]
fn creating_a_rectangle_does_not_snap_to_filter_edges() {
    let (mut engine, viewport) = snap_enabled_engine();
    create_element(
        &mut engine,
        viewport,
        ActiveTool::RectangleFilter,
        (100.0, 450.0),
        (180.0, 530.0),
    );
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Shape)
        .unwrap();
    pointer(&mut engine, viewport, PointerEventType::Down, 300.0, 200.0);
    pointer(&mut engine, viewport, PointerEventType::Move, 500.0, 537.0);
    assert_eq!(snap_guide_count(&engine, viewport), 0);
    pointer(&mut engine, viewport, PointerEventType::Up, 500.0, 537.0);

    let rect = engine
        .model
        .rectangle(engine.model.paint_order()[1])
        .unwrap();
    assert_eq!(rect.center, Point::new(0.0, 68.5));
    assert_eq!(rect.width, 200.0);
    assert_eq!(rect.height, 337.0);
}

#[test]
fn moving_a_pen_filter_does_not_snap_to_nearby_elements() {
    let (mut engine, viewport, _) = setup_with_rectangle();
    let filter_id = create_element(
        &mut engine,
        viewport,
        ActiveTool::PenFilter,
        (100.0, 450.0),
        (180.0, 530.0),
    );
    let before = engine.model.element_rect_proxy(filter_id).unwrap();
    let dx = -93.0 - proxy_min(&engine, filter_id).x;
    select(&mut engine, viewport, filter_id);
    let (start_x, start_y) = view_from_canvas(before.center.x, before.center.y);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        start_x,
        start_y,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        start_x + dx,
        start_y,
    );
    assert_eq!(snap_guide_count(&engine, viewport), 0);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        start_x + dx,
        start_y,
    );

    let after = engine.model.element_rect_proxy(filter_id).unwrap();
    assert_eq!(
        after.center,
        Point::new(before.center.x + dx, before.center.y)
    );
}

#[test]
fn moving_a_rectangle_does_not_snap_to_pen_filter_edges() {
    let (mut engine, viewport) = snap_enabled_engine();
    let filter_id = create_element(
        &mut engine,
        viewport,
        ActiveTool::PenFilter,
        (100.0, 450.0),
        (180.0, 530.0),
    );
    let rectangle_id = create_element(
        &mut engine,
        viewport,
        ActiveTool::Shape,
        (300.0, 200.0),
        (500.0, 400.0),
    );
    let dy = proxy_min(&engine, filter_id).y + 7.0 - (-100.0);
    select(&mut engine, viewport, rectangle_id);
    pointer(&mut engine, viewport, PointerEventType::Down, 400.0, 300.0);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        400.0,
        300.0 + dy,
    );
    assert_eq!(snap_guide_count(&engine, viewport), 0);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        400.0,
        300.0 + dy,
    );

    let rect = engine.model.rectangle(rectangle_id).unwrap();
    assert_eq!(rect.center, Point::new(0.0, dy));
    assert!(engine.model.pen_filter(filter_id).is_ok());
}

use super::*;
use snow_draw_engine_core::{
    ColorRgba8,
    arrow::{ArrowType, StrokeStyle},
    canvas_to_view,
};
use snow_draw_engine_document::{ElementMeta, FillStyle};
use snow_draw_engine_interaction::{PointerButtons, PointerDevice};

fn segment(document: &mut DocumentModel, points: &[Point<f64>], meta: ElementMeta) -> ElementId {
    let id = document.peek_next_element_id();
    let line = ArrowData::from_global_points(
        points,
        ColorRgba8 {
            a: 255,
            ..Default::default()
        },
        2.0,
        StrokeStyle::Solid,
        ArrowType::Straight,
        None,
        None,
    )
    .unwrap()
    .into_line(ColorRgba8::default(), FillStyle::Solid);
    let mut insert = Transaction::new("insert line");
    insert.insert_arrow(id, meta, line);
    document.apply_transaction(insert).unwrap();
    id
}

fn line_editor() -> Editor {
    let mut editor = Editor::new(EngineConfig::default()).unwrap();
    editor.set_surface_size(400, 300).unwrap();
    editor.set_active_tool(ActiveTool::Line).unwrap();
    editor
}

fn pointer(
    editor: &mut Editor,
    document: &DocumentModel,
    event_type: PointerEventType,
    point: Point<f64>,
    pressed: bool,
) -> EditorUpdate {
    let position = canvas_to_view(point, &editor.camera(), editor.surface_size());
    editor
        .process_input(
            document,
            InputEvent::Pointer(PointerEvent {
                pointer_id: 1,
                event_type,
                device: PointerDevice::Mouse,
                position,
                button: (event_type != PointerEventType::Move).then_some(PointerButton::Primary),
                buttons: if pressed {
                    PointerButtons(PointerButtons::PRIMARY)
                } else {
                    PointerButtons::default()
                },
                modifiers: Modifiers::default(),
                pressure: None,
                timestamp_ms: 0,
            }),
        )
        .unwrap()
}

#[test]
fn line_endpoint_snap_applies_to_both_ends_when_drawing() {
    let endpoints = [Point::new(-60.0, -40.0), Point::new(60.0, 40.0)];
    for [start, end] in [endpoints, [endpoints[1], endpoints[0]]] {
        let mut document = DocumentModel::new();
        let mut editor = line_editor();
        let target = segment(&mut document, &endpoints, ElementMeta::default());
        let (rectangle, _) = editor.preview_rectangle_with_snapping(
            &document,
            Point::new(55.0, 0.0),
            Point::new(110.0, 100.0),
            Modifiers::default(),
        );
        let mut insert = Transaction::new("insert rectangle by line endpoint");
        insert.insert_rectangle(
            document.peek_next_element_id(),
            ElementMeta::default(),
            rectangle.unwrap(),
        );
        document.apply_transaction(insert).unwrap();
        let id = document.peek_next_element_id();
        let press = Point::new(start.x, start.y + 2.0);
        let release = Point::new(end.x + 4.0, end.y + 4.0);

        pointer(&mut editor, &document, PointerEventType::Down, press, true);
        pointer(
            &mut editor,
            &document,
            PointerEventType::Move,
            release,
            true,
        );
        let Some(ElementCreationPreview::Arrow(preview)) =
            editor.presentation_state(&document).creation_preview
        else {
            panic!("drawing from a line endpoint should preview a new line");
        };
        assert_eq!(preview.global_points(), [start, end]);
        assert!(preview.bound_element_ids().is_empty());

        let update = pointer(&mut editor, &document, PointerEventType::Up, release, false);
        let Some(EditorCommand::ApplyTransaction(command)) = update.command else {
            panic!("releasing should create the snapped line");
        };
        document.apply_transaction(command.transaction).unwrap();
        assert_eq!(document.arrow(id).unwrap().global_points(), [start, end]);
        assert_eq!(document.arrow(target).unwrap().global_points(), endpoints);
    }
}

#[test]
fn line_endpoint_snap_persists_when_finishing_with_clicks() {
    let mut document = DocumentModel::new();
    let mut editor = line_editor();
    let end = Point::new(60.0, 40.0);
    segment(
        &mut document,
        &[end, Point::new(100.0, 80.0)],
        ElementMeta::default(),
    );
    let id = document.peek_next_element_id();
    let start = Point::new(-60.0, -40.0);
    let near_end = Point::new(end.x + 8.0, end.y);
    for point in [start, near_end] {
        pointer(&mut editor, &document, PointerEventType::Down, point, true);
        assert!(
            pointer(&mut editor, &document, PointerEventType::Up, point, false)
                .command
                .is_none()
        );
    }
    let update = pointer(
        &mut editor,
        &document,
        PointerEventType::DoubleClick,
        near_end,
        true,
    );
    let Some(EditorCommand::ApplyTransaction(command)) = update.command else {
        panic!("double-clicking at the snap radius should finish the line");
    };
    document.apply_transaction(command.transaction).unwrap();
    assert_eq!(document.arrow(id).unwrap().global_points(), [start, end]);
}

#[test]
fn line_endpoint_snap_applies_to_each_dragged_handle() {
    let mut document = DocumentModel::new();
    let mut editor = line_editor();
    let endpoints = [Point::new(-60.0, 30.0), Point::new(60.0, 30.0)];
    let original = [Point::new(-60.0, -40.0), Point::new(60.0, -40.0)];
    segment(&mut document, &endpoints, ElementMeta::default());
    let id = segment(&mut document, &original, ElementMeta::default());
    editor.select_element(&document, id).unwrap();

    let mut expected = original;
    for (index, endpoint) in endpoints.into_iter().enumerate() {
        let press = original[index];
        let release = Point::new(endpoint.x + 4.0, endpoint.y + 4.0);
        expected[index] = endpoint;

        pointer(&mut editor, &document, PointerEventType::Down, press, true);
        pointer(
            &mut editor,
            &document,
            PointerEventType::Move,
            release,
            true,
        );
        let preview = editor.presentation_state(&document).preview_arrows;
        assert_eq!(preview[0].arrow.global_points(), expected);

        let update = pointer(&mut editor, &document, PointerEventType::Up, release, false);
        let Some(EditorCommand::ApplyTransaction(command)) = update.command else {
            panic!("releasing should update the dragged endpoint");
        };
        document.apply_transaction(command.transaction).unwrap();
        assert_eq!(document.arrow(id).unwrap().global_points(), expected);
    }

    let press = endpoints[1];
    let release = Point::new(press.x + 2.0, press.y - 3.0);
    pointer(&mut editor, &document, PointerEventType::Down, press, true);
    pointer(
        &mut editor,
        &document,
        PointerEventType::Move,
        release,
        true,
    );
    assert!(
        pointer(&mut editor, &document, PointerEventType::Up, release, false)
            .command
            .is_none()
    );
}

#[test]
fn line_endpoint_snap_uses_nearest_visible_endpoint_in_screen_radius() {
    let mut document = DocumentModel::new();
    let mut editor = line_editor();
    editor
        .set_camera(Camera {
            zoom: 2.0,
            ..editor.camera()
        })
        .unwrap();
    let nearer = Point::new(13.0, 20.0);
    let source = Point::new(10.0, 20.0);
    let visible = segment(
        &mut document,
        &[nearer, Point::new(70.0, 20.0)],
        ElementMeta::default(),
    );
    segment(
        &mut document,
        &[Point::new(6.0, 20.0), Point::new(0.0, 50.0)],
        ElementMeta::default(),
    );
    segment(
        &mut document,
        &[source, Point::new(0.0, 20.0)],
        ElementMeta {
            visible: false,
            ..ElementMeta::default()
        },
    );

    assert_eq!(
        editor.nearby_line_endpoint(&document, source, None),
        Some(nearer)
    );
    assert_eq!(
        editor.nearby_line_endpoint(&document, source, Some(visible)),
        Some(Point::new(6.0, 20.0))
    );
    assert_eq!(
        editor.nearby_line_endpoint(&document, Point::new(10.0, 23.0), None),
        None
    );
    assert_eq!(
        editor.nearby_line_endpoint(&document, Point::new(40.0, 20.0), None),
        None
    );
}

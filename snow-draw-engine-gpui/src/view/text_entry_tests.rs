use super::*;

fn text_fixture(
    cx: &mut TestAppContext,
    tool: draw::ActiveTool,
    selected: bool,
    zoom: f64,
) -> (
    Entity<CanvasView>,
    VisualTestContext,
    draw::ElementId,
    Point<Pixels>,
) {
    let mut document = CanvasDocument::default();
    document
        .command(|engine| {
            engine.apply_annotation_json(
                br#"{"version":1,"operations":[{"type":"text","bounds":[100,100,200,80],"text":"Edit this text","style":{"font_size":20}}]}"#,
            )
        })
        .unwrap();
    let (view, mut visual) = fixture(cx, document);
    let (id, position) = visual.update(|window, cx| {
        view.update(cx, |view, cx| {
            let id = view
                .session
                .scene()
                .ordered_nodes()
                .map(|node| node.item.as_ref())
                .find_map(|item| {
                    if let draw::SceneDisplayItem::Text(text) = item {
                        Some(draw::ElementId {
                            index: text.id.index,
                            generation: text.id.generation,
                        })
                    } else {
                        None
                    }
                })
                .unwrap();
            let viewport = view.session.viewport();
            view.document
                .command(|engine| engine.reset_editing_state_with_viewport_changes(viewport))
                .unwrap();
            view.set_tool(tool, window, cx);
            if selected {
                view.document
                    .command(|engine| engine.select_element_with_viewport_changes(viewport, id))
                    .unwrap();
            }
            assert_eq!(
                view.document
                    .engine()
                    .viewport_active_tool(viewport)
                    .unwrap(),
                tool
            );
            let center = view.document.engine().text_element_info(id).unwrap().center;
            view.session
                .set_camera(&mut view.document, draw::Camera { center, zoom })
                .unwrap();
            view.changed(window, cx);
            let frame = view.session.scene().frame();
            let local = draw::canvas_to_view(center, &frame.camera, frame.surface);
            (
                id,
                view.bounds.origin + point(px(local.x as f32), px(local.y as f32)),
            )
        })
    });
    paint(&mut visual);
    (view, visual, id, position)
}

fn click(visual: &mut VisualTestContext, position: Point<Pixels>, modifiers: Modifiers) {
    visual.simulate_event(MouseDownEvent {
        button: MouseButton::Left,
        position,
        modifiers,
        click_count: 1,
        ..Default::default()
    });
    visual.simulate_event(MouseUpEvent {
        button: MouseButton::Left,
        position,
        modifiers,
        click_count: 1,
    });
    paint(visual);
}

#[gpui::test]
fn text_entry_click_opens_existing_text_and_preserves_commit_cancel_history(
    cx: &mut TestAppContext,
) {
    let command = if cfg!(target_os = "macos") {
        "cmd"
    } else {
        "ctrl"
    };
    for (tool, selected, zoom) in [
        (draw::ActiveTool::Text, false, 0.5),
        (draw::ActiveTool::Text, true, 2.0),
        (draw::ActiveTool::Select, true, 1.0),
    ] {
        let (view, mut visual, id, position) = text_fixture(cx, tool, selected, zoom);
        let before = view.read_with(&visual, |view, _| {
            view.document.engine().document_checkpoint().unwrap()
        });
        click(&mut visual, position, Modifiers::default());
        view.read_with(&visual, |view, _| {
            let draft = view.draft.as_ref().unwrap_or_else(|| {
                panic!("text click must open the native editor: tool={tool:?}, selected={selected}, zoom={zoom}, error={:?}, selected_ids={:?}", view.error, view.document.engine().selected_ids())
            });
            assert_eq!(draft.target, draw::TextCommitTarget::Existing(id));
            assert_eq!(draft.buffer.text, "Edit this text");
            assert_eq!(view.document.engine().selected_ids(), vec![id]);
            assert_eq!(
                view.document.engine().document_checkpoint().unwrap(),
                before
            );
        });
        visual.simulate_keystrokes(&format!("{command}-a"));
        visual.simulate_input("Cancelled edit");
        view.read_with(&visual, |view, _| {
            assert_eq!(view.draft.as_ref().unwrap().buffer.text, "Cancelled edit");
        });
        visual.simulate_keystrokes("escape");
        view.read_with(&visual, |view, _| {
            assert!(!view.has_text_draft());
            assert_eq!(
                view.document.engine().document_checkpoint().unwrap(),
                before
            );
        });
        paint(&mut visual);
        click(&mut visual, position, Modifiers::default());
        assert!(view.read_with(&visual, |view, _| view.has_text_draft()));
        visual.simulate_keystrokes(&format!("{command}-a"));
        visual.simulate_input("Committed edit");
        visual.simulate_keystrokes(&format!("{command}-enter"));
        view.read_with(&visual, |view, _| {
            assert!(!view.has_text_draft());
            assert_eq!(
                view.document.engine().text_element_info(id).unwrap().text,
                "Committed edit"
            );
        });
        visual.simulate_keystrokes(&format!("{command}-z"));
        view.read_with(&visual, |view, _| {
            assert_eq!(
                view.document.engine().document_checkpoint().unwrap(),
                before
            );
        });
    }
}

#[gpui::test]
fn text_entry_shift_selection_and_alt_drag_do_not_open_an_editor(cx: &mut TestAppContext) {
    for modifiers in [
        Modifiers {
            shift: true,
            ..Default::default()
        },
        Modifiers {
            alt: true,
            ..Default::default()
        },
    ] {
        let (view, mut visual, id, position) = text_fixture(cx, draw::ActiveTool::Text, false, 1.0);
        visual.simulate_event(MouseDownEvent {
            button: MouseButton::Left,
            position,
            modifiers,
            click_count: 1,
            ..Default::default()
        });
        view.read_with(&visual, |view, _| {
            assert!(!view.has_text_draft());
            assert_eq!(view.document.engine().selected_ids(), vec![id]);
        });
        let end = position + point(px(40.0), px(30.0));
        visual.simulate_mouse_move(end, Some(MouseButton::Left), modifiers);
        visual.simulate_event(MouseUpEvent {
            button: MouseButton::Left,
            position: end,
            modifiers,
            click_count: 1,
        });
        view.read_with(&visual, |view, _| {
            assert!(!view.has_text_draft());
            if modifiers.alt {
                assert_ne!(view.document.engine().selected_ids(), vec![id]);
                assert_eq!(
                    view.document.engine().text_element_info(id).unwrap().text,
                    "Edit this text"
                );
            }
        });
    }
}

#[gpui::test]
fn text_entry_finishing_press_does_not_start_a_new_draft(cx: &mut TestAppContext) {
    for text in ["", "New annotation"] {
        let (view, mut visual) = fixture(cx, CanvasDocument::default());
        visual.update(|window, cx| {
            view.update(cx, |view, cx| {
                view.set_tool(draw::ActiveTool::Text, window, cx);
            });
        });
        paint(&mut visual);
        let origin = view.read_with(&visual, |view, _| view.bounds.origin);
        click(
            &mut visual,
            origin + point(px(150.0), px(150.0)),
            Modifiers::default(),
        );
        assert!(view.read_with(&visual, |view, _| view.has_text_draft()));
        if !text.is_empty() {
            visual.simulate_input(text);
        }
        let blank = origin + point(px(500.0), px(350.0));
        click(&mut visual, blank, Modifiers::default());
        view.read_with(&visual, |view, _| {
            assert!(
                !view.has_text_draft(),
                "the finishing press must be consumed"
            );
            let texts: Vec<_> = view
                .session
                .scene()
                .ordered_nodes()
                .map(|node| node.item.as_ref())
                .filter_map(|item| match item {
                    draw::SceneDisplayItem::Text(item) => Some(item.text.as_str()),
                    _ => None,
                })
                .collect();
            assert_eq!(texts, if text.is_empty() { vec![] } else { vec![text] });
        });
        // A distinct press still follows the text tool's normal creation policy.
        click(&mut visual, blank, Modifiers::default());
        assert!(view.read_with(&visual, |view, _| view.has_text_draft()));
    }
}

#[gpui::test]
fn text_entry_dismissing_a_serial_label_leaves_the_next_badge_for_the_next_click(
    cx: &mut TestAppContext,
) {
    let (view, mut visual) = fixture(cx, CanvasDocument::default());
    visual.update(|window, cx| {
        view.update(cx, |view, cx| {
            view.set_tool(draw::ActiveTool::SerialNumber, window, cx)
        });
    });
    let origin = view.read_with(&visual, |view, _| view.bounds.origin);
    let start = origin + point(px(150.0), px(150.0));
    let end = start + point(px(80.0), px(20.0));
    visual.simulate_event(MouseDownEvent {
        button: MouseButton::Left,
        position: start,
        click_count: 1,
        ..Default::default()
    });
    visual.simulate_event(MouseMoveEvent {
        pressed_button: Some(MouseButton::Left),
        position: end,
        ..Default::default()
    });
    visual.simulate_event(MouseUpEvent {
        button: MouseButton::Left,
        position: end,
        ..Default::default()
    });
    paint(&mut visual);
    assert!(view.read_with(&visual, |view, _| view.has_text_draft()));
    visual.simulate_input("First badge");
    let blank = origin + point(px(500.0), px(350.0));
    for expected_badges in [1, 2] {
        click(&mut visual, blank, Modifiers::default());
        view.read_with(&visual, |view, _| {
            assert!(!view.has_text_draft());
            let badges = view.session.scene().ordered_nodes().filter(|node| {
                matches!(node.item.as_ref(), draw::SceneDisplayItem::SerialNumber(_))
            });
            assert_eq!(badges.count(), expected_badges);
        });
    }
}

#[gpui::test]
fn text_entry_finishing_press_can_open_another_existing_element(cx: &mut TestAppContext) {
    let (view, mut visual, id, position) = text_fixture(cx, draw::ActiveTool::Text, false, 1.0);
    let blank = view.read_with(&visual, |view, _| {
        view.bounds.origin + point(px(80.0), px(80.0))
    });
    click(&mut visual, blank, Modifiers::default());
    visual.simulate_input("Another annotation");
    click(&mut visual, position, Modifiers::default());
    view.read_with(&visual, |view, _| {
        let draft = view.draft.as_ref().unwrap();
        assert_eq!(draft.target, draw::TextCommitTarget::Existing(id));
        assert_eq!(draft.buffer.text, "Edit this text");
        assert!(view.session.scene().ordered_nodes().map(|node| node.item.as_ref()).any(|item| {
            matches!(item, draw::SceneDisplayItem::Text(item) if item.text == "Another annotation")
        }));
    });
}

#[gpui::test]
fn text_entry_initial_press_places_caret_and_drags_from_the_clicked_character(
    cx: &mut TestAppContext,
) {
    for zoom in [0.5, 2.0] {
        let (view, mut visual, id, _) = text_fixture(cx, draw::ActiveTool::Text, false, zoom);
        let (start, end) = visual.update(|window, cx| {
            view.update(cx, |view, cx| {
                view.begin_text(draw::Point::new(200.0, 140.0), Some(id), window, cx);
                let (x, y) = view.text_local_position(3).unwrap();
                let line_height = view.draft.as_ref().unwrap().layout.line_height;
                let start = view.text_world_position(x, y + line_height / 2.0).unwrap();
                let (x, y) = view.text_local_position(9).unwrap();
                let end = view.text_world_position(x, y + line_height / 2.0).unwrap();
                view.finish_text(false, window, cx);
                (start, end)
            })
        });
        paint(&mut visual);
        visual.simulate_event(MouseDownEvent {
            button: MouseButton::Left,
            position: start,
            modifiers: Modifiers::default(),
            click_count: 1,
            ..Default::default()
        });
        view.read_with(&visual, |view, _| {
            let draft = view.draft.as_ref().unwrap();
            assert_eq!(draft.target, draw::TextCommitTarget::Existing(id));
            assert_eq!(
                draft.buffer.caret, 3,
                "entry must hit-test the initial press"
            );
            assert_eq!(draft.buffer.anchor, 3);
            assert!(view.selecting_text);
        });
        visual.simulate_mouse_move(end, Some(MouseButton::Left), Modifiers::default());
        visual.simulate_event(MouseUpEvent {
            button: MouseButton::Left,
            position: end,
            modifiers: Modifiers::default(),
            click_count: 1,
        });
        view.read_with(&visual, |view, _| {
            assert_eq!(view.draft.as_ref().unwrap().buffer.selection(), 3..9);
            assert!(!view.selecting_text);
            assert!(!view.document.engine().has_pointer_capture());
        });
    }
}

#[gpui::test]
fn text_entry_rotated_wrapped_unicode_click_preserves_visual_line_and_grapheme(
    cx: &mut TestAppContext,
) {
    let text =
        "office e\u{0301} words words\nאבגדה q\u{0301} z 世界\nwrapping office text wraps again";
    for (rotation, zoom) in [(0.4, 0.75), (-0.5, 1.5)] {
        let mut document = CanvasDocument::default();
        let annotation = serde_json::to_vec(&serde_json::json!({
            "version": 1,
            "operations": [{
                "type": "text", "bounds": [100, 100, 140, 240], "text": text,
                "style": { "font_size": 20, "rotation": rotation }
            }]
        }))
        .unwrap();
        document
            .command(|engine| engine.apply_annotation_json(&annotation))
            .unwrap();
        let (view, mut visual) = fixture(cx, document);
        let (id, positions) = visual.update(|window, cx| {
            view.update(cx, |view, cx| {
                let id = view
                    .session
                    .scene()
                    .ordered_nodes()
                    .map(|node| node.item.as_ref())
                    .find_map(|item| match item {
                        draw::SceneDisplayItem::Text(item) => Some(draw::ElementId {
                            index: item.id.index,
                            generation: item.id.generation,
                        }),
                        _ => None,
                    })
                    .unwrap();
                let center = view.document.engine().text_element_info(id).unwrap().center;
                view.set_tool(draw::ActiveTool::Text, window, cx);
                view.session
                    .set_camera(&mut view.document, draw::Camera { center, zoom })
                    .unwrap();
                view.changed(window, cx);
                view.begin_text(center, Some(id), window, cx);
                let draft = view.draft.as_ref().unwrap();
                assert!(draft.layout.lines.len() > 3);
                let mut positions = Vec::new();
                for (row, line) in draft.layout.lines.iter().enumerate() {
                    let graphemes: Vec<_> = line
                        .text
                        .grapheme_indices(true)
                        .map(|(byte, _)| byte)
                        .collect();
                    let (byte, x) = line
                        .carets
                        .iter()
                        .find(|(byte, x)| {
                            *byte > 0
                                && graphemes.contains(byte)
                                && *x > 1.0
                                && *x < line.width - 1.0
                                && line.byte_at_x(*x) == *byte
                        })
                        .copied()
                        .unwrap();
                    let y =
                        -draft.layout.height / 2.0 + (row as f64 + 0.5) * draft.layout.line_height;
                    let position = view.text_world_position(-draft.width / 2.0 + x, y).unwrap();
                    positions.push((line.start + byte, row, x, position));
                }
                view.finish_text(false, window, cx);
                (id, positions)
            })
        });
        paint(&mut visual);
        for (byte, row, x, position) in positions {
            click(&mut visual, position, Modifiers::default());
            view.read_with(&visual, |view, _| {
                let draft = view.draft.as_ref().unwrap();
                assert_eq!(draft.target, draw::TextCommitTarget::Existing(id));
                assert_eq!(draft.buffer.caret, byte);
                assert_eq!(draft.buffer.anchor, byte);
                let affinity = draft.caret_affinity.unwrap();
                assert_eq!((affinity.0, affinity.1), (byte, row));
                assert!((affinity.2 - x).abs() < 0.05);
                assert!(text.grapheme_indices(true).any(|(index, _)| index == byte));
            });
            visual.simulate_keystrokes("escape");
            paint(&mut visual);
        }
    }
}

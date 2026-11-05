use super::*;
use snow_draw_engine_document::{ElementMeta, TextData, TextLayoutSize, Transaction};
use snow_draw_engine_editor::{TextCommitTarget, TextDraftCommit};
use snow_draw_engine_interaction::{
    Modifiers, PointerButton, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
};

#[test]
fn automatic_selection_serial_label_commit_preserves_live_selection() {
    for enabled in [false, true] {
        for excluded in [false, true] {
            for content in ["", "badge label"] {
                let (mut engine, viewport) = setup(ActiveTool::SerialNumber, enabled);
                engine
                    .set_automatic_selection_excluded_tools(if excluded {
                        ActiveTool::SerialNumber.policy_bit()
                    } else {
                        0
                    })
                    .unwrap();
                drag(&mut engine, viewport);
                let owner = engine.model.paint_order()[0];
                let text = engine.take_text_edit_request(viewport).unwrap().unwrap();
                let expected = if enabled && !excluded {
                    vec![owner]
                } else {
                    vec![]
                };
                assert_eq!(engine.selected_ids(), expected);
                engine
                    .set_automatically_select_newly_created_elements(!enabled)
                    .unwrap();
                let draft = TextDraftCommit::new(
                    TextCommitTarget::Existing(text),
                    engine.model.text(text).unwrap().center,
                    content,
                    TextLayoutSize::new(100.0, 30.0),
                    engine.editor.text_style(&engine.model),
                    true,
                    false,
                );
                engine
                    .commit_text_draft_with_viewport_changes(viewport, draft)
                    .unwrap();
                assert_eq!(
                    engine.selected_ids(),
                    expected,
                    "commit: enabled={enabled}, excluded={excluded}, content={content:?}"
                );
                assert!(engine.undo().unwrap());
                assert_eq!(engine.selected_ids(), expected);
                assert!(engine.redo().unwrap());
                assert_eq!(engine.selected_ids(), expected);
            }
        }
    }
}

fn setup(tool: ActiveTool, enabled: bool) -> (Engine, ViewportId) {
    let mut engine = Engine::default();
    assert!(!engine.editor.automatically_select_newly_created_elements());
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine.set_viewport_active_tool(viewport, tool).unwrap();
    engine
        .set_automatically_select_newly_created_elements(enabled)
        .unwrap();
    engine.set_quick_selection_disabled_tools(u64::MAX).unwrap();
    engine.set_automatic_selection_excluded_tools(0).unwrap();
    (engine, viewport)
}

#[test]
fn clear_selection_updates_viewports_without_changing_tool_or_history() {
    let (mut engine, viewport) = setup(ActiveTool::Arrow, true);
    let second = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine.set_viewport_surface_size(second, 800, 600).unwrap();
    drag(&mut engine, viewport);
    assert_eq!(engine.selected_ids().len(), 1);
    let revision = engine.model.document_revision();
    let history = engine.history_state();

    let result = engine
        .clear_selection_with_viewport_changes(viewport)
        .unwrap();
    assert!(engine.selected_ids().is_empty());
    assert!(result.changed_viewports.contains(&viewport));
    assert!(result.changed_viewports.contains(&second));
    assert_eq!(
        engine.viewport_active_tool(viewport).unwrap(),
        ActiveTool::Arrow
    );
    assert_eq!(engine.model.document_revision(), revision);
    assert_eq!(engine.history_state(), history);
    assert!(
        engine
            .clear_selection_with_viewport_changes(viewport)
            .unwrap()
            .changed_viewports
            .is_empty()
    );
    assert!(engine.undo().unwrap());
    assert!(engine.model.paint_order().is_empty());
}

fn pointer(engine: &mut Engine, viewport: ViewportId, kind: PointerEventType, x: f64, y: f64) {
    engine
        .process_input(
            viewport,
            InputEvent::Pointer(PointerEvent {
                pressure: None,
                timestamp_ms: 0,
                pointer_id: 1,
                event_type: kind,
                device: PointerDevice::Mouse,
                position: Point::new(x, y),
                button: matches!(kind, PointerEventType::Down | PointerEventType::Up)
                    .then_some(PointerButton::Primary),
                buttons: if matches!(kind, PointerEventType::Down | PointerEventType::Move) {
                    PointerButtons(PointerButtons::PRIMARY)
                } else {
                    PointerButtons::default()
                },
                modifiers: Modifiers::default(),
            }),
        )
        .unwrap();
}

fn drag(engine: &mut Engine, viewport: ViewportId) {
    pointer(engine, viewport, PointerEventType::Down, 100.0, 100.0);
    pointer(engine, viewport, PointerEventType::Move, 200.0, 160.0);
    pointer(engine, viewport, PointerEventType::Up, 200.0, 160.0);
}

#[test]
fn automatic_selection_covers_drawing_tools_without_switching_tools() {
    for tool in [
        ActiveTool::Shape,
        ActiveTool::Arrow,
        ActiveTool::Line,
        ActiveTool::Distance,
        ActiveTool::FreeDraw,
        ActiveTool::RectangleHighlight,
        ActiveTool::PenHighlight,
        ActiveTool::Spotlight,
        ActiveTool::Magnifier,
        ActiveTool::RectangleFilter,
        ActiveTool::PenFilter,
        ActiveTool::SpeechBubble,
    ] {
        for enabled in [false, true] {
            let (mut engine, viewport) = setup(tool, enabled);
            drag(&mut engine, viewport);
            let id = engine.model.paint_order()[0];
            assert_eq!(
                engine.selected_ids(),
                if enabled { vec![id] } else { vec![] },
                "{tool:?}, enabled={enabled}"
            );
            assert_eq!(engine.viewport_active_tool(viewport).unwrap(), tool);
            assert!(!engine.has_pointer_capture());
        }
    }
}

#[test]
fn automatic_selection_uses_measurement_owner_and_serial_badge() {
    for enabled in [false, true] {
        let (mut engine, viewport) = setup(ActiveTool::Angle, enabled);
        for (x, y) in [(200.0, 100.0), (100.0, 100.0), (100.0, 200.0)] {
            pointer(&mut engine, viewport, PointerEventType::Down, x, y);
            pointer(&mut engine, viewport, PointerEventType::Up, x, y);
        }
        assert_eq!(engine.model.paint_order().len(), 2);
        assert_eq!(
            engine.selected_ids(),
            if enabled {
                vec![engine.model.paint_order()[0]]
            } else {
                vec![]
            }
        );

        let (mut engine, viewport) = setup(ActiveTool::SerialNumber, enabled);
        pointer(&mut engine, viewport, PointerEventType::Down, 100.0, 100.0);
        pointer(&mut engine, viewport, PointerEventType::Up, 100.0, 100.0);
        assert_eq!(
            engine.selected_ids(),
            if enabled {
                vec![engine.model.paint_order()[0]]
            } else {
                vec![]
            }
        );
        assert_eq!(
            engine.viewport_active_tool(viewport).unwrap(),
            ActiveTool::SerialNumber
        );
    }
    let (mut engine, viewport) = setup(ActiveTool::SerialNumber, true);
    drag(&mut engine, viewport);
    assert_eq!(engine.model.paint_order().len(), 2);
    assert_eq!(engine.selected_ids(), vec![engine.model.paint_order()[0]]);
}

#[test]
fn automatic_selection_text_commit_and_existing_edits() {
    for enabled in [false, true] {
        let (mut engine, viewport) = setup(ActiveTool::Text, enabled);
        let draft = |engine: &Engine, target, content| {
            TextDraftCommit::new(
                target,
                Point::new(0.0, 0.0),
                content,
                TextLayoutSize::new(100.0, 30.0),
                engine.editor.text_style(&engine.model),
                true,
                true,
            )
        };
        engine
            .commit_text_draft_with_viewport_changes(
                viewport,
                draft(&engine, TextCommitTarget::New, "new text"),
            )
            .unwrap();
        let id = engine.model.paint_order()[0];
        assert_eq!(
            engine.selected_ids(),
            if enabled { vec![id] } else { vec![] }
        );
        assert_eq!(
            engine.viewport_active_tool(viewport).unwrap(),
            ActiveTool::Text
        );
        engine
            .commit_text_draft_with_viewport_changes(
                viewport,
                draft(&engine, TextCommitTarget::Existing(id), "edited text"),
            )
            .unwrap();
        assert!(
            engine.selected_ids().is_empty(),
            "existing text retains its commit behavior"
        );
        engine
            .commit_text_draft_with_viewport_changes(
                viewport,
                draft(&engine, TextCommitTarget::New, "   "),
            )
            .unwrap();
        assert_eq!(engine.model.paint_order().len(), 1);
        assert!(engine.selected_ids().is_empty());
    }
}

#[test]
fn automatic_selection_preserves_compound_text_editing() {
    for tool in [ActiveTool::Arrow, ActiveTool::SpeechBubble] {
        let (mut engine, viewport) = setup(tool, true);
        drag(&mut engine, viewport);
        let owner = engine.model.paint_order()[0];
        let (target, center, style) = if tool == ActiveTool::Arrow {
            let (info, style) = engine.arrow_text_target(viewport, None).unwrap().unwrap();
            (TextCommitTarget::NewArrow(owner), info.center, style)
        } else {
            let (info, style) = engine
                .speech_bubble_text_target(viewport, None)
                .unwrap()
                .unwrap();
            (TextCommitTarget::NewSpeechBubble(owner), info.center, style)
        };
        engine
            .commit_text_draft_with_viewport_changes(
                viewport,
                TextDraftCommit::new(
                    target,
                    center,
                    "attached text",
                    TextLayoutSize::new(80.0, 24.0),
                    style,
                    true,
                    false,
                ),
            )
            .unwrap();
        assert_eq!(engine.model.paint_order().len(), 2);
        assert_eq!(engine.selected_ids(), vec![owner]);
        assert_eq!(engine.viewport_active_tool(viewport).unwrap(), tool);
    }
}

#[test]
fn automatic_selection_is_applied_only_after_successful_creation() {
    let (mut engine, viewport) = setup(ActiveTool::Shape, true);
    pointer(&mut engine, viewport, PointerEventType::Down, 100.0, 100.0);
    pointer(&mut engine, viewport, PointerEventType::Move, 200.0, 160.0);
    assert!(engine.selected_ids().is_empty());
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Cancel,
        200.0,
        160.0,
    );
    assert!(engine.model.paint_order().is_empty());
    assert!(engine.selected_ids().is_empty());
    let id = engine.model.peek_next_element_id();
    let mut invalid = Transaction::new("invalid creation");
    invalid.insert_text(
        id,
        ElementMeta::default(),
        TextData {
            font_size: f64::NAN,
            ..Default::default()
        },
    );
    assert!(
        engine
            .commit_transaction(
                viewport,
                ApplyTransactionCommand {
                    transaction: invalid,
                    history_undo_snapshot: None,
                    created_selection_target: Some(id),
                }
            )
            .is_err()
    );
    assert!(engine.selected_ids().is_empty());
    assert!(!engine.history_state().can_undo);
}

#[test]
fn automatic_selection_preserves_history_and_live_preference_changes() {
    let (mut engine, viewport) = setup(ActiveTool::Shape, false);
    drag(&mut engine, viewport);
    assert!(engine.selected_ids().is_empty());
    engine
        .set_automatically_select_newly_created_elements(true)
        .unwrap();
    assert!(engine.selected_ids().is_empty());
    engine.undo().unwrap();
    drag(&mut engine, viewport);
    assert_eq!(engine.selected_ids().len(), 1);
    engine
        .set_automatically_select_newly_created_elements(false)
        .unwrap();
    assert_eq!(
        engine.selected_ids().len(),
        1,
        "preference changes preserve current selection"
    );
    assert!(engine.undo().unwrap());
    assert!(engine.selected_ids().is_empty());
    assert!(
        !engine.history_state().can_undo,
        "automatic selection adds no history entry"
    );
    assert!(engine.redo().unwrap());
    assert!(
        engine.selected_ids().is_empty(),
        "redo retains existing selection semantics"
    );
}

#[test]
fn automatic_selection_excludes_background_restore_elements() {
    for tool in [ActiveTool::RectangleEraser, ActiveTool::BrushEraser] {
        let (mut engine, viewport) = setup(tool, true);
        drag(&mut engine, viewport);
        assert_eq!(engine.model.paint_order().len(), 1);
        assert!(engine.selected_ids().is_empty());
    }
}

#[test]
fn automatic_selection_does_not_select_stroke_continuations() {
    let (mut engine, viewport) = setup(ActiveTool::FreeDraw, false);
    drag(&mut engine, viewport);
    let id = engine.model.paint_order()[0];
    let original = engine.model.free_draw(id).unwrap().clone();
    engine
        .set_automatically_select_newly_created_elements(true)
        .unwrap();
    pointer(&mut engine, viewport, PointerEventType::Down, 200.0, 160.0);
    pointer(&mut engine, viewport, PointerEventType::Move, 250.0, 170.0);
    pointer(&mut engine, viewport, PointerEventType::Up, 250.0, 170.0);
    assert_eq!(engine.model.paint_order(), &[id]);
    assert_ne!(*engine.model.free_draw(id).unwrap(), original);
    assert!(engine.selected_ids().is_empty());
}

#[test]
fn automatic_selection_allows_the_next_shape_drag_on_blank_canvas() {
    let (mut engine, viewport) = setup(ActiveTool::Shape, true);
    drag(&mut engine, viewport);
    let original = engine.model.paint_order()[0];
    assert_eq!(engine.selected_ids(), vec![original]);
    pointer(&mut engine, viewport, PointerEventType::Down, 500.0, 400.0);
    assert!(engine.selected_ids().is_empty());
    assert!(engine.has_pointer_capture());
    pointer(&mut engine, viewport, PointerEventType::Move, 550.0, 450.0);
    pointer(&mut engine, viewport, PointerEventType::Up, 550.0, 450.0);
    assert_eq!(engine.model.paint_order().len(), 2);
    assert_eq!(engine.selected_ids(), vec![engine.model.paint_order()[1]]);
    let rect = engine
        .model
        .rectangle(engine.model.paint_order()[1])
        .unwrap();
    assert_eq!(rect.center, Point::new(125.0, 125.0));
    assert_eq!((rect.width, rect.height), (50.0, 50.0));
    assert!(engine.undo().unwrap());
    assert_eq!(engine.model.paint_order(), &[original]);
}

#[test]
fn automatic_selection_exclusions_default_to_pen_filters_text_and_bubbles() {
    let defaults = Engine::default()
        .editor
        .automatic_selection_excluded_tools();
    assert_eq!(
        defaults,
        ActiveTool::FreeDraw.policy_bit()
            | ActiveTool::PenFilter.policy_bit()
            | ActiveTool::Text.policy_bit()
            | ActiveTool::SpeechBubble.policy_bit()
    );
    for tool in [
        ActiveTool::FreeDraw,
        ActiveTool::PenFilter,
        ActiveTool::Text,
        ActiveTool::SpeechBubble,
        ActiveTool::Shape,
    ] {
        let (mut engine, viewport) = setup(tool, true);
        engine
            .set_automatic_selection_excluded_tools(defaults)
            .unwrap();
        if tool == ActiveTool::Text {
            let draft = TextDraftCommit::new(
                TextCommitTarget::New,
                Point::new(0.0, 0.0),
                "default exclusions",
                TextLayoutSize::new(100.0, 30.0),
                engine.editor.text_style(&engine.model),
                true,
                true,
            );
            engine
                .commit_text_draft_with_viewport_changes(viewport, draft)
                .unwrap();
        } else {
            drag(&mut engine, viewport);
        }
        assert_eq!(
            engine.selected_ids().len(),
            usize::from(tool == ActiveTool::Shape)
        );
        assert_eq!(engine.viewport_active_tool(viewport).unwrap(), tool);
    }
}

#[test]
fn automatic_selection_exclusions_apply_live_without_changing_selection_or_history() {
    for tool in [
        ActiveTool::Shape,
        ActiveTool::Arrow,
        ActiveTool::Line,
        ActiveTool::Distance,
        ActiveTool::FreeDraw,
        ActiveTool::RectangleHighlight,
        ActiveTool::PenHighlight,
        ActiveTool::Spotlight,
        ActiveTool::Magnifier,
        ActiveTool::RectangleFilter,
        ActiveTool::PenFilter,
        ActiveTool::SerialNumber,
        ActiveTool::SpeechBubble,
    ] {
        let (mut engine, viewport) = setup(tool, true);
        let create = |engine: &mut Engine| {
            if tool == ActiveTool::SerialNumber {
                pointer(engine, viewport, PointerEventType::Down, 100.0, 100.0);
                pointer(engine, viewport, PointerEventType::Up, 100.0, 100.0);
            } else {
                drag(engine, viewport);
            }
        };
        engine
            .set_automatic_selection_excluded_tools(tool.policy_bit())
            .unwrap();
        create(&mut engine);
        assert!(engine.selected_ids().is_empty(), "excluded {tool:?}");
        engine.set_automatic_selection_excluded_tools(0).unwrap();
        assert!(
            engine.selected_ids().is_empty(),
            "changing policy does not select existing elements"
        );
        assert!(engine.undo().unwrap());
        create(&mut engine);
        assert_eq!(engine.selected_ids().len(), 1, "included {tool:?}");
        let selected = engine.selected_ids();
        engine
            .set_automatic_selection_excluded_tools(tool.policy_bit())
            .unwrap();
        assert_eq!(engine.selected_ids(), selected);
        assert!(engine.undo().unwrap());
        assert!(engine.selected_ids().is_empty(), "undo removes {tool:?}");
        assert!(
            !engine.history_state().can_undo,
            "policy changes add no history entries"
        );
        engine.set_automatic_selection_excluded_tools(0).unwrap();
        engine
            .set_automatically_select_newly_created_elements(false)
            .unwrap();
        create(&mut engine);
        assert!(
            engine.selected_ids().is_empty(),
            "the master switch still applies"
        );
        assert_eq!(engine.viewport_active_tool(viewport).unwrap(), tool);
    }
}

#[test]
fn automatic_selection_exclusions_cover_text_and_serial_labels() {
    let (mut engine, viewport) = setup(ActiveTool::Text, true);
    engine
        .set_automatic_selection_excluded_tools(ActiveTool::Text.policy_bit())
        .unwrap();
    let draft = TextDraftCommit::new(
        TextCommitTarget::New,
        Point::new(0.0, 0.0),
        "unselected text",
        TextLayoutSize::new(100.0, 30.0),
        engine.editor.text_style(&engine.model),
        true,
        true,
    );
    engine
        .commit_text_draft_with_viewport_changes(viewport, draft)
        .unwrap();
    assert_eq!(engine.model.paint_order().len(), 1);
    assert!(engine.selected_ids().is_empty());

    let (mut engine, viewport) = setup(ActiveTool::SerialNumber, true);
    engine
        .set_automatic_selection_excluded_tools(ActiveTool::SerialNumber.policy_bit())
        .unwrap();
    drag(&mut engine, viewport);
    assert_eq!(engine.model.paint_order().len(), 2);
    assert!(engine.selected_ids().is_empty());
}

#[test]
fn automatic_selection_speech_bubble_policy_survives_text_commit() {
    for enabled in [false, true] {
        for excluded in [false, true] {
            for content in ["", "bubble text"] {
                let (mut engine, viewport) = setup(ActiveTool::SpeechBubble, enabled);
                engine
                    .set_automatic_selection_excluded_tools(if excluded {
                        ActiveTool::SpeechBubble.policy_bit()
                    } else {
                        0
                    })
                    .unwrap();
                drag(&mut engine, viewport);
                let owner = engine.model.paint_order()[0];
                let expected = if enabled && !excluded {
                    vec![owner]
                } else {
                    vec![]
                };
                assert_eq!(
                    engine.selected_ids(),
                    expected,
                    "creation: enabled={enabled}, excluded={excluded}"
                );
                let (info, style) = engine
                    .take_speech_bubble_text_edit_target(viewport)
                    .unwrap()
                    .expect("text editing must not depend on automatic selection");
                assert_eq!(info.bubble_id, Some(owner));
                assert!(
                    engine
                        .take_speech_bubble_text_edit_target(viewport)
                        .unwrap()
                        .is_none()
                );
                engine
                    .commit_text_draft_with_viewport_changes(
                        viewport,
                        TextDraftCommit::new(
                            TextCommitTarget::NewSpeechBubble(owner),
                            info.center,
                            content,
                            TextLayoutSize::new(80.0, 24.0),
                            style,
                            true,
                            false,
                        ),
                    )
                    .unwrap();
                assert_eq!(
                    engine.selected_ids(),
                    expected,
                    "commit: enabled={enabled}, excluded={excluded}, content={content:?}"
                );
                assert_eq!(
                    engine.viewport_active_tool(viewport).unwrap(),
                    ActiveTool::SpeechBubble
                );
                assert!(!engine.has_pointer_capture());
            }
        }
    }
}

#[test]
fn automatic_selection_speech_bubble_text_commit_preserves_live_selection() {
    for selected in [false, true] {
        for content in ["", "bubble text"] {
            let (mut engine, viewport) = setup(ActiveTool::SpeechBubble, selected);
            drag(&mut engine, viewport);
            let (info, style) = engine
                .take_speech_bubble_text_edit_target(viewport)
                .unwrap()
                .unwrap();
            let owner = info.bubble_id.unwrap();
            let expected = engine.selected_ids();
            engine
                .set_automatically_select_newly_created_elements(!selected)
                .unwrap();
            engine
                .set_automatic_selection_excluded_tools(ActiveTool::SpeechBubble.policy_bit())
                .unwrap();
            engine
                .commit_text_draft_with_viewport_changes(
                    viewport,
                    TextDraftCommit::new(
                        TextCommitTarget::NewSpeechBubble(owner),
                        info.center,
                        content,
                        TextLayoutSize::new(80.0, 24.0),
                        style,
                        true,
                        false,
                    ),
                )
                .unwrap();
            assert_eq!(
                engine.selected_ids(),
                expected,
                "editing an existing owner must not reapply the creation policy"
            );
            if !content.is_empty() {
                assert!(engine.undo().unwrap());
                assert_eq!(engine.selected_ids(), expected);
                assert!(
                    engine
                        .model
                        .speech_bubble(owner)
                        .unwrap()
                        .text_element_id
                        .is_none()
                );
                assert!(engine.redo().unwrap());
                assert_eq!(engine.selected_ids(), expected);
                assert!(
                    engine
                        .model
                        .speech_bubble(owner)
                        .unwrap()
                        .text_element_id
                        .is_some()
                );
                engine.undo().unwrap();
            }
            assert!(engine.undo().unwrap());
            assert!(engine.model.paint_order().is_empty());
            assert!(engine.selected_ids().is_empty());
            assert!(!engine.history_state().can_undo);
        }
    }
}

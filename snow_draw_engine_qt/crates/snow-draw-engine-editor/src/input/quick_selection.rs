use super::*;

impl Editor {
    pub(crate) fn can_start_quick_selection(&self) -> bool {
        matches!(self.state.interaction, InteractionState::Idle)
            && self.state.eraser.active_pointers.is_empty()
            && !self.auto_filter_gesture_active()
    }

    pub(crate) fn selection_pointer_button(&self) -> Option<PointerButton> {
        match &self.state.interaction {
            InteractionState::PendingSelectionMove(state) => Some(state.button),
            InteractionState::EditingSelection(state) => Some(state.button),
            _ => None,
        }
    }

    pub fn hit_quick_selection_at(
        &self,
        document: &DocumentModel,
        canvas_point: Point<f64>,
        button: PointerButton,
    ) -> Option<ElementId> {
        if !matches!(button, PointerButton::Primary | PointerButton::Secondary)
            || !canvas_point.x.is_finite()
            || !canvas_point.y.is_finite()
            || !self.can_start_quick_selection()
        {
            return None;
        }
        match self.resolve_canvas_hit(
            document,
            self.quick_selection_policy(button),
            canvas_point,
            false,
        ) {
            CanvasHit::EligibleElement(id, _) => Some(id),
            _ => None,
        }
    }

    pub(super) fn handle_secondary_selection_pointer_down(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let Some(id) = self.hit_quick_selection_at(document, point, PointerButton::Secondary)
        else {
            if self.state.active_tool != ActiveTool::SerialNumber
                || !self
                    .state
                    .default_serial_number
                    .serial_number_type
                    .supports_number()
                || !matches!(
                    self.resolve_canvas_hit(
                        document,
                        Self::tool_policy_for(ActiveTool::Select),
                        point,
                        false,
                    ),
                    CanvasHit::Empty
                )
            {
                return Ok(InteractionOutput::default());
            }
            // Reset creation defaults after deselecting so existing annotations are untouched.
            // An explicit number edit also overrides the document's maximum for this format.
            self.clear_selection();
            self.clear_transient_visuals();
            let mut style = self.serial_number_style(document);
            style.number = 1;
            self.set_serial_number_style_patch(document, style, SERIAL_NUMBER_STYLE_MIXED_NUMBER)?;
            self.bump_overlay_state_revision();
            return Ok(InteractionOutput {
                consumed: true,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(self.tool_policy().default_cursor),
            });
        };
        self.clear_stroke_cursor_state();
        if event.modifiers.shift
            && self
                .quick_selection_policy(PointerButton::Secondary)
                .allow_shift_toggle
        {
            self.toggle_selection(document, id);
            return Ok(InteractionOutput {
                consumed: true,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(CursorStyle::Move),
            });
        }
        if !self.state.selection.contains(id) {
            self.set_selection_state_with_document(Some(document), vec![id], Some(id));
        }
        self.set_hovered_element(None);
        Ok(self.begin_current_selection_interaction(
            document,
            event,
            SelectionHitTarget::Move,
            point,
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{ColorRgba8, CornerRadii, EngineConfig};
    use snow_draw_engine_document::{
        ElementMeta, FillStyle, RectangleElementKind, SerialNumberNumericType, StrokeStyle,
        TextLayoutSize,
    };
    use snow_draw_engine_interaction::{PointerButtons, PointerDevice};

    fn rectangle(center: Point<f64>) -> RectangleData {
        RectangleData {
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: Default::default(),
            center,
            width: 40.0,
            height: 40.0,
            rotation: 0.0,
            fill: ColorRgba8 {
                r: 0,
                g: 0,
                b: 0,
                a: 255,
            },
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
    }

    fn fixture() -> (Editor, DocumentModel, ElementId) {
        let mut document = DocumentModel::new();
        let id = document.allocate_element_id();
        let mut transaction = Transaction::new("rectangle");
        transaction.insert_rectangle(id, ElementMeta::default(), rectangle(Point::new(0.0, 0.0)));
        document.apply_transaction(transaction).unwrap();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::Shape).unwrap();
        editor.set_quick_selection_disabled_tools(u64::MAX);
        (editor, document, id)
    }

    fn pointer(
        kind: PointerEventType,
        point: Point<f64>,
        button: PointerButton,
        modifiers: Modifiers,
    ) -> InputEvent {
        InputEvent::Pointer(PointerEvent {
            pointer_id: 1,
            event_type: kind,
            device: PointerDevice::Mouse,
            position: point,
            button: Some(button),
            buttons: PointerButtons(if kind == PointerEventType::Up {
                0
            } else {
                PointerButtons::SECONDARY
            }),
            modifiers,
        })
    }

    fn right(
        editor: &mut Editor,
        document: &DocumentModel,
        kind: PointerEventType,
        x: f64,
        y: f64,
    ) -> EditorUpdate {
        editor
            .process_input(
                document,
                pointer(
                    kind,
                    Point::new(x, y),
                    PointerButton::Secondary,
                    Modifiers::default(),
                ),
            )
            .unwrap()
    }

    #[test]
    fn right_selection_bypasses_setting_and_click_only_keeps_document_unchanged() {
        let (mut editor, document, id) = fixture();
        assert_eq!(
            editor.hit_quick_selection_at(&document, Point::new(0.0, 0.0), PointerButton::Primary),
            None
        );
        assert_eq!(
            editor.hit_quick_selection_at(
                &document,
                Point::new(0.0, 0.0),
                PointerButton::Secondary
            ),
            Some(id)
        );
        let down = right(&mut editor, &document, PointerEventType::Down, 100.0, 100.0);
        assert!(down.interaction.consumed);
        assert_eq!(editor.selected_ids(), vec![id]);
        assert_eq!(down.interaction.capture, PointerCaptureCommand::Capture(1));
        let up = right(&mut editor, &document, PointerEventType::Up, 100.0, 100.0);
        assert!(up.command.is_none());
        assert_eq!(up.interaction.capture, PointerCaptureCommand::Release);
        assert_eq!(editor.active_tool(), ActiveTool::Shape);
        editor.set_quick_selection_disabled_tools(0);
        assert_eq!(
            editor.hit_quick_selection_at(&document, Point::new(0.0, 0.0), PointerButton::Primary),
            Some(id)
        );
    }

    #[test]
    fn right_blank_serial_resets_only_current_numeric_type() {
        let types = [
            SerialNumberNumericType::Arabic,
            SerialNumberNumericType::Roman,
            SerialNumberNumericType::Chinese,
            SerialNumberNumericType::LowercaseLetters,
            SerialNumberNumericType::UppercaseLetters,
        ];
        for current in types {
            for event_type in [PointerEventType::Down, PointerEventType::DoubleClick] {
                let (mut editor, document, selected) = fixture();
                editor.set_active_tool(ActiveTool::SerialNumber).unwrap();
                for (index, numeric_type) in types.into_iter().enumerate() {
                    let mut style = editor.serial_number_style(&document);
                    style.numeric_type = numeric_type;
                    style.number = 10 + index as i64;
                    editor
                        .set_serial_number_style_patch(
                            &document,
                            style,
                            SERIAL_NUMBER_STYLE_MIXED_NUMERIC_TYPE
                                | SERIAL_NUMBER_STYLE_MIXED_NUMBER,
                        )
                        .unwrap();
                }
                let mut style = editor.serial_number_style(&document);
                style.numeric_type = current;
                editor
                    .set_serial_number_style_patch(
                        &document,
                        style,
                        SERIAL_NUMBER_STYLE_MIXED_NUMERIC_TYPE,
                    )
                    .unwrap();
                let mut expected = editor.serial_number_style(&document);
                expected.number = 1;
                editor.set_selection_state_with_document(
                    Some(&document),
                    vec![selected],
                    Some(selected),
                );
                let down = right(&mut editor, &document, event_type, 180.0, 180.0);
                assert!(down.interaction.consumed);
                assert_eq!(down.interaction.capture, PointerCaptureCommand::NoChange);
                assert!(down.command.is_none());
                assert!(editor.selected_ids().is_empty());
                assert_eq!(editor.serial_number_style(&document), expected);
                assert!(editor.state.serial_number_sequence_overridden[current as usize]);
                let up = right(&mut editor, &document, PointerEventType::Up, 180.0, 180.0);
                assert!(up.command.is_none());
                for (index, numeric_type) in types.into_iter().enumerate() {
                    assert_eq!(
                        editor.state.serial_number_values()[numeric_type as usize],
                        if numeric_type == current {
                            1
                        } else {
                            10 + index as i64
                        }
                    );
                }
                assert_eq!(document.paint_order().len(), 1);
            }
        }
    }

    #[test]
    fn right_serial_reset_requires_blank_canvas_and_active_serial_tool() {
        let (mut editor, document, _) = fixture();
        editor.set_active_tool(ActiveTool::SerialNumber).unwrap();
        let mut style = editor.serial_number_style(&document);
        style.number = 23;
        editor
            .set_serial_number_style_patch(
                &document,
                style.clone(),
                SERIAL_NUMBER_STYLE_MIXED_NUMBER,
            )
            .unwrap();
        let hit = right(&mut editor, &document, PointerEventType::Down, 100.0, 100.0);
        assert!(!hit.interaction.consumed && hit.command.is_none());
        assert_eq!(editor.serial_number_style(&document), style);
        editor.set_active_tool(ActiveTool::Shape).unwrap();
        let miss = right(&mut editor, &document, PointerEventType::Down, 180.0, 180.0);
        assert!(!miss.interaction.consumed && miss.command.is_none());
        assert_eq!(editor.serial_number_style(&document), style);
    }

    #[test]
    fn right_hit_skips_unmatched_hidden_and_restore_elements_in_paint_order() {
        let (editor, mut document, id) = fixture();
        let mut tx = Transaction::new("overlaps");
        let top = document.allocate_element_id();
        tx.insert_rectangle(top, ElementMeta::default(), rectangle(Point::new(0.0, 0.0)));
        let hidden = document.allocate_element_id();
        tx.insert_rectangle(
            hidden,
            ElementMeta {
                visible: false,
                ..ElementMeta::default()
            },
            rectangle(Point::new(0.0, 0.0)),
        );
        let text = document.allocate_element_id();
        tx.insert_text(
            text,
            ElementMeta::default(),
            TextData {
                text: "label".into(),
                auto_resize: false,
                layout: TextLayoutSize::new(40.0, 40.0),
                ..TextData::default()
            },
        );
        let restore = document.allocate_element_id();
        tx.insert_filter(
            restore,
            ElementMeta::default(),
            snow_draw_engine_document::FilterData {
                filter_type: snow_draw_engine_document::CanvasFilterType::RestoreBackground,
                strength: 1.0,
                width: 40.0,
                height: 40.0,
                ..Default::default()
            },
        );
        document.apply_transaction(tx).unwrap();
        assert_eq!(
            editor.hit_quick_selection_at(
                &document,
                Point::new(0.0, 0.0),
                PointerButton::Secondary
            ),
            Some(top)
        );
        assert_ne!(top, id);
        let mut editor = editor;
        editor.set_active_tool(ActiveTool::Text).unwrap();
        assert_eq!(
            editor.hit_quick_selection_at(
                &document,
                Point::new(0.0, 0.0),
                PointerButton::Secondary
            ),
            Some(text)
        );
        editor.set_active_tool(ActiveTool::FreeDraw).unwrap();
        let miss = right(&mut editor, &document, PointerEventType::Down, 100.0, 100.0);
        assert!(!miss.interaction.consumed && miss.command.is_none());
        assert!(editor.selected_ids().is_empty());
    }

    #[test]
    fn right_drag_ignores_other_button_release_and_alt_does_not_duplicate() {
        let (mut editor, mut document, id) = fixture();
        editor
            .process_input(
                &document,
                pointer(
                    PointerEventType::Down,
                    Point::new(100.0, 100.0),
                    PointerButton::Secondary,
                    Modifiers {
                        alt: true,
                        ..Default::default()
                    },
                ),
            )
            .unwrap();
        right(&mut editor, &document, PointerEventType::Move, 115.0, 112.0);
        let unrelated = editor
            .process_input(
                &document,
                pointer(
                    PointerEventType::Up,
                    Point::new(115.0, 112.0),
                    PointerButton::Primary,
                    Modifiers::default(),
                ),
            )
            .unwrap();
        assert!(!unrelated.interaction.consumed && unrelated.command.is_none());
        assert_eq!(
            editor.selection_pointer_button(),
            Some(PointerButton::Secondary)
        );
        let up = right(&mut editor, &document, PointerEventType::Up, 130.0, 125.0);
        let Some(EditorCommand::ApplyTransaction(command)) = up.command else {
            panic!("move transaction expected")
        };
        document.apply_transaction(command.transaction).unwrap();
        assert_eq!(
            document.rectangle(id).unwrap().center,
            Point::new(30.0, 25.0)
        );
        assert_eq!(document.paint_order().len(), 1);
    }

    #[test]
    fn right_drag_keeps_group_and_shift_toggles_without_capture() {
        let (mut editor, mut document, first) = fixture();
        let second = document.allocate_element_id();
        let mut tx = Transaction::new("second");
        tx.insert_rectangle(
            second,
            ElementMeta::default(),
            rectangle(Point::new(60.0, 0.0)),
        );
        document.apply_transaction(tx).unwrap();
        editor.set_selection_state_with_document(
            Some(&document),
            vec![first, second],
            Some(second),
        );
        right(&mut editor, &document, PointerEventType::Down, 100.0, 100.0);
        assert_eq!(editor.selected_ids(), vec![first, second]);
        right(&mut editor, &document, PointerEventType::Move, 100.0, 120.0);
        let up = right(&mut editor, &document, PointerEventType::Up, 100.0, 120.0);
        let Some(EditorCommand::ApplyTransaction(command)) = up.command else {
            panic!("group move expected")
        };
        document.apply_transaction(command.transaction).unwrap();
        assert_eq!(document.rectangle(second).unwrap().center.y, 20.0);
        let toggle = editor
            .process_input(
                &document,
                pointer(
                    PointerEventType::Down,
                    Point::new(100.0, 120.0),
                    PointerButton::Secondary,
                    Modifiers {
                        shift: true,
                        ..Default::default()
                    },
                ),
            )
            .unwrap();
        assert_eq!(editor.selected_ids(), vec![second]);
        assert_eq!(toggle.interaction.capture, PointerCaptureCommand::NoChange);
    }

    #[test]
    fn right_drag_cancellation_releases_capture_without_transaction() {
        for cancel in [
            InputEvent::FocusLost,
            InputEvent::Key(KeyEvent {
                event_type: KeyEventType::KeyDown,
                key_code: KeyCode::Escape,
                modifiers: Modifiers::default(),
                repeat: false,
            }),
        ] {
            let (mut editor, document, id) = fixture();
            right(&mut editor, &document, PointerEventType::Down, 100.0, 100.0);
            right(&mut editor, &document, PointerEventType::Move, 120.0, 120.0);
            let update = editor.process_input(&document, cancel).unwrap();
            assert_eq!(update.interaction.capture, PointerCaptureCommand::Release);
            assert!(update.command.is_none());
            assert_eq!(document.rectangle(id).unwrap().center, Point::new(0.0, 0.0));
            assert_eq!(editor.selection_pointer_button(), None);
        }
    }

    #[test]
    fn right_selection_never_steals_creation_or_erasing() {
        let (mut editor, document, _) = fixture();
        editor
            .process_input(
                &document,
                pointer(
                    PointerEventType::Down,
                    Point::new(100.0, 100.0),
                    PointerButton::Primary,
                    Modifiers::default(),
                ),
            )
            .unwrap();
        assert_eq!(
            editor.hit_quick_selection_at(
                &document,
                Point::new(0.0, 0.0),
                PointerButton::Secondary
            ),
            None
        );
        assert!(
            !right(&mut editor, &document, PointerEventType::Down, 100.0, 100.0)
                .interaction
                .consumed
        );
        editor.set_active_tool(ActiveTool::Eraser).unwrap();
        editor
            .process_input(
                &document,
                pointer(
                    PointerEventType::Down,
                    Point::new(100.0, 100.0),
                    PointerButton::Primary,
                    Modifiers::default(),
                ),
            )
            .unwrap();
        assert_eq!(
            editor.hit_quick_selection_at(
                &document,
                Point::new(0.0, 0.0),
                PointerButton::Secondary
            ),
            None
        );
        editor.set_active_tool(ActiveTool::AutoFilter).unwrap();
        editor
            .process_input(
                &document,
                pointer(
                    PointerEventType::Down,
                    Point::new(100.0, 100.0),
                    PointerButton::Primary,
                    Modifiers::default(),
                ),
            )
            .unwrap();
        assert!(!editor.can_start_quick_selection());
    }

    #[test]
    fn right_selection_moves_remaining_element_types_and_does_not_edit_handles() {
        use snow_draw_engine_document::{FilterData, PenFilterData};
        for tool in [
            ActiveTool::RectangleHighlight,
            ActiveTool::Spotlight,
            ActiveTool::RectangleFilter,
            ActiveTool::PenFilter,
            ActiveTool::Text,
            ActiveTool::SerialNumber,
            ActiveTool::Line,
        ] {
            let (mut editor, mut document, _) = fixture();
            let id = document.allocate_element_id();
            let mut tx = Transaction::new("matching element");
            match tool {
                ActiveTool::RectangleHighlight | ActiveTool::Spotlight => {
                    let mut rect = rectangle(Point::new(0.0, 0.0));
                    rect.rectangle_kind = if tool == ActiveTool::Spotlight {
                        rect.fill.a = 0;
                        RectangleElementKind::Spotlight
                    } else {
                        RectangleElementKind::RectangleHighlight
                    };
                    tx.insert_rectangle(id, ElementMeta::default(), rect)
                }
                ActiveTool::RectangleFilter => tx.insert_filter(
                    id,
                    ElementMeta::default(),
                    FilterData {
                        width: 40.0,
                        height: 40.0,
                        ..Default::default()
                    },
                ),
                ActiveTool::PenFilter => tx.insert_pen_filter(
                    id,
                    ElementMeta::default(),
                    PenFilterData::from_global_points(
                        &[Point::new(-20.0, 0.0), Point::new(20.0, 0.0)],
                        snow_draw_engine_document::CanvasFilterType::Mosaic,
                        0.5,
                        20.0,
                        1.0,
                    )
                    .unwrap(),
                ),
                ActiveTool::Text => tx.insert_text(
                    id,
                    ElementMeta::default(),
                    TextData {
                        text: "select text".into(),
                        auto_resize: false,
                        layout: TextLayoutSize::new(40.0, 30.0),
                        ..TextData::default()
                    },
                ),
                ActiveTool::SerialNumber => {
                    tx.insert_serial_number(id, ElementMeta::default(), SerialNumberData::default())
                }
                ActiveTool::Line => tx.insert_arrow(
                    id,
                    ElementMeta::default(),
                    ArrowData::from_global_points(
                        &[Point::new(-20.0, 0.0), Point::new(20.0, 0.0)],
                        ColorRgba8 {
                            a: 255,
                            ..Default::default()
                        },
                        2.0,
                        StrokeStyle::Solid,
                        snow_draw_engine_core::arrow::ArrowType::Straight,
                        None,
                        None,
                    )
                    .unwrap()
                    .into_line(ColorRgba8::default(), FillStyle::Solid),
                ),
                _ => unreachable!(),
            };
            document.apply_transaction(tx).unwrap();
            editor.set_active_tool(tool).unwrap();
            assert_eq!(
                editor.hit_quick_selection_at(
                    &document,
                    Point::new(0.0, 0.0),
                    PointerButton::Secondary
                ),
                Some(id),
                "{tool:?}"
            );
            right(&mut editor, &document, PointerEventType::Down, 100.0, 100.0);
            assert_eq!(editor.selected_ids(), vec![id]);
            right(&mut editor, &document, PointerEventType::Move, 110.0, 120.0);
            let up = right(&mut editor, &document, PointerEventType::Up, 110.0, 120.0);
            assert!(
                matches!(up.command, Some(EditorCommand::ApplyTransaction(_))),
                "{tool:?}"
            );
            assert!(editor.state.pending_text_edit.is_none());
        }
        let (mut editor, mut document, id) = fixture();
        editor.select_element(&document, id).unwrap();
        let bounds = editor.selection_bounds_snapshot(&document).unwrap();
        let handle = selection_rotation_handle_center(&bounds, 0.0, 1.0);
        let miss = right(
            &mut editor,
            &document,
            PointerEventType::Down,
            100.0 + handle.x,
            100.0 + handle.y,
        );
        assert!(!miss.interaction.consumed);
        assert!(matches!(editor.state.interaction, InteractionState::Idle));
        right(&mut editor, &document, PointerEventType::Down, 120.0, 120.0);
        right(&mut editor, &document, PointerEventType::Move, 140.0, 150.0);
        let up = right(&mut editor, &document, PointerEventType::Up, 140.0, 150.0);
        let Some(EditorCommand::ApplyTransaction(command)) = up.command else {
            panic!("body move at resize handle expected")
        };
        document.apply_transaction(command.transaction).unwrap();
        let rect = document.rectangle(id).unwrap();
        assert_eq!(rect.center, Point::new(20.0, 30.0));
        assert_eq!((rect.width, rect.height), (40.0, 40.0));
    }

    #[test]
    fn right_selection_uses_specialized_tool_scopes_without_deleting_or_creating() {
        let (mut editor, mut document, id) = fixture();
        let filter = document.allocate_element_id();
        let mut tx = Transaction::new("filter");
        tx.insert_filter(
            filter,
            ElementMeta::default(),
            snow_draw_engine_document::FilterData {
                width: 30.0,
                height: 30.0,
                ..Default::default()
            },
        );
        document.apply_transaction(tx).unwrap();
        for (tool, expected) in [
            (ActiveTool::Eraser, Some(filter)),
            (ActiveTool::AutoFilter, Some(filter)),
            (ActiveTool::BrushEraser, None),
            (ActiveTool::RectangleEraser, None),
            (ActiveTool::Watermark, None),
        ] {
            editor.set_active_tool(tool).unwrap();
            assert_eq!(
                editor.hit_quick_selection_at(
                    &document,
                    Point::new(0.0, 0.0),
                    PointerButton::Secondary
                ),
                expected
            );
            let down = right(&mut editor, &document, PointerEventType::Down, 100.0, 100.0);
            assert_eq!(down.interaction.consumed, expected.is_some());
            let moved = right(&mut editor, &document, PointerEventType::Move, 120.0, 120.0);
            assert_eq!(moved.interaction.consumed, expected.is_some());
            let up = right(&mut editor, &document, PointerEventType::Up, 120.0, 120.0);
            if let Some(expected_id) = expected {
                let Some(EditorCommand::ApplyTransaction(command)) = up.command else {
                    panic!("{tool:?} must dispatch right dragging to selection")
                };
                assert_eq!(command.transaction.operations().len(), 1);
                assert!(matches!(
                    &command.transaction.operations()[0],
                    snow_draw_engine_document::Operation::UpdateElementData { id, .. }
                        if *id == expected_id
                ));
            } else {
                assert!(up.command.is_none());
            }
        }
        assert!(document.rectangle(id).is_ok());
    }

    #[test]
    fn right_arrow_label_selects_owner_and_body_drag_moves_whole_arrow() {
        let (mut editor, mut document, _) = fixture();
        let arrow_id = document.allocate_element_id();
        let text_id = document.allocate_element_id();
        let mut arrow = ArrowData::from_global_points(
            &[Point::new(-40.0, 0.0), Point::new(40.0, 0.0)],
            ColorRgba8 {
                a: 255,
                ..Default::default()
            },
            2.0,
            StrokeStyle::Solid,
            snow_draw_engine_core::arrow::ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        arrow.text_element_id = Some(text_id);
        let mut tx = Transaction::new("labelled arrow");
        tx.insert_arrow(arrow_id, ElementMeta::default(), arrow);
        tx.insert_text(
            text_id,
            ElementMeta::default(),
            TextData {
                text: "label".into(),
                auto_resize: false,
                layout: TextLayoutSize::new(60.0, 30.0),
                ..TextData::default()
            },
        );
        document.apply_transaction(tx).unwrap();
        editor.set_active_tool(ActiveTool::Arrow).unwrap();
        assert_eq!(
            editor.hit_quick_selection_at(
                &document,
                Point::new(0.0, 0.0),
                PointerButton::Secondary
            ),
            Some(arrow_id)
        );
        editor.select_element(&document, arrow_id).unwrap();
        right(&mut editor, &document, PointerEventType::Down, 80.0, 100.0);
        assert!(matches!(
            editor.state.interaction,
            InteractionState::PendingSelectionMove(_)
        ));
        right(&mut editor, &document, PointerEventType::Move, 80.0, 120.0);
        let up = right(&mut editor, &document, PointerEventType::Up, 80.0, 120.0);
        let Some(EditorCommand::ApplyTransaction(command)) = up.command else {
            panic!("arrow move expected")
        };
        document.apply_transaction(command.transaction).unwrap();
        let points = document.arrow(arrow_id).unwrap().global_points();
        assert_eq!(
            points,
            vec![Point::new(-40.0, 20.0), Point::new(40.0, 20.0)]
        );
        right(&mut editor, &document, PointerEventType::Down, 60.0, 120.0);
        right(&mut editor, &document, PointerEventType::Move, 80.0, 150.0);
        let up = right(&mut editor, &document, PointerEventType::Up, 80.0, 150.0);
        let Some(EditorCommand::ApplyTransaction(command)) = up.command else {
            panic!("body move at arrow endpoint expected")
        };
        document.apply_transaction(command.transaction).unwrap();
        assert_eq!(
            document.arrow(arrow_id).unwrap().global_points(),
            vec![Point::new(-20.0, 50.0), Point::new(60.0, 50.0)]
        );
    }
}

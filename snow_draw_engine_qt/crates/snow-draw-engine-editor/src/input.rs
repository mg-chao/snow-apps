use super::*;

mod hit_resolution;
mod non_pointer;
mod pointer_idle;
mod pointer_intent;
mod pointer_selection;
mod policy;

impl Editor {
    pub fn process_input(
        &mut self,
        document: &DocumentModel,
        event: InputEvent,
    ) -> Result<EditorUpdate, ErrorCode> {
        self.pending_command = None;
        let interaction = match event {
            InputEvent::Pointer(pointer) => self.process_pointer_event(document, pointer),
            InputEvent::Wheel(wheel) => self.process_wheel_event(document, wheel),
            InputEvent::Key(key) => self.process_key_event(document, key),
            InputEvent::FocusLost => self.handle_focus_lost(document),
        }?;
        Ok(EditorUpdate {
            interaction,
            command: self.pending_command.take(),
        })
    }

    fn process_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        if self.state.active_tool == ActiveTool::Eraser {
            return self.process_eraser_pointer_event(document, event);
        }
        let uses_stroke_cursor = self.state.active_tool.uses_stroke_cursor();
        let mut output = match &self.state.interaction {
            InteractionState::CreatingRectangle(_) => {
                self.process_rectangle_creation_pointer_event(document, event)
            }
            InteractionState::CreatingPenHighlight(_) => {
                self.process_pen_highlight_creation_pointer_event(document, event)
            }
            InteractionState::CreatingArrow(_) => {
                self.process_arrow_creation_pointer_event(document, event)
            }
            InteractionState::CreatingFreeDraw(_) => {
                self.process_free_draw_creation_pointer_event(document, event)
            }
            InteractionState::CreatingPenFilter(_) => {
                self.process_pen_filter_creation_pointer_event(document, event)
            }
            InteractionState::CreatingSerialNumber(_) => {
                self.process_serial_number_creation_pointer_event(document, event)
            }
            InteractionState::MarqueeSelection(_)
            | InteractionState::PendingSelectionMove(_)
            | InteractionState::PendingArrowMove(_)
            | InteractionState::EditingSelection(_)
            | InteractionState::EditingArrow(_) => {
                self.process_selection_pointer_event(document, event)
            }
            InteractionState::Idle => self.process_idle_pointer_event(document, event),
        }?;

        if uses_stroke_cursor {
            self.apply_stroke_cursor_priority(event, &mut output);
        }
        Ok(output)
    }

    fn apply_stroke_cursor_priority(
        &mut self,
        event: PointerEvent,
        output: &mut InteractionOutput,
    ) {
        if event.event_type == PointerEventType::Leave {
            self.clear_stroke_cursor_state();
            output.cursor = CursorCommand::Set(CursorStyle::Default);
            return;
        }

        if !matches!(
            self.state.interaction,
            InteractionState::Idle
                | InteractionState::CreatingFreeDraw(_)
                | InteractionState::CreatingPenHighlight(_)
                | InteractionState::CreatingPenFilter(_)
        ) {
            self.clear_stroke_cursor_state();
            return;
        }

        match output.cursor {
            CursorCommand::Set(cursor) if cursor == self.tool_policy().default_cursor => {
                self.update_stroke_cursor(event);
                if self.state.stroke_cursor_canvas_position.is_some() {
                    output.cursor = CursorCommand::Set(CursorStyle::Hidden);
                }
            }
            CursorCommand::Set(_) => self.clear_stroke_cursor_state(),
            CursorCommand::NoChange if self.state.stroke_cursor_canvas_position.is_some() => {
                self.update_stroke_cursor(event);
                output.cursor = CursorCommand::Set(CursorStyle::Hidden);
            }
            CursorCommand::NoChange => {}
        }
    }

    pub(crate) fn clear_stroke_cursor_state(&mut self) {
        if self.state.stroke_cursor_canvas_position.take().is_some() {
            self.bump_overlay_state_revision();
        }
    }

    fn update_stroke_cursor(&mut self, event: PointerEvent) {
        match event.event_type {
            PointerEventType::Enter
            | PointerEventType::Move
            | PointerEventType::Down
            | PointerEventType::Up
            | PointerEventType::DoubleClick => {
                let position = view_to_canvas(event.position, &self.camera(), self.surface_size());
                if self.state.stroke_cursor_canvas_position != Some(position) {
                    self.state.stroke_cursor_canvas_position = Some(position);
                    self.bump_overlay_state_revision();
                }
            }
            PointerEventType::Leave => self.clear_stroke_cursor_state(),
            PointerEventType::Cancel => {}
        }
    }

    fn process_selection_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        match event.event_type {
            PointerEventType::Down => self.handle_select_pointer_down(document, event),
            PointerEventType::DoubleClick => self.handle_line_double_click(document, event),
            PointerEventType::Move => self.handle_select_pointer_move(document, event),
            PointerEventType::Up => self.handle_select_pointer_up(document, event),
            PointerEventType::Cancel => self.handle_select_pointer_cancel(document, event),
            PointerEventType::Enter => self.handle_select_pointer_hover(document, event),
            PointerEventType::Leave => {
                self.set_hovered_element(None);
                Ok(InteractionOutput {
                    consumed: false,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Default),
                })
            }
        }
    }

    fn process_idle_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        match event.event_type {
            PointerEventType::Down => self.handle_idle_pointer_down(document, event),
            PointerEventType::DoubleClick => self.handle_line_double_click(document, event),
            PointerEventType::Move | PointerEventType::Enter => {
                self.handle_idle_pointer_hover(document, event)
            }
            PointerEventType::Up | PointerEventType::Cancel => Ok(InteractionOutput::default()),
            PointerEventType::Leave => {
                self.set_hovered_element(None);
                Ok(InteractionOutput {
                    consumed: false,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Default),
                })
            }
        }
    }

    fn handle_line_double_click(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        if event.button != Some(PointerButton::Primary) {
            return Ok(InteractionOutput::default());
        }
        let Some((arrow_id, arrow)) = self.selected_single_arrow_snapshot(document) else {
            return Ok(InteractionOutput::default());
        };
        if !arrow.is_line() {
            return Ok(InteractionOutput::default());
        }
        let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let Some(ArrowHitTarget::Point(point_index)) =
            self.arrow_hit_target(document, arrow_id, &arrow, canvas_point)
        else {
            return Ok(InteractionOutput::default());
        };
        let Some(updated) = remove_linear_arrow_point(&arrow, point_index) else {
            return Ok(InteractionOutput::default());
        };
        let mut transaction = Transaction::new("delete line point");
        transaction.update_arrow(arrow_id, updated);
        self.queue_command(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::new(transaction),
        ));
        Ok(InteractionOutput {
            consumed: true,
            capture: PointerCaptureCommand::NoChange,
            cursor: CursorCommand::Set(CursorStyle::Default),
        })
    }
}

#[cfg(test)]
mod double_click_tests {
    use super::*;
    use snow_draw_engine_core::{
        ColorRgba8, EngineConfig,
        arrow::{StrokeStyle, ArrowType},
    };
    use snow_draw_engine_document::{ElementMeta, FillStyle};
    use snow_draw_engine_interaction::{InputEvent, Modifiers, PointerButtons, PointerDevice};

    #[test]
    fn double_clicking_an_interior_line_handle_queues_one_point_deletion() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let line = ArrowData::from_global_points(
            &[
                Point::new(-60.0, 0.0),
                Point::new(0.0, 40.0),
                Point::new(60.0, 0.0),
            ],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Curve,
            None,
            None,
        )
        .unwrap()
        .into_line(ColorRgba8::default(), FillStyle::Solid);
        let mut insert = Transaction::new("insert line");
        insert.insert_arrow(id, ElementMeta::default(), line);
        document.apply_transaction(insert).unwrap();

        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.select_element(&document, id).unwrap();
        let update = editor
            .process_input(
                &document,
                InputEvent::Pointer(PointerEvent {
                    pointer_id: 1,
                    event_type: PointerEventType::DoubleClick,
                    device: PointerDevice::Mouse,
                    position: Point::new(100.0, 140.0),
                    button: Some(PointerButton::Primary),
                    buttons: PointerButtons(PointerButtons::PRIMARY),
                    modifiers: Modifiers::default(),
                }),
            )
            .unwrap();
        let Some(EditorCommand::ApplyTransaction(command)) = update.command else {
            panic!("double-click should queue one line point update");
        };

        document.apply_transaction(command.transaction).unwrap();

        assert_eq!(document.arrow(id).unwrap().global_points().len(), 2);
    }
}

#[cfg(test)]
mod stroke_cursor_tests {
    use super::*;
    use snow_draw_engine_core::{
        EngineConfig, PathSegmentMode,
        arrow::{StrokeStyle, ArrowType},
    };
    use snow_draw_engine_document::{
        ElementMeta, FillStyle, FreeDrawData, FreeDrawStyle,
    };
    use snow_draw_engine_interaction::{InputEvent, Modifiers, PointerButtons, PointerDevice};

    fn pointer(event_type: PointerEventType, position: Point<f64>) -> InputEvent {
        InputEvent::Pointer(PointerEvent {
            pointer_id: 1,
            event_type,
            device: PointerDevice::Mouse,
            position,
            button: None,
            buttons: PointerButtons::default(),
            modifiers: Modifiers::default(),
        })
    }

    fn primary_pointer_down(position: Point<f64>) -> InputEvent {
        InputEvent::Pointer(PointerEvent {
            pointer_id: 1,
            event_type: PointerEventType::Down,
            device: PointerDevice::Mouse,
            position,
            button: Some(PointerButton::Primary),
            buttons: PointerButtons(PointerButtons::PRIMARY),
            modifiers: Modifiers::default(),
        })
    }

    #[test]
    fn stroke_tools_use_a_hidden_cursor_with_the_active_stroke_width() {
        let document = DocumentModel::new();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();

        for (tool, width) in [
            (ActiveTool::FreeDraw, 17.0),
            (ActiveTool::PenHighlight, 19.0),
            (ActiveTool::PenFilter, 23.0),
        ] {
            editor.set_active_tool(tool).unwrap();
            match tool {
                ActiveTool::FreeDraw | ActiveTool::PenHighlight => {
                    let mut style = editor.shape_style(&document);
                    style.stroke_width = width;
                    editor
                        .set_shape_style_patch(
                            &document,
                            ShapeStylePatch {
                                kind: if tool == ActiveTool::FreeDraw {
                                    ShapeKind::FreeDraw
                                } else {
                                    ShapeKind::PenHighlight
                                },
                                style,
                                properties: SHAPE_STYLE_PROPERTY_STROKE_WIDTH,
                            },
                        )
                        .unwrap();
                }
                ActiveTool::PenFilter => {
                    let mut style = editor.filter_style(&document);
                    style.stroke_width = width;
                    editor
                        .set_filter_style(&document, style, FILTER_STYLE_PROPERTY_STROKE_WIDTH)
                        .unwrap();
                }
                _ => unreachable!(),
            }

            let position = Point::new(120.0, 80.0);
            let enter = editor
                .process_input(&document, pointer(PointerEventType::Enter, position))
                .unwrap();
            assert_eq!(
                enter.interaction.cursor,
                CursorCommand::Set(CursorStyle::Hidden)
            );
            assert_eq!(
                editor.presentation_state(&document).stroke_cursor,
                Some(EditorStrokeCursor {
                    position: Point::new(20.0, -20.0),
                    stroke_width: width,
                })
            );

            let move_update = editor
                .process_input(
                    &document,
                    pointer(PointerEventType::Move, Point::new(140.0, 90.0)),
                )
                .unwrap();
            assert_eq!(
                move_update.interaction.cursor,
                CursorCommand::Set(CursorStyle::Hidden)
            );

            let leave = editor
                .process_input(&document, pointer(PointerEventType::Leave, position))
                .unwrap();
            assert_eq!(
                leave.interaction.cursor,
                CursorCommand::Set(CursorStyle::Default)
            );
            assert_eq!(editor.presentation_state(&document).stroke_cursor, None);
        }
    }

    #[test]
    fn active_stroke_width_changes_refresh_the_visible_cursor() {
        let document = DocumentModel::new();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::FreeDraw).unwrap();
        editor
            .process_input(
                &document,
                pointer(PointerEventType::Enter, Point::new(100.0, 100.0)),
            )
            .unwrap();
        let previous_revision = editor.overlay_input_revision();

        let mut style = editor.shape_style(&document);
        style.stroke_width = 26.0;
        editor
            .set_shape_style_patch(
                &document,
                ShapeStylePatch {
                    kind: ShapeKind::FreeDraw,
                    style,
                    properties: SHAPE_STYLE_PROPERTY_STROKE_WIDTH,
                },
            )
            .unwrap();

        assert!(editor.overlay_input_revision() > previous_revision);
        assert_eq!(
            editor
                .presentation_state(&document)
                .stroke_cursor
                .expect("cursor should remain visible")
                .stroke_width,
            26.0
        );
    }

    #[test]
    fn free_draw_control_points_take_priority_over_the_stroke_cursor() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let free_draw = FreeDrawData::from_global_vertices(
            &[Point::new(-40.0, -20.0), Point::new(40.0, 20.0)],
            vec![PathSegmentMode::Curve],
            false,
            FreeDrawStyle {
                stroke: Default::default(),
                stroke_width: 12.0,
                stroke_style: StrokeStyle::Solid,
                fill: Default::default(),
                fill_style: FillStyle::Solid,
                opacity: 1.0,
            },
        )
        .unwrap();
        let mut transaction = Transaction::new("free draw cursor priority");
        transaction.insert_free_draw(id, ElementMeta::default(), free_draw);
        document.apply_transaction(transaction).unwrap();

        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::FreeDraw).unwrap();
        editor.select_element(&document, id).unwrap();

        let bounds = editor.selection_bounds_snapshot(&document).unwrap();
        let selected_elements = editor.selected_document_elements_snapshot(&document);
        let selected_arrows = editor.selected_arrows_snapshot(&document);
        let frame_padding = editor.selection_frame_padding_for_selected_members(
            &document,
            &selected_elements,
            &selected_arrows,
        );
        let handle_position = selection_resize_handle_center(
            &bounds,
            frame_padding,
            selection_corner_handle_outset_for_members(
                editor.camera().zoom,
                selected_elements.len(),
                selected_arrows.len(),
            ),
            RectCorner::TopLeft,
        );
        let handle_view_position = Point::new(
            handle_position.x + editor.surface_size().width as f64 / 2.0,
            handle_position.y + editor.surface_size().height as f64 / 2.0,
        );

        let handle_hover = editor
            .process_input(
                &document,
                pointer(PointerEventType::Move, handle_view_position),
            )
            .unwrap();
        assert_eq!(
            handle_hover.interaction.cursor,
            CursorCommand::Set(CursorStyle::ResizeNwSe)
        );
        assert_eq!(editor.presentation_state(&document).stroke_cursor, None);

        let empty_canvas_hover = editor
            .process_input(
                &document,
                pointer(PointerEventType::Move, Point::new(190.0, 190.0)),
            )
            .unwrap();
        assert_eq!(
            empty_canvas_hover.interaction.cursor,
            CursorCommand::Set(CursorStyle::Hidden)
        );
        assert!(editor.presentation_state(&document).stroke_cursor.is_some());
    }

    #[test]
    fn active_control_point_cursor_wins_when_it_matches_the_tool_default() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let pen_highlight = ArrowData::from_global_points(
            &[Point::new(-40.0, 0.0), Point::new(40.0, 0.0)],
            Default::default(),
            12.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap()
        .into_pen_highlight();
        let mut transaction = Transaction::new("pen highlight cursor priority");
        transaction.insert_arrow(id, ElementMeta::default(), pen_highlight);
        document.apply_transaction(transaction).unwrap();

        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::PenHighlight).unwrap();
        editor.select_element(&document, id).unwrap();
        let endpoint_view_position = Point::new(60.0, 100.0);

        let hover = editor
            .process_input(
                &document,
                pointer(PointerEventType::Move, endpoint_view_position),
            )
            .unwrap();
        assert_eq!(
            hover.interaction.cursor,
            CursorCommand::Set(CursorStyle::Grab)
        );
        assert_eq!(editor.presentation_state(&document).stroke_cursor, None);

        let down = editor
            .process_input(&document, primary_pointer_down(endpoint_view_position))
            .unwrap();
        assert_eq!(
            down.interaction.cursor,
            CursorCommand::Set(CursorStyle::Crosshair)
        );
        assert_eq!(editor.presentation_state(&document).stroke_cursor, None);
        assert!(matches!(
            editor.state.interaction,
            InteractionState::EditingArrow(_)
        ));
    }
}

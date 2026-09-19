use super::*;

impl Editor {
    pub(super) fn handle_select_pointer_down(
        &mut self,
        _document: &DocumentModel,
        _event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        Ok(InteractionOutput::default())
    }

    pub(super) fn handle_select_pointer_move(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        if let InteractionState::MarqueeSelection(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }

            let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
            self.set_marquee(selection_marquee_rectangle(
                state.start_canvas_position,
                canvas_point,
                self.camera().zoom,
            ));
            return Ok(InteractionOutput {
                consumed: true,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(CursorStyle::Default),
            });
        }

        if let InteractionState::PendingSelectionMove(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }
            let state = state.clone();

            let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
            if pointer_drag_distance(state.start_view_position, event.position)
                < POINTER_DRAG_THRESHOLD
            {
                return Ok(InteractionOutput {
                    consumed: true,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Move),
                });
            }

            self.state.interaction = InteractionState::EditingSelection(
                self.begin_selection_edit_state(BeginSelectionEditRequest {
                    pointer_id: event.pointer_id,
                    original_elements: state.original_elements,
                    original_arrows: state.original_arrows,
                    original_bounds: state.original_bounds,
                    target: SelectionHitTarget::Move,
                    canvas_point: state.start_canvas_position,
                    frame_padding_override: None,
                }),
            );
            if let InteractionState::EditingSelection(active) = &mut self.state.interaction {
                active.duplicate = state.duplicate;
            }
            self.update_selection_edit_preview(document, event);
            let cursor = match &self.state.interaction {
                InteractionState::EditingSelection(active) => self
                    .active_cursor_for_selection_state(
                        document,
                        active,
                        canvas_point,
                        event.modifiers,
                    ),
                _ => CursorStyle::Move,
            };
            return Ok(InteractionOutput {
                consumed: true,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(cursor),
            });
        }

        if let InteractionState::PendingArrowMove(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }
            let state = state.clone();

            if pointer_drag_distance(state.start_view_position, event.position)
                < POINTER_DRAG_THRESHOLD
            {
                return Ok(InteractionOutput {
                    consumed: true,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Move),
                });
            }

            self.state.interaction = InteractionState::EditingArrow(EditArrowState {
                pointer_id: event.pointer_id,
                arrow_id: state.arrow_id,
                original_arrow: state.original_arrow.clone(),
                preview_arrow: state.original_arrow,
                mode: ArrowEditMode::Move,
                start_canvas_position: state.start_canvas_position,
                drag_offset: Point::new(0.0, 0.0),
                suggested_binding: None,
            });
            self.update_arrow_edit_preview(document, event);
            return Ok(InteractionOutput {
                consumed: true,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(CursorStyle::Move),
            });
        }

        if let InteractionState::EditingSelection(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }

            self.update_selection_edit_preview(document, event);
            let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
            let cursor = match &self.state.interaction {
                InteractionState::EditingSelection(active) => self
                    .active_cursor_for_selection_state(
                        document,
                        active,
                        canvas_point,
                        event.modifiers,
                    ),
                _ => CursorStyle::Default,
            };
            return Ok(InteractionOutput {
                consumed: true,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(cursor),
            });
        }

        if let InteractionState::EditingArrow(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }

            let mode = state.mode;
            self.update_arrow_edit_preview(document, event);
            return Ok(InteractionOutput {
                consumed: true,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(active_cursor_for_arrow_mode(mode)),
            });
        }

        self.handle_select_pointer_hover(document, event)
    }

    pub(super) fn handle_select_pointer_up(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        if let InteractionState::MarqueeSelection(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }
            let state = state.clone();

            let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
            self.state.interaction = InteractionState::Idle;
            self.clear_transient_visuals();
            self.commit_marquee_selection(document, state, canvas_point);

            return Ok(InteractionOutput {
                consumed: true,
                capture: self.release_capture_command(),
                cursor: CursorCommand::Set(self.hover_cursor_for_canvas_point(
                    document,
                    self.tool_policy(),
                    canvas_point,
                )),
            });
        }

        if let InteractionState::PendingSelectionMove(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }
            let _state = state.clone();

            self.state.interaction = InteractionState::Idle;
            let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
            return Ok(InteractionOutput {
                consumed: true,
                capture: self.release_capture_command(),
                cursor: CursorCommand::Set(self.hover_cursor_for_canvas_point(
                    document,
                    self.tool_policy(),
                    canvas_point,
                )),
            });
        }

        if let InteractionState::PendingArrowMove(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }

            self.state.interaction = InteractionState::Idle;
            let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
            return Ok(InteractionOutput {
                consumed: true,
                capture: self.release_capture_command(),
                cursor: CursorCommand::Set(self.hover_cursor_for_canvas_point(
                    document,
                    self.tool_policy(),
                    canvas_point,
                )),
            });
        }

        if let InteractionState::EditingSelection(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }

            self.update_selection_edit_preview(document, event);
            self.commit_selection_edit(document)?;

            let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
            return Ok(InteractionOutput {
                consumed: true,
                capture: self.release_capture_command(),
                cursor: CursorCommand::Set(self.hover_cursor_for_canvas_point(
                    document,
                    self.tool_policy(),
                    canvas_point,
                )),
            });
        }

        let InteractionState::EditingArrow(state) = &self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }

        self.update_arrow_edit_preview(document, event);
        self.commit_arrow_edit(document, event)?;

        let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        Ok(InteractionOutput {
            consumed: true,
            capture: self.release_capture_command(),
            cursor: CursorCommand::Set(self.hover_cursor_for_canvas_point(
                document,
                self.tool_policy(),
                canvas_point,
            )),
        })
    }

    pub(super) fn handle_select_pointer_cancel(
        &mut self,
        _document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        if let InteractionState::MarqueeSelection(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }

            self.cancel_interaction();
            return Ok(InteractionOutput {
                consumed: true,
                capture: self.release_capture_command(),
                cursor: CursorCommand::Set(CursorStyle::Default),
            });
        }

        if let InteractionState::PendingSelectionMove(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }
            let _state = state.clone();

            self.cancel_interaction();
            return Ok(InteractionOutput {
                consumed: true,
                capture: self.release_capture_command(),
                cursor: CursorCommand::Set(CursorStyle::Default),
            });
        }

        if let InteractionState::PendingArrowMove(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }

            self.cancel_interaction();
            return Ok(InteractionOutput {
                consumed: true,
                capture: self.release_capture_command(),
                cursor: CursorCommand::Set(CursorStyle::Default),
            });
        }

        if let InteractionState::EditingSelection(state) = &self.state.interaction {
            if state.pointer_id != event.pointer_id {
                return Ok(InteractionOutput::default());
            }

            self.cancel_interaction();
            return Ok(InteractionOutput {
                consumed: true,
                capture: self.release_capture_command(),
                cursor: CursorCommand::Set(CursorStyle::Default),
            });
        }

        let InteractionState::EditingArrow(state) = &self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }

        self.cancel_interaction();
        Ok(InteractionOutput {
            consumed: true,
            capture: self.release_capture_command(),
            cursor: CursorCommand::Set(CursorStyle::Default),
        })
    }

    pub(super) fn handle_select_pointer_hover(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let policy = self.tool_policy();
        let intent =
            self.resolve_primary_pointer_intent(document, policy, canvas_point, event.modifiers);
        let (cursor, hovered_element) =
            self.hover_feedback_for_primary_pointer_intent(document, policy, intent);
        self.set_hovered_element(hovered_element);
        Ok(InteractionOutput {
            consumed: false,
            capture: PointerCaptureCommand::NoChange,
            cursor: CursorCommand::Set(cursor),
        })
    }
}

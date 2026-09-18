use super::*;

impl Editor {
    pub(crate) fn can_begin_selected_text_edit(
        &self,
        document: &DocumentModel,
        id: ElementId,
        shift_pressed: bool,
    ) -> bool {
        if shift_pressed || !self.state.selection.contains(id) {
            return false;
        }
        if self
            .state
            .selection
            .ids
            .iter()
            .filter(|selected_id| document.text(**selected_id).is_ok())
            .count()
            != 1
        {
            return false;
        }

        match self.state.active_tool {
            ActiveTool::Select | ActiveTool::Text => true,
            ActiveTool::SerialNumber => document.is_text_bound_to_serial_number(id),
            _ => false,
        }
    }

    pub(crate) fn can_begin_selected_text_edit_at(
        &self,
        document: &DocumentModel,
        id: ElementId,
        shift_pressed: bool,
        canvas_point: Point<f64>,
    ) -> bool {
        if !self.can_begin_selected_text_edit(document, id, shift_pressed) {
            return false;
        }
        let Some((text_id, rect)) = self.selected_single_text_rect_snapshot(document) else {
            return false;
        };
        text_id == id
            && point_in_rotated_rect(
                rect.center,
                rect.width,
                rect.height,
                rect.rotation,
                canvas_point,
            )
    }

    pub(crate) fn begin_selection_interaction(
        &mut self,
        request: BeginSelectionInteractionRequest,
    ) -> InteractionOutput {
        self.state.interaction = match request.target {
            SelectionHitTarget::Move => {
                InteractionState::PendingSelectionMove(PendingSelectionMoveState {
                    duplicate: false,
                    pointer_id: request.pointer_id,
                    original_elements: request.original_elements,
                    original_arrows: request.original_arrows,
                    original_bounds: request.original_bounds,
                    start_canvas_position: request.canvas_point,
                    start_view_position: request.start_view_position,
                })
            }
            _ => InteractionState::EditingSelection(self.begin_selection_edit_state(
                BeginSelectionEditRequest {
                    pointer_id: request.pointer_id,
                    original_elements: request.original_elements,
                    original_arrows: request.original_arrows,
                    original_bounds: request.original_bounds,
                    target: request.target,
                    canvas_point: request.canvas_point,
                    frame_padding_override: Some(request.frame_padding),
                },
            )),
        };

        InteractionOutput {
            consumed: true,
            capture: self.capture_command_for_start(request.pointer_id),
            cursor: CursorCommand::Set(active_cursor_for_selection_target(
                request.target,
                request.original_bounds.rotation,
            )),
        }
    }

    fn begin_arrow_interaction(
        &mut self,
        event: PointerEvent,
        arrow_id: ElementId,
        original_arrow: ArrowData,
        target: ArrowHitTarget,
        canvas_point: Point<f64>,
    ) -> InteractionOutput {
        let drag_offset = arrow_target_position(&original_arrow, target)
            .map(|position| Point::new(position.x - canvas_point.x, position.y - canvas_point.y))
            .unwrap_or(Point::new(0.0, 0.0));
        self.state.interaction = match target {
            ArrowHitTarget::Move => InteractionState::PendingArrowMove(PendingArrowMoveState {
                pointer_id: event.pointer_id,
                arrow_id,
                original_arrow,
                start_canvas_position: canvas_point,
                start_view_position: event.position,
            }),
            ArrowHitTarget::Endpoint(edge) => InteractionState::EditingArrow(EditArrowState {
                pointer_id: event.pointer_id,
                arrow_id,
                preview_arrow: original_arrow.clone(),
                original_arrow,
                mode: ArrowEditMode::Endpoint(edge),
                start_canvas_position: canvas_point,
                drag_offset,
            }),
            ArrowHitTarget::Point(index) => InteractionState::EditingArrow(EditArrowState {
                pointer_id: event.pointer_id,
                arrow_id,
                preview_arrow: original_arrow.clone(),
                original_arrow,
                mode: ArrowEditMode::Point(index),
                start_canvas_position: canvas_point,
                drag_offset,
            }),
            ArrowHitTarget::FocusPoint(edge) => InteractionState::EditingArrow(EditArrowState {
                pointer_id: event.pointer_id,
                arrow_id,
                preview_arrow: original_arrow.clone(),
                original_arrow,
                mode: ArrowEditMode::FocusPoint(edge),
                start_canvas_position: canvas_point,
                drag_offset,
            }),
            ArrowHitTarget::Segment(index) => InteractionState::EditingArrow(EditArrowState {
                pointer_id: event.pointer_id,
                arrow_id,
                preview_arrow: original_arrow.clone(),
                original_arrow,
                mode: ArrowEditMode::Segment(index),
                start_canvas_position: canvas_point,
                drag_offset,
            }),
        };

        InteractionOutput {
            consumed: true,
            capture: self.capture_command_for_start(event.pointer_id),
            cursor: CursorCommand::Set(active_cursor_for_arrow_target(target)),
        }
    }

    pub(crate) fn begin_current_selection_interaction(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
        target: SelectionHitTarget,
        canvas_point: Point<f64>,
    ) -> InteractionOutput {
        let original_elements = self.selected_elements_snapshot(document);
        let original_arrows = self.selected_arrows_snapshot(document);
        let Some(bounds) = self.selection_bounds_snapshot(document) else {
            return InteractionOutput::default();
        };
        if original_elements.is_empty() && original_arrows.is_empty() {
            return InteractionOutput::default();
        }
        let frame_padding = self.selection_frame_padding_for_selected_members(
            document,
            &original_elements,
            &original_arrows,
        );
        self.begin_selection_interaction(BeginSelectionInteractionRequest {
            pointer_id: event.pointer_id,
            start_view_position: event.position,
            original_elements,
            original_arrows,
            original_bounds: bounds,
            target,
            canvas_point,
            frame_padding,
        })
    }

    fn begin_element_selection_move(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
        id: ElementId,
        canvas_point: Point<f64>,
    ) -> InteractionOutput {
        if !self.state.selection.contains(id) || self.state.selection.ids.len() != 1 {
            self.set_selection_state_with_document(Some(document), vec![id], Some(id));
        }

        let original_elements = self.selected_elements_snapshot(document);
        let original_arrows = self.selected_arrows_snapshot(document);
        let Some(bounds) = selection_bounds_from_selection(&original_elements, &original_arrows)
        else {
            return InteractionOutput::default();
        };
        let frame_padding = self.selection_frame_padding_for_selected_members(
            document,
            &original_elements,
            &original_arrows,
        );
        self.begin_selection_interaction(BeginSelectionInteractionRequest {
            pointer_id: event.pointer_id,
            start_view_position: event.position,
            original_elements,
            original_arrows,
            original_bounds: bounds,
            target: SelectionHitTarget::Move,
            canvas_point,
            frame_padding,
        })
    }

    fn begin_selected_arrow_interaction(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
        target: ArrowHitTarget,
        canvas_point: Point<f64>,
    ) -> InteractionOutput {
        let Some((arrow_id, arrow)) = self.selected_single_arrow_snapshot(document) else {
            return InteractionOutput::default();
        };
        self.begin_arrow_interaction(event, arrow_id, arrow, target, canvas_point)
    }

    fn begin_arrow_element_interaction(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
        id: ElementId,
        canvas_point: Point<f64>,
    ) -> InteractionOutput {
        if !self.state.selection.contains(id) || self.state.selection.ids.len() != 1 {
            self.set_selection_state_with_document(Some(document), vec![id], Some(id));
        }

        let Some(arrow) = self.arrow_snapshot(document, id) else {
            return InteractionOutput::default();
        };
        self.begin_arrow_interaction(event, id, arrow, ArrowHitTarget::Move, canvas_point)
    }

    fn handle_empty_canvas_pointer_down(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
        policy: ToolPolicy,
        canvas_point: Point<f64>,
    ) -> Result<InteractionOutput, ErrorCode> {
        match policy.empty_canvas_action {
            ToolEmptyCanvasAction::Configure => Ok(InteractionOutput {
                consumed: true,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(CursorStyle::Default),
            }),
            ToolEmptyCanvasAction::MarqueeSelect => {
                let additive = event.modifiers.shift && policy.allow_shift_toggle;
                let base_selection = self.state.selection.clone();
                if !additive {
                    self.clear_selection();
                }
                self.set_hovered_element(None);
                self.state.interaction =
                    InteractionState::MarqueeSelection(MarqueeSelectionState {
                        pointer_id: event.pointer_id,
                        start_canvas_position: canvas_point,
                        additive,
                        base_selection,
                    });
                self.clear_transient_visuals();
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.capture_command_for_start(event.pointer_id),
                    cursor: CursorCommand::Set(policy.default_cursor),
                })
            }
            ToolEmptyCanvasAction::CreateRectangle => {
                if !self.state.selection.is_empty() {
                    self.clear_selection();
                }
                let start_canvas_position =
                    self.snap_creation_start_position(canvas_point, event.modifiers);
                self.state.interaction =
                    InteractionState::CreatingRectangle(CreateRectangleState {
                        pointer_id: event.pointer_id,
                        start_canvas_position,
                    });
                self.clear_transient_visuals();
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.capture_command_for_start(event.pointer_id),
                    cursor: CursorCommand::Set(policy.default_cursor),
                })
            }
            ToolEmptyCanvasAction::CreateArrow => {
                if !self.state.selection.is_empty() {
                    self.clear_selection();
                }
                let start_canvas_position =
                    self.snap_creation_start_position(canvas_point, event.modifiers);
                self.begin_arrow_creation(event.pointer_id, start_canvas_position, event.position);
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.capture_command_for_start(event.pointer_id),
                    cursor: CursorCommand::Set(policy.default_cursor),
                })
            }
            ToolEmptyCanvasAction::CreateHighlight => {
                if !self.state.selection.is_empty() {
                    self.clear_selection();
                }
                let start_canvas_position =
                    self.snap_creation_start_position(canvas_point, event.modifiers);
                self.state.interaction =
                    InteractionState::CreatingRectangle(CreateRectangleState {
                        pointer_id: event.pointer_id,
                        start_canvas_position,
                    });
                self.clear_transient_visuals();
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.capture_command_for_start(event.pointer_id),
                    cursor: CursorCommand::Set(policy.default_cursor),
                })
            }
            ToolEmptyCanvasAction::CreatePenHighlight => {
                if !self.state.selection.is_empty() {
                    self.clear_selection();
                }
                let start_canvas_position =
                    self.snap_creation_start_position(canvas_point, event.modifiers);
                self.state.interaction =
                    InteractionState::CreatingPenHighlight(CreateRectangleState {
                        pointer_id: event.pointer_id,
                        start_canvas_position,
                    });
                self.clear_transient_visuals();
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.capture_command_for_start(event.pointer_id),
                    cursor: CursorCommand::Set(policy.default_cursor),
                })
            }
            ToolEmptyCanvasAction::CreateFreeDraw => {
                if !self.state.selection.is_empty() {
                    self.clear_selection();
                    return Ok(InteractionOutput {
                        consumed: true,
                        capture: PointerCaptureCommand::NoChange,
                        cursor: CursorCommand::Set(policy.default_cursor),
                    });
                }
                let start_canvas_position =
                    self.snap_creation_start_position(canvas_point, event.modifiers);
                self.begin_free_draw_creation(event.pointer_id, start_canvas_position);
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.capture_command_for_start(event.pointer_id),
                    cursor: CursorCommand::Set(policy.default_cursor),
                })
            }
            ToolEmptyCanvasAction::CreatePenFilter => {
                if !self.state.selection.is_empty() {
                    self.clear_selection();
                }
                self.begin_pen_filter_creation(event.pointer_id, canvas_point);
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.capture_command_for_start(event.pointer_id),
                    cursor: CursorCommand::Set(policy.default_cursor),
                })
            }
            ToolEmptyCanvasAction::CreateText => {
                if !self.state.selection.is_empty() {
                    self.clear_selection();
                    return Ok(InteractionOutput {
                        consumed: true,
                        capture: PointerCaptureCommand::NoChange,
                        cursor: CursorCommand::Set(policy.default_cursor),
                    });
                }
                self.set_hovered_element(None);
                Ok(InteractionOutput {
                    consumed: true,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Text),
                })
            }
            ToolEmptyCanvasAction::CreateSerialNumber => {
                if !self.state.selection.is_empty() {
                    self.clear_selection();
                    return Ok(InteractionOutput {
                        consumed: true,
                        capture: PointerCaptureCommand::NoChange,
                        cursor: CursorCommand::Set(policy.default_cursor),
                    });
                }
                let (center, snap_guides) = self.snap_serial_number_creation_center(
                    document,
                    canvas_point,
                    event.modifiers,
                );
                let preview = self.serial_number_creation_preview(document, center)?;
                self.state.interaction =
                    InteractionState::CreatingSerialNumber(CreateSerialNumberState {
                        pointer_id: event.pointer_id,
                        preview: preview.clone(),
                    });
                self.set_creation_preview(
                    Some(ElementCreationPreview::SerialNumber(preview)),
                    snap_guides,
                );
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.capture_command_for_start(event.pointer_id),
                    cursor: CursorCommand::Set(policy.default_cursor),
                })
            }
        }
    }

    fn try_begin_duplicate_drag(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
        intent: PrimaryPointerIntent,
        canvas_point: Point<f64>,
    ) -> Option<InteractionOutput> {
        if !event.modifiers.alt || self.state.active_text_draft.is_some() {
            return None;
        }

        // Resolve the drag selection before starting the shared copy workflow.
        // Existing selections keep all members; a new hit becomes the selection.
        // Handles, Shift-toggle and empty-canvas intents keep their normal behavior.
        match intent {
            PrimaryPointerIntent::BeginSelectionInteraction {
                target: SelectionHitTarget::Move,
            }
            | PrimaryPointerIntent::BeginSelectedArrowInteraction {
                target: ArrowHitTarget::Move,
            } => {}
            PrimaryPointerIntent::BeginArrowElementInteraction { id }
            | PrimaryPointerIntent::BeginElementSelectionMove { id }
            | PrimaryPointerIntent::TextEditCandidate { id } => {
                if !self.state.selection.contains(id) {
                    self.set_selection_state_with_document(Some(document), vec![id], Some(id));
                }
            }
            _ => return None,
        }

        let output = self.begin_current_selection_interaction(
            document,
            event,
            SelectionHitTarget::Move,
            canvas_point,
        );
        if let InteractionState::PendingSelectionMove(state) = &mut self.state.interaction {
            state.duplicate = true;
        }
        Some(output)
    }

    pub(super) fn handle_idle_pointer_down(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        if event.button != Some(PointerButton::Primary)
            || !matches!(&self.state.interaction, InteractionState::Idle)
        {
            return Ok(InteractionOutput::default());
        }

        let policy = self.tool_policy();
        let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let intent =
            self.resolve_primary_pointer_intent(document, policy, canvas_point, event.modifiers);
        if let Some(output) = self.try_begin_duplicate_drag(document, event, intent, canvas_point) {
            return Ok(output);
        }
        match intent {
            PrimaryPointerIntent::ToggleSelection { id } => {
                self.toggle_selection(document, id);
                Ok(InteractionOutput {
                    consumed: true,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(self.hover_cursor_for_canvas_point(
                        document,
                        policy,
                        canvas_point,
                    )),
                })
            }
            PrimaryPointerIntent::BeginSelectionInteraction { target } => {
                Ok(self.begin_current_selection_interaction(document, event, target, canvas_point))
            }
            PrimaryPointerIntent::BeginSelectedArrowInteraction { target } => {
                Ok(self.begin_selected_arrow_interaction(document, event, target, canvas_point))
            }
            PrimaryPointerIntent::BeginArrowElementInteraction { id } => {
                Ok(self.begin_arrow_element_interaction(document, event, id, canvas_point))
            }
            PrimaryPointerIntent::BeginElementSelectionMove { id } => {
                Ok(self.begin_element_selection_move(document, event, id, canvas_point))
            }
            PrimaryPointerIntent::TextEditCandidate { id } => {
                if !self.state.selection.contains(id) || self.state.selection.ids.len() != 1 {
                    self.set_selection_state_with_document(Some(document), vec![id], Some(id));
                }
                self.set_hovered_element(None);
                Ok(InteractionOutput {
                    consumed: true,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Text),
                })
            }
            PrimaryPointerIntent::EmptyCanvas => {
                self.handle_empty_canvas_pointer_down(document, event, policy, canvas_point)
            }
        }
    }

    pub(super) fn handle_idle_pointer_hover(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let policy = self.tool_policy();
        let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
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

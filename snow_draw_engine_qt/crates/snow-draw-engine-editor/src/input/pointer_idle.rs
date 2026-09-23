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
            ArrowHitTarget::Move | ArrowHitTarget::Label => {
                InteractionState::PendingArrowMove(PendingArrowMoveState {
                    pointer_id: event.pointer_id,
                    arrow_id,
                    original_arrow,
                    label: target == ArrowHitTarget::Label,
                    start_canvas_position: canvas_point,
                    start_view_position: event.position,
                })
            }
            ArrowHitTarget::Endpoint(edge) => InteractionState::EditingArrow(EditArrowState {
                pointer_id: event.pointer_id,
                arrow_id,
                preview_arrow: original_arrow.clone(),
                original_arrow,
                mode: ArrowEditMode::Endpoint(edge),
                start_canvas_position: canvas_point,
                drag_offset,
                suggested_binding: None,
            }),
            ArrowHitTarget::Point(index) => InteractionState::EditingArrow(EditArrowState {
                pointer_id: event.pointer_id,
                arrow_id,
                preview_arrow: original_arrow.clone(),
                original_arrow,
                mode: ArrowEditMode::Point(index),
                start_canvas_position: canvas_point,
                drag_offset,
                suggested_binding: None,
            }),
            ArrowHitTarget::FocusPoint(edge) => InteractionState::EditingArrow(EditArrowState {
                pointer_id: event.pointer_id,
                arrow_id,
                preview_arrow: original_arrow.clone(),
                original_arrow,
                mode: ArrowEditMode::FocusPoint(edge),
                start_canvas_position: canvas_point,
                drag_offset,
                suggested_binding: None,
            }),
            ArrowHitTarget::Segment(index) => InteractionState::EditingArrow(EditArrowState {
                pointer_id: event.pointer_id,
                arrow_id,
                preview_arrow: original_arrow.clone(),
                original_arrow,
                mode: ArrowEditMode::Segment(index),
                start_canvas_position: canvas_point,
                drag_offset,
                suggested_binding: None,
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
        let target = if self.arrow_label_hit(document, id, canvas_point) {
            ArrowHitTarget::Label
        } else {
            ArrowHitTarget::Move
        };
        self.begin_arrow_interaction(event, id, arrow, target, canvas_point)
    }

    fn handle_empty_canvas_pointer_down(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
        policy: ToolPolicy,
        canvas_point: Point<f64>,
    ) -> Result<InteractionOutput, ErrorCode> {
        // A press on blank canvas prioritizes deselecting an existing element
        // selection; the creation workflow only begins once nothing is
        // selected, so the same press never both deselects and starts
        // creating (which would turn a deselect click with slight movement
        // into an accidental element).
        if self.empty_canvas_press_deselects_first() {
            self.clear_selection();
            self.clear_transient_visuals();
            return Ok(InteractionOutput {
                consumed: true,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(policy.default_cursor),
            });
        }
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
                self.begin_pen_filter_creation(event.pointer_id, canvas_point);
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.capture_command_for_start(event.pointer_id),
                    cursor: CursorCommand::Set(policy.default_cursor),
                })
            }
            ToolEmptyCanvasAction::CreateText => {
                self.set_hovered_element(None);
                self.state.pending_new_text_draft = true;
                Ok(InteractionOutput {
                    consumed: true,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Text),
                })
            }
            ToolEmptyCanvasAction::CreateSerialNumber => {
                let (center, snap_guides) = self.snap_serial_number_creation_center(
                    document,
                    canvas_point,
                    event.modifiers,
                );
                let preview = self.serial_number_creation_preview(document, center)?;
                let serial_id = self.queue_serial_number_creation(document, preview)?;
                self.state.interaction =
                    InteractionState::CreatingSerialNumber(CreateSerialNumberState {
                        pointer_id: event.pointer_id,
                        serial_id,
                        start_view_position: event.position,
                        text: None,
                        label_measured: false,
                    });
                self.set_creation_preview(None, snap_guides);
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
            }
            | PrimaryPointerIntent::BeginSelectedArrowInteraction {
                target: ArrowHitTarget::Label,
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
        if let Some(target) = self.resolve_free_draw_endpoint(document, canvas_point) {
            self.begin_free_draw_continuation(document, event, target);
            return Ok(InteractionOutput {
                consumed: true,
                capture: self.capture_command_for_start(event.pointer_id),
                cursor: CursorCommand::Set(CursorStyle::Crosshair),
            });
        }
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
        if self.state.active_tool == ActiveTool::FreeDraw {
            self.state.ui.free_draw_hover_position = Some(event.position);
            self.bump_overlay_state_revision();
            if self
                .resolve_free_draw_endpoint(document, canvas_point)
                .is_some()
            {
                self.state.ui.hovered_element = None;
                return Ok(InteractionOutput {
                    consumed: false,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Crosshair),
                });
            }
        }
        Ok(InteractionOutput {
            consumed: false,
            capture: PointerCaptureCommand::NoChange,
            cursor: CursorCommand::Set(cursor),
        })
    }
}

#[cfg(test)]
mod empty_canvas_selection_tests {
    use super::*;
    use snow_draw_engine_core::{
        ColorRgba8, CornerRadii, EngineConfig,
        arrow::{ArrowType, StrokeStyle},
    };
    use snow_draw_engine_document::{ElementMeta, FillStyle, HighlightShape, RectangleElementKind};
    use snow_draw_engine_interaction::{PointerButtons, PointerDevice};

    fn document_with_rectangle() -> (DocumentModel, ElementId) {
        let mut document = DocumentModel::new();
        let id = document.allocate_element_id();
        let mut transaction = Transaction::new("insert rectangle");
        transaction.insert_rectangle(
            id,
            ElementMeta::default(),
            RectangleData {
                rectangle_kind: RectangleElementKind::Rectangle,
                highlight_shape: HighlightShape::Rectangle,
                center: Point::new(0.0, 0.0),
                width: 40.0,
                height: 40.0,
                rotation: 0.0,
                fill: ColorRgba8 {
                    r: 1,
                    g: 2,
                    b: 3,
                    a: 255,
                },
                fill_style: FillStyle::Solid,
                stroke: ColorRgba8::default(),
                stroke_width: 2.0,
                stroke_style: StrokeStyle::Solid,
                corner_radii: CornerRadii::default(),
                opacity: 1.0,
            },
        );
        document.apply_transaction(transaction).unwrap();
        (document, id)
    }

    fn document_with_arrow() -> (DocumentModel, ElementId) {
        let mut document = DocumentModel::new();
        let arrow = ArrowData::from_global_points(
            &[Point::new(-10.0, 0.0), Point::new(10.0, 0.0)],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        let id = document.allocate_element_id();
        let mut transaction = Transaction::new("insert arrow");
        transaction.insert_arrow(id, ElementMeta::default(), arrow);
        document.apply_transaction(transaction).unwrap();
        (document, id)
    }

    fn editor_with_tool_and_selection(
        document: &DocumentModel,
        id: ElementId,
        tool: ActiveTool,
    ) -> Editor {
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(tool).unwrap();
        editor.set_selection_state_with_document(Some(document), vec![id], Some(id));
        editor
    }

    fn pointer(event_type: PointerEventType, position: Point<f64>) -> InputEvent {
        InputEvent::Pointer(PointerEvent {
            pointer_id: 1,
            event_type,
            device: PointerDevice::Mouse,
            position,
            button: matches!(event_type, PointerEventType::Down | PointerEventType::Up)
                .then_some(PointerButton::Primary),
            buttons: PointerButtons(PointerButtons::PRIMARY),
            modifiers: Modifiers::default(),
        })
    }

    // Blank canvas at view (170, 170); the fixtures keep their elements within
    // canvas (-50, -50)..(50, 50).
    fn blank_canvas_view_point() -> Point<f64> {
        Point::new(170.0, 170.0)
    }

    #[test]
    fn shape_tool_blank_press_deselects_before_creating() {
        let (document, id) = document_with_rectangle();
        let mut editor = editor_with_tool_and_selection(&document, id, ActiveTool::Shape);

        // The first blank-canvas press spends itself on deselecting.
        let update = editor
            .process_input(
                &document,
                pointer(PointerEventType::Down, blank_canvas_view_point()),
            )
            .unwrap();
        assert!(update.interaction.consumed);
        assert!(update.command.is_none());
        assert!(editor.state.selection.ids.is_empty());
        assert!(matches!(editor.state.interaction, InteractionState::Idle));

        editor
            .process_input(
                &document,
                pointer(PointerEventType::Up, blank_canvas_view_point()),
            )
            .unwrap();

        // With nothing selected, the next press-drag-release creates.
        editor
            .process_input(
                &document,
                pointer(PointerEventType::Down, blank_canvas_view_point()),
            )
            .unwrap();
        assert!(matches!(
            editor.state.interaction,
            InteractionState::CreatingRectangle(_)
        ));
        editor
            .process_input(
                &document,
                pointer(PointerEventType::Move, Point::new(190.0, 190.0)),
            )
            .unwrap();
        let update = editor
            .process_input(
                &document,
                pointer(PointerEventType::Up, Point::new(190.0, 190.0)),
            )
            .unwrap();
        let Some(EditorCommand::ApplyTransaction(command)) = update.command else {
            panic!("the drag after deselecting should create a rectangle");
        };
        assert_eq!(command.transaction.label(), "create rectangle");
    }

    #[test]
    fn arrow_tool_blank_press_deselects_before_creating() {
        let (document, id) = document_with_arrow();
        let mut editor = editor_with_tool_and_selection(&document, id, ActiveTool::Arrow);

        let update = editor
            .process_input(
                &document,
                pointer(PointerEventType::Down, blank_canvas_view_point()),
            )
            .unwrap();
        assert!(update.interaction.consumed);
        assert!(update.command.is_none());
        assert!(editor.state.selection.ids.is_empty());
        assert!(matches!(editor.state.interaction, InteractionState::Idle));

        editor
            .process_input(
                &document,
                pointer(PointerEventType::Up, blank_canvas_view_point()),
            )
            .unwrap();
        editor
            .process_input(
                &document,
                pointer(PointerEventType::Down, blank_canvas_view_point()),
            )
            .unwrap();
        assert!(matches!(
            editor.state.interaction,
            InteractionState::CreatingArrow(_)
        ));
    }

    #[test]
    fn select_tool_blank_press_still_starts_marquee_deselection() {
        let (document, id) = document_with_rectangle();
        let mut editor = editor_with_tool_and_selection(&document, id, ActiveTool::Select);

        editor
            .process_input(
                &document,
                pointer(PointerEventType::Down, blank_canvas_view_point()),
            )
            .unwrap();
        assert!(editor.state.selection.ids.is_empty());
        assert!(matches!(
            editor.state.interaction,
            InteractionState::MarqueeSelection(_)
        ));
    }

    #[test]
    fn only_creation_actions_start_creation() {
        assert!(!ToolEmptyCanvasAction::Configure.starts_creation());
        assert!(!ToolEmptyCanvasAction::MarqueeSelect.starts_creation());
        for action in [
            ToolEmptyCanvasAction::CreateRectangle,
            ToolEmptyCanvasAction::CreateArrow,
            ToolEmptyCanvasAction::CreateFreeDraw,
            ToolEmptyCanvasAction::CreateHighlight,
            ToolEmptyCanvasAction::CreatePenHighlight,
            ToolEmptyCanvasAction::CreatePenFilter,
            ToolEmptyCanvasAction::CreateText,
            ToolEmptyCanvasAction::CreateSerialNumber,
        ] {
            assert!(action.starts_creation());
        }
    }

    #[test]
    fn empty_canvas_press_deselects_first_reports_the_shared_policy() {
        let (document, id) = document_with_rectangle();

        let mut shape = editor_with_tool_and_selection(&document, id, ActiveTool::Shape);
        assert!(shape.empty_canvas_press_deselects_first());
        shape.clear_selection();
        assert!(!shape.empty_canvas_press_deselects_first());

        // The select tool keeps spending the press on its marquee instead.
        let mut select = editor_with_tool_and_selection(&document, id, ActiveTool::Select);
        assert!(!select.empty_canvas_press_deselects_first());
        select.clear_selection();
        assert!(!select.empty_canvas_press_deselects_first());
    }

    #[test]
    fn text_tool_blank_press_deselects_without_requesting_a_draft() {
        let (document, id) = document_with_rectangle();
        let mut editor = editor_with_tool_and_selection(&document, id, ActiveTool::Text);

        editor
            .process_input(
                &document,
                pointer(PointerEventType::Down, blank_canvas_view_point()),
            )
            .unwrap();
        assert!(editor.state.selection.ids.is_empty());
        assert!(matches!(editor.state.interaction, InteractionState::Idle));
        assert!(!editor.take_new_text_draft_request());
    }

    #[test]
    fn text_tool_blank_press_requests_a_new_draft_when_unselected() {
        let document = DocumentModel::new();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::Text).unwrap();

        editor
            .process_input(
                &document,
                pointer(PointerEventType::Down, blank_canvas_view_point()),
            )
            .unwrap();
        assert!(matches!(editor.state.interaction, InteractionState::Idle));
        assert!(editor.take_new_text_draft_request());
        assert!(!editor.take_new_text_draft_request());
    }
}

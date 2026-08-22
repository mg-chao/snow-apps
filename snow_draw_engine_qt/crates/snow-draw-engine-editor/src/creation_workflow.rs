use super::*;
use snow_draw_engine_core::arrow::{ArrowEndpointEdge, ArrowStrokeStyle, ArrowType};
use snow_draw_engine_document::{
    ElementMeta, TextLayoutSize, arrow_is_degenerate, resolve_serial_number_diameter,
    validate_text_layout_size,
};

impl Editor {
    fn pen_highlight_preview(&self, start: Point<f64>, end: Point<f64>) -> Option<ArrowData> {
        let style = self.state.default_pen_highlight_style;
        ArrowData::from_global_points(
            &[start, end],
            style.stroke,
            style.stroke_width,
            ArrowStrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .map(ArrowData::into_pen_highlight)
    }

    pub(crate) fn process_pen_highlight_creation_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingPenHighlight(state) = self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }
        match event.event_type {
            PointerEventType::Move | PointerEventType::Enter => {
                let end = view_to_canvas(event.position, &self.camera(), self.surface_size());
                let preview = self.pen_highlight_preview(state.start_canvas_position, end);
                self.set_creation_preview(preview.map(ElementCreationPreview::Arrow), Vec::new());
                Ok(InteractionOutput {
                    consumed: true,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Crosshair),
                })
            }
            PointerEventType::Up => {
                let end = view_to_canvas(event.position, &self.camera(), self.surface_size());
                let preview = self.pen_highlight_preview(state.start_canvas_position, end);
                self.cancel_interaction();
                if let Some(pen) = preview.filter(|pen| !arrow_is_degenerate(pen)) {
                    let mut transaction = Transaction::new("create pen highlight");
                    transaction.insert_arrow(
                        document.peek_next_element_id(),
                        ElementMeta::default(),
                        pen,
                    );
                    self.queue_command(EditorCommand::ApplyTransaction(
                        ApplyTransactionCommand::new(transaction),
                    ));
                }
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.release_capture_command(),
                    cursor: CursorCommand::Set(CursorStyle::Crosshair),
                })
            }
            PointerEventType::Cancel => {
                self.cancel_interaction();
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.release_capture_command(),
                    cursor: CursorCommand::Set(CursorStyle::Crosshair),
                })
            }
            PointerEventType::Down | PointerEventType::DoubleClick | PointerEventType::Leave => {
                Ok(InteractionOutput::default())
            }
        }
    }

    fn minimum_committed_linear_length(&self) -> f64 {
        if self.state.active_tool == ActiveTool::Line {
            MIN_COMMITTED_LINE_LENGTH
        } else {
            MIN_COMMITTED_ARROW_LENGTH
        }
    }

    fn minimum_drag_created_linear_length(&self) -> f64 {
        MINIMUM_ARROW_SIZE_PX / self.camera().zoom.max(0.0001)
    }

    fn line_confirm_threshold(&self) -> f64 {
        LINE_CONFIRM_THRESHOLD_PX / self.camera().zoom.max(0.0001)
    }

    pub(crate) fn begin_arrow_creation(
        &mut self,
        pointer_id: u32,
        start_canvas_position: Point<f64>,
        start_view_position: Point<f64>,
    ) {
        self.state.interaction = InteractionState::CreatingArrow(CreateArrowState {
            pointer_id,
            committed_points: vec![start_canvas_position],
            press_view_position: start_view_position,
            phase: ArrowCreationPhase::InitialPress,
        });
        self.clear_transient_visuals();
    }

    pub(crate) fn arm_arrow_endpoint_creation(
        &mut self,
        mut state: CreateArrowState,
        pointer_id: u32,
        press_view_position: Point<f64>,
    ) {
        state.pointer_id = pointer_id;
        state.press_view_position = press_view_position;
        state.phase = ArrowCreationPhase::EndpointPress;
        self.state.interaction = InteractionState::CreatingArrow(state);
    }

    pub(crate) fn keep_arrow_creation_active(
        &mut self,
        mut state: CreateArrowState,
        preview_arrow: Option<ArrowData>,
        snap_guides: Vec<SnapGuide>,
    ) {
        state.phase = ArrowCreationPhase::AwaitingEndpoint;
        self.set_creation_preview(
            preview_arrow.map(ElementCreationPreview::Arrow),
            snap_guides,
        );
        self.state.interaction = InteractionState::CreatingArrow(state);
    }

    pub(crate) fn queue_arrow_creation(
        &mut self,
        document: &DocumentModel,
        arrow: ArrowData,
    ) -> Result<(), ErrorCode> {
        validate_arrow(&arrow)?;
        let mut transaction = Transaction::new(if arrow.is_line() {
            "create line"
        } else {
            "create arrow"
        });
        transaction.insert_arrow(
            document.peek_next_element_id(),
            ElementMeta::default(),
            arrow,
        );
        self.queue_command(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::new(transaction),
        ));
        Ok(())
    }

    pub fn queue_create_text_element(
        &mut self,
        document: &DocumentModel,
        center: Point<f64>,
        text_content: impl Into<String>,
        layout: TextLayoutSize,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        let text_content = text_content.into();
        if text_content.trim().is_empty() {
            return Ok(None);
        }
        let layout = validate_text_layout_size(layout)?;
        let mut text = TextData {
            center,
            text: text_content,
            ..self.state.default_text.clone()
        };
        if text.auto_resize {
            text.width = layout.width;
            text.height = layout.height;
        }
        validate_text(&text)?;
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("create text");
        transaction.insert_text(id, ElementMeta::default(), text);
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(
                transaction,
                self.capture_document_sync_snapshot(document),
            ),
        )))
    }

    pub(crate) fn serial_number_creation_preview(
        &self,
        document: &DocumentModel,
        center: Point<f64>,
    ) -> Result<SerialNumberData, ErrorCode> {
        let number = next_serial_number(document).max(self.state.default_serial_number.number);
        let mut serial = SerialNumberData {
            center,
            number,
            ..self.state.default_serial_number.clone()
        };
        serial.text_element_id = None;
        serial.diameter =
            resolve_serial_number_diameter(serial.number, serial.font_size, serial.diameter);
        validate_serial_number(&serial)?;
        Ok(serial)
    }

    pub(crate) fn queue_serial_number_creation(
        &mut self,
        document: &DocumentModel,
        serial: SerialNumberData,
    ) -> Result<ElementId, ErrorCode> {
        validate_serial_number(&serial)?;
        let id = document.peek_next_element_id();
        let next_default_number = serial.number.saturating_add(1);
        let mut transaction = Transaction::new("create serial number");
        transaction.insert_serial_number(id, ElementMeta::default(), serial);
        self.queue_command(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::new(transaction),
        ));
        self.state.default_serial_number.number = next_default_number;
        Ok(id)
    }

    pub(crate) fn snap_serial_number_creation_center(
        &self,
        document: &DocumentModel,
        current: Point<f64>,
        modifiers: Modifiers,
    ) -> (Point<f64>, Vec<SnapGuide>) {
        let snapping_mode = self.effective_snapping_mode(modifiers);
        match snapping_mode {
            SnappingMode::Grid => (
                GRID_SNAP_SERVICE.snap_point(current, self.config.grid.size),
                Vec::new(),
            ),
            SnappingMode::Object if self.config.snap.enable_point_snaps => {
                let snap = document.snap_point(&snow_draw_engine_core::SnapQuery {
                    point: current,
                    threshold: self.zoom_adjusted_snap_distance(),
                    include_grid: false,
                    grid_size: self.config.grid.size,
                });
                (
                    snap.point,
                    if self.config.snap.show_guides {
                        snap.guides
                    } else {
                        Vec::new()
                    },
                )
            }
            _ => (current, Vec::new()),
        }
    }

    pub(crate) fn finalize_arrow_creation_from_points(
        &mut self,
        document: &DocumentModel,
        points: &[Point<f64>],
        modifiers: Modifiers,
    ) -> Result<bool, ErrorCode> {
        let Some(arrow) = self.arrow_preview_from_points(document, points, modifiers, true) else {
            return Ok(false);
        };
        if arrow_length(&arrow) < self.minimum_committed_linear_length() {
            return Ok(false);
        }
        self.cancel_interaction();
        self.queue_arrow_creation(document, arrow)?;
        Ok(true)
    }

    pub(crate) fn snap_arrow_creation_point(
        &self,
        document: &DocumentModel,
        current: Point<f64>,
        modifiers: Modifiers,
    ) -> (Point<f64>, Vec<SnapGuide>) {
        let snapping_mode = self.effective_snapping_mode(modifiers);
        match snapping_mode {
            SnappingMode::Grid => (
                GRID_SNAP_SERVICE.snap_point(current, self.config.grid.size),
                Vec::new(),
            ),
            SnappingMode::Object if self.config.snap.enable_point_snaps => {
                let snap = document.snap_point(&snow_draw_engine_core::SnapQuery {
                    point: current,
                    threshold: self.zoom_adjusted_snap_distance(),
                    include_grid: false,
                    grid_size: self.config.grid.size,
                });
                (
                    snap.point,
                    if self.config.snap.show_guides {
                        snap.guides
                    } else {
                        Vec::new()
                    },
                )
            }
            _ => (current, Vec::new()),
        }
    }

    pub(crate) fn arrow_creation_preview(
        &self,
        document: &DocumentModel,
        committed_points: &[Point<f64>],
        current: Point<f64>,
        modifiers: Modifiers,
    ) -> (Point<f64>, Option<ArrowData>, Vec<SnapGuide>) {
        let (mut snapped_current, guides) =
            self.snap_arrow_creation_point(document, current, modifiers);
        if modifiers.shift
            && let Some(origin) = committed_points.last().copied()
            && (self.state.active_tool == ActiveTool::Line
                || self.state.default_arrow_style.arrow_type != ArrowType::Elbow)
        {
            snapped_current = lock_linear_point_to_discrete_angle(origin, snapped_current);
        }
        if self.state.active_tool == ActiveTool::Line
            && committed_points.len() >= 2
            && committed_points.first().is_some_and(|first| {
                point_distance(*first, snapped_current) <= self.line_confirm_threshold()
            })
        {
            snapped_current = committed_points[0];
        }
        let mut points = committed_points.to_vec();
        let should_append = points.last().is_none_or(|last| {
            point_distance(*last, snapped_current) >= self.line_confirm_threshold()
        });
        if should_append {
            points.push(snapped_current);
        }

        let preview_points = if points.len() >= 2 {
            points
        } else {
            committed_points.to_vec()
        };
        let preview = self.arrow_preview_from_points(document, &preview_points, modifiers, false);
        (snapped_current, preview, guides)
    }

    pub(crate) fn arrow_preview_from_points(
        &self,
        document: &DocumentModel,
        points: &[Point<f64>],
        modifiers: Modifiers,
        finalize: bool,
    ) -> Option<ArrowData> {
        let arrow = if self.state.active_tool == ActiveTool::Line {
            let style = self.state.default_line_style;
            preview_arrow_from_points(
                points,
                ArrowStyle {
                    stroke: style.stroke,
                    stroke_width: style.stroke_width,
                    start_arrowhead: None,
                    end_arrowhead: None,
                    stroke_style: style.stroke_style,
                    arrow_type: ArrowType::Curve,
                },
            )?
            .into_line(style.fill, style.fill_style)
        } else {
            preview_arrow_from_points(points, self.state.default_arrow_style)?
        };
        let arrow_id = document.peek_next_element_id();
        let bindables = self.bindable_elements(document, &[]);
        let arrow_context = self.arrow_engine_context(modifiers);

        if arrow.is_elbow() && points.len() == 2 {
            let mut preview_arrow = arrow.clone();
            for (edge, pointer) in [
                (ArrowEndpointEdge::Start, points.first().copied()?),
                (ArrowEndpointEdge::End, points.last().copied()?),
            ] {
                preview_arrow = preview_elbow_arrow_endpoint_binding(
                    &preview_arrow,
                    edge,
                    pointer,
                    &bindables,
                    arrow_context,
                    arrow_id,
                    finalize,
                );
            }
            return Some(preview_arrow);
        }

        let mut preview_arrow = arrow;

        for (edge, pointer) in [
            (ArrowEndpointEdge::Start, points.first().copied()?),
            (ArrowEndpointEdge::End, points.last().copied()?),
        ] {
            preview_arrow = compute_arrow_endpoint_drag(
                arrow_id,
                &preview_arrow,
                edge,
                pointer,
                &bindables,
                arrow_context,
                ArrowEndpointDragOptions {
                    new_arrow: true,
                    initial_binding: true,
                    alt_key: modifiers.alt,
                    finalize,
                },
            )
            .arrow;
        }

        Some(preview_arrow)
    }

    pub(crate) fn process_rectangle_creation_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        match event.event_type {
            PointerEventType::Down => Ok(InteractionOutput::default()),
            PointerEventType::DoubleClick => Ok(InteractionOutput::default()),
            PointerEventType::Move | PointerEventType::Enter => {
                self.handle_rectangle_pointer_move(document, event)
            }
            PointerEventType::Up => self.handle_rectangle_pointer_up(document, event),
            PointerEventType::Cancel => self.handle_rectangle_pointer_cancel(document, event),
            PointerEventType::Leave => Ok(InteractionOutput {
                consumed: false,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(CursorStyle::Default),
            }),
        }
    }

    pub(crate) fn handle_rectangle_pointer_move(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingRectangle(state) = &self.state.interaction else {
            return Ok(InteractionOutput {
                consumed: false,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(CursorStyle::Default),
            });
        };
        let state = *state;
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }

        let current_canvas = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let (preview, snap_guides) = self.preview_rectangle_with_snapping(
            document,
            state.start_canvas_position,
            current_canvas,
            event.modifiers,
        );
        let preview = if self.active_tool() == ActiveTool::Filter {
            preview.map(|rect| {
                let mut filter = self.state.default_filter;
                filter.center = rect.center;
                filter.width = rect.width;
                filter.height = rect.height;
                filter.rotation = rect.rotation;
                ElementCreationPreview::Filter(filter)
            })
        } else {
            preview.map(ElementCreationPreview::Rectangle)
        };
        self.set_creation_preview(preview, snap_guides);

        Ok(InteractionOutput {
            consumed: true,
            capture: PointerCaptureCommand::NoChange,
            cursor: CursorCommand::Set(self.tool_policy().default_cursor),
        })
    }

    pub(crate) fn handle_rectangle_pointer_up(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingRectangle(state) = &self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        let state = *state;
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }

        let current_canvas = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let (preview, _) = self.preview_rectangle_with_snapping(
            document,
            state.start_canvas_position,
            current_canvas,
            event.modifiers,
        );
        self.cancel_interaction();

        if let Some(rect) = preview {
            if self.active_tool() == ActiveTool::Filter {
                let mut filter = self.state.default_filter;
                filter.center = rect.center;
                filter.width = rect.width;
                filter.height = rect.height;
                filter.rotation = rect.rotation;
                validate_filter(&filter)?;
                let mut transaction = Transaction::new("create filter");
                transaction.insert_filter(
                    document.peek_next_element_id(),
                    ElementMeta::default(),
                    filter,
                );
                self.queue_command(EditorCommand::ApplyTransaction(
                    ApplyTransactionCommand::new(transaction),
                ));
                return Ok(InteractionOutput {
                    consumed: true,
                    capture: self.release_capture_command(),
                    cursor: CursorCommand::Set(self.tool_policy().default_cursor),
                });
            }
            validate_rectangle(&rect)?;
            let mut transaction = Transaction::new(if rect.is_highlight() {
                "create highlight"
            } else if rect.is_spotlight() {
                "create spotlight"
            } else {
                "create rectangle"
            });
            transaction.insert_rectangle(
                document.peek_next_element_id(),
                ElementMeta::default(),
                rect,
            );
            self.queue_command(EditorCommand::ApplyTransaction(
                ApplyTransactionCommand::new(transaction),
            ));
        }

        Ok(InteractionOutput {
            consumed: true,
            capture: self.release_capture_command(),
            cursor: CursorCommand::Set(self.tool_policy().default_cursor),
        })
    }

    pub(crate) fn handle_rectangle_pointer_cancel(
        &mut self,
        _document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingRectangle(state) = &self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        let state = *state;
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }

        self.cancel_interaction();
        Ok(InteractionOutput {
            consumed: true,
            capture: self.release_capture_command(),
            cursor: CursorCommand::Set(self.tool_policy().default_cursor),
        })
    }

    pub(crate) fn process_arrow_creation_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        match event.event_type {
            PointerEventType::Down => self.handle_arrow_pointer_down(document, event),
            PointerEventType::DoubleClick => self.handle_arrow_double_click(document, event),
            PointerEventType::Move | PointerEventType::Enter => {
                self.handle_arrow_pointer_move(document, event)
            }
            PointerEventType::Up => self.handle_arrow_pointer_up(document, event),
            PointerEventType::Cancel => self.handle_arrow_pointer_cancel(document, event),
            PointerEventType::Leave => Ok(InteractionOutput {
                consumed: false,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(CursorStyle::Crosshair),
            }),
        }
    }

    pub(crate) fn handle_arrow_double_click(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingArrow(state) = &self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        if event.button != Some(PointerButton::Primary)
            || state.pointer_id != event.pointer_id
            || state.phase != ArrowCreationPhase::AwaitingEndpoint
        {
            return Ok(InteractionOutput::default());
        }

        let current_canvas = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let (snapped_current, _) =
            self.snap_arrow_creation_point(document, current_canvas, event.modifiers);
        let Some(last_committed) = state.committed_points.last().copied() else {
            return Ok(InteractionOutput::default());
        };

        // Qt emits the first click as a normal press/release before the
        // double-click event. That release has already committed the terminal
        // point, so match Excalidraw's commit-zone behavior: only a second
        // click at that point finalizes the existing linear element.
        if point_distance(last_committed, snapped_current) >= self.line_confirm_threshold() {
            return Ok(InteractionOutput::default());
        }

        let points = state.committed_points.clone();
        let committed =
            self.finalize_arrow_creation_from_points(document, &points, event.modifiers)?;
        Ok(InteractionOutput {
            consumed: committed,
            capture: if committed {
                self.release_capture_command()
            } else {
                PointerCaptureCommand::NoChange
            },
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
        })
    }

    pub(crate) fn handle_arrow_pointer_down(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingArrow(state) = &self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        let state = state.clone();
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }

        if state.phase != ArrowCreationPhase::AwaitingEndpoint {
            return Ok(InteractionOutput::default());
        }

        match event.button {
            Some(PointerButton::Primary) => {
                let current_canvas =
                    view_to_canvas(event.position, &self.camera(), self.surface_size());
                let (snapped_current, preview, snap_guides) = self.arrow_creation_preview(
                    document,
                    &state.committed_points,
                    current_canvas,
                    event.modifiers,
                );

                // Excalidraw elbow arrows are defined by exactly two clicks:
                // the first sets the start and the second sets the end. They do
                // not enter the multi-point creation flow used by lines and
                // non-elbow arrows.
                if self.state.active_tool == ActiveTool::Arrow
                    && self.state.default_arrow_style.arrow_type == ArrowType::Elbow
                    && let Some(start) = state.committed_points.first().copied()
                    && self.finalize_arrow_creation_from_points(
                        document,
                        &[start, snapped_current],
                        event.modifiers,
                    )?
                {
                    return Ok(InteractionOutput {
                        consumed: true,
                        capture: PointerCaptureCommand::NoChange,
                        cursor: CursorCommand::Set(CursorStyle::Crosshair),
                    });
                }

                self.arm_arrow_endpoint_creation(state, event.pointer_id, event.position);
                self.set_creation_preview(preview.map(ElementCreationPreview::Arrow), snap_guides);

                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.capture_command_for_start(event.pointer_id),
                    cursor: CursorCommand::Set(CursorStyle::Crosshair),
                })
            }
            Some(PointerButton::Secondary) => {
                if self.finalize_arrow_creation_from_points(
                    document,
                    &state.committed_points,
                    event.modifiers,
                )? {
                    Ok(InteractionOutput {
                        consumed: true,
                        capture: PointerCaptureCommand::NoChange,
                        cursor: CursorCommand::Set(CursorStyle::Crosshair),
                    })
                } else {
                    Ok(InteractionOutput::default())
                }
            }
            _ => Ok(InteractionOutput::default()),
        }
    }

    pub(crate) fn handle_arrow_pointer_move(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingArrow(state) = &self.state.interaction else {
            return Ok(InteractionOutput {
                consumed: false,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(CursorStyle::Crosshair),
            });
        };
        let state = state.clone();
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }

        let current_canvas = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let (_, preview, snap_guides) = self.arrow_creation_preview(
            document,
            &state.committed_points,
            current_canvas,
            event.modifiers,
        );
        self.set_creation_preview(preview.map(ElementCreationPreview::Arrow), snap_guides);

        Ok(InteractionOutput {
            consumed: true,
            capture: PointerCaptureCommand::NoChange,
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
        })
    }

    pub(crate) fn handle_arrow_pointer_up(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingArrow(state) = &self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        let state = state.clone();
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }

        let current_canvas = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let (snapped_current, preview, snap_guides) = self.arrow_creation_preview(
            document,
            &state.committed_points,
            current_canvas,
            event.modifiers,
        );
        let is_click_release = pointer_drag_distance(state.press_view_position, event.position)
            < MINIMUM_ARROW_SIZE_PX;
        let committed_arrow = preview.clone().filter(|arrow| {
            arrow_length(arrow) >= self.minimum_drag_created_linear_length()
                && state.committed_points.last().is_none_or(|last| {
                    point_distance(*last, snapped_current) >= self.line_confirm_threshold()
                })
        });

        match state.phase {
            ArrowCreationPhase::InitialPress => {
                if !is_click_release {
                    if let Some(arrow) = committed_arrow {
                        self.cancel_interaction();
                        self.queue_arrow_creation(document, arrow)?;
                    } else {
                        self.cancel_interaction();
                    }
                } else {
                    self.keep_arrow_creation_active(state, preview, snap_guides);
                }
            }
            ArrowCreationPhase::AwaitingEndpoint => {
                return Ok(InteractionOutput::default());
            }
            ArrowCreationPhase::EndpointPress => {
                if !is_click_release {
                    if let Some(arrow) = committed_arrow {
                        self.cancel_interaction();
                        self.queue_arrow_creation(document, arrow)?;
                    } else {
                        self.keep_arrow_creation_active(state, preview, snap_guides);
                    }
                } else if state.committed_points.last().is_some_and(|last| {
                    point_distance(*last, snapped_current) < self.line_confirm_threshold()
                }) {
                    if !self.finalize_arrow_creation_from_points(
                        document,
                        &state.committed_points,
                        event.modifiers,
                    )? {
                        self.keep_arrow_creation_active(state, preview, snap_guides);
                    }
                } else {
                    let mut next_state = state;
                    next_state.committed_points.push(snapped_current);
                    let preview = self.arrow_preview_from_points(
                        document,
                        &next_state.committed_points,
                        event.modifiers,
                        false,
                    );
                    self.keep_arrow_creation_active(next_state, preview, snap_guides);
                }
            }
        }

        Ok(InteractionOutput {
            consumed: true,
            capture: self.release_capture_command(),
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
        })
    }

    pub(crate) fn handle_arrow_pointer_cancel(
        &mut self,
        _document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingArrow(state) = &self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }

        self.cancel_interaction();
        Ok(InteractionOutput {
            consumed: true,
            capture: self.release_capture_command(),
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
        })
    }

    pub(crate) fn process_serial_number_creation_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        match event.event_type {
            PointerEventType::Down => Ok(InteractionOutput::default()),
            PointerEventType::DoubleClick => Ok(InteractionOutput::default()),
            PointerEventType::Move | PointerEventType::Enter => {
                self.handle_serial_number_pointer_move(document, event)
            }
            PointerEventType::Up => self.handle_serial_number_pointer_up(document, event),
            PointerEventType::Cancel => self.handle_serial_number_pointer_cancel(document, event),
            PointerEventType::Leave => Ok(InteractionOutput {
                consumed: false,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(CursorStyle::Default),
            }),
        }
    }

    pub(crate) fn handle_serial_number_pointer_move(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingSerialNumber(state) = &self.state.interaction else {
            return Ok(InteractionOutput {
                consumed: false,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(CursorStyle::Default),
            });
        };
        let state = state.clone();
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }

        let current_canvas = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let (center, snap_guides) =
            self.snap_serial_number_creation_center(document, current_canvas, event.modifiers);
        let mut preview = state.preview;
        preview.center = center;
        validate_serial_number(&preview)?;
        self.state.interaction = InteractionState::CreatingSerialNumber(CreateSerialNumberState {
            pointer_id: event.pointer_id,
            preview: preview.clone(),
        });
        self.set_creation_preview(
            Some(ElementCreationPreview::SerialNumber(preview)),
            snap_guides,
        );

        Ok(InteractionOutput {
            consumed: true,
            capture: PointerCaptureCommand::NoChange,
            cursor: CursorCommand::Set(CursorStyle::Default),
        })
    }

    pub(crate) fn handle_serial_number_pointer_up(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingSerialNumber(state) = &self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        let state = state.clone();
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }

        let current_canvas = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let (center, _) =
            self.snap_serial_number_creation_center(document, current_canvas, event.modifiers);
        let mut preview = state.preview;
        preview.center = center;
        validate_serial_number(&preview)?;
        self.cancel_interaction();
        self.queue_serial_number_creation(document, preview)?;

        Ok(InteractionOutput {
            consumed: true,
            capture: self.release_capture_command(),
            cursor: CursorCommand::Set(CursorStyle::Default),
        })
    }

    pub(crate) fn handle_serial_number_pointer_cancel(
        &mut self,
        _document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingSerialNumber(state) = &self.state.interaction else {
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
}

#[cfg(test)]
mod line_creation_tests {
    use super::*;
    use snow_draw_engine_core::{SnapGuideAxis, SnapGuideKind};
    use snow_draw_engine_document::{CanvasFilterType, ElementData};
    use snow_draw_engine_interaction::{PointerButtons, PointerDevice};

    fn editor_with_non_dominant_x_snap_reference() -> (Editor, DocumentModel) {
        let mut config = EngineConfig::default();
        config.snap.enabled = true;
        config.snap.enable_gap_snaps = false;
        let editor = Editor::new(config).unwrap();
        let mut document = DocumentModel::new();
        let (reference, _) = editor.preview_rectangle_with_snapping(
            &document,
            Point::new(19.0, 29.0),
            Point::new(29.0, 49.0),
            Modifiers::default(),
        );
        let reference = reference.expect("reference rectangle should be valid");
        let mut transaction = Transaction::new("insert snap reference");
        transaction.insert_rectangle(
            document.peek_next_element_id(),
            ElementMeta::default(),
            reference,
        );
        document.apply_transaction(transaction).unwrap();
        (editor, document)
    }

    fn assert_vertical_point_guide_matches_right_edge(rect: &RectangleData, guides: &[SnapGuide]) {
        let bounds = rectangle_to_draw_rect(rect);
        let guide = guides
            .iter()
            .find(|guide| {
                guide.kind == SnapGuideKind::Point && guide.axis == SnapGuideAxis::Vertical
            })
            .expect("the applied X snap should return a vertical point guide");
        assert!((guide.start.x - bounds.max_x).abs() <= 1e-9);
        assert!((guide.end.x - bounds.max_x).abs() <= 1e-9);
    }

    #[test]
    fn alt_rectangle_creation_scales_from_the_press_center() {
        let document = DocumentModel::new();
        let editor = Editor::new(EngineConfig::default()).unwrap();
        let start = Point::new(10.0, 20.0);
        let current = Point::new(15.0, 30.0);

        let (preview, _) = editor.preview_rectangle_with_snapping(
            &document,
            start,
            current,
            Modifiers {
                alt: true,
                ..Modifiers::default()
            },
        );
        let rect = preview.expect("Alt rectangle drag should produce a preview");
        assert_eq!(rect.center, start);
        assert_eq!(rect.width, 10.0);
        assert_eq!(rect.height, 20.0);

        let (preview, _) = editor.preview_rectangle_with_snapping(
            &document,
            start,
            current,
            Modifiers {
                alt: true,
                shift: true,
                ..Modifiers::default()
            },
        );
        let rect = preview.expect("Alt+Shift rectangle drag should produce a preview");
        assert_eq!(rect.center, start);
        assert_eq!(rect.width, rect.height);
    }

    #[test]
    fn shift_rectangle_creation_applies_non_dominant_x_object_snap() {
        let (editor, document) = editor_with_non_dominant_x_snap_reference();
        let (preview, guides) = editor.preview_rectangle_with_snapping(
            &document,
            Point::new(0.0, 0.0),
            Point::new(10.0, 20.0),
            Modifiers {
                shift: true,
                ..Modifiers::default()
            },
        );
        let rect = preview.expect("Shift rectangle drag should produce a preview");
        let bounds = rectangle_to_draw_rect(&rect);

        assert!((bounds.max_x - 19.0).abs() <= 1e-9);
        assert!((rect.width - 19.0).abs() <= 1e-9);
        assert_eq!(rect.width, rect.height);
        assert_vertical_point_guide_matches_right_edge(&rect, &guides);
    }

    #[test]
    fn alt_shift_rectangle_creation_applies_non_dominant_x_object_snap_from_center() {
        let (editor, document) = editor_with_non_dominant_x_snap_reference();
        let start = Point::new(0.0, 0.0);
        let (preview, guides) = editor.preview_rectangle_with_snapping(
            &document,
            start,
            Point::new(10.0, 20.0),
            Modifiers {
                alt: true,
                shift: true,
                ..Modifiers::default()
            },
        );
        let rect = preview.expect("Alt+Shift rectangle drag should produce a preview");
        let bounds = rectangle_to_draw_rect(&rect);

        assert_eq!(rect.center, start);
        assert!((bounds.max_x - 19.0).abs() <= 1e-9);
        assert!((rect.width - 38.0).abs() <= 1e-9);
        assert_eq!(rect.width, rect.height);
        assert_vertical_point_guide_matches_right_edge(&rect, &guides);
    }

    #[test]
    fn line_drag_queues_one_canonical_line_element() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_active_tool(ActiveTool::Line).unwrap();

        assert!(
            editor
                .finalize_arrow_creation_from_points(
                    &document,
                    &[Point::new(20.0, 30.0), Point::new(28.0, 30.0)],
                    Modifiers::default(),
                )
                .unwrap()
        );
        let Some(EditorCommand::ApplyTransaction(command)) = editor.pending_command.take() else {
            panic!("a meaningful Line drag should queue one transaction");
        };
        assert_eq!(command.transaction.label(), "create line");
        document.apply_transaction(command.transaction).unwrap();

        let record = document.element(id).unwrap();
        assert_eq!(record.data.kind(), ElementKind::Line);
        let ElementData::Arrow(line) = &record.data else {
            panic!("Line should use the shared linear geometry record");
        };
        assert!(line.is_line());
        assert_eq!(line.arrow_type, ArrowType::Curve);
        assert_eq!(line.start_arrowhead, None);
        assert_eq!(line.end_arrowhead, None);
    }

    #[test]
    fn subthreshold_line_drag_queues_no_element_or_history_transaction() {
        let document = DocumentModel::new();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_active_tool(ActiveTool::Line).unwrap();

        assert!(
            !editor
                .finalize_arrow_creation_from_points(
                    &document,
                    &[Point::new(0.0, 0.0), Point::new(7.99, 0.0)],
                    Modifiers::default(),
                )
                .unwrap()
        );
        assert!(editor.pending_command.is_none());
        assert!(document.element(document.peek_next_element_id()).is_err());
    }

    #[test]
    fn line_preview_snaps_near_closure_to_exact_first_point() {
        let document = DocumentModel::new();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_active_tool(ActiveTool::Line).unwrap();
        let committed = [Point::new(10.0, 10.0), Point::new(80.0, 80.0)];

        let (snapped, preview, _) = editor.arrow_creation_preview(
            &document,
            &committed,
            Point::new(16.0, 14.0),
            Modifiers::default(),
        );
        assert_eq!(snapped, committed[0]);
        let preview = preview.unwrap();
        assert_eq!(
            preview.global_points().first(),
            preview.global_points().last()
        );
        assert_eq!(preview.global_points().len(), 3);
    }

    #[test]
    fn line_double_click_finishes_without_a_duplicate_terminal_point() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::Line).unwrap();
        editor.state.interaction = InteractionState::CreatingArrow(CreateArrowState {
            pointer_id: 4,
            committed_points: vec![
                Point::new(-50.0, 0.0),
                Point::new(0.0, 40.0),
                Point::new(50.0, 0.0),
            ],
            press_view_position: Point::new(150.0, 100.0),
            phase: ArrowCreationPhase::AwaitingEndpoint,
        });

        editor
            .process_arrow_creation_pointer_event(
                &document,
                PointerEvent {
                    pointer_id: 4,
                    event_type: PointerEventType::DoubleClick,
                    device: PointerDevice::Mouse,
                    position: Point::new(150.0, 100.0),
                    button: Some(PointerButton::Primary),
                    buttons: PointerButtons(PointerButtons::PRIMARY),
                    modifiers: Modifiers::default(),
                },
            )
            .unwrap();
        let Some(EditorCommand::ApplyTransaction(command)) = editor.pending_command.take() else {
            panic!("double-click should finish the active Line");
        };
        document.apply_transaction(command.transaction).unwrap();

        assert_eq!(document.arrow(id).unwrap().global_points().len(), 3);
        assert!(matches!(editor.state.interaction, InteractionState::Idle));
    }

    #[test]
    fn arrow_double_click_finishes_without_a_duplicate_terminal_point() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::Arrow).unwrap();
        editor.state.interaction = InteractionState::CreatingArrow(CreateArrowState {
            pointer_id: 4,
            committed_points: vec![
                Point::new(-50.0, 0.0),
                Point::new(0.0, 40.0),
                Point::new(50.0, 0.0),
            ],
            press_view_position: Point::new(150.0, 100.0),
            phase: ArrowCreationPhase::AwaitingEndpoint,
        });

        editor
            .process_arrow_creation_pointer_event(
                &document,
                PointerEvent {
                    pointer_id: 4,
                    event_type: PointerEventType::DoubleClick,
                    device: PointerDevice::Mouse,
                    position: Point::new(150.0, 100.0),
                    button: Some(PointerButton::Primary),
                    buttons: PointerButtons(PointerButtons::PRIMARY),
                    modifiers: Modifiers::default(),
                },
            )
            .unwrap();
        let Some(EditorCommand::ApplyTransaction(command)) = editor.pending_command.take() else {
            panic!("double-click should finish the active Arrow");
        };
        document.apply_transaction(command.transaction).unwrap();

        let arrow = document.arrow(id).unwrap();
        assert!(!arrow.is_line());
        assert_eq!(arrow.global_points().len(), 3);
        assert!(matches!(editor.state.interaction, InteractionState::Idle));
    }

    #[test]
    fn double_click_away_from_terminal_does_not_finish_linear_creation() {
        let document = DocumentModel::new();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::Line).unwrap();
        editor.state.interaction = InteractionState::CreatingArrow(CreateArrowState {
            pointer_id: 4,
            committed_points: vec![Point::new(-50.0, 0.0), Point::new(50.0, 0.0)],
            press_view_position: Point::new(150.0, 100.0),
            phase: ArrowCreationPhase::AwaitingEndpoint,
        });

        let output = editor
            .process_arrow_creation_pointer_event(
                &document,
                PointerEvent {
                    pointer_id: 4,
                    event_type: PointerEventType::DoubleClick,
                    device: PointerDevice::Mouse,
                    position: Point::new(250.0, 100.0),
                    button: Some(PointerButton::Primary),
                    buttons: PointerButtons(PointerButtons::PRIMARY),
                    modifiers: Modifiers::default(),
                },
            )
            .unwrap();

        assert!(!output.consumed);
        assert!(editor.pending_command.is_none());
        assert!(matches!(
            editor.state.interaction,
            InteractionState::CreatingArrow(_)
        ));
    }

    #[test]
    fn repeated_single_click_on_terminal_point_finishes_arrow_creation() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::Arrow).unwrap();
        editor.state.interaction = InteractionState::CreatingArrow(CreateArrowState {
            pointer_id: 4,
            committed_points: vec![Point::new(-50.0, 0.0), Point::new(50.0, 0.0)],
            press_view_position: Point::new(150.0, 100.0),
            phase: ArrowCreationPhase::EndpointPress,
        });

        editor
            .handle_arrow_pointer_up(
                &document,
                PointerEvent {
                    pointer_id: 4,
                    event_type: PointerEventType::Up,
                    device: PointerDevice::Mouse,
                    position: Point::new(150.0, 100.0),
                    button: Some(PointerButton::Primary),
                    buttons: PointerButtons::default(),
                    modifiers: Modifiers::default(),
                },
            )
            .unwrap();

        let Some(EditorCommand::ApplyTransaction(command)) = editor.pending_command.take() else {
            panic!("clicking the commit zone should finish Arrow creation");
        };
        document.apply_transaction(command.transaction).unwrap();
        assert_eq!(document.arrow(id).unwrap().global_points().len(), 2);
        assert!(matches!(editor.state.interaction, InteractionState::Idle));
    }

    #[test]
    fn elbow_arrow_finishes_on_the_second_pointer_down() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::Arrow).unwrap();
        editor.state.default_arrow_style.arrow_type = ArrowType::Elbow;
        editor.state.interaction = InteractionState::CreatingArrow(CreateArrowState {
            pointer_id: 4,
            committed_points: vec![Point::new(-50.0, -40.0)],
            press_view_position: Point::new(50.0, 60.0),
            phase: ArrowCreationPhase::AwaitingEndpoint,
        });

        let output = editor
            .handle_arrow_pointer_down(
                &document,
                PointerEvent {
                    pointer_id: 4,
                    event_type: PointerEventType::Down,
                    device: PointerDevice::Mouse,
                    position: Point::new(150.0, 140.0),
                    button: Some(PointerButton::Primary),
                    buttons: PointerButtons(PointerButtons::PRIMARY),
                    modifiers: Modifiers::default(),
                },
            )
            .unwrap();

        assert!(output.consumed);
        assert!(matches!(editor.state.interaction, InteractionState::Idle));
        let Some(EditorCommand::ApplyTransaction(command)) = editor.pending_command.take() else {
            panic!("the second Elbow Arrow click should queue its creation");
        };
        document.apply_transaction(command.transaction).unwrap();

        let arrow = document.arrow(id).unwrap();
        assert!(arrow.is_elbow());
        assert_eq!(arrow.start(), Point::new(-50.0, -40.0));
        assert_eq!(arrow.end(), Point::new(50.0, 40.0));
        assert!(arrow.global_points().windows(2).all(|segment| {
            (segment[0].x - segment[1].x).abs() <= 1e-6
                || (segment[0].y - segment[1].y).abs() <= 1e-6
        }));
    }

    #[test]
    fn secondary_double_click_does_not_finish_line_creation() {
        let document = DocumentModel::new();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::Line).unwrap();
        editor.state.interaction = InteractionState::CreatingArrow(CreateArrowState {
            pointer_id: 4,
            committed_points: vec![Point::new(-50.0, 0.0), Point::new(50.0, 0.0)],
            press_view_position: Point::new(150.0, 100.0),
            phase: ArrowCreationPhase::AwaitingEndpoint,
        });

        let output = editor
            .handle_arrow_double_click(
                &document,
                PointerEvent {
                    pointer_id: 4,
                    event_type: PointerEventType::DoubleClick,
                    device: PointerDevice::Mouse,
                    position: Point::new(150.0, 100.0),
                    button: Some(PointerButton::Secondary),
                    buttons: PointerButtons(PointerButtons::SECONDARY),
                    modifiers: Modifiers::default(),
                },
            )
            .unwrap();

        assert!(!output.consumed);
        assert!(editor.pending_command.is_none());
        assert!(matches!(
            editor.state.interaction,
            InteractionState::CreatingArrow(_)
        ));
    }

    #[test]
    fn filter_drag_queues_one_filter_with_default_style() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_active_tool(ActiveTool::Filter).unwrap();
        editor.state.interaction = InteractionState::CreatingRectangle(CreateRectangleState {
            pointer_id: 7,
            start_canvas_position: Point::new(10.0, 20.0),
        });

        editor
            .handle_rectangle_pointer_up(
                &document,
                PointerEvent {
                    pointer_id: 7,
                    event_type: PointerEventType::Up,
                    device: PointerDevice::Mouse,
                    position: Point::new(90.0, 60.0),
                    button: Some(PointerButton::Primary),
                    buttons: PointerButtons::default(),
                    modifiers: Modifiers::default(),
                },
            )
            .unwrap();
        let Some(EditorCommand::ApplyTransaction(command)) = editor.pending_command.take() else {
            panic!("a meaningful Filter drag should queue one transaction");
        };
        assert_eq!(command.transaction.label(), "create filter");
        assert_eq!(command.transaction.operations().len(), 1);
        document.apply_transaction(command.transaction).unwrap();

        let ElementData::Filter(filter) = &document.element(id).unwrap().data else {
            panic!("Filter should retain its first-class document identity");
        };
        assert_eq!(filter.filter_type, CanvasFilterType::Mosaic);
        assert_eq!(filter.strength, 0.5);
        assert_eq!(filter.opacity, 1.0);
        assert_eq!(filter.center, Point::new(50.0, 40.0));
        assert_eq!((filter.width, filter.height), (80.0, 40.0));
    }

    #[test]
    fn spotlight_drag_previews_and_commits_a_transparent_crosshair_rectangle() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_active_tool(ActiveTool::Spotlight).unwrap();
        editor.state.interaction = InteractionState::CreatingRectangle(CreateRectangleState {
            pointer_id: 9,
            start_canvas_position: Point::new(10.0, 20.0),
        });
        let event = |event_type, position| PointerEvent {
            pointer_id: 9,
            event_type,
            device: PointerDevice::Mouse,
            position,
            button: Some(PointerButton::Primary),
            buttons: PointerButtons::default(),
            modifiers: Modifiers::default(),
        };

        let move_output = editor
            .handle_rectangle_pointer_move(
                &document,
                event(PointerEventType::Move, Point::new(90.0, 60.0)),
            )
            .unwrap();
        assert_eq!(
            move_output.cursor,
            CursorCommand::Set(CursorStyle::Crosshair)
        );
        let Some(ElementCreationPreview::Rectangle(preview)) =
            editor.state.creation_preview.as_ref()
        else {
            panic!("the first Spotlight drag must activate a rectangle preview");
        };
        assert!(preview.is_spotlight());
        assert_eq!(preview.opacity, 1.0);
        assert_eq!(preview.fill.a, 0);
        assert_eq!(preview.stroke.a, 0);

        let up_output = editor
            .handle_rectangle_pointer_up(
                &document,
                event(PointerEventType::Up, Point::new(90.0, 60.0)),
            )
            .unwrap();
        assert_eq!(up_output.cursor, CursorCommand::Set(CursorStyle::Crosshair));
        let Some(EditorCommand::ApplyTransaction(command)) = editor.pending_command.take() else {
            panic!("a meaningful Spotlight drag should queue one transaction");
        };
        assert_eq!(command.transaction.label(), "create spotlight");
        document.apply_transaction(command.transaction).unwrap();
        let spotlight = document.rectangle(id).unwrap();
        assert!(spotlight.is_spotlight());
        assert_eq!(spotlight.center, Point::new(50.0, 40.0));
        assert_eq!((spotlight.width, spotlight.height), (80.0, 40.0));
        assert_eq!(spotlight.opacity, 1.0);
        assert_eq!(spotlight.fill.a, 0);
        assert_eq!(spotlight.stroke.a, 0);
    }

    #[test]
    fn pen_highlight_drag_previews_and_commits_one_straight_two_point_line() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_active_tool(ActiveTool::PenHighlight).unwrap();
        editor.state.interaction = InteractionState::CreatingPenHighlight(CreateRectangleState {
            pointer_id: 7,
            start_canvas_position: Point::new(10.0, 20.0),
        });
        let event = |event_type, position| PointerEvent {
            pointer_id: 7,
            event_type,
            device: PointerDevice::Mouse,
            position,
            button: Some(PointerButton::Primary),
            buttons: PointerButtons::default(),
            modifiers: Modifiers::default(),
        };

        editor
            .process_pen_highlight_creation_pointer_event(
                &document,
                event(PointerEventType::Move, Point::new(70.0, 50.0)),
            )
            .unwrap();
        let Some(ElementCreationPreview::Arrow(preview)) = editor.state.creation_preview.as_ref()
        else {
            panic!("pen drag should expose an arrow preview");
        };
        assert!(preview.is_pen_highlight());
        assert_eq!(preview.global_points().len(), 2);
        assert_eq!(preview.stroke_width, 30.0);

        editor
            .process_pen_highlight_creation_pointer_event(
                &document,
                event(PointerEventType::Up, Point::new(70.0, 50.0)),
            )
            .unwrap();
        let Some(EditorCommand::ApplyTransaction(command)) = editor.pending_command.take() else {
            panic!("pen drag should queue one transaction");
        };
        document.apply_transaction(command.transaction).unwrap();
        let ElementData::Arrow(pen) = &document.element(id).unwrap().data else {
            panic!("pen highlight should use linear document geometry");
        };
        assert!(pen.is_pen_highlight());
        assert_eq!(
            pen.global_points(),
            &[Point::new(10.0, 20.0), Point::new(70.0, 50.0)]
        );
        assert_eq!(pen.arrow_type, ArrowType::Straight);
        assert_eq!(pen.start_arrowhead, None);
        assert_eq!(pen.end_arrowhead, None);
    }

    #[test]
    fn pen_highlight_cancel_and_degenerate_release_commit_nothing() {
        for event_type in [PointerEventType::Cancel, PointerEventType::Up] {
            let document = DocumentModel::new();
            let mut editor = Editor::new(EngineConfig::default()).unwrap();
            editor.set_active_tool(ActiveTool::PenHighlight).unwrap();
            editor.state.interaction =
                InteractionState::CreatingPenHighlight(CreateRectangleState {
                    pointer_id: 3,
                    start_canvas_position: Point::new(20.0, 20.0),
                });
            editor
                .process_pen_highlight_creation_pointer_event(
                    &document,
                    PointerEvent {
                        pointer_id: 3,
                        event_type,
                        device: PointerDevice::Mouse,
                        position: Point::new(20.0, 20.0),
                        button: Some(PointerButton::Primary),
                        buttons: PointerButtons::default(),
                        modifiers: Modifiers::default(),
                    },
                )
                .unwrap();
            assert!(editor.pending_command.is_none());
            assert!(editor.state.creation_preview.is_none());
        }
    }
}

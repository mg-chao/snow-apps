use super::*;
use snow_draw_engine_core::arrow::ArrowEndpointEdge;

impl Editor {
    pub(crate) fn update_arrow_edit_preview(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) {
        let InteractionState::EditingArrow(state) = &self.state.interaction else {
            return;
        };

        let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let next_preview =
            self.arrow_edit_preview(document, state, canvas_point, event.modifiers, false);
        if next_preview.arrow == state.preview_arrow
            && next_preview.next_mode.unwrap_or(state.mode) == state.mode
        {
            return;
        }

        if let InteractionState::EditingArrow(active) = &mut self.state.interaction {
            active.preview_arrow = next_preview.arrow;
            if let Some(next_mode) = next_preview.next_mode {
                active.mode = next_mode;
            }
        }
        self.bump_scene_state_revision();
        self.bump_overlay_state_revision();
    }

    pub(crate) fn commit_arrow_edit(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<(), ErrorCode> {
        let state = match std::mem::replace(&mut self.state.interaction, InteractionState::Idle) {
            InteractionState::EditingArrow(state) => state,
            other => {
                self.state.interaction = other;
                return Ok(());
            }
        };

        let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let next_preview =
            self.arrow_edit_preview(document, &state, canvas_point, event.modifiers, true);
        if next_preview.arrow == state.original_arrow {
            return Ok(());
        }

        validate_arrow(&next_preview.arrow)?;
        let history_undo_snapshot = self.capture_document_sync_snapshot(document);
        let preview_selection_arrows = vec![SelectionArrowState {
            id: state.arrow_id,
            arrow: next_preview.arrow.clone(),
        }];
        self.state.selection.elements = Vec::new();
        self.state.selection.arrows = preview_selection_arrows.clone();
        self.state.selection.bounds = selection_bounds_from_selection(
            &self.state.selection.elements,
            &preview_selection_arrows,
        );
        let mut transaction = Transaction::new(arrow_edit_label(state.mode));
        transaction.update_arrow(state.arrow_id, next_preview.arrow.clone());
        self.append_arrow_reorder_targets(
            &mut transaction,
            document,
            state.arrow_id,
            &next_preview.reorder_targets,
        );
        self.queue_command(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, history_undo_snapshot),
        ));
        Ok(())
    }

    pub(crate) fn arrow_edit_preview(
        &self,
        document: &DocumentModel,
        state: &EditArrowState,
        canvas_point: Point<f64>,
        modifiers: Modifiers,
        finalize: bool,
    ) -> ArrowEditPreview {
        let drag_target = Point::new(
            canvas_point.x + state.drag_offset.x,
            canvas_point.y + state.drag_offset.y,
        );
        match state.mode {
            ArrowEditMode::Move => ArrowEditPreview {
                arrow: translated_arrow_for_move(
                    &state.original_arrow,
                    Point {
                        x: canvas_point.x - state.start_canvas_position.x,
                        y: canvas_point.y - state.start_canvas_position.y,
                    },
                ),
                reorder_targets: Vec::new(),
                next_mode: None,
            },
            ArrowEditMode::Endpoint(edge) => {
                let mut drag_target = self.snap_linear_arrow_control_point(drag_target, modifiers);
                if modifiers.shift && !state.original_arrow.is_elbow() {
                    let points = state.original_arrow.global_points();
                    let reference_index = match edge {
                        ArrowEndpointEdge::Start => 1,
                        ArrowEndpointEdge::End => points.len().saturating_sub(2),
                    };
                    if let Some(reference) = points.get(reference_index).copied() {
                        drag_target = lock_linear_point_to_discrete_angle(reference, drag_target);
                    }
                }
                let bindables = self.bindable_elements(document, &[]);
                let context = self.arrow_engine_context(modifiers);
                let result = compute_arrow_endpoint_drag(
                    state.arrow_id,
                    &state.original_arrow,
                    edge,
                    drag_target,
                    &bindables,
                    context,
                    ArrowEndpointDragOptions {
                        new_arrow: false,
                        initial_binding: false,
                        alt_key: modifiers.alt,
                        finalize,
                    },
                );
                let arrow = snap_linear_arrow_loop_endpoint(
                    &result.arrow,
                    edge,
                    LINE_CONFIRM_THRESHOLD_PX / self.camera().zoom.max(0.0001),
                );
                let mut arrow = arrow;
                if finalize {
                    let preview_binding = match edge {
                        ArrowEndpointEdge::Start => state.preview_arrow.start_binding.as_ref(),
                        ArrowEndpointEdge::End => state.preview_arrow.end_binding.as_ref(),
                    };
                    let finalized_binding = match edge {
                        ArrowEndpointEdge::Start => arrow.start_binding.as_ref(),
                        ArrowEndpointEdge::End => arrow.end_binding.as_ref(),
                    };
                    let preserved_binding = preview_binding
                        .zip(finalized_binding)
                        .filter(|(preview, finalized)| preview.element_id == finalized.element_id)
                        .map(|(preview, _)| preview.clone());
                    if let Some(binding) = preserved_binding {
                        let changed_bindable_ids = [binding.element_id];
                        match edge {
                            ArrowEndpointEdge::Start => {
                                arrow.set_start_element_binding(Some(binding))
                            }
                            ArrowEndpointEdge::End => arrow.set_end_element_binding(Some(binding)),
                        }
                        arrow = recompute_arrow_after_bindable_change(
                            state.arrow_id,
                            &arrow,
                            &bindables,
                            &changed_bindable_ids,
                            context,
                        )
                        .arrow;
                    }
                }
                ArrowEditPreview {
                    arrow,
                    reorder_targets: result.reorder_targets,
                    next_mode: None,
                }
            }
            ArrowEditMode::Point(index) => {
                let mut snapped_point =
                    self.snap_linear_arrow_control_point(drag_target, modifiers);
                if modifiers.shift
                    && let Some(reference) = state
                        .original_arrow
                        .global_points()
                        .get(if index == 0 { 1 } else { index - 1 })
                        .copied()
                {
                    snapped_point = lock_linear_point_to_discrete_angle(reference, snapped_point);
                }
                let arrow = move_linear_arrow_point(&state.original_arrow, index, snapped_point)
                    .unwrap_or_else(|| state.original_arrow.clone());
                let arrow = if finalize {
                    remove_linear_arrow_point_near_neighbor(
                        &arrow,
                        index,
                        LINE_CONFIRM_THRESHOLD_PX / self.camera().zoom.max(0.0001),
                    )
                } else {
                    arrow
                };
                ArrowEditPreview {
                    arrow,
                    reorder_targets: Vec::new(),
                    next_mode: None,
                }
            }
            ArrowEditMode::FocusPoint(edge) => {
                let result = compute_arrow_focus_drag(
                    state.arrow_id,
                    &state.original_arrow,
                    edge,
                    canvas_point,
                    &self.bindable_elements(document, &[]),
                    self.arrow_engine_context(modifiers),
                    ArrowFocusDragOptions {
                        switch_to_inside_binding: modifiers.alt,
                        grid_size: (self.effective_snapping_mode(modifiers) == SnappingMode::Grid)
                            .then_some(self.config.grid.size),
                    },
                );
                ArrowEditPreview {
                    arrow: result.arrow,
                    reorder_targets: result.reorder_targets,
                    next_mode: None,
                }
            }
            ArrowEditMode::Segment(index) => {
                if !state.original_arrow.is_elbow() {
                    let mut snapped_point =
                        self.snap_linear_arrow_control_point(drag_target, modifiers);
                    if modifiers.shift
                        && let Some(reference) =
                            state.original_arrow.global_points().get(index - 1).copied()
                    {
                        snapped_point =
                            lock_linear_point_to_discrete_angle(reference, snapped_point);
                    }
                    let drag_threshold = DRAGGING_THRESHOLD_PX / self.camera().zoom.max(0.0001);
                    if pointer_drag_distance(state.start_canvas_position, canvas_point)
                        < drag_threshold
                    {
                        return ArrowEditPreview {
                            arrow: state.original_arrow.clone(),
                            reorder_targets: Vec::new(),
                            next_mode: None,
                        };
                    }

                    return ArrowEditPreview {
                        arrow: insert_linear_arrow_segment_point(
                            &state.original_arrow,
                            index,
                            snapped_point,
                        )
                        .unwrap_or_else(|| state.original_arrow.clone()),
                        reorder_targets: Vec::new(),
                        next_mode: None,
                    };
                }

                let result = drag_elbow_arrow_segment(
                    state.arrow_id,
                    &state.preview_arrow,
                    index,
                    canvas_point,
                    &self.bindable_elements(document, &[]),
                    self.arrow_engine_context(modifiers),
                    finalize,
                );
                ArrowEditPreview {
                    arrow: result.arrow,
                    reorder_targets: Vec::new(),
                    next_mode: Some(ArrowEditMode::Segment(result.next_segment_index)),
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{
        ColorRgba8, CornerRadii, EngineConfig,
        arrow::{StrokeStyle, ArrowType},
    };
    use snow_draw_engine_document::{
        ElementMeta, FillStyle, HighlightShape, RectangleData, RectangleElementKind,
        Transaction,
    };
    use snow_draw_engine_interaction::{
        PointerButton, PointerButtons, PointerDevice, PointerEventType,
    };

    #[test]
    fn releasing_the_dragged_last_elbow_segment_commits_the_visible_route() {
        let base = ArrowData::from_global_points(
            &[
                Point::new(-80.0, 50.0),
                Point::new(0.0, 50.0),
                Point::new(0.0, -50.0),
                Point::new(80.0, -50.0),
            ],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Elbow,
            None,
            None,
        )
        .unwrap();
        let mut document = DocumentModel::new();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(400, 400).unwrap();

        let fixed_middle = drag_elbow_arrow_segment(
            ElementId::default(),
            &base,
            2,
            Point::new(25.0, 0.0),
            &[],
            editor.arrow_engine_context(Modifiers::default()),
            true,
        )
        .arrow;
        let last_segment_index = fixed_middle.points.len() - 1;
        let last_midpoint = snow_draw_engine_document::arrow_segment_midpoints(&fixed_middle)
            .into_iter()
            .find_map(|(index, midpoint)| (index == last_segment_index).then_some(midpoint))
            .unwrap();
        let drag_target = Point::new(last_midpoint.x, last_midpoint.y + 30.0);
        let visible_drag = drag_elbow_arrow_segment(
            ElementId::default(),
            &fixed_middle,
            last_segment_index,
            drag_target,
            &[],
            editor.arrow_engine_context(Modifiers::default()),
            false,
        );
        assert!(validate_arrow(&visible_drag.arrow).is_ok());

        let arrow_id = document.peek_next_element_id();
        let mut insert = Transaction::new("insert fixed elbow arrow");
        insert.insert_arrow(arrow_id, ElementMeta::default(), fixed_middle.clone());
        document.apply_transaction(insert).unwrap();
        editor.state.interaction = InteractionState::EditingArrow(EditArrowState {
            pointer_id: 9,
            arrow_id,
            original_arrow: fixed_middle.clone(),
            preview_arrow: visible_drag.arrow.clone(),
            mode: ArrowEditMode::Segment(visible_drag.next_segment_index),
            start_canvas_position: last_midpoint,
            drag_offset: Point::default(),
        });
        let pointer_up = PointerEvent {
            pointer_id: 9,
            event_type: PointerEventType::Up,
            device: PointerDevice::Mouse,
            position: Point::new(200.0 + drag_target.x, 200.0 + drag_target.y),
            button: Some(PointerButton::Primary),
            buttons: PointerButtons::default(),
            modifiers: Modifiers::default(),
        };

        let update = editor
            .process_input(&document, InputEvent::Pointer(pointer_up))
            .unwrap();
        let Some(EditorCommand::ApplyTransaction(command)) = update.command else {
            panic!("releasing the last elbow segment should commit its edit");
        };
        document.apply_transaction(command.transaction).unwrap();

        assert_ne!(visible_drag.arrow, fixed_middle);
        assert_eq!(document.arrow(arrow_id).unwrap(), &visible_drag.arrow);
    }

    #[test]
    fn finalizing_bound_elbow_endpoint_keeps_the_visible_snapped_position() {
        let mut document = DocumentModel::new();
        let rectangle_id = document.peek_next_element_id();
        let mut insert = Transaction::new("insert bindable rectangle");
        insert.insert_rectangle(
            rectangle_id,
            ElementMeta::default(),
            RectangleData {
                rectangle_kind: RectangleElementKind::Rectangle,
                highlight_shape: HighlightShape::Rectangle,
                center: Point::new(0.0, 0.0),
                width: 100.0,
                height: 100.0,
                rotation: 0.0,
                fill: ColorRgba8::default(),
                fill_style: FillStyle::Solid,
                stroke: ColorRgba8::default(),
                stroke_width: 2.0,
                stroke_style: StrokeStyle::Solid,
                corner_radii: CornerRadii::default(),
                opacity: 1.0,
            },
        );
        document.apply_transaction(insert).unwrap();

        let arrow = ArrowData::from_global_points(
            &[Point::new(-150.0, 30.0), Point::new(-100.0, 30.0)],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Elbow,
            None,
            None,
        )
        .unwrap();
        let arrow_id = document.peek_next_element_id();
        let state = EditArrowState {
            pointer_id: 1,
            arrow_id,
            original_arrow: arrow.clone(),
            preview_arrow: arrow,
            mode: ArrowEditMode::Endpoint(ArrowEndpointEdge::End),
            start_canvas_position: Point::new(-100.0, 30.0),
            drag_offset: Point::default(),
        };
        let editor = Editor::new(EngineConfig::default()).unwrap();
        let bindables = editor.bindable_elements(&document, &[]);
        let context = editor.arrow_engine_context(Modifiers::default());
        let pointer = Point::new(-8.0, 8.0);
        let visible =
            editor.arrow_edit_preview(&document, &state, pointer, Modifiers::default(), false);
        let raw_final = compute_arrow_endpoint_drag(
            arrow_id,
            &state.original_arrow,
            ArrowEndpointEdge::End,
            pointer,
            &bindables,
            context,
            ArrowEndpointDragOptions {
                finalize: true,
                ..ArrowEndpointDragOptions::default()
            },
        )
        .arrow;
        assert!(visible.arrow.end_binding.is_some());
        assert!(raw_final.end_binding.is_some());
        assert!(
            point_distance(visible.arrow.end(), raw_final.end()) > 1e-6,
            "the fixture must include a release-only bound endpoint offset"
        );
        let mut final_state = state.clone();
        final_state.preview_arrow = visible.arrow.clone();
        let finalized =
            editor.arrow_edit_preview(&document, &final_state, pointer, Modifiers::default(), true);

        let visible_binding = visible.arrow.end_binding.as_ref().unwrap();
        let finalized_binding = finalized.arrow.end_binding.as_ref().unwrap();
        assert_eq!(finalized_binding.element_id, visible_binding.element_id);
        assert_eq!(finalized_binding.mode, visible_binding.mode);
        assert!((finalized_binding.fixed_point[0] - visible_binding.fixed_point[0]).abs() < 1e-9);
        assert!((finalized_binding.fixed_point[1] - visible_binding.fixed_point[1]).abs() < 1e-9);
        let visible_endpoint = visible.arrow.end();
        let finalized_endpoint = finalized.arrow.end();
        assert!(
            point_distance(finalized_endpoint, visible_endpoint) < 1e-9,
            "the committed bound endpoint must not jump back to the pointer"
        );
        assert_ne!(
            visible_endpoint, pointer,
            "the fixture must exercise binding auto-snap away from the pointer"
        );
    }
}

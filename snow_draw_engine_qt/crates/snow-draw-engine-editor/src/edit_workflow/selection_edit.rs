use super::*;
use crate::text::{
    TextResizeLayoutOverride, text_resize_layout_override_matches_rect,
    text_resize_measurement_font_size, text_resize_measurement_requested_values,
};
use snow_draw_engine_document::{
    MIN_TEXT_FONT_SIZE, serial_number_minimum_selection_scale, serial_number_rect_proxy,
    serial_number_with_selection_rect,
};

#[derive(Clone, Copy, Debug, PartialEq)]
struct SelectionResizePolicy {
    force_aspect_lock: bool,
    allow_flip: bool,
    minimum_scale: f64,
    derive_bounds_from_members: bool,
}

impl Editor {
    pub(crate) fn begin_selection_edit_state(
        &self,
        request: BeginSelectionEditRequest,
    ) -> EditSelectionState {
        let original_element_count = request.original_elements.len();
        let original_arrow_count = request.original_arrows.len();
        let mode = match request.target {
            SelectionHitTarget::Move => SelectionEditMode::Move {
                start_canvas_position: request.canvas_point,
            },
            SelectionHitTarget::Resize(handle) => {
                let frame_padding = request.frame_padding_override.unwrap_or_else(|| {
                    selection_frame_padding_for_members(
                        self.camera().zoom,
                        original_element_count,
                        original_arrow_count,
                    )
                });
                let corner_handle_outset = selection_corner_handle_outset_for_members(
                    self.camera().zoom,
                    original_element_count,
                    original_arrow_count,
                );
                let handle_center = selection_resize_handle_center_for_handle(
                    &request.original_bounds,
                    frame_padding,
                    corner_handle_outset,
                    handle,
                );
                SelectionEditMode::Resize {
                    handle,
                    handle_offset_canvas: Point {
                        x: request.canvas_point.x - handle_center.x,
                        y: request.canvas_point.y - handle_center.y,
                    },
                    frame_padding,
                    corner_handle_outset,
                    scale_from_center: false,
                    text_layout_override: None,
                }
            }
            SelectionHitTarget::Rotate => SelectionEditMode::Rotate {
                start_pointer_angle: angle_between(
                    request.original_bounds.center,
                    request.canvas_point,
                ),
            },
            SelectionHitTarget::CornerRadius(corner) => SelectionEditMode::CornerRadius { corner },
        };

        EditSelectionState {
            pointer_id: request.pointer_id,
            preview_elements: request.original_elements.clone(),
            preview_arrows: request.original_arrows.clone(),
            original_elements: request.original_elements,
            original_arrows: request.original_arrows,
            original_bounds: request.original_bounds,
            preview_bounds: request.original_bounds,
            mode,
        }
    }

    pub(crate) fn update_selection_edit_preview(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) {
        let InteractionState::EditingSelection(state) = &self.state.interaction else {
            return;
        };

        let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let next_preview = self.selection_edit_preview(
            document,
            state,
            canvas_point,
            event.modifiers,
            self.camera().zoom,
        );
        let preview_changed = next_preview.elements != state.preview_elements
            || next_preview.arrows != state.preview_arrows
            || next_preview.bounds != state.preview_bounds;
        let guides_changed = next_preview.snap_guides != self.state.ui.snap_guides;
        if !preview_changed && !guides_changed {
            return;
        }

        if let InteractionState::EditingSelection(active) = &mut self.state.interaction {
            if let SelectionEditMode::Resize {
                scale_from_center, ..
            } = &mut active.mode
            {
                *scale_from_center = event.modifiers.alt;
            }
            active.preview_elements = next_preview.elements.clone();
            active.preview_arrows = next_preview.arrows.clone();
            active.preview_bounds = next_preview.bounds;
        }
        if preview_changed {
            self.bump_scene_state_revision();
        }
        if guides_changed {
            self.set_snap_guides(next_preview.snap_guides);
        } else if preview_changed {
            self.bump_overlay_state_revision();
        }
    }

    pub(crate) fn commit_selection_edit(
        &mut self,
        document: &DocumentModel,
    ) -> Result<(), ErrorCode> {
        let state = match std::mem::replace(&mut self.state.interaction, InteractionState::Idle) {
            InteractionState::EditingSelection(state) => state,
            other => {
                self.state.interaction = other;
                return Ok(());
            }
        };

        self.clear_transient_visuals();
        if state.preview_elements == state.original_elements
            && state.preview_arrows == state.original_arrows
        {
            return Ok(());
        }

        let history_undo_snapshot = self.capture_document_sync_snapshot(document);

        let selected_arrow_ids = state
            .preview_arrows
            .iter()
            .map(|arrow| arrow.id)
            .collect::<Vec<_>>();
        let recomputed_arrows = self
            .recompute_bound_arrows(document, &state.preview_elements)
            .into_iter()
            .filter(|(arrow_id, _, _)| !selected_arrow_ids.contains(arrow_id))
            .collect::<Vec<_>>();
        let preview_elements = state.preview_elements.clone();
        let preview_arrows = state.preview_arrows.clone();
        let active_text_id = self.active_text_draft_existing_id();
        let active_text_original_rect = active_text_id.and_then(|id| {
            state
                .original_elements
                .iter()
                .find(|element| element.id == id)
                .map(|element| element.rect)
        });
        let mut transaction = Transaction::new(selection_edit_label(state.mode));
        let (resize_handle, text_resize_layout_override) = match state.mode {
            SelectionEditMode::Resize {
                handle,
                text_layout_override,
                ..
            } => (Some(handle), text_layout_override),
            _ => (None, None),
        };
        let single_text_resize = resize_handle.is_some()
            && state.original_arrows.is_empty()
            && state.original_elements.len() == 1
            && document.text(state.original_elements[0].id).is_ok();
        let single_text_resize_font_size = if single_text_resize
            && state.preview_elements.len() == 1
            && let Some(layout_override) = text_resize_layout_override
            && text_resize_layout_override_matches_rect(
                layout_override,
                state.preview_elements[0].rect,
            ) {
            Some(layout_override.requested_font_size)
        } else {
            None
        };
        for preview in state.preview_elements {
            if Some(preview.id) == active_text_id {
                self.update_active_text_draft_selection_rect(
                    active_text_original_rect.unwrap_or(preview.rect),
                    preview.rect,
                    resize_handle,
                    single_text_resize,
                    single_text_resize_font_size,
                )?;
                continue;
            }
            append_selection_element_update(
                &mut transaction,
                document,
                preview,
                resize_handle,
                single_text_resize,
                single_text_resize_font_size,
            )?;
        }
        for preview in state.preview_arrows {
            transaction.update_arrow(preview.id, preview.arrow);
        }
        for (arrow_id, arrow, reorder_targets) in recomputed_arrows {
            transaction.update_arrow(arrow_id, arrow);
            self.append_arrow_reorder_targets(
                &mut transaction,
                document,
                arrow_id,
                &reorder_targets,
            );
        }
        self.state.selection.bounds = Some(state.preview_bounds);
        self.state.selection.elements = preview_elements;
        self.state.selection.arrows = preview_arrows;
        if !transaction.is_empty() {
            self.queue_command(EditorCommand::ApplyTransaction(
                ApplyTransactionCommand::with_history_undo_snapshot(
                    transaction,
                    history_undo_snapshot,
                ),
            ));
        }
        Ok(())
    }

    pub(crate) fn selection_edit_preview(
        &self,
        document: &DocumentModel,
        state: &EditSelectionState,
        canvas_point: Point<f64>,
        modifiers: Modifiers,
        zoom: f64,
    ) -> SelectionEditPreview {
        match state.mode {
            SelectionEditMode::Move {
                start_canvas_position,
            } => {
                let delta_x = canvas_point.x - start_canvas_position.x;
                let delta_y = canvas_point.y - start_canvas_position.y;
                let snapped = self.resolve_move_snap(MoveSnapRequest {
                    document,
                    original_bounds: &state.original_bounds,
                    original_elements: &state.original_elements,
                    original_arrows: &state.original_arrows,
                    base_dx: delta_x,
                    base_dy: delta_y,
                    modifiers,
                });
                SelectionEditPreview {
                    elements: state
                        .original_elements
                        .iter()
                        .map(|element| SelectionRectState {
                            id: element.id,
                            rect: RectangleData {
                                center: Point {
                                    x: element.rect.center.x + snapped.dx,
                                    y: element.rect.center.y + snapped.dy,
                                },
                                ..element.rect
                            },
                        })
                        .collect(),
                    arrows: state
                        .original_arrows
                        .iter()
                        .map(|arrow| SelectionArrowState {
                            id: arrow.id,
                            arrow: translated_arrow_for_move(
                                &arrow.arrow,
                                Point::new(snapped.dx, snapped.dy),
                            ),
                        })
                        .collect(),
                    bounds: SelectionBounds {
                        center: Point {
                            x: state.original_bounds.center.x + snapped.dx,
                            y: state.original_bounds.center.y + snapped.dy,
                        },
                        ..state.original_bounds
                    },
                    snap_guides: snapped.guides,
                }
            }
            SelectionEditMode::Resize {
                handle,
                handle_offset_canvas,
                frame_padding,
                corner_handle_outset,
                text_layout_override,
                ..
            } => self.resize_selection_preview(
                document,
                ResizeSelectionContext {
                    original_elements: &state.original_elements,
                    original_arrows: &state.original_arrows,
                    original_bounds: &state.original_bounds,
                    handle,
                    handle_offset_canvas,
                    frame_padding,
                    corner_handle_outset,
                },
                canvas_point,
                modifiers,
                text_layout_override,
            ),
            SelectionEditMode::Rotate {
                start_pointer_angle,
            } => {
                let raw_rotation_delta =
                    angle_between(state.original_bounds.center, canvas_point) - start_pointer_angle;
                let rotation_delta = if modifiers.shift {
                    lock_rotation_to_discrete_angle(
                        state.original_bounds.rotation + raw_rotation_delta,
                    ) - state.original_bounds.rotation
                } else {
                    raw_rotation_delta
                };
                SelectionEditPreview {
                    elements: state
                        .original_elements
                        .iter()
                        .map(|element| SelectionRectState {
                            id: element.id,
                            rect: RectangleData {
                                center: rotate_point_around(
                                    element.rect.center,
                                    state.original_bounds.center,
                                    rotation_delta,
                                ),
                                rotation: normalize_rotation(
                                    element.rect.rotation + rotation_delta,
                                ),
                                ..element.rect
                            },
                        })
                        .collect(),
                    arrows: state
                        .original_arrows
                        .iter()
                        .filter_map(|arrow| {
                            rotated_arrow_for_selection(
                                &arrow.arrow,
                                state.original_bounds.center,
                                rotation_delta,
                            )
                            .map(|preview| SelectionArrowState {
                                id: arrow.id,
                                arrow: preview,
                            })
                        })
                        .collect(),
                    bounds: SelectionBounds {
                        rotation: normalize_rotation(
                            state.original_bounds.rotation + rotation_delta,
                        ),
                        ..state.original_bounds
                    },
                    snap_guides: Vec::new(),
                }
            }
            SelectionEditMode::CornerRadius { corner } => SelectionEditPreview {
                elements: preview_elements_for_corner_radius(
                    &state.original_elements,
                    corner,
                    canvas_point,
                    modifiers.shift,
                    zoom,
                ),
                arrows: state.original_arrows.clone(),
                bounds: state.original_bounds,
                snap_guides: Vec::new(),
            },
        }
    }

    pub(crate) fn resize_selection_preview(
        &self,
        document: &DocumentModel,
        context: ResizeSelectionContext<'_>,
        canvas_point: Point<f64>,
        modifiers: Modifiers,
        text_layout_override: Option<TextResizeLayoutOverride>,
    ) -> SelectionEditPreview {
        let (snapped_canvas_point, snap_guides) = self.resolve_resize_snap(
            document,
            context,
            canvas_point,
            modifiers,
            text_layout_override,
        );
        let policy = self.selection_resize_policy(
            document,
            context.original_elements,
            context.original_arrows,
        );
        let geometry = resize_drag_geometry(ResizeDragGeometryRequest {
            original_elements: context.original_elements,
            original_bounds: context.original_bounds,
            handle: context.handle,
            handle_offset_canvas: context.handle_offset_canvas,
            frame_padding: context.frame_padding,
            corner_handle_outset: context.corner_handle_outset,
            canvas_point: snapped_canvas_point,
            modifiers,
            force_aspect_lock: policy.force_aspect_lock,
            allow_flip: policy.allow_flip,
            minimum_scale: policy.minimum_scale,
        });
        let (elements, arrows) = self.resized_selection_members(
            document,
            context,
            geometry,
            modifiers,
            text_layout_override,
        );
        let transformed_bounds =
            resized_selection_bounds_from_drag_geometry(context.original_bounds, geometry);
        let bounds = if policy.derive_bounds_from_members {
            selection_bounds_from_selection(&elements, &arrows).unwrap_or(transformed_bounds)
        } else {
            transformed_bounds
        };

        SelectionEditPreview {
            elements,
            arrows,
            bounds,
            snap_guides,
        }
    }

    fn resized_selection_members(
        &self,
        document: &DocumentModel,
        context: ResizeSelectionContext<'_>,
        geometry: ResizeDragGeometry,
        modifiers: Modifiers,
        text_layout_override: Option<TextResizeLayoutOverride>,
    ) -> (Vec<SelectionRectState>, Vec<SelectionArrowState>) {
        if let Some(text_preview) = self.single_text_resize_preview(
            document,
            context,
            geometry,
            modifiers,
            text_layout_override,
        ) {
            return (vec![text_preview], Vec::new());
        }

        let elements = context
            .original_elements
            .iter()
            .map(|element| {
                let requested_rect = resized_selection_rect(
                    &element.rect,
                    context.original_bounds,
                    geometry.anchor_local,
                    geometry.scale_x,
                    geometry.scale_y,
                );
                let rect = document
                    .serial_number(element.id)
                    .ok()
                    .map(|serial| {
                        serial_number_rect_proxy(&serial_number_with_selection_rect(
                            serial,
                            requested_rect,
                        ))
                    })
                    .unwrap_or(requested_rect);
                SelectionRectState {
                    id: element.id,
                    rect,
                }
            })
            .collect();
        let arrows = context
            .original_arrows
            .iter()
            .filter_map(|arrow| {
                resized_arrow_for_selection(
                    &arrow.arrow,
                    context.original_bounds,
                    geometry.anchor_local,
                    geometry.scale_x,
                    geometry.scale_y,
                )
                .map(|preview| SelectionArrowState {
                    id: arrow.id,
                    arrow: preview,
                })
            })
            .collect();
        (elements, arrows)
    }

    pub(crate) fn resolve_resize_snap(
        &self,
        document: &DocumentModel,
        context: ResizeSelectionContext<'_>,
        canvas_point: Point<f64>,
        modifiers: Modifiers,
        text_layout_override: Option<TextResizeLayoutOverride>,
    ) -> (Point<f64>, Vec<SnapGuide>) {
        let snapping_mode = self.effective_snapping_mode(modifiers);
        if snapping_mode == SnappingMode::None
            || context.original_bounds.rotation.abs() > f64::EPSILON
            || modifiers.alt
        {
            return (canvas_point, Vec::new());
        }

        let adjusted_handle_canvas = Point {
            x: canvas_point.x - context.handle_offset_canvas.x,
            y: canvas_point.y - context.handle_offset_canvas.y,
        };
        let handle_local_offset = selection_resize_handle_local_offset(
            context.handle,
            context.frame_padding,
            context.corner_handle_outset,
        );
        let handle_padding_canvas =
            rotate_vector(handle_local_offset, context.original_bounds.rotation);
        let actual_handle_canvas = Point {
            x: adjusted_handle_canvas.x - handle_padding_canvas.x,
            y: adjusted_handle_canvas.y - handle_padding_canvas.y,
        };

        let policy = self.selection_resize_policy(
            document,
            context.original_elements,
            context.original_arrows,
        );
        let geometry = resize_drag_geometry(ResizeDragGeometryRequest {
            original_elements: context.original_elements,
            original_bounds: context.original_bounds,
            handle: context.handle,
            handle_offset_canvas: context.handle_offset_canvas,
            frame_padding: context.frame_padding,
            corner_handle_outset: context.corner_handle_outset,
            canvas_point,
            modifiers,
            force_aspect_lock: policy.force_aspect_lock,
            allow_flip: policy.allow_flip,
            minimum_scale: policy.minimum_scale,
        });
        let (unsnapped_elements, unsnapped_arrows) = self.resized_selection_members(
            document,
            context,
            geometry,
            modifiers,
            text_layout_override,
        );
        let transformed_bounds =
            resized_selection_bounds_from_drag_geometry(context.original_bounds, geometry);
        let unsnapped_bounds = if policy.derive_bounds_from_members {
            selection_bounds_from_selection(&unsnapped_elements, &unsnapped_arrows)
                .unwrap_or(transformed_bounds)
        } else {
            transformed_bounds
        };
        let unsnapped_rect = selection_bounds_to_draw_rect(&unsnapped_bounds);
        let dragged_x_sign = context.handle.x_sign() * geometry.scale_x;
        let dragged_y_sign = context.handle.y_sign() * geometry.scale_y;
        let snap_min_x = dragged_x_sign < -f64::EPSILON;
        let snap_max_x = dragged_x_sign > f64::EPSILON;
        let snap_min_y = dragged_y_sign < -f64::EPSILON;
        let snap_max_y = dragged_y_sign > f64::EPSILON;

        let (snapped_rect, snap_guides) = match snapping_mode {
            SnappingMode::Grid if !modifiers.shift => (
                GRID_SNAP_SERVICE.snap_rect(
                    unsnapped_rect,
                    self.config.grid.size,
                    snap_min_x,
                    snap_max_x,
                    snap_min_y,
                    snap_max_y,
                ),
                Vec::new(),
            ),
            SnappingMode::Object if self.config.snap.enable_point_snaps => {
                let excluded = context
                    .original_elements
                    .iter()
                    .map(|element| element.id)
                    .collect::<Vec<_>>();
                let anchors_x = resize_snap_anchors_for_sign(dragged_x_sign);
                let anchors_y = resize_snap_anchors_for_sign(dragged_y_sign);
                let snap_result = OBJECT_SNAP_SERVICE.snap_resize(
                    unsnapped_rect,
                    &Self::visible_reference_rects(document, &excluded),
                    self.zoom_adjusted_snap_distance(),
                    &anchors_x,
                    &anchors_y,
                    self.config.snap.enable_point_snaps,
                );
                (
                    DrawRect::new(
                        unsnapped_rect.min_x + if snap_min_x { snap_result.dx } else { 0.0 },
                        unsnapped_rect.min_y + if snap_min_y { snap_result.dy } else { 0.0 },
                        unsnapped_rect.max_x + if snap_max_x { snap_result.dx } else { 0.0 },
                        unsnapped_rect.max_y + if snap_max_y { snap_result.dy } else { 0.0 },
                    ),
                    if self.config.snap.show_guides {
                        snap_result.guides
                    } else {
                        Vec::new()
                    },
                )
            }
            _ => (unsnapped_rect, Vec::new()),
        };
        let snapped_handle_canvas = Point {
            x: actual_handle_canvas.x
                + if snap_min_x {
                    snapped_rect.min_x - unsnapped_rect.min_x
                } else if snap_max_x {
                    snapped_rect.max_x - unsnapped_rect.max_x
                } else {
                    0.0
                },
            y: actual_handle_canvas.y
                + if snap_min_y {
                    snapped_rect.min_y - unsnapped_rect.min_y
                } else if snap_max_y {
                    snapped_rect.max_y - unsnapped_rect.max_y
                } else {
                    0.0
                },
        };

        (
            Point {
                x: snapped_handle_canvas.x
                    + handle_padding_canvas.x
                    + context.handle_offset_canvas.x,
                y: snapped_handle_canvas.y
                    + handle_padding_canvas.y
                    + context.handle_offset_canvas.y,
            },
            snap_guides,
        )
    }

    fn selection_resize_policy(
        &self,
        document: &DocumentModel,
        elements: &[SelectionRectState],
        arrows: &[SelectionArrowState],
    ) -> SelectionResizePolicy {
        // A single text box owns its reflow/measurement behavior. Once text is
        // grouped, its rectangle and font must share the group's uniform scale.
        let single_text_resize =
            arrows.is_empty() && elements.len() == 1 && document.text(elements[0].id).is_ok();
        let contains_text = elements
            .iter()
            .any(|element| document.text(element.id).is_ok());
        let contains_serial_number = elements
            .iter()
            .any(|element| document.serial_number(element.id).is_ok());
        let scales_text = contains_text && !single_text_resize;
        let minimum_scale = elements
            .iter()
            .filter_map(|element| {
                if let Ok(serial) = document.serial_number(element.id) {
                    return Some(serial_number_minimum_selection_scale(serial));
                }
                if !scales_text {
                    return None;
                }
                let text = self
                    .active_text_draft_text_for_id(element.id)
                    .or_else(|| document.text(element.id).ok().cloned())?;
                Some(minimum_text_selection_scale(text.font_size))
            })
            .fold(0.0, f64::max);

        SelectionResizePolicy {
            force_aspect_lock: contains_serial_number || scales_text,
            allow_flip: !contains_text && !contains_serial_number,
            minimum_scale,
            derive_bounds_from_members: contains_text
                || contains_serial_number
                || !arrows.is_empty(),
        }
    }

    pub(crate) fn single_text_resize_preview(
        &self,
        document: &DocumentModel,
        context: ResizeSelectionContext<'_>,
        geometry: ResizeDragGeometry,
        modifiers: Modifiers,
        text_layout_override: Option<TextResizeLayoutOverride>,
    ) -> Option<SelectionRectState> {
        if context.original_elements.len() != 1 || !context.original_arrows.is_empty() {
            return None;
        }
        let element = context.original_elements[0];
        let text = self
            .active_text_draft_text_for_id(element.id)
            .or_else(|| document.text(element.id).ok().cloned())?;
        let rect = text_resize_preview_rect(
            &text,
            context.original_bounds,
            context.handle,
            geometry,
            modifiers.alt,
            text_layout_override,
        )?;
        Some(SelectionRectState {
            id: element.id,
            rect,
        })
    }

    pub fn active_text_resize_measurement_request(
        &self,
        document: &DocumentModel,
    ) -> Option<TextResizeMeasurementRequest> {
        let InteractionState::EditingSelection(state) = &self.state.interaction else {
            return None;
        };
        let SelectionEditMode::Resize {
            handle,
            text_layout_override,
            ..
        } = state.mode
        else {
            return None;
        };
        let changes_width_only = text_resize_changes_width_only(handle);
        if !state.original_arrows.is_empty()
            || state.original_elements.len() != 1
            || state.preview_elements.len() != 1
        {
            return None;
        }
        let element = state.original_elements[0];
        let preview = state.preview_elements[0];
        if preview.id != element.id {
            return None;
        }
        let text = self
            .active_text_draft_text_for_id(element.id)
            .or_else(|| document.text(element.id).ok().cloned())?;
        let font_size = text_resize_measurement_font_size(
            text.font_size,
            element.rect,
            preview.rect,
            changes_width_only,
            text_layout_override,
        )?;
        Some(TextResizeMeasurementRequest {
            id: element.id,
            center: preview.rect.center,
            width: preview.rect.width,
            height: preview.rect.height,
            rotation: preview.rect.rotation,
            text: text.text.clone(),
            font_size,
            font_family: text.font_family.clone(),
            auto_resize: text.auto_resize,
            measure_natural_width: text.auto_resize && !changes_width_only,
        })
    }

    pub fn apply_active_text_resize_measurement(
        &mut self,
        document: &DocumentModel,
        layout: TextLayoutSize,
    ) -> Result<bool, ErrorCode> {
        let layout = validate_text_layout_size(layout)?;
        let mut changed = false;
        let mut applied = false;
        let active_text_snapshot = self.state.active_text_draft.clone();
        if let InteractionState::EditingSelection(state) = &mut self.state.interaction {
            let SelectionEditMode::Resize {
                handle,
                scale_from_center,
                text_layout_override,
                ..
            } = &mut state.mode
            else {
                return Ok(false);
            };
            let changes_width_only = text_resize_changes_width_only(*handle);
            if !state.original_arrows.is_empty()
                || state.original_elements.len() != 1
                || state.preview_elements.len() != 1
            {
                return Ok(false);
            }
            let element = state.original_elements[0];
            let Some(text) = active_text_snapshot
                .as_ref()
                .filter(|draft| draft.existing_id() == Some(element.id))
                .map(|draft| draft.text.clone())
                .or_else(|| document.text(element.id).ok().cloned())
            else {
                return Ok(false);
            };
            if state.preview_elements[0].id != element.id {
                return Ok(false);
            }
            let requested_rect = state.preview_elements[0].rect;
            let existing_text_layout_override = *text_layout_override;
            let requested_font_size = text_resize_measurement_font_size(
                text.font_size,
                element.rect,
                requested_rect,
                changes_width_only,
                existing_text_layout_override,
            )
            .ok_or(ErrorCode::InvalidArgument)?;
            let requested = text_resize_measurement_requested_values(
                requested_rect,
                requested_font_size,
                existing_text_layout_override,
            );
            if changes_width_only && layout.width + 1e-3 < requested_rect.width {
                return Ok(false);
            }

            let rect = resized_text_rect_from_size(
                &element.rect,
                &state.original_bounds,
                text_resize_anchor(*handle, *scale_from_center),
                layout.width,
                layout.height,
            );
            let next_elements = vec![SelectionRectState {
                id: element.id,
                rect,
            }];
            let next_bounds =
                selection_bounds_from_elements(&next_elements).unwrap_or(state.original_bounds);
            changed =
                state.preview_elements != next_elements || state.preview_bounds != next_bounds;
            state.preview_elements = next_elements;
            state.preview_arrows.clear();
            state.preview_bounds = next_bounds;
            *text_layout_override = Some(TextResizeLayoutOverride {
                requested_width: requested.width,
                requested_height: requested.height,
                requested_font_size: requested.font_size,
                layout,
            });
            applied = true;
        }

        if changed {
            self.bump_scene_state_revision();
            self.bump_overlay_state_revision();
        }
        Ok(applied)
    }
}

fn minimum_text_selection_scale(font_size: f64) -> f64 {
    if font_size.is_finite() && font_size > f64::EPSILON {
        MIN_TEXT_FONT_SIZE / font_size
    } else {
        1.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ActiveTextDraftPresentation, ActiveTextDraftTarget};
    use snow_draw_engine_core::{
        ColorRgba8, CornerRadii, EngineConfig,
        arrow::{ArrowStrokeStyle, ArrowType, Arrowhead, BindMode},
    };
    use snow_draw_engine_document::{
        ArrowData, ArrowEndpointBinding, ElementData, ElementMeta, MIN_SERIAL_NUMBER_FONT_SIZE,
        Operation, SerialNumberData, Transaction,
    };

    fn text_rect(text: &TextData) -> RectangleData {
        RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: text.center,
            width: text.width,
            height: text.height,
            rotation: text.rotation,
            fill: text.fill,
            fill_style: text.fill_style,
            stroke: text.stroke,
            stroke_width: text.stroke_width,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: text.corner_radii,
            opacity: text.opacity,
        }
    }

    fn assert_close(left: f64, right: f64) {
        assert!(
            (left - right).abs() <= 1e-9,
            "expected {left} to be close to {right}"
        );
    }

    #[test]
    fn shift_rotation_snaps_absolute_selection_angle_to_fifteen_degrees() {
        let initial_rotation = 10.0_f64.to_radians();
        let rect = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(0.0, 0.0),
            width: 40.0,
            height: 20.0,
            rotation: initial_rotation,
            fill: ColorRgba8::default(),
            fill_style: snow_draw_engine_document::FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        let state = EditSelectionState {
            pointer_id: 1,
            original_elements: vec![SelectionRectState {
                id: ElementId::default(),
                rect,
            }],
            preview_elements: vec![SelectionRectState {
                id: ElementId::default(),
                rect,
            }],
            original_arrows: Vec::new(),
            preview_arrows: Vec::new(),
            original_bounds: SelectionBounds {
                center: rect.center,
                width: rect.width,
                height: rect.height,
                rotation: rect.rotation,
            },
            preview_bounds: SelectionBounds {
                center: rect.center,
                width: rect.width,
                height: rect.height,
                rotation: rect.rotation,
            },
            mode: SelectionEditMode::Rotate {
                start_pointer_angle: 0.0,
            },
        };
        let editor = Editor::new(EngineConfig::default()).unwrap();
        let drag_angle = 10.0_f64.to_radians();
        let preview = editor.selection_edit_preview(
            &DocumentModel::new(),
            &state,
            Point::new(drag_angle.cos(), drag_angle.sin()),
            Modifiers {
                shift: true,
                ..Modifiers::default()
            },
            1.0,
        );

        assert_close(preview.bounds.rotation, 15.0_f64.to_radians());
        assert_close(preview.elements[0].rect.rotation, 15.0_f64.to_radians());
    }

    fn insert_text(document: &mut DocumentModel, text: TextData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert text");
        transaction.insert_text(id, ElementMeta::default(), text);
        document.apply_transaction(transaction).unwrap();
        id
    }

    fn insert_rectangle(document: &mut DocumentModel, rect: RectangleData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert rectangle");
        transaction.insert_rectangle(id, ElementMeta::default(), rect);
        document.apply_transaction(transaction).unwrap();
        id
    }

    fn insert_serial_number(document: &mut DocumentModel, serial: SerialNumberData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert serial number");
        transaction.insert_serial_number(id, ElementMeta::default(), serial);
        document.apply_transaction(transaction).unwrap();
        id
    }

    fn insert_arrow(document: &mut DocumentModel, arrow: ArrowData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert arrow");
        transaction.insert_arrow(id, ElementMeta::default(), arrow);
        document.apply_transaction(transaction).unwrap();
        id
    }

    #[test]
    fn selection_resize_policy_preserves_intrinsic_text_and_serial_geometry() {
        let mut document = DocumentModel::new();
        let rectangle = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(0.0, 0.0),
            width: 100.0,
            height: 40.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: snow_draw_engine_document::FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        let rectangle_id = insert_rectangle(&mut document, rectangle);
        let second_rectangle_id = insert_rectangle(
            &mut document,
            RectangleData {
                center: Point::new(150.0, 0.0),
                ..rectangle
            },
        );
        let text = TextData {
            center: Point::new(300.0, 0.0),
            width: 100.0,
            height: 40.0,
            text: "text".to_owned(),
            auto_resize: false,
            ..TextData::default()
        };
        let text_id = insert_text(&mut document, text.clone());
        let serial = SerialNumberData {
            center: Point::new(450.0, 0.0),
            diameter: 40.0,
            font_size: 16.0,
            ..SerialNumberData::default()
        };
        let serial_id = insert_serial_number(&mut document, serial.clone());
        let arrow = ArrowData::from_global_points(
            &[Point::new(0.0, 0.0), Point::new(100.0, 0.0)],
            ColorRgba8::default(),
            2.0,
            ArrowStrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        let arrow_id = insert_arrow(&mut document, arrow.clone());
        let rectangle_state = SelectionRectState {
            id: rectangle_id,
            rect: rectangle,
        };
        let second_rectangle_state = SelectionRectState {
            id: second_rectangle_id,
            rect: RectangleData {
                center: Point::new(150.0, 0.0),
                ..rectangle
            },
        };
        let text_state = SelectionRectState {
            id: text_id,
            rect: text_rect(&text),
        };
        let serial_state = SelectionRectState {
            id: serial_id,
            rect: serial_number_rect_proxy(&serial),
        };
        let arrows = [SelectionArrowState {
            id: arrow_id,
            arrow,
        }];
        let editor = Editor::new(EngineConfig::default()).unwrap();

        assert!(
            editor
                .selection_resize_policy(&document, &[rectangle_state], &arrows)
                .allow_flip
        );
        assert!(
            editor
                .selection_resize_policy(&document, &[rectangle_state], &arrows)
                .derive_bounds_from_members
        );
        assert!(
            editor
                .selection_resize_policy(
                    &document,
                    &[rectangle_state, second_rectangle_state],
                    &[],
                )
                .allow_flip
        );
        let single_text_policy = editor.selection_resize_policy(&document, &[text_state], &[]);
        assert!(!single_text_policy.allow_flip);
        assert!(!single_text_policy.force_aspect_lock);
        assert_close(single_text_policy.minimum_scale, 0.0);

        let mixed_text_policy =
            editor.selection_resize_policy(&document, &[rectangle_state, text_state], &[]);
        assert!(!mixed_text_policy.allow_flip);
        assert!(mixed_text_policy.force_aspect_lock);
        assert_close(
            mixed_text_policy.minimum_scale,
            MIN_TEXT_FONT_SIZE / text.font_size,
        );

        let single_serial_policy = editor.selection_resize_policy(&document, &[serial_state], &[]);
        assert!(!single_serial_policy.allow_flip);
        assert!(single_serial_policy.force_aspect_lock);
        assert_close(
            single_serial_policy.minimum_scale,
            MIN_SERIAL_NUMBER_FONT_SIZE / serial.font_size,
        );

        let mixed_serial_policy =
            editor.selection_resize_policy(&document, &[rectangle_state, serial_state], &[]);
        assert!(!mixed_serial_policy.allow_flip);
        assert!(mixed_serial_policy.force_aspect_lock);
        assert_close(
            mixed_serial_policy.minimum_scale,
            MIN_SERIAL_NUMBER_FONT_SIZE / serial.font_size,
        );
    }

    #[test]
    fn serial_number_resize_preview_stops_at_minimum_font_size() {
        let mut document = DocumentModel::new();
        let serial = SerialNumberData {
            center: Point::new(0.0, 0.0),
            diameter: 40.0,
            font_size: 16.0,
            ..SerialNumberData::default()
        };
        let serial_id = insert_serial_number(&mut document, serial.clone());
        let original_rect = serial_number_rect_proxy(&serial);
        let original_elements = [SelectionRectState {
            id: serial_id,
            rect: original_rect,
        }];
        let original_bounds = SelectionBounds {
            center: original_rect.center,
            width: original_rect.width,
            height: original_rect.height,
            rotation: original_rect.rotation,
        };
        let editor = Editor::new(EngineConfig::default()).unwrap();

        let preview = editor.resize_selection_preview(
            &document,
            ResizeSelectionContext {
                original_elements: &original_elements,
                original_arrows: &[],
                original_bounds: &original_bounds,
                handle: ResizeHandle::Right,
                handle_offset_canvas: Point::default(),
                frame_padding: 0.0,
                corner_handle_outset: 0.0,
            },
            Point::new(-18.0, 0.0),
            Modifiers::default(),
            None,
        );

        let preview_rect = preview.elements[0].rect;
        assert_close(preview_rect.width, 15.0);
        assert_close(preview_rect.height, 15.0);
        assert_close(preview.bounds.width, 15.0);
        assert_close(preview.bounds.height, 15.0);
        assert_close(
            serial_number_with_selection_rect(&serial, preview_rect).font_size,
            MIN_SERIAL_NUMBER_FONT_SIZE,
        );
    }

    #[test]
    fn arrow_resize_preview_matches_committed_selection_geometry() {
        let mut document = DocumentModel::new();
        let arrow = ArrowData::from_global_points(
            &[Point::new(0.0, 0.0), Point::new(100.0, 0.0)],
            ColorRgba8::default(),
            2.0,
            ArrowStrokeStyle::Solid,
            ArrowType::Straight,
            None,
            Some(Arrowhead::Arrow),
        )
        .unwrap();
        let arrow_id = insert_arrow(&mut document, arrow.clone());
        let original_arrows = [SelectionArrowState {
            id: arrow_id,
            arrow,
        }];
        let original_bounds = selection_bounds_from_selection(&[], &original_arrows).unwrap();
        let editor = Editor::new(EngineConfig::default()).unwrap();

        let preview = editor.resize_selection_preview(
            &document,
            ResizeSelectionContext {
                original_elements: &[],
                original_arrows: &original_arrows,
                original_bounds: &original_bounds,
                handle: ResizeHandle::Right,
                handle_offset_canvas: Point::default(),
                frame_padding: 0.0,
                corner_handle_outset: 0.0,
            },
            original_bounds.center,
            Modifiers::default(),
            None,
        );
        let expected_preview_bounds =
            selection_bounds_from_selection(&preview.elements, &preview.arrows).unwrap();

        assert_eq!(preview.bounds, expected_preview_bounds);

        let mut transaction = Transaction::new("commit arrow resize");
        transaction.update_arrow(arrow_id, preview.arrows[0].arrow.clone());
        document.apply_transaction(transaction).unwrap();

        let committed = [SelectionArrowState {
            id: arrow_id,
            arrow: document.arrow(arrow_id).unwrap().clone(),
        }];
        let committed_bounds = selection_bounds_from_selection(&[], &committed).unwrap();
        assert_eq!(preview.bounds, committed_bounds);
    }

    #[test]
    fn mixed_serial_resize_preview_matches_committed_selection_geometry() {
        let mut document = DocumentModel::new();
        let rectangle = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(0.0, 0.0),
            width: 100.0,
            height: 40.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: snow_draw_engine_document::FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        let rectangle_id = insert_rectangle(&mut document, rectangle);
        let serial = SerialNumberData {
            center: Point::new(100.0, 0.0),
            diameter: 40.0,
            font_size: 16.0,
            stroke_width: 2.0,
            ..SerialNumberData::default()
        };
        let serial_id = insert_serial_number(&mut document, serial.clone());
        let original_elements = vec![
            SelectionRectState {
                id: rectangle_id,
                rect: rectangle,
            },
            SelectionRectState {
                id: serial_id,
                rect: serial_number_rect_proxy(&serial),
            },
        ];
        let original_bounds = selection_bounds_from_elements(&original_elements).unwrap();
        let editor = Editor::new(EngineConfig::default()).unwrap();

        let preview = editor.resize_selection_preview(
            &document,
            ResizeSelectionContext {
                original_elements: &original_elements,
                original_arrows: &[],
                original_bounds: &original_bounds,
                handle: ResizeHandle::Right,
                handle_offset_canvas: Point::default(),
                frame_padding: 0.0,
                corner_handle_outset: 0.0,
            },
            Point::new(35.0, 0.0),
            Modifiers::default(),
            None,
        );

        let rectangle_preview = preview
            .elements
            .iter()
            .find(|element| element.id == rectangle_id)
            .unwrap();
        let serial_preview = preview
            .elements
            .iter()
            .find(|element| element.id == serial_id)
            .unwrap();
        assert_close(rectangle_preview.rect.width, 50.0);
        assert_close(rectangle_preview.rect.height, 20.0);
        assert_close(serial_preview.rect.width, 20.0);
        assert_close(serial_preview.rect.height, 20.0);
        assert_close(serial_preview.rect.corner_radii.top_left, 10.0);
        assert_close(preview.bounds.width, 85.0);
        assert_close(preview.bounds.height, 20.0);

        let mut transaction = Transaction::new("commit mixed serial resize");
        for element in preview.elements.iter().copied() {
            append_selection_element_update(
                &mut transaction,
                &document,
                element,
                Some(ResizeHandle::Right),
                false,
                None,
            )
            .unwrap();
        }
        document.apply_transaction(transaction).unwrap();

        let committed = Editor::selection_elements_from_ids(&document, &[rectangle_id, serial_id]);
        let committed_bounds = selection_bounds_from_elements(&committed).unwrap();
        assert_eq!(committed, preview.elements);
        assert_eq!(committed_bounds, preview.bounds);
    }

    #[test]
    fn multi_text_resize_stops_at_largest_member_minimum_and_commits_without_box_jump() {
        let mut document = DocumentModel::new();
        let first_text = TextData {
            center: Point::new(-60.0, 0.0),
            width: 80.0,
            height: 20.0,
            text: "first".to_owned(),
            font_size: 12.0,
            auto_resize: false,
            ..TextData::default()
        };
        let second_text = TextData {
            center: Point::new(60.0, 0.0),
            width: 80.0,
            height: 20.0,
            text: "second".to_owned(),
            font_size: 24.0,
            auto_resize: true,
            ..TextData::default()
        };
        let first_id = insert_text(&mut document, first_text.clone());
        let second_id = insert_text(&mut document, second_text.clone());
        let original_elements = vec![
            SelectionRectState {
                id: first_id,
                rect: text_rect(&first_text),
            },
            SelectionRectState {
                id: second_id,
                rect: text_rect(&second_text),
            },
        ];
        let original_bounds = selection_bounds_from_elements(&original_elements).unwrap();
        let editor = Editor::new(EngineConfig::default()).unwrap();

        let preview = editor.resize_selection_preview(
            &document,
            ResizeSelectionContext {
                original_elements: &original_elements,
                original_arrows: &[],
                original_bounds: &original_bounds,
                handle: ResizeHandle::Right,
                handle_offset_canvas: Point::default(),
                frame_padding: 0.0,
                corner_handle_outset: 0.0,
            },
            Point::new(-80.0, 0.0),
            Modifiers::default(),
            None,
        );

        for element in &preview.elements {
            assert_close(element.rect.width, 40.0);
            assert_close(element.rect.height, 10.0);
        }
        assert_close(preview.bounds.width, 100.0);
        assert_close(preview.bounds.height, 10.0);

        let mut transaction = Transaction::new("commit multi-text resize");
        for element in preview.elements.iter().copied() {
            append_selection_element_update(
                &mut transaction,
                &document,
                element,
                Some(ResizeHandle::Right),
                false,
                None,
            )
            .unwrap();
        }
        document.apply_transaction(transaction).unwrap();

        assert_close(
            document.text(first_id).unwrap().font_size,
            MIN_TEXT_FONT_SIZE,
        );
        assert_close(document.text(second_id).unwrap().font_size, 12.0);
        let committed = Editor::selection_elements_from_ids(&document, &[first_id, second_id]);
        let committed_bounds = selection_bounds_from_elements(&committed).unwrap();
        assert_eq!(committed, preview.elements);
        assert_eq!(committed_bounds, preview.bounds);
    }

    #[test]
    fn committing_selection_edit_for_active_text_updates_draft_without_document_command() {
        let mut document = DocumentModel::new();
        let text = TextData {
            center: Point::new(0.0, 0.0),
            width: 100.0,
            height: 30.0,
            text: "draft".to_owned(),
            auto_resize: false,
            ..TextData::default()
        };
        let text_id = insert_text(&mut document, text.clone());
        let original_rect = text_rect(&text);
        let preview_rect = RectangleData {
            center: Point::new(80.0, 24.0),
            ..original_rect
        };
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, text_id).unwrap();
        editor
            .set_active_text_draft_presentation(
                &document,
                ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::Existing(text_id),
                    revision: 1,
                    text: text.clone(),
                },
            )
            .unwrap();
        editor.state.interaction = InteractionState::EditingSelection(EditSelectionState {
            pointer_id: 1,
            original_elements: vec![SelectionRectState {
                id: text_id,
                rect: original_rect,
            }],
            preview_elements: vec![SelectionRectState {
                id: text_id,
                rect: preview_rect,
            }],
            original_arrows: Vec::new(),
            preview_arrows: Vec::new(),
            original_bounds: SelectionBounds {
                center: original_rect.center,
                width: original_rect.width,
                height: original_rect.height,
                rotation: original_rect.rotation,
            },
            preview_bounds: SelectionBounds {
                center: preview_rect.center,
                width: preview_rect.width,
                height: preview_rect.height,
                rotation: preview_rect.rotation,
            },
            mode: SelectionEditMode::Move {
                start_canvas_position: original_rect.center,
            },
        });

        editor.commit_selection_edit(&document).unwrap();

        let draft = editor
            .active_text_draft_presentation()
            .expect("active draft should remain active after selection edit");
        assert_eq!(draft.rect(), preview_rect);
        assert_eq!(document.text(text_id).unwrap().center, original_rect.center);
        assert!(editor.pending_command.is_none());
    }

    #[test]
    fn committing_active_text_resize_applies_text_resize_semantics_to_draft() {
        let mut document = DocumentModel::new();
        let text = TextData {
            center: Point::new(0.0, 0.0),
            width: 100.0,
            height: 20.0,
            text: "draft".to_owned(),
            font_size: 10.0,
            auto_resize: true,
            ..TextData::default()
        };
        let text_id = insert_text(&mut document, text.clone());
        let original_rect = text_rect(&text);
        let preview_rect = RectangleData {
            center: Point::new(0.0, 10.0),
            height: 40.0,
            ..original_rect
        };
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, text_id).unwrap();
        editor
            .set_active_text_draft_presentation(
                &document,
                ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::Existing(text_id),
                    revision: 1,
                    text: text.clone(),
                },
            )
            .unwrap();
        editor.state.interaction = InteractionState::EditingSelection(EditSelectionState {
            pointer_id: 1,
            original_elements: vec![SelectionRectState {
                id: text_id,
                rect: original_rect,
            }],
            preview_elements: vec![SelectionRectState {
                id: text_id,
                rect: preview_rect,
            }],
            original_arrows: Vec::new(),
            preview_arrows: Vec::new(),
            original_bounds: SelectionBounds {
                center: original_rect.center,
                width: original_rect.width,
                height: original_rect.height,
                rotation: original_rect.rotation,
            },
            preview_bounds: SelectionBounds {
                center: preview_rect.center,
                width: preview_rect.width,
                height: preview_rect.height,
                rotation: preview_rect.rotation,
            },
            mode: SelectionEditMode::Resize {
                handle: ResizeHandle::Bottom,
                handle_offset_canvas: Point::default(),
                frame_padding: 0.0,
                corner_handle_outset: 0.0,
                scale_from_center: false,
                text_layout_override: None,
            },
        });

        editor.commit_selection_edit(&document).unwrap();

        let draft = editor
            .active_text_draft_presentation()
            .expect("active draft should remain active after selection edit");
        assert_eq!(draft.rect(), preview_rect);
        assert_close(draft.text.font_size, 20.0);
        assert!(draft.text.auto_resize);
        assert_eq!(document.text(text_id).unwrap().font_size, text.font_size);
        assert!(editor.pending_command.is_none());
    }

    #[test]
    fn active_text_resize_preview_is_derived_without_mutating_draft() {
        let mut document = DocumentModel::new();
        let text = TextData {
            center: Point::new(0.0, 0.0),
            width: 100.0,
            height: 20.0,
            text: "draft".to_owned(),
            font_size: 10.0,
            auto_resize: true,
            ..TextData::default()
        };
        let text_id = insert_text(&mut document, text.clone());
        let original_rect = text_rect(&text);
        let preview_rect = RectangleData {
            center: Point::new(0.0, 10.0),
            height: 40.0,
            ..original_rect
        };
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, text_id).unwrap();
        editor
            .set_active_text_draft_presentation(
                &document,
                ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::Existing(text_id),
                    revision: 1,
                    text: text.clone(),
                },
            )
            .unwrap();
        editor.state.interaction = InteractionState::EditingSelection(EditSelectionState {
            pointer_id: 1,
            original_elements: vec![SelectionRectState {
                id: text_id,
                rect: original_rect,
            }],
            preview_elements: vec![SelectionRectState {
                id: text_id,
                rect: preview_rect,
            }],
            original_arrows: Vec::new(),
            preview_arrows: Vec::new(),
            original_bounds: SelectionBounds {
                center: original_rect.center,
                width: original_rect.width,
                height: original_rect.height,
                rotation: original_rect.rotation,
            },
            preview_bounds: SelectionBounds {
                center: preview_rect.center,
                width: preview_rect.width,
                height: preview_rect.height,
                rotation: preview_rect.rotation,
            },
            mode: SelectionEditMode::Resize {
                handle: ResizeHandle::Bottom,
                handle_offset_canvas: Point::default(),
                frame_padding: 0.0,
                corner_handle_outset: 0.0,
                scale_from_center: false,
                text_layout_override: Some(TextResizeLayoutOverride {
                    requested_width: 100.0,
                    requested_height: 40.0,
                    requested_font_size: 22.0,
                    layout: TextLayoutSize {
                        width: 100.0,
                        height: 40.0,
                    },
                }),
            },
        });

        let raw = editor
            .active_text_draft_presentation()
            .expect("raw active draft should remain available");
        let displayed = editor
            .active_text_draft_display_presentation()
            .expect("active draft should expose the transform preview");
        assert_eq!(raw.rect(), original_rect);
        assert_eq!(raw.text.font_size, 10.0);
        assert_eq!(displayed.rect(), preview_rect);
        assert_eq!(displayed.text.font_size, 22.0);

        editor.cancel_interaction();

        let restored = editor
            .active_text_draft_display_presentation()
            .expect("cancelled transform should preserve active draft");
        assert_eq!(restored.rect(), original_rect);
        assert_eq!(restored.text.font_size, 10.0);
    }

    #[test]
    fn single_text_resize_preview_uses_active_draft_style() {
        let mut document = DocumentModel::new();
        let text = TextData {
            center: Point::new(0.0, 0.0),
            width: 100.0,
            height: 20.0,
            text: "draft".to_owned(),
            font_size: 10.0,
            auto_resize: true,
            ..TextData::default()
        };
        let text_id = insert_text(&mut document, text.clone());
        let mut active_text = text.clone();
        active_text.font_size = 30.0;
        let original_rect = text_rect(&active_text);
        let original_elements = vec![SelectionRectState {
            id: text_id,
            rect: original_rect,
        }];
        let original_bounds = SelectionBounds {
            center: original_rect.center,
            width: original_rect.width,
            height: original_rect.height,
            rotation: original_rect.rotation,
        };
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor
            .set_active_text_draft_presentation(
                &document,
                ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::Existing(text_id),
                    revision: 1,
                    text: active_text,
                },
            )
            .unwrap();

        let preview = editor
            .single_text_resize_preview(
                &document,
                ResizeSelectionContext {
                    original_elements: &original_elements,
                    original_arrows: &[],
                    original_bounds: &original_bounds,
                    handle: ResizeHandle::Bottom,
                    handle_offset_canvas: Point::default(),
                    frame_padding: 0.0,
                    corner_handle_outset: 0.0,
                },
                ResizeDragGeometry {
                    anchor_local: Point::default(),
                    handle_local: Point::default(),
                    scale_x: 1.0,
                    scale_y: 0.5,
                },
                Modifiers::default(),
                None,
            )
            .expect("active text resize should produce a preview");

        assert_close(preview.rect.height, 10.0);
    }

    #[test]
    fn committing_active_text_move_persists_bound_arrow_updates() {
        let mut document = DocumentModel::new();
        let text = TextData {
            center: Point::new(0.0, 0.0),
            width: 100.0,
            height: 40.0,
            text: "draft".to_owned(),
            auto_resize: false,
            ..TextData::default()
        };
        let text_id = insert_text(&mut document, text.clone());
        let mut arrow = ArrowData::from_global_points(
            &[Point::new(50.0, 0.0), Point::new(250.0, 0.0)],
            ColorRgba8 {
                r: 0,
                g: 0,
                b: 0,
                a: 255,
            },
            2.0,
            ArrowStrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        arrow.start_binding = Some(ArrowEndpointBinding {
            element_id: text_id,
            fixed_point: [1.0, 0.5],
            mode: BindMode::Orbit,
        });
        let arrow_id = insert_arrow(&mut document, arrow.clone());
        let original_rect = text_rect(&text);
        let preview_rect = RectangleData {
            center: Point::new(100.0, 0.0),
            ..original_rect
        };
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, text_id).unwrap();
        editor
            .set_active_text_draft_presentation(
                &document,
                ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::Existing(text_id),
                    revision: 1,
                    text: text.clone(),
                },
            )
            .unwrap();
        editor.state.interaction = InteractionState::EditingSelection(EditSelectionState {
            pointer_id: 1,
            original_elements: vec![SelectionRectState {
                id: text_id,
                rect: original_rect,
            }],
            preview_elements: vec![SelectionRectState {
                id: text_id,
                rect: preview_rect,
            }],
            original_arrows: Vec::new(),
            preview_arrows: Vec::new(),
            original_bounds: SelectionBounds {
                center: original_rect.center,
                width: original_rect.width,
                height: original_rect.height,
                rotation: original_rect.rotation,
            },
            preview_bounds: SelectionBounds {
                center: preview_rect.center,
                width: preview_rect.width,
                height: preview_rect.height,
                rotation: preview_rect.rotation,
            },
            mode: SelectionEditMode::Move {
                start_canvas_position: original_rect.center,
            },
        });

        editor.commit_selection_edit(&document).unwrap();

        let draft = editor
            .active_text_draft_presentation()
            .expect("active draft should remain active after selection edit");
        assert_eq!(draft.rect(), preview_rect);
        assert_eq!(document.text(text_id).unwrap().center, original_rect.center);
        let Some(EditorCommand::ApplyTransaction(command)) = editor.pending_command.take() else {
            panic!("bound arrow update should be queued");
        };
        let updated_arrow = command
            .transaction
            .operations()
            .iter()
            .find_map(|operation| match operation {
                Operation::UpdateElementData {
                    id,
                    data: ElementData::Arrow(updated),
                } if *id == arrow_id => Some(updated),
                _ => None,
            })
            .expect("transaction should update the bound arrow");
        assert_ne!(updated_arrow, &arrow);
        assert_eq!(
            updated_arrow
                .start_binding
                .as_ref()
                .expect("arrow should remain bound")
                .element_id,
            text_id
        );
        assert!(!command.transaction.operations().iter().any(|operation| {
            matches!(
                operation,
                Operation::UpdateElementData {
                    id,
                    data: ElementData::Text(_),
                } if *id == text_id
            )
        }));
    }

    #[test]
    fn repeated_text_resize_measurement_preserves_requested_size_for_release_preview() {
        let text = TextData {
            center: Point::new(0.0, 0.0),
            width: 100.0,
            height: 20.0,
            text: "hello".to_owned(),
            font_size: 10.0,
            auto_resize: false,
            ..TextData::default()
        };
        let original_rect = text_rect(&text);
        let original_bounds = SelectionBounds {
            center: text.center,
            width: text.width,
            height: text.height,
            rotation: text.rotation,
        };
        let first_override = TextResizeLayoutOverride {
            requested_width: 100.0,
            requested_height: 40.0,
            requested_font_size: 20.0,
            layout: TextLayoutSize {
                width: 100.0,
                height: 36.0,
            },
        };
        let measured_preview_rect = resized_text_rect_from_size(
            &original_rect,
            &original_bounds,
            text_resize_anchor(ResizeHandle::Bottom, false),
            first_override.layout.width,
            first_override.layout.height,
        );

        let requested = text_resize_measurement_requested_values(
            measured_preview_rect,
            first_override.requested_font_size,
            Some(first_override),
        );
        let repeated_override = TextResizeLayoutOverride {
            requested_width: requested.width,
            requested_height: requested.height,
            requested_font_size: requested.font_size,
            layout: first_override.layout,
        };
        let release_preview = text_resize_preview_rect(
            &text,
            &original_bounds,
            ResizeHandle::Bottom,
            ResizeDragGeometry {
                anchor_local: Point::default(),
                handle_local: Point::default(),
                scale_x: 1.0,
                scale_y: 2.0,
            },
            false,
            Some(repeated_override),
        )
        .expect("release preview should resolve");

        assert_close(
            repeated_override.requested_height,
            first_override.requested_height,
        );
        assert_close(
            repeated_override.requested_font_size,
            first_override.requested_font_size,
        );
        assert_close(release_preview.height, first_override.layout.height);
        assert_close(release_preview.center.y, measured_preview_rect.center.y);
    }
}

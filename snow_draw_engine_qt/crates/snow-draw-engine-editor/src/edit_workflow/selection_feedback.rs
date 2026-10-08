use super::*;

impl Editor {
    pub(crate) fn active_cursor_for_selection_state(
        &self,
        _document: &DocumentModel,
        state: &EditSelectionState,
        _canvas_point: Point<f64>,
        _modifiers: Modifiers,
    ) -> CursorStyle {
        match state.mode {
            SelectionEditMode::Move { .. } => CursorStyle::Move,
            SelectionEditMode::Resize { handle, .. } => {
                resize_cursor_for_handle(handle, state.preview_bounds.rotation)
            }
            SelectionEditMode::Rotate { .. } => CursorStyle::Grabbing,
            SelectionEditMode::CornerRadius { .. } => CursorStyle::CornerRadius,
        }
    }

    pub(crate) fn commit_marquee_selection(
        &mut self,
        document: &DocumentModel,
        state: MarqueeSelectionState,
        end_canvas_position: Point<f64>,
    ) {
        let Some(bounds) = axis_aligned_bounds(state.start_canvas_position, end_canvas_position)
        else {
            if !state.additive {
                self.clear_selection();
            }
            return;
        };

        let matched_ids = self.selection_ids_intersecting_bounds(
            document,
            bounds,
            self.tool_policy().selection_scope,
        );
        if state.additive {
            let mut next_ids = state.base_selection.ids;
            for id in matched_ids.iter().copied() {
                if !next_ids.contains(&id) {
                    next_ids.push(id);
                }
            }
            let primary = matched_ids.last().copied().or(state.base_selection.primary);
            self.set_selection_state_with_document(Some(document), next_ids, primary);
            return;
        }

        let primary = matched_ids.last().copied();
        self.set_selection_state_with_document(Some(document), matched_ids, primary);
    }

    pub(crate) fn hover_cursor_for_canvas_point(
        &self,
        document: &DocumentModel,
        policy: ToolPolicy,
        canvas_point: Point<f64>,
    ) -> CursorStyle {
        self.hover_feedback_for_canvas_point(document, policy, canvas_point)
            .0
    }

    pub(crate) fn hover_feedback_for_canvas_point(
        &self,
        document: &DocumentModel,
        policy: ToolPolicy,
        canvas_point: Point<f64>,
    ) -> (CursorStyle, Option<ElementId>) {
        let intent = self.resolve_primary_pointer_intent(
            document,
            policy,
            canvas_point,
            Modifiers::default(),
        );
        self.hover_feedback_for_primary_pointer_intent(document, policy, intent)
    }

    pub(crate) fn selection_ids_intersecting_bounds(
        &self,
        document: &DocumentModel,
        bounds: AxisAlignedBounds,
        scope: ToolSelectionScope,
    ) -> Vec<ElementId> {
        let mut ids = Vec::new();
        for id in document.paint_order() {
            let Ok(element) = document.element(*id) else {
                continue;
            };
            if !element.meta.visible {
                continue;
            }
            let kind = element.data.kind();
            if !Self::selection_scope_matches_document(document, scope, *id, kind) {
                continue;
            }
            if let Some(rect) = document.element_rect_proxy(*id)
                && rectangle_intersects_axis_aligned_bounds(&rect, bounds)
            {
                ids.push(*id);
            }
        }
        document.for_each_visible_arrow(|id, arrow| {
            if Self::selection_scope_matches(scope, arrow.element_kind())
                && draw_rect_intersects_axis_aligned_bounds(arrow_bounds(arrow), bounds)
            {
                ids.push(id);
            }
        });
        ids
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::canvas_to_view;
    use snow_draw_engine_interaction::{PointerButtons, PointerDevice};

    fn pointer(
        editor: &mut Editor,
        document: &mut DocumentModel,
        event_type: PointerEventType,
        canvas_point: Point<f64>,
        shift: bool,
    ) {
        let update = editor
            .process_input(
                document,
                InputEvent::Pointer(PointerEvent {
                    pointer_id: 1,
                    event_type,
                    device: PointerDevice::Mouse,
                    position: canvas_to_view(canvas_point, &editor.camera(), editor.surface_size()),
                    button: match event_type {
                        PointerEventType::Down | PointerEventType::Up => {
                            Some(PointerButton::Primary)
                        }
                        _ => None,
                    },
                    buttons: if event_type == PointerEventType::Up {
                        PointerButtons::default()
                    } else {
                        PointerButtons(PointerButtons::PRIMARY)
                    },
                    modifiers: Modifiers {
                        shift,
                        ..Modifiers::default()
                    },
                }),
            )
            .unwrap();
        assert!(update.interaction.consumed);
        if let Some(command) = update.command {
            let EditorCommand::ApplyTransaction(command) = command else {
                panic!("stroke creation should apply a transaction");
            };
            document.apply_transaction(command.transaction).unwrap();
        }
    }

    fn create_stroke(
        editor: &mut Editor,
        document: &mut DocumentModel,
        start: Point<f64>,
        end: Point<f64>,
    ) -> ElementId {
        editor.set_active_tool(ActiveTool::FreeDraw).unwrap();
        let id = document.peek_next_element_id();
        pointer(editor, document, PointerEventType::Down, start, false);
        if start != end {
            pointer(editor, document, PointerEventType::Move, end, false);
        }
        pointer(editor, document, PointerEventType::Up, end, false);
        assert!(document.free_draw(id).is_ok());
        id
    }

    fn candidate_ids(editor: &Editor, document: &DocumentModel) -> Vec<ElementId> {
        editor
            .presentation_state(document)
            .marquee_candidate_elements
            .iter()
            .map(|element| element.id)
            .collect()
    }

    #[test]
    fn marquee_selects_tapped_dot_in_preview_and_on_release() {
        for zoom in [0.25, 1.0, 4.0] {
            for (x_sign, y_sign) in [(1.0, 1.0), (-1.0, 1.0), (1.0, -1.0), (-1.0, -1.0)] {
                for additive in [false, true] {
                    let mut document = DocumentModel::new();
                    let mut editor = Editor::new(EngineConfig::default()).unwrap();
                    editor.set_surface_size(800, 600).unwrap();
                    editor
                        .set_snap_config(SnapConfig {
                            enabled: false,
                            ..SnapConfig::default()
                        })
                        .unwrap();
                    editor
                        .set_camera(Camera {
                            zoom,
                            ..Camera::default()
                        })
                        .unwrap();
                    let dot = create_stroke(
                        &mut editor,
                        &mut document,
                        Point::new(0.0, 0.0),
                        Point::new(0.0, 0.0),
                    );
                    let outside = create_stroke(
                        &mut editor,
                        &mut document,
                        Point::new(80.0, 50.0),
                        Point::new(80.0, 50.0),
                    );
                    let proxy = document.element_rect_proxy(dot).unwrap();
                    assert!(proxy.width > 0.0);
                    assert_eq!(proxy.height, 0.0);
                    editor.set_active_tool(ActiveTool::Select).unwrap();
                    editor.set_selection_state_with_document(
                        Some(&document),
                        vec![outside],
                        Some(outside),
                    );
                    let revision = document.document_revision();
                    let start = Point::new(-30.0 * x_sign, -30.0 * y_sign);
                    let end = Point::new(30.0 * x_sign, 30.0 * y_sign);
                    pointer(
                        &mut editor,
                        &mut document,
                        PointerEventType::Down,
                        start,
                        additive,
                    );
                    pointer(
                        &mut editor,
                        &mut document,
                        PointerEventType::Move,
                        end,
                        additive,
                    );
                    assert_eq!(candidate_ids(&editor, &document), vec![dot]);
                    pointer(
                        &mut editor,
                        &mut document,
                        PointerEventType::Up,
                        end,
                        additive,
                    );
                    let expected = if additive {
                        vec![outside, dot]
                    } else {
                        vec![dot]
                    };
                    assert_eq!(editor.selected_ids(), expected);
                    assert!(candidate_ids(&editor, &document).is_empty());
                    assert_eq!(document.document_revision(), revision);
                }
            }
        }
    }

    #[test]
    fn marquee_selects_horizontal_and_vertical_strokes() {
        for end in [Point::new(40.0, 0.0), Point::new(0.0, 40.0)] {
            let mut document = DocumentModel::new();
            let mut editor = Editor::new(EngineConfig::default()).unwrap();
            editor.set_surface_size(800, 600).unwrap();
            let id = create_stroke(&mut editor, &mut document, Point::new(0.0, 0.0), end);
            editor.set_active_tool(ActiveTool::Select).unwrap();

            // Intersect only the middle of the stroke, leaving both endpoints outside.
            let center = Point::new(end.x / 2.0, end.y / 2.0);
            let start = Point::new(center.x - 12.0, center.y - 12.0);
            let end = Point::new(center.x + 12.0, center.y + 12.0);
            pointer(
                &mut editor,
                &mut document,
                PointerEventType::Down,
                start,
                false,
            );
            pointer(
                &mut editor,
                &mut document,
                PointerEventType::Move,
                end,
                false,
            );
            assert_eq!(candidate_ids(&editor, &document), vec![id]);
            pointer(&mut editor, &mut document, PointerEventType::Up, end, false);
            assert_eq!(editor.selected_ids(), vec![id]);
        }
    }
}

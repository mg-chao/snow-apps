use snow_draw_engine_core::{ErrorCode, Point, view_to_canvas};
use snow_draw_engine_interaction::{
    CursorCommand, CursorStyle, InteractionOutput, PointerButton, PointerCaptureCommand,
    PointerEvent, PointerEventType,
};
use snow_draw_engine_model::DocumentModel;

use crate::{ActiveTool, Editor};

const ERASER_RADIUS_VIEW: f64 = 8.0;

pub(crate) fn eraser_segment_samples(
    start: Point<f64>,
    end: Point<f64>,
    tolerance: f64,
    include_start: bool,
) -> Vec<Point<f64>> {
    let tolerance = if tolerance.is_finite() && tolerance > 0.0 {
        tolerance
    } else {
        0.0
    };
    let dx = end.x - start.x;
    let dy = end.y - start.y;
    let distance = dx.hypot(dy);
    if !distance.is_finite() {
        return include_start.then_some(start).into_iter().collect();
    }
    if tolerance == 0.0 || distance <= f64::EPSILON {
        let mut points = Vec::new();
        if include_start {
            points.push(start);
        }
        if distance > f64::EPSILON {
            points.push(end);
        }
        return points;
    }
    let count = (distance / (tolerance * 0.5)).ceil().max(1.0) as usize;
    let first = usize::from(!include_start);
    (first..=count)
        .map(|index| {
            let t = index as f64 / count as f64;
            Point::new(start.x + dx * t, start.y + dy * t)
        })
        .collect()
}

impl Editor {
    pub(crate) fn clear_eraser_state(&mut self) {
        let had_preview = !self.state.eraser.pending_ids.is_empty();
        let had_cursor = self.state.eraser.cursor_canvas_position.is_some();
        self.state.eraser = Default::default();
        if had_preview {
            self.bump_scene_state_revision();
        }
        if had_cursor {
            self.bump_overlay_state_revision();
        }
    }

    fn eraser_tolerance(&self) -> f64 {
        let zoom = self.camera().zoom;
        if zoom.is_finite() && zoom > 0.0 {
            ERASER_RADIUS_VIEW / zoom
        } else {
            0.0
        }
    }

    fn queue_eraser_hits(&mut self, document: &DocumentModel, samples: &[Point<f64>]) {
        let tolerance = self.eraser_tolerance();
        let mut changed = false;
        for sample in samples {
            for (id, _) in document.elements_at_with_tolerance(*sample, tolerance) {
                let id = document.arrow_id_for_text(id).unwrap_or(id);
                if !self.state.eraser.pending_ids.contains(&id) {
                    self.state.eraser.pending_ids.push(id);
                    changed = true;
                }
            }
        }
        if changed {
            self.bump_scene_state_revision();
        }
    }

    pub(crate) fn process_eraser_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        debug_assert_eq!(self.active_tool(), ActiveTool::Eraser);
        let canvas_point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        match event.event_type {
            PointerEventType::Enter => {
                self.state.eraser.cursor_canvas_position = Some(canvas_point);
                self.bump_overlay_state_revision();
                Ok(InteractionOutput {
                    consumed: false,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Hidden),
                })
            }
            PointerEventType::Leave => {
                if self.state.eraser.cursor_canvas_position.take().is_some() {
                    self.bump_overlay_state_revision();
                }
                Ok(InteractionOutput {
                    consumed: false,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Default),
                })
            }
            PointerEventType::Down | PointerEventType::DoubleClick => {
                if event.button != Some(PointerButton::Primary) {
                    return Ok(InteractionOutput::default());
                }
                self.state.eraser.cursor_canvas_position = Some(canvas_point);
                self.state
                    .eraser
                    .active_pointers
                    .insert(event.pointer_id, canvas_point);
                self.queue_eraser_hits(document, &[canvas_point]);
                self.bump_overlay_state_revision();
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.capture_command_for_start(event.pointer_id),
                    cursor: CursorCommand::Set(CursorStyle::Hidden),
                })
            }
            PointerEventType::Move => {
                self.state.eraser.cursor_canvas_position = Some(canvas_point);
                self.bump_overlay_state_revision();
                if let Some(previous) = self
                    .state
                    .eraser
                    .active_pointers
                    .get_mut(&event.pointer_id)
                    .map(|previous| {
                        let value = *previous;
                        *previous = canvas_point;
                        value
                    })
                {
                    let samples = eraser_segment_samples(
                        previous,
                        canvas_point,
                        self.eraser_tolerance(),
                        false,
                    );
                    self.queue_eraser_hits(document, &samples);
                }
                Ok(InteractionOutput {
                    consumed: self
                        .state
                        .eraser
                        .active_pointers
                        .contains_key(&event.pointer_id),
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Hidden),
                })
            }
            PointerEventType::Up | PointerEventType::Cancel => {
                let Some(previous) = self.state.eraser.active_pointers.remove(&event.pointer_id)
                else {
                    return Ok(InteractionOutput::default());
                };
                let samples =
                    eraser_segment_samples(previous, canvas_point, self.eraser_tolerance(), false);
                self.queue_eraser_hits(document, &samples);
                if self.state.eraser.active_pointers.is_empty() {
                    let ids = std::mem::take(&mut self.state.eraser.pending_ids);
                    self.bump_scene_state_revision();
                    if let Some(command) = self.delete_elements(document, &ids, "erase elements")? {
                        self.queue_command(command);
                    }
                }
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.release_capture_command(),
                    cursor: CursorCommand::Set(CursorStyle::Hidden),
                })
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{ColorRgba8, CornerRadii, EngineConfig};
    use snow_draw_engine_document::{
        ElementMeta, FillStyle, Operation, RectangleData, RectangleElementKind, StrokeStyle,
        Transaction,
    };
    use snow_draw_engine_interaction::{InputEvent, Modifiers, PointerButtons, PointerDevice};

    fn rectangle(center: Point<f64>, size: f64) -> RectangleData {
        RectangleData {
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center,
            width: size,
            height: size,
            rotation: 0.0,
            fill: ColorRgba8 {
                r: 1,
                g: 2,
                b: 3,
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

    fn document_with_rectangles(centers: &[Point<f64>], size: f64) -> DocumentModel {
        let mut document = DocumentModel::new();
        let mut transaction = Transaction::new("test rectangles");
        for center in centers {
            let id = document.allocate_element_id();
            transaction.insert_rectangle(id, ElementMeta::default(), rectangle(*center, size));
        }
        document.apply_transaction(transaction).unwrap();
        document
    }

    fn editor() -> Editor {
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::Eraser).unwrap();
        editor
    }

    fn pointer(
        pointer_id: u32,
        event_type: PointerEventType,
        position: Point<f64>,
        button: Option<PointerButton>,
    ) -> InputEvent {
        InputEvent::Pointer(PointerEvent {
            pointer_id,
            event_type,
            device: PointerDevice::Mouse,
            position,
            button,
            buttons: PointerButtons(if button == Some(PointerButton::Primary) {
                PointerButtons::PRIMARY
            } else {
                0
            }),
            modifiers: Modifiers::default(),
        })
    }

    #[test]
    fn segment_samples_use_half_radius_spacing_and_include_endpoint() {
        let samples =
            eraser_segment_samples(Point::new(0.0, 0.0), Point::new(20.0, 0.0), 8.0, true);
        assert_eq!(samples.len(), 6);
        assert_eq!(samples[0], Point::new(0.0, 0.0));
        assert_eq!(samples[5], Point::new(20.0, 0.0));
    }

    #[test]
    fn invalid_tolerance_degrades_to_endpoints() {
        let samples =
            eraser_segment_samples(Point::new(1.0, 2.0), Point::new(5.0, 6.0), f64::NAN, true);
        assert_eq!(samples, vec![Point::new(1.0, 2.0), Point::new(5.0, 6.0)]);
    }

    #[test]
    fn overlapping_hits_preview_at_half_opacity_and_commit_atomically() {
        let document =
            document_with_rectangles(&[Point::new(0.0, 0.0), Point::new(0.0, 0.0)], 20.0);
        let mut editor = editor();

        let down = editor
            .process_input(
                &document,
                pointer(
                    1,
                    PointerEventType::Down,
                    Point::new(100.0, 100.0),
                    Some(PointerButton::Primary),
                ),
            )
            .unwrap();
        assert!(down.command.is_none());
        let presentation = editor.presentation_state(&document);
        assert_eq!(presentation.preview_elements.len(), 2);
        assert!(
            presentation
                .preview_elements
                .iter()
                .all(|item| item.rect.opacity == 0.5)
        );

        let up = editor
            .process_input(
                &document,
                pointer(
                    1,
                    PointerEventType::Up,
                    Point::new(100.0, 100.0),
                    Some(PointerButton::Primary),
                ),
            )
            .unwrap();
        let Some(crate::EditorCommand::ApplyTransaction(command)) = up.command else {
            panic!("final pointer up should queue one transaction");
        };
        assert_eq!(command.transaction.label(), "erase elements");
        assert_eq!(command.transaction.operations().len(), 2);
        assert!(
            command
                .transaction
                .operations()
                .iter()
                .all(|operation| { matches!(operation, Operation::RemoveElement { .. }) })
        );
    }

    #[test]
    fn hover_move_and_secondary_button_do_not_start_erasing() {
        let document = document_with_rectangles(&[Point::new(0.0, 0.0)], 20.0);
        let mut editor = editor();

        let hover = editor
            .process_input(
                &document,
                pointer(1, PointerEventType::Move, Point::new(100.0, 100.0), None),
            )
            .unwrap();
        assert!(!hover.interaction.consumed);
        let secondary = editor
            .process_input(
                &document,
                pointer(
                    1,
                    PointerEventType::Down,
                    Point::new(100.0, 100.0),
                    Some(PointerButton::Secondary),
                ),
            )
            .unwrap();
        assert!(!secondary.interaction.consumed);
        let up = editor
            .process_input(
                &document,
                pointer(
                    1,
                    PointerEventType::Up,
                    Point::new(100.0, 100.0),
                    Some(PointerButton::Primary),
                ),
            )
            .unwrap();
        assert!(up.command.is_none());
        assert!(
            editor
                .presentation_state(&document)
                .preview_elements
                .is_empty()
        );
    }

    #[test]
    fn multiple_pointers_commit_only_after_final_pointer_ends() {
        let document =
            document_with_rectangles(&[Point::new(-20.0, 0.0), Point::new(20.0, 0.0)], 12.0);
        let mut editor = editor();
        for (id, x) in [(1, 80.0), (2, 120.0)] {
            editor
                .process_input(
                    &document,
                    pointer(
                        id,
                        PointerEventType::Down,
                        Point::new(x, 100.0),
                        Some(PointerButton::Primary),
                    ),
                )
                .unwrap();
        }
        let first_up = editor
            .process_input(
                &document,
                pointer(
                    1,
                    PointerEventType::Up,
                    Point::new(80.0, 100.0),
                    Some(PointerButton::Primary),
                ),
            )
            .unwrap();
        assert!(first_up.command.is_none());
        let final_up = editor
            .process_input(
                &document,
                pointer(2, PointerEventType::Cancel, Point::new(120.0, 100.0), None),
            )
            .unwrap();
        assert!(matches!(
            final_up.command,
            Some(crate::EditorCommand::ApplyTransaction(_))
        ));
    }

    #[test]
    fn switching_tools_discards_pending_eraser_work() {
        let document = document_with_rectangles(&[Point::new(0.0, 0.0)], 20.0);
        let mut editor = editor();
        editor
            .process_input(
                &document,
                pointer(
                    1,
                    PointerEventType::Down,
                    Point::new(100.0, 100.0),
                    Some(PointerButton::Primary),
                ),
            )
            .unwrap();
        assert_eq!(
            editor.presentation_state(&document).preview_elements.len(),
            1
        );

        editor.set_active_tool(ActiveTool::Select).unwrap();

        assert!(
            editor
                .presentation_state(&document)
                .preview_elements
                .is_empty()
        );
        assert!(editor.pending_command.is_none());
    }
}

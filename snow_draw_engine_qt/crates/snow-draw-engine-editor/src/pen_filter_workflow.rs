use super::*;
use snow_draw_engine_document::ElementMeta;

use crate::polyline_simplification::simplify_polyline;

// The bounded raw window keeps sample-time work amortized O(1) while avoiding
// artificial vertices at high-frequency tablet sampling rates.
const PEN_FILTER_SIMPLIFICATION_WINDOW: usize = 512;
const RAW_WINDOW_ERROR_FRACTION: f64 = 2.0 / 3.0;
const COMPACTION_ERROR_FRACTION: f64 = 1.0 / 6.0;
const FINAL_ERROR_FRACTION: f64 = 1.0 / 6.0;

impl Editor {
    pub(crate) fn begin_pen_filter_creation(
        &mut self,
        pointer_id: u32,
        start_canvas_position: Point<f64>,
    ) {
        let zoom = self.camera().zoom.max(f64::EPSILON);
        let stroke_width = self.state.default_pen_filter.stroke_width.max(0.0);
        let epsilon = (stroke_width * zoom * 0.04).clamp(0.75, 2.0) / zoom;
        self.state.interaction = InteractionState::CreatingPenFilter(CreatePenFilterState {
            pointer_id,
            committed_points: Vec::new(),
            pending_simplified_points: Vec::new(),
            pending_raw_points: vec![start_canvas_position],
            preview_committed_points: Vec::new(),
            epsilon,
            straight_anchor: None,
            straight_endpoint: None,
        });
        self.state.creation_preview = None;
        self.bump_scene_state_revision();
    }

    pub(crate) fn process_pen_filter_creation_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        match event.event_type {
            PointerEventType::Move | PointerEventType::Enter => {
                let point = view_to_canvas(event.position, &self.camera(), self.surface_size());
                let InteractionState::CreatingPenFilter(state) = &mut self.state.interaction else {
                    return Ok(InteractionOutput::default());
                };
                if state.pointer_id != event.pointer_id {
                    return Ok(InteractionOutput::default());
                }
                if append_pen_filter_point(state, point, event.modifiers.shift) {
                    self.bump_scene_state_revision();
                }
                Ok(InteractionOutput {
                    consumed: true,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Crosshair),
                })
            }
            PointerEventType::Up => {
                let point = view_to_canvas(event.position, &self.camera(), self.surface_size());
                let interaction =
                    std::mem::replace(&mut self.state.interaction, InteractionState::Idle);
                let InteractionState::CreatingPenFilter(mut state) = interaction else {
                    return Ok(InteractionOutput::default());
                };
                if state.pointer_id != event.pointer_id {
                    self.state.interaction = InteractionState::CreatingPenFilter(state);
                    return Ok(InteractionOutput::default());
                }
                append_pen_filter_point(&mut state, point, event.modifiers.shift);
                let streaming_points = finalized_streaming_points(&state);
                let points =
                    simplify_polyline(&streaming_points, state.epsilon * FINAL_ERROR_FRACTION);
                let filter = self.pen_filter_from_points(&points);
                self.cancel_interaction();
                if let Some(filter) = filter {
                    let mut transaction = Transaction::new("create pen filter");
                    transaction.insert_pen_filter(
                        document.peek_next_element_id(),
                        ElementMeta::default(),
                        filter,
                    );
                    self.queue_command(EditorCommand::ApplyTransaction(
                        ApplyTransactionCommand::new(transaction),
                    ));
                }
                self.bump_scene_state_revision();
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.release_capture_command(),
                    cursor: CursorCommand::Set(CursorStyle::Crosshair),
                })
            }
            PointerEventType::Cancel => {
                let InteractionState::CreatingPenFilter(state) = &self.state.interaction else {
                    return Ok(InteractionOutput::default());
                };
                if state.pointer_id != event.pointer_id {
                    return Ok(InteractionOutput::default());
                }
                self.cancel_interaction();
                self.bump_scene_state_revision();
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

    fn pen_filter_from_points(&self, points: &[Point<f64>]) -> Option<PenFilterData> {
        let style = &self.state.default_pen_filter;
        PenFilterData::from_global_points(
            points,
            style.filter_type,
            style.strength,
            style.stroke_width,
            style.opacity,
        )
    }

    pub(crate) fn pen_filter_creation_preview(&self) -> Option<ElementCreationPreview> {
        let InteractionState::CreatingPenFilter(state) = &self.state.interaction else {
            return None;
        };
        let points = streaming_points(state);
        if points.len() < 2 {
            return None;
        }
        let style = &self.state.default_pen_filter;
        Some(ElementCreationPreview::PenFilter(PenFilterPreview {
            global_points: points,
            filter_type: style.filter_type,
            strength: style.strength,
            stroke_width: style.stroke_width,
            opacity: style.opacity,
        }))
    }
}

fn append_streaming_point(state: &mut CreatePenFilterState, point: Point<f64>) -> bool {
    if !point.x.is_finite()
        || !point.y.is_finite()
        || state.pending_raw_points.last().copied() == Some(point)
    {
        return false;
    }
    state.pending_raw_points.push(point);
    if state.pending_raw_points.len() >= PEN_FILTER_SIMPLIFICATION_WINDOW {
        flush_pending_raw_prefix(state);
    }
    true
}

fn flush_pending_raw_prefix(state: &mut CreatePenFilterState) {
    let Some(endpoint) = state.pending_raw_points.last().copied() else {
        return;
    };
    let preview_chunk = simplify_polyline(&state.pending_raw_points, state.epsilon);
    append_without_endpoint(&mut state.preview_committed_points, &preview_chunk);

    let simplified = simplify_polyline(
        &state.pending_raw_points,
        state.epsilon * RAW_WINDOW_ERROR_FRACTION,
    );
    let committed_end = simplified.len().saturating_sub(1);
    for point in simplified.into_iter().take(committed_end) {
        if state.pending_simplified_points.last().copied() != Some(point) {
            state.pending_simplified_points.push(point);
        }
        if state.pending_simplified_points.len() >= PEN_FILTER_SIMPLIFICATION_WINDOW {
            let compacted = simplify_polyline(
                &state.pending_simplified_points,
                state.epsilon * COMPACTION_ERROR_FRACTION,
            );
            let compacted_end = compacted.len().saturating_sub(1);
            for compacted_point in compacted.into_iter().take(compacted_end) {
                if state.committed_points.last().copied() != Some(compacted_point) {
                    state.committed_points.push(compacted_point);
                }
            }
            state.pending_simplified_points.clear();
            state.pending_simplified_points.push(point);
        }
    }
    state.pending_raw_points.clear();
    state.pending_raw_points.push(endpoint);
}

/// Append a filter sample, optionally replacing the active Shift-straight
/// endpoint. This mirrors Free Draw's one-segment straight-line interaction
/// while retaining the filter's bounded streaming simplifier.
fn append_pen_filter_point(
    state: &mut CreatePenFilterState,
    point: Point<f64>,
    shift: bool,
) -> bool {
    if !point.x.is_finite() || !point.y.is_finite() {
        return false;
    }
    if !shift {
        state.straight_anchor = None;
        state.straight_endpoint = None;
        return append_streaming_point(state, point);
    }

    if state.straight_anchor.is_none() {
        let Some(anchor) = state
            .pending_raw_points
            .last()
            .copied()
            .or_else(|| state.preview_committed_points.last().copied())
            .or_else(|| state.committed_points.last().copied())
        else {
            return false;
        };
        // Freeze the curve before adding the movable endpoint so simplification
        // cannot discard the exact point where the straight segment begins.
        flush_pending_raw_prefix(state);
        state.straight_anchor = Some(anchor);
    }

    let Some(anchor) = state.straight_anchor else {
        return false;
    };
    let mut changed = false;
    if let Some(endpoint) = state.straight_endpoint.take() {
        // The final raw sample is the movable endpoint while Shift remains
        // held. Keep the segment's anchor in the stream and replace only that
        // endpoint so high-frequency pointer events do not add vertices.
        if state.pending_raw_points.last().copied() == Some(endpoint) {
            state.pending_raw_points.pop();
            changed = true;
        }
    }
    if point == anchor {
        return changed;
    }
    if append_streaming_point(state, point) {
        state.straight_endpoint = Some(point);
        return true;
    }
    changed
}

fn streaming_points(state: &CreatePenFilterState) -> Vec<Point<f64>> {
    // Each preview window is simplified exactly once from raw samples. Frozen
    // window prefixes prevent approximation error and geometry patches from
    // propagating backward as the stroke grows.
    let preview_tail = simplify_polyline(&state.pending_raw_points, state.epsilon);
    let mut points = state.preview_committed_points.clone();
    append_unique(&mut points, &preview_tail);
    points
}

fn finalized_streaming_points(state: &CreatePenFilterState) -> Vec<Point<f64>> {
    let raw_tail = simplify_polyline(
        &state.pending_raw_points,
        state.epsilon * RAW_WINDOW_ERROR_FRACTION,
    );
    combine_streaming_points(state, &raw_tail, state.epsilon * COMPACTION_ERROR_FRACTION)
}

fn append_without_endpoint(destination: &mut Vec<Point<f64>>, points: &[Point<f64>]) {
    append_unique(destination, &points[..points.len().saturating_sub(1)]);
}

fn append_unique(destination: &mut Vec<Point<f64>>, points: &[Point<f64>]) {
    for point in points {
        if destination.last().copied() != Some(*point) {
            destination.push(*point);
        }
    }
}

fn combine_streaming_points(
    state: &CreatePenFilterState,
    raw_tail: &[Point<f64>],
    epsilon: f64,
) -> Vec<Point<f64>> {
    let mut mutable_tail = state.pending_simplified_points.clone();
    for point in raw_tail {
        if mutable_tail.last().copied() != Some(*point) {
            mutable_tail.push(*point);
        }
    }
    let tail = simplify_polyline(&mutable_tail, epsilon);
    let mut points = Vec::with_capacity(state.committed_points.len() + tail.len());
    points.extend_from_slice(&state.committed_points);
    for point in tail {
        if points.last().copied() != Some(point) {
            points.push(point);
        }
    }
    points
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_interaction::{Modifiers, PointerButtons, PointerDevice};

    fn pointer(event_type: PointerEventType, position: Point<f64>) -> PointerEvent {
        PointerEvent {
            pointer_id: 7,
            event_type,
            device: PointerDevice::Pen,
            position,
            button: None,
            buttons: PointerButtons(PointerButtons::PRIMARY),
            modifiers: Modifiers::default(),
        }
    }

    fn distance_to_segment(point: Point<f64>, start: Point<f64>, end: Point<f64>) -> f64 {
        let dx = end.x - start.x;
        let dy = end.y - start.y;
        let length = dx * dx + dy * dy;
        let t = if length > 0.0 {
            (((point.x - start.x) * dx + (point.y - start.y) * dy) / length).clamp(0.0, 1.0)
        } else {
            0.0
        };
        ((point.x - start.x - t * dx).powi(2) + (point.y - start.y - t * dy).powi(2)).sqrt()
    }

    #[test]
    fn pen_filter_simplifies_samples_and_preserves_endpoints() {
        let mut editor = Editor::new(snow_draw_engine_core::EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::PenFilter).unwrap();
        editor.begin_pen_filter_creation(7, Point::new(1.0, 2.0));
        let document = DocumentModel::new();

        editor
            .process_pen_filter_creation_pointer_event(
                &document,
                pointer(PointerEventType::Move, Point::new(104.0, 105.0)),
            )
            .unwrap();
        editor
            .process_pen_filter_creation_pointer_event(
                &document,
                pointer(PointerEventType::Move, Point::new(104.0, 105.0)),
            )
            .unwrap();
        editor
            .process_pen_filter_creation_pointer_event(
                &document,
                pointer(PointerEventType::Move, Point::new(109.0, 112.0)),
            )
            .unwrap();

        let InteractionState::CreatingPenFilter(state) = &editor.state.interaction else {
            panic!("expected pen filter interaction");
        };
        let preview = streaming_points(state);
        assert_eq!(preview.first(), Some(&Point::new(1.0, 2.0)));
        assert_eq!(preview.last(), Some(&Point::new(9.0, 12.0)));

        let update = editor
            .process_input(
                &document,
                InputEvent::Pointer(pointer(PointerEventType::Up, Point::new(115.0, 118.0))),
            )
            .unwrap();
        let Some(EditorCommand::ApplyTransaction(command)) = update.command else {
            panic!("meaningful raw path should commit");
        };
        let mut document = document;
        document.apply_transaction(command.transaction).unwrap();
        let filter = document.pen_filter(document.paint_order()[0]).unwrap();
        let points = filter.global_points();
        assert_eq!(points.first(), Some(&Point::new(1.0, 2.0)));
        assert_eq!(points.last(), Some(&Point::new(15.0, 18.0)));
        assert!(points.len() <= 4);
    }

    #[test]
    fn zero_length_pen_filter_release_creates_nothing() {
        let mut editor = Editor::new(snow_draw_engine_core::EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::PenFilter).unwrap();
        editor.begin_pen_filter_creation(7, Point::new(3.0, 4.0));
        let document = DocumentModel::new();
        let update = editor
            .process_input(
                &document,
                InputEvent::Pointer(pointer(PointerEventType::Up, Point::new(103.0, 104.0))),
            )
            .unwrap();
        assert!(update.command.is_none());
        assert!(matches!(editor.state.interaction, InteractionState::Idle));
    }

    #[test]
    fn shift_replaces_one_pen_filter_straight_endpoint() {
        let mut state = CreatePenFilterState {
            pointer_id: 7,
            committed_points: Vec::new(),
            pending_simplified_points: Vec::new(),
            pending_raw_points: vec![Point::new(0.0, 0.0)],
            preview_committed_points: Vec::new(),
            epsilon: 0.75,
            straight_anchor: None,
            straight_endpoint: None,
        };

        // Starting Shift without moving must not turn the segment anchor into
        // the replaceable endpoint.
        assert!(!append_pen_filter_point(
            &mut state,
            Point::new(0.0, 0.0),
            true
        ));
        assert!(append_pen_filter_point(
            &mut state,
            Point::new(10.0, 3.0),
            true
        ));
        assert!(append_pen_filter_point(
            &mut state,
            Point::new(20.0, 8.0),
            true
        ));
        let points = streaming_points(&state);
        assert_eq!(points, vec![Point::new(0.0, 0.0), Point::new(20.0, 8.0)]);

        assert!(append_pen_filter_point(
            &mut state,
            Point::new(30.0, 30.0),
            false,
        ));
        assert_eq!(state.straight_anchor, None);
        assert_eq!(state.straight_endpoint, None);
        assert_eq!(
            state.pending_raw_points.last(),
            Some(&Point::new(30.0, 30.0))
        );
    }

    #[test]
    fn shift_preserves_the_straight_anchor_at_a_streaming_window_boundary() {
        let mut state = CreatePenFilterState {
            pointer_id: 7,
            committed_points: Vec::new(),
            pending_simplified_points: Vec::new(),
            pending_raw_points: vec![Point::new(0.0, 0.0)],
            preview_committed_points: Vec::new(),
            epsilon: 0.75,
            straight_anchor: None,
            straight_endpoint: None,
        };
        for index in 1..(PEN_FILTER_SIMPLIFICATION_WINDOW - 1) {
            state.pending_raw_points.push(Point::new(index as f64, 0.0));
        }
        let anchor = *state.pending_raw_points.last().unwrap();

        assert!(append_pen_filter_point(
            &mut state,
            Point::new(600.0, 0.0),
            true,
        ));
        assert!(append_pen_filter_point(
            &mut state,
            Point::new(700.0, 0.0),
            true,
        ));

        assert_eq!(
            streaming_points(&state),
            vec![Point::new(0.0, 0.0), anchor, Point::new(700.0, 0.0)]
        );
    }

    #[test]
    fn high_frequency_trace_is_reduced_within_the_error_budget() {
        let mut editor = Editor::new(snow_draw_engine_core::EngineConfig::default()).unwrap();
        editor.set_surface_size(2000, 400).unwrap();
        editor.set_active_tool(ActiveTool::PenFilter).unwrap();
        let raw = (0..=1000)
            .map(|index| Point::new(index as f64, (index % 2) as f64 * 0.2))
            .collect::<Vec<_>>();
        editor.begin_pen_filter_creation(7, raw[0]);
        let document = DocumentModel::new();
        for point in &raw[1..raw.len() - 1] {
            editor
                .process_pen_filter_creation_pointer_event(
                    &document,
                    pointer(
                        PointerEventType::Move,
                        Point::new(point.x + 1000.0, point.y + 200.0),
                    ),
                )
                .unwrap();
        }
        let update = editor
            .process_input(
                &document,
                InputEvent::Pointer(pointer(
                    PointerEventType::Up,
                    Point::new(
                        raw.last().unwrap().x + 1000.0,
                        raw.last().unwrap().y + 200.0,
                    ),
                )),
            )
            .unwrap();
        let Some(EditorCommand::ApplyTransaction(command)) = update.command else {
            panic!("trace should commit");
        };
        let mut document = document;
        document.apply_transaction(command.transaction).unwrap();
        let simplified = document
            .pen_filter(document.paint_order()[0])
            .unwrap()
            .global_points();
        assert!(simplified.len() * 5 <= raw.len());
        let maximum_deviation = raw
            .iter()
            .map(|point| {
                simplified
                    .windows(2)
                    .map(|edge| distance_to_segment(*point, edge[0], edge[1]))
                    .fold(f64::INFINITY, f64::min)
            })
            .fold(0.0, f64::max);
        assert!(
            maximum_deviation <= 1.2 + 1e-9,
            "deviation={maximum_deviation}"
        );
    }

    #[test]
    fn partial_window_preview_does_not_accumulate_subpixel_waves() {
        let mut state = CreatePenFilterState {
            pointer_id: 7,
            committed_points: Vec::new(),
            pending_simplified_points: Vec::new(),
            pending_raw_points: vec![Point::new(0.0, 0.0)],
            preview_committed_points: Vec::new(),
            epsilon: 0.75,
            straight_anchor: None,
            straight_endpoint: None,
        };
        for index in 1..3_000 {
            let x = index as f64 * 0.03;
            let y = (index as f64 * 0.15).sin() * 0.4;
            append_streaming_point(&mut state, Point::new(x, y));
        }

        let preview = streaming_points(&state);
        assert_eq!(preview.first(), Some(&Point::new(0.0, 0.0)));
        assert_eq!(preview.last(), state.pending_raw_points.last());
        assert!(
            preview.len() <= 100,
            "preview retained {} points",
            preview.len()
        );
    }

    #[test]
    fn preview_remains_compact_across_streaming_windows() {
        let mut state = CreatePenFilterState {
            pointer_id: 7,
            committed_points: Vec::new(),
            pending_simplified_points: Vec::new(),
            pending_raw_points: vec![Point::new(0.0, -0.2)],
            preview_committed_points: Vec::new(),
            epsilon: 0.75,
            straight_anchor: None,
            straight_endpoint: None,
        };
        for index in 1..32_000 {
            let progress = index as f64 / 31_999.0;
            let y = if index % 2 == 0 { -0.2 } else { 0.2 };
            append_streaming_point(&mut state, Point::new(progress * 1_700.0, y));
        }
        for index in 0..960 {
            let phase = index as f64;
            append_streaming_point(
                &mut state,
                Point::new(1_700.0 + phase * 0.03, (phase * 0.15).sin() * 0.4),
            );
        }

        let preview = streaming_points(&state);
        assert!(
            preview.len() <= 100,
            "preview retained {} points",
            preview.len()
        );
    }
}

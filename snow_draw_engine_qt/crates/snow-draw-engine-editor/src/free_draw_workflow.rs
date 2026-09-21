use std::sync::Arc;

use snow_draw_engine_core::{
    PATH_CHUNK_COMMAND_CAPACITY, PathGeometry, PathSegmentMode, Point, catmull_rom_path_commands,
};
use snow_draw_engine_document::{ElementMeta, FreeDrawData, FreeDrawStyle};

use super::*;

const MAX_PENDING_SAMPLES: usize = 128;
const PENDING_OVERLAP: usize = 4;
const MIN_FILTER_RESPONSE: f64 = 0.18;
const MAX_FILTER_RESPONSE: f64 = 0.82;

// Round caps only stroke a real segment, so a click without movement finishes as this
// sub-pixel stub to render a dot.
const DOT_SEGMENT_LENGTH: f64 = 1e-3;

// Resampling makes stabilization depend on stroke geometry instead of device event frequency.
#[derive(Clone, Debug, PartialEq)]
struct StrokePointFilter {
    last_raw: Point<f64>,
    filtered: Point<f64>,
    sample_spacing: f64,
    distance_until_sample: f64,
    response_distance: f64,
}

impl StrokePointFilter {
    fn new(start: Point<f64>, sample_spacing: f64, response_distance: f64) -> Self {
        Self {
            last_raw: start,
            filtered: start,
            sample_spacing,
            distance_until_sample: sample_spacing,
            response_distance,
        }
    }

    fn reset(&mut self, point: Point<f64>) {
        self.last_raw = point;
        self.filtered = point;
        self.distance_until_sample = self.sample_spacing;
    }

    fn ingest(&mut self, point: Point<f64>, output: &mut Vec<Point<f64>>) {
        let mut segment_start = self.last_raw;
        let mut segment_length = distance(segment_start, point);
        if segment_length <= 1e-12 {
            self.last_raw = point;
            return;
        }

        while segment_length + 1e-12 >= self.distance_until_sample {
            let ratio = self.distance_until_sample / segment_length;
            let sample = lerp_point(segment_start, point, ratio);
            output.push(self.stabilize(sample));
            segment_start = sample;
            segment_length = distance(segment_start, point);
            self.distance_until_sample = self.sample_spacing;
        }
        self.distance_until_sample = (self.distance_until_sample - segment_length).max(1e-12);
        self.last_raw = point;
    }

    fn stabilize(&mut self, sample: Point<f64>) -> Point<f64> {
        let normalized_error =
            (distance(self.filtered, sample) / self.response_distance).clamp(0.0, 1.0);
        let adaptive = smoothstep(normalized_error);
        let response = MIN_FILTER_RESPONSE + (MAX_FILTER_RESPONSE - MIN_FILTER_RESPONSE) * adaptive;
        self.filtered = lerp_point(self.filtered, sample, response);
        self.filtered
    }

    fn settle_endpoint(&mut self) -> Option<Point<f64>> {
        if distance(self.filtered, self.last_raw) <= 1e-12 {
            return None;
        }
        self.filtered = self.last_raw;
        self.distance_until_sample = self.sample_spacing;
        Some(self.filtered)
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct StreamingFreeDrawBuilder {
    committed_vertices: Vec<Point<f64>>,
    committed_modes: Vec<PathSegmentMode>,
    pending_samples: Vec<Point<f64>>,
    filtered_samples: Vec<Point<f64>>,
    point_filter: StrokePointFilter,
    shift_active: bool,
    rdp_tolerance: f64,
    closure_radius: f64,
    geometry_revision: u64,
    style: ShapeStyle,
    preview: Option<Arc<FreeDrawPreview>>,
    reverse_output: bool,
}

impl Default for StreamingFreeDrawBuilder {
    fn default() -> Self {
        Self::new(
            Point::default(),
            1.0,
            crate::defaults::editor_style_defaults().free_draw,
        )
    }
}

impl StreamingFreeDrawBuilder {
    pub(crate) fn new(start: Point<f64>, zoom: f64, style: ShapeStyle) -> Self {
        let stroke_width = style.stroke_width;
        let zoom = zoom.max(1e-6);
        let screen_width = stroke_width.max(0.0) * zoom;
        let sample_spacing = (screen_width * 0.03).clamp(0.75, 1.5) / zoom;
        let response_distance = (screen_width * 0.35).max(6.0) / zoom;
        Self {
            committed_vertices: Vec::new(),
            committed_modes: Vec::new(),
            pending_samples: vec![start],
            filtered_samples: Vec::new(),
            point_filter: StrokePointFilter::new(start, sample_spacing, response_distance),
            shift_active: false,
            rdp_tolerance: (screen_width * 0.06).clamp(0.35, 1.25) / zoom,
            closure_radius: 10.0_f64.max(screen_width * 0.5) / zoom,
            geometry_revision: 1,
            style,
            preview: None,
            reverse_output: false,
        }
    }

    fn continue_from(data: &FreeDrawData, at_start: bool, zoom: f64) -> Self {
        let mut vertices = data.global_vertices();
        let mut modes = data.segment_modes.clone();
        if at_start {
            vertices.reverse();
            modes.reverse();
        }
        let start = *vertices.last().expect("validated free draw has endpoints");
        let mut builder = Self::new(start, zoom, ShapeStyle::from_free_draw(data));
        builder.reverse_output = at_start;
        builder.committed_vertices = vertices;
        builder.committed_modes = modes;
        // Seed the exact original preview. Do not run closure detection until movement.
        builder.preview = Some(Arc::new(FreeDrawPreview {
            geometry: Arc::new(PathGeometry::from_commands(1, data.path_commands(), false)),
            stroke: data.stroke,
            stroke_width: data.stroke_width,
            stroke_style: data.stroke_style,
            fill: data.fill,
            fill_style: data.fill_style,
            opacity: data.opacity,
        }));
        builder
    }

    pub(crate) fn append(&mut self, point: Point<f64>, shift: bool) -> bool {
        if !point.x.is_finite() || !point.y.is_finite() {
            return false;
        }
        if shift {
            return self.append_straight(point);
        }
        if self.shift_active {
            self.shift_active = false;
            let anchor = self.last_point();
            self.pending_samples.clear();
            self.pending_samples.push(anchor);
            self.point_filter.reset(anchor);
        }
        self.filtered_samples.clear();
        self.point_filter.ingest(point, &mut self.filtered_samples);
        if self.filtered_samples.is_empty() {
            return false;
        }
        for index in 0..self.filtered_samples.len() {
            self.pending_samples.push(self.filtered_samples[index]);
            if self.pending_samples.len() == MAX_PENDING_SAMPLES {
                self.compact_pending_window();
            }
        }
        self.advance_revision();
        self.rebuild_preview();
        true
    }

    fn append_straight(&mut self, point: Point<f64>) -> bool {
        if !self.shift_active {
            if let Some(endpoint) = self.point_filter.settle_endpoint() {
                self.push_pending_sample(endpoint);
            }
            self.flush_pending();
            let start = self.last_point();
            if distance(start, point) < self.point_filter.sample_spacing {
                return false;
            }
            self.committed_vertices.push(point);
            self.committed_modes.push(PathSegmentMode::Straight);
            self.shift_active = true;
            self.advance_revision();
            self.rebuild_preview();
            return true;
        }
        let Some(endpoint) = self.committed_vertices.last_mut() else {
            return false;
        };
        if distance(*endpoint, point) < self.point_filter.sample_spacing {
            return false;
        }
        *endpoint = point;
        self.advance_revision();
        self.rebuild_preview();
        true
    }

    fn push_pending_sample(&mut self, point: Point<f64>) {
        if self
            .pending_samples
            .last()
            .is_some_and(|last| distance(*last, point) <= 1e-12)
        {
            return;
        }
        self.pending_samples.push(point);
        if self.pending_samples.len() == MAX_PENDING_SAMPLES {
            self.compact_pending_window();
        }
    }

    fn compact_pending_window(&mut self) {
        let boundary = MAX_PENDING_SAMPLES - PENDING_OVERLAP;
        let prefix = simplify_rdp(&self.pending_samples[..=boundary], self.rdp_tolerance);
        self.commit_curve_vertices(&prefix);
        self.pending_samples.drain(..boundary);
        debug_assert_eq!(self.pending_samples.len(), PENDING_OVERLAP);
    }

    fn flush_pending(&mut self) {
        if self.pending_samples.is_empty() {
            return;
        }
        let simplified = simplify_rdp(&self.pending_samples, self.rdp_tolerance);
        self.commit_curve_vertices(&simplified);
        self.pending_samples.clear();
    }

    fn commit_curve_vertices(&mut self, vertices: &[Point<f64>]) {
        for point in vertices {
            if self
                .committed_vertices
                .last()
                .is_some_and(|last| distance(*last, *point) <= 1e-12)
            {
                continue;
            }
            if !self.committed_vertices.is_empty() {
                self.committed_modes.push(PathSegmentMode::Curve);
            }
            self.committed_vertices.push(*point);
        }
    }

    pub(crate) fn preview(&self) -> Option<Arc<FreeDrawPreview>> {
        self.preview.clone()
    }

    fn rebuild_preview(&mut self) {
        let previous = self
            .preview
            .as_ref()
            .map(|preview| preview.geometry.as_ref());
        let closure_candidate = self
            .first_point()
            .is_some_and(|first| distance(first, self.last_point()) <= self.closure_radius);
        let mut full_vertices_and_modes = (closure_candidate
            || previous.is_some_and(|geometry| geometry.closed))
        .then(|| self.simplified_tail_vertices_and_modes(0));
        let preview_closed = closure_candidate
            && full_vertices_and_modes
                .as_ref()
                .is_some_and(|(vertices, _)| vertices.len() >= 4);
        let previous_command_count = previous.map_or(0, |geometry| {
            geometry
                .chunks
                .iter()
                .map(|chunk| chunk.commands.len())
                .sum()
        });
        let first_changed_command = self.committed_vertices.len().saturating_sub(3);
        let mut command_start =
            first_changed_command / PATH_CHUNK_COMMAND_CAPACITY * PATH_CHUNK_COMMAND_CAPACITY;
        if self.reverse_output || preview_closed || previous.is_some_and(|geometry| geometry.closed)
        {
            command_start = 0;
        }
        command_start = command_start.min(
            previous_command_count / PATH_CHUNK_COMMAND_CAPACITY * PATH_CHUNK_COMMAND_CAPACITY,
        );
        let vertex_start = if command_start == 0 {
            0
        } else {
            command_start - 2
        };
        let (mut vertices, mut modes) = full_vertices_and_modes
            .take()
            .unwrap_or_else(|| self.simplified_tail_vertices_and_modes(vertex_start));
        if vertices.len() < 2 {
            self.preview = None;
            return;
        }
        if preview_closed {
            vertices.pop();
            if modes.len() >= vertices.len() {
                modes.pop();
            }
            modes.push(if self.shift_active {
                PathSegmentMode::Straight
            } else {
                PathSegmentMode::Curve
            });
        }
        if self.reverse_output {
            vertices.reverse();
            modes.reverse();
            if preview_closed {
                modes.rotate_left(1);
            }
        }
        let mut commands = catmull_rom_path_commands(&vertices, &modes, preview_closed);
        let start_point = if command_start == 0 {
            [0.0; 2]
        } else {
            let point = vertices[1];
            commands.drain(..2);
            [point.x, point.y]
        };
        let geometry = Arc::new(if let Some(previous) = previous {
            previous.replace_tail(
                self.geometry_revision,
                command_start as u32,
                start_point,
                commands,
                preview_closed,
            )
        } else {
            PathGeometry::from_commands(self.geometry_revision, commands, preview_closed)
        });
        self.preview = Some(Arc::new(FreeDrawPreview {
            geometry,
            stroke: self.style.stroke,
            stroke_width: self.style.stroke_width,
            stroke_style: self.style.stroke_style,
            fill: self.style.fill,
            fill_style: self.style.fill_style,
            opacity: self.style.opacity,
        }));
    }

    pub(crate) fn finish(mut self) -> Option<(FreeDrawData, Arc<PathGeometry>)> {
        if !self.shift_active
            && let Some(endpoint) = self.point_filter.settle_endpoint()
        {
            self.push_pending_sample(endpoint);
            self.advance_revision();
            self.rebuild_preview();
        }
        let preview_geometry = self
            .preview
            .as_ref()
            .map(|preview| preview.geometry.clone());
        self.flush_pending();
        let &start = self.committed_vertices.first()?;
        if self.committed_vertices.len() == 1 {
            self.committed_vertices
                .push(Point::new(start.x + DOT_SEGMENT_LENGTH, start.y));
            self.committed_modes.push(PathSegmentMode::Curve);
        }
        let mut closed = false;
        if self.committed_vertices.len() >= 4
            && distance(self.committed_vertices[0], *self.committed_vertices.last()?)
                <= self.closure_radius
        {
            self.committed_vertices.pop();
            if self.committed_modes.len() >= self.committed_vertices.len() {
                self.committed_modes.pop();
            }
            if self.committed_vertices.len() >= 3 {
                closed = true;
                self.committed_modes.push(if self.shift_active {
                    PathSegmentMode::Straight
                } else {
                    PathSegmentMode::Curve
                });
            }
        }
        if self.reverse_output {
            self.committed_vertices.reverse();
            self.committed_modes.reverse();
            if closed {
                self.committed_modes.rotate_left(1);
            }
        }
        let data = FreeDrawData::from_global_vertices(
            &self.committed_vertices,
            self.committed_modes,
            closed,
            FreeDrawStyle {
                stroke: self.style.stroke,
                stroke_width: self.style.stroke_width,
                stroke_style: self.style.stroke_style,
                fill: self.style.fill,
                fill_style: self.style.fill_style,
                opacity: self.style.opacity,
            },
        )?;
        let geometry = if closed {
            Arc::new(data.path_geometry(self.geometry_revision))
        } else {
            preview_geometry.unwrap_or_else(|| Arc::new(data.path_geometry(self.geometry_revision)))
        };
        Some((data, geometry))
    }

    #[cfg(test)]
    fn simplified_vertices_and_modes(&self) -> (Vec<Point<f64>>, Vec<PathSegmentMode>) {
        self.simplified_tail_vertices_and_modes(0)
    }

    fn simplified_tail_vertices_and_modes(
        &self,
        vertex_start: usize,
    ) -> (Vec<Point<f64>>, Vec<PathSegmentMode>) {
        let vertex_start = vertex_start.min(self.committed_vertices.len());
        let mut vertices = self.committed_vertices[vertex_start..].to_vec();
        let mut modes =
            self.committed_modes[vertex_start.min(self.committed_modes.len())..].to_vec();
        let mut tail_samples = self.pending_samples.clone();
        if !self.shift_active {
            let endpoint = self.point_filter.last_raw;
            if tail_samples
                .last()
                .is_none_or(|last| distance(*last, endpoint) > 1e-12)
            {
                tail_samples.push(endpoint);
            }
        }
        let simplified = simplify_rdp(&tail_samples, self.rdp_tolerance);
        for point in simplified {
            if vertices
                .last()
                .is_some_and(|last| distance(*last, point) <= 1e-12)
            {
                continue;
            }
            if !vertices.is_empty() {
                modes.push(PathSegmentMode::Curve);
            }
            vertices.push(point);
        }
        (vertices, modes)
    }

    fn last_point(&self) -> Point<f64> {
        self.pending_samples
            .last()
            .copied()
            .or_else(|| self.committed_vertices.last().copied())
            .unwrap_or_default()
    }

    fn first_point(&self) -> Option<Point<f64>> {
        self.committed_vertices
            .first()
            .copied()
            .or_else(|| self.pending_samples.first().copied())
    }

    fn advance_revision(&mut self) {
        self.geometry_revision = self.geometry_revision.wrapping_add(1).max(1);
    }

    #[cfg(test)]
    fn pending_len(&self) -> usize {
        self.pending_samples.len()
    }
    #[cfg(test)]
    fn committed_vertices(&self) -> &[Point<f64>] {
        &self.committed_vertices
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct FreeDrawContinuation {
    id: ElementId,
    original: FreeDrawData,
    meta: ElementMeta,
    at_start: bool,
    endpoint: Point<f64>,
    press: Point<f64>,
    moved: bool,
}

impl FreeDrawContinuation {
    fn valid(&self, document: &DocumentModel) -> bool {
        document
            .free_draw(self.id)
            .is_ok_and(|data| data == &self.original)
            && document
                .element(self.id)
                .is_ok_and(|element| element.meta == self.meta)
    }

    fn sample(&mut self, point: Point<f64>) -> Point<f64> {
        self.moved |= distance(point, self.press) > 1e-9;
        if self.moved { point } else { self.endpoint }
    }
}

impl Editor {
    pub(crate) fn resolve_free_draw_endpoint(
        &self,
        document: &DocumentModel,
        point: Point<f64>,
    ) -> Option<(ElementId, bool, Point<f64>)> {
        if self.state.active_tool != ActiveTool::FreeDraw
            || !matches!(self.state.interaction, InteractionState::Idle)
        {
            return None;
        }
        let zoom = self.camera().zoom.max(1e-6);
        // A small viewport query uses the document spatial index and returns paint order.
        let candidates = document.visible_element_ids(snow_draw_engine_core::ViewportQuery {
            camera: snow_draw_engine_core::Camera {
                center: point,
                zoom,
            },
            surface: snow_draw_engine_core::SurfaceSize {
                width: 18,
                height: 18,
            },
        });
        let mut best = None;
        let mut best_distance = 8.0 / zoom;
        for id in candidates.into_iter().rev() {
            let Ok(element) = document.element(id) else {
                continue;
            };
            if !element.meta.visible || element.meta.locked {
                continue;
            }
            let Ok(data) = document.free_draw(id) else {
                continue;
            };
            if data.closed {
                continue;
            }
            let vertices = data.global_vertices();
            for (at_start, endpoint) in [(false, vertices.last()), (true, vertices.first())] {
                let Some(&endpoint) = endpoint else {
                    continue;
                };
                let d = distance(point, endpoint);
                if d <= best_distance && (best.is_none() || d < best_distance) {
                    best_distance = d;
                    best = Some((id, at_start, endpoint));
                }
            }
        }
        best
    }

    pub(crate) fn free_draw_endpoint_feedback(
        &self,
        document: &DocumentModel,
    ) -> Option<Point<f64>> {
        let position = self.state.ui.free_draw_hover_position?;
        let point = view_to_canvas(position, &self.camera(), self.surface_size());
        self.resolve_free_draw_endpoint(document, point)
            .map(|(_, _, endpoint)| endpoint)
    }

    pub(crate) fn begin_free_draw_continuation(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
        (id, at_start, endpoint): (ElementId, bool, Point<f64>),
    ) {
        let original = document.free_draw(id).expect("resolved endpoint").clone();
        let meta = document.element(id).expect("resolved endpoint").meta;
        let builder =
            StreamingFreeDrawBuilder::continue_from(&original, at_start, self.camera().zoom);
        let press = view_to_canvas(event.position, &self.camera(), self.surface_size());
        self.set_hovered_element(None);
        self.state.interaction = InteractionState::CreatingFreeDraw(CreateFreeDrawState {
            pointer_id: event.pointer_id,
            builder,
            continuation: Some(FreeDrawContinuation {
                id,
                original,
                meta,
                at_start,
                endpoint,
                press,
                moved: false,
            }),
        });
        self.state.creation_preview = None;
        self.bump_scene_state_revision();
    }

    pub(crate) fn free_draw_replacement_preview(
        &self,
        document: &DocumentModel,
    ) -> Option<(ElementId, Arc<FreeDrawPreview>)> {
        let InteractionState::CreatingFreeDraw(state) = &self.state.interaction else {
            return None;
        };
        let target = state.continuation.as_ref()?;
        if !target.valid(document) {
            return None;
        }
        Some((target.id, state.builder.preview()?))
    }
}

impl Editor {
    pub(crate) fn begin_free_draw_creation(
        &mut self,
        pointer_id: u32,
        start_canvas_position: Point<f64>,
    ) {
        let style = self.state.default_free_draw_style;
        self.set_hovered_element(None);
        self.state.interaction = InteractionState::CreatingFreeDraw(CreateFreeDrawState {
            pointer_id,
            continuation: None,
            builder: StreamingFreeDrawBuilder::new(
                start_canvas_position,
                self.camera().zoom,
                style,
            ),
        });
        self.state.creation_preview = None;
        self.bump_scene_state_revision();
    }

    pub(crate) fn process_free_draw_creation_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        if let InteractionState::CreatingFreeDraw(state) = &self.state.interaction
            && state
                .continuation
                .as_ref()
                .is_some_and(|target| !target.valid(document))
        {
            return self.handle_free_draw_pointer_cancel(event);
        }
        match event.event_type {
            PointerEventType::Move | PointerEventType::Enter => {
                self.handle_free_draw_pointer_move(event)
            }
            PointerEventType::Up => self.handle_free_draw_pointer_up(document, event),
            PointerEventType::Cancel => self.handle_free_draw_pointer_cancel(event),
            PointerEventType::Down | PointerEventType::DoubleClick | PointerEventType::Leave => {
                Ok(InteractionOutput::default())
            }
        }
    }

    fn handle_free_draw_pointer_move(
        &mut self,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let InteractionState::CreatingFreeDraw(state) = &mut self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }
        let point = state
            .continuation
            .as_mut()
            .map_or(point, |target| target.sample(point));
        let changed = state.builder.append(point, event.modifiers.shift);
        if changed {
            self.bump_scene_state_revision();
        }
        Ok(InteractionOutput {
            consumed: true,
            capture: PointerCaptureCommand::NoChange,
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
        })
    }

    fn handle_free_draw_pointer_up(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let interaction = std::mem::replace(&mut self.state.interaction, InteractionState::Idle);
        let InteractionState::CreatingFreeDraw(mut state) = interaction else {
            return Ok(InteractionOutput::default());
        };
        if state.pointer_id != event.pointer_id {
            self.state.interaction = InteractionState::CreatingFreeDraw(state);
            return Ok(InteractionOutput::default());
        }
        let point = state
            .continuation
            .as_mut()
            .map_or(point, |target| target.sample(point));
        state.builder.append(point, event.modifiers.shift);
        let finalized = if state
            .continuation
            .as_ref()
            .is_some_and(|target| !target.moved)
        {
            None
        } else {
            state.builder.finish()
        };
        self.cancel_interaction();
        self.bump_scene_state_revision();
        if let Some((free_draw, _geometry)) = finalized {
            let mut transaction = Transaction::new(if state.continuation.is_some() {
                "continue free draw"
            } else {
                "create free draw"
            });
            if let Some(target) = state.continuation {
                transaction.update_free_draw(target.id, free_draw);
            } else {
                transaction.insert_free_draw(
                    document.peek_next_element_id(),
                    ElementMeta::default(),
                    free_draw,
                );
            }
            self.queue_command(EditorCommand::ApplyTransaction(
                ApplyTransactionCommand::new(transaction),
            ));
        }
        Ok(InteractionOutput {
            consumed: true,
            capture: PointerCaptureCommand::Release,
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
        })
    }

    fn handle_free_draw_pointer_cancel(
        &mut self,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingFreeDraw(state) = &self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }
        self.cancel_interaction();
        self.bump_scene_state_revision();
        Ok(InteractionOutput {
            consumed: true,
            capture: PointerCaptureCommand::Release,
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
        })
    }

    pub(crate) fn free_draw_creation_preview(&self) -> Option<ElementCreationPreview> {
        let InteractionState::CreatingFreeDraw(state) = &self.state.interaction else {
            return None;
        };
        if state.continuation.is_some() {
            return None;
        }
        state
            .builder
            .preview()
            .map(ElementCreationPreview::FreeDraw)
    }
}

fn simplify_rdp(points: &[Point<f64>], tolerance: f64) -> Vec<Point<f64>> {
    if points.len() <= 2 {
        return points.to_vec();
    }
    let mut keep = vec![false; points.len()];
    keep[0] = true;
    keep[points.len() - 1] = true;
    let mut stack = vec![(0usize, points.len() - 1)];
    while let Some((start, end)) = stack.pop() {
        let mut maximum = tolerance;
        let mut furthest = None;
        for index in start + 1..end {
            let distance = distance_to_segment(points[index], points[start], points[end]);
            if distance > maximum {
                maximum = distance;
                furthest = Some(index);
            }
        }
        if let Some(index) = furthest {
            keep[index] = true;
            stack.push((start, index));
            stack.push((index, end));
        }
    }
    points
        .iter()
        .zip(keep)
        .filter_map(|(point, keep)| keep.then_some(*point))
        .collect()
}

fn distance_to_segment(point: Point<f64>, start: Point<f64>, end: Point<f64>) -> f64 {
    let dx = end.x - start.x;
    let dy = end.y - start.y;
    let length_sq = dx * dx + dy * dy;
    if length_sq <= 1e-12 {
        return distance(point, start);
    }
    let t = (((point.x - start.x) * dx + (point.y - start.y) * dy) / length_sq).clamp(0.0, 1.0);
    distance(point, Point::new(start.x + dx * t, start.y + dy * t))
}

fn distance(left: Point<f64>, right: Point<f64>) -> f64 {
    (right.x - left.x).hypot(right.y - left.y)
}

fn lerp_point(start: Point<f64>, end: Point<f64>, ratio: f64) -> Point<f64> {
    Point::new(
        start.x + (end.x - start.x) * ratio,
        start.y + (end.y - start.y) * ratio,
    )
}

fn smoothstep(value: f64) -> f64 {
    value * value * (3.0 - 2.0 * value)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn continuation_preserves_long_mixed_history_and_matches_preview() {
        for at_start in [false, true] {
            let vertices: Vec<_> = (0..600)
                .map(|i| Point::new(i as f64 * 3.0, (i as f64 * 0.4).sin() * 20.0))
                .collect();
            let modes: Vec<_> = (0..599)
                .map(|i| {
                    if i % 3 == 0 {
                        PathSegmentMode::Straight
                    } else {
                        PathSegmentMode::Curve
                    }
                })
                .collect();
            let data = FreeDrawData::from_global_vertices(
                &vertices,
                modes.clone(),
                false,
                FreeDrawStyle {
                    stroke: Default::default(),
                    stroke_width: 2.0,
                    stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
                    fill: Default::default(),
                    fill_style: snow_draw_engine_document::FillStyle::Solid,
                    opacity: 0.5,
                },
            )
            .unwrap();
            let mut builder = StreamingFreeDrawBuilder::continue_from(&data, at_start, 1.0);
            let endpoint = if at_start {
                vertices[0]
            } else {
                *vertices.last().unwrap()
            };
            // Shift on an unmoved endpoint must not edit the original terminal segment.
            assert!(!builder.append(endpoint, true));
            let direction = if at_start { -1.0 } else { 1.0 };
            builder.append(Point::new(endpoint.x + direction * 30.0, endpoint.y), true);
            for i in 1..300 {
                builder.append(
                    Point::new(
                        endpoint.x + direction * (30.0 + i as f64),
                        endpoint.y + i as f64 * 0.5,
                    ),
                    false,
                );
            }
            let preview = builder.preview().unwrap();
            let (result, geometry) = builder.finish().unwrap();
            // Normalizing into the new bounds can introduce floating-point rounding.
            fn coordinates(command: &snow_draw_engine_core::PathCommand) -> Vec<f64> {
                use snow_draw_engine_core::PathCommand::*;
                match command {
                    MoveTo { point } | LineTo { point } => point.to_vec(),
                    QuadTo { control, end } => [*control, *end].concat(),
                    CubicTo {
                        control_1,
                        control_2,
                        end,
                    } => [*control_1, *control_2, *end].concat(),
                }
            }
            let expected = result.path_commands();
            for commands in [
                preview.geometry.flattened_commands(),
                geometry.flattened_commands(),
            ] {
                assert_eq!(commands.len(), expected.len());
                for (index, (actual, expected)) in commands.iter().zip(&expected).enumerate() {
                    assert_eq!(
                        std::mem::discriminant(actual),
                        std::mem::discriminant(expected)
                    );
                    for (a, b) in coordinates(actual).iter().zip(coordinates(expected)) {
                        assert!((a - b).abs() < 1e-9, "command {index}: {a} vs {b}");
                    }
                }
            }
            let actual = result.global_vertices();
            let old = if at_start {
                &actual[actual.len() - vertices.len()..]
            } else {
                &actual[..vertices.len()]
            };
            for (a, b) in old.iter().zip(&vertices) {
                assert!(distance(*a, *b) < 1e-9);
            }
            let old_modes = if at_start {
                &result.segment_modes[result.segment_modes.len() - modes.len()..]
            } else {
                &result.segment_modes[..modes.len()]
            };
            assert_eq!(old_modes, modes);
        }
    }

    #[test]
    fn rejects_non_finite_duplicates_and_sub_spacing_samples() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 2.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        assert!(!builder.append(Point::new(0.0, 0.0), false));
        assert!(!builder.append(Point::new(f64::NAN, 1.0), false));
        assert!(!builder.append(Point::new(0.25, 0.0), false));
        assert!(builder.append(Point::new(1.0, 0.0), false));
    }

    #[test]
    fn pending_window_is_bounded_and_frozen_prefix_does_not_change() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        for index in 1..1000 {
            builder.append(Point::new(index as f64, (index % 11) as f64), false);
            assert!(builder.pending_len() <= MAX_PENDING_SAMPLES);
        }
        let frozen = builder.committed_vertices().to_vec();
        for index in 1000..1100 {
            builder.append(Point::new(index as f64, (index % 11) as f64), false);
        }
        assert_eq!(
            &builder.committed_vertices()[..frozen.len()],
            frozen.as_slice()
        );
    }

    #[test]
    fn steady_append_reuses_prefix_and_changes_at_most_three_chunks() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        for index in 1..=300 {
            builder.append(
                Point::new(index as f64, if index % 2 == 0 { 10.0 } else { -10.0 }),
                false,
            );
        }
        let before = builder.preview().unwrap().geometry.clone();
        builder.append(Point::new(301.0, -12.0), false);
        let after = builder.preview().unwrap().geometry.clone();
        let unchanged_prefix = before
            .chunks
            .iter()
            .zip(after.chunks.iter())
            .take_while(|(left, right)| Arc::ptr_eq(&left.commands, &right.commands))
            .count();

        assert!(unchanged_prefix > 0);
        assert!(after.chunks.len().saturating_sub(unchanged_prefix) <= 3);
    }

    #[test]
    fn rdp_retains_corners() {
        let points = [
            Point::new(0.0, 0.0),
            Point::new(10.0, 0.0),
            Point::new(10.0, 10.0),
        ];
        assert_eq!(simplify_rdp(&points, 0.5), points);
    }

    #[test]
    fn shift_creates_and_replaces_one_straight_endpoint() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        builder.append(Point::new(5.0, 1.0), false);
        builder.append(Point::new(10.0, 0.0), true);
        builder.append(Point::new(20.0, 0.0), true);
        let (vertices, modes) = builder.simplified_vertices_and_modes();
        assert_eq!(vertices.last(), Some(&Point::new(20.0, 0.0)));
        assert_eq!(modes.last(), Some(&PathSegmentMode::Straight));
    }

    #[test]
    fn finish_closes_without_duplicate_vertex_and_preview_matches_open_final() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        style.fill = snow_draw_engine_core::ColorRgba8 {
            r: 255,
            g: 0,
            b: 0,
            a: 255,
        };
        style.fill_style = snow_draw_engine_document::FillStyle::Line;
        let mut open = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        open.append(Point::new(20.0, 0.0), false);
        open.append(Point::new(30.0, 10.0), false);
        let preview = open.preview().unwrap();
        let (final_data, final_geometry) = open.finish().unwrap();
        assert_eq!(
            preview.geometry.flattened_commands(),
            final_geometry.flattened_commands()
        );
        let final_vertices = final_data.global_vertices();
        assert_eq!(final_vertices.first(), Some(&Point::new(0.0, 0.0)));
        assert_eq!(final_vertices.last(), Some(&Point::new(30.0, 10.0)));
        assert!(final_vertices.len() >= 3);

        let mut closed = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        closed.append(Point::new(20.0, 0.0), false);
        closed.append(Point::new(20.0, 20.0), false);
        closed.append(Point::new(1.0, 1.0), false);
        let preview = closed.preview().unwrap();
        assert!(preview.geometry.closed);
        assert_eq!(
            preview.fill_style,
            snow_draw_engine_document::FillStyle::Line
        );
        closed.append(Point::new(40.0, 40.0), false);
        assert!(!closed.preview().unwrap().geometry.closed);
        closed.append(Point::new(1.0, 1.0), false);
        assert!(closed.preview().unwrap().geometry.closed);
        let (data, _) = closed.finish().unwrap();
        assert!(data.closed);
        assert_ne!(data.vertices.first(), data.vertices.last());
        assert_eq!(data.segment_modes.len(), data.vertices.len());
    }

    #[test]
    fn finish_without_drag_registers_a_dot() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let builder = StreamingFreeDrawBuilder::new(Point::new(12.0, 34.0), 1.0, style);
        let (data, _) = builder
            .finish()
            .expect("a click without movement draws a dot");
        let vertices = data.global_vertices();
        assert_eq!(vertices.len(), 2);
        assert_eq!(vertices[0], Point::new(12.0, 34.0));
        assert!(distance(vertices[0], vertices[1]) <= DOT_SEGMENT_LENGTH);
        assert!(!data.closed);
    }

    #[test]
    fn finish_accepts_strokes_of_any_length() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        builder.append(Point::new(0.5, 0.0), false);
        let (data, _) = builder
            .finish()
            .expect("a short drag still registers a stroke");
        assert_eq!(data.global_vertices().last(), Some(&Point::new(0.5, 0.0)));
    }

    #[test]
    fn stabilization_reduces_slow_pointer_jitter() {
        let mut filter = StrokePointFilter::new(Point::new(0.0, 0.0), 1.0, 6.0);
        let mut filtered = Vec::new();
        for x in 1..=120 {
            let y = if x % 2 == 0 { 1.0 } else { -1.0 };
            filter.ingest(Point::new(x as f64, y), &mut filtered);
        }

        let settled = &filtered[12..];
        let maximum_deviation = settled
            .iter()
            .map(|point| point.y.abs())
            .fold(0.0_f64, f64::max);
        assert!(
            maximum_deviation < 0.35,
            "stabilized deviation was {maximum_deviation}"
        );
    }

    #[test]
    fn finalized_pipeline_keeps_stabilized_history_and_exact_endpoint() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        for x in 1..=120 {
            let y = if x % 2 == 0 { 1.0 } else { -1.0 };
            builder.append(Point::new(x as f64, y), false);
        }

        let (data, _) = builder.finish().unwrap();
        let vertices = data.global_vertices();
        let maximum_interior_deviation = vertices[1..vertices.len() - 1]
            .iter()
            .map(|point| point.y.abs())
            .fold(0.0_f64, f64::max);
        assert!(
            maximum_interior_deviation < 0.5,
            "persisted deviation was {maximum_interior_deviation}"
        );
        assert_eq!(vertices.last(), Some(&Point::new(120.0, 1.0)));
    }

    #[test]
    fn spatial_resampling_is_independent_of_event_density() {
        fn filtered_polyline(subdivisions: usize) -> Vec<Point<f64>> {
            let controls = [
                Point::new(0.0, 0.0),
                Point::new(30.0, 8.0),
                Point::new(60.0, -6.0),
                Point::new(90.0, 0.0),
            ];
            let mut filter = StrokePointFilter::new(controls[0], 1.0, 6.0);
            let mut output = Vec::new();
            for segment in controls.windows(2) {
                for step in 1..=subdivisions {
                    filter.ingest(
                        lerp_point(segment[0], segment[1], step as f64 / subdivisions as f64),
                        &mut output,
                    );
                }
            }
            output
        }

        let sparse = filtered_polyline(1);
        let dense = filtered_polyline(20);
        assert_eq!(sparse.len(), dense.len());
        assert!(
            sparse
                .iter()
                .zip(dense)
                .all(|(left, right)| distance(*left, right) < 1e-9)
        );
    }

    #[test]
    fn adaptive_response_tracks_deliberate_corners() {
        let mut filter = StrokePointFilter::new(Point::new(0.0, 0.0), 1.0, 6.0);
        let mut filtered = Vec::new();
        for x in 1..=40 {
            filter.ingest(Point::new(x as f64, 0.0), &mut filtered);
        }
        for y in 1..=40 {
            filter.ingest(Point::new(40.0, y as f64), &mut filtered);
        }

        let corner = Point::new(40.0, 0.0);
        let closest_corner_distance = filtered
            .iter()
            .map(|point| distance(*point, corner))
            .fold(f64::INFINITY, f64::min);
        assert!(
            closest_corner_distance < 3.0,
            "corner miss distance was {closest_corner_distance}"
        );
        assert!(distance(*filtered.last().unwrap(), Point::new(40.0, 40.0)) < 3.0);
    }

    #[test]
    fn finalized_stroke_preserves_exact_pointer_endpoint() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let endpoint = Point::new(47.25, 13.75);
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        builder.append(Point::new(20.0, 2.0), false);
        builder.append(endpoint, false);

        let (data, _) = builder.finish().unwrap();
        assert_eq!(data.global_vertices().last(), Some(&endpoint));
    }

    #[test]
    fn smoothing_parameters_are_locked_to_screen_space() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let normal = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        let zoomed = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 2.0, style);

        assert_eq!(
            normal.point_filter.sample_spacing,
            zoomed.point_filter.sample_spacing * 2.0
        );
        assert_eq!(
            normal.point_filter.response_distance,
            zoomed.point_filter.response_distance * 2.0
        );
        assert_eq!(normal.rdp_tolerance, zoomed.rdp_tolerance * 2.0);
    }
}

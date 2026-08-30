use snow_draw_engine_core::{
    Camera, Point, SurfaceSize, arrow::ArrowEndpointEdge, canvas_to_view, canvas_viewport,
    rectangle_intersects_viewport,
};
use snow_draw_engine_document::{
    ArrowData, ElementId, MIN_TEXT_FONT_SIZE, RectangleData, SerialNumberData, arrow_hit_test,
    arrow_segment_midpoints,
};
use snow_draw_engine_model::DocumentModel;

use crate::{
    ActiveTool, ArrowHandleKind, ArrowHandleState, Editor, EditorPresentationState,
    EditorStrokeCursor, EditorViewState, SelectionArrowState, SelectionBounds, SelectionRectState,
    SerialNumberToolbarState, TextPreviewFontSize,
    geometry::{
        element_hit_tolerance, selection_bounds_from_selection, selection_handle_hit_size,
        selection_handle_size, text_resize_changes_width_only,
    },
    state::{ArrowHitTarget, InteractionState, ResizeHandle, SelectionEditMode},
    text::text_resize_layout_override_matches_rect,
};
use crate::{
    arrow_endpoint_index, arrow_loop_active, point_distance, segment_handle_visible,
    segment_is_fixed, visible_arrow_focus_points, visible_arrow_point_indices,
};

const SERIAL_TOOLBAR_WIDTH: f64 = 72.0;
const SERIAL_TOOLBAR_HEIGHT: f64 = 24.0;
const SERIAL_TOOLBAR_VERTICAL_OFFSET: f64 = 16.0;

fn selection_bounds_view_rect(
    bounds: &SelectionBounds,
    camera: &Camera,
    surface: SurfaceSize,
) -> (f64, f64, f64, f64) {
    let half_width = bounds.width / 2.0;
    let half_height = bounds.height / 2.0;
    let (sin, cos) = bounds.rotation.sin_cos();
    let mut min_x = f64::INFINITY;
    let mut min_y = f64::INFINITY;
    let mut max_x = f64::NEG_INFINITY;
    let mut max_y = f64::NEG_INFINITY;

    for (local_x, local_y) in [
        (-half_width, -half_height),
        (half_width, -half_height),
        (half_width, half_height),
        (-half_width, half_height),
    ] {
        let canvas_point = Point::new(
            bounds.center.x + local_x * cos - local_y * sin,
            bounds.center.y + local_x * sin + local_y * cos,
        );
        let view_point = canvas_to_view(canvas_point, camera, surface);
        min_x = min_x.min(view_point.x);
        min_y = min_y.min(view_point.y);
        max_x = max_x.max(view_point.x);
        max_y = max_y.max(view_point.y);
    }

    (min_x, min_y, max_x, max_y)
}

impl Editor {
    pub fn scene_input_revision(&self) -> u64 {
        self.scene_state_revision
    }

    pub fn overlay_input_revision(&self) -> u64 {
        self.overlay_state_revision
    }

    pub fn view_state(&self) -> EditorViewState {
        EditorViewState {
            surface: self.view.surface,
            camera: self.view.camera,
            clear_color: self.config.clear_color,
        }
    }

    pub fn presentation_state(&self, document: &DocumentModel) -> EditorPresentationState {
        let mut preview_elements = self
            .selection_preview_elements()
            .map(<[SelectionRectState]>::to_vec)
            .unwrap_or_default();
        let mut preview_arrows = self.preview_selection_arrows(document);
        for id in &self.state.eraser.pending_ids {
            preview_elements.retain(|preview| preview.id != *id);
            preview_arrows.retain(|preview| preview.id != *id);
            if let Ok(arrow) = document.arrow(*id) {
                let mut arrow = arrow.clone();
                arrow.opacity = (arrow.opacity * 0.5).clamp(0.0, 1.0);
                preview_arrows.push(SelectionArrowState { id: *id, arrow });
            } else if let Some(mut rect) = document.element_rect_proxy(*id) {
                rect.opacity = (rect.opacity * 0.5).clamp(0.0, 1.0);
                preview_elements.push(SelectionRectState { id: *id, rect });
            }
        }
        let selection_elements = self.presentation_selection_elements(document);
        let selection_arrows = self.presentation_selection_arrows(document);
        let selection_bounds =
            self.presentation_selection_bounds(&selection_elements, &selection_arrows);
        let marquee_candidate_elements = self.marquee_candidate_elements(document);
        let marquee_candidate_arrows = self.marquee_candidate_arrows(document);
        let text_rect_ids = text_rect_ids_for_groups(
            document,
            &[
                preview_elements.as_slice(),
                marquee_candidate_elements.as_slice(),
                selection_elements.as_slice(),
            ],
        );
        let selected_single_rect = self
            .selected_single_rectangle_snapshot(document)
            .map(|(_, rect)| rect);
        let selected_single_text_rect = self
            .selected_single_text_rect_snapshot(document)
            .map(|(_, rect)| rect);
        let selected_single_arrow = self
            .selected_single_arrow_snapshot(document)
            .map(|(_, arrow)| arrow);
        let arrow_handles = self
            .selected_single_arrow_snapshot(document)
            .map_or_else(Vec::new, |(arrow_id, arrow)| {
                self.arrow_handle_states(document, arrow_id, &arrow)
            });
        let stroke_cursor = self
            .state
            .stroke_cursor_canvas_position
            .zip(match self.state.active_tool {
                ActiveTool::FreeDraw | ActiveTool::PenHighlight => {
                    Some(self.shape_style(document).stroke_width)
                }
                ActiveTool::PenFilter => Some(self.filter_style(document).stroke_width),
                _ => None,
            })
            .and_then(|(position, stroke_width)| {
                (stroke_width.is_finite() && stroke_width > 0.0).then_some(EditorStrokeCursor {
                    position,
                    stroke_width,
                })
            });
        EditorPresentationState {
            creation_preview: self
                .pen_filter_creation_preview()
                .or_else(|| self.free_draw_creation_preview())
                .or_else(|| self.state.creation_preview.clone()),
            active_text_draft: self.active_text_draft_display_presentation(),
            preview_arrows,
            preview_elements,
            preview_text_font_sizes: self.selection_preview_text_font_sizes(document),
            marquee: self.state.ui.marquee,
            marquee_candidate_elements,
            marquee_candidate_arrows,
            text_rect_ids,
            hovered_rect: self.hovered_rect(document),
            hovered_free_draw: self.hovered_free_draw(document),
            hovered_pen_filter: self.hovered_pen_filter(document),
            hovered_text_rect: self.hovered_text_rect(document),
            hovered_arrow: self.hovered_arrow(document),
            selection_bounds,
            selection_elements,
            selection_arrows,
            selected_single_rect,
            selected_single_text_rect,
            selected_single_arrow,
            arrow_handles,
            snap_guides: self.state.ui.snap_guides.clone(),
            eraser_cursor: self.state.eraser.cursor_canvas_position,
            stroke_cursor,
        }
    }

    pub fn serial_number_toolbar_state(
        &self,
        document: &DocumentModel,
    ) -> SerialNumberToolbarState {
        if self.state.selection.ids.len() != 1 {
            return SerialNumberToolbarState::default();
        }
        let Some(serial_numbers) = self.selected_serial_number_snapshots(document) else {
            return SerialNumberToolbarState::default();
        };
        let Some(bounds) = self.selection_bounds_snapshot(document) else {
            return SerialNumberToolbarState::default();
        };
        let surface = self.surface_size();
        if surface.width == 0 || surface.height == 0 {
            return SerialNumberToolbarState::default();
        }
        let Some(selected_rect) = self
            .presentation_selection_elements(document)
            .into_iter()
            .next()
        else {
            return SerialNumberToolbarState::default();
        };
        if !rectangle_intersects_viewport(
            selected_rect.rect.center,
            selected_rect.rect.width,
            selected_rect.rect.height,
            selected_rect.rect.rotation,
            selected_rect.rect.stroke_width,
            canvas_viewport(self.camera(), surface),
        ) {
            return SerialNumberToolbarState::default();
        }

        let (selection_left, _, selection_right, selection_bottom) =
            selection_bounds_view_rect(&bounds, &self.camera(), surface);
        let center_x = (selection_left + selection_right) / 2.0;
        let top = selection_bottom + SERIAL_TOOLBAR_VERTICAL_OFFSET;

        SerialNumberToolbarState {
            visible: true,
            left: center_x - SERIAL_TOOLBAR_WIDTH / 2.0,
            top,
            width: SERIAL_TOOLBAR_WIDTH,
            height: SERIAL_TOOLBAR_HEIGHT,
            can_decrease: serial_numbers.iter().any(|(_, serial)| serial.number > 0),
            can_increase: true,
            can_create_text: true,
        }
    }

    pub(crate) fn selection_preview_text_font_sizes(
        &self,
        document: &DocumentModel,
    ) -> Vec<TextPreviewFontSize> {
        let InteractionState::EditingSelection(state) = &self.state.interaction else {
            return Vec::new();
        };
        let SelectionEditMode::Resize {
            handle,
            text_layout_override,
            ..
        } = state.mode
        else {
            return Vec::new();
        };
        let single_text_resize = state.original_arrows.is_empty()
            && state.original_elements.len() == 1
            && document.text(state.original_elements[0].id).is_ok();

        state
            .preview_elements
            .iter()
            .filter_map(|preview| {
                let original = state
                    .original_elements
                    .iter()
                    .find(|element| element.id == preview.id)?;
                let text = document.text(preview.id).ok()?;
                let font_size = if single_text_resize
                    && !text_resize_changes_width_only(handle)
                    && let Some(layout_override) = text_layout_override
                    && text_resize_layout_override_matches_rect(layout_override, preview.rect)
                {
                    Some(layout_override.requested_font_size)
                } else {
                    preview_text_font_size(
                        text.font_size,
                        original.rect,
                        preview.rect,
                        handle,
                        single_text_resize,
                    )
                }?;
                Some(TextPreviewFontSize {
                    id: preview.id,
                    font_size,
                })
            })
            .collect()
    }

    pub(crate) fn selection_preview_elements(&self) -> Option<&[SelectionRectState]> {
        match &self.state.interaction {
            InteractionState::EditingSelection(state) => Some(&state.preview_elements),
            _ => None,
        }
    }

    pub(crate) fn selection_preview_arrows(&self) -> Option<&[SelectionArrowState]> {
        match &self.state.interaction {
            InteractionState::EditingSelection(state) => Some(&state.preview_arrows),
            _ => None,
        }
    }

    pub(crate) fn selected_elements_snapshot(
        &self,
        document: &DocumentModel,
    ) -> Vec<SelectionRectState> {
        if let Some(preview) = self.selection_preview_elements() {
            return preview.to_vec();
        }
        self.selected_document_elements_snapshot(document)
    }

    pub(crate) fn selected_document_elements_snapshot(
        &self,
        document: &DocumentModel,
    ) -> Vec<SelectionRectState> {
        let mut elements = Self::selection_elements_from_ids(document, &self.state.selection.ids);
        self.apply_active_text_draft_rect_to_elements(&mut elements);
        elements
    }

    pub(crate) fn rectangle_snapshot(
        &self,
        document: &DocumentModel,
        id: ElementId,
    ) -> Option<RectangleData> {
        if let Some(preview) = self.selection_preview_elements()
            && let Some(element) = preview.iter().find(|element| element.id == id)
        {
            return Some(element.rect);
        }
        document.rectangle(id).ok().copied()
    }

    pub(crate) fn arrow_snapshot(
        &self,
        document: &DocumentModel,
        id: ElementId,
    ) -> Option<ArrowData> {
        if let Some(preview) = self
            .selection_preview_arrows()
            .and_then(|arrows| arrows.iter().find(|arrow| arrow.id == id))
        {
            return Some(preview.arrow.clone());
        }
        if let InteractionState::EditingArrow(state) = &self.state.interaction
            && state.arrow_id == id
        {
            return Some(state.preview_arrow.clone());
        }
        document.arrow(id).ok().cloned()
    }

    pub(crate) fn selected_primary_rectangle_snapshot(
        &self,
        document: &DocumentModel,
    ) -> Option<(ElementId, RectangleData)> {
        let id = self.state.selection.primary?;
        if document.rectangle(id).is_err() {
            return None;
        }
        self.rectangle_snapshot(document, id).map(|rect| (id, rect))
    }

    pub(crate) fn selected_single_rectangle_snapshot(
        &self,
        document: &DocumentModel,
    ) -> Option<(ElementId, RectangleData)> {
        if self.state.selection.ids.len() != 1 {
            return None;
        }
        let id = self.state.selection.ids[0];
        if document.rectangle(id).is_err() {
            return None;
        }
        self.rectangle_snapshot(document, id).map(|rect| (id, rect))
    }

    pub(crate) fn selected_single_text_rect_snapshot(
        &self,
        document: &DocumentModel,
    ) -> Option<(ElementId, RectangleData)> {
        if self.state.selection.ids.len() != 1 {
            return None;
        }
        let id = self.state.selection.ids[0];
        if document.text(id).is_err() {
            return None;
        }
        if let Some(preview) = self.selection_preview_elements()
            && let Some(element) = preview.iter().find(|element| element.id == id)
        {
            return Some((id, element.rect));
        }
        if let Some(rect) = self.active_text_draft_rect_for_id(id) {
            return Some((id, rect));
        }
        document.element_rect_proxy(id).map(|rect| (id, rect))
    }

    pub(crate) fn selected_arrows_snapshot(
        &self,
        document: &DocumentModel,
    ) -> Vec<SelectionArrowState> {
        Self::selection_arrows_from_ids(document, &self.state.selection.ids)
            .into_iter()
            .filter_map(|arrow| {
                self.arrow_snapshot(document, arrow.id)
                    .map(|preview| SelectionArrowState {
                        id: arrow.id,
                        arrow: preview,
                    })
            })
            .collect()
    }

    pub(crate) fn selected_single_arrow_snapshot(
        &self,
        document: &DocumentModel,
    ) -> Option<(ElementId, ArrowData)> {
        if self.state.selection.ids.len() != 1 {
            return None;
        }
        let id = self.state.selection.ids[0];
        self.arrow_snapshot(document, id).map(|arrow| (id, arrow))
    }

    pub(crate) fn selected_serial_number_snapshots(
        &self,
        document: &DocumentModel,
    ) -> Option<Vec<(ElementId, SerialNumberData)>> {
        if self.state.selection.ids.is_empty() {
            return None;
        }
        self.state
            .selection
            .ids
            .iter()
            .copied()
            .map(|id| {
                document
                    .serial_number(id)
                    .ok()
                    .cloned()
                    .map(|serial| (id, serial))
            })
            .collect()
    }

    pub(crate) fn selection_bounds_snapshot(
        &self,
        document: &DocumentModel,
    ) -> Option<SelectionBounds> {
        match &self.state.interaction {
            InteractionState::EditingSelection(state) => Some(state.preview_bounds),
            _ => {
                let elements = self.selected_document_elements_snapshot(document);
                let arrows = self.selected_arrows_snapshot(document);
                selection_bounds_from_selection(&elements, &arrows).or(self.state.selection.bounds)
            }
        }
    }

    pub(crate) fn presentation_selection_elements(
        &self,
        document: &DocumentModel,
    ) -> Vec<SelectionRectState> {
        if let Some(preview) = self.selection_preview_elements() {
            return preview.to_vec();
        }
        self.selected_document_elements_snapshot(document)
    }

    pub(crate) fn presentation_selection_arrows(
        &self,
        document: &DocumentModel,
    ) -> Vec<SelectionArrowState> {
        if let Some(preview) = self.selection_preview_arrows() {
            return preview.to_vec();
        }
        self.selected_arrows_snapshot(document)
    }

    pub(crate) fn presentation_selection_bounds(
        &self,
        selection_elements: &[SelectionRectState],
        selection_arrows: &[SelectionArrowState],
    ) -> Option<SelectionBounds> {
        match &self.state.interaction {
            InteractionState::EditingSelection(state) => Some(state.preview_bounds),
            InteractionState::EditingArrow(_) => {
                selection_bounds_from_selection(selection_elements, selection_arrows)
                    .or(self.state.selection.bounds)
            }
            _ if self.selection_contains_active_text_draft() => {
                selection_bounds_from_selection(selection_elements, selection_arrows)
                    .or(self.state.selection.bounds)
            }
            _ => selection_bounds_from_selection(selection_elements, selection_arrows)
                .or(self.state.selection.bounds),
        }
    }

    pub(crate) fn preview_selection_arrows(
        &self,
        document: &DocumentModel,
    ) -> Vec<SelectionArrowState> {
        match &self.state.interaction {
            InteractionState::EditingSelection(state) => {
                let selected_arrow_ids = state
                    .preview_arrows
                    .iter()
                    .map(|arrow| arrow.id)
                    .collect::<Vec<_>>();
                let mut preview_arrows = state.preview_arrows.clone();
                preview_arrows.extend(
                    self.recompute_bound_arrows(document, &state.preview_elements)
                        .into_iter()
                        .filter(|(id, _, _)| !selected_arrow_ids.contains(id))
                        .map(|(id, arrow, _)| SelectionArrowState { id, arrow }),
                );
                preview_arrows
            }
            InteractionState::EditingArrow(state) => vec![SelectionArrowState {
                id: state.arrow_id,
                arrow: state.preview_arrow.clone(),
            }],
            _ => Vec::new(),
        }
    }

    pub(crate) fn selection_contains_active_text_draft(&self) -> bool {
        self.active_text_draft_existing_id()
            .is_some_and(|id| self.state.selection.contains(id))
    }

    pub(crate) fn apply_active_text_draft_rect_to_elements(
        &self,
        elements: &mut [SelectionRectState],
    ) {
        let Some(id) = self.active_text_draft_existing_id() else {
            return;
        };
        let Some(rect) = self.active_text_draft_rect_for_id(id) else {
            return;
        };
        for element in elements {
            if element.id == id {
                element.rect = rect;
            }
        }
    }

    pub(crate) fn arrow_handle_states(
        &self,
        document: &DocumentModel,
        arrow_id: ElementId,
        arrow: &ArrowData,
    ) -> Vec<ArrowHandleState> {
        let mut handles = Vec::new();
        let points = arrow.global_points();
        let loop_threshold = selection_handle_hit_size(self.camera().zoom) * 0.75;
        let loop_active = arrow_loop_active(arrow, loop_threshold);
        if loop_active {
            handles.push(ArrowHandleState {
                kind: ArrowHandleKind::LoopEnd,
                center: points[points.len() - 1],
                anchor: None,
                fixed_segment: false,
            });
            handles.push(ArrowHandleState {
                kind: ArrowHandleKind::LoopStart,
                center: points[0],
                anchor: None,
                fixed_segment: false,
            });
        }
        for index in visible_arrow_point_indices(arrow) {
            if loop_active && (index == 0 || index + 1 == points.len()) {
                continue;
            }
            if let Some(point) = points.get(index).copied() {
                handles.push(ArrowHandleState {
                    kind: ArrowHandleKind::Endpoint,
                    center: point,
                    anchor: None,
                    fixed_segment: false,
                });
            }
        }

        let focus_points = visible_arrow_focus_points(
            arrow_id,
            arrow,
            &self.bindable_elements(document, &[]),
            self.camera().zoom,
        );
        for focus in focus_points {
            let anchor = points
                .get(arrow_endpoint_index(points.len(), focus.edge))
                .copied();
            handles.push(ArrowHandleState {
                kind: ArrowHandleKind::FocusPoint,
                center: focus.point,
                anchor,
                fixed_segment: false,
            });
        }

        for (_, midpoint, fixed_segment) in self.visible_arrow_segment_midpoints(arrow) {
            handles.push(ArrowHandleState {
                kind: ArrowHandleKind::Segment,
                center: midpoint,
                anchor: None,
                fixed_segment,
            });
        }

        handles
    }

    pub(crate) fn visible_arrow_segment_midpoints(
        &self,
        arrow: &ArrowData,
    ) -> Vec<(usize, Point<f64>, bool)> {
        if arrow.is_pen_highlight() {
            return Vec::new();
        }
        let show_segment_handles = arrow.is_line() || arrow.is_elbow() || arrow.points.len() <= 2;
        if show_segment_handles {
            arrow_segment_midpoints(arrow)
                .into_iter()
                .filter(|(index, _)| {
                    !arrow.is_elbow() || segment_handle_visible(arrow, *index, self.camera().zoom)
                })
                .map(|(index, midpoint)| (index, midpoint, segment_is_fixed(arrow, index)))
                .collect()
        } else {
            Vec::new()
        }
    }

    pub(crate) fn arrow_hit_target(
        &self,
        document: &DocumentModel,
        arrow_id: ElementId,
        arrow: &ArrowData,
        canvas_point: Point<f64>,
    ) -> Option<ArrowHitTarget> {
        let handle_tolerance = selection_handle_hit_size(self.camera().zoom) / 2.0;
        let visual_radius = selection_handle_size(self.camera().zoom) * 1.25 / 2.0;
        let points = arrow.global_points();
        let loop_threshold = handle_tolerance * 1.5;
        let loop_active = arrow_loop_active(arrow, loop_threshold);
        if loop_active {
            let center = Point::new(
                f64::midpoint(points[0].x, points[points.len() - 1].x),
                f64::midpoint(points[0].y, points[points.len() - 1].y),
            );
            let distance = point_distance(center, canvas_point);
            let inner_radius = (handle_tolerance * 0.69).max(visual_radius);
            if distance <= inner_radius {
                return Some(ArrowHitTarget::Endpoint(ArrowEndpointEdge::Start));
            }
            let outer_radius = (handle_tolerance * 1.18).max(visual_radius * 2.0);
            if distance <= outer_radius {
                return Some(ArrowHitTarget::Endpoint(ArrowEndpointEdge::End));
            }
        }
        let turning_radius = (handle_tolerance * 1.11).max(visual_radius);
        for index in visible_arrow_point_indices(arrow) {
            if loop_active && (index == 0 || index + 1 == points.len()) {
                continue;
            }
            let Some(point) = points.get(index).copied() else {
                continue;
            };
            if point_distance(point, canvas_point) > turning_radius {
                continue;
            }

            if index == 0 {
                return Some(ArrowHitTarget::Endpoint(ArrowEndpointEdge::Start));
            }
            if index + 1 == points.len() {
                return Some(ArrowHitTarget::Endpoint(ArrowEndpointEdge::End));
            }
            return Some(ArrowHitTarget::Point(index));
        }

        let focus_points = visible_arrow_focus_points(
            arrow_id,
            arrow,
            &self.bindable_elements(document, &[]),
            self.camera().zoom,
        );
        for focus in focus_points {
            if point_distance(focus.point, canvas_point) <= handle_tolerance {
                return Some(ArrowHitTarget::FocusPoint(focus.edge));
            }
        }

        let addable_radius = (handle_tolerance * 1.43).max(visual_radius);
        for (index, midpoint, _) in self.visible_arrow_segment_midpoints(arrow) {
            if point_distance(midpoint, canvas_point) <= addable_radius {
                return Some(ArrowHitTarget::Segment(index));
            }
        }

        arrow_hit_test(
            arrow,
            canvas_point,
            element_hit_tolerance(self.camera().zoom),
        )
        .then_some(ArrowHitTarget::Move)
    }
}

fn text_rect_ids_for_groups(
    document: &DocumentModel,
    groups: &[&[SelectionRectState]],
) -> Vec<ElementId> {
    let mut ids = Vec::new();
    for group in groups {
        for element in *group {
            if ids.contains(&element.id) || document.text(element.id).is_err() {
                continue;
            }
            ids.push(element.id);
        }
    }
    ids
}

fn preview_text_font_size(
    original_font_size: f64,
    original_rect: RectangleData,
    preview_rect: RectangleData,
    resize_handle: ResizeHandle,
    single_text_resize: bool,
) -> Option<f64> {
    let size_changed = (original_rect.width - preview_rect.width).abs() > f64::EPSILON
        || (original_rect.height - preview_rect.height).abs() > f64::EPSILON;
    if !size_changed {
        return None;
    }
    if single_text_resize
        && resize_handle.x_sign().abs() > f64::EPSILON
        && resize_handle.y_sign().abs() <= f64::EPSILON
    {
        return None;
    }
    if !single_text_resize {
        return scaled_text_font_size_from_width(
            original_font_size,
            original_rect.width,
            preview_rect.width,
        );
    }
    scaled_text_font_size_from_height(
        original_font_size,
        original_rect.height,
        preview_rect.height,
    )
}

fn scaled_text_font_size_from_width(
    original_font_size: f64,
    original_width: f64,
    preview_width: f64,
) -> Option<f64> {
    if original_width.is_finite()
        && original_width > f64::EPSILON
        && preview_width.is_finite()
        && preview_width > f64::EPSILON
    {
        let scale = preview_width / original_width;
        if scale.is_finite() && scale > f64::EPSILON {
            return Some((original_font_size * scale).max(MIN_TEXT_FONT_SIZE));
        }
    }
    None
}

fn scaled_text_font_size_from_height(
    original_font_size: f64,
    original_height: f64,
    preview_height: f64,
) -> Option<f64> {
    if original_height.is_finite()
        && original_height > f64::EPSILON
        && preview_height.is_finite()
        && preview_height > f64::EPSILON
    {
        let scale = preview_height / original_height;
        if scale.is_finite() && scale > f64::EPSILON {
            return Some((original_font_size * scale).max(MIN_TEXT_FONT_SIZE));
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ActiveTextDraftPresentation, ActiveTextDraftTarget};
    use snow_draw_engine_core::{ColorRgba8, CornerRadii, EngineConfig, Point};
    use snow_draw_engine_document::{ElementMeta, TextData, Transaction};

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

    fn insert_text(document: &mut DocumentModel, text: TextData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert text");
        transaction.insert_text(id, ElementMeta::default(), text);
        document.apply_transaction(transaction).unwrap();
        id
    }

    fn editor_with_surface() -> Editor {
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(800, 600).unwrap();
        editor
    }

    #[test]
    fn multi_point_line_exposes_an_insertion_handle_for_every_segment() {
        let editor = editor_with_surface();
        let line = ArrowData::from_global_points(
            &[
                Point::new(-120.0, 0.0),
                Point::new(-40.0, 50.0),
                Point::new(40.0, -50.0),
                Point::new(120.0, 0.0),
            ],
            ColorRgba8::default(),
            2.0,
            snow_draw_engine_core::arrow::ArrowStrokeStyle::Solid,
            snow_draw_engine_core::arrow::ArrowType::Curve,
            None,
            None,
        )
        .unwrap()
        .into_line(
            ColorRgba8::default(),
            snow_draw_engine_document::FillStyle::Solid,
        );

        let handles = editor.visible_arrow_segment_midpoints(&line);

        assert_eq!(handles.len(), 3);
        assert_eq!(
            handles
                .iter()
                .map(|(index, _, _)| *index)
                .collect::<Vec<_>>(),
            vec![1, 2, 3]
        );
    }

    #[test]
    fn pen_highlight_does_not_expose_segment_insertion_handles() {
        let editor = editor_with_surface();
        let highlight = ArrowData::from_global_points(
            &[Point::new(-60.0, 0.0), Point::new(60.0, 0.0)],
            ColorRgba8::default(),
            12.0,
            snow_draw_engine_core::arrow::ArrowStrokeStyle::Solid,
            snow_draw_engine_core::arrow::ArrowType::Straight,
            None,
            None,
        )
        .unwrap()
        .into_pen_highlight();

        assert!(
            editor
                .visible_arrow_segment_midpoints(&highlight)
                .is_empty()
        );
        assert!(
            !editor
                .arrow_handle_states(&DocumentModel::new(), ElementId::default(), &highlight)
                .iter()
                .any(|handle| handle.kind == ArrowHandleKind::Segment)
        );
    }

    #[test]
    fn presentation_state_uses_active_text_draft_for_selected_text_geometry() {
        let mut document = DocumentModel::new();
        let text_id = insert_text(
            &mut document,
            TextData {
                center: Point::new(0.0, 0.0),
                width: 80.0,
                height: 24.0,
                text: "committed".to_owned(),
                auto_resize: false,
                ..TextData::default()
            },
        );
        let mut editor = editor_with_surface();
        editor.select_element(&document, text_id).unwrap();
        let draft_text = TextData {
            center: Point::new(140.0, 20.0),
            width: 120.0,
            height: 48.0,
            rotation: 0.25,
            text: "draft".to_owned(),
            auto_resize: false,
            ..document.text(text_id).unwrap().clone()
        };
        let draft_rect = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: draft_text.center,
            width: draft_text.width,
            height: draft_text.height,
            rotation: draft_text.rotation,
            fill: draft_text.fill,
            fill_style: draft_text.fill_style,
            stroke: draft_text.stroke,
            stroke_width: draft_text.stroke_width,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: draft_text.corner_radii,
            opacity: draft_text.opacity,
        };

        editor
            .set_active_text_draft_presentation(
                &document,
                ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::Existing(text_id),
                    revision: 1,
                    text: draft_text,
                },
            )
            .unwrap();

        let state = editor.presentation_state(&document);

        assert_eq!(state.selected_single_text_rect, Some(draft_rect));
        assert_eq!(
            state.selection_elements,
            vec![SelectionRectState {
                id: text_id,
                rect: draft_rect,
            }]
        );
        assert_eq!(
            state.selection_bounds,
            Some(SelectionBounds {
                center: draft_rect.center,
                width: draft_rect.width,
                height: draft_rect.height,
                rotation: draft_rect.rotation,
            })
        );
    }

    #[test]
    fn presentation_state_restores_committed_geometry_after_active_text_draft_clear() {
        let mut document = DocumentModel::new();
        let text_id = insert_text(
            &mut document,
            TextData {
                center: Point::new(0.0, 0.0),
                width: 80.0,
                height: 24.0,
                rotation: 0.0,
                text: "committed".to_owned(),
                auto_resize: false,
                ..TextData::default()
            },
        );
        let committed_rect = document.element_rect_proxy(text_id).unwrap();
        let stale_draft_rect = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(240.0, 60.0),
            width: 160.0,
            height: 64.0,
            rotation: 0.5,
            fill: ColorRgba8::default(),
            fill_style: snow_draw_engine_document::FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        let mut editor = editor_with_surface();
        editor.select_element(&document, text_id).unwrap();
        editor.state.selection.elements = vec![SelectionRectState {
            id: text_id,
            rect: stale_draft_rect,
        }];
        editor.state.selection.bounds = Some(SelectionBounds {
            center: stale_draft_rect.center,
            width: stale_draft_rect.width,
            height: stale_draft_rect.height,
            rotation: stale_draft_rect.rotation,
        });

        let state = editor.presentation_state(&document);

        assert_eq!(
            state.selection_elements,
            vec![SelectionRectState {
                id: text_id,
                rect: committed_rect,
            }]
        );
        assert_eq!(
            state.selection_bounds,
            Some(SelectionBounds {
                center: committed_rect.center,
                width: committed_rect.width,
                height: committed_rect.height,
                rotation: committed_rect.rotation,
            })
        );
    }

    #[test]
    fn serial_number_toolbar_is_hidden_for_multi_serial_selection() {
        let mut document = DocumentModel::new();
        let first = insert_serial_number(
            &mut document,
            SerialNumberData {
                center: Point::new(-24.0, 0.0),
                number: 0,
                ..SerialNumberData::default()
            },
        );
        let second = insert_serial_number(
            &mut document,
            SerialNumberData {
                center: Point::new(24.0, 0.0),
                number: 3,
                ..SerialNumberData::default()
            },
        );
        let mut editor = editor_with_surface();
        editor.set_selection_state_with_document(
            Some(&document),
            vec![first, second],
            Some(second),
        );

        let state = editor.serial_number_toolbar_state(&document);

        assert_eq!(state, SerialNumberToolbarState::default());
    }

    #[test]
    fn serial_number_toolbar_disables_decrease_when_the_selected_serial_is_zero() {
        let mut document = DocumentModel::new();
        let first = insert_serial_number(
            &mut document,
            SerialNumberData {
                center: Point::new(-24.0, 0.0),
                number: 0,
                ..SerialNumberData::default()
            },
        );
        let mut editor = editor_with_surface();
        editor.set_selection_state_with_document(Some(&document), vec![first], Some(first));

        let state = editor.serial_number_toolbar_state(&document);

        assert!(state.visible);
        assert!(!state.can_decrease);
        assert!(state.can_increase);
        assert!(state.can_create_text);
    }

    #[test]
    fn serial_number_toolbar_is_hidden_when_selected_serial_is_outside_viewport() {
        let mut document = DocumentModel::new();
        let serial_id = insert_serial_number(
            &mut document,
            SerialNumberData {
                center: Point::new(420.0, 0.0),
                diameter: 20.0,
                number: 1,
                ..SerialNumberData::default()
            },
        );
        let mut editor = editor_with_surface();
        editor.select_element(&document, serial_id).unwrap();
        let serial_center = canvas_to_view(
            Point::new(420.0, 0.0),
            &editor.camera(),
            editor.surface_size(),
        );
        assert!(serial_center.x - SERIAL_TOOLBAR_WIDTH / 2.0 < editor.surface_size().width as f64);

        let state = editor.serial_number_toolbar_state(&document);

        assert_eq!(state, SerialNumberToolbarState::default());
    }

    #[test]
    fn serial_number_toolbar_remains_visible_when_selected_serial_intersects_viewport() {
        let mut document = DocumentModel::new();
        let serial_id = insert_serial_number(
            &mut document,
            SerialNumberData {
                center: Point::new(409.0, 0.0),
                diameter: 20.0,
                number: 1,
                ..SerialNumberData::default()
            },
        );
        let mut editor = editor_with_surface();
        editor.select_element(&document, serial_id).unwrap();

        let state = editor.serial_number_toolbar_state(&document);

        assert!(state.visible);
        assert!(state.left < editor.surface_size().width as f64);
    }

    #[test]
    fn serial_number_toolbar_is_hidden_for_mixed_selection() {
        let mut document = DocumentModel::new();
        let serial_id = insert_serial_number(
            &mut document,
            SerialNumberData {
                center: Point::new(-24.0, 0.0),
                number: 1,
                ..SerialNumberData::default()
            },
        );
        let rect_id = insert_rectangle(
            &mut document,
            RectangleData {
                rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
                highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
                center: Point::new(24.0, 0.0),
                width: 32.0,
                height: 32.0,
                rotation: 0.0,
                fill: ColorRgba8::default(),
                fill_style: snow_draw_engine_document::FillStyle::Solid,
                stroke: ColorRgba8::default(),
                stroke_width: 0.0,
                stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
                corner_radii: CornerRadii::default(),
                opacity: 1.0,
            },
        );
        let mut editor = editor_with_surface();
        editor.set_selection_state_with_document(
            Some(&document),
            vec![serial_id, rect_id],
            Some(rect_id),
        );

        let state = editor.serial_number_toolbar_state(&document);

        assert_eq!(state, SerialNumberToolbarState::default());
    }
}

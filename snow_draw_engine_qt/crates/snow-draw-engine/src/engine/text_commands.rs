use super::{Engine, MutationResult, ViewportId};
use snow_draw_engine_core::{ErrorCode, Point};
use snow_draw_engine_document::{ElementId, TextData, TextLayoutSize, resolve_text_layout_rect};
use snow_draw_engine_editor::{
    ActiveTextDraftPresentation, SerialNumberStyle, TextDraftCommit, TextLayoutOverride,
    TextResizeMeasurementRequest, TextStyle,
};

#[derive(Clone, Debug, PartialEq)]
pub struct TextElementInfo {
    pub id: ElementId,
    pub arrow_id: Option<ElementId>,
    pub arrow_width: f64,
    pub center: Point<f64>,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub text: String,
    pub font_size: f64,
    pub font_family: Option<String>,
    pub auto_resize: bool,
    pub measure_natural_width: bool,
}

fn text_element_info_from_resize_request(request: TextResizeMeasurementRequest) -> TextElementInfo {
    TextElementInfo {
        id: request.id,
        arrow_id: None,
        arrow_width: 0.0,
        center: request.center,
        width: request.width,
        height: request.height,
        rotation: request.rotation,
        text: request.text,
        font_size: request.font_size,
        font_family: request.font_family,
        auto_resize: request.auto_resize,
        measure_natural_width: request.measure_natural_width,
    }
}

fn text_element_info_from_active_draft(draft: &ActiveTextDraftPresentation) -> TextElementInfo {
    TextElementInfo {
        id: draft.existing_id().unwrap_or_default(),
        arrow_id: None,
        arrow_width: 0.0,
        center: draft.text.center,
        width: draft.text.width,
        height: draft.text.height,
        rotation: draft.text.rotation,
        text: draft.text.text.clone(),
        font_size: draft.text.font_size,
        font_family: draft.text.font_family.clone(),
        auto_resize: draft.text.auto_resize,
        measure_natural_width: draft.text.auto_resize,
    }
}

fn text_style_from_text(text: &TextData) -> TextStyle {
    TextStyle {
        color: text.color,
        font_size: text.font_size,
        font_family: text.font_family.clone(),
        fill: text.fill,
        fill_style: text.fill_style,
        stroke: text.stroke,
        stroke_width: text.stroke_width,
        corner_radii: text.corner_radii,
        horizontal_align: text.horizontal_align,
        vertical_align: text.vertical_align,
        opacity: text.opacity,
    }
}

impl Engine {
    pub fn arrow_text_count(&self) -> usize {
        self.model.arrow_text_bindings().len()
    }

    pub fn arrow_text_layout_requests(
        &self,
        viewport: ViewportId,
    ) -> Result<Vec<snow_draw_engine_editor::ArrowTextLayoutRequest>, ErrorCode> {
        self.ensure_viewport(viewport)?;
        Ok(self.editor.arrow_text_layout_requests(&self.model))
    }

    pub fn apply_arrow_text_measurements(
        &mut self,
        viewport: ViewportId,
        layouts: &[(ElementId, u64, TextLayoutSize)],
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(viewport)?;
        let before = self.editor.snapshot();
        let editor_before = self.editor.clone();
        for (id, key, size) in layouts {
            if let Err(error) =
                self.editor
                    .apply_arrow_text_measurement(&self.model, *id, *key, *size)
            {
                self.editor = editor_before;
                return Err(error);
            }
        }
        self.refresh_after_session_mutation(before)
    }

    pub fn arrow_text_target(
        &self,
        viewport: ViewportId,
        point: Option<Point<f64>>,
    ) -> Result<Option<(TextElementInfo, TextStyle)>, ErrorCode> {
        self.ensure_viewport(viewport)?;
        if !self.editor.can_begin_arrow_text() {
            return Ok(None);
        }
        let target = if let Some(point) = point {
            let zoom = self.viewport_slot(viewport)?.view.camera.zoom;
            self.model
                .topmost_element_at_with_tolerance(point, 8.0 / zoom.max(0.01))
                .map(|(id, _)| self.model.arrow_id_for_text(id).unwrap_or(id))
        } else {
            let ids = self.editor.selected_ids();
            (ids.len() == 1).then(|| self.model.arrow_id_for_text(ids[0]).unwrap_or(ids[0]))
        };
        let Some(id) = target else {
            return Ok(None);
        };
        let Ok(arrow) = self.model.arrow(id) else {
            return Ok(None);
        };
        let meta = self.model.element(id)?.meta;
        if arrow.linear_kind != snow_draw_engine_document::LinearElementKind::Arrow
            || meta.locked
            || !meta.visible
        {
            return Ok(None);
        }
        if let Some(text_id) = arrow.text_element_id {
            return Ok(Some((
                self.text_element_info(text_id)?,
                text_style_from_text(self.model.text(text_id)?),
            )));
        }
        let mut style = self.editor.text_style(&self.model);
        style.horizontal_align = snow_draw_engine_document::TextHorizontalAlign::Center;
        style.vertical_align = snow_draw_engine_document::TextVerticalAlign::Center;
        Ok(Some((
            TextElementInfo {
                id: ElementId::default(),
                arrow_id: Some(id),
                arrow_width: arrow.width,
                center: snow_draw_engine_document::arrow_text_anchor(arrow),
                width: 1.0,
                height: style.font_size,
                rotation: 0.0,
                text: String::new(),
                font_size: style.font_size,
                font_family: style.font_family.clone(),
                auto_resize: true,
                measure_natural_width: false,
            },
            style,
        )))
    }

    pub fn set_viewport_text_style(
        &mut self,
        id: ViewportId,
        style: TextStyle,
        layouts: &[TextLayoutOverride],
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        let command = self.editor.set_text_style(&self.model, style, layouts)?;
        if let Some(command) = command {
            self.apply_editor_command(id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn set_viewport_serial_number_style(
        &mut self,
        id: ViewportId,
        style: SerialNumberStyle,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        let command = self.editor.set_serial_number_style(&self.model, style)?;
        if let Some(command) = command {
            self.apply_editor_command(id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn create_text_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        center: snow_draw_engine_core::Point<f64>,
        text_content: impl Into<String>,
        layout: TextLayoutSize,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let command =
            self.editor
                .queue_create_text_element(&self.model, center, text_content, layout)?;
        if let Some(command) = command {
            self.apply_editor_command(source_viewport_id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn hit_text_at(
        &self,
        source_viewport_id: ViewportId,
        point: Point<f64>,
    ) -> Result<Option<ElementId>, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        Ok(self.editor.hit_text_at(&self.model, point))
    }

    pub fn text_element_info(&self, id: ElementId) -> Result<TextElementInfo, ErrorCode> {
        let text = self.model.text(id)?;
        let layout = resolve_text_layout_rect(text);
        Ok(TextElementInfo {
            id,
            arrow_id: self.model.arrow_id_for_text(id),
            arrow_width: self
                .model
                .arrow_id_for_text(id)
                .and_then(|id| self.model.arrow(id).ok())
                .map_or(0.0, |arrow| arrow.width),
            center: layout.center,
            width: layout.width,
            height: layout.height,
            rotation: layout.rotation,
            text: text.text.clone(),
            font_size: text.font_size,
            font_family: text.font_family.clone(),
            auto_resize: text.auto_resize,
            measure_natural_width: false,
        })
    }

    pub fn commit_text_draft_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        draft: TextDraftCommit,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let command = self.editor.commit_text_draft(&self.model, draft)?;
        if let Some(command) = command {
            self.apply_editor_command(source_viewport_id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn selected_text_count(&self) -> usize {
        self.editor
            .selected_ids()
            .into_iter()
            .filter(|id| self.model.text(*id).is_ok())
            .count()
    }

    pub fn selected_text_elements(&self) -> Vec<TextElementInfo> {
        self.editor
            .selected_ids()
            .into_iter()
            .filter_map(|id| self.text_element_info(id).ok())
            .collect()
    }

    pub fn active_text_resize_measurement_request(
        &self,
        source_viewport_id: ViewportId,
    ) -> Result<Option<TextElementInfo>, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        Ok(self
            .editor
            .active_text_resize_measurement_request(&self.model)
            .map(text_element_info_from_resize_request))
    }

    pub fn active_text_draft_presentation(
        &self,
        source_viewport_id: ViewportId,
    ) -> Result<Option<(TextElementInfo, TextStyle)>, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        Ok(self
            .editor
            .active_text_draft_display_presentation()
            .map(|draft| {
                let style = text_style_from_text(&draft.text);
                let mut info = text_element_info_from_active_draft(&draft);
                info.arrow_id = match draft.target {
                    snow_draw_engine_editor::ActiveTextDraftTarget::NewArrow(id) => Some(id),
                    _ => draft
                        .existing_id()
                        .and_then(|id| self.model.arrow_id_for_text(id)),
                };
                info.arrow_width = info
                    .arrow_id
                    .and_then(|id| self.model.arrow(id).ok())
                    .map_or(0.0, |arrow| arrow.width);
                (info, style)
            }))
    }

    pub fn set_active_text_draft_presentation_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        mut draft: ActiveTextDraftPresentation,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let revision = self
            .editor
            .active_text_draft_presentation()
            .map_or(1, |current| {
                if current.target == draft.target && current.text == draft.text {
                    current.revision
                } else {
                    current.revision.wrapping_add(1)
                }
            });
        draft.revision = revision;
        let before = self.editor.snapshot();
        let changed = self
            .editor
            .set_active_text_draft_presentation(&self.model, draft)?;
        if changed {
            self.refresh_after_session_mutation(before)
        } else {
            Ok(MutationResult::default())
        }
    }

    pub fn clear_active_text_draft_presentation_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        if self.editor.clear_active_text_draft_presentation() {
            self.refresh_after_session_mutation(before)
        } else {
            Ok(MutationResult::default())
        }
    }

    pub fn apply_active_text_resize_measurement_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        layout: TextLayoutSize,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let applied = self
            .editor
            .apply_active_text_resize_measurement(&self.model, layout)?;
        if applied {
            self.refresh_after_session_mutation(before)
        } else {
            Ok(MutationResult::default())
        }
    }

    pub fn is_text_bound_to_serial_number(&self, id: ElementId) -> bool {
        self.model.is_text_bound_to_serial_number(id)
    }

    pub fn create_serial_number_text_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        layout: TextLayoutSize,
    ) -> Result<(MutationResult, Option<ElementId>), ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let operation = self
            .editor
            .create_serial_number_text_elements(&self.model, layout)?;
        let result = if let Some(command) = operation.command {
            self.apply_editor_command(source_viewport_id, command)?
        } else {
            self.refresh_after_session_mutation(before)?
        };
        if let Some(text_id) = operation.single_text_id {
            let before_select = self.editor.snapshot();
            if self.editor.select_element(&self.model, text_id).is_ok() {
                let selection_result = self.refresh_after_session_mutation(before_select)?;
                let mut changed_viewports = result.changed_viewports;
                changed_viewports.extend(selection_result.changed_viewports);
                changed_viewports.sort_by_key(|id| id.0);
                changed_viewports.dedup();
                return Ok((MutationResult { changed_viewports }, Some(text_id)));
            }
        }
        Ok((result, operation.single_text_id))
    }
}

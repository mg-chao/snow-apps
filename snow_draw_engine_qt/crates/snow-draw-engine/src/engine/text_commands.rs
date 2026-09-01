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
                (text_element_info_from_active_draft(&draft), style)
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

use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{EngineConfig, ErrorCode, GridConfig, Point, SnapConfig};
use snow_draw_engine_document::{ElementId, TextLayoutSize};
use snow_draw_engine_interaction::InputEvent;
use snow_draw_engine_model::DocumentModel;

use super::{
    ActiveTextDraftPresentation, ActiveTool, ArrowStyle, DocumentSyncSnapshot, Editor,
    EditorCommand, EditorPresentationState, EditorUpdate, EditorViewState, EditorViewportState,
    FilterStyle, RectangleShapeStyle, SerialNumberStyle, SerialNumberTextOperation,
    SerialNumberToolbarState, ShapeStyle, ShapeStylePatch, StyleToolbarSource, TextDraftCommit,
    TextLayoutOverride, TextResizeMeasurementRequest, TextStyle, state::EditorState,
};
use crate::defaults::EditorStyleDefaults;

#[derive(Clone, Debug, PartialEq)]
pub struct EditorSessionSnapshot {
    state: EditorState,
    config: EngineConfig,
    quick_selection_disabled_tools: u64,
}

#[derive(Clone, Debug)]
pub struct EditorSession {
    editor: Editor,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct PersistedEditorSession {
    config: EngineConfig,
    rectangle: RectangleShapeStyle,
    arrow: ArrowStyle,
    line: super::ShapeStyle,
    free_draw: super::ShapeStyle,
    rectangle_highlight: super::ShapeStyle,
    pen_highlight: super::ShapeStyle,
    filter: snow_draw_engine_document::FilterData,
    #[serde(default = "default_rectangle_filter_stroke_width")]
    rectangle_filter_stroke_width: f64,
    pen_filter: snow_draw_engine_document::PenFilterData,
    text: snow_draw_engine_document::TextData,
    serial_number: snow_draw_engine_document::SerialNumberData,
}

impl EditorSession {
    pub fn new(config: EngineConfig) -> Result<Self, ErrorCode> {
        Self::new_with_style_defaults(config, &EditorStyleDefaults::default())
    }

    pub fn new_with_style_defaults(
        config: EngineConfig,
        defaults: &EditorStyleDefaults,
    ) -> Result<Self, ErrorCode> {
        validate_editor_style_defaults(defaults)?;
        let mut editor = Editor::new(config)?;
        editor.state = EditorState::with_style_defaults(defaults);
        Ok(Self { editor })
    }

    pub fn snapshot(&self) -> EditorSessionSnapshot {
        EditorSessionSnapshot {
            state: self.editor.state.clone(),
            config: self.editor.config,
            quick_selection_disabled_tools: self.editor.quick_selection_disabled_tools(),
        }
    }

    pub fn persisted(&self) -> PersistedEditorSession {
        let state = &self.editor.state;
        PersistedEditorSession {
            config: self.editor.config,
            rectangle: state.default_rectangle_shape_style,
            arrow: state.default_arrow_style,
            line: state.default_line_style,
            free_draw: state.default_free_draw_style,
            rectangle_highlight: state.default_rectangle_highlight_style,
            pen_highlight: state.default_pen_highlight_style,
            filter: state.default_filter,
            rectangle_filter_stroke_width: state.default_filter_stroke_width,
            pen_filter: state.default_pen_filter.clone(),
            text: state.default_text.clone(),
            serial_number: state.default_serial_number.clone(),
        }
    }

    pub fn from_persisted(persisted: PersistedEditorSession) -> Result<Self, ErrorCode> {
        snow_draw_engine_core::validate_config(&persisted.config)?;
        snow_draw_engine_document::validate_filter(&persisted.filter)?;
        snow_draw_engine_document::validate_pen_filter(&persisted.pen_filter)?;
        snow_draw_engine_document::validate_text(&persisted.text)?;
        snow_draw_engine_document::validate_serial_number(&persisted.serial_number)?;
        validate_persisted_editor_styles(&persisted)?;

        let mut session = Self::new(persisted.config)?;
        let state = &mut session.editor.state;
        state.default_rectangle_shape_style = persisted.rectangle;
        state.default_arrow_style = persisted.arrow;
        state.default_line_style = persisted.line;
        state.default_free_draw_style = persisted.free_draw;
        state.default_rectangle_highlight_style = persisted.rectangle_highlight;
        state.default_pen_highlight_style = persisted.pen_highlight;
        state.default_filter = persisted.filter;
        state.default_filter_stroke_width = persisted.rectangle_filter_stroke_width;
        state.default_pen_filter = persisted.pen_filter;
        state.default_text = persisted.text;
        state.default_serial_number = persisted.serial_number;
        session.reset_editing_state();
        Ok(session)
    }

    pub fn snap_config(&self) -> SnapConfig {
        self.editor.snap_config()
    }

    pub fn config(&self) -> EngineConfig {
        self.editor.config
    }

    pub fn set_config(&mut self, config: EngineConfig) -> Result<(), ErrorCode> {
        snow_draw_engine_core::validate_config(&config)?;
        self.editor.config = config;
        Ok(())
    }

    pub fn quick_selection_disabled_tools(&self) -> u64 {
        self.editor.quick_selection_disabled_tools()
    }

    pub fn set_quick_selection_disabled_tools(&mut self, tools: u64) {
        self.editor.set_quick_selection_disabled_tools(tools);
    }

    pub fn set_snap_config(&mut self, snap: SnapConfig) -> Result<(), ErrorCode> {
        self.editor.set_snap_config(snap)
    }

    pub fn grid_config(&self) -> GridConfig {
        self.editor.grid_config()
    }

    pub fn set_grid_config(&mut self, grid: GridConfig) -> Result<(), ErrorCode> {
        self.editor.set_grid_config(grid)
    }

    pub fn active_tool(&self) -> ActiveTool {
        self.editor.active_tool()
    }

    pub fn set_active_tool(&mut self, active_tool: ActiveTool) -> Result<(), ErrorCode> {
        self.editor.set_active_tool(active_tool)
    }

    pub fn reset_editing_state(&mut self) {
        self.editor.reset_editing_state();
    }

    pub fn style_toolbar_source(&self, document: &DocumentModel) -> StyleToolbarSource {
        self.editor.style_toolbar_source(document)
    }

    pub fn shape_style(&self, document: &DocumentModel) -> ShapeStyle {
        self.editor.shape_style(document)
    }

    pub fn shape_style_mixed(&self, document: &DocumentModel) -> u32 {
        self.editor.shape_style_mixed(document)
    }

    pub fn rectangle_shape_style(&self, document: &DocumentModel) -> RectangleShapeStyle {
        self.editor.rectangle_shape_style(document)
    }

    pub fn arrow_style(&self, document: &DocumentModel) -> ArrowStyle {
        self.editor.arrow_style(document)
    }

    pub fn text_style(&self, document: &DocumentModel) -> TextStyle {
        self.editor.text_style(document)
    }

    pub fn text_style_mixed(&self, document: &DocumentModel) -> u32 {
        self.editor.text_style_mixed(document)
    }

    pub fn serial_number_style(&self, document: &DocumentModel) -> SerialNumberStyle {
        self.editor.serial_number_style(document)
    }

    pub fn serial_number_style_mixed(&self, document: &DocumentModel) -> u32 {
        self.editor.serial_number_style_mixed(document)
    }

    pub fn serial_number_toolbar_state(
        &self,
        document: &DocumentModel,
        view: &EditorViewportState,
    ) -> SerialNumberToolbarState {
        let mut editor = self.editor.clone();
        editor.view = *view;
        editor.serial_number_toolbar_state(document)
    }

    pub fn capture_document_sync_snapshot(&self, document: &DocumentModel) -> DocumentSyncSnapshot {
        self.editor.capture_document_sync_snapshot(document)
    }

    pub fn sync_after_document_change(
        &mut self,
        document: &DocumentModel,
        snapshot: &DocumentSyncSnapshot,
    ) {
        self.editor.sync_after_document_change(document, snapshot);
    }

    pub fn set_shape_style_patch(
        &mut self,
        document: &DocumentModel,
        patch: ShapeStylePatch,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.set_shape_style_patch(document, patch)
    }

    pub fn set_rectangle_shape_style(
        &mut self,
        document: &DocumentModel,
        style: RectangleShapeStyle,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.set_rectangle_shape_style(document, style)
    }

    pub fn set_text_style(
        &mut self,
        document: &DocumentModel,
        style: TextStyle,
        layouts: &[TextLayoutOverride],
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.set_text_style(document, style, layouts)
    }

    pub fn set_serial_number_style(
        &mut self,
        document: &DocumentModel,
        style: SerialNumberStyle,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.set_serial_number_style(document, style)
    }

    pub fn hit_text_at(&self, document: &DocumentModel, point: Point<f64>) -> Option<ElementId> {
        self.editor.hit_text_at(document, point)
    }

    pub fn selected_ids(&self) -> Vec<ElementId> {
        self.editor.selected_ids()
    }

    pub fn select_element(
        &mut self,
        document: &DocumentModel,
        id: ElementId,
    ) -> Result<(), ErrorCode> {
        self.editor.select_element(document, id)
    }

    pub fn queue_create_text_element(
        &mut self,
        document: &DocumentModel,
        center: Point<f64>,
        text_content: impl Into<String>,
        layout: TextLayoutSize,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor
            .queue_create_text_element(document, center, text_content, layout)
    }

    pub fn update_text_element(
        &mut self,
        document: &DocumentModel,
        id: ElementId,
        text_content: impl Into<String>,
        layout: TextLayoutSize,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor
            .update_text_element(document, id, text_content, layout)
    }

    pub fn commit_text_draft(
        &mut self,
        document: &DocumentModel,
        draft: TextDraftCommit,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.commit_text_draft(document, draft)
    }

    pub fn active_text_draft_presentation(&self) -> Option<ActiveTextDraftPresentation> {
        self.editor.active_text_draft_presentation()
    }

    pub fn active_text_draft_display_presentation(&self) -> Option<ActiveTextDraftPresentation> {
        self.editor.active_text_draft_display_presentation()
    }

    pub fn set_active_text_draft_presentation(
        &mut self,
        document: &DocumentModel,
        draft: ActiveTextDraftPresentation,
    ) -> Result<bool, ErrorCode> {
        self.editor
            .set_active_text_draft_presentation(document, draft)
    }

    pub fn clear_active_text_draft_presentation(&mut self) -> bool {
        self.editor.clear_active_text_draft_presentation()
    }

    pub fn delete_selected(
        &mut self,
        document: &DocumentModel,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.delete_selected(document)
    }

    pub fn duplicate_selected(
        &mut self,
        document: &DocumentModel,
        offset: Point<f64>,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.duplicate_selected(document, offset)
    }

    pub fn filter_style(&self, document: &DocumentModel) -> FilterStyle {
        self.editor.filter_style(document)
    }

    pub fn filter_style_mixed(&self, document: &DocumentModel) -> u32 {
        self.editor.filter_style_mixed(document)
    }

    pub fn set_filter_style(
        &mut self,
        document: &DocumentModel,
        style: FilterStyle,
        properties: u32,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.set_filter_style(document, style, properties)
    }

    pub fn set_watermark_config(
        &mut self,
        document: &DocumentModel,
        config: snow_draw_engine_document::WatermarkConfig,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.set_watermark_config(document, config)
    }

    pub fn set_spotlight_config(
        &mut self,
        document: &DocumentModel,
        config: snow_draw_engine_document::SpotlightConfig,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.set_spotlight_config(document, config)
    }

    pub fn reorder_selected(
        &mut self,
        document: &DocumentModel,
        action: u32,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.reorder_selected(document, action)
    }

    pub fn set_selected_opacity(
        &mut self,
        document: &DocumentModel,
        opacity: f64,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.set_selected_opacity(document, opacity)
    }

    pub fn adjust_selected_serial_numbers(
        &mut self,
        document: &DocumentModel,
        delta: i64,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.adjust_selected_serial_numbers(document, delta)
    }

    pub fn create_serial_number_text_elements(
        &mut self,
        document: &DocumentModel,
        layout: TextLayoutSize,
    ) -> Result<SerialNumberTextOperation, ErrorCode> {
        self.editor
            .create_serial_number_text_elements(document, layout)
    }

    pub fn process_input(
        &mut self,
        document: &DocumentModel,
        view: &mut EditorViewportState,
        event: InputEvent,
    ) -> Result<EditorUpdate, ErrorCode> {
        self.editor.view = *view;
        let update = self.editor.process_input(document, event)?;
        *view = self.editor.view;
        Ok(update)
    }

    pub fn active_text_resize_measurement_request(
        &self,
        document: &DocumentModel,
    ) -> Option<TextResizeMeasurementRequest> {
        self.editor.active_text_resize_measurement_request(document)
    }

    pub fn apply_active_text_resize_measurement(
        &mut self,
        document: &DocumentModel,
        layout: TextLayoutSize,
    ) -> Result<bool, ErrorCode> {
        self.editor
            .apply_active_text_resize_measurement(document, layout)
    }

    pub fn scene_input_revision(&self) -> u64 {
        self.editor.scene_input_revision()
    }

    pub fn overlay_input_revision(&self) -> u64 {
        self.editor.overlay_input_revision()
    }

    pub fn view_state(&self, view: &EditorViewportState) -> EditorViewState {
        EditorViewState {
            surface: view.surface,
            camera: view.camera,
            clear_color: self.editor.config.clear_color,
        }
    }

    pub fn presentation_state(
        &self,
        document: &DocumentModel,
        view: &EditorViewportState,
    ) -> EditorPresentationState {
        let mut editor = self.editor.clone();
        editor.view = *view;
        editor.presentation_state(document)
    }

    pub fn presentation_state_for_refresh(
        &mut self,
        document: &DocumentModel,
        view: &EditorViewportState,
    ) -> EditorPresentationState {
        let previous_view = std::mem::replace(&mut self.editor.view, *view);
        let presentation = self.editor.presentation_state(document);
        self.editor.view = previous_view;
        presentation
    }
}

const fn default_rectangle_filter_stroke_width() -> f64 {
    2.0
}

pub fn validate_editor_style_defaults(defaults: &EditorStyleDefaults) -> Result<(), ErrorCode> {
    super::style::validate_rectangle_shape_style(defaults.rectangle)?;
    super::style::validate_arrow_style(defaults.arrow)?;
    for style in [
        defaults.line,
        defaults.free_draw,
        defaults.rectangle_highlight,
        defaults.pen_highlight,
    ] {
        super::style::validate_line_style(style)?;
    }
    for (filter, stroke_width_range) in [
        (defaults.rectangle_filter, 0.0..=72.0),
        (defaults.pen_filter, 1.0..=72.0),
    ] {
        if !filter.strength.is_finite()
            || !(0.0..=1.0).contains(&filter.strength)
            || !filter.opacity.is_finite()
            || !(0.0..=1.0).contains(&filter.opacity)
            || !filter.stroke_width.is_finite()
            || !stroke_width_range.contains(&filter.stroke_width)
        {
            return Err(ErrorCode::InvalidArgument);
        }
    }
    super::style::validate_text_style(&defaults.text)?;
    super::style::validate_serial_number_style(&defaults.serial_number)?;
    Ok(())
}

fn validate_persisted_editor_styles(persisted: &PersistedEditorSession) -> Result<(), ErrorCode> {
    fn finite_non_negative(value: f64) -> bool {
        value.is_finite() && value >= 0.0
    }
    fn valid_corner_radii(radii: snow_draw_engine_core::CornerRadii) -> bool {
        [
            radii.top_left,
            radii.top_right,
            radii.bottom_right,
            radii.bottom_left,
        ]
        .into_iter()
        .all(finite_non_negative)
    }
    fn valid_shape(style: super::ShapeStyle) -> bool {
        finite_non_negative(style.stroke_width)
            && style.opacity.is_finite()
            && (0.0..=1.0).contains(&style.opacity)
            && valid_corner_radii(style.corner_radii)
    }

    if !finite_non_negative(persisted.rectangle.stroke_width)
        || !valid_corner_radii(persisted.rectangle.corner_radii)
        || !finite_non_negative(persisted.arrow.stroke_width)
        || !valid_shape(persisted.line)
        || !valid_shape(persisted.free_draw)
        || !valid_shape(persisted.rectangle_highlight)
        || !valid_shape(persisted.pen_highlight)
        || !finite_non_negative(persisted.rectangle_filter_stroke_width)
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

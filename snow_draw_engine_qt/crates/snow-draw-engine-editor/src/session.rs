use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{EngineConfig, ErrorCode, GridConfig, Point, SnapConfig};
use snow_draw_engine_document::{ElementId, SerialNumberNumericType, TextLayoutSize};
use snow_draw_engine_interaction::InputEvent;
use snow_draw_engine_model::DocumentModel;

use super::{
    ActiveTextDraftPresentation, ActiveTool, ArrowStyle, DocumentSyncSnapshot, Editor,
    EditorCommand, EditorPresentationState, EditorUpdate, EditorViewState, EditorViewportState,
    FilterStyle, RectangleShapeStyle, SerialNumberStyle, SerialNumberTextOperation,
    SerialNumberToolbarState, ShapeStyle, ShapeStylePatch, StyleToolbarSource, TextDraftCommit,
    TextLayoutOverride, TextResizeMeasurementRequest, TextStyle, state::EditorState,
};
use crate::DrawTemplate;
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
    #[serde(default)]
    spotlight_shape: snow_draw_engine_document::HighlightShape,
    rectangle: RectangleShapeStyle,
    arrow: ArrowStyle,
    #[serde(default)]
    distance: crate::DistanceStyle,
    #[serde(default)]
    angle: crate::AngleStyle,
    line: super::ShapeStyle,
    free_draw: super::ShapeStyle,
    rectangle_highlight: super::ShapeStyle,
    pen_highlight: super::ShapeStyle,
    filter: snow_draw_engine_document::FilterData,
    #[serde(default = "default_rectangle_filter_stroke_width")]
    rectangle_filter_stroke_width: f64,
    pen_filter: snow_draw_engine_document::PenFilterData,
    #[serde(default)]
    brush_eraser: crate::BrushEraserStyle,
    text: snow_draw_engine_document::TextData,
    serial_number: snow_draw_engine_document::SerialNumberData,
    #[serde(default)]
    serial_number_values: Option<[i64; 5]>,
    #[serde(default)]
    serial_number_sequence_overridden: [bool; 5],
}

impl EditorSession {
    pub fn take_text_edit_request(&mut self) -> Option<ElementId> {
        self.editor.state.pending_text_edit.take()
    }

    pub fn take_new_text_draft_request(&mut self) -> bool {
        self.editor.take_new_text_draft_request()
    }

    pub fn serial_number_label_layout_request(
        &self,
        document: &DocumentModel,
    ) -> Option<crate::SerialNumberLabelLayoutRequest> {
        self.editor.serial_number_label_layout_request(document)
    }

    pub fn apply_serial_number_label_layout(
        &mut self,
        document: &DocumentModel,
        text_id: ElementId,
        layout: TextLayoutSize,
    ) -> Result<bool, ErrorCode> {
        self.editor
            .apply_serial_number_label_layout(document, text_id, layout)
    }

    pub fn arrow_text_layout_requests(
        &self,
        document: &DocumentModel,
    ) -> Vec<crate::ArrowTextLayoutRequest> {
        self.editor.arrow_text_layout_requests(document)
    }
    pub fn arrow_text_request_build_count(&self) -> u64 {
        self.editor.arrow_text_request_build_count()
    }
    pub fn arrow_text_cached_owner_count(&self) -> usize {
        self.editor.arrow_text_cached_owner_count()
    }
    pub fn arrow_text_preview_candidate_count(&self) -> usize {
        self.editor.arrow_text_preview_candidate_count()
    }
    pub fn sync_arrow_text_cache_after_document_change(
        &mut self,
        document: &DocumentModel,
        delta: &snow_draw_engine_document::DocumentDelta,
    ) {
        self.editor
            .sync_arrow_text_cache_after_document_change(document, delta);
    }
    pub fn invalidate_arrow_text_measurements(&mut self) {
        self.editor.invalidate_arrow_text_measurements();
    }
    pub fn apply_arrow_text_measurements(
        &mut self,
        document: &DocumentModel,
        layouts: &[(ElementId, u64, TextLayoutSize, f64)],
    ) -> Result<bool, ErrorCode> {
        self.editor.apply_arrow_text_measurements(document, layouts)
    }

    pub fn apply_arrow_text_measurement(
        &mut self,
        document: &DocumentModel,
        id: ElementId,
        key: u64,
        size: TextLayoutSize,
        natural_width: f64,
    ) -> Result<bool, ErrorCode> {
        self.editor
            .apply_arrow_text_measurement(document, id, key, size, natural_width)
    }
    pub fn append_arrow_text_layouts(
        &self,
        document: &DocumentModel,
        transaction: &mut snow_draw_engine_document::Transaction,
    ) {
        self.editor.append_arrow_text_layouts(document, transaction);
    }

    pub fn append_arrow_text_layouts_preserving_labels(
        &self,
        document: &DocumentModel,
        transaction: &mut snow_draw_engine_document::Transaction,
        preserved_labels: &std::collections::HashSet<ElementId>,
    ) {
        self.editor.append_arrow_text_layouts_preserving_labels(
            document,
            transaction,
            preserved_labels,
        );
    }

    pub fn can_begin_arrow_text(&self) -> bool {
        matches!(
            self.editor.state.interaction,
            crate::state::InteractionState::Idle
        )
    }

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
            spotlight_shape: state.default_spotlight_shape,
            rectangle: state.default_rectangle_shape_style,
            arrow: state.default_arrow_style,
            distance: state.default_distance_style,
            angle: state.default_angle_style,
            line: state.default_line_style,
            free_draw: state.default_free_draw_style,
            rectangle_highlight: state.default_rectangle_highlight_style,
            pen_highlight: state.default_pen_highlight_style,
            filter: state.default_filter,
            rectangle_filter_stroke_width: state.default_filter_stroke_width,
            pen_filter: state.default_pen_filter.clone(),
            brush_eraser: state.default_brush_eraser,
            text: state.default_text.clone(),
            serial_number: state.default_serial_number.clone(),
            serial_number_values: Some(state.serial_number_values()),
            serial_number_sequence_overridden: state.serial_number_sequence_overridden,
        }
    }

    pub fn from_persisted(persisted: PersistedEditorSession) -> Result<Self, ErrorCode> {
        snow_draw_engine_core::validate_config(&persisted.config)?;
        snow_draw_engine_document::validate_filter(&persisted.filter)?;
        snow_draw_engine_document::validate_pen_filter(&persisted.pen_filter)?;
        snow_draw_engine_document::validate_text(&persisted.text)?;
        snow_draw_engine_document::validate_serial_number(&persisted.serial_number)?;
        if persisted
            .serial_number_values
            .is_some_and(|values| values.iter().any(|number| *number < 0))
        {
            return Err(ErrorCode::InvalidArgument);
        }
        validate_persisted_editor_styles(&persisted)?;

        let mut session = Self::new(persisted.config)?;
        let state = &mut session.editor.state;
        state.default_rectangle_shape_style = persisted.rectangle;
        state.default_spotlight_shape = persisted.spotlight_shape;
        state.default_arrow_style = persisted.arrow;
        state.default_distance_style = persisted.distance;
        state.default_angle_style = persisted.angle;
        crate::validate_angle_style(persisted.angle)?;
        state.default_line_style = ShapeStyle {
            arrow_type: crate::style::normalized_line_arrow_type(persisted.line.arrow_type),
            arrow_shaft_type: Default::default(),
            arrow_ratio: 1.0,
            ..persisted.line
        };
        state.default_free_draw_style = persisted.free_draw;
        state.default_rectangle_highlight_style = persisted.rectangle_highlight;
        state.default_pen_highlight_style = persisted.pen_highlight;
        state.default_filter = persisted.filter;
        state.default_filter_stroke_width = persisted.rectangle_filter_stroke_width;
        state.default_pen_filter = persisted.pen_filter;
        state.default_brush_eraser = persisted.brush_eraser;
        state.default_pen_filter.strength = state.default_filter.strength;
        if state.default_filter.filter_type
            == snow_draw_engine_document::CanvasFilterType::SmartErase
        {
            state.default_filter.strength = 0.5;
        }
        if state.default_pen_filter.filter_type
            == snow_draw_engine_document::CanvasFilterType::SmartErase
        {
            state.default_pen_filter.strength = 0.5;
        }
        state.default_text = persisted.text;
        state.default_serial_number = persisted.serial_number;
        state.serial_number_values_by_numeric_type =
            persisted.serial_number_values.unwrap_or([1; 5]);
        state.serial_number_sequence_overridden = persisted.serial_number_sequence_overridden;
        session.reset_editing_state();
        Ok(session)
    }

    pub fn from_persisted_with_document(
        persisted: PersistedEditorSession,
        document: &DocumentModel,
    ) -> Result<Self, ErrorCode> {
        let has_creation_counters = persisted.serial_number_values.is_some();
        let mut session = Self::from_persisted(persisted)?;
        if !has_creation_counters {
            session.advance_serial_number_counters_for_insertions(document, document.paint_order());
        }
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

    /// Discards measurements, numbering sequences, and transient storage from the old document.
    pub fn reset_document_retained_state(&mut self, initial_serial_number: &SerialNumberStyle) {
        self.editor.reset_editing_state();
        self.editor.invalidate_arrow_text_measurements();
        self.editor.state.arrow_text_measurements = std::collections::HashMap::new();
        self.editor.state.selection = Default::default();
        self.editor.state.ui = Default::default();
        // Counters and explicit starts belong to a document, unlike creation appearance.
        let state = &mut self.editor.state;
        state.serial_number_values_by_numeric_type = [1; 5];
        state.serial_number_values_by_numeric_type[initial_serial_number.numeric_type as usize] =
            initial_serial_number.number;
        state.serial_number_sequence_overridden = [false; 5];
        state.default_serial_number.number = state.serial_number_values_by_numeric_type
            [state.default_serial_number.numeric_type as usize];
    }

    pub fn style_toolbar_source(&self, document: &DocumentModel) -> StyleToolbarSource {
        self.editor.style_toolbar_source(document)
    }

    pub fn selected_element_count(&self, document: &DocumentModel) -> usize {
        self.editor.selected_element_count(document)
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

    pub fn angle_style(&self, document: &DocumentModel) -> crate::AngleStyle {
        self.editor.angle_style(document)
    }
    pub fn angle_style_mixed(&self, document: &DocumentModel) -> u32 {
        self.editor.angle_style_mixed(document)
    }
    pub fn set_angle_style_patch(
        &mut self,
        document: &DocumentModel,
        style: crate::AngleStyle,
        properties: u32,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor
            .set_angle_style_patch(document, style, properties)
    }
    pub fn adjust_angle_value(
        &mut self,
        document: &DocumentModel,
        delta_radians: f64,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.adjust_angle_value(document, delta_radians)
    }

    pub fn distance_style(&self, document: &DocumentModel) -> crate::DistanceStyle {
        self.editor.distance_style(document)
    }
    pub fn distance_style_mixed(&self, document: &DocumentModel) -> u32 {
        self.editor.distance_style_mixed(document)
    }
    pub fn distance_measured_length(&self, document: &DocumentModel) -> f64 {
        self.editor.distance_measured_length(document)
    }
    pub fn set_distance_style_patch(
        &mut self,
        document: &DocumentModel,
        style: crate::DistanceStyle,
        properties: u32,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor
            .set_distance_style_patch(document, style, properties)
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
        self.editor
            .with_view(view, |editor| editor.serial_number_toolbar_state(document))
    }

    pub fn capture_document_sync_snapshot(&self, document: &DocumentModel) -> DocumentSyncSnapshot {
        self.editor.capture_document_sync_snapshot(document)
    }

    pub fn smart_erase_presentation(&self, document: &DocumentModel) -> EditorPresentationState {
        self.editor.smart_erase_presentation(document)
    }

    pub fn serial_number_types_following_document(
        &self,
        document: &DocumentModel,
    ) -> Vec<SerialNumberNumericType> {
        let values = self.editor.state.serial_number_values();
        [
            SerialNumberNumericType::Arabic,
            SerialNumberNumericType::Roman,
            SerialNumberNumericType::LowercaseLetters,
            SerialNumberNumericType::UppercaseLetters,
            SerialNumberNumericType::Chinese,
        ]
        .into_iter()
        .filter(|numeric_type| {
            !self.editor.state.serial_number_sequence_overridden[*numeric_type as usize]
                && values[*numeric_type as usize]
                    == crate::document_ops::next_serial_number(document, *numeric_type)
        })
        .collect()
    }

    pub fn advance_serial_number_counters_for_insertions(
        &mut self,
        document: &DocumentModel,
        ids: &[ElementId],
    ) {
        let state = &mut self.editor.state;
        for id in ids {
            let Ok(serial) = document.serial_number(*id) else {
                continue;
            };
            if !serial.serial_number_type.supports_number()
                || state.serial_number_sequence_overridden[serial.numeric_type as usize]
            {
                continue;
            }
            let next = serial
                .number
                .saturating_add(1)
                .max(state.serial_number_values()[serial.numeric_type as usize]);
            state.set_serial_number_value(serial.numeric_type, next);
        }
    }

    pub fn sync_serial_number_types_after_document_change(
        &mut self,
        document: &DocumentModel,
        numeric_types: &[SerialNumberNumericType],
    ) {
        for numeric_type in numeric_types {
            self.editor.state.set_serial_number_value(
                *numeric_type,
                crate::document_ops::next_serial_number(document, *numeric_type),
            );
        }
    }

    /// Restore the selection stored with a duplication history entry.
    pub fn restore_history_selection(
        &mut self,
        document: &DocumentModel,
        snapshot: &DocumentSyncSnapshot,
    ) {
        self.editor.set_selection_state_with_document(
            Some(document),
            snapshot.selection.ids.clone(),
            snapshot.selection.primary,
        );
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

    pub fn set_text_creation_style(
        &mut self,
        style: TextStyle,
        properties: u32,
    ) -> Result<(), ErrorCode> {
        self.editor.set_text_creation_style(style, properties)
    }

    pub fn set_text_style(
        &mut self,
        document: &DocumentModel,
        style: TextStyle,
        properties: u32,
        layouts: &[TextLayoutOverride],
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor
            .set_text_style(document, style, properties, layouts)
    }

    pub fn set_serial_number_style_patch(
        &mut self,
        document: &DocumentModel,
        style: SerialNumberStyle,
        properties: u32,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor
            .set_serial_number_style_patch(document, style, properties)
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

    pub fn hit_quick_selection_at(
        &self,
        document: &DocumentModel,
        view: &EditorViewportState,
        point: Point<f64>,
        button: snow_draw_engine_interaction::PointerButton,
    ) -> Option<ElementId> {
        self.editor.with_view(view, |editor| {
            editor.hit_quick_selection_at(document, point, button)
        })
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

    pub fn delete_elements(
        &mut self,
        document: &DocumentModel,
        ids: &[ElementId],
        label: &str,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.delete_elements(document, ids, label)
    }

    pub fn duplicate_selected(
        &mut self,
        document: &DocumentModel,
        offset: Point<f64>,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.duplicate_selected(document, offset)
    }

    pub fn selected_draw_template(
        &self,
        document: &DocumentModel,
    ) -> Result<DrawTemplate, ErrorCode> {
        self.editor.selected_draw_template(document)
    }

    pub fn insert_draw_template(
        &mut self,
        document: &DocumentModel,
        template: &DrawTemplate,
        center: Point<f64>,
    ) -> Result<EditorCommand, ErrorCode> {
        self.editor.insert_draw_template(document, template, center)
    }

    pub fn brush_eraser_style(&self) -> crate::BrushEraserStyle {
        self.editor.brush_eraser_style()
    }

    pub fn set_brush_eraser_creation_style(
        &mut self,
        style: crate::BrushEraserStyle,
        properties: u32,
    ) -> Result<(), ErrorCode> {
        self.editor
            .set_brush_eraser_creation_style(style, properties)
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

    pub fn set_filter_creation_style(
        &mut self,
        style: FilterStyle,
        properties: u32,
        tool: ActiveTool,
    ) -> Result<(), ErrorCode> {
        self.editor
            .set_filter_creation_style(style, properties, tool)
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

    pub fn align_selected(
        &mut self,
        document: &DocumentModel,
        alignment: u32,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        self.editor.align_selected(document, alignment)
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
        self.editor.view.set(*view);
        let update = self.editor.process_input(document, event)?;
        *view = self.editor.view.get();
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

    pub fn angle_adjustment_target_revision(&self, document: &DocumentModel) -> u64 {
        self.editor.angle_adjustment_target_revision(document)
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
        self.editor
            .with_view(view, |editor| editor.presentation_state(document))
    }

    pub fn presentation_state_for_refresh(
        &mut self,
        document: &DocumentModel,
        view: &EditorViewportState,
    ) -> EditorPresentationState {
        self.editor
            .with_view(view, |editor| editor.presentation_state(document))
    }
}

const fn default_rectangle_filter_stroke_width() -> f64 {
    2.0
}

#[cfg(test)]
mod borrowed_view_query_tests {
    use super::*;
    use snow_draw_engine_core::{
        Camera, ColorRgba8, SurfaceSize,
        arrow::{ArrowType, StrokeStyle},
    };
    use snow_draw_engine_document::{ArrowData, ElementMeta, SerialNumberData, Transaction};
    use snow_draw_engine_interaction::PointerButton;

    #[test]
    fn serial_number_and_presentation_queries_borrow_the_view_and_reuse_label_cache() {
        let mut document = DocumentModel::new();
        let serial = document.allocate_element_id();
        let owner = document.allocate_element_id();
        let text = document.allocate_element_id();
        let mut arrow = ArrowData::from_global_points(
            &[
                Point::new(10_100.0, 10_000.0),
                Point::new(10_000.0, 10_000.0),
                Point::new(10_000.0, 9_900.0),
            ],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        crate::AngleStyle::default().apply_to_arrow(&mut arrow);
        arrow.text_element_id = Some(text);
        let mut tx = Transaction::new("borrowed view fixture");
        tx.insert_serial_number(serial, ElementMeta::default(), SerialNumberData::default());
        let label = snow_draw_engine_document::generated_annotation_label(&arrow, None).unwrap();
        tx.insert_arrow(owner, ElementMeta::default(), arrow);
        tx.insert_text(text, ElementMeta::default(), label);
        document.apply_transaction(tx).unwrap();
        let mut session = EditorSession::new(Default::default()).unwrap();
        session.set_active_tool(ActiveTool::Select).unwrap();
        session.select_element(&document, serial).unwrap();
        session
            .editor
            .set_camera(Camera {
                center: Point::new(77.0, -88.0),
                zoom: 3.0,
            })
            .unwrap();
        let original = session.editor.view.get();
        let snapshot = session.snapshot();
        let first = EditorViewportState {
            surface: SurfaceSize {
                width: 800,
                height: 600,
            },
            ..Default::default()
        };
        let second = EditorViewportState {
            camera: Camera {
                center: Point::new(10_000.0, 10_000.0),
                zoom: 2.0,
            },
            ..first
        };
        assert!(
            session
                .serial_number_toolbar_state(&document, &first)
                .visible
        );
        assert!(
            !session
                .serial_number_toolbar_state(&document, &second)
                .visible
        );
        assert!(
            session
                .presentation_state(&document, &first)
                .arrow_text_previews
                .is_empty()
        );
        let builds = session.arrow_text_request_build_count();
        session.presentation_state(&document, &second);
        assert_eq!(session.arrow_text_request_build_count(), builds);
        assert_eq!(
            session.hit_quick_selection_at(
                &document,
                &first,
                Point::new(0.0, 0.0),
                PointerButton::Primary
            ),
            Some(serial)
        );
        assert_eq!(session.editor.view.get(), original);
        assert_eq!(
            session.snapshot(),
            snapshot,
            "read-only viewport queries preserve retained editor state"
        );
    }

    #[test]
    fn borrowed_view_scopes_restore_after_nested_queries_and_unwind() {
        let editor = Editor::new(Default::default()).unwrap();
        let original = editor.view.get();
        let first = EditorViewportState {
            camera: Camera {
                center: Point::new(1.0, 2.0),
                zoom: 2.0,
            },
            ..original
        };
        let second = EditorViewportState {
            camera: Camera {
                center: Point::new(3.0, 4.0),
                zoom: 4.0,
            },
            ..original
        };
        editor.with_view(&first, |editor| {
            assert_eq!(editor.camera(), first.camera);
            editor.with_view(&second, |editor| assert_eq!(editor.camera(), second.camera));
            assert_eq!(editor.camera(), first.camera);
        });
        let unwind = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            editor.with_view(&second, |_| panic!("test view restoration"));
        }));
        assert!(unwind.is_err());
        assert_eq!(editor.view.get(), original);
    }
}

pub fn validate_editor_style_defaults(defaults: &EditorStyleDefaults) -> Result<(), ErrorCode> {
    super::style::validate_rectangle_shape_style(defaults.rectangle)?;
    super::style::validate_arrow_style(defaults.arrow)?;
    crate::validate_distance_style(defaults.distance)?;
    crate::validate_angle_style(defaults.angle)?;
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
    if !defaults.brush_eraser.stroke_width.is_finite()
        || !(1.0..=72.0).contains(&defaults.brush_eraser.stroke_width)
        || defaults.rectangle_filter.filter_type
            == snow_draw_engine_document::CanvasFilterType::RestoreBackground
        || defaults.pen_filter.filter_type
            == snow_draw_engine_document::CanvasFilterType::RestoreBackground
    {
        return Err(ErrorCode::InvalidArgument);
    }
    super::style::validate_text_style(&defaults.text)?;
    super::style::validate_serial_number_style(&defaults.serial_number)?;
    Ok(())
}

fn validate_persisted_editor_styles(persisted: &PersistedEditorSession) -> Result<(), ErrorCode> {
    crate::validate_distance_style(persisted.distance)?;
    crate::validate_angle_style(persisted.angle)?;
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

    if !persisted.brush_eraser.stroke_width.is_finite()
        || !(1.0..=72.0).contains(&persisted.brush_eraser.stroke_width)
        || persisted.filter.filter_type
            == snow_draw_engine_document::CanvasFilterType::RestoreBackground
        || persisted.pen_filter.filter_type
            == snow_draw_engine_document::CanvasFilterType::RestoreBackground
        || !finite_non_negative(persisted.rectangle.stroke_width)
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

#[cfg(test)]
mod document_reset_tests {
    use super::*;

    #[test]
    fn document_reset_releases_layouts_and_preserves_creation_styles() {
        let mut session = EditorSession::new(EngineConfig::default()).unwrap();
        session
            .editor
            .state
            .default_rectangle_shape_style
            .stroke_width = 17.0;
        session.editor.state.arrow_text_measurements.reserve(64);
        session.editor.state.arrow_text_measurements.insert(
            ElementId {
                index: 5,
                generation: 3,
            },
            crate::arrow_text::ArrowTextMeasurement {
                text_id: ElementId {
                    index: 5,
                    generation: 3,
                },
                key: 7,
                size: TextLayoutSize::new(20.0, 10.0),
                text_key: 11,
                natural_width: 20.0,
            },
        );
        session.editor.state.ui.snap_guides.reserve(64);
        session.editor.state.selection.ids.reserve(64);
        session.set_quick_selection_disabled_tools(3);
        let config = session.config();
        let generation = session.editor.state.arrow_text_measurement_generation;

        session.reset_document_retained_state(&EditorStyleDefaults::default().serial_number);
        assert_eq!(session.editor.state.arrow_text_measurements.capacity(), 0);
        assert_eq!(session.editor.state.ui.snap_guides.capacity(), 0);
        assert_eq!(session.editor.state.selection.ids.capacity(), 0);
        assert!(session.editor.state.arrow_text_measurement_generation > generation);
        assert_eq!(
            session
                .editor
                .state
                .default_rectangle_shape_style
                .stroke_width,
            17.0
        );
        assert_eq!(session.config(), config);
        assert_eq!(session.quick_selection_disabled_tools(), 3);
    }
}

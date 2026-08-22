mod active_text;
mod api;
mod arrow_ops;
mod creation_workflow;
mod defaults;
mod document_ops;
mod edit_workflow;
mod eraser_workflow;
mod free_draw_workflow;
mod geometry;
mod input;
mod pen_filter_workflow;
mod polyline_simplification;
mod presentation;
mod selection;
mod session;
mod snapping;
mod state;
mod style;
mod text;

pub use api::{
    ActiveTextDraftPresentation, ActiveTextDraftTarget, ActiveTool, ArrowHandleKind,
    ArrowHandleState, ArrowStyle, EditorPresentationState, EditorStrokeCursor, EditorViewState,
    EditorViewportState, ElementCreationPreview, FILTER_STYLE_PROPERTY_ALL,
    FILTER_STYLE_PROPERTY_OPACITY, FILTER_STYLE_PROPERTY_STRENGTH,
    FILTER_STYLE_PROPERTY_STROKE_WIDTH, FILTER_STYLE_PROPERTY_TYPE, FilterStyle, FreeDrawPreview,
    HistoryState, PenFilterPreview, RectangleShapeStyle, SERIAL_NUMBER_STYLE_MIXED_COLOR,
    SERIAL_NUMBER_STYLE_MIXED_FILL, SERIAL_NUMBER_STYLE_MIXED_FILL_STYLE,
    SERIAL_NUMBER_STYLE_MIXED_FONT_FAMILY, SERIAL_NUMBER_STYLE_MIXED_FONT_SIZE,
    SERIAL_NUMBER_STYLE_MIXED_NUMBER, SERIAL_NUMBER_STYLE_MIXED_OPACITY,
    SERIAL_NUMBER_STYLE_MIXED_STROKE_STYLE, SERIAL_NUMBER_STYLE_MIXED_STROKE_WIDTH,
    SHAPE_STYLE_MIXED_ARROW_TYPE, SHAPE_STYLE_MIXED_CORNER_RADII, SHAPE_STYLE_MIXED_END_ARROWHEAD,
    SHAPE_STYLE_MIXED_FILL, SHAPE_STYLE_MIXED_FILL_STYLE, SHAPE_STYLE_MIXED_HIGHLIGHT_SHAPE,
    SHAPE_STYLE_MIXED_OPACITY, SHAPE_STYLE_MIXED_SHAPE, SHAPE_STYLE_MIXED_START_ARROWHEAD,
    SHAPE_STYLE_MIXED_STROKE, SHAPE_STYLE_MIXED_STROKE_STYLE, SHAPE_STYLE_MIXED_STROKE_WIDTH,
    SHAPE_STYLE_PROPERTY_ALL, SHAPE_STYLE_PROPERTY_ARROW, SHAPE_STYLE_PROPERTY_ARROW_TYPE,
    SHAPE_STYLE_PROPERTY_CORNER_RADII, SHAPE_STYLE_PROPERTY_END_ARROWHEAD,
    SHAPE_STYLE_PROPERTY_FILL, SHAPE_STYLE_PROPERTY_FILL_STYLE,
    SHAPE_STYLE_PROPERTY_HIGHLIGHT_SHAPE, SHAPE_STYLE_PROPERTY_OPACITY,
    SHAPE_STYLE_PROPERTY_RECTANGLE, SHAPE_STYLE_PROPERTY_SHAPE,
    SHAPE_STYLE_PROPERTY_START_ARROWHEAD, SHAPE_STYLE_PROPERTY_STROKE,
    SHAPE_STYLE_PROPERTY_STROKE_STYLE, SHAPE_STYLE_PROPERTY_STROKE_WIDTH, SelectionArrowState,
    SelectionBounds, SelectionRectState, SerialNumberToolbarState, ShapeKind, ShapeStyle,
    ShapeStylePatch, StyleToolbarSource, StyleToolbarState, TEXT_STYLE_MIXED_COLOR,
    TEXT_STYLE_MIXED_CORNER_RADII, TEXT_STYLE_MIXED_FILL, TEXT_STYLE_MIXED_FILL_STYLE,
    TEXT_STYLE_MIXED_FONT_FAMILY, TEXT_STYLE_MIXED_FONT_SIZE, TEXT_STYLE_MIXED_HORIZONTAL_ALIGN,
    TEXT_STYLE_MIXED_OPACITY, TEXT_STYLE_MIXED_STROKE, TEXT_STYLE_MIXED_STROKE_WIDTH,
    TEXT_STYLE_MIXED_VERTICAL_ALIGN, selection_box_visible_for_members,
};
pub use defaults::{EditorStyleDefaults, editor_style_defaults};
pub use session::{
    EditorSession, EditorSessionSnapshot, PersistedEditorSession, validate_editor_style_defaults,
};
pub use state::DocumentSyncSnapshot;
pub use text::{
    SerialNumberStyle, TextCommitTarget, TextDraftCommit, TextLayoutOverride, TextPreviewFontSize,
    TextResizeMeasurementRequest, TextStyle,
};

pub(crate) use arrow_ops::*;
use document_ops::*;
pub(crate) use geometry::*;
use snow_draw_engine_core::{
    Camera, DrawRect, EngineConfig, ErrorCode, GRID_SNAP_SERVICE, GridConfig, OBJECT_SNAP_SERVICE,
    ObjectSnapRectRequest, ObjectSnapResult, Point, SnapAxisAnchor, SnapConfig, SnapGuide,
    SnappingMode, SurfaceSize, ZoomFocus, normalize_rotation, resolve_effective_snapping_mode,
    validate_camera, validate_config, view_to_canvas,
};
use snow_draw_engine_document::{
    ArrowData, DEFAULT_ARROW_MAX_COORDINATE, ElementId, ElementKind, PenFilterData, RectangleData,
    RectangleElementKind, SerialNumberData, TextData, TextLayoutSize, Transaction, arrow_bounds,
    arrow_length as document_arrow_length, normalize_corner_radii, validate_arrow, validate_filter,
    validate_rectangle, validate_serial_number, validate_text, validate_text_layout_size,
};
use snow_draw_engine_interaction::{
    CursorCommand, CursorStyle, InputEvent, InteractionOutput, KeyCode, KeyEvent, KeyEventType,
    Modifiers, PointerButton, PointerCaptureCommand, PointerEvent, PointerEventType,
    WheelDeltaKind, WheelEvent,
};
use snow_draw_engine_model::{BindableElementState, DocumentModel};
use state::*;

#[derive(Clone, Debug, Default, PartialEq)]
pub struct SerialNumberTextOperation {
    pub command: Option<EditorCommand>,
    pub single_text_id: Option<ElementId>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ApplyTransactionCommand {
    pub transaction: Transaction,
    pub history_undo_snapshot: Option<DocumentSyncSnapshot>,
}

impl ApplyTransactionCommand {
    fn new(transaction: Transaction) -> Self {
        Self {
            transaction,
            history_undo_snapshot: None,
        }
    }

    fn with_history_undo_snapshot(
        transaction: Transaction,
        history_undo_snapshot: DocumentSyncSnapshot,
    ) -> Self {
        Self {
            transaction,
            history_undo_snapshot: Some(history_undo_snapshot),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum EditorCommand {
    ApplyTransaction(ApplyTransactionCommand),
    Undo,
    Redo,
}

#[derive(Clone, Debug, PartialEq)]
pub struct EditorUpdate {
    pub interaction: InteractionOutput,
    pub command: Option<EditorCommand>,
}

#[derive(Clone, Debug)]
pub struct Editor {
    state: EditorState,
    view: EditorViewportState,
    config: EngineConfig,
    quick_selection_disabled_tools: u64,
    scene_state_revision: u64,
    overlay_state_revision: u64,
    pending_command: Option<EditorCommand>,
}

impl Editor {
    pub fn new(config: EngineConfig) -> Result<Self, ErrorCode> {
        validate_config(&config)?;
        Ok(Self {
            state: EditorState {
                active_tool: ActiveTool::Shape,
                ..EditorState::default()
            },
            view: EditorViewportState::default(),
            config,
            quick_selection_disabled_tools: 0,
            scene_state_revision: 0,
            overlay_state_revision: 0,
            pending_command: None,
        })
    }

    fn queue_command(&mut self, command: EditorCommand) {
        self.pending_command = Some(command);
    }

    pub fn snap_config(&self) -> SnapConfig {
        self.config.snap
    }

    pub fn set_snap_config(&mut self, snap: SnapConfig) -> Result<(), ErrorCode> {
        let mut next = self.config;
        next.snap = snap;
        validate_config(&next)?;
        if self.config.snap == snap {
            return Ok(());
        }

        self.config = next;
        if !self.config.snap.show_guides && !self.state.ui.snap_guides.is_empty() {
            self.state.ui.snap_guides.clear();
        }
        self.bump_overlay_state_revision();
        Ok(())
    }

    pub fn grid_config(&self) -> GridConfig {
        self.config.grid
    }

    pub fn set_grid_config(&mut self, grid: GridConfig) -> Result<(), ErrorCode> {
        let mut next = self.config;
        next.grid = grid;
        validate_config(&next)?;
        if self.config.grid == grid {
            return Ok(());
        }

        self.config = next;
        self.bump_overlay_state_revision();
        Ok(())
    }

    pub fn surface_size(&self) -> SurfaceSize {
        self.view.surface
    }

    pub fn camera(&self) -> Camera {
        self.view.camera
    }

    pub fn active_tool(&self) -> ActiveTool {
        self.state.active_tool
    }

    pub fn quick_selection_disabled_tools(&self) -> u64 {
        self.quick_selection_disabled_tools
    }

    pub fn set_quick_selection_disabled_tools(&mut self, tools: u64) {
        self.quick_selection_disabled_tools = tools;
    }

    pub fn set_active_tool(&mut self, active_tool: ActiveTool) -> Result<(), ErrorCode> {
        if self.state.active_tool == active_tool {
            return Ok(());
        }

        let previous_tool = self.state.active_tool;
        self.cancel_interaction();
        self.clear_eraser_state();
        self.clear_stroke_cursor_state();
        self.state.active_tool = active_tool;
        self.set_hovered_element(None);
        if Self::tool_policy_for(active_tool).clear_selection_on_activate
            || previous_tool == ActiveTool::Text
        {
            self.clear_selection();
        } else {
            self.bump_overlay_state_revision();
        }
        Ok(())
    }

    pub fn reset_editing_state(&mut self) {
        self.cancel_interaction();
        self.clear_eraser_state();
        self.clear_stroke_cursor_state();
        self.clear_active_text_draft_presentation();
        self.set_hovered_element(None);
        self.clear_selection();
        if self.state.active_tool != ActiveTool::Select {
            self.state.active_tool = ActiveTool::Select;
            self.bump_overlay_state_revision();
        }
    }

    pub fn capture_document_sync_snapshot(&self, document: &DocumentModel) -> DocumentSyncSnapshot {
        let _ = document;
        DocumentSyncSnapshot {
            selection: self.state.selection.clone(),
        }
    }

    pub fn sync_after_document_change(
        &mut self,
        document: &DocumentModel,
        snapshot: &DocumentSyncSnapshot,
    ) {
        self.sync_selection_after_document_change(document, &snapshot.selection);
        self.bump_scene_state_revision();
        self.bump_overlay_state_revision();
    }

    pub fn set_surface_size(&mut self, width: u32, height: u32) -> Result<(), ErrorCode> {
        let next = SurfaceSize { width, height };
        if self.view.surface != next {
            self.view.surface = next;
            self.bump_scene_state_revision();
            self.bump_overlay_state_revision();
        }
        Ok(())
    }

    pub fn set_camera(&mut self, camera: Camera) -> Result<(), ErrorCode> {
        validate_camera(&camera)?;
        if self.view.camera != camera {
            self.view.camera = camera;
            self.bump_scene_state_revision();
            self.bump_overlay_state_revision();
        }
        Ok(())
    }

    fn cancel_interaction(&mut self) {
        let had_selection_edit = matches!(
            &self.state.interaction,
            InteractionState::PendingSelectionMove(_)
                | InteractionState::EditingSelection(_)
                | InteractionState::EditingArrow(_)
        );
        self.state.interaction = InteractionState::Idle;
        if had_selection_edit {
            self.bump_scene_state_revision();
            self.bump_overlay_state_revision();
        }
        self.clear_transient_visuals();
    }

    fn set_creation_preview(
        &mut self,
        preview: Option<ElementCreationPreview>,
        snap_guides: Vec<SnapGuide>,
    ) {
        if self.state.creation_preview != preview {
            self.state.creation_preview = preview;
            self.bump_scene_state_revision();
        }
        if self.state.ui.marquee.take().is_some() || self.state.ui.snap_guides != snap_guides {
            self.state.ui.snap_guides = snap_guides;
            self.bump_overlay_state_revision();
        }
    }

    fn set_marquee(&mut self, marquee: Option<RectangleData>) {
        if self.state.creation_preview.take().is_some() {
            self.bump_scene_state_revision();
        }
        if self.state.ui.marquee != marquee || !self.state.ui.snap_guides.is_empty() {
            self.state.ui.marquee = marquee;
            self.state.ui.snap_guides.clear();
            self.bump_overlay_state_revision();
        }
    }

    fn set_snap_guides(&mut self, snap_guides: Vec<SnapGuide>) {
        if self.state.creation_preview.take().is_some() {
            self.bump_scene_state_revision();
        }
        if self.state.ui.marquee.take().is_some() || self.state.ui.snap_guides != snap_guides {
            self.state.ui.snap_guides = snap_guides;
            self.bump_overlay_state_revision();
        }
    }

    fn clear_transient_visuals(&mut self) {
        if self.state.creation_preview.take().is_some() {
            self.bump_scene_state_revision();
        }
        if self.state.ui.marquee.take().is_some() || !self.state.ui.snap_guides.is_empty() {
            self.state.ui.snap_guides.clear();
            self.bump_overlay_state_revision();
        }
    }

    fn bump_scene_state_revision(&mut self) {
        self.scene_state_revision = self.scene_state_revision.wrapping_add(1);
    }

    fn bump_overlay_state_revision(&mut self) {
        self.overlay_state_revision = self.overlay_state_revision.wrapping_add(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn selected_editor(active_tool: ActiveTool) -> Editor {
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        let id = ElementId {
            index: 1,
            generation: 0,
        };
        editor.state.active_tool = active_tool;
        editor.set_selection_state(vec![id], Some(id));
        editor
    }

    fn test_rect() -> RectangleData {
        RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(10.0, 20.0),
            width: 80.0,
            height: 60.0,
            rotation: 0.0,
            fill: snow_draw_engine_core::ColorRgba8::default(),
            fill_style: snow_draw_engine_document::FillStyle::Solid,
            stroke: snow_draw_engine_core::ColorRgba8::default(),
            stroke_width: 2.0,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: snow_draw_engine_core::CornerRadii::default(),
            opacity: 1.0,
        }
    }

    #[test]
    fn creation_preview_only_bumps_scene_revision() {
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        let scene_revision = editor.scene_input_revision();
        let overlay_revision = editor.overlay_input_revision();

        editor.set_creation_preview(
            Some(ElementCreationPreview::Rectangle(test_rect())),
            Vec::new(),
        );

        assert!(editor.scene_input_revision() > scene_revision);
        assert_eq!(editor.overlay_input_revision(), overlay_revision);
    }

    #[test]
    fn marquee_only_bumps_overlay_revision() {
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        let scene_revision = editor.scene_input_revision();
        let overlay_revision = editor.overlay_input_revision();

        editor.set_marquee(Some(test_rect()));

        assert_eq!(editor.scene_input_revision(), scene_revision);
        assert!(editor.overlay_input_revision() > overlay_revision);
    }

    #[test]
    fn switching_from_text_tool_to_select_clears_selection() {
        let mut editor = selected_editor(ActiveTool::Text);

        editor.set_active_tool(ActiveTool::Select).unwrap();

        assert!(editor.state.selection.is_empty());
    }

    #[test]
    fn switching_from_shape_tool_to_select_preserves_selection() {
        let mut editor = selected_editor(ActiveTool::Shape);

        editor.set_active_tool(ActiveTool::Select).unwrap();

        assert!(!editor.state.selection.is_empty());
    }

    #[test]
    fn resetting_editing_state_clears_selection_and_restores_select_tool() {
        let mut editor = selected_editor(ActiveTool::Shape);

        editor.reset_editing_state();

        assert!(editor.state.selection.is_empty());
        assert_eq!(editor.active_tool(), ActiveTool::Select);
    }

    #[test]
    fn updating_text_with_same_content_clears_selection() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let mut text = TextData {
            text: "unchanged".to_owned(),
            ..TextData::default()
        };
        text.width = 120.0;
        text.height = 30.0;

        let mut transaction = Transaction::new("insert text");
        transaction.insert_text(id, snow_draw_engine_document::ElementMeta::default(), text);
        document.apply_transaction(transaction).unwrap();

        let mut editor = selected_editor(ActiveTool::Text);
        editor.select_element(&document, id).unwrap();

        let command = editor
            .update_text_element(
                &document,
                id,
                "unchanged",
                TextLayoutSize {
                    width: 120.0,
                    height: 30.0,
                },
            )
            .unwrap();

        assert!(command.is_none());
        assert!(editor.state.selection.is_empty());
    }
}

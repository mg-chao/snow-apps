use std::collections::HashMap;

use crate::history::HistoryStore;
use snow_draw_engine_core::{
    Camera, EngineConfig as ViewEngineConfig, ErrorCode, GridConfig, Point, SnapConfig,
};
use snow_draw_engine_display::{PatchCursor, ViewportPatch};
use snow_draw_engine_document::{ElementId, SpotlightConfig, WatermarkConfig};
use snow_draw_engine_editor::{
    ActiveTool, ApplyTransactionCommand, ArrowStyle, DocumentSyncSnapshot, EditorCommand,
    EditorPresentationState, EditorSession, EditorSessionSnapshot, EditorStyleDefaults,
    EditorViewportState, FilterStyle, HistoryState, RectangleShapeStyle, SerialNumberToolbarState,
    ShapeStylePatch, StyleToolbarState,
};
use snow_draw_engine_interaction::{InputEvent, InteractionOutput};
use snow_draw_engine_model::DocumentModel;
use snow_draw_engine_scene::{DocumentSceneCache, ViewportComposer};

mod document_commands;
mod input;
mod mutations;
mod text_commands;
mod viewports;

pub use text_commands::TextElementInfo;

#[derive(Clone, Debug, Default, PartialEq)]
pub struct StyleDefaults {
    pub editor: EditorStyleDefaults,
    pub watermark: WatermarkConfig,
    pub spotlight: SpotlightConfig,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct EngineConfig {
    pub style_defaults: StyleDefaults,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ViewportConfig {
    pub engine: ViewEngineConfig,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct ViewportId(pub u64);

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MutationResult {
    pub changed_viewports: Vec<ViewportId>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct InputUpdate {
    pub interaction: InteractionOutput,
    pub changed_viewports: Vec<ViewportId>,
}

#[derive(Debug)]
struct ViewportSlot {
    view: EditorViewportState,
    composer: ViewportComposer,
}

#[derive(Debug)]
pub struct Engine {
    pub(crate) config: EngineConfig,
    pub(crate) model: DocumentModel,
    pub(crate) history: HistoryStore,
    pub(crate) editor: EditorSession,
    pub(crate) session_config_seeded: bool,
    pub(crate) scene_cache: DocumentSceneCache,
    viewports: HashMap<ViewportId, ViewportSlot>,
    next_viewport_id: u64,
}

impl Default for Engine {
    fn default() -> Self {
        Self::new(EngineConfig::default())
    }
}

impl Engine {
    pub fn new(config: EngineConfig) -> Self {
        Self::try_new(config).expect("runtime config should be valid")
    }

    pub fn try_new(config: EngineConfig) -> Result<Self, ErrorCode> {
        validate_style_defaults(&config.style_defaults)?;
        let mut model = DocumentModel::default();
        let mut initial_config = snow_draw_engine_document::Transaction::new("runtime defaults");
        initial_config.update_watermark(config.style_defaults.watermark.clone());
        initial_config.update_spotlight(config.style_defaults.spotlight);
        model.apply_transaction(initial_config)?;
        let mut engine = Self {
            editor: EditorSession::new_with_style_defaults(
                ViewEngineConfig::default(),
                &config.style_defaults.editor,
            )?,
            config,
            model,
            history: HistoryStore::default(),
            session_config_seeded: false,
            scene_cache: DocumentSceneCache::default(),
            viewports: HashMap::new(),
            next_viewport_id: 0,
        };
        engine.scene_cache.sync(&engine.model, None);
        Ok(engine)
    }

    pub fn style_defaults(&self) -> &StyleDefaults {
        &self.config.style_defaults
    }

    pub fn clear_document_preserving_viewports(&mut self) -> Result<MutationResult, ErrorCode> {
        let view_config = self.editor.config();
        let quick_selection_disabled_tools = self.editor.quick_selection_disabled_tools();
        let mut replacement = Self::try_new(self.config.clone())?;
        replacement.editor.set_config(view_config)?;
        replacement
            .editor
            .set_quick_selection_disabled_tools(quick_selection_disabled_tools);
        self.model = replacement.model;
        self.history = HistoryStore::default();
        self.editor = replacement.editor;
        self.session_config_seeded = false;
        self.scene_cache = DocumentSceneCache::default();
        self.scene_cache.sync(&self.model, None);
        self.refresh_all_viewports()
    }

    pub fn clone_document_session(&self) -> Self {
        let mut engine = Self {
            config: self.config.clone(),
            model: self.model.clone(),
            history: self.history.clone(),
            editor: self.editor.clone(),
            session_config_seeded: self.session_config_seeded,
            scene_cache: DocumentSceneCache::default(),
            viewports: self
                .viewports
                .iter()
                .map(|(id, slot)| {
                    (
                        *id,
                        ViewportSlot {
                            view: slot.view,
                            composer: ViewportComposer::new(),
                        },
                    )
                })
                .collect(),
            next_viewport_id: self.next_viewport_id,
        };
        engine.scene_cache.sync(&engine.model, None);
        let _ = engine.refresh_all_viewports();
        engine
    }

    pub fn clone_document_session_with_config(
        &self,
        config: EngineConfig,
    ) -> Result<Self, ErrorCode> {
        validate_style_defaults(&config.style_defaults)?;
        let mut clone = self.clone_document_session();
        clone.config = config;
        Ok(clone)
    }

    pub fn viewport_active_tool(&self, id: ViewportId) -> Result<ActiveTool, ErrorCode> {
        self.ensure_viewport(id)?;
        Ok(self.editor.active_tool())
    }

    pub fn set_viewport_active_tool(
        &mut self,
        id: ViewportId,
        tool: ActiveTool,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        self.editor.set_active_tool(tool)?;
        self.refresh_after_session_mutation(before)
    }

    pub fn quick_selection_disabled_tools(&self) -> u64 {
        self.editor.quick_selection_disabled_tools()
    }

    pub fn set_quick_selection_disabled_tools(
        &mut self,
        tools: u64,
    ) -> Result<MutationResult, ErrorCode> {
        if self.editor.quick_selection_disabled_tools() == tools {
            return Ok(MutationResult::default());
        }
        self.editor.set_quick_selection_disabled_tools(tools);
        self.refresh_all_viewports()
    }

    pub fn viewport_snap_config(&self, id: ViewportId) -> Result<SnapConfig, ErrorCode> {
        self.ensure_viewport(id)?;
        Ok(self.editor.snap_config())
    }

    pub fn set_viewport_snap_config(
        &mut self,
        id: ViewportId,
        snap: SnapConfig,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        self.editor.set_snap_config(snap)?;
        self.refresh_after_session_mutation(before)
    }

    pub fn viewport_grid_config(&self, id: ViewportId) -> Result<GridConfig, ErrorCode> {
        self.ensure_viewport(id)?;
        Ok(self.editor.grid_config())
    }

    pub fn set_viewport_grid_config(
        &mut self,
        id: ViewportId,
        grid: GridConfig,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        self.editor.set_grid_config(grid)?;
        self.refresh_after_session_mutation(before)
    }

    pub fn viewport_style_toolbar_state(
        &self,
        id: ViewportId,
    ) -> Result<StyleToolbarState, ErrorCode> {
        self.ensure_viewport(id)?;
        Ok(StyleToolbarState {
            source: self.editor.style_toolbar_source(&self.model),
            shape_style: self.editor.shape_style(&self.model),
            text_style: self.editor.text_style(&self.model),
            serial_number_style: self.editor.serial_number_style(&self.model),
            text_style_mixed: self.editor.text_style_mixed(&self.model),
            serial_number_style_mixed: self.editor.serial_number_style_mixed(&self.model),
            shape_style_mixed: self.editor.shape_style_mixed(&self.model),
            filter_style: self.editor.filter_style(&self.model),
            filter_style_mixed: self.editor.filter_style_mixed(&self.model),
        })
    }

    pub fn viewport_serial_number_toolbar_state(
        &self,
        id: ViewportId,
    ) -> Result<SerialNumberToolbarState, ErrorCode> {
        let slot = self.viewport_slot(id)?;
        Ok(self
            .editor
            .serial_number_toolbar_state(&self.model, &slot.view))
    }

    pub fn viewport_rectangle_shape_style(
        &self,
        id: ViewportId,
    ) -> Result<RectangleShapeStyle, ErrorCode> {
        self.ensure_viewport(id)?;
        Ok(self.editor.rectangle_shape_style(&self.model))
    }

    pub fn viewport_arrow_style(&self, id: ViewportId) -> Result<ArrowStyle, ErrorCode> {
        self.ensure_viewport(id)?;
        Ok(self.editor.arrow_style(&self.model))
    }

    pub fn set_viewport_shape_style_patch(
        &mut self,
        id: ViewportId,
        patch: ShapeStylePatch,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        let command = self.editor.set_shape_style_patch(&self.model, patch)?;
        if let Some(command) = command {
            self.apply_editor_command(id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn set_viewport_filter_style(
        &mut self,
        id: ViewportId,
        style: FilterStyle,
        properties: u32,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        let command = self
            .editor
            .set_filter_style(&self.model, style, properties)?;
        if let Some(command) = command {
            self.apply_editor_command(id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn watermark_config(&self) -> &WatermarkConfig {
        self.model.watermark_config()
    }

    pub fn set_viewport_watermark_config(
        &mut self,
        id: ViewportId,
        config: WatermarkConfig,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        if let Some(command) = self.editor.set_watermark_config(&self.model, config)? {
            self.apply_editor_command(id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn spotlight_config(&self) -> SpotlightConfig {
        self.model.spotlight_config()
    }

    pub fn set_viewport_spotlight_config(
        &mut self,
        id: ViewportId,
        config: SpotlightConfig,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        if let Some(command) = self.editor.set_spotlight_config(&self.model, config)? {
            self.apply_editor_command(id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn set_viewport_rectangle_shape_style(
        &mut self,
        id: ViewportId,
        style: RectangleShapeStyle,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        let command = self.editor.set_rectangle_shape_style(&self.model, style)?;
        if let Some(command) = command {
            self.apply_editor_command(id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }
}

fn validate_style_defaults(defaults: &StyleDefaults) -> Result<(), ErrorCode> {
    snow_draw_engine_editor::validate_editor_style_defaults(&defaults.editor)?;
    snow_draw_engine_document::validate_watermark_config(&defaults.watermark)?;
    snow_draw_engine_document::validate_spotlight_config(&defaults.spotlight)?;
    if !(0.0..=1.0).contains(&defaults.watermark.opacity)
        || defaults.watermark.gap <= 0.0
        || !(0.0..=1.0).contains(&defaults.spotlight.opacity)
        || defaults.watermark.text.trim() != defaults.watermark.text
        || defaults.watermark.font_family.trim() != defaults.watermark.font_family
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{ColorRgba8, CornerRadii};
    use snow_draw_engine_document::{CanvasFilterType, FillStyle};
    use snow_draw_engine_editor::FILTER_STYLE_PROPERTY_ALL;

    fn custom_config(seed: u8) -> EngineConfig {
        let mut defaults = StyleDefaults::default();
        let color = ColorRgba8 {
            r: seed,
            g: seed.wrapping_add(1),
            b: seed.wrapping_add(2),
            a: 0x80,
        };
        defaults.editor.rectangle.fill = color;
        defaults.editor.rectangle.fill_style = FillStyle::CrossLine;
        defaults.editor.rectangle.stroke_width = 3.0;
        defaults.editor.rectangle.corner_radii = CornerRadii::splat(4.0);
        defaults.editor.arrow.stroke = color;
        defaults.editor.arrow.stroke_width = 4.0;
        defaults.editor.line.stroke = color;
        defaults.editor.line.stroke_width = 5.0;
        defaults.editor.line.opacity = 0.91;
        defaults.editor.free_draw.stroke = color;
        defaults.editor.free_draw.stroke_width = 6.0;
        defaults.editor.free_draw.opacity = 0.81;
        defaults.editor.rectangle_highlight.fill = color;
        defaults.editor.rectangle_highlight.stroke_width = 7.0;
        defaults.editor.rectangle_highlight.opacity = 0.71;
        defaults.editor.pen_highlight.stroke = color;
        defaults.editor.pen_highlight.stroke_width = 8.0;
        defaults.editor.pen_highlight.opacity = 0.61;
        defaults.editor.rectangle_filter = FilterStyle {
            filter_type: CanvasFilterType::GaussianBlur,
            strength: 0.42,
            opacity: 0.52,
            stroke_width: 9.0,
        };
        defaults.editor.pen_filter = FilterStyle {
            filter_type: CanvasFilterType::Inversion,
            strength: 0.43,
            opacity: 0.53,
            stroke_width: 10.0,
        };
        defaults.editor.text.color = color;
        defaults.editor.text.font_size = 31.0;
        defaults.editor.text.font_family = Some(format!("Text {seed}"));
        defaults.editor.text.opacity = 0.72;
        defaults.editor.serial_number.number = i64::from(seed);
        defaults.editor.serial_number.color = color;
        defaults.editor.serial_number.font_size = 25.0;
        defaults.editor.serial_number.font_family = Some(format!("Serial {seed}"));
        defaults.editor.serial_number.opacity = 0.73;
        defaults.watermark.color = color;
        defaults.watermark.text = format!("Watermark{seed}");
        defaults.watermark.font_size = 13.0;
        defaults.watermark.font_family = format!("Watermark {seed}");
        defaults.watermark.angle = 21.0;
        defaults.watermark.gap = 44.0;
        defaults.watermark.opacity = 0.23;
        defaults.spotlight.color = color;
        defaults.spotlight.opacity = 0.63;
        EngineConfig {
            style_defaults: defaults,
        }
    }

    fn assert_editor_defaults(
        engine: &mut Engine,
        viewport: ViewportId,
        expected: &EditorStyleDefaults,
    ) {
        assert_eq!(
            engine.viewport_rectangle_shape_style(viewport).unwrap(),
            expected.rectangle
        );
        assert_eq!(
            engine.viewport_arrow_style(viewport).unwrap(),
            expected.arrow
        );

        for (tool, style) in [
            (ActiveTool::Line, expected.line),
            (ActiveTool::FreeDraw, expected.free_draw),
            (ActiveTool::RectangleHighlight, expected.rectangle_highlight),
            (ActiveTool::PenHighlight, expected.pen_highlight),
        ] {
            engine.set_viewport_active_tool(viewport, tool).unwrap();
            assert_eq!(
                engine
                    .viewport_style_toolbar_state(viewport)
                    .unwrap()
                    .shape_style,
                style
            );
        }

        for (tool, style) in [
            (ActiveTool::RectangleFilter, expected.rectangle_filter),
            (ActiveTool::PenFilter, expected.pen_filter),
        ] {
            engine.set_viewport_active_tool(viewport, tool).unwrap();
            assert_eq!(
                engine
                    .viewport_style_toolbar_state(viewport)
                    .unwrap()
                    .filter_style,
                style
            );
        }

        engine
            .set_viewport_active_tool(viewport, ActiveTool::Text)
            .unwrap();
        assert_eq!(
            engine
                .viewport_style_toolbar_state(viewport)
                .unwrap()
                .text_style,
            expected.text
        );
        engine
            .set_viewport_active_tool(viewport, ActiveTool::SerialNumber)
            .unwrap();
        assert_eq!(
            engine
                .viewport_style_toolbar_state(viewport)
                .unwrap()
                .serial_number_style,
            expected.serial_number
        );
    }

    #[test]
    fn configured_profile_is_authoritative_for_every_tool_and_reset() {
        let config = custom_config(17);
        let mut engine = Engine::try_new(config.clone()).unwrap();
        assert_eq!(engine.style_defaults(), &config.style_defaults);
        assert_eq!(engine.watermark_config(), &config.style_defaults.watermark);
        assert_eq!(engine.spotlight_config(), config.style_defaults.spotlight);

        let viewport = engine
            .create_viewport(ViewportConfig {
                engine: ViewEngineConfig {
                    snap: SnapConfig {
                        enabled: true,
                        ..SnapConfig::default()
                    },
                    ..ViewEngineConfig::default()
                },
            })
            .unwrap();
        assert_editor_defaults(&mut engine, viewport, &config.style_defaults.editor);

        let mut changed_rectangle = config.style_defaults.editor.rectangle;
        changed_rectangle.stroke_width = 22.0;
        engine
            .set_viewport_rectangle_shape_style(viewport, changed_rectangle)
            .unwrap();
        let mut changed_filter = config.style_defaults.editor.pen_filter;
        changed_filter.stroke_width = 33.0;
        engine
            .set_viewport_active_tool(viewport, ActiveTool::PenFilter)
            .unwrap();
        engine
            .set_viewport_filter_style(viewport, changed_filter, FILTER_STYLE_PROPERTY_ALL)
            .unwrap();
        engine
            .set_viewport_watermark_config(viewport, WatermarkConfig::default())
            .unwrap();

        engine.clear_document_preserving_viewports().unwrap();
        assert_editor_defaults(&mut engine, viewport, &config.style_defaults.editor);
        assert_eq!(engine.watermark_config(), &config.style_defaults.watermark);
        assert_eq!(engine.spotlight_config(), config.style_defaults.spotlight);
    }

    #[test]
    fn invalid_profiles_are_rejected_without_filter_role_ambiguity() {
        let mut config = custom_config(23);
        config.style_defaults.editor.rectangle_filter.stroke_width = 0.0;
        config.style_defaults.editor.pen_filter.stroke_width = 0.0;
        assert_eq!(
            Engine::try_new(config).unwrap_err(),
            ErrorCode::InvalidArgument
        );

        let mut config = custom_config(24);
        config.style_defaults.editor.rectangle_filter.stroke_width = 0.0;
        assert!(Engine::try_new(config).is_ok());

        let mut config = custom_config(25);
        config.style_defaults.editor.text.font_size = f64::NAN;
        assert_eq!(
            Engine::try_new(config).unwrap_err(),
            ErrorCode::InvalidArgument
        );

        let mut config = custom_config(26);
        config.style_defaults.watermark.opacity = 1.1;
        assert_eq!(
            Engine::try_new(config).unwrap_err(),
            ErrorCode::InvalidArgument
        );

        let mut config = custom_config(27);
        config.style_defaults.spotlight.opacity = -0.1;
        assert_eq!(
            Engine::try_new(config).unwrap_err(),
            ErrorCode::InvalidArgument
        );
    }

    #[test]
    fn restore_and_clone_keep_current_state_but_use_target_reset_profile() {
        let source_config = custom_config(31);
        let target_config = custom_config(71);
        let mut source = Engine::try_new(source_config).unwrap();
        let source_viewport = source.create_viewport(ViewportConfig::default()).unwrap();
        let mut edited_rectangle = source
            .viewport_rectangle_shape_style(source_viewport)
            .unwrap();
        edited_rectangle.stroke_width = 41.0;
        source
            .set_viewport_rectangle_shape_style(source_viewport, edited_rectangle)
            .unwrap();
        let mut edited_watermark = source.watermark_config().clone();
        edited_watermark.text = "Edited".to_owned();
        source
            .set_viewport_watermark_config(source_viewport, edited_watermark.clone())
            .unwrap();

        let session = source.serialize_document_session().unwrap();
        let history = source.serialize_document_history().unwrap();

        let mut full =
            Engine::from_serialized_document_session_with_config(&session, target_config.clone())
                .unwrap();
        let full_viewport = full.create_viewport(ViewportConfig::default()).unwrap();
        assert_eq!(
            full.viewport_rectangle_shape_style(full_viewport).unwrap(),
            edited_rectangle
        );
        assert_eq!(full.watermark_config(), &edited_watermark);
        assert_eq!(full.style_defaults(), &target_config.style_defaults);
        full.clear_document_preserving_viewports().unwrap();
        assert_editor_defaults(
            &mut full,
            full_viewport,
            &target_config.style_defaults.editor,
        );
        assert_eq!(
            full.watermark_config(),
            &target_config.style_defaults.watermark
        );

        let mut history_only =
            Engine::from_serialized_document_history_with_config(&history, target_config.clone())
                .unwrap();
        let history_viewport = history_only
            .create_viewport(ViewportConfig::default())
            .unwrap();
        assert_editor_defaults(
            &mut history_only,
            history_viewport,
            &target_config.style_defaults.editor,
        );
        assert_eq!(history_only.watermark_config(), &edited_watermark);
        history_only.clear_document_preserving_viewports().unwrap();
        assert_eq!(
            history_only.watermark_config(),
            &target_config.style_defaults.watermark
        );

        let mut cloned = source
            .clone_document_session_with_config(target_config.clone())
            .unwrap();
        assert_eq!(
            cloned
                .viewport_rectangle_shape_style(source_viewport)
                .unwrap(),
            edited_rectangle
        );
        assert_eq!(cloned.watermark_config(), &edited_watermark);
        assert_eq!(cloned.style_defaults(), &target_config.style_defaults);
        cloned.clear_document_preserving_viewports().unwrap();
        assert_editor_defaults(
            &mut cloned,
            source_viewport,
            &target_config.style_defaults.editor,
        );
        assert_eq!(
            cloned.watermark_config(),
            &target_config.style_defaults.watermark
        );
    }
}

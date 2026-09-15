use super::*;

impl Engine {
    pub fn reset_editing_state_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        self.editor.reset_editing_state();
        self.refresh_after_session_mutation(before)
    }

    pub fn select_element_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        id: ElementId,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        self.editor.select_element(&self.model, id)?;
        self.refresh_after_session_mutation(before)
    }

    pub fn selected_ids(&self) -> Vec<ElementId> {
        self.editor.selected_ids()
    }

    pub fn delete_selected_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let command = self.editor.delete_selected(&self.model)?;
        if let Some(command) = command {
            self.apply_editor_command(source_viewport_id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn delete_all_elements_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let ids: Vec<ElementId> = self.model.paint_order().to_vec();
        let command = self
            .editor
            .delete_elements(&self.model, &ids, "delete all elements")?;
        if let Some(command) = command {
            self.apply_editor_command(source_viewport_id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn duplicate_selected_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        offset: Point<f64>,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let command = self.editor.duplicate_selected(&self.model, offset)?;
        if let Some(command) = command {
            self.apply_editor_command(source_viewport_id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn reorder_selected_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        action: u32,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let command = self.editor.reorder_selected(&self.model, action)?;
        if let Some(command) = command {
            self.apply_editor_command(source_viewport_id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn set_selected_opacity_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        opacity: f64,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let command = self.editor.set_selected_opacity(&self.model, opacity)?;
        if let Some(command) = command {
            self.apply_editor_command(source_viewport_id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn adjust_selected_serial_numbers_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        delta: i64,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let command = self
            .editor
            .adjust_selected_serial_numbers(&self.model, delta)?;
        if let Some(command) = command {
            self.apply_editor_command(source_viewport_id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }
}

impl Engine {
    pub fn auto_filter_regions(
        &self,
    ) -> Option<&snow_draw_engine_document::AutoFilterRegionRecord> {
        self.model.auto_filter_regions()
    }
    pub fn auto_filter_generation(&self) -> u64 {
        self.model.auto_filter_generation()
    }
    pub fn set_auto_filter_regions(
        &mut self,
        viewport: ViewportId,
        record: Option<snow_draw_engine_document::AutoFilterRegionRecord>,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(viewport)?;
        let transaction = self.model.auto_filter_record_transaction(record)?;
        self.apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
    }
    pub fn fill_auto_filter_category(
        &mut self,
        viewport: ViewportId,
        category: &str,
    ) -> Result<MutationResult, ErrorCode> {
        let style = self.viewport_style_toolbar_state(viewport)?.filter_style;
        let ids: Vec<_> = self
            .model
            .auto_filter_regions()
            .map(|r| {
                r.regions
                    .iter()
                    .filter(|r| r.category == category)
                    .map(|r| r.id)
                    .collect()
            })
            .unwrap_or_default();
        let transaction =
            self.model
                .auto_filter_fill_transaction(&ids, style.filter_type, style.strength, false);
        if transaction.is_empty() {
            return Ok(MutationResult::default());
        }
        self.apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
    }
}

#[cfg(test)]
mod delete_all_elements_tests {
    use super::*;
    use snow_draw_engine_core::{ColorRgba8, CornerRadii};
    use snow_draw_engine_document::{
        ElementMeta, FillStyle, RectangleData, RectangleElementKind, StrokeStyle, Transaction,
        WatermarkConfig, WatermarkTemplateApplicationTime,
    };

    fn rectangle() -> RectangleData {
        RectangleData {
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(0.0, 0.0),
            width: 40.0,
            height: 20.0,
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

    fn engine_with_elements(count: usize) -> (Engine, ViewportId) {
        let mut engine = Engine::default();
        let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
        for _ in 0..count {
            let id = engine.model.peek_next_element_id();
            let mut transaction = Transaction::new("create rectangle");
            transaction.insert_rectangle(id, ElementMeta::default(), rectangle());
            engine
                .commit_transaction(
                    viewport,
                    ApplyTransactionCommand {
                        transaction,
                        history_undo_snapshot: None,
                    },
                )
                .unwrap();
        }
        (engine, viewport)
    }

    #[test]
    fn delete_all_elements_is_a_single_undoable_history_entry() {
        let (mut engine, viewport) = engine_with_elements(3);
        let ids: Vec<ElementId> = engine.model.paint_order().to_vec();
        engine.editor.select_element(&engine.model, ids[0]).unwrap();

        engine
            .delete_all_elements_with_viewport_changes(viewport)
            .unwrap();

        assert!(engine.model.paint_order().is_empty());
        assert!(engine.selected_ids().is_empty());
        assert!(engine.history_state().can_undo);

        // One undo must restore every element, unlike clear_document which
        // destroys the history alongside the elements.
        engine.undo().unwrap();
        assert_eq!(engine.model.paint_order(), &ids[..]);

        engine.redo().unwrap();
        assert!(engine.model.paint_order().is_empty());
    }

    #[test]
    fn delete_all_elements_preserves_document_wide_configuration() {
        let (mut engine, viewport) = engine_with_elements(1);
        let mut watermark = engine.watermark_config().clone();
        watermark.text = "kept".to_owned();
        engine
            .set_viewport_watermark_config(viewport, watermark.clone())
            .unwrap();

        engine
            .delete_all_elements_with_viewport_changes(viewport)
            .unwrap();

        assert_eq!(engine.watermark_config(), &watermark);
    }

    #[test]
    fn watermark_template_value_and_application_time_undo_and_redo_together() {
        let (mut engine, viewport) = engine_with_elements(0);
        let first = WatermarkConfig {
            text: "draft".to_owned(),
            template_value: "{text}-{YYYY}".to_owned(),
            template_application_time: Some(WatermarkTemplateApplicationTime {
                year: 2025,
                month: 1,
                day: 2,
                hour: 3,
                minute: 4,
                second: 5,
            }),
            ..WatermarkConfig::default()
        };
        let second = WatermarkConfig {
            template_value: "{text}-{YYYY-MM-DD_HH-mm-ss}".to_owned(),
            template_application_time: Some(WatermarkTemplateApplicationTime {
                year: 2026,
                month: 9,
                day: 15,
                hour: 12,
                minute: 34,
                second: 56,
            }),
            ..first.clone()
        };

        engine
            .set_viewport_watermark_config(viewport, first.clone())
            .unwrap();
        engine
            .set_viewport_watermark_config(viewport, second.clone())
            .unwrap();
        assert_eq!(engine.watermark_config(), &second);
        engine.undo().unwrap();
        assert_eq!(engine.watermark_config(), &first);
        engine.redo().unwrap();
        assert_eq!(engine.watermark_config(), &second);
    }

    #[test]
    fn delete_all_elements_on_an_empty_document_commits_no_history() {
        let (mut engine, viewport) = engine_with_elements(0);
        engine
            .delete_all_elements_with_viewport_changes(viewport)
            .unwrap();
        assert!(!engine.history_state().can_undo);
        assert!(!engine.history_state().can_redo);
    }
}

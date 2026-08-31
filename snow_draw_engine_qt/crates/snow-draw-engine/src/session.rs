use serde::{Deserialize, Serialize};
use snow_draw_engine_core::ErrorCode;
use snow_draw_engine_document::Document;
use snow_draw_engine_editor::PersistedEditorSession;
use snow_draw_engine_model::DocumentModel;

use crate::engine::EngineConfig as RuntimeEngineConfig;
use crate::{Engine, history::HistoryStore};

pub const DOCUMENT_SESSION_SCHEMA_VERSION: u32 = 1;
pub const DOCUMENT_HISTORY_SCHEMA_VERSION: u32 = 1;
pub const MAX_DOCUMENT_SESSION_BYTES: usize = 16 * 1024 * 1024;

#[derive(Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct DocumentSession {
    schema_version: u32,
    document: Document,
    history: HistoryStore,
    editor: PersistedEditorSession,
    session_config_seeded: bool,
}

#[derive(Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct DocumentHistory {
    schema_version: u32,
    document: Document,
    history: HistoryStore,
}

impl Engine {
    pub fn serialize_document_session(&self) -> Result<Vec<u8>, ErrorCode> {
        let session = DocumentSession {
            schema_version: DOCUMENT_SESSION_SCHEMA_VERSION,
            document: self.model.document().clone(),
            history: self.history.clone(),
            editor: self.editor.persisted(),
            session_config_seeded: self.session_config_seeded,
        };
        let bytes = serde_json::to_vec(&session).map_err(|_| ErrorCode::Internal)?;
        if bytes.len() > MAX_DOCUMENT_SESSION_BYTES {
            return Err(ErrorCode::InvalidState);
        }
        Ok(bytes)
    }

    #[cfg(test)]
    fn from_serialized_document_session(bytes: &[u8]) -> Result<Self, ErrorCode> {
        Self::from_serialized_document_session_with_config(bytes, RuntimeEngineConfig::default())
    }

    pub fn from_serialized_document_session_with_config(
        bytes: &[u8],
        config: RuntimeEngineConfig,
    ) -> Result<Self, ErrorCode> {
        if bytes.is_empty() || bytes.len() > MAX_DOCUMENT_SESSION_BYTES {
            return Err(ErrorCode::InvalidArgument);
        }
        let session: DocumentSession =
            serde_json::from_slice(bytes).map_err(|_| ErrorCode::InvalidArgument)?;
        if session.schema_version != DOCUMENT_SESSION_SCHEMA_VERSION {
            return Err(ErrorCode::Unsupported);
        }

        let model = DocumentModel::from_document(session.document)?;
        session.history.validate_session(&model)?;
        let editor = snow_draw_engine_editor::EditorSession::from_persisted(session.editor)?;
        let mut engine = Self::try_new(config)?;
        engine.model = model;
        engine.history = session.history;
        engine.editor = editor;
        engine.session_config_seeded = session.session_config_seeded;
        engine.scene_cache = Default::default();
        engine.scene_cache.sync(&engine.model, None);
        Ok(engine)
    }

    pub fn serialize_document_history(&self) -> Result<Vec<u8>, ErrorCode> {
        let history = DocumentHistory {
            schema_version: DOCUMENT_HISTORY_SCHEMA_VERSION,
            document: self.model.document().clone(),
            history: self.history.clone(),
        };
        let bytes = serde_json::to_vec(&history).map_err(|_| ErrorCode::Internal)?;
        if bytes.len() > MAX_DOCUMENT_SESSION_BYTES {
            return Err(ErrorCode::InvalidState);
        }
        Ok(bytes)
    }

    #[cfg(test)]
    fn from_serialized_document_history(bytes: &[u8]) -> Result<Self, ErrorCode> {
        Self::from_serialized_document_history_with_config(bytes, RuntimeEngineConfig::default())
    }

    pub fn from_serialized_document_history_with_config(
        bytes: &[u8],
        config: RuntimeEngineConfig,
    ) -> Result<Self, ErrorCode> {
        if bytes.is_empty() || bytes.len() > MAX_DOCUMENT_SESSION_BYTES {
            return Err(ErrorCode::InvalidArgument);
        }
        let history: DocumentHistory =
            serde_json::from_slice(bytes).map_err(|_| ErrorCode::InvalidArgument)?;
        if history.schema_version != DOCUMENT_HISTORY_SCHEMA_VERSION {
            return Err(ErrorCode::Unsupported);
        }

        let model = DocumentModel::from_document(history.document)?;
        history.history.validate_session(&model)?;
        let mut engine = Self::try_new(config)?;
        engine.editor.reset_editing_state();
        engine.model = model;
        engine.history = history.history;
        engine.scene_cache.sync(&engine.model, None);
        Ok(engine)
    }

    pub fn restore_document_history_preserving_editor_styles(
        &mut self,
        bytes: &[u8],
    ) -> Result<crate::MutationResult, ErrorCode> {
        let replacement =
            Self::from_serialized_document_history_with_config(bytes, self.config.clone())?;
        self.model = replacement.model;
        self.history = replacement.history;
        self.editor.reset_editing_state();
        self.scene_cache = Default::default();
        self.scene_cache.sync(&self.model, None);
        self.refresh_all_viewports()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{ColorRgba8, CornerRadii, Point};
    use snow_draw_engine_document::{
        ElementMeta, FillStyle, HighlightShape, RectangleData, RectangleElementKind, StrokeStyle,
        Transaction,
    };
    use snow_draw_engine_editor::ActiveTool;

    #[test]
    fn document_session_round_trips_and_rejects_invalid_payloads() {
        let mut engine = Engine::new(RuntimeEngineConfig::default());
        let id = engine.model.peek_next_element_id();
        let mut transaction = Transaction::new("rectangle");
        let rectangle = RectangleData {
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: HighlightShape::Rectangle,
            center: Point::new(24.0, 32.0),
            width: 80.0,
            height: 60.0,
            rotation: 0.0,
            fill: ColorRgba8 {
                r: 1,
                g: 2,
                b: 3,
                a: 255,
            },
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 1.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        transaction.insert_rectangle(id, ElementMeta::default(), rectangle);
        let first_redo = transaction.clone();
        engine.model.apply_transaction(transaction).unwrap();
        let first_snapshot = engine.editor.capture_document_sync_snapshot(&engine.model);
        let mut first_undo = Transaction::new("remove rectangle");
        first_undo.remove_element(id);
        engine.history.push_committed(
            "rectangle",
            first_redo,
            first_undo,
            first_snapshot.clone(),
            first_snapshot,
        );

        let second_id = engine.model.peek_next_element_id();
        let mut second_redo = Transaction::new("second rectangle");
        second_redo.insert_rectangle(
            second_id,
            ElementMeta::default(),
            RectangleData {
                center: Point::new(140.0, 120.0),
                ..rectangle
            },
        );
        engine.model.apply_transaction(second_redo.clone()).unwrap();
        let second_snapshot = engine.editor.capture_document_sync_snapshot(&engine.model);
        let mut second_undo = Transaction::new("remove second rectangle");
        second_undo.remove_element(second_id);
        engine.history.push_committed(
            "second rectangle",
            second_redo,
            second_undo,
            second_snapshot.clone(),
            second_snapshot,
        );
        engine.history.undo(&mut engine.model).unwrap();

        let bytes = engine.serialize_document_session().unwrap();
        let restored = Engine::from_serialized_document_session(&bytes).unwrap();
        assert_eq!(restored.model.document(), engine.model.document());
        assert_eq!(restored.history, engine.history);
        assert_eq!(restored.editor.persisted(), engine.editor.persisted());
        assert!(Engine::from_serialized_document_session(&bytes[..bytes.len() - 1]).is_err());

        let mut unsupported: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        unsupported["schemaVersion"] = serde_json::json!(999);
        assert!(
            Engine::from_serialized_document_session(&serde_json::to_vec(&unsupported).unwrap())
                .is_err()
        );

        let mut invalid_history: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        invalid_history["history"]["undoStack"][0]["redo"]["operations"] = serde_json::json!([]);
        assert!(
            Engine::from_serialized_document_session(
                &serde_json::to_vec(&invalid_history).unwrap()
            )
            .is_err()
        );

        let oversized = vec![b' '; MAX_DOCUMENT_SESSION_BYTES + 1];
        assert!(Engine::from_serialized_document_session(&oversized).is_err());
    }

    #[test]
    fn document_history_contains_only_elements_and_history() {
        let mut engine = Engine::new(RuntimeEngineConfig::default());
        let id = engine.model.peek_next_element_id();
        let mut transaction = Transaction::new("rectangle");
        let rectangle = RectangleData {
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: HighlightShape::Rectangle,
            center: Point::new(24.0, 32.0),
            width: 80.0,
            height: 60.0,
            rotation: 0.0,
            fill: ColorRgba8 {
                r: 1,
                g: 2,
                b: 3,
                a: 255,
            },
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 1.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        transaction.insert_rectangle(id, ElementMeta::default(), rectangle);
        let redo = transaction.clone();
        engine.model.apply_transaction(transaction).unwrap();
        let snapshot = engine.editor.capture_document_sync_snapshot(&engine.model);
        let mut undo = Transaction::new("remove rectangle");
        undo.remove_element(id);
        engine
            .history
            .push_committed("rectangle", redo, undo, snapshot.clone(), snapshot);
        engine.editor.set_active_tool(ActiveTool::Shape).unwrap();
        engine.editor.select_element(&engine.model, id).unwrap();
        engine.session_config_seeded = true;

        let bytes = engine.serialize_document_history().unwrap();
        let value: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        let keys = value
            .as_object()
            .unwrap()
            .keys()
            .cloned()
            .collect::<Vec<_>>();
        assert_eq!(keys, ["document", "history", "schemaVersion"]);

        let restored = Engine::from_serialized_document_history(&bytes).unwrap();
        assert_eq!(restored.model.document(), engine.model.document());
        assert_eq!(restored.history, engine.history);
        assert_eq!(restored.editor.active_tool(), ActiveTool::Select);
        assert!(restored.editor.selected_ids().is_empty());
        assert!(!restored.session_config_seeded);

        let mut unsupported = value;
        unsupported["schemaVersion"] = serde_json::json!(999);
        assert!(
            Engine::from_serialized_document_history(&serde_json::to_vec(&unsupported).unwrap())
                .is_err()
        );
    }

    #[test]
    fn restoring_document_history_preserves_editor_styles_and_resets_transient_state() {
        let mut source = Engine::new(RuntimeEngineConfig::default());
        let source_viewport = source.create_viewport(Default::default()).unwrap();
        let mut source_style = source
            .viewport_rectangle_shape_style(source_viewport)
            .unwrap();
        source_style.stroke_width = 13.0;
        source
            .set_viewport_rectangle_shape_style(source_viewport, source_style)
            .unwrap();
        let history = source.serialize_document_history().unwrap();

        let mut target = Engine::new(RuntimeEngineConfig::default());
        let target_viewport = target.create_viewport(Default::default()).unwrap();
        let mut shared_style = target
            .viewport_rectangle_shape_style(target_viewport)
            .unwrap();
        shared_style.stroke_width = 7.0;
        target
            .set_viewport_rectangle_shape_style(target_viewport, shared_style)
            .unwrap();
        target
            .set_viewport_active_tool(target_viewport, ActiveTool::Shape)
            .unwrap();

        target
            .restore_document_history_preserving_editor_styles(&history)
            .unwrap();

        assert_eq!(
            target
                .viewport_rectangle_shape_style(target_viewport)
                .unwrap(),
            shared_style
        );
        assert_eq!(
            target.viewport_active_tool(target_viewport).unwrap(),
            ActiveTool::Select
        );
    }
}

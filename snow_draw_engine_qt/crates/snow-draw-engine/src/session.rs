use serde::{Deserialize, Serialize};
use snow_draw_engine_core::ErrorCode;
use snow_draw_engine_document::Document;
use snow_draw_engine_editor::PersistedEditorSession;
use snow_draw_engine_model::DocumentModel;

use crate::engine::EngineConfig as RuntimeEngineConfig;
use crate::{Engine, history::HistoryStore};

pub const DOCUMENT_SESSION_SCHEMA_VERSION: u32 = 5;
pub const DOCUMENT_HISTORY_SCHEMA_VERSION: u32 = 5;
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

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct DocumentSessionRef<'a> {
    schema_version: u32,
    document: &'a Document,
    history: &'a HistoryStore,
    editor: PersistedEditorSession,
    session_config_seeded: bool,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct DocumentHistoryRef<'a> {
    schema_version: u32,
    document: &'a Document,
    history: &'a HistoryStore,
}

#[derive(Default)]
struct BoundedSessionWriter {
    bytes: Vec<u8>,
    exceeded: bool,
}

impl std::io::Write for BoundedSessionWriter {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        if bytes.len() > MAX_DOCUMENT_SESSION_BYTES.saturating_sub(self.bytes.len()) {
            self.exceeded = true;
            return Err(std::io::Error::other("document session capacity exceeded"));
        }
        let required = self.bytes.len() + bytes.len();
        if required > self.bytes.capacity() {
            let capacity = required
                .max(self.bytes.capacity().saturating_mul(2))
                .clamp(4096, MAX_DOCUMENT_SESSION_BYTES);
            self.bytes.reserve_exact(capacity - self.bytes.len());
        }
        self.bytes.extend_from_slice(bytes);
        Ok(bytes.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

fn serialize_bounded(value: &impl Serialize) -> Result<Vec<u8>, ErrorCode> {
    let mut output = BoundedSessionWriter::default();
    serde_json::to_writer(&mut output, value).map_err(|_| {
        if output.exceeded {
            ErrorCode::InvalidState
        } else {
            ErrorCode::Internal
        }
    })?;
    Ok(output.bytes)
}

impl Engine {
    pub fn serialize_selected_element_ids(&self) -> Result<Vec<u8>, ErrorCode> {
        serialize_bounded(&self.selected_ids())
    }

    pub fn serialize_selected_draw_template(&self) -> Result<Vec<u8>, ErrorCode> {
        let template = self.editor.selected_draw_template(&self.model)?;
        serialize_bounded(&template)
    }

    pub fn serialize_document_session(&self) -> Result<Vec<u8>, ErrorCode> {
        let session = DocumentSessionRef {
            schema_version: DOCUMENT_SESSION_SCHEMA_VERSION,
            document: self.model.document(),
            history: &self.history,
            editor: self.editor.persisted(),
            session_config_seeded: self.session_config_seeded,
        };
        serialize_bounded(&session)
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
        if !(1..=DOCUMENT_SESSION_SCHEMA_VERSION).contains(&session.schema_version) {
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
        let history = DocumentHistoryRef {
            schema_version: DOCUMENT_HISTORY_SCHEMA_VERSION,
            document: self.model.document(),
            history: &self.history,
        };
        serialize_bounded(&history)
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
        if !(1..=DOCUMENT_HISTORY_SCHEMA_VERSION).contains(&history.schema_version) {
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
        CanvasFilterType, ElementMeta, FillStyle, FilterData, HighlightShape, RectangleData,
        RectangleElementKind, SerialNumberData, SerialNumberType, StrokeStyle, Transaction,
        WatermarkConfig, WatermarkTemplateApplicationTime,
    };
    use snow_draw_engine_editor::ActiveTool;

    #[test]
    fn serialization_rejects_growth_before_exceeding_capacity() {
        use std::io::Write;
        let mut writer = BoundedSessionWriter::default();
        let block = [b'x'; 4096];
        for _ in 0..MAX_DOCUMENT_SESSION_BYTES / block.len() {
            writer.write_all(&block).unwrap();
        }
        assert_eq!(writer.bytes.len(), MAX_DOCUMENT_SESSION_BYTES);
        assert!(writer.write_all(b"x").is_err());
        assert_eq!(writer.bytes.len(), MAX_DOCUMENT_SESSION_BYTES);
        assert!(writer.bytes.capacity() <= MAX_DOCUMENT_SESSION_BYTES);
        assert!(writer.exceeded);
        assert_eq!(
            serialize_bounded(&writer.bytes).unwrap_err(),
            ErrorCode::InvalidState
        );
        let mut irregular = BoundedSessionWriter::default();
        irregular
            .write_all(&writer.bytes[..MAX_DOCUMENT_SESSION_BYTES / 2 + 1])
            .unwrap();
        irregular.write_all(b"x").unwrap();
        assert!(irregular.bytes.capacity() <= MAX_DOCUMENT_SESSION_BYTES);
    }

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
        let legacy = DocumentSession {
            schema_version: DOCUMENT_SESSION_SCHEMA_VERSION,
            document: engine.model.document().clone(),
            history: engine.history.clone(),
            editor: engine.editor.persisted(),
            session_config_seeded: engine.session_config_seeded,
        };
        assert_eq!(bytes, serde_json::to_vec(&legacy).unwrap());
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
    fn emboss_document_and_editor_session_round_trip_preserves_filter_variants() {
        assert_eq!(CanvasFilterType::Emboss as u32, 4);
        let mut config = RuntimeEngineConfig::default();
        config.style_defaults.editor.rectangle_filter.filter_type = CanvasFilterType::Emboss;
        config.style_defaults.editor.rectangle_filter.strength = 0.75;
        config.style_defaults.editor.pen_filter.filter_type = CanvasFilterType::Emboss;
        config.style_defaults.editor.pen_filter.strength = 0.75;
        let mut engine = Engine::new(config);

        let emboss_id = engine.model.allocate_element_id();
        let mut transaction = Transaction::new("emboss filter");
        transaction.insert_filter(
            emboss_id,
            ElementMeta::default(),
            FilterData {
                filter_type: CanvasFilterType::Emboss,
                strength: 0.75,
                ..FilterData::default()
            },
        );
        engine.model.apply_transaction(transaction).unwrap();

        let bytes = engine.serialize_document_session().unwrap();
        let json: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        let serialized = json.to_string();
        assert!(serialized.matches("\"Emboss\"").count() >= 3);

        let restored = Engine::from_serialized_document_session(&bytes).unwrap();
        assert_eq!(
            restored.model.filter(emboss_id).unwrap().filter_type,
            CanvasFilterType::Emboss
        );
        assert_eq!(restored.editor.persisted(), engine.editor.persisted());

        for (filter_type, representation) in [
            (CanvasFilterType::Mosaic, "Mosaic"),
            (CanvasFilterType::GaussianBlur, "GaussianBlur"),
            (CanvasFilterType::Grayscale, "Grayscale"),
            (CanvasFilterType::Inversion, "Inversion"),
            (CanvasFilterType::Emboss, "Emboss"),
        ] {
            assert_eq!(
                serde_json::to_value(filter_type).unwrap(),
                serde_json::Value::String(representation.to_owned())
            );
        }
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

    fn remove_watermark_template_fields(value: &mut serde_json::Value) {
        match value {
            serde_json::Value::Object(object) => {
                object.remove("template_value");
                object.remove("template_application_time");
                for child in object.values_mut() {
                    remove_watermark_template_fields(child);
                }
            }
            serde_json::Value::Array(array) => {
                for child in array {
                    remove_watermark_template_fields(child);
                }
            }
            _ => {}
        }
    }

    fn remove_serial_number_type_fields(value: &mut serde_json::Value) {
        match value {
            serde_json::Value::Object(object) => {
                object.remove("type");
                for child in object.values_mut() {
                    remove_serial_number_type_fields(child);
                }
            }
            serde_json::Value::Array(array) => {
                for child in array {
                    remove_serial_number_type_fields(child);
                }
            }
            _ => {}
        }
    }

    #[test]
    fn serial_number_type_round_trips_and_legacy_payloads_default_to_outline_circle() {
        for serial_number_type in [
            SerialNumberType::OutlinedCircle,
            SerialNumberType::SolidCircle,
            SerialNumberType::OutlinedSquare,
            SerialNumberType::SolidSquare,
            SerialNumberType::Circle,
        ] {
            let mut typed_engine = Engine::new(RuntimeEngineConfig::default());
            let typed_id = typed_engine.model.peek_next_element_id();
            let mut typed_transaction = Transaction::new("typed serial number");
            typed_transaction.insert_serial_number(
                typed_id,
                ElementMeta::default(),
                SerialNumberData {
                    serial_number_type,
                    ..SerialNumberData::default()
                },
            );
            typed_engine
                .model
                .apply_transaction(typed_transaction)
                .unwrap();
            for (bytes, session_payload) in [
                (typed_engine.serialize_document_session().unwrap(), true),
                (typed_engine.serialize_document_history().unwrap(), false),
            ] {
                let restored = if session_payload {
                    Engine::from_serialized_document_session(&bytes).unwrap()
                } else {
                    Engine::from_serialized_document_history(&bytes).unwrap()
                };
                assert_eq!(
                    restored
                        .model
                        .serial_number(typed_id)
                        .unwrap()
                        .serial_number_type,
                    serial_number_type
                );
            }
        }

        let mut engine = Engine::new(RuntimeEngineConfig::default());
        let id = engine.model.peek_next_element_id();
        let mut transaction = Transaction::new("serial number");
        transaction.insert_serial_number(
            id,
            ElementMeta::default(),
            SerialNumberData {
                serial_number_type: SerialNumberType::SolidSquare,
                ..SerialNumberData::default()
            },
        );
        engine.model.apply_transaction(transaction).unwrap();

        let session = engine.serialize_document_session().unwrap();
        let history = engine.serialize_document_history().unwrap();
        assert_eq!(
            Engine::from_serialized_document_session(&session)
                .unwrap()
                .model
                .serial_number(id)
                .unwrap()
                .serial_number_type,
            SerialNumberType::SolidSquare
        );
        assert_eq!(
            Engine::from_serialized_document_history(&history)
                .unwrap()
                .model
                .serial_number(id)
                .unwrap()
                .serial_number_type,
            SerialNumberType::SolidSquare
        );

        for schema_version in 1..=4 {
            for (bytes, session_payload) in [(&session, true), (&history, false)] {
                let mut legacy: serde_json::Value = serde_json::from_slice(bytes).unwrap();
                legacy["schemaVersion"] = serde_json::json!(schema_version);
                remove_serial_number_type_fields(&mut legacy);
                let encoded = serde_json::to_vec(&legacy).unwrap();
                let restored = if session_payload {
                    Engine::from_serialized_document_session(&encoded).unwrap()
                } else {
                    Engine::from_serialized_document_history(&encoded).unwrap()
                };
                assert_eq!(
                    restored.model.serial_number(id).unwrap().serial_number_type,
                    SerialNumberType::OutlinedCircle
                );
            }
        }
    }

    #[test]
    fn watermark_templates_round_trip_and_legacy_payloads_use_plain_text() {
        let mut engine = Engine::new(RuntimeEngineConfig::default());
        let viewport = engine.create_viewport(Default::default()).unwrap();
        let expected = WatermarkConfig {
            text: "draft".to_owned(),
            template_value: "{text} {YYYY-MM-DD_HH-mm-ss}".to_owned(),
            template_application_time: Some(WatermarkTemplateApplicationTime {
                year: 2026,
                month: 9,
                day: 15,
                hour: 12,
                minute: 34,
                second: 56,
            }),
            ..WatermarkConfig::default()
        };
        engine
            .set_viewport_watermark_config(viewport, expected.clone())
            .unwrap();

        let session = engine.serialize_document_session().unwrap();
        let history = engine.serialize_document_history().unwrap();
        let session_json: serde_json::Value = serde_json::from_slice(&session).unwrap();
        let history_json: serde_json::Value = serde_json::from_slice(&history).unwrap();
        assert_eq!(session_json["schemaVersion"], 5);
        assert_eq!(history_json["schemaVersion"], 5);
        assert_eq!(
            Engine::from_serialized_document_session(&session)
                .unwrap()
                .watermark_config(),
            &expected
        );
        assert_eq!(
            Engine::from_serialized_document_history(&history)
                .unwrap()
                .watermark_config(),
            &expected
        );

        let legacy_source = Engine::new(RuntimeEngineConfig::default());
        for schema_version in 1..=3 {
            let mut legacy_session: serde_json::Value =
                serde_json::from_slice(&legacy_source.serialize_document_session().unwrap())
                    .unwrap();
            legacy_session["schemaVersion"] = serde_json::json!(schema_version);
            remove_watermark_template_fields(&mut legacy_session);
            let restored = Engine::from_serialized_document_session(
                &serde_json::to_vec(&legacy_session).unwrap(),
            )
            .unwrap();
            assert!(restored.watermark_config().template_value.is_empty());
            assert_eq!(restored.watermark_config().template_application_time, None);
            assert_eq!(
                restored.watermark_config().resolved_text(),
                restored.watermark_config().text
            );

            let mut legacy_history: serde_json::Value =
                serde_json::from_slice(&legacy_source.serialize_document_history().unwrap())
                    .unwrap();
            legacy_history["schemaVersion"] = serde_json::json!(schema_version);
            remove_watermark_template_fields(&mut legacy_history);
            let restored = Engine::from_serialized_document_history(
                &serde_json::to_vec(&legacy_history).unwrap(),
            )
            .unwrap();
            assert!(restored.watermark_config().template_value.is_empty());
            assert_eq!(restored.watermark_config().template_application_time, None);
        }
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

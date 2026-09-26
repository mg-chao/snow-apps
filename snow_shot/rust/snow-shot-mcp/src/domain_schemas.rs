//! Application-domain contracts. Qt validates registry keys and resource invariants.
use super::*;

macro_rules! input {
    ($name:ident { $($field:ident : $ty:ty),* $(,)? }) => {
        #[derive(Deserialize, JsonSchema)]
        #[serde(deny_unknown_fields)]
        struct $name { $($field: $ty),* }
    };
}
macro_rules! choices {
    ($name:ident { $($variant:ident),* $(,)? }) => {
        #[derive(Deserialize, JsonSchema)]
        #[serde(rename_all = "snake_case")]
        enum $name { $($variant),* }
    };
}
macro_rules! limited_integer {
    ($name:ident, $minimum:expr, $maximum:expr) => {
        struct $name(u32);
        impl<'de> Deserialize<'de> for $name {
            fn deserialize<D:serde::Deserializer<'de>>(deserializer:D)->Result<Self,D::Error> {
                let value=u32::deserialize(deserializer)?;
                if ($minimum..=$maximum).contains(&value) {Ok(Self(value))}
                else {Err(serde::de::Error::custom(concat!(stringify!($name)," is outside the supported range")))}
            }
        }
        impl JsonSchema for $name {
            fn schema_name()->std::borrow::Cow<'static,str>{ stringify!($name).into() }
            fn json_schema(_: &mut schemars::SchemaGenerator)->schemars::Schema {
                schemars::json_schema!({"type":"integer","minimum":$minimum,"maximum":$maximum})
            }
        }
    };
}
macro_rules! integer_choices {
    ($name:ident, [$($allowed:expr),*]) => {
        struct $name(u32);
        impl<'de> Deserialize<'de> for $name {
            fn deserialize<D:serde::Deserializer<'de>>(deserializer:D)->Result<Self,D::Error> {
                let value=u32::deserialize(deserializer)?;
                if [$($allowed),*].contains(&value) {Ok(Self(value))}
                else {Err(serde::de::Error::custom(concat!("Unsupported ", stringify!($name))))}
            }
        }
        impl JsonSchema for $name {
            fn schema_name()->std::borrow::Cow<'static,str>{ stringify!($name).into() }
            fn json_schema(_: &mut schemars::SchemaGenerator)->schemars::Schema {
                schemars::json_schema!({"type":"integer","enum":[$($allowed),*]})
            }
        }
    };
}
limited_integer!(DelaySeconds, 0, 10);
limited_integer!(HistoryPageSize, 1, 200);
limited_integer!(TrailDuration, 100, 2000);
limited_integer!(KeyboardSize, 32, 128);
limited_integer!(Percentage, 0, 100);
limited_integer!(ArtifactChunkSize, 1, 262144);
limited_integer!(ExportQuality, 1, 100);
integer_choices!(FrameRate, [5, 10, 15, 24, 30, 60, 120, 83]);
integer_choices!(AnimatedFrameRate, [5, 10, 15, 24]);
struct DocumentScale(f64);
impl<'de> Deserialize<'de> for DocumentScale {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let value = f64::deserialize(deserializer)?;
        if (0.1..=4.0).contains(&value) {
            Ok(Self(value))
        } else {
            Err(serde::de::Error::custom(
                "Document scale must be between 0.1 and 4",
            ))
        }
    }
}
impl JsonSchema for DocumentScale {
    fn schema_name() -> std::borrow::Cow<'static, str> {
        "DocumentScale".into()
    }
    fn json_schema(_: &mut schemars::SchemaGenerator) -> schemars::Schema {
        schemars::json_schema!({"type":"number","minimum":0.1,"maximum":4})
    }
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Revision<T> {
    expected_revision: u64,
    idempotency_key: Option<String>,
    #[serde(flatten)]
    options: T,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct DocumentMutation<T> {
    document_id: String,
    expected_revision: u64,
    idempotency_key: Option<String>,
    #[serde(flatten)]
    options: T,
}
input!(Section { section: Option<String> });
choices!(AppAction {
    ShowMain,
    ShowSettings,
    ShowHistory,
    ShowPinned,
    ShowTranslation,
    Restart,
    Quit
});
input!(AppActionInput { action: AppAction, page_id: Option<String>, section_id: Option<String> });
input!(SettingsUpdate { values: Map<String, Value> });
input!(SettingsReset {
    page_id: String,
    section_id: String
});
input!(SettingsAction { field_id: String, path: Option<String> });
input!(Model { id: String, name: String, base_url: String, model: String, supports_vision: Option<bool>, supports_reasoning: Option<bool>, api_key: Option<String>, credential_set: Option<bool> });
input!(ModelsUpdate { models: Vec<Model> });
input!(Credential {
    provider: String,
    secret: String
});
input!(HistoryList { cursor: Option<String>, limit: Option<HistoryPageSize>, query: Option<String> });
input!(HistoryGet { history_id: String, include_image: Option<bool> });
input!(HistoryId { history_id: String });
choices!(HistoryAction { Edit, Pin });
input!(HistoryActionInput {
    history_id: String,
    action: HistoryAction
});
input!(FilePath { path: String });
choices!(CleanupKind {
    ThumbnailCache,
    TemporaryFiles,
    History,
    Pinned
});
input!(Cleanup { kind: CleanupKind });
input!(Permission { permission: String, open_settings: Option<bool> });
choices!(UpdateAction {
    Check,
    Download,
    Cancel,
    Apply
});
input!(UpdateInput {
    action: UpdateAction
});
choices!(TemplateKind { Drawing, Watermark });
input!(TemplateList { kind: TemplateKind });
input!(Template { name: String, payload: Option<Map<String, Value>>, text: Option<String> });
input!(TemplateUpdate { kind: TemplateKind, templates: Vec<Template> });
choices!(TranslationSource { Selection });
input!(TranslationStart { texts: Option<Vec<String>>, source: Option<TranslationSource>, source_language: Option<String>, target_language: Option<String>, model_id: Option<String>, retry_job_id: Option<String>, idempotency_key: Option<String> });

choices!(DocumentSource {
    File,
    History,
    Pinned,
    Clipboard,
    Capture,
    Text,
    Html
});
choices!(DocumentTarget {
    CurrentMonitor,
    FocusedWindow,
    Monitor
});
input!(DocumentOpen { path: Option<String>, source: Option<DocumentSource>, source_id: Option<String>, target: Option<DocumentTarget>, monitor_id: Option<String>, scale: Option<DocumentScale>, capture_cursor: Option<bool>, delay_seconds: Option<DelaySeconds>, text: Option<String>, html: Option<String>, idempotency_key: Option<String>, as_job: Option<bool> });
input!(DocumentRecapture { path: Option<String>, source: Option<DocumentSource>, source_id: Option<String>, target: Option<DocumentTarget>, monitor_id: Option<String>, scale: Option<DocumentScale>, capture_cursor: Option<bool>, delay_seconds: Option<DelaySeconds>, text: Option<String>, html: Option<String> });
input!(DocumentId {
    document_id: String
});
input!(DocumentPoint { point: [f64; 2] });
choices!(DocumentCanvasTool {
    Select,
    Rectangle,
    Arrow,
    Line,
    Freehand,
    RectangleHighlight,
    PenHighlight,
    Eraser,
    RectangleFilter,
    PenFilter,
    Text,
    SerialNumber,
    Watermark,
    Spotlight,
    AutoFilter
});
input!(DocumentTool {
    tool: DocumentCanvasTool
});
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct DocumentSave {
    path: String,
    scale: Option<DocumentScale>,
    format: Option<Format>,
    quality: Option<ExportQuality>,
    compression_level: Option<CompressionLevel>,
    pdf_page_size: Option<PdfPageSize>,
    #[schemars(length(max = 1024))]
    pdf_title: Option<String>,
}
choices!(DocumentElementAction {
    Select,
    Order,
    Align,
    Opacity,
    Duplicate,
    Delete,
    DeleteAll,
    AdjustSerialNumbers,
    CreateSerialText,
    ErasePath
});
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct DocumentElementEdit {
    action: DocumentElementAction,
    id: Option<ElementId>,
    order: Option<Order>,
    alignment: Option<Alignment>,
    opacity: Option<f64>,
    delta: Option<i32>,
    #[schemars(length(min = 1, max = 8192))]
    points: Option<Vec<[f64; 2]>>,
}
input!(DocumentRecognitionEdit { expected_recognition_revision: u64, action: RecognitionAction,
    text: Option<String>, value: Option<String>, range: Option<[u32;4]>, row: Option<u32>,
    column: Option<u32>, enabled: Option<bool> });
choices!(OriginalContentFormat { Json, Text, Html });
input!(OriginalContentOutput { output: RecognitionOutput, format: Option<OriginalContentFormat>, path: Option<String> });
input!(DocumentSelection { operation: Option<RegionOperation>, r#type: Option<RegionType>, bounds: Option<[f64; 4]>, points: Option<Vec<[f64; 2]>> });
choices!(RecognitionKind {
    Text,
    Table,
    Qr,
    Markdown,
    Html
});
input!(DocumentRecognize {
    kind: RecognitionKind
});
input!(JobId { job_id: String });
input!(ArtifactRead {artifact_id:String,offset:Option<u64>,max_bytes:Option<ArtifactChunkSize>});
input!(ArtifactRelease {artifact_id:String,idempotency_key:Option<String>});

choices!(RecordingFormat {
    Mp4,
    Gif,
    Apng,
    Webp
});
choices!(Encoder { H264, H265, H264Hw });
#[derive(Deserialize, JsonSchema)]
enum Clarity {
    #[serde(rename = "4k")]
    Uhd,
    #[serde(rename = "2k")]
    Qhd,
    #[serde(rename = "1080p")]
    FullHd,
    #[serde(rename = "720p")]
    Hd,
    #[serde(rename = "480p")]
    Sd,
}
#[derive(Deserialize, JsonSchema)]
enum AnimatedClarity {
    #[serde(rename = "1080p")]
    FullHd,
    #[serde(rename = "720p")]
    Hd,
    #[serde(rename = "480p")]
    Sd,
}
choices!(EncodingPreset {
    Ultrafast,
    Veryfast,
    Medium,
    Veryslow,
    Placebo
});
input!(RecordingOptions {
    path: Option<String>,
    format: Option<RecordingFormat>, start_delay_seconds: Option<DelaySeconds>, microphone: Option<bool>,
    system_audio: Option<bool>, frame_rate: Option<FrameRate>, animated_frame_rate: Option<AnimatedFrameRate>,
    clarity: Option<Clarity>, animated_clarity: Option<AnimatedClarity>, encoder: Option<Encoder>,
    encoding_preset: Option<EncodingPreset>, r#loop: Option<bool>, capture_toolbar: Option<bool>,
    show_cursor: Option<bool>, show_keyboard: Option<bool>, mouse_highlight: Option<bool>,
    record_mouse_clicks: Option<bool>, mouse_trail_duration_ms: Option<TrailDuration>, keyboard_size: Option<KeyboardSize>,
    keyboard_background: Option<String>, keyboard_foreground: Option<String>, mouse_trail: Option<String>,
    mouse_click: Option<String>, mouse_highlight_color: Option<String>
});
input!(RecordingStart { region: [f64; 4], options: Option<RecordingOptions>, idempotency_key: Option<String> });
choices!(RecordingAction {
    Pause,
    Resume,
    Stop,
    Cancel,
    Copy,
    Close,
    Annotations,
    Undo,
    Redo,
    Reset
});
input!(RecordingControl { recording_id: String, action: RecordingAction, payload: Option<Annotations> });
input!(PinnedId { id: String });
input!(PinnedList { offset: Option<u32>, limit: Option<HistoryPageSize> });
#[derive(Deserialize, JsonSchema)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
enum PinnedSource {
    File { path: String },
    Clipboard,
    Text { text: String },
    Html { html: String },
}
input!(PinnedCreate { source: PinnedSource, group_id: Option<String>, idempotency_key: Option<String> });
input!(PinnedReplace {
    id: String,
    source: PinnedSource
});
choices!(Rotation {
    Clockwise,
    Counterclockwise,
    Reset
});
choices!(Flip {
    Horizontal,
    Vertical
});
input!(PinnedProperties { geometry: Option<[f64; 4]>, scale_percent: Option<f64>, opacity_percent: Option<Percentage>, click_through: Option<bool>, click_through_opacity_percent: Option<Percentage>, always_on_top: Option<bool>, show_border: Option<bool>, thumbnail: Option<bool>, hide_to_top: Option<bool>, rotation: Option<Rotation>, flip: Option<Flip> });
input!(PinnedUpdate {
    id: String,
    properties: PinnedProperties
});
choices!(PinnedAction {
    Show,
    Hide,
    Close,
    Destroy,
    RestoreLast,
    ShowAll,
    HideOthers,
    CloseOthers,
    CloseAll
});
input!(PinnedActionInput { action: PinnedAction, id: Option<String> });
choices!(PinnedOutput { Copy, Save });
input!(PinnedExport { id: String, output: PinnedOutput, path: Option<String>, original: Option<bool> });
choices!(PinnedEditAction {
    Annotations,
    Undo,
    Redo,
    Reset,
    Recognize,
    Translate,
    RecognitionEdit,
    Duplicate,
    Delete,
    Order,
    Align,
    Opacity,
    SerialText,
    SerialAdjust,
    TemplateInsert,
    TemplateExport,
    AutoFilter,
    Tool,
    ToolStyle,
    Editing
});
input!(Editing { enabled: bool });
input!(Duplicate { offset:Option<[f64;2]> });
input!(Ordering { order: Order });
input!(Aligning {
    alignment: Alignment
});
input!(Opacity { opacity: f64 });
input!(SerialAdjust { delta: i32 });
input!(TemplateInsert {
    payload: String,
    center: [f64; 2]
});
choices!(FilterCategory {
    Text,
    TextInBox,
    Image,
    Avatar,
    Icon,
    MessageBox,
    TextBlock
});
input!(PinnedAutoFilter { categories:Vec<FilterCategory> });
#[derive(Deserialize, JsonSchema)]
#[serde(untagged)]
enum PinnedPayload {
    Annotations(Annotations),
    Recognize(DocumentRecognize),
    RecognitionEdit(EditRecognition),
    Duplicate(Duplicate),
    Ordering(Ordering),
    Aligning(Aligning),
    Opacity(Opacity),
    SerialAdjust(SerialAdjust),
    TemplateInsert(TemplateInsert),
    AutoFilter(PinnedAutoFilter),
    Tool(DocumentTool),
    ToolStyle(ToolStyle),
    Editing(Editing),
}
input!(PinnedEdit { id: String, action: PinnedEditAction, payload: Option<PinnedPayload> });
choices!(GroupAction {
    Create,
    Activate,
    Move,
    Delete,
    DeleteEmpty
});
input!(GroupUpdate { action: GroupAction, name: Option<String>, group_id: Option<String>, id: Option<String> });

pub const TOOLS: &[(&str, &str, bool)] = &[
    (
        "snow_shot_artifact_list",
        "List owned retained artifacts with MIME, size, digest and expiry metadata.",
        true,
    ),
    (
        "snow_shot_artifact_read",
        "Read an owned artifact in bounded base64 chunks; continue with next_offset until eof.",
        true,
    ),
    (
        "snow_shot_artifact_release",
        "Release an owned retained artifact.",
        false,
    ),
    (
        "snow_shot_app_status",
        "Read application status and available feature domains.",
        true,
    ),
    (
        "snow_shot_app_displays",
        "List capture displays and their coordinate mappings.",
        true,
    ),
    (
        "snow_shot_app_action",
        "Open an application page, restart, or quit Snow Shot.",
        false,
    ),
    (
        "snow_shot_settings_get",
        "Read settings with typed field metadata; credentials are never returned.",
        true,
    ),
    (
        "snow_shot_settings_update",
        "Update settings by field ID with an expected settings revision.",
        false,
    ),
    (
        "snow_shot_settings_reset",
        "Reset the specified settings section.",
        false,
    ),
    (
        "snow_shot_settings_action",
        "Run a settings action with explicit arguments.",
        false,
    ),
    (
        "snow_shot_models_list",
        "Read configured model metadata without credentials.",
        true,
    ),
    (
        "snow_shot_models_update",
        "Update model definitions, retaining omitted credentials.",
        false,
    ),
    (
        "snow_shot_credentials_set",
        "Replace a model credential without returning its value.",
        false,
    ),
    (
        "snow_shot_history_list",
        "Search screenshot history using a bounded page and opaque cursor.",
        true,
    ),
    (
        "snow_shot_history_get",
        "Read a history entry, optionally including its image.",
        true,
    ),
    (
        "snow_shot_history_delete",
        "Delete a history entry at the expected history revision.",
        false,
    ),
    (
        "snow_shot_history_clear",
        "Clear screenshot history at the expected history revision.",
        false,
    ),
    (
        "snow_shot_history_action",
        "Open a history image in the editor or pin it.",
        false,
    ),
    (
        "snow_shot_configuration_export",
        "Export configuration to an explicit absolute path.",
        false,
    ),
    (
        "snow_shot_configuration_import",
        "Import configuration from an explicit absolute path.",
        false,
    ),
    (
        "snow_shot_storage_status",
        "Read cache and storage usage.",
        true,
    ),
    (
        "snow_shot_storage_cleanup",
        "Clean an explicitly selected storage category.",
        false,
    ),
    (
        "snow_shot_permissions_get",
        "Read native capture and accessibility permission state.",
        true,
    ),
    (
        "snow_shot_permissions_request",
        "Request an OS permission or open its settings page.",
        false,
    ),
    (
        "snow_shot_updates_status",
        "Read application update state.",
        true,
    ),
    (
        "snow_shot_updates_action",
        "Check, download, cancel, or apply an application update.",
        false,
    ),
    (
        "snow_shot_templates_list",
        "List drawing or watermark templates.",
        true,
    ),
    (
        "snow_shot_templates_update",
        "Replace a validated drawing or watermark template collection.",
        false,
    ),
    (
        "snow_shot_translation_catalog",
        "Read translation models, languages, and preferences without credentials.",
        true,
    ),
    (
        "snow_shot_translation_start",
        "Translate explicit text using configured providers and return an owned job handle.",
        false,
    ),
    (
        "snow_shot_document_open",
        "Open an isolated background image document; file is the default source.",
        false,
    ),
    (
        "snow_shot_document_list",
        "List background documents owned by this client.",
        true,
    ),
    (
        "snow_shot_document_state",
        "Read an owned background document and its revision.",
        true,
    ),
    (
        "snow_shot_document_clone",
        "Clone an owned background document.",
        false,
    ),
    (
        "snow_shot_document_apply_annotations",
        "Apply one typed annotation transaction to a background document.",
        false,
    ),
    (
        "snow_shot_document_set_tool",
        "Select the active background document canvas tool.",
        false,
    ),
    (
        "snow_shot_document_set_tool_style",
        "Set background document drawing defaults or selected styles.",
        false,
    ),
    (
        "snow_shot_document_edit_elements",
        "Select, duplicate, order, align or edit background document elements.",
        false,
    ),
    (
        "snow_shot_document_draw_template",
        "Insert or export a background document drawing template.",
        false,
    ),
    (
        "snow_shot_document_sample_color",
        "Read the rendered color at a canvas point.",
        true,
    ),
    (
        "snow_shot_document_recapture",
        "Replace the source of an owned document, preserving valid annotations and selection.",
        false,
    ),
    (
        "snow_shot_document_present",
        "Explicitly present an owned document in the visible editor.",
        false,
    ),
    (
        "snow_shot_document_set_selection",
        "Change a background document selection in canvas coordinates.",
        false,
    ),
    (
        "snow_shot_document_undo",
        "Undo the last background document edit.",
        false,
    ),
    (
        "snow_shot_document_redo",
        "Redo a background document edit.",
        false,
    ),
    (
        "snow_shot_document_render",
        "Render an immutable snapshot of a background document.",
        true,
    ),
    (
        "snow_shot_document_copy",
        "Copy a background document snapshot to the clipboard.",
        false,
    ),
    (
        "snow_shot_document_pin",
        "Pin a background document snapshot.",
        false,
    ),
    (
        "snow_shot_document_set_selection_style",
        "Set background document selection decoration and aspect ratio.",
        false,
    ),
    (
        "snow_shot_document_save",
        "Save a background document to an explicit absolute path.",
        false,
    ),
    (
        "snow_shot_document_close",
        "Close an owned background document.",
        false,
    ),
    (
        "snow_shot_document_recognize",
        "Start background recognition and return an owned job handle.",
        false,
    ),
    (
        "snow_shot_document_recognition_state",
        "Read retained recognition state at the expected document revision.",
        true,
    ),
    (
        "snow_shot_document_auto_filter",
        "Detect and filter selected categories in an owned background document through a cancelable job.",
        false,
    ),
    (
        "snow_shot_document_edit_recognition",
        "Edit retained text or table recognition using its own revision.",
        false,
    ),
    (
        "snow_shot_document_export_recognition",
        "Return, copy or save retained document recognition.",
        false,
    ),
    (
        "snow_shot_document_original_content",
        "Return, copy or save original source text or HTML.",
        false,
    ),
    (
        "snow_shot_job_list",
        "List background jobs owned by this client.",
        true,
    ),
    (
        "snow_shot_job_get",
        "Read an owned background job status and result.",
        true,
    ),
    (
        "snow_shot_job_cancel",
        "Request cancellation of an owned background job.",
        false,
    ),
    (
        "snow_shot_recording_state",
        "Read recording lifecycle state and revision.",
        true,
    ),
    (
        "snow_shot_recording_start",
        "Start a region recording with explicit recording options.",
        false,
    ),
    (
        "snow_shot_recording_control",
        "Pause, resume, finish, cancel, copy, or close an owned recording.",
        false,
    ),
    (
        "snow_shot_pinned_list",
        "List pinned images and their revisions.",
        true,
    ),
    (
        "snow_shot_pinned_get",
        "Read a pinned image state by stable ID.",
        true,
    ),
    (
        "snow_shot_pinned_create",
        "Pin a file, clipboard image, text, or HTML.",
        false,
    ),
    (
        "snow_shot_pinned_replace",
        "Replace a pinned image from a typed source.",
        false,
    ),
    (
        "snow_shot_pinned_update",
        "Update pinned image geometry and presentation properties.",
        false,
    ),
    (
        "snow_shot_pinned_action",
        "Show, hide, close, restore, or destroy pinned windows.",
        false,
    ),
    (
        "snow_shot_pinned_export",
        "Save or copy a pinned image.",
        false,
    ),
    (
        "snow_shot_pinned_edit",
        "Edit pinned annotations and recognized content.",
        false,
    ),
    ("snow_shot_group_list", "List pinned window groups.", true),
    (
        "snow_shot_group_update",
        "Create, activate, move to, or delete a pinned window group.",
        false,
    ),
];

pub fn schema(name: &str, input: Option<Value>) -> Result<Map<String, Value>, serde_json::Error> {
    match name {
        "snow_shot_artifact_list" => model::<Empty>(input),
        "snow_shot_artifact_read" => model::<ArtifactRead>(input),
        "snow_shot_artifact_release" => model::<ArtifactRelease>(input),
        "snow_shot_app_status"
        | "snow_shot_app_displays"
        | "snow_shot_models_list"
        | "snow_shot_storage_status"
        | "snow_shot_permissions_get"
        | "snow_shot_updates_status"
        | "snow_shot_document_list"
        | "snow_shot_job_list"
        | "snow_shot_recording_state"
        | "snow_shot_group_list"
        | "snow_shot_translation_catalog" => model::<Empty>(input),
        "snow_shot_app_action" => model::<AppActionInput>(input),
        "snow_shot_settings_get" => model::<Section>(input),
        "snow_shot_settings_update" => model::<Revision<SettingsUpdate>>(input),
        "snow_shot_settings_reset" => model::<Revision<SettingsReset>>(input),
        "snow_shot_settings_action" => model::<Revision<SettingsAction>>(input),
        "snow_shot_models_update" => model::<Revision<ModelsUpdate>>(input),
        "snow_shot_credentials_set" => model::<Revision<Credential>>(input),
        "snow_shot_history_list" => model::<HistoryList>(input),
        "snow_shot_history_get" => model::<HistoryGet>(input),
        "snow_shot_history_delete" => model::<Revision<HistoryId>>(input),
        "snow_shot_history_clear" => model::<Revision<Empty>>(input),
        "snow_shot_history_action" => model::<HistoryActionInput>(input),
        "snow_shot_configuration_export" => model::<FilePath>(input),
        "snow_shot_configuration_import" => model::<Revision<FilePath>>(input),
        "snow_shot_storage_cleanup" => model::<Revision<Cleanup>>(input),
        "snow_shot_permissions_request" => model::<Permission>(input),
        "snow_shot_updates_action" => model::<UpdateInput>(input),
        "snow_shot_templates_list" => model::<TemplateList>(input),
        "snow_shot_templates_update" => model::<Revision<TemplateUpdate>>(input),
        "snow_shot_translation_start" => {
            let exclusive = [
                "texts",
                "source",
                "source_language",
                "target_language",
                "model_id",
            ];
            if input.as_ref().is_some_and(|v| {
                v.get("retry_job_id").is_some() && exclusive.iter().any(|key| v.get(key).is_some())
            }) {
                return Err(<serde_json::Error as serde::de::Error>::custom(
                    "retry_job_id cannot be combined with new translation input",
                ));
            }
            let listing = input.is_none();
            let mut schema = model::<TranslationStart>(input)?;
            if listing {
                schema.insert("allOf".into(), serde_json::json!([{"if":{"required":["retry_job_id"]},"then":{"not":{"anyOf":exclusive.iter().map(|key|serde_json::json!({"required":[key]})).collect::<Vec<_>>()}}}]));
            }
            Ok(schema)
        }
        "snow_shot_document_open" => model::<DocumentOpen>(input),
        "snow_shot_document_state" => model::<DocumentId>(input),
        "snow_shot_document_clone"
        | "snow_shot_document_close"
        | "snow_shot_document_undo"
        | "snow_shot_document_redo" => model::<DocumentMutation<Empty>>(input),
        "snow_shot_document_apply_annotations" => model::<DocumentMutation<Annotations>>(input),
        "snow_shot_document_set_tool" => model::<DocumentMutation<DocumentTool>>(input),
        "snow_shot_document_set_tool_style" => model::<DocumentMutation<ToolStyle>>(input),
        "snow_shot_document_edit_elements" => {
            if input.as_ref().is_some_and(|v| {
                v.get("action").and_then(Value::as_str) == Some("erase_path")
                    && !v
                        .get("points")
                        .and_then(Value::as_array)
                        .is_some_and(|p| (1..=8192).contains(&p.len()))
            }) {
                return Err(serde::de::Error::custom(
                    "erase_path requires 1 to 8192 points",
                ));
            }
            model::<DocumentMutation<DocumentElementEdit>>(input)
        }
        "snow_shot_document_draw_template" => model::<DocumentMutation<DrawTemplate>>(input),
        "snow_shot_document_sample_color" => model::<DocumentMutation<DocumentPoint>>(input),
        "snow_shot_document_recapture" => model::<DocumentMutation<DocumentRecapture>>(input),
        "snow_shot_document_present" => model::<DocumentMutation<Empty>>(input),
        "snow_shot_document_set_selection" => model::<DocumentMutation<DocumentSelection>>(input),
        "snow_shot_document_render" | "snow_shot_document_copy" | "snow_shot_document_pin" => {
            model::<DocumentMutation<Render>>(input)
        }
        "snow_shot_document_set_selection_style" => {
            model::<DocumentMutation<SelectionStyle>>(input)
        }
        "snow_shot_document_save" => model::<DocumentMutation<DocumentSave>>(input),
        "snow_shot_document_recognize" => model::<DocumentMutation<DocumentRecognize>>(input),
        "snow_shot_document_auto_filter" => model::<DocumentMutation<PinnedAutoFilter>>(input),
        "snow_shot_document_recognition_state" => model::<DocumentMutation<Empty>>(input),
        "snow_shot_document_edit_recognition" => {
            model::<DocumentMutation<DocumentRecognitionEdit>>(input)
        }
        "snow_shot_document_export_recognition" => {
            model::<DocumentMutation<ExportRecognition>>(input)
        }
        "snow_shot_document_original_content" => {
            model::<DocumentMutation<OriginalContentOutput>>(input)
        }
        "snow_shot_job_get" | "snow_shot_job_cancel" => model::<JobId>(input),
        "snow_shot_recording_start" => model::<RecordingStart>(input),
        "snow_shot_recording_control" => model::<Revision<RecordingControl>>(input),
        "snow_shot_pinned_get" => model::<PinnedId>(input),
        "snow_shot_pinned_list" => model::<PinnedList>(input),
        "snow_shot_pinned_create" => model::<PinnedCreate>(input),
        "snow_shot_pinned_replace" => model::<Revision<PinnedReplace>>(input),
        "snow_shot_pinned_update" => model::<Revision<PinnedUpdate>>(input),
        "snow_shot_pinned_action" => {
            if input.as_ref().is_some_and(|v| {
                v.get("action").and_then(Value::as_str) != Some("restore_last")
                    && !v
                        .get("id")
                        .and_then(Value::as_str)
                        .is_some_and(|id| !id.is_empty())
            }) {
                return Err(serde::de::Error::custom("Pinned action requires an id"));
            }
            let generating = input.is_none();
            let mut schema = model::<Revision<PinnedActionInput>>(input)?;
            if generating {
                schema.insert("allOf".into(), serde_json::json!([{"if":{"properties":{"action":{"const":"restore_last"}},"required":["action"]},"then":{},"else":{"required":["id"]}}]));
            }
            Ok(schema)
        }
        "snow_shot_pinned_export" => model::<Revision<PinnedExport>>(input),
        "snow_shot_pinned_edit" => model::<Revision<PinnedEdit>>(input),
        "snow_shot_group_update" => model::<Revision<GroupUpdate>>(input),
        _ => Err(<serde_json::Error as serde::de::Error>::custom(
            "unknown tool",
        )),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn all_domain_contracts_have_unique_schemas_and_mutations_are_revisioned() {
        let mut names = std::collections::HashSet::new();
        for (name, _, _) in TOOLS {
            assert!(names.insert(name), "{name}");
            assert!(
                schema(name, None).unwrap().get("type") == Some(&json!("object")),
                "{name}"
            );
        }
        assert!(schema("snow_shot_settings_update", Some(json!({"values":{}}))).is_err());
        assert!(schema("snow_shot_document_apply_annotations",Some(json!({"document_id":"d","expected_revision":1,"operations":[{"type":"execute"}]}))).is_err());
        assert!(schema("snow_shot_document_apply_annotations",Some(json!({"document_id":"d","expected_revision":1,"operations":[{"type":"rectangle","bounds":[0,0,20,20],"style":{}}]}))).is_ok());
        assert!(
            schema(
                "snow_shot_settings_update",
                Some(json!({"expected_revision":2,"values":{"general.language":"en_US"}}))
            )
            .is_ok()
        );
        assert!(
            schema(
                "snow_shot_document_open",
                Some(json!({"source":"capture","target":"focused_window","delay_seconds":10}))
            )
            .is_ok()
        );
        assert!(
            schema(
                "snow_shot_document_open",
                Some(json!({"source":"capture","delay_seconds":11}))
            )
            .is_err()
        );
    }
    #[test]
    fn recording_limits_and_nested_pin_operations_are_validated() {
        for action in [
            "show",
            "hide",
            "close",
            "destroy",
            "show_all",
            "hide_others",
            "close_others",
            "close_all",
        ] {
            assert!(
                schema(
                    "snow_shot_pinned_action",
                    Some(json!({"id":"pin","expected_revision":1,"action":action}))
                )
                .is_ok(),
                "{action}"
            );
            assert!(
                schema(
                    "snow_shot_pinned_action",
                    Some(json!({"expected_revision":1,"action":action}))
                )
                .is_err(),
                "{action}"
            );
        }
        assert!(
            schema(
                "snow_shot_pinned_action",
                Some(json!({"expected_revision":1,"action":"restore_last"}))
            )
            .is_ok()
        );
        let input = json!({"region":[0,0,1920,1080],"options":{"frame_rate":60,"clarity":"1080p","keyboard_size":32,"path":"C:/capture.mp4"}});
        assert!(schema("snow_shot_recording_start", Some(input.clone())).is_ok());
        for (key, value) in [
            ("frame_rate", json!(240)),
            ("clarity", json!("8k")),
            ("keyboard_size", json!(256)),
            ("surprise", json!(true)),
        ] {
            let mut invalid = input.clone();
            invalid["options"][key] = value;
            assert!(
                schema("snow_shot_recording_start", Some(invalid)).is_err(),
                "{key}"
            );
        }
        assert!(
            schema(
                "snow_shot_pinned_create",
                Some(json!({"source":{"kind":"file","path":"C:/image.png","command":"bad"}}))
            )
            .is_err()
        );
        assert!(schema("snow_shot_pinned_edit",Some(json!({"id":"p","expected_revision":1,"action":"recognize","payload":{"kind":"text"}}))).is_ok());
    }
    #[test]
    fn document_capture_recognition_and_artifact_contracts_have_distinct_bounds() {
        assert!(
            schema(
                "snow_shot_document_save",
                Some(json!({"document_id":"d","expected_revision":1,"automatic_path":true}))
            )
            .is_err()
        );
        assert!(schema("snow_shot_document_save", Some(json!({"document_id":"d","expected_revision":1,"path":"C:/output.pdf","format":"pdf","quality":90,"compression_level":"medium","pdf_page_size":"a4_landscape","pdf_title":"Fixture"}))).is_ok());
        assert!(schema("snow_shot_document_edit_elements", Some(json!({"document_id":"d","expected_revision":1,"action":"erase_path","points":[[1,2],[3,4]]}))).is_ok());
        assert!(schema("snow_shot_document_edit_elements", Some(json!({"document_id":"d","expected_revision":1,"action":"erase_path","points":[]}))).is_err());
        assert!(super::super::schema("screenshot_edit_elements", Some(json!({"session_id":"s","expected_revision":1,"action":"erase_path","points":[[1,2]]}))).is_err());
        assert!(schema("snow_shot_pinned_edit", Some(json!({"id":"p","expected_revision":1,"action":"tool_style","payload":{"target":"arrow","style":{"arrow_ratio":2}}}))).is_ok());
        assert!(
            schema(
                "snow_shot_document_open",
                Some(
                    json!({"source":"capture","target":"monitor","monitor_id":"primary","scale":4})
                )
            )
            .is_ok()
        );
        assert!(
            schema(
                "snow_shot_document_open",
                Some(json!({"source":"capture","scale":4.01}))
            )
            .is_err()
        );
        assert!(schema("snow_shot_document_recapture", Some(json!({"document_id":"one","expected_revision":1,"idempotency_key":"repeat","source":"file","path":"C:/source.png"}))).is_ok());
        assert!(
            schema(
                "snow_shot_document_set_tool",
                Some(json!({"document_id":"one","expected_revision":1,"tool":"ocr"}))
            )
            .is_err()
        );
        assert!(schema("snow_shot_document_edit_recognition", Some(json!({"document_id":"one","expected_revision":1,"expected_recognition_revision":2,"action":"set_text","text":"edited"}))).is_ok());
        assert!(schema("snow_shot_document_edit_recognition", Some(json!({"document_id":"one","expected_revision":1,"action":"set_text","text":"edited"}))).is_err());
        for bytes in [0, 262145] {
            assert!(
                schema(
                    "snow_shot_artifact_read",
                    Some(json!({"artifact_id":"a","max_bytes":bytes}))
                )
                .is_err()
            );
        }
        assert!(
            schema(
                "snow_shot_artifact_read",
                Some(json!({"artifact_id":"a","offset":262144,"max_bytes":262144}))
            )
            .is_ok()
        );
        assert!(
            schema(
                "snow_shot_translation_start",
                Some(json!({"source":"selection"}))
            )
            .is_ok()
        );
        assert!(
            schema(
                "snow_shot_translation_start",
                Some(json!({"retry_job_id":"prior"}))
            )
            .is_ok()
        );
        assert!(
            schema(
                "snow_shot_translation_start",
                Some(json!({"retry_job_id":"prior","texts":["new"]}))
            )
            .is_err()
        );
    }
}

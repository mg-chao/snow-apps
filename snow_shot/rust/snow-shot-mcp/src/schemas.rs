//! Public tool inputs. The application and drawing engine additionally enforce geometry limits.
#![allow(dead_code)] // Fields are consumed by serde validation and JSON Schema generation.
use schemars::JsonSchema;
use serde::{Deserialize, de::DeserializeOwned};
use serde_json::{Map, Value};
#[path = "domain_schemas.rs"]
pub mod domains;

#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Empty {}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Session {
    session_id: String,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Mutation<T> {
    session_id: String,
    expected_revision: u64,
    #[serde(default)]
    idempotency_key: Option<String>,
    #[serde(flatten)]
    options: T,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Presentation {
    Visible,
    Silent,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Target {
    AllDisplays,
    Monitor,
    CurrentMonitor,
    FocusedWindow,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum DirectTarget {
    CurrentMonitor,
    FocusedWindow,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Begin {
    #[serde(default)]
    presentation: Option<Presentation>,
    #[serde(default)]
    target: Option<Target>,
    #[serde(default)]
    monitor_id: Option<String>,
    #[serde(default)]
    capture_cursor: Option<bool>,
    #[serde(default)]
    smart_selection: Option<bool>,
    #[serde(default)]
    idempotency_key: Option<String>,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum RegionOperation {
    Replace,
    Add,
    Subtract,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum RegionType {
    Rectangle,
    Polygon,
    Polyline,
    Freehand,
}
#[derive(Deserialize, JsonSchema)]
struct Selection {
    #[serde(default)]
    operation: Option<RegionOperation>,
    #[serde(rename = "type")]
    region_type: RegionType,
    #[serde(default)]
    bounds: Option<[f64; 4]>,
    #[serde(default)]
    points: Option<Vec<[f64; 2]>>,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum CanvasTool {
    Move,
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
    AutoFilter,
    Ocr,
    Table,
    Qr,
    Markdown,
    Html,
}
#[derive(Deserialize, JsonSchema)]
struct ToolInput {
    tool: CanvasTool,
}
#[derive(Deserialize, JsonSchema)]
struct Annotations {
    #[serde(default)]
    version: Option<u32>,
    #[serde(default)]
    label: Option<String>,
    operations: Vec<Annotation>,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Format {
    Png,
    Jpeg,
    Webp,
    Avif,
    Jxl,
    Bmp,
    Pdf,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum CompressionLevel {
    Low,
    Medium,
    High,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum PdfPageSize {
    ImageSize,
    A4Portrait,
    A4Landscape,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Output {
    None,
    Render,
    Save,
    Copy,
}
#[derive(Deserialize, JsonSchema)]
struct Render {
    #[serde(default)]
    scale: Option<f64>,
}
#[derive(Deserialize, JsonSchema)]
struct Save {
    #[serde(default)]
    scale: Option<f64>,
    #[serde(default)]
    path: Option<String>,
    #[serde(default)]
    automatic_path: Option<bool>,
    #[serde(default)]
    format: Option<Format>,
    #[serde(default)]
    quality: Option<u32>,
    #[serde(default)]
    compression_level: Option<CompressionLevel>,
    #[serde(default)]
    pdf_page_size: Option<PdfPageSize>,
    #[serde(default)]
    #[schemars(length(max = 1024))]
    pdf_title: Option<String>,
}
#[derive(Deserialize, JsonSchema)]
struct Finish {
    #[serde(default)]
    output: Option<Output>,
    #[serde(default)]
    scale: Option<f64>,
    #[serde(default)]
    path: Option<String>,
    #[serde(default)]
    automatic_path: Option<bool>,
    #[serde(default)]
    format: Option<Format>,
    #[serde(default)]
    quality: Option<u32>,
    #[serde(default)]
    compression_level: Option<CompressionLevel>,
    #[serde(default)]
    pdf_page_size: Option<PdfPageSize>,
    #[serde(default)]
    #[schemars(length(max = 1024))]
    pdf_title: Option<String>,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum DirectOutput {
    Render,
    Save,
    Copy,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Direct {
    target: DirectTarget,
    output: DirectOutput,
    #[serde(default)]
    capture_cursor: Option<bool>,
    #[serde(default)]
    idempotency_key: Option<String>,
    #[serde(default)]
    scale: Option<f64>,
    #[serde(default)]
    path: Option<String>,
    #[serde(default)]
    automatic_path: Option<bool>,
    #[serde(default)]
    format: Option<Format>,
    #[serde(default)]
    quality: Option<u32>,
    #[serde(default)]
    compression_level: Option<CompressionLevel>,
    #[serde(default)]
    pdf_page_size: Option<PdfPageSize>,
    #[serde(default)]
    #[schemars(length(max = 1024))]
    pdf_title: Option<String>,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Cancel {
    #[serde(default)]
    operation_id: Option<String>,
    #[serde(default)]
    session_id: Option<String>,
    #[serde(default)]
    request_id: Option<String>,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct ElementId {
    index: u32,
    generation: u32,
}
#[derive(Deserialize, JsonSchema)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
enum Annotation {
    Rectangle {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    Ellipse {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    Diamond {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    EllipseHighlight {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    RoundedRectangle {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    RectangleHighlight {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    Arrow {
        points: Vec<[f64; 2]>,
        #[serde(default)]
        style: Style,
    },
    Line {
        points: Vec<[f64; 2]>,
        #[serde(default)]
        style: Style,
    },
    Freehand {
        points: Vec<[f64; 2]>,
        #[serde(default)]
        closed: bool,
        #[serde(default)]
        style: Style,
    },
    PenHighlight {
        points: Vec<[f64; 2]>,
        #[serde(default)]
        style: Style,
    },
    RectangleFilter {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    PenFilter {
        points: Vec<[f64; 2]>,
        #[serde(default)]
        style: Style,
    },
    Text {
        bounds: [f64; 4],
        text: String,
        #[serde(default)]
        style: Style,
    },
    SerialNumber {
        center: [f64; 2],
        number: i64,
        #[serde(default)]
        style: Style,
    },
    Watermark {
        text: String,
        #[serde(default)]
        style: Style,
    },
    Spotlight {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    Select {
        id: ElementId,
    },
    Delete {
        ids: Vec<ElementId>,
    },
}

#[derive(Default, Deserialize, JsonSchema)]
#[serde(default, deny_unknown_fields)]
struct Style {
    stroke: Option<[u8; 4]>,
    fill: Option<[u8; 4]>,
    color: Option<[u8; 4]>,
    stroke_width: Option<f64>,
    opacity: Option<f64>,
    corner_radius: Option<f64>,
    rotation: Option<f64>,
    font_size: Option<f64>,
    font_family: Option<String>,
    diameter: Option<f64>,
    strength: Option<f64>,
    gap: Option<f64>,
    stroke_style: Option<StrokeStyle>,
    arrow_type: Option<ArrowType>,
    filter: Option<Filter>,
}
#[derive(Default, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Filter {
    #[default]
    Mosaic,
    GaussianBlur,
    Grayscale,
    Inversion,
    Emboss,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
enum StrokeStyle {
    Solid,
    Dashed,
    Dotted,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
enum ArrowType {
    Straight,
    Curve,
    Elbow,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum HistoryTarget {
    Canvas,
    Text,
    Table,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct History {
    #[serde(default)]
    target: Option<HistoryTarget>,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct SelectionStyle {
    #[serde(default)]
    corner_radius: Option<u32>,
    #[serde(default)]
    shadow_width: Option<u32>,
    #[serde(default)]
    shadow_color: Option<[u8; 4]>,
    #[serde(default)]
    aspect_ratio_locked: Option<bool>,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum ScrollAxis {
    Vertical,
    Horizontal,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum ScrollDirection {
    Up,
    Down,
    Left,
    Right,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct ScrollOnce {
    direction: ScrollDirection,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum ScrollAction {
    Start,
    SetAxis,
    AutoScroll,
    Move,
    Trim,
    Stop,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Scrolling {
    action: ScrollAction,
    #[serde(default)]
    axis: Option<ScrollAxis>,
    #[serde(default)]
    enabled: Option<bool>,
    #[serde(default)]
    offset: Option<[i32; 2]>,
    #[serde(default)]
    start: Option<u32>,
    #[serde(default)]
    end: Option<u32>,
}

#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum RecognitionKind {
    Text,
    Table,
    Qr,
    Markdown,
    Html,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Recognize {
    kind: RecognitionKind,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Operation {
    session_id: String,
    operation_id: String,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Order {
    SendToBack,
    SendBackward,
    BringForward,
    BringToFront,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Alignment {
    Left,
    CenterHorizontally,
    Right,
    Top,
    CenterVertically,
    Bottom,
    DistributeHorizontally,
    DistributeVertically,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum ElementAction {
    Select,
    Order,
    Align,
    Opacity,
    Duplicate,
    Delete,
    DeleteAll,
    AdjustSerialNumbers,
    CreateSerialText,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct EditElements {
    action: ElementAction,
    #[serde(default)]
    id: Option<ElementId>,
    #[serde(default)]
    order: Option<Order>,
    #[serde(default)]
    alignment: Option<Alignment>,
    #[serde(default)]
    opacity: Option<f64>,
    #[serde(default)]
    delta: Option<i32>,
}

#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum TemplateAction {
    Export,
    Insert,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct DrawTemplate {
    action: TemplateAction,
    #[serde(default)]
    payload: Option<String>,
}

#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum RecognitionAction {
    SetText,
    ResetText,
    Format,
    Punctuation,
    SelectCells,
    SetCell,
    MergeCells,
    SplitCells,
    ResetTable,
    ShowOriginal,
    Undo,
    Redo,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct EditRecognition {
    action: RecognitionAction,
    #[serde(default)]
    text: Option<String>,
    #[serde(default)]
    value: Option<String>,
    #[serde(default)]
    range: Option<[u32; 4]>,
    #[serde(default)]
    row: Option<u32>,
    #[serde(default)]
    column: Option<u32>,
    #[serde(default)]
    enabled: Option<bool>,
}

#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum TextFormat {
    None,
    Keep,
    Remove,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Punctuation {
    None,
    Half,
    Full,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum RecognitionOutput {
    Return,
    Copy,
    Save,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum RecognitionFormat {
    Text,
    Html,
    Markdown,
    Json,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct ExportRecognition {
    output: RecognitionOutput,
    #[serde(default)]
    format: Option<RecognitionFormat>,
    #[serde(default)]
    path: Option<String>,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct AutoFilter {
    categories: Vec<String>,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum StyleTarget {
    Rectangle,
    Arrow,
    Line,
    Freehand,
    RectangleHighlight,
    PenHighlight,
    RectangleFilter,
    PenFilter,
    Text,
    SerialNumber,
    Watermark,
    Spotlight,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum ArrowShaftType {
    Plain,
    Tapered,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum TextHorizontalAlign {
    Left,
    Center,
    Right,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum TextVerticalAlign {
    Top,
    Center,
    Bottom,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum SerialType {
    OutlinedCircle,
    SolidCircle,
    OutlinedSquare,
    SolidSquare,
    Circle,
}
macro_rules! bounded_style_number {
    ($name:ident, $ty:ty, $kind:literal, $minimum:expr, $maximum:expr) => {
        struct $name($ty);
        impl<'de> Deserialize<'de> for $name {
            fn deserialize<D:serde::Deserializer<'de>>(deserializer:D)->Result<Self,D::Error> {
                let value=<$ty>::deserialize(deserializer)?;
                if ($minimum..=$maximum).contains(&value) { Ok(Self(value)) }
                else { Err(serde::de::Error::custom(concat!(stringify!($name), " is outside its supported range"))) }
            }
        }
        impl JsonSchema for $name {
            fn schema_name()->std::borrow::Cow<'static,str>{ stringify!($name).into() }
            fn json_schema(_: &mut schemars::SchemaGenerator)->schemars::Schema {
                schemars::json_schema!({"type":$kind,"minimum":$minimum,"maximum":$maximum})
            }
        }
    };
}
bounded_style_number!(ArrowRatio, f64, "number", 1.0, 3.0);
bounded_style_number!(CornerRadius, f64, "number", 0.0, 8192.0);
bounded_style_number!(SerialNumber, u64, "integer", 0_u64, 9007199254740991_u64);
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum ShapeVariant {
    Rectangle,
    Ellipse,
    Diamond,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum FillStyle {
    Line,
    CrossLine,
    Solid,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Arrowhead {
    None,
    Arrow,
    Bar,
    Dot,
    Circle,
    CircleOutline,
    Triangle,
    TriangleOutline,
    Diamond,
    DiamondOutline,
    CrowfootOne,
    CrowfootMany,
    CrowfootOneOrMany,
    IndentedTriangle,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum FilterKind {
    Mosaic,
    GaussianBlur,
    Grayscale,
    Inversion,
    Emboss,
    SmartErase,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct ToolStyle {
    target: StyleTarget,
    style: StylePatch,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct StylePatch {
    #[serde(default)]
    arrow_shaft_type: Option<ArrowShaftType>,
    #[serde(default)]
    arrow_ratio: Option<ArrowRatio>,
    /// Per-corner radii in top-left, top-right, bottom-right, bottom-left order.
    /// Mutually exclusive with corner_radius; enforced by the application.
    #[serde(default)]
    corner_radii: Option<[CornerRadius; 4]>,
    #[serde(default)]
    horizontal_align: Option<TextHorizontalAlign>,
    #[serde(default)]
    vertical_align: Option<TextVerticalAlign>,
    #[serde(default)]
    serial_type: Option<SerialType>,
    #[serde(default)]
    number: Option<SerialNumber>,
    #[serde(default)]
    stroke: Option<[u8; 4]>,
    #[serde(default)]
    fill: Option<[u8; 4]>,
    #[serde(default)]
    color: Option<[u8; 4]>,
    #[serde(default)]
    stroke_width: Option<f64>,
    #[serde(default)]
    font_size: Option<f64>,
    #[serde(default)]
    font_family: Option<String>,
    #[serde(default)]
    opacity: Option<f64>,
    #[serde(default)]
    corner_radius: Option<f64>,
    #[serde(default)]
    shape: Option<ShapeVariant>,
    #[serde(default)]
    fill_style: Option<FillStyle>,
    #[serde(default)]
    stroke_style: Option<StrokeStyle>,
    #[serde(default)]
    start_arrowhead: Option<Arrowhead>,
    #[serde(default)]
    end_arrowhead: Option<Arrowhead>,
    #[serde(default)]
    arrow_type: Option<ArrowType>,
    #[serde(default)]
    filter: Option<FilterKind>,
    #[serde(default)]
    strength: Option<f64>,
    #[serde(default)]
    text: Option<String>,
    #[serde(default)]
    angle: Option<f64>,
    #[serde(default)]
    gap: Option<f64>,
}
fn model<T: JsonSchema + DeserializeOwned>(
    input: Option<Value>,
) -> Result<Map<String, Value>, serde_json::Error> {
    if let Some(input) = input {
        let _: T = serde_json::from_value(input)?;
        return Ok(Map::new());
    }
    Ok(schemars::schema_for!(T)
        .as_object()
        .expect("object input schema")
        .clone())
}
pub fn schema(name: &str, input: Option<Value>) -> Result<Map<String, Value>, serde_json::Error> {
    match name {
        "snow_shot_status" => model::<Empty>(input),
        "screenshot_set_selection_style" => model::<Mutation<SelectionStyle>>(input),
        "screenshot_set_tool_style" => model::<Mutation<ToolStyle>>(input),
        "screenshot_edit_elements" => model::<Mutation<EditElements>>(input),
        "screenshot_recapture" => model::<Mutation<Empty>>(input),
        "screenshot_scrolling" => model::<Mutation<Scrolling>>(input),
        "screenshot_scroll_once" => model::<Mutation<ScrollOnce>>(input),
        "screenshot_recognize" => model::<Mutation<Recognize>>(input),
        "screenshot_translate" => model::<Mutation<Empty>>(input),
        "screenshot_auto_filter" => model::<Mutation<AutoFilter>>(input),
        "screenshot_operation" => model::<Operation>(input),
        "screenshot_edit_recognition" => model::<Mutation<EditRecognition>>(input),
        "screenshot_export_recognition" => model::<Mutation<ExportRecognition>>(input),
        "screenshot_draw_template" => model::<Mutation<DrawTemplate>>(input),
        "screenshot_undo" => model::<Mutation<History>>(input),
        "screenshot_redo" => model::<Mutation<History>>(input),

        "screenshot_begin" => model::<Begin>(input),
        "screenshot_state" => model::<Session>(input),
        "screenshot_set_selection" => model::<Mutation<Selection>>(input),
        "screenshot_set_tool" => model::<Mutation<ToolInput>>(input),
        "screenshot_apply_annotations" => model::<Mutation<Annotations>>(input),
        "screenshot_render" | "screenshot_copy" | "screenshot_pin" => {
            model::<Mutation<Render>>(input)
        }
        "screenshot_save" => model::<Mutation<Save>>(input),
        "screenshot_finish" => model::<Mutation<Finish>>(input),
        "screenshot_cancel" => model::<Cancel>(input),
        "screenshot_direct_capture" => model::<Direct>(input),
        _ => domains::schema(name, input),
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn every_legacy_tool_retains_its_checked_input_contract() {
        let fixture: Value =
            serde_json::from_str(include_str!("../../../tests/mcp_contract_fixtures.json"))
                .unwrap();
        let mut covered = std::collections::HashSet::new();
        for case in fixture["fixtures"].as_array().unwrap() {
            let name = case["name"].as_str().unwrap();
            assert!(covered.insert(name), "duplicate contract: {name}");
            assert!(
                schema(name, Some(case["arguments"].clone())).is_ok(),
                "legacy contract rejected: {name}"
            );
            if case["arguments"].get("expected_revision").is_some() {
                let mut stale = case["arguments"].clone();
                stale.as_object_mut().unwrap().remove("expected_revision");
                assert!(
                    schema(name, Some(stale)).is_err(),
                    "mutation lost revision guard: {name}"
                );
            }
        }
        assert_eq!(
            covered,
            crate::server::TOOLS
                .iter()
                .map(|(name, _, _)| *name)
                .collect()
        );
    }
    #[test]
    fn workflow_schemas_are_typed() {
        assert!(schema("screenshot_set_tool_style", Some(json!({"session_id":"s","expected_revision":1,"target":"arrow","style":{"arrow_shaft_type":"tapered","arrow_ratio":3}}))).is_ok());
        assert!(schema("screenshot_set_tool_style", Some(json!({"session_id":"s","expected_revision":1,"target":"arrow","style":{"arrow_ratio":3.1}}))).is_err());
        assert!(schema("screenshot_set_tool_style", Some(json!({"session_id":"s","expected_revision":1,"target":"text","style":{"horizontal_align":"center","vertical_align":"bottom","corner_radii":[0,1,2,3]}}))).is_ok());
        assert!(schema("screenshot_set_tool_style", Some(json!({"session_id":"s","expected_revision":1,"target":"serial_number","style":{"serial_type":"solid_square","number":9007199254740991_u64}}))).is_ok());
        assert!(schema("screenshot_set_tool_style", Some(json!({"session_id":"s","expected_revision":1,"target":"serial_number","style":{"number":9007199254740992_u64}}))).is_err());
        for direction in ["up", "down", "left", "right"] {
            assert!(
                schema(
                    "screenshot_scroll_once",
                    Some(json!({"session_id":"s","expected_revision":1,"direction":direction}))
                )
                .is_ok()
            );
        }
        for bad in [
            json!({"session_id":"s","expected_revision":1,"direction":"diagonal"}),
            json!({"session_id":"s","expected_revision":1,"direction":"down","count":100}),
        ] {
            assert!(schema("screenshot_scroll_once", Some(bad)).is_err());
        }
        assert!(
            schema(
                "screenshot_operation",
                Some(json!({"session_id":"s","operation_id":"o"}))
            )
            .is_ok()
        );
        assert!(
            schema(
                "screenshot_scrolling",
                Some(json!({"session_id":"s","expected_revision":1,"action":"execute"}))
            )
            .is_err()
        );
        assert!(schema("screenshot_edit_recognition", Some(json!({"session_id":"s","expected_revision":1,"action":"set_cell","row":0,"column":1,"text":"value"}))).is_ok());
        assert!(
            schema(
                "screenshot_undo",
                Some(json!({"session_id":"s","expected_revision":1,"target":"table"}))
            )
            .is_ok()
        );
        assert!(schema("not_a_tool", None).is_err());
    }
    #[test]
    fn typed_inputs_require_revision_and_reject_unknown_operations() {
        assert!(schema("screenshot_undo", Some(json!({"session_id":"s"}))).is_err());
        assert!(schema("screenshot_apply_annotations",Some(json!({"session_id":"s","expected_revision":2,"operations":[{"type":"execute"}]}))).is_err());
        assert!(schema("screenshot_set_selection",Some(json!({"session_id":"s","expected_revision":2,"type":"rectangle","bounds":[0,0,20,20]}))).is_ok());
        assert!(schema("screenshot_apply_annotations",Some(json!({"session_id":"s","expected_revision":2,"operations":[{"type":"rectangle","bounds":[0,0,20,20],"style":{}}]}))).is_ok());
        let finish = schema(
            "screenshot_finish",
            Some(json!({
                "session_id": "s", "expected_revision": 2, "output": "render"
            })),
        );
        assert!(finish.is_ok(), "{finish:?}");
        assert!(
            schema(
                "screenshot_finish",
                Some(json!({
                    "session_id": "s", "expected_revision": 2, "output": "save",
                    "path": "C:\\capture.png", "format": "png"
                }))
            )
            .is_ok()
        );
        assert!(
            schema(
                "screenshot_direct_capture",
                Some(json!({
                    "target": "current_monitor", "output": "render"
                }))
            )
            .is_ok()
        );
        assert!(
            schema(
                "screenshot_direct_capture",
                Some(json!({
                    "target": "current_monitor", "output": "save",
                    "path": "C:\\capture.png", "format": "png"
                }))
            )
            .is_ok()
        );
        assert!(
            schema(
                "screenshot_direct_capture",
                Some(json!({
                    "target": "current_monitor"
                }))
            )
            .is_err()
        );
    }
}

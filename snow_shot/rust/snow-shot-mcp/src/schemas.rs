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
    Distance,
    Angle,
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
    RectangleEraser,
    BrushEraser,
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
    Distance {
        points: [[f64; 2]; 2],
        #[serde(default)]
        style: DistanceAnnotationStyle,
    },
    Angle {
        /// Three points ordered as first endpoint, vertex, second endpoint.
        points: [[f64; 2]; 3],
        #[serde(default)]
        style: AngleAnnotationStyle,
        /// Distinguishes a full revolution from zero for coincident rays.
        #[serde(default)]
        full_turn: bool,
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
    Brightness,
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
    #[cfg(not(feature = "mini"))]
    Table,
    #[cfg(not(feature = "mini"))]
    Qr,
    #[cfg(not(feature = "mini"))]
    Markdown,
    #[cfg(not(feature = "mini"))]
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
    #[cfg(not(feature = "mini"))]
    SelectCells,
    #[cfg(not(feature = "mini"))]
    SetCell,
    #[cfg(not(feature = "mini"))]
    MergeCells,
    #[cfg(not(feature = "mini"))]
    SplitCells,
    #[cfg(not(feature = "mini"))]
    ResetTable,
    #[cfg(not(feature = "mini"))]
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
    #[cfg(not(feature = "mini"))]
    #[serde(default)]
    value: Option<String>,
    #[cfg(not(feature = "mini"))]
    #[serde(default)]
    range: Option<[u32; 4]>,
    #[cfg(not(feature = "mini"))]
    #[serde(default)]
    row: Option<u32>,
    #[cfg(not(feature = "mini"))]
    #[serde(default)]
    column: Option<u32>,
    #[cfg(not(feature = "mini"))]
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
    #[cfg(not(feature = "mini"))]
    Html,
    #[cfg(not(feature = "mini"))]
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
    Distance,
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
bounded_style_number!(DistanceFactor, f64, "number", 0.01, 1000.0);
bounded_style_number!(DistanceDecimals, u8, "integer", 0, 3);
bounded_style_number!(DistanceStrokeWidth, f64, "number", 1.0, 72.0);
bounded_style_number!(AngleDecimals, u8, "integer", 0, 3);
bounded_style_number!(AngleStrokeWidth, f64, "number", 1.0, 72.0);
bounded_style_number!(ArrowRatio, f64, "number", 0.5, 3.0);
bounded_style_number!(CornerRadius, f64, "number", 0.0, 8192.0);
bounded_style_number!(SerialNumber, u64, "integer", 0_u64, 9007199254740991_u64);
bounded_style_number!(BrushEraserWidth, f64, "number", 1.0, 72.0);
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
#[serde(rename_all = "lowercase")]
enum DistanceUnit {
    Px,
    Cm,
    M,
    Km,
    Mm,
}

#[derive(Default, Deserialize, JsonSchema)]
#[serde(default, deny_unknown_fields)]
struct DistanceAnnotationStyle {
    stroke: Option<[u8; 4]>,
    stroke_width: Option<DistanceStrokeWidth>,
    factor: Option<DistanceFactor>,
    unit: Option<DistanceUnit>,
    decimal_places: Option<DistanceDecimals>,
    endpoint_scale: Option<ArrowRatio>,
    endpoint_style: Option<Arrowhead>,
}

#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum AngleUnit {
    Degrees,
    Radians,
}

#[derive(Default, Deserialize, JsonSchema)]
#[serde(default, deny_unknown_fields)]
struct AngleAnnotationStyle {
    stroke: Option<[u8; 4]>,
    stroke_width: Option<AngleStrokeWidth>,
    unit: Option<AngleUnit>,
    decimal_places: Option<AngleDecimals>,
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
    Brightness,
    SmartErase,
}
#[derive(Deserialize, JsonSchema)]
#[serde(untagged)]
enum ToolStyle {
    Standard(Box<StandardToolStyle>),
    BrushEraser(BrushEraserToolStyle),
    Angle(AngleToolStyle),
}
// Serde cannot consume a flattened untagged enum under deny_unknown_fields.
// Keep the union outside each complete mutation so its strict object branch
// owns all fields, including the revision guard.
#[derive(Deserialize, JsonSchema)]
#[serde(untagged)]
#[schemars(extend("type" = "object"))]
enum ScreenshotToolStyleMutation {
    Standard(Box<Mutation<StandardToolStyle>>),
    BrushEraser(Mutation<BrushEraserToolStyle>),
    Angle(Mutation<AngleToolStyle>),
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum AngleTarget {
    Angle,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct AngleToolStyle {
    target: AngleTarget,
    style: AngleAnnotationStyle,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct StandardToolStyle {
    target: StyleTarget,
    style: StylePatch,
}
#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum BrushEraserTarget {
    BrushEraser,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct BrushEraserToolStyle {
    target: BrushEraserTarget,
    style: BrushEraserStylePatch,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct BrushEraserStylePatch {
    /// Width in canvas pixels for future Brush Eraser strokes.
    stroke_width: BrushEraserWidth,
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct StylePatch {
    #[serde(default)]
    factor: Option<DistanceFactor>,
    #[serde(default)]
    unit: Option<DistanceUnit>,
    #[serde(default)]
    decimal_places: Option<DistanceDecimals>,
    #[serde(default)]
    endpoint_scale: Option<ArrowRatio>,
    #[serde(default)]
    endpoint_style: Option<Arrowhead>,
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
    if !crate::edition::method_enabled(name)
        || input
            .as_ref()
            .is_some_and(|value| !crate::edition::input_enabled(name, value))
    {
        return Err(serde::de::Error::custom(
            "This operation is unavailable in the compiled edition",
        ));
    }
    match name {
        "snow_shot_mcp_status" => model::<Empty>(input),
        "snow_shot_screenshot_set_selection_style" => model::<Mutation<SelectionStyle>>(input),
        "snow_shot_screenshot_set_tool_style" => model::<ScreenshotToolStyleMutation>(input),
        "snow_shot_screenshot_edit_elements" => model::<Mutation<EditElements>>(input),
        "snow_shot_screenshot_recapture" => model::<Mutation<Empty>>(input),
        "snow_shot_screenshot_scrolling" => model::<Mutation<Scrolling>>(input),
        "snow_shot_screenshot_scroll_once" => model::<Mutation<ScrollOnce>>(input),
        "snow_shot_screenshot_recognize" => model::<Mutation<Recognize>>(input),
        #[cfg(not(feature = "mini"))]
        "snow_shot_screenshot_translate" => model::<Mutation<Empty>>(input),
        "snow_shot_screenshot_auto_filter" => model::<Mutation<AutoFilter>>(input),
        "snow_shot_screenshot_operation" => model::<Operation>(input),
        "snow_shot_screenshot_edit_recognition" => model::<Mutation<EditRecognition>>(input),
        "snow_shot_screenshot_export_recognition" => model::<Mutation<ExportRecognition>>(input),
        "snow_shot_screenshot_draw_template" => model::<Mutation<DrawTemplate>>(input),
        "snow_shot_screenshot_undo" => model::<Mutation<History>>(input),
        "snow_shot_screenshot_redo" => model::<Mutation<History>>(input),

        "snow_shot_screenshot_begin" => model::<Begin>(input),
        "snow_shot_screenshot_state" => model::<Session>(input),
        "snow_shot_screenshot_set_selection" => model::<Mutation<Selection>>(input),
        "snow_shot_screenshot_set_tool" => model::<Mutation<ToolInput>>(input),
        "snow_shot_screenshot_apply_annotations" => model::<Mutation<Annotations>>(input),
        "snow_shot_screenshot_render"
        | "snow_shot_screenshot_copy"
        | "snow_shot_screenshot_pin" => model::<Mutation<Render>>(input),
        "snow_shot_screenshot_save" => model::<Mutation<Save>>(input),
        "snow_shot_screenshot_finish" => model::<Mutation<Finish>>(input),
        "snow_shot_screenshot_cancel" => model::<Cancel>(input),
        "snow_shot_screenshot_direct_capture" => model::<Direct>(input),
        _ => domains::schema(name, input),
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn eraser_tool_ids_are_accepted_for_screenshots_and_documents() {
        for (name, owner) in [
            ("snow_shot_screenshot_set_tool", "session_id"),
            ("snow_shot_document_set_tool", "document_id"),
        ] {
            for tool in ["eraser", "rectangle_eraser", "brush_eraser"] {
                let input = json!({owner:"owned","expected_revision":1,"tool":tool});
                assert!(schema(name, Some(input.clone())).is_ok(), "{name}: {tool}");
                let mut invalid = input;
                invalid.as_object_mut().unwrap().remove("expected_revision");
                assert!(schema(name, Some(invalid)).is_err(), "{name}: {tool}");
            }
            assert!(
                schema(
                    name,
                    Some(json!({owner:"owned","expected_revision":1,"tool":"restore_background"}))
                )
                .is_err()
            );
        }
    }

    #[test]
    fn eraser_brush_style_requires_only_a_bounded_width() {
        for (name, owner) in [
            ("snow_shot_screenshot_set_tool_style", "session_id"),
            ("snow_shot_document_set_tool_style", "document_id"),
        ] {
            for width in [1.0, 30.5, 72.0] {
                let result = schema(
                    name,
                    Some(json!({owner:"owned","expected_revision":1,
                    "target":"brush_eraser","style":{"stroke_width":width}})),
                );
                assert!(result.is_ok(), "{name}: {width}: {result:?}");
            }
            for style in [
                json!({}),
                json!({"stroke_width":0.999}),
                json!({"stroke_width":72.001}),
                json!({"stroke_width":null}),
                json!({"stroke_width":"30"}),
                json!({"stroke_width":30,"opacity":1}),
                json!({"stroke_width":30,"strength":1}),
                json!({"stroke_width":30,"filter":"mosaic"}),
                json!({"stroke_width":30,"stroke":[0,0,0,255]}),
                json!({"stroke_width":30,"unknown":true}),
            ] {
                assert!(
                    schema(
                        name,
                        Some(json!({owner:"owned","expected_revision":1,
                    "target":"brush_eraser","style":style}))
                    )
                    .is_err(),
                    "{name}: {style}"
                );
            }
            for target in ["eraser", "rectangle_eraser", "restore_background"] {
                assert!(
                    schema(
                        name,
                        Some(json!({owner:"owned","expected_revision":1,
                    "target":target,"style":{"stroke_width":30}}))
                    )
                    .is_err(),
                    "{name}: {target}"
                );
            }
            for target in ["rectangle_filter", "pen_filter"] {
                assert!(
                    schema(
                        name,
                        Some(json!({owner:"owned","expected_revision":1,
                    "target":target,"style":{"filter":"mosaic","stroke_width":30}}))
                    )
                    .is_ok()
                );
                assert!(
                    schema(
                        name,
                        Some(json!({owner:"owned","expected_revision":1,
                    "target":target,"style":{"filter":"restore_background"}}))
                    )
                    .is_err()
                );
            }
            assert!(
                schema(
                    name,
                    Some(json!({owner:"owned","target":"brush_eraser",
                "style":{"stroke_width":30}}))
                )
                .is_err()
            );
        }
    }

    #[test]
    fn eraser_pinned_payloads_reuse_the_document_contract() {
        for tool in ["rectangle_eraser", "brush_eraser"] {
            assert!(
                schema(
                    "snow_shot_pinned_edit",
                    Some(json!({"id":"pinned",
                "expected_revision":1,"action":"tool","payload":{"tool":tool}}))
                )
                .is_ok()
            );
        }
        assert!(
            schema(
                "snow_shot_pinned_edit",
                Some(json!({"id":"pinned",
            "expected_revision":1,"action":"tool_style","payload":{
                "target":"brush_eraser","style":{"stroke_width":30}}}))
            )
            .is_ok()
        );
        for style in [
            json!({"stroke_width":73}),
            json!({"stroke_width":30,"opacity":1}),
        ] {
            assert!(
                schema(
                    "snow_shot_pinned_edit",
                    Some(json!({"id":"pinned",
                "expected_revision":1,"action":"tool_style","payload":{
                    "target":"brush_eraser","style":style}}))
                )
                .is_err()
            );
        }
    }
    #[test]
    fn full_only_tools_and_recognition_fields_follow_the_compiled_edition() {
        for name in [
            "snow_shot_screenshot_translate",
            "snow_shot_models_list",
            "snow_shot_models_update",
            "snow_shot_credentials_set",
            "snow_shot_translation_catalog",
            "snow_shot_translation_start",
        ] {
            assert_eq!(schema(name, None).is_ok(), !crate::edition::MINI, "{name}");
            let included = crate::server::TOOLS
                .iter()
                .chain(domains::TOOLS.iter())
                .any(|(tool, _, _)| *tool == name);
            assert_eq!(included, !crate::edition::MINI, "{name}");
        }
        for name in [
            "snow_shot_screenshot_edit_recognition",
            "snow_shot_document_edit_recognition",
        ] {
            let published = schema(name, None).unwrap();
            for (field, value) in [
                ("value", json!("cell")),
                ("range", json!([0, 0, 1, 1])),
                ("row", json!(0)),
                ("column", json!(0)),
                ("enabled", json!(true)),
            ] {
                assert_eq!(
                    published["properties"].get(field).is_some(),
                    !crate::edition::MINI,
                    "{name}: {field}"
                );
                let mut arguments = if name.contains("screenshot") {
                    json!({"session_id":"s","expected_revision":1,"action":"set_text","text":"edited"})
                } else {
                    json!({"document_id":"d","expected_revision":1,"expected_recognition_revision":1,"action":"set_text","text":"edited"})
                };
                arguments[field] = value;
                assert_eq!(schema(name, Some(arguments)).is_ok(), !crate::edition::MINI);
            }
        }
    }
    #[test]
    fn every_screenshot_tool_accepts_its_checked_input_contract() {
        let fixture: Value =
            serde_json::from_str(include_str!("../../../tests/mcp_contract_fixtures.json"))
                .unwrap();
        let mut covered = std::collections::HashSet::new();
        for case in fixture["fixtures"].as_array().unwrap() {
            let name = case["name"].as_str().unwrap();
            if !crate::edition::method_enabled(name) {
                continue;
            }
            assert!(covered.insert(name), "duplicate contract: {name}");
            assert!(
                schema(name, Some(case["arguments"].clone())).is_ok(),
                "screenshot contract rejected: {name}"
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
                .filter(|(name, _, _)| crate::edition::method_enabled(name))
                .map(|(name, _, _)| *name)
                .collect()
        );
    }
    #[test]
    fn distance_annotation_schemas_validate_settings_and_two_endpoints() {
        for unit in ["px", "cm", "m", "km", "mm"] {
            let style = json!({"stroke":[245,34,45,255],"stroke_width":2,"factor":0.01,
                "unit":unit,"decimal_places":3,"endpoint_scale":0.5,"endpoint_style":"bar"});
            assert!(
                schema(
                    "snow_shot_screenshot_set_tool_style",
                    Some(json!({
                        "session_id":"s","expected_revision":1,"target":"distance","style":style
                    }))
                )
                .is_ok()
            );
            assert!(
                schema(
                    "snow_shot_document_set_tool_style",
                    Some(json!({
                        "document_id":"d","expected_revision":1,"target":"distance","style":style
                    }))
                )
                .is_ok()
            );
            assert!(
                schema(
                    "snow_shot_screenshot_apply_annotations",
                    Some(json!({
                        "session_id":"s","expected_revision":1,"operations":[{
                            "type":"distance","points":[[0,0],[3,4]],"style":style
                        }]
                    }))
                )
                .is_ok()
            );
        }
        for style in [
            json!({"factor":0}),
            json!({"factor":1000.1}),
            json!({"decimal_places":4}),
            json!({"decimal_places":1.5}),
            json!({"unit":"unsupported"}),
            json!({"endpoint_scale":0.4}),
            json!({"endpoint_style":"unsupported"}),
        ] {
            assert!(
                schema(
                    "snow_shot_screenshot_set_tool_style",
                    Some(json!({
                        "session_id":"s","expected_revision":1,"target":"distance","style":style
                    }))
                )
                .is_err()
            );
        }
        for points in [json!([[0, 0]]), json!([[0, 0], [3, 4], [5, 6]])] {
            assert!(
                schema(
                    "snow_shot_screenshot_apply_annotations",
                    Some(json!({
                        "session_id":"s","expected_revision":1,"operations":[{
                            "type":"distance","points":points
                        }]
                    }))
                )
                .is_err()
            );
        }
        // Image calibration belongs to the host, never caller-controlled annotation style.
        assert!(
            schema(
                "snow_shot_screenshot_apply_annotations",
                Some(json!({
                    "session_id":"s","expected_revision":1,"operations":[{
                        "type":"distance","points":[[0,0],[3,4]],"pixel_scale":[2,2]
                    }]
                }))
            )
            .is_err()
        );
        assert!(
            schema(
                "snow_shot_screenshot_set_tool",
                Some(json!({
                    "session_id":"s","expected_revision":1,"tool":"distance"
                }))
            )
            .is_ok()
        );
        assert!(
            schema(
                "snow_shot_document_set_tool",
                Some(json!({
                    "document_id":"d","expected_revision":1,"tool":"distance"
                }))
            )
            .is_ok()
        );
    }

    #[test]
    fn angle_annotation_schemas_accept_tools_styles_and_three_ordered_points() {
        for (name, owner) in [
            ("snow_shot_screenshot_set_tool", "session_id"),
            ("snow_shot_document_set_tool", "document_id"),
        ] {
            let input = json!({owner:"owned","expected_revision":1,"tool":"angle"});
            assert!(schema(name, Some(input.clone())).is_ok(), "{name}");
            let mut invalid = input;
            invalid.as_object_mut().unwrap().remove("expected_revision");
            assert!(schema(name, Some(invalid)).is_err(), "{name}");
        }
        for unit in ["degrees", "radians"] {
            for decimals in 0..=3 {
                for width in [1.0, 2.5, 72.0] {
                    let style = json!({"stroke":[245,34,45,255],"stroke_width":width,
                        "unit":unit,"decimal_places":decimals});
                    for (name, owner) in [
                        ("snow_shot_screenshot_set_tool_style", "session_id"),
                        ("snow_shot_document_set_tool_style", "document_id"),
                    ] {
                        assert!(
                            schema(
                                name,
                                Some(json!({owner:"owned","expected_revision":1,
                            "target":"angle","style":style}))
                            )
                            .is_ok(),
                            "{name}: {style}"
                        );
                    }
                    for (name, owner) in [
                        ("snow_shot_screenshot_apply_annotations", "session_id"),
                        ("snow_shot_document_apply_annotations", "document_id"),
                    ] {
                        for full_turn in [false, true] {
                            let input = json!({owner:"owned","expected_revision":1,"operations":[{
                                "type":"angle","points":[[100,0],[0,0],[0,100]],
                                "style":style,"full_turn":full_turn}]});
                            assert!(schema(name, Some(input)).is_ok(), "{name}: {style}");
                        }
                    }
                    assert!(
                        schema(
                            "snow_shot_pinned_edit",
                            Some(json!({"id":"pinned",
                        "expected_revision":1,"action":"tool_style","payload":{
                            "target":"angle","style":style}}))
                        )
                        .is_ok()
                    );
                }
            }
        }
        assert!(
            schema(
                "snow_shot_pinned_edit",
                Some(json!({"id":"pinned",
            "expected_revision":1,"action":"tool","payload":{"tool":"angle"}}))
            )
            .is_ok()
        );
        assert!(
            schema(
                "snow_shot_pinned_edit",
                Some(json!({"id":"pinned",
            "expected_revision":1,"action":"annotations","payload":{"operations":[{
                "type":"angle","points":[[100,0],[0,0],[0,100]]}]}}))
            )
            .is_ok()
        );
        for (name, owner) in [
            ("snow_shot_screenshot_apply_annotations", "session_id"),
            ("snow_shot_document_apply_annotations", "document_id"),
        ] {
            assert!(
                schema(
                    name,
                    Some(json!({owner:"owned","expected_revision":1,"operations":[{
                "type":"angle","points":[[100,0],[0,0],[0,100]]}]}))
                )
                .is_ok()
            );
        }
    }

    #[test]
    fn angle_annotation_schemas_reject_invalid_values_and_unrelated_style_targets() {
        for style in [
            json!({"stroke_width":0.99}),
            json!({"stroke_width":72.01}),
            json!({"stroke_width":"2"}),
            json!({"stroke":[0,0,0,256]}),
            json!({"decimal_places":-1}),
            json!({"decimal_places":4}),
            json!({"decimal_places":1.5}),
            json!({"unit":"px"}),
            json!({"unit":"degree"}),
            json!({"factor":1}),
            json!({"fill":[0,0,0,255]}),
            json!({"endpoint_style":"bar"}),
            json!({"full_turn":true}),
        ] {
            for (name, owner) in [
                ("snow_shot_screenshot_set_tool_style", "session_id"),
                ("snow_shot_document_set_tool_style", "document_id"),
            ] {
                assert!(
                    schema(
                        name,
                        Some(json!({owner:"owned","expected_revision":1,
                    "target":"angle","style":style}))
                    )
                    .is_err(),
                    "{name}: {style}"
                );
            }
            for (name, owner) in [
                ("snow_shot_screenshot_apply_annotations", "session_id"),
                ("snow_shot_document_apply_annotations", "document_id"),
            ] {
                assert!(
                    schema(
                        name,
                        Some(json!({owner:"owned","expected_revision":1,"operations":[{
                    "type":"angle","points":[[100,0],[0,0],[0,100]],"style":style}]}))
                    )
                    .is_err(),
                    "{name}: {style}"
                );
            }
            assert!(
                schema(
                    "snow_shot_pinned_edit",
                    Some(json!({"id":"pinned",
                "expected_revision":1,"action":"tool_style","payload":{
                    "target":"angle","style":style}}))
                )
                .is_err(),
                "pinned: {style}"
            );
        }
        for target in [
            "distance",
            "arrow",
            "text",
            "rectangle",
            "watermark",
            "brush_eraser",
        ] {
            for (name, owner) in [
                ("snow_shot_screenshot_set_tool_style", "session_id"),
                ("snow_shot_document_set_tool_style", "document_id"),
            ] {
                assert!(
                    schema(
                        name,
                        Some(json!({owner:"owned","expected_revision":1,
                    "target":target,"style":{"unit":"degrees"}}))
                    )
                    .is_err(),
                    "{name}: {target}"
                );
            }
        }
        for points in [
            json!([]),
            json!([[0, 0]]),
            json!([[0, 0], [10, 10]]),
            json!([[0, 0], [10, 10], [20, 0], [30, 30]]),
            json!([[0, 0], [10, 10], [20]]),
        ] {
            for (name, owner) in [
                ("snow_shot_screenshot_apply_annotations", "session_id"),
                ("snow_shot_document_apply_annotations", "document_id"),
            ] {
                assert!(
                    schema(
                        name,
                        Some(json!({owner:"owned","expected_revision":1,"operations":[{
                    "type":"angle","points":points}]}))
                    )
                    .is_err(),
                    "{name}: {points}"
                );
            }
        }
        for value in [json!(0), json!("true")] {
            assert!(
                schema(
                    "snow_shot_screenshot_apply_annotations",
                    Some(json!({
                        "session_id":"owned","expected_revision":1,"operations":[{
                            "type":"angle","points":[[100,0],[0,0],[0,100]],"full_turn":value}]
                    }))
                )
                .is_err()
            );
        }
    }

    #[test]
    fn angle_capability_snapshots_include_the_tool_and_style_sources() {
        for source in [
            include_str!("../../../mcp-capabilities.json"),
            include_str!("../../../mcp-capabilities-mini.json"),
        ] {
            let snapshot: Value = serde_json::from_str(source).unwrap();
            let contracts = &snapshot["surface_contracts"];
            assert_eq!(contracts["canvas_tools"]["members"]["Angle"], "Angle");
            for source in ["DefaultAngle", "SelectedAngle"] {
                assert!(
                    contracts["canvas_style_sources"]["members"]
                        .as_array()
                        .unwrap()
                        .iter()
                        .any(|member| member == source)
                );
            }
        }
    }

    #[test]
    fn workflow_schemas_are_typed() {
        for ratio in [0.5, 0.6, 1.0] {
            assert!(schema("snow_shot_screenshot_set_tool_style", Some(json!({"session_id":"s","expected_revision":1,"target":"arrow","style":{"arrow_ratio":ratio}}))).is_ok());
        }
        for ratio in [0.0, 0.4] {
            assert!(schema("snow_shot_screenshot_set_tool_style", Some(json!({"session_id":"s","expected_revision":1,"target":"arrow","style":{"arrow_ratio":ratio}}))).is_err());
        }
        assert!(schema("snow_shot_screenshot_set_tool_style", Some(json!({"session_id":"s","expected_revision":1,"target":"arrow","style":{"arrow_shaft_type":"tapered","arrow_ratio":3}}))).is_ok());
        assert!(schema("snow_shot_screenshot_set_tool_style", Some(json!({"session_id":"s","expected_revision":1,"target":"arrow","style":{"arrow_ratio":3.1}}))).is_err());
        assert!(schema("snow_shot_screenshot_set_tool_style", Some(json!({"session_id":"s","expected_revision":1,"target":"text","style":{"horizontal_align":"center","vertical_align":"bottom","corner_radii":[0,1,2,3]}}))).is_ok());
        assert!(schema("snow_shot_screenshot_set_tool_style", Some(json!({"session_id":"s","expected_revision":1,"target":"serial_number","style":{"serial_type":"solid_square","number":9007199254740991_u64}}))).is_ok());
        assert!(schema("snow_shot_screenshot_set_tool_style", Some(json!({"session_id":"s","expected_revision":1,"target":"serial_number","style":{"number":9007199254740992_u64}}))).is_err());
        for direction in ["up", "down", "left", "right"] {
            assert!(
                schema(
                    "snow_shot_screenshot_scroll_once",
                    Some(json!({"session_id":"s","expected_revision":1,"direction":direction}))
                )
                .is_ok()
            );
        }
        for bad in [
            json!({"session_id":"s","expected_revision":1,"direction":"diagonal"}),
            json!({"session_id":"s","expected_revision":1,"direction":"down","count":100}),
        ] {
            assert!(schema("snow_shot_screenshot_scroll_once", Some(bad)).is_err());
        }
        assert!(
            schema(
                "snow_shot_screenshot_operation",
                Some(json!({"session_id":"s","operation_id":"o"}))
            )
            .is_ok()
        );
        assert!(
            schema(
                "snow_shot_screenshot_scrolling",
                Some(json!({"session_id":"s","expected_revision":1,"action":"execute"}))
            )
            .is_err()
        );
        assert_eq!(schema("snow_shot_screenshot_edit_recognition", Some(json!({"session_id":"s","expected_revision":1,"action":"set_cell","row":0,"column":1,"text":"value"}))).is_ok(), !crate::edition::MINI);
        assert!(
            schema(
                "snow_shot_screenshot_undo",
                Some(json!({"session_id":"s","expected_revision":1,"target":"table"}))
            )
            .is_ok()
        );
        assert!(schema("not_a_tool", None).is_err());
    }
    #[test]
    fn typed_inputs_require_revision_and_reject_unknown_operations() {
        assert!(schema("snow_shot_screenshot_undo", Some(json!({"session_id":"s"}))).is_err());
        assert!(schema("snow_shot_screenshot_apply_annotations",Some(json!({"session_id":"s","expected_revision":2,"operations":[{"type":"execute"}]}))).is_err());
        assert!(schema("snow_shot_screenshot_set_selection",Some(json!({"session_id":"s","expected_revision":2,"type":"rectangle","bounds":[0,0,20,20]}))).is_ok());
        assert!(schema("snow_shot_screenshot_apply_annotations",Some(json!({"session_id":"s","expected_revision":2,"operations":[{"type":"rectangle","bounds":[0,0,20,20],"style":{}}]}))).is_ok());
        let finish = schema(
            "snow_shot_screenshot_finish",
            Some(json!({
                "session_id": "s", "expected_revision": 2, "output": "render"
            })),
        );
        assert!(finish.is_ok(), "{finish:?}");
        assert!(
            schema(
                "snow_shot_screenshot_finish",
                Some(json!({
                    "session_id": "s", "expected_revision": 2, "output": "save",
                    "path": "C:\\capture.png", "format": "png"
                }))
            )
            .is_ok()
        );
        assert!(
            schema(
                "snow_shot_screenshot_direct_capture",
                Some(json!({
                    "target": "current_monitor", "output": "render"
                }))
            )
            .is_ok()
        );
        assert!(
            schema(
                "snow_shot_screenshot_direct_capture",
                Some(json!({
                    "target": "current_monitor", "output": "save",
                    "path": "C:\\capture.png", "format": "png"
                }))
            )
            .is_ok()
        );
        assert!(
            schema(
                "snow_shot_screenshot_direct_capture",
                Some(json!({
                    "target": "current_monitor"
                }))
            )
            .is_err()
        );
    }
}

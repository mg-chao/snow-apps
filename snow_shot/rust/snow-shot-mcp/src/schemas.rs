//! Public tool inputs. The application and drawing engine additionally enforce geometry limits.
#![allow(dead_code)] // Fields are consumed by serde validation and JSON Schema generation.
use schemars::JsonSchema;
use serde::{Deserialize, de::DeserializeOwned};
use serde_json::{Map, Value};

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
    Pdf,
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
}
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Cancel {
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
        _ => model::<Mutation<Empty>>(input),
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
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

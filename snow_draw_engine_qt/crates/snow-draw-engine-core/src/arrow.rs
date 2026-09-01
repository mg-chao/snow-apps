//! Stable arrow data types used by documents, editor state, display lists, and
//! FFI conversions. Arrow routing, binding, and hit-test algorithms live in the
//! document layer; this module owns the shared contract they operate on.

use serde::{Deserialize, Serialize};

pub use crate::PathCommand as ArrowPathCommand;

pub type Point = [f64; 2];
pub type Bounds = [f64; 4];
pub type ArrowheadPoints = Vec<f64>;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum BindMode {
    Inside,
    Orbit,
    Skip,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ArrowEndpointEdge {
    Start,
    End,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArrowEndpointPosition {
    Start,
    End,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum ArrowEndpointSelector {
    Start,
    End,
    StartBinding,
    EndBinding,
}

pub fn normalize_arrow_endpoint_edge(edge: ArrowEndpointSelector) -> ArrowEndpointEdge {
    match edge {
        ArrowEndpointSelector::Start | ArrowEndpointSelector::StartBinding => {
            ArrowEndpointEdge::Start
        }
        ArrowEndpointSelector::End | ArrowEndpointSelector::EndBinding => ArrowEndpointEdge::End,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Arrowhead {
    #[serde(rename = "arrow")]
    Arrow,
    #[serde(rename = "bar")]
    Bar,
    #[serde(rename = "dot")]
    Dot,
    #[serde(rename = "circle")]
    Circle,
    #[serde(rename = "circle_outline")]
    CircleOutline,
    #[serde(rename = "triangle")]
    Triangle,
    #[serde(rename = "triangle_outline")]
    TriangleOutline,
    #[serde(rename = "diamond")]
    Diamond,
    #[serde(rename = "diamond_outline")]
    DiamondOutline,
    #[serde(rename = "crowfoot_one")]
    CrowfootOne,
    #[serde(rename = "crowfoot_many")]
    CrowfootMany,
    #[serde(rename = "crowfoot_one_or_many")]
    CrowfootOneOrMany,
    #[serde(rename = "square")]
    Square,
    #[serde(rename = "invertedTriangle")]
    InvertedTriangle,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum StrokeStyle {
    #[default]
    Solid,
    Dashed,
    Dotted,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ArrowType {
    #[default]
    Straight,
    Curve,
    Elbow,
}

impl ArrowType {
    pub const fn is_elbow(self) -> bool {
        matches!(self, Self::Elbow)
    }

    pub const fn is_curve(self) -> bool {
        matches!(self, Self::Curve)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum ArrowheadDashMode {
    #[serde(rename = "inherit")]
    Inherit,
    #[serde(rename = "solid")]
    Solid,
    #[serde(rename = "dotted-cap")]
    DottedCap,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ArrowheadFillMode {
    Stroke,
    Background,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ArrowheadPrimitiveKind {
    Line,
    Polygon,
    Circle,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ArrowheadLinePrimitive {
    pub kind: ArrowheadPrimitiveKind,
    pub from: Point,
    pub to: Point,
    pub dash_mode: ArrowheadDashMode,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub roughness_cap: Option<f64>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ArrowheadPolygonPrimitive {
    pub kind: ArrowheadPrimitiveKind,
    pub points: Vec<Point>,
    pub fill_mode: ArrowheadFillMode,
    pub dash_mode: ArrowheadDashMode,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub roughness_cap: Option<f64>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ArrowheadCirclePrimitive {
    pub kind: ArrowheadPrimitiveKind,
    pub center: Point,
    pub diameter: f64,
    pub fill_mode: ArrowheadFillMode,
    pub dash_mode: ArrowheadDashMode,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub roughness_cap: Option<f64>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(untagged)]
pub enum ArrowheadRenderPrimitive {
    Line(ArrowheadLinePrimitive),
    Polygon(ArrowheadPolygonPrimitive),
    Circle(ArrowheadCirclePrimitive),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum BindableShape {
    Rectangle,
    Ellipse,
    Diamond,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct CurvePathOp {
    pub op: String,
    pub data: Vec<f64>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct FixedSegment {
    pub start: Point,
    pub end: Point,
    pub index: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EngineContext {
    pub zoom: f64,
    pub is_binding_enabled: bool,
    pub bind_mode: BindMode,
    pub max_coordinate: f64,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PartialEngineContext {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub zoom: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub is_binding_enabled: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bind_mode: Option<BindMode>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_coordinate: Option<f64>,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ComputeEndpointDragOptions {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub new_arrow: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub alt_key: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub angle_locked: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub finalize: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub complex_bindings: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub initial_binding: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub preserve_opposite_inside_binding: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub opposite_orbit_focus_point: Option<Point>,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RecomputeAfterBindableChangeOptions {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub move_mid_points_with_element: Option<bool>,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateElbowArrowOptions {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub is_dragging: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub validate_invariants: Option<bool>,
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FocusPointContext {
    pub zoom: f64,
    pub is_binding_enabled: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FocusPointOptions {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ignore_overlap: Option<bool>,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ComputeFocusPointDragOptions {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub switch_to_inside_binding: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub grid_size: Option<f64>,
}

pub const DEFAULT_ENGINE_CONTEXT: EngineContext = EngineContext {
    zoom: 1.0,
    is_binding_enabled: true,
    bind_mode: BindMode::Orbit,
    max_coordinate: 1e6,
};

pub fn normalize_engine_context(context: Option<&PartialEngineContext>) -> EngineContext {
    let Some(context) = context else {
        return DEFAULT_ENGINE_CONTEXT;
    };

    EngineContext {
        zoom: context
            .zoom
            .filter(|zoom| zoom.is_finite())
            .unwrap_or(DEFAULT_ENGINE_CONTEXT.zoom),
        is_binding_enabled: context
            .is_binding_enabled
            .unwrap_or(DEFAULT_ENGINE_CONTEXT.is_binding_enabled),
        bind_mode: context
            .bind_mode
            .unwrap_or(DEFAULT_ENGINE_CONTEXT.bind_mode),
        max_coordinate: context
            .max_coordinate
            .filter(|max_coordinate| max_coordinate.is_finite())
            .unwrap_or(DEFAULT_ENGINE_CONTEXT.max_coordinate),
    }
}

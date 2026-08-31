use super::*;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowArrowPoint {
    pub x: f64,
    pub y: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowArrowPathCommand {
    pub kind: SnowArrowPathCommandKind,
    pub reserved0: u32,
    pub point: SnowArrowPoint,
    pub control1: SnowArrowPoint,
    pub control2: SnowArrowPoint,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowArrowheadPrimitive {
    pub kind: SnowArrowheadPrimitiveKind,
    pub fill_mode: SnowArrowheadFillMode,
    pub dash_mode: SnowArrowheadDashMode,
    pub point_count: u32,
    pub points: [SnowArrowPoint; SNOW_ARROWHEAD_PRIMITIVE_POINT_CAPACITY],
    pub center: SnowArrowPoint,
    pub diameter: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowArrowhead {
    None = 0,
    Arrow = 1,
    Bar = 2,
    Dot = 3,
    Circle = 4,
    CircleOutline = 5,
    Triangle = 6,
    TriangleOutline = 7,
    Diamond = 8,
    DiamondOutline = 9,
    CrowfootOne = 10,
    CrowfootMany = 11,
    CrowfootOneOrMany = 12,
    Square = 13,
    InvertedTriangle = 14,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowArrowType {
    Straight = 0,
    Curve = 1,
    Elbow = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SnowArrowPathCommandKind {
    #[default]
    None = 0,
    MoveTo = 1,
    LineTo = 2,
    QuadTo = 3,
    CubicTo = 4,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SnowArrowheadPrimitiveKind {
    #[default]
    None = 0,
    Line = 1,
    Polygon = 2,
    Circle = 3,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SnowArrowheadFillMode {
    #[default]
    Stroke = 0,
    Background = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SnowArrowheadDashMode {
    #[default]
    Inherit = 0,
    Solid = 1,
    DottedCap = 2,
}

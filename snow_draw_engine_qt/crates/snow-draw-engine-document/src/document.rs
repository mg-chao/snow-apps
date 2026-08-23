use std::collections::{HashMap, HashSet};

use serde::{Deserialize, Serialize};

use crate::{
    ApplyResult, ArrowData, ChangeSet, DocumentDelta, ElementChangeSnapshot, FreeDrawData,
    Operation, Transaction, arrow_bounds, arrow_hit_test, arrow_is_degenerate,
    document_geometry::{
        element_visible_bounds, filter_bounds, filter_rect_proxy, normalize_font_family,
        pen_filter_bounds, rect_bounds, rectangle_hit_test, resolve_serial_number_diameter,
        serial_number_bounds, serial_number_hit_test, serial_number_rect_proxy, text_bounds,
        text_hit_test, validate_element_data, validate_filter, validate_serial_number,
        validate_text,
    },
    element_schema::{
        FILTER_TYPE_ID, PEN_FILTER_TYPE_ID, RECTANGLE_HIGHLIGHT_TYPE_ID, RECTANGLE_TYPE_ID,
        SERIAL_NUMBER_TYPE_ID, SPOTLIGHT_TYPE_ID, TEXT_TYPE_ID, field,
    },
    free_draw_bounds, free_draw_hit_test,
};
use serde_json::{Map, Number, Value};
pub use snow_draw_engine_core::arrow::StrokeStyle;
use snow_draw_engine_core::{
    Camera, ColorRgba8, CornerRadii, DrawRect, ErrorCode, Point, validate_camera,
};

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct WatermarkConfig {
    pub color: ColorRgba8,
    pub text: String,
    pub font_size: f64,
    pub font_family: String,
    pub angle: f64,
    pub gap: f64,
    pub opacity: f64,
}

impl Default for WatermarkConfig {
    fn default() -> Self {
        Self {
            color: ColorRgba8 {
                r: 0x00,
                g: 0x00,
                b: 0x00,
                a: 0xff,
            },
            text: String::new(),
            font_size: 12.0,
            font_family: String::new(),
            angle: 30.0,
            gap: 56.0,
            opacity: 0.16,
        }
    }
}

impl WatermarkConfig {
    pub fn normalized(mut self) -> Self {
        self.text = self.text.trim().to_owned();
        self.font_family = self.font_family.trim().to_owned();
        self.gap = if self.gap.is_finite() {
            self.gap.clamp(10.0, 200.0)
        } else {
            56.0
        };
        self.opacity = if self.opacity.is_nan() {
            0.0
        } else {
            self.opacity.clamp(0.0, 1.0)
        };
        self
    }

    pub fn effective_alpha(&self) -> f64 {
        (f64::from(self.color.a) / 255.0) * self.opacity.clamp(0.0, 1.0)
    }

    pub fn is_visible(&self) -> bool {
        !self.text.trim().is_empty() && self.effective_alpha() >= 0.004
    }
}

pub fn validate_watermark_config(config: &WatermarkConfig) -> Result<(), ErrorCode> {
    if config.font_size.is_finite()
        && config.font_size > 0.0
        && config.angle.is_finite()
        && config.gap.is_finite()
        && config.opacity.is_finite()
    {
        Ok(())
    } else {
        Err(ErrorCode::InvalidArgument)
    }
}

#[derive(
    Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize,
)]
pub struct ElementId {
    pub index: u32,
    pub generation: u32,
}

impl ElementId {
    pub fn stable_key(self) -> String {
        format!("{}:{}", self.index, self.generation)
    }

    pub fn from_stable_key(key: &str) -> Option<Self> {
        let (index, generation) = key.split_once(':')?;
        Some(Self {
            index: index.parse().ok()?,
            generation: generation.parse().ok()?,
        })
    }
}

pub fn element_id_key(id: ElementId) -> String {
    id.stable_key()
}

pub fn element_id_from_key(key: &str) -> Option<ElementId> {
    ElementId::from_stable_key(key)
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct DocumentRevision(pub u64);

#[derive(Clone, Copy, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct TextLayoutRect {
    pub center: Point<f64>,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct TextLayoutSize {
    /// Exact text layout width supplied by the host renderer.
    pub width: f64,
    /// Exact text layout height supplied by the host renderer.
    pub height: f64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct SerialNumberTextConnection {
    pub start: Point<f64>,
    pub end: Point<f64>,
    pub text_baseline_start: Option<Point<f64>>,
    pub text_baseline_end: Option<Point<f64>>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SerialNumberTextBinding {
    pub serial_id: ElementId,
    pub text_id: ElementId,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ElementMeta {
    pub visible: bool,
    pub locked: bool,
}

impl Default for ElementMeta {
    fn default() -> Self {
        Self {
            visible: true,
            locked: false,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct RectangleData {
    pub rectangle_kind: RectangleElementKind,
    pub highlight_shape: HighlightShape,
    pub center: Point<f64>,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub fill: ColorRgba8,
    pub fill_style: FillStyle,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub stroke_style: StrokeStyle,
    pub corner_radii: CornerRadii,
    pub opacity: f64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum CanvasFilterType {
    #[default]
    Mosaic,
    GaussianBlur,
    Grayscale,
    Inversion,
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct FilterData {
    pub center: Point<f64>,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub filter_type: CanvasFilterType,
    pub strength: f64,
    pub opacity: f64,
}

impl Default for FilterData {
    fn default() -> Self {
        Self {
            center: Point::default(),
            width: 1.0,
            height: 1.0,
            rotation: 0.0,
            filter_type: CanvasFilterType::Mosaic,
            strength: 0.5,
            opacity: 1.0,
        }
    }
}

impl FilterData {
    pub fn normalized_strength(value: f64) -> f64 {
        if value.is_nan() {
            1.0
        } else if value.is_finite() {
            value.clamp(0.0, 1.0)
        } else if value.is_sign_negative() {
            0.0
        } else {
            1.0
        }
    }

    pub fn with_strength(mut self, strength: f64) -> Self {
        self.strength = Self::normalized_strength(strength);
        self
    }

    pub fn to_json_payload(&self) -> Result<Map<String, Value>, ErrorCode> {
        validate_filter(self)?;
        let mut payload = Map::new();
        payload.insert(
            field::TYPE_ID.to_owned(),
            Value::String(FILTER_TYPE_ID.to_owned()),
        );
        payload.insert(
            field::FILTER_TYPE.to_owned(),
            Value::String(
                match self.filter_type {
                    CanvasFilterType::Mosaic => "mosaic",
                    CanvasFilterType::GaussianBlur => "gaussianBlur",
                    CanvasFilterType::Grayscale => "grayscale",
                    CanvasFilterType::Inversion => "inversion",
                }
                .to_owned(),
            ),
        );
        payload.insert(
            field::STRENGTH.to_owned(),
            Value::Number(
                Number::from_f64(Self::normalized_strength(self.strength))
                    .ok_or(ErrorCode::InvalidArgument)?,
            ),
        );
        Ok(payload)
    }

    pub fn from_json_payload(
        payload: &Map<String, Value>,
        center: Point<f64>,
        width: f64,
        height: f64,
    ) -> Result<Self, ErrorCode> {
        if payload.len() != 3
            || payload.get(field::TYPE_ID).and_then(Value::as_str) != Some(FILTER_TYPE_ID)
        {
            return Err(ErrorCode::InvalidArgument);
        }
        let filter_type = match payload.get(field::FILTER_TYPE).and_then(Value::as_str) {
            Some("mosaic") => CanvasFilterType::Mosaic,
            Some("gaussianBlur") => CanvasFilterType::GaussianBlur,
            Some("grayscale") => CanvasFilterType::Grayscale,
            Some("inversion") => CanvasFilterType::Inversion,
            _ => return Err(ErrorCode::InvalidArgument),
        };
        let strength = payload
            .get(field::STRENGTH)
            .and_then(Value::as_f64)
            .ok_or(ErrorCode::InvalidArgument)?;
        let filter = Self {
            center,
            width,
            height,
            rotation: 0.0,
            filter_type,
            strength: Self::normalized_strength(strength),
            opacity: 1.0,
        };
        validate_filter(&filter)?;
        Ok(filter)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct SpotlightConfig {
    pub color: ColorRgba8,
    pub opacity: f64,
}

impl Default for SpotlightConfig {
    fn default() -> Self {
        Self {
            color: ColorRgba8 {
                r: 0,
                g: 0,
                b: 0,
                a: 0xff,
            },
            opacity: 0.64,
        }
    }
}

impl SpotlightConfig {
    pub fn normalized(mut self) -> Self {
        self.opacity = if self.opacity.is_nan() {
            0.0
        } else {
            self.opacity.clamp(0.0, 1.0)
        };
        self
    }

    pub fn effective_alpha(self) -> f64 {
        f64::from(self.color.a) / 255.0 * self.opacity.clamp(0.0, 1.0)
    }
}

pub fn validate_spotlight_config(config: &SpotlightConfig) -> Result<(), ErrorCode> {
    if config.opacity.is_finite() {
        Ok(())
    } else {
        Err(ErrorCode::InvalidArgument)
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct PenFilterData {
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    /// Points normalized to the unrotated element bounds.
    pub points: Vec<[f64; 2]>,
    pub filter_type: CanvasFilterType,
    pub strength: f64,
    pub stroke_width: f64,
    pub opacity: f64,
}

impl Default for PenFilterData {
    fn default() -> Self {
        Self {
            x: 0.0,
            y: 0.0,
            width: 1.0,
            height: 1.0,
            rotation: 0.0,
            points: vec![[0.0, 0.0], [1.0, 1.0]],
            filter_type: CanvasFilterType::Mosaic,
            strength: 0.5,
            stroke_width: 30.0,
            opacity: 1.0,
        }
    }
}

impl PenFilterData {
    pub fn from_global_points(
        points: &[Point<f64>],
        filter_type: CanvasFilterType,
        strength: f64,
        stroke_width: f64,
        opacity: f64,
    ) -> Option<Self> {
        if points.len() < 2
            || points
                .iter()
                .any(|point| !point.x.is_finite() || !point.y.is_finite())
        {
            return None;
        }
        let min_x = points
            .iter()
            .map(|point| point.x)
            .fold(f64::INFINITY, f64::min);
        let min_y = points
            .iter()
            .map(|point| point.y)
            .fold(f64::INFINITY, f64::min);
        let max_x = points
            .iter()
            .map(|point| point.x)
            .fold(f64::NEG_INFINITY, f64::max);
        let max_y = points
            .iter()
            .map(|point| point.y)
            .fold(f64::NEG_INFINITY, f64::max);
        let width = max_x - min_x;
        let height = max_y - min_y;
        let normalize = |value: f64, min: f64, span: f64| {
            if span > 0.0 {
                (value - min) / span
            } else {
                0.0
            }
        };
        let pen_filter = Self {
            x: min_x,
            y: min_y,
            width,
            height,
            rotation: 0.0,
            points: points
                .iter()
                .map(|point| {
                    [
                        normalize(point.x, min_x, width),
                        normalize(point.y, min_y, height),
                    ]
                })
                .collect(),
            filter_type,
            strength: FilterData::normalized_strength(strength),
            stroke_width,
            opacity,
        };
        crate::validate_pen_filter(&pen_filter)
            .ok()
            .map(|_| pen_filter)
    }

    pub fn center(&self) -> Point<f64> {
        Point::new(self.x + self.width / 2.0, self.y + self.height / 2.0)
    }

    pub fn global_points(&self) -> Vec<Point<f64>> {
        let center = self.center();
        let cosine = self.rotation.cos();
        let sine = self.rotation.sin();
        self.points
            .iter()
            .map(|point| {
                let x = self.x + point[0] * self.width;
                let y = self.y + point[1] * self.height;
                let dx = x - center.x;
                let dy = y - center.y;
                Point::new(
                    center.x + dx * cosine - dy * sine,
                    center.y + dx * sine + dy * cosine,
                )
            })
            .collect()
    }

    pub fn to_json_payload(&self) -> Result<Map<String, Value>, ErrorCode> {
        crate::validate_pen_filter(self)?;
        let mut payload = Map::new();
        payload.insert(
            field::TYPE_ID.to_owned(),
            Value::String(PEN_FILTER_TYPE_ID.to_owned()),
        );
        payload.insert(
            field::POINTS.to_owned(),
            Value::Array(
                self.points
                    .iter()
                    .map(|point| {
                        let mut value = Map::new();
                        value.insert(
                            "x".to_owned(),
                            Value::Number(Number::from_f64(point[0]).unwrap()),
                        );
                        value.insert(
                            "y".to_owned(),
                            Value::Number(Number::from_f64(point[1]).unwrap()),
                        );
                        Value::Object(value)
                    })
                    .collect(),
            ),
        );
        payload.insert(
            field::FILTER_TYPE.to_owned(),
            Value::String(
                match self.filter_type {
                    CanvasFilterType::Mosaic => "mosaic",
                    CanvasFilterType::GaussianBlur => "gaussianBlur",
                    CanvasFilterType::Grayscale => "grayscale",
                    CanvasFilterType::Inversion => "inversion",
                }
                .to_owned(),
            ),
        );
        payload.insert(
            field::STRENGTH.to_owned(),
            Value::Number(
                Number::from_f64(FilterData::normalized_strength(self.strength)).unwrap(),
            ),
        );
        payload.insert(
            field::STROKE_WIDTH.to_owned(),
            Value::Number(Number::from_f64(self.stroke_width).unwrap()),
        );
        Ok(payload)
    }

    pub fn from_json_payload(
        payload: &Map<String, Value>,
        bounds: DrawRect,
    ) -> Result<Self, ErrorCode> {
        if payload.len() != 5
            || payload.get(field::TYPE_ID).and_then(Value::as_str) != Some(PEN_FILTER_TYPE_ID)
            || ![bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y]
                .into_iter()
                .all(f64::is_finite)
            || bounds.max_x < bounds.min_x
            || bounds.max_y < bounds.min_y
        {
            return Err(ErrorCode::InvalidArgument);
        }
        let point_values = payload
            .get(field::POINTS)
            .and_then(Value::as_array)
            .ok_or(ErrorCode::InvalidArgument)?;
        if point_values.len() < 2 {
            return Err(ErrorCode::InvalidArgument);
        }
        let points = point_values
            .iter()
            .map(|value| {
                let point = value.as_object().ok_or(ErrorCode::InvalidArgument)?;
                if point.len() != 2 {
                    return Err(ErrorCode::InvalidArgument);
                }
                let x = point
                    .get("x")
                    .and_then(Value::as_f64)
                    .ok_or(ErrorCode::InvalidArgument)?;
                let y = point
                    .get("y")
                    .and_then(Value::as_f64)
                    .ok_or(ErrorCode::InvalidArgument)?;
                if !(0.0..=1.0).contains(&x) || !(0.0..=1.0).contains(&y) {
                    return Err(ErrorCode::InvalidArgument);
                }
                Ok([x, y])
            })
            .collect::<Result<Vec<_>, ErrorCode>>()?;
        let filter_type = match payload.get(field::FILTER_TYPE).and_then(Value::as_str) {
            Some("mosaic") => CanvasFilterType::Mosaic,
            Some("gaussianBlur") => CanvasFilterType::GaussianBlur,
            Some("grayscale") => CanvasFilterType::Grayscale,
            Some("inversion") => CanvasFilterType::Inversion,
            _ => return Err(ErrorCode::InvalidArgument),
        };
        let pen_filter = Self {
            x: bounds.min_x,
            y: bounds.min_y,
            width: bounds.max_x - bounds.min_x,
            height: bounds.max_y - bounds.min_y,
            rotation: 0.0,
            points,
            filter_type,
            strength: FilterData::normalized_strength(
                payload
                    .get(field::STRENGTH)
                    .and_then(Value::as_f64)
                    .ok_or(ErrorCode::InvalidArgument)?,
            ),
            stroke_width: payload
                .get(field::STROKE_WIDTH)
                .and_then(Value::as_f64)
                .ok_or(ErrorCode::InvalidArgument)?,
            opacity: 1.0,
        };
        crate::validate_pen_filter(&pen_filter)?;
        Ok(pen_filter)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum RectangleElementKind {
    #[default]
    Rectangle,
    RectangleHighlight,
    Spotlight,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum HighlightShape {
    #[default]
    Rectangle,
    Ellipse,
    Diamond,
}

impl RectangleData {
    pub fn to_rectangle_json_payload(&self) -> Result<Map<String, Value>, ErrorCode> {
        if self.is_highlight()
            || self.corner_radii != CornerRadii::splat(self.corner_radii.top_left)
        {
            return Err(ErrorCode::InvalidArgument);
        }
        crate::validate_rectangle(self)?;

        let mut payload = Map::new();
        payload.insert(
            field::TYPE_ID.to_owned(),
            Value::String(RECTANGLE_TYPE_ID.to_owned()),
        );
        payload.insert(
            field::CORNER_RADIUS.to_owned(),
            number_value(self.corner_radii.top_left),
        );
        payload.insert(
            field::SHAPE.to_owned(),
            Value::String(
                match self.highlight_shape {
                    HighlightShape::Rectangle => "rectangle",
                    HighlightShape::Ellipse => "ellipse",
                    HighlightShape::Diamond => "diamond",
                }
                .to_owned(),
            ),
        );
        payload.insert(field::FILL_COLOR.to_owned(), argb_value(self.fill));
        payload.insert(field::COLOR.to_owned(), argb_value(self.stroke));
        payload.insert(
            field::STROKE_WIDTH.to_owned(),
            number_value(self.stroke_width),
        );
        payload.insert(
            field::STROKE_STYLE.to_owned(),
            Value::String(stroke_style_name(self.stroke_style).to_owned()),
        );
        payload.insert(
            field::FILL_STYLE.to_owned(),
            Value::String(fill_style_name(self.fill_style).to_owned()),
        );
        Ok(payload)
    }

    pub fn from_rectangle_json_payload(
        payload: &Map<String, Value>,
        center: Point<f64>,
        width: f64,
        height: f64,
    ) -> Result<Self, ErrorCode> {
        expect_type_id(payload, RECTANGLE_TYPE_ID)?;
        let rectangle = Self {
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: match payload.get(field::SHAPE).and_then(Value::as_str) {
                None | Some("rectangle") => HighlightShape::Rectangle,
                Some("ellipse") => HighlightShape::Ellipse,
                Some("diamond") => HighlightShape::Diamond,
                _ => return Err(ErrorCode::InvalidArgument),
            },
            center,
            width,
            height,
            rotation: 0.0,
            fill: required_color(payload, field::FILL_COLOR)?,
            fill_style: decode_fill_style(required_string(payload, field::FILL_STYLE)?)?,
            stroke: required_color(payload, field::COLOR)?,
            stroke_width: required_f64(payload, field::STROKE_WIDTH)?,
            stroke_style: decode_stroke_style(required_string(payload, field::STROKE_STYLE)?)?,
            corner_radii: CornerRadii::splat(required_f64(payload, field::CORNER_RADIUS)?),
            opacity: 1.0,
        };
        crate::validate_rectangle(&rectangle)?;
        Ok(rectangle)
    }

    pub fn into_highlight(mut self, shape: HighlightShape) -> Self {
        debug_assert!(shape != HighlightShape::Diamond);
        self.rectangle_kind = RectangleElementKind::RectangleHighlight;
        self.highlight_shape = shape;
        self.corner_radii = CornerRadii::default();
        self.fill_style = FillStyle::Solid;
        self.stroke_style = StrokeStyle::Solid;
        self
    }

    pub fn is_highlight(&self) -> bool {
        self.rectangle_kind == RectangleElementKind::RectangleHighlight
    }

    pub fn into_spotlight(mut self) -> Self {
        self.rectangle_kind = RectangleElementKind::Spotlight;
        self.highlight_shape = HighlightShape::Rectangle;
        self.fill = ColorRgba8::default();
        self.stroke = ColorRgba8::default();
        self.stroke_width = 0.0;
        self.corner_radii = CornerRadii::default();
        self.fill_style = FillStyle::Solid;
        self.stroke_style = StrokeStyle::Solid;
        self.opacity = 1.0;
        self
    }

    pub fn is_spotlight(&self) -> bool {
        self.rectangle_kind == RectangleElementKind::Spotlight
    }

    pub fn supports_corner_radius(&self) -> bool {
        self.rectangle_kind == RectangleElementKind::Rectangle
            && self.highlight_shape == HighlightShape::Rectangle
    }

    pub fn element_kind(&self) -> ElementKind {
        if self.is_highlight() {
            ElementKind::RectangleHighlight
        } else if self.is_spotlight() {
            ElementKind::Spotlight
        } else {
            ElementKind::Rectangle
        }
    }

    pub fn to_highlight_json_payload(&self) -> Result<Map<String, Value>, ErrorCode> {
        if !self.is_highlight() || !self.stroke_width.is_finite() {
            return Err(ErrorCode::InvalidArgument);
        }
        let mut payload = Map::new();
        payload.insert(
            field::TYPE_ID.to_owned(),
            Value::String(RECTANGLE_HIGHLIGHT_TYPE_ID.to_owned()),
        );
        payload.insert(field::COLOR.to_owned(), argb_value(self.fill));
        payload.insert(field::STROKE_COLOR.to_owned(), argb_value(self.stroke));
        payload.insert(
            field::STROKE_WIDTH.to_owned(),
            Value::Number(Number::from_f64(self.stroke_width).ok_or(ErrorCode::InvalidArgument)?),
        );
        Ok(payload)
    }

    pub fn from_highlight_json_payload(
        payload: &Map<String, Value>,
        center: Point<f64>,
        width: f64,
        height: f64,
    ) -> Result<Self, ErrorCode> {
        if payload.get(field::TYPE_ID).and_then(Value::as_str) != Some(RECTANGLE_HIGHLIGHT_TYPE_ID)
        {
            return Err(ErrorCode::InvalidArgument);
        }
        let color = required_color(payload, field::COLOR)?;
        let stroke = required_color(payload, field::STROKE_COLOR)?;
        let stroke_width = payload
            .get(field::STROKE_WIDTH)
            .and_then(Value::as_f64)
            .filter(|value| value.is_finite() && *value >= 0.0)
            .ok_or(ErrorCode::InvalidArgument)?;
        let highlight = RectangleData {
            rectangle_kind: RectangleElementKind::RectangleHighlight,
            highlight_shape: HighlightShape::Rectangle,
            center,
            width,
            height,
            rotation: 0.0,
            fill: color,
            fill_style: FillStyle::Solid,
            stroke,
            stroke_width,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        crate::validate_rectangle(&highlight)?;
        Ok(highlight)
    }

    pub fn to_spotlight_json_payload(&self) -> Result<Map<String, Value>, ErrorCode> {
        if !self.is_spotlight() || self.opacity != 1.0 {
            return Err(ErrorCode::InvalidArgument);
        }
        crate::validate_rectangle(self)?;
        let mut payload = Map::new();
        payload.insert(
            field::TYPE_ID.to_owned(),
            Value::String(SPOTLIGHT_TYPE_ID.to_owned()),
        );
        Ok(payload)
    }

    pub fn from_spotlight_json_payload(
        payload: &Map<String, Value>,
        center: Point<f64>,
        width: f64,
        height: f64,
    ) -> Result<Self, ErrorCode> {
        expect_type_id(payload, SPOTLIGHT_TYPE_ID)?;
        if payload.len() != 1 {
            return Err(ErrorCode::InvalidArgument);
        }
        let spotlight = RectangleData {
            rectangle_kind: RectangleElementKind::Spotlight,
            highlight_shape: HighlightShape::Rectangle,
            center,
            width,
            height,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        crate::validate_rectangle(&spotlight)?;
        Ok(spotlight)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum TextHorizontalAlign {
    #[default]
    Left,
    Center,
    Right,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum TextVerticalAlign {
    Top,
    #[default]
    Center,
    Bottom,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum FillStyle {
    Line,
    CrossLine,
    #[default]
    Solid,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct TextData {
    pub center: Point<f64>,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub text: String,
    pub color: ColorRgba8,
    pub font_size: f64,
    pub font_family: Option<String>,
    pub fill: ColorRgba8,
    pub fill_style: FillStyle,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub corner_radii: CornerRadii,
    pub horizontal_align: TextHorizontalAlign,
    pub vertical_align: TextVerticalAlign,
    pub auto_resize: bool,
    pub opacity: f64,
}

impl TextData {
    pub fn to_json_payload(&self) -> Map<String, Value> {
        let mut payload = Map::new();
        payload.insert(
            field::TYPE_ID.to_owned(),
            Value::String(TEXT_TYPE_ID.to_owned()),
        );
        payload.insert(field::TEXT.to_owned(), Value::String(self.text.clone()));
        payload.insert(field::COLOR.to_owned(), argb_value(self.color));
        payload.insert(field::FONT_SIZE.to_owned(), number_value(self.font_size));
        payload.insert(
            field::FONT_FAMILY.to_owned(),
            Value::String(self.font_family.clone().unwrap_or_default()),
        );
        payload.insert(
            field::HORIZONTAL_ALIGN.to_owned(),
            Value::String(text_horizontal_align_name(self.horizontal_align).to_owned()),
        );
        payload.insert(
            field::VERTICAL_ALIGN.to_owned(),
            Value::String(text_vertical_align_name(self.vertical_align).to_owned()),
        );
        payload.insert(field::FILL_COLOR.to_owned(), argb_value(self.fill));
        payload.insert(
            field::FILL_STYLE.to_owned(),
            Value::String(fill_style_name(self.fill_style).to_owned()),
        );
        payload.insert(field::STROKE_COLOR.to_owned(), argb_value(self.stroke));
        payload.insert(
            field::STROKE_WIDTH.to_owned(),
            number_value(self.stroke_width),
        );
        payload.insert(
            field::CORNER_RADIUS.to_owned(),
            number_value(self.corner_radii.top_left),
        );
        payload.insert(field::AUTO_RESIZE.to_owned(), Value::Bool(self.auto_resize));
        payload
    }

    pub fn from_json_payload(payload: &Map<String, Value>) -> Result<Self, ErrorCode> {
        expect_type_id(payload, TEXT_TYPE_ID)?;
        let mut text = TextData {
            text: required_string(payload, field::TEXT)?.to_owned(),
            color: required_color(payload, field::COLOR)?,
            font_size: required_f64(payload, field::FONT_SIZE)?,
            font_family: normalize_font_family(required_nullable_string(
                payload,
                field::FONT_FAMILY,
            )?),
            horizontal_align: decode_text_horizontal_align(required_string(
                payload,
                field::HORIZONTAL_ALIGN,
            )?)?,
            vertical_align: decode_text_vertical_align(required_string(
                payload,
                field::VERTICAL_ALIGN,
            )?)?,
            fill: required_color(payload, field::FILL_COLOR)?,
            fill_style: decode_fill_style(required_string(payload, field::FILL_STYLE)?)?,
            stroke: required_color(payload, field::STROKE_COLOR)?,
            stroke_width: required_f64(payload, field::STROKE_WIDTH)?,
            auto_resize: required_bool(payload, field::AUTO_RESIZE)?,
            ..TextData::default()
        };
        text.corner_radii = CornerRadii::splat(required_f64(payload, field::CORNER_RADIUS)?);
        validate_text(&text)?;
        Ok(text)
    }

    pub fn from_json_value(value: &Value) -> Result<Self, ErrorCode> {
        let payload = value.as_object().ok_or(ErrorCode::InvalidArgument)?;
        Self::from_json_payload(payload)
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct SerialNumberData {
    pub center: Point<f64>,
    pub diameter: f64,
    pub rotation: f64,
    pub number: i64,
    pub color: ColorRgba8,
    pub fill: ColorRgba8,
    pub fill_style: FillStyle,
    pub font_size: f64,
    pub font_family: Option<String>,
    pub stroke_width: f64,
    pub stroke_style: StrokeStyle,
    pub opacity: f64,
    pub text_element_id: Option<ElementId>,
}

impl SerialNumberData {
    pub fn to_json_payload(&self) -> Map<String, Value> {
        let mut payload = Map::new();
        payload.insert(
            field::TYPE_ID.to_owned(),
            Value::String(SERIAL_NUMBER_TYPE_ID.to_owned()),
        );
        payload.insert(field::NUMBER.to_owned(), Value::from(self.number));
        payload.insert(field::COLOR.to_owned(), argb_value(self.color));
        payload.insert(field::FILL_COLOR.to_owned(), argb_value(self.fill));
        payload.insert(
            field::FILL_STYLE.to_owned(),
            Value::String(fill_style_name(self.fill_style).to_owned()),
        );
        payload.insert(field::FONT_SIZE.to_owned(), number_value(self.font_size));
        payload.insert(
            field::FONT_FAMILY.to_owned(),
            Value::String(self.font_family.clone().unwrap_or_default()),
        );
        payload.insert(
            field::STROKE_WIDTH.to_owned(),
            number_value(self.stroke_width),
        );
        payload.insert(
            field::STROKE_STYLE.to_owned(),
            Value::String(stroke_style_name(self.stroke_style).to_owned()),
        );
        payload.insert(
            field::TEXT_ELEMENT_ID.to_owned(),
            Value::String(
                self.text_element_id
                    .map(ElementId::stable_key)
                    .unwrap_or_default(),
            ),
        );
        payload
    }

    pub fn from_json_payload(payload: &Map<String, Value>) -> Result<Self, ErrorCode> {
        expect_type_id(payload, SERIAL_NUMBER_TYPE_ID)?;
        let number = required_i64(payload, field::NUMBER)?.max(0);
        let font_size = required_f64(payload, field::FONT_SIZE)?;
        let mut serial = SerialNumberData {
            number,
            color: required_color(payload, field::COLOR)?,
            fill: required_color(payload, field::FILL_COLOR)?,
            fill_style: decode_fill_style(required_string(payload, field::FILL_STYLE)?)?,
            font_size,
            font_family: normalize_font_family(required_nullable_string(
                payload,
                field::FONT_FAMILY,
            )?),
            stroke_width: required_f64(payload, field::STROKE_WIDTH)?,
            stroke_style: decode_stroke_style(required_string(payload, field::STROKE_STYLE)?)?,
            text_element_id: decode_optional_element_id(payload, field::TEXT_ELEMENT_ID)?,
            ..SerialNumberData::default()
        };
        serial.diameter =
            resolve_serial_number_diameter(serial.number, serial.font_size, serial.diameter);
        validate_serial_number(&serial)?;
        Ok(serial)
    }

    pub fn from_json_value(value: &Value) -> Result<Self, ErrorCode> {
        let payload = value.as_object().ok_or(ErrorCode::InvalidArgument)?;
        Self::from_json_payload(payload)
    }
}

fn argb_value(color: ColorRgba8) -> Value {
    Value::from(
        ((color.a as u64) << 24)
            | ((color.r as u64) << 16)
            | ((color.g as u64) << 8)
            | color.b as u64,
    )
}

fn color_from_argb(argb: u32) -> ColorRgba8 {
    ColorRgba8 {
        a: ((argb >> 24) & 0xff) as u8,
        r: ((argb >> 16) & 0xff) as u8,
        g: ((argb >> 8) & 0xff) as u8,
        b: (argb & 0xff) as u8,
    }
}

fn number_value(value: f64) -> Value {
    Value::Number(Number::from_f64(value).unwrap_or_else(|| Number::from(0)))
}

fn expect_type_id(payload: &Map<String, Value>, expected: &str) -> Result<(), ErrorCode> {
    if required_string(payload, field::TYPE_ID)? == expected {
        Ok(())
    } else {
        Err(ErrorCode::InvalidArgument)
    }
}

fn required_string<'a>(payload: &'a Map<String, Value>, field: &str) -> Result<&'a str, ErrorCode> {
    payload
        .get(field)
        .and_then(Value::as_str)
        .ok_or(ErrorCode::InvalidArgument)
}

fn required_nullable_string(
    payload: &Map<String, Value>,
    field: &str,
) -> Result<Option<String>, ErrorCode> {
    match payload.get(field).ok_or(ErrorCode::InvalidArgument)? {
        Value::Null => Ok(None),
        Value::String(value) => Ok(normalize_payload_string(value)),
        _ => Err(ErrorCode::InvalidArgument),
    }
}

fn normalize_payload_string(value: &str) -> Option<String> {
    let trimmed = value.trim();
    (!trimmed.is_empty()).then(|| trimmed.to_owned())
}

fn required_f64(payload: &Map<String, Value>, field: &str) -> Result<f64, ErrorCode> {
    payload
        .get(field)
        .and_then(Value::as_f64)
        .ok_or(ErrorCode::InvalidArgument)
}

fn required_i64(payload: &Map<String, Value>, field: &str) -> Result<i64, ErrorCode> {
    payload
        .get(field)
        .and_then(Value::as_i64)
        .ok_or(ErrorCode::InvalidArgument)
}

fn required_bool(payload: &Map<String, Value>, field: &str) -> Result<bool, ErrorCode> {
    payload
        .get(field)
        .and_then(Value::as_bool)
        .ok_or(ErrorCode::InvalidArgument)
}

fn required_color(payload: &Map<String, Value>, field: &str) -> Result<ColorRgba8, ErrorCode> {
    let value = payload
        .get(field)
        .and_then(Value::as_u64)
        .ok_or(ErrorCode::InvalidArgument)?;
    let argb = u32::try_from(value).map_err(|_| ErrorCode::InvalidArgument)?;
    Ok(color_from_argb(argb))
}

fn decode_optional_element_id(
    payload: &Map<String, Value>,
    field: &str,
) -> Result<Option<ElementId>, ErrorCode> {
    required_nullable_string(payload, field)?
        .map(|value| ElementId::from_stable_key(&value).ok_or(ErrorCode::InvalidArgument))
        .transpose()
}

fn fill_style_name(style: FillStyle) -> &'static str {
    match style {
        FillStyle::Line => "line",
        FillStyle::CrossLine => "crossLine",
        FillStyle::Solid => "solid",
    }
}

fn decode_fill_style(value: &str) -> Result<FillStyle, ErrorCode> {
    match value {
        "line" => Ok(FillStyle::Line),
        "crossLine" => Ok(FillStyle::CrossLine),
        "solid" => Ok(FillStyle::Solid),
        _ => Err(ErrorCode::InvalidArgument),
    }
}

fn stroke_style_name(style: StrokeStyle) -> &'static str {
    match style {
        StrokeStyle::Solid => "solid",
        StrokeStyle::Dashed => "dashed",
        StrokeStyle::Dotted => "dotted",
    }
}

fn decode_stroke_style(value: &str) -> Result<StrokeStyle, ErrorCode> {
    match value {
        "solid" => Ok(StrokeStyle::Solid),
        "dashed" => Ok(StrokeStyle::Dashed),
        "dotted" => Ok(StrokeStyle::Dotted),
        _ => Err(ErrorCode::InvalidArgument),
    }
}

fn text_horizontal_align_name(align: TextHorizontalAlign) -> &'static str {
    match align {
        TextHorizontalAlign::Left => "left",
        TextHorizontalAlign::Center => "center",
        TextHorizontalAlign::Right => "right",
    }
}

fn decode_text_horizontal_align(value: &str) -> Result<TextHorizontalAlign, ErrorCode> {
    match value {
        "left" => Ok(TextHorizontalAlign::Left),
        "center" => Ok(TextHorizontalAlign::Center),
        "right" => Ok(TextHorizontalAlign::Right),
        _ => Err(ErrorCode::InvalidArgument),
    }
}

fn text_vertical_align_name(align: TextVerticalAlign) -> &'static str {
    match align {
        TextVerticalAlign::Top => "top",
        TextVerticalAlign::Center => "center",
        TextVerticalAlign::Bottom => "bottom",
    }
}

fn decode_text_vertical_align(value: &str) -> Result<TextVerticalAlign, ErrorCode> {
    match value {
        "top" => Ok(TextVerticalAlign::Top),
        "center" => Ok(TextVerticalAlign::Center),
        "bottom" => Ok(TextVerticalAlign::Bottom),
        _ => Err(ErrorCode::InvalidArgument),
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub enum ElementData {
    Rectangle(RectangleData),
    Filter(FilterData),
    PenFilter(PenFilterData),
    Arrow(ArrowData),
    FreeDraw(FreeDrawData),
    Text(TextData),
    SerialNumber(SerialNumberData),
}

impl ElementData {
    pub fn kind(&self) -> ElementKind {
        match self {
            Self::Rectangle(rect) => rect.element_kind(),
            Self::Filter(_) => ElementKind::Filter,
            Self::PenFilter(_) => ElementKind::PenFilter,
            Self::Arrow(arrow) => arrow.element_kind(),
            Self::FreeDraw(_) => ElementKind::FreeDraw,
            Self::Text(_) => ElementKind::Text,
            Self::SerialNumber(_) => ElementKind::SerialNumber,
        }
    }

    pub(crate) fn bounds(&self) -> DrawRect {
        match self {
            Self::Rectangle(rect) => rect_bounds(rect),
            Self::Filter(filter) => filter_bounds(filter),
            Self::PenFilter(filter) => pen_filter_bounds(filter),
            Self::Arrow(arrow) => arrow_bounds(arrow),
            Self::FreeDraw(free_draw) => free_draw_bounds(free_draw),
            Self::Text(text) => text_bounds(text),
            Self::SerialNumber(serial) => serial_number_bounds(serial),
        }
    }

    /// The unpainted element rectangle corresponding to reference
    /// `ElementState.rect`. Stroke and other visual overflow are excluded.
    pub fn state_rect(&self) -> DrawRect {
        match self {
            Self::Rectangle(rect) => centered_rect(rect.center, rect.width, rect.height),
            Self::Filter(filter) => centered_rect(filter.center, filter.width, filter.height),
            Self::PenFilter(filter) => DrawRect::new(
                filter.x,
                filter.y,
                filter.x + filter.width,
                filter.y + filter.height,
            ),
            Self::Arrow(arrow) => DrawRect::new(
                arrow.x,
                arrow.y,
                arrow.x + arrow.width,
                arrow.y + arrow.height,
            ),
            Self::FreeDraw(free_draw) => DrawRect::new(
                free_draw.x,
                free_draw.y,
                free_draw.x + free_draw.width,
                free_draw.y + free_draw.height,
            ),
            Self::Text(text) => centered_rect(text.center, text.width, text.height),
            Self::SerialNumber(serial) => {
                centered_rect(serial.center, serial.diameter, serial.diameter)
            }
        }
    }

    pub fn rotation(&self) -> f64 {
        match self {
            Self::Rectangle(rect) => rect.rotation,
            Self::Filter(filter) => filter.rotation,
            Self::PenFilter(filter) => filter.rotation,
            Self::Arrow(arrow) => arrow.rotation,
            Self::FreeDraw(free_draw) => free_draw.rotation,
            Self::Text(text) => text.rotation,
            Self::SerialNumber(serial) => serial.rotation,
        }
    }

    pub fn opacity(&self) -> f64 {
        match self {
            Self::Rectangle(rect) => rect.opacity,
            Self::Filter(filter) => filter.opacity,
            Self::PenFilter(filter) => filter.opacity,
            Self::Arrow(arrow) => arrow.opacity,
            Self::FreeDraw(free_draw) => free_draw.opacity,
            Self::Text(text) => text.opacity,
            Self::SerialNumber(serial) => serial.opacity,
        }
    }
}

fn centered_rect(center: Point<f64>, width: f64, height: f64) -> DrawRect {
    DrawRect::new(
        center.x - width / 2.0,
        center.y - height / 2.0,
        center.x + width / 2.0,
        center.y + height / 2.0,
    )
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum ElementKind {
    Rectangle,
    Arrow,
    Line,
    FreeDraw,
    RectangleHighlight,
    PenHighlight,
    Filter,
    PenFilter,
    Text,
    SerialNumber,
    Spotlight,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct ElementRecord {
    pub id: ElementId,
    pub meta: ElementMeta,
    pub data: ElementData,
}

/// Canonical common fields plus the type-specific element payload.
///
/// This mirrors the reference `ElementState` boundary. The payload-specific
/// storage remains an implementation detail while editor drafts are migrated.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ElementState<'a> {
    pub id: ElementId,
    pub rect: DrawRect,
    pub rotation: f64,
    pub opacity: f64,
    pub z_index: usize,
    pub visible: bool,
    pub locked: bool,
    pub data: &'a ElementData,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Document {
    pub(crate) slots: Vec<Option<ElementRecord>>,
    pub(crate) paint_order: Vec<ElementId>,
    revision: DocumentRevision,
    next_index: u32,
    watermark: WatermarkConfig,
    spotlight: SpotlightConfig,
}

impl Document {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn validate_session(&self) -> Result<(), ErrorCode> {
        const MAX_ELEMENT_SLOTS: usize = 1_000_000;
        if self.slots.len() > MAX_ELEMENT_SLOTS || self.next_index as usize != self.slots.len() {
            return Err(ErrorCode::InvalidArgument);
        }
        validate_watermark_config(&self.watermark)?;
        validate_spotlight_config(&self.spotlight)?;

        let mut active_ids = HashSet::with_capacity(self.paint_order.len());
        for (index, slot) in self.slots.iter().enumerate() {
            let Some(element) = slot else {
                continue;
            };
            if element.id.index as usize != index || element.id.generation == 0 {
                return Err(ErrorCode::InvalidArgument);
            }
            validate_element_data(&element.data)?;
            active_ids.insert(element.id);
        }
        if active_ids.len() != self.paint_order.len() {
            return Err(ErrorCode::InvalidArgument);
        }
        let mut painted = HashSet::with_capacity(self.paint_order.len());
        for id in &self.paint_order {
            if !active_ids.contains(id) || !painted.insert(*id) {
                return Err(ErrorCode::InvalidArgument);
            }
        }

        for element in self.slots.iter().flatten() {
            let ElementData::Arrow(arrow) = &element.data else {
                continue;
            };
            for binding in [arrow.start_binding.as_ref(), arrow.end_binding.as_ref()]
                .into_iter()
                .flatten()
            {
                if binding.element_id == element.id || !active_ids.contains(&binding.element_id) {
                    return Err(ErrorCode::InvalidArgument);
                }
            }
        }
        Ok(())
    }

    pub fn has_same_session_content(&self, other: &Self) -> bool {
        self.slots == other.slots
            && self.paint_order == other.paint_order
            && self.next_index == other.next_index
            && self.watermark == other.watermark
            && self.spotlight == other.spotlight
    }

    pub fn document_revision(&self) -> DocumentRevision {
        self.revision
    }

    pub fn watermark_config(&self) -> &WatermarkConfig {
        &self.watermark
    }

    pub fn spotlight_config(&self) -> SpotlightConfig {
        self.spotlight
    }

    pub fn has_visible_spotlight(&self) -> bool {
        self.paint_order.iter().any(|id| {
            self.element(*id).is_ok_and(|element| {
                element.meta.visible
                    && matches!(&element.data, ElementData::Rectangle(rect) if rect.is_spotlight())
            })
        })
    }

    pub fn allocate_element_id(&mut self) -> ElementId {
        let id = ElementId {
            index: self.next_index,
            generation: 1,
        };
        self.next_index = self.next_index.saturating_add(1);
        id
    }

    pub fn peek_next_element_id(&self) -> ElementId {
        ElementId {
            index: self.next_index,
            generation: 1,
        }
    }

    pub fn element(&self, id: ElementId) -> Result<&ElementRecord, ErrorCode> {
        let Some(slot) = self.slots.get(id.index as usize) else {
            return Err(ErrorCode::NotFound);
        };
        let Some(element) = slot.as_ref() else {
            return Err(ErrorCode::NotFound);
        };
        if element.id != id {
            return Err(ErrorCode::NotFound);
        }
        Ok(element)
    }

    pub fn element_state(&self, id: ElementId) -> Result<ElementState<'_>, ErrorCode> {
        let element = self.element(id)?;
        let z_index = self
            .paint_order
            .iter()
            .position(|paint_id| *paint_id == id)
            .ok_or(ErrorCode::NotFound)?;
        Ok(ElementState {
            id,
            rect: element.data.state_rect(),
            rotation: element.data.rotation(),
            opacity: element.data.opacity(),
            z_index,
            visible: element.meta.visible,
            locked: element.meta.locked,
            data: &element.data,
        })
    }

    pub fn element_states(&self) -> impl Iterator<Item = ElementState<'_>> {
        self.paint_order
            .iter()
            .copied()
            .enumerate()
            .filter_map(|(z_index, id)| {
                let element = self.element(id).ok()?;
                Some(ElementState {
                    id,
                    rect: element.data.state_rect(),
                    rotation: element.data.rotation(),
                    opacity: element.data.opacity(),
                    z_index,
                    visible: element.meta.visible,
                    locked: element.meta.locked,
                    data: &element.data,
                })
            })
    }

    pub fn rectangle(&self, id: ElementId) -> Result<&RectangleData, ErrorCode> {
        match &self.element(id)?.data {
            ElementData::Rectangle(rect) => Ok(rect),
            _ => Err(ErrorCode::InvalidArgument),
        }
    }

    pub fn filter(&self, id: ElementId) -> Result<&FilterData, ErrorCode> {
        match &self.element(id)?.data {
            ElementData::Filter(filter) => Ok(filter),
            _ => Err(ErrorCode::InvalidArgument),
        }
    }

    pub fn pen_filter(&self, id: ElementId) -> Result<&PenFilterData, ErrorCode> {
        match &self.element(id)?.data {
            ElementData::PenFilter(filter) => Ok(filter),
            _ => Err(ErrorCode::InvalidArgument),
        }
    }

    pub fn arrow(&self, id: ElementId) -> Result<&ArrowData, ErrorCode> {
        match &self.element(id)?.data {
            ElementData::Arrow(arrow) => Ok(arrow),
            _ => Err(ErrorCode::InvalidArgument),
        }
    }

    pub fn free_draw(&self, id: ElementId) -> Result<&FreeDrawData, ErrorCode> {
        match &self.element(id)?.data {
            ElementData::FreeDraw(free_draw) => Ok(free_draw),
            _ => Err(ErrorCode::InvalidArgument),
        }
    }

    pub fn text(&self, id: ElementId) -> Result<&TextData, ErrorCode> {
        match &self.element(id)?.data {
            ElementData::Text(text) => Ok(text),
            _ => Err(ErrorCode::InvalidArgument),
        }
    }

    pub fn serial_number(&self, id: ElementId) -> Result<&SerialNumberData, ErrorCode> {
        match &self.element(id)?.data {
            ElementData::SerialNumber(serial) => Ok(serial),
            _ => Err(ErrorCode::InvalidArgument),
        }
    }

    pub fn element_kind(&self, id: ElementId) -> Result<ElementKind, ErrorCode> {
        Ok(self.element(id)?.data.kind())
    }

    pub fn element_rect_proxy(&self, id: ElementId) -> Option<RectangleData> {
        if let Ok(rect) = self.rectangle(id) {
            return Some(*rect);
        }
        if let Ok(filter) = self.filter(id) {
            return Some(filter_rect_proxy(filter));
        }
        if let Ok(filter) = self.pen_filter(id) {
            return Some(RectangleData {
                rectangle_kind: RectangleElementKind::Rectangle,
                highlight_shape: HighlightShape::Rectangle,
                center: filter.center(),
                width: filter.width,
                height: filter.height,
                rotation: filter.rotation,
                fill: ColorRgba8::default(),
                fill_style: FillStyle::Solid,
                stroke: ColorRgba8::default(),
                stroke_width: 0.0,
                stroke_style: StrokeStyle::Solid,
                corner_radii: CornerRadii::default(),
                opacity: filter.opacity,
            });
        }
        if let Ok(free_draw) = self.free_draw(id) {
            return Some(RectangleData {
                rectangle_kind: RectangleElementKind::Rectangle,
                highlight_shape: HighlightShape::Rectangle,
                center: Point::new(
                    free_draw.x + free_draw.width / 2.0,
                    free_draw.y + free_draw.height / 2.0,
                ),
                width: free_draw.width,
                height: free_draw.height,
                rotation: free_draw.rotation,
                fill: free_draw.fill,
                fill_style: free_draw.fill_style,
                stroke: free_draw.stroke,
                stroke_width: free_draw.stroke_width,
                stroke_style: free_draw.stroke_style,
                corner_radii: CornerRadii::default(),
                opacity: free_draw.opacity,
            });
        }
        if let Ok(text) = self.text(id) {
            return Some(RectangleData {
                rectangle_kind: RectangleElementKind::Rectangle,
                highlight_shape: HighlightShape::Rectangle,
                center: text.center,
                width: text.width,
                height: text.height,
                rotation: text.rotation,
                fill: text.fill,
                fill_style: text.fill_style,
                stroke: text.stroke,
                stroke_width: text.stroke_width,
                stroke_style: StrokeStyle::Solid,
                corner_radii: text.corner_radii,
                opacity: text.opacity,
            });
        }
        if let Ok(serial) = self.serial_number(id) {
            return Some(serial_number_rect_proxy(serial));
        }
        None
    }

    pub fn apply(&mut self, transaction: &Transaction) -> Result<ApplyResult, ErrorCode> {
        if transaction.is_empty() {
            return Err(ErrorCode::InvalidArgument);
        }

        let mut inverse = Vec::with_capacity(transaction.operations.len());
        let mut changes = DocumentDelta::default();
        let start_revision = self.revision;
        let start_next_index = self.next_index;

        for operation in &transaction.operations {
            match self.apply_operation(operation, &mut changes) {
                Ok(inverse_operation) => inverse.push(inverse_operation),
                Err(error) => {
                    for rollback_operation in inverse.iter().rev() {
                        self.apply_operation(rollback_operation, &mut DocumentDelta::default())
                            .expect("document rollback must succeed");
                    }
                    self.revision = start_revision;
                    self.next_index = start_next_index;
                    return Err(error);
                }
            }
        }

        inverse.reverse();
        self.revision.0 = self.revision.0.wrapping_add(1);
        let document_revision = self.revision;

        Ok(ApplyResult {
            document_revision,
            inverse: Transaction {
                label: format!("undo {}", transaction.label()),
                operations: inverse,
            },
            changes,
        })
    }

    pub fn for_each_visible_rectangle(&self, mut f: impl FnMut(ElementId, &RectangleData)) {
        for id in &self.paint_order {
            let Ok(element) = self.element(*id) else {
                continue;
            };
            if !element.meta.visible {
                continue;
            }
            if let ElementData::Rectangle(rect) = &element.data
                && rect.width > 0.0
                && rect.height > 0.0
            {
                f(*id, rect);
            }
        }
    }

    pub fn for_each_visible_arrow(&self, mut f: impl FnMut(ElementId, &ArrowData)) {
        for id in &self.paint_order {
            let Ok(element) = self.element(*id) else {
                continue;
            };
            if !element.meta.visible {
                continue;
            }
            match &element.data {
                ElementData::Arrow(arrow) if !arrow_is_degenerate(arrow) => f(*id, arrow),
                _ => {}
            }
        }
    }

    pub fn for_each_visible_text(&self, mut f: impl FnMut(ElementId, &TextData)) {
        for id in &self.paint_order {
            let Ok(element) = self.element(*id) else {
                continue;
            };
            if !element.meta.visible {
                continue;
            }
            match &element.data {
                ElementData::Text(text) if text.width > 0.0 && text.height > 0.0 => f(*id, text),
                _ => {}
            }
        }
    }

    pub fn for_each_visible_serial_number(&self, mut f: impl FnMut(ElementId, &SerialNumberData)) {
        for id in &self.paint_order {
            let Ok(element) = self.element(*id) else {
                continue;
            };
            if !element.meta.visible {
                continue;
            }
            match &element.data {
                ElementData::SerialNumber(serial) if serial.diameter > 0.0 => f(*id, serial),
                _ => {}
            }
        }
    }

    pub fn topmost_rectangle_at(&self, point: Point<f64>) -> Option<ElementId> {
        self.topmost_rectangle_at_with_tolerance(point, 0.0)
    }

    pub fn topmost_rectangle_at_with_tolerance(
        &self,
        point: Point<f64>,
        hit_tolerance: f64,
    ) -> Option<ElementId> {
        self.paint_order.iter().rev().find_map(|id| {
            let element = self.element(*id).ok()?;
            if !element.meta.visible {
                return None;
            }
            match &element.data {
                ElementData::Rectangle(rect) if rectangle_hit_test(rect, point, hit_tolerance) => {
                    Some(*id)
                }
                _ => None,
            }
        })
    }

    pub fn topmost_element_at(&self, point: Point<f64>) -> Option<(ElementId, ElementKind)> {
        self.topmost_element_at_with_tolerance(point, 0.0)
    }

    pub fn topmost_element_at_with_tolerance(
        &self,
        point: Point<f64>,
        hit_tolerance: f64,
    ) -> Option<(ElementId, ElementKind)> {
        self.paint_order.iter().rev().find_map(|id| {
            let element = self.element(*id).ok()?;
            if !element.meta.visible {
                return None;
            }
            match &element.data {
                ElementData::Rectangle(rect) if rectangle_hit_test(rect, point, hit_tolerance) => {
                    Some((*id, ElementKind::Rectangle))
                }
                ElementData::Arrow(arrow) if arrow_hit_test(arrow, point, hit_tolerance) => {
                    Some((*id, arrow.element_kind()))
                }
                ElementData::FreeDraw(free_draw)
                    if free_draw_hit_test(free_draw, point, hit_tolerance) =>
                {
                    Some((*id, ElementKind::FreeDraw))
                }
                ElementData::Text(text) if text_hit_test(text, point, hit_tolerance) => {
                    Some((*id, ElementKind::Text))
                }
                ElementData::SerialNumber(serial)
                    if serial_number_hit_test(serial, point, hit_tolerance) =>
                {
                    Some((*id, ElementKind::SerialNumber))
                }
                _ => None,
            }
        })
    }

    pub fn topmost_text_at_with_tolerance(
        &self,
        point: Point<f64>,
        hit_tolerance: f64,
    ) -> Option<ElementId> {
        self.paint_order.iter().rev().find_map(|id| {
            let element = self.element(*id).ok()?;
            if !element.meta.visible {
                return None;
            }
            match &element.data {
                ElementData::Text(text) if text_hit_test(text, point, hit_tolerance) => Some(*id),
                _ => None,
            }
        })
    }

    pub fn serial_number_text_bindings(&self) -> Vec<SerialNumberTextBinding> {
        self.paint_order
            .iter()
            .filter_map(|serial_id| {
                let text_id = self.serial_number(*serial_id).ok()?.text_element_id?;
                self.text(text_id).ok()?;
                Some(SerialNumberTextBinding {
                    serial_id: *serial_id,
                    text_id,
                })
            })
            .collect()
    }

    pub fn text_ids_bound_to_serial_numbers(&self) -> HashSet<ElementId> {
        self.serial_number_text_bindings()
            .into_iter()
            .map(|binding| binding.text_id)
            .collect()
    }

    pub fn is_text_bound_to_serial_number(&self, text_id: ElementId) -> bool {
        self.serial_number_text_bindings()
            .into_iter()
            .any(|binding| binding.text_id == text_id)
    }

    pub fn bound_text_id_for_serial_number(&self, serial_id: ElementId) -> Option<ElementId> {
        self.serial_number_text_bindings()
            .into_iter()
            .find(|binding| binding.serial_id == serial_id)
            .map(|binding| binding.text_id)
    }

    pub fn serial_number_ids_with_text(&self, text_id: ElementId) -> Vec<ElementId> {
        self.serial_number_text_bindings()
            .into_iter()
            .filter(|binding| binding.text_id == text_id)
            .map(|binding| binding.serial_id)
            .collect()
    }

    pub fn validate_view_state(&self, camera: &Camera) -> Result<(), ErrorCode> {
        validate_camera(camera)
    }

    pub fn paint_order(&self) -> &[ElementId] {
        &self.paint_order
    }

    pub fn element_bounds(&self, id: ElementId) -> Result<DrawRect, ErrorCode> {
        let element = self.element(id)?;
        Ok(element.data.bounds())
    }

    fn element_change_snapshot(&self, id: ElementId) -> Option<ElementChangeSnapshot> {
        let element = self.element(id).ok()?;
        let bounds = element_visible_bounds(element);
        let paint_index = self
            .paint_order
            .iter()
            .position(|candidate| *candidate == id)
            .map(|index| index as u32);
        Some(ElementChangeSnapshot {
            kind: element.data.kind(),
            bounds,
            paint_index,
        })
    }

    fn apply_operation(
        &mut self,
        operation: &Operation,
        changes: &mut ChangeSet,
    ) -> Result<Operation, ErrorCode> {
        match operation {
            Operation::UpdateWatermark { config } => {
                let normalized = config.clone().normalized();
                validate_watermark_config(&normalized)?;
                let inverse = std::mem::replace(&mut self.watermark, normalized);
                Ok(Operation::UpdateWatermark { config: inverse })
            }
            Operation::UpdateSpotlight { config } => {
                let normalized = config.normalized();
                validate_spotlight_config(&normalized)?;
                let inverse = std::mem::replace(&mut self.spotlight, normalized);
                Ok(Operation::UpdateSpotlight { config: inverse })
            }
            Operation::InsertElement { id, meta, data } => {
                validate_element_data(data)?;
                let old_snapshot = self.element_change_snapshot(*id);
                self.insert_element(
                    *id,
                    ElementRecord {
                        id: *id,
                        meta: *meta,
                        data: data.clone(),
                    },
                    self.paint_order.len() as u32,
                )?;
                changes.touch(*id);
                changes.created.push(*id);
                changes.note_element_bounds(data);
                changes.z_order_changed = true;
                changes.note_element_transition(
                    *id,
                    old_snapshot,
                    self.element_change_snapshot(*id),
                );
                if element_data_has_arrow_relations(data) || self.has_arrow_bound_to_element(*id) {
                    changes.relations_changed = true;
                }
                Ok(Operation::RemoveElement { id: *id })
            }
            Operation::UpdateElementData { id, data } => {
                validate_element_data(data)?;
                let old_snapshot = self.element_change_snapshot(*id);
                changes.note_existing_bounds(self, *id);
                let element = self.lookup_mut(*id)?;
                if element.meta.locked {
                    return Err(ErrorCode::InvalidState);
                }
                let inverse_data = element.data.clone();
                if element.data.kind() != data.kind() {
                    return Err(ErrorCode::InvalidArgument);
                }
                let relations_changed =
                    element_data_arrow_relation_targets_changed(&inverse_data, data);
                element.data = data.clone();
                changes.touch(*id);
                changes.note_element_bounds(data);
                changes.note_element_transition(
                    *id,
                    old_snapshot,
                    self.element_change_snapshot(*id),
                );
                if relations_changed {
                    changes.relations_changed = true;
                }
                Ok(Operation::UpdateElementData {
                    id: *id,
                    data: inverse_data,
                })
            }
            Operation::UpdateElementMeta { id, meta } => {
                let old_snapshot = self.element_change_snapshot(*id);
                changes.note_existing_bounds(self, *id);
                let element = self.lookup_mut(*id)?;
                let inverse = element.meta;
                element.meta = *meta;
                changes.touch(*id);
                changes.note_existing_bounds(self, *id);
                changes.note_element_transition(
                    *id,
                    old_snapshot,
                    self.element_change_snapshot(*id),
                );
                Ok(Operation::UpdateElementMeta {
                    id: *id,
                    meta: inverse,
                })
            }
            Operation::RemoveElement { id } => {
                let old_snapshot = self.element_change_snapshot(*id);
                let paint_index = self
                    .paint_order
                    .iter()
                    .position(|candidate| *candidate == *id)
                    .ok_or(ErrorCode::NotFound)? as u32;
                let element = self.lookup(*id)?.clone();
                if element.meta.locked {
                    return Err(ErrorCode::InvalidState);
                }
                let relations_changed = element_data_has_arrow_relations(&element.data)
                    || self.has_arrow_bound_to_element(*id);
                changes.note_existing_bounds(self, *id);
                self.remove_element(*id)?;
                changes.touch(*id);
                changes.removed.push(*id);
                changes.z_order_changed = true;
                changes.note_element_transition(
                    *id,
                    old_snapshot,
                    self.element_change_snapshot(*id),
                );
                if relations_changed {
                    changes.relations_changed = true;
                }
                Ok(Operation::RestoreElement {
                    element,
                    paint_index,
                })
            }
            Operation::RestoreElement {
                element,
                paint_index,
            } => {
                let old_snapshot = self.element_change_snapshot(element.id);
                self.insert_element(element.id, element.clone(), *paint_index)?;
                changes.touch(element.id);
                changes.note_existing_bounds(self, element.id);
                changes.z_order_changed = true;
                changes.note_element_transition(
                    element.id,
                    old_snapshot,
                    self.element_change_snapshot(element.id),
                );
                if element_data_has_arrow_relations(&element.data)
                    || self.has_arrow_bound_to_element(element.id)
                {
                    changes.relations_changed = true;
                }
                Ok(Operation::RemoveElement { id: element.id })
            }
            Operation::ReorderElements { ids, paint_index } => {
                if ids.is_empty() {
                    return Err(ErrorCode::InvalidArgument);
                }
                let old_snapshots: HashMap<_, _> = ids
                    .iter()
                    .copied()
                    .map(|id| (id, self.element_change_snapshot(id)))
                    .collect();
                let inverse_positions = self.current_paint_positions(ids)?;
                self.reorder_elements_internal(ids, *paint_index)?;
                for id in ids {
                    changes.note_existing_bounds(self, *id);
                    changes.touch(*id);
                    let old_snapshot = old_snapshots.get(id).copied().flatten();
                    changes.note_element_transition(
                        *id,
                        old_snapshot,
                        self.element_change_snapshot(*id),
                    );
                }
                changes.z_order_changed = true;
                Ok(Operation::ReorderElements {
                    ids: inverse_positions.iter().map(|(id, _)| *id).collect(),
                    paint_index: inverse_positions
                        .iter()
                        .map(|(_, index)| *index)
                        .min()
                        .unwrap_or(0),
                })
            }
        }
    }

    fn insert_element(
        &mut self,
        id: ElementId,
        element: ElementRecord,
        paint_index: u32,
    ) -> Result<(), ErrorCode> {
        if id.generation == 0 {
            return Err(ErrorCode::InvalidArgument);
        }

        let index = id.index as usize;
        if index > self.slots.len() {
            return Err(ErrorCode::InvalidState);
        }
        if index == self.slots.len() {
            self.slots.push(Some(element));
        } else if self.slots[index].is_none() {
            self.slots[index] = Some(element);
        } else {
            return Err(ErrorCode::InvalidState);
        }

        let paint_index = paint_index.min(self.paint_order.len() as u32) as usize;
        self.paint_order.insert(paint_index, id);
        self.next_index = self.next_index.max(id.index.saturating_add(1));
        Ok(())
    }

    fn remove_element(&mut self, id: ElementId) -> Result<(), ErrorCode> {
        let index = id.index as usize;
        let Some(slot) = self.slots.get_mut(index) else {
            return Err(ErrorCode::NotFound);
        };
        let Some(element) = slot.as_ref() else {
            return Err(ErrorCode::NotFound);
        };
        if element.id != id {
            return Err(ErrorCode::NotFound);
        }

        *slot = None;
        self.paint_order.retain(|candidate| *candidate != id);
        Ok(())
    }

    fn current_paint_positions(
        &self,
        ids: &[ElementId],
    ) -> Result<Vec<(ElementId, u32)>, ErrorCode> {
        let mut positions = Vec::with_capacity(ids.len());
        if ids.len() <= 4 {
            for id in ids {
                let index = self
                    .paint_order
                    .iter()
                    .position(|candidate| candidate == id)
                    .ok_or(ErrorCode::NotFound)? as u32;
                positions.push((*id, index));
            }
            return Ok(positions);
        }

        let targets: HashSet<_> = ids.iter().copied().collect();
        let mut lookup = HashMap::with_capacity(targets.len());
        for (index, candidate) in self.paint_order.iter().copied().enumerate() {
            if targets.contains(&candidate) {
                lookup.insert(candidate, index as u32);
                if lookup.len() == targets.len() {
                    break;
                }
            }
        }

        for id in ids {
            positions.push((*id, *lookup.get(id).ok_or(ErrorCode::NotFound)?));
        }
        Ok(positions)
    }

    fn reorder_elements_internal(
        &mut self,
        ids: &[ElementId],
        paint_index: u32,
    ) -> Result<(), ErrorCode> {
        if ids.is_empty() {
            return Err(ErrorCode::InvalidArgument);
        }

        let mut moving = Vec::with_capacity(ids.len());
        let mut moving_ids = HashSet::with_capacity(ids.len());
        for id in ids {
            if self.element(*id).is_err() || !moving_ids.insert(*id) {
                return Err(ErrorCode::InvalidArgument);
            }
            moving.push(*id);
        }

        self.paint_order
            .retain(|candidate| !moving_ids.contains(candidate));
        let insert_at = paint_index.min(self.paint_order.len() as u32) as usize;
        self.paint_order
            .splice(insert_at..insert_at, moving.iter().copied());
        Ok(())
    }

    fn has_arrow_bound_to_element(&self, id: ElementId) -> bool {
        self.paint_order.iter().any(|arrow_id| {
            self.arrow(*arrow_id)
                .is_ok_and(|arrow| arrow.bound_element_ids().contains(&id))
        })
    }

    fn lookup(&self, id: ElementId) -> Result<&ElementRecord, ErrorCode> {
        self.element(id)
    }

    fn lookup_mut(&mut self, id: ElementId) -> Result<&mut ElementRecord, ErrorCode> {
        let Some(slot) = self.slots.get_mut(id.index as usize) else {
            return Err(ErrorCode::NotFound);
        };
        let Some(element) = slot.as_mut() else {
            return Err(ErrorCode::NotFound);
        };
        if element.id != id {
            return Err(ErrorCode::NotFound);
        }
        Ok(element)
    }
}

fn element_data_has_arrow_relations(data: &ElementData) -> bool {
    matches!(data, ElementData::Arrow(arrow) if !arrow.bound_element_ids().is_empty())
}

fn element_data_arrow_relation_targets_changed(previous: &ElementData, next: &ElementData) -> bool {
    let (ElementData::Arrow(previous), ElementData::Arrow(next)) = (previous, next) else {
        return false;
    };
    let previous_ids = previous.bound_element_ids();
    let next_ids = next.bound_element_ids();
    previous_ids.len() != next_ids.len() || previous_ids.iter().any(|id| !next_ids.contains(id))
}

#[cfg(test)]
mod tests {
    use std::collections::HashSet;

    use super::*;
    use crate::{
        FILTER_SCHEMA, PEN_FILTER_SCHEMA, RECTANGLE_HIGHLIGHT_SCHEMA, RECTANGLE_SCHEMA,
        SERIAL_NUMBER_SCHEMA, SPOTLIGHT_SCHEMA, TEXT_SCHEMA,
    };

    fn assert_schema(payload: &Map<String, Value>, schema: crate::ElementPayloadSchema) {
        assert_eq!(
            payload.keys().map(String::as_str).collect::<HashSet<_>>(),
            schema.fields.iter().copied().collect::<HashSet<_>>()
        );
        assert_eq!(
            payload.get(field::TYPE_ID).and_then(Value::as_str),
            Some(schema.type_id)
        );
    }

    #[test]
    fn element_payload_codecs_emit_the_canonical_reference_fields() {
        let rectangle = RectangleData {
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: HighlightShape::Rectangle,
            center: Point::new(10.0, 20.0),
            width: 80.0,
            height: 60.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 2.0,
            stroke_style: StrokeStyle::Dashed,
            corner_radii: CornerRadii::splat(4.0),
            opacity: 1.0,
        };
        assert_schema(
            &rectangle.to_rectangle_json_payload().unwrap(),
            RECTANGLE_SCHEMA,
        );
        assert_schema(
            &rectangle
                .into_highlight(HighlightShape::Rectangle)
                .to_highlight_json_payload()
                .unwrap(),
            RECTANGLE_HIGHLIGHT_SCHEMA,
        );
        assert_schema(
            &FilterData::default().to_json_payload().unwrap(),
            FILTER_SCHEMA,
        );
        assert_schema(&TextData::default().to_json_payload(), TEXT_SCHEMA);
        assert_schema(
            &SerialNumberData::default().to_json_payload(),
            SERIAL_NUMBER_SCHEMA,
        );
    }

    #[test]
    fn common_element_state_has_one_geometry_opacity_and_z_order_definition() {
        let rectangle = RectangleData {
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: HighlightShape::Rectangle,
            center: Point::new(10.0, 20.0),
            width: 8.0,
            height: 6.0,
            rotation: 0.25,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 0.75,
        };
        let filter = FilterData {
            center: Point::new(30.0, 40.0),
            width: 12.0,
            height: 10.0,
            rotation: 0.5,
            opacity: 0.25,
            ..FilterData::default()
        };
        let mut document = Document::new();
        let rectangle_id = document.allocate_element_id();
        let filter_id = document.allocate_element_id();
        let mut transaction = Transaction::new("canonical common state");
        transaction.insert_rectangle(rectangle_id, ElementMeta::default(), rectangle);
        transaction.insert_filter(filter_id, ElementMeta::default(), filter);
        document.apply(&transaction).unwrap();

        let rectangle_state = document.element_state(rectangle_id).unwrap();
        assert_eq!(rectangle_state.rect, DrawRect::new(6.0, 17.0, 14.0, 23.0));
        assert_eq!(rectangle_state.rotation, 0.25);
        assert_eq!(rectangle_state.opacity, 0.75);
        assert_eq!(rectangle_state.z_index, 0);

        let filter_state = document.element_state(filter_id).unwrap();
        assert_eq!(filter_state.rect, DrawRect::new(24.0, 35.0, 36.0, 45.0));
        assert_eq!(filter_state.rotation, 0.5);
        assert_eq!(filter_state.opacity, 0.25);
        assert_eq!(filter_state.z_index, 1);
        assert_eq!(
            document
                .element_states()
                .map(|state| state.id)
                .collect::<Vec<_>>(),
            vec![rectangle_id, filter_id]
        );
    }

    fn insert_text_and_serial(
        document: &mut Document,
        text_id: ElementId,
        serial_id: ElementId,
        serial: SerialNumberData,
    ) {
        let mut transaction = Transaction::new("insert bound serial");
        transaction.insert_text(text_id, ElementMeta::default(), TextData::default());
        transaction.insert_serial_number(serial_id, ElementMeta::default(), serial);
        document.apply(&transaction).unwrap();
    }

    #[test]
    fn serial_number_text_bindings_include_existing_text_only() {
        let mut document = Document::new();
        let text_id = ElementId {
            index: 0,
            generation: 1,
        };
        let serial_id = ElementId {
            index: 1,
            generation: 1,
        };
        insert_text_and_serial(
            &mut document,
            text_id,
            serial_id,
            SerialNumberData {
                text_element_id: Some(text_id),
                ..SerialNumberData::default()
            },
        );

        assert_eq!(
            document.serial_number_text_bindings(),
            vec![SerialNumberTextBinding { serial_id, text_id }]
        );
        assert_eq!(
            document.text_ids_bound_to_serial_numbers(),
            HashSet::from([text_id])
        );
        assert!(document.is_text_bound_to_serial_number(text_id));
        assert_eq!(
            document.bound_text_id_for_serial_number(serial_id),
            Some(text_id)
        );
        assert_eq!(
            document.serial_number_ids_with_text(text_id),
            vec![serial_id]
        );
    }

    #[test]
    fn serial_number_text_bindings_ignore_missing_text() {
        let mut document = Document::new();
        let missing_text_id = ElementId {
            index: 9,
            generation: 1,
        };
        let serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let mut transaction = Transaction::new("insert stale serial");
        transaction.insert_serial_number(
            serial_id,
            ElementMeta::default(),
            SerialNumberData {
                text_element_id: Some(missing_text_id),
                ..SerialNumberData::default()
            },
        );
        document.apply(&transaction).unwrap();

        assert!(document.serial_number_text_bindings().is_empty());
        assert!(document.text_ids_bound_to_serial_numbers().is_empty());
        assert!(!document.is_text_bound_to_serial_number(missing_text_id));
        assert_eq!(document.bound_text_id_for_serial_number(serial_id), None);
        assert!(
            document
                .serial_number_ids_with_text(missing_text_id)
                .is_empty()
        );
    }

    #[test]
    fn highlight_payload_is_canonical_and_strict() {
        let highlight = RectangleData {
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: HighlightShape::Rectangle,
            center: Point::default(),
            width: 80.0,
            height: 60.0,
            rotation: 0.0,
            fill: ColorRgba8 {
                r: 0xf5,
                g: 0x22,
                b: 0x2d,
                a: 0xff,
            },
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8 {
                r: 0,
                g: 0,
                b: 0,
                a: 0xff,
            },
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
        .into_highlight(HighlightShape::Rectangle);
        let payload = highlight.to_highlight_json_payload().unwrap();
        assert_eq!(
            payload.get("typeId").and_then(Value::as_str),
            Some("rectangle_highlight")
        );
        assert!(!payload.contains_key("shape"));
        assert!(
            RectangleData::from_highlight_json_payload(
                &payload,
                Point::new(10.0, 20.0),
                80.0,
                60.0,
            )
            .unwrap()
            .is_highlight()
        );

        let mut legacy_payload = payload.clone();
        legacy_payload.insert("typeId".to_owned(), Value::String("highlight".to_owned()));
        assert_eq!(
            RectangleData::from_highlight_json_payload(
                &legacy_payload,
                Point::default(),
                80.0,
                60.0
            ),
            Err(ErrorCode::InvalidArgument)
        );
        let mut missing_color = payload;
        missing_color.remove("color");
        assert_eq!(
            RectangleData::from_highlight_json_payload(
                &missing_color,
                Point::default(),
                80.0,
                60.0
            ),
            Err(ErrorCode::InvalidArgument)
        );
    }

    #[test]
    fn filter_payload_round_trips_all_stable_types_and_is_strict() {
        for (filter_type, expected) in [
            (CanvasFilterType::Mosaic, "mosaic"),
            (CanvasFilterType::GaussianBlur, "gaussianBlur"),
            (CanvasFilterType::Grayscale, "grayscale"),
            (CanvasFilterType::Inversion, "inversion"),
        ] {
            let filter = FilterData {
                filter_type,
                strength: 0.5,
                ..FilterData::default()
            };
            let payload = filter.to_json_payload().unwrap();
            assert_eq!(payload.len(), 3);
            assert_eq!(
                payload.get("typeId").and_then(Value::as_str),
                Some("filter")
            );
            assert_eq!(payload.get("type").and_then(Value::as_str), Some(expected));
            assert_eq!(
                FilterData::from_json_payload(&payload, Point::new(1.0, 2.0), 30.0, 40.0)
                    .unwrap()
                    .filter_type,
                filter_type
            );
        }

        let mut missing = FilterData::default().to_json_payload().unwrap();
        missing.remove("strength");
        assert!(FilterData::from_json_payload(&missing, Point::default(), 1.0, 1.0).is_err());
        let mut unknown = FilterData::default().to_json_payload().unwrap();
        unknown.insert("type".to_owned(), Value::String("future".to_owned()));
        assert!(FilterData::from_json_payload(&unknown, Point::default(), 1.0, 1.0).is_err());
        let mut extra = FilterData::default().to_json_payload().unwrap();
        extra.insert("extra".to_owned(), Value::Bool(true));
        assert!(FilterData::from_json_payload(&extra, Point::default(), 1.0, 1.0).is_err());
    }

    #[test]
    fn filter_strength_normalization_is_deterministic() {
        assert_eq!(FilterData::normalized_strength(f64::NAN), 1.0);
        assert_eq!(FilterData::normalized_strength(-2.0), 0.0);
        assert_eq!(FilterData::normalized_strength(2.0), 1.0);
        assert_eq!(FilterData::normalized_strength(0.25), 0.25);
    }

    #[test]
    fn spotlight_defaults_schema_visibility_and_inverse_are_stable() {
        let default = SpotlightConfig::default();
        assert_eq!(
            default,
            SpotlightConfig {
                color: ColorRgba8 {
                    r: 0,
                    g: 0,
                    b: 0,
                    a: 255,
                },
                opacity: 0.64,
            }
        );
        assert_eq!(
            SpotlightConfig {
                opacity: -1.0,
                ..default
            }
            .normalized()
            .opacity,
            0.0
        );
        assert_eq!(
            SpotlightConfig {
                opacity: 2.0,
                ..default
            }
            .normalized()
            .opacity,
            1.0
        );
        assert_eq!(
            SpotlightConfig {
                opacity: f64::NAN,
                ..default
            }
            .normalized()
            .opacity,
            0.0
        );
        assert_eq!(
            validate_spotlight_config(&SpotlightConfig {
                opacity: f64::INFINITY,
                ..default
            }),
            Err(ErrorCode::InvalidArgument)
        );

        let spotlight = RectangleData {
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: HighlightShape::Rectangle,
            center: Point::new(10.0, 20.0),
            width: 80.0,
            height: 60.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
        .into_spotlight();
        let payload = spotlight.to_spotlight_json_payload().unwrap();
        assert_schema(&payload, SPOTLIGHT_SCHEMA);
        assert_eq!(
            RectangleData::from_spotlight_json_payload(
                &payload,
                spotlight.center,
                spotlight.width,
                spotlight.height,
            )
            .unwrap(),
            spotlight
        );
        let mut extra = payload;
        extra.insert("color".to_owned(), Value::String("#000000".to_owned()));
        assert_eq!(
            RectangleData::from_spotlight_json_payload(
                &extra,
                spotlight.center,
                spotlight.width,
                spotlight.height,
            ),
            Err(ErrorCode::InvalidArgument)
        );

        let mut document = Document::new();
        assert_eq!(document.spotlight_config(), default);
        assert!(!document.has_visible_spotlight());
        let id = document.allocate_element_id();
        let mut insert = Transaction::new("insert spotlight");
        insert.insert_rectangle(id, ElementMeta::default(), spotlight);
        document.apply(&insert).unwrap();
        assert!(document.has_visible_spotlight());

        let mut hide = Transaction::new("hide spotlight");
        hide.update_element_meta(
            id,
            ElementMeta {
                visible: false,
                ..ElementMeta::default()
            },
        );
        document.apply(&hide).unwrap();
        assert!(!document.has_visible_spotlight());

        let changed = SpotlightConfig {
            color: ColorRgba8 {
                r: 20,
                g: 40,
                b: 60,
                a: 128,
            },
            opacity: 0.75,
        };
        let mut update = Transaction::new("update spotlight");
        update.update_spotlight(changed);
        let result = document.apply(&update).unwrap();
        assert_eq!(document.spotlight_config(), changed);
        document.apply(&result.inverse).unwrap();
        assert_eq!(document.spotlight_config(), default);
    }

    #[test]
    fn pen_filter_payload_round_trip_is_strict_and_preserves_raw_points() {
        let original = PenFilterData::from_global_points(
            &[
                Point::new(10.0, 20.0),
                Point::new(15.0, 32.0),
                Point::new(40.0, 25.0),
            ],
            CanvasFilterType::GaussianBlur,
            0.37,
            8.0,
            1.0,
        )
        .unwrap();
        let payload = original.to_json_payload().unwrap();
        assert_eq!(payload.len(), PEN_FILTER_SCHEMA.fields.len());
        assert_eq!(payload["typeId"], "pen_filter");
        let decoded = PenFilterData::from_json_payload(
            &payload,
            DrawRect::new(
                original.x,
                original.y,
                original.x + original.width,
                original.y + original.height,
            ),
        )
        .unwrap();
        assert_eq!(decoded.global_points(), original.global_points());
        assert_eq!(decoded.filter_type, CanvasFilterType::GaussianBlur);
        assert_eq!(decoded.strength, 0.37);
        assert_eq!(decoded.stroke_width, 8.0);

        let mut extra = payload.clone();
        extra.insert("legacy".to_owned(), Value::Bool(true));
        assert!(
            PenFilterData::from_json_payload(&extra, DrawRect::new(0.0, 0.0, 1.0, 1.0)).is_err()
        );

        let mut invalid_width = payload.clone();
        invalid_width.insert(field::STROKE_WIDTH.to_owned(), Value::from(0.0));
        assert!(
            PenFilterData::from_json_payload(&invalid_width, DrawRect::new(0.0, 0.0, 30.0, 12.0))
                .is_err()
        );

        let mut invalid_points = payload;
        invalid_points.insert(field::POINTS.to_owned(), Value::Array(vec![]));
        assert!(
            PenFilterData::from_json_payload(&invalid_points, DrawRect::new(0.0, 0.0, 30.0, 12.0))
                .is_err()
        );
    }

    #[test]
    fn watermark_defaults_normalization_visibility_and_inverse_are_stable() {
        let default = WatermarkConfig::default();
        assert_eq!(
            default.color,
            ColorRgba8 {
                r: 0,
                g: 0,
                b: 0,
                a: 255
            }
        );
        assert_eq!(
            (
                default.font_size,
                default.angle,
                default.gap,
                default.opacity
            ),
            (12.0, 30.0, 56.0, 0.16)
        );
        assert!(!default.is_visible());

        let config = WatermarkConfig {
            text: "  CONFIDENTIAL  ".to_owned(),
            gap: 500.0,
            opacity: 0.5,
            ..default.clone()
        }
        .normalized();
        assert_eq!(config.text, "CONFIDENTIAL");
        assert_eq!(config.gap, 200.0);
        assert!(config.is_visible());

        let mut document = Document::new();
        let mut transaction = Transaction::new("Update watermark");
        transaction.update_watermark(config.clone());
        let result = document.apply(&transaction).unwrap();
        assert_eq!(document.watermark_config(), &config);
        document.apply(&result.inverse).unwrap();
        assert_eq!(document.watermark_config(), &default);
    }
}

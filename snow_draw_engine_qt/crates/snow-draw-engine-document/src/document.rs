use std::collections::{HashMap, HashSet};

use serde::{Deserialize, Deserializer, Serialize, Serializer};

use crate::{
    ApplyResult, ArrowData, ChangeSet, DocumentDelta, ElementChangeSnapshot, FreeDrawData,
    Operation, Transaction, arrow_bounds, arrow_hit_test, arrow_is_degenerate,
    document_geometry::{
        element_visible_bounds, filter_bounds, filter_rect_proxy, pen_filter_bounds,
        pen_filter_rect_proxy, rectangle_hit_test, serial_number_hit_test,
        serial_number_rect_proxy, text_bounds, text_hit_test, validate_element_data,
    },
    free_draw_bounds, free_draw_hit_test,
};
pub use snow_draw_engine_core::arrow::{StrokeRenderingStyle, StrokeStyle};
use snow_draw_engine_core::{ColorRgba8, CornerRadii, DrawRect, ErrorCode, Point};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct WatermarkTemplateApplicationTime {
    pub year: i32,
    pub month: u8,
    pub day: u8,
    pub hour: u8,
    pub minute: u8,
    pub second: u8,
}

impl WatermarkTemplateApplicationTime {
    pub fn is_valid(self) -> bool {
        if self.year < 1
            || !(1..=12).contains(&self.month)
            || self.hour > 23
            || self.minute > 59
            || self.second > 59
        {
            return false;
        }
        let days_in_month = match self.month {
            1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
            4 | 6 | 9 | 11 => 30,
            2 if is_leap_year(self.year) => 29,
            2 => 28,
            _ => return false,
        };
        (1..=days_in_month).contains(&self.day)
    }
}

fn is_leap_year(year: i32) -> bool {
    year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct WatermarkConfig {
    pub color: ColorRgba8,
    pub text: String,
    #[serde(default)]
    pub template_value: String,
    #[serde(
        default,
        deserialize_with = "deserialize_watermark_template_application_time"
    )]
    pub template_application_time: Option<WatermarkTemplateApplicationTime>,
    pub font_size: f64,
    pub font_family: String,
    pub angle: f64,
    pub gap: f64,
    pub opacity: f64,
}

fn deserialize_watermark_template_application_time<'de, D>(
    deserializer: D,
) -> Result<Option<WatermarkTemplateApplicationTime>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    Ok(
        Option::<WatermarkTemplateApplicationTime>::deserialize(deserializer)?
            .filter(|time| time.is_valid()),
    )
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
            template_value: String::new(),
            template_application_time: None,
            font_size: 16.0,
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
        self.template_application_time = self
            .template_application_time
            .filter(|time| time.is_valid());
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
        !self.resolved_text().trim().is_empty() && self.effective_alpha() >= 0.004
    }

    pub fn resolved_text(&self) -> String {
        resolve_watermark_template(
            &self.template_value,
            &self.text,
            self.template_application_time
                .filter(|time| time.is_valid()),
        )
    }
}

pub fn resolve_watermark_template(
    template: &str,
    text: &str,
    application_time: Option<WatermarkTemplateApplicationTime>,
) -> String {
    if template.is_empty() {
        return text.to_owned();
    }

    let mut resolved = String::with_capacity(template.len().saturating_add(text.len()));
    let mut cursor = 0;
    while let Some(relative_open) = template[cursor..].find('{') {
        let open = cursor + relative_open;
        resolved.push_str(&template[cursor..open]);

        let mut depth = 0_u32;
        let mut close = None;
        let mut nested = false;
        for (relative, ch) in template[open..].char_indices() {
            match ch {
                '{' => {
                    depth += 1;
                    nested |= depth > 1;
                }
                '}' => {
                    if depth == 0 {
                        continue;
                    }
                    depth -= 1;
                    if depth == 0 {
                        close = Some(open + relative);
                        break;
                    }
                }
                _ => {}
            }
        }

        let Some(close) = close else {
            resolved.push_str(&template[open..]);
            return resolved;
        };
        let placeholder = &template[open..=close];
        if nested {
            resolved.push_str(placeholder);
        } else {
            let content = &template[open + 1..close];
            if content == "text" {
                resolved.push_str(text);
            } else if contains_timestamp_token(content) {
                if let Some(time) = application_time {
                    resolved.push_str(&expand_timestamp_tokens(content, time));
                } else {
                    resolved.push_str(placeholder);
                }
            } else {
                resolved.push_str(placeholder);
            }
        }
        cursor = close + 1;
    }
    resolved.push_str(&template[cursor..]);
    resolved
}

fn contains_timestamp_token(value: &str) -> bool {
    ["YYYY", "MM", "DD", "HH", "mm", "ss"]
        .iter()
        .any(|token| value.contains(token))
}

fn expand_timestamp_tokens(value: &str, time: WatermarkTemplateApplicationTime) -> String {
    value
        .replace("YYYY", &format!("{:04}", time.year))
        .replace("MM", &format!("{:02}", time.month))
        .replace("DD", &format!("{:02}", time.day))
        .replace("HH", &format!("{:02}", time.hour))
        .replace("mm", &format!("{:02}", time.minute))
        .replace("ss", &format!("{:02}", time.second))
}

pub fn validate_watermark_config(config: &WatermarkConfig) -> Result<(), ErrorCode> {
    if config.font_size.is_finite()
        && config.font_size > 0.0
        && config.angle.is_finite()
        && config.gap.is_finite()
        && config.opacity.is_finite()
        && config
            .template_application_time
            .is_none_or(WatermarkTemplateApplicationTime::is_valid)
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

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct DocumentRevision(pub u64);

#[derive(Clone, Copy, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct TextLayoutRect {
    pub center: Point<f64>,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
}

/// Painted text ink (the box the glyphs actually cover) as measured by the
/// host renderer. Constructed only from positive, finite pairs, so an
/// `InkBox` always is a real measurement; "not measured" is `None`, never a
/// sentinel number.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct InkBox {
    width: f64,
    height: f64,
}

impl InkBox {
    /// `Some` only when both axes are finite and positive. The legacy
    /// serialized form and the C ABI use `0` for "not measured"; mixed,
    /// negative, or non-finite pairs are garbage and read the same way.
    pub fn new(width: f64, height: f64) -> Option<Self> {
        if width.is_finite() && width > 0.0 && height.is_finite() && height > 0.0 {
            Some(Self { width, height })
        } else {
            None
        }
    }

    pub fn width(self) -> f64 {
        self.width
    }

    pub fn height(self) -> f64 {
        self.height
    }

    fn scaled(self, scale: f64) -> Self {
        Self {
            width: self.width * scale,
            height: self.height * scale,
        }
    }
}

/// Host-measured wrap rectangle plus painted ink. Fields are private so a
/// measurement can only be stored as one value; splitting wrap from ink is
/// what left serial connectors and dirty regions on stale edges. Ink is
/// `None` until the host measures it.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TextLayoutSize {
    width: f64,
    height: f64,
    ink: Option<InkBox>,
}

/// On-disk form of [`TextLayoutSize`]: the flat `width` / `height` /
/// `content_width` / `content_height` keys documents have always used, with
/// `0` ink standing in for "not measured".
#[derive(Serialize, Deserialize)]
struct TextLayoutSizeFields {
    width: f64,
    height: f64,
    #[serde(default)]
    content_width: f64,
    #[serde(default)]
    content_height: f64,
}

impl From<TextLayoutSizeFields> for TextLayoutSize {
    fn from(fields: TextLayoutSizeFields) -> Self {
        Self::with_content(
            fields.width,
            fields.height,
            fields.content_width,
            fields.content_height,
        )
    }
}

impl From<TextLayoutSize> for TextLayoutSizeFields {
    fn from(layout: TextLayoutSize) -> Self {
        let (content_width, content_height) = layout
            .ink
            .map(|ink| (ink.width, ink.height))
            .unwrap_or((0.0, 0.0));
        Self {
            width: layout.width,
            height: layout.height,
            content_width,
            content_height,
        }
    }
}

impl Serialize for TextLayoutSize {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        TextLayoutSizeFields::from(*self).serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for TextLayoutSize {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        Ok(TextLayoutSizeFields::deserialize(deserializer)?.into())
    }
}

impl TextLayoutSize {
    /// Layout whose painted ink the host has not measured yet.
    pub fn new(width: f64, height: f64) -> Self {
        Self {
            width,
            height,
            ink: None,
        }
    }

    /// Layout together with its measured painted ink; a pair that is not a
    /// real measurement (see [`InkBox::new`]) is stored as unmeasured.
    pub fn with_content(width: f64, height: f64, content_width: f64, content_height: f64) -> Self {
        Self {
            width,
            height,
            ink: InkBox::new(content_width, content_height),
        }
    }

    pub fn width(self) -> f64 {
        self.width
    }

    pub fn height(self) -> f64 {
        self.height
    }

    /// The measured painted ink, if the host measured one.
    pub fn ink(self) -> Option<InkBox> {
        self.ink
    }

    /// The content box paint geometry must use: the measured ink, or the wrap
    /// rectangle while no measurement has landed. Paint geometry, scene
    /// items, and connector resolution all resolve through here, so they can
    /// never disagree about the painted edge.
    pub fn ink_or_wrap(self) -> (f64, f64) {
        self.ink
            .map(|ink| (ink.width, ink.height))
            .unwrap_or((self.width, self.height))
    }

    /// Keeps the stored ink and replaces only the wrap rectangle (user-driven
    /// frames such as creation defaults, moves, and resize drags).
    pub fn with_wrap(self, width: f64, height: f64) -> Self {
        Self {
            width,
            height,
            ink: self.ink,
        }
    }

    /// Keeps the wrap rectangle and replaces only the painted ink with a host
    /// report; `None` marks the ink unmeasured again.
    pub fn with_ink(self, ink: Option<InkBox>) -> Self {
        Self { ink, ..self }
    }

    /// Keeps the wrap width, replaces height and ink. Used when fixed-width
    /// text reflows and the top edge must stay put via a center adjustment.
    pub fn with_wrapped_height(self, height: f64, ink: Option<InkBox>) -> Self {
        Self {
            height,
            ink,
            ..self
        }
    }

    /// Scales a previously measured ink box with the font. Unmeasured ink
    /// stays unmeasured so consumers keep falling back to the wrap size.
    pub fn scaled_ink(self, scale: f64) -> Self {
        if !(scale.is_finite() && scale > 0.0) {
            return self;
        }
        Self {
            ink: self.ink.map(|ink| ink.scaled(scale)),
            ..self
        }
    }
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
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub text_element_id: Option<ElementId>,
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
    #[serde(default)]
    pub stroke_rendering_style: snow_draw_engine_core::arrow::StrokeRenderingStyle,
    #[serde(default)]
    pub stroke_rendering_seed: u32,
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
    Emboss = 4,
    SmartErase = 5,
    Brightness = 6,
    RestoreBackground = 7,
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct FilterData {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub auto_region_id: Option<u64>,
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
            auto_region_id: None,
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
        if points.is_empty()
            || (points.len() < 2 && filter_type != CanvasFilterType::RestoreBackground)
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
    ThoughtBubble,
}

impl RectangleData {
    pub fn into_highlight(mut self, shape: HighlightShape) -> Self {
        self.text_element_id = None;
        debug_assert!(shape != HighlightShape::Diamond);
        self.rectangle_kind = RectangleElementKind::RectangleHighlight;
        self.highlight_shape = shape;
        self.corner_radii = CornerRadii::default();
        self.fill_style = FillStyle::Solid;
        self.stroke_style = StrokeStyle::Solid;
        self.stroke_rendering_style = Default::default();
        self
    }

    pub fn is_highlight(&self) -> bool {
        self.rectangle_kind == RectangleElementKind::RectangleHighlight
    }

    pub fn into_spotlight(mut self) -> Self {
        self.text_element_id = None;
        self.rectangle_kind = RectangleElementKind::Spotlight;
        self.fill = ColorRgba8::default();
        self.stroke = ColorRgba8::default();
        self.stroke_width = 0.0;
        self.fill_style = FillStyle::Solid;
        self.stroke_style = StrokeStyle::Solid;
        self.stroke_rendering_style = Default::default();
        self.opacity = 1.0;
        self
    }

    pub fn is_spotlight(&self) -> bool {
        self.rectangle_kind == RectangleElementKind::Spotlight
    }

    pub fn supports_corner_radius(&self) -> bool {
        !self.is_highlight()
            && !matches!(
                self.highlight_shape,
                HighlightShape::Ellipse | HighlightShape::ThoughtBubble
            )
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
    Border = 3,
}

impl FillStyle {
    pub fn is_visible(self, color: ColorRgba8) -> bool {
        self != Self::Border && color.a != 0
    }
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum SerialNumberType {
    #[default]
    OutlinedCircle = 0,
    SolidCircle = 1,
    OutlinedSquare = 2,
    SolidSquare = 3,
    Circle = 4,
}

impl SerialNumberType {
    pub fn supports_number(self) -> bool {
        self != Self::Circle
    }

    pub fn is_square(self) -> bool {
        matches!(self, Self::OutlinedSquare | Self::SolidSquare)
    }

    pub fn is_solid(self) -> bool {
        matches!(self, Self::SolidCircle | Self::SolidSquare)
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct TextData {
    pub center: Point<f64>,
    /// Wrap rectangle and painted ink as one stored measurement. Assign this
    /// field as a whole (`TextLayoutSize::new` / `with_content` / `with_wrap`
    /// / `with_ink`); splitting wrap from ink left serial connectors and dirty
    /// regions anchored to stale edges. Flattened so on-disk documents keep
    /// the legacy `width` / `height` / `content_width` / `content_height`
    /// keys, with `0` ink reading back as unmeasured.
    #[serde(flatten)]
    pub layout: TextLayoutSize,
    pub rotation: f64,
    pub text: String,
    /// Extra local padding measured by the host for glyph overhangs.
    pub text_ink_reserve: f64,
    pub color: ColorRgba8,
    pub font_size: f64,
    #[serde(default)]
    pub bold: bool,
    #[serde(default)]
    pub italic: bool,
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
    pub fn width(&self) -> f64 {
        self.layout.width()
    }

    pub fn height(&self) -> f64 {
        self.layout.height()
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct SerialNumberData {
    pub center: Point<f64>,
    pub diameter: f64,
    pub rotation: f64,
    pub number: i64,
    #[serde(default, rename = "type")]
    pub serial_number_type: SerialNumberType,
    #[serde(default)]
    pub numeric_type: crate::SerialNumberNumericType,
    pub color: ColorRgba8,
    pub fill: ColorRgba8,
    pub fill_style: FillStyle,
    pub font_size: f64,
    pub font_family: Option<String>,
    pub stroke_width: f64,
    pub stroke_style: StrokeStyle,
    #[serde(default)]
    pub stroke_rendering_style: snow_draw_engine_core::arrow::StrokeRenderingStyle,
    #[serde(default)]
    pub stroke_rendering_seed: u32,
    pub opacity: f64,
    pub text_element_id: Option<ElementId>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub enum ElementData {
    Rectangle(RectangleData),
    SpeechBubble(crate::SpeechBubbleData),
    Magnifier(crate::MagnifierData),
    Filter(FilterData),
    PenFilter(PenFilterData),
    Arrow(ArrowData),
    FreeDraw(FreeDrawData),
    Text(TextData),
    SerialNumber(SerialNumberData),
}

impl ElementData {
    /// Normalize legacy angles at import boundaries, before strict validation.
    pub fn normalize_angle_invariant(&mut self) -> Result<(), ErrorCode> {
        if let Self::Arrow(arrow) = self
            && arrow.is_angle()
        {
            crate::normalize_angle_in_place(arrow)?;
        }
        Ok(())
    }

    pub fn kind(&self) -> ElementKind {
        match self {
            Self::Rectangle(rect) => rect.element_kind(),
            Self::SpeechBubble(_) => ElementKind::SpeechBubble,
            Self::Magnifier(_) => ElementKind::Magnifier,
            Self::Filter(filter) => {
                if filter.auto_region_id.is_some() {
                    ElementKind::AutoFilter
                } else {
                    ElementKind::Filter
                }
            }
            Self::PenFilter(_) => ElementKind::PenFilter,
            Self::Arrow(arrow) => arrow.element_kind(),
            Self::FreeDraw(_) => ElementKind::FreeDraw,
            Self::Text(_) => ElementKind::Text,
            Self::SerialNumber(_) => ElementKind::SerialNumber,
        }
    }

    pub fn is_filter(&self) -> bool {
        self.kind().is_filter()
    }

    /// Tests geometry only; callers choose visibility, locking, and filter eligibility.
    pub fn hit_test(&self, point: Point<f64>, hit_tolerance: f64) -> bool {
        match self {
            Self::Rectangle(rect) => rectangle_hit_test(rect, point, hit_tolerance),
            Self::SpeechBubble(value) => crate::speech_bubble_hit_test(value, point, hit_tolerance),
            Self::Magnifier(value) => crate::magnifier_hit_test(value, point, hit_tolerance),
            Self::Filter(filter) => crate::filter_hit_test(filter, point, hit_tolerance),
            Self::PenFilter(filter) => crate::pen_filter_hit_test(filter, point, hit_tolerance),
            Self::Arrow(arrow) => arrow_hit_test(arrow, point, hit_tolerance),
            Self::FreeDraw(free_draw) => free_draw_hit_test(free_draw, point, hit_tolerance),
            Self::Text(text) => text_hit_test(text, point, hit_tolerance),
            Self::SerialNumber(serial) => serial_number_hit_test(serial, point, hit_tolerance),
        }
    }

    pub(crate) fn bounds(&self) -> DrawRect {
        match self {
            Self::Rectangle(rect) => crate::rect_paint_bounds(rect),
            Self::SpeechBubble(value) => crate::speech_bubble_bounds(value),
            Self::Magnifier(value) => crate::magnifier_interaction_bounds(value),
            Self::Filter(filter) => filter_bounds(filter),
            Self::PenFilter(filter) => pen_filter_bounds(filter),
            Self::Arrow(arrow) => crate::arrow_paint_bounds(arrow),
            Self::FreeDraw(free_draw) => free_draw_bounds(free_draw),
            Self::Text(text) => text_bounds(text),
            Self::SerialNumber(serial) => crate::serial_number_paint_bounds(serial),
        }
    }

    /// The unpainted element rectangle corresponding to reference
    /// `ElementState.rect`. Stroke and other visual overflow are excluded.
    pub fn state_rect(&self) -> DrawRect {
        match self {
            Self::Rectangle(rect) => centered_rect(rect.center, rect.width, rect.height),
            Self::SpeechBubble(value) => {
                centered_rect(value.body.center, value.body.width, value.body.height)
            }
            Self::Magnifier(value) => centered_rect(value.source_center, value.width, value.height),
            Self::Filter(filter) => centered_rect(filter.center, filter.width, filter.height),
            Self::PenFilter(filter) => DrawRect::new(
                filter.x,
                filter.y,
                filter.x + filter.width,
                filter.y + filter.height,
            ),
            Self::Arrow(arrow) if arrow.is_angle() || arrow.is_bracket() => {
                let bounds = arrow_bounds(arrow);
                let half_stroke = arrow.stroke_width.max(0.0) / 2.0;
                DrawRect::new(
                    bounds.min_x + half_stroke,
                    bounds.min_y + half_stroke,
                    bounds.max_x - half_stroke,
                    bounds.max_y - half_stroke,
                )
            }
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
            Self::Text(text) => centered_rect(text.center, text.width(), text.height()),
            Self::SerialNumber(serial) => {
                centered_rect(serial.center, serial.diameter, serial.diameter)
            }
        }
    }

    pub fn rotation(&self) -> f64 {
        match self {
            Self::Rectangle(rect) => rect.rotation,
            Self::SpeechBubble(value) => value.body.rotation,
            Self::Magnifier(value) => value.rotation,
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
            Self::SpeechBubble(value) => value.body.opacity,
            Self::Magnifier(value) => value.opacity,
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
    Magnifier,
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
    AutoFilter,
    Distance,
    Angle,
    SpeechBubble,
    Bracket,
}

impl ElementKind {
    /// Coverage overlays, not layout objects. New variants must choose a branch.
    pub const fn is_filter(self) -> bool {
        match self {
            Self::Filter | Self::AutoFilter | Self::PenFilter => true,
            Self::Rectangle
            | Self::Magnifier
            | Self::Arrow
            | Self::Line
            | Self::FreeDraw
            | Self::RectangleHighlight
            | Self::PenHighlight
            | Self::Text
            | Self::SerialNumber
            | Self::Spotlight
            | Self::Distance
            | Self::Angle
            | Self::Bracket => false,
            Self::SpeechBubble => false,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct ElementRecord {
    pub id: ElementId,
    pub meta: ElementMeta,
    pub data: ElementData,
}

/// Canonical common fields plus the type-specific element payload.
///
/// This mirrors the reference `ElementState` boundary; the payload-specific
/// storage behind `data` is an implementation detail.
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

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Document {
    pub(crate) slots: Vec<Option<ElementRecord>>,
    pub(crate) paint_order: Vec<ElementId>,
    revision: DocumentRevision,
    next_index: u32,
    watermark: WatermarkConfig,
    spotlight: SpotlightConfig,
    #[serde(default)]
    pub(crate) auto_filter_regions: Option<crate::AutoFilterRegionRecord>,
    /// Full synchronization certifies ownership, generated labels, and canonical
    /// paint order. Deserialized documents must establish these invariants again.
    #[serde(skip)]
    annotation_sync_certified: bool,
    /// Derived only after complete ownership/order validation. Local edits use
    /// this reverse index to synchronize an edited label without a scene scan.
    #[serde(skip)]
    synchronized_label_owners: HashMap<ElementId, ElementId>,
    #[serde(skip)]
    paint_ranks: std::sync::OnceLock<Vec<Option<(u32, u32)>>>,
    #[cfg(test)]
    #[serde(skip)]
    synchronization_work: (u64, u64),
}

#[derive(Default)]
struct LocalSynchronization {
    arrows: Vec<(ElementId, ElementId)>,
    bubbles: Vec<(ElementId, ElementId)>,
}

impl PartialEq for Document {
    fn eq(&self, other: &Self) -> bool {
        self.slots == other.slots
            && self.paint_order == other.paint_order
            && self.revision == other.revision
            && self.next_index == other.next_index
            && self.watermark == other.watermark
            && self.spotlight == other.spotlight
            && self.auto_filter_regions == other.auto_filter_regions
    }
}

impl ElementData {
    /// Original-background restoration coverage cannot be selected or edited.
    pub fn is_background_restore(&self) -> bool {
        match self {
            Self::Filter(f) => f.filter_type == CanvasFilterType::RestoreBackground,
            Self::PenFilter(f) => f.filter_type == CanvasFilterType::RestoreBackground,
            _ => false,
        }
    }

    pub fn is_smart_erase(&self) -> bool {
        match self {
            Self::Filter(f) => f.filter_type == CanvasFilterType::SmartErase,
            Self::PenFilter(f) => f.filter_type == CanvasFilterType::SmartErase,
            _ => false,
        }
    }

    pub fn normalize_smart_erase(&mut self) {
        match self {
            Self::Filter(f) if f.filter_type == CanvasFilterType::SmartErase => f.strength = 0.5,
            Self::PenFilter(f) if f.filter_type == CanvasFilterType::SmartErase => f.strength = 0.5,
            _ => {}
        }
    }
}

impl Document {
    /// Canonical committed content for dirty checks, independent of history and allocation state.
    /// Deleted trailing slots and the revision/next-index counters do not affect document content.
    pub fn content_checkpoint(&self) -> impl Serialize + '_ {
        #[derive(Serialize)]
        struct Content<'a> {
            slots: &'a [Option<ElementRecord>],
            paint_order: &'a [ElementId],
            watermark: &'a WatermarkConfig,
            spotlight: &'a SpotlightConfig,
            auto_filter_regions: &'a Option<crate::AutoFilterRegionRecord>,
        }
        let slot_count = self
            .slots
            .iter()
            .rposition(Option::is_some)
            .map_or(0, |index| index + 1);
        Content {
            slots: &self.slots[..slot_count],
            paint_order: &self.paint_order,
            watermark: &self.watermark,
            spotlight: &self.spotlight,
            auto_filter_regions: &self.auto_filter_regions,
        }
    }

    pub fn normalize_angle_invariants(&mut self) -> Result<(), ErrorCode> {
        for element in self.slots.iter_mut().flatten() {
            element.data.normalize_angle_invariant()?;
        }
        Ok(())
    }

    pub fn normalize_filter_invariants(&mut self) {
        for element in self.slots.iter_mut().flatten() {
            element.data.normalize_smart_erase();
        }
        self.normalize_auto_filter_order(&mut DocumentDelta::default());
    }

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
        self.validate_auto_filters()?;

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
                if binding.element_id == element.id
                    || !active_ids.contains(&binding.element_id)
                    || self
                        .element(binding.element_id)
                        .is_ok_and(|target| target.data.is_background_restore())
                {
                    return Err(ErrorCode::InvalidArgument);
                }
            }
        }
        self.validate_arrow_text_bindings()
    }

    /// Recompute owned annotation labels before replaying saved history, which
    /// may contain font sizes generated by an older sizing rule.
    pub fn normalize_distance_label_invariants(&mut self) -> Result<(), ErrorCode> {
        self.normalize_generated_annotation_label_invariants()
    }

    pub fn normalize_generated_annotation_label_invariants(&mut self) -> Result<(), ErrorCode> {
        for (arrow_id, text_id) in self.arrow_text_bindings() {
            let arrow = self.arrow(arrow_id)?;
            if !arrow.is_generated_annotation() {
                continue;
            }
            let text = self.text(text_id)?;
            let derived = crate::generated_annotation_label(arrow, Some(text.layout))
                .ok_or(ErrorCode::InvalidArgument)?;
            self.lookup_mut(text_id)?.data = ElementData::Text(derived);
        }
        Ok(())
    }

    pub fn arrow_text_bindings(&self) -> Vec<(ElementId, ElementId)> {
        self.paint_order
            .iter()
            .filter_map(|id| {
                self.arrow(*id)
                    .ok()?
                    .text_element_id
                    .map(|text_id| (*id, text_id))
            })
            .collect()
    }

    pub fn bound_text_id_for_arrow(&self, id: ElementId) -> Option<ElementId> {
        self.arrow(id).ok()?.text_element_id
    }

    pub fn arrow_id_for_text(&self, text_id: ElementId) -> Option<ElementId> {
        self.paint_order
            .iter()
            .copied()
            .find(|id| self.bound_text_id_for_arrow(*id) == Some(text_id))
    }

    fn validate_arrow_text_bindings(&self) -> Result<(), ErrorCode> {
        let mut owned = HashSet::new();
        let serial_labels: HashSet<_> = self
            .serial_number_text_bindings()
            .into_iter()
            .map(|binding| binding.text_id)
            .collect();
        for (arrow_id, text_id) in self.arrow_text_bindings() {
            if !matches!(
                self.arrow(arrow_id)?.linear_kind,
                crate::LinearElementKind::Arrow
                    | crate::LinearElementKind::Distance
                    | crate::LinearElementKind::Angle
            ) || self.text(text_id).is_err()
                || !owned.insert(text_id)
                || serial_labels.contains(&text_id)
            {
                return Err(ErrorCode::InvalidArgument);
            }
        }
        for (_, text_id) in self.speech_bubble_text_bindings() {
            if self.text(text_id).is_err()
                || !owned.insert(text_id)
                || serial_labels.contains(&text_id)
            {
                return Err(ErrorCode::InvalidArgument);
            }
        }
        for id in &self.paint_order {
            let Ok(shape) = self.rectangle(*id) else {
                continue;
            };
            let Some(text_id) = shape.text_element_id else {
                continue;
            };
            if shape.rectangle_kind != RectangleElementKind::Rectangle
                || self.text(text_id).is_err()
                || !owned.insert(text_id)
                || serial_labels.contains(&text_id)
            {
                return Err(ErrorCode::InvalidArgument);
            }
        }
        Ok(())
    }

    fn synchronize_arrow_text(
        &mut self,
        inverse: &mut Vec<Operation>,
        changes: &mut DocumentDelta,
    ) -> Result<(), ErrorCode> {
        // Direct document deletion (including annotation batches) may remove a
        // label without going through the editor's dependency expansion. Detach
        // only links removed by this transaction; malformed new links and
        // imported documents still fail ownership validation below.
        if !changes.removed.is_empty() {
            let removed: HashSet<_> = changes.removed.iter().copied().collect();
            let detached: Vec<_> = self
                .paint_order
                .iter()
                .filter_map(|id| {
                    let mut shape = *self.rectangle(*id).ok()?;
                    let label = shape.text_element_id?;
                    if !removed.contains(&label) || self.text(label).is_ok() {
                        return None;
                    }
                    shape.text_element_id = None;
                    Some((*id, shape))
                })
                .collect();
            for (id, shape) in detached {
                inverse.push(self.apply_operation(
                    &Operation::UpdateElementData {
                        id,
                        data: ElementData::Rectangle(shape),
                    },
                    changes,
                )?);
            }
        }
        self.validate_arrow_text_bindings()?;
        let bindings = self.arrow_text_bindings();
        self.synchronize_arrow_text_pairs(&bindings, inverse, changes)?;
        let bubble_bindings = self.speech_bubble_text_bindings();
        self.synchronize_bubble_text_pairs(&bubble_bindings, inverse, changes)?;
        let bindings: Vec<_> = bindings.into_iter().chain(bubble_bindings).collect();
        if !bindings.is_empty() {
            let labels: HashSet<_> = bindings.iter().map(|(_, text)| *text).collect();
            let owners: HashMap<_, _> = bindings.iter().copied().collect();
            let mut order = Vec::with_capacity(self.paint_order.len());
            for id in &self.paint_order {
                if labels.contains(id) {
                    continue;
                }
                order.push(*id);
                if let Some(label) = owners.get(id) {
                    order.push(*label);
                }
            }
            if order != self.paint_order {
                inverse.push(self.apply_operation(
                    &Operation::ReorderElements {
                        ids: order,
                        paint_index: 0,
                    },
                    changes,
                )?);
            }
        }
        self.synchronized_label_owners = bindings
            .into_iter()
            .map(|(owner, label)| (label, owner))
            .collect();
        Ok(())
    }

    fn synchronize_bubble_text_pairs(
        &mut self,
        bindings: &[(ElementId, ElementId)],
        inverse: &mut Vec<Operation>,
        changes: &mut DocumentDelta,
    ) -> Result<(), ErrorCode> {
        for (bubble_id, text_id) in bindings {
            let bubble = self.speech_bubble(*bubble_id)?;
            let mut text = self.text(*text_id)?.clone();
            text.center = bubble.body.center;
            text.rotation = bubble.body.rotation;
            let meta = self.element(*bubble_id)?.meta;
            if self.text(*text_id)? != &text {
                inverse.push(self.apply_operation(
                    &Operation::UpdateElementData {
                        id: *text_id,
                        data: ElementData::Text(text),
                    },
                    changes,
                )?);
            }
            if self.element(*text_id)?.meta != meta {
                inverse.push(self.apply_operation(
                    &Operation::UpdateElementMeta { id: *text_id, meta },
                    changes,
                )?);
            }
        }
        Ok(())
    }

    fn synchronize_arrow_text_pairs(
        &mut self,
        bindings: &[(ElementId, ElementId)],
        inverse: &mut Vec<Operation>,
        changes: &mut DocumentDelta,
    ) -> Result<(), ErrorCode> {
        for (arrow_id, text_id) in bindings {
            let arrow = self.arrow(*arrow_id)?;
            let mut text = self.text(*text_id)?.clone();
            if let Some(derived) = crate::generated_annotation_label(arrow, Some(text.layout)) {
                text = derived;
            } else {
                text.center = crate::arrow_text_anchor(arrow);
            }
            text.rotation = 0.0;
            text.auto_resize = true;
            let meta = self.element(*arrow_id)?.meta;
            if self.element(*text_id)?.meta != meta {
                inverse.push(self.apply_operation(
                    &Operation::UpdateElementMeta { id: *text_id, meta },
                    changes,
                )?);
            }
            if self.text(*text_id)? != &text {
                inverse.push(self.apply_operation(
                    &Operation::UpdateElementData {
                        id: *text_id,
                        data: ElementData::Text(text),
                    },
                    changes,
                )?);
            }
        }
        Ok(())
    }

    /// A successful full synchronization certifies ownership, label adjacency,
    /// and fixed filter layers. Data/meta edits preserving those relationships
    /// need only resynchronize their owners; validation of each payload remains
    /// in apply_operation. Structural/imported changes still take the full path.
    fn local_synchronization_plan(
        &self,
        transaction: &Transaction,
    ) -> Option<LocalSynchronization> {
        if !self.annotation_sync_certified {
            return None;
        }
        let mut owners = HashSet::new();
        for operation in transaction.operations() {
            let (id, previous) = match operation {
                Operation::UpdateElementData { id, data } => {
                    let previous = &self.element(*id).ok()?.data;
                    if previous.kind() != data.kind()
                        || element_data_relation_targets_changed(previous, data)
                        || previous.is_smart_erase() != data.is_smart_erase()
                    {
                        return None;
                    }
                    if let (ElementData::Filter(previous), ElementData::Filter(next)) =
                        (previous, data)
                        && (previous.auto_region_id != next.auto_region_id
                            || (next.auto_region_id.is_some()
                                && (next.opacity != 1.0 || next.rotation != 0.0)))
                    {
                        return None;
                    }
                    (*id, previous)
                }
                Operation::UpdateElementMeta { id, .. } => (*id, &self.element(*id).ok()?.data),
                Operation::UpdateWatermark { .. } | Operation::UpdateSpotlight { .. } => continue,
                _ => return None,
            };
            match previous {
                ElementData::Arrow(arrow) if arrow.text_element_id.is_some() => {
                    owners.insert(id);
                }
                ElementData::SpeechBubble(bubble) if bubble.text_element_id.is_some() => {
                    owners.insert(id);
                }
                ElementData::Text(_) => {
                    if let Some(owner) = self.synchronized_label_owners.get(&id) {
                        owners.insert(*owner);
                    }
                }
                _ => {}
            }
        }
        // Stable owner order makes inverse transactions and delta ordering
        // deterministic, independent of HashSet insertion/randomization.
        let mut owners: Vec<_> = owners.into_iter().collect();
        owners.sort_unstable();
        let mut plan = LocalSynchronization::default();
        for owner in owners {
            match &self.element(owner).ok()?.data {
                ElementData::Arrow(arrow) => {
                    plan.arrows.push((owner, arrow.text_element_id?));
                }
                ElementData::SpeechBubble(bubble) => {
                    plan.bubbles.push((owner, bubble.text_element_id?));
                }
                _ => return None,
            }
        }
        Some(plan)
    }

    #[cfg(test)]
    fn annotation_sync_pairs_for_transaction(
        &self,
        transaction: &Transaction,
    ) -> Option<Vec<(ElementId, ElementId)>> {
        self.local_synchronization_plan(transaction)
            .map(|plan| plan.arrows)
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
        let z_index = self.paint_rank(id).ok_or(ErrorCode::NotFound)? as usize;
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

    pub fn magnifier(&self, id: ElementId) -> Result<&crate::MagnifierData, ErrorCode> {
        match &self.element(id)?.data {
            ElementData::Magnifier(value) => Ok(value),
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

    pub fn speech_bubble(&self, id: ElementId) -> Result<&crate::SpeechBubbleData, ErrorCode> {
        match &self.element(id)?.data {
            ElementData::SpeechBubble(value) => Ok(value),
            _ => Err(ErrorCode::InvalidArgument),
        }
    }

    pub fn speech_bubble_text_bindings(&self) -> Vec<(ElementId, ElementId)> {
        self.paint_order
            .iter()
            .filter_map(|id| {
                self.speech_bubble(*id)
                    .ok()?
                    .text_element_id
                    .map(|text| (*id, text))
            })
            .collect()
    }

    pub fn bound_text_id_for_speech_bubble(&self, id: ElementId) -> Option<ElementId> {
        self.speech_bubble(id).ok()?.text_element_id
    }

    pub fn speech_bubble_id_for_text(&self, text_id: ElementId) -> Option<ElementId> {
        self.paint_order
            .iter()
            .copied()
            .find(|id| self.bound_text_id_for_speech_bubble(*id) == Some(text_id))
    }

    pub fn text_owner_id(&self, text_id: ElementId) -> Option<ElementId> {
        self.arrow_id_for_text(text_id)
            .or_else(|| self.speech_bubble_id_for_text(text_id))
    }

    pub fn element_kind(&self, id: ElementId) -> Result<ElementKind, ErrorCode> {
        Ok(self.element(id)?.data.kind())
    }

    pub fn element_rect_proxy(&self, id: ElementId) -> Option<RectangleData> {
        if let Ok(value) = self.speech_bubble(id) {
            return Some(crate::speech_bubble_body_proxy(value));
        }
        if let Ok(rect) = self.rectangle(id) {
            return Some(*rect);
        }
        if let Ok(value) = self.magnifier(id) {
            return Some(value.source_rect());
        }
        if let Ok(filter) = self.filter(id) {
            return Some(filter_rect_proxy(filter));
        }
        if let Ok(filter) = self.pen_filter(id) {
            return Some(pen_filter_rect_proxy(filter));
        }
        if let Ok(free_draw) = self.free_draw(id) {
            return Some(RectangleData {
                text_element_id: None,
                stroke_rendering_seed: 0,
                stroke_rendering_style: Default::default(),
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
                text_element_id: None,
                stroke_rendering_seed: 0,
                stroke_rendering_style: Default::default(),
                rectangle_kind: RectangleElementKind::Rectangle,
                highlight_shape: HighlightShape::Rectangle,
                center: text.center,
                width: text.width(),
                height: text.height(),
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
        let local_plan = self.local_synchronization_plan(transaction);
        let local_annotation_sync = local_plan.is_some();
        let start_annotation_sync_certified = self.annotation_sync_certified;
        // Type conversion changes fixed-layer membership. Its inverse must restore
        // the previous ordinary layer position as well as the previous type.
        let previous_order = transaction
            .operations
            .iter()
            .any(|operation| {
                if let Operation::UpdateElementData { id, data } = operation {
                    self.element(*id)
                        .is_ok_and(|element| element.data.is_smart_erase() != data.is_smart_erase())
                } else {
                    false
                }
            })
            .then(|| self.paint_order.clone());
        let mut changes = DocumentDelta::default();
        let start_revision = self.revision;
        let start_next_index = self.next_index;
        let start_slots_len = self.slots.len();

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
                    self.slots.truncate(start_slots_len);
                    self.annotation_sync_certified = start_annotation_sync_certified;
                    return Err(error);
                }
            }
        }

        let synchronized = if let Some(plan) = local_plan {
            #[cfg(test)]
            {
                self.synchronization_work.1 += (plan.arrows.len() + plan.bubbles.len()) as u64;
            }
            self.synchronize_arrow_text_pairs(&plan.arrows, &mut inverse, &mut changes)
                .and_then(|()| {
                    self.synchronize_bubble_text_pairs(&plan.bubbles, &mut inverse, &mut changes)
                })
        } else {
            #[cfg(test)]
            {
                self.synchronization_work.0 += self.paint_order.len() as u64;
            }
            self.validate_auto_filters()
                .and_then(|()| self.synchronize_arrow_text(&mut inverse, &mut changes))
        };
        if let Err(error) = synchronized {
            for operation in inverse.iter().rev() {
                self.apply_operation(operation, &mut DocumentDelta::default())
                    .expect("document rollback must succeed");
            }
            self.revision = start_revision;
            self.next_index = start_next_index;
            self.slots.truncate(start_slots_len);
            self.annotation_sync_certified = start_annotation_sync_certified;
            return Err(error);
        }
        if !local_annotation_sync {
            self.normalize_auto_filter_order(&mut changes);
        }
        self.annotation_sync_certified = true;
        inverse.reverse();
        if let Some(ids) = previous_order.filter(|ids| !ids.is_empty()) {
            inverse.push(Operation::ReorderElements {
                ids,
                paint_index: 0,
            });
        }
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

    pub fn topmost_element_at_with_tolerance(
        &self,
        point: Point<f64>,
        hit_tolerance: f64,
    ) -> Option<(ElementId, ElementKind)> {
        self.paint_order.iter().rev().find_map(|id| {
            let element = self.element(*id).ok()?;
            if !element.meta.visible || element.data.is_filter() {
                return None;
            }
            let kind = match &element.data {
                ElementData::Rectangle(_) => ElementKind::Rectangle,
                data => data.kind(),
            };
            element
                .data
                .hit_test(point, hit_tolerance)
                .then_some((*id, kind))
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

    pub fn is_text_bound_to_serial_number(&self, text_id: ElementId) -> bool {
        self.serial_number_text_bindings()
            .into_iter()
            .any(|binding| binding.text_id == text_id)
    }

    pub fn bound_text_id_for_serial_number(&self, serial_id: ElementId) -> Option<ElementId> {
        let text_id = self.serial_number(serial_id).ok()?.text_element_id?;
        self.text(text_id).ok().map(|_| text_id)
    }

    pub fn bound_text_id_for_shape(&self, shape_id: ElementId) -> Option<ElementId> {
        let shape = self.rectangle(shape_id).ok()?;
        if shape.rectangle_kind != RectangleElementKind::Rectangle {
            return None;
        }
        let text_id = shape.text_element_id?;
        self.text(text_id).ok().map(|_| text_id)
    }

    pub fn shape_ids_with_text(&self, text_id: ElementId) -> Vec<ElementId> {
        self.paint_order
            .iter()
            .copied()
            .filter(|id| self.bound_text_id_for_shape(*id) == Some(text_id))
            .collect()
    }

    pub fn serial_number_ids_with_text(&self, text_id: ElementId) -> Vec<ElementId> {
        self.serial_number_text_bindings()
            .into_iter()
            .filter(|binding| binding.text_id == text_id)
            .map(|binding| binding.serial_id)
            .collect()
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
        Some(ElementChangeSnapshot {
            kind: element.data.kind(),
            bounds,
            paint_index: self.paint_rank(id),
        })
    }

    fn paint_rank(&self, id: ElementId) -> Option<u32> {
        self.paint_ranks
            .get_or_init(|| {
                let mut ranks = vec![None; self.slots.len()];
                for (rank, id) in self.paint_order.iter().enumerate() {
                    if let Some(entry) = ranks.get_mut(id.index as usize) {
                        *entry = Some((id.generation, rank as u32));
                    }
                }
                ranks
            })
            .get(id.index as usize)
            .and_then(|entry| *entry)
            .and_then(|(generation, rank)| (generation == id.generation).then_some(rank))
    }

    pub(crate) fn invalidate_paint_ranks(&mut self) {
        self.paint_ranks.take();
    }

    fn apply_operation(
        &mut self,
        operation: &Operation,
        changes: &mut ChangeSet,
    ) -> Result<Operation, ErrorCode> {
        let mut normalized;
        let operation = match operation {
            Operation::InsertElement { data, .. } | Operation::UpdateElementData { data, .. }
                if data.is_smart_erase() =>
            {
                normalized = operation.clone();
                match &mut normalized {
                    Operation::InsertElement { data, .. }
                    | Operation::UpdateElementData { data, .. } => data.normalize_smart_erase(),
                    _ => unreachable!(),
                }
                &normalized
            }
            Operation::RestoreElement { element, .. } if element.data.is_smart_erase() => {
                normalized = operation.clone();
                if let Operation::RestoreElement { element, .. } = &mut normalized {
                    element.data.normalize_smart_erase();
                }
                &normalized
            }
            _ => operation,
        };
        match operation {
            Operation::UpdateAutoFilterRegions { record } => {
                if let Some(record) = record {
                    record.validate()?;
                }
                let inverse = std::mem::replace(&mut self.auto_filter_regions, record.clone());
                Ok(Operation::UpdateAutoFilterRegions { record: inverse })
            }
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
                self.validate_background_restore_bindings_for_data(data)?;
                let relations_changed =
                    element_data_has_relations(data) || self.has_arrow_bound_to_element(*id);
                if data.is_background_restore() && relations_changed {
                    return Err(ErrorCode::InvalidArgument);
                }
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
                if relations_changed {
                    changes.relations_changed = true;
                }
                Ok(Operation::RemoveElement { id: *id })
            }
            Operation::UpdateElementData { id, data } => {
                validate_element_data(data)?;
                self.validate_background_restore_bindings_for_data(data)?;
                let old_snapshot = self.element_change_snapshot(*id);
                changes.note_existing_bounds(self, *id);
                let element = self.lookup_mut(*id)?;
                if element.meta.locked
                    || element.data.is_background_restore()
                    || data.is_background_restore()
                {
                    return Err(ErrorCode::InvalidState);
                }
                let inverse_data = element.data.clone();
                if element.data.kind() != data.kind() {
                    return Err(ErrorCode::InvalidArgument);
                }
                let relations_changed = element_data_relation_targets_changed(&inverse_data, data);
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
                if element.data.is_background_restore() {
                    return Err(ErrorCode::InvalidState);
                }
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
                let paint_index = self.paint_rank(*id).ok_or(ErrorCode::NotFound)?;
                let element = self.lookup(*id)?.clone();
                if element.meta.locked {
                    return Err(ErrorCode::InvalidState);
                }
                let relations_changed = element_data_has_relations(&element.data)
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
                validate_element_data(&element.data)?;
                self.validate_background_restore_bindings_for_data(&element.data)?;
                let relations_changed = element_data_has_relations(&element.data)
                    || self.has_arrow_bound_to_element(element.id);
                if element.data.is_background_restore() && relations_changed {
                    return Err(ErrorCode::InvalidArgument);
                }
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
                if relations_changed {
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
                // A reorder can permute or gather disjoint layers. Retain the
                // complete prior order so undo also restores intervening items.
                let inverse_order = self.paint_order.clone();
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
                    ids: inverse_order,
                    paint_index: 0,
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
        if paint_index == self.paint_order.len() {
            if let Some(ranks) = self.paint_ranks.get_mut() {
                ranks.resize(self.slots.len(), None);
                ranks[index] = Some((id.generation, paint_index as u32));
            }
        } else {
            self.invalidate_paint_ranks();
        }
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
        self.invalidate_paint_ranks();
        Ok(())
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
        self.invalidate_paint_ranks();
        Ok(())
    }

    fn validate_background_restore_bindings_for_data(
        &self,
        data: &ElementData,
    ) -> Result<(), ErrorCode> {
        if let ElementData::Arrow(arrow) = data {
            for binding in [arrow.start_binding.as_ref(), arrow.end_binding.as_ref()]
                .into_iter()
                .flatten()
            {
                if self
                    .element(binding.element_id)
                    .is_ok_and(|target| target.data.is_background_restore())
                {
                    return Err(ErrorCode::InvalidArgument);
                }
            }
        }
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

fn element_data_has_relations(data: &ElementData) -> bool {
    match data {
        ElementData::Rectangle(shape) => shape.text_element_id.is_some(),
        ElementData::Arrow(arrow) => {
            arrow.text_element_id.is_some() || !arrow.bound_element_ids().is_empty()
        }
        ElementData::SerialNumber(serial) => serial.text_element_id.is_some(),
        ElementData::SpeechBubble(value) => value.text_element_id.is_some(),
        _ => false,
    }
}

fn element_data_relation_targets_changed(previous: &ElementData, next: &ElementData) -> bool {
    if let (ElementData::Rectangle(previous), ElementData::Rectangle(next)) = (previous, next) {
        return previous.text_element_id != next.text_element_id;
    }
    if let (ElementData::SpeechBubble(previous), ElementData::SpeechBubble(next)) = (previous, next)
    {
        return previous.text_element_id != next.text_element_id;
    }
    if let (ElementData::SerialNumber(previous), ElementData::SerialNumber(next)) = (previous, next)
    {
        return previous.text_element_id != next.text_element_id;
    }
    let (ElementData::Arrow(previous), ElementData::Arrow(next)) = (previous, next) else {
        return false;
    };
    let previous_ids = previous.bound_element_ids();
    let next_ids = next.bound_element_ids();
    previous.text_element_id != next.text_element_id
        || previous_ids.len() != next_ids.len()
        || previous_ids.iter().any(|id| !next_ids.contains(id))
}

#[cfg(test)]
mod shape_text_binding_tests {
    use super::*;

    #[test]
    fn shape_text_binding_round_trips_and_legacy_payload_defaults_to_none() {
        let mut shape = crate::MagnifierData::default().magnified_rect();
        let legacy = serde_json::to_value(shape).unwrap();
        assert!(legacy.get("text_element_id").is_none());
        assert_eq!(
            serde_json::from_value::<RectangleData>(legacy).unwrap(),
            shape
        );
        let mut document = Document::new();
        let owner = document.allocate_element_id();
        let label = document.allocate_element_id();
        shape.text_element_id = Some(label);
        let mut insert = Transaction::new("shape text");
        insert.insert_rectangle(owner, ElementMeta::default(), shape);
        insert.insert_text(label, ElementMeta::default(), TextData::default());
        document.apply(&insert).unwrap();
        assert_eq!(document.bound_text_id_for_shape(owner), Some(label));
        assert_eq!(document.shape_ids_with_text(label), vec![owner]);
        let restored: Document =
            serde_json::from_str(&serde_json::to_string(&document).unwrap()).unwrap();
        restored.validate_session().unwrap();
        assert_eq!(restored.bound_text_id_for_shape(owner), Some(label));
        assert!(
            shape
                .into_highlight(HighlightShape::Rectangle)
                .text_element_id
                .is_none()
        );
        assert!(shape.into_spotlight().text_element_id.is_none());
        assert!(
            crate::SpeechBubbleData::new(shape)
                .body
                .text_element_id
                .is_none()
        );
    }

    #[test]
    fn shape_text_binding_rejects_missing_nontext_and_shared_labels() {
        let mut document = Document::new();
        let owner = document.allocate_element_id();
        let label = document.allocate_element_id();
        let mut shape = crate::MagnifierData::default().magnified_rect();
        shape.text_element_id = Some(label);
        let mut missing = Transaction::new("missing label");
        missing.insert_rectangle(owner, ElementMeta::default(), shape);
        assert_eq!(
            document.apply(&missing).unwrap_err(),
            ErrorCode::InvalidArgument
        );
        let mut self_bound = shape;
        self_bound.text_element_id = Some(owner);
        let mut invalid = Transaction::new("nontext label");
        invalid.insert_rectangle(owner, ElementMeta::default(), self_bound);
        assert_eq!(
            document.apply(&invalid).unwrap_err(),
            ErrorCode::InvalidArgument
        );
        missing.insert_text(label, ElementMeta::default(), TextData::default());
        document.apply(&missing).unwrap();
        let mut shared = Transaction::new("shared label");
        shared.insert_rectangle(
            document.allocate_element_id(),
            ElementMeta::default(),
            shape,
        );
        assert_eq!(
            document.apply(&shared).unwrap_err(),
            ErrorCode::InvalidArgument
        );
    }

    #[test]
    fn shape_text_binding_direct_label_removal_detaches_and_undo_restores() {
        let mut document = Document::new();
        let owner = document.allocate_element_id();
        let label = document.allocate_element_id();
        let mut shape = crate::MagnifierData::default().magnified_rect();
        shape.text_element_id = Some(label);
        let mut insert = Transaction::new("shape and text");
        insert.insert_rectangle(owner, ElementMeta::default(), shape);
        insert.insert_text(label, ElementMeta::default(), TextData::default());
        document.apply(&insert).unwrap();
        let mut remove = Transaction::new("direct label removal");
        remove.remove_element(label);
        let removed = document.apply(&remove).unwrap();
        assert!(document.rectangle(owner).unwrap().text_element_id.is_none());
        assert!(removed.changes.relations_changed);
        let restored = document.apply(&removed.inverse).unwrap();
        assert_eq!(document.bound_text_id_for_shape(owner), Some(label));
        document.apply(&restored.inverse).unwrap();
        assert!(document.rectangle(owner).unwrap().text_element_id.is_none());
    }
}

#[cfg(test)]
mod annotation_sync_tests {
    use super::*;
    use crate::{
        AngleAnnotation, DistanceAnnotation, LinearElementKind, generated_annotation_label,
    };
    use snow_draw_engine_core::arrow::ArrowType;

    fn fixture() -> (Document, [(ElementId, ElementId); 2]) {
        let mut document = Document::new();
        let mut transaction = Transaction::new("create annotations");
        let mut pairs = [(ElementId::default(), ElementId::default()); 2];
        for (index, pair) in pairs.iter_mut().enumerate() {
            let owner = document.allocate_element_id();
            let label = document.allocate_element_id();
            let points = if index == 0 {
                vec![
                    Point::new(100.0, 0.0),
                    Point::new(0.0, 0.0),
                    Point::new(0.0, -100.0),
                ]
            } else {
                vec![Point::new(200.0, 0.0), Point::new(300.0, 0.0)]
            };
            let mut arrow = ArrowData::from_global_points(
                &points,
                ColorRgba8 {
                    r: 20,
                    g: 80,
                    b: 200,
                    a: 255,
                },
                2.0,
                StrokeStyle::Solid,
                ArrowType::Straight,
                None,
                None,
            )
            .unwrap();
            if index == 0 {
                arrow.linear_kind = LinearElementKind::Angle;
                arrow.angle = Some(AngleAnnotation::default());
            } else {
                arrow.linear_kind = LinearElementKind::Distance;
                arrow.distance = Some(DistanceAnnotation::default());
            }
            arrow.text_element_id = Some(label);
            let text =
                generated_annotation_label(&arrow, Some(TextLayoutSize::new(72.0, 24.0))).unwrap();
            transaction.insert_arrow(owner, ElementMeta::default(), arrow);
            transaction.insert_text(label, ElementMeta::default(), text);
            *pair = (owner, label);
        }
        document.apply(&transaction).unwrap();
        assert!(document.annotation_sync_certified);
        (document, pairs)
    }

    fn owner_update(document: &Document, owner: ElementId) -> Transaction {
        let mut arrow = document.arrow(owner).unwrap().clone();
        arrow.stroke_width = 4.0;
        let mut transaction = Transaction::new("update annotation");
        transaction.update_arrow(owner, arrow);
        transaction
    }

    fn assert_derived_labels(document: &Document, pairs: &[(ElementId, ElementId)]) {
        for (owner, label) in pairs {
            let arrow = document.arrow(*owner).unwrap();
            let text = document.text(*label).unwrap();
            assert_eq!(
                text,
                &generated_annotation_label(arrow, Some(text.layout)).unwrap()
            );
            assert_eq!(
                document.element(*owner).unwrap().meta,
                document.element(*label).unwrap().meta
            );
        }
    }

    #[test]
    fn angle_annotation_sync_deserialization_repairs_order_and_unrelated_stale_labels_once() {
        let (document, pairs) = fixture();
        let mut saved = serde_json::to_value(&document).unwrap();
        assert!(saved.get("annotation_sync_certified").is_none());
        saved["paint_order"] =
            serde_json::to_value([pairs[0].0, pairs[1].0, pairs[1].1, pairs[0].1]).unwrap();
        saved["slots"][pairs[1].1.index as usize]["data"]["Text"]["text"] = "stale".into();
        let mut restored: Document = serde_json::from_value(saved).unwrap();
        assert!(!restored.annotation_sync_certified);
        let update = owner_update(&restored, pairs[0].0);
        assert!(
            restored
                .annotation_sync_pairs_for_transaction(&update)
                .is_none()
        );
        let repaired = restored.apply(&update).unwrap();
        assert!(restored.annotation_sync_certified);
        assert!(repaired.changes.z_order_changed);
        assert!(repaired.changes.touched.contains(&pairs[1].1));
        assert_eq!(restored.paint_order(), document.paint_order());
        assert_derived_labels(&restored, &pairs);

        let unrelated_label = restored.text(pairs[1].1).unwrap().clone();
        let mut local = owner_update(&restored, pairs[0].0);
        let mut arrow = restored.arrow(pairs[0].0).unwrap().clone();
        arrow.stroke_width = 6.0;
        local.update_arrow(pairs[0].0, arrow);
        assert_eq!(
            restored.annotation_sync_pairs_for_transaction(&local),
            Some(vec![pairs[0]])
        );
        let updated = restored.apply(&local).unwrap();
        assert!(!updated.changes.z_order_changed);
        assert!(!updated.changes.touched.contains(&pairs[1].1));
        assert_eq!(restored.text(pairs[1].1).unwrap(), &unrelated_label);
        assert_derived_labels(&restored, &pairs);
        assert!(restored.clone().annotation_sync_certified);
    }

    #[test]
    fn angle_annotation_sync_duplicate_ownership_and_failed_local_updates_restore_certificates() {
        let (mut document, pairs) = fixture();
        let original = document.clone();
        let mut duplicate = document.arrow(pairs[1].0).unwrap().clone();
        duplicate.text_element_id = Some(pairs[0].1);
        let mut invalid = Transaction::new("duplicate owner");
        invalid.update_arrow(pairs[1].0, duplicate);
        assert!(
            document
                .annotation_sync_pairs_for_transaction(&invalid)
                .is_none()
        );
        assert_eq!(document.apply(&invalid), Err(ErrorCode::InvalidArgument));
        assert_eq!(document, original);
        assert!(document.annotation_sync_certified);

        let mut saved = serde_json::to_value(&document).unwrap();
        saved["slots"][pairs[1].0.index as usize]["data"]["Arrow"]["text_element_id"] =
            serde_json::to_value(pairs[0].1).unwrap();
        let mut raw: Document = serde_json::from_value(saved).unwrap();
        let raw_original = raw.clone();
        assert_eq!(
            raw.apply(&owner_update(&raw, pairs[0].0)),
            Err(ErrorCode::InvalidArgument)
        );
        assert_eq!(raw, raw_original);
        assert!(!raw.annotation_sync_certified);

        let mut invalid_local = owner_update(&document, pairs[0].0);
        let mut text = document.text(pairs[0].1).unwrap().clone();
        text.font_size = 0.0;
        invalid_local.update_text(pairs[0].1, text);
        assert_eq!(
            document.annotation_sync_pairs_for_transaction(&invalid_local),
            Some(vec![pairs[0]])
        );
        assert_eq!(
            document.apply(&invalid_local),
            Err(ErrorCode::InvalidArgument)
        );
        assert_eq!(document, original);
        assert!(document.annotation_sync_certified);
    }

    #[test]
    fn angle_annotation_sync_multi_owner_transform_measurement_and_undo_remain_local() {
        let (mut document, pairs) = fixture();
        let original = document.clone();
        let mut update = Transaction::new("transform annotations");
        for (owner, label) in pairs {
            let mut arrow = document.arrow(owner).unwrap().clone();
            arrow.x += 20.0;
            arrow.y -= 30.0;
            arrow.stroke_width = 8.0;
            update.update_arrow(owner, arrow);
            let mut text = document.text(label).unwrap().clone();
            text.layout = TextLayoutSize::new(96.0, 30.0);
            update.update_text(label, text);
        }
        assert_eq!(
            document.annotation_sync_pairs_for_transaction(&update),
            Some(pairs.to_vec())
        );
        let applied = document.apply(&update).unwrap();
        assert!(!applied.changes.z_order_changed);
        assert_derived_labels(&document, &pairs);
        assert_eq!(
            document.annotation_sync_pairs_for_transaction(&applied.inverse),
            Some(pairs.to_vec())
        );
        document.apply(&applied.inverse).unwrap();
        assert!(document.has_same_session_content(&original));
        assert!(document.annotation_sync_certified);

        let mut meta = Transaction::new("hide annotation");
        meta.update_element_meta(
            pairs[0].0,
            ElementMeta {
                visible: false,
                locked: false,
            },
        );
        assert_eq!(
            document.annotation_sync_pairs_for_transaction(&meta),
            Some(vec![pairs[0]])
        );
        document.apply(&meta).unwrap();
        assert_derived_labels(&document, &pairs);
    }

    #[test]
    fn local_scene_edits_visit_no_unrelated_elements_in_ten_thousand_element_document() {
        let mut document = Document::new();
        let shape = crate::MagnifierData::default().magnified_rect();
        for index in 0..10_000 {
            let id = document.allocate_element_id();
            let data = if index % 2 == 0 {
                ElementData::Rectangle(shape)
            } else {
                ElementData::Text(TextData::default())
            };
            document.slots.push(Some(ElementRecord {
                id,
                meta: ElementMeta::default(),
                data,
            }));
            document.paint_order.push(id);
        }
        document.validate_session().unwrap();
        let rectangle = document.paint_order[5000];
        let text_id = document.paint_order[5001];
        let mut certify = Transaction::new("establish imported invariants");
        certify.update_rectangle(rectangle, shape);
        document.apply(&certify).unwrap();
        let work = document.synchronization_work;
        assert_eq!(work, (10_000, 0));
        let ranks = document.paint_ranks.get().unwrap().as_ptr();
        for value in [0.2, 0.6, 0.8] {
            let mut rect = *document.rectangle(rectangle).unwrap();
            rect.opacity = value;
            let mut text = document.text(text_id).unwrap().clone();
            text.opacity = value;
            let mut update = Transaction::new("local material edits");
            update.update_rectangle(rectangle, rect);
            update.update_text(text_id, text);
            let before = document.clone();
            let applied = document.apply(&update).unwrap();
            assert_eq!(applied.changes.touched, [rectangle, text_id]);
            assert!(!applied.changes.z_order_changed);
            assert_eq!(document.synchronization_work, work);
            assert_eq!(document.paint_ranks.get().unwrap().as_ptr(), ranks);
            document.apply(&applied.inverse).unwrap();
            assert!(document.has_same_session_content(&before));
            assert_eq!(document.synchronization_work, work);
        }
    }

    #[test]
    fn local_owner_and_label_edits_match_full_synchronization_and_undo() {
        let (mut document, pairs) = fixture();
        let bubble_id = document.allocate_element_id();
        let bubble_label = document.allocate_element_id();
        let shape_id = document.allocate_element_id();
        let shape_label = document.allocate_element_id();
        let mut bubble =
            crate::SpeechBubbleData::new(crate::MagnifierData::default().magnified_rect());
        bubble.text_element_id = Some(bubble_label);
        let mut shape = crate::MagnifierData::default().magnified_rect();
        shape.text_element_id = Some(shape_label);
        let mut insert = Transaction::new("mixed owners");
        insert.insert_speech_bubble(bubble_id, ElementMeta::default(), bubble);
        insert.insert_text(bubble_label, ElementMeta::default(), TextData::default());
        insert.insert_rectangle(shape_id, ElementMeta::default(), shape);
        insert.insert_text(shape_label, ElementMeta::default(), TextData::default());
        document.apply(&insert).unwrap();
        assert_eq!(document.synchronized_label_owners.len(), 3);

        let mut update = owner_update(&document, pairs[0].0);
        let mut text = document.text(pairs[1].1).unwrap().clone();
        text.layout = TextLayoutSize::new(77.0, 33.0);
        text.text = "must derive from owner".to_owned();
        update.update_text(pairs[1].1, text);
        let mut bubble = document.speech_bubble(bubble_id).unwrap().clone();
        bubble.body.center.x += 15.0;
        bubble.body.rotation = 0.3;
        update.update_speech_bubble(bubble_id, bubble);
        update.update_element_meta(
            bubble_id,
            ElementMeta {
                visible: false,
                locked: false,
            },
        );
        let mut text = document.text(bubble_label).unwrap().clone();
        text.center.x = -999.0;
        text.text = "preserve editable content".to_owned();
        update.update_text(bubble_label, text);
        let mut shape = *document.rectangle(shape_id).unwrap();
        shape.opacity = 0.4;
        update.update_rectangle(shape_id, shape);
        let before = document.clone();
        let mut full = document.clone();
        full.annotation_sync_certified = false;
        let work = document.synchronization_work;
        let local_result = document.apply(&update).unwrap();
        let full_result = full.apply(&update).unwrap();
        assert_eq!(document, full);
        assert_eq!(document.synchronization_work, (work.0, work.1 + 3));
        assert_eq!(
            document.text(bubble_label).unwrap().text,
            "preserve editable content"
        );
        document.apply(&local_result.inverse).unwrap();
        full.apply(&full_result.inverse).unwrap();
        assert!(document.has_same_session_content(&before));
        assert!(full.has_same_session_content(&before));
        assert_eq!(document, full);
    }

    #[test]
    fn derived_paint_ranks_follow_append_remove_restore_reorder_and_rollback() {
        let mut document = Document::new();
        let mut insert = Transaction::new("rank fixture");
        for _ in 0..6 {
            let id = document.allocate_element_id();
            insert.insert_rectangle(
                id,
                ElementMeta::default(),
                crate::MagnifierData::default().magnified_rect(),
            );
        }
        document.apply(&insert).unwrap();
        let original = document.clone();
        let ids = document.paint_order().to_vec();
        let assert_ranks = |document: &Document| {
            for (index, id) in document.paint_order().iter().copied().enumerate() {
                assert_eq!(document.paint_rank(id), Some(index as u32));
                assert_eq!(document.element_state(id).unwrap().z_index, index);
            }
        };
        assert_ranks(&document);
        let mut remove = Transaction::new("remove middle");
        remove.remove_element(ids[2]);
        let removed = document.apply(&remove).unwrap();
        assert_eq!(removed.changes.element_changes[0].old_paint_index, Some(2));
        assert_eq!(removed.changes.element_changes[0].new_paint_index, None);
        assert_eq!(document.paint_rank(ids[2]), None);
        assert_ranks(&document);
        document.apply(&removed.inverse).unwrap();
        assert_ranks(&document);
        assert!(document.has_same_session_content(&original));

        let mut reorder = Transaction::new("reorder then invalid edit");
        reorder.reorder_elements(vec![ids[4], ids[0]], 1);
        let mut invalid = *document.rectangle(ids[2]).unwrap();
        invalid.opacity = f64::NAN;
        reorder.update_rectangle(ids[2], invalid);
        assert_eq!(document.apply(&reorder), Err(ErrorCode::InvalidArgument));
        assert_ranks(&document);
        assert!(document.has_same_session_content(&original));
        let saved = serde_json::to_value(&document).unwrap();
        assert!(saved.get("paint_ranks").is_none());
        assert!(saved.get("synchronized_label_owners").is_none());
        let restored: Document = serde_json::from_value(saved).unwrap();
        assert!(restored.paint_ranks.get().is_none());
        assert_ranks(&restored);
    }

    #[test]
    fn local_filter_invariants_reject_invalid_auto_regions_and_reorder_smart_erase() {
        let mut document = Document::new();
        let shape_id = document.allocate_element_id();
        let filter_id = document.allocate_element_id();
        let mut insert = Transaction::new("filter layers");
        insert.insert_rectangle(
            shape_id,
            ElementMeta::default(),
            crate::MagnifierData::default().magnified_rect(),
        );
        insert.insert_filter(filter_id, ElementMeta::default(), FilterData::default());
        document.apply(&insert).unwrap();
        let mut filter = *document.filter(filter_id).unwrap();
        filter.filter_type = CanvasFilterType::SmartErase;
        let mut convert = Transaction::new("smart erase conversion");
        convert.update_filter(filter_id, filter);
        assert!(document.local_synchronization_plan(&convert).is_none());
        let converted = document.apply(&convert).unwrap();
        assert_eq!(document.paint_order(), &[filter_id, shape_id]);
        document.apply(&converted.inverse).unwrap();
        assert_eq!(document.paint_order(), &[shape_id, filter_id]);

        let mut filter = *document.filter(filter_id).unwrap();
        filter.auto_region_id = Some(7);
        let mut invalid = Transaction::new("missing auto region");
        invalid.update_filter(filter_id, filter);
        assert!(document.local_synchronization_plan(&invalid).is_none());
        let before = document.clone();
        assert_eq!(document.apply(&invalid), Err(ErrorCode::InvalidArgument));
        assert_eq!(document, before);
        assert!(document.annotation_sync_certified);
    }

    #[test]
    fn annotation_sync_owned_text_is_local_and_structural_commands_use_full_path() {
        let (mut document, pairs) = fixture();
        let original = document.clone();
        let mut text_only = Transaction::new("modify label");
        let mut text = document.text(pairs[0].1).unwrap().clone();
        text.text = "stale".to_owned();
        text_only.update_text(pairs[0].1, text);
        assert_eq!(
            document.annotation_sync_pairs_for_transaction(&text_only),
            Some(vec![pairs[0]])
        );
        document.apply(&text_only).unwrap();
        assert_derived_labels(&document, &pairs);

        let mut deletion = Transaction::new("delete annotation");
        deletion.remove_element(pairs[0].0);
        deletion.remove_element(pairs[0].1);
        assert!(
            document
                .annotation_sync_pairs_for_transaction(&deletion)
                .is_none()
        );
        let removed = document.apply(&deletion).unwrap();
        assert!(
            document
                .annotation_sync_pairs_for_transaction(&removed.inverse)
                .is_none()
        );
        document.apply(&removed.inverse).unwrap();
        assert!(document.has_same_session_content(&original));
        assert_derived_labels(&document, &pairs);
    }
}

#[cfg(test)]
mod tests {

    #[test]
    fn border_fill_serialization_preserves_color_and_legacy_styles() {
        let color = ColorRgba8 {
            r: 255,
            g: 204,
            b: 199,
            a: 255,
        };
        for (style, name) in [
            (FillStyle::Line, "Line"),
            (FillStyle::CrossLine, "CrossLine"),
            (FillStyle::Solid, "Solid"),
            (FillStyle::Border, "Border"),
        ] {
            let text = TextData {
                fill: color,
                fill_style: style,
                ..TextData::default()
            };
            let saved = serde_json::to_value(&text).unwrap();
            assert_eq!(saved["fill_style"], name);
            let restored: TextData = serde_json::from_value(saved).unwrap();
            assert_eq!(restored.fill, color);
            assert_eq!(restored.fill_style, style);
            assert_eq!(style.is_visible(color), style != FillStyle::Border);
            assert!(!style.is_visible(ColorRgba8::default()));
        }
        assert_eq!(FillStyle::default(), FillStyle::Solid);
    }

    use super::*;

    #[test]
    fn text_layout_size_roundtrips_through_legacy_flat_keys() {
        let layout = TextLayoutSize::with_content(80.0, 40.0, 44.0, 20.0);
        let json = serde_json::to_value(layout).unwrap();
        assert_eq!(json["width"], 80.0);
        assert_eq!(json["height"], 40.0);
        assert_eq!(json["content_width"], 44.0);
        assert_eq!(json["content_height"], 20.0);
        assert_eq!(
            serde_json::from_value::<TextLayoutSize>(json).unwrap(),
            layout
        );

        // Unmeasured ink keeps writing the legacy 0 keys so older readers and
        // diff tools see the shape they always did.
        let unmeasured = TextLayoutSize::new(80.0, 40.0);
        let json = serde_json::to_value(unmeasured).unwrap();
        assert_eq!(json["content_width"], 0.0);
        assert_eq!(json["content_height"], 0.0);
        assert_eq!(
            serde_json::from_value::<TextLayoutSize>(json).unwrap(),
            unmeasured
        );
    }

    #[test]
    fn text_layout_size_reads_legacy_documents() {
        // Very old documents have no content keys at all.
        let legacy = serde_json::json!({
            "width": 80.0,
            "height": 40.0,
        });
        assert_eq!(
            serde_json::from_value::<TextLayoutSize>(legacy).unwrap(),
            TextLayoutSize::new(80.0, 40.0)
        );

        // 0 ink reads as unmeasured; mixed or negative pairs are garbage and
        // must not surface as a half-measurement.
        for (content_width, content_height) in [(0.0, 0.0), (0.0, 20.0), (44.0, 0.0), (-1.0, 20.0)]
        {
            let json = serde_json::json!({
                "width": 80.0,
                "height": 40.0,
                "content_width": content_width,
                "content_height": content_height,
            });
            assert_eq!(
                serde_json::from_value::<TextLayoutSize>(json)
                    .unwrap()
                    .ink(),
                None,
                "pair ({content_width}, {content_height}) must read as unmeasured"
            );
        }

        // Non-finite reports cannot travel through JSON; they arrive through
        // the constructors, which read them the same way.
        assert_eq!(
            TextLayoutSize::with_content(80.0, 40.0, f64::NAN, 20.0).ink(),
            None
        );
    }

    #[test]
    fn text_data_flattens_layout_into_legacy_keys() {
        let text = TextData {
            layout: TextLayoutSize::with_content(80.0, 40.0, 44.0, 20.0),
            ..TextData::default()
        };
        let json = serde_json::to_value(&text).unwrap();
        assert_eq!(json["width"], 80.0);
        assert_eq!(json["height"], 40.0);
        assert_eq!(json["content_width"], 44.0);
        assert_eq!(json["content_height"], 20.0);
        assert_eq!(
            serde_json::from_value::<TextData>(json).unwrap().layout,
            text.layout
        );
    }

    #[test]
    fn ink_or_wrap_resolves_the_single_content_box() {
        assert_eq!(
            TextLayoutSize::with_content(80.0, 40.0, 44.0, 20.0).ink_or_wrap(),
            (44.0, 20.0)
        );
        assert_eq!(TextLayoutSize::new(80.0, 40.0).ink_or_wrap(), (80.0, 40.0));
    }

    #[test]
    fn text_element_data_roundtrips_through_the_tagged_enum() {
        // The flattened layout sits inside an externally tagged enum; this is
        // the shape persisted documents actually carry.
        let element = ElementData::Text(TextData {
            layout: TextLayoutSize::with_content(80.0, 40.0, 44.0, 20.0),
            text: "note".to_owned(),
            ..TextData::default()
        });
        let json = serde_json::to_value(&element).unwrap();
        let variant = json.get("Text").expect("externally tagged variant");
        assert_eq!(variant["width"], 80.0);
        assert_eq!(variant["content_width"], 44.0);
        assert_eq!(
            serde_json::from_value::<ElementData>(json).unwrap(),
            element
        );
    }

    #[test]
    fn reorder_inverse_restores_permuted_and_disjoint_layers() {
        let mut document = Document::new();
        let ids: Vec<_> = (0..6)
            .map(|index| ElementId {
                index,
                generation: 1,
            })
            .collect();
        let mut setup = Transaction::new("layers");
        for id in &ids {
            setup.insert_text(*id, ElementMeta::default(), TextData::default());
        }
        document.apply(&setup).unwrap();
        let original = document.clone();
        for reordered in [vec![ids[5], ids[1]], ids.iter().rev().copied().collect()] {
            let mut tx = Transaction::new("reorder layers");
            tx.reorder_elements(reordered, 0);
            let result = document.apply(&tx).unwrap();
            assert_ne!(document.paint_order(), original.paint_order());
            document.apply(&result.inverse).unwrap();
            assert!(document.has_same_session_content(&original));
            tx.remove_element(ElementId {
                index: 100,
                generation: 1,
            });
            assert!(document.apply(&tx).is_err());
            assert!(document.has_same_session_content(&original));
        }
    }

    #[test]
    fn filter_element_kinds_classify_coverage_overlays() {
        assert!(ElementKind::Filter.is_filter());
        assert!(ElementKind::AutoFilter.is_filter());
        assert!(ElementKind::PenFilter.is_filter());
        assert!(!ElementKind::Rectangle.is_filter());
        assert!(!ElementKind::Spotlight.is_filter());

        let auto = FilterData {
            auto_region_id: Some(1),
            ..FilterData::default()
        };
        let data = ElementData::Filter(auto);
        assert_eq!(data.kind(), ElementKind::AutoFilter);
        assert!(data.is_filter());
        assert!(ElementData::Filter(FilterData::default()).is_filter());
        assert!(ElementData::PenFilter(PenFilterData::default()).is_filter());
    }

    #[test]
    fn common_element_state_has_one_geometry_opacity_and_z_order_definition() {
        let rectangle = RectangleData {
            text_element_id: None,
            stroke_rendering_style: Default::default(),
            stroke_rendering_seed: 0,
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
        assert!(!document.is_text_bound_to_serial_number(missing_text_id));
        assert_eq!(document.bound_text_id_for_serial_number(serial_id), None);
        assert!(
            document
                .serial_number_ids_with_text(missing_text_id)
                .is_empty()
        );
    }

    #[test]
    fn filter_strength_normalization_is_deterministic() {
        assert_eq!(FilterData::normalized_strength(f64::NAN), 1.0);
        assert_eq!(FilterData::normalized_strength(-2.0), 0.0);
        assert_eq!(FilterData::normalized_strength(2.0), 1.0);
        assert_eq!(FilterData::normalized_strength(0.25), 0.25);
    }

    #[test]
    fn spotlight_defaults_visibility_and_inverse_are_stable() {
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
            text_element_id: None,
            stroke_rendering_style: Default::default(),
            stroke_rendering_seed: 0,
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
            (16.0, 30.0, 56.0, 0.16)
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

    #[test]
    fn watermark_template_resolution_is_best_effort_and_time_is_fixed() {
        let time = WatermarkTemplateApplicationTime {
            year: 2026,
            month: 9,
            day: 5,
            hour: 7,
            minute: 8,
            second: 9,
        };
        assert!(time.is_valid());
        assert!(
            !WatermarkTemplateApplicationTime {
                day: 31,
                month: 2,
                ..time
            }
            .is_valid()
        );
        assert!(
            WatermarkTemplateApplicationTime {
                year: 2024,
                day: 29,
                month: 2,
                ..time
            }
            .is_valid()
        );

        assert_eq!(resolve_watermark_template("", "draft", Some(time)), "draft");
        assert_eq!(
            resolve_watermark_template("{text} Design {YYYY-MM-DD_HH-mm-ss}", "test", Some(time)),
            "test Design 2026-09-05_07-08-09"
        );
        assert_eq!(
            resolve_watermark_template(
                "{YYYY}/{MM}/{DD} {HH}:{mm}:{ss} {prefix-YYYY} {YYYYYYYY}",
                "",
                Some(time)
            ),
            "2026/09/05 07:08:09 prefix-2026 20262026"
        );
        assert_eq!(
            resolve_watermark_template("{unknown} {Text} {} tail", "value", Some(time)),
            "{unknown} {Text} {} tail"
        );
        assert_eq!(
            resolve_watermark_template("before {YYYY after", "", Some(time)),
            "before {YYYY after"
        );
        assert_eq!(
            resolve_watermark_template("nested {outer-{YYYY}}", "", Some(time)),
            "nested {outer-{YYYY}}"
        );
        assert_eq!(
            resolve_watermark_template("{text} {YYYY}", "changed", None),
            "changed {YYYY}"
        );

        let mut config = WatermarkConfig {
            text: "first".to_owned(),
            template_value: "{text}-{YYYY}".to_owned(),
            template_application_time: Some(time),
            ..WatermarkConfig::default()
        };
        assert_eq!(config.resolved_text(), "first-2026");
        config.text = "second".to_owned();
        assert_eq!(config.resolved_text(), "second-2026");
        assert_eq!(config.template_application_time, Some(time));
    }

    #[test]
    fn watermark_template_controls_visibility_and_preserves_value_whitespace() {
        let time = WatermarkTemplateApplicationTime {
            year: 2026,
            month: 1,
            day: 2,
            hour: 3,
            minute: 4,
            second: 5,
        };
        let timestamp_only = WatermarkConfig {
            template_value: "{YYYY}".to_owned(),
            template_application_time: Some(time),
            ..WatermarkConfig::default()
        };
        assert!(timestamp_only.is_visible());

        let text_only = WatermarkConfig {
            template_value: "{text}".to_owned(),
            template_application_time: Some(time),
            ..WatermarkConfig::default()
        };
        assert!(!text_only.is_visible());

        let normalized = WatermarkConfig {
            template_value: "  {text}  ".to_owned(),
            template_application_time: Some(WatermarkTemplateApplicationTime { month: 13, ..time }),
            ..WatermarkConfig::default()
        }
        .normalized();
        assert_eq!(normalized.template_value, "  {text}  ");
        assert_eq!(normalized.template_application_time, None);
    }
}

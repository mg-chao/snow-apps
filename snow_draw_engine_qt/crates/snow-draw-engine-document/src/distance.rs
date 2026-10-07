use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{CornerRadii, ErrorCode};

use crate::{
    ArrowData, FillStyle, TextData, TextHorizontalAlign, TextLayoutSize, TextVerticalAlign,
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum DistanceUnit {
    Px,
    #[default]
    Cm,
    M,
    Km,
    Mm,
}

impl DistanceUnit {
    pub const fn suffix(self) -> &'static str {
        match self {
            Self::Px => "px",
            Self::Cm => "cm",
            Self::M => "m",
            Self::Km => "km",
            Self::Mm => "mm",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct DistanceAnnotation {
    pub factor: f64,
    pub unit: DistanceUnit,
    pub decimal_places: u8,
    #[serde(default = "default_pixel_scale")]
    pub pixel_scale_x: f64,
    #[serde(default = "default_pixel_scale")]
    pub pixel_scale_y: f64,
}

const fn default_pixel_scale() -> f64 {
    1.0
}

impl Default for DistanceAnnotation {
    fn default() -> Self {
        Self {
            factor: 1.0,
            unit: DistanceUnit::default(),
            decimal_places: 0,
            pixel_scale_x: 1.0,
            pixel_scale_y: 1.0,
        }
    }
}

pub fn validate_distance_annotation(distance: DistanceAnnotation) -> Result<(), ErrorCode> {
    if !distance.factor.is_finite()
        || !(0.01..=1000.0).contains(&distance.factor)
        || distance.decimal_places > 3
        || !distance.pixel_scale_x.is_finite()
        || distance.pixel_scale_x <= 0.0
        || !distance.pixel_scale_y.is_finite()
        || distance.pixel_scale_y <= 0.0
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

pub fn distance_value(arrow: &ArrowData) -> Option<f64> {
    let distance = arrow.distance?;
    let start = arrow.start();
    let end = arrow.end();
    let value = ((end.x - start.x) * distance.pixel_scale_x)
        .hypot((end.y - start.y) * distance.pixel_scale_y)
        * distance.factor;
    value.is_finite().then_some(value)
}

pub fn distance_label_text(arrow: &ArrowData) -> Option<String> {
    let distance = arrow.distance?;
    let value = distance_value(arrow)?;
    Some(format!(
        "{:.*} {}",
        usize::from(distance.decimal_places),
        value,
        distance.unit.suffix()
    ))
}

/// The label is derived from its owner; independent text preferences never
/// change the meaning or appearance of a distance annotation.
pub fn distance_label(arrow: &ArrowData, layout: Option<TextLayoutSize>) -> Option<TextData> {
    // Keep the 2px stroke's 20px label while damping size changes in both directions.
    let font_size = 20.0 * (arrow.stroke_width / 2.0).sqrt();
    Some(TextData {
        center: crate::arrow_text_anchor(arrow),
        layout: layout.unwrap_or_else(|| TextLayoutSize::new(1.0, font_size * 1.2)),
        rotation: 0.0,
        text: distance_label_text(arrow)?,
        color: arrow.stroke,
        font_size,
        font_family: None,
        fill: Default::default(),
        fill_style: FillStyle::Solid,
        stroke: Default::default(),
        stroke_width: 0.0,
        corner_radii: CornerRadii::default(),
        horizontal_align: TextHorizontalAlign::Center,
        vertical_align: TextVerticalAlign::Center,
        auto_resize: true,
        opacity: arrow.opacity,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{ColorRgba8, Point, arrow::ArrowType, arrow::StrokeStyle};

    #[test]
    fn distance_label_font_grows_sublinearly_across_supported_stroke_widths() {
        let mut arrow = ArrowData::from_global_points(
            &[Point::new(0.0, 0.0), Point::new(100.0, 0.0)],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        arrow.linear_kind = crate::LinearElementKind::Distance;
        arrow.distance = Some(DistanceAnnotation::default());
        assert_eq!(distance_label(&arrow, None).unwrap().font_size, 20.0);

        arrow.stroke_width = 1.0;
        let thin_label = distance_label(&arrow, None).unwrap();
        assert!(thin_label.font_size > 10.0 && thin_label.font_size < 20.0);
        let mut previous_size = thin_label.font_size;
        let mut previous_growth = f64::INFINITY;
        let mut previous_ratio = previous_size;
        let measured = TextLayoutSize::new(120.0, 24.0);
        for width in 2..=72 {
            arrow.stroke_width = f64::from(width);
            let label = distance_label(&arrow, None).unwrap();
            let growth = label.font_size - previous_size;
            let ratio = label.font_size / arrow.stroke_width;
            assert!(growth > 0.0 && growth < previous_growth);
            assert!(ratio < previous_ratio);
            assert_eq!(label.layout.height(), label.font_size * 1.2);
            assert_eq!(
                distance_label(&arrow, Some(measured)).unwrap().layout,
                measured
            );
            if width == 10 {
                assert!(label.font_size > 20.0 && label.font_size < 50.0);
            }
            previous_size = label.font_size;
            previous_growth = growth;
            previous_ratio = ratio;
        }
    }
}

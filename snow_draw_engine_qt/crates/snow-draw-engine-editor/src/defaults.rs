use snow_draw_engine_core::{
    ColorRgba8, CornerRadii,
    arrow::{StrokeStyle, ArrowType, Arrowhead},
};
use snow_draw_engine_document::{FillStyle, TextHorizontalAlign, TextVerticalAlign};

use crate::{
    ArrowStyle, FilterStyle, RectangleShapeStyle, SerialNumberStyle, ShapeStyle, TextStyle,
};

#[derive(Clone, Debug, PartialEq)]
pub struct EditorStyleDefaults {
    pub rectangle: RectangleShapeStyle,
    pub arrow: ArrowStyle,
    pub line: ShapeStyle,
    pub free_draw: ShapeStyle,
    pub rectangle_highlight: ShapeStyle,
    pub pen_highlight: ShapeStyle,
    pub rectangle_filter: FilterStyle,
    pub pen_filter: FilterStyle,
    pub text: TextStyle,
    pub serial_number: SerialNumberStyle,
}

impl Default for EditorStyleDefaults {
    fn default() -> Self {
        editor_style_defaults()
    }
}

pub fn editor_style_defaults() -> EditorStyleDefaults {
    let stroke = ColorRgba8 {
        r: 0x21,
        g: 0x6b,
        b: 0xa5,
        a: 0xff,
    };

    EditorStyleDefaults {
        rectangle: RectangleShapeStyle {
            shape: snow_draw_engine_document::HighlightShape::Rectangle,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke,
            stroke_width: 2.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::splat(6.0),
        },
        arrow: ArrowStyle {
            stroke,
            stroke_width: 2.0,
            start_arrowhead: None,
            end_arrowhead: Some(Arrowhead::Arrow),
            stroke_style: StrokeStyle::Solid,
            arrow_type: ArrowType::Curve,
        },
        line: ShapeStyle {
            shape: snow_draw_engine_document::HighlightShape::Rectangle,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8 {
                r: 0x1e,
                g: 0x1e,
                b: 0x1e,
                a: 0xff,
            },
            stroke_width: 2.0,
            corner_radii: CornerRadii::default(),
            start_arrowhead: None,
            end_arrowhead: None,
            stroke_style: StrokeStyle::Solid,
            arrow_type: ArrowType::Curve,
            opacity: 1.0,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
        },
        free_draw: ShapeStyle {
            shape: snow_draw_engine_document::HighlightShape::Rectangle,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8 {
                r: 0x1e,
                g: 0x1e,
                b: 0x1e,
                a: 0xff,
            },
            stroke_width: 2.0,
            corner_radii: CornerRadii::default(),
            start_arrowhead: None,
            end_arrowhead: None,
            stroke_style: StrokeStyle::Solid,
            arrow_type: ArrowType::Curve,
            opacity: 1.0,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
        },
        rectangle_highlight: ShapeStyle {
            shape: snow_draw_engine_document::HighlightShape::Rectangle,
            fill: stroke,
            fill_style: FillStyle::Solid,
            stroke,
            stroke_width: 0.0,
            corner_radii: CornerRadii::default(),
            start_arrowhead: None,
            end_arrowhead: None,
            stroke_style: StrokeStyle::Solid,
            arrow_type: ArrowType::Straight,
            opacity: 1.0,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
        },
        pen_highlight: ShapeStyle {
            shape: snow_draw_engine_document::HighlightShape::Rectangle,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke,
            stroke_width: 30.0,
            corner_radii: CornerRadii::default(),
            start_arrowhead: None,
            end_arrowhead: None,
            stroke_style: StrokeStyle::Solid,
            arrow_type: ArrowType::Straight,
            opacity: 1.0,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
        },
        rectangle_filter: FilterStyle::default(),
        pen_filter: FilterStyle {
            stroke_width: 30.0,
            ..FilterStyle::default()
        },
        text: TextStyle {
            color: stroke,
            font_size: 30.0,
            font_family: None,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8 {
                r: 0xb7,
                g: 0xd5,
                b: 0xeb,
                a: 0xff,
            },
            stroke_width: 0.0,
            corner_radii: CornerRadii::splat(6.0),
            horizontal_align: TextHorizontalAlign::Left,
            vertical_align: TextVerticalAlign::Center,
            opacity: 1.0,
        },
        serial_number: SerialNumberStyle {
            number: 1,
            color: stroke,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            font_size: 24.0,
            font_family: None,
            stroke_width: 2.0,
            stroke_style: StrokeStyle::Solid,
            opacity: 1.0,
        },
    }
}

#[cfg(test)]
mod tests {
    use snow_draw_engine_core::CornerRadii;
    use snow_draw_engine_core::arrow::{ArrowType, Arrowhead};

    use super::editor_style_defaults;

    #[test]
    fn arrow_creation_default_uses_standard_end_arrowhead() {
        let styles = editor_style_defaults();

        assert_eq!(styles.arrow.end_arrowhead, Some(Arrowhead::Arrow));
        assert_eq!(styles.arrow.arrow_type, ArrowType::Curve);
        assert_eq!(styles.arrow.stroke_width, 2.0);
    }

    #[test]
    fn rectangle_creation_default_uses_six_pixel_corners() {
        let styles = editor_style_defaults();

        assert_eq!(styles.rectangle.corner_radii, CornerRadii::splat(6.0));
    }

    #[test]
    fn rectangle_highlight_default_uses_generic_stroke_color() {
        let styles = editor_style_defaults();

        assert_eq!(
            styles.rectangle_highlight.stroke,
            snow_draw_engine_core::ColorRgba8 {
                r: 0x21,
                g: 0x6b,
                b: 0xa5,
                a: 0xff,
            }
        );
    }
}

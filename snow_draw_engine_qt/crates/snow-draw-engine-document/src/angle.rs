use std::f64::consts::{FRAC_PI_2, TAU};

use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{CornerRadii, DrawRect, ErrorCode, Point, arrow::ArrowPathCommand};

use crate::{
    ArrowData, FillStyle, TextData, TextHorizontalAlign, TextLayoutSize, TextVerticalAlign,
};

const COINCIDENT_DIRECTION_TOLERANCE: f64 = 1e-12;
// A circular cubic spanning at most 90 degrees deviates radially by less
// than 0.000273 of its radius. Include that envelope in bounds and picking.
const CUBIC_ARC_RADIUS_PADDING: f64 = 0.0003;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum AngleUnit {
    #[default]
    Degrees,
    Radians,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct AngleAnnotation {
    pub unit: AngleUnit,
    pub decimal_places: u8,
    /// Coincident rays normally measure zero; wheel adjustment can explicitly
    /// reach a complete turn without storing a redundant numeric angle.
    #[serde(default)]
    pub full_turn: bool,
}

pub fn validate_angle_annotation(angle: AngleAnnotation) -> Result<(), ErrorCode> {
    if angle.decimal_places > 3 {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AngleGeometry {
    pub vertex: Point<f64>,
    pub start_direction: Point<f64>,
    pub end_direction: Point<f64>,
    /// Visual counterclockwise sweep in the downward-positive canvas, in radians.
    pub sweep: f64,
    pub radius: f64,
    pub bisector: Point<f64>,
}

pub fn angle_geometry(arrow: &ArrowData) -> Option<AngleGeometry> {
    let annotation = arrow.angle?;
    let [a, vertex, b] = arrow.points.as_slice() else {
        return None;
    };
    if arrow
        .points
        .iter()
        .flatten()
        .chain([&arrow.x, &arrow.y, &arrow.stroke_width])
        .any(|value| !value.is_finite())
        || arrow.stroke_width <= 0.0
    {
        return None;
    }
    let start = Point::new(a[0] - vertex[0], a[1] - vertex[1]);
    let end = Point::new(b[0] - vertex[0], b[1] - vertex[1]);
    let start_length = start.x.hypot(start.y);
    let end_length = end.x.hypot(end.y);
    if !start_length.is_finite()
        || !end_length.is_finite()
        || start_length <= 0.0
        || end_length <= 0.0
    {
        return None;
    }
    let start_direction = Point::new(start.x / start_length, start.y / start_length);
    let end_direction = Point::new(end.x / end_length, end.y / end_length);
    let coincident = (start_direction.x - end_direction.x)
        .hypot(start_direction.y - end_direction.y)
        <= COINCIDENT_DIRECTION_TOLERANCE;
    if annotation.full_turn && !coincident {
        return None;
    }
    let cross = start_direction.x * end_direction.y - start_direction.y * end_direction.x;
    let dot = start_direction.x * end_direction.x + start_direction.y * end_direction.y;
    let sweep = if annotation.full_turn {
        TAU
    } else if coincident {
        0.0
    } else {
        (-cross).atan2(dot).rem_euclid(TAU)
    };
    let (sin, cos) = (sweep / 2.0).sin_cos();
    let bisector = Point::new(
        start_direction.x * cos + start_direction.y * sin,
        start_direction.y * cos - start_direction.x * sin,
    );
    let font_size = annotation_font_size(arrow);
    let radius = (0.30 * start_length.min(end_length))
        .min(24.0_f64.max(1.25 * font_size + arrow.stroke_width));
    let vertex = Point::new(arrow.x + vertex[0], arrow.y + vertex[1]);
    if !vertex.x.is_finite() || !vertex.y.is_finite() {
        return None;
    }
    Some(AngleGeometry {
        vertex,
        start_direction,
        end_direction,
        sweep,
        radius,
        bisector,
    })
}

/// Angle values are always geometric radians, independent of display preferences.
pub fn angle_value(arrow: &ArrowData) -> Option<f64> {
    Some(angle_geometry(arrow)?.sweep)
}

pub fn angle_arc_radius(arrow: &ArrowData) -> Option<f64> {
    Some(angle_geometry(arrow)?.radius)
}

pub fn angle_label_text(arrow: &ArrowData) -> Option<String> {
    Some(format_angle_value(arrow.angle?, angle_value(arrow)?))
}

fn format_angle_value(angle: AngleAnnotation, radians: f64) -> String {
    let (value, suffix) = match angle.unit {
        AngleUnit::Degrees => (radians.to_degrees(), "°"),
        AngleUnit::Radians => (radians, " rad"),
    };
    format!("{:.*}{}", usize::from(angle.decimal_places), value, suffix)
}

pub fn angle_label_anchor(arrow: &ArrowData, layout: TextLayoutSize) -> Option<Point<f64>> {
    let geometry = angle_geometry(arrow)?;
    Some(label_anchor(geometry, arrow.stroke_width, layout))
}

fn label_anchor(geometry: AngleGeometry, stroke_width: f64, layout: TextLayoutSize) -> Point<f64> {
    let offset =
        geometry.radius + layout.width().hypot(layout.height()) / 2.0 + 6.0 + stroke_width / 2.0;
    Point::new(
        geometry.vertex.x + geometry.bisector.x * offset,
        geometry.vertex.y + geometry.bisector.y * offset,
    )
}

pub fn angle_label(arrow: &ArrowData, layout: Option<TextLayoutSize>) -> Option<TextData> {
    let geometry = angle_geometry(arrow)?;
    let font_size = annotation_font_size(arrow);
    let layout = layout.unwrap_or_else(|| TextLayoutSize::new(1.0, font_size * 1.2));
    Some(TextData {
        center: label_anchor(geometry, arrow.stroke_width, layout),
        layout,
        rotation: 0.0,
        text: format_angle_value(arrow.angle?, geometry.sweep),
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

pub fn generated_annotation_label(
    arrow: &ArrowData,
    layout: Option<TextLayoutSize>,
) -> Option<TextData> {
    if arrow.is_angle() {
        angle_label(arrow, layout)
    } else if arrow.is_distance() {
        crate::distance_label(arrow, layout)
    } else {
        None
    }
}

fn annotation_font_size(arrow: &ArrowData) -> f64 {
    20.0 * (arrow.stroke_width / 2.0).sqrt()
}

fn arc_point(geometry: AngleGeometry, rotation: f64) -> Point<f64> {
    let (sin, cos) = rotation.sin_cos();
    Point::new(
        geometry.vertex.x
            + geometry.radius
                * (geometry.start_direction.x * cos + geometry.start_direction.y * sin),
        geometry.vertex.y
            + geometry.radius
                * (geometry.start_direction.y * cos - geometry.start_direction.x * sin),
    )
}

pub(crate) fn angle_path_commands(arrow: &ArrowData) -> Vec<ArrowPathCommand> {
    let mut commands = Vec::with_capacity(8);
    for (index, point) in arrow.points.iter().enumerate() {
        let point = [arrow.x + point[0], arrow.y + point[1]];
        commands.push(if index == 0 {
            ArrowPathCommand::MoveTo { point }
        } else {
            ArrowPathCommand::LineTo { point }
        });
    }
    let Some(geometry) = angle_geometry(arrow) else {
        return commands;
    };
    if geometry.sweep == 0.0 {
        return commands;
    }
    let segment_count = (geometry.sweep / FRAC_PI_2).ceil() as usize;
    let step = geometry.sweep / segment_count as f64;
    let control_factor = (step / 4.0).tan() * 4.0 / 3.0;
    let start = arc_point(geometry, 0.0);
    commands.push(ArrowPathCommand::MoveTo {
        point: [start.x, start.y],
    });
    for segment in 0..segment_count {
        let start = arc_point(geometry, segment as f64 * step);
        let end = arc_point(geometry, (segment + 1) as f64 * step);
        let start_delta = Point::new(start.x - geometry.vertex.x, start.y - geometry.vertex.y);
        let end_delta = Point::new(end.x - geometry.vertex.x, end.y - geometry.vertex.y);
        commands.push(ArrowPathCommand::CubicTo {
            control_1: [
                start.x + control_factor * start_delta.y,
                start.y - control_factor * start_delta.x,
            ],
            control_2: [
                end.x - control_factor * end_delta.y,
                end.y + control_factor * end_delta.x,
            ],
            end: [end.x, end.y],
        });
    }
    commands
}

fn direction_sweep(geometry: AngleGeometry, direction: Point<f64>) -> f64 {
    let cross = geometry.start_direction.x * direction.y - geometry.start_direction.y * direction.x;
    let dot = geometry.start_direction.x * direction.x + geometry.start_direction.y * direction.y;
    (-cross).atan2(dot).rem_euclid(TAU)
}

pub(crate) fn angle_arc_bounds(arrow: &ArrowData) -> Option<DrawRect> {
    let geometry = angle_geometry(arrow)?;
    if geometry.sweep == 0.0 {
        return None;
    }
    let start = arc_point(geometry, 0.0);
    let end = arc_point(geometry, geometry.sweep);
    let mut bounds = DrawRect::new(
        start.x.min(end.x),
        start.y.min(end.y),
        start.x.max(end.x),
        start.y.max(end.y),
    );
    for direction in [
        Point::new(1.0, 0.0),
        Point::new(0.0, -1.0),
        Point::new(-1.0, 0.0),
        Point::new(0.0, 1.0),
    ] {
        if direction_sweep(geometry, direction) <= geometry.sweep + f64::EPSILON {
            let x = geometry.vertex.x + geometry.radius * direction.x;
            let y = geometry.vertex.y + geometry.radius * direction.y;
            bounds.min_x = bounds.min_x.min(x);
            bounds.min_y = bounds.min_y.min(y);
            bounds.max_x = bounds.max_x.max(x);
            bounds.max_y = bounds.max_y.max(y);
        }
    }
    let padding = geometry.radius * CUBIC_ARC_RADIUS_PADDING;
    Some(DrawRect::new(
        bounds.min_x - padding,
        bounds.min_y - padding,
        bounds.max_x + padding,
        bounds.max_y + padding,
    ))
}

pub(crate) fn angle_arc_hit_test(arrow: &ArrowData, point: Point<f64>, tolerance: f64) -> bool {
    let Some(geometry) = angle_geometry(arrow) else {
        return false;
    };
    if geometry.sweep == 0.0 {
        return false;
    }
    let delta = Point::new(point.x - geometry.vertex.x, point.y - geometry.vertex.y);
    let length = delta.x.hypot(delta.y);
    let tolerance = tolerance + geometry.radius * CUBIC_ARC_RADIUS_PADDING;
    if length > 0.0
        && (length - geometry.radius).abs() <= tolerance
        && direction_sweep(geometry, Point::new(delta.x / length, delta.y / length))
            <= geometry.sweep + f64::EPSILON
    {
        return true;
    }
    // The circular arc uses round stroke caps, including near its end directions.
    [
        arc_point(geometry, 0.0),
        arc_point(geometry, geometry.sweep),
    ]
    .into_iter()
    .any(|end| (point.x - end.x).hypot(point.y - end.y) <= tolerance)
}

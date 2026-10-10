use std::f64::consts::{FRAC_PI_2, TAU};

use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{CornerRadii, DrawRect, ErrorCode, Point, arrow::ArrowPathCommand};

use crate::{
    ArrowData, FillStyle, TextData, TextHorizontalAlign, TextLayoutSize, TextVerticalAlign,
};

const COINCIDENT_DIRECTION_TOLERANCE: f64 = 1e-12;
const SIDE_LENGTH_TOLERANCE: f64 = 1e-9;
const ANGLE_SNAP_STEP: f64 = std::f64::consts::PI / 12.0;
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

#[derive(Clone, Copy, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct AngleAnnotation {
    pub unit: AngleUnit,
    pub decimal_places: u8,
    /// Coincident rays normally measure zero; wheel adjustment can explicitly
    /// reach a complete turn without storing a redundant numeric angle.
    #[serde(default)]
    pub full_turn: bool,
    /// Canvas-space radius chosen by the user; absent values retain automatic sizing.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub arc_radius: Option<f64>,
}

pub fn validate_angle_annotation(angle: AngleAnnotation) -> Result<(), ErrorCode> {
    if angle.decimal_places > 3
        || angle
            .arc_radius
            .is_some_and(|radius| !radius.is_finite() || radius <= 0.0)
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

/// Canonical angle geometry. The second side has a direction, never its own length.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AngleShape {
    pub vertex: Point<f64>,
    pub side_length: f64,
    pub first_bearing: f64,
    pub sweep: f64,
}

// Rendering and migration use vectors directly; only editing needs a polar bearing.
struct AngleRays {
    vertex: Point<f64>,
    side_length: f64,
    second_length: f64,
    start_direction: Point<f64>,
    end_direction: Point<f64>,
    sweep: f64,
}

impl AngleRays {
    fn read_points(points: [Point<f64>; 3], full_turn: bool) -> Option<(Self, bool)> {
        if points.iter().any(|p| !p.x.is_finite() || !p.y.is_finite()) {
            return None;
        }
        let [first, vertex, second] = points;
        let first = Point::new(first.x - vertex.x, first.y - vertex.y);
        let second = Point::new(second.x - vertex.x, second.y - vertex.y);
        let side_length = first.x.hypot(first.y);
        let direction_length = second.x.hypot(second.y);
        if !side_length.is_finite()
            || side_length <= 0.0
            || !direction_length.is_finite()
            || direction_length <= 0.0
        {
            return None;
        }
        let start_direction = Point::new(first.x / side_length, first.y / side_length);
        let end_direction = Point::new(second.x / direction_length, second.y / direction_length);
        let direction_dx = start_direction.x - end_direction.x;
        let direction_dy = start_direction.y - end_direction.y;
        let coincident = direction_dx * direction_dx + direction_dy * direction_dy
            <= COINCIDENT_DIRECTION_TOLERANCE * COINCIDENT_DIRECTION_TOLERANCE;
        if full_turn && !coincident {
            return None;
        }
        let equal =
            (side_length - direction_length).abs() <= SIDE_LENGTH_TOLERANCE * side_length.max(1.0);
        Some((
            Self {
                vertex,
                side_length,
                second_length: direction_length,
                start_direction,
                end_direction,
                sweep: if full_turn {
                    TAU
                } else if coincident {
                    0.0
                } else {
                    let cross =
                        start_direction.x * end_direction.y - start_direction.y * end_direction.x;
                    let dot =
                        start_direction.x * end_direction.x + start_direction.y * end_direction.y;
                    (-cross).atan2(dot).rem_euclid(TAU)
                },
            },
            equal,
        ))
    }

    fn shape(&self) -> AngleShape {
        AngleShape {
            vertex: self.vertex,
            side_length: self.side_length,
            first_bearing: self.start_direction.y.atan2(self.start_direction.x),
            sweep: self.sweep,
        }
    }
}

impl AngleShape {
    /// The third point supplies only a direction; its distance is discarded.
    pub fn from_points(points: [Point<f64>; 3], full_turn: bool) -> Option<Self> {
        AngleRays::read_points(points, full_turn).map(|(rays, _)| rays.shape())
    }

    pub fn snapped(self) -> Self {
        Self {
            sweep: (self.sweep / ANGLE_SNAP_STEP).round() * ANGLE_SNAP_STEP,
            ..self
        }
    }

    fn point_at(self, bearing: f64) -> Point<f64> {
        let (sin, cos) = bearing.sin_cos();
        Point::new(
            self.vertex.x + self.side_length * cos,
            self.vertex.y + self.side_length * sin,
        )
    }

    fn second_point(self, first: Point<f64>) -> Point<f64> {
        if self.sweep == 0.0 || self.sweep == TAU {
            first
        } else {
            self.point_at(self.first_bearing - self.sweep)
        }
    }

    pub fn points(self) -> [Point<f64>; 3] {
        let first = self.point_at(self.first_bearing);
        [first, self.vertex, self.second_point(first)]
    }
}

fn angle_local_points(arrow: &ArrowData) -> Option<[Point<f64>; 3]> {
    if !arrow.is_angle() || !arrow.width.is_finite() || !arrow.height.is_finite() {
        return None;
    }
    validate_angle_annotation(arrow.angle?).ok()?;
    let [first, vertex, second] = arrow.points.as_slice() else {
        return None;
    };
    Some([first, vertex, second].map(|point| Point::new(point[0], point[1])))
}

fn angle_points(arrow: &ArrowData) -> Option<[Point<f64>; 3]> {
    Some(angle_local_points(arrow)?.map(|point| Point::new(arrow.x + point.x, arrow.y + point.y)))
}

fn rebuilt_angle(
    arrow: &ArrowData,
    points: [Point<f64>; 3],
    full_turn: bool,
    side_length: f64,
) -> Option<ArrowData> {
    let geometry = crate::arrow_geom::normalize_arrow_from_global_points(
        &points.map(|point| [point.x, point.y]),
        crate::DEFAULT_ARROW_MAX_COORDINATE,
    );
    let mut next = arrow.clone();
    next.x = geometry.x;
    next.y = geometry.y;
    next.width = geometry.width;
    next.height = geometry.height;
    next.points = geometry.points;
    let annotation = next.angle.as_mut()?;
    annotation.full_turn = full_turn;
    if let Some(radius) = annotation.arc_radius {
        annotation.arc_radius = Some(radius.min(side_length));
    }
    crate::validate_arrow(&next).ok()?;
    Some(next)
}

/// Migrate legacy geometry without changing its first side or measured angle.
/// Already canonical records remain byte-for-byte unchanged.
pub fn normalize_angle(arrow: &ArrowData) -> Option<ArrowData> {
    let mut next = arrow.clone();
    normalize_angle_in_place(&mut next).ok()?;
    Some(next)
}

/// Normalize an imported record without reallocating points or changing its origin.
/// Validation failures leave the record unchanged.
pub fn normalize_angle_in_place(arrow: &mut ArrowData) -> Result<(), ErrorCode> {
    let points = angle_local_points(arrow).ok_or(ErrorCode::InvalidArgument)?;
    let (rays, equal) = AngleRays::read_points(
        points,
        arrow.angle.ok_or(ErrorCode::InvalidArgument)?.full_turn,
    )
    .ok_or(ErrorCode::InvalidArgument)?;
    if equal {
        return crate::validate_arrow(arrow);
    }
    let endpoint = if rays.sweep == 0.0 || rays.sweep == TAU {
        points[0]
    } else {
        Point::new(
            rays.vertex.x + rays.side_length * rays.end_direction.x,
            rays.vertex.y + rays.side_length * rays.end_direction.y,
        )
    };
    let previous = (arrow.points[2], arrow.width, arrow.height, arrow.angle);
    arrow.points[2] = [endpoint.x, endpoint.y];
    (arrow.width, arrow.height) = crate::arrow_geom::compute_bounds_from_points(&arrow.points);
    if let Some(annotation) = &mut arrow.angle
        && let Some(radius) = annotation.arc_radius
    {
        annotation.arc_radius = Some(radius.min(rays.side_length));
    }
    if let Err(error) = crate::validate_arrow(arrow) {
        arrow.points[2] = previous.0;
        arrow.width = previous.1;
        arrow.height = previous.2;
        arrow.angle = previous.3;
        return Err(error);
    }
    Ok(())
}

pub fn angle_with_sweep(arrow: &ArrowData, sweep: f64) -> Option<ArrowData> {
    crate::validate_arrow(arrow).ok()?;
    if !sweep.is_finite() || !(0.0..=TAU).contains(&sweep) {
        return None;
    }
    let mut points = angle_points(arrow)?;
    let mut shape = AngleShape::from_points(points, arrow.angle?.full_turn)?;
    if shape.sweep == sweep {
        return Some(arrow.clone());
    }
    shape.sweep = sweep;
    points[2] = shape.second_point(points[0]);
    rebuilt_angle(arrow, points, sweep == TAU, shape.side_length)
}

pub fn angle_with_moved_point(
    arrow: &ArrowData,
    index: usize,
    point: Point<f64>,
    snap_sweep: bool,
) -> Option<ArrowData> {
    crate::validate_arrow(arrow).ok()?;
    let mut points = angle_points(arrow)?;
    if *points.get(index)? == point {
        let shape = AngleShape::from_points(points, arrow.angle?.full_turn)?;
        if !snap_sweep
            || index == 1
            || (shape.snapped().sweep - shape.sweep).abs() <= COINCIDENT_DIRECTION_TOLERANCE
        {
            return Some(arrow.clone());
        }
    }
    points[index] = point;
    let (rays, _) = AngleRays::read_points(points, false)?;
    let mut shape = rays.shape();
    if index == 2 {
        let length = rays.second_length;
        if length != shape.side_length {
            points[0] = Point::new(
                shape.vertex.x + rays.start_direction.x * length,
                shape.vertex.y + rays.start_direction.y * length,
            );
        }
        shape.side_length = length;
    }
    if snap_sweep && index != 1 {
        let snapped = shape.snapped();
        if index == 0 {
            shape.first_bearing += snapped.sweep - shape.sweep;
            shape.sweep = snapped.sweep;
            points[0] = shape.point_at(shape.first_bearing);
        } else {
            shape.sweep = snapped.sweep;
        }
    }
    if index != 2 || snap_sweep {
        points[2] = shape.second_point(points[0]);
    }
    let full_turn = shape.sweep == TAU || (shape.sweep == 0.0 && arrow.angle?.full_turn);
    rebuilt_angle(arrow, points, full_turn, shape.side_length)
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AngleGeometry {
    pub vertex: Point<f64>,
    pub start_direction: Point<f64>,
    pub end_direction: Point<f64>,
    /// Visual counterclockwise sweep in the downward-positive canvas, in radians.
    pub sweep: f64,
    pub radius: f64,
    pub automatic_radius: f64,
    pub maximum_radius: f64,
    pub bisector: Point<f64>,
}

pub fn angle_geometry(arrow: &ArrowData) -> Option<AngleGeometry> {
    let annotation = arrow.angle?;
    validate_angle_annotation(annotation).ok()?;
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
    let points = [
        Point::new(a[0], a[1]),
        Point::new(vertex[0], vertex[1]),
        Point::new(b[0], b[1]),
    ];
    let (rays, equal) = AngleRays::read_points(points, annotation.full_turn)?;
    if !equal {
        return None;
    }
    let start_direction = rays.start_direction;
    let end_direction = if rays.sweep == 0.0 || rays.sweep == TAU {
        start_direction
    } else {
        rays.end_direction
    };
    let sweep = rays.sweep;
    let (sin, cos) = (sweep / 2.0).sin_cos();
    let bisector = Point::new(
        start_direction.x * cos + start_direction.y * sin,
        start_direction.y * cos - start_direction.x * sin,
    );
    let font_size = annotation_font_size(arrow);
    let maximum_radius = rays.side_length;
    let automatic_radius =
        (0.30 * maximum_radius).min(24.0_f64.max(1.25 * font_size + arrow.stroke_width));
    let radius = annotation
        .arc_radius
        .unwrap_or(automatic_radius)
        .min(maximum_radius);
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
        automatic_radius,
        maximum_radius,
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

pub fn angle_arc_control_point(arrow: &ArrowData) -> Option<Point<f64>> {
    let geometry = angle_geometry(arrow)?;
    (geometry.sweep > 0.0).then(|| {
        Point::new(
            geometry.vertex.x + geometry.radius * geometry.bisector.x,
            geometry.vertex.y + geometry.radius * geometry.bisector.y,
        )
    })
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
        bold: false,
        italic: false,
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

pub fn angle_arc_hit_test(arrow: &ArrowData, point: Point<f64>, tolerance: f64) -> bool {
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

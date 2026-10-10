use crate::document_geometry::SHAPE_CONNECTOR_GAP;
use crate::{
    ArrowData, FillStyle, HighlightShape, RectangleData, RectangleElementKind, rect_bounds,
};
use serde::{Deserialize, Serialize};
use snow_draw_engine_core::arrow::{ArrowType, Arrowhead, StrokeStyle};
use snow_draw_engine_core::{ColorRgba8, CornerRadii, DrawRect, ErrorCode, Point};

/// A lens samples the pristine base image, independently of annotation paint order.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct MagnifierData {
    pub source_center: Point<f64>,
    pub width: f64,
    pub height: f64,
    pub magnified_center: Point<f64>,
    pub rotation: f64,
    pub shape: HighlightShape,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub factor: f64,
    pub show_leader: bool,
    pub leader_arrowhead: Option<Arrowhead>,
    #[serde(default = "default_corner_radii")]
    pub corner_radii: CornerRadii,
    pub opacity: f64,
}

fn default_corner_radii() -> CornerRadii {
    CornerRadii::splat(6.0)
}

impl Default for MagnifierData {
    fn default() -> Self {
        Self {
            source_center: Point::default(),
            width: 1.0,
            height: 1.0,
            magnified_center: Point::default(),
            rotation: 0.0,
            shape: HighlightShape::Rectangle,
            stroke: ColorRgba8 {
                r: 0x21,
                g: 0x6b,
                b: 0xa5,
                a: 255,
            },
            stroke_width: 2.0,
            factor: 2.0,
            show_leader: true,
            leader_arrowhead: None,
            corner_radii: default_corner_radii(),
            opacity: 1.0,
        }
    }
}

impl MagnifierData {
    pub fn source_rect(self) -> RectangleData {
        RectangleData {
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: self.shape,
            center: self.source_center,
            width: self.width,
            height: self.height,
            rotation: self.rotation,
            // Interaction uses the full shape interior, even without a base image.
            fill: ColorRgba8 {
                r: 0,
                g: 0,
                b: 0,
                a: 255,
            },
            fill_style: FillStyle::Solid,
            stroke: self.stroke,
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: crate::normalize_corner_radii(self.width, self.height, self.corner_radii),
            opacity: self.opacity,
        }
    }
    pub fn magnified_rect(self) -> RectangleData {
        RectangleData {
            center: self.magnified_center,
            width: self.width * self.factor,
            height: self.height * self.factor,
            stroke_width: self.stroke_width,
            corner_radii: crate::normalize_corner_radii(
                self.width * self.factor,
                self.height * self.factor,
                self.corner_radii,
            ),
            ..self.source_rect()
        }
    }
    pub fn translated(mut self, delta: Point<f64>) -> Self {
        self.source_center.x += delta.x;
        self.source_center.y += delta.y;
        self.magnified_center.x += delta.x;
        self.magnified_center.y += delta.y;
        self
    }
    /// Ends at the source center, leaving a gap outside the lens's stroked edge.
    pub fn leader(self) -> Option<ArrowData> {
        if !self.show_leader {
            return None;
        }
        let (sin, cos) = self.rotation.sin_cos();
        let dx = self.source_center.x - self.magnified_center.x;
        let dy = self.source_center.y - self.magnified_center.y;
        let x = dx * cos + dy * sin;
        let y = -dx * sin + dy * cos;
        let half_w = self.width * self.factor / 2.0;
        let half_h = self.height * self.factor / 2.0;
        if half_w <= 0.0 || half_h <= 0.0 {
            return None;
        }
        let metric = match self.shape {
            HighlightShape::Rectangle => (x.abs() / half_w).max(y.abs() / half_h),
            HighlightShape::Ellipse => (x / half_w).hypot(y / half_h),
            HighlightShape::Diamond => x.abs() / half_w + y.abs() / half_h,
        };
        let radii = self.magnified_rect().corner_radii;
        if contour_distance(self.shape, half_w, half_h, radii, x, y) == 0.0 {
            return None;
        }
        let length = dx.hypot(dy);
        if length <= f64::EPSILON {
            return None;
        }
        let half_stroke = self.stroke_width / 2.0;
        let outer_radius = half_w.hypot(half_h) + half_stroke;
        if length <= outer_radius
            && contour_distance(self.shape, half_w, half_h, radii, x, y) <= half_stroke
        {
            return None;
        }
        // Rounded corners can intersect the ray before the bounding rectangle.
        let mut low = if self.shape == HighlightShape::Rectangle {
            0.0
        } else {
            length / metric
        };
        let mut high = length.min(outer_radius);
        // Intersect the actual round-joined stroked contour, including oblique
        // ellipse and diamond rays; expanding their radii is not equivalent.
        for _ in 0..48 {
            let middle = f64::midpoint(low, high);
            if contour_distance(
                self.shape,
                half_w,
                half_h,
                radii,
                x * (middle / length),
                y * (middle / length),
            ) <= half_stroke
            {
                low = middle;
            } else {
                high = middle;
            }
        }
        // Keep the round line cap clear of the lens, even with a thick stroke.
        let start_distance = high + SHAPE_CONNECTOR_GAP + half_stroke;
        let intersection_tolerance = outer_radius * f64::EPSILON * 32.0;
        if length - start_distance <= half_stroke + intersection_tolerance {
            return None;
        }
        let start = Point::new(
            self.magnified_center.x + dx * (start_distance / length),
            self.magnified_center.y + dy * (start_distance / length),
        );
        ArrowData::from_global_points(
            &[start, self.source_center],
            self.stroke,
            self.stroke_width,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            self.leader_arrowhead,
        )
        .map(|mut arrow| {
            arrow.opacity = self.opacity;
            arrow
        })
    }
}

pub fn validate_magnifier(value: &MagnifierData) -> Result<(), ErrorCode> {
    if [
        value.source_center.x,
        value.source_center.y,
        value.width,
        value.height,
        value.magnified_center.x,
        value.magnified_center.y,
        value.rotation,
        value.stroke_width,
        value.factor,
        value.opacity,
        value.corner_radii.top_left,
        value.corner_radii.top_right,
        value.corner_radii.bottom_right,
        value.corner_radii.bottom_left,
    ]
    .iter()
    .any(|v| !v.is_finite())
        || value.width < 0.0
        || value.height < 0.0
        || [
            value.corner_radii.top_left,
            value.corner_radii.top_right,
            value.corner_radii.bottom_right,
            value.corner_radii.bottom_left,
        ]
        .iter()
        .any(|radius| *radius < 0.0)
        || !(0.0..=72.0).contains(&value.stroke_width)
        || !(1.0..=10.0).contains(&value.factor)
        || !(0.0..=1.0).contains(&value.opacity)
        || !(value.width * value.factor).is_finite()
        || !(value.height * value.factor).is_finite()
    {
        return Err(ErrorCode::InvalidArgument);
    }
    let source = rect_bounds(&value.source_rect());
    let lens = rect_bounds(&value.magnified_rect());
    if [
        source.min_x,
        source.min_y,
        source.max_x,
        source.max_y,
        lens.min_x,
        lens.min_y,
        lens.max_x,
        lens.max_y,
        value.source_center.x - value.magnified_center.x,
        value.source_center.y - value.magnified_center.y,
        (value.source_center.x - value.magnified_center.x)
            .hypot(value.source_center.y - value.magnified_center.y),
        (value.width * value.factor / 2.0).hypot(value.height * value.factor / 2.0)
            + value.stroke_width / 2.0,
    ]
    .iter()
    .any(|v| !v.is_finite())
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

pub fn magnifier_bounds(value: &MagnifierData) -> DrawRect {
    let mut bounds = rect_bounds(&value.magnified_rect());
    if let Some(leader) = value.leader() {
        let next = crate::arrow_bounds(&leader);
        bounds.min_x = bounds.min_x.min(next.min_x);
        bounds.min_y = bounds.min_y.min(next.min_y);
        bounds.max_x = bounds.max_x.max(next.max_x);
        bounds.max_y = bounds.max_y.max(next.max_y);
    }
    bounds
}

pub fn magnifier_interaction_bounds(value: &MagnifierData) -> DrawRect {
    let mut bounds = magnifier_bounds(value);
    let source = rect_bounds(&value.source_rect());
    bounds.min_x = bounds.min_x.min(source.min_x);
    bounds.min_y = bounds.min_y.min(source.min_y);
    bounds.max_x = bounds.max_x.max(source.max_x);
    bounds.max_y = bounds.max_y.max(source.max_y);
    bounds
}

pub fn magnifier_hit_test(value: &MagnifierData, point: Point<f64>, tolerance: f64) -> bool {
    crate::rectangle_hit_test(&value.source_rect(), point, tolerance)
        || crate::rectangle_hit_test(&value.magnified_rect(), point, tolerance)
}

fn contour_distance(
    shape: HighlightShape,
    a: f64,
    b: f64,
    radii: CornerRadii,
    x: f64,
    y: f64,
) -> f64 {
    if shape == HighlightShape::Rectangle {
        let mut distance = (x.abs() - a).max(0.0).hypot((y.abs() - b).max(0.0));
        for (sx, sy, radius) in [
            (-1.0, -1.0, radii.top_left),
            (1.0, -1.0, radii.top_right),
            (1.0, 1.0, radii.bottom_right),
            (-1.0, 1.0, radii.bottom_left),
        ] {
            let dx = sx * x - (a - radius);
            let dy = sy * y - (b - radius);
            if dx > 0.0 && dy > 0.0 {
                distance = distance.max(dx.hypot(dy) - radius);
            }
        }
        return distance;
    }
    let x = x.abs();
    let y = y.abs();
    match shape {
        HighlightShape::Rectangle => unreachable!(),
        HighlightShape::Diamond => {
            if x / a + y / b <= 1.0 {
                return 0.0;
            }
            let ax = a;
            let ay = 0.0;
            let bx = 0.0;
            let by = b;
            let scale = a.max(b).max(x).max(y).max(1.0);
            let dx = (bx - ax) / scale;
            let dy = (by - ay) / scale;
            let px = (x - ax) / scale;
            let py = (y - ay) / scale;
            let t = ((px * dx + py * dy) / (dx * dx + dy * dy)).clamp(0.0, 1.0);
            (px - dx * t).hypot(py - dy * t) * scale
        }
        HighlightShape::Ellipse => {
            if (x / a).hypot(y / b) <= 1.0 {
                return 0.0;
            }
            let scale = a.max(b).max(x).max(y).max(1.0);
            let a = a / scale;
            let b = b / scale;
            let x = x / scale;
            let y = y / scale;
            let mut low = 0.0;
            let mut high = a * x + b * y;
            for _ in 0..48 {
                let lambda = f64::midpoint(low, high);
                let metric = (a * x / (lambda + a * a)).hypot(b * y / (lambda + b * b));
                if metric > 1.0 {
                    low = lambda;
                } else {
                    high = lambda;
                }
            }
            let nearest_x = a * a * x / (high + a * a);
            let nearest_y = b * b * y / (high + b * b);
            (x - nearest_x).hypot(y - nearest_y) * scale
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn magnifier_rounded_corners_default_normalize_and_hit_test() {
        let value = MagnifierData {
            width: 20.0,
            height: 20.0,
            magnified_center: Point::new(100.0, 100.0),
            stroke_width: 0.0,
            ..Default::default()
        };
        assert_eq!(value.corner_radii, CornerRadii::splat(6.0));
        assert_eq!(value.source_rect().corner_radii, CornerRadii::splat(6.0));
        assert_eq!(value.magnified_rect().corner_radii, CornerRadii::splat(6.0));
        assert!(!magnifier_hit_test(&value, Point::new(-9.9, -9.9), 0.0));
        assert!(!magnifier_hit_test(&value, Point::new(80.1, 80.1), 0.0));
        assert!(magnifier_hit_test(&value, Point::new(0.0, 0.0), 0.0));
        assert!(magnifier_hit_test(&value, Point::new(100.0, 100.0), 0.0));
        let small = MagnifierData {
            width: 4.0,
            height: 2.0,
            ..value
        };
        assert_eq!(small.source_rect().corner_radii, CornerRadii::splat(1.0));
        assert_eq!(small.magnified_rect().corner_radii, CornerRadii::splat(2.0));
        let square = MagnifierData {
            corner_radii: CornerRadii::default(),
            ..value
        };
        assert!(magnifier_hit_test(&square, Point::new(-9.9, -9.9), 0.0));
        for radius in [f64::NAN, f64::INFINITY, -1.0] {
            assert_eq!(
                validate_magnifier(&MagnifierData {
                    corner_radii: CornerRadii::splat(radius),
                    ..value
                }),
                Err(ErrorCode::InvalidArgument)
            );
        }
    }

    #[test]
    fn magnifier_missing_serialized_radii_use_six_pixel_default() {
        let mut json = serde_json::to_value(MagnifierData::default()).unwrap();
        json.as_object_mut().unwrap().remove("corner_radii");
        let restored: MagnifierData = serde_json::from_value(json).unwrap();
        assert_eq!(restored.corner_radii, CornerRadii::splat(6.0));
    }

    #[test]
    fn magnifier_rounded_leader_leaves_gap_at_each_actual_corner() {
        for (sx, sy) in [(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)] {
            let value = MagnifierData {
                width: 20.0,
                height: 20.0,
                source_center: Point::new(sx * 100.0, sy * 100.0),
                corner_radii: CornerRadii {
                    top_left: 3.0,
                    top_right: 6.0,
                    bottom_right: 12.0,
                    bottom_left: 24.0,
                },
                stroke_width: 2.0,
                ..Default::default()
            };
            let edge = value.leader().unwrap().global_points()[0];
            let radii = value.magnified_rect().corner_radii;
            let radius = match (sx > 0.0, sy > 0.0) {
                (false, false) => radii.top_left,
                (true, false) => radii.top_right,
                (true, true) => radii.bottom_right,
                (false, true) => radii.bottom_left,
            };
            let expected =
                20.0 - radius + (radius + 1.0 + SHAPE_CONNECTOR_GAP + 1.0) / 2.0_f64.sqrt();
            assert!((edge.x - sx * expected).abs() < 1e-8);
            assert!((edge.y - sy * expected).abs() < 1e-8);
        }
        // A source outside a rounded corner still needs room for the connector gap.
        let value = MagnifierData {
            width: 20.0,
            height: 20.0,
            source_center: Point::new(19.9, 19.9),
            corner_radii: CornerRadii::splat(10.0),
            stroke_width: 0.0,
            ..Default::default()
        };
        assert!(value.leader().is_none());
        // A larger radius leaves enough room even inside the lens's bounding box.
        assert!(
            MagnifierData {
                corner_radii: CornerRadii::splat(20.0),
                ..value
            }
            .leader()
            .is_some()
        );
    }

    #[test]
    fn magnifier_leader_leaves_gap_from_actual_outer_stroke_on_oblique_rays() {
        for shape in [
            HighlightShape::Rectangle,
            HighlightShape::Ellipse,
            HighlightShape::Diamond,
        ] {
            for source in [Point::new(300.0, 140.0), Point::new(-280.0, 200.0)] {
                let value = MagnifierData {
                    shape,
                    width: 80.0,
                    height: 50.0,
                    source_center: source,
                    stroke_width: 32.0,
                    ..Default::default()
                };
                let leader = value.leader().unwrap();
                let points = leader.global_points();
                assert_eq!(points[1], source);
                let length = source.x.hypot(source.y);
                let offset = 8.0 + value.stroke_width / 2.0;
                let edge = Point::new(
                    points[0].x - source.x / length * offset,
                    points[0].y - source.y / length * offset,
                );
                let distance = contour_distance(
                    shape,
                    80.0,
                    50.0,
                    value.magnified_rect().corner_radii,
                    edge.x,
                    edge.y,
                );
                assert!((distance - 16.0).abs() < 1e-8, "{shape:?}: {distance}");
                let cross = points[0].x * source.y - points[0].y * source.x;
                assert!(cross.abs() < 1e-8);
            }
        }
    }

    #[test]
    fn magnifier_leader_gap_survives_rotation_and_stroke_width_changes() {
        for shape in [
            HighlightShape::Rectangle,
            HighlightShape::Ellipse,
            HighlightShape::Diamond,
        ] {
            for stroke_width in [0.0, 2.0, 16.0, 72.0] {
                for rotation in [0.0, 0.7, std::f64::consts::FRAC_PI_2] {
                    let (sin, cos) = rotation.sin_cos();
                    let center = Point::new(20.0, -40.0);
                    let source = Point::new(center.x + 300.0 * cos, center.y + 300.0 * sin);
                    let value = MagnifierData {
                        shape,
                        width: 80.0,
                        height: 50.0,
                        source_center: source,
                        magnified_center: center,
                        rotation,
                        stroke_width,
                        ..Default::default()
                    };
                    let points = value.leader().unwrap().global_points();
                    let expected = 80.0 + stroke_width + 8.0;
                    assert!((points[0].x - center.x - expected * cos).abs() < 1e-8);
                    assert!((points[0].y - center.y - expected * sin).abs() < 1e-8);
                    assert!((points[1].x - source.x).abs() < 1e-8);
                    assert!((points[1].y - source.y).abs() < 1e-8);
                }
            }
        }
    }

    #[test]
    fn magnifier_leader_omits_geometry_without_room_for_gap_and_caps() {
        for shape in [
            HighlightShape::Rectangle,
            HighlightShape::Ellipse,
            HighlightShape::Diamond,
        ] {
            for stroke_width in [2.0, 16.0, 72.0] {
                let minimum_distance = 80.0 + 8.0 + 1.5 * stroke_width;
                let value = MagnifierData {
                    shape,
                    width: 80.0,
                    height: 50.0,
                    stroke_width,
                    ..Default::default()
                };
                for x in [
                    80.0,
                    80.0 + stroke_width / 2.0,
                    minimum_distance - 1.0,
                    minimum_distance,
                ] {
                    assert!(
                        MagnifierData {
                            source_center: Point::new(x, 0.0),
                            ..value
                        }
                        .leader()
                        .is_none(),
                        "{shape:?}: {stroke_width}, {x}"
                    );
                }
                assert!(
                    MagnifierData {
                        source_center: Point::new(minimum_distance + 1.0, 0.0),
                        ..value
                    }
                    .leader()
                    .is_some()
                );
            }
        }
    }

    #[test]
    fn magnifier_leader_contains_center_and_toggle_omit_geometry() {
        for shape in [
            HighlightShape::Rectangle,
            HighlightShape::Ellipse,
            HighlightShape::Diamond,
        ] {
            let value = MagnifierData {
                shape,
                width: 80.0,
                height: 50.0,
                source_center: Point::new(20.0, 0.0),
                ..Default::default()
            };
            assert!(value.leader().is_none());
            assert!(
                MagnifierData {
                    source_center: Point::new(300.0, 0.0),
                    show_leader: false,
                    ..value
                }
                .leader()
                .is_none()
            );
        }
    }

    #[test]
    fn magnifier_validation_rejects_invalid_factor_and_overflow() {
        for factor in [f64::NAN, f64::INFINITY, 0.9, 10.1] {
            assert!(
                validate_magnifier(&MagnifierData {
                    factor,
                    ..Default::default()
                })
                .is_err()
            );
        }
        assert!(
            validate_magnifier(&MagnifierData {
                width: f64::MAX,
                factor: 2.0,
                ..Default::default()
            })
            .is_err()
        );
        assert!(
            validate_magnifier(&MagnifierData {
                magnified_center: Point::new(f64::MAX, 0.0),
                width: f64::MAX,
                factor: 1.0,
                ..Default::default()
            })
            .is_err()
        );
        for factor in [1.0, 2.0, 10.0] {
            assert!(
                validate_magnifier(&MagnifierData {
                    factor,
                    ..Default::default()
                })
                .is_ok()
            );
        }
    }

    #[test]
    fn magnifier_paint_bounds_exclude_source_while_interaction_includes_it() {
        let value = MagnifierData {
            width: 20.0,
            height: 30.0,
            source_center: Point::new(500.0, 500.0),
            show_leader: false,
            ..Default::default()
        };
        let paint = magnifier_bounds(&value);
        let interaction = magnifier_interaction_bounds(&value);
        assert!(paint.max_x < 100.0 && paint.max_y < 100.0);
        assert!(interaction.max_x >= 510.0 && interaction.max_y >= 515.0);
    }

    #[test]
    fn magnifier_far_away_source_keeps_leader_finite_and_preserves_opacity() {
        for shape in [
            HighlightShape::Rectangle,
            HighlightShape::Ellipse,
            HighlightShape::Diamond,
        ] {
            let value = MagnifierData {
                source_center: Point::new(1e200, 1e200),
                width: 100.0,
                height: 80.0,
                opacity: 0.4,
                shape,
                ..Default::default()
            };
            validate_magnifier(&value).unwrap();
            let leader = value.leader().unwrap();
            assert_eq!(leader.opacity, 0.4);
            let bounds = crate::arrow_bounds(&leader);
            assert!(
                [bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y]
                    .iter()
                    .all(|v| v.is_finite())
            );
        }
        assert_eq!(
            validate_magnifier(&MagnifierData {
                source_center: Point::new(f64::MAX, f64::MAX),
                ..Default::default()
            }),
            Err(ErrorCode::InvalidArgument)
        );
    }
}

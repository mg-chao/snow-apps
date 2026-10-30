use std::hash::{Hash, Hasher};

use crate::Point;

#[path = "snap/snap_candidates.rs"]
mod snap_candidates;
#[path = "snap/snap_guides.rs"]
mod snap_guides;
#[path = "snap/snap_scoring.rs"]
mod snap_scoring;

#[cfg(test)]
#[path = "snap/bracket_snap_tests.rs"]
mod bracket_snap_tests;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum SnappingMode {
    #[default]
    None,
    Object,
    Grid,
}

pub fn resolve_persistent_snapping_mode(grid_enabled: bool, object_enabled: bool) -> SnappingMode {
    if grid_enabled {
        SnappingMode::Grid
    } else if object_enabled {
        SnappingMode::Object
    } else {
        SnappingMode::None
    }
}

pub fn resolve_effective_snapping_mode(
    grid_enabled: bool,
    object_enabled: bool,
    ctrl_pressed: bool,
) -> SnappingMode {
    if ctrl_pressed {
        if grid_enabled || object_enabled {
            SnappingMode::None
        } else {
            SnappingMode::Object
        }
    } else {
        resolve_persistent_snapping_mode(grid_enabled, object_enabled)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SnapGuideKind {
    Point,
    Gap,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SnapGuideAxis {
    Horizontal,
    Vertical,
}

#[derive(Clone, Debug, PartialEq)]
pub struct SnapGuide {
    pub kind: SnapGuideKind,
    pub axis: SnapGuideAxis,
    pub start: Point<f64>,
    pub end: Point<f64>,
    pub markers: Vec<Point<f64>>,
    pub label: Option<f64>,
}

impl SnapGuide {
    pub fn new(
        kind: SnapGuideKind,
        axis: SnapGuideAxis,
        start: Point<f64>,
        end: Point<f64>,
    ) -> Self {
        Self {
            kind,
            axis,
            start,
            end,
            markers: Vec::new(),
            label: None,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct DrawRect {
    pub min_x: f64,
    pub min_y: f64,
    pub max_x: f64,
    pub max_y: f64,
}

impl DrawRect {
    pub const fn new(min_x: f64, min_y: f64, max_x: f64, max_y: f64) -> Self {
        Self {
            min_x,
            min_y,
            max_x,
            max_y,
        }
    }

    pub fn from_center(center: Point<f64>, width: f64, height: f64) -> Self {
        Self::new(
            center.x - width / 2.0,
            center.y - height / 2.0,
            center.x + width / 2.0,
            center.y + height / 2.0,
        )
    }

    pub fn width(self) -> f64 {
        self.max_x - self.min_x
    }

    pub fn height(self) -> f64 {
        self.max_y - self.min_y
    }

    pub fn center_x(self) -> f64 {
        f64::midpoint(self.min_x, self.max_x)
    }

    pub fn center_y(self) -> f64 {
        f64::midpoint(self.min_y, self.max_y)
    }

    pub fn center(self) -> Point<f64> {
        Point::new(self.center_x(), self.center_y())
    }

    pub fn translate(self, offset: Point<f64>) -> Self {
        Self::new(
            self.min_x + offset.x,
            self.min_y + offset.y,
            self.max_x + offset.x,
            self.max_y + offset.y,
        )
    }
}

impl Eq for DrawRect {}

impl Hash for DrawRect {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.min_x.to_bits().hash(state);
        self.min_y.to_bits().hash(state);
        self.max_x.to_bits().hash(state);
        self.max_y.to_bits().hash(state);
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnapQuery {
    pub point: Point<f64>,
    pub threshold: f64,
    pub include_grid: bool,
    pub grid_size: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct SnapResult {
    pub point: Point<f64>,
    pub guides: Vec<SnapGuide>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GridSnapService;

impl GridSnapService {
    pub const fn new() -> Self {
        Self
    }

    pub fn snap_value(&self, value: f64, grid_size: f64) -> f64 {
        if !value.is_finite() || !grid_size.is_finite() || grid_size <= 0.0 {
            return value;
        }

        let snapped = (value / grid_size).round() * grid_size;
        if snapped.is_finite() { snapped } else { value }
    }

    pub fn snap_point(&self, point: Point<f64>, grid_size: f64) -> Point<f64> {
        Point::new(
            self.snap_value(point.x, grid_size),
            self.snap_value(point.y, grid_size),
        )
    }

    pub fn snap_rect(
        &self,
        rect: DrawRect,
        grid_size: f64,
        snap_min_x: bool,
        snap_max_x: bool,
        snap_min_y: bool,
        snap_max_y: bool,
    ) -> DrawRect {
        if !snap_min_x && !snap_max_x && !snap_min_y && !snap_max_y {
            return rect;
        }

        DrawRect::new(
            if snap_min_x {
                self.snap_value(rect.min_x, grid_size)
            } else {
                rect.min_x
            },
            if snap_min_y {
                self.snap_value(rect.min_y, grid_size)
            } else {
                rect.min_y
            },
            if snap_max_x {
                self.snap_value(rect.max_x, grid_size)
            } else {
                rect.max_x
            },
            if snap_max_y {
                self.snap_value(rect.max_y, grid_size)
            } else {
                rect.max_y
            },
        )
    }
}

pub const GRID_SNAP_SERVICE: GridSnapService = GridSnapService::new();

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SnapAxis {
    X,
    Y,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SnapAxisAnchor {
    Start,
    Center,
    End,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ObjectSnapResult {
    pub dx: f64,
    pub dy: f64,
    pub guides: Vec<SnapGuide>,
}

impl ObjectSnapResult {
    pub fn new(dx: f64, dy: f64, guides: Vec<SnapGuide>) -> Self {
        Self { dx, dy, guides }
    }

    pub fn has_snap(&self) -> bool {
        self.dx != 0.0 || self.dy != 0.0
    }
}

#[derive(Clone, Copy, Debug)]
pub struct ObjectSnapRectRequest<'a> {
    pub target_rect: DrawRect,
    pub reference_rects: &'a [DrawRect],
    pub snap_distance: f64,
    pub target_anchors_x: &'a [SnapAxisAnchor],
    pub target_anchors_y: &'a [SnapAxisAnchor],
    pub enable_point_snaps: bool,
    pub enable_gap_snaps: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ObjectSnapService;

/// Translation constraints applied before choosing movement snaps and building guides.
/// Bounds must contain the target size; position_step must be finite and positive.
#[derive(Clone, Copy, Debug)]
pub struct ObjectSnapMoveConstraints {
    pub bounds: DrawRect,
    pub position_step: f64,
}

impl ObjectSnapMoveConstraints {
    fn position(self, value: f64, min: f64, max: f64) -> f64 {
        // Restrict to lattice positions inside the bounds, including fractional bounds.
        let min = (min / self.position_step).ceil();
        let max = (max / self.position_step).floor();
        (value / self.position_step).round().clamp(min, max) * self.position_step
    }

    fn resolve(self, rect: DrawRect) -> DrawRect {
        let x = self.position(
            rect.min_x,
            self.bounds.min_x,
            self.bounds.max_x - rect.width(),
        );
        let y = self.position(
            rect.min_y,
            self.bounds.min_y,
            self.bounds.max_y - rect.height(),
        );
        rect.translate(Point::new(x - rect.min_x, y - rect.min_y))
    }
}

impl ObjectSnapService {
    const EPSILON: f64 = 0.0001;
    const ALL_ANCHORS: [SnapAxisAnchor; 3] = [
        SnapAxisAnchor::Start,
        SnapAxisAnchor::Center,
        SnapAxisAnchor::End,
    ];
    const PRIORITY_DISTANCE_SLACK_FACTOR: f64 = 0.05;
    const PRIORITY_DISTANCE_SLACK_MAX: f64 = 0.5;
    const STRENGTH_SLACK: f64 = 0.05;
    const POINT_DISTANCE_WEIGHT: f64 = 0.45;
    const POINT_PERPENDICULAR_WEIGHT: f64 = 0.4;
    const POINT_ANCHOR_WEIGHT: f64 = 0.15;
    const GAP_DISTANCE_WEIGHT: f64 = 0.7;
    const GAP_FREQUENCY_WEIGHT: f64 = 0.2;
    const GAP_KIND_WEIGHT: f64 = 0.1;
    const GAP_STRENGTH_SCALE: f64 = 0.9;
    const PERPENDICULAR_SIZE_RANGE_FACTOR: f64 = 1.5;
    const PERPENDICULAR_SNAP_RANGE_FACTOR: f64 = 4.0;
    const MAX_ASSOCIATED_GAP_GUIDES: usize = 4;
    const MAX_ANCHOR_PRIORITY: f64 = 3.0;

    pub const fn new() -> Self {
        Self
    }

    pub fn snap_move(
        &self,
        target_rect: DrawRect,
        reference_rects: &[DrawRect],
        snap_distance: f64,
        enable_point_snaps: bool,
        enable_gap_snaps: bool,
    ) -> ObjectSnapResult {
        self.snap_rect(ObjectSnapRectRequest {
            target_rect,
            reference_rects,
            snap_distance,
            target_anchors_x: &Self::ALL_ANCHORS,
            target_anchors_y: &Self::ALL_ANCHORS,
            enable_point_snaps,
            enable_gap_snaps,
        })
    }

    pub fn snap_resize(
        &self,
        target_rect: DrawRect,
        reference_rects: &[DrawRect],
        snap_distance: f64,
        target_anchors_x: &[SnapAxisAnchor],
        target_anchors_y: &[SnapAxisAnchor],
        enable_point_snaps: bool,
    ) -> ObjectSnapResult {
        self.snap_rect(ObjectSnapRectRequest {
            target_rect,
            reference_rects,
            snap_distance,
            target_anchors_x,
            target_anchors_y,
            enable_point_snaps,
            enable_gap_snaps: false,
        })
    }

    pub fn snap_rect(&self, request: ObjectSnapRectRequest<'_>) -> ObjectSnapResult {
        self.snap_rect_with_reference_points(request, &[])
    }

    /// Point references supply axis alignments and markers, but never spacing geometry.
    pub fn snap_rect_with_reference_points(
        &self,
        request: ObjectSnapRectRequest<'_>,
        reference_points: &[Point<f64>],
    ) -> ObjectSnapResult {
        self.snap_rect_impl(request, reference_points, None)
    }

    /// Keep the unconstrained editor path unchanged. Movement clients may restrict
    /// positions to a pixel lattice and capture bounds; infeasible candidates never
    /// compete with representable alignments, and all guides use the resolved rect.
    pub fn snap_rect_with_move_constraints(
        &self,
        request: ObjectSnapRectRequest<'_>,
        constraints: Option<ObjectSnapMoveConstraints>,
    ) -> ObjectSnapResult {
        self.snap_rect_impl(request, &[], constraints)
    }

    fn snap_rect_impl(
        &self,
        request: ObjectSnapRectRequest<'_>,
        reference_points: &[Point<f64>],
        constraints: Option<ObjectSnapMoveConstraints>,
    ) -> ObjectSnapResult {
        let ObjectSnapRectRequest {
            target_rect,
            reference_rects,
            snap_distance,
            target_anchors_x,
            target_anchors_y,
            enable_point_snaps,
            enable_gap_snaps,
        } = request;

        if snap_distance <= 0.0
            || (reference_rects.is_empty() && reference_points.is_empty())
            || (!enable_point_snaps && !enable_gap_snaps)
            || (target_anchors_x.is_empty() && target_anchors_y.is_empty())
        {
            let resolved = constraints.map_or(target_rect, |limits| limits.resolve(target_rect));
            return ObjectSnapResult::new(
                resolved.min_x - target_rect.min_x,
                resolved.min_y - target_rect.min_y,
                Vec::new(),
            );
        }

        let effective_target_anchors_x = Self::deduplicate_anchors(target_anchors_x);
        let effective_target_anchors_y = Self::deduplicate_anchors(target_anchors_y);

        let mut candidates_x = Self::build_axis_candidates(
            SnapAxis::X,
            target_rect,
            reference_rects,
            &effective_target_anchors_x,
            snap_distance,
            enable_point_snaps,
            enable_gap_snaps,
        );
        let mut candidates_y = Self::build_axis_candidates(
            SnapAxis::Y,
            target_rect,
            reference_rects,
            &effective_target_anchors_y,
            snap_distance,
            enable_point_snaps,
            enable_gap_snaps,
        );

        if enable_point_snaps {
            let point_rects: Vec<_> = reference_points
                .iter()
                .filter(|point| point.x.is_finite() && point.y.is_finite())
                .map(|point| DrawRect::new(point.x, point.y, point.x, point.y))
                .collect();
            candidates_x.extend(Self::build_point_candidates(
                SnapAxis::X,
                target_rect,
                &point_rects,
                &effective_target_anchors_x,
                snap_distance,
            ));
            candidates_y.extend(Self::build_point_candidates(
                SnapAxis::Y,
                target_rect,
                &point_rects,
                &effective_target_anchors_y,
                snap_distance,
            ));
        }

        if let Some(limits) = constraints {
            candidates_x.retain(|candidate| {
                let proposed = target_rect.translate(Point::new(candidate.offset, 0.0));
                (limits.resolve(proposed).min_x - proposed.min_x).abs() < Self::EPSILON
            });
            candidates_y.retain(|candidate| {
                let proposed = target_rect.translate(Point::new(0.0, candidate.offset));
                (limits.resolve(proposed).min_y - proposed.min_y).abs() < Self::EPSILON
            });
        }
        let x_candidate = Self::select_best_candidate(&candidates_x, target_rect, snap_distance);
        let y_candidate = Self::select_best_candidate(&candidates_y, target_rect, snap_distance);

        let dx = x_candidate.map_or(0.0, |candidate| candidate.offset);
        let dy = y_candidate.map_or(0.0, |candidate| candidate.offset);
        let proposed = target_rect.translate(Point::new(dx, dy));
        let snapped_rect = constraints.map_or(proposed, |limits| limits.resolve(proposed));

        let mut guides = Vec::new();
        Self::append_candidate_guides(
            x_candidate,
            y_candidate,
            snapped_rect,
            reference_rects,
            snap_distance,
            &mut guides,
        );
        Self::append_candidate_guides(
            y_candidate,
            x_candidate,
            snapped_rect,
            reference_rects,
            snap_distance,
            &mut guides,
        );

        if constraints.is_some() {
            ObjectSnapResult::new(
                snapped_rect.min_x - target_rect.min_x,
                snapped_rect.min_y - target_rect.min_y,
                guides,
            )
        } else {
            ObjectSnapResult::new(dx, dy, guides)
        }
    }

    fn overlaps_perpendicular(a: DrawRect, b: DrawRect, axis: SnapAxis) -> bool {
        if axis == SnapAxis::X {
            return a.max_y >= b.min_y && a.min_y <= b.max_y;
        }

        a.max_x >= b.min_x && a.min_x <= b.max_x
    }

    fn anchor_position(rect: DrawRect, axis: SnapAxis, anchor: SnapAxisAnchor) -> f64 {
        match axis {
            SnapAxis::X => match anchor {
                SnapAxisAnchor::Start => rect.min_x,
                SnapAxisAnchor::Center => rect.center_x(),
                SnapAxisAnchor::End => rect.max_x,
            },
            SnapAxis::Y => match anchor {
                SnapAxisAnchor::Start => rect.min_y,
                SnapAxisAnchor::Center => rect.center_y(),
                SnapAxisAnchor::End => rect.max_y,
            },
        }
    }

    fn axis_min(rect: DrawRect, axis: SnapAxis) -> f64 {
        if axis == SnapAxis::X {
            rect.min_x
        } else {
            rect.min_y
        }
    }

    fn axis_max(rect: DrawRect, axis: SnapAxis) -> f64 {
        if axis == SnapAxis::X {
            rect.max_x
        } else {
            rect.max_y
        }
    }

    fn axis_center(rect: DrawRect, axis: SnapAxis) -> f64 {
        if axis == SnapAxis::X {
            rect.center_x()
        } else {
            rect.center_y()
        }
    }

    fn axis_size(rect: DrawRect, axis: SnapAxis) -> f64 {
        if axis == SnapAxis::X {
            rect.width()
        } else {
            rect.height()
        }
    }

    fn perpendicular_axis(axis: SnapAxis) -> SnapAxis {
        if axis == SnapAxis::X {
            SnapAxis::Y
        } else {
            SnapAxis::X
        }
    }

    fn rect_perpendicular_distance(a: DrawRect, b: DrawRect, axis: SnapAxis) -> f64 {
        if axis == SnapAxis::X {
            if a.max_y < b.min_y {
                return b.min_y - a.max_y;
            }
            if b.max_y < a.min_y {
                return a.min_y - b.max_y;
            }
            return 0.0;
        }

        if a.max_x < b.min_x {
            return b.min_x - a.max_x;
        }
        if b.max_x < a.min_x {
            return a.min_x - b.max_x;
        }
        0.0
    }

    fn anchor_priority(target: SnapAxisAnchor, reference: SnapAxisAnchor) -> i32 {
        if target == SnapAxisAnchor::Center && reference == SnapAxisAnchor::Center {
            return 0;
        }
        if target == reference {
            return 1;
        }
        if target == SnapAxisAnchor::Center || reference == SnapAxisAnchor::Center {
            return 2;
        }
        3
    }

    fn is_exact(offset: f64) -> bool {
        offset.abs() <= Self::EPSILON
    }

    fn deduplicate_anchors(anchors: &[SnapAxisAnchor]) -> Vec<SnapAxisAnchor> {
        if anchors.len() < 2 {
            return anchors.to_vec();
        }

        let mut deduplicated = Vec::with_capacity(anchors.len());
        for &anchor in anchors {
            if !deduplicated.contains(&anchor) {
                deduplicated.push(anchor);
            }
        }

        deduplicated
    }
}

pub const OBJECT_SNAP_SERVICE: ObjectSnapService = ObjectSnapService::new();

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SnapKind {
    Point,
    GapCenter,
    GapSide,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum GapSide {
    Before,
    After,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum GapNeighborDirection {
    Before,
    After,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct AxisCandidate {
    axis: SnapAxis,
    offset: f64,
    kind: SnapKind,
    reference_rect: Option<DrawRect>,
    target_anchor: Option<SnapAxisAnchor>,
    reference_anchor: Option<SnapAxisAnchor>,
    perpendicular_distance: Option<f64>,
    gap_before_rect: Option<DrawRect>,
    gap_after_rect: Option<DrawRect>,
    gap_size: Option<f64>,
    gap_side: Option<GapSide>,
    gap_frequency: Option<i32>,
}

impl AxisCandidate {
    fn point(
        axis: SnapAxis,
        offset: f64,
        reference_rect: DrawRect,
        target_anchor: SnapAxisAnchor,
        reference_anchor: SnapAxisAnchor,
        perpendicular_distance: f64,
    ) -> Self {
        Self {
            axis,
            offset,
            kind: SnapKind::Point,
            reference_rect: Some(reference_rect),
            target_anchor: Some(target_anchor),
            reference_anchor: Some(reference_anchor),
            perpendicular_distance: Some(perpendicular_distance),
            gap_before_rect: None,
            gap_after_rect: None,
            gap_size: None,
            gap_side: None,
            gap_frequency: None,
        }
    }

    fn gap_center(
        axis: SnapAxis,
        offset: f64,
        gap_before_rect: DrawRect,
        gap_after_rect: DrawRect,
        gap_size: f64,
        gap_frequency: impl Into<Option<i32>>,
    ) -> Self {
        Self {
            axis,
            offset,
            kind: SnapKind::GapCenter,
            reference_rect: None,
            target_anchor: None,
            reference_anchor: None,
            perpendicular_distance: None,
            gap_before_rect: Some(gap_before_rect),
            gap_after_rect: Some(gap_after_rect),
            gap_size: Some(gap_size),
            gap_side: None,
            gap_frequency: gap_frequency.into(),
        }
    }

    fn gap_side(
        axis: SnapAxis,
        offset: f64,
        reference_rect: DrawRect,
        gap_size: f64,
        gap_frequency: i32,
        side: GapSide,
    ) -> Self {
        Self {
            axis,
            offset,
            kind: SnapKind::GapSide,
            reference_rect: Some(reference_rect),
            target_anchor: None,
            reference_anchor: None,
            perpendicular_distance: None,
            gap_before_rect: None,
            gap_after_rect: None,
            gap_size: Some(gap_size),
            gap_side: Some(side),
            gap_frequency: Some(gap_frequency),
        }
    }

    fn distance(self) -> f64 {
        self.offset.abs()
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct GapSegment {
    before: DrawRect,
    after: DrawRect,
    gap: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct GapSizeBucket {
    size: f64,
    count: i32,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct GapSideCandidateRequest {
    axis: SnapAxis,
    target_rect: DrawRect,
    target_size: f64,
    snap_distance: f64,
    gap_size: f64,
    gap_frequency: i32,
    gap_side: GapSide,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct GapSideCandidate {
    axis: SnapAxis,
    offset: f64,
    snap_distance: f64,
    reference_rect: DrawRect,
    gap_size: f64,
    gap_frequency: i32,
    gap_side: GapSide,
}

#[cfg(test)]
mod constrained_move_tests {
    use super::*;

    fn rect(x: f64, y: f64, w: f64, h: f64) -> DrawRect {
        DrawRect::new(x, y, x + w, y + h)
    }

    fn snap(target: DrawRect, references: &[DrawRect], bounds: DrawRect) -> ObjectSnapResult {
        OBJECT_SNAP_SERVICE.snap_rect_with_move_constraints(
            ObjectSnapRectRequest {
                target_rect: target,
                reference_rects: references,
                snap_distance: 8.0,
                target_anchors_x: &ObjectSnapService::ALL_ANCHORS,
                target_anchors_y: &ObjectSnapService::ALL_ANCHORS,
                enable_point_snaps: true,
                enable_gap_snaps: true,
            },
            Some(ObjectSnapMoveConstraints {
                bounds,
                position_step: 1.0,
            }),
        )
    }

    #[test]
    fn pixel_centers_preserve_the_other_axis_and_rebuild_markers() {
        let bounds = rect(-200.0, -200.0, 600.0, 600.0);
        let result = snap(
            rect(31.0, 22.0, 20.0, 20.0),
            &[rect(20.0, 20.0, 41.0, 40.0)],
            bounds,
        );
        assert_eq!((result.dx, result.dy), (0.0, -2.0));
        assert_eq!(result.guides.len(), 1);
        let guide = &result.guides[0];
        assert_eq!(guide.axis, SnapGuideAxis::Horizontal);
        assert_eq!(
            guide.markers,
            vec![Point::new(41.0, 20.0), Point::new(40.5, 20.0)]
        );
        let transposed = snap(
            rect(22.0, 31.0, 20.0, 20.0),
            &[rect(20.0, 20.0, 40.0, 41.0)],
            bounds,
        );
        assert_eq!((transposed.dx, transposed.dy), (-2.0, 0.0));
        assert_eq!(transposed.guides.len(), 1);
        assert_eq!(transposed.guides[0].axis, SnapGuideAxis::Vertical);
        assert_eq!(
            transposed.guides[0].markers,
            vec![Point::new(20.0, 41.0), Point::new(20.0, 40.5)]
        );
        // An infeasible center must not displace a nearby representable edge candidate.
        let alternative = snap(
            rect(39.0, 100.0, 20.0, 20.0),
            &[rect(20.0, 20.0, 41.0, 40.0)],
            bounds,
        );
        assert_eq!(alternative.dx, 2.0);
        assert!(!alternative.guides.is_empty());
    }

    #[test]
    fn boundary_rejection_preserves_perpendicular_alignment() {
        let result = snap(
            rect(24.0, 22.0, 40.0, 40.0),
            &[rect(28.0, 20.0, 40.0, 40.0)],
            rect(0.0, 0.0, 64.0, 300.0),
        );
        assert_eq!((result.dx, result.dy), (0.0, -2.0));
        assert_eq!(result.guides.len(), 1);
        assert_eq!(result.guides[0].axis, SnapGuideAxis::Horizontal);
        assert_eq!(
            result.guides[0].markers,
            vec![Point::new(44.0, 40.0), Point::new(48.0, 40.0)]
        );
    }

    #[test]
    fn gap_endpoints_use_the_final_perpendicular_position() {
        let result = snap(
            rect(178.0, 40.0, 40.0, 20.0),
            &[rect(20.0, 30.0, 40.0, 41.0), rect(100.0, 30.0, 40.0, 41.0)],
            rect(-200.0, -200.0, 600.0, 600.0),
        );
        assert_eq!((result.dx, result.dy), (2.0, 0.0));
        let gap = result
            .guides
            .iter()
            .find(|guide| {
                guide.kind == SnapGuideKind::Gap && guide.start.x == 140.0 && guide.end.x == 180.0
            })
            .expect("moving gap");
        assert_eq!(gap.start.y, 50.0);
        assert_eq!(gap.end.y, 50.0);
        assert_eq!(gap.label, Some(40.0));
    }

    #[test]
    fn empty_references_still_resolve_the_pixel_bounds() {
        let result = snap(
            rect(-2.5, 100.5, 20.0, 20.0),
            &[],
            rect(0.2, 0.2, 100.0, 100.0),
        );
        assert_eq!((result.dx, result.dy), (3.5, -20.5));
        assert!(result.guides.is_empty());
    }
}

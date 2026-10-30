use std::cmp::Ordering;

use super::*;

impl ObjectSnapService {
    pub(super) fn build_axis_candidates(
        axis: SnapAxis,
        target_rect: DrawRect,
        reference_rects: &[DrawRect],
        target_anchors: &[SnapAxisAnchor],
        snap_distance: f64,
        enable_point_snaps: bool,
        enable_gap_snaps: bool,
    ) -> Vec<AxisCandidate> {
        if target_anchors.is_empty() {
            return Vec::new();
        }

        let mut candidates = Vec::new();
        if enable_point_snaps {
            candidates.extend(Self::build_point_candidates(
                axis,
                target_rect,
                reference_rects,
                target_anchors,
                snap_distance,
            ));
        }
        if enable_gap_snaps {
            candidates.extend(Self::build_gap_candidates(
                axis,
                target_rect,
                reference_rects,
                target_anchors,
                snap_distance,
            ));
        }
        candidates
    }

    pub(super) fn build_point_candidates(
        axis: SnapAxis,
        target_rect: DrawRect,
        reference_rects: &[DrawRect],
        target_anchors: &[SnapAxisAnchor],
        snap_distance: f64,
    ) -> Vec<AxisCandidate> {
        let mut candidates = Vec::new();

        for &reference_rect in reference_rects {
            let perpendicular_distance =
                Self::rect_perpendicular_distance(target_rect, reference_rect, axis);

            for &target_anchor in target_anchors {
                let target_pos = Self::anchor_position(target_rect, axis, target_anchor);
                for &reference_anchor in &Self::ALL_ANCHORS {
                    let reference_pos =
                        Self::anchor_position(reference_rect, axis, reference_anchor);
                    let offset = reference_pos - target_pos;
                    if offset.abs() <= snap_distance {
                        candidates.push(AxisCandidate::point(
                            axis,
                            offset,
                            reference_rect,
                            target_anchor,
                            reference_anchor,
                            perpendicular_distance,
                        ));
                    }
                }
            }
        }

        candidates
    }

    fn build_gap_candidates(
        axis: SnapAxis,
        target_rect: DrawRect,
        reference_rects: &[DrawRect],
        target_anchors: &[SnapAxisAnchor],
        snap_distance: f64,
    ) -> Vec<AxisCandidate> {
        let mut candidates = Vec::new();
        let allow_center = target_anchors.contains(&SnapAxisAnchor::Center);

        let filtered = Self::resolve_gap_reference_rects(axis, target_rect, reference_rects);
        if filtered.len() < 2 {
            return candidates;
        }

        let segments = Self::build_gap_segments(axis, &filtered);
        if segments.is_empty() {
            return candidates;
        }

        let gap_buckets = Self::gap_size_buckets(&segments);
        if gap_buckets.is_empty() {
            return candidates;
        }

        let target_center = Self::anchor_position(target_rect, axis, SnapAxisAnchor::Center);
        let target_size = Self::axis_size(target_rect, axis);

        let before_neighbor =
            Self::closest_neighbor(axis, target_rect, &filtered, GapNeighborDirection::Before);
        let after_neighbor =
            Self::closest_neighbor(axis, target_rect, &filtered, GapNeighborDirection::After);

        for segment in &segments {
            if allow_center {
                let desired_center = (Self::axis_max(segment.before, axis)
                    + Self::axis_min(segment.after, axis))
                    / 2.0;
                let offset = desired_center - target_center;
                if offset.abs() <= snap_distance {
                    let gap_frequency = Self::gap_frequency_for(&gap_buckets, segment.gap);
                    candidates.push(AxisCandidate::gap_center(
                        axis,
                        offset,
                        segment.before,
                        segment.after,
                        segment.gap,
                        gap_frequency,
                    ));
                }
            }
        }

        for bucket in &gap_buckets {
            let gap_size = bucket.size;
            let gap_frequency = bucket.count;

            Self::add_gap_side_candidate_from_neighbor(
                &mut candidates,
                before_neighbor,
                GapSideCandidateRequest {
                    axis,
                    target_rect,
                    target_size,
                    snap_distance,
                    gap_size,
                    gap_frequency,
                    gap_side: GapSide::After,
                },
            );
            Self::add_gap_side_candidate_from_neighbor(
                &mut candidates,
                after_neighbor,
                GapSideCandidateRequest {
                    axis,
                    target_rect,
                    target_size,
                    snap_distance,
                    gap_size,
                    gap_frequency,
                    gap_side: GapSide::Before,
                },
            );
        }

        candidates
    }

    pub(super) fn resolve_gap_reference_rects(
        axis: SnapAxis,
        target_rect: DrawRect,
        reference_rects: &[DrawRect],
    ) -> Vec<DrawRect> {
        let mut filtered = reference_rects
            .iter()
            .copied()
            .filter(|rect| Self::overlaps_perpendicular(*rect, target_rect, axis))
            .collect::<Vec<_>>();

        filtered.sort_by(|left, right| {
            Self::axis_min(*left, axis)
                .partial_cmp(&Self::axis_min(*right, axis))
                .unwrap_or(Ordering::Equal)
        });

        filtered
    }

    pub(super) fn build_gap_segments(axis: SnapAxis, sorted_rects: &[DrawRect]) -> Vec<GapSegment> {
        if sorted_rects.len() < 2 {
            return Vec::new();
        }

        let mut segments = Vec::new();
        for index in 0..(sorted_rects.len() - 1) {
            let before = sorted_rects[index];
            let after = sorted_rects[index + 1];
            let gap = Self::axis_min(after, axis) - Self::axis_max(before, axis);
            if gap > 0.0 {
                segments.push(GapSegment { before, after, gap });
            }
        }

        segments
    }

    fn gap_size_buckets(segments: &[GapSegment]) -> Vec<GapSizeBucket> {
        let mut buckets = Vec::new();

        for segment in segments {
            let gap = segment.gap;
            if let Some(index) = buckets
                .iter()
                .position(|bucket: &GapSizeBucket| (bucket.size - gap).abs() <= Self::EPSILON)
            {
                buckets[index].count += 1;
            } else {
                buckets.push(GapSizeBucket {
                    size: gap,
                    count: 1,
                });
            }
        }

        buckets
    }

    fn gap_frequency_for(buckets: &[GapSizeBucket], gap: f64) -> i32 {
        buckets
            .iter()
            .find(|bucket| (bucket.size - gap).abs() <= Self::EPSILON)
            .map_or(0, |bucket| bucket.count)
    }

    fn closest_neighbor(
        axis: SnapAxis,
        target_rect: DrawRect,
        reference_rects: &[DrawRect],
        direction: GapNeighborDirection,
    ) -> Option<DrawRect> {
        let target_min = Self::axis_min(target_rect, axis);
        let target_max = Self::axis_max(target_rect, axis);

        let mut closest = None;
        let mut best_distance = f64::INFINITY;

        for &reference_rect in reference_rects {
            let distance = match direction {
                GapNeighborDirection::Before => target_min - Self::axis_max(reference_rect, axis),
                GapNeighborDirection::After => Self::axis_min(reference_rect, axis) - target_max,
            };

            if distance < -Self::EPSILON || distance > best_distance {
                continue;
            }

            best_distance = distance;
            closest = Some(reference_rect);
        }

        closest
    }

    fn add_gap_side_candidate_from_neighbor(
        candidates: &mut Vec<AxisCandidate>,
        neighbor: Option<DrawRect>,
        request: GapSideCandidateRequest,
    ) {
        let Some(neighbor) = neighbor else {
            return;
        };

        let desired_start = match request.gap_side {
            GapSide::After => Self::axis_max(neighbor, request.axis) + request.gap_size,
            GapSide::Before => {
                Self::axis_min(neighbor, request.axis) - request.gap_size - request.target_size
            }
        };

        Self::add_gap_side_candidate(
            candidates,
            GapSideCandidate {
                axis: request.axis,
                offset: desired_start - Self::axis_min(request.target_rect, request.axis),
                snap_distance: request.snap_distance,
                reference_rect: neighbor,
                gap_size: request.gap_size,
                gap_frequency: request.gap_frequency,
                gap_side: request.gap_side,
            },
        );
    }

    fn add_gap_side_candidate(candidates: &mut Vec<AxisCandidate>, candidate: GapSideCandidate) {
        if candidate.offset.abs() > candidate.snap_distance {
            return;
        }

        candidates.push(AxisCandidate::gap_side(
            candidate.axis,
            candidate.offset,
            candidate.reference_rect,
            candidate.gap_size,
            candidate.gap_frequency,
            candidate.gap_side,
        ));
    }
}

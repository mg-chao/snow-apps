use std::{collections::BTreeMap, sync::OnceLock};

use rayon::prelude::*;

use crate::{
    CandidateDiagnostics, Frame, Geometry, MotionDiagnostics, MotionEstimate,
    MotionEstimatorOptions, MotionStage, PixelFormat, RegionDiagnostics, StitchAxis, StitchError,
    region::{GrayImage, SimilarityMap, TemporalRegionModel, TileLayout},
    sampling::SamplingPlan,
};

const LOWE_RATIO: f32 = 0.8;
const MAX_HAMMING_DISTANCE: f32 = 64.0;
const MAX_CROSS_AXIS_DELTA: i32 = 4;
const INLIER_TOLERANCE: i32 = 2;
const MAX_CANDIDATES: usize = 8;
const MAX_FEATURES_PER_TILE: usize = 8;
const MIN_INLIER_MATCHES: u32 = 8;
const MIN_INLIER_TILES: u32 = 4;
const MIN_RESIDUAL_GAIN: f32 = 0.15;

#[derive(Debug, Clone, Copy, PartialEq)]
struct MatchObservation {
    distance: f32,
    reference_x: f32,
    reference_y: f32,
    incoming_x: f32,
    incoming_y: f32,
    reference_tile: usize,
    dx: i32,
    dy: i32,
}

#[derive(Debug, Clone, Copy, PartialEq)]
struct BinaryFeature {
    x: f32,
    y: f32,
    descriptor: [u8; 32],
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct DescriptorMatch {
    train_index: usize,
    distance: u32,
}

#[derive(Debug, Clone)]
struct ScoredCandidate {
    diagnostics: CandidateDiagnostics,
    compensated: SimilarityMap,
}

struct FeatureEvidence {
    reference_features: u32,
    incoming_features: u32,
    observations: Vec<MatchObservation>,
}

struct EvaluationImages<'a> {
    reference: &'a Frame,
    incoming: &'a Frame,
    reference_gray: &'a GrayImage,
    incoming_gray: &'a GrayImage,
    direct: &'a SimilarityMap,
    zero_alignment: &'a SimilarityMap,
}

struct EvaluatedMotion {
    estimate: MotionEstimate,
    compensated: Option<SimilarityMap>,
    scene_cut: bool,
    margin: f32,
}

impl EvaluatedMotion {
    fn rejected(scene_cut: bool, confidence: f32, diagnostics: MotionDiagnostics) -> Self {
        Self {
            estimate: MotionEstimate::indeterminate(confidence, diagnostics),
            compensated: None,
            scene_cut,
            margin: 0.0,
        }
    }

    fn needs_full_resolution(&self) -> bool {
        self.compensated.is_none()
            || self.margin <= 0.0
            || !self
                .estimate
                .diagnostics
                .candidates
                .first()
                .is_some_and(|candidate| {
                    candidate
                        .precise_alignment_error
                        .is_some_and(|error| error < 1.0)
                })
    }
}

fn round_coordinate(value: f32) -> i32 {
    (value + 0.5).trunc() as i32
}

fn hamming_distance(left: &[u8; 32], right: &[u8; 32]) -> u32 {
    left.chunks_exact(8)
        .zip(right.chunks_exact(8))
        .map(|(left, right)| {
            (u64::from_ne_bytes(left.try_into().expect("eight-byte descriptor word"))
                ^ u64::from_ne_bytes(right.try_into().expect("eight-byte descriptor word")))
            .count_ones()
        })
        .sum()
}

fn nearest_two(query: &BinaryFeature, train: &[BinaryFeature]) -> Option<[DescriptorMatch; 2]> {
    let mut first = DescriptorMatch {
        train_index: usize::MAX,
        distance: u32::MAX,
    };
    let mut second = first;
    for (train_index, candidate) in train.iter().enumerate() {
        let distance = hamming_distance(&query.descriptor, &candidate.descriptor);
        let matched = DescriptorMatch {
            train_index,
            distance,
        };
        if (distance, train_index) < (first.distance, first.train_index) {
            second = first;
            first = matched;
        } else if (distance, train_index) < (second.distance, second.train_index) {
            second = matched;
        }
    }
    (second.train_index != usize::MAX).then_some([first, second])
}

fn match_descriptors_portable(
    queries: &[BinaryFeature],
    train: &[BinaryFeature],
) -> Vec<Option<DescriptorMatch>> {
    queries
        .par_iter()
        .map(|feature| nearest_two(feature, train).and_then(passes_ratio))
        .collect()
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "popcnt")]
unsafe fn nearest_two_popcnt(
    query: &BinaryFeature,
    train: &[BinaryFeature],
) -> Option<[DescriptorMatch; 2]> {
    let query_words = query.descriptor.as_ptr().cast::<u64>();
    let query_words = unsafe {
        [
            std::ptr::read_unaligned(query_words),
            std::ptr::read_unaligned(query_words.add(1)),
            std::ptr::read_unaligned(query_words.add(2)),
            std::ptr::read_unaligned(query_words.add(3)),
        ]
    };
    let mut first = DescriptorMatch {
        train_index: usize::MAX,
        distance: u32::MAX,
    };
    let mut second = first;
    for (train_index, candidate) in train.iter().enumerate() {
        let candidate_words = candidate.descriptor.as_ptr().cast::<u64>();
        let candidate_words = unsafe {
            [
                std::ptr::read_unaligned(candidate_words),
                std::ptr::read_unaligned(candidate_words.add(1)),
                std::ptr::read_unaligned(candidate_words.add(2)),
                std::ptr::read_unaligned(candidate_words.add(3)),
            ]
        };
        let distance = query_words
            .into_iter()
            .zip(candidate_words)
            .map(|(query, candidate)| (query ^ candidate).count_ones())
            .sum();
        let matched = DescriptorMatch {
            train_index,
            distance,
        };
        if (distance, train_index) < (first.distance, first.train_index) {
            second = first;
            first = matched;
        } else if (distance, train_index) < (second.distance, second.train_index) {
            second = matched;
        }
    }
    (second.train_index != usize::MAX).then_some([first, second])
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "popcnt")]
unsafe fn match_descriptors_popcnt(
    queries: &[BinaryFeature],
    train: &[BinaryFeature],
) -> Vec<Option<DescriptorMatch>> {
    queries
        .par_iter()
        .map(|query| unsafe { nearest_two_popcnt(query, train).and_then(passes_ratio) })
        .collect()
}

#[cfg(target_arch = "x86")]
use std::arch::x86::{
    __m256i, _mm256_add_epi8, _mm256_and_si256, _mm256_loadu_si256, _mm256_sad_epu8,
    _mm256_set1_epi8, _mm256_setr_epi8, _mm256_setzero_si256, _mm256_shuffle_epi8,
    _mm256_srli_epi16, _mm256_storeu_si256, _mm256_xor_si256,
};
#[cfg(target_arch = "x86_64")]
use std::arch::x86_64::{
    __m256i, _mm256_add_epi8, _mm256_and_si256, _mm256_loadu_si256, _mm256_sad_epu8,
    _mm256_set1_epi8, _mm256_setr_epi8, _mm256_setzero_si256, _mm256_shuffle_epi8,
    _mm256_srli_epi16, _mm256_storeu_si256, _mm256_xor_si256,
};

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn nearest_two_avx2(
    query: &BinaryFeature,
    train: &[BinaryFeature],
) -> Option<[DescriptorMatch; 2]> {
    let lookup = _mm256_setr_epi8(
        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3,
        3, 4,
    );
    let low_nibble = _mm256_set1_epi8(0x0f);
    let zero = _mm256_setzero_si256();
    let query = unsafe { _mm256_loadu_si256(query.descriptor.as_ptr().cast::<__m256i>()) };
    let mut first = DescriptorMatch {
        train_index: usize::MAX,
        distance: u32::MAX,
    };
    let mut second = first;
    for (train_index, candidate) in train.iter().enumerate() {
        let candidate =
            unsafe { _mm256_loadu_si256(candidate.descriptor.as_ptr().cast::<__m256i>()) };
        let difference = _mm256_xor_si256(query, candidate);
        let low = _mm256_and_si256(difference, low_nibble);
        let high = _mm256_and_si256(_mm256_srli_epi16(difference, 4), low_nibble);
        let counts = _mm256_add_epi8(
            _mm256_shuffle_epi8(lookup, low),
            _mm256_shuffle_epi8(lookup, high),
        );
        let sums = _mm256_sad_epu8(counts, zero);
        let mut lanes = [0_u64; 4];
        unsafe { _mm256_storeu_si256(lanes.as_mut_ptr().cast::<__m256i>(), sums) };
        let distance = lanes.into_iter().sum::<u64>() as u32;
        let matched = DescriptorMatch {
            train_index,
            distance,
        };
        if (distance, train_index) < (first.distance, first.train_index) {
            second = first;
            first = matched;
        } else if (distance, train_index) < (second.distance, second.train_index) {
            second = matched;
        }
    }
    (second.train_index != usize::MAX).then_some([first, second])
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn match_descriptors_avx2(
    queries: &[BinaryFeature],
    train: &[BinaryFeature],
) -> Vec<Option<DescriptorMatch>> {
    queries
        .par_iter()
        .map(|query| unsafe { nearest_two_avx2(query, train).and_then(passes_ratio) })
        .collect()
}

fn match_descriptors(
    queries: &[BinaryFeature],
    train: &[BinaryFeature],
) -> Vec<Option<DescriptorMatch>> {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    if std::arch::is_x86_feature_detected!("avx2") {
        return unsafe { match_descriptors_avx2(queries, train) };
    }
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    if std::arch::is_x86_feature_detected!("popcnt") {
        return unsafe { match_descriptors_popcnt(queries, train) };
    }
    match_descriptors_portable(queries, train)
}

fn passes_ratio(matches: [DescriptorMatch; 2]) -> Option<DescriptorMatch> {
    let first = matches[0];
    let second = matches[1];
    (first.distance as f32 <= MAX_HAMMING_DISTANCE
        && (first.distance as f32) < LOWE_RATIO * second.distance as f32)
        .then_some(first)
}

fn mutual_observations_rust(
    reference: &[BinaryFeature],
    incoming: &[BinaryFeature],
    layout: TileLayout,
    axis: StitchAxis,
    parallel_work: bool,
) -> Vec<MatchObservation> {
    let (reverse_best, forward_best) = if parallel_work {
        rayon::join(
            || match_descriptors(incoming, reference),
            || match_descriptors(reference, incoming),
        )
    } else {
        (
            match_descriptors(incoming, reference),
            match_descriptors(reference, incoming),
        )
    };
    let mut observations = Vec::new();
    for (reference_index, matched) in forward_best.into_iter().enumerate() {
        let Some(matched) = matched else {
            continue;
        };
        let incoming_index = matched.train_index;
        if reverse_best
            .get(incoming_index)
            .and_then(|matched| *matched)
            .map(|matched| matched.train_index)
            != Some(reference_index)
        {
            continue;
        }
        let feature = reference[reference_index];
        let incoming_feature = incoming[incoming_index];
        let dx = round_coordinate(incoming_feature.x) - round_coordinate(feature.x);
        let dy = round_coordinate(incoming_feature.y) - round_coordinate(feature.y);
        if axis.cross_delta(dx, dy).abs() > MAX_CROSS_AXIS_DELTA {
            continue;
        }
        let Some(reference_tile) = layout.index(feature.x, feature.y) else {
            continue;
        };
        observations.push(MatchObservation {
            distance: matched.distance as f32,
            reference_x: feature.x,
            reference_y: feature.y,
            incoming_x: incoming_feature.x,
            incoming_y: incoming_feature.y,
            reference_tile,
            dx,
            dy,
        });
    }
    observations
}

fn detect_balanced_rust(
    frame: &Frame,
    layout: TileLayout,
    regions: &TemporalRegionModel,
    max_features: usize,
    pyramid_plan: &crate::orb::PyramidPlan,
    sampling: Option<&SamplingPlan>,
) -> Vec<BinaryFeature> {
    let candidate_limit = max_features.saturating_mul(2).max(max_features);
    let image = crate::orb::grayscale(frame);
    let image = if let Some(plan) = sampling {
        plan.apply(image)
    } else {
        image
    };
    let detection = crate::orb::detect(&image, candidate_limit, pyramid_plan);
    let mut ranked = detection
        .keypoints()
        .iter()
        .copied()
        .filter_map(|keypoint| {
            let (x, y) = sampling.map_or((keypoint.x, keypoint.y), |plan| {
                plan.source_coordinates(keypoint.x, keypoint.y)
            });
            let tile = layout.index(x, y)?;
            let score = keypoint.response.max(0.001) * regions.weight_at_tile(tile);
            Some((score, tile, keypoint))
        })
        .collect::<Vec<_>>();
    ranked.sort_by(|left, right| {
        right
            .0
            .total_cmp(&left.0)
            .then_with(|| left.2.x.total_cmp(&right.2.x))
            .then_with(|| left.2.y.total_cmp(&right.2.y))
            .then_with(|| left.2.octave.cmp(&right.2.octave))
            .then_with(|| left.2.response.total_cmp(&right.2.response))
            .then_with(|| left.2.angle.total_cmp(&right.2.angle))
    });

    let mut tile_counts = vec![0_usize; layout.len()];
    let mut selected = Vec::new();
    for (_, tile, keypoint) in ranked {
        if selected.len() >= max_features {
            break;
        }
        if tile_counts[tile] >= MAX_FEATURES_PER_TILE {
            continue;
        }
        tile_counts[tile] += 1;
        selected.push(keypoint);
    }
    detection
        .compute(selected)
        .into_iter()
        .map(|feature| {
            let (x, y) = sampling.map_or((feature.keypoint.x, feature.keypoint.y), |plan| {
                plan.source_coordinates(feature.keypoint.x, feature.keypoint.y)
            });
            BinaryFeature {
                x,
                y,
                descriptor: feature.descriptor,
            }
        })
        .collect()
}

// The estimator hands this its full state (frames, tiling, regions, tuning);
// grouping them into a struct would churn every call site for no clarity.
#[allow(clippy::too_many_arguments)]
fn pure_rust_feature_evidence(
    reference: &Frame,
    incoming: &Frame,
    layout: TileLayout,
    regions: &TemporalRegionModel,
    max_features: usize,
    axis: StitchAxis,
    parallel_work: bool,
    pyramid_plan: &crate::orb::PyramidPlan,
    sampling: Option<&SamplingPlan>,
) -> FeatureEvidence {
    let feature_perf = crate::perf::Scope::new(crate::perf::Stage::FeatureExtraction);
    let detect =
        |frame| detect_balanced_rust(frame, layout, regions, max_features, pyramid_plan, sampling);
    let (reference_features, incoming_features) = if parallel_work {
        rayon::join(|| detect(reference), || detect(incoming))
    } else {
        (detect(reference), detect(incoming))
    };
    feature_perf.finish();
    let _perf = crate::perf::Scope::new(crate::perf::Stage::DescriptorMatching);
    let observations = mutual_observations_rust(
        &reference_features,
        &incoming_features,
        layout,
        axis,
        parallel_work,
    );

    FeatureEvidence {
        reference_features: reference_features.len() as u32,
        incoming_features: incoming_features.len() as u32,
        observations,
    }
}

fn candidate_offsets(
    observations: &[MatchObservation],
    axis: StitchAxis,
    maximum_shift: i32,
) -> Vec<i32> {
    let mut exact = BTreeMap::<i32, f32>::new();
    for observation in observations {
        let primary = axis.primary_delta(observation.dx, observation.dy);
        let offset = if primary.abs() < 2 { 0 } else { primary };
        if offset.abs() <= maximum_shift {
            *exact.entry(offset).or_default() +=
                (1.0 - observation.distance / MAX_HAMMING_DISTANCE).clamp(0.05, 1.0);
        }
    }

    let mut modes = exact
        .keys()
        .copied()
        .map(|candidate| {
            let support = exact
                .range(candidate - INLIER_TOLERANCE..=candidate + INLIER_TOLERANCE)
                .map(|(_, support)| support)
                .sum::<f32>();
            (candidate, support)
        })
        .collect::<Vec<_>>();
    modes.sort_by(|left, right| {
        right
            .1
            .total_cmp(&left.1)
            .then_with(|| left.0.abs().cmp(&right.0.abs()))
            .then_with(|| left.0.cmp(&right.0))
    });

    let mut selected = Vec::with_capacity(MAX_CANDIDATES);
    selected.push(0);
    for (candidate, _) in modes {
        if selected.len() >= MAX_CANDIDATES {
            break;
        }
        if selected
            .iter()
            .all(|selected| (selected - candidate).abs() > INLIER_TOLERANCE)
        {
            selected.push(candidate);
        }
    }
    selected
}

fn average_similarity(map: &SimilarityMap, observation: &MatchObservation) -> f32 {
    let reference = map
        .at_point(observation.reference_x, observation.reference_y)
        .unwrap_or(0.5);
    let incoming = map
        .at_point(observation.incoming_x, observation.incoming_y)
        .unwrap_or(reference);
    0.5 * (reference + incoming)
}

fn alignment_errors(
    offset: i32,
    direct: &SimilarityMap,
    zero_alignment: &SimilarityMap,
    compensated: &SimilarityMap,
    regions: &TemporalRegionModel,
) -> (f32, f32) {
    let mut samples = Vec::new();
    for tile in 0..direct.layout().len() {
        let (Some(direct_similarity), Some(zero_similarity), Some(candidate_similarity)) = (
            direct.at_tile(tile),
            zero_alignment.at_tile(tile),
            compensated.at_tile(tile),
        ) else {
            continue;
        };
        let pair_evidence = if offset == 0 {
            direct_similarity
        } else {
            1.0 - direct_similarity
        };
        let texture = direct.texture_at(tile).max(compensated.texture_at(tile));
        if texture < 0.05 {
            continue;
        }
        let weight =
            (0.1 + 0.9 * pair_evidence) * (0.1 + 0.9 * texture) * regions.weight_at_tile(tile);
        samples.push((1.0 - candidate_similarity, 1.0 - zero_similarity, weight));
    }
    if samples.is_empty() {
        return (1.0, 1.0);
    }
    samples.sort_by(|left, right| left.0.total_cmp(&right.0));
    let retained_weight = samples.iter().map(|sample| sample.2).sum::<f32>() * 0.75;
    let mut used_weight = 0.0;
    let mut zero_error = 0.0;
    let mut candidate_error = 0.0;
    for (candidate_sample, zero_sample, weight) in samples {
        let used = weight.min((retained_weight - used_weight).max(0.0));
        if used <= 0.0 {
            break;
        }
        candidate_error += candidate_sample * used;
        zero_error += zero_sample * used;
        used_weight += used;
    }
    if used_weight > 0.0 {
        (zero_error / used_weight, candidate_error / used_weight)
    } else {
        (1.0, 1.0)
    }
}

fn score_candidate(
    offset: i32,
    axis: StitchAxis,
    observations: &[MatchObservation],
    direct: &SimilarityMap,
    zero_alignment: &SimilarityMap,
    compensated: SimilarityMap,
    regions: &TemporalRegionModel,
) -> ScoredCandidate {
    let mut eligible_by_tile = vec![0.0_f32; direct.layout().len()];
    let mut inlier_by_tile = vec![0.0_f32; direct.layout().len()];
    let mut raw_inliers = 0_u32;

    for observation in observations {
        debug_assert!(
            axis.cross_delta(observation.dx, observation.dy).abs() <= MAX_CROSS_AXIS_DELTA
        );
        let direct_similarity = average_similarity(direct, observation);
        let pair_evidence = if offset == 0 {
            direct_similarity
        } else {
            1.0 - direct_similarity
        };
        let descriptor_quality =
            (1.0 - observation.distance / MAX_HAMMING_DISTANCE).clamp(0.05, 1.0);
        let history = 0.5
            * (regions.weight_at_point(observation.reference_x, observation.reference_y)
                + regions.weight_at_point(observation.incoming_x, observation.incoming_y));
        let weight = descriptor_quality * (0.1 + 0.9 * pair_evidence) * history;
        eligible_by_tile[observation.reference_tile] += weight;
        if (axis.primary_delta(observation.dx, observation.dy) - offset).abs() <= INLIER_TOLERANCE {
            inlier_by_tile[observation.reference_tile] += weight;
            raw_inliers = raw_inliers.saturating_add(1);
        }
    }

    let eligible_support = eligible_by_tile
        .iter()
        .map(|weight| weight.min(1.0))
        .sum::<f32>();
    let weighted_support = inlier_by_tile
        .iter()
        .map(|weight| weight.min(1.0))
        .sum::<f32>();
    let weighted_inlier_share = if eligible_support > 0.0 {
        (weighted_support / eligible_support).clamp(0.0, 1.0)
    } else {
        0.0
    };
    let inlier_tiles = inlier_by_tile
        .iter()
        .filter(|weight| **weight >= 0.05)
        .count() as u32;
    let spatial_coverage = (inlier_tiles as f32 / 12.0).clamp(0.0, 1.0);
    let (zero_error, alignment_error) =
        alignment_errors(offset, direct, zero_alignment, &compensated, regions);
    let residual_gain = if offset == 0 || zero_error <= f32::EPSILON {
        0.0
    } else {
        ((zero_error - alignment_error) / zero_error).clamp(0.0, 1.0)
    };
    let score = 0.6 * weighted_inlier_share + 0.25 * residual_gain + 0.15 * spatial_coverage;

    ScoredCandidate {
        diagnostics: CandidateDiagnostics {
            offset,
            raw_inliers,
            inlier_tiles,
            weighted_support,
            weighted_inlier_share,
            spatial_coverage,
            alignment_error,
            precise_alignment_error: None,
            residual_gain,
            score,
        },
        compensated,
    }
}

fn luminance_at(frame: &Frame, x: u32, y: u32) -> u8 {
    let channels = frame.pixel_format().channels() as usize;
    let offset = ((y as usize) * (frame.width() as usize) + x as usize) * channels;
    match frame.pixel_format() {
        PixelFormat::Gray8 => frame.pixels()[offset],
        PixelFormat::Rgb8 | PixelFormat::Rgba8 => {
            let red = u32::from(frame.pixels()[offset]);
            let green = u32::from(frame.pixels()[offset + 1]);
            let blue = u32::from(frame.pixels()[offset + 2]);
            ((77 * red + 150 * green + 29 * blue + 128) >> 8) as u8
        }
    }
}

fn luminance_gradient(frame: &Frame, x: u32, y: u32) -> u16 {
    let horizontal = luminance_at(frame, x + 1, y).abs_diff(luminance_at(frame, x - 1, y));
    let vertical = luminance_at(frame, x, y + 1).abs_diff(luminance_at(frame, x, y - 1));
    u16::from(horizontal) + u16::from(vertical)
}

// Independent scans avoid histogram-reduction overhead for small viewports.
fn precise_alignment_error(
    reference: &Frame,
    incoming: &Frame,
    direct: &SimilarityMap,
    axis: StitchAxis,
    offset: i32,
) -> f32 {
    let mut histogram = [0_u64; 256];
    let mut samples = 0_u64;
    let width = incoming.width() as i32;
    let height = incoming.height() as i32;
    for incoming_y in (1..incoming.height().saturating_sub(1)).step_by(4) {
        for x in (1..incoming.width().saturating_sub(1)).step_by(4) {
            let (reference_x, reference_y) = match axis {
                StitchAxis::Vertical => (x as i32, incoming_y as i32 - offset),
                StitchAxis::Horizontal => (x as i32 - offset, incoming_y as i32),
            };
            if reference_x <= 0
                || reference_x >= width - 1
                || reference_y <= 0
                || reference_y >= height - 1
            {
                continue;
            }
            if direct.at_point(x as f32, incoming_y as f32).unwrap_or(1.0) >= 0.85 {
                continue;
            }
            let reference_value = luminance_at(reference, reference_x as u32, reference_y as u32);
            let incoming_value = luminance_at(incoming, x, incoming_y);
            let reference_gradient =
                luminance_gradient(reference, reference_x as u32, reference_y as u32);
            let incoming_gradient = luminance_gradient(incoming, x, incoming_y);
            if reference_gradient.max(incoming_gradient) < 16 {
                continue;
            }
            let intensity_error = u16::from(reference_value.abs_diff(incoming_value));
            let gradient_error = reference_gradient.abs_diff(incoming_gradient) / 2;
            let difference = intensity_error.saturating_add(gradient_error).min(255) as usize;
            histogram[difference] += 1;
            samples += 1;
        }
    }
    if samples == 0 {
        return 1.0;
    }
    let target = samples.saturating_mul(3).div_ceil(5);
    let mut accumulated = 0_u64;
    for (difference, count) in histogram.into_iter().enumerate() {
        accumulated += count;
        if accumulated >= target {
            return difference as f32 / 255.0;
        }
    }
    1.0
}

fn precise_alignment_errors(
    reference: &Frame,
    incoming: &Frame,
    direct: &SimilarityMap,
    axis: StitchAxis,
    strongest_mode: i32,
    maximum_shift: i32,
) -> [(i32, f32); (2 * INLIER_TOLERANCE + 1) as usize] {
    const OFFSETS: usize = (2 * INLIER_TOLERANCE + 1) as usize;
    let offsets: [i32; OFFSETS] =
        std::array::from_fn(|index| strongest_mode - INLIER_TOLERANCE + index as i32);
    let width = incoming.width() as i32;
    let height = incoming.height() as i32;
    // Incoming values and the fixed-region mask are identical for every offset.
    // Reduce integer histograms across row groups to preserve exact error scores.
    let histograms = (0..incoming.height().saturating_sub(2).div_ceil(4))
        .into_par_iter()
        .with_min_len(16)
        .fold(
            || [[0_u64; 256]; OFFSETS],
            |mut histograms, row| {
                let y = 1 + row * 4;
                for x in (1..incoming.width().saturating_sub(1)).step_by(4) {
                    if direct.at_point(x as f32, y as f32).unwrap_or(1.0) >= 0.85 {
                        continue;
                    }
                    let incoming_value = luminance_at(incoming, x, y);
                    let incoming_gradient = luminance_gradient(incoming, x, y);
                    for (offset, histogram) in offsets.iter().zip(&mut histograms) {
                        if offset.abs() > maximum_shift {
                            continue;
                        }
                        let (reference_x, reference_y) = match axis {
                            StitchAxis::Vertical => (x as i32, y as i32 - offset),
                            StitchAxis::Horizontal => (x as i32 - offset, y as i32),
                        };
                        if reference_x <= 0
                            || reference_x >= width - 1
                            || reference_y <= 0
                            || reference_y >= height - 1
                        {
                            continue;
                        }
                        let reference_x = reference_x as u32;
                        let reference_y = reference_y as u32;
                        let reference_value = luminance_at(reference, reference_x, reference_y);
                        let reference_gradient =
                            luminance_gradient(reference, reference_x, reference_y);
                        if reference_gradient.max(incoming_gradient) < 16 {
                            continue;
                        }
                        let intensity_error = u16::from(reference_value.abs_diff(incoming_value));
                        let gradient_error = reference_gradient.abs_diff(incoming_gradient) / 2;
                        let difference = intensity_error.saturating_add(gradient_error).min(255);
                        histogram[usize::from(difference)] += 1;
                    }
                }
                histograms
            },
        )
        .reduce(
            || [[0_u64; 256]; OFFSETS],
            |mut left, right| {
                for (left, right) in left.iter_mut().flatten().zip(right.iter().flatten()) {
                    *left += right;
                }
                left
            },
        );
    std::array::from_fn(|index| {
        let offset = offsets[index];
        let histogram = histograms[index];
        let samples = histogram.iter().sum::<u64>();
        let target = samples.saturating_mul(3).div_ceil(5);
        let mut accumulated = 0_u64;
        let error = if samples == 0 {
            1.0
        } else {
            histogram
                .into_iter()
                .position(|count| {
                    accumulated += count;
                    accumulated >= target
                })
                .map_or(1.0, |difference| difference as f32 / 255.0)
        };
        (offset, error)
    })
}

fn refine_motion_offset(
    reference: &Frame,
    incoming: &Frame,
    direct: &SimilarityMap,
    axis: StitchAxis,
    strongest_mode: i32,
    maximum_shift: i32,
) -> (i32, f32) {
    let compare = |left: &(i32, f32), right: &(i32, f32)| {
        left.1
            .total_cmp(&right.1)
            .then_with(|| {
                (left.0 - strongest_mode)
                    .abs()
                    .cmp(&(right.0 - strongest_mode).abs())
            })
            .then_with(|| left.0.cmp(&right.0))
    };
    // Histogram reduction loses on small images and cheap Gray8 scans at 1080p.
    let minimum_shared_pixels = match incoming.pixel_format() {
        PixelFormat::Gray8 => 4 * 1024 * 1024,
        PixelFormat::Rgb8 | PixelFormat::Rgba8 => 1024 * 1024,
    };
    let best = if u64::from(incoming.width()) * u64::from(incoming.height()) < minimum_shared_pixels
    {
        (strongest_mode - INLIER_TOLERANCE..=strongest_mode + INLIER_TOLERANCE)
            .into_par_iter()
            .filter(|offset| offset.abs() <= maximum_shift)
            .map(|offset| {
                (
                    offset,
                    precise_alignment_error(reference, incoming, direct, axis, offset),
                )
            })
            .min_by(compare)
    } else {
        precise_alignment_errors(
            reference,
            incoming,
            direct,
            axis,
            strongest_mode,
            maximum_shift,
        )
        .into_iter()
        .filter(|(offset, _)| offset.abs() <= maximum_shift)
        .min_by(compare)
    };
    best.expect("motion refinement range contains its center")
}

fn options_error(message: impl Into<String>) -> StitchError {
    StitchError::InvalidOptions {
        message: message.into(),
    }
}

pub(crate) fn validate_estimator_options(
    options: MotionEstimatorOptions,
) -> Result<(), StitchError> {
    if !(8..=256).contains(&options.tile_size) {
        return Err(options_error("tile_size must be in [8, 256]"));
    }
    if !(64..=20_000).contains(&options.max_features) {
        return Err(options_error("max_features must be in [64, 20000]"));
    }
    if !options.max_motion_ratio.is_finite() || !(0.0..=0.9).contains(&options.max_motion_ratio) {
        return Err(options_error(
            "max_motion_ratio must be finite and in [0, 0.9]",
        ));
    }
    if !options.min_confidence.is_finite() || !(0.0..=1.0).contains(&options.min_confidence) {
        return Err(options_error("min_confidence must be finite and in [0, 1]"));
    }
    if !options.temporal_learning_rate.is_finite()
        || !(0.0..=1.0).contains(&options.temporal_learning_rate)
        || options.temporal_learning_rate == 0.0
    {
        return Err(options_error(
            "temporal_learning_rate must be finite and in (0, 1]",
        ));
    }
    Ok(())
}

pub struct VerticalMotionEstimator {
    geometry: Geometry,
    axis: StitchAxis,
    options: MotionEstimatorOptions,
    layout: TileLayout,
    regions: TemporalRegionModel,
    scene_cut_streak: u8,
    parallel_work: bool,
    pyramid_plan: crate::orb::PyramidPlan,
    sampling: SamplingPlan,
    full_pyramid_plan: OnceLock<crate::orb::PyramidPlan>,
}

impl std::fmt::Debug for VerticalMotionEstimator {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("VerticalMotionEstimator")
            .field("geometry", &self.geometry)
            .field("axis", &self.axis)
            .field("options", &self.options)
            .field("regions", &self.regions.summary())
            .field("scene_cut_streak", &self.scene_cut_streak)
            .field("parallel_work", &self.parallel_work)
            .finish()
    }
}

impl VerticalMotionEstimator {
    pub fn new(geometry: Geometry, options: MotionEstimatorOptions) -> Result<Self, StitchError> {
        Self::new_for_axis(geometry, StitchAxis::Vertical, options)
    }

    pub fn new_for_axis(
        geometry: Geometry,
        axis: StitchAxis,
        options: MotionEstimatorOptions,
    ) -> Result<Self, StitchError> {
        validate_estimator_options(options)?;
        let layout = TileLayout::new(geometry.width, geometry.height, options.tile_size);
        let sampling = SamplingPlan::new(geometry, axis);
        let (width, height) = sampling.dimensions(geometry);
        let pyramid_plan = crate::orb::PyramidPlan::new(width, height);
        Ok(Self {
            geometry,
            axis,
            options,
            layout,
            regions: TemporalRegionModel::new(layout),
            scene_cut_streak: 0,
            parallel_work: std::thread::available_parallelism()
                .is_ok_and(|parallelism| parallelism.get() > 1),
            pyramid_plan,
            sampling,
            full_pyramid_plan: OnceLock::new(),
        })
    }

    pub fn region_summary(&self) -> RegionDiagnostics {
        self.regions.summary()
    }

    fn rejected_estimate(
        &mut self,
        scene_cut: bool,
        confidence: f32,
        mut diagnostics: MotionDiagnostics,
    ) -> MotionEstimate {
        if scene_cut {
            self.scene_cut_streak = self.scene_cut_streak.saturating_add(1);
        } else {
            self.scene_cut_streak = 0;
        }
        self.regions.decay_toward_neutral(0.05);
        if self.scene_cut_streak >= 3 {
            self.regions.reset();
            self.scene_cut_streak = 0;
            diagnostics.stage = MotionStage::SceneCut;
        }
        diagnostics.regions = self.regions.summary();
        MotionEstimate::indeterminate(confidence, diagnostics)
    }

    pub fn estimate(
        &mut self,
        motion_reference: &Frame,
        previous_raw: &Frame,
        incoming: &Frame,
    ) -> Result<MotionEstimate, StitchError> {
        for (name, frame) in [
            ("motion reference", motion_reference),
            ("previous raw frame", previous_raw),
            ("incoming frame", incoming),
        ] {
            if frame.geometry() != self.geometry {
                return Err(StitchError::InvalidFrame {
                    message: format!(
                        "{name} has geometry {}; estimator expects {}",
                        frame.geometry(),
                        self.geometry
                    ),
                });
            }
        }
        if self.geometry.width < 5 || self.geometry.height < 5 {
            return Ok(MotionEstimate::no_motion(
                1.0,
                MotionDiagnostics::at(MotionStage::InputTooSmall),
            ));
        }

        if motion_reference.visible_interior_pixels_equal(incoming) {
            self.scene_cut_streak = 0;
            let mut diagnostics = MotionDiagnostics::at(MotionStage::IdenticalInterior);
            diagnostics.direct_similarity = 1.0;
            diagnostics.regions = self.regions.summary();
            return Ok(MotionEstimate::no_motion(1.0, diagnostics));
        }

        let grayscale_perf = crate::perf::Scope::new(crate::perf::Stage::Grayscale);
        let reference_gray = GrayImage::from_frame(motion_reference)?;
        let previous_gray = GrayImage::from_frame(previous_raw)?;
        let incoming_gray = GrayImage::from_frame(incoming)?;
        grayscale_perf.finish();
        let similarity_perf = crate::perf::Scope::new(crate::perf::Stage::SimilarityMaps);
        let direct =
            SimilarityMap::between(&previous_gray, &incoming_gray, self.layout, self.axis, 0);
        let zero_alignment =
            SimilarityMap::between(&reference_gray, &incoming_gray, self.layout, self.axis, 0);
        similarity_perf.finish();

        let max_features = self.options.max_features as usize;
        let evidence = pure_rust_feature_evidence(
            motion_reference,
            incoming,
            self.layout,
            &self.regions,
            max_features,
            self.axis,
            self.parallel_work,
            &self.pyramid_plan,
            Some(&self.sampling),
        );
        let images = EvaluationImages {
            reference: motion_reference,
            incoming,
            reference_gray: &reference_gray,
            incoming_gray: &incoming_gray,
            direct: &direct,
            zero_alignment: &zero_alignment,
        };
        let mut evaluated = self.evaluate(&images, evidence);
        if self.sampling.reduced() && evaluated.needs_full_resolution() {
            let pyramid = self.full_pyramid_plan.get_or_init(|| {
                crate::orb::PyramidPlan::new(
                    self.geometry.width as usize,
                    self.geometry.height as usize,
                )
            });
            let evidence = pure_rust_feature_evidence(
                motion_reference,
                incoming,
                self.layout,
                &self.regions,
                max_features,
                self.axis,
                self.parallel_work,
                pyramid,
                None,
            );
            evaluated = self.evaluate(&images, evidence);
        }
        Ok(self.commit_evaluation(&direct, evaluated))
    }

    fn commit_evaluation(
        &mut self,
        direct: &SimilarityMap,
        evaluated: EvaluatedMotion,
    ) -> MotionEstimate {
        let EvaluatedMotion {
            mut estimate,
            compensated,
            scene_cut,
            ..
        } = evaluated;
        if let Some(selected_map) = compensated {
            let _perf = crate::perf::Scope::new(crate::perf::Stage::RegionUpdate);
            self.regions
                .update(direct, &selected_map, self.options.temporal_learning_rate);
            self.scene_cut_streak = 0;
            estimate.diagnostics.regions = self.regions.summary();
            estimate
        } else if matches!(estimate.outcome, crate::MotionOutcome::NoMotion) {
            self.scene_cut_streak = 0;
            estimate
        } else {
            self.rejected_estimate(scene_cut, estimate.confidence, estimate.diagnostics)
        }
    }

    fn evaluate(
        &self,
        images: &EvaluationImages<'_>,
        evidence: FeatureEvidence,
    ) -> EvaluatedMotion {
        let EvaluationImages {
            reference: motion_reference,
            incoming,
            reference_gray,
            incoming_gray,
            direct,
            zero_alignment,
        } = *images;
        let direct_similarity = direct.mean();
        let primary_extent = self
            .axis
            .primary_extent(self.geometry.width, self.geometry.height);
        let maximum_shift = (primary_extent as f32 * self.options.max_motion_ratio)
            .floor()
            .clamp(0.0, i32::MAX as f32) as i32;
        let mut diagnostics = MotionDiagnostics::at(MotionStage::EmptyDescriptors);
        diagnostics.reference_keypoints = evidence.reference_features;
        diagnostics.incoming_keypoints = evidence.incoming_features;
        diagnostics.direct_similarity = direct_similarity;
        diagnostics.regions = self.regions.summary();
        if evidence.observations.is_empty() {
            return EvaluatedMotion::rejected(direct_similarity < 0.6, 0.0, diagnostics);
        }

        let observations = evidence.observations;
        diagnostics.mutual_matches = observations.len() as u32;
        if observations.is_empty() {
            diagnostics.stage = MotionStage::NoMatches;
            return EvaluatedMotion::rejected(direct_similarity < 0.6, 0.0, diagnostics);
        }

        let scoring_perf = crate::perf::Scope::new(crate::perf::Stage::CandidateScoring);
        let candidates = candidate_offsets(&observations, self.axis, maximum_shift);
        if candidates.is_empty() {
            diagnostics.stage = MotionStage::NoCandidates;
            return EvaluatedMotion::rejected(direct_similarity < 0.6, 0.0, diagnostics);
        }

        let mut scored = candidates
            .into_iter()
            .map(|offset| {
                let compensated = if offset == 0 {
                    zero_alignment.clone()
                } else {
                    SimilarityMap::between(
                        reference_gray,
                        incoming_gray,
                        self.layout,
                        self.axis,
                        offset,
                    )
                };
                score_candidate(
                    offset,
                    self.axis,
                    &observations,
                    direct,
                    zero_alignment,
                    compensated,
                    &self.regions,
                )
            })
            .collect::<Vec<_>>();
        scored.sort_by(|left, right| {
            right
                .diagnostics
                .score
                .total_cmp(&left.diagnostics.score)
                .then_with(|| {
                    left.diagnostics
                        .offset
                        .abs()
                        .cmp(&right.diagnostics.offset.abs())
                })
                .then_with(|| left.diagnostics.offset.cmp(&right.diagnostics.offset))
        });

        scoring_perf.finish();
        let strongest_mode = scored[0].diagnostics.offset;
        if strongest_mode != 0 {
            let _perf = crate::perf::Scope::new(crate::perf::Stage::Refinement);
            let (refined_offset, precise_error) = refine_motion_offset(
                motion_reference,
                incoming,
                direct,
                self.axis,
                strongest_mode,
                maximum_shift,
            );
            let mut refined = if let Some(index) = scored
                .iter()
                .position(|candidate| candidate.diagnostics.offset == refined_offset)
            {
                scored.remove(index)
            } else {
                let compensated = SimilarityMap::between(
                    reference_gray,
                    incoming_gray,
                    self.layout,
                    self.axis,
                    refined_offset,
                );
                score_candidate(
                    refined_offset,
                    self.axis,
                    &observations,
                    direct,
                    zero_alignment,
                    compensated,
                    &self.regions,
                )
            };
            refined.diagnostics.precise_alignment_error = Some(precise_error);
            scored.insert(0, refined);
        }

        let best_score = scored[0].diagnostics.score;
        let second_score = scored
            .iter()
            .skip(1)
            .find(|candidate| {
                (candidate.diagnostics.offset - scored[0].diagnostics.offset).abs()
                    > INLIER_TOLERANCE
            })
            .map(|candidate| candidate.diagnostics.score)
            .unwrap_or(0.0);
        let margin = if best_score > f32::EPSILON {
            ((best_score - second_score) / best_score).clamp(0.0, 1.0)
        } else {
            0.0
        };
        let best = &scored[0].diagnostics;
        let confidence = (0.40 * best.weighted_inlier_share
            + 0.25 * best.spatial_coverage
            + 0.20 * best.residual_gain
            + 0.15 * margin)
            .clamp(0.0, 1.0);
        diagnostics.selected_offset = Some(best.offset);
        diagnostics.candidates = scored
            .iter()
            .map(|candidate| candidate.diagnostics.clone())
            .collect();

        if best.offset == 0 {
            diagnostics.stage = MotionStage::SelectedNoMotion;
            return EvaluatedMotion {
                estimate: MotionEstimate::no_motion(confidence, diagnostics),
                compensated: None,
                scene_cut: false,
                margin,
            };
        }

        let accepted = best.raw_inliers >= MIN_INLIER_MATCHES
            && best.inlier_tiles >= MIN_INLIER_TILES
            && best.residual_gain >= MIN_RESIDUAL_GAIN
            && confidence >= self.options.min_confidence;
        if accepted {
            let selected_offset = best.offset;
            let selected_map = scored.remove(0).compensated;
            diagnostics.stage = MotionStage::Selected;
            return EvaluatedMotion {
                estimate: MotionEstimate::motion(selected_offset, confidence, diagnostics),
                compensated: Some(selected_map),
                scene_cut: false,
                margin,
            };
        }

        let scene_cut = direct_similarity < 0.5
            && scored
                .iter()
                .all(|candidate| candidate.diagnostics.alignment_error > 0.6);
        diagnostics.stage = MotionStage::LowConfidence;
        EvaluatedMotion::rejected(scene_cut, confidence, diagnostics)
    }
}

#[cfg(test)]
#[path = "estimator_optimization_tests.rs"]
mod optimization_tests;

#[cfg(test)]
mod tests {
    use super::*;

    fn full_resolution_estimator(frame: &Frame, axis: StitchAxis) -> VerticalMotionEstimator {
        let mut estimator = VerticalMotionEstimator::new_for_axis(
            frame.geometry(),
            axis,
            MotionEstimatorOptions::default(),
        )
        .unwrap();
        estimator.sampling = SamplingPlan::full_resolution(frame.geometry(), axis);
        estimator.pyramid_plan =
            crate::orb::PyramidPlan::new(frame.width() as usize, frame.height() as usize);
        estimator
    }

    fn directional_fixture(
        axis: StitchAxis,
        cross: u32,
        primary: u32,
        scroll: u32,
        sparse: bool,
    ) -> Frame {
        let (width, height) = match axis {
            StitchAxis::Vertical => (cross, primary),
            StitchAxis::Horizontal => (primary, cross),
        };
        let pixels = (0..height)
            .flat_map(|y| {
                (0..width).map(move |x| {
                    let (cross, along) = match axis {
                        StitchAxis::Vertical => (x, y + scroll),
                        StitchAxis::Horizontal => (y, x + scroll),
                    };
                    if sparse && !(cross > 400 && cross < 560) {
                        return 255;
                    }
                    let mut hash =
                        cross.wrapping_mul(0xc2b2_ae35) ^ along.wrapping_mul(0x27d4_eb2d);
                    hash ^= hash >> 16;
                    hash = hash.wrapping_mul(0x7feb_352d);
                    hash ^= hash >> 15;
                    (hash >> 24) as u8
                })
            })
            .collect();
        Frame::new(width, height, PixelFormat::Gray8, pixels).unwrap()
    }

    #[test]
    fn directional_sampling_preserves_offsets_and_exact_stitched_pixels() {
        for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
            for cross in [320, 1024, 2048] {
                for shift in [1, 17] {
                    let first = directional_fixture(axis, cross, 256, 0, false);
                    let second = directional_fixture(axis, cross, 256, shift, false);
                    for reverse in [false, true] {
                        let (reference, incoming) = if reverse {
                            (&second, &first)
                        } else {
                            (&first, &second)
                        };
                        let mut estimator = VerticalMotionEstimator::new_for_axis(
                            reference.geometry(),
                            axis,
                            MotionEstimatorOptions::default(),
                        )
                        .unwrap();
                        let estimated = estimator.estimate(reference, reference, incoming).unwrap();
                        if shift == 1 {
                            let baseline = full_resolution_estimator(reference, axis)
                                .estimate(reference, reference, incoming)
                                .unwrap();
                            assert_eq!(
                                estimated.outcome, baseline.outcome,
                                "one-pixel compatibility: {axis:?}, cross={cross}"
                            );
                            continue;
                        }
                        let expected = if reverse {
                            shift as i32
                        } else {
                            -(shift as i32)
                        };
                        assert_eq!(
                            estimated.offset(),
                            Some(expected),
                            "{axis:?}, cross={cross}, shift={expected}: {estimated:#?}"
                        );
                        let stitched = crate::stitch(
                            &[reference.clone(), incoming.clone()],
                            crate::StitchOptions {
                                axis,
                                ..crate::StitchOptions::default()
                            },
                        )
                        .unwrap();
                        let document = directional_fixture(axis, cross, 256 + shift, 0, false);
                        assert_eq!(
                            stitched.image, document,
                            "{axis:?}, cross={cross}, shift={expected}"
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn narrow_content_retains_known_motion() {
        for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
            let first = directional_fixture(axis, 2048, 320, 0, true);
            let second = directional_fixture(axis, 2048, 320, 17, true);
            let mut estimator = VerticalMotionEstimator::new_for_axis(
                first.geometry(),
                axis,
                MotionEstimatorOptions::default(),
            )
            .unwrap();
            let result = estimator.estimate(&first, &first, &second).unwrap();
            assert_eq!(result.offset(), Some(-17), "{axis:?}: {result:#?}");
        }
    }

    #[test]
    fn large_fixed_overlays_and_repeated_rules_preserve_motion() {
        for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
            let make = |scroll| {
                let frame = directional_fixture(axis, 1024, 320, scroll, false);
                let fixed = directional_fixture(axis, 1024, 320, 0, false);
                let mut pixels = frame.pixels().to_vec();
                for y in 0..frame.height() {
                    for x in 0..frame.width() {
                        let (cross, primary) = match axis {
                            StitchAxis::Vertical => (x, y),
                            StitchAxis::Horizontal => (y, x),
                        };
                        let index = (y * frame.width() + x) as usize;
                        if cross < 128 || primary < 32 {
                            pixels[index] = fixed.pixels()[index];
                        } else if (primary + scroll) % 24 == 0 {
                            pixels[index] = 0;
                        }
                    }
                }
                Frame::new(frame.width(), frame.height(), PixelFormat::Gray8, pixels).unwrap()
            };
            let mut previous = make(0);
            let mut estimator = VerticalMotionEstimator::new_for_axis(
                previous.geometry(),
                axis,
                MotionEstimatorOptions::default(),
            )
            .unwrap();
            for step in 1..=4 {
                let incoming = make(step * 17);
                let estimate = estimator.estimate(&previous, &previous, &incoming).unwrap();
                assert_eq!(estimate.offset(), Some(-17), "{axis:?}: {estimate:#?}");
                previous = incoming;
            }
            assert!(estimator.region_summary().fixed_tiles > 0);
            assert!(estimator.region_summary().scrolling_tiles > 0);
        }
    }

    #[test]
    fn texture_too_narrow_for_refinement_uses_full_resolution_result() {
        for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
            let make = |scroll| {
                let frame = directional_fixture(axis, 2048, 320, scroll, false);
                let mut pixels = frame.pixels().to_vec();
                for y in 0..frame.height() {
                    for x in 0..frame.width() {
                        let cross = match axis {
                            StitchAxis::Vertical => x,
                            StitchAxis::Horizontal => y,
                        };
                        if !(420..500).contains(&cross) {
                            pixels[(y * frame.width() + x) as usize] = 255;
                        }
                    }
                }
                Frame::new(frame.width(), frame.height(), PixelFormat::Gray8, pixels).unwrap()
            };
            let reference = make(0);
            let incoming = make(17);
            let mut estimator = VerticalMotionEstimator::new_for_axis(
                reference.geometry(),
                axis,
                MotionEstimatorOptions::default(),
            )
            .unwrap();
            let mut full = full_resolution_estimator(&reference, axis);
            let actual = estimator
                .estimate(&reference, &reference, &incoming)
                .unwrap();
            assert_eq!(
                actual,
                full.estimate(&reference, &reference, &incoming).unwrap()
            );
            assert!(estimator.full_pyramid_plan.get().is_some());
            assert_eq!(
                format!("{:?}", estimator.regions),
                format!("{:?}", full.regions)
            );
        }
    }

    #[test]
    fn rejected_reduced_pass_retries_without_double_scene_cut_updates() {
        for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
            let first = directional_fixture(axis, 1024, 224, 0, false);
            let mut estimator = VerticalMotionEstimator::new_for_axis(
                first.geometry(),
                axis,
                MotionEstimatorOptions::default(),
            )
            .unwrap();
            let mut full = full_resolution_estimator(&first, axis);
            let mut previous = first;
            for seed in 1..=3 {
                // Flat, unrelated frames guarantee empty descriptors on both attempts.
                let incoming = Frame::new(
                    previous.width(),
                    previous.height(),
                    PixelFormat::Gray8,
                    vec![if seed % 2 == 0 { 255 } else { 0 }; previous.pixels().len()],
                )
                .unwrap();
                #[cfg(feature = "perf-instrumentation")]
                crate::perf::reset();
                let actual = estimator.estimate(&previous, &previous, &incoming).unwrap();
                #[cfg(feature = "perf-instrumentation")]
                assert_eq!(
                    crate::perf::snapshot().calls[crate::perf::Stage::FeatureExtraction as usize],
                    2
                );
                let expected = full.estimate(&previous, &previous, &incoming).unwrap();
                assert_eq!(actual, expected);
                assert_eq!(estimator.scene_cut_streak, full.scene_cut_streak);
                assert_eq!(
                    format!("{:?}", estimator.regions),
                    format!("{:?}", full.regions)
                );
                assert!(estimator.full_pyramid_plan.get().is_some());
                previous = incoming;
            }
        }
    }

    #[test]
    fn duplicates_bypass_sampling_and_no_motion_retries_once() {
        let first = directional_fixture(StitchAxis::Vertical, 1024, 256, 0, false);
        let mut estimator =
            VerticalMotionEstimator::new(first.geometry(), MotionEstimatorOptions::default())
                .unwrap();
        assert_eq!(
            estimator
                .estimate(&first, &first, &first)
                .unwrap()
                .diagnostics
                .stage,
            MotionStage::IdenticalInterior
        );
        assert!(estimator.full_pyramid_plan.get().is_none());
        let mut pixels = first.pixels().to_vec();
        for y in 80..100 {
            pixels[y * 1024 + 80..y * 1024 + 100].fill(0);
        }
        let incoming = Frame::new(1024, 256, PixelFormat::Gray8, pixels).unwrap();
        #[cfg(feature = "perf-instrumentation")]
        crate::perf::reset();
        let actual = estimator.estimate(&first, &first, &incoming).unwrap();
        #[cfg(feature = "perf-instrumentation")]
        assert_eq!(
            crate::perf::snapshot().calls[crate::perf::Stage::FeatureExtraction as usize],
            2
        );
        let expected = full_resolution_estimator(&first, StitchAxis::Vertical)
            .estimate(&first, &first, &incoming)
            .unwrap();
        assert_eq!(actual, expected);
        assert_eq!(actual.outcome, crate::MotionOutcome::NoMotion);
        assert!(estimator.full_pyramid_plan.get().is_some());
    }

    #[test]
    fn retry_requires_accepted_motion_with_positive_margin() {
        let frame = scrolling_fixture(0, 0);
        let gray = GrayImage::from_frame(&frame).unwrap();
        let layout = TileLayout::new(frame.width(), frame.height(), 32);
        let map = SimilarityMap::between(&gray, &gray, layout, StitchAxis::Vertical, 0);
        let mut evaluated = EvaluatedMotion {
            estimate: MotionEstimate::motion(
                -17,
                1.0,
                MotionDiagnostics::at(MotionStage::Selected),
            ),
            compensated: Some(map),
            scene_cut: false,
            margin: 0.0,
        };
        assert!(evaluated.needs_full_resolution());
        evaluated.margin = 0.1;
        assert!(evaluated.needs_full_resolution());
        evaluated
            .estimate
            .diagnostics
            .candidates
            .push(CandidateDiagnostics {
                offset: -17,
                raw_inliers: 20,
                inlier_tiles: 8,
                weighted_support: 8.0,
                weighted_inlier_share: 1.0,
                spatial_coverage: 1.0,
                alignment_error: 0.0,
                precise_alignment_error: Some(1.0),
                residual_gain: 1.0,
                score: 1.0,
            });
        assert!(evaluated.needs_full_resolution());
        evaluated.estimate.diagnostics.candidates[0].precise_alignment_error = Some(0.0);
        assert!(!evaluated.needs_full_resolution());
        evaluated.compensated = None;
        assert!(evaluated.needs_full_resolution());
    }

    #[cfg(feature = "perf-instrumentation")]
    #[test]
    #[ignore = "Release-only matcher benchmark using the local scrolling image fixture"]
    fn directional_sampling_release_benchmark() {
        if cfg!(debug_assertions) {
            panic!("use the windows-msvc-performance Release environment");
        }
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../snow_shot/test-imgs/scrollscreenshot-test.png");
        let source = Frame::decode(path).unwrap();
        let frames: Vec<_> = (0..=12)
            .map(|step| source.crop(0, step * 25, source.width(), 1600).unwrap())
            .collect();
        let mut changed_pixels = frames[0].pixels().to_vec();
        let channels = frames[0].pixel_format().channels() as usize;
        for y in 80..100 {
            let start = (y * frames[0].width() as usize + 80) * channels;
            changed_pixels[start..start + 20 * channels].fill(0);
        }
        let changed = Frame::new(
            frames[0].width(),
            frames[0].height(),
            frames[0].pixel_format(),
            changed_pixels,
        )
        .unwrap();
        let mut reports = Vec::new();
        // Alternate execution order, with an unreported warmup for each mode.
        for round in 0..4 {
            for adaptive in if round % 2 == 0 {
                [false, true]
            } else {
                [true, false]
            } {
                let mut estimator = if adaptive {
                    VerticalMotionEstimator::new(
                        frames[0].geometry(),
                        MotionEstimatorOptions::default(),
                    )
                    .unwrap()
                } else {
                    full_resolution_estimator(&frames[0], StitchAxis::Vertical)
                };
                let mut samples = Vec::new();
                for pair in frames.windows(2) {
                    crate::perf::reset();
                    let started = std::time::Instant::now();
                    let result = estimator.estimate(&pair[0], &pair[0], &pair[1]).unwrap();
                    let elapsed = started.elapsed().as_nanos() as u64;
                    let perf = crate::perf::snapshot();
                    assert_eq!(
                        result.offset(),
                        Some(-25),
                        "adaptive={adaptive}, round={round}: {result:#?}"
                    );
                    samples.push(serde_json::json!({
                        "estimate_ns": elapsed,
                        "feature_ns": perf.elapsed_ns[crate::perf::Stage::FeatureExtraction as usize],
                        "attempts": perf.calls[crate::perf::Stage::FeatureExtraction as usize],
                        "offset": result.offset(),
                    }));
                }
                if round > 0 {
                    reports.push(serde_json::json!({"workload": "scroll", "round": round, "adaptive": adaptive, "samples": samples}));
                }
                let mut fallback_samples = Vec::new();
                for _ in 0..4 {
                    crate::perf::reset();
                    let started = std::time::Instant::now();
                    let result = estimator
                        .estimate(&frames[0], &frames[0], &changed)
                        .unwrap();
                    let elapsed = started.elapsed().as_nanos() as u64;
                    let perf = crate::perf::snapshot();
                    assert_eq!(result.outcome, crate::MotionOutcome::NoMotion);
                    assert_eq!(
                        perf.calls[crate::perf::Stage::FeatureExtraction as usize],
                        if adaptive { 2 } else { 1 }
                    );
                    fallback_samples.push(serde_json::json!({
                        "estimate_ns": elapsed,
                        "feature_ns": perf.elapsed_ns[crate::perf::Stage::FeatureExtraction as usize],
                        "attempts": perf.calls[crate::perf::Stage::FeatureExtraction as usize],
                    }));
                }
                if round > 0 {
                    reports.push(serde_json::json!({"workload": "no-motion", "round": round, "adaptive": adaptive, "samples": fallback_samples}));
                }
            }
        }
        let output = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../build/windows-msvc-performance/directional-matcher-results.json");
        std::fs::write(&output, serde_json::to_vec_pretty(&reports).unwrap()).unwrap();
        eprintln!("matcher benchmark report: {}", output.display());
    }

    fn observation(tile: usize, distance: f32, dx: i32, dy: i32) -> MatchObservation {
        MatchObservation {
            distance,
            reference_x: tile as f32 * 32.0 + 8.0,
            reference_y: 8.0,
            incoming_x: tile as f32 * 32.0 + 8.0,
            incoming_y: 8.0 + dy as f32,
            reference_tile: tile,
            dx,
            dy,
        }
    }

    #[test]
    fn candidate_generation_clusters_neighboring_offsets() {
        let observations = vec![
            observation(0, 10.0, 0, -20),
            observation(1, 10.0, 0, -21),
            observation(2, 10.0, 0, -19),
            observation(3, 10.0, 0, 0),
        ];
        let candidates = candidate_offsets(&observations, StitchAxis::Vertical, 100);
        assert_eq!(candidates[0], 0);
        assert!(
            candidates
                .iter()
                .any(|candidate| (-22..=-18).contains(candidate))
        );
        assert_eq!(
            candidates
                .iter()
                .filter(|candidate| (-22..=-18).contains(*candidate))
                .count(),
            1
        );
    }

    #[test]
    fn invalid_options_are_rejected() {
        for options in [
            MotionEstimatorOptions {
                tile_size: 0,
                ..MotionEstimatorOptions::default()
            },
            MotionEstimatorOptions {
                max_features: 10,
                ..MotionEstimatorOptions::default()
            },
            MotionEstimatorOptions {
                min_confidence: f32::NAN,
                ..MotionEstimatorOptions::default()
            },
            MotionEstimatorOptions {
                temporal_learning_rate: 0.0,
                ..MotionEstimatorOptions::default()
            },
        ] {
            assert!(validate_estimator_options(options).is_err());
        }
    }

    #[test]
    fn coordinate_rounding_happens_before_subtraction() {
        assert_eq!(round_coordinate(1.49) - round_coordinate(0.51), 0);
        assert_eq!(round_coordinate(1.5) - round_coordinate(0.49), 2);
    }

    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    #[test]
    fn popcnt_matcher_matches_portable_results() {
        if !std::arch::is_x86_feature_detected!("popcnt") {
            return;
        }
        let feature = |seed: u8| BinaryFeature {
            x: 0.0,
            y: 0.0,
            descriptor: std::array::from_fn(|index| {
                seed.wrapping_mul(37)
                    .wrapping_add((index as u8).wrapping_mul(13))
            }),
        };
        let queries = [feature(1), feature(7), feature(19)];
        let train = [feature(2), feature(6), feature(8), feature(18), feature(20)];
        let portable = match_descriptors_portable(&queries, &train);
        let accelerated = unsafe { match_descriptors_popcnt(&queries, &train) };
        assert_eq!(accelerated, portable);
    }

    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    #[test]
    fn avx2_matcher_matches_portable_results() {
        if !std::arch::is_x86_feature_detected!("avx2") {
            return;
        }
        let feature = |seed: u8| BinaryFeature {
            x: 0.0,
            y: 0.0,
            descriptor: std::array::from_fn(|index| {
                seed.wrapping_mul(37)
                    .wrapping_add((index as u8).wrapping_mul(13))
            }),
        };
        let queries = [feature(1), feature(7), feature(19)];
        let train = [feature(2), feature(6), feature(8), feature(18), feature(20)];
        let portable = match_descriptors_portable(&queries, &train);
        let accelerated = unsafe { match_descriptors_avx2(&queries, &train) };
        assert_eq!(accelerated, portable);
    }

    fn scrolling_fixture(scroll: u32, animation: u8) -> Frame {
        const WIDTH: u32 = 320;
        const HEIGHT: u32 = 224;
        const TOP: u32 = 40;
        const BOTTOM: u32 = 32;
        const SIDEBAR: u32 = 64;
        let mut pixels = Vec::with_capacity((WIDTH * HEIGHT) as usize);
        for y in 0..HEIGHT {
            for x in 0..WIDTH {
                let fixed = !(TOP..HEIGHT - BOTTOM).contains(&y) || x < SIDEBAR;
                let source_y = if fixed { y } else { y - TOP + scroll };
                let mut hash = if fixed {
                    x.wrapping_mul(0x9e37_79b9) ^ y.wrapping_mul(0x85eb_ca6b)
                } else {
                    x.wrapping_mul(0xc2b2_ae35) ^ source_y.wrapping_mul(0x27d4_eb2d)
                };
                hash ^= hash >> 16;
                hash = hash.wrapping_mul(0x7feb_352d);
                hash ^= hash >> 15;
                let mut value = (hash >> 24) as u8;
                if (192..320).contains(&x) && (64..160).contains(&y) {
                    let mut dynamic_hash = x.wrapping_mul(0x1656_67b1)
                        ^ y.wrapping_mul(0xd3a2_646c)
                        ^ u32::from(animation).wrapping_mul(0xfd70_46c5);
                    dynamic_hash ^= dynamic_hash >> 16;
                    dynamic_hash = dynamic_hash.wrapping_mul(0x7feb_352d);
                    value = (dynamic_hash >> 24) as u8;
                }
                pixels.push(value);
            }
        }
        Frame::new(WIDTH, HEIGHT, PixelFormat::Gray8, pixels).unwrap()
    }

    fn horizontal_scrolling_fixture(scroll: u32) -> Frame {
        const WIDTH: u32 = 224;
        const HEIGHT: u32 = 192;
        const LEFT: u32 = 32;
        const RIGHT: u32 = 24;
        const TOP: u32 = 32;
        let mut pixels = Vec::with_capacity((WIDTH * HEIGHT) as usize);
        for y in 0..HEIGHT {
            for x in 0..WIDTH {
                let fixed = !(LEFT..WIDTH - RIGHT).contains(&x) || y < TOP;
                let source_x = if fixed { x } else { x - LEFT + scroll };
                let mut hash = if fixed {
                    x.wrapping_mul(0x9e37_79b9) ^ y.wrapping_mul(0x85eb_ca6b)
                } else {
                    source_x.wrapping_mul(0xc2b2_ae35) ^ y.wrapping_mul(0x27d4_eb2d)
                };
                hash ^= hash >> 16;
                hash = hash.wrapping_mul(0x7feb_352d);
                hash ^= hash >> 15;
                pixels.push((hash >> 24) as u8);
            }
        }
        Frame::new(WIDTH, HEIGHT, PixelFormat::Gray8, pixels).unwrap()
    }

    #[test]
    fn robust_estimator_recovers_scroll_among_fixed_and_dynamic_regions() {
        let reference = scrolling_fixture(0, 0);
        let incoming = scrolling_fixture(24, 1);
        let mut estimator =
            VerticalMotionEstimator::new(reference.geometry(), MotionEstimatorOptions::default())
                .unwrap();
        let estimate = estimator
            .estimate(&reference, &reference, &incoming)
            .unwrap();
        assert_eq!(
            estimate.outcome,
            crate::MotionOutcome::Motion { offset: -24 },
            "{estimate:#?}"
        );
        assert!(estimate.confidence >= MotionEstimatorOptions::default().min_confidence);
        assert!(
            estimate
                .diagnostics
                .candidates
                .iter()
                .find(|candidate| candidate.offset == -24)
                .is_some_and(|candidate| candidate.inlier_tiles >= MIN_INLIER_TILES)
        );
    }

    #[test]
    fn robust_estimator_recovers_reverse_scroll() {
        let reference = scrolling_fixture(24, 0);
        let incoming = scrolling_fixture(0, 1);
        let mut estimator =
            VerticalMotionEstimator::new(reference.geometry(), MotionEstimatorOptions::default())
                .unwrap();
        let estimate = estimator
            .estimate(&reference, &reference, &incoming)
            .unwrap();
        assert_eq!(
            estimate.outcome,
            crate::MotionOutcome::Motion { offset: 24 },
            "{estimate:#?}"
        );
    }

    #[test]
    fn horizontal_estimator_recovers_both_scroll_directions() {
        let left = horizontal_scrolling_fixture(0);
        let right = horizontal_scrolling_fixture(24);
        let mut estimator = VerticalMotionEstimator::new_for_axis(
            left.geometry(),
            StitchAxis::Horizontal,
            MotionEstimatorOptions::default(),
        )
        .unwrap();
        assert_eq!(
            estimator.estimate(&left, &left, &right).unwrap().outcome,
            crate::MotionOutcome::Motion { offset: -24 }
        );

        let mut estimator = VerticalMotionEstimator::new_for_axis(
            left.geometry(),
            StitchAxis::Horizontal,
            MotionEstimatorOptions::default(),
        )
        .unwrap();
        assert_eq!(
            estimator.estimate(&right, &right, &left).unwrap().outcome,
            crate::MotionOutcome::Motion { offset: 24 }
        );
    }

    #[test]
    fn animation_without_scroll_never_creates_false_motion() {
        let reference = scrolling_fixture(0, 0);
        let incoming = scrolling_fixture(0, 1);
        let mut estimator =
            VerticalMotionEstimator::new(reference.geometry(), MotionEstimatorOptions::default())
                .unwrap();
        let estimate = estimator
            .estimate(&reference, &reference, &incoming)
            .unwrap();
        assert!(!matches!(
            estimate.outcome,
            crate::MotionOutcome::Motion { .. }
        ));
    }

    #[test]
    fn temporal_model_learns_after_repeated_accepted_scrolls() {
        let mut previous = scrolling_fixture(0, 0);
        let mut estimator =
            VerticalMotionEstimator::new(previous.geometry(), MotionEstimatorOptions::default())
                .unwrap();
        for step in 1..=4 {
            let incoming = scrolling_fixture(step * 16, step as u8);
            let estimate = estimator.estimate(&previous, &previous, &incoming).unwrap();
            assert_eq!(
                estimate.outcome,
                crate::MotionOutcome::Motion { offset: -16 },
                "{estimate:#?}"
            );
            previous = incoming;
        }
        let summary = estimator.region_summary();
        assert!(summary.fixed_tiles > 0, "{summary:?}");
        assert!(summary.scrolling_tiles > 0, "{summary:?}");
        assert!(summary.dynamic_tiles > 0, "{summary:?}");
    }

    fn unrelated_fixture(seed: u32) -> Frame {
        let mut pixels = Vec::with_capacity(320 * 224);
        for y in 0_u32..224 {
            for x in 0_u32..320 {
                let mut hash = x.wrapping_mul(0x9e37_79b9)
                    ^ y.wrapping_mul(0x85eb_ca6b)
                    ^ seed.wrapping_mul(0xc2b2_ae35);
                hash ^= hash >> 16;
                hash = hash.wrapping_mul(0x7feb_352d);
                pixels.push((hash >> 24) as u8);
            }
        }
        Frame::new(320, 224, PixelFormat::Gray8, pixels).unwrap()
    }

    #[test]
    fn repeated_scene_cuts_reset_temporal_state() {
        let mut previous = unrelated_fixture(0);
        let mut estimator =
            VerticalMotionEstimator::new(previous.geometry(), MotionEstimatorOptions::default())
                .unwrap();
        let mut last = None;
        for seed in 1..=3 {
            let incoming = unrelated_fixture(seed);
            let estimate = estimator.estimate(&previous, &previous, &incoming).unwrap();
            assert!(!matches!(
                estimate.outcome,
                crate::MotionOutcome::Motion { .. }
            ));
            previous = incoming;
            last = Some(estimate);
        }
        let last = last.unwrap();
        assert_eq!(last.diagnostics.stage, MotionStage::SceneCut, "{last:#?}");
        let summary = estimator.region_summary();
        assert_eq!(summary.neutral_tiles as usize, estimator.layout.len());
    }
}

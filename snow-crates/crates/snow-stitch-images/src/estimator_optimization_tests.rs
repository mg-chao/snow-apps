use super::*;
use std::{hint::black_box, time::Instant};

fn original_refinement(
    reference: &Frame,
    incoming: &Frame,
    direct: &SimilarityMap,
    axis: StitchAxis,
    strongest: i32,
    maximum: i32,
) -> (i32, f32) {
    (strongest - INLIER_TOLERANCE..=strongest + INLIER_TOLERANCE)
        .into_par_iter()
        .filter(|offset| offset.abs() <= maximum)
        .map(|offset| {
            (
                offset,
                precise_alignment_error(reference, incoming, direct, axis, offset),
            )
        })
        .min_by(|left, right| {
            left.1
                .total_cmp(&right.1)
                .then_with(|| (left.0 - strongest).abs().cmp(&(right.0 - strongest).abs()))
                .then_with(|| left.0.cmp(&right.0))
        })
        .unwrap()
}

fn direct_map(reference: &Frame, incoming: &Frame, axis: StitchAxis) -> SimilarityMap {
    SimilarityMap::between(
        &GrayImage::from_frame(reference).unwrap(),
        &GrayImage::from_frame(incoming).unwrap(),
        TileLayout::new(incoming.width(), incoming.height(), 32),
        axis,
        0,
    )
}

fn texture(width: u32, height: u32, format: PixelFormat, phase: u32) -> Frame {
    let mut pixels = Vec::new();
    for y in 0..height {
        for x in 0..width {
            for channel in 0..format.channels() {
                let mut value = x.wrapping_mul(0xc2b2_ae35)
                    ^ y.wrapping_mul(0x27d4_eb2d)
                    ^ channel.wrapping_mul(0x1656_67b1);
                // Retain some fixed content as well as changing textured regions.
                if x > width / 3 {
                    value ^= phase.wrapping_mul(0x85eb_ca77);
                }
                value ^= value >> 16;
                value = value.wrapping_mul(0x7feb_352d);
                pixels.push((value >> 24) as u8);
            }
        }
    }
    Frame::new(width, height, format, pixels).unwrap()
}

#[test]
fn shared_refinement_matches_independent_errors_and_ties() {
    for format in [PixelFormat::Gray8, PixelFormat::Rgb8, PixelFormat::Rgba8] {
        for (width, height) in [(1, 1), (2, 3), (5, 7), (97, 69), (129, 193)] {
            let reference = texture(width, height, format, 0);
            for phase in [0, 1] {
                let incoming = texture(width, height, format, phase);
                for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
                    let direct = direct_map(&reference, &incoming, axis);
                    for (strongest, maximum) in [
                        (-99, 100),
                        (-10, 10),
                        (-1, 10),
                        (0, 0),
                        (1, 10),
                        (10, 10),
                        (99, 100),
                    ] {
                        for (offset, error) in precise_alignment_errors(
                            &reference, &incoming, &direct, axis, strongest, maximum,
                        ) {
                            if offset.abs() <= maximum {
                                assert_eq!(
                                    error,
                                    precise_alignment_error(
                                        &reference, &incoming, &direct, axis, offset
                                    ),
                                    "{width}x{height} {format:?} {axis:?} phase={phase} offset={offset}",
                                );
                            }
                        }
                        assert_eq!(
                            refine_motion_offset(
                                &reference, &incoming, &direct, axis, strongest, maximum
                            ),
                            original_refinement(
                                &reference, &incoming, &direct, axis, strongest, maximum
                            ),
                        );
                    }
                }
            }
        }
    }
}

#[test]
fn refinement_dispatch_boundaries_preserve_results() {
    for (format, side) in [(PixelFormat::Rgba8, 1024), (PixelFormat::Gray8, 2048)] {
        for (width, height) in [(side - 1, side + 1), (side, side)] {
            let reference = texture(width, height, format, 0);
            let incoming = texture(width, height, format, 1);
            for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
                let direct = direct_map(&reference, &incoming, axis);
                for strongest in [-17, 17] {
                    assert_eq!(
                        refine_motion_offset(&reference, &incoming, &direct, axis, strongest, 900),
                        original_refinement(&reference, &incoming, &direct, axis, strongest, 900),
                        "{width}x{height} {format:?} {axis:?}",
                    );
                }
            }
        }
    }
}

fn benchmark_frame(rgb: &Frame, format: PixelFormat, axis: StitchAxis) -> Frame {
    let (width, height) = match axis {
        StitchAxis::Vertical => (rgb.width(), rgb.height()),
        StitchAxis::Horizontal => (rgb.height(), rgb.width()),
    };
    let mut pixels = Vec::with_capacity((width * height * format.channels()) as usize);
    for y in 0..height {
        for x in 0..width {
            let (source_x, source_y) = match axis {
                StitchAxis::Vertical => (x, y),
                StitchAxis::Horizontal => (y, x),
            };
            let start = ((source_y * rgb.width() + source_x) * 3) as usize;
            match format {
                PixelFormat::Gray8 => pixels.push(luminance_at(rgb, source_x, source_y)),
                PixelFormat::Rgb8 => pixels.extend_from_slice(&rgb.pixels()[start..start + 3]),
                PixelFormat::Rgba8 => {
                    pixels.extend_from_slice(&rgb.pixels()[start..start + 3]);
                    pixels.push(255);
                }
            }
        }
    }
    Frame::new(width, height, format, pixels).unwrap()
}

fn timing_summary(samples: &[u64]) -> serde_json::Value {
    let mut samples = samples.to_vec();
    samples.sort_unstable();
    serde_json::json!({
        "count": samples.len(),
        "p50_ns": samples[(samples.len() * 50).div_ceil(100) - 1],
        "p95_ns": samples[(samples.len() * 95).div_ceil(100) - 1],
        "mean_ns": samples.iter().sum::<u64>() / samples.len() as u64,
    })
}

#[test]
#[ignore = "Release-only paired algorithm benchmark using the local scrolling image fixture"]
fn refinement_release_benchmark() {
    if cfg!(debug_assertions) {
        panic!("use windows-msvc-performance Release");
    }
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..");
    let source = Frame::decode(root.join("snow_shot/test-imgs/scrollscreenshot-test.png")).unwrap();
    let mut cases = Vec::new();
    for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
        for format in [PixelFormat::Gray8, PixelFormat::Rgb8, PixelFormat::Rgba8] {
            for (top, crop_width, crop_height) in [
                (0, 320, 240),
                (8_000, 800, 600),
                (0, 1280, 720),
                (8_000, 1920, 1080),
                (0, source.width(), 1600),
                (8_000, source.width(), 1600),
                (18_000, source.width(), 1600),
            ] {
                let left = (source.width() - crop_width) / 2;
                let first = benchmark_frame(
                    &source.crop(left, top, crop_width, crop_height).unwrap(),
                    format,
                    axis,
                );
                let second = benchmark_frame(
                    &source
                        .crop(left, top + 75, crop_width, crop_height)
                        .unwrap(),
                    format,
                    axis,
                );
                for reverse in [false, true] {
                    let (reference, incoming, strongest) = if reverse {
                        (&second, &first, 75)
                    } else {
                        (&first, &second, -75)
                    };
                    let direct = direct_map(reference, incoming, axis);
                    let expected =
                        original_refinement(reference, incoming, &direct, axis, strongest, 1200);
                    if crop_width == source.width() {
                        assert_eq!(expected, (strongest, 0.0));
                    }
                    let mut samples = [Vec::new(), Vec::new()];
                    // Warm both paths, then alternate their order on identical inputs.
                    for round in 0..11 {
                        for shared in if round % 2 == 0 {
                            [false, true]
                        } else {
                            [true, false]
                        } {
                            let started = Instant::now();
                            let actual = black_box(if shared {
                                refine_motion_offset(
                                    black_box(reference),
                                    black_box(incoming),
                                    black_box(&direct),
                                    axis,
                                    strongest,
                                    1200,
                                )
                            } else {
                                original_refinement(
                                    black_box(reference),
                                    black_box(incoming),
                                    black_box(&direct),
                                    axis,
                                    strongest,
                                    1200,
                                )
                            });
                            let elapsed = started.elapsed().as_nanos() as u64;
                            assert_eq!(actual, expected);
                            if round > 0 {
                                samples[usize::from(shared)].push(elapsed);
                            }
                        }
                    }
                    let original = timing_summary(&samples[0]);
                    let shared = timing_summary(&samples[1]);
                    eprintln!(
                        "{axis:?} {format:?} {crop_width}x{crop_height} top={top} reverse={reverse}: original={} ns shared={} ns",
                        original["p50_ns"], shared["p50_ns"]
                    );
                    cases.push(serde_json::json!({
                        "axis": axis, "format": format, "top": top, "reverse": reverse,
                        "width": first.width(), "height": first.height(),
                        "original": original, "shared": shared, "samples_ns": samples,
                    }));
                }
            }
        }
    }
    let output = std::env::var_os("SNOW_SCROLLING_ALGORITHM_PERF_OUTPUT")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| root.join("build/windows-msvc-performance/refinement-results.json"));
    std::fs::write(&output, serde_json::to_vec_pretty(&cases).unwrap()).unwrap();
    eprintln!("refinement report: {}", output.display());
}

fn assert_same_map(left: &SimilarityMap, right: &SimilarityMap) {
    assert_eq!(left.layout(), right.layout());
    for tile in 0..left.layout().len() {
        assert_eq!(left.at_tile(tile), right.at_tile(tile));
        assert_eq!(left.texture_at(tile), right.texture_at(tile));
    }
}

#[test]
#[ignore = "Release-only paired zero-candidate benchmark using the local scrolling image fixture"]
fn zero_candidate_release_benchmark() {
    if cfg!(debug_assertions) {
        panic!("use windows-msvc-performance Release");
    }
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..");
    let source = Frame::decode(root.join("snow_shot/test-imgs/scrollscreenshot-test.png")).unwrap();
    let mut cases = Vec::new();
    for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
        for (width, height) in [(320, 240), (800, 600), (3840, 1600)] {
            for top in [0, 8_000, 18_000] {
                let left = (source.width() - width) / 2;
                let reference = benchmark_frame(
                    &source.crop(left, top, width, height).unwrap(),
                    PixelFormat::Rgba8,
                    axis,
                );
                let incoming = benchmark_frame(
                    &source.crop(left, top + 75, width, height).unwrap(),
                    PixelFormat::Rgba8,
                    axis,
                );
                let layout = TileLayout::new(reference.width(), reference.height(), 32);
                let reference_gray = GrayImage::from_frame(&reference).unwrap();
                let incoming_gray = GrayImage::from_frame(&incoming).unwrap();
                let zero = SimilarityMap::between(&reference_gray, &incoming_gray, layout, axis, 0);
                let regions = TemporalRegionModel::new(layout);
                let mut observations = Vec::new();
                for y in (1..reference.height()).step_by(32) {
                    for x in (1..reference.width()).step_by(32) {
                        observations.push(MatchObservation {
                            distance: 10.0,
                            reference_x: x as f32,
                            reference_y: y as f32,
                            incoming_x: x as f32,
                            incoming_y: y as f32,
                            reference_tile: layout.index(x as f32, y as f32).unwrap(),
                            dx: 0,
                            dy: 0,
                        });
                    }
                }
                let expected =
                    score_candidate(0, axis, &observations, &zero, &zero, zero.clone(), &regions);
                let mut samples = [Vec::new(), Vec::new()];
                for round in 0..21 {
                    for reuse in if round % 2 == 0 {
                        [false, true]
                    } else {
                        [true, false]
                    } {
                        let started = Instant::now();
                        let map = if reuse {
                            black_box(&zero).clone()
                        } else {
                            SimilarityMap::between(
                                black_box(&reference_gray),
                                black_box(&incoming_gray),
                                layout,
                                axis,
                                0,
                            )
                        };
                        let actual = black_box(score_candidate(
                            0,
                            axis,
                            black_box(&observations),
                            &zero,
                            &zero,
                            map,
                            &regions,
                        ));
                        let elapsed = started.elapsed().as_nanos() as u64;
                        assert_eq!(actual.diagnostics, expected.diagnostics);
                        assert_same_map(&actual.compensated, &expected.compensated);
                        if round > 0 {
                            samples[usize::from(reuse)].push(elapsed);
                        }
                    }
                }
                let original = timing_summary(&samples[0]);
                let reused = timing_summary(&samples[1]);
                eprintln!(
                    "zero candidate {axis:?} {width}x{height} top={top}: original={} ns reused={} ns",
                    original["p50_ns"], reused["p50_ns"]
                );
                cases.push(serde_json::json!({
                    "axis": axis, "width": width, "height": height, "top": top,
                    "original": original, "reused": reused, "samples_ns": samples,
                }));
            }
        }
    }
    let output = std::env::var_os("SNOW_SCROLLING_ALGORITHM_PERF_OUTPUT")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| root.join("build/windows-msvc-performance/zero-candidate-results.json"));
    std::fs::write(&output, serde_json::to_vec_pretty(&cases).unwrap()).unwrap();
    eprintln!("zero-candidate report: {}", output.display());
}

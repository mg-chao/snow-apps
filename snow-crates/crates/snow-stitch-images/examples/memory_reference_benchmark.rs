//! Repeated production motion-reference preparation while scrolling within an
//! existing canvas. Input cloning remains in total latency; additional sample
//! series expose each nonzero instrumented scope, including reference preparation.
//! Scopes are inclusive wall times; overlapping scopes must not be summed.
//!
//! `cargo run --release -p snow-stitch-images --no-default-features
//! --features perf-instrumentation --example memory_reference_benchmark --
//! stitch-reference-vertical 31`
//! Also supports stitch-reference-horizontal, stitch-reference-vertical-4k and
//! stitch-reference-horizontal-4k. The 4K cases exercise a 31.6 MiB comparison
//! viewport. Copy this same source and example declaration to the comparison
//! checkout and enable its existing perf feature.

#[path = "../../../benchmark-support/memory.rs"]
mod memory;

use snow_stitch_images::{
    Frame, PixelFormat, ReferenceMode, StitchAxis, StitchBranch, StitchOptions, Stitcher, perf,
};

fn texture(width: u32, height: u32, position: u32, axis: StitchAxis) -> Frame {
    let mut pixels = Vec::with_capacity(width as usize * height as usize * 4);
    for y in 0..height {
        for x in 0..width {
            let (cross, primary) = match axis {
                StitchAxis::Vertical => (x, y + position),
                StitchAxis::Horizontal => (y, x + position),
            };
            let mut value =
                (cross / 4).wrapping_mul(0xc2b2_ae35) ^ (primary / 4).wrapping_mul(0x27d4_eb2d);
            value ^= value >> 16;
            value = value.wrapping_mul(0x7feb_352d);
            let gray = (value >> 24) as u8;
            pixels.extend_from_slice(&[gray, gray, gray, 255]);
        }
    }
    Frame::new(width, height, PixelFormat::Rgba8, pixels).unwrap()
}

fn full_checksum(bytes: &[u8]) -> u64 {
    // Full output verification occurs outside all latency samples.
    bytes.iter().fold(bytes.len() as u64, |hash, byte| {
        hash.wrapping_mul(0x100_0000_01b3) ^ u64::from(*byte)
    })
}

fn main() {
    let (scenario, repetitions) = memory::arguments("stitch-reference-vertical");
    let (width, height, axis) = match scenario.as_str() {
        "stitch-reference-vertical" => (1024, 512, StitchAxis::Vertical),
        "stitch-reference-horizontal" => (512, 1024, StitchAxis::Horizontal),
        "stitch-reference-vertical-4k" => (3840, 2160, StitchAxis::Vertical),
        "stitch-reference-horizontal-4k" => (2160, 3840, StitchAxis::Horizontal),
        _ => panic!("unknown reference scenario: {scenario}"),
    };
    const CONTAINED_POSITIONS: [u32; 4] = [32, 64, 48, 80];
    assert!(axis.primary_extent(width, height) > 128);
    assert!(CONTAINED_POSITIONS.iter().all(|position| *position < 128));
    memory::phase(&scenario, "empty", width, height, 0);
    let mut stitcher = Stitcher::new(StitchOptions {
        axis,
        record_decisions: true,
        ..StitchOptions::default()
    })
    .unwrap();
    stitcher.push(texture(width, height, 0, axis)).unwrap();
    let boundary = stitcher
        .push(texture(width, height, 128, axis))
        .unwrap()
        .unwrap();
    assert_eq!(boundary.branch, StitchBranch::Append);
    assert!(
        boundary
            .accepted_offset
            .is_some_and(|offset| (offset + 128).abs() <= 2)
    );
    let completed_dimensions = stitcher.image_dimensions().unwrap();
    let frames: Vec<_> = CONTAINED_POSITIONS
        .into_iter()
        .map(|position| texture(width, height, position, axis))
        .collect();
    // Establish contained motion before the measured three warm-up pushes.
    // All measured positions remain well inside the completed canvas; samples
    // therefore exercise repeated materialization without canvas growth.
    for frame in &frames {
        let decision = stitcher.push(frame.clone()).unwrap().unwrap();
        assert_eq!(decision.branch, StitchBranch::Contained);
        assert_eq!(stitcher.image_dimensions(), Some(completed_dimensions));
    }
    stitcher.clear_decisions();
    let expected_output = stitcher.image().unwrap();
    let output_checksum = full_checksum(expected_output.pixels())
        .wrapping_add(memory::checksum(expected_output.pixels()));
    let canvas_dimensions = stitcher.image_dimensions().unwrap();
    let viewport_bytes = width as usize * height as usize * 4;
    // Four input fixtures, two live viewport frames, the live tiled canvas,
    // and the independent retained validation output. This shared logical count
    // deliberately excludes implementation scratch: the private comparison
    // cache (one 31.6 MiB viewport at 4K), estimator internals and spare tiles are
    // observed by RSS/allocator metrics rather than hidden in a fixed estimate.
    let logical_bytes = viewport_bytes * (frames.len() + 2) + expected_output.pixels().len() * 2;
    memory::phase(&scenario, "setup", width, height, logical_bytes);
    let mut scopes = Vec::with_capacity(repetitions + 3);
    let mut next = 0;
    memory::measure(&scenario, width, height, repetitions, logical_bytes, || {
        perf::reset();
        let decision = stitcher
            .push(frames[next % frames.len()].clone())
            .unwrap()
            .unwrap();
        next += 1;
        let recorded = perf::snapshot();
        assert_eq!(decision.branch, StitchBranch::Contained);
        assert_eq!(decision.reference_mode, ReferenceMode::CanvasWindow);
        assert_eq!(
            recorded.calls[perf::Stage::ReferencePreparation as usize],
            1
        );
        let diagnostics = decision.motion_diagnostics.as_ref().unwrap();
        let checksum = output_checksum
            ^ u64::from(decision.accepted_offset.unwrap() as u32)
            ^ (u64::from(diagnostics.reference_keypoints) << 16)
            ^ (u64::from(diagnostics.incoming_keypoints) << 32)
            ^ (u64::from(diagnostics.mutual_matches) << 48);
        scopes.push((recorded, checksum));
        stitcher.clear_decisions();
        checksum
    });
    // Emit the calling-thread snapshot captured after each complete production push.
    // The preparation suffix retains the identity used by the original campaign.
    // Other scopes are inclusive and can overlap (for example PushTotal contains
    // the whole estimator); parallel work is timed around its join in production.
    // Skip the same three warm-up operations as the total-latency helper.
    for (iteration, (recorded, checksum)) in scopes.into_iter().skip(3).enumerate() {
        for (stage, label) in [
            (perf::Stage::FrameFreeze, "frame-freeze"),
            (perf::Stage::DuplicateCheck, "duplicate-check"),
            (perf::Stage::ReferencePreparation, "preparation"),
            (perf::Stage::Grayscale, "grayscale"),
            (perf::Stage::SimilarityMaps, "similarity-maps"),
            (perf::Stage::FeatureExtraction, "feature-extraction"),
            (perf::Stage::DescriptorMatching, "descriptor-matching"),
            (perf::Stage::CandidateScoring, "candidate-scoring"),
            (perf::Stage::Refinement, "refinement"),
            (perf::Stage::RegionUpdate, "region-update"),
            (perf::Stage::CanvasComposition, "canvas-composition"),
            (perf::Stage::ReferenceSynthesis, "reference-synthesis"),
            (perf::Stage::PreviewScaling, "preview-scaling"),
            (perf::Stage::PushTotal, "push-total"),
            (perf::Stage::Initialization, "initialization"),
            (perf::Stage::Reserved, "reserved"),
        ] {
            let elapsed_ns = recorded.elapsed_ns[stage as usize];
            let scope_calls = recorded.calls[stage as usize];
            if scope_calls != 0 {
                println!(
                    "{{\"record\":\"sample\",\"scenario\":\"{scenario}-{label}\",\"stage\":\"{label}\",\"scope_calls\":{scope_calls},\"width\":{width},\"height\":{height},\"iteration\":{iteration},\"elapsed_ns\":{elapsed_ns},\"checksum\":{checksum},\"logical_bytes\":{logical_bytes}}}"
                );
            }
        }
    }
    assert_eq!(stitcher.image_dimensions(), Some(canvas_dimensions));
    assert_eq!(stitcher.image().unwrap(), expected_output);
    drop(expected_output);
    drop(frames);
    if scenario.ends_with("-4k") {
        // With fixture and validation owners released, isolate the active
        // session's residency, including any retained comparison viewport.
        let canvas_bytes = canvas_dimensions.0 as usize * canvas_dimensions.1 as usize * 4;
        memory::phase(
            &scenario,
            "session",
            width,
            height,
            viewport_bytes * 2 + canvas_bytes,
        );
    }
    drop(stitcher);
    memory::phase(&scenario, "drop", width, height, 0);
}

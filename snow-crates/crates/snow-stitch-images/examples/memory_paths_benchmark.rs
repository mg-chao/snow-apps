//! `cargo run --release -p snow-stitch-images --no-default-features
//! --example memory_paths_benchmark -- CASE 31`
//! CASE: stitch-repaint, stitch-retained, stitch-export, stitch-materialize,
//! stitch-horizontal, stitch-orb. Compatible with the baseline public API.

#[path = "../../../benchmark-support/memory.rs"]
mod memory;

use snow_stitch_images::{
    Frame, MotionEstimatorOptions, PixelFormat, StitchAxis, TiledCanvas, VerticalMotionEstimator,
};
use std::hint::black_box;

fn texture(width: u32, height: u32, shift: u32) -> Frame {
    let mut pixels = Vec::with_capacity(width as usize * height as usize * 4);
    for y in 0..height {
        for x in 0..width {
            let mut value =
                (x / 4).wrapping_mul(0xc2b2_ae35) ^ ((y + shift) / 4).wrapping_mul(0x27d4_eb2d);
            value ^= value >> 16;
            value = value.wrapping_mul(0x7feb_352d);
            let gray = (value >> 24) as u8;
            pixels.extend_from_slice(&[gray, gray, gray, 255]);
        }
    }
    Frame::new(width, height, PixelFormat::Rgba8, pixels).unwrap()
}

fn main() {
    let (scenario, repetitions) = memory::arguments("stitch-repaint");
    if scenario == "stitch-orb" {
        orb(&scenario, repetitions);
        return;
    }
    let (width, height, axis, extent) = if scenario == "stitch-horizontal" {
        (2160, 1920, StitchAxis::Horizontal, 21600)
    } else {
        (3840, 2160, StitchAxis::Vertical, 21600)
    };
    let source_extent = if axis == StitchAxis::Vertical {
        height
    } else {
        width
    };
    let source_bytes = width as usize * height as usize * 4;
    let cross = if axis == StitchAxis::Vertical {
        width
    } else {
        height
    };
    let canvas_bytes = cross as usize * extent as usize * 4;
    let (canvas_width, canvas_height) = match axis {
        StitchAxis::Vertical => (width, extent),
        StitchAxis::Horizontal => (extent, height),
    };
    memory::phase(&scenario, "empty", canvas_width, canvas_height, 0);
    let source = texture(width, height, 0);
    let mut canvas = TiledCanvas::new_for_axis(source.clone(), axis).unwrap();
    while canvas.extent() < extent {
        let count = source_extent.min(extent - canvas.extent());
        canvas.append_axis(&source, 0, count).unwrap();
    }
    let mut leases = Vec::with_capacity(8);
    let mut output = None;
    let mut rows = vec![0_u8; canvas.width() as usize * 4 * 64];
    memory::phase(
        &scenario,
        "setup",
        canvas.width(),
        canvas.height(),
        source_bytes + canvas_bytes,
    );
    let logical_bytes = match scenario.as_str() {
        "stitch-materialize" => source_bytes + canvas_bytes * 2,
        "stitch-retained" => source_bytes + canvas_bytes + cross as usize * 128 * 4 * 8,
        "stitch-export" | "stitch-repaint" | "stitch-horizontal" => source_bytes + canvas_bytes,
        _ => panic!("unknown stitching scenario: {scenario}"),
    };
    memory::measure(
        &scenario,
        canvas.width(),
        canvas.height(),
        repetitions,
        logical_bytes,
        || match scenario.as_str() {
            "stitch-repaint" | "stitch-retained" | "stitch-horizontal" => {
                leases.clear();
                for _ in 0..8 {
                    if scenario == "stitch-retained" {
                        leases.push(canvas.snapshot_axis(0, extent).unwrap());
                    }
                    canvas.truncate_end(extent - 128).unwrap();
                    canvas
                        .append_axis(&source, source_extent - 128, source_extent)
                        .unwrap();
                }
                let edge = canvas.materialize_axis(extent - 1, extent).unwrap();
                assert_eq!(canvas.extent(), extent);
                assert_eq!(edge.pixels().len(), cross as usize * 4);
                memory::checksum(black_box(edge.pixels()))
            }
            "stitch-materialize" => {
                let frame = canvas.materialize().unwrap();
                assert_eq!(frame.pixels().len(), canvas_bytes);
                let checksum = memory::checksum(frame.pixels());
                output = Some(frame);
                checksum
            }
            "stitch-export" => {
                let snapshot = canvas.snapshot_axis(17, extent - 19).unwrap();
                let mut hash = 0_u64;
                for first in (0..snapshot.height()).step_by(64) {
                    let count = 64.min(snapshot.height() - first);
                    snapshot.copy_rows(first, count, &mut rows).unwrap();
                    hash = hash.wrapping_add(memory::checksum(
                        &rows[..count as usize * width as usize * 4],
                    ));
                }
                hash
            }
            _ => unreachable!(),
        },
    );
    drop(rows);
    drop(output);
    drop(leases);
    drop(canvas);
    drop(source);
    memory::phase(&scenario, "drop", canvas_width, canvas_height, 0);
}

fn orb(scenario: &str, repetitions: usize) {
    let (width, height) = (1920, 1080);
    let logical_bytes = width as usize * height as usize * 4 * 2;
    memory::phase(scenario, "empty", width, height, 0);
    let reference = texture(width, height, 0);
    let incoming = texture(width, height, 64);
    let mut estimator =
        VerticalMotionEstimator::new(reference.geometry(), MotionEstimatorOptions::default())
            .unwrap();
    memory::phase(scenario, "setup", width, height, logical_bytes);
    memory::measure(scenario, width, height, repetitions, logical_bytes, || {
        let estimate = estimator
            .estimate(&reference, &reference, &incoming)
            .unwrap();
        // Positive document coordinates move incoming content upward. The
        // four-pixel texture blocks can tie within the estimator's two-pixel
        // refinement tolerance; keep the full result in the paired checksum.
        assert!(
            estimate
                .offset()
                .is_some_and(|offset| (offset + 64).abs() <= 2)
        );
        assert!(estimate.diagnostics.reference_keypoints > 0);
        assert!(estimate.diagnostics.mutual_matches > 0);
        u64::from(estimate.offset().unwrap() as u32)
            ^ (u64::from(estimate.diagnostics.reference_keypoints) << 16)
            ^ (u64::from(estimate.diagnostics.incoming_keypoints) << 32)
            ^ (u64::from(estimate.diagnostics.mutual_matches) << 48)
    });
    drop(estimator);
    drop(incoming);
    drop(reference);
    memory::phase(scenario, "drop", width, height, 0);
}

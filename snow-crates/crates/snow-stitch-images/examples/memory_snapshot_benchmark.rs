//! Measure snapshot metadata work and ownership of a small retained scroll slice.
//! `cargo run --release -p snow-stitch-images --no-default-features
//! --example memory_snapshot_benchmark -- stitch-small-snapshot 31`
//! Uses the same public APIs and fixture on both optimization revisions.

#[path = "../../../benchmark-support/memory.rs"]
mod memory;

use snow_stitch_images::{Frame, PixelFormat, TiledCanvas};
use std::hint::black_box;

fn main() {
    let (scenario, repetitions) = memory::arguments("stitch-small-snapshot");
    assert_eq!(scenario, "stitch-small-snapshot");
    let width = 3840_u32;
    let viewport_height = 2160_u32;
    let canvas_height = 21600_u32;
    let start = 12345_u32;
    let snapshot_height = 128_u32;
    let row_bytes = width as usize * 4;
    let source_bytes = row_bytes * viewport_height as usize;
    let snapshot_bytes = row_bytes * snapshot_height as usize;
    memory::phase(&scenario, "empty", width, snapshot_height, 0);

    let mut pixels = Vec::with_capacity(source_bytes);
    for y in 0..viewport_height {
        for x in 0..width {
            pixels.extend_from_slice(&[
                (x.wrapping_mul(37) ^ y.wrapping_mul(13)) as u8,
                (x.wrapping_mul(17) + y.wrapping_mul(43)) as u8,
                (x.wrapping_mul(11) ^ y.wrapping_mul(29)) as u8,
                255,
            ]);
        }
    }
    let source = Frame::new(width, viewport_height, PixelFormat::Rgba8, pixels).unwrap();
    let mut canvas = TiledCanvas::new(source.clone()).unwrap();
    while canvas.height() < canvas_height {
        let count = viewport_height.min(canvas_height - canvas.height());
        canvas.append_rows(&source, 0, count).unwrap();
    }
    // The selected range lies wholly inside the repeated viewport fixture and
    // one 256-row tile. Borrow expected pixels without creating another raster.
    let source_row = (start % viewport_height) as usize;
    assert!(source_row + snapshot_height as usize <= viewport_height as usize);
    let expected_hash = memory::checksum(
        &source.pixels()[source_row * row_bytes..source_row * row_bytes + snapshot_bytes],
    );
    memory::phase(
        &scenario,
        "setup",
        width,
        snapshot_height,
        source_bytes + row_bytes * canvas_height as usize,
    );
    memory::measure(
        &scenario,
        width,
        snapshot_height,
        repetitions,
        source_bytes + row_bytes * canvas_height as usize,
        || {
            let snapshot = canvas.snapshot(start, start + snapshot_height).unwrap();
            assert_eq!(
                (snapshot.width(), snapshot.height()),
                (width, snapshot_height)
            );
            black_box(&snapshot);
            drop(snapshot);
            expected_hash
        },
    );

    let retained = canvas.snapshot(start, start + snapshot_height).unwrap();
    drop(canvas);
    drop(source);
    memory::phase(
        &scenario,
        "snapshot-retained",
        width,
        snapshot_height,
        snapshot_bytes,
    );

    // Validate after the retention checkpoint, so the materialized output does
    // not inflate the measured ownership of the snapshot itself.
    let materialized = retained.materialize().unwrap();
    assert_eq!(
        (materialized.width(), materialized.height()),
        (width, snapshot_height)
    );
    assert_eq!(materialized.pixels().len(), snapshot_bytes);
    assert_eq!(memory::checksum(materialized.pixels()), expected_hash);
    drop(materialized);
    drop(retained);
    memory::phase(&scenario, "drop", width, snapshot_height, 0);
}

use std::{env, fs, hint::black_box, path::PathBuf, time::Instant};

use anyhow::{Result, bail};
use serde::Serialize;
use snow_stitch_images::{Frame, PixelFormat, StitchAxis, TiledCanvas};

#[derive(Serialize)]
struct Measurement {
    scenario: String,
    operation: String,
    width: u32,
    height: u32,
    samples_us: Vec<f64>,
    median_us: f64,
    p95_us: f64,
}

fn measure(
    scenario: &str,
    operation: &str,
    width: u32,
    height: u32,
    mut run: impl FnMut(),
) -> Measurement {
    for _ in 0..5 {
        run();
    }
    let mut samples_us = Vec::new();
    for _ in 0..31 {
        let start = Instant::now();
        run();
        samples_us.push(start.elapsed().as_secs_f64() * 1_000_000.0);
    }
    let mut sorted = samples_us.clone();
    sorted.sort_by(f64::total_cmp);
    Measurement {
        scenario: scenario.to_owned(),
        operation: operation.to_owned(),
        width,
        height,
        median_us: sorted[15],
        p95_us: sorted[29],
        samples_us,
    }
}

fn main() -> Result<()> {
    if cfg!(debug_assertions) {
        bail!("snapshot export benchmark requires --release");
    }
    let output = env::args_os().nth(1).map(PathBuf::from);
    let mut measurements = Vec::new();
    for (name, width, height, axis) in [
        ("vertical-corpus-size", 1588, 3828, StitchAxis::Vertical),
        ("vertical-long", 3840, 21600, StitchAxis::Vertical),
        ("horizontal-long", 21600, 2160, StitchAxis::Horizontal),
    ] {
        let pixels = (0..width as usize * height as usize * 4)
            .map(|i| (i.wrapping_mul(37) ^ (i >> 13)) as u8)
            .collect();
        let canvas = TiledCanvas::new_for_axis(
            Frame::new(width, height, PixelFormat::Rgba8, pixels)?,
            axis,
        )?;
        let extent = canvas.extent();
        // Unaligned trims exercise partial tiles as well as full interior tiles.
        let snapshot = canvas.snapshot_axis(17, extent - 19)?;
        let row_bytes = snapshot.width() as usize * 4;
        for batch in [1, 64, 256] {
            let mut buffer = vec![0; row_bytes * batch as usize];
            measurements.push(measure(
                name,
                &format!("copy_rows_{batch}"),
                snapshot.width(),
                snapshot.height(),
                || {
                    for first in (0..snapshot.height()).step_by(batch as usize) {
                        let count = batch.min(snapshot.height() - first);
                        snapshot
                            .copy_rows(first, count, black_box(&mut buffer))
                            .unwrap();
                        black_box(&buffer);
                    }
                },
            ));
        }
        for (label, start, end) in [
            ("preview-full", 0, snapshot.axis_extent()),
            (
                "preview-edge",
                snapshot.axis_extent() - 512,
                snapshot.axis_extent(),
            ),
        ] {
            let patch = snapshot.slice_axis(start, end)?;
            let (out_width, out_height) = match axis {
                StitchAxis::Vertical => (128, (patch.height() * 128).div_ceil(patch.width())),
                StitchAxis::Horizontal => ((patch.width() * 128).div_ceil(patch.height()), 128),
            };
            measurements.push(measure(name, label, patch.width(), patch.height(), || {
                black_box(patch.render_scaled(out_width, out_height).unwrap());
            }));
        }
    }
    for m in &measurements {
        println!(
            "{} {}: median {:.1} us, p95 {:.1} us",
            m.scenario, m.operation, m.median_us, m.p95_us
        );
    }
    if let Some(output) = output {
        if let Some(parent) = output.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(output, serde_json::to_vec_pretty(&measurements)?)?;
    }
    Ok(())
}

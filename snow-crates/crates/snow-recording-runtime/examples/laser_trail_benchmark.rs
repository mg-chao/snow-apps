//! Run through scripts/run-recording-laser-perf.ps1 (Release only).
//! Compares just overlay raster/composition against the former 350 ms circle trail.
//! Optional first argument writes PPM previews for visual inspection.
#[path = "../src/laser_trail.rs"]
mod laser_trail;

use laser_trail::LaserTrail;
use std::hint::black_box;
use std::io::Write;
use std::path::Path;
use std::time::Instant;

type Point = (i32, i32, u64);
const COLOR: [u8; 4] = [255, 35, 60, 160];

fn samples(size: (u32, u32), pattern: &str) -> Vec<Point> {
    (0..60)
        .map(|index| {
            let t = index as f32 / 59.0;
            let (x, y) = match pattern {
                "loop" => {
                    let angle = t * std::f32::consts::TAU;
                    (0.5 + angle.sin() * 0.35, 0.5 + (angle * 2.0).sin() * 0.3)
                }
                "zigzag" => (0.1 + t * 0.8, if index % 10 < 5 { 0.2 } else { 0.8 }),
                "uturn" => {
                    let angle = t * std::f32::consts::PI;
                    (0.15 + angle.sin() * 0.7, 0.5 - angle.cos() * 0.25)
                }
                _ => (0.1 + t * 0.8, 0.15 + t * 0.7),
            };
            (
                (x * size.0 as f32) as i32,
                (y * size.1 as f32) as i32,
                index * 16,
            )
        })
        .collect()
}

fn legacy(rgba: &mut [u8], size: (u32, u32), points: &[Point], now: u64) {
    // Preserve the previous implementation's per-frame copy, 350 ms window,
    // radius-three circle stamps, and repeated source-over alpha blending.
    let points: Vec<_> = points
        .iter()
        .copied()
        .filter(|point| now.saturating_sub(point.2) <= 350)
        .collect();
    for pair in points.windows(2) {
        let from = pair[0];
        let to = pair[1];
        let alpha = u32::from(COLOR[3]) * (350 - now.saturating_sub(to.2)) as u32 / 350;
        let dx = to.0 - from.0;
        let dy = to.1 - from.1;
        let steps = dx.unsigned_abs().max(dy.unsigned_abs()).max(1);
        for step in 0..=steps {
            let x = from.0 + (i64::from(dx) * i64::from(step) / i64::from(steps)) as i32;
            let y = from.1 + (i64::from(dy) * i64::from(step) / i64::from(steps)) as i32;
            for oy in -3..=3 {
                for ox in -3..=3 {
                    if ox * ox + oy * oy > 9 {
                        continue;
                    }
                    let (px, py) = (x + ox, y + oy);
                    if px < 0 || py < 0 || px >= size.0 as i32 || py >= size.1 as i32 || alpha == 0
                    {
                        continue;
                    }
                    let offset = (py as usize * size.0 as usize + px as usize) * 4;
                    for channel in 0..3 {
                        rgba[offset + channel] = ((u32::from(COLOR[channel]) * alpha
                            + u32::from(rgba[offset + channel]) * (255 - alpha)
                            + 127)
                            / 255) as u8;
                    }
                    rgba[offset + 3] = 255;
                }
            }
        }
    }
}

fn measure(mut draw: impl FnMut()) -> (f64, f64) {
    for _ in 0..20 {
        draw();
    }
    let mut times = Vec::with_capacity(300);
    for _ in 0..300 {
        let started = Instant::now();
        draw();
        times.push(started.elapsed().as_secs_f64() * 1_000_000.0);
    }
    times.sort_by(f64::total_cmp);
    (times[150], times[285])
}

fn ppm(path: &Path, size: (u32, u32), rgba: &[u8]) -> std::io::Result<()> {
    let mut file = std::io::BufWriter::new(std::fs::File::create(path)?);
    write!(file, "P6\n{} {}\n255\n", size.0, size.1)?;
    for pixel in rgba.chunks_exact(4) {
        file.write_all(&pixel[..3])?;
    }
    Ok(())
}

fn main() -> std::io::Result<()> {
    if cfg!(debug_assertions) {
        return Err(std::io::Error::other(
            "use the windows-msvc-performance Release environment",
        ));
    }
    println!("size,pattern,legacy_p50_us,laser_p50_us,legacy_p95_us,laser_p95_us");
    for size in [(1920, 1080), (3840, 2160)] {
        for pattern in ["line", "loop", "zigzag", "uturn"] {
            let points = samples(size, pattern);
            let mut trail = LaserTrail::default();
            for &(x, y, time) in &points {
                trail.observe(Some((x, y)), size, size, time);
            }
            let mut rgba = vec![0; (size.0 * size.1 * 4) as usize];
            let old = measure(|| {
                legacy(black_box(&mut rgba), size, black_box(&points), 944);
            });
            let new = measure(|| {
                trail.draw(black_box(&mut rgba), size, black_box(944), COLOR);
            });
            println!(
                "{}x{},{pattern},{:.2},{:.2},{:.2},{:.2}",
                size.0, size.1, old.0, new.0, old.1, new.1
            );
            assert!(trail.has_active_animation(944));
            trail.clear();
            assert!(!trail.has_active_animation(1944));
        }
    }
    if let Some(directory) = std::env::args_os().nth(1) {
        let directory = Path::new(&directory);
        std::fs::create_dir_all(directory)?;
        let size = (640, 240);
        for pattern in ["line", "loop", "zigzag", "uturn"] {
            let points = samples(size, pattern);
            let mut trail = LaserTrail::default();
            for &(x, y, time) in &points {
                trail.observe(Some((x, y)), size, size, time);
            }
            for (name, time) in [("fresh", 944), ("decay", 1194), ("expired", 1444)] {
                let background = [246, 246, 248, 255];
                let mut rgba = background.repeat((size.0 * size.1) as usize);
                trail.draw(&mut rgba, size, time, COLOR);
                ppm(
                    &directory.join(format!("laser-{pattern}-{name}.ppm")),
                    size,
                    &rgba,
                )?;
                rgba = background.repeat((size.0 * size.1) as usize);
                legacy(&mut rgba, size, &points, time);
                ppm(
                    &directory.join(format!("legacy-{pattern}-{name}.ppm")),
                    size,
                    &rgba,
                )?;
            }
        }
    }
    Ok(())
}

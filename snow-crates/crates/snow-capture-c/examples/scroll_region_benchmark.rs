use std::fs;
use std::hint::black_box;
use std::path::PathBuf;
use std::ptr;
use std::slice;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use snow_capture_c::*;

const DEFAULT_WIDTH: u32 = 800;
const DEFAULT_HEIGHT: u32 = 600;
const DEFAULT_WARMUPS: usize = 30;
const DEFAULT_SAMPLES: usize = 240;

struct Options {
    output: PathBuf,
    x: Option<i32>,
    y: Option<i32>,
    width: u32,
    height: u32,
    warmups: usize,
    samples: usize,
}

struct Desktop(*mut SnowCaptureDesktopSessionImpl);
struct Region(*mut SnowCaptureRegionSessionImpl);

impl Drop for Desktop {
    fn drop(&mut self) {
        unsafe { snow_capture_desktop_session_destroy(self.0) };
    }
}

impl Drop for Region {
    fn drop(&mut self) {
        unsafe { snow_capture_region_session_destroy(self.0) };
    }
}

fn main() -> Result<()> {
    if cfg!(debug_assertions) {
        bail!("scroll_region_benchmark must run with --release");
    }
    let options = parse_args()?;
    let desktop = Desktop(snow_capture_desktop_session_create(ptr::null()));
    if desktop.0.is_null() || snow_capture_desktop_session_prepare(desktop.0) == 0 {
        bail!("failed to prepare desktop capture session");
    }
    let (x, y, width, height) = match (options.x, options.y) {
        (Some(x), Some(y)) => (x, y, options.width, options.height),
        (None, None) => {
            let bounds = primary_bounds(desktop.0)?;
            let width = options.width.min(bounds.2);
            let height = options.height.min(bounds.3);
            (
                bounds.0 + ((bounds.2 - width) / 2) as i32,
                bounds.1 + ((bounds.3 - height) / 2) as i32,
                width,
                height,
            )
        }
        _ => bail!("--x and --y must be provided together"),
    };
    let config = SnowCaptureRegionSessionConfig {
        x,
        y,
        width,
        height,
        capture_retry_count: 1,
        wgc_update_mode: 0,
        capture_backend: 0,
        reserved: [0; 30],
    };
    let region = Region(unsafe { snow_capture_region_session_create(&config) });
    if region.0.is_null() || unsafe { snow_capture_region_session_prepare(region.0) } == 0 {
        bail!("failed to prepare region capture session");
    }

    let mut destination = vec![0u8; width as usize * height as usize * 4];
    for _ in 0..options.warmups {
        capture_desktop_crop(desktop.0, (x, y, width, height), &mut destination)?;
        capture_region(region.0, &mut destination)?;
    }

    let mut desktop_times = Vec::with_capacity(options.samples);
    let mut region_times = Vec::with_capacity(options.samples);
    for index in 0..options.samples {
        if index % 2 == 0 {
            desktop_times.push(measure(|| {
                capture_desktop_crop(desktop.0, (x, y, width, height), &mut destination)
            })?);
            region_times.push(measure(|| capture_region(region.0, &mut destination))?);
        } else {
            region_times.push(measure(|| capture_region(region.0, &mut destination))?);
            desktop_times.push(measure(|| {
                capture_desktop_crop(desktop.0, (x, y, width, height), &mut destination)
            })?);
        }
        black_box(destination.first().copied());
    }

    let desktop_stats = stats(&mut desktop_times);
    let region_stats = stats(&mut region_times);
    let improvement = (desktop_stats.1 - region_stats.1) / desktop_stats.1 * 100.0;
    println!(
        "desktop p50={:.3}ms p95={:.3}ms; region p50={:.3}ms p95={:.3}ms; p95 improvement={:.2}%",
        desktop_stats.0, desktop_stats.1, region_stats.0, region_stats.1, improvement
    );
    if let Some(parent) = options.output.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(
        &options.output,
        format!(
            "{{\n  \"region\": [{x},{y},{width},{height}],\n  \"samples\": {},\n  \"warmups\": {},\n  \"desktop\": {{\"p50_ms\": {:.6}, \"p95_ms\": {:.6}}},\n  \"region_capture\": {{\"p50_ms\": {:.6}, \"p95_ms\": {:.6}}},\n  \"p95_improvement_pct\": {:.6}\n}}\n",
            options.samples,
            options.warmups,
            desktop_stats.0,
            desktop_stats.1,
            region_stats.0,
            region_stats.1,
            improvement,
        ),
    )?;
    Ok(())
}

fn parse_args() -> Result<Options> {
    let mut options = Options {
        output: PathBuf::from("target/scrolling-perf/02-region-capture.json"),
        x: None,
        y: None,
        width: DEFAULT_WIDTH,
        height: DEFAULT_HEIGHT,
        warmups: DEFAULT_WARMUPS,
        samples: DEFAULT_SAMPLES,
    };
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut index = 0usize;
    while index < args.len() {
        let flag = &args[index];
        let value = args
            .get(index + 1)
            .with_context(|| format!("{flag} requires a value"))?;
        match flag.as_str() {
            "--output" => options.output = PathBuf::from(value),
            "--x" => options.x = Some(value.parse().context("invalid --x")?),
            "--y" => options.y = Some(value.parse().context("invalid --y")?),
            "--width" => options.width = value.parse().context("invalid --width")?,
            "--height" => options.height = value.parse().context("invalid --height")?,
            "--warmups" => options.warmups = value.parse().context("invalid --warmups")?,
            "--samples" => options.samples = value.parse().context("invalid --samples")?,
            _ => bail!("unknown argument {flag}"),
        }
        index += 2;
    }
    if options.width == 0 || options.height == 0 || options.samples == 0 {
        bail!("width, height, and samples must be positive");
    }
    Ok(options)
}

fn primary_bounds(session: *mut SnowCaptureDesktopSessionImpl) -> Result<(i32, i32, u32, u32)> {
    let snapshot = snow_capture_desktop_session_capture_all(session);
    if snapshot.is_null() {
        bail!("desktop capture failed");
    }
    let result = (0..snow_capture_snapshot_count(snapshot)).find_map(|index| {
        let mut info = empty_desktop_info();
        let ok = unsafe { snow_capture_snapshot_frame_info(snapshot, index, &mut info) };
        (ok != 0 && info.is_primary != 0).then_some((info.x, info.y, info.width, info.height))
    });
    unsafe { snow_capture_snapshot_destroy(snapshot) };
    result.context("no primary monitor frame")
}

fn capture_desktop_crop(
    session: *mut SnowCaptureDesktopSessionImpl,
    region: (i32, i32, u32, u32),
    destination: &mut [u8],
) -> Result<()> {
    let snapshot = snow_capture_desktop_session_capture_all(session);
    if snapshot.is_null() {
        bail!("desktop capture failed");
    }
    destination.fill(0);
    let mut copied_pixels = 0u64;
    for index in 0..snow_capture_snapshot_count(snapshot) {
        let mut info = empty_desktop_info();
        if unsafe { snow_capture_snapshot_frame_info(snapshot, index, &mut info) } == 0 {
            continue;
        }
        let left = i64::from(region.0).max(i64::from(info.x));
        let top = i64::from(region.1).max(i64::from(info.y));
        let right = (i64::from(region.0) + i64::from(region.2))
            .min(i64::from(info.x) + i64::from(info.width));
        let bottom = (i64::from(region.1) + i64::from(region.3))
            .min(i64::from(info.y) + i64::from(info.height));
        if right <= left || bottom <= top {
            continue;
        }
        let source = unsafe { slice::from_raw_parts(info.rgba_bytes, info.rgba_len) };
        let source_x = (left - i64::from(info.x)) as usize;
        let source_y = (top - i64::from(info.y)) as usize;
        let destination_x = (left - i64::from(region.0)) as usize;
        let destination_y = (top - i64::from(region.1)) as usize;
        let copy_width = (right - left) as usize;
        let copy_height = (bottom - top) as usize;
        let row_bytes = copy_width * 4;
        let destination_stride = region.2 as usize * 4;
        for row in 0..copy_height {
            let src = (source_y + row) * info.stride_bytes as usize + source_x * 4;
            let dst = (destination_y + row) * destination_stride + destination_x * 4;
            destination[dst..dst + row_bytes].copy_from_slice(&source[src..src + row_bytes]);
        }
        copied_pixels += copy_width as u64 * copy_height as u64;
    }
    unsafe { snow_capture_snapshot_destroy(snapshot) };
    (copied_pixels == u64::from(region.2) * u64::from(region.3))
        .then_some(())
        .context("region was not fully covered by non-overlapping monitor frames")
}

fn capture_region(
    session: *mut SnowCaptureRegionSessionImpl,
    destination: &mut [u8],
) -> Result<()> {
    let mut info = SnowCaptureRegionFrameInfo {
        width: 0,
        height: 0,
        stride_bytes: 0,
        is_duplicate: 0,
        reserved0: [0; 3],
        rgba_bytes: ptr::null(),
        rgba_len: 0,
    };
    if unsafe { snow_capture_region_session_capture(session, &mut info) } == 0 {
        bail!("region capture failed");
    }
    let source = unsafe { slice::from_raw_parts(info.rgba_bytes, info.rgba_len) };
    destination.copy_from_slice(source);
    Ok(())
}

fn empty_desktop_info() -> SnowCaptureFrameInfo {
    SnowCaptureFrameInfo {
        stable_id: ptr::null(),
        name: ptr::null(),
        x: 0,
        y: 0,
        width: 0,
        height: 0,
        is_primary: 0,
        backend_kind: 0,
        reserved0: [0; 2],
        stride_bytes: 0,
        rgba_bytes: ptr::null(),
        rgba_len: 0,
    }
}

fn measure(operation: impl FnOnce() -> Result<()>) -> Result<Duration> {
    let start = Instant::now();
    operation()?;
    Ok(start.elapsed())
}

fn stats(values: &mut [Duration]) -> (f64, f64) {
    values.sort_unstable();
    let last = values.len() - 1;
    (
        values[last / 2].as_secs_f64() * 1000.0,
        values[((last as f64) * 0.95).round() as usize].as_secs_f64() * 1000.0,
    )
}

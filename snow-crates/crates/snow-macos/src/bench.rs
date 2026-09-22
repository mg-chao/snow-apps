//! Side-by-side timings for the capture hot-path choices.
//! The example `capture_path_bench` prints one row per comparison.
use crate::compositor::{Compositor, Layer};
use crate::content::{self, fetch_shareable_content};
use snow_media::{
    PixelFormat,
    geometry::{PixelRect, PixelSize},
};
use std::thread;
use std::time::{Duration, Instant};

#[derive(Clone, Debug)]
pub struct Timing {
    pub label: String,
    pub median_ms: f64,
    pub p95_ms: f64,
    pub note: String,
}

pub fn capture_path_timings() -> Vec<Timing> {
    let mut rows = Vec::new();
    match gpu_rows() {
        Ok(mut gpu) => rows.append(&mut gpu),
        Err(error) => rows.push(skipped("gpu", &error)),
    }
    match cpu_rows() {
        Ok(mut cpu) => rows.append(&mut cpu),
        Err(error) => rows.push(skipped("cpu", &error)),
    }
    rows.extend(enumeration_rows());
    rows.extend(window_probe_rows());
    rows
}

fn skipped(label: &str, error: &str) -> Timing {
    Timing {
        label: label.to_owned(),
        median_ms: 0.0,
        p95_ms: 0.0,
        note: format!("skipped: {error}"),
    }
}

fn percentile(samples: &mut [f64]) -> (f64, f64) {
    samples.sort_by(|left, right| left.partial_cmp(right).unwrap());
    let median = samples[samples.len() / 2];
    let index = ((samples.len() - 1) as f64 * 0.95).round() as usize;
    (median, samples[index])
}

fn time_ms(warmup: usize, count: usize, mut op: impl FnMut()) -> (f64, f64) {
    for _ in 0..warmup {
        op();
    }
    let mut samples = Vec::with_capacity(count);
    for _ in 0..count {
        let start = Instant::now();
        op();
        samples.push(start.elapsed().as_secs_f64() * 1_000.0);
    }
    percentile(&mut samples)
}

fn row(label: &str, median_ms: f64, p95_ms: f64, note: impl Into<String>) -> Timing {
    Timing {
        label: label.to_owned(),
        median_ms,
        p95_ms,
        note: note.into(),
    }
}

fn gpu_rows() -> Result<Vec<Timing>, String> {
    let mut rows = Vec::new();
    for (width, height) in [(1920, 1080), (3840, 2160)] {
        let size = PixelSize::new(width, height).map_err(|error| error.to_string())?;
        let mut cached = Compositor::with_intermediate_cache(size, PixelFormat::Bgra8, 4, true)
            .map_err(|error| error.to_string())?;
        let mut direct = Compositor::with_intermediate_cache(size, PixelFormat::Bgra8, 4, false)
            .map_err(|error| error.to_string())?;
        let source = direct
            .compose(&[], true)
            .map_err(|error| error.to_string())?;
        let rect = PixelRect {
            x: 0,
            y: 0,
            width,
            height,
        };
        let copied = direct
            .copy_identical(&source)
            .map_err(|error| error.to_string())?;
        if source.to_cpu().map_err(|error| error.to_string())?.bytes
            != copied.to_cpu().map_err(|error| error.to_string())?.bytes
        {
            return Err(format!("metal blit changed {width}x{height} pixels"));
        }
        let (compose_median, compose_p95) = time_ms(4, 16, || {
            let image = cached
                .compose(
                    &[Layer {
                        image: &source,
                        source: rect,
                        destination: rect,
                    }],
                    false,
                )
                .expect("core image identity");
            std::hint::black_box(image);
        });
        let (uncached_median, uncached_p95) = time_ms(4, 16, || {
            let image = direct
                .compose(
                    &[Layer {
                        image: &source,
                        source: rect,
                        destination: rect,
                    }],
                    false,
                )
                .expect("uncached core image");
            std::hint::black_box(image);
        });
        let (blit_median, blit_p95) = time_ms(4, 16, || {
            let image = direct.copy_identical(&source).expect("metal blit");
            std::hint::black_box(image);
        });
        rows.push(row(
            &format!("core-image identity {width}x{height}, intermediates cached"),
            compose_median,
            compose_p95,
            "previous detach path",
        ));
        rows.push(row(
            &format!("core-image identity {width}x{height}, intermediates off"),
            uncached_median,
            uncached_p95,
            ratio(uncached_median, compose_median),
        ));
        rows.push(row(
            &format!("metal blit {width}x{height}"),
            blit_median,
            blit_p95,
            ratio(blit_median, compose_median),
        ));
    }
    Ok(rows)
}

fn cpu_rows() -> Result<Vec<Timing>, String> {
    let mut rows = Vec::new();
    for (width, height) in [(1920, 1080), (3840, 2160)] {
        let size = PixelSize::new(width, height).map_err(|error| error.to_string())?;
        let mut compositor =
            Compositor::new(size, PixelFormat::Bgra8, 2).map_err(|error| error.to_string())?;
        let image = compositor
            .compose(&[], true)
            .map_err(|error| error.to_string())?;
        let bytes = (width as usize) * (height as usize) * 4;
        let (double_median, double_p95) = time_ms(4, 20, || {
            let cpu = image.to_cpu().expect("readback");
            let mut frame = vec![0; bytes];
            frame.copy_from_slice(&cpu.bytes);
            std::hint::black_box(frame);
        });
        let (single_median, single_p95) = time_ms(4, 20, || {
            let mut frame = vec![0; bytes];
            image.copy_packed(&mut frame, false).expect("packed copy");
            std::hint::black_box(frame);
        });
        let (swap_median, swap_p95) = time_ms(4, 20, || {
            let mut frame = vec![0; bytes];
            image
                .copy_packed(&mut frame, true)
                .expect("packed channel swap");
            std::hint::black_box(frame);
        });
        let (two_pass_median, two_pass_p95) = time_ms(4, 20, || {
            let mut frame = vec![0; bytes];
            image.copy_packed(&mut frame, false).expect("packed copy");
            snow_media::convert::swap_red_blue(&mut frame);
            std::hint::black_box(frame);
        });
        rows.push(row(
            &format!("cpu to_cpu + memcpy {width}x{height}"),
            double_median,
            double_p95,
            "previous screenshot readback",
        ));
        rows.push(row(
            &format!("cpu copy_packed {width}x{height}"),
            single_median,
            single_p95,
            ratio(single_median, double_median),
        ));
        rows.push(row(
            &format!("cpu copy_packed + swap pass {width}x{height}"),
            two_pass_median,
            two_pass_p95,
            "copy then swap",
        ));
        rows.push(row(
            &format!("cpu copy_packed swapping {width}x{height}"),
            swap_median,
            swap_p95,
            ratio(swap_median, two_pass_median),
        ));
    }
    Ok(rows)
}

fn enumeration_rows() -> Vec<Timing> {
    let token = crate::CancellationToken::default();
    let mut rows = Vec::new();
    let full = match sample_fetch(false, false, &token, 1, 5) {
        Ok(timing) => timing,
        Err(error) => return vec![skipped("shareable content", &error.to_string())],
    };
    rows.push(row(
        "shareable content, all windows",
        full.median_ms,
        full.p95_ms,
        format!("{} displays, {} windows", full.displays, full.windows),
    ));
    match sample_fetch(false, true, &token, 1, 5) {
        Ok(timing) => rows.push(row(
            "shareable content, on-screen windows only",
            timing.median_ms,
            timing.p95_ms,
            format!(
                "{} displays, {} windows; {}",
                timing.displays,
                timing.windows,
                ratio(timing.median_ms, full.median_ms)
            ),
        )),
        Err(error) => rows.push(skipped("on-screen enumeration", &error.to_string())),
    }
    let serial = serial_display_wall(&token);
    let parallel = parallel_display_wall(&token);
    rows.push(row(
        "4 serial display enumerations",
        serial.0,
        serial.0,
        if serial.1 {
            "distinct requests, no overlap".to_owned()
        } else {
            "a serial enumeration failed".to_owned()
        },
    ));
    rows.push(row(
        "4 parallel display enumerations, shared token",
        parallel.0,
        parallel.0,
        if parallel.1 {
            format!(
                "wall versus serial {:.2} ms; {}",
                serial.0,
                ratio(parallel.0, serial.0)
            )
        } else {
            "a parallel enumeration failed".to_owned()
        },
    ));
    rows
}

struct FetchSample {
    median_ms: f64,
    p95_ms: f64,
    displays: usize,
    windows: usize,
}

fn sample_fetch(
    excluding_desktop: bool,
    on_screen_only: bool,
    token: &crate::CancellationToken,
    warmup: usize,
    count: usize,
) -> Result<FetchSample, crate::MacError> {
    for _ in 0..warmup {
        fetch_shareable_content(
            Duration::from_secs(5),
            token,
            excluding_desktop,
            on_screen_only,
        )?;
    }
    let mut samples = Vec::with_capacity(count);
    let mut last = None;
    for _ in 0..count {
        let start = Instant::now();
        last = Some(fetch_shareable_content(
            Duration::from_secs(5),
            token,
            excluding_desktop,
            on_screen_only,
        )?);
        samples.push(start.elapsed().as_secs_f64() * 1_000.0);
    }
    let content = last.ok_or(crate::MacError::Inactive)?;
    let (median_ms, p95_ms) = percentile(&mut samples);
    Ok(FetchSample {
        median_ms,
        p95_ms,
        displays: unsafe { content.displays() }.len(),
        windows: unsafe { content.windows() }.len(),
    })
}

fn serial_display_wall(token: &crate::CancellationToken) -> (f64, bool) {
    let start = Instant::now();
    let mut ok = true;
    for _ in 0..4 {
        ok &= content::displays_cancelable(Duration::from_secs(5), token).is_ok();
    }
    (start.elapsed().as_secs_f64() * 1_000.0, ok)
}

fn parallel_display_wall(token: &crate::CancellationToken) -> (f64, bool) {
    let start = Instant::now();
    let mut handles = Vec::new();
    for _ in 0..4 {
        let shared = token.clone();
        handles.push(thread::spawn(move || {
            content::displays_cancelable(Duration::from_secs(5), &shared).is_ok()
        }));
    }
    let ok = handles
        .into_iter()
        .all(|handle| handle.join().unwrap_or(false));
    (start.elapsed().as_secs_f64() * 1_000.0, ok)
}

fn sample_window_list() -> Result<(f64, f64, usize), crate::MacError> {
    content::windows(Duration::from_secs(5))?;
    let mut samples = Vec::with_capacity(3);
    let mut count = 0;
    for _ in 0..3 {
        let start = Instant::now();
        count = content::windows(Duration::from_secs(5))?.len();
        samples.push(start.elapsed().as_secs_f64() * 1_000.0);
    }
    let (median, p95) = percentile(&mut samples);
    Ok((median, p95, count))
}

fn window_probe_rows() -> Vec<Timing> {
    let Some(id) = first_window_id() else {
        return vec![skipped("window probe", "no window id")];
    };
    let (probe_median, probe_p95) = time_ms(10, 40, || {
        std::hint::black_box(content::probe_window(id));
    });
    let (list_median, list_p95) = time_ms(10, 40, || {
        std::hint::black_box(on_screen_window_info_count());
    });
    let mut rows = vec![
        row(
            "single-window geometry probe",
            probe_median,
            probe_p95,
            format!("window {id}"),
        ),
        row(
            "CGWindowList on-screen window info",
            list_median,
            list_p95,
            format!(
                "{} windows; {}",
                on_screen_window_info_count().unwrap_or(0),
                ratio(probe_median, list_median)
            ),
        ),
    ];
    match sample_window_list() {
        Ok((median, p95, windows)) => rows.push(row(
            "full shareable-content window list",
            median,
            p95,
            format!("{windows} windows; {}", ratio(probe_median, median)),
        )),
        Err(error) => rows.push(skipped("window list", &error.to_string())),
    }
    rows
}

fn on_screen_window_info_count() -> Option<isize> {
    let list = objc2_core_graphics::CGWindowListCopyWindowInfo(
        objc2_core_graphics::CGWindowListOption::OptionOnScreenOnly,
        objc2_core_graphics::kCGNullWindowID,
    )?;
    Some(list.count())
}

fn first_window_id() -> Option<u32> {
    // probe_window returns None for a missing id; ask WindowServer for any real one.
    let list = objc2_core_graphics::CGWindowListCopyWindowInfo(
        objc2_core_graphics::CGWindowListOption::OptionOnScreenOnly,
        objc2_core_graphics::kCGNullWindowID,
    )?;
    if list.count() < 1 {
        return None;
    }
    let value = unsafe { list.value_at_index(0) };
    let dict =
        std::ptr::NonNull::new(value.cast_mut())?.cast::<objc2_core_foundation::CFDictionary>();
    let dict = unsafe {
        objc2_core_foundation::CFRetained::<objc2_core_foundation::CFDictionary>::retain(dict)
    };
    let raw =
        unsafe { dict.value(std::ptr::from_ref(objc2_core_graphics::kCGWindowNumber).cast()) };
    let ptr = std::ptr::NonNull::new(raw.cast_mut().cast::<objc2_core_foundation::CFType>())?;
    let number = unsafe { objc2_core_foundation::CFRetained::retain(ptr) }
        .downcast::<objc2_core_foundation::CFNumber>()
        .ok()?;
    u32::try_from(number.as_i64()?).ok()
}

fn ratio(candidate: f64, baseline: f64) -> String {
    if baseline <= f64::EPSILON {
        return "no baseline".to_owned();
    }
    let percent = (1.0 - candidate / baseline) * 100.0;
    if percent >= 5.0 {
        format!("{percent:.1}% faster than baseline")
    } else if percent <= -5.0 {
        format!("{:.1}% slower than baseline", -percent)
    } else {
        format!("{percent:.1}% versus baseline, within noise")
    }
}

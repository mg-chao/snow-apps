#![cfg(target_os = "windows")]

use std::time::{Duration, Instant};

use snow_capture::backend::CaptureBackendKind;
use snow_capture::frame::Frame;
use snow_capture::{CaptureOptions, CaptureSystem, CaptureTarget, WgcUpdateMode};
use windows::Win32::UI::HiDpi::{
    DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetThreadDpiAwarenessContext,
};

const DEFAULT_SAMPLE_COUNT: usize = 1;
const SAMPLE_COUNT_ENV: &str = "SNOW_CAPTURE_SINGLE_CAPTURE_SAMPLES";

#[derive(Clone, Copy, Debug)]
struct Timing {
    system_build: Duration,
    session_open: Duration,
    capture_wall: Duration,
    total: Duration,
    pipeline: Option<Duration>,
}

struct TimedCapture {
    backend: CaptureBackendKind,
    frame: Frame,
    timing: Timing,
}

struct ThreadDpiAwareness(DPI_AWARENESS_CONTEXT);

impl ThreadDpiAwareness {
    fn per_monitor_v2() -> Self {
        Self(unsafe { SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) })
    }
}

impl Drop for ThreadDpiAwareness {
    fn drop(&mut self) {
        unsafe {
            SetThreadDpiAwarenessContext(self.0);
        }
    }
}

fn backend_name(backend: CaptureBackendKind) -> &'static str {
    match backend {
        CaptureBackendKind::Gdi => "GDI",
        CaptureBackendKind::WindowsGraphicsCapture => "WGC",
        CaptureBackendKind::DxgiDuplication => "DXGI",
        CaptureBackendKind::Auto => "Auto",
    }
}

fn duration_ms(duration: Duration) -> f64 {
    duration.as_secs_f64() * 1000.0
}

fn parse_sample_count() -> usize {
    let raw = std::env::var(SAMPLE_COUNT_ENV).unwrap_or_else(|_| DEFAULT_SAMPLE_COUNT.to_string());
    let count = raw.parse::<usize>().unwrap_or_else(|error| {
        panic!("{SAMPLE_COUNT_ENV} must be a positive integer, got {raw:?}: {error}")
    });
    assert!(
        count > 0,
        "{SAMPLE_COUNT_ENV} must be a positive integer, got {count}"
    );
    count
}

fn capture_once(backend: CaptureBackendKind) -> TimedCapture {
    let target = CaptureTarget::PrimaryMonitor;
    let options = CaptureOptions {
        wgc_update_mode: WgcUpdateMode::CompleteOnly,
        ..CaptureOptions::default()
    };

    let total_start = Instant::now();
    let system_start = Instant::now();
    let system = CaptureSystem::builder()
        .with_backend_kind(backend)
        .build()
        .unwrap_or_else(|error| {
            panic!(
                "failed to initialize {} capture system: {error}",
                backend_name(backend)
            )
        });
    let system_build = system_start.elapsed();

    let session_start = Instant::now();
    let mut session = system
        .open_session(target, options)
        .unwrap_or_else(|error| {
            panic!(
                "failed to open {} capture session: {error}",
                backend_name(backend)
            )
        });
    let session_open = session_start.elapsed();

    let capture_start = Instant::now();
    let frame = session.capture_once().unwrap_or_else(|error| {
        panic!(
            "failed to capture one {} frame: {error}",
            backend_name(backend)
        )
    });
    let capture_wall = capture_start.elapsed();
    let total = total_start.elapsed();

    assert_eq!(
        frame.metadata().backend_kind(),
        backend,
        "capture reported the wrong concrete backend"
    );

    TimedCapture {
        backend,
        timing: Timing {
            system_build,
            session_open,
            capture_wall,
            total,
            pipeline: frame.metadata().capture_duration(),
        },
        frame,
    }
}

fn corners(frame: &Frame) -> [[u8; 4]; 4] {
    assert!(
        frame.width() > 0 && frame.height() > 0,
        "{} returned an empty frame",
        backend_name(frame.metadata().backend_kind())
    );

    let width = frame.width() as usize;
    let height = frame.height() as usize;
    let expected_len = width
        .checked_mul(height)
        .and_then(|pixels| pixels.checked_mul(4))
        .expect("frame dimensions overflowed RGBA buffer length");
    let bytes = frame.as_rgba_bytes();
    assert_eq!(
        bytes.len(),
        expected_len,
        "{} returned an RGBA buffer with an unexpected length",
        backend_name(frame.metadata().backend_kind())
    );

    let pixel = |x: usize, y: usize| {
        let offset = (y * width + x) * 4;
        [
            bytes[offset],
            bytes[offset + 1],
            bytes[offset + 2],
            bytes[offset + 3],
        ]
    };

    [
        pixel(0, 0),
        pixel(width - 1, 0),
        pixel(0, height - 1),
        pixel(width - 1, height - 1),
    ]
}

fn assert_matches_gdi(gdi: &TimedCapture, candidate: &TimedCapture, sample: usize) {
    assert_eq!(
        candidate.frame.dimensions(),
        gdi.frame.dimensions(),
        "sample {sample}: {} dimensions differ from GDI",
        backend_name(candidate.backend)
    );

    let gdi_corners = corners(&gdi.frame);
    let candidate_corners = corners(&candidate.frame);
    let names = ["top-left", "top-right", "bottom-left", "bottom-right"];
    for (name, (gdi_pixel, candidate_pixel)) in names
        .iter()
        .zip(gdi_corners.iter().zip(candidate_corners.iter()))
    {
        assert_eq!(
            candidate_pixel,
            gdi_pixel,
            "sample {sample}: {} {name} pixel differs from GDI (candidate={candidate_pixel:?}, gdi={gdi_pixel:?})",
            backend_name(candidate.backend)
        );
    }
}

fn print_timing(sample: usize, capture: &TimedCapture) {
    let pipeline = capture
        .timing
        .pipeline
        .map(duration_ms)
        .map_or_else(|| "n/a".to_string(), |value| format!("{value:.3}"));
    println!(
        "sample={sample} backend={} dimensions={}x{} system_build_ms={:.3} session_open_ms={:.3} capture_wall_ms={:.3} pipeline_ms={} total_ms={:.3}",
        backend_name(capture.backend),
        capture.frame.width(),
        capture.frame.height(),
        duration_ms(capture.timing.system_build),
        duration_ms(capture.timing.session_open),
        duration_ms(capture.timing.capture_wall),
        pipeline,
        duration_ms(capture.timing.total),
    );
}

fn print_summary(backend: CaptureBackendKind, timings: &[Timing]) {
    let average = |value: fn(&Timing) -> Duration| {
        timings
            .iter()
            .map(|timing| value(timing).as_secs_f64())
            .sum::<f64>()
            / timings.len() as f64
    };
    let min_max = |value: fn(&Timing) -> Duration| {
        timings
            .iter()
            .map(value)
            .fold((Duration::MAX, Duration::ZERO), |(min, max), sample| {
                (min.min(sample), max.max(sample))
            })
    };
    let format_stat = |value: fn(&Timing) -> Duration| {
        let average_ms = average(value) * 1000.0;
        let (min, max) = min_max(value);
        format!(
            "avg={average_ms:.3} min={:.3} max={:.3}",
            duration_ms(min),
            duration_ms(max)
        )
    };

    println!(
        "summary backend={} samples={} system_build_ms({}) session_open_ms({}) capture_wall_ms({}) total_ms({})",
        backend_name(backend),
        timings.len(),
        format_stat(|timing| timing.system_build),
        format_stat(|timing| timing.session_open),
        format_stat(|timing| timing.capture_wall),
        format_stat(|timing| timing.total),
    );
}

#[test]
#[ignore = "requires an interactive Windows desktop and GDI, WGC, and DXGI"]
fn cold_single_capture_backend_performance_and_corner_consistency() {
    let _dpi_awareness = ThreadDpiAwareness::per_monitor_v2();
    let sample_count = parse_sample_count();
    let backends = [
        CaptureBackendKind::Gdi,
        CaptureBackendKind::WindowsGraphicsCapture,
        CaptureBackendKind::DxgiDuplication,
    ];
    let mut captures = Vec::with_capacity(backends.len());
    let mut timing_samples: [Vec<Timing>; 3] =
        std::array::from_fn(|_| Vec::with_capacity(sample_count));

    println!(
        "cold single-capture benchmark: samples={sample_count} target=primary-monitor backends=GDI,WGC,DXGI"
    );

    for sample in 1..=sample_count {
        captures.clear();
        for backend in backends {
            let capture = capture_once(backend);
            print_timing(sample, &capture);
            let backend_index = match backend {
                CaptureBackendKind::Gdi => 0,
                CaptureBackendKind::WindowsGraphicsCapture => 1,
                CaptureBackendKind::DxgiDuplication => 2,
                CaptureBackendKind::Auto => unreachable!("benchmark backends are explicit"),
            };
            timing_samples[backend_index].push(capture.timing);
            captures.push(capture);
        }

        let gdi = &captures[0];
        let wgc = &captures[1];
        let dxgi = &captures[2];
        assert_matches_gdi(gdi, wgc, sample);
        assert_matches_gdi(gdi, dxgi, sample);
    }

    for (index, backend) in backends.iter().enumerate() {
        print_summary(*backend, &timing_samples[index]);
    }
}

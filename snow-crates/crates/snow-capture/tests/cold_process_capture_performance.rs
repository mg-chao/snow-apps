#![cfg(target_os = "windows")]

use std::fmt::Write as _;
use std::process::Command;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use snow_capture::backend::CaptureBackendKind;
use snow_capture::frame::Frame;
use snow_capture::{CaptureOptions, CaptureSystem, CaptureTarget, WgcUpdateMode};
use windows::Win32::UI::HiDpi::{
    DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetThreadDpiAwarenessContext,
};

const DEFAULT_SAMPLE_COUNT: usize = 3;
const SAMPLE_COUNT_ENV: &str = "SNOW_CAPTURE_SINGLE_CAPTURE_SAMPLES";
const CHILD_BACKEND_ENV: &str = "SNOW_CAPTURE_COLD_CHILD_BACKEND";
const SHUFFLE_SEED_ENV: &str = "SNOW_CAPTURE_COLD_SEED";
const DIAGNOSTIC_ALLOW_INVALID_ENV: &str = "SNOW_CAPTURE_COLD_ALLOW_INVALID";
const RESULT_MARKER: &str = "SNOW_CAPTURE_COLD_RESULT";
const GRID_COLUMNS: usize = 17;
const GRID_ROWS: usize = 11;
const MAX_GRID_MISMATCH_PERCENT: usize = 10;
const MAX_MEAN_RGB_ERROR: f64 = 12.0;

#[derive(Clone, Copy, Debug)]
struct Timing {
    system_build: Duration,
    session_open: Duration,
    capture_wall: Duration,
    api_total: Duration,
    session_drop: Duration,
    system_drop: Duration,
    helper_equivalent_total: Duration,
    process_wall: Duration,
    pipeline: Option<Duration>,
}

#[derive(Debug)]
struct ChildCapture {
    backend: CaptureBackendKind,
    width: u32,
    height: u32,
    timing: Timing,
    grid: Vec<[u8; 4]>,
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
        CaptureBackendKind::Auto => "AUTO",
    }
}

fn parse_backend(value: &str) -> CaptureBackendKind {
    match value {
        "GDI" => CaptureBackendKind::Gdi,
        "WGC" => CaptureBackendKind::WindowsGraphicsCapture,
        "DXGI" => CaptureBackendKind::DxgiDuplication,
        other => panic!("unsupported cold-capture child backend {other:?}"),
    }
}

fn duration_ms(duration: Duration) -> f64 {
    duration.as_secs_f64() * 1_000.0
}

fn parse_positive_usize(name: &str, default: usize) -> usize {
    let raw = std::env::var(name).unwrap_or_else(|_| default.to_string());
    let value = raw
        .parse::<usize>()
        .unwrap_or_else(|error| panic!("{name} must be a positive integer, got {raw:?}: {error}"));
    assert!(value > 0, "{name} must be positive, got {value}");
    value
}

fn checked_duration_from_nanos(value: &str, field: &str) -> Duration {
    let nanos = value
        .parse::<u64>()
        .unwrap_or_else(|error| panic!("invalid {field} value {value:?}: {error}"));
    Duration::from_nanos(nanos)
}

fn duration_nanos(duration: Duration) -> u64 {
    duration
        .as_nanos()
        .try_into()
        .expect("cold-capture duration exceeded u64 nanoseconds")
}

fn grid_coordinate(index: usize, count: usize, extent: usize) -> usize {
    // Avoid the outermost pixels while spreading samples over the complete image.
    ((index + 1) * extent / (count + 1)).min(extent.saturating_sub(1))
}

fn sample_grid(frame: &Frame) -> Vec<[u8; 4]> {
    let width = frame.width() as usize;
    let height = frame.height() as usize;
    assert!(width > 0 && height > 0, "capture returned an empty frame");

    let expected_len = width
        .checked_mul(height)
        .and_then(|pixels| pixels.checked_mul(4))
        .expect("frame dimensions overflowed RGBA buffer length");
    let bytes = frame.as_rgba_bytes();
    assert_eq!(
        bytes.len(),
        expected_len,
        "capture returned an RGBA buffer with an unexpected length"
    );

    let mut grid = Vec::with_capacity(GRID_COLUMNS * GRID_ROWS);
    for row in 0..GRID_ROWS {
        let y = grid_coordinate(row, GRID_ROWS, height);
        for column in 0..GRID_COLUMNS {
            let x = grid_coordinate(column, GRID_COLUMNS, width);
            let offset = (y * width + x) * 4;
            grid.push([
                bytes[offset],
                bytes[offset + 1],
                bytes[offset + 2],
                bytes[offset + 3],
            ]);
        }
    }
    assert!(
        grid.iter().all(|pixel| pixel[3] == 255),
        "capture returned non-opaque alpha in the validation grid"
    );
    grid
}

fn encode_grid(grid: &[[u8; 4]]) -> String {
    let mut encoded = String::with_capacity(grid.len() * 8);
    for pixel in grid {
        write!(
            encoded,
            "{:02x}{:02x}{:02x}{:02x}",
            pixel[0], pixel[1], pixel[2], pixel[3]
        )
        .expect("writing to a String cannot fail");
    }
    encoded
}

fn decode_grid(encoded: &str) -> Vec<[u8; 4]> {
    assert_eq!(
        encoded.len(),
        GRID_COLUMNS * GRID_ROWS * 8,
        "cold child returned an invalid validation-grid length"
    );
    encoded
        .as_bytes()
        .chunks_exact(8)
        .map(|pixel| {
            let pixel = std::str::from_utf8(pixel).expect("validation grid was not ASCII");
            [
                u8::from_str_radix(&pixel[0..2], 16).expect("invalid red grid channel"),
                u8::from_str_radix(&pixel[2..4], 16).expect("invalid green grid channel"),
                u8::from_str_radix(&pixel[4..6], 16).expect("invalid blue grid channel"),
                u8::from_str_radix(&pixel[6..8], 16).expect("invalid alpha grid channel"),
            ]
        })
        .collect()
}

fn capture_in_child(backend: CaptureBackendKind) -> (Frame, Timing) {
    let target = CaptureTarget::PrimaryMonitor;
    let options = CaptureOptions {
        wgc_update_mode: WgcUpdateMode::CompleteOnly,
        ..CaptureOptions::default()
    };

    let api_start = Instant::now();
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
    let api_total = api_start.elapsed();

    assert_eq!(
        frame.metadata().backend_kind(),
        backend,
        "capture reported the wrong concrete backend"
    );
    let pipeline = frame.metadata().capture_duration();
    let session_drop_started = Instant::now();
    drop(session);
    let session_drop = session_drop_started.elapsed();
    let system_drop_started = Instant::now();
    drop(system);
    let system_drop = system_drop_started.elapsed();
    let helper_equivalent_total = api_start.elapsed();
    (
        frame,
        Timing {
            system_build,
            session_open,
            capture_wall,
            api_total,
            session_drop,
            system_drop,
            helper_equivalent_total,
            process_wall: Duration::ZERO,
            pipeline,
        },
    )
}

fn child_result_line(backend: CaptureBackendKind, frame: &Frame, timing: Timing) -> String {
    let pipeline = timing
        .pipeline
        .map(duration_nanos)
        .map_or_else(|| "none".to_owned(), |value| value.to_string());
    format!(
        "{RESULT_MARKER} backend={} width={} height={} system_build_ns={} session_open_ns={} capture_wall_ns={} api_total_ns={} session_drop_ns={} system_drop_ns={} helper_equivalent_total_ns={} pipeline_ns={} grid={}",
        backend_name(backend),
        frame.width(),
        frame.height(),
        duration_nanos(timing.system_build),
        duration_nanos(timing.session_open),
        duration_nanos(timing.capture_wall),
        duration_nanos(timing.api_total),
        duration_nanos(timing.session_drop),
        duration_nanos(timing.system_drop),
        duration_nanos(timing.helper_equivalent_total),
        pipeline,
        encode_grid(&sample_grid(frame)),
    )
}

fn parse_child_result(output: &str, process_wall: Duration) -> ChildCapture {
    let marker_offset = output
        .find(RESULT_MARKER)
        .unwrap_or_else(|| panic!("cold child did not emit {RESULT_MARKER}:\n{output}"));
    let line = output[marker_offset..]
        .lines()
        .next()
        .expect("cold child result marker was empty");
    let mut backend = None;
    let mut width = None;
    let mut height = None;
    let mut system_build = None;
    let mut session_open = None;
    let mut capture_wall = None;
    let mut api_total = None;
    let mut session_drop = None;
    let mut system_drop = None;
    let mut helper_equivalent_total = None;
    let mut pipeline = None;
    let mut grid = None;

    for field in line.split_ascii_whitespace().skip(1) {
        let (name, value) = field
            .split_once('=')
            .unwrap_or_else(|| panic!("malformed cold child field {field:?}"));
        match name {
            "backend" => backend = Some(parse_backend(value)),
            "width" => width = Some(value.parse::<u32>().expect("invalid child width")),
            "height" => height = Some(value.parse::<u32>().expect("invalid child height")),
            "system_build_ns" => {
                system_build = Some(checked_duration_from_nanos(value, name));
            }
            "session_open_ns" => {
                session_open = Some(checked_duration_from_nanos(value, name));
            }
            "capture_wall_ns" => {
                capture_wall = Some(checked_duration_from_nanos(value, name));
            }
            "api_total_ns" => api_total = Some(checked_duration_from_nanos(value, name)),
            "session_drop_ns" => {
                session_drop = Some(checked_duration_from_nanos(value, name));
            }
            "system_drop_ns" => system_drop = Some(checked_duration_from_nanos(value, name)),
            "helper_equivalent_total_ns" => {
                helper_equivalent_total = Some(checked_duration_from_nanos(value, name));
            }
            "pipeline_ns" => {
                pipeline =
                    Some((value != "none").then(|| checked_duration_from_nanos(value, name)));
            }
            "grid" => grid = Some(decode_grid(value)),
            other => panic!("unknown cold child result field {other:?}"),
        }
    }

    ChildCapture {
        backend: backend.expect("cold child omitted backend"),
        width: width.expect("cold child omitted width"),
        height: height.expect("cold child omitted height"),
        timing: Timing {
            system_build: system_build.expect("cold child omitted system_build_ns"),
            session_open: session_open.expect("cold child omitted session_open_ns"),
            capture_wall: capture_wall.expect("cold child omitted capture_wall_ns"),
            api_total: api_total.expect("cold child omitted api_total_ns"),
            session_drop: session_drop.expect("cold child omitted session_drop_ns"),
            system_drop: system_drop.expect("cold child omitted system_drop_ns"),
            helper_equivalent_total: helper_equivalent_total
                .expect("cold child omitted helper_equivalent_total_ns"),
            process_wall,
            pipeline: pipeline.expect("cold child omitted pipeline_ns"),
        },
        grid: grid.expect("cold child omitted validation grid"),
    }
}

fn spawn_cold_child(backend: CaptureBackendKind) -> ChildCapture {
    let executable = std::env::current_exe().expect("failed to locate cold benchmark test binary");
    let started_at = Instant::now();
    let output = Command::new(executable)
        .args([
            "--exact",
            "cold_single_capture_child",
            "--ignored",
            "--nocapture",
            "--test-threads=1",
        ])
        .env(CHILD_BACKEND_ENV, backend_name(backend))
        .output()
        .unwrap_or_else(|error| {
            panic!(
                "failed to spawn {} cold child: {error}",
                backend_name(backend)
            )
        });
    let process_wall = started_at.elapsed();
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    if !stdout.is_empty() {
        print!("{stdout}");
    }
    if !stderr.is_empty() {
        eprint!("{stderr}");
    }
    assert!(
        output.status.success(),
        "{} cold child failed with {}\nstdout:\n{}\nstderr:\n{}",
        backend_name(backend),
        output.status,
        stdout,
        stderr,
    );
    let capture = parse_child_result(&stdout, process_wall);
    assert_eq!(
        capture.backend, backend,
        "cold child reported wrong backend"
    );
    capture
}

fn shuffle_backends(backends: &mut [CaptureBackendKind], mut state: u64) {
    for index in (1..backends.len()).rev() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        backends.swap(index, state as usize % (index + 1));
    }
}

fn seed() -> u64 {
    std::env::var(SHUFFLE_SEED_ENV)
        .ok()
        .map(|raw| {
            raw.parse::<u64>()
                .unwrap_or_else(|error| panic!("invalid {SHUFFLE_SEED_ENV} {raw:?}: {error}"))
        })
        .unwrap_or_else(|| {
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .expect("system clock is before the Unix epoch")
                .as_nanos() as u64
        })
}

fn assert_matches_gdi(gdi: &ChildCapture, candidate: &ChildCapture, sample: usize) {
    assert_eq!(
        (candidate.width, candidate.height),
        (gdi.width, gdi.height),
        "sample {sample}: {} dimensions differ from GDI",
        backend_name(candidate.backend)
    );
    assert_eq!(candidate.grid.len(), gdi.grid.len());

    let mut mismatches = 0usize;
    let mut rgb_error = 0u64;
    for (actual, expected) in candidate.grid.iter().zip(&gdi.grid) {
        let channel_error = actual[..3]
            .iter()
            .zip(&expected[..3])
            .map(|(&lhs, &rhs)| u64::from(lhs.abs_diff(rhs)))
            .sum::<u64>();
        rgb_error += channel_error;
        if channel_error != 0 {
            mismatches += 1;
        }
    }

    let mismatch_percent = mismatches * 100 / candidate.grid.len();
    let mean_rgb_error = rgb_error as f64 / (candidate.grid.len() * 3) as f64;
    println!(
        "sample={sample} validation={} reference=GDI grid={}x{} mismatches={}/{} mismatch_percent={} mean_rgb_error={mean_rgb_error:.3}",
        backend_name(candidate.backend),
        GRID_COLUMNS,
        GRID_ROWS,
        mismatches,
        candidate.grid.len(),
        mismatch_percent,
    );
    let valid =
        mismatch_percent <= MAX_GRID_MISMATCH_PERCENT && mean_rgb_error <= MAX_MEAN_RGB_ERROR;
    if !valid && std::env::var_os(DIAGNOSTIC_ALLOW_INVALID_ENV).is_some() {
        eprintln!(
            "sample={sample} diagnostic_invalid_backend={} mismatches={mismatches}/{} mean_rgb_error={mean_rgb_error:.3}",
            backend_name(candidate.backend),
            candidate.grid.len(),
        );
        return;
    }
    assert!(
        valid,
        "sample {sample}: {} differs materially from GDI across the interior validation grid (mismatches={mismatches}/{}, mean_rgb_error={mean_rgb_error:.3})",
        backend_name(candidate.backend),
        candidate.grid.len(),
    );
}

fn print_timing(sample: usize, capture: &ChildCapture) {
    let pipeline = capture
        .timing
        .pipeline
        .map(duration_ms)
        .map_or_else(|| "n/a".to_owned(), |value| format!("{value:.3}"));
    println!(
        "sample={sample} backend={} dimensions={}x{} system_build_ms={:.3} session_open_ms={:.3} capture_wall_ms={:.3} pipeline_ms={} api_total_ms={:.3} session_drop_ms={:.3} system_drop_ms={:.3} helper_equivalent_total_ms={:.3} process_wall_ms={:.3}",
        backend_name(capture.backend),
        capture.width,
        capture.height,
        duration_ms(capture.timing.system_build),
        duration_ms(capture.timing.session_open),
        duration_ms(capture.timing.capture_wall),
        pipeline,
        duration_ms(capture.timing.api_total),
        duration_ms(capture.timing.session_drop),
        duration_ms(capture.timing.system_drop),
        duration_ms(capture.timing.helper_equivalent_total),
        duration_ms(capture.timing.process_wall),
    );
}

fn print_summary(backend: CaptureBackendKind, timings: &[Timing]) {
    let stat = |value: fn(&Timing) -> Duration| {
        let mut samples = timings.iter().map(value).collect::<Vec<_>>();
        samples.sort_unstable();
        let mean = samples.iter().map(Duration::as_secs_f64).sum::<f64>() / samples.len() as f64;
        let median = samples[samples.len() / 2];
        format!(
            "mean={:.3} median={:.3} min={:.3} max={:.3}",
            mean * 1_000.0,
            duration_ms(median),
            duration_ms(*samples.first().unwrap()),
            duration_ms(*samples.last().unwrap()),
        )
    };

    println!(
        "summary backend={} samples={} system_build_ms({}) session_open_ms({}) capture_wall_ms({}) api_total_ms({}) session_drop_ms({}) system_drop_ms({}) helper_equivalent_total_ms({}) process_wall_ms({})",
        backend_name(backend),
        timings.len(),
        stat(|timing| timing.system_build),
        stat(|timing| timing.session_open),
        stat(|timing| timing.capture_wall),
        stat(|timing| timing.api_total),
        stat(|timing| timing.session_drop),
        stat(|timing| timing.system_drop),
        stat(|timing| timing.helper_equivalent_total),
        stat(|timing| timing.process_wall),
    );
}

#[test]
#[ignore = "launched by the parent cold-process benchmark"]
fn cold_single_capture_child() {
    let backend = std::env::var(CHILD_BACKEND_ENV)
        .map(|value| parse_backend(&value))
        .expect("cold child backend environment variable is missing");
    let _dpi_awareness = ThreadDpiAwareness::per_monitor_v2();
    let (frame, timing) = capture_in_child(backend);
    println!("{}", child_result_line(backend, &frame, timing));
}

#[test]
#[ignore = "requires an interactive Windows desktop"]
fn gdi_primary_fast_path_transitions_to_canonical_monitor_identity() {
    let _dpi_awareness = ThreadDpiAwareness::per_monitor_v2();
    let system = CaptureSystem::builder()
        .with_backend_kind(CaptureBackendKind::Gdi)
        .build()
        .expect("failed to build GDI capture system");

    let mut primary_session = system
        .open_session(CaptureTarget::PrimaryMonitor, CaptureOptions::default())
        .expect("failed to open semantic-primary GDI session");
    let primary_frame = primary_session
        .capture_once()
        .expect("semantic-primary GDI capture failed");

    let canonical_primary = system
        .primary_monitor()
        .expect("canonical primary-monitor discovery failed");
    let mut explicit_session = system
        .open_session(
            CaptureTarget::Monitor(canonical_primary),
            CaptureOptions::default(),
        )
        .expect("failed to open canonical-monitor GDI session");
    let explicit_frame = explicit_session
        .capture_once()
        .expect("canonical-monitor GDI capture failed");

    assert_eq!(
        primary_frame.dimensions(),
        explicit_frame.dimensions(),
        "semantic and canonical primary captures used different geometry"
    );
    assert_eq!(
        primary_frame.metadata().backend_kind(),
        CaptureBackendKind::Gdi
    );
    assert_eq!(
        explicit_frame.metadata().backend_kind(),
        CaptureBackendKind::Gdi
    );
}

#[test]
#[ignore = "requires an interactive Windows desktop and GDI, WGC, and DXGI"]
fn cold_process_single_capture_performance_and_grid_consistency() {
    assert!(
        std::env::var_os(CHILD_BACKEND_ENV).is_none(),
        "the parent cold benchmark cannot run in child mode"
    );
    let sample_count = parse_positive_usize(SAMPLE_COUNT_ENV, DEFAULT_SAMPLE_COUNT);
    let seed = seed();
    let canonical_backends = [
        CaptureBackendKind::Gdi,
        CaptureBackendKind::WindowsGraphicsCapture,
        CaptureBackendKind::DxgiDuplication,
    ];
    let mut timing_samples: [Vec<Timing>; 3] =
        std::array::from_fn(|_| Vec::with_capacity(sample_count));

    println!(
        "process-cold single-capture benchmark: samples={sample_count} target=primary-monitor seed={seed} validation_grid={}x{}",
        GRID_COLUMNS, GRID_ROWS
    );

    for sample in 1..=sample_count {
        let mut order = canonical_backends;
        shuffle_backends(
            &mut order,
            seed ^ (sample as u64).wrapping_mul(0x9e37_79b9_7f4a_7c15),
        );
        println!(
            "sample={sample} order={}",
            order.map(backend_name).join(",")
        );

        let mut captures = Vec::with_capacity(order.len());
        for backend in order {
            let capture = spawn_cold_child(backend);
            print_timing(sample, &capture);
            let backend_index = canonical_backends
                .iter()
                .position(|candidate| *candidate == backend)
                .unwrap();
            timing_samples[backend_index].push(capture.timing);
            captures.push(capture);
        }

        let gdi = captures
            .iter()
            .find(|capture| capture.backend == CaptureBackendKind::Gdi)
            .expect("sample omitted GDI reference capture");
        for candidate in &captures {
            if candidate.backend != CaptureBackendKind::Gdi {
                assert_matches_gdi(gdi, candidate, sample);
            }
        }
    }

    for (index, backend) in canonical_backends.into_iter().enumerate() {
        print_summary(backend, &timing_samples[index]);
    }
}

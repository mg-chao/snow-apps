//! Realtime recording benchmark simulating snow-shot's live recording path.
//!
//! Drives a real `DirectRecordingSession` (capture stream, low-level mouse and
//! keyboard hooks, overlay compositing, and FFmpeg encoding to a temporary
//! MP4) against a fullscreen window that repaints continuously on the
//! leftmost monitor, while feeding the overlays synthetic input observations
//! through the session's bench API (`bench-synthetic-input` feature). The
//! benchmark never injects OS input: the physical mouse and keyboard stay in
//! the user's hands, and the workload window never takes focus.
//!
//! * `mouse-only`  â€” cursor + mouse trail/click effects; synthetic cursor
//!   movement and clicks, no keyboard overlay.
//! * `all-effects` â€” everything above plus the keyboard overlay, with
//!   synthetic key presses and periodic Ctrl+Shift chords.
//!
//! Detailed metric groups are compile-time gated (see the `bench-*-timing`
//! cargo features); with none enabled only the always-on metrics below are
//! reported. Run via `scripts/run-realtime-recording-perf.ps1`, which uses the
//! `windows-msvc-performance` preset environment. The benchmark covers the
//! leftmost monitor with its workload window while it runs.

#![cfg(windows)]

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::SyncSender;
use std::thread::JoinHandle;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, bail};
use snow_capture::region::MonitorGeometry;
use snow_capture::{CaptureSystem, MonitorLayout};
use snow_recording_export::{ExportFormat, VideoCodec, scaled_output_dimensions};
use snow_recording_model::VideoEncodingSpeed;
#[cfg(any(
    feature = "bench-stage-timing",
    feature = "bench-compositor-timing",
    feature = "bench-pipeline-timing"
))]
use snow_recording_runtime::bench_timing::SampleStats;
use snow_recording_runtime::{
    CaptureBackendKind, DirectRecordingConfig, DirectRecordingSession, KeyboardOverlayConfig,
    RecordingRegion,
};
use windows::Win32::Foundation::{COLORREF, HWND, LPARAM, LRESULT, RECT, WPARAM};
use windows::Win32::Graphics::Dwm::{DWMWA_EXCLUDED_FROM_PEEK, DwmSetWindowAttribute};
use windows::Win32::Graphics::Gdi::{
    BeginPaint, BitBlt, CreateCompatibleBitmap, CreateCompatibleDC, CreateSolidBrush, DeleteDC,
    DeleteObject, Ellipse, EndPaint, FillRect, GetStockObject, HBITMAP, HDC, InvalidateRect,
    NULL_PEN, PAINTSTRUCT, SRCCOPY, SelectObject, SetBkMode, SetTextColor, TextOutW,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::ProcessStatus::{
    K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS, PROCESS_MEMORY_COUNTERS_EX,
};
use windows::Win32::System::Threading::{GetCurrentProcess, GetProcessTimes};
use windows::Win32::UI::HiDpi::{
    DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetThreadDpiAwarenessContext,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CREATESTRUCTW, CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, GWLP_USERDATA,
    GetMessageW, GetWindowLongPtrW, IDC_ARROW, LoadCursorW, MSG, PostMessageW, PostQuitMessage,
    RegisterClassW, SW_SHOW, SetTimer, SetWindowLongPtrW, ShowWindow, WM_CLOSE, WM_DESTROY,
    WM_ERASEBKGND, WM_NCCREATE, WM_PAINT, WM_TIMER, WNDCLASSW, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW,
    WS_EX_TOPMOST, WS_POPUP, WS_VISIBLE,
};
use windows::core::w;

const DEFAULT_DURATION_SECONDS: u64 = 12;
const DEFAULT_WARMUP_SECONDS: u64 = 2;
const DEFAULT_SAMPLES: usize = 1;
const DEFAULT_FPS: u32 = 60;
const DEFAULT_MOVE_INTERVAL_MS: u64 = 16;
const DEFAULT_CLICK_INTERVAL_MS: u64 = 1_000;
const DEFAULT_KEY_INTERVAL_MS: u64 = 333;
const DEFAULT_CHORD_INTERVAL_MS: u64 = 5_000;
const DEFAULT_WINDOW_TIMER_MS: u32 = 15;
const MIB: f64 = 1024.0 * 1024.0;

// Effect colors mirror the palette the app exposes for these overlays; a zero
// alpha would disable an effect, so the benchmark uses fully opaque values.
const TRAIL_RGBA: [u8; 4] = [255, 85, 0, 255];
const CLICK_RGBA: [u8; 4] = [255, 0, 0, 255];
const KEYBOARD_BACKGROUND_RGBA: [u8; 4] = [0, 0, 0, 204];
const KEYBOARD_TEXT_RGBA: [u8; 4] = [255, 255, 255, 255];
const KEYBOARD_BORDER_RGBA: [u8; 4] = [255, 255, 255, 128];
const TRAIL_DURATION_MS: u64 = 500;

const WORKLOAD_TIMER_ID: usize = 1;
static WORKLOAD_PUBLISHED: AtomicU64 = AtomicU64::new(0);
static WORKLOAD_FAILURES: AtomicU64 = AtomicU64::new(0);
static WORKLOAD_PUBLICATIONS: std::sync::Mutex<Vec<(u64, Instant)>> =
    std::sync::Mutex::new(Vec::new());

#[derive(Clone, Copy)]
enum Clarity {
    Native,
    Fixed(u32, u32),
}

impl Clarity {
    fn parse(value: &str) -> Result<Self> {
        match value.to_ascii_lowercase().as_str() {
            "native" => Ok(Self::Native),
            "4k" => Ok(Self::Fixed(3840, 2160)),
            "2k" => Ok(Self::Fixed(2560, 1440)),
            "1080p" => Ok(Self::Fixed(1920, 1080)),
            "720p" => Ok(Self::Fixed(1280, 720)),
            "480p" => Ok(Self::Fixed(854, 480)),
            other => {
                bail!("unknown clarity '{other}' (expected 4k, 2k, 1080p, 720p, 480p, native)")
            }
        }
    }

    fn maximum_dimensions(self) -> (Option<u32>, Option<u32>) {
        match self {
            Self::Native => (None, None),
            Self::Fixed(width, height) => (Some(width), Some(height)),
        }
    }
}

struct Options {
    region_size: Option<(u32, u32)>,
    duration_seconds: u64,
    warmup_seconds: u64,
    samples: usize,
    fps: u32,
    clarity: Clarity,
    backend: CaptureBackendKind,
    prefer_hardware: bool,
    encode_threads: Option<u8>,
    conversion_threads: u8,
    force_hardware_failure: bool,
    asynchronous: bool,
    automatic_policies: bool,
    partial_composition: bool,
    restoration_only: bool,
    skip_unneeded_cursor: bool,
    half_resize: bool,
    direct_output: bool,
    resize_threads: Option<u8>,
    align_capture: Option<bool>,
    scenario_filter: Option<String>,
    output_directory: PathBuf,
    move_interval_ms: u64,
    click_interval_ms: u64,
    key_interval_ms: u64,
    chord_interval_ms: u64,
    window_timer_ms: u32,
    allow_debug: bool,
    workload: String,
    audio: bool,
}

fn print_usage() {
    println!(
        "Usage: cargo run --release -p snow-recording-runtime --features \
bench-stage-timing,bench-compositor-timing,bench-pipeline-timing \
--example realtime_recording_benchmark -- [options]\n\
  --duration-seconds <n>     measured recording length per sample (default: {DEFAULT_DURATION_SECONDS})\n\
  --encode-threads <n>       override encoder threads; 0 requests automatic worker selection\n\
  --resize-threads <n>       override row workers; 0/1 serial, maximum 4\n\
  --align-capture            experimental acquisition/output clock alignment\n\
  --free-running-capture     disable acquisition/output clock alignment\n\
  --conversion-threads <n>   experimental frame converter: 1, 2, or 4 workers; 0 uses legacy\n\
  --async-encoder            experimental encoder worker with one waiting video frame\n\
  --automatic-policies       opt in to provisional validated-domain policy selection\n\
  --half-resize              experimental exact 2:1 resize specialization\n\
  --direct-output            experimental direct resize when overlays are absent\n\
  --partial-composition      experimental conservative damage updates and restoration\n\
  --restoration-only         restore overlays only while the background is unchanged\n\
  --skip-unneeded-cursor     omit cursor observations when cursor and trail are disabled\n\
  --force-hardware-failure    simulate failure of the preferred hardware encoder\n\
  --warmup-seconds <n>       discarded warmup recording per scenario, 0 disables (default: \
{DEFAULT_WARMUP_SECONDS})\n\
  --samples <n>              measured samples per scenario (default: {DEFAULT_SAMPLES})\n\
  --fps <n>                  capture and output frame rate (default: {DEFAULT_FPS})\n\
  --clarity <kind>           output cap: 4k, 2k, 1080p, 720p, 480p, native (default: 1080p)\n\
  --backend <kind>           capture backend: auto, dxgi, wgc, gdi (default: auto)\n\
  --prefer-hardware          prefer a hardware H.264 encoder\n\
  --scenario <name>          application-default, no-effects, mouse-only, or all-effects\n\
  --workload <name>          continuous, static, or sparse (default: continuous)\n\
  --region-size <WxH>        source region within the leftmost monitor, at least 528x96\n\
  --audio                    include system audio in the recording\n\
  --move-interval-ms <n>     simulated mouse move interval (default: {DEFAULT_MOVE_INTERVAL_MS})\n\
  --click-interval-ms <n>    simulated left click interval (default: {DEFAULT_CLICK_INTERVAL_MS})\n\
  --key-interval-ms <n>      simulated key tap interval, all-effects only (default: \
{DEFAULT_KEY_INTERVAL_MS})\n\
  --chord-interval-ms <n>    simulated Ctrl+Shift chord interval, all-effects only (default: \
{DEFAULT_CHORD_INTERVAL_MS})\n\
  --window-timer-ms <n>      workload window repaint interval (default: {DEFAULT_WINDOW_TIMER_MS})\n\
  --output <directory>       report directory (default: target/perf/realtime-recording)\n\
  --allow-debug              allow a short Debug smoke run"
    );
}

fn parse_args() -> Result<Options> {
    let mut options = Options {
        region_size: None,
        duration_seconds: DEFAULT_DURATION_SECONDS,
        warmup_seconds: DEFAULT_WARMUP_SECONDS,
        samples: DEFAULT_SAMPLES,
        fps: DEFAULT_FPS,
        clarity: Clarity::Fixed(1920, 1080),
        backend: CaptureBackendKind::Auto,
        prefer_hardware: false,
        encode_threads: None,
        conversion_threads: 0,
        force_hardware_failure: false,
        asynchronous: false,
        automatic_policies: false,
        partial_composition: false,
        restoration_only: false,
        skip_unneeded_cursor: false,
        half_resize: false,
        direct_output: false,
        resize_threads: None,
        align_capture: None,
        scenario_filter: None,
        output_directory: PathBuf::from("target/perf/realtime-recording"),
        move_interval_ms: DEFAULT_MOVE_INTERVAL_MS,
        click_interval_ms: DEFAULT_CLICK_INTERVAL_MS,
        key_interval_ms: DEFAULT_KEY_INTERVAL_MS,
        chord_interval_ms: DEFAULT_CHORD_INTERVAL_MS,
        window_timer_ms: DEFAULT_WINDOW_TIMER_MS,
        allow_debug: false,
        workload: "continuous".into(),
        audio: false,
    };
    let args = std::env::args().collect::<Vec<_>>();
    let next_value = |flag: &str, args: &[String], index: &mut usize| -> Result<String> {
        *index += 1;
        let Some(value) = args.get(*index) else {
            bail!("{flag} requires a value");
        };
        Ok(value.clone())
    };
    let mut index = 1usize;
    while index < args.len() {
        match args[index].as_str() {
            "--help" | "-h" => {
                print_usage();
                std::process::exit(0);
            }
            "--duration-seconds" => {
                options.duration_seconds = next_value("--duration-seconds", &args, &mut index)?
                    .parse()
                    .context("--duration-seconds")?;
            }
            "--align-capture" => options.align_capture = Some(true),
            "--resize-threads" => {
                options.resize_threads = Some(
                    next_value("--resize-threads", &args, &mut index)?
                        .parse()
                        .context("--resize-threads")?,
                );
            }
            "--free-running-capture" => options.align_capture = Some(false),
            "--region-size" => {
                let value = next_value("--region-size", &args, &mut index)?;
                let (width, height) = value
                    .split_once('x')
                    .context("--region-size requires WIDTHxHEIGHT")?;
                options.region_size = Some((width.parse()?, height.parse()?));
            }
            "--half-resize" => options.half_resize = true,
            "--direct-output" => options.direct_output = true,
            "--skip-unneeded-cursor" => options.skip_unneeded_cursor = true,
            "--partial-composition" => options.partial_composition = true,
            "--restoration-only" => options.restoration_only = true,
            "--async-encoder" => options.asynchronous = true,
            "--automatic-policies" => options.automatic_policies = true,
            "--force-hardware-failure" => options.force_hardware_failure = true,
            "--conversion-threads" => {
                options.conversion_threads = next_value("--conversion-threads", &args, &mut index)?
                    .parse()
                    .context("--conversion-threads")?;
            }
            "--encode-threads" => {
                options.encode_threads = Some(
                    next_value("--encode-threads", &args, &mut index)?
                        .parse()
                        .context("--encode-threads")?,
                );
            }
            "--warmup-seconds" => {
                options.warmup_seconds = next_value("--warmup-seconds", &args, &mut index)?
                    .parse()
                    .context("--warmup-seconds")?;
            }
            "--samples" => {
                options.samples = next_value("--samples", &args, &mut index)?
                    .parse()
                    .context("--samples")?;
            }
            "--fps" => {
                options.fps = next_value("--fps", &args, &mut index)?
                    .parse()
                    .context("--fps")?;
            }
            "--clarity" => {
                options.clarity = Clarity::parse(&next_value("--clarity", &args, &mut index)?)?;
            }
            "--backend" => {
                options.backend = parse_backend(&next_value("--backend", &args, &mut index)?)?;
            }
            "--prefer-hardware" => options.prefer_hardware = true,
            "--scenario" => {
                options.scenario_filter = Some(next_value("--scenario", &args, &mut index)?);
            }
            "--move-interval-ms" => {
                options.move_interval_ms = next_value("--move-interval-ms", &args, &mut index)?
                    .parse()
                    .context("--move-interval-ms")?;
            }
            "--click-interval-ms" => {
                options.click_interval_ms = next_value("--click-interval-ms", &args, &mut index)?
                    .parse()
                    .context("--click-interval-ms")?;
            }
            "--key-interval-ms" => {
                options.key_interval_ms = next_value("--key-interval-ms", &args, &mut index)?
                    .parse()
                    .context("--key-interval-ms")?;
            }
            "--chord-interval-ms" => {
                options.chord_interval_ms = next_value("--chord-interval-ms", &args, &mut index)?
                    .parse()
                    .context("--chord-interval-ms")?;
            }
            "--window-timer-ms" => {
                options.window_timer_ms = next_value("--window-timer-ms", &args, &mut index)?
                    .parse()
                    .context("--window-timer-ms")?;
            }
            "--output" => {
                options.output_directory =
                    PathBuf::from(next_value("--output", &args, &mut index)?);
            }
            "--allow-debug" => options.allow_debug = true,
            "--workload" => options.workload = next_value("--workload", &args, &mut index)?,
            "--audio" => options.audio = true,
            other => bail!("unknown argument: {other}. Use --help for usage."),
        }
        index += 1;
    }
    if let Some(filter) = options.scenario_filter.as_deref()
        && SCENARIOS.iter().all(|scenario| scenario.name != filter)
    {
        bail!(
            "unknown scenario '{filter}' (expected application-default, no-effects, mouse-only or all-effects)"
        );
    }
    if !["continuous", "static", "sparse"].contains(&options.workload.as_str()) {
        bail!("--workload must be continuous, static, or sparse");
    }
    if options.duration_seconds == 0 {
        bail!("--duration-seconds must be greater than zero");
    }
    if options.samples == 0 {
        bail!("--samples must be greater than zero");
    }
    if options.fps == 0 {
        bail!("--fps must be greater than zero");
    }
    if options.move_interval_ms == 0 {
        bail!("--move-interval-ms must be greater than zero");
    }
    if cfg!(debug_assertions) && !options.allow_debug {
        bail!(
            "realtime recording benchmarks must use Release; pass --allow-debug only for a short \
             smoke run"
        );
    }
    Ok(options)
}

fn parse_backend(value: &str) -> Result<CaptureBackendKind> {
    match value.to_ascii_lowercase().as_str() {
        "auto" => Ok(CaptureBackendKind::Auto),
        "dxgi" => Ok(CaptureBackendKind::DxgiDuplication),
        "wgc" => Ok(CaptureBackendKind::WindowsGraphicsCapture),
        "gdi" => Ok(CaptureBackendKind::Gdi),
        other => bail!("unknown backend '{other}' (expected auto, dxgi, wgc, gdi)"),
    }
}

/// The leftmost monitor of the current device, breaking ties by topmost
/// origin so the choice is deterministic for a given layout.
fn select_recording_monitor(layout: &MonitorLayout) -> Option<&MonitorGeometry> {
    layout
        .monitors
        .iter()
        .min_by_key(|monitor| (monitor.x, monitor.y))
}

struct Scenario {
    mouse_effects: bool,
    name: &'static str,
    keyboard_overlay: bool,
    simulate_keys: bool,
}

const SCENARIOS: [Scenario; 4] = [
    Scenario {
        name: "application-default",
        mouse_effects: false,
        keyboard_overlay: false,
        simulate_keys: false,
    },
    Scenario {
        name: "no-effects",
        mouse_effects: false,
        keyboard_overlay: false,
        simulate_keys: false,
    },
    Scenario {
        name: "mouse-only",
        mouse_effects: true,
        keyboard_overlay: false,
        simulate_keys: false,
    },
    Scenario {
        name: "all-effects",
        mouse_effects: true,
        keyboard_overlay: true,
        simulate_keys: true,
    },
];

#[derive(Clone, Copy, Debug, Default)]
struct InputOutcome {
    moves_sent: u64,
    clicks_sent: u64,
    keys_sent: u64,
    chords_sent: u64,
    send_failures: u64,
}

struct ProcessUsage {
    samples: Vec<(f64, u64, u64)>,
    working_set_start_bytes: u64,
    working_set_peak_bytes: u64,
    private_peak_bytes: u64,
    cpu_percent: f64,
}

struct SampleResult {
    workload: String,
    published_frames: u64,
    workload_failures: u64,
    requested_hardware: bool,
    scenario: &'static str,
    sample: usize,
    duration_seconds: u64,
    measured_seconds: f64,
    decoded_frames: u64,
    fresh_frames: u64,
    unreadable_ids: u64,
    fps: u32,
    region: (u32, u32),
    output: (u32, u32),
    expected_frames: u64,
    encoded_frames: u64,
    coalesced_frames: u64,
    superseded_capture_frames: u64,
    missed_output_slots: u64,
    dropped_capture_frames: u64,
    encoded_audio_frames: u64,
    inserted_silence_frames: u64,
    dropped_audio_frames: u64,
    setup_ms: f64,
    stop_ms: f64,
    output_bytes: u64,
    video_encoder: String,
    used_hardware_video_encoder: bool,
    input: InputOutcome,
    usage: ProcessUsage,
    report: snow_recording_runtime::DirectRecordingReport,
}

fn elapsed_ms(started: Instant) -> f64 {
    started.elapsed().as_secs_f64() * 1_000.0
}

fn main() -> Result<()> {
    let arguments: Vec<_> = std::env::args().collect();
    if arguments.get(1).is_some_and(|arg| arg == "--inspect-media") {
        return inspect_media(Path::new(arguments.get(2).context("missing media file")?));
    }
    if arguments.get(1).is_some_and(|arg| arg == "--inspect-audio") {
        return inspect_audio(Path::new(arguments.get(2).context("missing media file")?));
    }
    if arguments.get(1).is_some_and(|arg| arg == "--inspect") {
        let result = decode_frame_ids(
            Path::new(arguments.get(2).context("missing inspect file")?),
            3840,
            2160,
        )?;
        println!("decoded={result:?}");
        return Ok(());
    }
    let options = parse_args()?;
    print_compiled_metric_groups();

    let _dpi = ThreadDpiAwareness::per_monitor_v2();
    let system = CaptureSystem::builder()
        .build()
        .context("building capture system")?;
    let layout = system.monitor_layout().context("enumerating monitors")?;
    // The leftmost monitor keeps the user's primary display usable while the
    // benchmark covers one screen with its workload window.
    let monitor =
        select_recording_monitor(&layout).context("monitor layout contains no monitors")?;
    let (width, height) = options
        .region_size
        .unwrap_or((monitor.width, monitor.height));
    validate_workload_region(width, height, monitor.width, monitor.height)?;
    // Even dimensions, matching screenRecordingCompatibleCaptureRegion.
    let region = RecordingRegion::new(monitor.x, monitor.y, width & !1, height & !1);

    println!(
        "region: {}x{} at ({}, {}) on the leftmost monitor; virtual desktop {}x{} at ({}, {})",
        region.width,
        region.height,
        region.x,
        region.y,
        layout.virtual_width,
        layout.virtual_height,
        layout.virtual_left,
        layout.virtual_top,
    );
    let (maximum_width, maximum_height) = options.clarity.maximum_dimensions();
    let (output_width, output_height) = scaled_output_dimensions(
        region.width,
        region.height,
        maximum_width,
        maximum_height,
        ExportFormat::Mp4,
    );
    println!(
        "recording: {} fps, output {}x{}, backend {:?}, {} encoder",
        options.fps,
        output_width,
        output_height,
        options.backend,
        if options.prefer_hardware {
            "hardware-preferred"
        } else {
            "software"
        },
    );

    let timer_ms = match options.workload.as_str() {
        "static" => 0,
        _ => options.window_timer_ms,
    };
    let window = WorkloadWindow::spawn(region, timer_ms, options.workload == "sparse")?;
    // Let the first frames reach the compositor before capturing or injecting.
    std::thread::sleep(Duration::from_millis(750));

    fs::create_dir_all(&options.output_directory).with_context(|| {
        format!(
            "failed to create benchmark output directory {}",
            options.output_directory.display()
        )
    })?;

    fs::write(
        options.output_directory.join("run-metadata.txt"),
        format!(
            "schema_version=10\nrevision={}\nrelease={}\nbackend={:?}\nworkload={}\naudio={}\nsynthetic_input={}\nfeatures=stage:{},compose:{},pipeline:{}\nencode_threads={}\nencode_threads_override={:?}\nalign_capture_override={:?}\nresize_threads_override={:?}\n",
            option_env!("SNOW_BENCH_REVISION").unwrap_or("unknown"),
            !cfg!(debug_assertions),
            options.backend,
            options.workload,
            options.audio,
            true,
            cfg!(feature = "bench-stage-timing"),
            cfg!(feature = "bench-compositor-timing"),
            cfg!(feature = "bench-pipeline-timing"),
            options.encode_threads.unwrap_or(0),
            options.encode_threads,
            options.align_capture,
            options.resize_threads
        ),
    )?;
    let mut rows = Vec::new();
    let mut validation_errors = Vec::new();
    for scenario in SCENARIOS.iter().filter(|scenario| {
        options
            .scenario_filter
            .as_deref()
            .is_none_or(|filter| filter == scenario.name)
    }) {
        if options.warmup_seconds > 0 {
            println!(
                "[{}] warming up for {} s",
                scenario.name, options.warmup_seconds
            );
            let warmup = run_sample(&options, scenario, region, &window, 0, true)?;
            if let Err(error) = validate_sample(&warmup, true) {
                fs::write(
                    options.output_directory.join("validation-errors.txt"),
                    format!("{error:#}\n"),
                )?;
                return Err(error);
            }
        }
        for sample in 0..options.samples {
            let result = run_sample(&options, scenario, region, &window, sample, false)?;
            // Print first so a gate failure still shows the sample's numbers.
            print_sample(&result);
            let validation = validate_sample(&result, false);
            rows.push(result);
            write_summary_csv(&options.output_directory, &rows)?;
            #[cfg(any(
                feature = "bench-stage-timing",
                feature = "bench-compositor-timing",
                feature = "bench-pipeline-timing"
            ))]
            write_stages_csv(&options.output_directory, &rows)?;
            if let Err(error) = validation {
                eprintln!("{error:#}");
                validation_errors.push(format!("{error:#}"));
            }
        }
    }
    drop(window);

    let summary_path = write_summary_csv(&options.output_directory, &rows)?;
    println!("Wrote {}", summary_path.display());
    #[cfg(any(
        feature = "bench-stage-timing",
        feature = "bench-compositor-timing",
        feature = "bench-pipeline-timing"
    ))]
    {
        let stages_path = write_stages_csv(&options.output_directory, &rows)?;
        println!("Wrote {}", stages_path.display());
    }
    fs::write(
        options.output_directory.join("validation-errors.txt"),
        validation_errors.join("\n"),
    )?;
    if !validation_errors.is_empty() {
        bail!("{}", validation_errors.join("; "));
    }
    Ok(())
}

fn inspect_media(path: &Path) -> Result<()> {
    use ffmpeg_next as ffmpeg;
    ffmpeg::init()?;
    let mut input = ffmpeg::format::input(path)?;
    let streams: Vec<_> = input
        .streams()
        .map(|stream| {
            (
                stream.index(),
                format!("{:?}", stream.parameters().medium()),
                format!("{:?}", stream.parameters().id()),
                stream.time_base(),
                stream.start_time(),
                stream.duration(),
            )
        })
        .collect();
    let mut packets = BTreeMap::new();
    for (stream, packet) in input.packets() {
        let stats = packets
            .entry(stream.index())
            .or_insert((0u64, None::<i64>, None::<i64>, 0u64));
        stats.0 += 1;
        if let Some(dts) = packet.dts() {
            if stats.1.is_some_and(|previous| dts < previous) {
                stats.3 += 1;
            }
            stats.1 = Some(dts);
        }
        if let Some(pts) = packet.pts() {
            let end = pts.saturating_add(packet.duration());
            stats.2 = Some(stats.2.map_or(end, |previous| previous.max(end)));
        }
    }
    println!(
        "stream,kind,codec,time_base_numerator,time_base_denominator,start_seconds,duration_seconds,packets,last_packet_end_seconds,dts_regressions"
    );
    for (index, kind, codec, base, start, duration) in streams {
        let scale = f64::from(base.numerator()) / f64::from(base.denominator());
        let stats = packets.get(&index).copied().unwrap_or_default();
        println!(
            "{index},{kind},{codec},{},{},{:.6},{:.6},{},{:.6},{}",
            base.numerator(),
            base.denominator(),
            start as f64 * scale,
            duration as f64 * scale,
            stats.0,
            stats.2.unwrap_or(0) as f64 * scale,
            stats.3
        );
    }
    Ok(())
}

#[derive(Debug)]
struct AudioInspection {
    sample_values: u64,
    nonzero_values: u64,
    rms: f64,
    peak: f64,
    longest_silent_frames: u64,
    start_seconds: f64,
    end_seconds: f64,
    discontinuities: u64,
}

fn inspect_audio(path: &Path) -> Result<()> {
    let result = read_audio(path)?;
    println!(
        "sample_values,nonzero_values,rms,peak,longest_silent_frames,start_seconds,end_seconds,discontinuities"
    );
    println!(
        "{},{},{:.9},{:.9},{},{:.9},{:.9},{}",
        result.sample_values,
        result.nonzero_values,
        result.rms,
        result.peak,
        result.longest_silent_frames,
        result.start_seconds,
        result.end_seconds,
        result.discontinuities
    );
    Ok(())
}

fn read_audio(path: &Path) -> Result<AudioInspection> {
    let path = path.to_owned();
    std::thread::spawn(move || read_audio_mta(&path))
        .join()
        .map_err(|_| anyhow::anyhow!("audio inspection worker panicked"))?
}

fn read_audio_mta(path: &Path) -> Result<AudioInspection> {
    use std::os::windows::ffi::OsStrExt;
    use windows::Win32::Media::MediaFoundation::*;
    use windows::Win32::System::Com::{COINIT_MULTITHREADED, CoInitializeEx, CoUninitialize};
    struct Apartment;
    impl Drop for Apartment {
        fn drop(&mut self) {
            unsafe {
                CoUninitialize();
            }
        }
    }
    struct Foundation;
    impl Drop for Foundation {
        fn drop(&mut self) {
            unsafe {
                let _ = MFShutdown();
            }
        }
    }
    // The deployed FFmpeg intentionally has no AAC decoder. Use the installed
    // Windows decoder only for offline validation, on its own COM apartment.
    unsafe {
        CoInitializeEx(None, COINIT_MULTITHREADED).ok()?;
        let _apartment = Apartment;
        MFStartup(MF_VERSION, MFSTARTUP_LITE)?;
        let _foundation = Foundation;
        let wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        let source = MFCreateSourceReaderFromURL(windows::core::PCWSTR(wide.as_ptr()), None)?;
        let audio = MF_SOURCE_READER_FIRST_AUDIO_STREAM.0 as u32;
        source.SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS.0 as u32, false)?;
        source.SetStreamSelection(audio, true)?;
        let requested = MFCreateMediaType()?;
        requested.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Audio)?;
        requested.SetGUID(&MF_MT_SUBTYPE, &MFAudioFormat_PCM)?;
        requested.SetUINT32(&MF_MT_AUDIO_BITS_PER_SAMPLE, 16)?;
        source.SetCurrentMediaType(audio, None, &requested)?;
        let format = source.GetCurrentMediaType(audio)?;
        let channels = format.GetUINT32(&MF_MT_AUDIO_NUM_CHANNELS)? as usize;
        let rate = format.GetUINT32(&MF_MT_AUDIO_SAMPLES_PER_SECOND)?;
        if channels == 0 || rate == 0 || format.GetUINT32(&MF_MT_AUDIO_BITS_PER_SAMPLE)? != 16 {
            bail!("audio inspection requires negotiated 16-bit PCM");
        }
        let mut result = AudioInspection {
            sample_values: 0,
            nonzero_values: 0,
            rms: 0.0,
            peak: 0.0,
            longest_silent_frames: 0,
            start_seconds: 0.0,
            end_seconds: 0.0,
            discontinuities: 0,
        };
        let mut squares = 0.0;
        let mut silent_frames = 0u64;
        loop {
            let mut flags = 0;
            let mut timestamp = 0;
            let mut sample = None;
            source.ReadSample(
                audio,
                0,
                None,
                Some(&mut flags),
                Some(&mut timestamp),
                Some(&mut sample),
            )?;
            if flags & MF_SOURCE_READERF_ERROR.0 as u32 != 0 {
                bail!("audio source reader failed");
            }
            if let Some(sample) = sample {
                let current = source.GetCurrentMediaType(audio)?;
                if current.GetUINT32(&MF_MT_AUDIO_NUM_CHANNELS)? as usize != channels
                    || current.GetUINT32(&MF_MT_AUDIO_SAMPLES_PER_SECOND)? != rate
                    || current.GetUINT32(&MF_MT_AUDIO_BITS_PER_SAMPLE)? != 16
                {
                    bail!("audio format changed during inspection");
                }
                let buffer = sample.ConvertToContiguousBuffer()?;
                let mut data = std::ptr::null_mut();
                let mut length = 0;
                buffer.Lock(&mut data, None, Some(&mut length))?;
                if !(length as usize).is_multiple_of(channels * 2)
                    || (data.is_null() && length != 0)
                {
                    buffer.Unlock()?;
                    bail!("invalid decoded PCM buffer");
                }
                let frames = length as usize / (channels * 2);
                if length != 0 {
                    let bytes = std::slice::from_raw_parts(data, length as usize);
                    for frame in bytes.chunks_exact(channels * 2) {
                        let mut audible = false;
                        for sample in frame.chunks_exact(2) {
                            let value =
                                f64::from(i16::from_le_bytes([sample[0], sample[1]])) / 32768.0;
                            result.sample_values += 1;
                            squares += value * value;
                            result.peak = result.peak.max(value.abs());
                            if value != 0.0 {
                                result.nonzero_values += 1;
                                audible = true;
                            }
                        }
                        silent_frames = if audible { 0 } else { silent_frames + 1 };
                        result.longest_silent_frames =
                            result.longest_silent_frames.max(silent_frames);
                    }
                }
                buffer.Unlock()?;
                let start = timestamp as f64 / 10_000_000.0;
                if result.sample_values == (frames * channels) as u64 {
                    result.start_seconds = start;
                } else if (start - result.end_seconds).abs() > 2.0 / f64::from(rate) {
                    result.discontinuities += 1;
                }
                result.end_seconds = start + frames as f64 / f64::from(rate);
            }
            if flags & MF_SOURCE_READERF_ENDOFSTREAM.0 as u32 != 0 {
                break;
            }
        }
        if result.sample_values == 0 {
            bail!("audio stream decoded no samples");
        }
        result.rms = (squares / result.sample_values as f64).sqrt();
        Ok(result)
    }
}

/// Encoder traces use the first admission as their origin. Composition may
/// precede it; retaining the sign keeps those earlier boundaries distinguishable.
#[cfg(any(test, feature = "bench-pipeline-timing"))]
fn trace_elapsed_ns(at: Instant, origin: Instant) -> i128 {
    if at >= origin {
        at.duration_since(origin).as_nanos() as i128
    } else {
        -(origin.duration_since(at).as_nanos() as i128)
    }
}

fn print_compiled_metric_groups() {
    let group = |enabled: bool| if enabled { "on" } else { "off" };
    println!(
        "metric groups compiled in: capture-stages={} compositor={} pipeline={}",
        group(cfg!(feature = "bench-stage-timing")),
        group(cfg!(feature = "bench-compositor-timing")),
        group(cfg!(feature = "bench-pipeline-timing")),
    );
    if !cfg!(feature = "bench-stage-timing")
        && !cfg!(feature = "bench-compositor-timing")
        && !cfg!(feature = "bench-pipeline-timing")
    {
        println!(
            "note: no bench-*-timing feature enabled; only always-on metrics are reported. Pass \
             --features bench-stage-timing,bench-compositor-timing,bench-pipeline-timing for the \
             detailed breakdowns."
        );
    }
}

fn run_sample(
    options: &Options,
    scenario: &Scenario,
    region: RecordingRegion,
    window: &WorkloadWindow,
    sample: usize,
    warmup: bool,
) -> Result<SampleResult> {
    let output_path = if warmup {
        options
            .output_directory
            .join(format!("{}-warmup.mp4", scenario.name))
    } else {
        options
            .output_directory
            .join(format!("{}-sample-{}.mp4", scenario.name, sample + 1))
    };
    if output_path.is_file() {
        bail!(
            "recording artifact already exists: {}; choose a new output directory",
            output_path.display()
        );
    }

    let (maximum_width, maximum_height) = options.clarity.maximum_dimensions();
    let (output_width, output_height) = scaled_output_dimensions(
        region.width,
        region.height,
        maximum_width,
        maximum_height,
        ExportFormat::Mp4,
    );
    let keyboard = scenario.keyboard_overlay.then(|| KeyboardOverlayConfig {
        keycap_size: 64,
        background_rgba: KEYBOARD_BACKGROUND_RGBA,
        text_rgba: KEYBOARD_TEXT_RGBA,
        border_rgba: KEYBOARD_BORDER_RGBA,
        labels: BTreeMap::new(),
    });
    let duration_seconds = if warmup {
        options.warmup_seconds
    } else {
        options.duration_seconds
    };
    let config = DirectRecordingConfig {
        region,
        capture_backend: options.backend,
        output_path: output_path.clone(),
        format: ExportFormat::Mp4,
        capture_fps: options.fps,
        output_fps: options.fps,
        maximum_width,
        maximum_height,
        codec: VideoCodec::H264,
        preset: VideoEncodingSpeed::VeryFast,
        prefer_hardware_encoder: options.prefer_hardware,
        enable_microphone: false,
        enable_system_audio: options.audio,
        show_cursor: scenario.mouse_effects || scenario.name == "application-default",
        keyboard,
        mouse_trail_rgba: if scenario.mouse_effects {
            TRAIL_RGBA
        } else {
            [0; 4]
        },
        mouse_trail_duration_ms: TRAIL_DURATION_MS,
        mouse_click_rgba: if scenario.mouse_effects {
            CLICK_RGBA
        } else {
            [0; 4]
        },
    };

    let window_started = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs_f64();
    let (usage_running, usage_thread) = spawn_usage_sampler(Duration::from_millis(500));
    let setup_started = Instant::now();
    let mut session = DirectRecordingSession::create(config)?;
    if let Some(threads) = options.encode_threads {
        session.set_encode_threads(threads)?;
    }
    session.set_bench_encoding(options.conversion_threads, options.force_hardware_failure)?;
    session.set_bench_async_encoding(options.asynchronous)?;
    session.set_bench_automatic_policies(options.automatic_policies)?;
    session.set_bench_partial_composition(options.partial_composition)?;
    session.set_bench_restoration_only(options.restoration_only)?;
    session.set_bench_skip_unneeded_cursor(options.skip_unneeded_cursor)?;
    session.set_bench_pixel_paths(options.half_resize, options.direct_output)?;
    if let Some(threads) = options.resize_threads {
        session.set_resize_threads(threads)?;
    }
    if let Some(aligned) = options.align_capture {
        session.set_aligned_capture(aligned)?;
    }
    // Overlay inputs are synthetic observations fed straight to the session;
    // the physical mouse and keyboard are never touched.
    session.set_bench_synthetic_input(true)?;
    session
        .start()
        .context("starting direct recording session")?;
    let setup_ms = elapsed_ms(setup_started);

    WORKLOAD_PUBLICATIONS.lock().unwrap().clear();
    let published_start = WORKLOAD_PUBLISHED.load(Ordering::Acquire);
    let failure_start = WORKLOAD_FAILURES.load(Ordering::Acquire);
    let recording_started = Instant::now();
    if options.workload == "static" {
        // Establish the static image after this session's acquisition and
        // recording clock start. An image painted only before startup may not
        // produce a usable first DXGI presentation. No timer runs afterward.
        window.publish_once()?;
    }
    let input = simulate_input(
        &session,
        &InputPlan {
            move_interval_ms: options.move_interval_ms,
            click_interval_ms: options.click_interval_ms,
            key_interval_ms: options.key_interval_ms,
            chord_interval_ms: options.chord_interval_ms,
            region,
            simulate_keys: scenario.simulate_keys,
            duration: Duration::from_secs(duration_seconds),
        },
    )?;

    let measured_seconds = recording_started.elapsed().as_secs_f64();
    let published_frames = WORKLOAD_PUBLISHED
        .load(Ordering::Acquire)
        .saturating_sub(published_start);
    let workload_failures = WORKLOAD_FAILURES
        .load(Ordering::Acquire)
        .saturating_sub(failure_start);
    let recording_ended = Instant::now();
    let publications = WORKLOAD_PUBLICATIONS.lock().unwrap().clone();
    let stop_started = Instant::now();
    let report = session
        .stop()
        .context("stopping direct recording session")?;
    let stop_ms = elapsed_ms(stop_started);
    let window_stopped = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs_f64();
    // Preserve setup and drain as part of the protected recording window.
    // Offline decoding below does not affect already completed recording work.
    fs::write(
        output_path.with_extension("recording-window.csv"),
        format!(
            "started_unix_seconds,stopped_unix_seconds\n{window_started:.9},{window_stopped:.9}\n"
        ),
    )?;

    usage_running.store(false, Ordering::Release);
    let usage = usage_thread
        .join()
        .map_err(|_| anyhow::anyhow!("usage sampler thread panicked"))??;
    let mut usage_csv = String::from("seconds,working_set_bytes,private_bytes\n");
    for (seconds, working, private) in &usage.samples {
        usage_csv.push_str(&format!("{seconds:.6},{working},{private}\n"));
    }
    fs::write(output_path.with_extension("usage.csv"), usage_csv)?;
    let mut workload_csv = String::from("source_id,publication_seconds,interval_ms\n");
    let mut previous = None;
    for (id, at) in publications {
        if at < recording_started || at > recording_ended {
            continue;
        }
        let interval = previous
            .map(|previous| at.duration_since(previous).as_secs_f64() * 1000.0)
            .unwrap_or(0.0);
        previous = Some(at);
        workload_csv.push_str(&format!(
            "{id},{:.9},{interval:.6}\n",
            at.duration_since(recording_started).as_secs_f64()
        ));
    }
    fs::write(output_path.with_extension("workload.csv"), workload_csv)?;
    fs::write(
        output_path.with_extension("session-report.txt"),
        format!("{report:#?}\n"),
    )?;
    let output_bytes = fs::metadata(&output_path)
        .with_context(|| format!("failed to stat {}", output_path.display()))?
        .len();
    let (decoded_frames, fresh_frames, unreadable_ids) =
        decode_frame_ids(&output_path, region.width, region.height)?;

    Ok(SampleResult {
        workload: options.workload.clone(),
        published_frames,
        workload_failures,
        requested_hardware: options.prefer_hardware,
        scenario: scenario.name,
        sample: sample + 1,
        duration_seconds,
        fps: options.fps,
        region: (region.width, region.height),
        output: (output_width, output_height),
        measured_seconds,
        decoded_frames,
        fresh_frames,
        unreadable_ids,
        expected_frames: duration_seconds * u64::from(options.fps),
        encoded_frames: report.encoded_frames,
        coalesced_frames: report.coalesced_frames,
        superseded_capture_frames: report.superseded_capture_frames,
        missed_output_slots: report.missed_output_slots,
        dropped_capture_frames: report.dropped_capture_frames,
        encoded_audio_frames: report.encoded_audio_frames,
        inserted_silence_frames: report.inserted_silence_frames,
        dropped_audio_frames: report.dropped_audio_frames,
        setup_ms,
        stop_ms,
        output_bytes,
        video_encoder: report.video_encoder.clone(),
        used_hardware_video_encoder: report.used_hardware_video_encoder,
        input,
        usage,
        report,
    })
}

fn validate_sample(result: &SampleResult, warmup: bool) -> Result<()> {
    let label = if warmup {
        format!("{} warmup", result.scenario)
    } else {
        format!("{} sample {}", result.scenario, result.sample)
    };
    if result.workload_failures != 0 || result.published_frames == 0 {
        bail!("{label} workload failed to publish frames");
    }
    if result.unreadable_ids != 0 {
        bail!(
            "{label} has {} unreadable source identifiers; workload was obstructed or pixels were corrupted",
            result.unreadable_ids
        );
    }
    if result.input.send_failures > 0 {
        bail!(
            "{label} failed to inject {} input events",
            result.input.send_failures
        );
    }
    if warmup {
        return Ok(());
    }
    // Static/sparse desktops deliberately retain variable-duration images.
    // Moving-content throughput must count distinct decoded source content.
    let achieved_fps = result.fresh_frames as f64 / result.measured_seconds;
    if result.coalesced_frames > 0 {
        println!(
            "warning: {label} coalesced {} frames",
            result.coalesced_frames
        );
    }
    if result.workload == "continuous" && achieved_fps < f64::from(result.fps) * 0.95 {
        println!(
            "warning: {label} achieved {achieved_fps:.1} fresh fps, below 95% of the {} fps target",
            result.fps
        );
    }
    Ok(())
}

fn print_sample(result: &SampleResult) {
    println!(
        "[{} sample {}] encoded={}/{} coalesced={} dropped={} setup={:.1}ms stop={:.1}ms \
encoder={} output={:.1}MiB",
        result.scenario,
        result.sample,
        result.encoded_frames,
        result.expected_frames,
        result.coalesced_frames,
        result.dropped_capture_frames,
        result.setup_ms,
        result.stop_ms,
        result.video_encoder,
        result.output_bytes as f64 / MIB,
    );
    println!(
        "  input: moves={} clicks={} keys={} chords={} failures={}",
        result.input.moves_sent,
        result.input.clicks_sent,
        result.input.keys_sent,
        result.input.chords_sent,
        result.input.send_failures,
    );
    println!(
        "  usage: working_set={:+.1}MiB private_peak={:.1}MiB cpu={:.1}%",
        (result
            .usage
            .working_set_peak_bytes
            .saturating_sub(result.usage.working_set_start_bytes)) as f64
            / MIB,
        result.usage.private_peak_bytes as f64 / MIB,
        result.usage.cpu_percent,
    );
    #[cfg(any(
        feature = "bench-stage-timing",
        feature = "bench-compositor-timing",
        feature = "bench-pipeline-timing"
    ))]
    print_metric_tables(result);
}

#[cfg(any(
    feature = "bench-stage-timing",
    feature = "bench-compositor-timing",
    feature = "bench-pipeline-timing"
))]
fn print_metric_tables(result: &SampleResult) {
    #[cfg(feature = "bench-stage-timing")]
    if let Some(timings) = result.report.capture_stage_timings.as_ref() {
        print_stage_table("capture stages", &timings.snapshot());
    }
    #[cfg(feature = "bench-compositor-timing")]
    if let Some(timings) = result.report.compositor_timings.as_ref() {
        print_stage_table("compositor stages", &timings.snapshot());
    }
    #[cfg(feature = "bench-pipeline-timing")]
    if let Some(pipeline) = result.report.pipeline.as_ref() {
        print_stage_table(
            "pipeline",
            &BTreeMap::from([
                ("pipeline.queue_dwell", pipeline.queue_dwell),
                ("pipeline.source_age", pipeline.source_age),
                ("pipeline.compose", pipeline.compose),
                ("pipeline.encode_push", pipeline.encode_push),
                ("pipeline.end_to_end", pipeline.end_to_end),
            ]),
        );
        println!(
            "  time_to_first_handoff={} synthetic_overlay_frames={} stream: captured={} \
dropped={} capture_fps={:.1} capture_latency_avg={:.2}ms",
            pipeline
                .time_to_first_handoff
                .map(|value| format!("{:.1}ms", value.as_secs_f64() * 1_000.0))
                .unwrap_or_else(|| "n/a".into()),
            pipeline.synthetic_overlay_frames,
            pipeline.stream_frames_captured,
            pipeline.stream_frames_dropped,
            pipeline.stream_capture_fps,
            pipeline.stream_capture_latency_avg.as_secs_f64() * 1_000.0,
        );
    }
}

#[cfg(any(
    feature = "bench-stage-timing",
    feature = "bench-compositor-timing",
    feature = "bench-pipeline-timing"
))]
fn print_stage_table(title: &str, stages: &BTreeMap<&'static str, SampleStats>) {
    if stages.is_empty() {
        return;
    }
    println!("  [{title}]");
    for (stage, stats) in stages {
        println!(
            "    {stage:<38} n={:<5} p50={:8.3}ms p95={:8.3}ms max={:8.3}ms",
            stats.count,
            stats.p50.as_secs_f64() * 1_000.0,
            stats.p95.as_secs_f64() * 1_000.0,
            stats.max.as_secs_f64() * 1_000.0,
        );
    }
}

fn write_summary_csv(output_directory: &Path, rows: &[SampleResult]) -> Result<PathBuf> {
    let mut cells = Vec::with_capacity(rows.len());
    for row in rows {
        let base_columns: Vec<(String, String)> = vec![
            ("schema_version".into(), "10".into()),
            (
                "requested_video_encoder".into(),
                row.report.requested_video_encoder.clone(),
            ),
            (
                "half_resize".into(),
                u8::from(row.report.half_resize).to_string(),
            ),
            (
                "direct_output".into(),
                u8::from(row.report.direct_output).to_string(),
            ),
            (
                "workload_published_fps".into(),
                format!("{:.6}", row.published_frames as f64 / row.measured_seconds),
            ),
            (
                "workload_failures".into(),
                row.workload_failures.to_string(),
            ),
            (
                "hardware_requested".into(),
                u8::from(row.requested_hardware).to_string(),
            ),
            (
                "hardware_fallback".into(),
                u8::from(row.report.hardware_fallback).to_string(),
            ),
            (
                "gpu_memory_bytes".into(),
                row.report.gpu_memory_bytes.to_string(),
            ),
            (
                "selected_pipeline".into(),
                row.report.selected_pipeline.clone(),
            ),
            (
                "adapter".into(),
                row.report.adapter.clone().unwrap_or_default(),
            ),
            (
                "fallback_reason".into(),
                row.report.fallback_reason.clone().unwrap_or_default(),
            ),
            (
                "fallback_stage".into(),
                row.report.fallback_stage.clone().unwrap_or_default(),
            ),
            (
                "encoder_attempts".into(),
                row.report.encoder_attempts.join(" -> "),
            ),
            (
                "abandoned_video_frames".into(),
                row.report.abandoned_video_frames.to_string(),
            ),
            #[cfg(feature = "bench-pipeline-timing")]
            (
                "cpu_encoder_conversions".into(),
                row.report.encoder_timings.cpu_conversions.to_string(),
            ),
            #[cfg(feature = "bench-pipeline-timing")]
            (
                "gpu_surface_submissions".into(),
                row.report
                    .encoder_timings
                    .gpu_surface_submissions
                    .to_string(),
            ),
            (
                "recovery_count".into(),
                row.report.recovery_count.to_string(),
            ),
            (
                "overlay_upload_bytes".into(),
                row.report.overlay_upload_bytes.to_string(),
            ),
            #[cfg(feature = "bench-pipeline-timing")]
            (
                "captured_pixel_readbacks".into(),
                row.report.pixel_counters.captured_readbacks.to_string(),
            ),
            #[cfg(feature = "bench-pipeline-timing")]
            (
                "captured_host_transfers".into(),
                row.report.pixel_counters.host_transfers.to_string(),
            ),
            #[cfg(feature = "bench-pipeline-timing")]
            (
                "capture_cpu_conversions".into(),
                row.report.pixel_counters.cpu_conversions.to_string(),
            ),
            ("pixel_format".into(), row.report.pixel_format.clone()),
            (
                "effective_encode_threads".into(),
                row.report.effective_encode_threads.to_string(),
            ),
            (
                "effective_resize_threads".into(),
                row.report.resize_threads.to_string(),
            ),
            (
                "effective_conversion_threads".into(),
                row.report
                    .effective_conversion_threads
                    .map(|value| value.to_string())
                    .unwrap_or_default(),
            ),
            (
                "configured_conversion_threads".into(),
                row.report.conversion_threads.to_string(),
            ),
            (
                "conversion_backend".into(),
                row.report.conversion_backend.clone(),
            ),
            (
                "aligned_capture".into(),
                u8::from(row.report.aligned_capture).to_string(),
            ),
            (
                "partial_composition".into(),
                u8::from(row.report.partial_composition).to_string(),
            ),
            (
                "cursor_attachment_requested".into(),
                u8::from(row.report.cursor_attachment_requested).to_string(),
            ),
            (
                "execution_mode".into(),
                if row.report.asynchronous {
                    "asynchronous"
                } else {
                    "synchronous"
                }
                .into(),
            ),
            (
                "queued_video_replacements".into(),
                row.report.queued_video_replacements.to_string(),
            ),
            (
                "restoration_only".into(),
                u8::from(row.report.restoration_only).to_string(),
            ),
            ("capture_backend".into(), row.report.capture_backend.clone()),
            ("scenario".into(), row.scenario.to_string()),
            ("workload".into(), row.workload.clone()),
            (
                "measured_seconds".into(),
                format!("{:.6}", row.measured_seconds),
            ),
            ("decoded_frames".into(), row.decoded_frames.to_string()),
            ("fresh_frames".into(), row.fresh_frames.to_string()),
            ("unreadable_ids".into(), row.unreadable_ids.to_string()),
            (
                "useful_fps".into(),
                format!("{:.6}", row.fresh_frames as f64 / row.measured_seconds),
            ),
            ("sample".into(), row.sample.to_string()),
            ("duration_seconds".into(), row.duration_seconds.to_string()),
            ("fps".into(), row.fps.to_string()),
            ("region_w".into(), row.region.0.to_string()),
            ("region_h".into(), row.region.1.to_string()),
            ("output_w".into(), row.output.0.to_string()),
            ("output_h".into(), row.output.1.to_string()),
            ("expected_frames".into(), row.expected_frames.to_string()),
            ("encoded_frames".into(), row.encoded_frames.to_string()),
            ("coalesced_frames".into(), row.coalesced_frames.to_string()),
            (
                "superseded_capture_frames".into(),
                row.superseded_capture_frames.to_string(),
            ),
            (
                "missed_output_slots".into(),
                row.missed_output_slots.to_string(),
            ),
            (
                "dropped_capture_frames".into(),
                row.dropped_capture_frames.to_string(),
            ),
            ("setup_ms".into(), format!("{:.3}", row.setup_ms)),
            ("stop_ms".into(), format!("{:.3}", row.stop_ms)),
            (
                "achieved_fps".into(),
                format!(
                    "{:.3}",
                    row.encoded_frames as f64 / row.duration_seconds as f64
                ),
            ),
            ("moves_sent".into(), row.input.moves_sent.to_string()),
            ("clicks_sent".into(), row.input.clicks_sent.to_string()),
            ("keys_sent".into(), row.input.keys_sent.to_string()),
            ("chords_sent".into(), row.input.chords_sent.to_string()),
            ("input_failures".into(), row.input.send_failures.to_string()),
            (
                "working_set_delta_mib".into(),
                format!(
                    "{:.3}",
                    (row.usage
                        .working_set_peak_bytes
                        .saturating_sub(row.usage.working_set_start_bytes))
                        as f64
                        / MIB
                ),
            ),
            (
                "private_peak_mib".into(),
                format!("{:.3}", row.usage.private_peak_bytes as f64 / MIB),
            ),
            (
                "cpu_percent".into(),
                format!("{:.3}", row.usage.cpu_percent),
            ),
            (
                "private_plateau_mib".into(),
                format!("{:.3}", {
                    let tail = &row.usage.samples[row.usage.samples.len() * 4 / 5..];
                    tail.iter()
                        .map(|(_, _, bytes)| *bytes as f64 / MIB)
                        .sum::<f64>()
                        / tail.len().max(1) as f64
                }),
            ),
            ("output_bytes".into(), row.output_bytes.to_string()),
            (
                "encoded_audio_frames".into(),
                row.encoded_audio_frames.to_string(),
            ),
            (
                "inserted_silence_frames".into(),
                row.inserted_silence_frames.to_string(),
            ),
            (
                "dropped_audio_frames".into(),
                row.dropped_audio_frames.to_string(),
            ),
            ("video_encoder".into(), row.video_encoder.clone()),
            (
                "used_hardware_video_encoder".into(),
                u8::from(row.used_hardware_video_encoder).to_string(),
            ),
        ];
        #[cfg(feature = "bench-pipeline-timing")]
        let mut columns = base_columns;
        #[cfg(not(feature = "bench-pipeline-timing"))]
        let columns = base_columns;
        #[cfg(feature = "bench-pipeline-timing")]
        if let Some(pipeline) = row.report.pipeline.as_ref() {
            columns.extend([
                (
                    "encoder_copied_bytes".into(),
                    row.report.encoder_timings.copied_bytes.to_string(),
                ),
                (
                    "first_packet_ms".into(),
                    row.report
                        .encoder_timings
                        .first_packet
                        .map(|d| format!("{:.3}", d.as_secs_f64() * 1000.0))
                        .unwrap_or_default(),
                ),
                (
                    "time_to_first_handoff_ms".into(),
                    pipeline
                        .time_to_first_handoff
                        .map(|value| format!("{:.3}", value.as_secs_f64() * 1_000.0))
                        .unwrap_or_default(),
                ),
                (
                    "synthetic_overlay_frames".into(),
                    pipeline.synthetic_overlay_frames.to_string(),
                ),
                (
                    "stream_frames_captured".into(),
                    pipeline.stream_frames_captured.to_string(),
                ),
                (
                    "stream_capture_fps".into(),
                    format!("{:.3}", pipeline.stream_capture_fps),
                ),
                (
                    "stream_capture_latency_avg_ms".into(),
                    format!(
                        "{:.3}",
                        pipeline.stream_capture_latency_avg.as_secs_f64() * 1_000.0
                    ),
                ),
            ]);
        }
        cells.push(columns);
    }
    let mut csv = String::new();
    if let Some(first) = cells.first() {
        csv.push_str(
            &first
                .iter()
                .map(|(name, _)| name.as_str())
                .collect::<Vec<_>>()
                .join(","),
        );
        csv.push('\n');
    }
    for row in &cells {
        csv.push_str(
            &row.iter()
                .map(|(_, value)| value.as_str())
                .collect::<Vec<_>>()
                .join(","),
        );
        csv.push('\n');
    }
    let path = output_directory.join("realtime-recording-benchmark.csv");
    fs::write(&path, csv).with_context(|| format!("failed to write {}", path.display()))?;
    Ok(path)
}

#[cfg(any(
    feature = "bench-stage-timing",
    feature = "bench-compositor-timing",
    feature = "bench-pipeline-timing"
))]
fn write_stages_csv(output_directory: &Path, rows: &[SampleResult]) -> Result<PathBuf> {
    let mut csv = String::from("scenario,sample,group,stage,count,p50_ms,p95_ms,max_ms\n");
    let mut raw = String::from("scenario,sample,group,stage,index,duration_ns\n");
    for row in rows {
        let mut emit_raw = |group: &str, stages: &BTreeMap<&str, Vec<Duration>>| {
            for (stage, samples) in stages {
                for (index, duration) in samples.iter().enumerate() {
                    raw.push_str(&format!(
                        "{},{},{group},{stage},{index},{}\n",
                        row.scenario,
                        row.sample,
                        duration.as_nanos()
                    ));
                }
            }
        };
        #[cfg(feature = "bench-stage-timing")]
        if let Some(timings) = &row.report.capture_stage_timings {
            emit_raw("capture", timings.raw_samples());
        }
        #[cfg(feature = "bench-compositor-timing")]
        if let Some(timings) = &row.report.compositor_timings {
            emit_raw("compositor", timings.raw_samples());
        }
        #[cfg(feature = "bench-pipeline-timing")]
        {
            emit_raw("encoder", &row.report.encoder_timings.stages);
            if let Some(pipeline) = &row.report.pipeline {
                emit_raw("pipeline", &pipeline.raw_stages);
                let mut latencies = String::from("output_pts,capture_to_first_packet_ns\n");
                for (pts, duration) in &pipeline.packet_latencies {
                    latencies.push_str(&format!("{pts},{}\n", duration.as_nanos()));
                }
                fs::write(
                    output_directory.join(format!(
                        "{}-{}-packet-latencies.csv",
                        row.scenario, row.sample
                    )),
                    latencies,
                )?;
            }
        }
        let mut emit = |group: &str, stage: &str, stats: &SampleStats| {
            csv.push_str(&format!(
                "{},{},{},{},{},{:.3},{:.3},{:.3}\n",
                row.scenario,
                row.sample,
                group,
                stage,
                stats.count,
                stats.p50.as_secs_f64() * 1_000.0,
                stats.p95.as_secs_f64() * 1_000.0,
                stats.max.as_secs_f64() * 1_000.0,
            ));
        };
        #[cfg(feature = "bench-pipeline-timing")]
        {
            let mut trace = String::from("kind,index,value\n");
            for (kind, values) in [
                ("submitted_pts", &row.report.encoder_timings.submitted_pts),
                ("encoded_pts", &row.report.encoder_timings.encoded_pts),
            ] {
                for (index, value) in values.iter().enumerate() {
                    trace.push_str(&format!("{kind},{index},{value}\n"));
                }
            }
            let mut packet_trace = String::from("kind,pts,elapsed_ns\n");
            let origin = row
                .report
                .encoder_timings
                .admissions
                .first()
                .map(|(_, at)| *at);
            if let Some(origin) = origin {
                for (kind, events) in [
                    ("admission", &row.report.encoder_timings.admissions),
                    ("submission", &row.report.encoder_timings.submissions),
                    ("packet", &row.report.encoder_timings.packets),
                ] {
                    for (pts, at) in events {
                        packet_trace.push_str(&format!(
                            "{kind},{pts},{}\n",
                            at.saturating_duration_since(origin).as_nanos()
                        ));
                    }
                }
            }
            fs::write(
                output_directory.join(format!("{}-{}-packets.csv", row.scenario, row.sample)),
                packet_trace,
            )?;
            let mut composition_trace = String::from(
                "output_pts,composition_start_elapsed_ns,composition_end_elapsed_ns\n",
            );
            if let (Some(origin), Some(pipeline)) = (origin, &row.report.pipeline) {
                for (pts, started, finished) in &pipeline.compositions {
                    composition_trace.push_str(&format!(
                        "{pts},{},{}\n",
                        trace_elapsed_ns(*started, origin),
                        trace_elapsed_ns(*finished, origin),
                    ));
                }
            }
            fs::write(
                output_directory.join(format!("{}-{}-composition.csv", row.scenario, row.sample)),
                composition_trace,
            )?;
            if let Some(pipeline) = &row.report.pipeline {
                for (sequence, duplicate) in &pipeline.capture_sequences {
                    trace.push_str(&format!("capture,{sequence},{}\n", u8::from(*duplicate)));
                }
                for (sequence, generation) in &pipeline.capture_contents {
                    trace.push_str(&format!(
                        "content_generation,{sequence},{}\n",
                        generation
                            .map(|value| value.to_string())
                            .unwrap_or_default()
                    ));
                }
                let mut sources = String::from(
                    "output_pts,capture_sequence,content_generation,source_active_ns\n",
                );
                for (pts, sequence, generation, source_ns) in &pipeline.output_sources {
                    sources.push_str(&format!("{pts},{sequence},{generation},{source_ns}\n"));
                }
                fs::write(
                    output_directory.join(format!("{}-{}-sources.csv", row.scenario, row.sample)),
                    sources,
                )?;
            }
            fs::write(
                output_directory.join(format!("{}-{}-trace.csv", row.scenario, row.sample)),
                trace,
            )?;
        }
        #[cfg(feature = "bench-stage-timing")]
        if let Some(timings) = row.report.capture_stage_timings.as_ref() {
            for (stage, stats) in timings.snapshot() {
                emit("capture", stage, &stats);
            }
        }
        #[cfg(feature = "bench-pipeline-timing")]
        for (stage, samples) in &row.report.encoder_timings.stages {
            emit(
                "encoder",
                stage,
                &SampleStats::from_samples(&mut samples.clone()),
            );
        }
        #[cfg(feature = "bench-compositor-timing")]
        if let Some(timings) = row.report.compositor_timings.as_ref() {
            for (stage, stats) in timings.snapshot() {
                emit("compositor", stage, &stats);
            }
        }
        #[cfg(feature = "bench-pipeline-timing")]
        if let Some(pipeline) = row.report.pipeline.as_ref() {
            for (stage, stats) in [
                ("pipeline.queue_dwell", pipeline.queue_dwell),
                ("pipeline.source_age", pipeline.source_age),
                ("pipeline.compose", pipeline.compose),
                ("pipeline.encode_push", pipeline.encode_push),
                ("pipeline.end_to_end", pipeline.end_to_end),
            ] {
                emit("pipeline", stage, &stats);
            }
        }
    }
    let path = output_directory.join("realtime-recording-stages.csv");
    fs::write(
        output_directory.join("realtime-recording-stage-samples.csv"),
        raw,
    )?;
    fs::write(&path, csv).with_context(|| format!("failed to write {}", path.display()))?;
    Ok(path)
}

// ---------------------------------------------------------------------------
// Synthetic input
// ---------------------------------------------------------------------------

// Left-side virtual keys used by the simulated Ctrl+Shift chord.
const VK_CONTROL: u16 = 0xA2;
const VK_SHIFT: u16 = 0xA0;

struct InputPlan {
    move_interval_ms: u64,
    click_interval_ms: u64,
    key_interval_ms: u64,
    chord_interval_ms: u64,
    region: RecordingRegion,
    simulate_keys: bool,
    duration: Duration,
}

/// Drives the session's overlay inputs with synthetic observations. Nothing
/// here touches the OS input stack: the physical mouse and keyboard stay in
/// the user's hands for the whole run.
fn simulate_input(session: &DirectRecordingSession, plan: &InputPlan) -> Result<InputOutcome> {
    let mut outcome = InputOutcome::default();
    let started = Instant::now();
    let mut next_move_ms = 0u64;
    let mut next_click_ms = plan.click_interval_ms / 2;
    let mut next_key_ms = plan.key_interval_ms / 2;
    let mut next_chord_ms = plan.chord_interval_ms;
    while started.elapsed() < plan.duration {
        let elapsed_ms = started.elapsed().as_millis() as u64;
        while next_move_ms <= elapsed_ms && outcome.send_failures == 0 {
            let (x, y) = sweep_position(&plan.region, Duration::from_millis(next_move_ms));
            if send_mouse_move(session, x, y, &mut outcome) {
                next_move_ms = next_move_ms.saturating_add(plan.move_interval_ms);
            } else {
                break;
            }
        }
        while next_click_ms <= elapsed_ms && outcome.send_failures == 0 {
            let (x, y) = sweep_position(&plan.region, Duration::from_millis(next_click_ms));
            if send_mouse_click(session, x, y, &mut outcome) {
                next_click_ms = next_click_ms.saturating_add(plan.click_interval_ms);
            } else {
                break;
            }
        }
        if plan.simulate_keys {
            while next_key_ms <= elapsed_ms && outcome.send_failures == 0 {
                if send_key_tap(session, letter_for(outcome.keys_sent), &mut outcome) {
                    next_key_ms = next_key_ms.saturating_add(plan.key_interval_ms);
                } else {
                    break;
                }
            }
            while next_chord_ms <= elapsed_ms && outcome.send_failures == 0 {
                if send_chord(session, letter_for(outcome.keys_sent), &mut outcome) {
                    next_chord_ms = next_chord_ms.saturating_add(plan.chord_interval_ms);
                } else {
                    break;
                }
            }
        }
        if outcome.send_failures != 0 {
            break;
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    Ok(outcome)
}

fn letter_for(keys_sent: u64) -> u16 {
    u16::from(b'A' + (keys_sent % 26) as u8)
}

/// Deterministic Lissajous sweep inside the region with a 10% margin, in
/// region-relative coordinates.
fn sweep_position(region: &RecordingRegion, elapsed: Duration) -> (i32, i32) {
    let seconds = elapsed.as_secs_f64();
    let margin_x = region.width as f64 * 0.1;
    let margin_y = region.height as f64 * 0.1;
    let span_x = (region.width as f64 - 2.0 * margin_x).max(1.0);
    let span_y = (region.height as f64 - 2.0 * margin_y).max(1.0);
    let fx = 0.5 + 0.5 * (std::f64::consts::TAU * seconds / 4.7).sin();
    let fy = 0.5 + 0.5 * (std::f64::consts::TAU * seconds / 3.1).sin();
    let x = margin_x + fx * span_x;
    let y = margin_y + fy * span_y;
    (x.round() as i32, y.round() as i32)
}

fn send_mouse_move(
    session: &DirectRecordingSession,
    x: i32,
    y: i32,
    outcome: &mut InputOutcome,
) -> bool {
    if let Err(error) = session.bench_observe_cursor(x, y) {
        eprintln!("synthetic cursor move failed: {error:#}");
        outcome.send_failures += 1;
        return false;
    }
    outcome.moves_sent += 1;
    true
}

fn send_mouse_click(
    session: &DirectRecordingSession,
    x: i32,
    y: i32,
    outcome: &mut InputOutcome,
) -> bool {
    if let Err(error) = session.bench_observe_click(x, y) {
        eprintln!("synthetic click failed: {error:#}");
        outcome.send_failures += 1;
        return false;
    }
    outcome.clicks_sent += 1;
    true
}

fn send_key_tap(session: &DirectRecordingSession, vk: u16, outcome: &mut InputOutcome) -> bool {
    for down in [true, false] {
        if let Err(error) = session.bench_observe_key(vk, down) {
            eprintln!("synthetic key failed: {error:#}");
            outcome.send_failures += 1;
            return false;
        }
    }
    outcome.keys_sent += 1;
    true
}

fn send_chord(session: &DirectRecordingSession, vk: u16, outcome: &mut InputOutcome) -> bool {
    let sequence = [
        (VK_CONTROL, true),
        (VK_SHIFT, true),
        (vk, true),
        (vk, false),
        (VK_SHIFT, false),
        (VK_CONTROL, false),
    ];
    for (key, down) in sequence {
        if let Err(error) = session.bench_observe_key(key, down) {
            eprintln!("synthetic chord key failed: {error:#}");
            outcome.send_failures += 1;
            return false;
        }
    }
    outcome.chords_sent += 1;
    true
}

#[cfg(test)]
mod synthetic_input_tests {
    use super::*;

    #[test]
    fn composition_trace_preserves_boundaries_before_encoder_admission() {
        let started = Instant::now();
        let composed = started + Duration::from_millis(3);
        let admitted = composed + Duration::from_millis(2);
        assert_eq!(trace_elapsed_ns(started, admitted), -5_000_000);
        assert_eq!(trace_elapsed_ns(composed, admitted), -2_000_000);
        assert_eq!(trace_elapsed_ns(admitted, admitted), 0);
        assert_eq!(
            trace_elapsed_ns(admitted + Duration::from_millis(1), admitted),
            1_000_000
        );
    }

    #[test]
    fn audio_inspection_distinguishes_continuous_aac_signal_from_silence() {
        use snow_recording_export::{
            ExportExecutionMode, SoftwareH264Priority, StreamingAudioConfig, StreamingEncoder,
            StreamingEncoderConfig,
        };
        use snow_recording_model::VideoEncodeConfig;
        let directory = tempfile::tempdir().unwrap();
        for signal in [false, true] {
            let path = directory.path().join(format!("audio-{signal}.mp4"));
            let mut encoder = StreamingEncoder::create(StreamingEncoderConfig {
                output_path: path.clone(),
                format: ExportFormat::Mp4,
                width: 16,
                height: 16,
                fps: 10,
                codec: VideoCodec::H264,
                prefer_hardware_h264: false,
                execution_mode: ExportExecutionMode::SoftwareOnly,
                software_h264_priority: SoftwareH264Priority::X264First,
                video: VideoEncodeConfig {
                    quality: 80,
                    speed: VideoEncodingSpeed::VeryFast,
                },
                encode_threads: 1,
                audio: Some(StreamingAudioConfig {
                    sample_rate_hz: 48_000,
                    channels: 2,
                    bitrate_kbps: 160,
                }),
            })
            .unwrap();
            encoder
                .push_owned_rgba_frame_at_pts(0, vec![128; 16 * 16 * 4])
                .unwrap();
            for block in 0..100u64 {
                let samples: Vec<i16> = (0..960)
                    .map(|value| {
                        if signal {
                            (2000.0 * (std::f64::consts::TAU * (value / 2) as f64 / 48.0).sin())
                                as i16
                        } else {
                            0
                        }
                    })
                    .collect();
                encoder.push_audio_pcm_i16(block * 10, &samples).unwrap();
            }
            encoder.finish_at_pts(10).unwrap();
            let inspection = read_audio(&path).unwrap();
            assert!(inspection.sample_values >= 96_000, "{inspection:?}");
            assert_eq!(inspection.discontinuities, 0, "{inspection:?}");
            assert!(inspection.start_seconds.abs() < 0.03, "{inspection:?}");
            assert!(
                (inspection.end_seconds - 1.0).abs() < 0.03,
                "{inspection:?}"
            );
            if signal {
                assert!(inspection.nonzero_values > 80_000, "{inspection:?}");
                assert!((0.03..0.06).contains(&inspection.rms), "{inspection:?}");
                assert!(inspection.longest_silent_frames < 1024, "{inspection:?}");
            } else {
                assert_eq!(inspection.nonzero_values, 0, "{inspection:?}");
                assert_eq!(inspection.rms, 0.0, "{inspection:?}");
            }
        }
    }

    #[test]
    fn sweep_stays_inside_the_region_with_margin() {
        let region = RecordingRegion::new(-1920, 500, 1920, 1080);
        for ms in (0..20_000u64).step_by(97) {
            let (x, y) = sweep_position(&region, Duration::from_millis(ms));
            assert!(x >= 0 && x < region.width as i32, "x={x} at {ms}ms");
            assert!(y >= 0 && y < region.height as i32, "y={y} at {ms}ms");
        }
    }
}

#[cfg(test)]
mod monitor_selection_tests {
    use super::*;

    fn geometry(index: usize, x: i32, y: i32) -> MonitorGeometry {
        MonitorGeometry {
            monitor: snow_capture::MonitorId::from_name(
                index as isize,
                format!("monitor-{index}"),
                index == 0,
            ),
            x,
            y,
            width: 1920,
            height: 1080,
        }
    }

    fn layout(monitors: Vec<MonitorGeometry>) -> MonitorLayout {
        MonitorLayout {
            monitors,
            virtual_left: -3840,
            virtual_top: 0,
            virtual_width: 5760,
            virtual_height: 1080,
        }
    }

    #[test]
    fn prefers_the_leftmost_monitor_over_the_primary() {
        let primary_at_origin = layout(vec![geometry(0, 0, 0), geometry(1, -1920, 0)]);
        let selected = select_recording_monitor(&primary_at_origin).expect("a monitor");
        assert_eq!((selected.x, selected.y), (-1920, 0));
        assert!(!selected.monitor.is_primary());
    }

    #[test]
    fn breaks_position_ties_with_the_topmost_monitor() {
        let stacked = layout(vec![geometry(0, -1920, 300), geometry(1, -1920, 0)]);
        let selected = select_recording_monitor(&stacked).expect("a monitor");
        assert_eq!((selected.x, selected.y), (-1920, 0));
    }

    #[test]
    fn single_monitor_layout_selects_that_monitor() {
        let single = layout(vec![geometry(0, 0, 0)]);
        assert_eq!(select_recording_monitor(&single).map(|m| m.x), Some(0));
        assert!(select_recording_monitor(&layout(Vec::new())).is_none());
    }

    #[test]
    fn workload_region_contains_identifier_strip_and_fits_monitor() {
        for (width, height) in [(528, 96), (529, 97), (1080, 1920), (3840, 2160)] {
            assert!(validate_workload_region(width, height, 3840, 2160).is_ok());
        }
        for (width, height) in [(0, 0), (527, 1080), (1920, 95), (3841, 2160), (1080, 2161)] {
            assert!(validate_workload_region(width, height, 3840, 2160).is_err());
        }
    }
}

fn validate_workload_region(
    width: u32,
    height: u32,
    monitor_width: u32,
    monitor_height: u32,
) -> Result<()> {
    // All 32 identifier cells occupy [16, 528) x [64, 96). A truncated
    // strip can retain its eight-bit sentinel while silently losing source ID bits.
    if width < 528 || height < 96 || width > monitor_width || height > monitor_height {
        bail!(
            "--region-size must fit within the leftmost monitor and contain the 528x96 identifier strip"
        );
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Process usage sampling
// ---------------------------------------------------------------------------

fn sample_memory() -> Result<(u64, u64)> {
    let handle = unsafe { GetCurrentProcess() };
    let mut counters = PROCESS_MEMORY_COUNTERS_EX::default();
    let ok = unsafe {
        K32GetProcessMemoryInfo(
            handle,
            &mut counters as *mut _ as *mut PROCESS_MEMORY_COUNTERS,
            std::mem::size_of::<PROCESS_MEMORY_COUNTERS_EX>() as u32,
        )
    };
    ok.ok().context("K32GetProcessMemoryInfo failed")?;
    Ok((counters.WorkingSetSize as u64, counters.PrivateUsage as u64))
}

fn process_cpu_ticks() -> Result<u64> {
    let handle = unsafe { GetCurrentProcess() };
    let mut creation = windows::Win32::Foundation::FILETIME::default();
    let mut exit = windows::Win32::Foundation::FILETIME::default();
    let mut kernel = windows::Win32::Foundation::FILETIME::default();
    let mut user = windows::Win32::Foundation::FILETIME::default();
    unsafe { GetProcessTimes(handle, &mut creation, &mut exit, &mut kernel, &mut user) }
        .ok()
        .context("GetProcessTimes failed")?;
    let filetime = |value: windows::Win32::Foundation::FILETIME| {
        (u64::from(value.dwHighDateTime) << 32) | u64::from(value.dwLowDateTime)
    };
    Ok(filetime(kernel) + filetime(user))
}

fn spawn_usage_sampler(
    interval: Duration,
) -> (Arc<AtomicBool>, JoinHandle<anyhow::Result<ProcessUsage>>) {
    let running = Arc::new(AtomicBool::new(true));
    let handle = std::thread::Builder::new()
        .name("snow-bench-usage".to_string())
        .spawn({
            let running = Arc::clone(&running);
            move || -> anyhow::Result<ProcessUsage> {
                let (working_set_start, private_start) = sample_memory()?;
                let mut usage = ProcessUsage {
                    samples: Vec::new(),
                    working_set_start_bytes: working_set_start,
                    working_set_peak_bytes: working_set_start,
                    private_peak_bytes: private_start,
                    cpu_percent: 0.0,
                };
                let started = Instant::now();
                let ticks_start = process_cpu_ticks()?;
                let mut ticks_last = ticks_start;
                while running.load(Ordering::Acquire) {
                    std::thread::sleep(interval);
                    if let Ok((working_set, private_bytes)) = sample_memory() {
                        usage.working_set_peak_bytes =
                            usage.working_set_peak_bytes.max(working_set);
                        usage.private_peak_bytes = usage.private_peak_bytes.max(private_bytes);
                        usage.samples.push((
                            started.elapsed().as_secs_f64(),
                            working_set,
                            private_bytes,
                        ));
                    }
                    ticks_last = process_cpu_ticks().unwrap_or(ticks_last);
                }
                let wall = started.elapsed().as_secs_f64().max(f64::EPSILON);
                let tick_seconds = ticks_last.saturating_sub(ticks_start) as f64 * 1e-7;
                usage.cpu_percent = tick_seconds / wall * 100.0;
                Ok(usage)
            }
        })
        .expect("failed to spawn usage sampler thread");
    (running, handle)
}

// ---------------------------------------------------------------------------
// Workload window
// ---------------------------------------------------------------------------

struct ThreadDpiAwareness(DPI_AWARENESS_CONTEXT);

impl ThreadDpiAwareness {
    fn per_monitor_v2() -> Self {
        let previous =
            unsafe { SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) };
        Self(previous)
    }
}

impl Drop for ThreadDpiAwareness {
    fn drop(&mut self) {
        if !self.0.is_invalid() {
            let _ = unsafe { SetThreadDpiAwarenessContext(self.0) };
        }
    }
}

// Publish each workload image in one blit: a capture must never see a
// half-written identifier while GDI is repainting its background.
struct WorkloadSurface {
    dc: HDC,
    bitmap: HBITMAP,
    previous: windows::Win32::Graphics::Gdi::HGDIOBJ,
}
impl Drop for WorkloadSurface {
    fn drop(&mut self) {
        unsafe {
            SelectObject(self.dc, self.previous);
            let _ = DeleteObject(self.bitmap.into());
            let _ = DeleteDC(self.dc);
        }
    }
}

struct WindowState {
    surface: Option<WorkloadSurface>,
    width: i32,
    height: i32,
    tick: u64,
    animate: bool,
    sparse: bool,
}

struct WorkloadWindow {
    handle: isize,
    thread: Option<JoinHandle<()>>,
}

impl WorkloadWindow {
    fn publish_once(&self) -> Result<()> {
        unsafe {
            PostMessageW(
                Some(HWND(self.handle as *mut _)),
                WM_TIMER,
                WPARAM(WORKLOAD_TIMER_ID),
                LPARAM(0),
            )
        }
        .context("requesting the initial static workload presentation")
    }

    fn spawn(region: RecordingRegion, timer_ms: u32, sparse: bool) -> Result<Self> {
        let (ready_tx, ready_rx) = std::sync::mpsc::sync_channel(1);
        let thread = std::thread::Builder::new()
            .name("snow-bench-workload-window".to_string())
            .spawn(move || {
                let _dpi = ThreadDpiAwareness::per_monitor_v2();
                if let Err(error) = run_window_thread(region, timer_ms, sparse, &ready_tx) {
                    // The parent may already have timed out; surface it anyway.
                    let _ = ready_tx.send(Err(error));
                }
            })
            .context("failed to spawn workload window thread")?;
        let received = ready_rx.recv_timeout(Duration::from_secs(10));
        let handle = match received {
            Ok(Ok(handle)) => handle,
            Ok(Err(message)) => {
                let _ = thread.join();
                bail!("workload window failed: {message}");
            }
            Err(_) => {
                let _ = thread.join();
                bail!("workload window did not appear within 10 s");
            }
        };
        Ok(Self {
            handle,
            thread: Some(thread),
        })
    }
}

impl Drop for WorkloadWindow {
    fn drop(&mut self) {
        let _ = unsafe {
            PostMessageW(
                Some(HWND(self.handle as *mut _)),
                WM_CLOSE,
                WPARAM(0),
                LPARAM(0),
            )
        };
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn run_window_thread(
    region: RecordingRegion,
    timer_ms: u32,
    sparse: bool,
    ready: &SyncSender<Result<isize, String>>,
) -> Result<(), String> {
    unsafe {
        let instance =
            GetModuleHandleW(None).map_err(|error| format!("GetModuleHandleW: {error}"))?;
        let class_name = w!("SnowRealtimeRecordingWorkloadWindow");
        let class = WNDCLASSW {
            lpfnWndProc: Some(workload_wnd_proc),
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            hInstance: instance.into(),
            lpszClassName: class_name,
            ..Default::default()
        };
        if RegisterClassW(&class) == 0 {
            return Err("RegisterClassW failed".into());
        }

        let state = Box::new(WindowState {
            surface: None,
            width: region.width as i32,
            height: region.height as i32,
            tick: 0,
            animate: timer_ms != 0,
            sparse,
        });
        let state_raw = Box::into_raw(state);
        // The window must stay visible for capture but never steal focus: no
        // OS input is injected, so nothing requires foreground activation.
        let created = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            class_name,
            w!("snow realtime recording workload"),
            WS_POPUP | WS_VISIBLE,
            region.x,
            region.y,
            region.width as i32,
            region.height as i32,
            None,
            None,
            Some(instance.into()),
            Some(state_raw.cast()),
        );
        let hwnd = match created {
            Ok(hwnd) => hwnd,
            Err(error) => {
                drop(Box::from_raw(state_raw));
                return Err(format!("CreateWindowExW: {error}"));
            }
        };

        // A nonactivating tool window can remain WS_VISIBLE while desktop
        // Peek removes it from composition. Keep this controlled workload
        // present without moving the physical pointer or taking focus.
        let excluded_from_peek = 1i32;
        if let Err(error) = DwmSetWindowAttribute(
            hwnd,
            DWMWA_EXCLUDED_FROM_PEEK,
            (&excluded_from_peek as *const i32).cast(),
            std::mem::size_of::<i32>() as u32,
        ) {
            let _ = DestroyWindow(hwnd);
            return Err(format!("exclude workload from Peek: {error}"));
        }
        let _ = ShowWindow(hwnd, SW_SHOW);
        if timer_ms > 0 {
            SetTimer(Some(hwnd), WORKLOAD_TIMER_ID, timer_ms, None);
        }
        let _ = ready.send(Ok(hwnd.0 as isize));

        let mut message = MSG::default();
        while GetMessageW(&mut message, None, 0, 0).as_bool() {
            let _ = DispatchMessageW(&message);
        }
        Ok(())
    }
}

unsafe extern "system" fn workload_wnd_proc(
    hwnd: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    unsafe {
        match message {
            WM_NCCREATE => {
                let create = &*(lparam.0 as *const CREATESTRUCTW);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, create.lpCreateParams as isize);
                DefWindowProcW(hwnd, message, wparam, lparam)
            }
            WM_TIMER => {
                let damage = window_state(hwnd)
                    .filter(|state| (**state).sparse)
                    .map(|_| RECT {
                        left: 0,
                        top: 0,
                        right: 768,
                        bottom: 384,
                    });
                let _ = InvalidateRect(
                    Some(hwnd),
                    damage.as_ref().map(|rect| rect as *const RECT),
                    false,
                );
                LRESULT(0)
            }
            WM_ERASEBKGND => LRESULT(1),
            WM_PAINT => {
                let mut paint = PAINTSTRUCT::default();
                let dc = BeginPaint(hwnd, &mut paint);
                if let Some(state) = window_state(hwnd) {
                    let state = &mut *state;
                    if state.surface.is_none() {
                        let memory = CreateCompatibleDC(Some(dc));
                        let bitmap = CreateCompatibleBitmap(dc, state.width, state.height);
                        let previous = SelectObject(memory, bitmap.into());
                        state.surface = Some(WorkloadSurface {
                            dc: memory,
                            bitmap,
                            previous,
                        });
                    }
                    let memory = state.surface.as_ref().unwrap().dc;
                    // Repainting a static window publishes its existing image;
                    // exposure or the startup presentation must not invent new
                    // source content or increment the decoded identifier.
                    if state.tick == 0 || state.animate {
                        render_workload_frame(memory, state);
                    }
                    let published = BitBlt(
                        dc,
                        paint.rcPaint.left,
                        paint.rcPaint.top,
                        paint.rcPaint.right - paint.rcPaint.left,
                        paint.rcPaint.bottom - paint.rcPaint.top,
                        Some(memory),
                        paint.rcPaint.left,
                        paint.rcPaint.top,
                        SRCCOPY,
                    );
                    if published.is_ok() {
                        WORKLOAD_PUBLISHED.fetch_add(1, Ordering::Release);
                        WORKLOAD_PUBLICATIONS
                            .lock()
                            .unwrap()
                            .push((state.tick, Instant::now()));
                    } else {
                        WORKLOAD_FAILURES.fetch_add(1, Ordering::Release);
                    }
                }
                let _ = EndPaint(hwnd, &paint);
                LRESULT(0)
            }
            WM_DESTROY => {
                if let Some(state) = window_state(hwnd) {
                    drop(Box::from_raw(state));
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                }
                PostQuitMessage(0);
                LRESULT(0)
            }
            _ => DefWindowProcW(hwnd, message, wparam, lparam),
        }
    }
}

fn window_state(hwnd: HWND) -> Option<*mut WindowState> {
    let pointer = unsafe { GetWindowLongPtrW(hwnd, GWLP_USERDATA) } as *mut WindowState;
    (!pointer.is_null()).then_some(pointer)
}

/// Draws one continuously changing frame: a phase-shifting background, a
/// moving rectangle and ellipse, and a tick counter.
unsafe fn render_workload_frame(dc: HDC, state: &mut WindowState) {
    unsafe {
        state.tick = state.tick.wrapping_add(1);
        let tick = state.tick;
        if state.sparse && tick == 1 {
            fill_rect(dc, 0, 0, state.width, state.height, rgb(32, 32, 32));
        }
        let width = if state.sparse {
            state.width.min(768)
        } else {
            state.width
        };
        let height = if state.sparse {
            state.height.min(384)
        } else {
            state.height
        };
        let background = rgb(
            (tick.wrapping_mul(3) % 256) as u8,
            (tick.wrapping_mul(5) % 256) as u8,
            (tick.wrapping_mul(7) % 256) as u8,
        );
        fill_rect(dc, 0, 0, width, height, background);

        let rect_size = (width / 6).max(40);
        let rect_x = (tick.wrapping_mul(11) % ((width - rect_size).max(1) as u64)) as i32;
        let rect_y = (tick.wrapping_mul(7) % ((height - rect_size).max(1) as u64)) as i32;
        fill_rect(
            dc,
            rect_x,
            rect_y,
            rect_x + rect_size,
            rect_y + rect_size,
            rgb(
                255 - (tick.wrapping_mul(3) % 256) as u8,
                (tick.wrapping_mul(7) % 256) as u8,
                (tick.wrapping_mul(5) % 256) as u8,
            ),
        );

        let ellipse_size = (width / 7).max(34);
        let ellipse_x = (tick.wrapping_mul(5) % ((width - ellipse_size).max(1) as u64)) as i32;
        let ellipse_y = (tick.wrapping_mul(13) % ((height - ellipse_size).max(1) as u64)) as i32;
        draw_ellipse(
            dc,
            ellipse_x,
            ellipse_y,
            ellipse_x + ellipse_size,
            ellipse_y + ellipse_size,
            rgb(32, 224, 255),
        );

        let code = ((tick as u32 & 0x00ff_ffff) << 8) | 0xa5;
        for bit in 0..32 {
            let level = if code & (1 << bit) != 0 { 255 } else { 0 };
            fill_rect(
                dc,
                16 + bit * 16,
                64,
                32 + bit * 16,
                96,
                rgb(level, level, level),
            );
        }
        let text = format!("tick={tick}");
        let wide: Vec<u16> = text.encode_utf16().collect();
        let _ = SetBkMode(dc, windows::Win32::Graphics::Gdi::TRANSPARENT);
        let _ = SetTextColor(dc, rgb(255, 255, 255));
        let _ = TextOutW(dc, 16, 10, &wide);
    }
}

fn rgb(red: u8, green: u8, blue: u8) -> COLORREF {
    COLORREF(u32::from(red) | (u32::from(green) << 8) | (u32::from(blue) << 16))
}

unsafe fn fill_rect(dc: HDC, left: i32, top: i32, right: i32, bottom: i32, color: COLORREF) {
    unsafe {
        let brush = CreateSolidBrush(color);
        let rect = RECT {
            left,
            top,
            right,
            bottom,
        };
        FillRect(dc, &rect, brush);
        let _ = DeleteObject(brush.into());
    }
}

unsafe fn draw_ellipse(dc: HDC, left: i32, top: i32, right: i32, bottom: i32, color: COLORREF) {
    unsafe {
        let brush = CreateSolidBrush(color);
        let old_brush = SelectObject(dc, brush.into());
        let old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        let _ = Ellipse(dc, left, top, right, bottom);
        let _ = SelectObject(dc, old_pen);
        let _ = SelectObject(dc, old_brush);
        let _ = DeleteObject(brush.into());
    }
}

// Count changed source identifiers after decoding: packet count alone can hide duplicates.
fn decode_frame_ids(path: &Path, source_width: u32, source_height: u32) -> Result<(u64, u64, u64)> {
    use ffmpeg_next as ffmpeg;
    let mut input = ffmpeg::format::input(path)?;
    let stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .context("missing video")?;
    let index = stream.index();
    let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())?
        .decoder()
        .video()?;
    let mut scaler = ffmpeg::software::scaling::Context::get(
        decoder.format(),
        decoder.width(),
        decoder.height(),
        ffmpeg::format::Pixel::RGB24,
        decoder.width(),
        decoder.height(),
        ffmpeg::software::scaling::flag::Flags::POINT,
    )?;
    let mut count = 0;
    let mut fresh = 0;
    let mut unreadable = 0;
    let mut identifiers = String::from("decoded_frame,pts,identifier,valid\n");
    let mut previous = None;
    let mut receive = |decoder: &mut ffmpeg::decoder::Video| -> Result<()> {
        let mut frame = ffmpeg::frame::Video::empty();
        while decoder.receive_frame(&mut frame).is_ok() {
            let mut rgb = ffmpeg::frame::Video::empty();
            scaler.run(&frame, &mut rgb)?;
            count += 1;
            if count == 1 {
                let mut ppm = format!("P6\n{} {}\n255\n", rgb.width(), rgb.height()).into_bytes();
                for row in rgb
                    .data(0)
                    .chunks(rgb.stride(0))
                    .take(rgb.height() as usize)
                {
                    ppm.extend_from_slice(&row[..rgb.width() as usize * 3]);
                }
                fs::write(path.with_extension(format!("{count}.ppm")), ppm)?;
            }
            let mut code = 0u32;
            for bit in 0..32u32 {
                let x =
                    ((24 + bit * 16) as u64 * rgb.width() as u64 / source_width as u64) as usize;
                let y = (80u64 * rgb.height() as u64 / source_height as u64) as usize;
                let pixel = y * rgb.stride(0) + x * 3;
                if rgb.data(0).get(pixel).copied().unwrap_or(0) >= 128 {
                    code |= 1 << bit;
                }
            }
            identifiers.push_str(&format!(
                "{count},{},{},{}\n",
                frame.pts().unwrap_or(-1),
                code >> 8,
                u8::from(code & 255 == 0xa5)
            ));
            if code & 255 != 0xa5 {
                if unreadable == 0 {
                    let mut ppm =
                        format!("P6\n{} {}\n255\n", rgb.width(), rgb.height()).into_bytes();
                    for row in rgb
                        .data(0)
                        .chunks(rgb.stride(0))
                        .take(rgb.height() as usize)
                    {
                        ppm.extend_from_slice(&row[..rgb.width() as usize * 3]);
                    }
                    fs::write(path.with_extension("first-invalid.ppm"), ppm)?;
                }
                unreadable += 1;
                continue;
            }
            let id = code >> 8;
            if previous != Some(id) {
                fresh += 1;
            }
            previous = Some(id);
        }
        Ok(())
    };
    for (stream, packet) in input.packets() {
        if stream.index() == index {
            decoder.send_packet(&packet)?;
            receive(&mut decoder)?;
        }
    }
    decoder.send_eof()?;
    receive(&mut decoder)?;
    fs::write(path.with_extension("identifiers.csv"), identifiers)?;
    Ok((count, fresh, unreadable))
}

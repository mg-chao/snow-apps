//! Realtime recording benchmark simulating snow-shot's live recording path.
//!
//! Drives a real `DirectRecordingSession` (capture stream, low-level mouse and
//! keyboard hooks, overlay compositing, and FFmpeg encoding to a temporary
//! MP4) against a fullscreen window that repaints continuously, while
//! simulating user input with `SendInput`:
//!
//! * `mouse-only`  — cursor + mouse trail/click effects; mouse movement and
//!   clicks simulated, no keyboard overlay.
//! * `all-effects` — everything above plus the keyboard overlay, with
//!   simulated key presses and periodic Ctrl+Shift chords.
//!
//! Detailed metric groups are compile-time gated (see the `bench-*-timing`
//! cargo features); with none enabled only the always-on metrics below are
//! reported. Run via `scripts/run-realtime-recording-perf.ps1`, which uses the
//! `windows-msvc-performance` preset environment. The benchmark takes over
//! the primary monitor and injects input while it runs.

#![cfg(windows)]

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::SyncSender;
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
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
use windows::Win32::Foundation::{COLORREF, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    BeginPaint, CreateSolidBrush, DeleteObject, Ellipse, EndPaint, FillRect, GetStockObject, HDC,
    InvalidateRect, NULL_PEN, PAINTSTRUCT, SelectObject, SetBkMode, SetTextColor, TextOutW,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::ProcessStatus::{
    K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS, PROCESS_MEMORY_COUNTERS_EX,
};
use windows::Win32::System::Threading::{
    AttachThreadInput, GetCurrentProcess, GetCurrentThreadId, GetProcessTimes,
};
use windows::Win32::UI::HiDpi::{
    DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetThreadDpiAwarenessContext,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetAsyncKeyState, INPUT, INPUT_0, INPUT_KEYBOARD, INPUT_MOUSE, KEYBD_EVENT_FLAGS, KEYBDINPUT,
    KEYEVENTF_KEYUP, MOUSEEVENTF_ABSOLUTE, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP,
    MOUSEEVENTF_MOVE, MOUSEEVENTF_VIRTUALDESK, MOUSEINPUT, SendInput, VIRTUAL_KEY,
};
use windows::Win32::UI::WindowsAndMessaging::{
    BringWindowToTop, CREATESTRUCTW, CreateWindowExW, DefWindowProcW, DispatchMessageW,
    GWLP_USERDATA, GetCursorPos, GetForegroundWindow, GetMessageW, GetWindowLongPtrW,
    GetWindowThreadProcessId, IDC_ARROW, LoadCursorW, MSG, PostMessageW, PostQuitMessage,
    RegisterClassW, SW_SHOW, SetCursorPos, SetForegroundWindow, SetTimer, SetWindowLongPtrW,
    ShowWindow, TranslateMessage, WM_CLOSE, WM_DESTROY, WM_ERASEBKGND, WM_NCCREATE, WM_PAINT,
    WM_TIMER, WNDCLASSW, WS_EX_TOOLWINDOW, WS_EX_TOPMOST, WS_POPUP, WS_VISIBLE,
};
use windows::core::w;

const DEFAULT_DURATION_SECONDS: u64 = 12;
const DEFAULT_WARMUP_SECONDS: u64 = 2;
const DEFAULT_SAMPLES: usize = 1;
const DEFAULT_FPS: u32 = 30;
const DEFAULT_MOVE_INTERVAL_MS: u64 = 16;
const DEFAULT_CLICK_INTERVAL_MS: u64 = 1_000;
const DEFAULT_KEY_INTERVAL_MS: u64 = 333;
const DEFAULT_CHORD_INTERVAL_MS: u64 = 5_000;
const DEFAULT_WINDOW_TIMER_MS: u32 = 15;
const MAX_WORKING_SET_DELTA_MIB: f64 = 768.0;
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
    duration_seconds: u64,
    warmup_seconds: u64,
    samples: usize,
    fps: u32,
    clarity: Clarity,
    backend: CaptureBackendKind,
    prefer_hardware: bool,
    scenario_filter: Option<String>,
    output_directory: PathBuf,
    move_interval_ms: u64,
    click_interval_ms: u64,
    key_interval_ms: u64,
    chord_interval_ms: u64,
    window_timer_ms: u32,
    allow_debug: bool,
}

fn print_usage() {
    println!(
        "Usage: cargo run --release -p snow-recording-runtime --features \
bench-stage-timing,bench-compositor-timing,bench-pipeline-timing \
--example realtime_recording_benchmark -- [options]\n\
  --duration-seconds <n>     measured recording length per sample (default: {DEFAULT_DURATION_SECONDS})\n\
  --warmup-seconds <n>       discarded warmup recording per scenario, 0 disables (default: \
{DEFAULT_WARMUP_SECONDS})\n\
  --samples <n>              measured samples per scenario (default: {DEFAULT_SAMPLES})\n\
  --fps <n>                  capture and output frame rate (default: {DEFAULT_FPS})\n\
  --clarity <kind>           output cap: 4k, 2k, 1080p, 720p, 480p, native (default: 1080p)\n\
  --backend <kind>           capture backend: auto, dxgi, wgc, gdi (default: auto)\n\
  --prefer-hardware          prefer a hardware H.264 encoder\n\
  --scenario <name>          run only one scenario: mouse-only or all-effects\n\
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
        duration_seconds: DEFAULT_DURATION_SECONDS,
        warmup_seconds: DEFAULT_WARMUP_SECONDS,
        samples: DEFAULT_SAMPLES,
        fps: DEFAULT_FPS,
        clarity: Clarity::Fixed(1920, 1080),
        backend: CaptureBackendKind::Auto,
        prefer_hardware: false,
        scenario_filter: None,
        output_directory: PathBuf::from("target/perf/realtime-recording"),
        move_interval_ms: DEFAULT_MOVE_INTERVAL_MS,
        click_interval_ms: DEFAULT_CLICK_INTERVAL_MS,
        key_interval_ms: DEFAULT_KEY_INTERVAL_MS,
        chord_interval_ms: DEFAULT_CHORD_INTERVAL_MS,
        window_timer_ms: DEFAULT_WINDOW_TIMER_MS,
        allow_debug: false,
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
            other => bail!("unknown argument: {other}. Use --help for usage."),
        }
        index += 1;
    }
    if let Some(filter) = options.scenario_filter.as_deref()
        && SCENARIOS.iter().all(|scenario| scenario.name != filter)
    {
        bail!("unknown scenario '{filter}' (expected mouse-only or all-effects)");
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

struct Scenario {
    name: &'static str,
    keyboard_overlay: bool,
    simulate_keys: bool,
}

const SCENARIOS: [Scenario; 2] = [
    Scenario {
        name: "mouse-only",
        keyboard_overlay: false,
        simulate_keys: false,
    },
    Scenario {
        name: "all-effects",
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
    working_set_start_bytes: u64,
    working_set_peak_bytes: u64,
    private_peak_bytes: u64,
    cpu_percent: f64,
}

struct SampleResult {
    scenario: &'static str,
    sample: usize,
    duration_seconds: u64,
    fps: u32,
    region: (u32, u32),
    output: (u32, u32),
    expected_frames: u64,
    encoded_frames: u64,
    coalesced_frames: u64,
    dropped_capture_frames: u64,
    setup_ms: f64,
    stop_ms: f64,
    output_bytes: u64,
    video_encoder: String,
    used_hardware_video_encoder: bool,
    input: InputOutcome,
    usage: ProcessUsage,
    /// Kept for the compile-time gated metric groups; unread otherwise.
    #[cfg(any(
        feature = "bench-stage-timing",
        feature = "bench-compositor-timing",
        feature = "bench-pipeline-timing"
    ))]
    report: snow_recording_runtime::DirectRecordingReport,
}

fn elapsed_ms(started: Instant) -> f64 {
    started.elapsed().as_secs_f64() * 1_000.0
}

fn main() -> Result<()> {
    let options = parse_args()?;
    print_compiled_metric_groups();

    let _dpi = ThreadDpiAwareness::per_monitor_v2();
    let system = CaptureSystem::builder()
        .build()
        .context("building capture system")?;
    let layout = system.monitor_layout().context("enumerating monitors")?;
    let primary = system
        .primary_monitor()
        .context("resolving primary monitor")?;
    let monitor = layout
        .monitors
        .iter()
        .find(|monitor| monitor.monitor == primary)
        .context("primary monitor disappeared from the layout")?;
    // Even dimensions, matching screenRecordingCompatibleCaptureRegion.
    let region = RecordingRegion::new(
        monitor.x,
        monitor.y,
        monitor.width & !1,
        monitor.height & !1,
    );

    println!(
        "region: {}x{} at ({}, {}) on the primary monitor; virtual desktop {}x{} at ({}, {})",
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

    let window = WorkloadWindow::spawn(region, options.window_timer_ms)?;
    // Let the first frames reach the compositor before capturing or injecting.
    std::thread::sleep(Duration::from_millis(750));

    fs::create_dir_all(&options.output_directory).with_context(|| {
        format!(
            "failed to create benchmark output directory {}",
            options.output_directory.display()
        )
    })?;

    let mut rows = Vec::new();
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
            let warmup = run_sample(&options, scenario, region, &layout, window.handle, 0, true)?;
            validate_sample(&warmup, true)?;
        }
        for sample in 0..options.samples {
            let result = run_sample(
                &options,
                scenario,
                region,
                &layout,
                window.handle,
                sample,
                false,
            )?;
            // Print first so a gate failure still shows the sample's numbers.
            print_sample(&result);
            validate_sample(&result, false)?;
            rows.push(result);
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
    Ok(())
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

#[allow(clippy::too_many_arguments)]
fn run_sample(
    options: &Options,
    scenario: &Scenario,
    region: RecordingRegion,
    layout: &MonitorLayout,
    workload_hwnd: isize,
    sample: usize,
    warmup: bool,
) -> Result<SampleResult> {
    let output_path = if warmup {
        options.output_directory.join("warmup.mp4")
    } else {
        options
            .output_directory
            .join(format!("{}-sample-{}.mp4", scenario.name, sample + 1))
    };
    if output_path.is_file() {
        fs::remove_file(&output_path)
            .with_context(|| format!("failed to replace {}", output_path.display()))?;
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
        enable_system_audio: false,
        show_cursor: true,
        keyboard,
        mouse_trail_rgba: TRAIL_RGBA,
        mouse_trail_duration_ms: TRAIL_DURATION_MS,
        mouse_click_rgba: CLICK_RGBA,
    };

    let (usage_running, usage_thread) = spawn_usage_sampler(Duration::from_millis(500));
    let setup_started = Instant::now();
    let mut session = DirectRecordingSession::create(config)?;
    session
        .start()
        .context("starting direct recording session")?;
    let setup_ms = elapsed_ms(setup_started);

    let input_thread = std::thread::Builder::new()
        .name("snow-bench-input".to_string())
        .spawn({
            let plan = InputPlan {
                move_interval_ms: options.move_interval_ms,
                click_interval_ms: options.click_interval_ms,
                key_interval_ms: options.key_interval_ms,
                chord_interval_ms: options.chord_interval_ms,
                region,
                desktop: VirtualDesktop {
                    left: layout.virtual_left,
                    top: layout.virtual_top,
                    width: layout.virtual_width,
                    height: layout.virtual_height,
                },
                workload_hwnd,
                simulate_keys: scenario.simulate_keys,
                duration: Duration::from_secs(duration_seconds),
            };
            move || simulate_input(&plan)
        })?;
    let input = input_thread
        .join()
        .map_err(|_| anyhow::anyhow!("input simulation thread panicked"))??;

    let stop_started = Instant::now();
    let report = session
        .stop()
        .context("stopping direct recording session")?;
    let stop_ms = elapsed_ms(stop_started);

    usage_running.store(false, Ordering::Release);
    let usage = usage_thread
        .join()
        .map_err(|_| anyhow::anyhow!("usage sampler thread panicked"))??;
    let output_bytes = fs::metadata(&output_path)
        .with_context(|| format!("failed to stat {}", output_path.display()))?
        .len();
    if warmup {
        let _ = fs::remove_file(&output_path);
    }

    Ok(SampleResult {
        scenario: scenario.name,
        sample: sample + 1,
        duration_seconds,
        fps: options.fps,
        region: (region.width, region.height),
        output: (output_width, output_height),
        expected_frames: duration_seconds * u64::from(options.fps),
        encoded_frames: report.encoded_frames,
        coalesced_frames: report.coalesced_frames,
        dropped_capture_frames: report.dropped_capture_frames,
        setup_ms,
        stop_ms,
        output_bytes,
        video_encoder: report.video_encoder.clone(),
        used_hardware_video_encoder: report.used_hardware_video_encoder,
        input,
        usage,
        #[cfg(any(
            feature = "bench-stage-timing",
            feature = "bench-compositor-timing",
            feature = "bench-pipeline-timing"
        ))]
        report,
    })
}

fn validate_sample(result: &SampleResult, warmup: bool) -> Result<()> {
    let label = if warmup {
        format!("{} warmup", result.scenario)
    } else {
        format!("{} sample {}", result.scenario, result.sample)
    };
    if result.dropped_capture_frames > 0 {
        bail!(
            "{label} dropped {} capture frames",
            result.dropped_capture_frames
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
    let working_set_delta_mib = (result
        .usage
        .working_set_peak_bytes
        .saturating_sub(result.usage.working_set_start_bytes))
        as f64
        / MIB;
    if working_set_delta_mib > MAX_WORKING_SET_DELTA_MIB {
        bail!(
            "{label} exceeded the {:.0} MiB working-set budget: {working_set_delta_mib:.1} MiB",
            MAX_WORKING_SET_DELTA_MIB
        );
    }
    // Frame-count shortfalls against the requested fps are machine-dependent
    // performance signal (captured frames may also be coalesced away as
    // timestamp duplicates), so they warn instead of failing the run; the
    // dropped-frame and injection gates above are the correctness checks.
    let achieved_fps = result.encoded_frames as f64 / result.duration_seconds as f64;
    if result.coalesced_frames > 0 {
        println!(
            "warning: {label} coalesced {} frames",
            result.coalesced_frames
        );
    }
    if achieved_fps < f64::from(result.fps) * 0.95 {
        println!(
            "warning: {label} achieved {achieved_fps:.1} fps, below 95% of the {} fps target",
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
                ("pipeline.compose", pipeline.compose),
                ("pipeline.encode_push", pipeline.encode_push),
                ("pipeline.end_to_end", pipeline.end_to_end),
            ]),
        );
        println!(
            "  time_to_first_encoded_frame={} synthetic_overlay_frames={} stream: captured={} \
dropped={} capture_fps={:.1} capture_latency_avg={:.2}ms",
            pipeline
                .time_to_first_encoded_frame
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
            ("scenario".into(), row.scenario.to_string()),
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
            ("output_bytes".into(), row.output_bytes.to_string()),
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
                    "time_to_first_encoded_frame_ms".into(),
                    pipeline
                        .time_to_first_encoded_frame
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
    for row in rows {
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
        #[cfg(feature = "bench-stage-timing")]
        if let Some(timings) = row.report.capture_stage_timings.as_ref() {
            for (stage, stats) in timings.snapshot() {
                emit("capture", stage, &stats);
            }
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
                ("pipeline.compose", pipeline.compose),
                ("pipeline.encode_push", pipeline.encode_push),
                ("pipeline.end_to_end", pipeline.end_to_end),
            ] {
                emit("pipeline", stage, &stats);
            }
        }
    }
    let path = output_directory.join("realtime-recording-stages.csv");
    fs::write(&path, csv).with_context(|| format!("failed to write {}", path.display()))?;
    Ok(path)
}

// ---------------------------------------------------------------------------
// Simulated input
// ---------------------------------------------------------------------------

// Both sides of Ctrl, Shift, Alt, and Win; injected only after confirming all
// of them are physically up so the benchmark never releases a real modifier.
const MODIFIER_VKS: [u16; 8] = [0xA2, 0xA3, 0xA0, 0xA1, 0xA4, 0xA5, 0x5B, 0x5C];
const VK_CONTROL: u16 = 0xA2;
const VK_SHIFT: u16 = 0xA0;

struct VirtualDesktop {
    left: i32,
    top: i32,
    width: u32,
    height: u32,
}

struct InputPlan {
    move_interval_ms: u64,
    click_interval_ms: u64,
    key_interval_ms: u64,
    chord_interval_ms: u64,
    region: RecordingRegion,
    desktop: VirtualDesktop,
    workload_hwnd: isize,
    simulate_keys: bool,
    duration: Duration,
}

fn simulate_input(plan: &InputPlan) -> Result<InputOutcome> {
    for vk in MODIFIER_VKS {
        if unsafe { GetAsyncKeyState(i32::from(vk)) } < 0 {
            bail!("a Ctrl/Shift/Alt/Win modifier ({vk:#04x}) is held; release it before running");
        }
    }
    let foreground = unsafe { GetForegroundWindow() };
    if foreground.0 as isize != plan.workload_hwnd {
        bail!(
            "workload window lost foreground focus; refusing to inject input into other \
             applications"
        );
    }
    let mut saved_cursor = POINT::default();
    let has_cursor = unsafe { GetCursorPos(&mut saved_cursor) }.is_ok();
    let (start_x, start_y) = sweep_position(&plan.region, Duration::ZERO);
    if has_cursor {
        let _ = unsafe { SetCursorPos(start_x, start_y) };
    }

    let mut outcome = InputOutcome::default();
    let started = Instant::now();
    let mut next_move_ms = 0u64;
    let mut next_click_ms = plan.click_interval_ms / 2;
    let mut next_key_ms = plan.key_interval_ms / 2;
    let mut next_chord_ms = plan.chord_interval_ms;
    while started.elapsed() < plan.duration {
        let elapsed_ms = started.elapsed().as_millis() as u64;
        while next_move_ms <= elapsed_ms {
            let (x, y) = sweep_position(&plan.region, Duration::from_millis(next_move_ms));
            if send_mouse_move(x, y, &plan.desktop, &mut outcome) {
                next_move_ms = next_move_ms.saturating_add(plan.move_interval_ms);
            } else {
                break;
            }
        }
        while next_click_ms <= elapsed_ms {
            if send_mouse_click(&mut outcome) {
                next_click_ms = next_click_ms.saturating_add(plan.click_interval_ms);
            } else {
                break;
            }
        }
        if plan.simulate_keys {
            while next_key_ms <= elapsed_ms {
                if send_key_tap(letter_for(outcome.keys_sent), &mut outcome) {
                    next_key_ms = next_key_ms.saturating_add(plan.key_interval_ms);
                } else {
                    break;
                }
            }
            while next_chord_ms <= elapsed_ms {
                if send_chord(letter_for(outcome.keys_sent), &mut outcome) {
                    next_chord_ms = next_chord_ms.saturating_add(plan.chord_interval_ms);
                } else {
                    break;
                }
            }
        }
        std::thread::sleep(Duration::from_millis(2));
    }

    release_all_modifiers(&mut outcome);
    if has_cursor {
        let _ = unsafe { SetCursorPos(saved_cursor.x, saved_cursor.y) };
    }
    Ok(outcome)
}

fn letter_for(keys_sent: u64) -> u16 {
    u16::from(b'A' + (keys_sent % 26) as u8)
}

/// Deterministic Lissajous sweep inside the region with a 10% margin.
fn sweep_position(region: &RecordingRegion, elapsed: Duration) -> (i32, i32) {
    let seconds = elapsed.as_secs_f64();
    let margin_x = region.width as f64 * 0.1;
    let margin_y = region.height as f64 * 0.1;
    let span_x = (region.width as f64 - 2.0 * margin_x).max(1.0);
    let span_y = (region.height as f64 - 2.0 * margin_y).max(1.0);
    let fx = 0.5 + 0.5 * (std::f64::consts::TAU * seconds / 4.7).sin();
    let fy = 0.5 + 0.5 * (std::f64::consts::TAU * seconds / 3.1).sin();
    let x = region.x as f64 + margin_x + fx * span_x;
    let y = region.y as f64 + margin_y + fy * span_y;
    (x.round() as i32, y.round() as i32)
}

fn send_inputs(inputs: &[INPUT], outcome: &mut InputOutcome) -> bool {
    let sent = unsafe { SendInput(inputs, std::mem::size_of::<INPUT>() as i32) };
    if sent == inputs.len() as u32 {
        return true;
    }
    outcome.send_failures += 1;
    false
}

fn mouse_move_input(x: i32, y: i32, desktop: &VirtualDesktop) -> INPUT {
    let virtual_width = (desktop.width.max(1) as i64 - 1).max(1);
    let virtual_height = (desktop.height.max(1) as i64 - 1).max(1);
    let dx = ((i64::from(x - desktop.left) * 65_535) / virtual_width) as i32;
    let dy = ((i64::from(y - desktop.top) * 65_535) / virtual_height) as i32;
    INPUT {
        r#type: INPUT_MOUSE,
        Anonymous: INPUT_0 {
            mi: MOUSEINPUT {
                dx,
                dy,
                mouseData: 0,
                dwFlags: MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
                time: 0,
                dwExtraInfo: 0,
            },
        },
    }
}

fn send_mouse_move(x: i32, y: i32, desktop: &VirtualDesktop, outcome: &mut InputOutcome) -> bool {
    if !send_inputs(&[mouse_move_input(x, y, desktop)], outcome) {
        return false;
    }
    outcome.moves_sent += 1;
    true
}

fn send_mouse_click(outcome: &mut InputOutcome) -> bool {
    let click = |down: bool| INPUT {
        r#type: INPUT_MOUSE,
        Anonymous: INPUT_0 {
            mi: MOUSEINPUT {
                dx: 0,
                dy: 0,
                mouseData: 0,
                dwFlags: if down {
                    MOUSEEVENTF_LEFTDOWN
                } else {
                    MOUSEEVENTF_LEFTUP
                },
                time: 0,
                dwExtraInfo: 0,
            },
        },
    };
    if !send_inputs(&[click(true), click(false)], outcome) {
        return false;
    }
    outcome.clicks_sent += 1;
    true
}

fn key_input(vk: u16, down: bool) -> INPUT {
    INPUT {
        r#type: INPUT_KEYBOARD,
        Anonymous: INPUT_0 {
            ki: KEYBDINPUT {
                wVk: VIRTUAL_KEY(vk),
                wScan: 0,
                dwFlags: if down {
                    KEYBD_EVENT_FLAGS(0)
                } else {
                    KEYEVENTF_KEYUP
                },
                time: 0,
                dwExtraInfo: 0,
            },
        },
    }
}

fn send_key_tap(vk: u16, outcome: &mut InputOutcome) -> bool {
    if !send_inputs(&[key_input(vk, true), key_input(vk, false)], outcome) {
        return false;
    }
    outcome.keys_sent += 1;
    true
}

fn send_chord(vk: u16, outcome: &mut InputOutcome) -> bool {
    let inputs = [
        key_input(VK_CONTROL, true),
        key_input(VK_SHIFT, true),
        key_input(vk, true),
        key_input(vk, false),
        key_input(VK_SHIFT, false),
        key_input(VK_CONTROL, false),
    ];
    if !send_inputs(&inputs, outcome) {
        return false;
    }
    outcome.chords_sent += 1;
    true
}

fn release_all_modifiers(outcome: &mut InputOutcome) {
    let releases: Vec<INPUT> = MODIFIER_VKS
        .iter()
        .map(|vk| key_input(*vk, false))
        .collect();
    send_inputs(&releases, outcome);
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

struct WindowState {
    width: i32,
    height: i32,
    tick: u64,
}

struct WorkloadWindow {
    handle: isize,
    thread: Option<JoinHandle<()>>,
}

impl WorkloadWindow {
    fn spawn(region: RecordingRegion, timer_ms: u32) -> Result<Self> {
        let (ready_tx, ready_rx) = std::sync::mpsc::sync_channel(1);
        let thread = std::thread::Builder::new()
            .name("snow-bench-workload-window".to_string())
            .spawn(move || {
                let _dpi = ThreadDpiAwareness::per_monitor_v2();
                if let Err(error) = run_window_thread(region, timer_ms, &ready_tx) {
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
            width: region.width as i32,
            height: region.height as i32,
            tick: 0,
        });
        let state_raw = Box::into_raw(state);
        let created = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
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

        bring_to_foreground(hwnd);
        let _ = ShowWindow(hwnd, SW_SHOW);
        SetTimer(Some(hwnd), WORKLOAD_TIMER_ID, timer_ms, None);
        let _ = ready.send(Ok(hwnd.0 as isize));

        let mut message = MSG::default();
        while GetMessageW(&mut message, None, 0, 0).as_bool() {
            let _ = TranslateMessage(&message);
            let _ = DispatchMessageW(&message);
        }
        Ok(())
    }
}

fn bring_to_foreground(hwnd: HWND) {
    unsafe {
        let previous = GetForegroundWindow();
        let foreground_thread = GetWindowThreadProcessId(previous, None);
        let current_thread = GetCurrentThreadId();
        let attached = foreground_thread != 0
            && foreground_thread != current_thread
            && AttachThreadInput(current_thread, foreground_thread, true).as_bool();
        let _ = BringWindowToTop(hwnd);
        let _ = SetForegroundWindow(hwnd);
        if attached {
            let _ = AttachThreadInput(current_thread, foreground_thread, false);
        }
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
                let _ = InvalidateRect(Some(hwnd), None, false);
                LRESULT(0)
            }
            WM_ERASEBKGND => LRESULT(1),
            WM_PAINT => {
                let mut paint = PAINTSTRUCT::default();
                let dc = BeginPaint(hwnd, &mut paint);
                if let Some(state) = window_state(hwnd) {
                    render_workload_frame(dc, &mut *state);
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
        let background = rgb(
            (tick.wrapping_mul(3) % 256) as u8,
            (tick.wrapping_mul(5) % 256) as u8,
            (tick.wrapping_mul(7) % 256) as u8,
        );
        fill_rect(dc, 0, 0, state.width, state.height, background);

        let rect_size = (state.width / 6).max(40);
        let rect_x = (tick.wrapping_mul(11) % ((state.width - rect_size).max(1) as u64)) as i32;
        let rect_y = (tick.wrapping_mul(7) % ((state.height - rect_size).max(1) as u64)) as i32;
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

        let ellipse_size = (state.width / 7).max(34);
        let ellipse_x =
            (tick.wrapping_mul(5) % ((state.width - ellipse_size).max(1) as u64)) as i32;
        let ellipse_y =
            (tick.wrapping_mul(13) % ((state.height - ellipse_size).max(1) as u64)) as i32;
        draw_ellipse(
            dc,
            ellipse_x,
            ellipse_y,
            ellipse_x + ellipse_size,
            ellipse_y + ellipse_size,
            rgb(32, 224, 255),
        );

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

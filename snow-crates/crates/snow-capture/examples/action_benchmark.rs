use std::fs;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use rustc_hash::FxHashMap;
use snow_capture::backend::CaptureBackendKind;
use snow_capture::frame::CaptureEvent;
use snow_capture::{
    CaptureOptions, CaptureRegion, CaptureStream, CaptureStreamConfig, CaptureSystem,
    CaptureTarget, CaptureWorkload, MonitorLayout, WindowId,
};

#[cfg(target_os = "windows")]
use windows::Win32::System::ProcessStatus::{
    K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS, PROCESS_MEMORY_COUNTERS_EX,
};
#[cfg(target_os = "windows")]
use windows::Win32::System::Threading::GetCurrentProcess;

const DEFAULT_WARMUP_FRAMES: usize = 30;
const DEFAULT_MEASURE_FRAMES: usize = 240;
const DEFAULT_ROUNDS: usize = 3;
const DEFAULT_MAX_REGRESSION_PCT: f64 = 10.0;
const DEFAULT_RECORDING_TARGET_FPS: u32 = 60;
const DEFAULT_RECORDING_BUFFER_DEPTH: usize = 6;
const DEFAULT_RECORDING_MAX_ERRORS: usize = 30;
const DEFAULT_RECORDING_MIN_FPS: u32 = 15;
const DEFAULT_RECORDING_FRAME_TIMEOUT_MS: u64 = 2_000;
const MIB: f64 = 1024.0 * 1024.0;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum BenchAction {
    Screenshot,
    Recording,
}

impl BenchAction {
    fn parse(raw: &str) -> Option<Self> {
        match raw.trim().to_ascii_lowercase().as_str() {
            "screenshot" | "shot" | "snap" => Some(Self::Screenshot),
            "recording" | "record" | "stream" | "recording-stream" => Some(Self::Recording),
            _ => None,
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::Screenshot => "screenshot",
            Self::Recording => "recording",
        }
    }
}

#[derive(Clone, Debug)]
enum BenchTarget {
    PrimaryMonitor,
    Region(CaptureRegion),
    Window(WindowId),
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
enum RegressionMetric {
    Setup,
    Start,
    FirstFrame,
    Avg,
    #[default]
    P50,
    P95,
    P99,
    AvgWorkingSetDelta,
    PeakWorkingSetDelta,
    AvgPrivateDelta,
    PeakPrivateDelta,
}

impl RegressionMetric {
    fn parse(raw: &str) -> Option<Self> {
        match raw.trim().to_ascii_lowercase().as_str() {
            "setup" => Some(Self::Setup),
            "start" | "stream_start" => Some(Self::Start),
            "first" | "first_frame" | "first-frame" => Some(Self::FirstFrame),
            "avg" | "average" => Some(Self::Avg),
            "p50" | "median" => Some(Self::P50),
            "p95" => Some(Self::P95),
            "p99" => Some(Self::P99),
            "avg_ws" | "avg_ws_delta" | "avg-working-set" => Some(Self::AvgWorkingSetDelta),
            "peak_ws" | "peak_ws_delta" | "peak-working-set" => Some(Self::PeakWorkingSetDelta),
            "avg_private" | "avg_private_delta" => Some(Self::AvgPrivateDelta),
            "peak_private" | "peak_private_delta" => Some(Self::PeakPrivateDelta),
            _ => None,
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::Setup => "setup",
            Self::Start => "start",
            Self::FirstFrame => "first_frame",
            Self::Avg => "avg",
            Self::P50 => "p50",
            Self::P95 => "p95",
            Self::P99 => "p99",
            Self::AvgWorkingSetDelta => "avg_ws_delta",
            Self::PeakWorkingSetDelta => "peak_ws_delta",
            Self::AvgPrivateDelta => "avg_private_delta",
            Self::PeakPrivateDelta => "peak_private_delta",
        }
    }

    fn column_name(self) -> &'static str {
        match self {
            Self::Setup => "setup_ms",
            Self::Start => "start_ms",
            Self::FirstFrame => "first_frame_ms",
            Self::Avg => "avg_ms",
            Self::P50 => "p50_ms",
            Self::P95 => "p95_ms",
            Self::P99 => "p99_ms",
            Self::AvgWorkingSetDelta => "avg_ws_delta_mb",
            Self::PeakWorkingSetDelta => "peak_ws_delta_mb",
            Self::AvgPrivateDelta => "avg_private_delta_mb",
            Self::PeakPrivateDelta => "peak_private_delta_mb",
        }
    }
}

#[derive(Clone, Debug)]
struct Config {
    warmup_frames: usize,
    measure_frames: usize,
    rounds: usize,
    sample_interval: Option<Duration>,
    actions: Vec<BenchAction>,
    backends: Vec<CaptureBackendKind>,
    target: BenchTarget,
    target_label_override: Option<String>,
    baseline_path: Option<PathBuf>,
    save_baseline_path: Option<PathBuf>,
    max_regression_pct: f64,
    regression_metrics: Vec<RegressionMetric>,
    max_duplicate_pct: Option<f64>,
    recording_target_fps: u32,
    recording_buffer_depth: usize,
    recording_max_consecutive_errors: usize,
    recording_adaptive_fps: bool,
    recording_min_fps: u32,
    recording_frame_timeout: Duration,
}

#[derive(Clone, Debug)]
struct BenchResult {
    action: BenchAction,
    backend: CaptureBackendKind,
    target_label: String,
    setup_ms: f64,
    start_ms: f64,
    first_frame_ms: f64,
    capture_only_ms: f64,
    avg_ms: f64,
    p50_ms: f64,
    p95_ms: f64,
    p99_ms: f64,
    min_ms: f64,
    max_ms: f64,
    stddev_ms: f64,
    fps: f64,
    duplicate_pct: f64,
    fresh_fps: f64,
    dropped_frames: u64,
    errors_recovered: u64,
    buffer_peak_pct: f64,
    base_ws_mb: f64,
    base_private_mb: f64,
    avg_ws_delta_mb: f64,
    peak_ws_delta_mb: f64,
    post_ws_delta_mb: f64,
    avg_private_delta_mb: f64,
    peak_private_delta_mb: f64,
    post_private_delta_mb: f64,
}

impl BenchResult {
    fn metric_value(&self, metric: RegressionMetric) -> f64 {
        match metric {
            RegressionMetric::Setup => self.setup_ms,
            RegressionMetric::Start => self.start_ms,
            RegressionMetric::FirstFrame => self.first_frame_ms,
            RegressionMetric::Avg => self.avg_ms,
            RegressionMetric::P50 => self.p50_ms,
            RegressionMetric::P95 => self.p95_ms,
            RegressionMetric::P99 => self.p99_ms,
            RegressionMetric::AvgWorkingSetDelta => self.avg_ws_delta_mb,
            RegressionMetric::PeakWorkingSetDelta => self.peak_ws_delta_mb,
            RegressionMetric::AvgPrivateDelta => self.avg_private_delta_mb,
            RegressionMetric::PeakPrivateDelta => self.peak_private_delta_mb,
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
struct SampleStats {
    avg: f64,
    p50: f64,
    p95: f64,
    p99: f64,
    min: f64,
    max: f64,
    stddev: f64,
}

#[derive(Clone, Copy, Debug, Default)]
struct ProcessMemorySample {
    working_set_bytes: u64,
    private_bytes: u64,
}

impl ProcessMemorySample {
    fn capture() -> Result<Self> {
        #[cfg(target_os = "windows")]
        {
            let handle = unsafe { GetCurrentProcess() };
            let mut counters = PROCESS_MEMORY_COUNTERS_EX::default();
            unsafe {
                K32GetProcessMemoryInfo(
                    handle,
                    &mut counters as *mut _ as *mut PROCESS_MEMORY_COUNTERS,
                    std::mem::size_of::<PROCESS_MEMORY_COUNTERS_EX>() as u32,
                )
            }
            .ok()
            .context("K32GetProcessMemoryInfo failed")?;

            Ok(Self {
                working_set_bytes: counters.WorkingSetSize as u64,
                private_bytes: counters.PrivateUsage as u64,
            })
        }

        #[cfg(not(target_os = "windows"))]
        {
            Ok(Self::default())
        }
    }
}

#[derive(Debug, Default)]
struct MemoryAccumulator {
    base_ws_mb: Vec<f64>,
    base_private_mb: Vec<f64>,
    measure_ws_delta_mb: Vec<f64>,
    measure_private_delta_mb: Vec<f64>,
    post_ws_delta_mb: Vec<f64>,
    post_private_delta_mb: Vec<f64>,
    peak_ws_delta_mb: f64,
    peak_private_delta_mb: f64,
}

impl MemoryAccumulator {
    fn record_base(&mut self, sample: ProcessMemorySample) {
        self.base_ws_mb.push(bytes_to_mib(sample.working_set_bytes));
        self.base_private_mb
            .push(bytes_to_mib(sample.private_bytes));
    }

    fn record_warmup(&mut self, base: ProcessMemorySample, sample: ProcessMemorySample) {
        let (ws_delta_mb, private_delta_mb) = memory_deltas_mb(base, sample);
        self.update_peaks(ws_delta_mb, private_delta_mb);
    }

    fn record_measure(&mut self, base: ProcessMemorySample, sample: ProcessMemorySample) {
        let (ws_delta_mb, private_delta_mb) = memory_deltas_mb(base, sample);
        self.measure_ws_delta_mb.push(ws_delta_mb);
        self.measure_private_delta_mb.push(private_delta_mb);
        self.update_peaks(ws_delta_mb, private_delta_mb);
    }

    fn record_post(&mut self, base: ProcessMemorySample, sample: ProcessMemorySample) {
        let (ws_delta_mb, private_delta_mb) = memory_deltas_mb(base, sample);
        self.post_ws_delta_mb.push(ws_delta_mb);
        self.post_private_delta_mb.push(private_delta_mb);
        self.update_peaks(ws_delta_mb, private_delta_mb);
    }

    fn update_peaks(&mut self, ws_delta_mb: f64, private_delta_mb: f64) {
        self.peak_ws_delta_mb = self.peak_ws_delta_mb.max(ws_delta_mb);
        self.peak_private_delta_mb = self.peak_private_delta_mb.max(private_delta_mb);
    }
}

fn bytes_to_mib(bytes: u64) -> f64 {
    bytes as f64 / MIB
}

fn memory_deltas_mb(base: ProcessMemorySample, sample: ProcessMemorySample) -> (f64, f64) {
    (
        (sample.working_set_bytes as f64 - base.working_set_bytes as f64) / MIB,
        (sample.private_bytes as f64 - base.private_bytes as f64) / MIB,
    )
}

fn backend_name(kind: CaptureBackendKind) -> &'static str {
    match kind {
        CaptureBackendKind::Auto => "auto",
        CaptureBackendKind::DxgiDuplication => "dxgi",
        CaptureBackendKind::WindowsGraphicsCapture => "wgc",
        CaptureBackendKind::Gdi => "gdi",
    }
}

fn parse_backend(token: &str) -> Option<CaptureBackendKind> {
    match token.trim().to_ascii_lowercase().as_str() {
        "dxgi" | "dxgi-duplication" | "duplication" => Some(CaptureBackendKind::DxgiDuplication),
        "wgc" | "windowsgraphicscapture" | "windows-graphics-capture" => {
            Some(CaptureBackendKind::WindowsGraphicsCapture)
        }
        "gdi" => Some(CaptureBackendKind::Gdi),
        "auto" => Some(CaptureBackendKind::Auto),
        _ => None,
    }
}

fn parse_backends(csv: &str) -> Result<Vec<CaptureBackendKind>> {
    let mut out = Vec::new();
    for token in csv.split(',') {
        if token.trim().is_empty() {
            continue;
        }
        let Some(kind) = parse_backend(token) else {
            bail!("unknown backend token in --backends: {token}");
        };
        if !out.contains(&kind) {
            out.push(kind);
        }
    }
    if out.is_empty() {
        bail!("--backends resolved to empty backend list");
    }
    Ok(out)
}

fn parse_actions(csv: &str) -> Result<Vec<BenchAction>> {
    let mut out = Vec::new();
    for token in csv.split(',') {
        let trimmed = token.trim();
        if trimmed.is_empty() {
            continue;
        }
        if matches!(trimmed.to_ascii_lowercase().as_str(), "both" | "all") {
            if !out.contains(&BenchAction::Screenshot) {
                out.push(BenchAction::Screenshot);
            }
            if !out.contains(&BenchAction::Recording) {
                out.push(BenchAction::Recording);
            }
            continue;
        }
        let Some(action) = BenchAction::parse(trimmed) else {
            bail!("unknown action token in --actions: {token}");
        };
        if !out.contains(&action) {
            out.push(action);
        }
    }
    if out.is_empty() {
        bail!("--actions resolved to empty action list");
    }
    Ok(out)
}

fn parse_regression_metrics(csv: &str) -> Result<Vec<RegressionMetric>> {
    let mut metrics = Vec::new();
    for token in csv.split(',') {
        if token.trim().is_empty() {
            continue;
        }
        let Some(metric) = RegressionMetric::parse(token) else {
            bail!("invalid regression metric token `{token}` in --regression-metrics");
        };
        if !metrics.contains(&metric) {
            metrics.push(metric);
        }
    }
    if metrics.is_empty() {
        bail!("--regression-metrics resolved to empty metric list");
    }
    Ok(metrics)
}

fn parse_usize_arg(flag: &str, value: Option<&str>) -> Result<usize> {
    let Some(raw) = value else {
        bail!("{flag} requires a value");
    };
    raw.parse::<usize>()
        .with_context(|| format!("failed to parse {flag} value: {raw}"))
}

fn parse_u32_arg(flag: &str, value: Option<&str>) -> Result<u32> {
    let Some(raw) = value else {
        bail!("{flag} requires a value");
    };
    raw.parse::<u32>()
        .with_context(|| format!("failed to parse {flag} value: {raw}"))
}

fn parse_u64_arg(flag: &str, value: Option<&str>) -> Result<u64> {
    let Some(raw) = value else {
        bail!("{flag} requires a value");
    };
    raw.parse::<u64>()
        .with_context(|| format!("failed to parse {flag} value: {raw}"))
}

fn parse_f64_arg(flag: &str, value: Option<&str>) -> Result<f64> {
    let Some(raw) = value else {
        bail!("{flag} requires a value");
    };
    raw.parse::<f64>()
        .with_context(|| format!("failed to parse {flag} value: {raw}"))
}

fn parse_window_handle(raw: &str) -> Result<isize> {
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        bail!("window handle cannot be empty");
    }

    let parsed_u64 = if let Some(hex) = trimmed
        .strip_prefix("0x")
        .or_else(|| trimmed.strip_prefix("0X"))
    {
        u64::from_str_radix(hex, 16)
            .with_context(|| format!("failed to parse hex window handle: {trimmed}"))?
    } else {
        trimmed
            .parse::<u64>()
            .with_context(|| format!("failed to parse window handle: {trimmed}"))?
    };

    if parsed_u64 > isize::MAX as u64 {
        bail!("window handle out of range for this platform: {trimmed}");
    }
    Ok(parsed_u64 as isize)
}

fn window_under_cursor() -> Result<WindowId> {
    use windows::Win32::Foundation::POINT;
    use windows::Win32::UI::WindowsAndMessaging::{
        GA_ROOT, GetAncestor, GetCursorPos, WindowFromPoint,
    };

    let mut pt = POINT::default();
    unsafe { GetCursorPos(&mut pt) }
        .ok()
        .context("GetCursorPos failed while resolving benchmark window")?;

    let hwnd = unsafe { WindowFromPoint(pt) };
    if hwnd.0.is_null() {
        bail!("no window found under cursor at ({}, {})", pt.x, pt.y);
    }

    let root = unsafe { GetAncestor(hwnd, GA_ROOT) };
    let handle = if root.0.is_null() { hwnd } else { root };
    Ok(WindowId::from_raw_handle(handle.0 as isize))
}

fn parse_region_csv(raw: &str) -> Result<CaptureRegion> {
    let parts: Vec<&str> = raw.split(',').map(|part| part.trim()).collect();
    if parts.len() != 4 {
        bail!("--region expects x,y,width,height (got: {raw})");
    }

    let x = parts[0]
        .parse::<i32>()
        .with_context(|| format!("failed to parse region x: {}", parts[0]))?;
    let y = parts[1]
        .parse::<i32>()
        .with_context(|| format!("failed to parse region y: {}", parts[1]))?;
    let width = parts[2]
        .parse::<u32>()
        .with_context(|| format!("failed to parse region width: {}", parts[2]))?;
    let height = parts[3]
        .parse::<u32>()
        .with_context(|| format!("failed to parse region height: {}", parts[3]))?;
    CaptureRegion::new(x, y, width, height).context("invalid --region dimensions")
}

fn parse_size_2d(raw: &str) -> Result<(u32, u32)> {
    let trimmed = raw.trim();
    let Some((w_raw, h_raw)) = trimmed.split_once('x').or_else(|| trimmed.split_once('X')) else {
        bail!("expected <width>x<height>, got: {raw}");
    };
    let width = w_raw
        .trim()
        .parse::<u32>()
        .with_context(|| format!("failed to parse width in {raw}"))?;
    let height = h_raw
        .trim()
        .parse::<u32>()
        .with_context(|| format!("failed to parse height in {raw}"))?;
    if width == 0 || height == 0 {
        bail!("region dimensions must be > 0: {raw}");
    }
    Ok((width, height))
}

fn centered_primary_region(width: u32, height: u32) -> Result<CaptureRegion> {
    let layout = MonitorLayout::snapshot().context("failed to snapshot monitor layout")?;
    let primary = layout
        .monitors
        .iter()
        .find(|monitor| monitor.monitor.is_primary())
        .or_else(|| layout.monitors.first())
        .context("no monitor available for --region-center")?;

    if primary.width == 0 || primary.height == 0 {
        bail!("primary monitor has zero-sized bounds");
    }

    let fit_width = width.min(primary.width);
    let fit_height = height.min(primary.height);
    let x = i32::try_from(i64::from(primary.x) + i64::from((primary.width - fit_width) / 2))
        .context("centered region x overflowed i32 range")?;
    let y = i32::try_from(i64::from(primary.y) + i64::from((primary.height - fit_height) / 2))
        .context("centered region y overflowed i32 range")?;
    CaptureRegion::new(x, y, fit_width, fit_height)
        .context("failed to build centered primary region")
}

fn target_label(target: &BenchTarget, override_label: Option<&str>) -> String {
    if let Some(label) = override_label {
        return label.to_string();
    }
    match target {
        BenchTarget::PrimaryMonitor => "primary-monitor".to_string(),
        BenchTarget::Region(region) => {
            format!(
                "region:{}:{}:{}:{}",
                region.x, region.y, region.width, region.height
            )
        }
        BenchTarget::Window(window) => format!("window:{}", window.stable_id()),
    }
}

fn print_usage() {
    println!(
        "Usage: cargo run --release --example action_benchmark -- [options]
  --actions <csv>                   screenshot,recording,both (default: both)
  --backends <csv>                  dxgi,wgc,gdi (default: all three)
  --warmup <n>                      warmup frames/events per round (default: {DEFAULT_WARMUP_FRAMES})
  --frames <n>                      measured frames/events per round (default: {DEFAULT_MEASURE_FRAMES})
  --rounds <n>                      number of rounds per action/backend (default: {DEFAULT_ROUNDS})
  --sample-interval-ms <ms>         sleep between independent screenshot samples (default: 0)
  --recording-target-fps <n>        target stream fps for recording benchmark (default: {DEFAULT_RECORDING_TARGET_FPS})
  --recording-buffer-depth <n>      stream buffer depth (default: {DEFAULT_RECORDING_BUFFER_DEPTH})
  --recording-max-errors <n>        max consecutive transient errors (default: {DEFAULT_RECORDING_MAX_ERRORS})
  --recording-adaptive-fps          enable adaptive fps for recording stream
  --recording-min-fps <n>           min fps when adaptive mode is enabled (default: {DEFAULT_RECORDING_MIN_FPS})
  --recording-frame-timeout-ms <n>  timeout while waiting for stream events (default: {DEFAULT_RECORDING_FRAME_TIMEOUT_MS})
  --window-under-cursor             benchmark the window under the cursor
  --window-handle <hwnd>            benchmark a specific top-level window
  --region <x,y,w,h>                benchmark a fixed region in virtual desktop coordinates
  --region-center <wxh>             benchmark a centered region on the primary monitor
  --target-label <name>             override target label written to csv/baseline
  --save-baseline <path>            write full benchmark rows to a csv file
  --baseline <path>                 compare current rows against a saved baseline csv
  --max-regression-pct <pct>        fail when a regression exceeds this limit (default: {DEFAULT_MAX_REGRESSION_PCT})
  --regression-metrics <csv>        setup,start,first_frame,avg,p50,p95,p99,avg_ws,peak_ws,avg_private,peak_private
  --max-duplicate-pct <pct>         fail when duplicate-frame ratio exceeds this limit

Notes:
  screenshot benchmarks recreate and drop a fresh session for every sample
  screenshot working-set deltas are process-global; use one backend per run for cold memory comparisons
  screenshot `auto` with the default policy resolves to the GDI path

Examples:
  cargo run --release --example action_benchmark -- --actions screenshot,recording --region-center 1920x1080
  cargo run --release --example action_benchmark -- --actions recording --backends dxgi --window-under-cursor --recording-target-fps 60
  cargo run --release --example action_benchmark -- --actions screenshot,recording --save-baseline target/perf/action-benchmark.csv
  cargo run --release --example action_benchmark -- --actions screenshot,recording --baseline target/perf/action-benchmark.csv --regression-metrics p50,p95,peak_ws"
    );
}

fn parse_args() -> Result<Config> {
    let mut warmup_frames = DEFAULT_WARMUP_FRAMES;
    let mut measure_frames = DEFAULT_MEASURE_FRAMES;
    let mut rounds = DEFAULT_ROUNDS;
    let mut sample_interval = None;
    let mut actions = vec![BenchAction::Screenshot, BenchAction::Recording];
    let mut backends = vec![
        CaptureBackendKind::DxgiDuplication,
        CaptureBackendKind::WindowsGraphicsCapture,
        CaptureBackendKind::Gdi,
    ];
    let mut target = BenchTarget::PrimaryMonitor;
    let mut target_label_override = None;
    let mut baseline_path = None;
    let mut save_baseline_path = None;
    let mut max_regression_pct = DEFAULT_MAX_REGRESSION_PCT;
    let mut regression_metrics = vec![RegressionMetric::default()];
    let mut max_duplicate_pct = None;
    let mut recording_target_fps = DEFAULT_RECORDING_TARGET_FPS;
    let mut recording_buffer_depth = DEFAULT_RECORDING_BUFFER_DEPTH;
    let mut recording_max_consecutive_errors = DEFAULT_RECORDING_MAX_ERRORS;
    let mut recording_adaptive_fps = false;
    let mut recording_min_fps = DEFAULT_RECORDING_MIN_FPS;
    let mut recording_frame_timeout = Duration::from_millis(DEFAULT_RECORDING_FRAME_TIMEOUT_MS);

    let args: Vec<String> = std::env::args().collect();
    let mut i = 1usize;
    while i < args.len() {
        match args[i].as_str() {
            "--help" | "-h" => {
                print_usage();
                std::process::exit(0);
            }
            "--actions" => {
                let Some(raw) = args.get(i + 1).map(String::as_str) else {
                    bail!("--actions requires a comma-separated value");
                };
                actions = parse_actions(raw)?;
                i += 2;
            }
            "--warmup" => {
                warmup_frames = parse_usize_arg("--warmup", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--frames" => {
                measure_frames = parse_usize_arg("--frames", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--rounds" => {
                rounds = parse_usize_arg("--rounds", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--sample-interval-ms" => {
                let raw_value =
                    parse_f64_arg("--sample-interval-ms", args.get(i + 1).map(String::as_str))?;
                if !raw_value.is_finite() {
                    bail!("--sample-interval-ms must be a finite number");
                }
                if raw_value < 0.0 {
                    bail!("--sample-interval-ms must be >= 0");
                }
                sample_interval = if raw_value > 0.0 {
                    Some(
                        Duration::try_from_secs_f64(raw_value / 1000.0).with_context(|| {
                            format!("--sample-interval-ms is out of range: {raw_value}")
                        })?,
                    )
                } else {
                    None
                };
                i += 2;
            }
            "--backends" => {
                let Some(raw) = args.get(i + 1).map(String::as_str) else {
                    bail!("--backends requires a comma-separated value");
                };
                backends = parse_backends(raw)?;
                i += 2;
            }
            "--window-under-cursor" => {
                target = BenchTarget::Window(window_under_cursor()?);
                i += 1;
            }
            "--window-handle" => {
                let Some(raw) = args.get(i + 1).map(String::as_str) else {
                    bail!("--window-handle requires a value (decimal or hex, e.g. 0x1234)");
                };
                target = BenchTarget::Window(WindowId::from_raw_handle(parse_window_handle(raw)?));
                i += 2;
            }
            "--region" => {
                let Some(raw) = args.get(i + 1).map(String::as_str) else {
                    bail!("--region requires x,y,width,height");
                };
                target = BenchTarget::Region(parse_region_csv(raw)?);
                i += 2;
            }
            "--region-center" => {
                let Some(raw) = args.get(i + 1).map(String::as_str) else {
                    bail!("--region-center requires <width>x<height>");
                };
                let (width, height) = parse_size_2d(raw)?;
                target = BenchTarget::Region(centered_primary_region(width, height)?);
                i += 2;
            }
            "--target-label" => {
                let Some(raw) = args.get(i + 1) else {
                    bail!("--target-label requires a value");
                };
                let trimmed = raw.trim();
                if trimmed.is_empty() {
                    bail!("--target-label cannot be empty");
                }
                target_label_override = Some(trimmed.to_string());
                i += 2;
            }
            "--save-baseline" => {
                let Some(raw) = args.get(i + 1) else {
                    bail!("--save-baseline requires a file path");
                };
                save_baseline_path = Some(PathBuf::from(raw));
                i += 2;
            }
            "--baseline" => {
                let Some(raw) = args.get(i + 1) else {
                    bail!("--baseline requires a file path");
                };
                baseline_path = Some(PathBuf::from(raw));
                i += 2;
            }
            "--max-regression-pct" => {
                max_regression_pct =
                    parse_f64_arg("--max-regression-pct", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--regression-metrics" => {
                let Some(raw) = args.get(i + 1).map(String::as_str) else {
                    bail!("--regression-metrics requires a comma-separated value");
                };
                regression_metrics = parse_regression_metrics(raw)?;
                i += 2;
            }
            "--max-duplicate-pct" => {
                max_duplicate_pct = Some(parse_f64_arg(
                    "--max-duplicate-pct",
                    args.get(i + 1).map(String::as_str),
                )?);
                i += 2;
            }
            "--recording-target-fps" => {
                recording_target_fps = parse_u32_arg(
                    "--recording-target-fps",
                    args.get(i + 1).map(String::as_str),
                )?;
                i += 2;
            }
            "--recording-buffer-depth" => {
                recording_buffer_depth = parse_usize_arg(
                    "--recording-buffer-depth",
                    args.get(i + 1).map(String::as_str),
                )?;
                i += 2;
            }
            "--recording-max-errors" => {
                recording_max_consecutive_errors = parse_usize_arg(
                    "--recording-max-errors",
                    args.get(i + 1).map(String::as_str),
                )?;
                i += 2;
            }
            "--recording-adaptive-fps" => {
                recording_adaptive_fps = true;
                i += 1;
            }
            "--recording-min-fps" => {
                recording_min_fps =
                    parse_u32_arg("--recording-min-fps", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--recording-frame-timeout-ms" => {
                let timeout_ms = parse_u64_arg(
                    "--recording-frame-timeout-ms",
                    args.get(i + 1).map(String::as_str),
                )?;
                if timeout_ms == 0 {
                    bail!("--recording-frame-timeout-ms must be > 0");
                }
                recording_frame_timeout = Duration::from_millis(timeout_ms);
                i += 2;
            }
            other => bail!("unknown argument: {other}. Use --help for usage."),
        }
    }

    if measure_frames == 0 {
        bail!("--frames must be > 0 so there are measured samples");
    }
    if rounds == 0 {
        bail!("--rounds must be > 0");
    }
    if recording_buffer_depth == 0 {
        bail!("--recording-buffer-depth must be > 0");
    }
    if recording_adaptive_fps && recording_min_fps == 0 {
        bail!("--recording-min-fps must be > 0 when adaptive fps is enabled");
    }
    if let Some(value) = max_duplicate_pct
        && (!value.is_finite() || !(0.0..=100.0).contains(&value))
    {
        bail!("--max-duplicate-pct must be in [0, 100]");
    }

    Ok(Config {
        warmup_frames,
        measure_frames,
        rounds,
        sample_interval,
        actions,
        backends,
        target,
        target_label_override,
        baseline_path,
        save_baseline_path,
        max_regression_pct,
        regression_metrics,
        max_duplicate_pct,
        recording_target_fps,
        recording_buffer_depth,
        recording_max_consecutive_errors,
        recording_adaptive_fps,
        recording_min_fps,
        recording_frame_timeout,
    })
}

fn capture_target_for(target: &BenchTarget) -> CaptureTarget {
    match target {
        BenchTarget::PrimaryMonitor => CaptureTarget::PrimaryMonitor,
        BenchTarget::Region(region) => CaptureTarget::Region(*region),
        BenchTarget::Window(window) => CaptureTarget::Window(*window),
    }
}

fn percentile(sorted: &[f64], p: f64) -> f64 {
    let n = sorted.len();
    if n == 0 {
        return 0.0;
    }
    let clamped = p.clamp(0.0, 1.0);
    let idx = ((n - 1) as f64 * clamped).round() as usize;
    sorted[idx]
}

fn mean_or_zero(samples: &[f64]) -> f64 {
    if samples.is_empty() {
        0.0
    } else {
        samples.iter().sum::<f64>() / samples.len() as f64
    }
}

fn summarize_samples(samples: &[f64]) -> Result<SampleStats> {
    if samples.is_empty() {
        bail!("no measured samples collected");
    }
    let mut sorted = samples.to_vec();
    sorted.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let avg = mean_or_zero(samples);
    let variance = samples
        .iter()
        .map(|sample| {
            let d = *sample - avg;
            d * d
        })
        .sum::<f64>()
        / samples.len() as f64;
    Ok(SampleStats {
        avg,
        p50: percentile(&sorted, 0.50),
        p95: percentile(&sorted, 0.95),
        p99: percentile(&sorted, 0.99),
        min: *sorted.first().unwrap(),
        max: *sorted.last().unwrap(),
        stddev: variance.sqrt(),
    })
}

fn elapsed_ms(start: Instant) -> f64 {
    start.elapsed().as_secs_f64() * 1000.0
}

fn make_stream_config(config: &Config) -> CaptureStreamConfig {
    CaptureStreamConfig {
        target_fps: config.recording_target_fps,
        buffer_depth: config.recording_buffer_depth,
        max_consecutive_errors: config.recording_max_consecutive_errors,
        adaptive_fps: config.recording_adaptive_fps,
        min_fps: config.recording_min_fps,
        pause_on_resolution_change: false,
        include_cursor: true,
    }
}

fn run_screenshot_backend(
    kind: CaptureBackendKind,
    config: &Config,
    bench_target: &BenchTarget,
    target_label_override: Option<&str>,
) -> Result<BenchResult> {
    let target = capture_target_for(bench_target);
    let target_label = target_label(bench_target, target_label_override);

    let total_samples = config
        .measure_frames
        .checked_mul(config.rounds)
        .context("screenshot benchmark sample count overflow")?;
    let mut capture_samples_ms = Vec::with_capacity(total_samples);
    let mut setup_samples_ms = Vec::with_capacity(total_samples);
    let mut capture_only_samples_ms = Vec::with_capacity(total_samples);
    let mut first_frame_samples_ms = Vec::with_capacity(total_samples);
    let mut duplicate_samples = 0usize;
    let mut memory = MemoryAccumulator::default();

    for _ in 0..config.rounds {
        let total_iterations = config.warmup_frames + config.measure_frames;

        for iteration_idx in 0..total_iterations {
            let base_memory = ProcessMemorySample::capture().with_context(|| {
                format!(
                    "failed to sample memory before screenshot benchmark ({})",
                    backend_name(kind)
                )
            })?;
            memory.record_base(base_memory);

            let sample_started = Instant::now();
            let setup_started = Instant::now();
            let system = CaptureSystem::builder()
                .with_backend_kind(kind)
                .build()
                .with_context(|| {
                    format!(
                        "failed to initialize screenshot system for {}",
                        backend_name(kind)
                    )
                })?;
            let mut session = system
                .open_session(target.clone(), Default::default())
                .with_context(|| {
                    format!(
                        "failed to initialize screenshot session for {}",
                        backend_name(kind)
                    )
                })?;
            let setup_ms = elapsed_ms(setup_started);

            let capture_started = Instant::now();
            let frame = session
                .capture()
                .with_context(|| format!("screenshot capture failed for {}", backend_name(kind)))?;
            let capture_only_ms = elapsed_ms(capture_started);
            let sample_ms = elapsed_ms(sample_started);

            let memory_sample = ProcessMemorySample::capture().with_context(|| {
                format!(
                    "failed to sample memory during screenshot benchmark ({})",
                    backend_name(kind)
                )
            })?;

            let is_warmup = iteration_idx < config.warmup_frames;
            if !is_warmup {
                setup_samples_ms.push(setup_ms);
                capture_only_samples_ms.push(capture_only_ms);
                capture_samples_ms.push(sample_ms);
                first_frame_samples_ms.push(sample_ms);
                if frame.metadata().is_duplicate() {
                    duplicate_samples = duplicate_samples.saturating_add(1);
                }
                memory.record_measure(base_memory, memory_sample);
            } else {
                memory.record_warmup(base_memory, memory_sample);
            }

            drop(frame);
            drop(session);

            let post_memory = ProcessMemorySample::capture().with_context(|| {
                format!(
                    "failed to sample memory after screenshot benchmark ({})",
                    backend_name(kind)
                )
            })?;
            memory.record_post(base_memory, post_memory);

            if let Some(interval) = config.sample_interval {
                std::thread::sleep(interval);
            }
        }
    }

    let stats = summarize_samples(&capture_samples_ms)?;
    let total_capture_ms: f64 = capture_samples_ms.iter().sum();
    let fresh_samples = capture_samples_ms.len().saturating_sub(duplicate_samples);
    let duplicate_pct = if capture_samples_ms.is_empty() {
        0.0
    } else {
        (duplicate_samples as f64 * 100.0) / capture_samples_ms.len() as f64
    };

    Ok(BenchResult {
        action: BenchAction::Screenshot,
        backend: kind,
        target_label,
        setup_ms: mean_or_zero(&setup_samples_ms),
        start_ms: 0.0,
        first_frame_ms: mean_or_zero(&first_frame_samples_ms),
        capture_only_ms: mean_or_zero(&capture_only_samples_ms),
        avg_ms: stats.avg,
        p50_ms: stats.p50,
        p95_ms: stats.p95,
        p99_ms: stats.p99,
        min_ms: stats.min,
        max_ms: stats.max,
        stddev_ms: stats.stddev,
        fps: if stats.avg > 0.0 {
            1000.0 / stats.avg
        } else {
            0.0
        },
        duplicate_pct,
        fresh_fps: if total_capture_ms > 0.0 {
            (fresh_samples as f64 * 1000.0) / total_capture_ms
        } else {
            0.0
        },
        dropped_frames: 0,
        errors_recovered: 0,
        buffer_peak_pct: 0.0,
        base_ws_mb: mean_or_zero(&memory.base_ws_mb),
        base_private_mb: mean_or_zero(&memory.base_private_mb),
        avg_ws_delta_mb: mean_or_zero(&memory.measure_ws_delta_mb),
        peak_ws_delta_mb: memory.peak_ws_delta_mb,
        post_ws_delta_mb: mean_or_zero(&memory.post_ws_delta_mb),
        avg_private_delta_mb: mean_or_zero(&memory.measure_private_delta_mb),
        peak_private_delta_mb: memory.peak_private_delta_mb,
        post_private_delta_mb: mean_or_zero(&memory.post_private_delta_mb),
    })
}

fn run_recording_backend(
    kind: CaptureBackendKind,
    config: &Config,
    bench_target: &BenchTarget,
    target_label_override: Option<&str>,
) -> Result<BenchResult> {
    let target = capture_target_for(bench_target);
    let target_label = target_label(bench_target, target_label_override);
    let stream_config = make_stream_config(config);

    let total_samples = config
        .measure_frames
        .checked_mul(config.rounds)
        .context("recording benchmark sample count overflow")?;
    let mut capture_samples_ms = Vec::with_capacity(total_samples);
    let mut setup_samples_ms = Vec::with_capacity(config.rounds);
    let mut start_samples_ms = Vec::with_capacity(config.rounds);
    let mut first_frame_samples_ms = Vec::with_capacity(config.rounds);
    let mut duplicate_samples = 0usize;
    let mut measured_wall_time_s = 0.0f64;
    let mut dropped_frames = 0u64;
    let mut errors_recovered = 0u64;
    let mut buffer_peak_pct = 0.0f64;
    let mut memory = MemoryAccumulator::default();

    for _ in 0..config.rounds {
        let base_memory = ProcessMemorySample::capture().with_context(|| {
            format!(
                "failed to sample memory before recording benchmark ({})",
                backend_name(kind)
            )
        })?;
        memory.record_base(base_memory);

        let setup_started = Instant::now();
        let system = CaptureSystem::builder()
            .with_backend_kind(kind)
            .build()
            .with_context(|| {
                format!(
                    "failed to initialize recording system for {}",
                    backend_name(kind)
                )
            })?;
        let session = system
            .open_session(
                target.clone(),
                CaptureOptions {
                    workload: CaptureWorkload::Continuous,
                    ..Default::default()
                },
            )
            .with_context(|| {
                format!(
                    "failed to initialize recording session for {}",
                    backend_name(kind)
                )
            })?;
        setup_samples_ms.push(elapsed_ms(setup_started));

        let stream_started = Instant::now();
        let stream = CaptureStream::spawn(session, stream_config.clone()).with_context(|| {
            format!(
                "failed to start recording stream for {}",
                backend_name(kind)
            )
        })?;
        start_samples_ms.push(elapsed_ms(stream_started));

        let stats = stream.stats().clone();
        let mut remaining_warmup = config.warmup_frames;
        let mut measured_frames = 0usize;
        let mut first_frame_recorded = false;
        let mut measure_started_at = None;

        loop {
            buffer_peak_pct = buffer_peak_pct.max(stream.buffer_fill_percent() * 100.0);

            match stream.recv_timeout(config.recording_frame_timeout) {
                Ok(CaptureEvent::Frame(frame)) => {
                    let memory_sample = ProcessMemorySample::capture().with_context(|| {
                        format!(
                            "failed to sample memory during recording benchmark ({})",
                            backend_name(kind)
                        )
                    })?;

                    if !first_frame_recorded {
                        first_frame_samples_ms.push(elapsed_ms(stream_started));
                        first_frame_recorded = true;
                    }

                    if remaining_warmup > 0 {
                        remaining_warmup -= 1;
                        memory.record_warmup(base_memory, memory_sample);
                        continue;
                    }

                    if measure_started_at.is_none() {
                        measure_started_at = Some(Instant::now());
                    }

                    measured_frames += 1;
                    let capture_duration_ms = frame
                        .metadata()
                        .capture_duration()
                        .map(|duration| duration.as_secs_f64() * 1000.0)
                        .unwrap_or_default();
                    capture_samples_ms.push(capture_duration_ms);
                    if frame.metadata().is_duplicate() {
                        duplicate_samples = duplicate_samples.saturating_add(1);
                    }
                    memory.record_measure(base_memory, memory_sample);

                    if measured_frames >= config.measure_frames {
                        break;
                    }
                }
                Ok(CaptureEvent::FramesDropped { .. }) => {}
                Ok(CaptureEvent::ResolutionChanged { .. }) => {}
                Ok(CaptureEvent::Paused { .. }) => {}
                Ok(CaptureEvent::Resumed { .. }) => {}
                Ok(CaptureEvent::StreamEnded) => {
                    bail!(
                        "recording stream ended before collecting {} measured frames for {}",
                        config.measure_frames,
                        backend_name(kind)
                    );
                }
                Ok(CaptureEvent::Error(err)) => {
                    bail!("recording stream error for {}: {err}", backend_name(kind));
                }
                Err(snow_core::error::RecvTimeoutError::Timeout) => {
                    bail!(
                        "timed out waiting for recording frame from {} after {:?}",
                        backend_name(kind),
                        config.recording_frame_timeout
                    );
                }
                Err(snow_core::error::RecvTimeoutError::Disconnected) => {
                    bail!(
                        "recording stream disconnected unexpectedly for {}",
                        backend_name(kind)
                    );
                }
            }
        }

        if let Some(measure_started) = measure_started_at {
            measured_wall_time_s += measure_started.elapsed().as_secs_f64();
        }

        let _tail = stream.stop_and_drain();
        let snapshot = stats.snapshot();
        dropped_frames = dropped_frames.saturating_add(snapshot.frames_dropped);
        errors_recovered = errors_recovered.saturating_add(snapshot.errors_recovered);

        let post_memory = ProcessMemorySample::capture().with_context(|| {
            format!(
                "failed to sample memory after recording benchmark ({})",
                backend_name(kind)
            )
        })?;
        memory.record_post(base_memory, post_memory);
    }

    let stats = summarize_samples(&capture_samples_ms)?;
    let fresh_samples = capture_samples_ms.len().saturating_sub(duplicate_samples);
    let duplicate_pct = if capture_samples_ms.is_empty() {
        0.0
    } else {
        (duplicate_samples as f64 * 100.0) / capture_samples_ms.len() as f64
    };

    Ok(BenchResult {
        action: BenchAction::Recording,
        backend: kind,
        target_label,
        setup_ms: mean_or_zero(&setup_samples_ms),
        start_ms: mean_or_zero(&start_samples_ms),
        first_frame_ms: mean_or_zero(&first_frame_samples_ms),
        capture_only_ms: 0.0,
        avg_ms: stats.avg,
        p50_ms: stats.p50,
        p95_ms: stats.p95,
        p99_ms: stats.p99,
        min_ms: stats.min,
        max_ms: stats.max,
        stddev_ms: stats.stddev,
        fps: if measured_wall_time_s > 0.0 {
            capture_samples_ms.len() as f64 / measured_wall_time_s
        } else {
            0.0
        },
        duplicate_pct,
        fresh_fps: if measured_wall_time_s > 0.0 {
            fresh_samples as f64 / measured_wall_time_s
        } else {
            0.0
        },
        dropped_frames,
        errors_recovered,
        buffer_peak_pct,
        base_ws_mb: mean_or_zero(&memory.base_ws_mb),
        base_private_mb: mean_or_zero(&memory.base_private_mb),
        avg_ws_delta_mb: mean_or_zero(&memory.measure_ws_delta_mb),
        peak_ws_delta_mb: memory.peak_ws_delta_mb,
        post_ws_delta_mb: mean_or_zero(&memory.post_ws_delta_mb),
        avg_private_delta_mb: mean_or_zero(&memory.measure_private_delta_mb),
        peak_private_delta_mb: memory.peak_private_delta_mb,
        post_private_delta_mb: mean_or_zero(&memory.post_private_delta_mb),
    })
}

fn run_action_backend(
    action: BenchAction,
    backend: CaptureBackendKind,
    config: &Config,
) -> Result<BenchResult> {
    match action {
        BenchAction::Screenshot => run_screenshot_backend(
            backend,
            config,
            &config.target,
            config.target_label_override.as_deref(),
        ),
        BenchAction::Recording => run_recording_backend(
            backend,
            config,
            &config.target,
            config.target_label_override.as_deref(),
        ),
    }
}

fn csv_header() -> &'static str {
    "action,target,backend,setup_ms,start_ms,first_frame_ms,capture_only_ms,avg_ms,p50_ms,p95_ms,p99_ms,min_ms,max_ms,stddev_ms,fps,duplicate_pct,fresh_fps,dropped_frames,errors_recovered,buffer_peak_pct,base_ws_mb,base_private_mb,avg_ws_delta_mb,peak_ws_delta_mb,post_ws_delta_mb,avg_private_delta_mb,peak_private_delta_mb,post_private_delta_mb"
}

fn csv_row(result: &BenchResult) -> String {
    format!(
        "{},{},{},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.3},{:.6},{},{},{:.3},{:.3},{:.3},{:.3},{:.3},{:.3},{:.3},{:.3},{:.3}",
        result.action.as_str(),
        result.target_label,
        backend_name(result.backend),
        result.setup_ms,
        result.start_ms,
        result.first_frame_ms,
        result.capture_only_ms,
        result.avg_ms,
        result.p50_ms,
        result.p95_ms,
        result.p99_ms,
        result.min_ms,
        result.max_ms,
        result.stddev_ms,
        result.fps,
        result.duplicate_pct,
        result.fresh_fps,
        result.dropped_frames,
        result.errors_recovered,
        result.buffer_peak_pct,
        result.base_ws_mb,
        result.base_private_mb,
        result.avg_ws_delta_mb,
        result.peak_ws_delta_mb,
        result.post_ws_delta_mb,
        result.avg_private_delta_mb,
        result.peak_private_delta_mb,
        result.post_private_delta_mb,
    )
}

fn baseline_key(action: &str, target: &str, backend: &str) -> String {
    format!("{action}|{target}|{backend}")
}

fn save_baseline(path: &PathBuf, results: &[BenchResult]) -> Result<()> {
    if let Some(parent) = path.parent()
        && !parent.as_os_str().is_empty()
    {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create baseline directory {}", parent.display()))?;
    }
    let mut out = String::from(csv_header());
    out.push('\n');
    for result in results {
        out.push_str(&csv_row(result));
        out.push('\n');
    }
    fs::write(path, out)
        .with_context(|| format!("failed to write baseline file {}", path.display()))
}

fn load_baseline(path: &PathBuf) -> Result<FxHashMap<String, FxHashMap<String, f64>>> {
    let text = fs::read_to_string(path)
        .with_context(|| format!("failed to read baseline file {}", path.display()))?;
    let mut lines = text.lines();
    let header_line = lines
        .next()
        .context("baseline file is empty (missing header row)")?;
    let header: Vec<&str> = header_line.split(',').map(|column| column.trim()).collect();

    let action_idx = header
        .iter()
        .position(|column| column.eq_ignore_ascii_case("action"))
        .context("baseline header is missing required `action` column")?;
    let target_idx = header
        .iter()
        .position(|column| column.eq_ignore_ascii_case("target"))
        .context("baseline header is missing required `target` column")?;
    let backend_idx = header
        .iter()
        .position(|column| column.eq_ignore_ascii_case("backend"))
        .context("baseline header is missing required `backend` column")?;

    let mut out = FxHashMap::default();
    for (line_offset, line) in lines.enumerate() {
        let line_number = line_offset + 2;
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        let parts: Vec<&str> = trimmed.split(',').collect();
        if parts.len() != header.len() {
            bail!(
                "invalid baseline line {line_number}: expected {} columns, got {}",
                header.len(),
                parts.len()
            );
        }

        let action = parts[action_idx].trim();
        let target = parts[target_idx].trim();
        let backend = parts[backend_idx].trim();
        let key = baseline_key(action, target, backend);

        let mut metrics = FxHashMap::default();
        for (idx, column_name) in header.iter().enumerate() {
            if idx == action_idx || idx == target_idx || idx == backend_idx {
                continue;
            }
            let value = parts[idx].trim();
            if value.is_empty() {
                continue;
            }
            let parsed = value.parse::<f64>().with_context(|| {
                format!(
                    "invalid numeric value in baseline line {line_number}, column {column_name}: {value}"
                )
            })?;
            metrics.insert((*column_name).to_string(), parsed);
        }
        out.insert(key, metrics);
    }
    Ok(out)
}

fn check_regression(
    baseline: &FxHashMap<String, FxHashMap<String, f64>>,
    current: &[BenchResult],
    max_regression_pct: f64,
    metric: RegressionMetric,
) -> Result<()> {
    let mut regressions = Vec::new();
    let column_name = metric.column_name();

    for result in current {
        let key = baseline_key(
            result.action.as_str(),
            &result.target_label,
            backend_name(result.backend),
        );
        let Some(entry) = baseline.get(&key) else {
            continue;
        };
        let Some(base_value) = entry.get(column_name).copied() else {
            continue;
        };
        if base_value <= 0.0 {
            continue;
        }

        let current_value = result.metric_value(metric);
        let delta_pct = ((current_value - base_value) / base_value) * 100.0;
        if delta_pct > max_regression_pct {
            regressions.push(format!(
                "{} {} [{}] {} regressed by {:.2}% (baseline {:.3} -> current {:.3}, limit {:.2}%)",
                result.action.as_str(),
                backend_name(result.backend),
                result.target_label,
                metric.as_str(),
                delta_pct,
                base_value,
                current_value,
                max_regression_pct
            ));
        }
    }

    if regressions.is_empty() {
        return Ok(());
    }

    bail!(
        "performance regression detected:\n{}",
        regressions.join("\n")
    )
}

fn check_duplicate_budget(current: &[BenchResult], max_duplicate_pct: f64) -> Result<()> {
    let mut offenders = Vec::new();
    for result in current {
        if result.duplicate_pct > max_duplicate_pct {
            offenders.push(format!(
                "{} {} [{}] duplicate_pct {:.2}% exceeded limit {:.2}%",
                result.action.as_str(),
                backend_name(result.backend),
                result.target_label,
                result.duplicate_pct,
                max_duplicate_pct
            ));
        }
    }
    if offenders.is_empty() {
        return Ok(());
    }
    bail!("duplicate frame budget exceeded:\n{}", offenders.join("\n"))
}

fn print_latency_results(results: &[BenchResult]) {
    println!(
        "{:<12} {:<28} {:<6} {:>10} {:>10} {:>10} {:>10} {:>10} {:>10} {:>9} {:>9} {:>8} {:>8}",
        "action",
        "target",
        "backend",
        "setup_ms",
        "start_ms",
        "first_ms",
        "avg_ms",
        "p50_ms",
        "p95_ms",
        "p99_ms",
        "fps",
        "dup_%",
        "buf_%"
    );
    for result in results {
        println!(
            "{:<12} {:<28} {:<6} {:>10.3} {:>10.3} {:>10.3} {:>10.3} {:>10.3} {:>10.3} {:>9.3} {:>9.2} {:>8.2} {:>8.2}",
            result.action.as_str(),
            result.target_label,
            backend_name(result.backend),
            result.setup_ms,
            result.start_ms,
            result.first_frame_ms,
            result.avg_ms,
            result.p50_ms,
            result.p95_ms,
            result.p99_ms,
            result.fps,
            result.duplicate_pct,
            result.buffer_peak_pct,
        );
    }
}

fn print_memory_results(results: &[BenchResult]) {
    println!(
        "{:<12} {:<28} {:<6} {:>10} {:>10} {:>10} {:>10} {:>10} {:>10} {:>10}",
        "action",
        "target",
        "backend",
        "base_ws",
        "avg_ws_d",
        "peak_ws_d",
        "post_ws_d",
        "base_priv",
        "avg_priv_d",
        "peak_priv_d"
    );
    for result in results {
        println!(
            "{:<12} {:<28} {:<6} {:>10.2} {:>10.2} {:>10.2} {:>10.2} {:>10.2} {:>10.2} {:>10.2}",
            result.action.as_str(),
            result.target_label,
            backend_name(result.backend),
            result.base_ws_mb,
            result.avg_ws_delta_mb,
            result.peak_ws_delta_mb,
            result.post_ws_delta_mb,
            result.base_private_mb,
            result.avg_private_delta_mb,
            result.peak_private_delta_mb,
        );
    }
}

fn main() -> Result<()> {
    let config = parse_args()?;
    println!(
        "Running action benchmark: actions={} target={} warmup={} frames={} rounds={} backends={} sample_interval_ms={} recording_fps={} buffer_depth={} adaptive_fps={} regression_metrics={} max_duplicate_pct={}",
        config
            .actions
            .iter()
            .map(|action| action.as_str())
            .collect::<Vec<_>>()
            .join(","),
        target_label(&config.target, config.target_label_override.as_deref()),
        config.warmup_frames,
        config.measure_frames,
        config.rounds,
        config
            .backends
            .iter()
            .map(|kind| backend_name(*kind))
            .collect::<Vec<_>>()
            .join(","),
        config
            .sample_interval
            .map(|duration| format!("{:.3}", duration.as_secs_f64() * 1000.0))
            .unwrap_or_else(|| "0".to_string()),
        config.recording_target_fps,
        config.recording_buffer_depth,
        config.recording_adaptive_fps,
        config
            .regression_metrics
            .iter()
            .map(|metric| metric.as_str())
            .collect::<Vec<_>>()
            .join(","),
        config
            .max_duplicate_pct
            .map(|value| format!("{value:.2}"))
            .unwrap_or_else(|| "none".to_string()),
    );

    let mut results = Vec::with_capacity(config.actions.len() * config.backends.len());
    for action in &config.actions {
        for backend in &config.backends {
            println!(
                "Benchmarking {} / {}...",
                action.as_str(),
                backend_name(*backend)
            );
            results.push(run_action_backend(*action, *backend, &config)?);
        }
    }

    println!();
    print_latency_results(&results);
    println!();
    print_memory_results(&results);

    if let Some(path) = &config.save_baseline_path {
        save_baseline(path, &results)?;
        println!("Saved baseline to {}", path.display());
    }

    if let Some(path) = &config.baseline_path {
        let baseline = load_baseline(path)?;
        for metric in &config.regression_metrics {
            check_regression(&baseline, &results, config.max_regression_pct, *metric)?;
            println!(
                "Regression check passed ({}, max allowed regression: {:.2}%)",
                metric.as_str(),
                config.max_regression_pct
            );
        }
    }

    if let Some(max_duplicate_pct) = config.max_duplicate_pct {
        check_duplicate_budget(&results, max_duplicate_pct)?;
        println!(
            "Duplicate budget check passed (max allowed duplicate frames: {:.2}%)",
            max_duplicate_pct
        );
    }

    Ok(())
}

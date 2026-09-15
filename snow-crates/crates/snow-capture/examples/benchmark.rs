use std::fs;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use rustc_hash::FxHashMap;
use snow_capture::backend::CaptureBackendKind;
use snow_capture::{CaptureRegion, CaptureSystem, CaptureTarget, MonitorLayout, WindowId};

const DEFAULT_WARMUP_FRAMES: usize = 30;
const DEFAULT_MEASURE_FRAMES: usize = 240;
const DEFAULT_ROUNDS: usize = 3;
const DEFAULT_MAX_REGRESSION_PCT: f64 = 10.0;

#[derive(Clone, Debug)]
enum BenchTarget {
    PrimaryMonitor,
    Region(CaptureRegion),
    Window(WindowId),
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
enum RegressionMetric {
    Avg,
    #[default]
    P50,
    P95,
    P99,
}

impl RegressionMetric {
    fn parse(raw: &str) -> Option<Self> {
        match raw.trim().to_ascii_lowercase().as_str() {
            "avg" | "average" => Some(Self::Avg),
            "p50" | "median" => Some(Self::P50),
            "p95" => Some(Self::P95),
            "p99" => Some(Self::P99),
            _ => None,
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::Avg => "avg",
            Self::P50 => "p50",
            Self::P95 => "p95",
            Self::P99 => "p99",
        }
    }

    fn current_value(self, result: &BenchResult) -> f64 {
        match self {
            Self::Avg => result.avg_ms,
            Self::P50 => result.p50_ms,
            Self::P95 => result.p95_ms,
            Self::P99 => result.p99_ms,
        }
    }

    fn baseline_value(self, entry: &BaselineEntry) -> f64 {
        match self {
            Self::Avg => entry.avg_ms,
            Self::P50 => entry.p50_ms,
            Self::P95 => entry.p95_ms,
            Self::P99 => entry.p99_ms,
        }
    }
}

#[derive(Clone, Debug)]
struct Config {
    warmup_frames: usize,
    measure_frames: usize,
    rounds: usize,
    sample_interval: Option<Duration>,
    backends: Vec<CaptureBackendKind>,
    target: BenchTarget,
    target_label_override: Option<String>,
    baseline_path: Option<PathBuf>,
    save_baseline_path: Option<PathBuf>,
    max_regression_pct: f64,
    regression_metrics: Vec<RegressionMetric>,
    max_duplicate_pct: Option<f64>,
}

#[derive(Clone, Debug)]
struct BenchResult {
    backend: CaptureBackendKind,
    target_label: String,
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
}

#[derive(Clone, Copy, Debug)]
struct BaselineEntry {
    avg_ms: f64,
    p50_ms: f64,
    p95_ms: f64,
    p99_ms: f64,
}

fn baseline_key(target: &str, backend: &str) -> String {
    format!("{target}|{backend}")
}

fn backend_name(kind: CaptureBackendKind) -> &'static str {
    match kind {
        CaptureBackendKind::Auto => "auto",
        CaptureBackendKind::DxgiDuplication => "dxgi",
        CaptureBackendKind::WindowsGraphicsCapture => "wgc",
        CaptureBackendKind::Gdi => "gdi",
        CaptureBackendKind::ScreenCaptureKit => "sck",
    }
}

fn parse_backend(token: &str) -> Option<CaptureBackendKind> {
    match token.trim().to_ascii_lowercase().as_str() {
        "dxgi" | "dxgi-duplication" | "duplication" => Some(CaptureBackendKind::DxgiDuplication),
        "wgc" | "windowsgraphicscapture" | "windows-graphics-capture" => {
            Some(CaptureBackendKind::WindowsGraphicsCapture)
        }
        "gdi" => Some(CaptureBackendKind::Gdi),
        "sck" => Some(CaptureBackendKind::ScreenCaptureKit),
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

fn parse_regression_metrics(csv: &str) -> Result<Vec<RegressionMetric>> {
    let mut metrics = Vec::new();
    for token in csv.split(',') {
        if token.trim().is_empty() {
            continue;
        }
        let Some(metric) = RegressionMetric::parse(token) else {
            bail!(
                "invalid regression metric token `{token}` in --regression-metrics (use avg,p50,p95,p99)"
            );
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

#[cfg(windows)]
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
    Ok(WindowId::from_windows_handle(handle.0 as isize))
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
        BenchTarget::Window(window) => format!("window:{}", window.session_id()),
    }
}

fn parse_args() -> Result<Config> {
    let mut warmup_frames = DEFAULT_WARMUP_FRAMES;
    let mut measure_frames = DEFAULT_MEASURE_FRAMES;
    let mut rounds = DEFAULT_ROUNDS;
    let mut sample_interval = None;
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

    let args: Vec<String> = std::env::args().collect();
    let mut i = 1usize;
    while i < args.len() {
        match args[i].as_str() {
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
                if raw_value > 0.0 {
                    sample_interval = Some(
                        Duration::try_from_secs_f64(raw_value / 1000.0).with_context(|| {
                            format!("--sample-interval-ms is out of range: {raw_value}")
                        })?,
                    );
                } else {
                    sample_interval = None;
                }
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
                target =
                    BenchTarget::Window(WindowId::from_windows_handle(parse_window_handle(raw)?));
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
            "--baseline" => {
                let Some(raw) = args.get(i + 1) else {
                    bail!("--baseline requires a file path");
                };
                baseline_path = Some(PathBuf::from(raw));
                i += 2;
            }
            "--save-baseline" => {
                let Some(raw) = args.get(i + 1) else {
                    bail!("--save-baseline requires a file path");
                };
                save_baseline_path = Some(PathBuf::from(raw));
                i += 2;
            }
            "--max-regression-pct" => {
                max_regression_pct =
                    parse_f64_arg("--max-regression-pct", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--regression-metric" => {
                let Some(raw) = args.get(i + 1).map(String::as_str) else {
                    bail!("--regression-metric requires one of: avg, p50, p95, p99");
                };
                let Some(metric) = RegressionMetric::parse(raw) else {
                    bail!("invalid --regression-metric: {raw}. Use avg, p50, p95, or p99");
                };
                regression_metrics = vec![metric];
                i += 2;
            }
            "--regression-metrics" => {
                let Some(raw) = args.get(i + 1).map(String::as_str) else {
                    bail!("--regression-metrics requires a comma-separated list (e.g. avg,p50)");
                };
                regression_metrics = parse_regression_metrics(raw)?;
                i += 2;
            }
            "--max-duplicate-pct" => {
                let value =
                    parse_f64_arg("--max-duplicate-pct", args.get(i + 1).map(String::as_str))?;
                if !(0.0..=100.0).contains(&value) {
                    bail!("--max-duplicate-pct must be between 0 and 100");
                }
                max_duplicate_pct = Some(value);
                i += 2;
            }
            "--help" | "-h" => {
                println!(
                    "Usage: cargo run --release --example benchmark -- [options]
  --warmup <n>               Warmup frames per backend (default: {DEFAULT_WARMUP_FRAMES})
  --frames <n>               Measured frames per backend (default: {DEFAULT_MEASURE_FRAMES})
  --rounds <n>               Benchmark rounds per backend (default: {DEFAULT_ROUNDS})
  --sample-interval-ms <f>   Sleep this long after each independent capture sample (default: 0)
  --backends <csv>           Backends list, e.g. dxgi,wgc,gdi
  --window-under-cursor      Benchmark window capture for the window under the cursor
  --window-handle <value>    Benchmark window capture for an HWND (decimal or 0xHEX)
  --region <x,y,w,h>         Benchmark region capture in virtual desktop coordinates
  --region-center <WxH>      Benchmark a centered region on the primary monitor
  --target-label <name>      Override target label used in output/baselines
  --baseline <path>          Compare current run to baseline CSV
  --save-baseline <path>     Save current run as baseline CSV
  --max-regression-pct <f>   Allowed metric increase vs baseline (default: {DEFAULT_MAX_REGRESSION_PCT})
  --regression-metric <m>    Metric for regression checks: avg | p50 | p95 | p99 (default: p50)
  --regression-metrics <csv> Metrics for regression checks, e.g. avg,p50,p95
  --max-duplicate-pct <f>    Fail if duplicate-frame percentage exceeds this threshold

Notes:
  screenshot benchmarks recreate and drop a fresh session for every sample

Tip: compare the GDI duplicate-frame fast path with baseline behavior:
  optimized: cargo run --release --example benchmark -- --backends gdi --region-center 1920x1080
  baseline (PowerShell): $env:SNOW_CAPTURE_DISABLE_GDI_DUPLICATE_PROBE=1; cargo run --release --example benchmark -- --backends gdi --region-center 1920x1080; Remove-Item Env:SNOW_CAPTURE_DISABLE_GDI_DUPLICATE_PROBE
  baseline (cmd.exe):    setlocal && set SNOW_CAPTURE_DISABLE_GDI_DUPLICATE_PROBE=1 && cargo run --release --example benchmark -- --backends gdi --region-center 1920x1080 && endlocal
  baseline (bash):       SNOW_CAPTURE_DISABLE_GDI_DUPLICATE_PROBE=1 cargo run --release --example benchmark -- --backends gdi --region-center 1920x1080

Tip: compare DXGI trusted dirty-rect direct conversion in window mode:
  optimized: cargo run --release --example benchmark -- --backends dxgi --window-under-cursor
  baseline (PowerShell): $env:SNOW_CAPTURE_DISABLE_DIRTY_RECT_TRUSTED_DIRECT=1; cargo run --release --example benchmark -- --backends dxgi --window-under-cursor; Remove-Item Env:SNOW_CAPTURE_DISABLE_DIRTY_RECT_TRUSTED_DIRECT
  baseline (cmd.exe):    setlocal && set SNOW_CAPTURE_DISABLE_DIRTY_RECT_TRUSTED_DIRECT=1 && cargo run --release --example benchmark -- --backends dxgi --window-under-cursor && endlocal
  baseline (bash):       SNOW_CAPTURE_DISABLE_DIRTY_RECT_TRUSTED_DIRECT=1 cargo run --release --example benchmark -- --backends dxgi --window-under-cursor

Tip: compare DXGI BGRA dirty-rect batch row-kernel path in window mode:
  optimized: cargo run --release --example benchmark -- --backends dxgi --window-under-cursor
  baseline (PowerShell): $env:SNOW_CAPTURE_DISABLE_DIRTY_RECT_BGRA_BATCH_KERNEL=1; cargo run --release --example benchmark -- --backends dxgi --window-under-cursor; Remove-Item Env:SNOW_CAPTURE_DISABLE_DIRTY_RECT_BGRA_BATCH_KERNEL
  baseline (cmd.exe):    setlocal && set SNOW_CAPTURE_DISABLE_DIRTY_RECT_BGRA_BATCH_KERNEL=1 && cargo run --release --example benchmark -- --backends dxgi --window-under-cursor && endlocal
  baseline (bash):       SNOW_CAPTURE_DISABLE_DIRTY_RECT_BGRA_BATCH_KERNEL=1 cargo run --release --example benchmark -- --backends dxgi --window-under-cursor

Tip: compare DXGI monitor low-latency dirty GPU copy in full-frame mode:
  optimized: cargo run --release --example benchmark -- --backends dxgi --region-center 1920x1080
  baseline (PowerShell): $env:SNOW_CAPTURE_DXGI_DISABLE_MONITOR_DIRTY_GPU_COPY=1; cargo run --release --example benchmark -- --backends dxgi --region-center 1920x1080; Remove-Item Env:SNOW_CAPTURE_DXGI_DISABLE_MONITOR_DIRTY_GPU_COPY
  baseline (cmd.exe):    setlocal && set SNOW_CAPTURE_DXGI_DISABLE_MONITOR_DIRTY_GPU_COPY=1 && cargo run --release --example benchmark -- --backends dxgi --region-center 1920x1080 && endlocal
  baseline (bash):       SNOW_CAPTURE_DXGI_DISABLE_MONITOR_DIRTY_GPU_COPY=1 cargo run --release --example benchmark -- --backends dxgi --region-center 1920x1080

Tip: compare DXGI region move-rect reconstruction in window mode:
  optimized: cargo run --release --example benchmark -- --backends dxgi --window-under-cursor
  baseline (PowerShell): $env:SNOW_CAPTURE_DXGI_DISABLE_REGION_MOVE_RECONSTRUCT=1; cargo run --release --example benchmark -- --backends dxgi --window-under-cursor; Remove-Item Env:SNOW_CAPTURE_DXGI_DISABLE_REGION_MOVE_RECONSTRUCT
  baseline (cmd.exe):    setlocal && set SNOW_CAPTURE_DXGI_DISABLE_REGION_MOVE_RECONSTRUCT=1 && cargo run --release --example benchmark -- --backends dxgi --window-under-cursor && endlocal
  baseline (bash):       SNOW_CAPTURE_DXGI_DISABLE_REGION_MOVE_RECONSTRUCT=1 cargo run --release --example benchmark -- --backends dxgi --window-under-cursor

Tip: compare WGC dirty-rect conversion hints in window mode:
  optimized: cargo run --release --example benchmark -- --backends wgc --window-under-cursor
  baseline (PowerShell): $env:SNOW_CAPTURE_WGC_DISABLE_DIRTY_HINTS=1; cargo run --release --example benchmark -- --backends wgc --window-under-cursor; Remove-Item Env:SNOW_CAPTURE_WGC_DISABLE_DIRTY_HINTS
  baseline (cmd.exe):    setlocal && set SNOW_CAPTURE_WGC_DISABLE_DIRTY_HINTS=1 && cargo run --release --example benchmark -- --backends wgc --window-under-cursor && endlocal
  baseline (bash):       SNOW_CAPTURE_WGC_DISABLE_DIRTY_HINTS=1 cargo run --release --example benchmark -- --backends wgc --window-under-cursor

Tip: compare WGC dirty-region batch fetching in region mode:
  optimized: cargo run --release --example benchmark -- --backends wgc --region-center 1600x900 --sample-interval-ms 8
  baseline (PowerShell): $env:SNOW_CAPTURE_WGC_DISABLE_DIRTY_REGION_BATCH_FETCH=1; cargo run --release --example benchmark -- --backends wgc --region-center 1600x900 --sample-interval-ms 8; Remove-Item Env:SNOW_CAPTURE_WGC_DISABLE_DIRTY_REGION_BATCH_FETCH
  baseline (cmd.exe):    setlocal && set SNOW_CAPTURE_WGC_DISABLE_DIRTY_REGION_BATCH_FETCH=1 && cargo run --release --example benchmark -- --backends wgc --region-center 1600x900 --sample-interval-ms 8 && endlocal
  baseline (bash):       SNOW_CAPTURE_WGC_DISABLE_DIRTY_REGION_BATCH_FETCH=1 cargo run --release --example benchmark -- --backends wgc --region-center 1600x900 --sample-interval-ms 8

Tip: compare WGC dense dirty-region fallback in region mode:
  optimized: cargo run --release --example benchmark -- --backends wgc --region-center 1920x1080 --sample-interval-ms 8
  baseline (PowerShell): $env:SNOW_CAPTURE_WGC_DISABLE_REGION_DIRTY_DENSE_FALLBACK=1; cargo run --release --example benchmark -- --backends wgc --region-center 1920x1080 --sample-interval-ms 8; Remove-Item Env:SNOW_CAPTURE_WGC_DISABLE_REGION_DIRTY_DENSE_FALLBACK
  baseline (cmd.exe):    setlocal && set SNOW_CAPTURE_WGC_DISABLE_REGION_DIRTY_DENSE_FALLBACK=1 && cargo run --release --example benchmark -- --backends wgc --region-center 1920x1080 --sample-interval-ms 8 && endlocal
  baseline (bash):       SNOW_CAPTURE_WGC_DISABLE_REGION_DIRTY_DENSE_FALLBACK=1 cargo run --release --example benchmark -- --backends wgc --region-center 1920x1080 --sample-interval-ms 8

Tip: compare WGC full-frame dense dirty fallback in window mode:
  optimized: cargo run --release --example benchmark -- --backends wgc --window-under-cursor
  baseline (PowerShell): $env:SNOW_CAPTURE_WGC_DISABLE_FULL_DIRTY_DENSE_FALLBACK=1; cargo run --release --example benchmark -- --backends wgc --window-under-cursor; Remove-Item Env:SNOW_CAPTURE_WGC_DISABLE_FULL_DIRTY_DENSE_FALLBACK
  baseline (cmd.exe):    setlocal && set SNOW_CAPTURE_WGC_DISABLE_FULL_DIRTY_DENSE_FALLBACK=1 && cargo run --release --example benchmark -- --backends wgc --window-under-cursor && endlocal
  baseline (bash):       SNOW_CAPTURE_WGC_DISABLE_FULL_DIRTY_DENSE_FALLBACK=1 cargo run --release --example benchmark -- --backends wgc --window-under-cursor

Tip: compare the GDI span single-scan incremental path (sequential + parallel):
  optimized: cargo run --release --example benchmark -- --backends gdi --region-center 1600x900
  baseline (PowerShell): $env:SNOW_CAPTURE_DISABLE_GDI_SPAN_SINGLE_SCAN=1; cargo run --release --example benchmark -- --backends gdi --region-center 1600x900; Remove-Item Env:SNOW_CAPTURE_DISABLE_GDI_SPAN_SINGLE_SCAN
  baseline (cmd.exe):    setlocal && set SNOW_CAPTURE_DISABLE_GDI_SPAN_SINGLE_SCAN=1 && cargo run --release --example benchmark -- --backends gdi --region-center 1600x900 && endlocal
  baseline (bash):       SNOW_CAPTURE_DISABLE_GDI_SPAN_SINGLE_SCAN=1 cargo run --release --example benchmark -- --backends gdi --region-center 1600x900

Tip: compare the GDI parallel sparse-span hybrid path:
  optimized: cargo run --release --example benchmark -- --backends gdi --region-center 2560x1440
  baseline (PowerShell): $env:SNOW_CAPTURE_DISABLE_GDI_PARALLEL_SPAN_SCAN=1; cargo run --release --example benchmark -- --backends gdi --region-center 2560x1440; Remove-Item Env:SNOW_CAPTURE_DISABLE_GDI_PARALLEL_SPAN_SCAN
  baseline (cmd.exe):    setlocal && set SNOW_CAPTURE_DISABLE_GDI_PARALLEL_SPAN_SCAN=1 && cargo run --release --example benchmark -- --backends gdi --region-center 2560x1440 && endlocal
  baseline (bash):       SNOW_CAPTURE_DISABLE_GDI_PARALLEL_SPAN_SCAN=1 cargo run --release --example benchmark -- --backends gdi --region-center 2560x1440

Tip: compare the GDI adaptive parallel span mode-history heuristic:
  optimized: cargo run --release --example benchmark -- --backends gdi --region-center 2560x1440
  baseline (PowerShell): $env:SNOW_CAPTURE_DISABLE_GDI_PARALLEL_SPAN_MODE_HISTORY=1; cargo run --release --example benchmark -- --backends gdi --region-center 2560x1440; Remove-Item Env:SNOW_CAPTURE_DISABLE_GDI_PARALLEL_SPAN_MODE_HISTORY
  baseline (cmd.exe):    setlocal && set SNOW_CAPTURE_DISABLE_GDI_PARALLEL_SPAN_MODE_HISTORY=1 && cargo run --release --example benchmark -- --backends gdi --region-center 2560x1440 && endlocal
  baseline (bash):       SNOW_CAPTURE_DISABLE_GDI_PARALLEL_SPAN_MODE_HISTORY=1 cargo run --release --example benchmark -- --backends gdi --region-center 2560x1440"
                );
                std::process::exit(0);
            }
            other => {
                bail!("unknown argument: {other}");
            }
        }
    }

    if warmup_frames == 0 {
        bail!("--warmup must be >= 1");
    }
    if measure_frames == 0 {
        bail!("--frames must be >= 1");
    }
    if rounds == 0 {
        bail!("--rounds must be >= 1");
    }
    if max_regression_pct < 0.0 {
        bail!("--max-regression-pct must be >= 0");
    }

    Ok(Config {
        warmup_frames,
        measure_frames,
        rounds,
        sample_interval,
        backends,
        target,
        target_label_override,
        baseline_path,
        save_baseline_path,
        max_regression_pct,
        regression_metrics,
        max_duplicate_pct,
    })
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

fn capture_target_for(target: &BenchTarget) -> CaptureTarget {
    match target {
        BenchTarget::PrimaryMonitor => CaptureTarget::PrimaryMonitor,
        BenchTarget::Region(region) => CaptureTarget::Region(*region),
        BenchTarget::Window(window) => CaptureTarget::Window(*window),
    }
}

fn run_backend(
    kind: CaptureBackendKind,
    warmup_frames: usize,
    measure_frames: usize,
    rounds: usize,
    sample_interval: Option<Duration>,
    bench_target: &BenchTarget,
    target_label_override: Option<&str>,
) -> Result<BenchResult> {
    let target = capture_target_for(bench_target);
    let target_label = target_label(bench_target, target_label_override);

    let total_samples = measure_frames
        .checked_mul(rounds)
        .context("benchmark sample count overflow")?;
    let mut samples_ms = Vec::with_capacity(total_samples);
    let mut duplicate_samples = 0usize;

    for _round in 0..rounds {
        let total_iterations = warmup_frames + measure_frames;
        for iteration_idx in 0..total_iterations {
            let t0 = Instant::now();
            let system = CaptureSystem::builder()
                .with_backend_kind(kind)
                .build()
                .with_context(|| {
                    format!("failed to initialize {} backend system", backend_name(kind))
                })?;
            let mut session = system
                .open_session(target.clone(), Default::default())
                .with_context(|| {
                    format!("failed to open {} backend session", backend_name(kind))
                })?;
            let frame = session
                .capture()
                .with_context(|| format!("capture failed for {}", backend_name(kind)))?;
            let elapsed_ms = t0.elapsed().as_secs_f64() * 1000.0;
            if iteration_idx >= warmup_frames {
                samples_ms.push(elapsed_ms);
                if frame.metadata().is_duplicate() {
                    duplicate_samples = duplicate_samples.saturating_add(1);
                }
            }
            drop(frame);
            drop(session);
            if let Some(interval) = sample_interval {
                std::thread::sleep(interval);
            }
        }
    }

    let mut sorted = samples_ms.clone();
    sorted.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let sum_ms: f64 = samples_ms.iter().sum();
    let avg_ms = sum_ms / samples_ms.len() as f64;
    let variance = samples_ms
        .iter()
        .map(|sample| {
            let d = *sample - avg_ms;
            d * d
        })
        .sum::<f64>()
        / samples_ms.len() as f64;
    let stddev_ms = variance.sqrt();
    let duplicate_pct = if samples_ms.is_empty() {
        0.0
    } else {
        (duplicate_samples as f64 * 100.0) / samples_ms.len() as f64
    };
    let fresh_samples = samples_ms.len().saturating_sub(duplicate_samples);
    let fresh_fps = if sum_ms > 0.0 {
        (fresh_samples as f64 * 1000.0) / sum_ms
    } else {
        0.0
    };

    Ok(BenchResult {
        backend: kind,
        target_label,
        avg_ms,
        p50_ms: percentile(&sorted, 0.50),
        p95_ms: percentile(&sorted, 0.95),
        p99_ms: percentile(&sorted, 0.99),
        min_ms: *sorted.first().unwrap(),
        max_ms: *sorted.last().unwrap(),
        stddev_ms,
        fps: if avg_ms > 0.0 { 1000.0 / avg_ms } else { 0.0 },
        duplicate_pct,
        fresh_fps,
    })
}

fn save_baseline(path: &PathBuf, results: &[BenchResult]) -> Result<()> {
    let mut out = String::from(
        "target,backend,avg_ms,p50_ms,p95_ms,p99_ms,min_ms,max_ms,stddev_ms,fps,duplicate_pct,fresh_fps\n",
    );
    for result in results {
        out.push_str(&format!(
            "{},{},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.3},{:.6}\n",
            result.target_label,
            backend_name(result.backend),
            result.avg_ms,
            result.p50_ms,
            result.p95_ms,
            result.p99_ms,
            result.min_ms,
            result.max_ms,
            result.stddev_ms,
            result.fps,
            result.duplicate_pct,
            result.fresh_fps
        ));
    }
    fs::write(path, out)
        .with_context(|| format!("failed to write baseline file {}", path.display()))
}

fn load_baseline(path: &PathBuf) -> Result<FxHashMap<String, BaselineEntry>> {
    let text = fs::read_to_string(path)
        .with_context(|| format!("failed to read baseline file {}", path.display()))?;
    let mut lines = text.lines();
    let header_line = lines
        .next()
        .context("baseline file is empty (missing header row)")?;
    let header: Vec<&str> = header_line.split(',').map(|column| column.trim()).collect();

    let column_index = |name: &str| {
        header
            .iter()
            .position(|column| column.eq_ignore_ascii_case(name))
    };
    let backend_idx =
        column_index("backend").context("baseline header is missing required `backend` column")?;
    let target_idx = column_index("target");
    let avg_idx =
        column_index("avg_ms").context("baseline header is missing required `avg_ms` column")?;
    let p50_idx =
        column_index("p50_ms").context("baseline header is missing required `p50_ms` column")?;
    let p95_idx =
        column_index("p95_ms").context("baseline header is missing required `p95_ms` column")?;
    let p99_idx = column_index("p99_ms");

    let mut out = FxHashMap::default();
    for (line_offset, line) in lines.enumerate() {
        let line_number = line_offset + 2;
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        let parts: Vec<&str> = trimmed.split(',').collect();
        if parts.len() <= p95_idx || parts.len() <= backend_idx {
            bail!("invalid baseline line {line_number}: {line}");
        }

        let parse_metric = |column_name: &str, index: usize| -> Result<f64> {
            parts
                .get(index)
                .context(format!(
                    "baseline line {line_number} is missing `{column_name}` value"
                ))?
                .trim()
                .parse::<f64>()
                .with_context(|| {
                    format!(
                        "invalid {column_name} in baseline line {line_number}: {}",
                        line
                    )
                })
        };

        let avg_ms = parse_metric("avg_ms", avg_idx)?;
        let p50_ms = parse_metric("p50_ms", p50_idx)?;
        let p95_ms = parse_metric("p95_ms", p95_idx)?;
        let p99_ms = if let Some(index) = p99_idx {
            if index < parts.len() && !parts[index].trim().is_empty() {
                parse_metric("p99_ms", index)?
            } else {
                p95_ms
            }
        } else {
            p95_ms
        };
        let backend = parts[backend_idx].trim();
        if backend.is_empty() {
            bail!("baseline line {line_number} has empty backend value");
        }
        let target = target_idx
            .and_then(|index| parts.get(index))
            .map(|raw| raw.trim())
            .filter(|value| !value.is_empty())
            .unwrap_or("primary-monitor");

        out.insert(
            baseline_key(target, backend),
            BaselineEntry {
                avg_ms,
                p50_ms,
                p95_ms,
                p99_ms,
            },
        );
    }
    Ok(out)
}

fn check_regression(
    baseline: &FxHashMap<String, BaselineEntry>,
    current: &[BenchResult],
    max_regression_pct: f64,
    metric: RegressionMetric,
) -> Result<()> {
    let mut regressions = Vec::new();
    for result in current {
        let key = baseline_key(&result.target_label, backend_name(result.backend));
        let Some(base) = baseline.get(&key) else {
            continue;
        };
        let base_value = metric.baseline_value(base);
        if base_value <= 0.0 {
            continue;
        }
        let current_value = metric.current_value(result);
        let delta_pct = ((current_value - base_value) / base_value) * 100.0;
        if delta_pct > max_regression_pct {
            regressions.push(format!(
                "{} [{}] {} regressed by {:.2}% (baseline {:.3} ms -> current {:.3} ms, limit {:.2}%)",
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
                "{} [{}] duplicate_pct {:.2}% exceeded limit {:.2}%",
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
    bail!("duplicate frame budget exceeded:\n{}", offenders.join("\n"));
}

fn print_results(results: &[BenchResult]) {
    println!(
        "{:<32} {:<6} {:>12} {:>12} {:>12} {:>12} {:>12} {:>12} {:>12} {:>10} {:>10} {:>10}",
        "target",
        "backend",
        "avg_ms",
        "p50_ms",
        "p95_ms",
        "p99_ms",
        "min_ms",
        "max_ms",
        "stddev",
        "fps",
        "dup_%",
        "fresh_fps"
    );
    for r in results {
        println!(
            "{:<32} {:<6} {:>12.6} {:>12.6} {:>12.6} {:>12.6} {:>12.6} {:>12.6} {:>12.6} {:>10.2} {:>10.2} {:>10.2}",
            r.target_label,
            backend_name(r.backend),
            r.avg_ms,
            r.p50_ms,
            r.p95_ms,
            r.p99_ms,
            r.min_ms,
            r.max_ms,
            r.stddev_ms,
            r.fps,
            r.duplicate_pct,
            r.fresh_fps
        );
    }
}

fn main() -> Result<()> {
    let config = parse_args()?;
    println!(
        "Running benchmark: target={} warmup={} frames={} rounds={} sample_interval_ms={} backends={} regression_metrics={} max_duplicate_pct={}",
        target_label(&config.target, config.target_label_override.as_deref()),
        config.warmup_frames,
        config.measure_frames,
        config.rounds,
        config
            .sample_interval
            .map(|d| format!("{:.3}", d.as_secs_f64() * 1000.0))
            .unwrap_or_else(|| "0".to_string()),
        config
            .backends
            .iter()
            .map(|k| backend_name(*k))
            .collect::<Vec<_>>()
            .join(","),
        config
            .regression_metrics
            .iter()
            .map(|metric| metric.as_str())
            .collect::<Vec<_>>()
            .join(","),
        config
            .max_duplicate_pct
            .map(|v| format!("{v:.2}"))
            .unwrap_or_else(|| "none".to_string()),
    );

    let mut results = Vec::with_capacity(config.backends.len());
    for backend in &config.backends {
        println!("Benchmarking {}...", backend_name(*backend));
        let result = run_backend(
            *backend,
            config.warmup_frames,
            config.measure_frames,
            config.rounds,
            config.sample_interval,
            &config.target,
            config.target_label_override.as_deref(),
        )?;
        results.push(result);
    }
    print_results(&results);

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

#[cfg(not(windows))]
fn window_under_cursor() -> Result<WindowId> {
    anyhow::bail!(
        "automatic window picking is Windows-only; use the macOS harness for window enumeration"
    )
}

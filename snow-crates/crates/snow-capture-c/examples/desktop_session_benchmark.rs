use std::fs;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use snow_capture_c::{
    SCREENSHOT_REQUEST_VERSION, SnowCaptureScreenshotRequest, snow_capture_desktop_session_capture,
    snow_capture_desktop_session_create, snow_capture_desktop_session_destroy,
    snow_capture_desktop_session_prepare, snow_capture_desktop_session_refresh_layout,
    snow_capture_desktop_session_reset_to_prepared, snow_capture_screenshot_result_destroy,
    snow_capture_screenshot_result_display_count,
};
use windows::Win32::System::ProcessStatus::{
    K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS, PROCESS_MEMORY_COUNTERS_EX,
};
use windows::Win32::System::Threading::GetCurrentProcess;

const DEFAULT_WARM_SAMPLES: usize = 8;
const DEFAULT_TRIM_SAMPLES: usize = 8;
const DEFAULT_IDLE_MS: u64 = 15_000;
const MIB: f64 = 1024.0 * 1024.0;

#[derive(Clone, Debug)]
struct Config {
    warm_samples: usize,
    trim_samples: usize,
    idle_ms: u64,
    output: PathBuf,
    allow_debug: bool,
}

#[derive(Clone, Copy, Debug, Default)]
struct ProcessMemorySample {
    working_set_bytes: u64,
    private_bytes: u64,
}

impl ProcessMemorySample {
    fn capture() -> Result<Self> {
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
}

#[derive(Clone, Debug)]
struct BenchRow {
    phase: &'static str,
    index: usize,
    latency_ms: f64,
    frame_count: usize,
    base_ws_mb: f64,
    base_private_mb: f64,
    peak_ws_delta_mb: f64,
    peak_private_delta_mb: f64,
    post_ws_delta_mb: f64,
    post_private_delta_mb: f64,
}

struct DesktopSession {
    raw: *mut snow_capture_c::SnowCaptureDesktopSessionImpl,
}

impl DesktopSession {
    fn create() -> Result<Self> {
        let raw = snow_capture_desktop_session_create(std::ptr::null());
        if raw.is_null() {
            bail!("snow_capture_desktop_session_create failed");
        }
        Ok(Self { raw })
    }

    fn prepare(&mut self) -> Result<()> {
        if snow_capture_desktop_session_prepare(self.raw) == 0 {
            bail!("snow_capture_desktop_session_prepare failed");
        }
        Ok(())
    }

    fn refresh_layout(&mut self) -> Result<()> {
        if snow_capture_desktop_session_refresh_layout(self.raw) == 0 {
            bail!("snow_capture_desktop_session_refresh_layout failed");
        }
        Ok(())
    }

    fn reset_to_prepared(&mut self) -> Result<()> {
        if snow_capture_desktop_session_reset_to_prepared(self.raw) == 0 {
            bail!("snow_capture_desktop_session_reset_to_prepared failed");
        }
        Ok(())
    }

    fn capture(&mut self) -> Result<usize> {
        let request = SnowCaptureScreenshotRequest {
            version: SCREENSHOT_REQUEST_VERSION,
            struct_size: std::mem::size_of::<SnowCaptureScreenshotRequest>() as u32,
            flags: 0,
            reserved0: 0,
            focused_window: 0,
            cancellation_token: std::ptr::null(),
            reserved: [0; 32],
        };
        let result = unsafe { snow_capture_desktop_session_capture(self.raw, &request) };
        if result.is_null() {
            bail!("snow_capture_desktop_session_capture failed");
        }

        let frame_count = unsafe { snow_capture_screenshot_result_display_count(result) };
        unsafe {
            snow_capture_screenshot_result_destroy(result);
        }
        Ok(frame_count)
    }
}

impl Drop for DesktopSession {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe {
                snow_capture_desktop_session_destroy(self.raw);
            }
            self.raw = std::ptr::null_mut();
        }
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

fn elapsed_ms(start: Instant) -> f64 {
    start.elapsed().as_secs_f64() * 1000.0
}

fn parse_usize_arg(flag: &str, value: Option<&str>) -> Result<usize> {
    let Some(raw) = value else {
        bail!("{flag} requires a value");
    };
    raw.parse::<usize>()
        .with_context(|| format!("failed to parse {flag} value: {raw}"))
}

fn parse_u64_arg(flag: &str, value: Option<&str>) -> Result<u64> {
    let Some(raw) = value else {
        bail!("{flag} requires a value");
    };
    raw.parse::<u64>()
        .with_context(|| format!("failed to parse {flag} value: {raw}"))
}

fn print_usage() {
    println!(
        "Usage: cargo run --release -p snow-capture-c --example desktop_session_benchmark -- [options]
  --warm-samples <n>   warm capture samples before trim (default: {DEFAULT_WARM_SAMPLES})
  --trim-samples <n>   capture samples after explicit trim (default: {DEFAULT_TRIM_SAMPLES})
  --idle-ms <n>        sleep before final post-idle memory sample (default: {DEFAULT_IDLE_MS})
  --output <path>      csv output path (default: target/perf/desktop-session-benchmark.csv)
  --allow-debug        allow running outside Release for smoke checks"
    );
}

fn parse_args() -> Result<Config> {
    let mut config = Config {
        warm_samples: DEFAULT_WARM_SAMPLES,
        trim_samples: DEFAULT_TRIM_SAMPLES,
        idle_ms: DEFAULT_IDLE_MS,
        output: PathBuf::from("target/perf/desktop-session-benchmark.csv"),
        allow_debug: false,
    };

    let args: Vec<String> = std::env::args().collect();
    let mut i = 1usize;
    while i < args.len() {
        match args[i].as_str() {
            "--help" | "-h" => {
                print_usage();
                std::process::exit(0);
            }
            "--warm-samples" => {
                config.warm_samples =
                    parse_usize_arg("--warm-samples", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--trim-samples" => {
                config.trim_samples =
                    parse_usize_arg("--trim-samples", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--idle-ms" => {
                config.idle_ms = parse_u64_arg("--idle-ms", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--output" => {
                let Some(raw) = args.get(i + 1) else {
                    bail!("--output requires a path");
                };
                config.output = PathBuf::from(raw);
                i += 2;
            }
            "--allow-debug" => {
                config.allow_debug = true;
                i += 1;
            }
            other => bail!("unknown argument: {other}. Use --help for usage."),
        }
    }

    if config.warm_samples == 0 {
        bail!("--warm-samples must be > 0");
    }
    if config.trim_samples == 0 {
        bail!("--trim-samples must be > 0");
    }
    if cfg!(debug_assertions) && !config.allow_debug {
        bail!(
            "desktop session benchmarks must be run in Release; pass --allow-debug only for smoke checks"
        );
    }
    Ok(config)
}

fn measure_capture(
    session: &mut DesktopSession,
    phase: &'static str,
    index: usize,
) -> Result<BenchRow> {
    let base = ProcessMemorySample::capture()?;
    let started = Instant::now();
    let frame_count = session.capture()?;
    let latency_ms = elapsed_ms(started);
    let peak = ProcessMemorySample::capture()?;
    let (peak_ws_delta_mb, peak_private_delta_mb) = memory_deltas_mb(base, peak);
    let post = ProcessMemorySample::capture()?;
    let (post_ws_delta_mb, post_private_delta_mb) = memory_deltas_mb(base, post);

    Ok(BenchRow {
        phase,
        index,
        latency_ms,
        frame_count,
        base_ws_mb: bytes_to_mib(base.working_set_bytes),
        base_private_mb: bytes_to_mib(base.private_bytes),
        peak_ws_delta_mb,
        peak_private_delta_mb,
        post_ws_delta_mb,
        post_private_delta_mb,
    })
}

fn csv_header() -> &'static str {
    "phase,index,latency_ms,frame_count,base_ws_mb,base_private_mb,peak_ws_delta_mb,peak_private_delta_mb,post_ws_delta_mb,post_private_delta_mb\n"
}

fn csv_row(row: &BenchRow) -> String {
    format!(
        "{},{},{:.3},{},{:.3},{:.3},{:.3},{:.3},{:.3},{:.3}\n",
        row.phase,
        row.index,
        row.latency_ms,
        row.frame_count,
        row.base_ws_mb,
        row.base_private_mb,
        row.peak_ws_delta_mb,
        row.peak_private_delta_mb,
        row.post_ws_delta_mb,
        row.post_private_delta_mb
    )
}

fn summarize_phase(rows: &[BenchRow], phase: &str) {
    let mut latencies = rows
        .iter()
        .filter(|row| row.phase == phase)
        .map(|row| row.latency_ms)
        .collect::<Vec<_>>();
    if latencies.is_empty() {
        return;
    }
    latencies.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let avg = latencies.iter().sum::<f64>() / latencies.len() as f64;
    let p50 = latencies[(latencies.len() - 1) / 2];
    let p95 = latencies[((latencies.len() - 1) as f64 * 0.95).round() as usize];
    println!(
        "{phase:>12}: samples={} avg={avg:.3}ms p50={p50:.3}ms p95={p95:.3}ms",
        latencies.len()
    );
}

fn write_csv(path: &PathBuf, rows: &[BenchRow]) -> Result<()> {
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create output directory {}", parent.display()))?;
    }

    let mut out = String::from(csv_header());
    for row in rows {
        out.push_str(&csv_row(row));
    }
    fs::write(path, out).with_context(|| format!("failed to write {}", path.display()))
}

fn main() -> Result<()> {
    let config = parse_args()?;
    let mut rows = Vec::with_capacity(config.warm_samples + config.trim_samples + 4);
    let base = ProcessMemorySample::capture()?;
    let mut session = DesktopSession::create()?;

    let started = Instant::now();
    session.prepare()?;
    let prepare_ms = elapsed_ms(started);
    let prepared = ProcessMemorySample::capture()?;
    let (prepare_ws_delta_mb, prepare_private_delta_mb) = memory_deltas_mb(base, prepared);
    rows.push(BenchRow {
        phase: "prepare",
        index: 0,
        latency_ms: prepare_ms,
        frame_count: 0,
        base_ws_mb: bytes_to_mib(base.working_set_bytes),
        base_private_mb: bytes_to_mib(base.private_bytes),
        peak_ws_delta_mb: prepare_ws_delta_mb,
        peak_private_delta_mb: prepare_private_delta_mb,
        post_ws_delta_mb: prepare_ws_delta_mb,
        post_private_delta_mb: prepare_private_delta_mb,
    });

    rows.push(measure_capture(&mut session, "cold", 0)?);
    for index in 0..config.warm_samples {
        rows.push(measure_capture(&mut session, "warm", index)?);
    }

    let before_trim = ProcessMemorySample::capture()?;
    let started = Instant::now();
    session.reset_to_prepared()?;
    let trim_ms = elapsed_ms(started);
    let after_trim = ProcessMemorySample::capture()?;
    let (trim_ws_delta_mb, trim_private_delta_mb) = memory_deltas_mb(before_trim, after_trim);
    rows.push(BenchRow {
        phase: "trim",
        index: 0,
        latency_ms: trim_ms,
        frame_count: 0,
        base_ws_mb: bytes_to_mib(before_trim.working_set_bytes),
        base_private_mb: bytes_to_mib(before_trim.private_bytes),
        peak_ws_delta_mb: trim_ws_delta_mb,
        peak_private_delta_mb: trim_private_delta_mb,
        post_ws_delta_mb: trim_ws_delta_mb,
        post_private_delta_mb: trim_private_delta_mb,
    });

    rows.push(measure_capture(&mut session, "after_trim", 0)?);
    for index in 0..config.trim_samples {
        rows.push(measure_capture(&mut session, "post_trim_warm", index)?);
    }

    session.refresh_layout()?;
    rows.push(measure_capture(&mut session, "after_refresh", 0)?);
    session.reset_to_prepared()?;

    if config.idle_ms > 0 {
        std::thread::sleep(Duration::from_millis(config.idle_ms));
    }
    let post_idle = ProcessMemorySample::capture()?;
    let (post_idle_ws_delta_mb, post_idle_private_delta_mb) = memory_deltas_mb(base, post_idle);
    rows.push(BenchRow {
        phase: "post_idle",
        index: 0,
        latency_ms: config.idle_ms as f64,
        frame_count: 0,
        base_ws_mb: bytes_to_mib(base.working_set_bytes),
        base_private_mb: bytes_to_mib(base.private_bytes),
        peak_ws_delta_mb: post_idle_ws_delta_mb,
        peak_private_delta_mb: post_idle_private_delta_mb,
        post_ws_delta_mb: post_idle_ws_delta_mb,
        post_private_delta_mb: post_idle_private_delta_mb,
    });

    write_csv(&config.output, &rows)?;
    println!("Wrote {}", config.output.display());
    summarize_phase(&rows, "cold");
    summarize_phase(&rows, "warm");
    summarize_phase(&rows, "after_trim");
    summarize_phase(&rows, "post_trim_warm");
    summarize_phase(&rows, "after_refresh");

    if let Some(row) = rows.iter().find(|row| row.phase == "post_idle") {
        println!(
            "{:>12}: ws_delta={:.3}MiB private_delta={:.3}MiB after {}ms",
            row.phase, row.post_ws_delta_mb, row.post_private_delta_mb, config.idle_ms
        );
    }

    Ok(())
}

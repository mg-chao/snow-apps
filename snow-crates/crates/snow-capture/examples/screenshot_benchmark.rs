//! Screenshot-scenario performance benchmark for the DXGI, WGC and GDI
//! capture backends.
//!
//! Measures, for one backend per process run (so memory numbers are not
//! contaminated between backends):
//!
//! 1. **Initialization memory footprint** - working set / private bytes at
//!    each init step: `CaptureSystem` build, `open_session`, optional
//!    `prewarm_environment`, and the first capture (which performs the lazy
//!    backend initialization such as DXGI `DuplicateOutput`, the WGC worker
//!    + frame pool, or the GDI DIB sections).
//! 2. **Capture latency in detail** - per-capture wall latency, the session
//!    `capture_duration`, and the per-stage breakdown recorded by the
//!    backends (`CaptureOptions::record_stage_timings`). This separates the
//!    raw Windows system call latency (`dxgi.sys.acquire_next_frame`,
//!    `gdi.sys.bitblt`, the WGC frame-pool transport wait) from the
//!    implementation's processing stages (dirty-rect handling, GPU copy,
//!    readback wait, staging map, CPU conversion, frame buffer allocation),
//!    plus the OS frame age reported by DXGI/WGC timestamps.
//! 3. **Memory footprint after capture completes** - post-loop, warm-loop
//!    average/peak, and settled samples after releasing idle resources, to
//!    expose retained buffers and growth.
//!
//! Two loops are measured:
//! - **warm reuse** loop: repeated `capture_reuse` on a kept frame buffer
//!   (the steady-state screenshot path), and
//! - **cold snapshot** loop: repeated `capture_once` (releases capture
//!   access after every shot, forcing per-shot re-acquisition like a
//!   screenshot app that parks its session between shots).
//!
//! This example requires the `stage-timing` cargo feature (`required-
//! features` in Cargo.toml), which compiles the instrumentation into the
//! capture backends. Production/release builds never enable that feature,
//! so they contain none of this instrumentation.
//!
//! Usage:
//! ```text
//! cargo run -p snow-capture --release --features stage-timing --example screenshot_benchmark -- --backend dxgi
//! cargo run -p snow-capture --release --features stage-timing --example screenshot_benchmark -- --backend wgc --frames 50 --cold-frames 10
//! cargo run -p snow-capture --release --features stage-timing --example screenshot_benchmark -- --backend gdi --save-baseline target/perf/gdi-baseline.csv
//! cargo run -p snow-capture --release --features stage-timing --example screenshot_benchmark -- --backend dxgi --baseline target/perf/dxgi-baseline.csv
//! ```
//!
//! Run all three backends via `scripts/perf/run_screenshot_benchmark.ps1`,
//! which collects the per-backend rows into one CSV.
//!
//! Stage labels are stable:
//! - `dxgi.*`: `lazy_init` (record, cold only), `acquire_loop` (mark),
//!   `sys.acquire_next_frame` (record, pure OS call), `frame_age` (record,
//!   OS present-to-read delay), `frame_metadata`, `hdr_prepare`, `dirty_eval`
//!   (marks), `gpu_copy`, `readback_wait` (records)
//! - `wgc.*`: `transport_wait`, `submit` (marks), `gpu_copy`,
//!   `readback_wait`, `frame_age` (records)
//! - `gdi.*`: `surface_prepare`, `sys.bitblt` (records), `dirty_scan`
//!   (record, continuous workload only), `convert` (record)
//! - `readback.*` (shared GPU readback path): `map`, `convert` (records),
//!   `frame_alloc` (record; overlaps `readback.convert` / precedes
//!   `gdi.convert`)
//!
//! Entries marked *(record)* may overlap other entries and are diagnostic
//! detail; the additive per-backend primary stage sets used for the
//! "accounted / unaccounted" numbers are listed in `primary_stages`.

use std::fs;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use rustc_hash::FxHashMap;
use snow_capture::backend::CaptureBackendKind;
use snow_capture::timing::StageTiming;
use snow_capture::{CaptureOptions, CaptureSystem, CaptureTarget};

#[cfg(target_os = "windows")]
use windows::Win32::System::ProcessStatus::{
    K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS, PROCESS_MEMORY_COUNTERS_EX,
};
#[cfg(target_os = "windows")]
use windows::Win32::System::Threading::GetCurrentProcess;

const DEFAULT_WARMUP_FRAMES: usize = 10;
const DEFAULT_MEASURE_FRAMES: usize = 50;
const DEFAULT_COLD_FRAMES: usize = 10;
const DEFAULT_SETTLE_MS: u64 = 500;
const DEFAULT_MAX_REGRESSION_PCT: f64 = 10.0;
const MIB: f64 = 1024.0 * 1024.0;
const US: f64 = 1000.0; // per-millisecond multiplier

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum RegressionMetric {
    SystemBuild,
    SessionOpen,
    FirstCapture,
    WarmAvg,
    WarmP50,
    WarmP95,
    InitWsDelta,
    InitPrivDelta,
    PostWsDelta,
    PostPrivDelta,
}

impl RegressionMetric {
    fn parse(raw: &str) -> Option<Self> {
        match raw.trim().to_ascii_lowercase().as_str() {
            "system_build" | "build" => Some(Self::SystemBuild),
            "session_open" | "open" => Some(Self::SessionOpen),
            "first_capture" | "first" => Some(Self::FirstCapture),
            "avg" | "average" => Some(Self::WarmAvg),
            "p50" | "median" => Some(Self::WarmP50),
            "p95" => Some(Self::WarmP95),
            "init_ws_delta" | "init_ws" => Some(Self::InitWsDelta),
            "init_priv_delta" | "init_priv" => Some(Self::InitPrivDelta),
            "post_ws_delta" | "post_ws" => Some(Self::PostWsDelta),
            "post_priv_delta" | "post_priv" => Some(Self::PostPrivDelta),
            _ => None,
        }
    }

    /// CSV column name; the baseline comparison looks metrics up by this.
    fn as_str(self) -> &'static str {
        match self {
            Self::SystemBuild => "system_build_ms",
            Self::SessionOpen => "session_open_ms",
            Self::FirstCapture => "first_capture_ms",
            Self::WarmAvg => "warm_wall_avg_ms",
            Self::WarmP50 => "warm_wall_p50_ms",
            Self::WarmP95 => "warm_wall_p95_ms",
            Self::InitWsDelta => "mem_init_ws_delta_mb",
            Self::InitPrivDelta => "mem_init_priv_delta_mb",
            Self::PostWsDelta => "mem_post_ws_delta_mb",
            Self::PostPrivDelta => "mem_post_priv_delta_mb",
        }
    }
}

#[derive(Clone, Debug)]
struct Config {
    backend: CaptureBackendKind,
    warmup_frames: usize,
    measure_frames: usize,
    cold_frames: usize,
    prewarm: bool,
    settle_ms: u64,
    csv_path: Option<PathBuf>,
    baseline_path: Option<PathBuf>,
    save_baseline_path: Option<PathBuf>,
    max_regression_pct: f64,
    regression_metrics: Vec<RegressionMetric>,
    allow_debug: bool,
}

/// Additive per-backend stage sets. These entries tile the backend capture
/// call; everything else (`sys.*`, `frame_age`, `frame_alloc`, `gpu_copy`
/// where it overlaps `submit`) is overlapping detail.
fn primary_stages(kind: CaptureBackendKind) -> &'static [&'static str] {
    match kind {
        CaptureBackendKind::DxgiDuplication => &[
            "dxgi.acquire_loop",
            "dxgi.frame_metadata",
            "dxgi.hdr_prepare",
            "dxgi.dirty_eval",
            "dxgi.gpu_copy",
            "readback.map",
            "readback.convert",
        ],
        CaptureBackendKind::WindowsGraphicsCapture => &[
            "wgc.transport_wait",
            "wgc.submit",
            "wgc.readback_wait",
            "readback.map",
            "readback.convert",
        ],
        // `gdi.dirty_scan` only occurs in the continuous workload; summing
        // only the entries present in a given frame keeps this additive.
        CaptureBackendKind::Gdi => &[
            "gdi.surface_prepare",
            "gdi.sys.bitblt",
            "readback.frame_alloc",
            "gdi.dirty_scan",
            "gdi.convert",
        ],
        CaptureBackendKind::Auto => &[],
    }
}

fn backend_name(kind: CaptureBackendKind) -> &'static str {
    kind.as_str()
}

fn parse_backend(token: &str) -> Option<CaptureBackendKind> {
    match token.trim().to_ascii_lowercase().as_str() {
        "dxgi" | "dxgi-duplication" | "duplication" => Some(CaptureBackendKind::DxgiDuplication),
        "wgc" | "windowsgraphicscapture" | "windows-graphics-capture" => {
            Some(CaptureBackendKind::WindowsGraphicsCapture)
        }
        "gdi" => Some(CaptureBackendKind::Gdi),
        _ => None,
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

    fn ws_mib(self) -> f64 {
        self.working_set_bytes as f64 / MIB
    }

    fn priv_mib(self) -> f64 {
        self.private_bytes as f64 / MIB
    }
}

fn bytes_to_mib(bytes: u64) -> f64 {
    bytes as f64 / MIB
}

/// Per-stage latency samples (microseconds) collected across captures.
#[derive(Default)]
struct StageCollector {
    samples: FxHashMap<&'static str, Vec<f64>>,
}

impl StageCollector {
    fn record(&mut self, timings: &[StageTiming]) {
        for timing in timings {
            self.samples
                .entry(timing.name)
                .or_default()
                .push(timing.duration.as_secs_f64() * 1000.0 * US);
        }
    }

    /// Sum of the additive primary stages for a single capture, in
    /// microseconds. Only entries present in `timings` contribute.
    fn primary_sum(kind: CaptureBackendKind, timings: &[StageTiming]) -> f64 {
        let primary = primary_stages(kind);
        timings
            .iter()
            .filter(|timing| primary.contains(&timing.name))
            .map(|timing| timing.duration.as_secs_f64() * 1000.0 * US)
            .sum()
    }

    fn sorted_names(&self) -> Vec<&'static str> {
        let mut names: Vec<&'static str> = self.samples.keys().copied().collect();
        names.sort_unstable();
        names
    }

    fn stats(&self, name: &str) -> Option<SampleStats> {
        let samples = self.samples.get(name)?;
        Some(summarize_samples(samples))
    }
}

#[derive(Clone, Debug, Default)]
struct LoopStats {
    wall: Option<SampleStats>,
    capture_duration: Option<SampleStats>,
    fresh_wall: Option<SampleStats>,
    unaccounted: Option<SampleStats>,
    duplicates: u64,
    frames: u64,
    warm_avg_ws_delta_mb: f64,
    warm_avg_priv_delta_mb: f64,
    warm_peak_ws_delta_mb: f64,
    warm_peak_priv_delta_mb: f64,
}

#[derive(Clone, Debug)]
struct ScreenshotBenchResult {
    backend: CaptureBackendKind,
    width: u32,
    height: u32,
    system_build_ms: f64,
    session_open_ms: f64,
    prewarm_ms: Option<f64>,
    first_capture_ms: f64,
    first_stage_timings: Vec<StageTiming>,
    warm: LoopStats,
    cold: LoopStats,
    base: ProcessMemorySample,
    after_system: ProcessMemorySample,
    after_session: ProcessMemorySample,
    after_prewarm: Option<ProcessMemorySample>,
    after_first: ProcessMemorySample,
    post: ProcessMemorySample,
    settled: ProcessMemorySample,
    stage_order: Vec<&'static str>,
    stages: FxHashMap<&'static str, SampleStats>,
}

impl ScreenshotBenchResult {
    /// Memory added by initializing the backend, relative to process base.
    fn init_delta(&self) -> ProcessMemorySample {
        ProcessMemorySample {
            working_set_bytes: self.after_first.working_set_bytes - self.base.working_set_bytes,
            private_bytes: self.after_first.private_bytes - self.base.private_bytes,
        }
    }

    /// Memory retained after the loops complete and idle resources settle.
    fn post_delta(&self) -> ProcessMemorySample {
        ProcessMemorySample {
            working_set_bytes: self.settled.working_set_bytes - self.base.working_set_bytes,
            private_bytes: self.settled.private_bytes - self.base.private_bytes,
        }
    }

    fn metric_value(&self, metric: RegressionMetric) -> f64 {
        match metric {
            RegressionMetric::SystemBuild => self.system_build_ms,
            RegressionMetric::SessionOpen => self.session_open_ms,
            RegressionMetric::FirstCapture => self.first_capture_ms,
            RegressionMetric::WarmAvg => self.warm.wall.as_ref().map_or(0.0, |s| s.avg),
            RegressionMetric::WarmP50 => self.warm.wall.as_ref().map_or(0.0, |s| s.p50),
            RegressionMetric::WarmP95 => self.warm.wall.as_ref().map_or(0.0, |s| s.p95),
            RegressionMetric::InitWsDelta => bytes_to_mib(self.init_delta().working_set_bytes),
            RegressionMetric::InitPrivDelta => bytes_to_mib(self.init_delta().private_bytes),
            RegressionMetric::PostWsDelta => bytes_to_mib(self.post_delta().working_set_bytes),
            RegressionMetric::PostPrivDelta => bytes_to_mib(self.post_delta().private_bytes),
        }
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

fn summarize_samples(samples: &[f64]) -> SampleStats {
    if samples.is_empty() {
        return SampleStats::default();
    }
    let mut sorted = samples.to_vec();
    sorted.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let avg = samples.iter().sum::<f64>() / samples.len() as f64;
    let variance = samples
        .iter()
        .map(|sample| {
            let d = *sample - avg;
            d * d
        })
        .sum::<f64>()
        / samples.len() as f64;
    SampleStats {
        avg,
        p50: percentile(&sorted, 0.50),
        p95: percentile(&sorted, 0.95),
        p99: percentile(&sorted, 0.99),
        min: *sorted.first().unwrap(),
        max: *sorted.last().unwrap(),
        stddev: variance.sqrt(),
    }
}

fn elapsed_ms(start: Instant) -> f64 {
    start.elapsed().as_secs_f64() * 1000.0
}

#[derive(Default)]
struct LoopMeasurements {
    wall_us: Vec<f64>,
    capture_us: Vec<f64>,
    fresh_wall_us: Vec<f64>,
    unaccounted_us: Vec<f64>,
    duplicates: u64,
    frames: u64,
}

fn run_backend(kind: CaptureBackendKind, config: &Config) -> Result<ScreenshotBenchResult> {
    println!("=== screenshot benchmark: {} ===", backend_name(kind));

    // Phase 1: process baseline before touching capture APIs.
    let base = ProcessMemorySample::capture()?;
    println!(
        "base memory: ws {:.2} MiB, private {:.2} MiB",
        base.ws_mib(),
        base.priv_mib()
    );

    // Phase 2: initialization (the cold screenshot path).
    let build_start = Instant::now();
    let system = CaptureSystem::builder()
        .with_backend_kind(kind)
        .build()
        .with_context(|| format!("failed to build capture system for {}", backend_name(kind)))?;
    let system_build_ms = elapsed_ms(build_start);
    let after_system = ProcessMemorySample::capture()?;

    let open_start = Instant::now();
    let mut session = system
        .open_session(
            CaptureTarget::PrimaryMonitor,
            CaptureOptions {
                record_stage_timings: true,
                // A quiet desktop may present no new frame within the DXGI
                // snapshot acquisition budget; a session-level retry (which
                // recreates the duplication and waits again) recovers the
                // same way a real screenshot caller would.
                capture_retry_count: 3,
                ..CaptureOptions::default()
            },
        )
        .with_context(|| format!("failed to open session for {}", backend_name(kind)))?;
    let session_open_ms = elapsed_ms(open_start);
    let after_session = ProcessMemorySample::capture()?;

    let mut prewarm_ms = None;
    let mut after_prewarm = None;
    if config.prewarm {
        let prewarm_start = Instant::now();
        session
            .prewarm_environment()
            .context("prewarm_environment failed")?;
        prewarm_ms = Some(elapsed_ms(prewarm_start));
        after_prewarm = Some(ProcessMemorySample::capture()?);
    }

    // First capture: completes the lazy backend initialization (DXGI
    // duplication object, WGC worker + frame pool, GDI DIBs) and releases
    // capture access like a one-shot screenshot.
    let first_start = Instant::now();
    let mut frame = session
        .capture_once()
        .context("first capture failed; the backend may be unavailable on this machine")?;
    let first_capture_ms = elapsed_ms(first_start);
    let first_stage_timings = frame.metadata().stage_timings().to_vec();
    let after_first = ProcessMemorySample::capture()?;
    let (width, height) = frame.dimensions();
    println!(
        "init: system {:.3} ms, session {:.3} ms, first capture {:.3} ms ({}x{})",
        system_build_ms, session_open_ms, first_capture_ms, width, height
    );
    println!(
        "init memory: after system ws {:.2}/priv {:.2} MiB, after session ws {:.2}/priv {:.2} MiB, after first capture ws {:.2}/priv {:.2} MiB (deltas {:.2}/{:.2} MiB)",
        after_system.ws_mib(),
        after_system.priv_mib(),
        after_session.ws_mib(),
        after_session.priv_mib(),
        after_first.ws_mib(),
        after_first.priv_mib(),
        bytes_to_mib(after_first.working_set_bytes - base.working_set_bytes),
        bytes_to_mib(after_first.private_bytes - base.private_bytes),
    );
    if !first_stage_timings.is_empty() {
        println!("first capture stages:");
        for timing in &first_stage_timings {
            println!(
                "  {:<32} {:>10.3} us",
                timing.name,
                timing.duration.as_secs_f64() * 1000.0 * US
            );
        }
    }

    // Phase 3: warmup (steady-state screenshot path on a reused buffer).
    for _ in 0..config.warmup_frames {
        frame = session
            .capture_reuse(frame)
            .context("warmup capture failed")?;
    }

    // Phase 4: measured warm-reuse loop. Stage samples are collected from
    // the same iterations, recorded after the timed bracket closes.
    let mut warm = LoopMeasurements::default();
    let mut warm_stage_collector = StageCollector::default();
    let mut peak_ws_delta = 0.0f64;
    let mut peak_priv_delta = 0.0f64;
    let mut ws_deltas = Vec::new();
    let mut priv_deltas = Vec::new();
    for _ in 0..config.measure_frames {
        let start = Instant::now();
        frame = session
            .capture_reuse(frame)
            .context("warm capture failed")?;
        let wall = start.elapsed();

        let metadata = frame.metadata();
        let capture_us = metadata
            .capture_duration()
            .map_or(0.0, |d| d.as_secs_f64() * 1000.0 * US);
        let primary_us = StageCollector::primary_sum(kind, metadata.stage_timings());
        warm.wall_us.push(wall.as_secs_f64() * 1000.0 * US);
        warm.capture_us.push(capture_us);
        warm.unaccounted_us.push((capture_us - primary_us).max(0.0));
        if !metadata.is_duplicate() {
            warm.fresh_wall_us.push(wall.as_secs_f64() * 1000.0 * US);
        }
        if metadata.is_duplicate() {
            warm.duplicates += 1;
        }
        warm.frames += 1;
        warm_stage_collector.record(metadata.stage_timings());

        // Memory sampled outside the timed bracket above.
        let sample = ProcessMemorySample::capture()?;
        let ws_delta = bytes_to_mib(
            sample
                .working_set_bytes
                .saturating_sub(base.working_set_bytes),
        );
        let priv_delta = bytes_to_mib(sample.private_bytes.saturating_sub(base.private_bytes));
        peak_ws_delta = peak_ws_delta.max(ws_delta);
        peak_priv_delta = peak_priv_delta.max(priv_delta);
        ws_deltas.push(ws_delta);
        priv_deltas.push(priv_delta);
    }

    // Phase 5: cold-snapshot loop (release + re-acquire per shot).
    let mut cold = LoopMeasurements::default();
    let mut cold_stage_collector = StageCollector::default();
    for _ in 0..config.cold_frames {
        let start = Instant::now();
        frame = session.capture_once().context("cold capture failed")?;
        let wall = start.elapsed();

        let metadata = frame.metadata();
        let capture_us = metadata
            .capture_duration()
            .map_or(0.0, |d| d.as_secs_f64() * 1000.0 * US);
        let primary_us = StageCollector::primary_sum(kind, metadata.stage_timings());
        cold.wall_us.push(wall.as_secs_f64() * 1000.0 * US);
        cold.capture_us.push(capture_us);
        cold.unaccounted_us.push((capture_us - primary_us).max(0.0));
        if !metadata.is_duplicate() {
            cold.fresh_wall_us.push(wall.as_secs_f64() * 1000.0 * US);
        }
        if metadata.is_duplicate() {
            cold.duplicates += 1;
        }
        cold.frames += 1;
        cold_stage_collector.record(metadata.stage_timings());
    }

    // Phase 6: post-capture memory. Drop the frame, take an immediate
    // sample, then release idle resources and let the process settle.
    drop(frame);
    let post = ProcessMemorySample::capture()?;
    session.release_idle_resources();
    std::thread::sleep(Duration::from_millis(config.settle_ms));
    let settled = ProcessMemorySample::capture()?;

    println!(
        "post memory: ws {:.2}/priv {:.2} MiB, settled ws {:.2}/priv {:.2} MiB (deltas vs base {:.2}/{:.2} MiB)",
        post.ws_mib(),
        post.priv_mib(),
        settled.ws_mib(),
        settled.priv_mib(),
        bytes_to_mib(
            settled
                .working_set_bytes
                .saturating_sub(base.working_set_bytes)
        ),
        bytes_to_mib(settled.private_bytes.saturating_sub(base.private_bytes)),
    );

    // Merge warm+cold stage samples for reporting.
    let mut merged_stages = StageCollector::default();
    for (name, samples) in warm_stage_collector.samples {
        merged_stages.samples.insert(name, samples);
    }
    for (name, samples) in cold_stage_collector.samples {
        merged_stages
            .samples
            .entry(name)
            .or_default()
            .extend(samples);
    }
    let stage_order = merged_stages.sorted_names();
    let stage_stats: FxHashMap<&'static str, SampleStats> = stage_order
        .iter()
        .filter_map(|name| merged_stages.stats(name).map(|stats| (*name, stats)))
        .collect();

    let warm_stats = LoopStats {
        wall: Some(summarize_samples(&warm.wall_us)),
        capture_duration: Some(summarize_samples(&warm.capture_us)),
        fresh_wall: (!warm.fresh_wall_us.is_empty())
            .then(|| summarize_samples(&warm.fresh_wall_us)),
        unaccounted: Some(summarize_samples(&warm.unaccounted_us)),
        duplicates: warm.duplicates,
        frames: warm.frames,
        warm_avg_ws_delta_mb: ws_deltas.iter().sum::<f64>() / ws_deltas.len() as f64,
        warm_avg_priv_delta_mb: priv_deltas.iter().sum::<f64>() / priv_deltas.len() as f64,
        warm_peak_ws_delta_mb: peak_ws_delta,
        warm_peak_priv_delta_mb: peak_priv_delta,
    };
    let cold_stats = LoopStats {
        wall: (!cold.wall_us.is_empty()).then(|| summarize_samples(&cold.wall_us)),
        capture_duration: (!cold.capture_us.is_empty())
            .then(|| summarize_samples(&cold.capture_us)),
        fresh_wall: (!cold.fresh_wall_us.is_empty())
            .then(|| summarize_samples(&cold.fresh_wall_us)),
        unaccounted: (!cold.unaccounted_us.is_empty())
            .then(|| summarize_samples(&cold.unaccounted_us)),
        duplicates: cold.duplicates,
        frames: cold.frames,
        ..LoopStats::default()
    };

    print_loop_report("warm reuse (capture_reuse)", &warm_stats);
    print_loop_report("cold snapshot (capture_once)", &cold_stats);

    println!("stage timings (warm + cold captures merged):");
    println!(
        "  {:<32} {:>10} {:>10} {:>10} {:>10}",
        "stage", "avg_us", "p50_us", "p95_us", "max_us"
    );
    for name in &stage_order {
        if let Some(stats) = stage_stats.get(name) {
            println!(
                "  {:<32} {:>10.1} {:>10.1} {:>10.1} {:>10.1}",
                name, stats.avg, stats.p50, stats.p95, stats.max
            );
        }
    }
    let primary_names = primary_stages(kind);
    println!(
        "additive primary stages for {}: {:?}",
        backend_name(kind),
        primary_names
    );

    Ok(ScreenshotBenchResult {
        backend: kind,
        width,
        height,
        system_build_ms,
        session_open_ms,
        prewarm_ms,
        first_capture_ms,
        first_stage_timings,
        warm: warm_stats,
        cold: cold_stats,
        base,
        after_system,
        after_session,
        after_prewarm,
        after_first,
        post,
        settled,
        stage_order,
        stages: stage_stats,
    })
}

fn print_loop_report(label: &str, stats: &LoopStats) {
    println!("-- {label} --");
    if stats.frames == 0 {
        println!("  skipped");
        return;
    }
    if let Some(wall) = &stats.wall {
        println!(
            "  wall us:           avg {:>10.1}  p50 {:>10.1}  p95 {:>10.1}  p99 {:>10.1}  min {:>10.1}  max {:>10.1}  stddev {:>8.1}  (frames {})",
            wall.avg, wall.p50, wall.p95, wall.p99, wall.min, wall.max, wall.stddev, stats.frames
        );
    }
    if let Some(capture) = &stats.capture_duration {
        println!(
            "  capture_duration:  avg {:>10.1}  p50 {:>10.1}  p95 {:>10.1}  p99 {:>10.1}",
            capture.avg, capture.p50, capture.p95, capture.p99
        );
    }
    if let Some(fresh) = &stats.fresh_wall {
        println!(
            "  fresh-frame wall:  avg {:>10.1}  p50 {:>10.1}  p95 {:>10.1}",
            fresh.avg, fresh.p50, fresh.p95
        );
    }
    if let Some(unaccounted) = &stats.unaccounted {
        println!(
            "  unaccounted us:    avg {:>10.1}  p50 {:>10.1}  p95 {:>10.1}  (capture_duration minus primary stages)",
            unaccounted.avg, unaccounted.p50, unaccounted.p95
        );
    }
    let duplicate_pct = stats.duplicates as f64 / stats.frames as f64 * 100.0;
    println!("  duplicates: {} ({:.1}%)", stats.duplicates, duplicate_pct);
}

fn format_csv_field(value: f64) -> String {
    if value.is_finite() {
        format!("{value:.3}")
    } else {
        String::new()
    }
}

fn csv_header(result: &ScreenshotBenchResult) -> String {
    let mut columns = vec![
        "backend".to_string(),
        "width".to_string(),
        "height".to_string(),
        "system_build_ms".to_string(),
        "session_open_ms".to_string(),
        "prewarm_ms".to_string(),
        "first_capture_ms".to_string(),
        "warm_wall_avg_ms".to_string(),
        "warm_wall_p50_ms".to_string(),
        "warm_wall_p95_ms".to_string(),
        "warm_wall_p99_ms".to_string(),
        "warm_capture_p50_ms".to_string(),
        "warm_capture_p95_ms".to_string(),
        "warm_fresh_wall_p50_ms".to_string(),
        "warm_unaccounted_p50_us".to_string(),
        "warm_unaccounted_p95_us".to_string(),
        "warm_duplicate_pct".to_string(),
        "cold_wall_avg_ms".to_string(),
        "cold_wall_p50_ms".to_string(),
        "cold_wall_p95_ms".to_string(),
        "cold_unaccounted_p50_us".to_string(),
        "mem_base_ws_mb".to_string(),
        "mem_base_priv_mb".to_string(),
        "mem_after_system_ws_mb".to_string(),
        "mem_after_system_priv_mb".to_string(),
        "mem_after_session_ws_mb".to_string(),
        "mem_after_session_priv_mb".to_string(),
        "mem_after_prewarm_ws_mb".to_string(),
        "mem_after_prewarm_priv_mb".to_string(),
        "mem_after_first_ws_mb".to_string(),
        "mem_after_first_priv_mb".to_string(),
        "mem_init_ws_delta_mb".to_string(),
        "mem_init_priv_delta_mb".to_string(),
        "mem_warm_avg_ws_delta_mb".to_string(),
        "mem_warm_avg_priv_delta_mb".to_string(),
        "mem_warm_peak_ws_delta_mb".to_string(),
        "mem_warm_peak_priv_delta_mb".to_string(),
        "mem_post_ws_mb".to_string(),
        "mem_post_priv_mb".to_string(),
        "mem_settled_ws_mb".to_string(),
        "mem_settled_priv_mb".to_string(),
        "mem_post_ws_delta_mb".to_string(),
        "mem_post_priv_delta_mb".to_string(),
    ];
    for name in &result.stage_order {
        columns.push(format!("stage_{name}_avg_us"));
        columns.push(format!("stage_{name}_p95_us"));
    }
    for timing in &result.first_stage_timings {
        columns.push(format!("first_stage_{}_us", timing.name));
    }
    columns.join(",")
}

fn csv_row(result: &ScreenshotBenchResult) -> String {
    let stage = |name: &str, pick: fn(&SampleStats) -> f64| -> f64 {
        result.stages.get(name).map_or(f64::NAN, pick)
    };
    let warm_wall = result.warm.wall.as_ref();
    let warm_capture = result.warm.capture_duration.as_ref();
    let warm_fresh = result.warm.fresh_wall.as_ref();
    let warm_unaccounted = result.warm.unaccounted.as_ref();
    let cold_wall = result.cold.wall.as_ref();
    let cold_unaccounted = result.cold.unaccounted.as_ref();
    let warm_duplicate_pct = if result.warm.frames > 0 {
        result.warm.duplicates as f64 / result.warm.frames as f64 * 100.0
    } else {
        f64::NAN
    };

    let mut fields = vec![
        backend_name(result.backend).to_string(),
        result.width.to_string(),
        result.height.to_string(),
        format_csv_field(result.system_build_ms),
        format_csv_field(result.session_open_ms),
        result.prewarm_ms.map(format_csv_field).unwrap_or_default(),
        format_csv_field(result.first_capture_ms),
        format_csv_field(warm_wall.map_or(f64::NAN, |s| s.avg)),
        format_csv_field(warm_wall.map_or(f64::NAN, |s| s.p50)),
        format_csv_field(warm_wall.map_or(f64::NAN, |s| s.p95)),
        format_csv_field(warm_wall.map_or(f64::NAN, |s| s.p99)),
        format_csv_field(warm_capture.map_or(f64::NAN, |s| s.p50)),
        format_csv_field(warm_capture.map_or(f64::NAN, |s| s.p95)),
        format_csv_field(warm_fresh.map_or(f64::NAN, |s| s.p50)),
        format_csv_field(warm_unaccounted.map_or(f64::NAN, |s| s.p50)),
        format_csv_field(warm_unaccounted.map_or(f64::NAN, |s| s.p95)),
        format_csv_field(warm_duplicate_pct),
        format_csv_field(cold_wall.map_or(f64::NAN, |s| s.avg)),
        format_csv_field(cold_wall.map_or(f64::NAN, |s| s.p50)),
        format_csv_field(cold_wall.map_or(f64::NAN, |s| s.p95)),
        format_csv_field(cold_unaccounted.map_or(f64::NAN, |s| s.p50)),
        format_csv_field(result.base.ws_mib()),
        format_csv_field(result.base.priv_mib()),
        format_csv_field(result.after_system.ws_mib()),
        format_csv_field(result.after_system.priv_mib()),
        format_csv_field(result.after_session.ws_mib()),
        format_csv_field(result.after_session.priv_mib()),
        result
            .after_prewarm
            .map(|sample| format_csv_field(sample.ws_mib()))
            .unwrap_or_default(),
        result
            .after_prewarm
            .map(|sample| format_csv_field(sample.priv_mib()))
            .unwrap_or_default(),
        format_csv_field(result.after_first.ws_mib()),
        format_csv_field(result.after_first.priv_mib()),
        format_csv_field(bytes_to_mib(result.init_delta().working_set_bytes)),
        format_csv_field(bytes_to_mib(result.init_delta().private_bytes)),
        format_csv_field(result.warm.warm_avg_ws_delta_mb),
        format_csv_field(result.warm.warm_avg_priv_delta_mb),
        format_csv_field(result.warm.warm_peak_ws_delta_mb),
        format_csv_field(result.warm.warm_peak_priv_delta_mb),
        format_csv_field(result.post.ws_mib()),
        format_csv_field(result.post.priv_mib()),
        format_csv_field(result.settled.ws_mib()),
        format_csv_field(result.settled.priv_mib()),
        format_csv_field(bytes_to_mib(result.post_delta().working_set_bytes)),
        format_csv_field(bytes_to_mib(result.post_delta().private_bytes)),
    ];
    for name in &result.stage_order {
        fields.push(format_csv_field(stage(name, |s| s.avg)));
        fields.push(format_csv_field(stage(name, |s| s.p95)));
    }
    for timing in &result.first_stage_timings {
        fields.push(format_csv_field(
            timing.duration.as_secs_f64() * 1000.0 * US,
        ));
    }
    fields.join(",")
}

fn read_baseline_row(path: &PathBuf) -> Result<FxHashMap<String, f64>> {
    let content = fs::read_to_string(path)
        .with_context(|| format!("failed to read baseline file {}", path.display()))?;
    let mut lines = content.lines().filter(|line| !line.trim().is_empty());
    let header = lines.next().context("baseline file has no header row")?;
    let row = lines.next().context("baseline file has no data row")?;
    let header_fields: Vec<&str> = header.split(',').collect();
    let row_fields: Vec<&str> = row.split(',').collect();
    if header_fields.len() != row_fields.len() {
        bail!(
            "baseline file column mismatch: {} header fields vs {} row fields",
            header_fields.len(),
            row_fields.len()
        );
    }
    let mut values = FxHashMap::default();
    for (name, raw) in header_fields.iter().zip(row_fields.iter()) {
        if let Ok(value) = raw.trim().parse::<f64>() {
            values.insert(name.trim().to_string(), value);
        }
    }
    Ok(values)
}

fn check_regression(result: &ScreenshotBenchResult, config: &Config) -> Result<()> {
    let Some(baseline_path) = &config.baseline_path else {
        return Ok(());
    };
    let baseline = read_baseline_row(baseline_path)?;
    let mut failures = Vec::new();
    for metric in &config.regression_metrics {
        let column = metric.as_str();
        let Some(base_value) = baseline.get(column) else {
            println!("regression: baseline has no column {column}, skipping");
            continue;
        };
        let current = result.metric_value(*metric);
        if *base_value <= 0.0 {
            continue;
        }
        let regression_pct = (current - *base_value) / *base_value * 100.0;
        let verdict = if regression_pct > config.max_regression_pct {
            "REGRESSION"
        } else {
            "ok"
        };
        println!(
            "regression {column}: baseline {base_value:.3} -> current {current:.3} ({regression_pct:+.1}%) {verdict}"
        );
        if regression_pct > config.max_regression_pct {
            failures.push(format!(
                "{column} regressed {regression_pct:.1}% > {:.1}% (baseline {base_value:.3}, current {current:.3})",
                config.max_regression_pct
            ));
        }
    }
    if failures.is_empty() {
        Ok(())
    } else {
        for failure in &failures {
            eprintln!("REGRESSION FAILURE: {failure}");
        }
        bail!("{} regression gate(s) failed", failures.len())
    }
}

fn write_csv(path: &PathBuf, result: &ScreenshotBenchResult) -> Result<()> {
    if let Some(parent) = path.parent()
        && !parent.as_os_str().is_empty()
    {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create {}", parent.display()))?;
    }
    let content = format!("{}\n{}\n", csv_header(result), csv_row(result));
    fs::write(path, content).with_context(|| format!("failed to write {}", path.display()))?;
    println!("csv written to {}", path.display());
    Ok(())
}

fn print_usage() {
    println!(
        "screenshot_benchmark - per-backend screenshot scenario memory/latency benchmark

Usage:
  screenshot_benchmark --backend <dxgi|wgc|gdi> [options]

One backend per process run keeps memory measurements isolated; use
scripts/perf/run_screenshot_benchmark.ps1 to run all three.

Options:
  --backend <name>         capture backend to benchmark (required)
  --warmup <n>             warmup captures before measurement (default {DEFAULT_WARMUP_FRAMES})
  --frames <n>             measured warm-reuse captures (default {DEFAULT_MEASURE_FRAMES})
  --cold-frames <n>        measured cold-snapshot captures via capture_once (default {DEFAULT_COLD_FRAMES})
  --prewarm                additionally measure a prewarm_environment phase
  --settle-ms <n>          idle settle time before the settled memory sample (default {DEFAULT_SETTLE_MS})
  --csv <path>             write a single-row CSV of all metrics
  --save-baseline <path>   write the CSV row for later --baseline comparison
  --baseline <path>        compare metrics against a saved baseline row
  --max-regression-pct <p> regression gate threshold (default {DEFAULT_MAX_REGRESSION_PCT})
  --regression-metrics <list> comma-separated metrics: system_build, session_open, first_capture,
                           avg, p50, p95, init_ws, init_priv, post_ws, post_priv (default p50)
  --allow-debug            permit running from a debug build (numbers are unreliable)"
    );
}

fn parse_usize_arg(flag: &str, raw: Option<&str>) -> Result<usize> {
    let Some(raw) = raw else {
        bail!("{flag} requires a value");
    };
    raw.trim()
        .parse::<usize>()
        .with_context(|| format!("{flag} expects a non-negative integer, got {raw}"))
}

fn parse_u64_arg(flag: &str, raw: Option<&str>) -> Result<u64> {
    let Some(raw) = raw else {
        bail!("{flag} requires a value");
    };
    raw.trim()
        .parse::<u64>()
        .with_context(|| format!("{flag} expects a non-negative integer, got {raw}"))
}

fn parse_f64_arg(flag: &str, raw: Option<&str>) -> Result<f64> {
    let Some(raw) = raw else {
        bail!("{flag} requires a value");
    };
    raw.trim()
        .parse::<f64>()
        .with_context(|| format!("{flag} expects a number, got {raw}"))
}

fn parse_config() -> Result<Config> {
    let mut backend = None;
    let mut warmup_frames = DEFAULT_WARMUP_FRAMES;
    let mut measure_frames = DEFAULT_MEASURE_FRAMES;
    let mut cold_frames = DEFAULT_COLD_FRAMES;
    let mut prewarm = false;
    let mut settle_ms = DEFAULT_SETTLE_MS;
    let mut csv_path = None;
    let mut baseline_path = None;
    let mut save_baseline_path = None;
    let mut max_regression_pct = DEFAULT_MAX_REGRESSION_PCT;
    let mut regression_metrics = vec![RegressionMetric::WarmP50];
    let mut allow_debug = false;

    let args: Vec<String> = std::env::args().collect();
    let mut i = 1usize;
    while i < args.len() {
        match args[i].as_str() {
            "--help" | "-h" => {
                print_usage();
                std::process::exit(0);
            }
            "--backend" => {
                let Some(raw) = args.get(i + 1).map(String::as_str) else {
                    bail!("--backend requires a value (dxgi|wgc|gdi)");
                };
                let Some(kind) = parse_backend(raw) else {
                    bail!("unknown backend {raw}; expected dxgi, wgc or gdi");
                };
                backend = Some(kind);
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
            "--cold-frames" => {
                cold_frames =
                    parse_usize_arg("--cold-frames", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--prewarm" => {
                prewarm = true;
                i += 1;
            }
            "--settle-ms" => {
                settle_ms = parse_u64_arg("--settle-ms", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--csv" => {
                let Some(raw) = args.get(i + 1) else {
                    bail!("--csv requires a file path");
                };
                csv_path = Some(PathBuf::from(raw));
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
                let mut metrics = Vec::new();
                for token in raw.split(',') {
                    let Some(metric) = RegressionMetric::parse(token) else {
                        bail!("unknown regression metric {token}");
                    };
                    metrics.push(metric);
                }
                if metrics.is_empty() {
                    bail!("--regression-metrics must list at least one metric");
                }
                regression_metrics = metrics;
                i += 2;
            }
            "--allow-debug" => {
                allow_debug = true;
                i += 1;
            }
            other => bail!("unknown argument: {other}. Use --help for usage."),
        }
    }

    let Some(backend) = backend else {
        bail!(
            "--backend <dxgi|wgc|gdi> is required; one backend per process keeps memory measurements isolated"
        );
    };
    if measure_frames == 0 {
        bail!("--frames must be > 0 so there are measured samples");
    }
    if !max_regression_pct.is_finite() || max_regression_pct < 0.0 {
        bail!("--max-regression-pct must be a finite value >= 0");
    }

    Ok(Config {
        backend,
        warmup_frames,
        measure_frames,
        cold_frames,
        prewarm,
        settle_ms,
        csv_path,
        baseline_path,
        save_baseline_path,
        max_regression_pct,
        regression_metrics,
        allow_debug,
    })
}

fn main() -> Result<()> {
    let config = parse_config()?;

    if cfg!(debug_assertions) && !config.allow_debug {
        bail!(
            "screenshot_benchmark must run with --release for meaningful numbers; pass --allow-debug to override"
        );
    }

    let result = run_backend(config.backend, &config)?;

    if let Some(path) = &config.csv_path {
        write_csv(path, &result)?;
    }
    if let Some(path) = &config.save_baseline_path {
        write_csv(path, &result)?;
    }
    check_regression(&result, &config)?;

    Ok(())
}

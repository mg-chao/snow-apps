#[cfg(windows)]
mod windows_example {
    use std::fs;
    use std::path::PathBuf;
    use std::time::Instant;

    use anyhow::{Context, Result, bail};
    use snow_capture::{
        CaptureOptions, CaptureRegion, CaptureSystem, CaptureTarget, CaptureWorkload, MonitorLayout,
    };
    use windows::Win32::System::ProcessStatus::{
        K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS, PROCESS_MEMORY_COUNTERS_EX,
    };
    use windows::Win32::System::Threading::GetCurrentProcess;

    const DEFAULT_SAMPLES: usize = 30;
    const DEFAULT_TTL_GROUP_SIZE: usize = 3;
    const MIB: f64 = 1024.0 * 1024.0;

    #[derive(Clone, Copy, Debug)]
    enum SnapshotPolicy {
        MemoryFirst,
        WarmTtl,
        AlwaysWarm,
    }

    impl SnapshotPolicy {
        const ALL: [Self; 3] = [Self::MemoryFirst, Self::WarmTtl, Self::AlwaysWarm];

        fn as_str(self) -> &'static str {
            match self {
                Self::MemoryFirst => "memory-first",
                Self::WarmTtl => "warm-ttl",
                Self::AlwaysWarm => "always-warm",
            }
        }
    }

    #[derive(Clone, Debug)]
    struct Config {
        samples: usize,
        ttl_group_size: usize,
        output: PathBuf,
        allow_debug: bool,
    }

    #[derive(Clone)]
    struct BenchTarget {
        label: String,
        target: CaptureTarget,
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
        target: String,
        policy: SnapshotPolicy,
        index: usize,
        after_trim: bool,
        setup_ms: f64,
        capture_ms: f64,
        total_ms: f64,
        duplicate: bool,
        base_ws_mb: f64,
        base_private_mb: f64,
        peak_ws_delta_mb: f64,
        peak_private_delta_mb: f64,
        post_ws_delta_mb: f64,
        post_private_delta_mb: f64,
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

    fn centered_primary_region(width: u32, height: u32) -> Result<CaptureRegion> {
        let layout = MonitorLayout::snapshot().context("failed to snapshot monitor layout")?;
        let primary = layout
            .monitors
            .iter()
            .find(|monitor| monitor.monitor.is_primary())
            .or_else(|| layout.monitors.first())
            .context("no monitor available for centered region")?;
        let fit_width = width.min(primary.width);
        let fit_height = height.min(primary.height);
        let x = primary.x + i32::try_from((primary.width - fit_width) / 2)?;
        let y = primary.y + i32::try_from((primary.height - fit_height) / 2)?;
        CaptureRegion::new(x, y, fit_width, fit_height).context("failed to build centered region")
    }

    fn parse_usize_arg(flag: &str, value: Option<&str>) -> Result<usize> {
        let Some(raw) = value else {
            bail!("{flag} requires a value");
        };
        raw.parse::<usize>()
            .with_context(|| format!("failed to parse {flag}: {raw}"))
    }

    fn print_usage() {
        println!(
        "Usage: cargo run --release --example snapshot_policy_benchmark -- [options]
  --samples <n>          samples per target/policy (default: {DEFAULT_SAMPLES})
  --ttl-group-size <n>   captures kept warm before trim in warm-ttl policy (default: {DEFAULT_TTL_GROUP_SIZE})
  --output <path>        csv output path (default: target/perf/snapshot-policy-benchmark.csv)
  --allow-debug          allow running outside Release for smoke checks"
    );
    }

    fn parse_args() -> Result<Config> {
        let mut config = Config {
            samples: DEFAULT_SAMPLES,
            ttl_group_size: DEFAULT_TTL_GROUP_SIZE,
            output: PathBuf::from("target/perf/snapshot-policy-benchmark.csv"),
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
                "--samples" => {
                    config.samples =
                        parse_usize_arg("--samples", args.get(i + 1).map(String::as_str))?;
                    i += 2;
                }
                "--ttl-group-size" => {
                    config.ttl_group_size =
                        parse_usize_arg("--ttl-group-size", args.get(i + 1).map(String::as_str))?;
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

        if config.samples == 0 {
            bail!("--samples must be > 0");
        }
        if config.ttl_group_size == 0 {
            bail!("--ttl-group-size must be > 0");
        }
        if cfg!(debug_assertions) && !config.allow_debug {
            bail!(
                "snapshot policy benchmarks must be run in Release; pass --allow-debug only for smoke checks"
            );
        }
        Ok(config)
    }

    fn capture_options() -> CaptureOptions {
        CaptureOptions {
            workload: CaptureWorkload::Snapshot,
            ..Default::default()
        }
    }

    fn capture_with_session(
        session: &mut snow_capture::CaptureSession,
        target_label: &str,
        policy: SnapshotPolicy,
        index: usize,
        setup_ms: f64,
        after_trim: bool,
    ) -> Result<BenchRow> {
        let base = ProcessMemorySample::capture()?;
        let total_started = Instant::now();
        let capture_started = Instant::now();
        let frame = session
            .capture()
            .with_context(|| format!("capture failed for {target_label}/{}", policy.as_str()))?;
        let capture_ms = elapsed_ms(capture_started);
        let total_ms = elapsed_ms(total_started) + setup_ms;
        let peak = ProcessMemorySample::capture()?;
        let (peak_ws_delta_mb, peak_private_delta_mb) = memory_deltas_mb(base, peak);
        let duplicate = frame.metadata().is_duplicate();
        drop(frame);
        let post = ProcessMemorySample::capture()?;
        let (post_ws_delta_mb, post_private_delta_mb) = memory_deltas_mb(base, post);

        Ok(BenchRow {
            target: target_label.to_string(),
            policy,
            index,
            after_trim,
            setup_ms,
            capture_ms,
            total_ms,
            duplicate,
            base_ws_mb: bytes_to_mib(base.working_set_bytes),
            base_private_mb: bytes_to_mib(base.private_bytes),
            peak_ws_delta_mb,
            peak_private_delta_mb,
            post_ws_delta_mb,
            post_private_delta_mb,
        })
    }

    fn run_policy(
        system: &CaptureSystem,
        bench_target: &BenchTarget,
        policy: SnapshotPolicy,
        config: &Config,
    ) -> Result<Vec<BenchRow>> {
        let mut rows = Vec::with_capacity(config.samples);
        match policy {
            SnapshotPolicy::MemoryFirst => {
                for index in 0..config.samples {
                    let setup_started = Instant::now();
                    let mut session =
                        system.open_session(bench_target.target.clone(), capture_options())?;
                    let setup_ms = elapsed_ms(setup_started);
                    rows.push(capture_with_session(
                        &mut session,
                        &bench_target.label,
                        policy,
                        index,
                        setup_ms,
                        true,
                    )?);
                }
            }
            SnapshotPolicy::WarmTtl => {
                let mut session =
                    system.open_session(bench_target.target.clone(), capture_options())?;
                let mut after_trim = true;
                for index in 0..config.samples {
                    if index > 0 && index % config.ttl_group_size == 0 {
                        session.release_idle_resources();
                        after_trim = true;
                    }
                    rows.push(capture_with_session(
                        &mut session,
                        &bench_target.label,
                        policy,
                        index,
                        0.0,
                        after_trim,
                    )?);
                    after_trim = false;
                }
                session.release_idle_resources();
            }
            SnapshotPolicy::AlwaysWarm => {
                let mut session =
                    system.open_session(bench_target.target.clone(), capture_options())?;
                for index in 0..config.samples {
                    rows.push(capture_with_session(
                        &mut session,
                        &bench_target.label,
                        policy,
                        index,
                        0.0,
                        index == 0,
                    )?);
                }
            }
        }
        Ok(rows)
    }

    fn csv_header() -> &'static str {
        "target,policy,index,after_trim,setup_ms,capture_ms,total_ms,duplicate,base_ws_mb,base_private_mb,peak_ws_delta_mb,peak_private_delta_mb,post_ws_delta_mb,post_private_delta_mb\n"
    }

    fn csv_row(row: &BenchRow) -> String {
        format!(
            "{},{},{},{},{:.3},{:.3},{:.3},{},{:.3},{:.3},{:.3},{:.3},{:.3},{:.3}\n",
            row.target,
            row.policy.as_str(),
            row.index,
            row.after_trim,
            row.setup_ms,
            row.capture_ms,
            row.total_ms,
            row.duplicate,
            row.base_ws_mb,
            row.base_private_mb,
            row.peak_ws_delta_mb,
            row.peak_private_delta_mb,
            row.post_ws_delta_mb,
            row.post_private_delta_mb
        )
    }

    fn write_csv(path: &PathBuf, rows: &[BenchRow]) -> Result<()> {
        if let Some(parent) = path
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
        {
            fs::create_dir_all(parent).with_context(|| {
                format!("failed to create output directory {}", parent.display())
            })?;
        }
        let mut out = String::from(csv_header());
        for row in rows {
            out.push_str(&csv_row(row));
        }
        fs::write(path, out).with_context(|| format!("failed to write {}", path.display()))
    }

    fn summarize(rows: &[BenchRow]) {
        for target in ["primary-monitor", "region-center-1920x1080"] {
            for policy in SnapshotPolicy::ALL {
                let mut samples = rows
                    .iter()
                    .filter(|row| row.target == target && row.policy.as_str() == policy.as_str())
                    .map(|row| row.total_ms)
                    .collect::<Vec<_>>();
                if samples.is_empty() {
                    continue;
                }
                samples.sort_by(|a, b| a.partial_cmp(b).unwrap());
                let avg = samples.iter().sum::<f64>() / samples.len() as f64;
                let p95 = samples[((samples.len() - 1) as f64 * 0.95).round() as usize];
                println!(
                    "{target:>24} {:>12}: samples={} avg={avg:.3}ms p95={p95:.3}ms",
                    policy.as_str(),
                    samples.len()
                );
            }
        }
    }

    pub fn main() -> Result<()> {
        let config = parse_args()?;
        let system = CaptureSystem::builder().build()?;
        let targets = vec![
            BenchTarget {
                label: "primary-monitor".to_string(),
                target: CaptureTarget::PrimaryMonitor,
            },
            BenchTarget {
                label: "region-center-1920x1080".to_string(),
                target: CaptureTarget::Region(centered_primary_region(1920, 1080)?),
            },
        ];

        let mut rows = Vec::new();
        for target in &targets {
            for policy in SnapshotPolicy::ALL {
                rows.extend(run_policy(&system, target, policy, &config)?);
            }
        }

        write_csv(&config.output, &rows)?;
        println!("Wrote {}", config.output.display());
        summarize(&rows);
        Ok(())
    }
}
#[cfg(windows)]
fn main() -> anyhow::Result<()> {
    windows_example::main()
}
#[cfg(not(windows))]
fn main() {
    eprintln!("This benchmark measures Windows snapshot retention policies.");
}

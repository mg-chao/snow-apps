use std::fs;
use std::hint::black_box;
use std::path::PathBuf;
use std::time::Instant;

use anyhow::{Context, Result, bail};
use half::f16;
use rustc_hash::FxHashMap;
use snow_capture::CapturePixelFormat;
use snow_capture::convert::{
    HdrFrameContext, SurfaceConversionOptions, SurfacePixelFormat, convert_surface_to_rgba,
};

const DEFAULT_WARMUP_ITERS: usize = 24;
const DEFAULT_MEASURE_ITERS: usize = 240;
const DEFAULT_MAX_REGRESSION_PCT: f64 = 8.0;

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
    warmup_iters: usize,
    measure_iters: usize,
    scenario_filter: Option<String>,
    baseline_path: Option<PathBuf>,
    save_baseline_path: Option<PathBuf>,
    max_regression_pct: f64,
    min_improvement_pct: Option<f64>,
    regression_metrics: Vec<RegressionMetric>,
}

#[derive(Clone, Copy, Debug)]
enum HdrPattern {
    Sdr,
    SpatialMixed,
    InterleavedMixed,
    Hdr,
}

#[derive(Clone, Copy, Debug)]
struct Scenario {
    name: &'static str,
    format: SurfacePixelFormat,
    width: usize,
    height: usize,
    src_pitch: usize,
    dst_pitch: usize,
    options: SurfaceConversionOptions,
    hdr_pattern: Option<HdrPattern>,
}

#[derive(Clone, Debug)]
struct BenchResult {
    scenario: String,
    avg_ms: f64,
    p50_ms: f64,
    p95_ms: f64,
    p99_ms: f64,
    min_ms: f64,
    max_ms: f64,
    stddev_ms: f64,
    fps: f64,
}

#[derive(Clone, Copy, Debug)]
struct BaselineEntry {
    avg_ms: f64,
    p50_ms: f64,
    p95_ms: f64,
    p99_ms: f64,
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

fn parse_args() -> Result<Config> {
    let mut warmup_iters = DEFAULT_WARMUP_ITERS;
    let mut measure_iters = DEFAULT_MEASURE_ITERS;
    let mut scenario_filter = None;
    let mut baseline_path = None;
    let mut save_baseline_path = None;
    let mut max_regression_pct = DEFAULT_MAX_REGRESSION_PCT;
    let mut min_improvement_pct = None;
    let mut regression_metrics = vec![RegressionMetric::default()];

    let args: Vec<String> = std::env::args().collect();
    let mut i = 1usize;
    while i < args.len() {
        match args[i].as_str() {
            "--warmup" => {
                warmup_iters = parse_usize_arg("--warmup", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--iters" => {
                measure_iters = parse_usize_arg("--iters", args.get(i + 1).map(String::as_str))?;
                i += 2;
            }
            "--scenario" => {
                let Some(raw) = args.get(i + 1) else {
                    bail!("--scenario requires a value (scenario name or `all`)");
                };
                let trimmed = raw.trim();
                if !trimmed.is_empty() && !trimmed.eq_ignore_ascii_case("all") {
                    scenario_filter = Some(trimmed.to_ascii_lowercase());
                } else {
                    scenario_filter = None;
                }
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
            "--min-improvement-pct" => {
                min_improvement_pct = Some(parse_f64_arg(
                    "--min-improvement-pct",
                    args.get(i + 1).map(String::as_str),
                )?);
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
            "--help" | "-h" => {
                println!(
                    "Usage: cargo run --release --example convert_benchmark -- [options]
  --warmup <n>               Warmup iterations per scenario (default: {DEFAULT_WARMUP_ITERS})
  --iters <n>                Measured iterations per scenario (default: {DEFAULT_MEASURE_ITERS})
  --scenario <name|all>      Scenario filter (default: all)
  --baseline <path>          Compare current run to baseline CSV
  --save-baseline <path>     Save current run as baseline CSV
  --max-regression-pct <f>   Allowed metric increase vs baseline (default: {DEFAULT_MAX_REGRESSION_PCT})
  --min-improvement-pct <f>  Required metric improvement for HDR scenarios vs baseline
  --regression-metric <m>    Metric for regression checks: avg | p50 | p95 | p99 (default: p50)
  --regression-metrics <csv> Metrics for regression checks, e.g. avg,p50,p95

Tip: compare batched-row-fence optimization with baseline mode:
  optimized: cargo run --release --example convert_benchmark -- --scenario bgra_row_serial_nt
  baseline (PowerShell): $env:SNOW_CAPTURE_DISABLE_BATCHED_ROW_NT_FENCE=1; cargo run --release --example convert_benchmark -- --scenario bgra_row_serial_nt
  baseline (cmd.exe):    set SNOW_CAPTURE_DISABLE_BATCHED_ROW_NT_FENCE=1 && cargo run --release --example convert_benchmark -- --scenario bgra_row_serial_nt
  baseline (bash):       SNOW_CAPTURE_DISABLE_BATCHED_ROW_NT_FENCE=1 cargo run --release --example convert_benchmark -- --scenario bgra_row_serial_nt"
                );
                std::process::exit(0);
            }
            other => {
                bail!("unknown argument: {other}");
            }
        }
    }

    if warmup_iters == 0 {
        bail!("--warmup must be >= 1");
    }
    if measure_iters == 0 {
        bail!("--iters must be >= 1");
    }
    if !max_regression_pct.is_finite() || max_regression_pct < 0.0 {
        bail!("--max-regression-pct must be a finite value >= 0");
    }
    if let Some(min_improvement_pct) = min_improvement_pct
        && (!min_improvement_pct.is_finite() || min_improvement_pct < 0.0)
    {
        bail!("--min-improvement-pct must be a finite value >= 0");
    }
    if let (Some(baseline), Some(save_baseline)) = (&baseline_path, &save_baseline_path)
        && baseline == save_baseline
    {
        bail!("--baseline and --save-baseline must point to different files");
    }

    Ok(Config {
        warmup_iters,
        measure_iters,
        scenario_filter,
        baseline_path,
        save_baseline_path,
        max_regression_pct,
        min_improvement_pct,
        regression_metrics,
    })
}

fn align_up(value: usize, align: usize) -> usize {
    if align <= 1 {
        return value;
    }
    (value + align - 1) & !(align - 1)
}

fn scenario_catalog() -> Vec<Scenario> {
    let contiguous_4k = Scenario {
        name: "bgra_contiguous_4k",
        format: SurfacePixelFormat::Bgra8,
        width: 3840,
        height: 2160,
        src_pitch: 3840 * 4,
        dst_pitch: 3840 * 4,
        options: SurfaceConversionOptions::default(),
        hdr_pattern: None,
    };

    let row_serial_width = 510usize;
    let row_serial_height = 300usize;
    let row_serial_src_pitch = align_up(row_serial_width * 4, 256);
    let row_serial = Scenario {
        name: "bgra_row_serial_nt",
        format: SurfacePixelFormat::Bgra8,
        width: row_serial_width,
        height: row_serial_height,
        src_pitch: row_serial_src_pitch,
        dst_pitch: row_serial_width * 4,
        options: SurfaceConversionOptions::default(),
        hdr_pattern: None,
    };

    let row_parallel_width = 1919usize;
    let row_parallel_height = 1079usize;
    let row_parallel_src_pitch = align_up(row_parallel_width * 4, 256);
    let row_parallel = Scenario {
        name: "bgra_row_parallel_nt",
        format: SurfacePixelFormat::Bgra8,
        width: row_parallel_width,
        height: row_parallel_height,
        src_pitch: row_parallel_src_pitch,
        dst_pitch: row_parallel_width * 4,
        options: SurfaceConversionOptions::default(),
        hdr_pattern: None,
    };

    let hdr_lut = HdrFrameContext {
        sdr_white_nits: 160.0,
        hdr_peak_nits: 1000.0,
        tonemap_use_lut: true,
        ..HdrFrameContext::default()
    };
    let hdr_precise = HdrFrameContext {
        tonemap_use_lut: false,
        ..hdr_lut
    };
    let hdr_contiguous_4k_lut = Scenario {
        name: "hdr_contiguous_4k_lut",
        format: SurfacePixelFormat::Rgba16Float,
        width: 3840,
        height: 2160,
        src_pitch: 3840 * 8,
        dst_pitch: 3840 * 4,
        options: SurfaceConversionOptions {
            hdr_to_sdr: Some(hdr_lut),
            ..SurfaceConversionOptions::default()
        },
        hdr_pattern: None,
    };
    let hdr_contiguous_4k_precise = Scenario {
        name: "hdr_contiguous_4k_precise",
        format: SurfacePixelFormat::Rgba16Float,
        width: 3840,
        height: 2160,
        src_pitch: 3840 * 8,
        dst_pitch: 3840 * 4,
        options: SurfaceConversionOptions {
            hdr_to_sdr: Some(hdr_precise),
            ..SurfaceConversionOptions::default()
        },
        hdr_pattern: None,
    };
    let hdr_pitched_4k_width = 3839usize;
    let hdr_pitched_4k_height = 2160usize;
    let hdr_pitched_4k_src_pitch = align_up(hdr_pitched_4k_width * 8, 256);
    let hdr_pitched_4k_lut = Scenario {
        name: "hdr_pitched_4k_lut",
        format: SurfacePixelFormat::Rgba16Float,
        width: hdr_pitched_4k_width,
        height: hdr_pitched_4k_height,
        src_pitch: hdr_pitched_4k_src_pitch,
        dst_pitch: hdr_pitched_4k_width * 4,
        options: SurfaceConversionOptions {
            hdr_to_sdr: Some(hdr_lut),
            ..SurfaceConversionOptions::default()
        },
        hdr_pattern: None,
    };
    let hdr_pitched_4k_precise = Scenario {
        name: "hdr_pitched_4k_precise",
        format: SurfacePixelFormat::Rgba16Float,
        width: hdr_pitched_4k_width,
        height: hdr_pitched_4k_height,
        src_pitch: hdr_pitched_4k_src_pitch,
        dst_pitch: hdr_pitched_4k_width * 4,
        options: SurfaceConversionOptions {
            hdr_to_sdr: Some(hdr_precise),
            ..SurfaceConversionOptions::default()
        },
        hdr_pattern: None,
    };
    let hdr_pitched_1080_width = 1919usize;
    let hdr_pitched_1080_height = 1079usize;
    let hdr_pitched_1080_src_pitch = align_up(hdr_pitched_1080_width * 8, 256);
    let hdr_pitched_1080_lut = Scenario {
        name: "hdr_pitched_1080p_lut",
        format: SurfacePixelFormat::Rgba16Float,
        width: hdr_pitched_1080_width,
        height: hdr_pitched_1080_height,
        src_pitch: hdr_pitched_1080_src_pitch,
        dst_pitch: hdr_pitched_1080_width * 4,
        options: SurfaceConversionOptions {
            hdr_to_sdr: Some(hdr_lut),
            ..SurfaceConversionOptions::default()
        },
        hdr_pattern: None,
    };
    let hdr_pitched_1080_precise = Scenario {
        name: "hdr_pitched_1080p_precise",
        format: SurfacePixelFormat::Rgba16Float,
        width: hdr_pitched_1080_width,
        height: hdr_pitched_1080_height,
        src_pitch: hdr_pitched_1080_src_pitch,
        dst_pitch: hdr_pitched_1080_width * 4,
        options: SurfaceConversionOptions {
            hdr_to_sdr: Some(hdr_precise),
            ..SurfaceConversionOptions::default()
        },
        hdr_pattern: None,
    };

    let mut scenarios = vec![
        contiguous_4k,
        row_serial,
        row_parallel,
        hdr_contiguous_4k_lut,
        hdr_contiguous_4k_precise,
        hdr_pitched_4k_lut,
        hdr_pitched_4k_precise,
        hdr_pitched_1080_lut,
        hdr_pitched_1080_precise,
    ];
    // These patterns distinguish uniform SDR blocks from interleaved HDR lanes.
    // BGRA is the screenshot output format; RGBA cases guard its shared kernel.
    for (pattern, names) in [
        (
            HdrPattern::Sdr,
            [
                "hdr_sdr_4k_rgba_lut",
                "hdr_sdr_4k_bgra_lut",
                "hdr_sdr_4k_rgba_precise",
                "hdr_sdr_4k_bgra_precise",
            ],
        ),
        (
            HdrPattern::SpatialMixed,
            [
                "hdr_spatial_4k_rgba_lut",
                "hdr_spatial_4k_bgra_lut",
                "hdr_spatial_4k_rgba_precise",
                "hdr_spatial_4k_bgra_precise",
            ],
        ),
        (
            HdrPattern::InterleavedMixed,
            [
                "hdr_interleaved_4k_rgba_lut",
                "hdr_interleaved_4k_bgra_lut",
                "hdr_interleaved_4k_rgba_precise",
                "hdr_interleaved_4k_bgra_precise",
            ],
        ),
        (
            HdrPattern::Hdr,
            [
                "hdr_only_4k_rgba_lut",
                "hdr_only_4k_bgra_lut",
                "hdr_only_4k_rgba_precise",
                "hdr_only_4k_bgra_precise",
            ],
        ),
    ] {
        for (index, name) in names.into_iter().enumerate() {
            scenarios.push(Scenario {
                name,
                options: SurfaceConversionOptions {
                    hdr_to_sdr: Some(if index < 2 { hdr_lut } else { hdr_precise }),
                    output_pixel_format: if index % 2 == 0 {
                        CapturePixelFormat::Rgba8
                    } else {
                        CapturePixelFormat::Bgra8
                    },
                    ..SurfaceConversionOptions::default()
                },
                hdr_pattern: Some(pattern),
                ..hdr_contiguous_4k_lut
            });
        }
    }
    scenarios
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

fn source_bytes_per_pixel(format: SurfacePixelFormat) -> usize {
    match format {
        SurfacePixelFormat::Bgra8 | SurfacePixelFormat::Rgba8 => 4usize,
        SurfacePixelFormat::Rgba16Float => 8usize,
    }
}

fn required_surface_bytes(pitch: usize, row_bytes: usize, height: usize) -> usize {
    pitch
        .checked_mul(height.saturating_sub(1))
        .and_then(|base| base.checked_add(row_bytes))
        .expect("surface byte count overflow")
}

fn fill_source_buffer_hdr_f16(scenario: Scenario) -> Vec<u8> {
    let row_bytes = scenario.width * 8;
    let len = required_surface_bytes(scenario.src_pitch, row_bytes, scenario.height);
    let mut out = vec![0u8; len];

    let mut state = 0x9e37_79b9_7f4a_7c15_u64;
    let inv_w = if scenario.width > 1 {
        1.0 / (scenario.width - 1) as f32
    } else {
        1.0
    };
    let inv_h = if scenario.height > 1 {
        1.0 / (scenario.height - 1) as f32
    } else {
        1.0
    };

    for y in 0..scenario.height {
        let row_off = y * scenario.src_pitch;
        for x in 0..scenario.width {
            state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
            let noise0 = ((state >> 40) & 0xFF) as f32 / 255.0;
            state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
            let noise1 = ((state >> 40) & 0xFF) as f32 / 255.0;
            state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
            let noise2 = ((state >> 40) & 0xFF) as f32 / 255.0;

            if let Some(pattern) = scenario.hdr_pattern {
                let t = ((x * 17 + y * 31) % 1024) as f32 / 1023.0;
                let b = ((x * 13 + y * 29) % 1024) as f32 / 1023.0;
                let hdr = match pattern {
                    HdrPattern::Sdr => false,
                    HdrPattern::SpatialMixed => x >= scenario.width * 3 / 4,
                    HdrPattern::InterleavedMixed => ((x ^ y) & 3) == 0,
                    HdrPattern::Hdr => true,
                };
                let scale = if hdr { 2.0 * (2.0 + 6.0 * t) } else { 2.0 };
                let channels = [
                    (0.05 + 0.95 * t) * scale,
                    (0.03 + 0.85 * (1.0 - t)) * scale,
                    (0.02 + 0.7 * b) * scale,
                    [0.0, 0.5, 1.0][(y * scenario.width + x) % 3],
                ];
                for (channel, value) in channels.into_iter().enumerate() {
                    let offset = row_off + x * 8 + channel * 2;
                    out[offset..offset + 2]
                        .copy_from_slice(&f16::from_f32(value).to_bits().to_le_bytes());
                }
                continue;
            }

            let tx = x as f32 * inv_w;
            let ty = y as f32 * inv_h;
            let base = 0.04 + 0.96 * tx;
            let mix = 0.6 * tx + 0.4 * ty;

            // Roughly 1/4 pixels stay in SDR-ish range after inverse boost.
            let hdr_boost = if ((x ^ y) & 0x3) == 0 {
                1.0 + 0.4 * mix
            } else {
                2.2 + 6.2 * mix.powf(1.3)
            };

            let r = (base + 0.08 * noise0) * hdr_boost;
            let g = (0.03 + 0.85 * mix + 0.07 * noise1) * (0.85 * hdr_boost + 0.2);
            let b = (0.02 + 0.75 * (1.0 - tx) + 0.09 * noise2) * (0.75 * hdr_boost + 0.25);

            let px_off = row_off + x * 8;
            let r_bits = f16::from_f32(r.max(0.0)).to_bits();
            let g_bits = f16::from_f32(g.max(0.0)).to_bits();
            let b_bits = f16::from_f32(b.max(0.0)).to_bits();
            let a_bits = f16::from_f32(1.0).to_bits();
            out[px_off..px_off + 2].copy_from_slice(&r_bits.to_le_bytes());
            out[px_off + 2..px_off + 4].copy_from_slice(&g_bits.to_le_bytes());
            out[px_off + 4..px_off + 6].copy_from_slice(&b_bits.to_le_bytes());
            out[px_off + 6..px_off + 8].copy_from_slice(&a_bits.to_le_bytes());
        }
    }

    out
}

fn fill_source_buffer(scenario: Scenario) -> Vec<u8> {
    if scenario.format == SurfacePixelFormat::Rgba16Float {
        return fill_source_buffer_hdr_f16(scenario);
    }

    let row_bytes = scenario.width * source_bytes_per_pixel(scenario.format);
    let len = required_surface_bytes(scenario.src_pitch, row_bytes, scenario.height);
    let mut out = vec![0u8; len];

    let mut state = 0x9e37_79b9_7f4a_7c15_u64;
    for byte in &mut out {
        state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
        *byte = (state >> 32) as u8;
    }

    out
}

fn run_scenario(
    scenario: Scenario,
    warmup_iters: usize,
    measure_iters: usize,
) -> Result<BenchResult> {
    let src = fill_source_buffer(scenario);
    let dst_row_bytes = scenario
        .width
        .checked_mul(4)
        .context("destination row byte count overflow")?;
    let dst_len = required_surface_bytes(scenario.dst_pitch, dst_row_bytes, scenario.height);
    let mut dst = vec![0u8; dst_len];

    for _ in 0..warmup_iters {
        let src_slice = black_box(src.as_slice());
        let dst_slice = black_box(dst.as_mut_slice());
        convert_surface_to_rgba(
            scenario.format,
            src_slice,
            scenario.src_pitch,
            dst_slice,
            scenario.dst_pitch,
            scenario.width,
            scenario.height,
            scenario.options,
        );
        black_box(&*dst_slice);
    }

    let mut samples_ms = Vec::with_capacity(measure_iters);
    for _ in 0..measure_iters {
        let src_slice = black_box(src.as_slice());
        let dst_slice = black_box(dst.as_mut_slice());
        let t0 = Instant::now();
        convert_surface_to_rgba(
            scenario.format,
            src_slice,
            scenario.src_pitch,
            dst_slice,
            scenario.dst_pitch,
            scenario.width,
            scenario.height,
            scenario.options,
        );
        black_box(&*dst_slice);
        samples_ms.push(t0.elapsed().as_secs_f64() * 1000.0);
    }

    let mut sorted = samples_ms.clone();
    sorted.sort_by(|a, b| a.total_cmp(b));
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

    Ok(BenchResult {
        scenario: scenario.name.to_string(),
        avg_ms,
        p50_ms: percentile(&sorted, 0.50),
        p95_ms: percentile(&sorted, 0.95),
        p99_ms: percentile(&sorted, 0.99),
        min_ms: *sorted.first().unwrap(),
        max_ms: *sorted.last().unwrap(),
        stddev_ms,
        fps: if avg_ms > 0.0 { 1000.0 / avg_ms } else { 0.0 },
    })
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

    let scenario_idx = column_index("scenario")
        .context("baseline header is missing required `scenario` column")?;
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
        if parts.len() <= p95_idx || parts.len() <= scenario_idx {
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

        let scenario = parts[scenario_idx].trim();
        if scenario.is_empty() {
            bail!("baseline line {line_number} has empty scenario value");
        }

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

        out.insert(
            scenario.to_string(),
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

fn save_baseline(path: &PathBuf, results: &[BenchResult]) -> Result<()> {
    let mut out =
        String::from("scenario,avg_ms,p50_ms,p95_ms,p99_ms,min_ms,max_ms,stddev_ms,fps\n");
    for result in results {
        out.push_str(&format!(
            "{},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.3}\n",
            result.scenario,
            result.avg_ms,
            result.p50_ms,
            result.p95_ms,
            result.p99_ms,
            result.min_ms,
            result.max_ms,
            result.stddev_ms,
            result.fps,
        ));
    }

    fs::write(path, out)
        .with_context(|| format!("failed to write baseline file {}", path.display()))
}

fn check_regression(
    baseline: &FxHashMap<String, BaselineEntry>,
    current: &[BenchResult],
    max_regression_pct: f64,
    metric: RegressionMetric,
) -> Result<()> {
    let mut regressions = Vec::new();

    for result in current {
        let Some(base) = baseline.get(&result.scenario) else {
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
                "{} {} regressed by {:.2}% (baseline {:.3} ms -> current {:.3} ms, limit {:.2}%)",
                result.scenario,
                metric.as_str(),
                delta_pct,
                base_value,
                current_value,
                max_regression_pct,
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

#[inline]
fn is_hdr_scenario(name: &str) -> bool {
    name.starts_with("hdr_")
}

fn check_min_improvement(
    baseline: &FxHashMap<String, BaselineEntry>,
    current: &[BenchResult],
    min_improvement_pct: f64,
    metric: RegressionMetric,
) -> Result<()> {
    let mut missing_baseline = Vec::new();
    let mut misses = Vec::new();

    for result in current {
        if !is_hdr_scenario(&result.scenario) {
            continue;
        }

        let Some(base) = baseline.get(&result.scenario) else {
            missing_baseline.push(result.scenario.clone());
            continue;
        };

        let base_value = metric.baseline_value(base);
        if base_value <= 0.0 {
            continue;
        }
        let current_value = metric.current_value(result);
        let improvement_pct = ((base_value - current_value) / base_value) * 100.0;
        if improvement_pct < min_improvement_pct {
            misses.push(format!(
                "{} {} improvement {:.2}% is below required {:.2}% (baseline {:.3} ms -> current {:.3} ms)",
                result.scenario,
                metric.as_str(),
                improvement_pct,
                min_improvement_pct,
                base_value,
                current_value,
            ));
        }
    }

    if !missing_baseline.is_empty() {
        missing_baseline.sort();
        missing_baseline.dedup();
        bail!(
            "missing baseline entries for HDR scenarios: {}",
            missing_baseline.join(",")
        );
    }

    if misses.is_empty() {
        return Ok(());
    }

    bail!("required improvement check failed:\n{}", misses.join("\n"))
}

fn print_results(results: &[BenchResult]) {
    println!(
        "{:<28} {:>12} {:>12} {:>12} {:>12} {:>12} {:>12} {:>12} {:>10}",
        "scenario", "avg_ms", "p50_ms", "p95_ms", "p99_ms", "min_ms", "max_ms", "stddev", "fps"
    );
    for result in results {
        println!(
            "{:<28} {:>12.6} {:>12.6} {:>12.6} {:>12.6} {:>12.6} {:>12.6} {:>12.6} {:>10.2}",
            result.scenario,
            result.avg_ms,
            result.p50_ms,
            result.p95_ms,
            result.p99_ms,
            result.min_ms,
            result.max_ms,
            result.stddev_ms,
            result.fps,
        );
    }
}

fn main() -> Result<()> {
    let config = parse_args()?;
    let all_scenarios = scenario_catalog();
    let available_scenarios = all_scenarios
        .iter()
        .map(|scenario| scenario.name)
        .collect::<Vec<_>>()
        .join(",");
    let scenarios: Vec<Scenario> = all_scenarios
        .into_iter()
        .filter(|scenario| {
            config
                .scenario_filter
                .as_ref()
                .is_none_or(|needle| scenario.name.eq_ignore_ascii_case(needle))
        })
        .collect();

    if scenarios.is_empty() {
        bail!(
            "no scenarios matched the requested filter; available scenarios: {}",
            available_scenarios
        );
    }

    println!(
        "Running conversion benchmark: warmup={} iters={} scenarios={} regression_metrics={} min_improvement_pct={}",
        config.warmup_iters,
        config.measure_iters,
        scenarios
            .iter()
            .map(|scenario| scenario.name)
            .collect::<Vec<_>>()
            .join(","),
        config
            .regression_metrics
            .iter()
            .map(|metric| metric.as_str())
            .collect::<Vec<_>>()
            .join(","),
        config
            .min_improvement_pct
            .map(|v| format!("{v:.2}"))
            .unwrap_or_else(|| "disabled".to_string()),
    );

    let mut results = Vec::with_capacity(scenarios.len());
    for scenario in scenarios {
        println!("Benchmarking {}...", scenario.name);
        let result = run_scenario(scenario, config.warmup_iters, config.measure_iters)?;
        results.push(result);
    }

    print_results(&results);

    if let Some(path) = &config.save_baseline_path {
        save_baseline(path, &results)?;
        println!("Saved baseline to {}", path.display());
    }

    if config.min_improvement_pct.is_some() && config.baseline_path.is_none() {
        bail!("--min-improvement-pct requires --baseline");
    }

    if let Some(path) = &config.baseline_path {
        let baseline = load_baseline(path)?;
        for metric in &config.regression_metrics {
            check_regression(&baseline, &results, config.max_regression_pct, *metric)?;
            println!(
                "Regression check passed ({}, max allowed regression: {:.2}%)",
                metric.as_str(),
                config.max_regression_pct,
            );
            if let Some(min_improvement_pct) = config.min_improvement_pct {
                check_min_improvement(&baseline, &results, min_improvement_pct, *metric)?;
                println!(
                    "Required improvement check passed ({}, min required improvement for HDR scenarios: {:.2}%)",
                    metric.as_str(),
                    min_improvement_pct,
                );
            }
        }
    }

    Ok(())
}

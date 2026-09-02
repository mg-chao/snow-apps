use std::{
    collections::BTreeMap,
    env, fs,
    path::{Path, PathBuf},
    process::ExitCode,
    time::{Duration, Instant},
};

use anyhow::{Context, Result, bail};
use serde::Serialize;
use snow_stitch_images::{
    Frame, Geometry, MotionStage, StitchDecision, StitchOptions, Stitcher, stitch_files,
};

const DEFAULT_SAMPLES: usize = 1;
const REPORT_SCHEMA_VERSION: u32 = 1;

#[derive(Debug)]
struct Config {
    samples: usize,
    output_directory: PathBuf,
    allow_debug: bool,
}

#[derive(Debug, Serialize)]
struct BenchmarkReport {
    schema_version: u32,
    dataset: DatasetDiagnostics,
    runtime: RuntimeDiagnostics,
    options: StitchOptions,
    diagnostic_run: DiagnosticRun,
    end_to_end_samples: EndToEndSamples,
}

#[derive(Debug, Serialize)]
struct DatasetDiagnostics {
    input_directory: PathBuf,
    frame_count: usize,
    compressed_bytes: u64,
}

#[derive(Debug, Serialize)]
struct RuntimeDiagnostics {
    operating_system: String,
    architecture: String,
    available_parallelism: Option<usize>,
    release_build: bool,
    timing_unit: &'static str,
    output_encoding_included: bool,
}

#[derive(Debug, Serialize)]
struct DiagnosticRun {
    wall_time_us: u64,
    finish_us: u64,
    decode: TimingDistribution,
    stitch: TimingDistribution,
    frame_total: TimingDistribution,
    by_branch: Vec<TimingGroup>,
    by_motion_stage: Vec<TimingGroup>,
    by_reference_mode: Vec<TimingGroup>,
    final_canvas: CanvasDiagnostics,
    frames: Vec<FrameDiagnostic>,
}

#[derive(Debug, Serialize)]
struct EndToEndSamples {
    sample_count: usize,
    wall_time: TimingDistribution,
    samples: Vec<EndToEndSample>,
}

#[derive(Debug, Serialize)]
struct EndToEndSample {
    sample_index: usize,
    wall_time_us: u64,
    final_canvas: CanvasDiagnostics,
}

#[derive(Debug, Serialize)]
struct FrameDiagnostic {
    input_index: usize,
    file_name: String,
    compressed_bytes: u64,
    geometry: Geometry,
    decoded_bytes: u64,
    decode_us: u64,
    stitch_us: u64,
    total_us: u64,
    decision: Option<StitchDecision>,
}

#[derive(Debug, Clone, Copy, Serialize)]
struct CanvasDiagnostics {
    geometry: Geometry,
    decoded_bytes: u64,
}

#[derive(Debug, Serialize)]
struct TimingGroup {
    label: String,
    timing: TimingDistribution,
}

#[derive(Debug, Clone, Serialize)]
struct TimingDistribution {
    count: usize,
    total_us: u64,
    mean_us: f64,
    min_us: u64,
    p50_us: u64,
    p95_us: u64,
    max_us: u64,
}

impl TimingDistribution {
    fn from_samples(samples: impl IntoIterator<Item = u64>) -> Self {
        let mut samples = samples.into_iter().collect::<Vec<_>>();
        samples.sort_unstable();
        let count = samples.len();
        let total_us = samples.iter().copied().sum::<u64>();
        let mean_us = if count == 0 {
            0.0
        } else {
            total_us as f64 / count as f64
        };
        Self {
            count,
            total_us,
            mean_us,
            min_us: samples.first().copied().unwrap_or(0),
            p50_us: percentile(&samples, 0.50),
            p95_us: percentile(&samples, 0.95),
            max_us: samples.last().copied().unwrap_or(0),
        }
    }
}

fn percentile(sorted: &[u64], quantile: f64) -> u64 {
    if sorted.is_empty() {
        return 0;
    }
    let index = (quantile * (sorted.len() - 1) as f64).ceil() as usize;
    sorted[index]
}

fn elapsed_us(start: Instant) -> u64 {
    let micros = start.elapsed().as_micros();
    u64::try_from(micros).unwrap_or(u64::MAX)
}

fn frame_bytes(geometry: Geometry) -> u64 {
    u64::from(geometry.width)
        .saturating_mul(u64::from(geometry.height))
        .saturating_mul(u64::from(geometry.pixel_format.channels()))
}

fn canvas_diagnostics(frame: &Frame) -> CanvasDiagnostics {
    let geometry = frame.geometry();
    CanvasDiagnostics {
        geometry,
        decoded_bytes: frame_bytes(geometry),
    }
}

fn usage() -> &'static str {
    "Usage: cargo run --release -p snow-stitch-images --example scroll_4_benchmark -- [OPTIONS]\n\
\n\
Options:\n\
  --samples <COUNT>    Number of production end-to-end samples after diagnostics (default: 1)\n\
  --output <PATH>      Directory for diagnostics.json and frames.csv\n\
  --allow-debug        Allow a debug build for a smoke run\n\
  -h, --help           Show this help"
}

fn parse_count(value: Option<String>, flag: &str) -> Result<usize> {
    let value = value.with_context(|| format!("{flag} requires a value"))?;
    let count = value
        .parse::<usize>()
        .with_context(|| format!("invalid value for {flag}: {value:?}"))?;
    if count == 0 {
        bail!("{flag} must be greater than zero");
    }
    Ok(count)
}

fn parse_arguments() -> Result<Config> {
    let manifest_directory = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let mut config = Config {
        samples: DEFAULT_SAMPLES,
        output_directory: manifest_directory.join("artifacts/scroll-4-benchmark"),
        allow_debug: false,
    };
    let mut arguments = env::args().skip(1);
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "-h" | "--help" => {
                println!("{}", usage());
                std::process::exit(0);
            }
            "--samples" => config.samples = parse_count(arguments.next(), "--samples")?,
            "--output" => {
                let output = arguments.next().context("--output requires a path")?;
                config.output_directory = PathBuf::from(output);
            }
            "--allow-debug" => config.allow_debug = true,
            _ if argument.starts_with("--samples=") => {
                config.samples =
                    parse_count(Some(argument["--samples=".len()..].to_owned()), "--samples")?;
            }
            _ if argument.starts_with("--output=") => {
                config.output_directory = PathBuf::from(&argument["--output=".len()..]);
            }
            _ => bail!("unknown argument {argument:?}\n\n{}", usage()),
        }
    }
    if cfg!(debug_assertions) && !config.allow_debug {
        bail!(
            "scroll-4 benchmark must be run with --release; pass --allow-debug only for a smoke run"
        );
    }
    Ok(config)
}

fn input_paths() -> Result<Vec<PathBuf>> {
    let directory = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("test-imgs/scroll-4");
    let mut paths = fs::read_dir(&directory)
        .with_context(|| {
            format!(
                "could not read benchmark input directory {}",
                directory.display()
            )
        })?
        .collect::<std::io::Result<Vec<_>>>()
        .with_context(|| {
            format!(
                "could not enumerate benchmark input directory {}",
                directory.display()
            )
        })?
        .into_iter()
        .filter_map(|entry| {
            let path = entry.path();
            let is_png = path
                .extension()
                .and_then(|extension| extension.to_str())
                .is_some_and(|extension| extension.eq_ignore_ascii_case("png"));
            is_png.then_some(path)
        })
        .collect::<Vec<_>>();
    paths.sort_by(|left, right| left.file_name().cmp(&right.file_name()));
    if paths.len() < 2 {
        bail!(
            "benchmark dataset must contain at least two PNG frames: {}",
            directory.display()
        );
    }
    Ok(paths)
}

fn dataset_diagnostics(paths: &[PathBuf]) -> Result<DatasetDiagnostics> {
    let compressed_bytes = paths.iter().try_fold(0_u64, |total, path| {
        let length = fs::metadata(path)
            .with_context(|| format!("could not read input metadata for {}", path.display()))?
            .len();
        Ok::<_, anyhow::Error>(total.saturating_add(length))
    })?;
    Ok(DatasetDiagnostics {
        input_directory: paths
            .first()
            .and_then(|path| path.parent())
            .context("benchmark input path unexpectedly has no parent")?
            .to_path_buf(),
        frame_count: paths.len(),
        compressed_bytes,
    })
}

fn group_timings<F>(frames: &[FrameDiagnostic], label: F) -> Vec<TimingGroup>
where
    F: Fn(&FrameDiagnostic) -> String,
{
    let mut groups = BTreeMap::<String, Vec<u64>>::new();
    for frame in frames {
        groups
            .entry(label(frame))
            .or_default()
            .push(frame.stitch_us);
    }
    groups
        .into_iter()
        .map(|(label, values)| TimingGroup {
            label,
            timing: TimingDistribution::from_samples(values),
        })
        .collect()
}

fn branch_label(frame: &FrameDiagnostic) -> String {
    frame
        .decision
        .as_ref()
        .map(|decision| format!("{:?}", decision.branch))
        .unwrap_or_else(|| "Initial".to_owned())
}

fn motion_stage_label(frame: &FrameDiagnostic) -> String {
    frame
        .decision
        .as_ref()
        .and_then(|decision| decision.motion_diagnostics.as_ref())
        .map(|diagnostics| match diagnostics.stage {
            MotionStage::InputTooSmall => "InputTooSmall".to_owned(),
            MotionStage::IdenticalInterior => "IdenticalInterior".to_owned(),
            MotionStage::EmptyDescriptors => "EmptyDescriptors".to_owned(),
            MotionStage::NoMatches => "NoMatches".to_owned(),
            MotionStage::NoCandidates => "NoCandidates".to_owned(),
            MotionStage::SelectedNoMotion => "SelectedNoMotion".to_owned(),
            MotionStage::LowConfidence => "LowConfidence".to_owned(),
            MotionStage::SceneCut => "SceneCut".to_owned(),
            MotionStage::Selected => "Selected".to_owned(),
        })
        .unwrap_or_else(|| "None".to_owned())
}

fn reference_mode_label(frame: &FrameDiagnostic) -> String {
    frame
        .decision
        .as_ref()
        .map(|decision| format!("{:?}", decision.reference_mode))
        .unwrap_or_else(|| "Initial".to_owned())
}

fn run_diagnostics(paths: &[PathBuf]) -> Result<DiagnosticRun> {
    let run_started = Instant::now();
    let options = StitchOptions {
        record_decisions: true,
        ..StitchOptions::default()
    };
    let mut stitcher = Stitcher::new(options)?;
    let mut frames = Vec::with_capacity(paths.len());

    for (input_index, path) in paths.iter().enumerate() {
        let compressed_bytes = fs::metadata(path)
            .with_context(|| format!("could not read input metadata for {}", path.display()))?
            .len();
        let frame_started = Instant::now();
        let decode_started = Instant::now();
        let frame = Frame::decode(path)
            .with_context(|| format!("could not decode benchmark input {}", path.display()))?;
        let decode_us = elapsed_us(decode_started);
        let geometry = frame.geometry();
        let decoded_bytes = frame_bytes(geometry);
        let stitch_started = Instant::now();
        let decision = stitcher
            .push(frame)
            .with_context(|| format!("could not stitch benchmark input {}", path.display()))?;
        let stitch_us = elapsed_us(stitch_started);
        let file_name = path
            .file_name()
            .and_then(|name| name.to_str())
            .context("benchmark input file name is not valid UTF-8")?
            .to_owned();
        frames.push(FrameDiagnostic {
            input_index,
            file_name,
            compressed_bytes,
            geometry,
            decoded_bytes,
            decode_us,
            stitch_us,
            total_us: elapsed_us(frame_started),
            decision,
        });
    }

    let finish_started = Instant::now();
    let result = stitcher.finish()?;
    let finish_us = elapsed_us(finish_started);
    if result.decisions.len() != frames.len().saturating_sub(1) {
        bail!(
            "decision log contains {} records for {} input frames",
            result.decisions.len(),
            frames.len()
        );
    }
    let final_canvas = canvas_diagnostics(&result.image);
    let decode = TimingDistribution::from_samples(frames.iter().map(|frame| frame.decode_us));
    let stitch = TimingDistribution::from_samples(frames.iter().map(|frame| frame.stitch_us));
    let frame_total = TimingDistribution::from_samples(frames.iter().map(|frame| frame.total_us));

    Ok(DiagnosticRun {
        wall_time_us: elapsed_us(run_started),
        finish_us,
        decode,
        stitch,
        frame_total,
        by_branch: group_timings(&frames, branch_label),
        by_motion_stage: group_timings(&frames, motion_stage_label),
        by_reference_mode: group_timings(&frames, reference_mode_label),
        final_canvas,
        frames,
    })
}

fn run_end_to_end_samples(paths: &[PathBuf], sample_count: usize) -> Result<EndToEndSamples> {
    let mut samples = Vec::with_capacity(sample_count);
    for sample_index in 0..sample_count {
        let started = Instant::now();
        let result = stitch_files(paths, StitchOptions::default())
            .with_context(|| format!("end-to-end sample {} failed", sample_index + 1))?;
        samples.push(EndToEndSample {
            sample_index,
            wall_time_us: elapsed_us(started),
            final_canvas: canvas_diagnostics(&result.image),
        });
    }
    Ok(EndToEndSamples {
        sample_count,
        wall_time: TimingDistribution::from_samples(
            samples.iter().map(|sample| sample.wall_time_us),
        ),
        samples,
    })
}

fn write_csv(path: &Path, frames: &[FrameDiagnostic]) -> Result<()> {
    let mut output = String::from(
        "input_index,file_name,compressed_bytes,width,height,pixel_format,decoded_bytes,decode_us,stitch_us,total_us,branch,motion_stage,confidence,accepted_offset,growth,canvas_height,reference_mode,reference_keypoints,incoming_keypoints,mutual_matches,candidate_count\n",
    );
    for frame in frames {
        let decision = frame.decision.as_ref();
        let branch = decision
            .map(|event| format!("{:?}", event.branch))
            .unwrap_or_else(|| "Initial".to_owned());
        let motion_stage = decision
            .and_then(|event| event.motion_diagnostics.as_ref())
            .map(|diagnostics| format!("{:?}", diagnostics.stage))
            .unwrap_or_else(|| "None".to_owned());
        let confidence = decision
            .and_then(|event| event.confidence)
            .map(|value| value.to_string())
            .unwrap_or_default();
        let accepted_offset = decision
            .and_then(|event| event.accepted_offset)
            .map(|value| value.to_string())
            .unwrap_or_default();
        let growth = decision.map(|event| event.growth).unwrap_or(0);
        let canvas_height = decision
            .map(|event| event.after.canvas_height)
            .unwrap_or(frame.geometry.height);
        let reference_mode = decision
            .map(|event| format!("{:?}", event.reference_mode))
            .unwrap_or_else(|| "Initial".to_owned());
        let diagnostics = decision.and_then(|event| event.motion_diagnostics.as_ref());
        let reference_keypoints = diagnostics
            .map(|diagnostics| diagnostics.reference_keypoints)
            .unwrap_or(0);
        let incoming_keypoints = diagnostics
            .map(|diagnostics| diagnostics.incoming_keypoints)
            .unwrap_or(0);
        let mutual_matches = diagnostics
            .map(|diagnostics| diagnostics.mutual_matches)
            .unwrap_or(0);
        let candidate_count = diagnostics
            .map(|diagnostics| diagnostics.candidates.len())
            .unwrap_or(0);
        output.push_str(&format!(
            "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
            frame.input_index,
            frame.file_name,
            frame.compressed_bytes,
            frame.geometry.width,
            frame.geometry.height,
            frame.geometry.pixel_format,
            frame.decoded_bytes,
            frame.decode_us,
            frame.stitch_us,
            frame.total_us,
            branch,
            motion_stage,
            confidence,
            accepted_offset,
            growth,
            canvas_height,
            reference_mode,
            reference_keypoints,
            incoming_keypoints,
            mutual_matches,
            candidate_count,
        ));
    }
    fs::write(path, output)
        .with_context(|| format!("could not write CSV report {}", path.display()))
}

fn write_report(config: &Config, report: &BenchmarkReport) -> Result<()> {
    fs::create_dir_all(&config.output_directory).with_context(|| {
        format!(
            "could not create benchmark output directory {}",
            config.output_directory.display()
        )
    })?;
    let json_path = config.output_directory.join("diagnostics.json");
    let json =
        serde_json::to_vec_pretty(report).context("could not serialize benchmark diagnostics")?;
    fs::write(&json_path, json)
        .with_context(|| format!("could not write JSON report {}", json_path.display()))?;
    let csv_path = config.output_directory.join("frames.csv");
    write_csv(&csv_path, &report.diagnostic_run.frames)?;
    println!("diagnostics: {}", json_path.display());
    println!("per-frame CSV: {}", csv_path.display());
    Ok(())
}

fn format_duration(duration_us: u64) -> String {
    let duration = Duration::from_micros(duration_us);
    format!("{:.3}s", duration.as_secs_f64())
}

fn run() -> Result<()> {
    let config = parse_arguments()?;
    let paths = input_paths()?;
    let dataset = dataset_diagnostics(&paths)?;
    println!(
        "scroll-4: {} frames, {:.2} MiB compressed",
        dataset.frame_count,
        dataset.compressed_bytes as f64 / (1024.0 * 1024.0)
    );

    let diagnostic_run = run_diagnostics(&paths)?;
    let end_to_end_samples = run_end_to_end_samples(&paths, config.samples)?;
    let report = BenchmarkReport {
        schema_version: REPORT_SCHEMA_VERSION,
        dataset,
        runtime: RuntimeDiagnostics {
            operating_system: env::consts::OS.to_owned(),
            architecture: env::consts::ARCH.to_owned(),
            available_parallelism: std::thread::available_parallelism().ok().map(usize::from),
            release_build: !cfg!(debug_assertions),
            timing_unit: "microseconds",
            output_encoding_included: false,
        },
        options: StitchOptions::default(),
        diagnostic_run,
        end_to_end_samples,
    };
    write_report(&config, &report)?;
    println!(
        "diagnostic run: {}, per-frame stitch p50/p95: {} / {}",
        format_duration(report.diagnostic_run.wall_time_us),
        format_duration(report.diagnostic_run.stitch.p50_us),
        format_duration(report.diagnostic_run.stitch.p95_us),
    );
    println!(
        "production end-to-end samples: {}, p50/p95: {} / {}",
        report.end_to_end_samples.sample_count,
        format_duration(report.end_to_end_samples.wall_time.p50_us),
        format_duration(report.end_to_end_samples.wall_time.p95_us),
    );
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: {error:#}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timing_distribution_reports_percentiles() {
        let distribution = TimingDistribution::from_samples([7, 1, 5, 3, 9]);
        assert_eq!(distribution.count, 5);
        assert_eq!(distribution.total_us, 25);
        assert_eq!(distribution.min_us, 1);
        assert_eq!(distribution.p50_us, 5);
        assert_eq!(distribution.p95_us, 9);
        assert_eq!(distribution.max_us, 9);
    }

    #[test]
    fn frame_byte_count_uses_pixel_format_channels() {
        let geometry = Geometry {
            width: 3,
            height: 2,
            pixel_format: snow_stitch_images::PixelFormat::Rgba8,
        };
        assert_eq!(frame_bytes(geometry), 24);
    }
}

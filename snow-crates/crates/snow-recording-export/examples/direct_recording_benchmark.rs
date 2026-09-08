use std::fs;
use std::path::{Path, PathBuf};
use std::time::Instant;

use anyhow::{Context, Result, bail};
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingAudioConfig,
    StreamingEncoder, StreamingEncoderConfig, VideoCodec,
};
use snow_recording_model::{VideoEncodeConfig, VideoEncodingSpeed};
use windows::Win32::System::ProcessStatus::{
    K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS, PROCESS_MEMORY_COUNTERS_EX,
};
use windows::Win32::System::Threading::GetCurrentProcess;

const DEFAULT_DURATION_SECONDS: u64 = 60;
const DEFAULT_SAMPLES: usize = 5;
const STREAMING_PENDING_DEPTH: usize = 1;
const MAX_WORKING_SET_DELTA_MIB: f64 = 768.0;
const MP4_FINALIZATION_P95_LIMIT_MS: f64 = 2_000.0;
const AUDIO_SAMPLE_RATE: u32 = 48_000;
const AUDIO_CHANNELS: u16 = 2;
const AUDIO_PACKET_MS: u64 = 10;
const MIB: f64 = 1024.0 * 1024.0;

#[derive(Clone, Debug)]
struct Config {
    duration_seconds: u64,
    samples: usize,
    output_directory: PathBuf,
    allow_debug: bool,
}

#[derive(Clone, Copy, Debug)]
struct Scenario {
    name: &'static str,
    format: ExportFormat,
    width: u32,
    height: u32,
    fps: u32,
    audio: bool,
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

    fn update_max(&mut self, sample: Self) {
        self.working_set_bytes = self.working_set_bytes.max(sample.working_set_bytes);
        self.private_bytes = self.private_bytes.max(sample.private_bytes);
    }
}

#[derive(Clone, Debug)]
struct BenchRow {
    scenario: &'static str,
    sample: usize,
    duration_seconds: u64,
    width: u32,
    height: u32,
    fps: u32,
    expected_frames: u64,
    encoded_frames: u64,
    coalesced_frames: u64,
    dropped_frames: u64,
    encoded_audio_frames: u64,
    inserted_silence_frames: u64,
    maximum_queue_depth: usize,
    working_set_delta_mib: f64,
    private_delta_mib: f64,
    initialize_ms: f64,
    encode_ms: f64,
    finalization_ms: f64,
    post_stop_decode_reencode_ms: f64,
    output_bytes: u64,
    video_encoder: String,
}

fn parse_u64(flag: &str, value: Option<&str>) -> Result<u64> {
    let Some(value) = value else {
        bail!("{flag} requires a value");
    };
    value
        .parse::<u64>()
        .with_context(|| format!("failed to parse {flag} value: {value}"))
}

fn parse_usize(flag: &str, value: Option<&str>) -> Result<usize> {
    let Some(value) = value else {
        bail!("{flag} requires a value");
    };
    value
        .parse::<usize>()
        .with_context(|| format!("failed to parse {flag} value: {value}"))
}

fn print_usage() {
    println!(
        "Usage: cargo run --release -p snow-recording-export --example \
direct_recording_benchmark -- [options]\n\
  --duration-seconds <n>  fixture duration (default: {DEFAULT_DURATION_SECONDS})\n\
  --samples <n>           samples per format (default: {DEFAULT_SAMPLES})\n\
  --output <directory>    fixture/report directory \
(default: target/perf/direct-recording)\n\
  --allow-debug           allow a short Debug smoke run"
    );
}

fn parse_args() -> Result<Config> {
    let mut config = Config {
        duration_seconds: DEFAULT_DURATION_SECONDS,
        samples: DEFAULT_SAMPLES,
        output_directory: PathBuf::from("target/perf/direct-recording"),
        allow_debug: false,
    };
    let args = std::env::args().collect::<Vec<_>>();
    let mut index = 1usize;
    while index < args.len() {
        match args[index].as_str() {
            "--help" | "-h" => {
                print_usage();
                std::process::exit(0);
            }
            "--duration-seconds" => {
                config.duration_seconds = parse_u64(
                    "--duration-seconds",
                    args.get(index + 1).map(String::as_str),
                )?;
                index += 2;
            }
            "--samples" => {
                config.samples = parse_usize("--samples", args.get(index + 1).map(String::as_str))?;
                index += 2;
            }
            "--output" => {
                let Some(value) = args.get(index + 1) else {
                    bail!("--output requires a directory");
                };
                config.output_directory = PathBuf::from(value);
                index += 2;
            }
            "--allow-debug" => {
                config.allow_debug = true;
                index += 1;
            }
            other => bail!("unknown argument: {other}. Use --help for usage."),
        }
    }
    if config.duration_seconds == 0 {
        bail!("--duration-seconds must be greater than zero");
    }
    if config.samples == 0 {
        bail!("--samples must be greater than zero");
    }
    if cfg!(debug_assertions) && !config.allow_debug {
        bail!(
            "direct-recording performance benchmarks must use Release; pass --allow-debug only \
             for a short smoke run"
        );
    }
    Ok(config)
}

fn render_frame(rgba: &mut [u8], width: u32, height: u32, frame_index: u64) {
    let phase = frame_index as u8;
    for (index, pixel) in rgba.chunks_exact_mut(4).enumerate() {
        let x = index as u32 % width;
        let y = index as u32 / width;
        pixel.copy_from_slice(&[
            (x as u8).wrapping_add(phase),
            (y as u8).wrapping_add(phase.wrapping_mul(3)),
            ((x ^ y) as u8).wrapping_add(phase.wrapping_mul(7)),
            255,
        ]);
    }
    let marker_x = (frame_index as u32 * 13) % width;
    let marker_y = (frame_index as u32 * 7) % height;
    for y in marker_y.saturating_sub(12)..(marker_y + 12).min(height) {
        for x in marker_x.saturating_sub(12)..(marker_x + 12).min(width) {
            let offset = (y as usize * width as usize + x as usize) * 4;
            rgba[offset..offset + 4].copy_from_slice(&[255, 32, 32, 255]);
        }
    }
}

fn elapsed_ms(started: Instant) -> f64 {
    started.elapsed().as_secs_f64() * 1_000.0
}

fn delta_mib(high: u64, low: u64) -> f64 {
    high.saturating_sub(low) as f64 / MIB
}

fn timestamp_ms(frame_index: u64, fps: u32) -> u64 {
    (u128::from(frame_index) * 1_000 / u128::from(fps)).min(u128::from(u64::MAX)) as u64
}

fn run_sample(config: &Config, scenario: Scenario, sample: usize) -> Result<BenchRow> {
    fs::create_dir_all(&config.output_directory).with_context(|| {
        format!(
            "failed to create benchmark output directory {}",
            config.output_directory.display()
        )
    })?;
    let output_path = config.output_directory.join(format!(
        "{}-sample-{}.{}",
        scenario.name,
        sample + 1,
        scenario.format.file_extension()
    ));
    if output_path.is_file() {
        fs::remove_file(&output_path)
            .with_context(|| format!("failed to replace {}", output_path.display()))?;
    }

    let memory_start = ProcessMemorySample::capture()?;
    let mut memory_peak = memory_start;
    let initialize_started = Instant::now();
    let mut encoder = StreamingEncoder::create(StreamingEncoderConfig {
        output_path: output_path.clone(),
        format: scenario.format,
        width: scenario.width,
        height: scenario.height,
        fps: scenario.fps,
        codec: VideoCodec::H264,
        prefer_hardware_h264: false,
        execution_mode: ExportExecutionMode::SoftwareOnly,
        software_h264_priority: SoftwareH264Priority::X264First,
        video: VideoEncodeConfig {
            quality: 80,
            speed: VideoEncodingSpeed::VeryFast,
        },
        encode_threads: 0,
        audio: scenario.audio.then_some(StreamingAudioConfig {
            sample_rate_hz: AUDIO_SAMPLE_RATE,
            channels: AUDIO_CHANNELS,
            bitrate_kbps: 160,
        }),
    })?;
    let initialize_ms = elapsed_ms(initialize_started);

    let expected_frames = config
        .duration_seconds
        .checked_mul(u64::from(scenario.fps))
        .context("benchmark frame count overflowed")?;
    let mut rgba = vec![0u8; scenario.width as usize * scenario.height as usize * 4];
    let silent_audio = vec![
        0i16;
        (AUDIO_SAMPLE_RATE as u64 * AUDIO_PACKET_MS / 1_000) as usize
            * usize::from(AUDIO_CHANNELS)
    ];
    let mut next_audio_ms = 0u64;
    let encode_started = Instant::now();
    for frame_index in 0..expected_frames {
        let timestamp_ms = timestamp_ms(frame_index, scenario.fps);
        if scenario.audio {
            while next_audio_ms <= timestamp_ms {
                encoder.push_audio_pcm_i16(next_audio_ms, &silent_audio)?;
                next_audio_ms = next_audio_ms.saturating_add(AUDIO_PACKET_MS);
            }
        }
        render_frame(&mut rgba, scenario.width, scenario.height, frame_index);
        encoder.push_rgba_frame(timestamp_ms, &rgba)?;
        if frame_index % u64::from(scenario.fps) == 0 {
            memory_peak.update_max(ProcessMemorySample::capture()?);
        }
    }
    if scenario.audio {
        let duration_ms = config.duration_seconds.saturating_mul(1_000);
        while next_audio_ms < duration_ms {
            encoder.push_audio_pcm_i16(next_audio_ms, &silent_audio)?;
            next_audio_ms = next_audio_ms.saturating_add(AUDIO_PACKET_MS);
        }
    }
    let encode_ms = elapsed_ms(encode_started);

    let finalization_started = Instant::now();
    let report = encoder.finish()?;
    let finalization_ms = elapsed_ms(finalization_started);
    memory_peak.update_max(ProcessMemorySample::capture()?);
    let output_bytes = fs::metadata(&output_path)
        .with_context(|| format!("failed to stat {}", output_path.display()))?
        .len();
    let row = BenchRow {
        scenario: scenario.name,
        sample,
        duration_seconds: config.duration_seconds,
        width: scenario.width,
        height: scenario.height,
        fps: scenario.fps,
        expected_frames,
        encoded_frames: report.encoded_frames,
        coalesced_frames: report.coalesced_frames,
        dropped_frames: 0,
        encoded_audio_frames: report.encoded_audio_frames,
        inserted_silence_frames: report.inserted_silence_frames,
        maximum_queue_depth: STREAMING_PENDING_DEPTH,
        working_set_delta_mib: delta_mib(
            memory_peak.working_set_bytes,
            memory_start.working_set_bytes,
        ),
        private_delta_mib: delta_mib(memory_peak.private_bytes, memory_start.private_bytes),
        initialize_ms,
        encode_ms,
        finalization_ms,
        post_stop_decode_reencode_ms: 0.0,
        output_bytes,
        video_encoder: report.video_encoder,
    };
    validate_row(&row, scenario.audio)?;
    Ok(row)
}

fn validate_row(row: &BenchRow, expect_audio: bool) -> Result<()> {
    if row.encoded_frames != row.expected_frames {
        bail!(
            "{} sample {} encoded {} of {} expected frames",
            row.scenario,
            row.sample + 1,
            row.encoded_frames,
            row.expected_frames
        );
    }
    if row.coalesced_frames != 0 || row.dropped_frames != 0 {
        bail!(
            "{} sample {} unexpectedly coalesced {} or dropped {} frames",
            row.scenario,
            row.sample + 1,
            row.coalesced_frames,
            row.dropped_frames
        );
    }
    if row.maximum_queue_depth > STREAMING_PENDING_DEPTH {
        bail!("streaming pending-frame depth exceeded its fixed bound");
    }
    if row.working_set_delta_mib > MAX_WORKING_SET_DELTA_MIB {
        bail!(
            "{} sample {} exceeded the {:.0} MiB working-set budget: {:.1} MiB",
            row.scenario,
            row.sample + 1,
            MAX_WORKING_SET_DELTA_MIB,
            row.working_set_delta_mib
        );
    }
    if expect_audio && row.encoded_audio_frames == 0 {
        bail!("MP4 benchmark did not encode the configured AAC audio track");
    }
    if !expect_audio && row.encoded_audio_frames != 0 {
        bail!("animated benchmark unexpectedly encoded audio");
    }
    if row.post_stop_decode_reencode_ms != 0.0 {
        bail!("direct finalization performed a post-stop decode/re-encode stage");
    }
    Ok(())
}

fn csv_header() -> &'static str {
    "scenario,sample,duration_seconds,width,height,fps,expected_frames,encoded_frames,\
coalesced_frames,dropped_frames,encoded_audio_frames,inserted_silence_frames,\
maximum_queue_depth,working_set_delta_mib,private_delta_mib,initialize_ms,encode_ms,\
finalization_ms,post_stop_decode_reencode_ms,output_bytes,video_encoder\n"
}

fn csv_row(row: &BenchRow) -> String {
    format!(
        "{},{},{},{},{},{},{},{},{},{},{},{},{},{:.3},{:.3},{:.3},{:.3},{:.3},\
{:.3},{},{}\n",
        row.scenario,
        row.sample + 1,
        row.duration_seconds,
        row.width,
        row.height,
        row.fps,
        row.expected_frames,
        row.encoded_frames,
        row.coalesced_frames,
        row.dropped_frames,
        row.encoded_audio_frames,
        row.inserted_silence_frames,
        row.maximum_queue_depth,
        row.working_set_delta_mib,
        row.private_delta_mib,
        row.initialize_ms,
        row.encode_ms,
        row.finalization_ms,
        row.post_stop_decode_reencode_ms,
        row.output_bytes,
        row.video_encoder,
    )
}

fn percentile(values: &mut [f64], percentile: f64) -> f64 {
    values.sort_by(|left, right| left.total_cmp(right));
    values[((values.len() - 1) as f64 * percentile).round() as usize]
}

fn write_report(output_directory: &Path, rows: &[BenchRow]) -> Result<PathBuf> {
    let path = output_directory.join("direct-recording-benchmark.csv");
    let mut csv = String::from(csv_header());
    for row in rows {
        csv.push_str(&csv_row(row));
    }
    fs::write(&path, csv).with_context(|| format!("failed to write {}", path.display()))?;
    Ok(path)
}

fn main() -> Result<()> {
    let config = parse_args()?;
    let scenarios = [
        Scenario {
            name: "mp4-1080p30",
            format: ExportFormat::Mp4,
            width: 1_920,
            height: 1_080,
            fps: 30,
            audio: true,
        },
        Scenario {
            name: "webp-720p10",
            format: ExportFormat::Webp,
            width: 1_280,
            height: 720,
            fps: 10,
            audio: false,
        },
    ];
    let mut rows = Vec::with_capacity(config.samples * scenarios.len());
    for scenario in scenarios {
        for sample in 0..config.samples {
            let row = run_sample(&config, scenario, sample)?;
            println!(
                "{} sample {}: encoded={}/{} coalesced={} dropped={} queue_max={} \
                 working_set_delta={:.1}MiB encode={:.1}ms finalize={:.1}ms output={} bytes",
                row.scenario,
                row.sample + 1,
                row.encoded_frames,
                row.expected_frames,
                row.coalesced_frames,
                row.dropped_frames,
                row.maximum_queue_depth,
                row.working_set_delta_mib,
                row.encode_ms,
                row.finalization_ms,
                row.output_bytes,
            );
            rows.push(row);
        }
    }

    let report_path = write_report(&config.output_directory, &rows)?;
    let mut mp4_finalization = rows
        .iter()
        .filter(|row| row.scenario == "mp4-1080p30")
        .map(|row| row.finalization_ms)
        .collect::<Vec<_>>();
    let mp4_p95_ms = percentile(&mut mp4_finalization, 0.95);
    println!("MP4 finalization p95: {mp4_p95_ms:.1}ms");
    println!("Wrote {}", report_path.display());
    if config.duration_seconds >= DEFAULT_DURATION_SECONDS
        && mp4_p95_ms >= MP4_FINALIZATION_P95_LIMIT_MS
    {
        bail!(
            "60-second 1080p30 MP4 finalization p95 was {:.1}ms; expected below {:.1}ms",
            mp4_p95_ms,
            MP4_FINALIZATION_P95_LIMIT_MS
        );
    }
    Ok(())
}

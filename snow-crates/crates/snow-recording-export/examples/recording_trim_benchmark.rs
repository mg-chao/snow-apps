//! Run only from the windows-msvc-performance preset (Release).
use anyhow::{Result, ensure};
use snow_core::cancellation::CancellationToken;
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoder,
    StreamingEncoderConfig,
    clip::{ClipSource, VideoReader},
    playback::Playback,
};
use snow_recording_model::{VideoCodec, VideoEncodeConfig, VideoEncodingSpeed};
use std::path::Path;
use std::time::{Duration, Instant};

#[cfg(windows)]
fn memory_mib() -> f64 {
    use windows::Win32::System::{ProcessStatus::*, Threading::GetCurrentProcess};
    let mut counters = PROCESS_MEMORY_COUNTERS_EX::default();
    unsafe {
        let _ = K32GetProcessMemoryInfo(
            GetCurrentProcess(),
            (&mut counters as *mut PROCESS_MEMORY_COUNTERS_EX).cast(),
            std::mem::size_of_val(&counters) as u32,
        );
    }
    counters.PrivateUsage as f64 / (1024.0 * 1024.0)
}
#[cfg(not(windows))]
fn memory_mib() -> f64 {
    0.0
}

fn scenario(
    directory: &Path,
    name: &str,
    size: (u32, u32),
    seconds: u64,
    stride: u64,
) -> Result<()> {
    let config = StreamingEncoderConfig {
        output_path: directory.join(format!("{name}.mp4")),
        format: ExportFormat::Mp4,
        width: size.0,
        height: size.1,
        fps: 60,
        codec: VideoCodec::H264,
        prefer_hardware_h264: false,
        execution_mode: ExportExecutionMode::SoftwareOnly,
        software_h264_priority: SoftwareH264Priority::X264First,
        video: VideoEncodeConfig {
            quality: 80,
            speed: VideoEncodingSpeed::UltraFast,
        },
        encode_threads: 4,
        audio: Vec::new(),
        loop_animated_images: false,
    };
    let mut pixels = vec![0; size.0 as usize * size.1 as usize * 4];
    if !config.output_path.exists() {
        let mut encoder = StreamingEncoder::create(config.clone())?;
        for frame in (0..seconds * 60).step_by(stride as usize) {
            for (index, pixel) in pixels.chunks_exact_mut(4).enumerate() {
                let x = index as u32 % size.0;
                let y = index as u32 / size.0;
                pixel.copy_from_slice(&[
                    40 + ((x / 16 + frame as u32) % 160) as u8,
                    40 + ((y / 16) % 160) as u8,
                    100,
                    255,
                ]);
            }
            encoder.push_rgba_frame_at_pts(frame, &pixels)?;
        }
        encoder.finish_at_duration_ms(seconds * 1000)?;
    }
    let base = memory_mib();
    let start = Instant::now();
    let source = ClipSource::open(&config.output_path, Some(config.clone()))?;
    let open_ms = start.elapsed().as_secs_f64() * 1000.0;
    let token = CancellationToken::default();
    let at = source.boundary((seconds * 60).saturating_sub(90));
    let mut software = VideoReader::open(source.clone(), false)?;
    software.read(at, &token)?;
    let mut reference = Vec::new();
    software.copy_rgba(&mut reference)?;
    drop(software);
    let start = Instant::now();
    let mut video = VideoReader::open(source.clone(), true)?;
    video.read(at, &token)?;
    video.copy_rgba(&mut pixels)?;
    let first_ms = start.elapsed().as_secs_f64() * 1000.0;
    let error = reference
        .iter()
        .zip(&pixels)
        .map(|(a, b)| f64::from(a.abs_diff(*b)))
        .sum::<f64>()
        / pixels.len() as f64;
    ensure!(
        error < 5.0,
        "hardware fallback changes pixels: mean error {error}"
    );
    let mut seeks = Vec::new();
    let mut peak = memory_mib();
    for index in [0, 3, 1, 4, 2, 5, 3, 0, 4, 5] {
        let start = Instant::now();
        video.read(source.boundary(source.frame_count() * index / 6), &token)?;
        video.copy_rgba(&mut pixels)?;
        seeks.push(start.elapsed().as_secs_f64() * 1000.0);
        peak = peak.max(memory_mib());
    }
    seeks.sort_by(f64::total_cmp);
    drop(video);
    let playback = Playback::open(source.clone())?;
    let revision = playback.seek(at, (at + 1_000_000).min(source.duration_us), true);
    let start = Instant::now();
    let mut polls = 0;
    loop {
        let snapshot = playback.snapshot();
        ensure!(snapshot.error.is_empty(), "{}", snapshot.error);
        peak = peak.max(memory_mib());
        polls += 1;
        if snapshot.revision == revision && !snapshot.playing {
            break;
        }
        ensure!(
            start.elapsed() < Duration::from_secs(15),
            "playback stalled"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
    let playback_ms = start.elapsed().as_secs_f64() * 1000.0;
    drop(playback);
    let output = directory.join(format!("{name}-trim.mp4"));
    if output.exists() {
        std::fs::remove_file(&output)?;
    }
    let start = Instant::now();
    source.export(
        source.range(source.frame_at(at), source.frame_at(at) + 60)?,
        &output,
        &token,
        |_| {},
    )?;
    let export_ms = start.elapsed().as_secs_f64() * 1000.0;
    ensure!(
        peak - base < 768.0,
        "unbounded preview buffering: {} MiB",
        peak - base
    );
    println!(
        "{name},{open_ms:.2},{first_ms:.2},{:.2},{:.2},{playback_ms:.2},{polls},{export_ms:.2},{:.2},{error:.3}",
        seeks[5],
        seeks[9],
        peak - base
    );
    Ok(())
}
fn main() -> Result<()> {
    ensure!(
        !cfg!(debug_assertions),
        "Use windows-msvc-performance, never Debug"
    );
    let directory = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "recording-trim-performance".into());
    let directory = Path::new(&directory);
    std::fs::create_dir_all(directory)?;
    println!(
        "scenario,open_ms,first_frame_ms,seek_p50_ms,seek_max_ms,playback_ms,polls,export_ms,peak_delta_mib,hardware_pixel_error"
    );
    scenario(directory, "1080p60", (1920, 1080), 10, 1)?;
    scenario(directory, "4k60", (3840, 2160), 3, 1)?;
    scenario(directory, "long-30min", (1920, 1080), 1800, 60)?;
    Ok(())
}

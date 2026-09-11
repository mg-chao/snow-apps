//! Live capture -> streaming export benchmark. Requires `stage-timing` and a
//! Release build. This intentionally uses the same public stream and encoder
//! APIs as the application recording path.
use anyhow::{Context, Result, bail};
use snow_capture::backend::CaptureBackendKind;
use snow_capture::{
    CaptureEvent, CaptureOptions, CaptureStream, CaptureStreamConfig, CaptureSystem, CaptureTarget,
    CaptureWorkload,
};
use snow_recording_export::config::{VideoEncodeConfig, VideoEncodingSpeed};
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoder,
    StreamingEncoderConfig, VideoCodec,
};
use std::{
    fs,
    path::PathBuf,
    time::{Duration, Instant},
};

fn main() -> Result<()> {
    let mut backend = CaptureBackendKind::WindowsGraphicsCapture;
    let mut seconds = 10u64;
    let mut fps = 60u32;
    let mut warmup = 30usize;
    let mut output = PathBuf::from("target/perf/recording-export.mp4");
    let mut hardware = true;
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--backend" => {
                i += 1;
                backend = parse_backend(args.get(i))?;
            }
            "--seconds" => {
                i += 1;
                seconds = parse_u64("--seconds", args.get(i))?;
            }
            "--fps" => {
                i += 1;
                fps = parse_u64("--fps", args.get(i))?
                    .try_into()
                    .context("--fps exceeds u32")?;
            }
            "--warmup" => {
                i += 1;
                warmup = parse_u64("--warmup", args.get(i))?
                    .try_into()
                    .context("--warmup exceeds usize")?;
            }
            "--output" => {
                i += 1;
                output = PathBuf::from(args.get(i).context("--output requires a path")?);
            }
            "--software" => hardware = false,
            "--help" | "-h" => {
                println!(
                    "--backend dxgi|wgc|gdi --seconds N --fps N --warmup N --output PATH [--software]"
                );
                return Ok(());
            }
            other => bail!("unknown argument: {other}"),
        }
        i += 1;
    }
    if seconds == 0 || fps == 0 {
        bail!("seconds and fps must be greater than zero");
    }
    if cfg!(debug_assertions) {
        bail!("performance benchmarks require Release");
    }
    fs::create_dir_all(output.parent().unwrap_or(std::path::Path::new(".")))?;

    let started = Instant::now();
    let system = CaptureSystem::builder()
        .with_backend_kind(backend)
        .build()?;
    let session = system.open_session(
        CaptureTarget::PrimaryMonitor,
        CaptureOptions {
            workload: CaptureWorkload::Continuous,
            record_stage_timings: true,
            ..Default::default()
        },
    )?;
    let stream = CaptureStream::spawn(
        session,
        CaptureStreamConfig {
            target_fps: fps,
            buffer_depth: 6,
            include_cursor: true,
            adaptive_fps: false,
            pause_on_resolution_change: false,
            ..Default::default()
        },
    )?;
    let mut encoder: Option<StreamingEncoder> = None;
    let mut frames = 0u64;
    let mut duplicates = 0u64;
    let mut dropped = 0u64;
    let mut errors = 0u64;
    let mut capture_ms = Vec::new();
    let mut push_ms = Vec::new();
    let mut measured_start = None;
    let mut encoder_setup_ms = 0.0;
    let mut dimensions = (0, 0);
    let mut queue_peak = 0;
    let mut initial_drops = 0;
    let mut deadline = Instant::now() + Duration::from_secs(30);
    let mut warmed = 0usize;
    loop {
        if Instant::now() >= deadline {
            break;
        }
        match stream.recv_timeout(Duration::from_secs(2)) {
            Ok(CaptureEvent::Frame(frame)) => {
                if warmed < warmup {
                    warmed += 1;
                    continue;
                }
                if encoder.is_none() {
                    dimensions = frame.dimensions();
                    let cfg = StreamingEncoderConfig {
                        output_path: output.clone(),
                        format: ExportFormat::Mp4,
                        width: frame.width(),
                        height: frame.height(),
                        fps,
                        codec: VideoCodec::H264,
                        prefer_hardware_h264: hardware,
                        execution_mode: if hardware {
                            ExportExecutionMode::HardwarePreferred
                        } else {
                            ExportExecutionMode::SoftwareOnly
                        },
                        software_h264_priority: SoftwareH264Priority::X264First,
                        video: VideoEncodeConfig {
                            quality: 80,
                            speed: VideoEncodingSpeed::VeryFast,
                        },
                        encode_threads: 0,
                        audio: None,
                    };
                    let encoder_started = Instant::now();
                    encoder = Some(StreamingEncoder::create(cfg)?);
                    encoder_setup_ms = encoder_started.elapsed().as_secs_f64() * 1000.0;
                    let now = Instant::now();
                    measured_start = Some(now);
                    deadline = now + Duration::from_secs(seconds);
                    initial_drops = stream.stats().snapshot().frames_dropped;
                    println!("setup_ms={:.2}", started.elapsed().as_secs_f64() * 1000.0);
                }
                let measured_start = measured_start.context("measurement did not start")?;
                let captured_at = frame
                    .metadata()
                    .stream_timestamp()
                    .context("missing capture timestamp")?
                    .instant;
                if captured_at < measured_start {
                    continue;
                }
                if frame.metadata().is_duplicate() {
                    duplicates += 1;
                }
                capture_ms.push(
                    frame
                        .metadata()
                        .capture_duration()
                        .context("missing capture duration")?
                        .as_secs_f64()
                        * 1000.0,
                );
                let push_started = Instant::now();
                encoder.as_mut().unwrap().push_rgba_frame(
                    captured_at.duration_since(measured_start).as_millis() as u64,
                    frame.as_rgba_bytes(),
                )?;
                push_ms.push(push_started.elapsed().as_secs_f64() * 1000.0);
                queue_peak = queue_peak.max(stream.stats().snapshot().buffer_fill);
                frames += 1;
                if Instant::now() >= deadline {
                    break;
                }
            }
            Ok(CaptureEvent::FramesDropped { count }) => dropped += u64::from(count),
            Ok(CaptureEvent::Error(error)) => {
                errors += 1;
                eprintln!("capture error: {error}");
            }
            Ok(CaptureEvent::StreamEnded) => break,
            Ok(_) => {}
            Err(_) if Instant::now() >= deadline => break,
            Err(_) => {}
        }
    }
    let measured_seconds = measured_start
        .context("no frames captured")?
        .elapsed()
        .as_secs_f64();
    let stats = stream.stats().snapshot();
    drop(stream);
    let finish_started = Instant::now();
    let report = encoder.context("no frames captured")?.finish()?;
    let finish_ms = finish_started.elapsed().as_secs_f64() * 1000.0;
    println!("video_stage_totals={:?}", report.video_stage_timings);
    let timings = &report.video_stage_timings;
    fs::write(
        output.with_extension("stages.csv"),
        format!(
            "encoded_frames,copy_ms,convert_ms,send_ms,drain_and_mux_ms\n{},{:.3},{:.3},{:.3},{:.3}\n",
            report.encoded_frames,
            timings.copy.as_secs_f64() * 1000.0,
            timings.convert.as_secs_f64() * 1000.0,
            timings.send.as_secs_f64() * 1000.0,
            timings.drain_and_mux.as_secs_f64() * 1000.0
        ),
    )?;
    let avg = if capture_ms.is_empty() {
        0.0
    } else {
        capture_ms.iter().sum::<f64>() / capture_ms.len() as f64
    };
    let size = fs::metadata(&output).map(|m| m.len()).unwrap_or(0);
    let report_path = output.with_extension("csv");
    fs::write(
        &report_path,
        format!(
            "backend,width,height,frames,measured_seconds,target_fps,duplicates,stream_dropped,errors,avg_capture_ms,effective_fps,push_p50_ms,push_p95_ms,queue_peak,encoder_setup_ms,finish_ms,encoded_frames,coalesced_frames,output_bytes,encoder\n{},{},{},{},{:.6},{},{},{},{},{:.3},{:.3},{:.3},{:.3},{},{:.3},{:.3},{},{},{},{}\n",
            backend.as_str(),
            dimensions.0,
            dimensions.1,
            frames,
            measured_seconds,
            fps,
            duplicates,
            stats.frames_dropped.saturating_sub(initial_drops),
            errors,
            avg,
            frames as f64 / measured_seconds,
            percentile(&push_ms, 0.5),
            percentile(&push_ms, 0.95),
            queue_peak,
            encoder_setup_ms,
            finish_ms,
            report.encoded_frames,
            report.coalesced_frames,
            size,
            report.video_encoder
        ),
    )?;
    println!(
        "frames={frames} duplicates={duplicates} dropped_events={dropped} errors={errors} avg_capture_ms={avg:.3} effective_fps={:.2} queue_fill={} stream_dropped={} stream_errors={} output_bytes={} encoder={} total_ms={:.2}",
        frames as f64 / measured_seconds,
        stats.buffer_fill,
        stats.frames_dropped.saturating_sub(initial_drops),
        stats.errors_recovered,
        size,
        report.video_encoder,
        started.elapsed().as_secs_f64() * 1000.0
    );
    println!(
        "push_p50_ms={:.3} push_p95_ms={:.3} finish_ms={finish_ms:.3}",
        percentile(&push_ms, 0.5),
        percentile(&push_ms, 0.95)
    );
    println!(
        "capture_p50_ms={:.3} capture_p95_ms={:.3} encoded_frames={} coalesced_frames={}",
        percentile(&capture_ms, 0.5),
        percentile(&capture_ms, 0.95),
        report.encoded_frames,
        report.coalesced_frames
    );
    if frames == 0 || errors != 0 {
        bail!("invalid benchmark run: frames={frames}, errors={errors}");
    }
    Ok(())
}

fn parse_u64(flag: &str, value: Option<&String>) -> Result<u64> {
    Ok(value.context(format!("{flag} requires a value"))?.parse()?)
}
fn parse_backend(value: Option<&String>) -> Result<CaptureBackendKind> {
    match value
        .context("--backend requires a value")?
        .to_ascii_lowercase()
        .as_str()
    {
        "dxgi" => Ok(CaptureBackendKind::DxgiDuplication),
        "wgc" => Ok(CaptureBackendKind::WindowsGraphicsCapture),
        "gdi" => Ok(CaptureBackendKind::Gdi),
        v => bail!("unknown backend: {v}"),
    }
}
fn percentile(values: &[f64], p: f64) -> f64 {
    if values.is_empty() {
        return 0.0;
    }
    let mut v = values.to_vec();
    v.sort_by(f64::total_cmp);
    v[((v.len() - 1) as f64 * p).round() as usize]
}

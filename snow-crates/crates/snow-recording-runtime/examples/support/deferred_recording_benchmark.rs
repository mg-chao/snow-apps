//! End-to-end clean capture, source disk, deterministic replay and final encode.
//! Run Release with the performance preset; `bench-pipeline-timing` enables stages.
//! The workload is the same no-focus fullscreen window as the live benchmark.
#[path = "realtime_recording_benchmark.rs"]
#[allow(dead_code)]
mod live_workload;

use anyhow::{Context, Result, bail};
use snow_recording_export::{ExportFormat, VideoCodec};
use snow_recording_model::{PlaybackOverlay, VideoEncodingSpeed};
use snow_recording_runtime::{
    CaptureBackendKind, DeferredRecordingOptions, DeferredRecordingSession, DeferredRenderState,
    DirectRecordingConfig, KeyboardOverlayConfig, RecordingAudioMode, RecordingRegion,
};
use std::{
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicU8, AtomicU64, Ordering},
    },
    time::{Duration, Instant},
};
use windows::Win32::System::{
    ProcessStatus::{K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS, PROCESS_MEMORY_COUNTERS_EX},
    Threading::{GetCurrentProcess, GetProcessTimes},
};

pub(crate) fn run() -> Result<()> {
    live_workload::initialize_process_dpi()?;
    let _dpi = live_workload::ThreadDpiAwareness::per_monitor_v2();
    if std::env::args().nth(1).as_deref() == Some("--inspect-file") {
        return inspect_media(Path::new(
            &std::env::args().nth(2).context("missing inspection path")?,
        ));
    }
    let mut seconds = 5u64;
    let mut fps = 30u32;
    let mut size = (1920, 1080);
    let mut workload = "moving".to_string();
    let mut audio = "none".to_string();
    let mut overlay = "bar".to_string();
    let mut hardware = false;
    let mut quality = false;
    let mut effects = false;
    let mut encode_threads = None;
    let mut keep_source = false;
    let mut backend = CaptureBackendKind::Gdi;
    let mut directory = PathBuf::from("build/deferred-recording-perf");
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let value = |args: &mut std::iter::Skip<std::env::Args>| {
            args.next().context("missing option value")
        };
        match arg.as_str() {
            "--duration-seconds" => seconds = value(&mut args)?.parse()?,
            "--fps" => fps = value(&mut args)?.parse()?,
            "--region-size" => {
                let v = value(&mut args)?;
                let (w, h) = v.split_once('x').context("size must be WIDTHxHEIGHT")?;
                size = (w.parse()?, h.parse()?);
            }
            "--workload" => workload = value(&mut args)?,
            "--audio" => audio = value(&mut args)?,
            "--overlay" => overlay = value(&mut args)?,
            "--output-directory" => directory = PathBuf::from(value(&mut args)?),
            "--hardware" => hardware = true,
            "--all-effects" => effects = true,
            "--quality" => quality = true,
            "--encode-threads" => encode_threads = Some(value(&mut args)?.parse::<u8>()?),
            "--keep-source" => keep_source = true,
            "--backend" => {
                backend = match value(&mut args)?.as_str() {
                    "gdi" => CaptureBackendKind::Gdi,
                    "wgc" => CaptureBackendKind::WindowsGraphicsCapture,
                    "dxgi" => CaptureBackendKind::DxgiDuplication,
                    "auto" => CaptureBackendKind::Auto,
                    _ => bail!("unknown backend"),
                }
            }
            _ => bail!("unknown option {arg}"),
        }
    }
    if seconds == 0 || fps == 0 {
        bail!("duration and FPS must be positive");
    }
    let capture = snow_capture::CaptureSystem::builder().build()?;
    let layout = capture.monitor_layout()?;
    let monitor = layout
        .monitors
        .iter()
        .min_by_key(|m| (m.x, m.y))
        .context("no monitors")?;
    if size.0 > monitor.width || size.1 > monitor.height || size.0 < 528 || size.1 < 96 {
        bail!(
            "workload must fit the leftmost monitor ({}x{} at {},{}) and be at least 528x96",
            monitor.width,
            monitor.height,
            monitor.x,
            monitor.y
        );
    }
    let region = RecordingRegion::new(monitor.x, monitor.y, size.0, size.1);
    let window = match workload.as_str() {
        "desktop" => None,
        "static" | "moving" | "sparse" => Some(live_workload::WorkloadWindow::spawn(
            region,
            if workload == "static" { 0 } else { 15 },
            workload == "sparse",
        )?),
        _ => bail!("workload must be static, moving, sparse, or desktop"),
    };
    if let Some(window) = &window {
        window.publish_once()?;
    }
    std::thread::sleep(Duration::from_millis(200));
    std::fs::create_dir_all(&directory)?;
    let output = directory.join(format!("deferred-{}.mp4", uuid::Uuid::new_v4()));
    let playback_overlay = match overlay.as_str() {
        "none" => PlaybackOverlay::None,
        "bar" => PlaybackOverlay::ProgressBar {
            rgba: [255, 85, 0, 255],
        },
        "time" => PlaybackOverlay::PlaybackTime { rgba: [255; 4] },
        _ => bail!("overlay must be none, bar, or time"),
    };
    let (system, microphone, mode) = match audio.as_str() {
        "none" => (false, false, RecordingAudioMode::Mixed),
        "system" => (true, false, RecordingAudioMode::Mixed),
        "mixed" => (true, true, RecordingAudioMode::Mixed),
        "separate" => (true, true, RecordingAudioMode::Separate),
        _ => bail!("audio must be none, system, mixed, or separate"),
    };
    let config = DirectRecordingConfig {
        loop_animated_images: false,
        region,
        capture_backend: backend,
        output_path: output.clone(),
        format: ExportFormat::Mp4,
        capture_fps: fps,
        output_fps: fps,
        maximum_width: None,
        maximum_height: None,
        codec: VideoCodec::H264,
        preset: VideoEncodingSpeed::VeryFast,
        quality: 80,
        prefer_hardware_encoder: hardware,
        enable_system_audio: system,
        enable_microphone: microphone,
        audio_mode: mode,
        show_cursor: effects,
        keyboard: effects.then(|| KeyboardOverlayConfig {
            font: None,
            keycap_size: 64,
            background_rgba: [0, 0, 0, 204],
            text_rgba: [255; 4],
            border_rgba: [255, 255, 255, 128],
            labels: Default::default(),
        }),
        show_keyboard: effects,
        record_mouse_clicks: effects,
        mouse_trail_rgba: if effects { [255, 85, 0, 255] } else { [0; 4] },
        mouse_trail_duration_ms: 500,
        mouse_click_rgba: if effects { [255, 0, 0, 255] } else { [0; 4] },
        mouse_highlight_rgba: [0; 4],
        excluded_windows: Default::default(),
        excluded_processes: Default::default(),
    };
    let peak = Arc::new(AtomicU64::new(0));
    let render_peak = Arc::new(AtomicU64::new(0));
    let sampling_phase = Arc::new(AtomicU8::new(1));
    let sampling = Arc::new(AtomicBool::new(true));
    let sampler = std::thread::spawn({
        let peak = Arc::clone(&peak);
        let render_peak = Arc::clone(&render_peak);
        let phase = Arc::clone(&sampling_phase);
        let sampling = Arc::clone(&sampling);
        move || {
            while sampling.load(Ordering::Acquire) {
                match phase.load(Ordering::Acquire) {
                    1 => {
                        peak.fetch_max(memory_bytes(), Ordering::Relaxed);
                    }
                    2 => {
                        render_peak.fetch_max(memory_bytes(), Ordering::Relaxed);
                    }
                    _ => {}
                }
                std::thread::sleep(Duration::from_millis(25));
            }
        }
    });
    let initial_cpu = cpu_ticks();
    let setup = Instant::now();
    let mut session = DeferredRecordingSession::create(
        config,
        DeferredRecordingOptions {
            working_directory: Some(directory),
            playback_overlay,
        },
    )?;
    session.start()?;
    let startup_ms = setup.elapsed().as_millis();
    let recording = Instant::now();
    std::thread::sleep(Duration::from_secs(seconds));
    session.request_stop()?;
    let capture_ms = recording.elapsed().as_millis();
    let finalize = Instant::now();
    let source = session.stop()?;
    let finalize_ms = finalize.elapsed().as_millis();
    let capture_cpu = cpu_ticks().saturating_sub(initial_cpu);
    sampling_phase.store(0, Ordering::Release);
    if effects {
        augment_inputs(source.path(), size, fps, source.duration_ms())?;
    }
    let source_bytes = std::fs::metadata(source.path())?.len();
    if keep_source {
        let preserved = output.parent().unwrap().join("clean-source.snowrec");
        std::fs::copy(source.path(), &preserved)?;
        eprintln!("preserved clean source: {}", preserved.display());
    }
    let source_report = source.capture_report().clone();
    let reference = if quality {
        let reference = tempfile::NamedTempFile::new()?;
        std::fs::write(reference.path(), first_rgba(source.path())?)?;
        Some(reference)
    } else {
        None
    };
    // A benchmark-only final-encoder override. Capture, saved settings and
    // production auto-thread policy remain identical to the default run.
    let render_override = encode_threads
        .map(|threads| -> Result<_> {
            let mut snapshot = snow_recording_export::read_deferred_output_settings(source.path())?;
            snapshot.encode_threads = threads;
            Ok(snapshot)
        })
        .transpose()?;
    let render = Instant::now();
    sampling_phase.store(2, Ordering::Release);
    let render_cpu = cpu_ticks();
    let task = if let Some(snapshot) = render_override {
        snow_recording_export::DeferredRenderTask::start(
            source.path().to_path_buf(),
            snapshot,
            Arc::new(AtomicBool::new(false)),
        )?
    } else {
        source.render_async()?
    };
    let total = loop {
        let snapshot = task.snapshot();
        if snapshot.state != DeferredRenderState::Running {
            break snapshot.total_frames;
        }
        std::thread::sleep(Duration::from_millis(10));
    };
    let result = task.wait()?;
    let render_ms = render.elapsed().as_millis();
    let render_cpu = cpu_ticks().saturating_sub(render_cpu);
    sampling_phase.store(0, Ordering::Release);
    sampling.store(false, Ordering::Release);
    sampler
        .join()
        .map_err(|_| anyhow::anyhow!("memory sampler panicked"))?;
    let psnr = reference
        .map(|reference| -> Result<f64> {
            Ok(psnr(
                &std::fs::read(reference.path())?,
                &first_rgba(&output)?,
                size,
            ))
        })
        .transpose()?;
    let stages = &result.runtime_report.stage_durations_ms;
    println!(
        "workload,width,height,fps,audio,overlay,source_encoder,source_hardware,source_fallback,source_bytes,startup_ms,capture_ms,source_finalize_ms,capture_cpu_ms,render_ms,render_cpu_ms,render_fps,capture_peak_private_bytes,render_peak_private_bytes,plan_ms,decode_ms,compose_ms,video_encode_ms,audio_encode_ms,finalize_ms,output_bytes,output_encoder,output_hardware,source_reencode_psnr_db"
    );
    println!(
        "{workload},{},{},{fps},{audio},{overlay},{},{},{},{source_bytes},{startup_ms},{capture_ms},{finalize_ms},{:.3},{render_ms},{:.3},{:.3},{},{},{},{},{},{},{},{},{},{},{},{}",
        size.0,
        size.1,
        source_report.video_encoder,
        source_report.used_hardware_video_encoder,
        source_report.hardware_fallback,
        capture_cpu as f64 / 10000.0,
        render_cpu as f64 / 10000.0,
        total as f64 / (render_ms as f64 / 1000.0).max(0.0001),
        peak.load(Ordering::Relaxed),
        render_peak.load(Ordering::Relaxed),
        stages.plan,
        stages.decode,
        stages.compose,
        stages.video_encode,
        stages.audio_encode,
        stages.finalize,
        std::fs::metadata(&output)?.len(),
        result
            .runtime_report
            .video_encoder
            .as_deref()
            .unwrap_or("unknown"),
        result.runtime_report.used_hardware_encode,
        psnr.map(|v| format!("{v:.3}")).unwrap_or_default()
    );
    eprintln!("output: {}", output.display());
    if let Some(threads) = encode_threads {
        eprintln!("benchmark final encoder thread override: {threads}");
    }
    if quality && effects {
        eprintln!(
            "PSNR also includes intended cursor and effect differences; use a clean run for codec quality."
        );
    }
    Ok(())
}

/// Decode complete outputs after quiet measurements to check dimensions,
/// frame count, container endpoint and actual audio routing independently.
fn inspect_media(path: &Path) -> Result<()> {
    use ffmpeg_next as ffmpeg;
    ffmpeg::init()?;
    let mut input = ffmpeg::format::input(path)?;
    let stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .context("missing video")?;
    let video_index = stream.index();
    let mut video = ffmpeg::codec::context::Context::from_parameters(stream.parameters())?
        .decoder()
        .video()?;
    println!(
        "video,{},{},{},{}x{},fps={}/{},duration_ms={:.3}",
        path.display(),
        video_index,
        stream.parameters().id().name(),
        video.width(),
        video.height(),
        stream.avg_frame_rate().numerator(),
        stream.avg_frame_rate().denominator(),
        stream.duration() as f64 * f64::from(stream.time_base().numerator()) * 1000.0
            / f64::from(stream.time_base().denominator())
    );
    let mut audio = Vec::new();
    for stream in input.streams() {
        if stream.parameters().medium() != ffmpeg::media::Type::Audio {
            continue;
        }
        let decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())?
            .decoder()
            .audio();
        // SAFETY: The stream owns these codec parameters throughout inspection.
        let (rate, channels) = unsafe {
            let parameters = stream.parameters();
            (
                (*parameters.as_ptr()).sample_rate,
                (*parameters.as_ptr()).ch_layout.nb_channels,
            )
        };
        let metadata = stream.metadata();
        println!(
            "audio,{},codec={},title={:?},default={},rate={},channels={},duration_ms={:.3}",
            stream.index(),
            stream.parameters().id().name(),
            metadata
                .get("title")
                .or_else(|| metadata.get("handler_name"))
                .unwrap_or(""),
            stream
                .disposition()
                .contains(ffmpeg::format::stream::Disposition::DEFAULT),
            rate,
            channels,
            stream.duration() as f64 * f64::from(stream.time_base().numerator()) * 1000.0
                / f64::from(stream.time_base().denominator())
        );
        match decoder {
            Ok(decoder) => audio.push((stream.index(), decoder, 0usize)),
            Err(ffmpeg::Error::DecoderNotFound) => {
                eprintln!(
                    "audio decoder omitted by the production FFmpeg profile; use recording_audio_inspect for independent AAC decoding"
                );
            }
            Err(error) => return Err(error.into()),
        }
    }
    let mut decoded = ffmpeg::frame::Video::empty();
    let mut decoded_audio = ffmpeg::frame::Audio::empty();
    let mut frames = 0;
    let drain_video = |decoder: &mut ffmpeg::decoder::Video,
                       frame: &mut ffmpeg::frame::Video,
                       count: &mut u64|
     -> Result<()> {
        loop {
            match decoder.receive_frame(frame) {
                Ok(()) => {
                    if (frame.width(), frame.height()) != (decoder.width(), decoder.height()) {
                        bail!("decoded output dimensions changed");
                    }
                    *count += 1;
                }
                Err(ffmpeg::Error::Eof) => return Ok(()),
                Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {
                    return Ok(());
                }
                Err(error) => return Err(error.into()),
            }
        }
    };
    let drain_audio = |decoder: &mut ffmpeg::decoder::Audio,
                       frame: &mut ffmpeg::frame::Audio,
                       count: &mut usize|
     -> Result<()> {
        loop {
            match decoder.receive_frame(frame) {
                Ok(()) => *count += frame.samples(),
                Err(ffmpeg::Error::Eof) => return Ok(()),
                Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {
                    return Ok(());
                }
                Err(error) => return Err(error.into()),
            }
        }
    };
    for (stream, packet) in input.packets() {
        if stream.index() == video_index {
            video.send_packet(&packet)?;
            drain_video(&mut video, &mut decoded, &mut frames)?;
        } else if let Some((_, decoder, count)) = audio
            .iter_mut()
            .find(|(index, _, _)| *index == stream.index())
        {
            decoder.send_packet(&packet)?;
            drain_audio(decoder, &mut decoded_audio, count)?;
        }
    }
    video.send_eof()?;
    drain_video(&mut video, &mut decoded, &mut frames)?;
    println!("decoded_video_frames,{frames}");
    for (index, mut decoder, mut count) in audio {
        decoder.send_eof()?;
        drain_audio(&mut decoder, &mut decoded_audio, &mut count)?;
        println!("decoded_audio_frames,{index},{count}");
    }
    Ok(())
}
fn memory_bytes() -> u64 {
    let mut counters = PROCESS_MEMORY_COUNTERS_EX::default();
    let result = unsafe {
        K32GetProcessMemoryInfo(
            GetCurrentProcess(),
            &mut counters as *mut _ as *mut PROCESS_MEMORY_COUNTERS,
            std::mem::size_of_val(&counters) as u32,
        )
    };
    if result.as_bool() {
        counters.PrivateUsage as u64
    } else {
        0
    }
}
fn cpu_ticks() -> u64 {
    let (mut creation, mut exit, mut kernel, mut user) = (
        Default::default(),
        Default::default(),
        Default::default(),
        Default::default(),
    );
    if unsafe {
        GetProcessTimes(
            GetCurrentProcess(),
            &mut creation,
            &mut exit,
            &mut kernel,
            &mut user,
        )
    }
    .is_err()
    {
        return 0;
    }
    let ticks = |v: windows::Win32::Foundation::FILETIME| {
        (u64::from(v.dwHighDateTime) << 32) | u64::from(v.dwLowDateTime)
    };
    ticks(kernel) + ticks(user)
}
fn first_rgba(path: &Path) -> Result<Vec<u8>> {
    use ffmpeg_next as ffmpeg;
    let mut input = ffmpeg::format::input(path)?;
    let stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .context("missing video")?;
    let index = stream.index();
    let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())?
        .decoder()
        .video()?;
    let mut decoded = ffmpeg::frame::Video::empty();
    for (stream, packet) in input.packets() {
        if stream.index() != index {
            continue;
        }
        decoder.send_packet(&packet)?;
        if decoder.receive_frame(&mut decoded).is_ok() {
            break;
        }
    }
    if decoded.width() == 0 {
        decoder.send_eof()?;
        decoder.receive_frame(&mut decoded)?;
    }
    let mut conversion = ffmpeg::software::scaling::Context::get(
        decoded.format(),
        decoded.width(),
        decoded.height(),
        ffmpeg::format::Pixel::RGBA,
        decoded.width(),
        decoded.height(),
        ffmpeg::software::scaling::Flags::BICUBIC,
    )?;
    let mut rgba = ffmpeg::frame::Video::empty();
    conversion.run(&decoded, &mut rgba)?;
    let mut bytes = Vec::new();
    for row in 0..rgba.height() as usize {
        bytes.extend_from_slice(
            &rgba.data(0)[row * rgba.stride(0)..row * rgba.stride(0) + rgba.width() as usize * 4],
        );
    }
    Ok(bytes)
}
fn psnr(reference: &[u8], encoded: &[u8], size: (u32, u32)) -> f64 {
    let bytes = (size.0 as usize * size.1.saturating_sub(64) as usize * 4)
        .min(reference.len())
        .min(encoded.len());
    let (mut error, mut count) = (0f64, 0u64);
    for (a, b) in reference[..bytes]
        .chunks_exact(4)
        .zip(encoded[..bytes].chunks_exact(4))
    {
        for c in 0..3 {
            error += (f64::from(a[c]) - f64::from(b[c])).powi(2);
            count += 1;
        }
    }
    if error == 0.0 {
        f64::INFINITY
    } else {
        10.0 * (255.0 * 255.0 / (error / count.max(1) as f64)).log10()
    }
}

/// Deterministic effect input, with no OS input injection. Preparation is
/// outside measured capture/render stages and copies payloads in bounded chunks.
fn augment_inputs(path: &Path, size: (u32, u32), fps: u32, duration: u64) -> Result<()> {
    use snow_recording_model::{
        BundleAssetKind, CursorFrameRecord, CursorShapeCompositionMode, CursorShapeRecord,
        InputMouseButton, InputStoreWriter, KeyEventRecord, RecordedInput, RecordedInputEvent,
        RecordedMouseClick, RecordingBundleAsset,
    };
    use std::io::{Read, Seek, SeekFrom};
    let footer = snow_recording_model::read_recording_bundle_footer(path)?;
    let directory = tempfile::tempdir_in(path.parent().context("source parent missing")?)?;
    let timeline =
        snow_recording_model::FinalizedTimeline::new(duration, fps).map_err(anyhow::Error::msg)?;
    let input = directory.path().join("input.bin");
    let mut writer = InputStoreWriter::new(&input)?;
    let mut sequence = 0;
    let mut push = |timestamp_ms, event| -> Result<()> {
        writer.push(&RecordedInputEvent {
            timestamp_ms,
            sequence,
            event,
        })?;
        sequence += 1;
        Ok(())
    };
    push(
        0,
        RecordedInput::CursorShape(CursorShapeRecord {
            shape_id: 1,
            hotspot_x: 1,
            hotspot_y: 1,
            width: 16,
            height: 16,
            mode: CursorShapeCompositionMode::AlphaBlend,
            shape_rgba: [255, 255, 255, 220].repeat(256),
        }),
    )?;
    for frame in timeline.iter() {
        let (x, y) = (
            (size.0 / 4 + ((frame.index * 17) % (u64::from(size.0) / 2)) as u32) as i32,
            (size.1 / 4 + ((frame.index * 11) % (u64::from(size.1) / 2)) as u32) as i32,
        );
        push(
            frame.timestamp_ms,
            RecordedInput::Cursor(CursorFrameRecord {
                timestamp_ms: frame.timestamp_ms,
                x,
                y,
                visible: true,
                shape_id: Some(1),
            }),
        )?;
        push(
            frame.timestamp_ms,
            RecordedInput::Pointer {
                position: Some((x, y)),
                continuity: frame.index / 120,
                at_ms: frame.timestamp_ms,
            },
        )?;
        if frame.index % 30 == 0 || frame.index % 30 == 3 {
            let down = frame.index % 30 == 0;
            let button = match (frame.index / 30) % 5 {
                0 => InputMouseButton::Left,
                1 => InputMouseButton::Right,
                2 => InputMouseButton::Middle,
                3 => InputMouseButton::Button4,
                _ => InputMouseButton::Button5,
            };
            let (key, label) = match button {
                InputMouseButton::Left => (1, "MouseLeft"),
                InputMouseButton::Right => (2, "MouseRight"),
                InputMouseButton::Middle => (4, "MouseMiddle"),
                InputMouseButton::Button4 => (5, "Mouse4"),
                InputMouseButton::Button5 => (6, "Mouse5"),
            };
            push(
                frame.timestamp_ms,
                RecordedInput::Click(RecordedMouseClick {
                    x,
                    y,
                    button,
                    down,
                    key: KeyEventRecord {
                        at_ms: frame.timestamp_ms,
                        key,
                        down,
                        label: label.into(),
                        modifiers: vec![(17, "Ctrl".into()), (16, "Shift".into())],
                    },
                }),
            )?;
        }
        if frame.index % 15 == 0 || frame.index % 15 == 3 {
            let key = 65 + (frame.index / 15 % 26) as u16;
            push(
                frame.timestamp_ms,
                RecordedInput::Key(KeyEventRecord {
                    at_ms: frame.timestamp_ms,
                    key,
                    down: frame.index % 15 == 0,
                    label: char::from_u32(u32::from(key)).unwrap().to_string(),
                    modifiers: vec![(17, "Ctrl".into()), (16, "Shift".into())],
                }),
            )?;
        }
    }
    writer.finish()?;
    let staging = directory.path().join("source.mp4");
    let mut original = std::fs::File::open(path)?;
    std::io::copy(
        &mut (&mut original).take(footer.video_payload_len),
        &mut std::fs::File::create(&staging)?,
    )?;
    let mut asset_paths = Vec::new();
    for (index, asset) in footer.assets.iter().enumerate() {
        let file = if asset.kind == BundleAssetKind::InputEvents {
            input.clone()
        } else {
            let file = directory.path().join(format!("asset-{index}"));
            original.seek(SeekFrom::Start(asset.offset))?;
            std::io::copy(
                &mut (&mut original).take(asset.len),
                &mut std::fs::File::create(&file)?,
            )?;
            file
        };
        asset_paths.push(file);
    }
    let assets: Vec<_> = footer
        .assets
        .iter()
        .zip(&asset_paths)
        .map(|(asset, path)| RecordingBundleAsset {
            kind: asset.kind,
            asset_id: asset.asset_id.as_deref(),
            path,
        })
        .collect();
    snow_recording_model::write_recording_bundle(&staging, &footer.manifest, &assets)?;
    drop(original);
    tempfile::TempPath::try_from_path(staging)?
        .persist(path)
        .map_err(|e| e.error)?;
    Ok(())
}

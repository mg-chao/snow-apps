//! Deterministic replay of the production direct-recording worker path.
use super::*;
use ffmpeg_next as ffmpeg;
use std::path::Path;
use windows::Win32::Foundation::FILETIME;
use windows::Win32::System::ProcessStatus::{K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS};
use windows::Win32::System::Threading::{GetCurrentProcess, GetProcessTimes};

struct Rasterizer;
impl crate::keyboard_overlay::KeycapRasterizer for Rasterizer {
    fn rasterize(
        &mut self,
        _: &str,
        _: f32,
    ) -> std::result::Result<crate::keyboard_overlay::Keycap, String> {
        Ok(crate::keyboard_overlay::Keycap {
            width: 80,
            height: 40,
            pixels: [180, 40, 60, 255].repeat(3200),
        })
    }
}

fn cpu_ms() -> std::result::Result<f64, windows::core::Error> {
    let mut creation = FILETIME::default();
    let mut exit = FILETIME::default();
    let mut kernel = FILETIME::default();
    let mut user = FILETIME::default();
    // SAFETY: all output pointers refer to initialized, live FILETIMEs.
    unsafe {
        GetProcessTimes(
            GetCurrentProcess(),
            &mut creation,
            &mut exit,
            &mut kernel,
            &mut user,
        )?;
    }
    let ticks = |t: FILETIME| (u64::from(t.dwHighDateTime) << 32) | u64::from(t.dwLowDateTime);
    Ok((ticks(kernel) + ticks(user)) as f64 / 10_000.0)
}

fn peak_memory() -> std::result::Result<usize, windows::core::Error> {
    let mut counters = PROCESS_MEMORY_COUNTERS::default();
    // SAFETY: counters points to writable storage of the supplied size.
    unsafe {
        K32GetProcessMemoryInfo(
            GetCurrentProcess(),
            &mut counters,
            std::mem::size_of::<PROCESS_MEMORY_COUNTERS>() as u32,
        )
        .ok()?;
    }
    Ok(counters.PeakWorkingSetSize)
}

fn verify(
    path: &Path,
    dimensions: (u32, u32),
) -> std::result::Result<(u64, f64), Box<dyn std::error::Error>> {
    let mut input = ffmpeg::format::input(path)?;
    let stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .ok_or("missing video")?;
    let index = stream.index();
    let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())?
        .decoder()
        .video()?;
    if (decoder.width(), decoder.height()) != dimensions {
        return Err("wrong dimensions".into());
    }
    let duration = input.duration() as f64 / 1000.0;
    let mut frame = ffmpeg::frame::Video::empty();
    let mut count = 0;
    let mut previous = None;
    let mut drain = |decoder: &mut ffmpeg::decoder::video::Video| -> std::result::Result<(), Box<dyn std::error::Error>> {
        loop { match decoder.receive_frame(&mut frame) {
            Ok(()) => {
                if let (Some(last), Some(pts)) = (previous, frame.pts())
                    && pts <= last
                {
                    return Err("non-increasing decoded PTS".into());
                }
                previous = frame.pts(); count += 1;
            }
            Err(ffmpeg::Error::Eof) => break,
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => break,
            Err(error) => return Err(error.into()),
        }}
        Ok(())
    };
    for (stream, packet) in input.packets() {
        if stream.index() == index {
            decoder.send_packet(&packet)?;
            drain(&mut decoder)?;
        }
    }
    decoder.send_eof()?;
    drain(&mut decoder)?;
    if count == 0 {
        return Err("empty decoded output".into());
    }
    Ok((count, duration))
}

struct Sample {
    frame: CapturedFrame,
    cursor: Option<AttachedCursorSample>,
    timestamp_ms: u64,
    duplicate: bool,
}

#[allow(clippy::too_many_arguments)]
pub fn run(
    directory: &Path,
    scenario: &str,
    width: u32,
    height: u32,
    fps: u32,
    frames: u64,
    hardware: bool,
) -> std::result::Result<(), Box<dyn std::error::Error>> {
    if !matches!(
        scenario,
        "static" | "motion" | "cursor" | "effects" | "resize" | "animated" | "audio"
    ) || width == 0
        || height == 0
        || fps == 0
        || frames < 2
    {
        return Err("invalid replay arguments".into());
    }
    std::fs::create_dir_all(directory)?;
    let format = if scenario == "animated" {
        ExportFormat::Apng
    } else {
        ExportFormat::Mp4
    };
    let path = directory.join(format!("replay.{}", format.file_extension()));
    let config = DirectRecordingConfig {
        region: RecordingRegion::new(0, 0, width, height),
        capture_backend: CaptureBackendKind::Auto,
        output_path: path.clone(),
        format,
        capture_fps: fps,
        output_fps: if scenario == "animated" { 10 } else { fps },
        maximum_width: (scenario == "resize").then_some(1920),
        maximum_height: (scenario == "resize").then_some(1080),
        codec: VideoCodec::H264,
        preset: VideoEncodingSpeed::VeryFast,
        prefer_hardware_encoder: hardware,
        enable_microphone: false,
        enable_system_audio: scenario == "audio",
        show_cursor: true,
        keyboard: None,
        mouse_trail_rgba: if scenario == "effects" {
            [220, 30, 60, 180]
        } else {
            [0; 4]
        },
        mouse_trail_duration_ms: 500,
        mouse_click_rgba: if scenario == "effects" {
            [40, 180, 220, 120]
        } else {
            [0; 4]
        },
    };
    // The production recording path captures BGRA; replay the same order so
    // the benchmark exercises the compositor and encoder transport the app
    // uses.
    let sources: Vec<snow_capture::frame::Frame> = (0..4)
        .map(|phase| {
            let mut pixels = vec![0; width as usize * height as usize * 4];
            for (index, pixel) in pixels.chunks_exact_mut(4).enumerate() {
                let (x, y) = (index as u32 % width, index as u32 / width);
                let v = if (x / 64 + y / 32 + phase) % 7 == 0 {
                    180
                } else {
                    40
                };
                pixel.copy_from_slice(&[v, v, v, 255]);
            }
            snow_capture::frame::Frame::from_bgra8(width, height, pixels).unwrap()
        })
        .collect();
    let shape = CursorShape::from_rgba(
        0,
        0,
        16,
        16,
        CursorCompositionMode::AlphaBlend,
        [255, 255, 255, 255].repeat(256),
    );
    let samples: Vec<_> = (0..frames)
        .map(|index| {
            let moving = matches!(scenario, "motion" | "resize" | "animated" | "audio");
            // The static scenario replays an unchanged desktop: backends
            // report those captures as duplicates, which the worker skips.
            let mut frame = sources[if moving { index as usize % 4 } else { 0 }].clone();
            if scenario == "static" && index > 0 {
                frame.mark_duplicate_for_tests();
            }
            Sample {
                duplicate: frame.metadata().is_duplicate(),
                frame: frame.into(),
                timestamp_ms: index * 1000 / u64::from(fps),
                cursor: Some(AttachedCursorSample {
                    x: if matches!(scenario, "cursor" | "effects") {
                        (index * 7 % u64::from(width)) as i32
                    } else {
                        0
                    },
                    y: (height / 2) as i32,
                    visible: true,
                    shape: CursorShapeState::Embedded(shape.clone()),
                }),
            }
        })
        .collect();
    let mut compositor = VisualCompositor::new(config.output_dimensions());
    if scenario == "effects" {
        compositor.keyboard = Some(KeyboardOverlay::new(
            config.output_dimensions(),
            Box::new(Rasterizer),
        ));
        for (at_ms, down) in [(100, true), (300, false)] {
            compositor.pending_keys.push_back(KeyEvent {
                at_ms,
                key: 65,
                down,
                label: "A".into(),
                modifiers: vec![],
            });
        }
        compositor.clicks.push_back(RenderClick {
            timestamp_ms: 100,
            x: 100,
            y: 100,
            button: snow_recording_effects::mouse_hook::ObservedMouseButton::Left,
        });
    }
    let setup = Instant::now();
    let mut encoder = StreamingEncoder::create(config.streaming_config())?;
    let setup_ms = setup.elapsed().as_secs_f64() * 1000.0;
    let mut pushes = Vec::with_capacity(frames as usize);
    let audio = vec![100i16; 480 * 2];
    let cpu_start = cpu_ms()?;
    let start = Instant::now();
    let mut audio_at = 0;
    let mut compositions = 0;
    let mut next_overlay_frame_ms = 0u64;
    let output_interval_ms = (1_000 / u64::from(config.output_fps.max(1))).max(1);
    let mut mixer = LiveAudioMixer::new(true, false);
    let epoch = Instant::now();
    let clock = RecordingClock::new(epoch);
    let mut pause_done = false;
    for sample in samples {
        let timestamp = sample.timestamp_ms;
        let push = Instant::now();
        if scenario == "audio" && !pause_done && timestamp >= frames * 500 / u64::from(fps) {
            clock
                .controller()
                .mark_pause(epoch + Duration::from_millis(timestamp));
            clock
                .controller()
                .mark_resume(epoch + Duration::from_millis(timestamp + 250));
            mixer.reset_alignment(None);
            pause_done = true;
        }
        if scenario == "audio" {
            let wall = epoch + Duration::from_millis(timestamp + if pause_done { 250 } else { 0 });
            assert_eq!(clock.active_elapsed_ms(wall), timestamp);
        }
        // The production worker composes every accepted capture, then
        // recomposes the latest capture at output cadence while an overlay
        // animation is active. When the recomposition fires for the same
        // iteration it replaces the capture push, so the worker skips the
        // capture push entirely (decided once, before the event). Duplicate
        // captures are skipped as well unless an animation is redrawing.
        let overlay_due = timestamp >= next_overlay_frame_ms
            && compositor.has_active_animation(&config, timestamp);
        let skip_duplicate =
            sample.duplicate && !compositor.has_active_animation(&config, timestamp);
        if !overlay_due && !skip_duplicate {
            let composed = compositor.compose_with_cursor(
                &config,
                &sample.frame,
                sample.cursor.as_ref(),
                timestamp,
            )?;
            encoder.push_rgba_frame(timestamp, composed.as_slice())?;
            compositions += 1;
        }
        if overlay_due {
            let composed = compositor.compose_with_cursor(
                &config,
                &sample.frame,
                sample.cursor.as_ref(),
                timestamp,
            )?;
            encoder.push_rgba_frame(timestamp, composed.as_slice())?;
            compositions += 1;
            next_overlay_frame_ms = timestamp.saturating_add(output_interval_ms);
        }
        if scenario == "audio" {
            while audio_at <= timestamp {
                mixer.insert_samples(
                    AudioSourceKind::System,
                    audio_at * 48,
                    480,
                    &audio,
                    Duration::from_millis(timestamp),
                );
                audio_at += 10;
            }
            mixer.emit_ready(Duration::from_millis(timestamp), false, &mut encoder)?;
        }
        pushes.push(push.elapsed().as_secs_f64() * 1000.0);
    }
    let end_ms = frames * 1000 / u64::from(fps);
    if scenario == "audio" {
        while audio_at < end_ms {
            mixer.insert_samples(
                AudioSourceKind::System,
                audio_at * 48,
                480,
                &audio,
                Duration::from_millis(end_ms),
            );
            audio_at += 10;
        }
        mixer.emit_ready(Duration::from_millis(end_ms), true, &mut encoder)?;
    }
    let process_ms = start.elapsed().as_secs_f64() * 1000.0;
    let finish = Instant::now();
    let report = encoder.finish()?;
    let finish_ms = finish.elapsed().as_secs_f64() * 1000.0;
    let cpu = cpu_ms()? - cpu_start;
    let memory = peak_memory()?;
    pushes.sort_by(f64::total_cmp);
    let p95 = pushes[(pushes.len() - 1) * 95 / 100];
    let (decoded, duration) = verify(&path, config.output_dimensions())?;
    let row = format!(
        "{scenario},{width},{height},{fps},{frames},{:.3},{setup_ms:.3},{process_ms:.3},{finish_ms:.3},{cpu:.3},{p95:.3},{compositions},{},{},{},{decoded},{duration:.3},{},{memory},{}\n",
        frames as f64 * 1000.0 / f64::from(fps),
        report.encoded_frames,
        report.coalesced_frames,
        std::fs::metadata(&path)?.len(),
        report.video_encoder,
        report.used_hardware_video_encoder
    );
    std::fs::write(
        directory.join("replay.csv"),
        format!(
            "scenario,width,height,fps,input_frames,recorded_ms,setup_ms,process_ms,finish_ms,cpu_ms,p95_ms,compositions,encoded,coalesced,decoded,duration_ms,output_bytes,peak_working_set_bytes,encoder,hardware_used\n{row}"
        ),
    )?;
    print!("{row}");
    Ok(())
}

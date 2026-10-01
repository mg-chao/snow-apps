//! Native clean sources preserve CoreVideo/VideoToolbox and the fixed output canvas.
use super::*;
use crate::DirectRecordingConfig;
use crate::deferred::{CaptureProduct, Command, FrameIndexWriter, InputRecorder, start_audio};
use crossbeam_channel::{Receiver, Sender};
use snow_recording_model::CursorFrameRecord;
use std::path::Path;
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicU8, Ordering},
};

pub(crate) fn run_capture(
    config: &DirectRecordingConfig,
    path: &Path,
    commands: Receiver<Command>,
    state: Arc<AtomicU8>,
    ready: Sender<std::result::Result<(), String>>,
    stop_boundary: Arc<Mutex<Option<Instant>>>,
    control_clock: Arc<Mutex<Option<RecordingClock>>>,
) -> Result<CaptureProduct> {
    let mut native = super::direct::native_config(config.clone(), Default::default())?;
    let logical = native.output;
    let coded = PixelSize::new(
        logical.width.div_ceil(2) * 2,
        logical.height.div_ceil(2) * 2,
    )
    .map_err(|e| ScreenRecorderError::InvalidConfig(e.to_string()))?;
    let video = path.join("source.mp4");
    let inputs = path.join("inputs.bin");
    let index = path.join("video.index");
    native.output = coded;
    native.output_path = video.clone();
    native.format = ExportFormat::Mp4;
    native.effects = NativeEffectsConfig::default();
    native.audio = None;
    native.capture.cursor = snow_media::CursorMode::Hidden;
    native.execution = ExportExecutionMode::HardwarePreferred;
    native.video.quality = config.quality.max(95);
    native.video.speed = snow_recording_model::VideoEncodingSpeed::VeryFast;
    native.codec = if native.capture.dynamic_range == DynamicRange::Hdr {
        VideoCodec::H265
    } else {
        VideoCodec::H264
    };
    let source_codec = native.codec;
    let mut recording = NativeRecordingSession::start_source(native, logical)?;
    recording.set_stop_boundary(Arc::clone(&stop_boundary));
    let clock = recording.source_clock();
    clock.controller().mark_pause(clock.started_at());
    let mut audio = start_audio(config, path, &clock)?;
    let mut input = InputRecorder::new(config, &inputs, (logical.width, logical.height))?;
    let mut cursor = config
        .show_cursor
        .then(snow_macos::cursor::CursorSampler::default);
    let mut last_cursor = None::<(snow_macos::cursor::CursorShape, (f64, f64), u64)>;
    let mut next_shape = 1u64;
    let mut frame_index = FrameIndexWriter::new(&index, config.output_fps)?;
    let origin = Instant::now();
    clock.controller().mark_resume(origin);
    input.reset(&clock, origin)?;
    let mut paused = false;
    let mut since = origin;
    let mut stop = None;
    let mut admitted = false;
    let mut schedule = crate::output_schedule::OutputSchedule::new(config.output_fps);
    *control_clock.lock().unwrap_or_else(|e| e.into_inner()) = Some(clock.clone());
    state.store(1, Ordering::Release);
    let _ = ready.send(Ok(()));
    while stop.is_none() {
        let first = if paused {
            commands.recv_timeout(Duration::from_millis(10)).ok()
        } else {
            commands.try_recv().ok()
        };
        for command in first.into_iter().chain(commands.try_iter()) {
            match command {
                Command::Pause(at) if !paused => {
                    recording.pause_capture_at(at);
                    if let Some(audio) = &audio {
                        audio.pause();
                    }
                    input.reset(&clock, at)?;
                    paused = true;
                }
                Command::Resume(at) if paused => {
                    recording.resume_capture();
                    if let Some(audio) = &audio {
                        audio.resume();
                    }
                    input.reset(&clock, at)?;
                    paused = false;
                    since = at;
                }
                Command::Stop(at) => {
                    stop = Some(at);
                    break;
                }
                Command::Cancel => return Err(ScreenRecorderError::ExportCanceled),
                _ => {}
            }
        }
        if stop.is_some() {
            break;
        }
        if state.load(Ordering::Acquire) == 3 {
            return Err(ScreenRecorderError::ExportCanceled);
        }
        if let Some(at) = *stop_boundary.lock().unwrap_or_else(|e| e.into_inner()) {
            stop = Some(at);
            break;
        }
        if paused {
            input.drain(&clock, true, since, None)?;
            continue;
        }
        let event = recording.step(schedule.wait(clock.active_elapsed_duration(Instant::now())))?;
        match event {
            NativeRecordingEvent::Frame { pts } => {
                frame_index.push(pts)?;
                admitted = true;
            }
            NativeRecordingEvent::Interruption { .. } => {
                input.reset(&clock, Instant::now())?;
            }
            NativeRecordingEvent::Configuration { .. } | NativeRecordingEvent::Idle => {}
        }
        if let Some(at) = *stop_boundary.lock().unwrap_or_else(|e| e.into_inner()) {
            stop = Some(at);
            break;
        }
        if admitted
            && let Some(slot) = crate::deferred::admit_source_slot(
                &mut schedule,
                &clock,
                &stop_boundary,
                &control_clock,
            )
        {
            let slot_ms = (u128::from(slot.pts) * 1000 / u128::from(config.output_fps)) as u64;
            let at = Instant::now();
            input.begin_slot(slot_ms);
            let transform = recording.source_transform();
            if input.update_geometry(
                &clock,
                at,
                (transform.output.width, transform.output.height),
                Some(transform),
            )? {
                // Placement changes reset replay's shape cache too. Re-advertise
                // the sampled native shape even when its pixel scale is unchanged.
                last_cursor = None;
            }
            input.drain(&clock, false, since, None)?;
            let mut captured_pointer = None;
            if let Some(cursor) = &mut cursor {
                let sample = cursor.sample().map_err(native_error)?;
                let transform = recording.source_transform();
                let destination = aspect_fit(transform.output, logical)
                    .map_err(|e| ScreenRecorderError::InvalidConfig(e.to_string()))?;
                let point =
                    crate::macos_effects::project(sample.x, sample.y, transform, destination);
                let mut shape_record = None;
                let mut shape_id = None;
                if point.is_some() {
                    let shape = sample.shape.ok_or_else(|| {
                        ScreenRecorderError::UnsupportedFeature(
                    "public system cursor shape is unavailable for post-recording rendering".into())
                    })?;
                    let scale = (
                        destination.width as f64 / transform.source.width,
                        destination.height as f64 / transform.source.height,
                    );
                    let same = last_cursor
                        .as_ref()
                        .is_some_and(|(cached, cached_scale, _)| {
                            *cached_scale == scale
                                && cached.width == shape.width
                                && cached.height == shape.height
                                && cached.point_width == shape.point_width
                                && cached.point_height == shape.point_height
                                && cached.hotspot_x == shape.hotspot_x
                                && cached.hotspot_y == shape.hotspot_y
                                && cached.rgba == shape.rgba
                        });
                    if same {
                        shape_id = last_cursor.as_ref().map(|(_, _, id)| *id);
                    } else {
                        let id = next_shape;
                        next_shape = next_shape.checked_add(1).ok_or_else(|| {
                            ScreenRecorderError::InvalidConfig(
                                "cursor shape identifier overflow".into(),
                            )
                        })?;
                        shape_record = Some(super::editable_cursor::rasterize(&shape, scale, id)?);
                        shape_id = Some(id);
                        last_cursor = Some((shape, scale, id));
                    }
                }
                let (x, y) = point.unwrap_or((0, 0));
                captured_pointer = Some(point);
                input.native_cursor(
                    &clock,
                    at,
                    shape_record,
                    CursorFrameRecord {
                        timestamp_ms: slot_ms,
                        x,
                        y,
                        visible: point.is_some(),
                        shape_id,
                    },
                )?;
            }
            input.pointer(&clock, at, captured_pointer, slot_ms)?;
            input.end_slot();
        }
    }
    let stop = stop.expect("stop boundary");
    let duration_ms = clock
        .active_elapsed_duration(stop)
        .as_nanos()
        .div_ceil(1_000_000)
        .max(1) as u64;
    recording.freeze_source(stop);
    input.drain(&clock, true, since, Some(stop))?;
    if !admitted {
        return Err(ScreenRecorderError::Encode(
            "no screen frame was captured".into(),
        ));
    }
    let mut report = recording.finish_source(duration_ms)?;
    report.media.cursor = snow_media::CursorMode::Separate;
    let tracks = audio
        .take()
        .map(snow_audio_recorder::AudioRecordingSession::finish)
        .transpose()?
        .map(|a| a.tracks)
        .unwrap_or_default();
    report.media.discontinuities.extend(
        tracks
            .iter()
            .flat_map(|t| t.discontinuities.iter().cloned()),
    );
    input.finish()?;
    frame_index.finish(duration_ms)?;
    clock.controller().finalize(stop);
    Ok(CaptureProduct {
        duration_ms,
        logical: (logical.width, logical.height),
        coded: (coded.width, coded.height),
        media: report.media,
        clock,
        tracks,
        video,
        index,
        inputs,
        source_codec,
        encoder_report: report.encoder,
    })
}

use std::fs::File;
use std::io::{BufWriter, Write};
use std::sync::Mutex;
use std::sync::atomic::{AtomicU8, Ordering};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, Sender, TrySendError};
use ffmpeg_next as ffmpeg;
use snow_audio_recorder::{
    AudioRecordingArtifact, AudioRecordingConfig, AudioRecordingSession, AudioTrackConfig,
    AudioTrackDevice,
};
use snow_capture::{
    CaptureEvent, CaptureOptions, CaptureStream, CaptureStreamConfig, CaptureSystem,
    CaptureWorkload, CapturedFrame,
};
use snow_core::error::{Classify, ErrorClass};
use snow_core::recording_clock::RecordingClock;
use snow_pipeline::{
    PipelineControl as MuxCommand, PipelineStatus as MuxStatus, build_recording_pipeline,
};
use snow_recording_model::{
    BundleAssetKind, LocalRecordingPaths, PauseInterval, RecordingArtifact, RecordingBundleAsset,
    SessionManifest, write_mouse_records, write_recording_bundle,
};
use uuid::Uuid;

use crate::adapter::video::resolve_capture_target;
use crate::config::{IntermediateRecordingProfile, RecordingConfig, VideoEncodeConfig};
use crate::error::{Result, ScreenRecorderError};
use crate::ffmpeg_util::{
    copy_rgba_into_frame, ensure_ffmpeg_initialized, ensure_video_frame_writable, is_eagain,
};
use crate::processor::{CursorProcessor, VideoProcessor};
use crate::temp::TempLayout;
use crate::video_quality::{quality_to_h264_crf, smart_quality_bitrate_bps};

const VIDEO_INDEX_MAGIC: &[u8] = b"SVIDX\0\0";

#[derive(Clone, Copy, Debug)]
enum ControlCommand {
    Pause,
    Resume,
    Stop,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecordingState {
    Created,
    Running,
    Paused,
    Stopped,
}

impl RecordingState {
    fn as_u8(self) -> u8 {
        match self {
            Self::Created => 0,
            Self::Running => 1,
            Self::Paused => 2,
            Self::Stopped => 3,
        }
    }

    fn from_u8(v: u8) -> Self {
        match v {
            1 => Self::Running,
            2 => Self::Paused,
            3 => Self::Stopped,
            _ => Self::Created,
        }
    }
}

struct RuntimeHandles {
    control_tx: Sender<ControlCommand>,
    worker_handle: JoinHandle<Result<WorkerOutcome>>,
}

enum VideoWorkerCommand {
    Frame { frame: CapturedFrame, ts_ms: u64 },
    Finalize { final_ts_ms: u64 },
}

#[derive(Debug)]
struct VideoWorkerOutcome {
    width: u32,
    height: u32,
}

#[derive(Debug)]
pub(crate) struct WorkerOutcome {
    pub(crate) width: u32,
    pub(crate) height: u32,
    pub(crate) pause_intervals: Vec<PauseInterval>,
    pub(crate) audio_artifact: AudioRecordingArtifact,
}

pub struct RecordingSession {
    config: RecordingConfig,
    session_id: String,
    layout: TempLayout,
    cleanup_paths_on_drop: bool,
    state: AtomicU8,
    runtime: Mutex<Option<RuntimeHandles>>,
    capture_origin: Mutex<(i32, i32)>,
}

impl RecordingSession {
    pub fn create(config: RecordingConfig) -> Result<Self> {
        config
            .validate()
            .map_err(ScreenRecorderError::InvalidConfig)?;

        let session_id = Uuid::new_v4().simple().to_string();
        let layout = TempLayout::create(&config, &session_id)?;

        Ok(Self {
            config,
            session_id,
            layout,
            cleanup_paths_on_drop: true,
            state: AtomicU8::new(RecordingState::Created.as_u8()),
            runtime: Mutex::new(None),
            capture_origin: Mutex::new((0, 0)),
        })
    }

    /// Lock the runtime mutex, mapping poison errors to `InvalidConfig`.
    fn lock_runtime(&self) -> Result<std::sync::MutexGuard<'_, Option<RuntimeHandles>>> {
        self.runtime
            .lock()
            .map_err(|_| ScreenRecorderError::InvalidConfig("runtime lock poisoned".to_string()))
    }

    /// Send a control command to the running worker, returning an error
    /// if the runtime is not initialized or the worker has stopped.
    fn send_control(&self, cmd: ControlCommand) -> Result<()> {
        let guard = self.lock_runtime()?;
        let runtime = guard.as_ref().ok_or_else(|| {
            ScreenRecorderError::InvalidConfig("recording runtime is not initialized".to_string())
        })?;
        runtime
            .control_tx
            .send(cmd)
            .map_err(|_| ScreenRecorderError::Encode("recording worker has stopped".to_string()))
    }

    /// Lock the capture_origin mutex, mapping poison errors to `InvalidConfig`.
    fn lock_capture_origin(&self) -> Result<std::sync::MutexGuard<'_, (i32, i32)>> {
        self.capture_origin.lock().map_err(|_| {
            ScreenRecorderError::InvalidConfig("capture_origin lock poisoned".to_string())
        })
    }

    fn transition_state(
        &self,
        from: RecordingState,
        to: RecordingState,
        command: ControlCommand,
        invalid_state_message: &'static str,
    ) -> Result<()> {
        if self.state() != from {
            return Err(ScreenRecorderError::InvalidConfig(
                invalid_state_message.to_string(),
            ));
        }
        self.send_control(command)?;
        self.state.store(to.as_u8(), Ordering::Release);
        Ok(())
    }

    pub fn start(&mut self) -> Result<()> {
        if self.state() != RecordingState::Created {
            return Err(ScreenRecorderError::InvalidConfig(
                "recording session can only be started from Created state".to_string(),
            ));
        }

        let capture_target = resolve_capture_target(&self.config.target)?;
        let capture_system = CaptureSystem::builder()
            .with_backend_kind(self.config.capture_backend)
            .build()?;
        let capture_session = capture_system.open_session(
            capture_target,
            CaptureOptions {
                workload: CaptureWorkload::Continuous,
                ..Default::default()
            },
        )?;
        let target_info = capture_session.target_info()?;
        let origin = (target_info.origin_x, target_info.origin_y);
        *self.lock_capture_origin()? = origin;

        let started_at = Instant::now();
        let clock = RecordingClock::new(started_at);
        let capture_stream = match CaptureStream::spawn(
            capture_session,
            CaptureStreamConfig {
                target_fps: self.config.fps,
                buffer_depth: 16,
                max_consecutive_errors: 30,
                adaptive_fps: true,
                min_fps: 10,
                pause_on_resolution_change: false,
                include_cursor: true,
            },
        ) {
            Ok(stream) => stream,
            Err(err) => return Err(ScreenRecorderError::Capture(err)),
        };
        let audio_recording =
            start_audio_recording_if_enabled(&self.config, &self.layout.session_dir, &clock);

        let (control_tx, control_rx) = crossbeam_channel::unbounded::<ControlCommand>();
        let layout = self.layout.clone();
        let config = self.config.clone();
        let worker_handle = std::thread::Builder::new()
            .name("snow-screen-recorder-worker".to_string())
            .spawn(move || {
                new_recording_worker(
                    config,
                    layout,
                    capture_stream,
                    audio_recording,
                    control_rx,
                    clock,
                )
            })
            .map_err(|e| ScreenRecorderError::Io(std::io::Error::other(e)))?;

        let runtime = RuntimeHandles {
            control_tx,
            worker_handle,
        };

        let mut guard = self.lock_runtime()?;
        *guard = Some(runtime);
        self.state
            .store(RecordingState::Running.as_u8(), Ordering::Release);
        Ok(())
    }

    pub fn pause(&self) -> Result<()> {
        self.transition_state(
            RecordingState::Running,
            RecordingState::Paused,
            ControlCommand::Pause,
            "pause is only allowed while recording is Running",
        )
    }

    pub fn resume(&self) -> Result<()> {
        self.transition_state(
            RecordingState::Paused,
            RecordingState::Running,
            ControlCommand::Resume,
            "resume is only allowed while recording is Paused",
        )
    }

    pub fn stop(mut self) -> Result<RecordingArtifact> {
        let mut runtime_guard = self.lock_runtime()?;
        let runtime = runtime_guard.take().ok_or_else(|| {
            ScreenRecorderError::InvalidConfig("recording session was not started".to_string())
        })?;
        drop(runtime_guard);

        let _ = runtime.control_tx.send(ControlCommand::Stop);

        let worker_result = runtime.worker_handle.join().map_err(|_| {
            ScreenRecorderError::Encode("recording worker thread panicked".to_string())
        })?;

        self.state
            .store(RecordingState::Stopped.as_u8(), Ordering::Release);

        let outcome = worker_result?;

        let (capture_origin_x, capture_origin_y) = *self.lock_capture_origin()?;

        let manifest = SessionManifest {
            session_id: self.session_id.clone(),
            output_dir: self.layout.output_dir.clone(),
            keep_temp_files: self.config.keep_temp_files,
            fps: self.config.fps,
            intermediate_profile: self.config.intermediate_profile,
            recording_video: self.config.video,
            width: outcome.width,
            height: outcome.height,
            capture_origin_x,
            capture_origin_y,
            audio_tracks: outcome
                .audio_artifact
                .tracks
                .iter()
                .map(|track| track.manifest.clone())
                .collect(),
            pause_intervals: outcome.pause_intervals,
        };

        let mut assets = vec![
            RecordingBundleAsset {
                kind: BundleAssetKind::VideoIndex,
                asset_id: None,
                path: &self.layout.video_index_path,
            },
            RecordingBundleAsset {
                kind: BundleAssetKind::MouseStore,
                asset_id: None,
                path: &self.layout.mouse_path,
            },
        ];
        for track in &outcome.audio_artifact.tracks {
            if !track.manifest.recorded || !track.path.exists() {
                continue;
            }
            assets.push(RecordingBundleAsset {
                kind: BundleAssetKind::AudioTrack,
                asset_id: Some(track.manifest.asset_id.as_str()),
                path: &track.path,
            });
        }
        write_recording_bundle(&self.layout.bundle_path, &manifest, &assets)?;

        if !self.config.keep_temp_files && self.layout.session_dir.exists() {
            // The bundle is complete and is now the authoritative recording.
            // A transient cleanup failure must not turn a successful stop into
            // an error whose Drop path removes that valid bundle.
            let _ = std::fs::remove_dir_all(&self.layout.session_dir);
        }

        let artifact = RecordingArtifact {
            session_id: self.session_id.clone(),
            output_dir: self.layout.output_dir.clone(),
            local_paths: LocalRecordingPaths {
                temp_dir: self.layout.session_dir.clone(),
                video_intermediate_path: self.layout.bundle_path.clone(),
                video_index_path: self.layout.video_index_path.clone(),
                mouse_path: self.layout.mouse_path.clone(),
            },
            bundle_path: self.layout.bundle_path.clone(),
            audio_tracks: manifest.audio_tracks,
        };
        self.cleanup_paths_on_drop = false;
        Ok(artifact)
    }

    pub fn state(&self) -> RecordingState {
        RecordingState::from_u8(self.state.load(Ordering::Acquire))
    }

    fn cleanup_recording_paths(&self) {
        if self.config.keep_temp_files {
            return;
        }

        let _ = std::fs::remove_file(&self.layout.bundle_path);
        let _ = std::fs::remove_dir_all(&self.layout.session_dir);
    }
}

impl Drop for RecordingSession {
    fn drop(&mut self) {
        if !self.cleanup_paths_on_drop {
            return;
        }

        let runtime = match self.runtime.get_mut() {
            Ok(runtime) => runtime.take(),
            Err(poisoned) => poisoned.into_inner().take(),
        };
        if let Some(runtime) = runtime {
            let _ = runtime.control_tx.send(ControlCommand::Stop);
            let _ = runtime.worker_handle.join();
        }

        self.cleanup_recording_paths();
    }
}

fn start_audio_recording_if_enabled(
    config: &RecordingConfig,
    session_dir: &std::path::Path,
    clock: &RecordingClock,
) -> Option<AudioRecordingSession> {
    if config.audio.tracks.iter().all(|track| !track.enabled) {
        return None;
    }

    let audio_config = AudioRecordingConfig {
        output_dir: session_dir.to_path_buf(),
        sample_rate_hz: config.audio.sample_rate_hz,
        channels: config.audio.channels.channels(),
        tracks: config
            .audio
            .tracks
            .iter()
            .map(|track| AudioTrackConfig {
                track_id: track.track_id.clone(),
                role: track.role,
                device: match &track.source {
                    crate::config::RecordingAudioTrackSource::SystemDefault => {
                        AudioTrackDevice::SystemDefault
                    }
                    crate::config::RecordingAudioTrackSource::MicrophoneDefault => {
                        AudioTrackDevice::MicrophoneDefault
                    }
                    crate::config::RecordingAudioTrackSource::InputDeviceId(id) => {
                        AudioTrackDevice::InputDeviceId(id.clone())
                    }
                },
                enabled: track.enabled,
            })
            .collect(),
        ..AudioRecordingConfig::default()
    };

    AudioRecordingSession::start(audio_config, clock.clone()).ok()
}

fn finish_audio_recording_if_available(
    recording: Option<AudioRecordingSession>,
) -> AudioRecordingArtifact {
    recording
        .and_then(|recording| recording.finish().ok())
        .unwrap_or_default()
}

pub(crate) struct LiveVideoEncoder {
    output: ffmpeg::format::context::Output,
    encoder: ffmpeg::encoder::video::Encoder,
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
    scaler: ffmpeg::software::scaling::Context,
    rgba_frame: ffmpeg::frame::Video,
    encode_frame: ffmpeg::frame::Video,
    width: u32,
    height: u32,
    fps: u32,
    last_pts: Option<i64>,
    index_writer: VideoIndexWriter,
    nominal_frame_duration_ms: u32,
}

struct VideoIndexWriter {
    writer: BufWriter<File>,
    pending: Option<(u64, u64)>,
    next_index: u64,
}

impl VideoIndexWriter {
    fn create(path: &std::path::Path) -> Result<Self> {
        let file = File::create(path)?;
        let mut writer = BufWriter::with_capacity(128 * 1024, file);
        writer.write_all(VIDEO_INDEX_MAGIC)?;
        Ok(Self {
            writer,
            pending: None,
            next_index: 0,
        })
    }

    fn push(&mut self, timestamp_ms: u64) -> Result<()> {
        if let Some((idx, prev_ts)) = self.pending.take() {
            let duration = timestamp_ms.saturating_sub(prev_ts).max(1);
            Self::write_entry(
                &mut self.writer,
                idx,
                prev_ts,
                duration.min(u64::from(u32::MAX)) as u32,
            )?;
        }
        let idx = self.next_index;
        self.next_index = self.next_index.saturating_add(1);
        self.pending = Some((idx, timestamp_ms));
        Ok(())
    }

    fn finalize(&mut self, final_timestamp_ms: u64, nominal_frame_duration_ms: u32) -> Result<()> {
        if let Some((idx, prev_ts)) = self.pending.take() {
            let duration = final_timestamp_ms
                .saturating_sub(prev_ts)
                .max(u64::from(nominal_frame_duration_ms.max(1)));
            Self::write_entry(
                &mut self.writer,
                idx,
                prev_ts,
                duration.min(u64::from(u32::MAX)) as u32,
            )?;
        }
        self.writer.flush()?;
        Ok(())
    }

    fn write_entry(
        writer: &mut BufWriter<File>,
        idx: u64,
        timestamp_ms: u64,
        duration_ms: u32,
    ) -> Result<()> {
        let mut entry = [0u8; 20];
        entry[..8].copy_from_slice(&idx.to_le_bytes());
        entry[8..16].copy_from_slice(&timestamp_ms.to_le_bytes());
        entry[16..20].copy_from_slice(&duration_ms.to_le_bytes());
        writer.write_all(&entry)?;
        Ok(())
    }
}

impl LiveVideoEncoder {
    pub(crate) fn create(
        path: &std::path::Path,
        index_path: &std::path::Path,
        width: u32,
        height: u32,
        fps: u32,
        intermediate_profile: IntermediateRecordingProfile,
        video_config: &VideoEncodeConfig,
    ) -> Result<Self> {
        ensure_ffmpeg_initialized()?;

        let mut output = ffmpeg::format::output_as(path, "matroska").map_err(|err| {
            ScreenRecorderError::Encode(format!(
                "failed to create temporary video output {}: {err}",
                path.display()
            ))
        })?;
        let global_header = output
            .format()
            .flags()
            .contains(ffmpeg::format::Flags::GLOBAL_HEADER);
        let fps = fps.max(1).min(i32::MAX as u32);
        let video_time_base = ffmpeg::Rational(1, fps as i32);
        let video_frame_rate = ffmpeg::Rational(fps as i32, 1);

        let container_video_codec = output.format().codec(path, ffmpeg::media::Type::Video);
        let mut video_codecs = Vec::new();
        for name in ["libx264", "h264_nvenc", "h264_qsv", "h264_amf"] {
            push_unique_video_codec(&mut video_codecs, ffmpeg::encoder::find_by_name(name));
        }
        push_unique_video_codec(
            &mut video_codecs,
            ffmpeg::encoder::find(ffmpeg::codec::Id::H264),
        );
        push_unique_video_codec(
            &mut video_codecs,
            ffmpeg::encoder::find(container_video_codec),
        );

        let mut encoder_failures = Vec::new();
        let mut selected_encoder = None;
        for video_codec in video_codecs {
            match open_temporary_video_encoder(
                video_codec,
                width,
                height,
                fps,
                video_time_base,
                video_frame_rate,
                global_header,
                intermediate_profile,
                video_config,
            ) {
                Ok((video_encoder, pixel_format)) => {
                    selected_encoder = Some((video_codec, video_encoder, pixel_format));
                    break;
                }
                Err(error) => encoder_failures.push(format!("{}: {error}", video_codec.name())),
            }
        }
        let Some((video_codec, video_encoder, pixel_format)) = selected_encoder else {
            return Err(ScreenRecorderError::Encode(format!(
                "no usable video encoder available for temporary recording file ({})",
                encoder_failures.join("; ")
            )));
        };

        let stream_index = {
            let mut stream = output.add_stream(video_codec).map_err(|err| {
                ScreenRecorderError::Encode(format!(
                    "failed to add temporary video output stream: {err}"
                ))
            })?;
            stream.set_time_base(video_time_base);
            stream.set_rate(video_frame_rate);
            stream.set_avg_frame_rate(video_frame_rate);
            stream.set_parameters(&video_encoder);
            stream.index()
        };

        output.write_header().map_err(|err| {
            ScreenRecorderError::Encode(format!(
                "failed to write temporary video output header: {err}"
            ))
        })?;
        let stream_time_base = output
            .stream(stream_index)
            .map(|stream| stream.time_base())
            .ok_or_else(|| {
                ScreenRecorderError::Encode(format!(
                    "failed to resolve temporary video stream {stream_index} after header"
                ))
            })?;

        let scaler = ffmpeg::software::scaling::Context::get(
            ffmpeg::format::Pixel::RGBA,
            width,
            height,
            pixel_format,
            width,
            height,
            ffmpeg::software::scaling::flag::Flags::BILINEAR,
        )
        .map_err(|err| {
            ScreenRecorderError::Encode(format!("failed to create temporary video scaler: {err}"))
        })?;
        let index_writer = VideoIndexWriter::create(index_path)?;
        let nominal_frame_duration_ms = ((1000.0 / fps.max(1) as f64).round() as u32).max(1);

        Ok(Self {
            output,
            encoder: video_encoder,
            stream_index,
            stream_time_base,
            scaler,
            rgba_frame: ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height),
            encode_frame: ffmpeg::frame::Video::new(pixel_format, width, height),
            width,
            height,
            fps,
            last_pts: None,
            index_writer,
            nominal_frame_duration_ms,
        })
    }

    pub(crate) fn encode_frame(&mut self, rgba: &[u8], timestamp_ms: u64) -> Result<()> {
        let pts = self
            .timestamp_to_pts(timestamp_ms)
            .max(self.last_pts.unwrap_or(-1).saturating_add(1));
        self.encode_frame_at_pts(rgba, pts, timestamp_ms)
    }

    pub(crate) fn finalize(
        mut self,
        final_timestamp_ms: u64,
        tail_rgba: Option<&[u8]>,
    ) -> Result<()> {
        if let (Some(last_pts), Some(rgba)) = (self.last_pts, tail_rgba) {
            let final_pts = self.timestamp_to_pts(final_timestamp_ms);
            if final_pts > last_pts {
                self.encode_frame_at_pts(rgba, final_pts, final_timestamp_ms)?;
            }
        }

        self.encoder.send_eof().map_err(|err| {
            ScreenRecorderError::Encode(format!(
                "failed to finalize temporary video encoder: {err}"
            ))
        })?;
        self.drain_packets(true)?;
        self.output.write_trailer().map_err(|err| {
            ScreenRecorderError::Encode(format!(
                "failed to write temporary video output trailer: {err}"
            ))
        })?;
        self.index_writer
            .finalize(final_timestamp_ms, self.nominal_frame_duration_ms)?;
        Ok(())
    }

    fn encode_frame_at_pts(&mut self, rgba: &[u8], pts: i64, timestamp_ms: u64) -> Result<()> {
        let expected_len = self.width as usize * self.height as usize * 4;
        if rgba.len() != expected_len {
            return Err(ScreenRecorderError::Encode(format!(
                "temporary video RGBA size mismatch: expected {expected_len} bytes, got {}",
                rgba.len()
            )));
        }

        copy_rgba_into_frame(&mut self.rgba_frame, self.width, rgba);
        ensure_video_frame_writable(&mut self.encode_frame)?;
        self.scaler
            .run(&self.rgba_frame, &mut self.encode_frame)
            .map_err(|err| {
                ScreenRecorderError::Encode(format!(
                    "failed to convert frame for temporary video encoding: {err}"
                ))
            })?;
        self.encode_frame.set_pts(Some(pts));

        self.encoder.send_frame(&self.encode_frame).map_err(|err| {
            ScreenRecorderError::Encode(format!(
                "failed to send frame to temporary video encoder: {err}"
            ))
        })?;
        self.drain_packets(false)?;
        self.last_pts = Some(pts);
        self.index_writer.push(timestamp_ms)?;
        Ok(())
    }

    fn drain_packets(&mut self, draining: bool) -> Result<()> {
        loop {
            let mut packet = ffmpeg::Packet::empty();
            match self.encoder.receive_packet(&mut packet) {
                Ok(()) => {
                    packet.set_stream(self.stream_index);
                    packet.rescale_ts(self.encoder.time_base(), self.stream_time_base);
                    packet.write_interleaved(&mut self.output).map_err(|err| {
                        ScreenRecorderError::Encode(format!(
                            "failed to write temporary encoded video packet: {err}"
                        ))
                    })?;
                }
                Err(ffmpeg::Error::Eof) => break,
                Err(err) if is_eagain(&err) && !draining => break,
                Err(err) if is_eagain(&err) && draining => continue,
                Err(err) => {
                    return Err(ScreenRecorderError::Encode(format!(
                        "failed to receive temporary encoded video packet: {err}"
                    )));
                }
            }
        }
        Ok(())
    }

    fn timestamp_to_pts(&self, timestamp_ms: u64) -> i64 {
        let pts = (u128::from(timestamp_ms) * u128::from(self.fps) + 500) / 1_000;
        pts.min(i64::MAX as u128) as i64
    }
}

fn push_unique_video_codec(codecs: &mut Vec<ffmpeg::Codec>, candidate: Option<ffmpeg::Codec>) {
    let Some(codec) = candidate else {
        return;
    };
    if codecs
        .iter()
        .all(|existing| existing.name() != codec.name())
    {
        codecs.push(codec);
    }
}

#[allow(clippy::too_many_arguments)]
fn open_temporary_video_encoder(
    video_codec: ffmpeg::Codec,
    width: u32,
    height: u32,
    fps: u32,
    video_time_base: ffmpeg::Rational,
    video_frame_rate: ffmpeg::Rational,
    global_header: bool,
    intermediate_profile: IntermediateRecordingProfile,
    video_config: &VideoEncodeConfig,
) -> Result<(ffmpeg::encoder::video::Encoder, ffmpeg::format::Pixel)> {
    let codec_video_info = video_codec.video().map_err(|err| {
        ScreenRecorderError::Encode(format!("selected codec is not a video encoder: {err}"))
    })?;
    let pixel_format = choose_video_pixel_format(codec_video_info);
    let mut video_encoder = ffmpeg::codec::context::Context::new_with_codec(video_codec)
        .encoder()
        .video()
        .map_err(|err| {
            ScreenRecorderError::Encode(format!("failed to create encoder context: {err}"))
        })?;
    video_encoder.set_width(width);
    video_encoder.set_height(height);
    video_encoder.set_format(pixel_format);
    video_encoder.set_time_base(video_time_base);
    video_encoder.set_frame_rate(Some(video_frame_rate));
    video_encoder.set_bit_rate(smart_quality_bitrate_bps(
        width,
        height,
        fps,
        video_config,
        false,
    ));
    if global_header {
        video_encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
    }

    let video_encoder = if video_codec.name().eq_ignore_ascii_case("libx264") {
        let mut options = ffmpeg::Dictionary::new();
        match intermediate_profile {
            IntermediateRecordingProfile::EditFast => {
                options.set("preset", video_config.speed.as_x264_preset());
                options.set("tune", "zerolatency");
                options.set("g", "1");
                options.set("bf", "0");
                options.set(
                    "crf",
                    &quality_to_h264_crf(video_config.quality).to_string(),
                );
            }
        };
        video_encoder
            .open_as_with(video_codec, options)
            .map_err(|err| ScreenRecorderError::Encode(err.to_string()))?
    } else if let Some(options) = temporary_hardware_encoder_options(&video_codec) {
        video_encoder
            .open_as_with(video_codec, options)
            .map_err(|err| ScreenRecorderError::Encode(err.to_string()))?
    } else {
        video_encoder
            .open_as(video_codec)
            .map_err(|err| ScreenRecorderError::Encode(err.to_string()))?
    };
    Ok((video_encoder, pixel_format))
}

fn temporary_hardware_encoder_options(
    codec: &ffmpeg::Codec,
) -> Option<ffmpeg::Dictionary<'static>> {
    let name = codec.name().to_ascii_lowercase();
    let mut options = ffmpeg::Dictionary::new();
    if name.contains("nvenc") {
        options.set("preset", "p1");
        options.set("tune", "ull");
        options.set("rc", "constqp");
        options.set("g", "1");
        options.set("bf", "0");
        options.set("delay", "0");
        return Some(options);
    }
    if name.contains("qsv") {
        options.set("preset", "veryfast");
        options.set("look_ahead", "0");
        options.set("async_depth", "1");
        options.set("g", "1");
        options.set("bf", "0");
        return Some(options);
    }
    if name.contains("amf") {
        options.set("usage", "transcoding");
        options.set("quality", "speed");
        options.set("g", "1");
        options.set("bf", "0");
        return Some(options);
    }
    None
}

fn choose_video_pixel_format(codec: ffmpeg::codec::Video) -> ffmpeg::format::Pixel {
    let preferred = [
        ffmpeg::format::Pixel::YUV420P,
        ffmpeg::format::Pixel::YUV422P,
        ffmpeg::format::Pixel::RGB24,
    ];
    if let Some(formats) = codec.formats() {
        let available: Vec<_> = formats.collect();
        for pixel in preferred {
            if available.contains(&pixel) {
                return pixel;
            }
        }
        if let Some(first) = available.first().copied() {
            return first;
        }
    }
    ffmpeg::format::Pixel::YUV420P
}

fn new_recording_worker(
    config: RecordingConfig,
    layout: TempLayout,
    capture_stream: snow_capture::CaptureStream,
    audio_recording: Option<AudioRecordingSession>,
    control_rx: Receiver<ControlCommand>,
    clock: RecordingClock,
) -> Result<WorkerOutcome> {
    let target_fps = config.fps.max(1);
    let frame_interval_ms = ((1000.0 / target_fps as f32).round() as u32).max(1);
    let mut cursor = CursorProcessor::new();
    let clock_controller = clock.controller();

    let (video_tx, video_rx) = crossbeam_channel::bounded::<VideoWorkerCommand>(8);
    let video_drop_rx = video_rx.clone();
    let video_handle = std::thread::Builder::new()
        .name("snow-screen-recorder-video".to_string())
        .spawn({
            let video_temp_path = layout.video_temp_path.clone();
            let video_index_path = layout.video_index_path.clone();
            let intermediate_profile = config.intermediate_profile;
            let video_config = config.video;
            move || {
                run_video_worker(
                    target_fps,
                    intermediate_profile,
                    video_config,
                    video_temp_path,
                    video_index_path,
                    video_rx,
                )
            }
        })
        .map_err(|err| ScreenRecorderError::Io(std::io::Error::other(err)))?;

    let (multiplexer, _) = build_recording_pipeline(capture_stream, None);
    let mut video_ended = false;
    let mut fatal_error = false;
    let mut invalid_config_error = false;
    let mut control_stop = false;
    let mut last_observed_ts_ms: Option<u64> = None;

    loop {
        while let Ok(cmd) = control_rx.try_recv() {
            let _ = multiplexer.send_command(mux_command_from_control(&cmd));
            match cmd {
                ControlCommand::Pause => {
                    if let Some(audio) = audio_recording.as_ref() {
                        audio.pause();
                    }
                }
                ControlCommand::Resume => {
                    if let Some(audio) = audio_recording.as_ref() {
                        audio.resume();
                    }
                }
                ControlCommand::Stop => {}
            }
            if matches!(cmd, ControlCommand::Stop) {
                control_stop = true;
            }
        }

        while let Ok(status) = multiplexer.try_recv_status() {
            match status {
                MuxStatus::SourceEnded(source)
                | MuxStatus::SourceDisconnected(source)
                | MuxStatus::SourceForwarderPanicked(source) => {
                    if source == snow_pipeline::SourceKind::Video.source_id() {
                        video_ended = true;
                    }
                }
                MuxStatus::Completed => {
                    video_ended = true;
                }
            }
        }

        if fatal_error || invalid_config_error || control_stop || video_ended {
            break;
        }

        match multiplexer.recv_timeout(Duration::from_millis(25)) {
            Ok(event) => match event {
                snow_pipeline::PipelineEvent::Video(te) => match te.event {
                    CaptureEvent::Frame(frame) => {
                        let instant = frame
                            .metadata()
                            .stream_timestamp()
                            .map(|st| st.instant)
                            .unwrap_or_else(Instant::now);
                        let ts_ms = normalize_video_time(
                            &mut last_observed_ts_ms,
                            clock.active_elapsed_ms(instant),
                        );
                        if let Some(cursor_sample) = frame.metadata().cursor() {
                            cursor.record_frame(ts_ms, cursor_sample);
                        }
                        if !frame.metadata().is_duplicate() {
                            enqueue_video_frame(
                                &video_tx,
                                &video_drop_rx,
                                VideoWorkerCommand::Frame { frame, ts_ms },
                            )?;
                        }
                    }
                    CaptureEvent::Paused { at } => {
                        observe_video_time(&mut last_observed_ts_ms, clock.active_elapsed_ms(at));
                        clock_controller.mark_pause(at);
                    }
                    CaptureEvent::Resumed { at, .. } => {
                        clock_controller.mark_resume(at);
                    }
                    CaptureEvent::StreamEnded => {
                        video_ended = true;
                    }
                    CaptureEvent::Error(err) => {
                        handle_source_error(
                            &err,
                            &mut fatal_error,
                            &mut invalid_config_error,
                            &mut video_ended,
                        );
                    }
                    CaptureEvent::FramesDropped { count, .. } => {
                        if let Some(last) = last_observed_ts_ms {
                            let dropped_span =
                                u64::from(frame_interval_ms).saturating_mul(u64::from(count));
                            let next_ts = last.saturating_add(dropped_span);
                            observe_video_time(&mut last_observed_ts_ms, next_ts);
                            cursor.synthesize_frame_for_drop(next_ts);
                        }
                    }
                    CaptureEvent::ResolutionChanged { .. } => {}
                },
                snow_pipeline::PipelineEvent::Audio(_) => {}
            },
            Err(snow_core::error::RecvTimeoutError::Timeout) => {}
            Err(snow_core::error::RecvTimeoutError::Disconnected) => {
                video_ended = true;
            }
        }
    }

    let _ = multiplexer.send_command(MuxCommand::Stop);
    while let Ok(event) = multiplexer.try_recv() {
        match event {
            snow_pipeline::PipelineEvent::Video(te) => match te.event {
                CaptureEvent::Frame(frame) => {
                    let instant = frame
                        .metadata()
                        .stream_timestamp()
                        .map(|st| st.instant)
                        .unwrap_or_else(Instant::now);
                    let ts_ms = normalize_video_time(
                        &mut last_observed_ts_ms,
                        clock.active_elapsed_ms(instant),
                    );
                    if let Some(cursor_sample) = frame.metadata().cursor() {
                        cursor.record_frame(ts_ms, cursor_sample);
                    }
                    if !frame.metadata().is_duplicate() {
                        enqueue_video_frame(
                            &video_tx,
                            &video_drop_rx,
                            VideoWorkerCommand::Frame { frame, ts_ms },
                        )?;
                    }
                }
                CaptureEvent::Paused { at } => {
                    observe_video_time(&mut last_observed_ts_ms, clock.active_elapsed_ms(at));
                    clock_controller.mark_pause(at);
                }
                CaptureEvent::Resumed { at, .. } => {
                    clock_controller.mark_resume(at);
                }
                CaptureEvent::FramesDropped { count, .. } => {
                    if let Some(last) = last_observed_ts_ms {
                        let dropped_span =
                            u64::from(frame_interval_ms).saturating_mul(u64::from(count));
                        let next_ts = last.saturating_add(dropped_span);
                        observe_video_time(&mut last_observed_ts_ms, next_ts);
                        cursor.synthesize_frame_for_drop(next_ts);
                    }
                }
                CaptureEvent::StreamEnded => {
                    video_ended = true;
                }
                CaptureEvent::Error(err) => {
                    handle_source_error(
                        &err,
                        &mut fatal_error,
                        &mut invalid_config_error,
                        &mut video_ended,
                    );
                }
                CaptureEvent::ResolutionChanged { .. } => {}
            },
            snow_pipeline::PipelineEvent::Audio(_) => {}
        }
    }

    let finalize_at = Instant::now();
    clock_controller.finalize(finalize_at);
    let final_ts_ms = normalize_video_time(
        &mut last_observed_ts_ms,
        clock.active_elapsed_ms(finalize_at),
    );
    video_tx
        .send(VideoWorkerCommand::Finalize { final_ts_ms })
        .map_err(|_| {
            ScreenRecorderError::Encode("video worker stopped before finalization".to_string())
        })?;

    let video_outcome = video_handle
        .join()
        .map_err(|_| ScreenRecorderError::Encode("video worker thread panicked".to_string()))??;

    let mouse_store = cursor.into_mouse_store();
    let audio_artifact = finish_audio_recording_if_available(audio_recording);
    write_mouse_records(&layout.mouse_path, &mouse_store)?;

    Ok(WorkerOutcome {
        width: video_outcome.width,
        height: video_outcome.height,
        pause_intervals: clock
            .pause_intervals()
            .into_iter()
            .map(|interval| PauseInterval {
                start_ms: interval.start_ms,
                end_ms: interval.end_ms,
            })
            .collect(),
        audio_artifact,
    })
}

fn enqueue_video_frame(
    sender: &Sender<VideoWorkerCommand>,
    drop_rx: &Receiver<VideoWorkerCommand>,
    command: VideoWorkerCommand,
) -> Result<()> {
    match sender.try_send(command) {
        Ok(()) => Ok(()),
        Err(TrySendError::Full(command)) => {
            let _ = drop_rx.try_recv();
            sender.try_send(command).map_err(|err| match err {
                TrySendError::Full(_) => ScreenRecorderError::Encode(
                    "video worker queue remained full after drop".to_string(),
                ),
                TrySendError::Disconnected(_) => {
                    ScreenRecorderError::Encode("video worker queue disconnected".to_string())
                }
            })
        }
        Err(TrySendError::Disconnected(_)) => Err(ScreenRecorderError::Encode(
            "video worker queue disconnected".to_string(),
        )),
    }
}

fn run_video_worker(
    target_fps: u32,
    intermediate_profile: IntermediateRecordingProfile,
    video_config: VideoEncodeConfig,
    video_temp_path: std::path::PathBuf,
    video_index_path: std::path::PathBuf,
    rx: Receiver<VideoWorkerCommand>,
) -> Result<VideoWorkerOutcome> {
    let mut video = VideoProcessor::new(
        target_fps,
        intermediate_profile,
        video_config,
        video_temp_path,
        video_index_path,
    );
    while let Ok(command) = rx.recv() {
        match command {
            VideoWorkerCommand::Frame { frame, ts_ms } => {
                let width = frame.width();
                let height = frame.height();
                video.handle_resolution_change(width, height)?;
                video.encode_frame(frame, ts_ms)?;
            }
            VideoWorkerCommand::Finalize { final_ts_ms } => {
                video.finalize(final_ts_ms)?;
                return Ok(VideoWorkerOutcome {
                    width: video.width(),
                    height: video.height(),
                });
            }
        }
    }
    Err(ScreenRecorderError::Encode(
        "video worker channel closed before finalization".to_string(),
    ))
}

fn mux_command_from_control(cmd: &ControlCommand) -> MuxCommand {
    match cmd {
        ControlCommand::Pause => MuxCommand::Pause,
        ControlCommand::Resume => MuxCommand::Resume,
        ControlCommand::Stop => MuxCommand::Stop,
    }
}

fn observe_video_time(last_observed_ts_ms: &mut Option<u64>, ts_ms: u64) {
    if last_observed_ts_ms.is_none_or(|prev| ts_ms > prev) {
        *last_observed_ts_ms = Some(ts_ms);
    }
}

fn normalize_video_time(last_observed_ts_ms: &mut Option<u64>, ts_ms: u64) -> u64 {
    let normalized = last_observed_ts_ms.map_or(ts_ms, |prev| ts_ms.max(prev));
    observe_video_time(last_observed_ts_ms, normalized);
    normalized
}

fn handle_source_error<E: Classify>(
    err: &E,
    fatal_error: &mut bool,
    invalid_config_error: &mut bool,
    ended: &mut bool,
) {
    match err.class() {
        ErrorClass::Fatal => *fatal_error = true,
        ErrorClass::Transient => *ended = true,
        ErrorClass::InvalidConfig => *invalid_config_error = true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn video_index_writer_streams_expected_records() {
        let path = std::env::temp_dir().join(format!(
            "snow-video-index-{}.bin",
            uuid::Uuid::new_v4().simple()
        ));

        let mut writer = VideoIndexWriter::create(&path).unwrap();
        writer.push(10).unwrap();
        writer.push(25).unwrap();
        writer.push(40).unwrap();
        writer.finalize(60, 10).unwrap();

        let bytes = fs::read(&path).unwrap();
        let _ = fs::remove_file(&path);
        assert_eq!(&bytes[..VIDEO_INDEX_MAGIC.len()], VIDEO_INDEX_MAGIC);

        let payload = &bytes[VIDEO_INDEX_MAGIC.len()..];
        assert_eq!(payload.len(), 3 * 20);

        let read_u64 = |offset: usize| {
            let mut raw = [0u8; 8];
            raw.copy_from_slice(&payload[offset..offset + 8]);
            u64::from_le_bytes(raw)
        };
        let read_u32 = |offset: usize| {
            let mut raw = [0u8; 4];
            raw.copy_from_slice(&payload[offset..offset + 4]);
            u32::from_le_bytes(raw)
        };

        assert_eq!(read_u64(0), 0);
        assert_eq!(read_u64(8), 10);
        assert_eq!(read_u32(16), 15);

        assert_eq!(read_u64(20), 1);
        assert_eq!(read_u64(28), 25);
        assert_eq!(read_u32(36), 15);

        assert_eq!(read_u64(40), 2);
        assert_eq!(read_u64(48), 40);
        assert_eq!(read_u32(56), 20);
    }

    #[test]
    fn state_round_trip() {
        for state in [
            RecordingState::Created,
            RecordingState::Running,
            RecordingState::Paused,
            RecordingState::Stopped,
        ] {
            assert_eq!(state, RecordingState::from_u8(state.as_u8()));
        }
    }

    #[test]
    fn unfinished_session_removes_temporary_paths_on_drop() {
        let output = tempfile::tempdir().expect("temporary output should be created");
        let config = RecordingConfig {
            output_dir: output.path().to_path_buf(),
            keep_temp_files: false,
            ..RecordingConfig::default()
        };
        let session =
            RecordingSession::create(config).expect("recording session should be created");
        let session_dir = session.layout.session_dir.clone();
        let bundle_path = session.layout.bundle_path.clone();
        fs::write(&bundle_path, b"partial recording").expect("partial recording should be created");

        drop(session);

        assert!(!session_dir.exists());
        assert!(!bundle_path.exists());
    }

    #[test]
    fn unfinished_session_preserves_temporary_paths_when_requested() {
        let output = tempfile::tempdir().expect("temporary output should be created");
        let config = RecordingConfig {
            output_dir: output.path().to_path_buf(),
            keep_temp_files: true,
            ..RecordingConfig::default()
        };
        let session =
            RecordingSession::create(config).expect("recording session should be created");
        let session_dir = session.layout.session_dir.clone();
        let bundle_path = session.layout.bundle_path.clone();
        fs::write(&bundle_path, b"partial recording").expect("partial recording should be created");

        drop(session);

        assert!(session_dir.exists());
        assert!(bundle_path.exists());
    }

    #[test]
    fn failed_stop_removes_temporary_paths() {
        let output = tempfile::tempdir().expect("temporary output should be created");
        let config = RecordingConfig {
            output_dir: output.path().to_path_buf(),
            keep_temp_files: false,
            ..RecordingConfig::default()
        };
        let session =
            RecordingSession::create(config).expect("recording session should be created");
        let session_dir = session.layout.session_dir.clone();
        let bundle_path = session.layout.bundle_path.clone();
        fs::write(&bundle_path, b"partial recording").expect("partial recording should be created");

        assert!(session.stop().is_err());

        assert!(!session_dir.exists());
        assert!(!bundle_path.exists());
    }
}

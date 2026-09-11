use std::collections::{BTreeMap, HashMap, VecDeque};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, Sender};
use snow_audio_recorder::{
    AudioEvent, AudioFormat, AudioPacket, AudioSession, AudioSourceKind, AudioStreamConfig,
    AudioStreamHandle,
};
use snow_capture::{
    CaptureEvent, CaptureOptions, CaptureStream, CaptureStreamConfig, CaptureSystem,
    CaptureWorkload, CapturedFrame,
};
use snow_core::recording_clock::RecordingClock;
use snow_cursor::{AttachedCursorSample, CursorCompositionMode, CursorShape, CursorShapeState};
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingAudioConfig,
    StreamingEncoder, StreamingEncoderConfig, StreamingEncoderReport, VideoCodec,
    scaled_output_dimensions,
};
use snow_recording_model::{VideoEncodeConfig, VideoEncodingSpeed};

use crate::adapter::video::resolve_capture_target;
use crate::config::{CaptureBackendKind, RecordingRegion, RecordingTarget};
use crate::error::{Result, ScreenRecorderError};
use crate::keyboard_hook::KeyboardInput;
use crate::keyboard_overlay::{KeyEvent, KeyboardOverlay, KeyboardOverlayConfig};
use crate::laser_trail::LaserTrail;
use crate::mouse_hook::{MouseClickObservation, MouseHookObserver};
use crate::recording::RecordingState;

#[cfg(feature = "recording-benchmark")]
#[path = "direct_benchmark.rs"]
pub mod benchmark;

use snow_recording_effects::mouse_effects::{
    CLICK_ANIMATION_MS, CLICK_QUEUE_DEPTH, RenderClick, draw_clicks, scale_coordinate, scale_point,
};
const AUDIO_SAMPLE_RATE: u32 = 48_000;
const AUDIO_CHANNELS: u16 = 2;
const AUDIO_SLOT_MS: u64 = 10;
const AUDIO_JITTER_MS: u64 = 100;

#[derive(Clone, Debug)]
pub struct DirectRecordingConfig {
    pub region: RecordingRegion,
    pub capture_backend: CaptureBackendKind,
    pub output_path: PathBuf,
    pub format: ExportFormat,
    pub capture_fps: u32,
    pub output_fps: u32,
    pub maximum_width: Option<u32>,
    pub maximum_height: Option<u32>,
    pub codec: VideoCodec,
    pub preset: VideoEncodingSpeed,
    pub prefer_hardware_encoder: bool,
    pub enable_microphone: bool,
    pub enable_system_audio: bool,
    pub show_cursor: bool,
    pub keyboard: Option<KeyboardOverlayConfig>,
    pub mouse_trail_rgba: [u8; 4],
    pub mouse_trail_duration_ms: u64,
    pub mouse_click_rgba: [u8; 4],
}

impl DirectRecordingConfig {
    pub fn validate(&self) -> std::result::Result<(), String> {
        if !(100..=2000).contains(&self.mouse_trail_duration_ms) {
            return Err("trail duration must be between 100 and 2000 ms".into());
        }
        if self.region.width == 0 || self.region.height == 0 {
            return Err("direct recording region must have non-zero dimensions".to_string());
        }
        if self.capture_fps == 0 || self.output_fps == 0 {
            return Err("direct recording frame rates must be greater than zero".to_string());
        }
        if self.output_path.as_os_str().is_empty() {
            return Err("direct recording output path must not be empty".to_string());
        }
        if self.maximum_width.is_some() != self.maximum_height.is_some() {
            return Err(
                "direct recording maximum dimensions must both be set or both be unset".to_string(),
            );
        }
        if self.maximum_width == Some(0) || self.maximum_height == Some(0) {
            return Err("direct recording maximum dimensions must be non-zero".to_string());
        }
        let extension = self
            .output_path
            .extension()
            .and_then(|value| value.to_str())
            .unwrap_or_default();
        if !extension.eq_ignore_ascii_case(self.format.file_extension()) {
            return Err(format!(
                "direct recording output extension must be .{}",
                self.format.file_extension()
            ));
        }
        Ok(())
    }

    fn output_dimensions(&self) -> (u32, u32) {
        scaled_output_dimensions(
            self.region.width,
            self.region.height,
            self.maximum_width,
            self.maximum_height,
            self.format,
        )
    }

    fn streaming_config(&self) -> StreamingEncoderConfig {
        let (width, height) = self.output_dimensions();
        StreamingEncoderConfig {
            output_path: self.output_path.clone(),
            format: self.format,
            width,
            height,
            fps: self.output_fps,
            codec: self.codec,
            prefer_hardware_h264: self.prefer_hardware_encoder,
            execution_mode: if self.prefer_hardware_encoder {
                ExportExecutionMode::HardwarePreferred
            } else {
                ExportExecutionMode::SoftwareOnly
            },
            software_h264_priority: SoftwareH264Priority::X264First,
            video: VideoEncodeConfig {
                quality: 80,
                speed: self.preset,
            },
            encode_threads: 0,
            audio: (self.format == ExportFormat::Mp4
                && (self.enable_microphone || self.enable_system_audio))
                .then_some(StreamingAudioConfig {
                    sample_rate_hz: AUDIO_SAMPLE_RATE,
                    channels: AUDIO_CHANNELS,
                    bitrate_kbps: 160,
                }),
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct DirectRecordingReport {
    pub encoded_frames: u64,
    pub coalesced_frames: u64,
    pub dropped_capture_frames: u64,
    pub video_encoder: String,
    pub used_hardware_video_encoder: bool,
    pub encoded_audio_frames: u64,
    pub inserted_silence_frames: u64,
    pub dropped_audio_frames: u64,
}

#[derive(Clone, Copy, Debug)]
enum ControlCommand {
    Pause,
    Resume,
    Stop,
    Cancel,
}

struct RuntimeHandles {
    control_tx: Sender<ControlCommand>,
    worker: JoinHandle<Result<DirectRecordingReport>>,
}

pub struct DirectRecordingSession {
    config: DirectRecordingConfig,
    state: Arc<AtomicU8>,
    runtime: Mutex<Option<RuntimeHandles>>,
}

impl DirectRecordingSession {
    pub fn create(config: DirectRecordingConfig) -> Result<Self> {
        config
            .validate()
            .map_err(ScreenRecorderError::InvalidConfig)?;
        Ok(Self {
            config,
            state: Arc::new(AtomicU8::new(state_to_u8(RecordingState::Created))),
            runtime: Mutex::new(None),
        })
    }

    pub fn start(&mut self) -> Result<()> {
        if self.state() != RecordingState::Created {
            return Err(ScreenRecorderError::InvalidConfig(
                "direct recording can only start from Created state".to_string(),
            ));
        }

        let capture_system = CaptureSystem::builder()
            .with_backend_kind(self.config.capture_backend)
            .build()?;
        let capture_session = capture_system.open_session(
            resolve_capture_target(&RecordingTarget::Region(self.config.region))?,
            CaptureOptions {
                workload: CaptureWorkload::Continuous,
                ..CaptureOptions::default()
            },
        )?;
        let capture_stream = CaptureStream::spawn(
            capture_session,
            CaptureStreamConfig {
                target_fps: self.config.capture_fps,
                min_fps: self.config.output_fps.min(self.config.capture_fps).max(1),
                buffer_depth: 8,
                max_consecutive_errors: 30,
                adaptive_fps: false,
                pause_on_resolution_change: false,
                include_cursor: true,
            },
        )?;
        let (click_tx, click_rx) = crossbeam_channel::bounded(CLICK_QUEUE_DEPTH);
        let mouse_hook = MouseHookObserver::start(
            (
                self.config.region.x,
                self.config.region.y,
                self.config.region.width,
                self.config.region.height,
            ),
            click_tx,
        )
        .map_err(ScreenRecorderError::Encode)?;
        let keyboard_input = self
            .config
            .keyboard
            .as_ref()
            .map(|_| KeyboardInput::start())
            .transpose()
            .map_err(|error| ScreenRecorderError::Encode(format!("keyboard recording: {error}")))?;
        let (control_tx, control_rx) = crossbeam_channel::unbounded();
        let audio_stream = start_optional_audio_stream(&self.config);
        let config = self.config.clone();
        let clock = RecordingClock::new(Instant::now());
        let (ready_tx, ready_rx) = std::sync::mpsc::sync_channel(1);
        let worker_state = Arc::clone(&self.state);
        let worker = std::thread::Builder::new()
            .name("snow-direct-recording".to_string())
            .spawn(move || {
                let result = (|| {
                    let mut compositor = VisualCompositor::new(config.output_dimensions());
                    if let Some(style) = config.keyboard.as_ref() {
                        match crate::keyboard_rasterizer::create(style) {
                            Ok(rasterizer) => {
                                compositor.keyboard = Some(
                                    KeyboardOverlay::new(config.output_dimensions(), rasterizer)
                                        .with_keycap_size(style.keycap_size),
                                )
                            }
                            Err(error) => {
                                let message = format!("keyboard recording: {error}");
                                let _ = ready_tx.send(Err(message.clone()));
                                return Err(ScreenRecorderError::Encode(message));
                            }
                        }
                    }
                    let encoder = match StreamingEncoder::create(config.streaming_config()) {
                        Ok(encoder) => {
                            let _ = ready_tx.send(Ok(()));
                            encoder
                        }
                        Err(error) => {
                            let message = error.to_string();
                            let _ = ready_tx.send(Err(message));
                            return Err(ScreenRecorderError::from(error));
                        }
                    };
                    run_direct_worker(DirectWorkerInputs {
                        config,
                        encoder,
                        capture_stream,
                        mouse_hook,
                        click_rx,
                        keyboard_input,
                        compositor,
                        control_rx,
                        clock,
                        audio_stream,
                    })
                })();
                worker_state.store(state_to_u8(RecordingState::Stopped), Ordering::Release);
                result
            })
            .map_err(|error| ScreenRecorderError::Io(std::io::Error::other(error)))?;
        match ready_rx.recv() {
            Ok(Ok(())) => {}
            Ok(Err(error)) => {
                let _ = worker.join();
                return Err(ScreenRecorderError::Encode(error));
            }
            Err(_) => {
                let _ = worker.join();
                return Err(ScreenRecorderError::Encode(
                    "direct recording worker stopped during initialization".to_string(),
                ));
            }
        }
        *self.runtime.lock().map_err(|_| {
            ScreenRecorderError::InvalidConfig("direct recording runtime lock poisoned".to_string())
        })? = Some(RuntimeHandles { control_tx, worker });
        let _ = self.state.compare_exchange(
            state_to_u8(RecordingState::Created),
            state_to_u8(RecordingState::Running),
            Ordering::AcqRel,
            Ordering::Acquire,
        );
        Ok(())
    }

    pub fn pause(&self) -> Result<()> {
        self.transition(
            RecordingState::Running,
            RecordingState::Paused,
            ControlCommand::Pause,
        )
    }

    pub fn resume(&self) -> Result<()> {
        self.transition(
            RecordingState::Paused,
            RecordingState::Running,
            ControlCommand::Resume,
        )
    }

    pub fn stop(self) -> Result<DirectRecordingReport> {
        let runtime = self
            .runtime
            .lock()
            .map_err(|_| {
                ScreenRecorderError::InvalidConfig(
                    "direct recording runtime lock poisoned".to_string(),
                )
            })?
            .take()
            .ok_or_else(|| {
                ScreenRecorderError::InvalidConfig(
                    "direct recording session was not started".to_string(),
                )
            })?;
        let _ = runtime.control_tx.send(ControlCommand::Stop);
        let result = runtime.worker.join().map_err(|_| {
            ScreenRecorderError::Encode("direct recording worker panicked".to_string())
        })?;
        self.state
            .store(state_to_u8(RecordingState::Stopped), Ordering::Release);
        result
    }

    pub fn state(&self) -> RecordingState {
        state_from_u8(self.state.load(Ordering::Acquire))
    }

    fn transition(
        &self,
        expected: RecordingState,
        next: RecordingState,
        command: ControlCommand,
    ) -> Result<()> {
        if self.state() != expected {
            return Err(ScreenRecorderError::InvalidConfig(format!(
                "direct recording transition requires {expected:?} state"
            )));
        }
        let runtime = self.runtime.lock().map_err(|_| {
            ScreenRecorderError::InvalidConfig("direct recording runtime lock poisoned".to_string())
        })?;
        runtime
            .as_ref()
            .ok_or_else(|| {
                ScreenRecorderError::InvalidConfig(
                    "direct recording runtime is not initialized".to_string(),
                )
            })?
            .control_tx
            .send(command)
            .map_err(|_| {
                ScreenRecorderError::Encode("direct recording worker stopped".to_string())
            })?;
        self.state.store(state_to_u8(next), Ordering::Release);
        Ok(())
    }
}

impl Drop for DirectRecordingSession {
    fn drop(&mut self) {
        let runtime = match self.runtime.get_mut() {
            Ok(runtime) => runtime.take(),
            Err(poisoned) => poisoned.into_inner().take(),
        };
        if let Some(runtime) = runtime {
            let _ = runtime.control_tx.send(ControlCommand::Cancel);
            let _ = runtime.worker.join();
        }
    }
}

fn state_to_u8(state: RecordingState) -> u8 {
    match state {
        RecordingState::Created => 0,
        RecordingState::Running => 1,
        RecordingState::Paused => 2,
        RecordingState::Stopped => 3,
    }
}

fn state_from_u8(value: u8) -> RecordingState {
    match value {
        1 => RecordingState::Running,
        2 => RecordingState::Paused,
        3 => RecordingState::Stopped,
        _ => RecordingState::Created,
    }
}

struct DirectWorkerInputs {
    config: DirectRecordingConfig,
    encoder: StreamingEncoder,
    capture_stream: CaptureStream,
    mouse_hook: MouseHookObserver,
    click_rx: Receiver<MouseClickObservation>,
    keyboard_input: Option<KeyboardInput>,
    compositor: VisualCompositor,
    control_rx: Receiver<ControlCommand>,
    clock: RecordingClock,
    audio_stream: Option<AudioStreamHandle>,
}

fn run_direct_worker(inputs: DirectWorkerInputs) -> Result<DirectRecordingReport> {
    let DirectWorkerInputs {
        config,
        mut encoder,
        capture_stream,
        mouse_hook,
        click_rx,
        mut keyboard_input,
        mut compositor,
        control_rx,
        clock,
        mut audio_stream,
    } = inputs;
    let _mouse_hook = mouse_hook;
    let clock_controller = clock.controller();
    // Initialization can take time; keys used before the worker is ready are not recording input.
    let mut keyboard_since = Instant::now();
    let mut keyboard_generation = 0;
    let mut paused = false;
    let mut stopping = false;
    let mut canceled = false;
    let mut input_end = None;
    let mut dropped_capture_frames = 0u64;
    let mut last_timestamp_ms = None;
    let mut latest_frame = None;
    let output_interval_ms = (1_000 / u64::from(config.output_fps.max(1))).max(1);
    let mut next_overlay_frame_ms = 0u64;
    let mut audio_mixer = encoder
        .has_audio()
        .then(|| LiveAudioMixer::new(config.enable_system_audio, config.enable_microphone));

    while !stopping && !canceled {
        while let Ok(command) = control_rx.try_recv() {
            match command {
                ControlCommand::Pause if !paused => {
                    let at = Instant::now();
                    capture_stream.pause();
                    if let Some(audio) = audio_stream.as_ref() {
                        audio.pause();
                    }
                    reset_keyboard(
                        keyboard_input.as_ref(),
                        &mut compositor,
                        clock.active_elapsed_ms(at),
                    );
                    clock_controller.mark_pause(at);
                    if let Some(mixer) = audio_mixer.as_mut() {
                        mixer.reset_alignment(None);
                    }
                    paused = true;
                }
                ControlCommand::Resume if paused => {
                    let at = Instant::now();
                    clock_controller.mark_resume(at);
                    keyboard_since = at;
                    reset_keyboard(
                        keyboard_input.as_ref(),
                        &mut compositor,
                        clock.active_elapsed_ms(at),
                    );
                    if let Some(audio) = audio_stream.as_ref() {
                        audio.resume();
                    }
                    capture_stream.resume();
                    paused = false;
                }
                ControlCommand::Stop => {
                    input_end = Some(Instant::now());
                    stopping = true;
                }
                ControlCommand::Cancel => {
                    input_end = Some(Instant::now());
                    canceled = true;
                }
                ControlCommand::Pause | ControlCommand::Resume => {}
            }
        }
        drain_click_observations(&click_rx, &clock, paused, &mut compositor);
        if let (Some(input), Some(style)) = (keyboard_input.as_ref(), config.keyboard.as_ref()) {
            let generation = input.generation.load(Ordering::Acquire);
            if keyboard_generation != generation {
                compositor.pending_keys.clear();
                if let Some(keyboard) = compositor.keyboard.as_mut() {
                    keyboard
                        .model
                        .reset(clock.active_elapsed_ms(Instant::now()));
                }
                keyboard_generation = generation;
            }
            while let Ok(event) = input.receiver.try_recv() {
                if paused
                    || event.at < keyboard_since
                    || event.generation != generation
                    || input_end.is_some_and(|end| event.at > end)
                {
                    continue;
                }
                if compositor.pending_keys.len() == 256 {
                    reset_keyboard(
                        Some(input),
                        &mut compositor,
                        clock.active_elapsed_ms(Instant::now()),
                    );
                    break;
                }
                compositor
                    .pending_keys
                    .push_back(event.event(clock.active_elapsed_ms(event.at), style));
            }
        }
        drain_audio_events(audio_stream.as_ref(), &clock, paused, audio_mixer.as_mut());
        if !paused && let Some(mixer) = audio_mixer.as_mut() {
            mixer.emit_ready(
                clock.active_elapsed_duration(Instant::now()),
                false,
                &mut encoder,
            )?;
        }
        if stopping || canceled {
            break;
        }
        match capture_stream.recv_timeout(Duration::from_millis(20)) {
            Ok(event) => process_capture_event(
                event,
                CaptureEventContext {
                    config: &config,
                    clock: &clock,
                    last_timestamp_ms: &mut last_timestamp_ms,
                    latest_frame: &mut latest_frame,
                    dropped_capture_frames: &mut dropped_capture_frames,
                    compositor: &mut compositor,
                    encoder: &mut encoder,
                },
            )?,
            Err(snow_core::error::RecvTimeoutError::Timeout) => {}
            Err(snow_core::error::RecvTimeoutError::Disconnected) => stopping = true,
        }
        if !paused {
            let timestamp_ms = clock.active_elapsed_ms(Instant::now());
            if timestamp_ms >= next_overlay_frame_ms
                && compositor.has_active_animation(&config, timestamp_ms)
                && let Some(frame) = latest_frame.as_ref()
            {
                let timestamp_ms = monotonic_timestamp(&mut last_timestamp_ms, timestamp_ms);
                let rgba = compositor.compose(&config, frame, timestamp_ms)?;
                encoder.push_rgba_frame(timestamp_ms, &rgba)?;
                next_overlay_frame_ms = timestamp_ms.saturating_add(output_interval_ms);
            }
        }
    }

    // Stop observation before potentially expensive encoder draining/finalization.
    drop(keyboard_input.take());
    if canceled {
        capture_stream.stop();
        if let Some(audio) = audio_stream.take() {
            audio.stop();
        }
        return Err(ScreenRecorderError::ExportCanceled);
    }

    for event in capture_stream.stop_and_drain() {
        process_capture_event(
            event,
            CaptureEventContext {
                config: &config,
                clock: &clock,
                last_timestamp_ms: &mut last_timestamp_ms,
                latest_frame: &mut latest_frame,
                dropped_capture_frames: &mut dropped_capture_frames,
                compositor: &mut compositor,
                encoder: &mut encoder,
            },
        )?;
    }
    let final_at = Instant::now();
    clock_controller.finalize(final_at);
    let mut audio_frames_dropped = 0u64;
    if let Some(audio) = audio_stream.take() {
        audio_frames_dropped = audio.stats().snapshot().frames_dropped;
        for event in audio.stop_and_drain() {
            process_audio_event(event, &clock, false, audio_mixer.as_mut());
        }
    }
    if let Some(mixer) = audio_mixer.as_mut() {
        mixer.emit_ready(clock.active_elapsed_duration(final_at), true, &mut encoder)?;
    }
    audio_frames_dropped = audio_frames_dropped
        .saturating_add(audio_mixer.as_ref().map_or(0, |mixer| mixer.dropped_frames));
    let report = encoder.finish()?;
    Ok(report_from_encoder(
        report,
        dropped_capture_frames,
        audio_frames_dropped,
    ))
}

fn report_from_encoder(
    report: StreamingEncoderReport,
    dropped_capture_frames: u64,
    audio_frames_dropped: u64,
) -> DirectRecordingReport {
    DirectRecordingReport {
        encoded_frames: report.encoded_frames,
        coalesced_frames: report.coalesced_frames,
        dropped_capture_frames,
        video_encoder: report.video_encoder,
        used_hardware_video_encoder: report.used_hardware_video_encoder,
        encoded_audio_frames: report.encoded_audio_frames,
        inserted_silence_frames: report.inserted_silence_frames,
        dropped_audio_frames: report
            .dropped_audio_frames
            .saturating_add(audio_frames_dropped),
    }
}

fn start_optional_audio_stream(config: &DirectRecordingConfig) -> Option<AudioStreamHandle> {
    if config.format != ExportFormat::Mp4
        || (!config.enable_system_audio && !config.enable_microphone)
    {
        return None;
    }
    let mut stream_config = AudioStreamConfig::default();
    stream_config.system.enabled = config.enable_system_audio;
    stream_config.system.required = false;
    stream_config.system.output_format = AudioFormat::new(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS);
    stream_config.system.packet_duration = Duration::from_millis(AUDIO_SLOT_MS);
    stream_config.microphone.enabled = config.enable_microphone;
    stream_config.microphone.required = false;
    stream_config.microphone.output_format = AudioFormat::new(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS);
    stream_config.microphone.packet_duration = Duration::from_millis(AUDIO_SLOT_MS);
    stream_config.event_buffer_depth = 64;
    AudioSession::new()
        .and_then(|session| session.start_streaming(stream_config))
        .ok()
}

#[derive(Default)]
struct AudioMixSlot {
    system: Option<Vec<i16>>,
    microphone: Option<Vec<i16>>,
}

struct LiveAudioMixer {
    enabled_system: bool,
    enabled_microphone: bool,
    slot_frames: u64,
    jitter_frames: u64,
    next_slot: u64,
    slots: BTreeMap<u64, AudioMixSlot>,
    dropped_frames: u64,
    next_system_frame: Option<u64>,
    next_microphone_frame: Option<u64>,
}

impl LiveAudioMixer {
    fn new(enabled_system: bool, enabled_microphone: bool) -> Self {
        Self {
            enabled_system,
            enabled_microphone,
            slot_frames: u64::from(AUDIO_SAMPLE_RATE) * AUDIO_SLOT_MS / 1_000,
            jitter_frames: u64::from(AUDIO_SAMPLE_RATE) * AUDIO_JITTER_MS / 1_000,
            next_slot: 0,
            slots: BTreeMap::new(),
            dropped_frames: 0,
            next_system_frame: None,
            next_microphone_frame: None,
        }
    }

    fn reset_alignment(&mut self, source: Option<AudioSourceKind>) {
        if source.is_none_or(|source| source == AudioSourceKind::System) {
            self.next_system_frame = None;
        }
        if source.is_none_or(|source| source == AudioSourceKind::Microphone) {
            self.next_microphone_frame = None;
        }
    }

    fn insert_packet(&mut self, packet: AudioPacket, clock: &RecordingClock) {
        if packet.frames == 0
            || packet.format != AudioFormat::new(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS)
            || packet.data.len() != packet.frames as usize * usize::from(AUDIO_CHANNELS)
        {
            return;
        }
        let enabled = match packet.source {
            AudioSourceKind::System => self.enabled_system,
            AudioSourceKind::Microphone => self.enabled_microphone,
        };
        if !enabled {
            return;
        }
        let Some(started_at) = packet.start_capture_time() else {
            return;
        };
        let next_frame = match packet.source {
            AudioSourceKind::System => &mut self.next_system_frame,
            AudioSourceKind::Microphone => &mut self.next_microphone_frame,
        };
        // Capture instants include device-read scheduling jitter. Preserve PCM continuity
        // between alignment boundaries, as the buffered recording writer does.
        let start_frame = if packet.metadata.discontinuity {
            duration_to_audio_frames(clock.active_elapsed_duration(started_at))
        } else {
            next_frame.unwrap_or_else(|| {
                duration_to_audio_frames(clock.active_elapsed_duration(started_at))
            })
        };
        *next_frame = Some(start_frame.saturating_add(u64::from(packet.frames)));
        self.insert_samples(
            packet.source,
            start_frame,
            packet.frames,
            &packet.data,
            clock.active_elapsed_duration(Instant::now()),
        );
    }

    fn insert_samples(
        &mut self,
        source: AudioSourceKind,
        start_frame: u64,
        frames: u32,
        data: &[i16],
        active_elapsed: Duration,
    ) {
        let channels = usize::from(AUDIO_CHANNELS);
        // The recording worker drains captured audio before emitting it. Overlay rendering
        // or video encoding can stall that worker, leaving next_slot behind valid queued
        // packets. Bound future timestamps against the recording clock, not encoder progress.
        let maximum_slot = (duration_to_audio_frames(active_elapsed) / self.slot_frames)
            .saturating_add(self.jitter_frames.div_ceil(self.slot_frames))
            .saturating_add(2);
        for source_frame in 0..u64::from(frames) {
            let timeline_frame = start_frame.saturating_add(source_frame);
            let slot_index = timeline_frame / self.slot_frames;
            if slot_index < self.next_slot || slot_index > maximum_slot {
                self.dropped_frames = self.dropped_frames.saturating_add(1);
                continue;
            }
            let frame_in_slot = (timeline_frame % self.slot_frames) as usize;
            let slot_samples = self.slot_frames as usize * channels;
            let source_offset = source_frame as usize * channels;
            let destination_offset = frame_in_slot * channels;
            let slot = self.slots.entry(slot_index).or_default();
            let destination = match source {
                AudioSourceKind::System => slot.system.get_or_insert_with(|| vec![0; slot_samples]),
                AudioSourceKind::Microphone => {
                    slot.microphone.get_or_insert_with(|| vec![0; slot_samples])
                }
            };
            destination[destination_offset..destination_offset + channels]
                .copy_from_slice(&data[source_offset..source_offset + channels]);
        }
    }

    fn emit_ready(
        &mut self,
        active_elapsed: Duration,
        flush: bool,
        encoder: &mut StreamingEncoder,
    ) -> Result<()> {
        let active_frame = duration_to_audio_frames(active_elapsed);
        let release_frame = if flush {
            active_frame
        } else {
            active_frame.saturating_sub(self.jitter_frames)
        };
        loop {
            let slot_start = self.next_slot.saturating_mul(self.slot_frames);
            let slot_end = slot_start.saturating_add(self.slot_frames);
            let ready = if flush {
                slot_start < release_frame
            } else {
                slot_end <= release_frame
            };
            if !ready {
                break;
            }
            let slot_index = self.next_slot;
            let slot = self.slots.remove(&slot_index).unwrap_or_default();
            let samples = mix_audio_slot(
                slot,
                self.slot_frames as usize * usize::from(AUDIO_CHANNELS),
            );
            let timestamp_ms = slot_index.saturating_mul(AUDIO_SLOT_MS);
            encoder.push_audio_pcm_i16(timestamp_ms, &samples)?;
            self.next_slot = self.next_slot.saturating_add(1);
        }
        Ok(())
    }
}

fn duration_to_audio_frames(duration: Duration) -> u64 {
    ((duration.as_nanos() * u128::from(AUDIO_SAMPLE_RATE) + 500_000_000) / 1_000_000_000)
        .min(u128::from(u64::MAX)) as u64
}

fn mix_audio_slot(slot: AudioMixSlot, sample_count: usize) -> Vec<i16> {
    let mut mixed = vec![0i16; sample_count];
    for source in [slot.system, slot.microphone].into_iter().flatten() {
        for (destination, sample) in mixed.iter_mut().zip(source) {
            *destination = i32::from(*destination)
                .saturating_add(i32::from(sample))
                .clamp(i32::from(i16::MIN), i32::from(i16::MAX)) as i16;
        }
    }
    mixed
}

fn drain_audio_events(
    audio: Option<&AudioStreamHandle>,
    clock: &RecordingClock,
    paused: bool,
    mixer: Option<&mut LiveAudioMixer>,
) {
    let (Some(audio), Some(mixer)) = (audio, mixer) else {
        return;
    };
    while let Ok(event) = audio.try_recv() {
        process_audio_event(event, clock, paused, Some(&mut *mixer));
    }
}

fn process_audio_event(
    event: AudioEvent,
    clock: &RecordingClock,
    paused: bool,
    mixer: Option<&mut LiveAudioMixer>,
) {
    let Some(mixer) = mixer else {
        return;
    };
    match event {
        AudioEvent::Packet(packet) if !paused => mixer.insert_packet(packet, clock),
        AudioEvent::Paused { .. } | AudioEvent::Resumed { .. } => mixer.reset_alignment(None),
        AudioEvent::SourceRestarted { source, .. } | AudioEvent::PacketDropped { source, .. } => {
            mixer.reset_alignment(Some(source))
        }
        _ => {}
    }
}

struct CaptureEventContext<'a> {
    config: &'a DirectRecordingConfig,
    clock: &'a RecordingClock,
    last_timestamp_ms: &'a mut Option<u64>,
    latest_frame: &'a mut Option<CapturedFrame>,
    dropped_capture_frames: &'a mut u64,
    compositor: &'a mut VisualCompositor,
    encoder: &'a mut StreamingEncoder,
}

fn process_capture_event(event: CaptureEvent, context: CaptureEventContext<'_>) -> Result<()> {
    let CaptureEventContext {
        config,
        clock,
        last_timestamp_ms,
        latest_frame,
        dropped_capture_frames,
        compositor,
        encoder,
    } = context;
    match event {
        CaptureEvent::Frame(frame) => {
            let instant = frame
                .metadata()
                .stream_timestamp()
                .map(|timestamp| timestamp.instant)
                .unwrap_or_else(Instant::now);
            let timestamp_ms =
                monotonic_timestamp(last_timestamp_ms, clock.active_elapsed_ms(instant));
            let rgba = compositor.compose(config, &frame, timestamp_ms)?;
            encoder.push_rgba_frame(timestamp_ms, &rgba)?;
            *latest_frame = Some(frame);
        }
        CaptureEvent::FramesDropped { count, .. } => {
            *dropped_capture_frames = dropped_capture_frames.saturating_add(u64::from(count));
        }
        CaptureEvent::Error(error) => return Err(ScreenRecorderError::Capture(error)),
        CaptureEvent::Paused { .. }
        | CaptureEvent::Resumed { .. }
        | CaptureEvent::ResolutionChanged { .. }
        | CaptureEvent::StreamEnded => {}
    }
    Ok(())
}

fn monotonic_timestamp(last: &mut Option<u64>, candidate: u64) -> u64 {
    let value = candidate.max(last.unwrap_or(0));
    *last = Some(value);
    value
}

fn reset_keyboard(input: Option<&KeyboardInput>, compositor: &mut VisualCompositor, now: u64) {
    if let Some(input) = input {
        input.reset();
    }
    compositor.pending_keys.clear();
    if let Some(keyboard) = compositor.keyboard.as_mut() {
        keyboard.model.reset(now);
    }
}

fn drain_click_observations(
    receiver: &Receiver<MouseClickObservation>,
    clock: &RecordingClock,
    paused: bool,
    compositor: &mut VisualCompositor,
) {
    while let Ok(observation) = receiver.try_recv() {
        if paused {
            continue;
        }
        if compositor.clicks.len() >= CLICK_QUEUE_DEPTH {
            compositor.clicks.pop_front();
        }
        compositor.clicks.push_back(RenderClick {
            timestamp_ms: clock.active_elapsed_ms(observation.at),
            x: observation.x,
            y: observation.y,
            button: observation.button,
        });
    }
}

struct VisualCompositor {
    output_size: (u32, u32),
    trail: LaserTrail,
    clicks: VecDeque<RenderClick>,
    cursor_shapes: HashMap<u64, CursorShape>,
    keyboard: Option<KeyboardOverlay>,
    pending_keys: VecDeque<KeyEvent>,
}

impl VisualCompositor {
    fn new(output_size: (u32, u32)) -> Self {
        Self {
            output_size,
            trail: LaserTrail::default(),
            clicks: VecDeque::new(),
            cursor_shapes: HashMap::new(),
            keyboard: None,
            pending_keys: VecDeque::new(),
        }
    }

    fn compose(
        &mut self,
        config: &DirectRecordingConfig,
        frame: &CapturedFrame,
        timestamp_ms: u64,
    ) -> Result<Vec<u8>> {
        self.trail.set_lifetime_ms(config.mouse_trail_duration_ms);
        let source_size = frame.dimensions();
        let mut rgba = resize_rgba(frame.as_rgba_bytes(), source_size, self.output_size);
        let cursor = frame.metadata().cursor().cloned();
        if config.mouse_trail_rgba[3] != 0 {
            self.trail.observe(
                cursor
                    .as_ref()
                    .filter(|cursor| cursor.visible)
                    .map(|cursor| (cursor.x, cursor.y)),
                source_size,
                self.output_size,
                timestamp_ms,
            );
        } else {
            self.trail.clear();
        }
        while self.clicks.front().is_some_and(|click| {
            timestamp_ms.saturating_sub(click.timestamp_ms) > CLICK_ANIMATION_MS
        }) {
            self.clicks.pop_front();
        }

        if config.mouse_trail_rgba[3] != 0 {
            self.trail.draw(
                &mut rgba,
                self.output_size,
                timestamp_ms,
                config.mouse_trail_rgba,
            );
        }
        if config.mouse_click_rgba[3] != 0 {
            draw_clicks(
                &mut rgba,
                self.output_size,
                &self.clicks,
                timestamp_ms,
                config.mouse_click_rgba,
                source_size,
            );
        }
        if config.show_cursor
            && let Some(cursor) = cursor.as_ref()
        {
            draw_cursor(
                &mut rgba,
                self.output_size,
                source_size,
                cursor,
                &mut self.cursor_shapes,
            );
        }
        if let Some(keyboard) = self.keyboard.as_mut() {
            while self
                .pending_keys
                .front()
                .is_some_and(|event| event.at_ms <= timestamp_ms)
            {
                keyboard
                    .model
                    .event(self.pending_keys.pop_front().expect("pending key"));
            }
            keyboard.draw(&mut rgba, timestamp_ms).map_err(|error| {
                ScreenRecorderError::Encode(format!("keyboard recording: {error}"))
            })?;
        }
        Ok(rgba)
    }

    /// Compose with an explicit cursor sample; the deterministic benchmark
    /// replays synthetic captures whose cursors are not in frame metadata.
    /// Mirrors `compose`; keep the two in sync.
    #[cfg(feature = "recording-benchmark")]
    fn compose_with_cursor(
        &mut self,
        config: &DirectRecordingConfig,
        frame: &CapturedFrame,
        cursor: Option<&AttachedCursorSample>,
        timestamp_ms: u64,
    ) -> Result<Vec<u8>> {
        self.trail.set_lifetime_ms(config.mouse_trail_duration_ms);
        let source_size = frame.dimensions();
        let mut rgba = resize_rgba(frame.as_rgba_bytes(), source_size, self.output_size);
        if config.mouse_trail_rgba[3] != 0 {
            self.trail.observe(
                cursor
                    .filter(|cursor| cursor.visible)
                    .map(|cursor| (cursor.x, cursor.y)),
                source_size,
                self.output_size,
                timestamp_ms,
            );
        } else {
            self.trail.clear();
        }
        while self.clicks.front().is_some_and(|click| {
            timestamp_ms.saturating_sub(click.timestamp_ms) > CLICK_ANIMATION_MS
        }) {
            self.clicks.pop_front();
        }

        if config.mouse_trail_rgba[3] != 0 {
            self.trail.draw(
                &mut rgba,
                self.output_size,
                timestamp_ms,
                config.mouse_trail_rgba,
            );
        }
        if config.mouse_click_rgba[3] != 0 {
            draw_clicks(
                &mut rgba,
                self.output_size,
                &self.clicks,
                timestamp_ms,
                config.mouse_click_rgba,
                source_size,
            );
        }
        if config.show_cursor
            && let Some(cursor) = cursor
        {
            draw_cursor(
                &mut rgba,
                self.output_size,
                source_size,
                cursor,
                &mut self.cursor_shapes,
            );
        }
        if let Some(keyboard) = self.keyboard.as_mut() {
            while self
                .pending_keys
                .front()
                .is_some_and(|event| event.at_ms <= timestamp_ms)
            {
                keyboard
                    .model
                    .event(self.pending_keys.pop_front().expect("pending key"));
            }
            keyboard.draw(&mut rgba, timestamp_ms).map_err(|error| {
                ScreenRecorderError::Encode(format!("keyboard recording: {error}"))
            })?;
        }
        Ok(rgba)
    }

    fn has_active_animation(&self, config: &DirectRecordingConfig, timestamp_ms: u64) -> bool {
        let trail_active =
            config.mouse_trail_rgba[3] > 0 && self.trail.has_active_animation(timestamp_ms);
        let click_active = config.mouse_click_rgba[3] > 0
            && self.clicks.back().is_some_and(|click| {
                timestamp_ms.saturating_sub(click.timestamp_ms) <= CLICK_ANIMATION_MS
            });
        trail_active
            || click_active
            || self
                .pending_keys
                .front()
                .is_some_and(|event| event.at_ms <= timestamp_ms)
            || self
                .keyboard
                .as_ref()
                .is_some_and(|keyboard| keyboard.model.needs_frame(timestamp_ms))
    }
}

fn resize_rgba(source: &[u8], source_size: (u32, u32), output_size: (u32, u32)) -> Vec<u8> {
    let (source_width, source_height) = source_size;
    let (output_width, output_height) = output_size;
    if source_size == output_size {
        return source.to_vec();
    }
    let mut output = vec![0u8; output_width as usize * output_height as usize * 4];
    for y in 0..output_height {
        let source_y = (u64::from(y) * u64::from(source_height) / u64::from(output_height)) as u32;
        for x in 0..output_width {
            let source_x =
                (u64::from(x) * u64::from(source_width) / u64::from(output_width)) as u32;
            let source_index = (source_y as usize * source_width as usize + source_x as usize) * 4;
            let output_index = (y as usize * output_width as usize + x as usize) * 4;
            if source_index + 4 <= source.len() {
                output[output_index..output_index + 4]
                    .copy_from_slice(&source[source_index..source_index + 4]);
            }
        }
    }
    output
}

fn draw_cursor(
    rgba: &mut [u8],
    output_size: (u32, u32),
    source_size: (u32, u32),
    cursor: &AttachedCursorSample,
    shapes: &mut HashMap<u64, CursorShape>,
) {
    let shape = match &cursor.shape {
        CursorShapeState::Embedded(shape) => {
            shapes.insert(shape.shape_id.get(), shape.clone());
            Some(shape)
        }
        CursorShapeState::Cached(shape_id) => shapes.get(&shape_id.get()),
        CursorShapeState::Unavailable => None,
    };
    let Some(shape) = shape else {
        return;
    };
    let (cursor_x, cursor_y) = scale_point(cursor.x, cursor.y, source_size, output_size);
    let scaled_width = ((u64::from(shape.width) * u64::from(output_size.0))
        / u64::from(source_size.0.max(1)))
    .max(1) as u32;
    let scaled_height = ((u64::from(shape.height) * u64::from(output_size.1))
        / u64::from(source_size.1.max(1)))
    .max(1) as u32;
    let hotspot_x = scale_coordinate(
        shape.hotspot_x.min(i32::MAX as u32) as i32,
        source_size.0,
        output_size.0,
    );
    let hotspot_y = scale_coordinate(
        shape.hotspot_y.min(i32::MAX as u32) as i32,
        source_size.1,
        output_size.1,
    );
    let origin_x = cursor_x.saturating_sub(hotspot_x);
    let origin_y = cursor_y.saturating_sub(hotspot_y);
    for y in 0..scaled_height {
        let source_y = (u64::from(y) * u64::from(shape.height) / u64::from(scaled_height)) as u32;
        for x in 0..scaled_width {
            let source_x = (u64::from(x) * u64::from(shape.width) / u64::from(scaled_width)) as u32;
            let index = (source_y as usize * shape.width as usize + source_x as usize) * 4;
            if index + 4 > shape.shape_rgba.len() {
                continue;
            }
            composite_cursor_pixel(
                rgba,
                output_size,
                origin_x.saturating_add(x as i32),
                origin_y.saturating_add(y as i32),
                [
                    shape.shape_rgba[index],
                    shape.shape_rgba[index + 1],
                    shape.shape_rgba[index + 2],
                    shape.shape_rgba[index + 3],
                ],
                shape.composition_mode,
            );
        }
    }
}

fn composite_cursor_pixel(
    rgba: &mut [u8],
    size: (u32, u32),
    x: i32,
    y: i32,
    color: [u8; 4],
    mode: CursorCompositionMode,
) {
    match (mode, color[3]) {
        (CursorCompositionMode::AlphaBlend, _) | (CursorCompositionMode::MaskedColor, 1..=254) => {
            blend_pixel(rgba, size, x, y, color)
        }
        (CursorCompositionMode::MaskedColor, 0 | 255) => {
            if x < 0 || y < 0 || x as u32 >= size.0 || y as u32 >= size.1 {
                return;
            }
            let index = (y as usize * size.0 as usize + x as usize) * 4;
            if index + 4 > rgba.len() || color == [0, 0, 0, 255] {
                return;
            }
            // Masked cursor alpha stores the AND mask, not opacity: zero copies
            // the color, while 255 XORs it with the existing background.
            for channel in 0..3 {
                rgba[index + channel] = (rgba[index + channel] & color[3]) ^ color[channel];
            }
            rgba[index + 3] = 255;
        }
    }
}

fn blend_pixel(rgba: &mut [u8], size: (u32, u32), x: i32, y: i32, color: [u8; 4]) {
    if x < 0 || y < 0 || x as u32 >= size.0 || y as u32 >= size.1 || color[3] == 0 {
        return;
    }
    let index = (y as usize * size.0 as usize + x as usize) * 4;
    if index + 4 > rgba.len() {
        return;
    }
    let alpha = u16::from(color[3]);
    let inverse = 255u16.saturating_sub(alpha);
    for channel in 0..3 {
        rgba[index + channel] =
            ((u16::from(color[channel]) * alpha + u16::from(rgba[index + channel]) * inverse + 127)
                / 255) as u8;
    }
    rgba[index + 3] = 255;
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mouse_hook::ObservedMouseButton;
    use snow_cursor::CursorShapeId;

    fn config() -> DirectRecordingConfig {
        DirectRecordingConfig {
            region: RecordingRegion::new(0, 0, 4, 4),
            capture_backend: CaptureBackendKind::Auto,
            output_path: PathBuf::from("recording.mp4"),
            format: ExportFormat::Mp4,
            capture_fps: 30,
            output_fps: 30,
            maximum_width: None,
            maximum_height: None,
            codec: VideoCodec::H264,
            preset: VideoEncodingSpeed::VeryFast,
            prefer_hardware_encoder: false,
            enable_microphone: false,
            enable_system_audio: false,
            show_cursor: true,
            keyboard: None,
            mouse_trail_rgba: [0, 0, 0, 0],
            mouse_trail_duration_ms: 500,
            mouse_click_rgba: [0, 0, 0, 0],
        }
    }

    #[test]
    fn direct_config_rejects_incomplete_caps_and_wrong_extension() {
        let mut value = config();
        value.maximum_width = Some(1920);
        assert!(value.validate().is_err());
        value.maximum_height = Some(1080);
        assert!(value.validate().is_ok());
        value.output_path = PathBuf::from("recording.gif");
        assert!(value.validate().is_err());
    }

    #[test]
    fn source_coordinates_scale_into_output_space() {
        assert_eq!(scale_point(100, 50, (200, 100), (100, 50)), (50, 25));
        let source = vec![255u8; 4 * 4 * 4];
        assert_eq!(resize_rgba(&source, (4, 4), (2, 2)).len(), 16);
    }

    struct KeyboardTestRasterizer;
    impl crate::keyboard_overlay::KeycapRasterizer for KeyboardTestRasterizer {
        fn rasterize(
            &mut self,
            _: &str,
            _: f32,
        ) -> std::result::Result<crate::keyboard_overlay::Keycap, String> {
            Ok(crate::keyboard_overlay::Keycap {
                width: 40,
                height: 20,
                pixels: [200, 0, 0, 255].repeat(800),
            })
        }
    }

    #[test]
    fn keyboard_compositor_waits_for_event_time_and_emits_final_clean_static_frame() {
        let config = config();
        let size = (320, 180);
        let original = [20, 40, 60, 255].repeat(size.0 as usize * size.1 as usize);
        let frame: CapturedFrame =
            snow_capture::frame::Frame::from_rgba8(size.0, size.1, original.clone())
                .unwrap()
                .into();
        let mut compositor = VisualCompositor::new(size);
        compositor.keyboard = Some(KeyboardOverlay::new(size, Box::new(KeyboardTestRasterizer)));
        for (at_ms, down) in [(100, true), (200, false)] {
            compositor.pending_keys.push_back(KeyEvent {
                at_ms,
                key: 65,
                down,
                label: "A".into(),
                modifiers: vec![],
            });
        }
        assert!(!compositor.has_active_animation(&config, 50));
        assert_eq!(compositor.compose(&config, &frame, 50).unwrap(), original);
        assert!(compositor.has_active_animation(&config, 100));
        assert_ne!(compositor.compose(&config, &frame, 300).unwrap(), original);
        assert!(
            !compositor.has_active_animation(&config, 1000),
            "retention is stationary"
        );
        assert!(
            compositor.has_active_animation(&config, 1500),
            "fade schedules on static desktop"
        );
        assert!(
            compositor.has_active_animation(&config, 1800),
            "expiration still owes a clean frame"
        );
        assert_eq!(compositor.compose(&config, &frame, 1800).unwrap(), original);
        assert!(!compositor.has_active_animation(&config, 1800));
    }

    #[test]
    fn keyboard_pause_discards_queued_input_and_freezes_history_on_recording_clock() {
        let config = config();
        let size = (320, 180);
        let original = [20, 40, 60, 255].repeat(size.0 as usize * size.1 as usize);
        let frame: CapturedFrame =
            snow_capture::frame::Frame::from_rgba8(size.0, size.1, original.clone())
                .unwrap()
                .into();
        let mut compositor = VisualCompositor::new(size);
        compositor.keyboard = Some(KeyboardOverlay::new(size, Box::new(KeyboardTestRasterizer)));
        compositor.pending_keys.push_back(KeyEvent {
            at_ms: 0,
            key: 65,
            down: true,
            label: "A".into(),
            modifiers: vec![],
        });
        compositor.compose(&config, &frame, 0).unwrap();
        compositor.pending_keys.push_back(KeyEvent {
            at_ms: 210,
            key: 66,
            down: true,
            label: "B".into(),
            modifiers: vec![],
        });
        reset_keyboard(None, &mut compositor, 200);
        assert!(compositor.pending_keys.is_empty());
        let started = Instant::now();
        let clock = RecordingClock::new(started);
        clock
            .controller()
            .mark_pause(started + Duration::from_millis(200));
        clock
            .controller()
            .mark_resume(started + Duration::from_millis(5200));
        let active = clock.active_elapsed_ms(started + Duration::from_millis(5300));
        assert_eq!(active, 300);
        assert_ne!(
            compositor.compose(&config, &frame, active).unwrap(),
            original
        );
        assert_eq!(compositor.compose(&config, &frame, 1800).unwrap(), original);
    }

    #[test]
    fn transparent_and_visible_overlays_respect_alpha() {
        let mut rgba = vec![0u8; 8 * 8 * 4];
        blend_pixel(&mut rgba, (8, 8), 2, 2, [255, 0, 0, 0]);
        assert_eq!(rgba[(2 * 8 + 2) * 4], 0);
        blend_pixel(&mut rgba, (8, 8), 2, 2, [255, 0, 0, 128]);
        assert!(rgba[(2 * 8 + 2) * 4] > 0);
    }

    #[test]
    fn cursor_is_composed_last_when_visible() {
        let shape = CursorShape {
            shape_id: CursorShapeId::from_raw(1),
            hotspot_x: 0,
            hotspot_y: 0,
            width: 1,
            height: 1,
            composition_mode: CursorCompositionMode::AlphaBlend,
            shape_rgba: vec![0, 255, 0, 255].into(),
        };
        let cursor = AttachedCursorSample {
            x: 1,
            y: 1,
            visible: true,
            shape: CursorShapeState::Embedded(shape),
        };
        let mut rgba = vec![0u8; 2 * 2 * 4];
        let mut shapes = HashMap::new();
        draw_cursor(&mut rgba, (2, 2), (2, 2), &cursor, &mut shapes);
        let index = (2 + 1) * 4;
        assert_eq!(&rgba[index..index + 4], &[0, 255, 0, 255]);
    }

    #[test]
    fn cursor_composition_respects_mask_operations_and_alpha() {
        let cases = [
            (CursorCompositionMode::MaskedColor, [0, 0, 0, 255]),
            (CursorCompositionMode::MaskedColor, [0, 0, 0, 0]),
            (CursorCompositionMode::MaskedColor, [255, 255, 255, 0]),
            (CursorCompositionMode::MaskedColor, [255, 255, 255, 255]),
            (CursorCompositionMode::MaskedColor, [53, 170, 204, 0]),
            (CursorCompositionMode::MaskedColor, [53, 170, 204, 255]),
            (CursorCompositionMode::MaskedColor, [53, 170, 204, 128]),
            (CursorCompositionMode::AlphaBlend, [53, 170, 204, 0]),
            (CursorCompositionMode::AlphaBlend, [53, 170, 204, 128]),
            (CursorCompositionMode::AlphaBlend, [53, 170, 204, 255]),
        ];
        for background in [[37, 91, 163, 255], [255, 255, 255, 255], [0, 0, 0, 255]] {
            for (mode, pixel) in cases {
                let mut expected = background;
                if mode == CursorCompositionMode::MaskedColor && matches!(pixel[3], 0 | 255) {
                    // Windows masked cursors apply (destination AND mask) XOR color.
                    for channel in 0..3 {
                        expected[channel] = (background[channel] & pixel[3]) ^ pixel[channel];
                    }
                } else {
                    for channel in 0..3 {
                        expected[channel] = ((u32::from(pixel[channel]) * u32::from(pixel[3])
                            + u32::from(background[channel]) * (255 - u32::from(pixel[3]))
                            + 127)
                            / 255) as u8;
                    }
                }
                let shape = CursorShape::from_rgba(0, 0, 1, 1, mode, pixel.to_vec());
                let shape_id = shape.shape_id;
                let mut shapes = HashMap::new();
                for state in [
                    CursorShapeState::Embedded(shape),
                    CursorShapeState::Cached(shape_id),
                ] {
                    let cursor = AttachedCursorSample {
                        x: 0,
                        y: 0,
                        visible: true,
                        shape: state,
                    };
                    for output_size in [(1, 1), (2, 2)] {
                        let count = (output_size.0 * output_size.1) as usize;
                        let mut rgba = background.repeat(count);
                        draw_cursor(&mut rgba, output_size, (1, 1), &cursor, &mut shapes);
                        assert_eq!(
                            rgba,
                            expected.repeat(count),
                            "{mode:?}, {pixel:?}, {background:?}"
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn masked_cursor_preserves_background_around_visible_pixels_when_clipped() {
        let background = [37, 91, 163, 255];
        let mut pixels = [0, 0, 0, 255].repeat(9);
        pixels[16..20].copy_from_slice(&[255, 255, 255, 255]);
        let cursor = AttachedCursorSample {
            x: 0,
            y: 0,
            visible: true,
            shape: CursorShapeState::Embedded(CursorShape::from_rgba(
                1,
                1,
                3,
                3,
                CursorCompositionMode::MaskedColor,
                pixels,
            )),
        };
        let mut rgba = background.repeat(4);
        let mut expected = rgba.clone();
        expected[..4].copy_from_slice(&[218, 164, 92, 255]);
        draw_cursor(&mut rgba, (2, 2), (2, 2), &cursor, &mut HashMap::new());
        assert_eq!(rgba, expected);
    }

    #[test]
    fn live_audio_mixer_preserves_backlog_after_recording_worker_stalls() {
        // Overlay composition/encoding can delay the consumer while capture continues.
        // All packets here fit in the capture queue, and none has been emitted yet.
        let started_at = Instant::now() - Duration::from_secs(1);
        let clock = RecordingClock::new(started_at);
        for sources in [
            vec![AudioSourceKind::System],
            vec![AudioSourceKind::Microphone],
            vec![AudioSourceKind::System, AudioSourceKind::Microphone],
        ] {
            let mut mixer = LiveAudioMixer::new(true, true);
            for source in &sources {
                for index in 0..20 {
                    mixer.insert_packet(
                        test_audio_packet(
                            *source,
                            started_at + Duration::from_millis((index + 1) * 10),
                            index + 1,
                            vec![index as i16 + 1; 960],
                        ),
                        &clock,
                    );
                }
            }
            assert_eq!(mixer.dropped_frames, 0, "queued PCM must not be discarded");
            for index in 0..20 {
                let actual = mix_audio_slot(mixer.slots.remove(&index).unwrap_or_default(), 960);
                assert_eq!(actual, vec![(index as i16 + 1) * sources.len() as i16; 960]);
            }
        }
    }

    #[test]
    fn live_audio_mixer_accepts_delayed_start_but_never_rewrites_emitted_audio() {
        let mut mixer = LiveAudioMixer::new(true, false);
        let samples = vec![1000; 960];
        let elapsed = Duration::from_millis(500);
        // A source can initialize after the worker starts, including at final drain.
        mixer.insert_samples(AudioSourceKind::System, 23_520, 480, &samples, elapsed);
        assert_eq!(mixer.dropped_frames, 0);
        assert_eq!(
            mix_audio_slot(mixer.slots.remove(&49).unwrap(), 960),
            samples
        );

        mixer.next_slot = 50;
        mixer.insert_samples(AudioSourceKind::System, 23_520, 480, &samples, elapsed);
        assert!(mixer.slots.is_empty());
        assert_eq!(mixer.dropped_frames, 480);

        // A stalled consumer must not allow genuinely future timestamps either.
        mixer.insert_samples(AudioSourceKind::System, 48_000, 480, &samples, elapsed);
        assert!(mixer.slots.is_empty());
        assert_eq!(mixer.dropped_frames, 960);
    }

    #[test]
    fn live_audio_mixer_preserves_contiguous_samples_despite_timestamp_jitter() {
        for source in [AudioSourceKind::System, AudioSourceKind::Microphone] {
            let started_at = Instant::now();
            let clock = RecordingClock::new(started_at);
            let mut mixer = LiveAudioMixer::new(true, true);
            let mut expected = Vec::new();
            for (index, end_us) in [10_000, 20_500, 29_500].into_iter().enumerate() {
                let data: Vec<i16> = (0..960)
                    .map(|sample| (index * 960 + sample + 1) as i16)
                    .collect();
                expected.extend_from_slice(&data);
                mixer.insert_packet(
                    test_audio_packet(
                        source,
                        started_at + Duration::from_micros(end_us),
                        index as u64 + 1,
                        data,
                    ),
                    &clock,
                );
            }
            let actual: Vec<i16> = (0..3)
                .flat_map(|slot| mix_audio_slot(mixer.slots.remove(&slot).unwrap_or_default(), 960))
                .collect();
            let mismatches = actual.iter().zip(&expected).filter(|(a, b)| a != b).count();
            assert_eq!(
                mismatches, 0,
                "{source:?}: contiguous PCM must survive timestamp jitter"
            );
        }
    }

    fn test_audio_packet(
        source: AudioSourceKind,
        end: Instant,
        sequence: u64,
        data: Vec<i16>,
    ) -> AudioPacket {
        AudioPacket {
            source,
            format: AudioFormat::new(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS),
            frames: (data.len() / usize::from(AUDIO_CHANNELS)) as u32,
            data,
            metadata: snow_audio_recorder::AudioPacketMetadata {
                sequence,
                stream_timestamp: Some(snow_core::timestamp::StreamTimestamp {
                    instant: end,
                    raw_os_ticks: None,
                    tick_format: snow_core::timestamp::TickFormat::Hns100,
                }),
                ..Default::default()
            },
        }
    }

    #[test]
    fn live_audio_mixer_realigns_at_stream_boundaries() {
        let started_at = Instant::now();
        let clock = RecordingClock::new(started_at);
        for event in [
            AudioEvent::PacketDropped {
                source: AudioSourceKind::System,
                dropped_frames: 480,
            },
            AudioEvent::SourceRestarted {
                source: AudioSourceKind::System,
                old_device_id: None,
                new_device_id: "test".to_string(),
                downtime: Duration::from_millis(10),
            },
            AudioEvent::Paused { at: started_at },
            AudioEvent::Resumed {
                at: started_at,
                gap: Duration::from_millis(10),
            },
        ] {
            let mut mixer = LiveAudioMixer::new(true, true);
            mixer.insert_packet(
                test_audio_packet(
                    AudioSourceKind::System,
                    started_at + Duration::from_millis(10),
                    1,
                    vec![1000; 960],
                ),
                &clock,
            );
            process_audio_event(event, &clock, true, Some(&mut mixer));
            mixer.insert_packet(
                test_audio_packet(
                    AudioSourceKind::System,
                    started_at + Duration::from_millis(30),
                    2,
                    vec![2000; 960],
                ),
                &clock,
            );
            assert!(
                !mixer.slots.contains_key(&1),
                "a real gap must remain silent"
            );
            assert_eq!(
                mix_audio_slot(mixer.slots.remove(&2).unwrap(), 960),
                vec![2000; 960]
            );
        }
    }

    #[test]
    fn live_audio_mixer_keeps_source_positions_independent_and_realigns_discontinuities() {
        let started_at = Instant::now();
        let clock = RecordingClock::new(started_at);
        let mut mixer = LiveAudioMixer::new(true, true);
        mixer.insert_packet(
            test_audio_packet(
                AudioSourceKind::System,
                started_at + Duration::from_millis(10),
                1,
                vec![1000; 960],
            ),
            &clock,
        );
        mixer.insert_packet(
            test_audio_packet(
                AudioSourceKind::Microphone,
                started_at + Duration::from_millis(20),
                1,
                vec![2000; 960],
            ),
            &clock,
        );
        let mut packet = test_audio_packet(
            AudioSourceKind::System,
            started_at + Duration::from_millis(30),
            2,
            vec![3000; 960],
        );
        packet.metadata.discontinuity = true;
        mixer.insert_packet(packet, &clock);
        assert_eq!(
            mix_audio_slot(mixer.slots.remove(&0).unwrap(), 960),
            vec![1000; 960]
        );
        assert_eq!(
            mix_audio_slot(mixer.slots.remove(&1).unwrap(), 960),
            vec![2000; 960]
        );
        assert_eq!(
            mix_audio_slot(mixer.slots.remove(&2).unwrap(), 960),
            vec![3000; 960]
        );
    }

    #[test]
    fn live_audio_mixer_saturates_sources_and_bounds_future_slots() {
        let mut mixer = LiveAudioMixer::new(true, true);
        let samples = vec![24_000i16; 480 * 2];
        mixer.insert_samples(AudioSourceKind::System, 0, 480, &samples, Duration::ZERO);
        mixer.insert_samples(
            AudioSourceKind::Microphone,
            0,
            480,
            &samples,
            Duration::ZERO,
        );
        let slot = mixer.slots.remove(&0).unwrap();
        let mixed = mix_audio_slot(slot, 480 * 2);
        assert!(mixed.iter().all(|sample| *sample == i16::MAX));

        mixer.insert_samples(
            AudioSourceKind::System,
            48_000,
            480,
            &samples,
            Duration::ZERO,
        );
        assert!(mixer.slots.is_empty());
        assert_eq!(mixer.dropped_frames, 480);
    }

    #[test]
    fn overlay_scheduler_stays_active_for_trail_and_click_decay() {
        let mut value = config();
        value.mouse_trail_rgba = [255, 0, 0, 255];
        value.mouse_click_rgba = [0, 255, 0, 128];
        let mut compositor = VisualCompositor::new((4, 4));
        compositor.trail.observe(Some((1, 1)), (4, 4), (4, 4), 0);
        compositor.trail.observe(Some((2, 1)), (4, 4), (4, 4), 100);
        assert!(compositor.has_active_animation(&value, 599));
        assert!(!compositor.has_active_animation(&value, 600));
        compositor.trail.clear();
        compositor.clicks.push_back(RenderClick {
            timestamp_ms: 500,
            x: 1,
            y: 1,
            button: ObservedMouseButton::Left,
        });
        assert!(compositor.has_active_animation(&value, 950));
        assert!(!compositor.has_active_animation(&value, 951));
    }

    #[test]
    fn click_animation_history_remains_bounded_before_the_first_frame() {
        let (sender, receiver) = crossbeam_channel::unbounded();
        let started_at = Instant::now();
        let clock = RecordingClock::new(started_at);
        for index in 0..CLICK_QUEUE_DEPTH * 3 {
            sender
                .send(MouseClickObservation {
                    at: started_at + Duration::from_millis(index as u64),
                    x: index as i32,
                    y: 0,
                    button: ObservedMouseButton::Left,
                })
                .unwrap();
        }
        let mut compositor = VisualCompositor::new((4, 4));
        drain_click_observations(&receiver, &clock, false, &mut compositor);
        assert_eq!(compositor.clicks.len(), CLICK_QUEUE_DEPTH);
        assert_eq!(
            compositor.clicks.front().map(|click| click.x),
            Some((CLICK_QUEUE_DEPTH * 2) as i32)
        );
    }
}

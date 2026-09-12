use std::collections::{BTreeMap, HashMap, VecDeque};
use std::path::PathBuf;
#[cfg(feature = "bench-synthetic-input")]
use std::sync::atomic::AtomicU64;
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
use snow_recording_export::resize::NearestResizePlan;
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingAudioConfig,
    StreamingEncoder, StreamingEncoderConfig, StreamingEncoderReport, VideoCodec,
    scaled_output_dimensions,
};
use snow_recording_model::{VideoEncodeConfig, VideoEncodingSpeed};

use crate::adapter::video::resolve_capture_target;
#[cfg(any(feature = "bench-stage-timing", feature = "bench-compositor-timing"))]
use crate::bench_timing::StageHistogram;
#[cfg(feature = "bench-pipeline-timing")]
use crate::bench_timing::{CapturePipelineStats, PipelineTimings};
use crate::config::{CaptureBackendKind, RecordingRegion, RecordingTarget};
use crate::error::{Result, ScreenRecorderError};
#[cfg(feature = "bench-synthetic-input")]
use crate::keyboard_hook::KeyObservation;
use crate::keyboard_hook::KeyboardInput;
use crate::keyboard_overlay::{KeyEvent, KeyboardOverlay, KeyboardOverlayConfig};
use crate::laser_trail::LaserTrail;
use crate::mouse_hook::MouseClickObservation;
use crate::mouse_hook::MouseHookObserver;
#[cfg(feature = "bench-synthetic-input")]
use crate::mouse_hook::ObservedMouseButton;
use crate::recording::RecordingState;

use snow_recording_effects::mouse_effects::{
    CLICK_ANIMATION_MS, CLICK_QUEUE_DEPTH, RenderClick, draw_clicks, scale_coordinate, scale_point,
};
const AUDIO_SAMPLE_RATE: u32 = 48_000;
const AUDIO_CHANNELS: u16 = 2;
const AUDIO_SLOT_MS: u64 = 10;
const AUDIO_JITTER_MS: u64 = 100;
/// Synthetic cursor queue depth; the worker drains once per output slot, so
/// this only needs to absorb brief worker stalls (`bench-synthetic-input`).
#[cfg(feature = "bench-synthetic-input")]
const BENCH_CURSOR_QUEUE_DEPTH: usize = 64;

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
            encode_threads: self.automatic_encode_threads(),
            audio: (self.format == ExportFormat::Mp4
                && (self.enable_microphone || self.enable_system_audio))
                .then_some(StreamingAudioConfig {
                    sample_rate_hz: AUDIO_SAMPLE_RATE,
                    channels: AUDIO_CHANNELS,
                    bitrate_kbps: 160,
                }),
        }
    }

    fn automatic_encode_threads(&self) -> u8 {
        let (width, height) = self.output_dimensions();
        // Two threads reduce the measured live H.264 working set without losing
        // cadence. Keep other rates, sizes, speeds, codecs, and hardware on the
        // encoder's existing automatic policy until they have equivalent evidence.
        if self.format != ExportFormat::Mp4
            || self.codec != VideoCodec::H264
            || self.prefer_hardware_encoder
            || self.preset != VideoEncodingSpeed::VeryFast
            || self.output_fps > 30
            || u64::from(width) * u64::from(height) > 1920 * 1080
        {
            return 0;
        }
        let logical = std::thread::available_parallelism().map_or(1, |value| value.get());
        let physical = num_cpus::get_physical();
        let available = if physical == 0 {
            logical
        } else {
            physical.min(logical)
        };
        available.clamp(1, 2) as u8
    }

    fn automatic_resize_threads(&self) -> u8 {
        let output = self.output_dimensions();
        // The live 4K-to-1080p comparison supports two row workers in this
        // software encoding budget. Native and small images stay serial.
        if self.automatic_encode_threads() == 2
            && std::thread::available_parallelism().is_ok_and(|value| value.get() >= 4)
            && output != (self.region.width, self.region.height)
            && u64::from(output.0) * u64::from(output.1) >= 1_000_000
        {
            2
        } else {
            0
        }
    }

    fn aligned_capture(&self) -> bool {
        // The combined clock/thread/resize/owned-conversion gate passed for
        // matched 30 fps downscaled DXGI/Auto recording. Keep other workloads
        // on their existing acquisition clock until separately measured.
        self.capture_fps == self.output_fps
            && self.output_fps == 30
            && self.automatic_resize_threads() == 2
            && matches!(
                self.capture_backend,
                CaptureBackendKind::Auto | CaptureBackendKind::DxgiDuplication
            )
    }
}

#[derive(Clone, Debug, Default)]
pub struct DirectRecordingReport {
    #[cfg(feature = "bench-pipeline-timing")]
    pub encoder_timings: snow_recording_export::bench_timing::EncoderTimings,
    pub encoded_frames: u64,
    pub superseded_capture_frames: u64,
    pub missed_output_slots: u64,
    pub coalesced_frames: u64,
    pub dropped_capture_frames: u64,
    pub video_encoder: String,
    pub used_hardware_video_encoder: bool,
    pub encoded_audio_frames: u64,
    pub inserted_silence_frames: u64,
    pub dropped_audio_frames: u64,
    /// Per-backend capture stage breakdown (`bench-stage-timing` builds only).
    #[cfg(feature = "bench-stage-timing")]
    pub capture_stage_timings: Option<StageHistogram>,
    /// Overlay compositing stage breakdown (`bench-compositor-timing` builds only).
    #[cfg(feature = "bench-compositor-timing")]
    pub compositor_timings: Option<StageHistogram>,
    /// Capture-to-encode latency summary (`bench-pipeline-timing` builds only).
    #[cfg(feature = "bench-pipeline-timing")]
    pub pipeline: Option<CapturePipelineStats>,
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
    /// Synthetic overlay input senders (`bench-synthetic-input` builds only).
    #[cfg(feature = "bench-synthetic-input")]
    synthetic: Option<BenchSyntheticInput>,
}

pub struct DirectRecordingSession {
    config: DirectRecordingConfig,
    encode_threads: u8,
    resize_threads: u8,
    align_capture: bool,
    #[cfg(feature = "bench-synthetic-input")]
    bench_synthetic_input: bool,
    state: Arc<AtomicU8>,
    runtime: Mutex<Option<RuntimeHandles>>,
}

impl DirectRecordingSession {
    pub fn create(config: DirectRecordingConfig) -> Result<Self> {
        config
            .validate()
            .map_err(ScreenRecorderError::InvalidConfig)?;
        let resize_threads = config.automatic_resize_threads();
        let align_capture = config.aligned_capture();
        Ok(Self {
            config,
            encode_threads: 0,
            resize_threads,
            align_capture,
            #[cfg(feature = "bench-synthetic-input")]
            bench_synthetic_input: false,
            state: Arc::new(AtomicU8::new(state_to_u8(RecordingState::Created))),
            runtime: Mutex::new(None),
        })
    }

    /// Override encoder worker count before startup; zero keeps automatic selection.
    /// This Rust-only control leaves the recording settings and C ABI unchanged.
    pub fn set_encode_threads(&mut self, threads: u8) -> Result<()> {
        if self.state() != RecordingState::Created {
            return Err(ScreenRecorderError::InvalidConfig(
                "encoder threads must be configured before recording starts".into(),
            ));
        }
        self.encode_threads = threads;
        Ok(())
    }

    /// Override whether capture acquisition is aligned to this session's output clock.
    pub fn set_aligned_capture(&mut self, enabled: bool) -> Result<()> {
        if self.state() != RecordingState::Created {
            return Err(ScreenRecorderError::InvalidConfig(
                "capture pacing must be configured before recording starts".into(),
            ));
        }
        self.align_capture = enabled;
        Ok(())
    }

    /// Configure an optional bounded resize pool before recording starts.
    /// Zero or one retains serial pixel selection; at most four row workers are allowed.
    pub fn set_resize_threads(&mut self, threads: u8) -> Result<()> {
        if self.state() != RecordingState::Created || threads > 4 {
            return Err(ScreenRecorderError::InvalidConfig(
                "resize workers must be configured before recording and cannot exceed four".into(),
            ));
        }
        self.resize_threads = threads;
        Ok(())
    }

    /// Bench-only (`bench-synthetic-input` builds): drive the overlay inputs
    /// with synthetic observations instead of OS input injection. The hooks
    /// stay installed; only the event source changes.
    #[cfg(feature = "bench-synthetic-input")]
    pub fn set_bench_synthetic_input(&mut self, enabled: bool) -> Result<()> {
        if self.state() != RecordingState::Created {
            return Err(ScreenRecorderError::InvalidConfig(
                "synthetic bench input must be configured before recording starts".into(),
            ));
        }
        self.bench_synthetic_input = enabled;
        Ok(())
    }

    /// Feed a synthetic cursor position (region-relative) to the trail and
    /// cursor overlays without moving the physical pointer.
    #[cfg(feature = "bench-synthetic-input")]
    pub fn bench_observe_cursor(&self, x: i32, y: i32) -> Result<()> {
        self.with_bench_synthetic(|synthetic| {
            synthetic
                .cursor
                .try_send((Instant::now(), x, y))
                .map_err(|error| ScreenRecorderError::Encode(format!("synthetic cursor: {error}")))
        })
    }

    /// Feed a synthetic left-button click (region-relative) to the click
    /// overlay without pressing the physical mouse.
    #[cfg(feature = "bench-synthetic-input")]
    pub fn bench_observe_click(&self, x: i32, y: i32) -> Result<()> {
        self.with_bench_synthetic(|synthetic| {
            synthetic
                .clicks
                .try_send(MouseClickObservation {
                    at: Instant::now(),
                    x,
                    y,
                    button: ObservedMouseButton::Left,
                })
                .map_err(|error| ScreenRecorderError::Encode(format!("synthetic click: {error}")))
        })
    }

    /// Feed a synthetic key edge to the keyboard overlay without emitting a
    /// physical keystroke. Modifier state is tracked across calls.
    #[cfg(feature = "bench-synthetic-input")]
    pub fn bench_observe_key(&self, key: u16, down: bool) -> Result<()> {
        self.with_bench_synthetic(|synthetic| synthetic.observe_key(key, down))
    }

    #[cfg(feature = "bench-synthetic-input")]
    fn with_bench_synthetic(
        &self,
        observe: impl FnOnce(&mut BenchSyntheticInput) -> Result<()>,
    ) -> Result<()> {
        let mut runtime = self.runtime.lock().map_err(|_| {
            ScreenRecorderError::InvalidConfig("direct recording runtime lock poisoned".to_string())
        })?;
        let Some(synthetic) = runtime
            .as_mut()
            .and_then(|handles| handles.synthetic.as_mut())
        else {
            return Err(ScreenRecorderError::InvalidConfig(
                "synthetic bench input requires a started session that opted in".into(),
            ));
        };
        observe(synthetic)
    }

    pub fn start(&mut self) -> Result<()> {
        if self.state() != RecordingState::Created {
            return Err(ScreenRecorderError::InvalidConfig(
                "direct recording can only start from Created state".to_string(),
            ));
        }

        let resize_pool = if self.resize_threads > 1 {
            Some(
                rayon::ThreadPoolBuilder::new()
                    .num_threads(usize::from(self.resize_threads))
                    .build()
                    .map_err(|error| {
                        ScreenRecorderError::Encode(format!("resize pool: {error}"))
                    })?,
            )
        } else {
            None
        };
        let capture_system = CaptureSystem::builder()
            .with_backend_kind(self.config.capture_backend)
            .with_auto_backend_policy(crate::recording::recording_auto_backend_policy(
                crate::recording::RecordingCapturePath::Direct,
            ))
            .build()?;
        let capture_session = capture_system.open_session(
            resolve_capture_target(&RecordingTarget::Region(self.config.region))?,
            CaptureOptions {
                workload: CaptureWorkload::Continuous,
                #[cfg(feature = "bench-stage-timing")]
                record_stage_timings: true,
                ..CaptureOptions::default()
            },
        )?;
        let capture_stream = CaptureStream::spawn(
            capture_session,
            CaptureStreamConfig {
                target_fps: self.config.capture_fps,
                min_fps: self.config.output_fps.min(self.config.capture_fps).max(1),
                buffer_depth: 2,
                max_consecutive_errors: 30,
                adaptive_fps: false,
                pause_on_resolution_change: false,
                include_cursor: true,
            },
        )?;
        let (click_tx, click_rx) = crossbeam_channel::bounded(CLICK_QUEUE_DEPTH);
        #[cfg(feature = "bench-synthetic-input")]
        let bench_click_tx = self.bench_synthetic_input.then(|| click_tx.clone());
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
        #[cfg(feature = "bench-synthetic-input")]
        let (keyboard_input, bench_keyboard_tx, bench_keyboard_generation) =
            match (self.config.keyboard.is_some(), self.bench_synthetic_input) {
                (true, true) => {
                    let (input, sender) =
                        KeyboardInput::start_with_synthetic_sender().map_err(|error| {
                            ScreenRecorderError::Encode(format!("keyboard recording: {error}"))
                        })?;
                    let generation = Arc::clone(&input.generation);
                    (Some(input), Some(sender), Some(generation))
                }
                (true, false) => (
                    Some(KeyboardInput::start().map_err(|error| {
                        ScreenRecorderError::Encode(format!("keyboard recording: {error}"))
                    })?),
                    None,
                    None,
                ),
                (false, _) => (None, None, None),
            };
        #[cfg(not(feature = "bench-synthetic-input"))]
        let keyboard_input = self
            .config
            .keyboard
            .as_ref()
            .map(|_| KeyboardInput::start())
            .transpose()
            .map_err(|error| ScreenRecorderError::Encode(format!("keyboard recording: {error}")))?;
        #[cfg(feature = "bench-synthetic-input")]
        let (bench_cursor_tx, bench_cursor_rx) = if self.bench_synthetic_input {
            let (sender, receiver) = crossbeam_channel::bounded(BENCH_CURSOR_QUEUE_DEPTH);
            (Some(sender), Some(receiver))
        } else {
            (None, None)
        };
        let (control_tx, control_rx) = crossbeam_channel::unbounded();
        let audio_stream = start_optional_audio_stream(&self.config);
        let config = self.config.clone();
        let encode_threads = self.encode_threads;
        let align_capture = self.align_capture;
        let clock = RecordingClock::new(Instant::now());
        let (ready_tx, ready_rx) = std::sync::mpsc::sync_channel(1);
        let worker_state = Arc::clone(&self.state);
        let worker = std::thread::Builder::new()
            .name("snow-direct-recording".to_string())
            .spawn(move || {
                let result = (|| {
                    let mut compositor = VisualCompositor::new(config.output_dimensions());
                    compositor.resize_pool = resize_pool;
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
                    let mut streaming_config = config.streaming_config();
                    if encode_threads != 0 {
                        streaming_config.encode_threads = encode_threads;
                    }
                    let encoder = match StreamingEncoder::create(streaming_config) {
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
                        align_capture,
                        capture_stream,
                        mouse_hook,
                        click_rx,
                        keyboard_input,
                        compositor,
                        control_rx,
                        clock,
                        audio_stream,
                        #[cfg(feature = "bench-synthetic-input")]
                        bench_cursor_rx,
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
        #[cfg(feature = "bench-synthetic-input")]
        let synthetic = self.bench_synthetic_input.then(|| BenchSyntheticInput {
            clicks: bench_click_tx.expect("synthetic clicks sender"),
            // Absent when the session records without a keyboard overlay.
            keys: bench_keyboard_tx,
            generation: bench_keyboard_generation,
            cursor: bench_cursor_tx.expect("synthetic cursor sender"),
            pressed: [0; 256],
            layout: bench_keyboard_layout(),
        });
        *self.runtime.lock().map_err(|_| {
            ScreenRecorderError::InvalidConfig("direct recording runtime lock poisoned".to_string())
        })? = Some(RuntimeHandles {
            control_tx,
            worker,
            #[cfg(feature = "bench-synthetic-input")]
            synthetic,
        });
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

/// Sender side of a started session's synthetic overlay inputs
/// (`bench-synthetic-input` builds only). Coordinates are region-relative,
/// matching what the low-level hooks report. Nothing here injects OS input:
/// the mouse and keyboard devices are never touched.
#[cfg(feature = "bench-synthetic-input")]
struct BenchSyntheticInput {
    clicks: Sender<MouseClickObservation>,
    /// Absent when the session records without a keyboard overlay.
    keys: Option<Sender<KeyObservation>>,
    /// Current keyboard generation so synthetic events pass the worker's
    /// overflow rejection check without forcing a reset.
    generation: Option<Arc<AtomicU64>>,
    cursor: Sender<(Instant, i32, i32)>,
    /// Synthetic modifier state mirrored from the observed key edges.
    pressed: [u8; 256],
    layout: usize,
}

#[cfg(feature = "bench-synthetic-input")]
impl BenchSyntheticInput {
    fn observe_key(&mut self, key: u16, down: bool) -> Result<()> {
        let Some(keys) = self.keys.as_ref() else {
            return Err(ScreenRecorderError::InvalidConfig(
                "synthetic keys require an enabled keyboard overlay".into(),
            ));
        };
        if usize::from(key) >= self.pressed.len() {
            return Err(ScreenRecorderError::InvalidConfig(format!(
                "virtual key {key:#04x} is outside the synthetic keyboard state range"
            )));
        }
        self.pressed[usize::from(key)] = u8::from(down) * 0x80;
        let observation = KeyObservation {
            at: Instant::now(),
            key,
            scan: bench_scan_code(key, self.layout),
            down,
            layout: self.layout,
            pressed: self.pressed,
            alt_gr: false,
            generation: self
                .generation
                .as_deref()
                .map_or(0, |generation| generation.load(Ordering::Acquire)),
        };
        keys.try_send(observation)
            .map_err(|error| ScreenRecorderError::Encode(format!("synthetic key: {error}")))
    }
}

#[cfg(all(windows, feature = "bench-synthetic-input"))]
fn bench_keyboard_layout() -> usize {
    unsafe { windows::Win32::UI::Input::KeyboardAndMouse::GetKeyboardLayout(0) }.0 as usize
}

#[cfg(all(not(windows), feature = "bench-synthetic-input"))]
fn bench_keyboard_layout() -> usize {
    0
}

#[cfg(all(windows, feature = "bench-synthetic-input"))]
fn bench_scan_code(key: u16, layout: usize) -> u32 {
    use windows::Win32::UI::Input::KeyboardAndMouse::{HKL, MAPVK_VK_TO_VSC_EX, MapVirtualKeyExW};
    unsafe {
        MapVirtualKeyExW(
            u32::from(key),
            MAPVK_VK_TO_VSC_EX,
            Some(HKL(layout as *mut _)),
        )
    }
}

#[cfg(all(not(windows), feature = "bench-synthetic-input"))]
fn bench_scan_code(_key: u16, _layout: usize) -> u32 {
    0
}

/// Deterministic wedge cursor drawn by the overlays when synthetic cursor
/// positions replace the capture-attached samples.
#[cfg(feature = "bench-synthetic-input")]
fn synthetic_arrow_cursor_shape() -> CursorShape {
    const SIZE: u32 = 24;
    const EDGE: i32 = 2;
    let mut rgba = Vec::with_capacity((SIZE * SIZE * 4) as usize);
    for y in 0..SIZE {
        for x in 0..SIZE {
            let (x, y) = (x as i32, y as i32);
            let pixel = if x <= y {
                [0, 0, 0, 255]
            } else if x <= y + EDGE {
                [255, 255, 255, 255]
            } else {
                [0, 0, 0, 0]
            };
            rgba.extend_from_slice(&pixel);
        }
    }
    CursorShape::from_rgba(0, 0, SIZE, SIZE, CursorCompositionMode::AlphaBlend, rgba)
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
    align_capture: bool,
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
    /// Synthetic cursor positions (`bench-synthetic-input` builds only).
    #[cfg(feature = "bench-synthetic-input")]
    bench_cursor_rx: Option<Receiver<(Instant, i32, i32)>>,
}

fn run_direct_worker(inputs: DirectWorkerInputs) -> Result<DirectRecordingReport> {
    let DirectWorkerInputs {
        align_capture,
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
        #[cfg(feature = "bench-synthetic-input")]
        bench_cursor_rx,
    } = inputs;
    let _mouse_hook = mouse_hook;
    // Synthetic cursor positions replace the capture-attached samples so the
    // trail and cursor overlays follow the benchmark's sweep, not the
    // physical pointer (`bench-synthetic-input` builds only).
    #[cfg(feature = "bench-synthetic-input")]
    let bench_cursor = bench_cursor_rx.is_some();
    #[cfg(not(feature = "bench-synthetic-input"))]
    let bench_cursor = false;
    #[cfg(feature = "bench-synthetic-input")]
    let bench_cursor_shape = bench_cursor.then(synthetic_arrow_cursor_shape);
    let clock_controller = clock.controller();
    if align_capture {
        capture_stream.set_pacing_origin(Some(clock.started_at()));
    }
    // Initialization can take time; keys used before the worker is ready are not recording input.
    let mut keyboard_since = Instant::now();
    let mut keyboard_generation = 0;
    let mut paused = false;
    let mut stopping = false;
    let mut canceled = false;
    let mut input_end = None;
    let mut dropped_capture_frames = 0u64;
    let mut captures = CaptureInbox::default();
    let mut cursors = CursorInbox::default();
    let mut latest_cursor = None;
    let mut latest_frame = None;
    let mut fresh_since = clock.started_at();
    let mut schedule = crate::output_schedule::OutputSchedule::new(config.output_fps);
    let mut overlay_was_active = false;
    #[cfg(feature = "bench-stage-timing")]
    let mut capture_stage_timings = StageHistogram::default();
    #[cfg(feature = "bench-pipeline-timing")]
    let mut pipeline_timings = PipelineTimings::default();
    #[cfg(feature = "bench-pipeline-timing")]
    pipeline_timings.begin();

    let mut audio_mixer = encoder
        .has_audio()
        .then(|| LiveAudioMixer::new(config.enable_system_audio, config.enable_microphone));

    while !stopping && !canceled {
        while let Ok(command) = control_rx.try_recv() {
            match command {
                ControlCommand::Pause if !paused => {
                    let at = Instant::now();
                    captures.clear();
                    cursors.frames.clear();
                    latest_cursor = None;
                    latest_frame = None;
                    compositor.background_sequence = None;
                    compositor.clicks.clear();
                    compositor.trail.clear();
                    overlay_was_active = false;
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
                    if align_capture {
                        capture_stream
                            .set_pacing_origin(at.checked_sub(clock.active_elapsed_duration(at)));
                    }
                    keyboard_since = at;
                    fresh_since = at;
                    captures.clear();
                    cursors.frames.clear();
                    latest_cursor = None;
                    latest_frame = None;
                    compositor.background_sequence = None;
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
        drain_click_observations(&click_rx, &clock, paused, fresh_since, &mut compositor);
        #[cfg(feature = "bench-synthetic-input")]
        if let (Some(receiver), Some(shape)) =
            (bench_cursor_rx.as_ref(), bench_cursor_shape.as_ref())
            && !paused
        {
            while let Ok((at, x, y)) = receiver.try_recv() {
                cursors.push(
                    at,
                    AttachedCursorSample {
                        x,
                        y,
                        visible: true,
                        shape: CursorShapeState::Embedded(shape.clone()),
                    },
                );
            }
        }
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
        let elapsed = clock.active_elapsed_duration(Instant::now());
        let wait = if paused {
            Duration::from_millis(10)
        } else {
            schedule.wait(elapsed)
        };
        let first = match capture_stream.recv_timeout(wait) {
            Ok(event) => Some(event),
            Err(snow_core::error::RecvTimeoutError::Timeout) => None,
            Err(snow_core::error::RecvTimeoutError::Disconnected) => {
                stopping = true;
                None
            }
        };
        for event in first
            .into_iter()
            .chain(std::iter::from_fn(|| capture_stream.try_recv().ok()))
        {
            match event {
                CaptureEvent::Frame(frame) => {
                    #[cfg(feature = "bench-stage-timing")]
                    for stage in frame.metadata().stage_timings() {
                        capture_stage_timings.record(stage.name, stage.duration);
                    }
                    let received = Instant::now();
                    #[cfg(feature = "bench-pipeline-timing")]
                    pipeline_timings.observe_capture(&frame, received);
                    if !paused
                        && frame
                            .metadata()
                            .observation_started_at()
                            .unwrap_or_else(|| frame_instant(&frame))
                            >= fresh_since
                    {
                        if let Some(cursor) = frame.metadata().cursor().filter(|_| !bench_cursor) {
                            cursors.push(
                                frame.metadata().queued_at().unwrap_or(received),
                                cursor.clone(),
                            );
                        }
                        captures.push(frame, received);
                    }
                }
                CaptureEvent::FramesDropped { count, .. } => {
                    dropped_capture_frames += u64::from(count)
                }
                CaptureEvent::Error(error) => return Err(ScreenRecorderError::Capture(error)),
                CaptureEvent::StreamEnded => stopping = true,
                _ => {}
            }
        }
        if paused || stopping {
            continue;
        }
        let Some(slot) = schedule.poll(clock.active_elapsed_duration(Instant::now())) else {
            continue;
        };
        let new_frame = captures.select(&clock, slot.at);
        let mut changed = false;
        if let Some(cursor) = cursors.select(&clock, slot.at) {
            changed = (config.show_cursor || config.mouse_trail_rgba[3] != 0)
                && latest_cursor.as_ref() != Some(&cursor);
            latest_cursor = Some(cursor);
        }
        if let Some((frame, _received)) = new_frame {
            changed |= latest_frame
                .as_ref()
                .is_none_or(|previous: &CapturedFrame| {
                    !frame.metadata().is_duplicate()
                        || previous.metadata().content_generation()
                            != frame.metadata().content_generation()
                });
            latest_frame = Some(frame);
        }
        let timestamp_ms = slot.at.as_millis() as u64;
        let active = compositor.has_active_animation(&config, timestamp_ms);
        if (changed || active || overlay_was_active)
            && let Some(frame) = latest_frame.as_ref()
        {
            let compose_started = Instant::now();
            let rgba = compositor.compose_with_cursor(
                &config,
                frame,
                timestamp_ms,
                latest_cursor.as_ref(),
            )?;
            #[cfg(feature = "bench-pipeline-timing")]
            let composed = Instant::now();
            compositor.rgba = encoder.push_owned_rgba_frame_at_pts(slot.pts, rgba)?;
            #[cfg(feature = "bench-pipeline-timing")]
            {
                pipeline_timings.observe_frame(
                    frame_instant(frame),
                    compose_started,
                    composed,
                    Instant::now(),
                );
                pipeline_timings.output_sources.push((
                    slot.pts,
                    frame.metadata().sequence(),
                    frame
                        .metadata()
                        .content_generation()
                        .unwrap_or(frame.metadata().sequence()),
                    clock
                        .active_elapsed_duration(frame_instant(frame))
                        .as_nanos()
                        .min(u128::from(u64::MAX)) as u64,
                ));
                if !changed {
                    pipeline_timings.synthetic_overlay_frames += 1;
                }
            }
            let _ = compose_started;
            overlay_was_active = active;
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

    let final_at = input_end.unwrap_or_else(Instant::now);
    let endpoint = schedule.endpoint(clock.active_elapsed_duration(final_at));
    clock_controller.finalize(final_at);
    #[cfg(feature = "bench-pipeline-timing")]
    let stream_stats = capture_stream.stats().snapshot();
    // Captures after the accepted stop boundary must not extend the recording.
    for event in capture_stream.stop_and_drain() {
        if let CaptureEvent::Error(error) = event {
            return Err(ScreenRecorderError::Capture(error));
        }
    }
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
    let report = encoder.finish_at_pts(endpoint)?;
    let mut report = report_from_encoder(
        report,
        dropped_capture_frames,
        audio_frames_dropped,
        #[cfg(feature = "bench-stage-timing")]
        Some(std::mem::take(&mut capture_stage_timings)),
        #[cfg(feature = "bench-compositor-timing")]
        Some(compositor.timings.clone()),
        #[cfg(feature = "bench-pipeline-timing")]
        Some(pipeline_timings.stats(&stream_stats)),
    );
    report.superseded_capture_frames = captures.superseded;
    report.missed_output_slots = schedule.missed_slots;
    Ok(report)
}

fn report_from_encoder(
    report: StreamingEncoderReport,
    dropped_capture_frames: u64,
    audio_frames_dropped: u64,
    #[cfg(feature = "bench-stage-timing")] capture_stage_timings: Option<StageHistogram>,
    #[cfg(feature = "bench-compositor-timing")] compositor_timings: Option<StageHistogram>,
    #[cfg(feature = "bench-pipeline-timing")] pipeline: Option<CapturePipelineStats>,
) -> DirectRecordingReport {
    DirectRecordingReport {
        #[cfg(feature = "bench-pipeline-timing")]
        encoder_timings: report.timings,
        encoded_frames: report.encoded_frames,
        superseded_capture_frames: 0,
        missed_output_slots: 0,
        coalesced_frames: report.coalesced_frames,
        dropped_capture_frames,
        video_encoder: report.video_encoder,
        used_hardware_video_encoder: report.used_hardware_video_encoder,
        encoded_audio_frames: report.encoded_audio_frames,
        inserted_silence_frames: report.inserted_silence_frames,
        dropped_audio_frames: report
            .dropped_audio_frames
            .saturating_add(audio_frames_dropped),
        #[cfg(feature = "bench-stage-timing")]
        capture_stage_timings,
        #[cfg(feature = "bench-compositor-timing")]
        compositor_timings,
        #[cfg(feature = "bench-pipeline-timing")]
        pipeline,
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
        if !clock.is_active_at(started_at) {
            return;
        }
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
                (if flush {
                    self.slot_frames
                        .min(release_frame.saturating_sub(slot_start))
                } else {
                    self.slot_frames
                }) as usize
                    * usize::from(AUDIO_CHANNELS),
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

fn frame_instant(frame: &CapturedFrame) -> Instant {
    frame
        .metadata()
        .stream_timestamp()
        .map(|stamp| stamp.instant)
        .unwrap_or_else(Instant::now)
}

#[derive(Default)]
struct CaptureInbox {
    frames: VecDeque<(CapturedFrame, Instant, Instant)>,
    superseded: u64,
}

impl CaptureInbox {
    fn clear(&mut self) {
        self.frames.clear();
    }

    fn push(&mut self, frame: CapturedFrame, received: Instant) {
        // Keep enough history to select the most recent frame at the output deadline,
        // including one frame that arrived just after it. No unbounded backlog.
        if self.frames.len() == 2 {
            self.frames.pop_front();
            self.superseded += 1;
        }
        let at = frame_instant(&frame);
        self.frames.push_back((frame, received, at));
    }

    fn select(&mut self, clock: &RecordingClock, at: Duration) -> Option<(CapturedFrame, Instant)> {
        let mut selected = None;
        while self
            .frames
            .front()
            .is_some_and(|(_, _, captured)| clock.active_elapsed_duration(*captured) <= at)
        {
            if selected.is_some() {
                self.superseded += 1;
            }
            selected = self
                .frames
                .pop_front()
                .map(|(frame, received, _)| (frame, received));
        }
        selected
    }
}

/// Cursor observations have a later sampling time than their desktop images.
/// Keep their deadlines independent so fresh desktop pixels need not be delayed.
#[derive(Default)]
struct CursorInbox {
    frames: VecDeque<(Instant, AttachedCursorSample)>,
}
impl CursorInbox {
    fn push(&mut self, at: Instant, cursor: AttachedCursorSample) {
        if self.frames.len() == 2 {
            self.frames.pop_front();
        }
        self.frames.push_back((at, cursor));
    }
    fn select(&mut self, clock: &RecordingClock, at: Duration) -> Option<AttachedCursorSample> {
        let mut selected = None;
        while self
            .frames
            .front()
            .is_some_and(|(observed, _)| clock.active_elapsed_duration(*observed) <= at)
        {
            selected = self.frames.pop_front().map(|(_, cursor)| cursor);
        }
        selected
    }
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
    fresh_since: Instant,
    compositor: &mut VisualCompositor,
) {
    while let Ok(observation) = receiver.try_recv() {
        if paused || observation.at < fresh_since || !clock.is_active_at(observation.at) {
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
    background: Vec<u8>,
    rgba: Vec<u8>,
    resize_plan: Option<NearestResizePlan>,
    resize_pool: Option<rayon::ThreadPool>,
    background_sequence: Option<u64>,
    trail: LaserTrail,
    clicks: VecDeque<RenderClick>,
    cursor_shapes: HashMap<u64, CursorShape>,
    keyboard: Option<KeyboardOverlay>,
    pending_keys: VecDeque<KeyEvent>,
    #[cfg(feature = "bench-compositor-timing")]
    timings: StageHistogram,
}

impl VisualCompositor {
    fn new(output_size: (u32, u32)) -> Self {
        Self {
            output_size,
            background: Vec::new(),
            rgba: Vec::new(),
            resize_plan: None,
            resize_pool: None,
            background_sequence: None,
            trail: LaserTrail::default(),
            clicks: VecDeque::new(),
            cursor_shapes: HashMap::new(),
            keyboard: None,
            pending_keys: VecDeque::new(),
            #[cfg(feature = "bench-compositor-timing")]
            timings: StageHistogram::default(),
        }
    }

    #[cfg(test)]
    fn compose(
        &mut self,
        config: &DirectRecordingConfig,
        frame: &CapturedFrame,
        timestamp_ms: u64,
    ) -> Result<Vec<u8>> {
        self.compose_with_cursor(config, frame, timestamp_ms, frame.metadata().cursor())
    }

    fn compose_with_cursor(
        &mut self,
        config: &DirectRecordingConfig,
        frame: &CapturedFrame,
        timestamp_ms: u64,
        cursor: Option<&AttachedCursorSample>,
    ) -> Result<Vec<u8>> {
        self.trail.set_lifetime_ms(config.mouse_trail_duration_ms);
        let source_size = frame.dimensions();
        #[cfg(feature = "bench-compositor-timing")]
        let stage = Instant::now();
        let sequence = frame
            .metadata()
            .content_generation()
            .unwrap_or_else(|| frame.metadata().sequence());
        let geometry_changed = self
            .resize_plan
            .as_ref()
            .is_none_or(|plan| !plan.matches(source_size, self.output_size));
        if geometry_changed {
            self.resize_plan = Some(NearestResizePlan::new(
                source_size.0,
                source_size.1,
                self.output_size.0,
                self.output_size.1,
            ));
        }
        let bytes = self.output_size.0 as usize * self.output_size.1 as usize * 4;
        self.background.resize(bytes, 0);
        if geometry_changed
            || self.background_sequence.is_none()
            || self.background_sequence != Some(sequence)
        {
            let plan = self.resize_plan.as_ref().expect("resize plan");
            if let Some(pool) = self.resize_pool.as_ref() {
                plan.resize_into_with_pool(frame.as_rgba_bytes(), &mut self.background, pool);
            } else {
                plan.resize_into(frame.as_rgba_bytes(), &mut self.background);
            }
        }
        self.background_sequence = Some(sequence);
        self.rgba.resize(bytes, 0);
        self.rgba.copy_from_slice(&self.background);
        let mut rgba = std::mem::take(&mut self.rgba);
        #[cfg(feature = "bench-compositor-timing")]
        self.timings.record("compose.resize", stage.elapsed());
        let cursor = cursor.cloned();
        #[cfg(feature = "bench-compositor-timing")]
        let stage = Instant::now();
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
        #[cfg(feature = "bench-compositor-timing")]
        self.timings
            .record("compose.trail_observe", stage.elapsed());

        #[cfg(feature = "bench-compositor-timing")]
        let stage = Instant::now();
        if config.mouse_trail_rgba[3] != 0 {
            self.trail.draw(
                &mut rgba,
                self.output_size,
                timestamp_ms,
                config.mouse_trail_rgba,
            );
        }
        #[cfg(feature = "bench-compositor-timing")]
        self.timings.record("compose.trail_draw", stage.elapsed());

        #[cfg(feature = "bench-compositor-timing")]
        let stage = Instant::now();
        while self.clicks.front().is_some_and(|click| {
            timestamp_ms.saturating_sub(click.timestamp_ms) > CLICK_ANIMATION_MS
        }) {
            self.clicks.pop_front();
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
        #[cfg(feature = "bench-compositor-timing")]
        self.timings.record("compose.clicks", stage.elapsed());

        #[cfg(feature = "bench-compositor-timing")]
        let stage = Instant::now();
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
        #[cfg(feature = "bench-compositor-timing")]
        self.timings.record("compose.cursor", stage.elapsed());

        #[cfg(feature = "bench-compositor-timing")]
        let stage = Instant::now();
        if let Some(keyboard) = self.keyboard.as_mut() {
            #[cfg(feature = "bench-compositor-timing")]
            let (hits, misses) = keyboard.cache_stats();
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
            #[cfg(feature = "bench-compositor-timing")]
            {
                let (new_hits, new_misses) = keyboard.cache_stats();
                for _ in hits..new_hits {
                    self.timings.record("keyboard.cache_hit", Duration::ZERO);
                }
                for _ in misses..new_misses {
                    self.timings.record("keyboard.cache_miss", Duration::ZERO);
                }
            }
        }
        #[cfg(feature = "bench-compositor-timing")]
        self.timings.record("compose.keyboard", stage.elapsed());
        Ok(rgba)
    }

    fn has_active_animation(&self, config: &DirectRecordingConfig, timestamp_ms: u64) -> bool {
        let trail_active =
            config.mouse_trail_rgba[3] > 0 && self.trail.has_active_animation(timestamp_ms);
        let click_active = config.mouse_click_rgba[3] > 0
            && self.clicks.iter().any(|click| {
                timestamp_ms
                    .checked_sub(click.timestamp_ms)
                    .is_some_and(|age| age <= CLICK_ANIMATION_MS)
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

#[cfg(test)]
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
    fn automatic_thread_cap_is_limited_to_the_validated_live_configuration() {
        assert!((1..=2).contains(&config().streaming_config().encode_threads));
        for variant in 0..5 {
            let mut other = config();
            match variant {
                0 => other.output_fps = 60,
                1 => other.region = RecordingRegion::new(0, 0, 3840, 2160),
                2 => other.codec = VideoCodec::H265,
                3 => other.prefer_hardware_encoder = true,
                _ => other.preset = VideoEncodingSpeed::Medium,
            }
            assert_eq!(other.streaming_config().encode_threads, 0);
        }
        for format in [ExportFormat::Gif, ExportFormat::Apng, ExportFormat::Webp] {
            let mut other = config();
            other.format = format;
            assert_eq!(other.streaming_config().encode_threads, 0);
        }
    }

    #[test]
    fn resize_workers_require_a_large_resized_image_and_the_validated_encode_budget() {
        let mut value = config();
        value.region = RecordingRegion::new(0, 0, 3840, 2160);
        value.maximum_width = Some(1920);
        value.maximum_height = Some(1080);
        let expected = if value.automatic_encode_threads() == 2
            && std::thread::available_parallelism().is_ok_and(|value| value.get() >= 4)
        {
            2
        } else {
            0
        };
        assert_eq!(value.automatic_resize_threads(), expected);
        let mut session = DirectRecordingSession::create(value.clone()).unwrap();
        assert_eq!(session.resize_threads, expected);
        session.set_resize_threads(1).unwrap();
        assert_eq!(session.resize_threads, 1);
        assert!(session.set_resize_threads(5).is_err());
        value.output_fps = 60;
        assert_eq!(value.automatic_resize_threads(), 0);
        value.output_fps = 30;
        value.region = RecordingRegion::new(0, 0, 1920, 1080);
        assert_eq!(value.automatic_resize_threads(), 0);
        value.maximum_width = Some(960);
        value.maximum_height = Some(540);
        assert_eq!(value.automatic_resize_threads(), 0);
    }

    #[test]
    fn capture_alignment_default_is_limited_to_the_measured_clock_and_capture_path() {
        let mut value = config();
        value.region = RecordingRegion::new(0, 0, 3840, 2160);
        value.maximum_width = Some(1920);
        value.maximum_height = Some(1080);
        let expected = value.automatic_resize_threads() == 2;
        assert_eq!(value.aligned_capture(), expected);
        let mut session = DirectRecordingSession::create(value.clone()).unwrap();
        assert_eq!(session.align_capture, expected);
        session.set_aligned_capture(false).unwrap();
        assert!(!session.align_capture);
        value.capture_fps = 20;
        assert!(!value.aligned_capture());
        value.capture_fps = 60;
        value.output_fps = 60;
        assert!(!value.aligned_capture());
        value.capture_fps = 30;
        value.output_fps = 30;
        for backend in [
            CaptureBackendKind::WindowsGraphicsCapture,
            CaptureBackendKind::Gdi,
        ] {
            value.capture_backend = backend;
            assert!(!value.aligned_capture());
        }
    }

    #[test]
    fn composed_storage_never_accumulates_overlays_and_geometry_invalidates_plan() {
        let frame = snow_capture::frame::Frame::from_rgba8(4, 4, vec![90; 64]).unwrap();
        let frame = CapturedFrame::from(frame);
        let mut compositor = VisualCompositor::new((2, 2));
        let first = compositor.compose(&config(), &frame, 0).unwrap();
        assert_eq!(first, resize_rgba(frame.as_rgba_bytes(), (4, 4), (2, 2)));
        compositor.rgba = first;
        compositor.rgba.fill(255);
        assert_eq!(
            compositor.compose(&config(), &frame, 33).unwrap(),
            vec![90; 16]
        );
        let changed = CapturedFrame::from(
            snow_capture::frame::Frame::from_rgba8(3, 5, vec![42; 60]).unwrap(),
        );
        assert_eq!(
            compositor.compose(&config(), &changed, 66).unwrap(),
            vec![42; 16]
        );
        compositor.background_sequence = None;
        let resumed = CapturedFrame::from(
            snow_capture::frame::Frame::from_rgba8(3, 5, vec![60; 60]).unwrap(),
        );
        assert_eq!(
            compositor.compose(&config(), &resumed, 100).unwrap(),
            vec![60; 16]
        );
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
    fn live_audio_pause_observations_do_not_shift_resumed_pcm() {
        let start = Instant::now() - Duration::from_secs(1);
        let clock = RecordingClock::new(start);
        clock
            .controller()
            .mark_pause(start + Duration::from_millis(100));
        clock
            .controller()
            .mark_resume(start + Duration::from_millis(300));
        let mut mixer = LiveAudioMixer::new(true, false);
        for (at, value) in [(100, 1), (200, 2), (320, 3)] {
            // The worker resets continuity at pause/resume control boundaries.
            mixer.reset_alignment(None);
            mixer.insert_packet(
                test_audio_packet(
                    AudioSourceKind::System,
                    start + Duration::from_millis(at),
                    at,
                    vec![value; 960],
                ),
                &clock,
            );
        }
        assert_eq!(
            mix_audio_slot(mixer.slots.remove(&9).unwrap(), 960),
            vec![1; 960]
        );
        assert_eq!(
            mix_audio_slot(mixer.slots.remove(&11).unwrap(), 960),
            vec![3; 960]
        );
        assert!(
            mixer.slots.is_empty(),
            "paused PCM must not enter the active timeline"
        );
    }

    #[test]
    fn live_audio_final_flush_stops_inside_the_last_pcm_slot() {
        let mut config = config();
        config.output_path =
            std::env::temp_dir().join(format!("snow-audio-endpoint-{}.mp4", uuid::Uuid::new_v4()));
        config.enable_system_audio = true;
        let mut encoder = StreamingEncoder::create(config.streaming_config()).unwrap();
        encoder
            .push_owned_rgba_frame_at_pts(0, vec![128; 4 * 4 * 4])
            .unwrap();
        let mut mixer = LiveAudioMixer::new(true, false);
        mixer.insert_samples(
            AudioSourceKind::System,
            0,
            48_480,
            &vec![1000; 48_480 * 2],
            Duration::from_millis(1010),
        );
        mixer
            .emit_ready(Duration::from_millis(1001), true, &mut encoder)
            .unwrap();
        let report = encoder.finish_at_pts(31).unwrap();
        assert_eq!(
            report.encoded_audio_frames, 48_048,
            "finalization must not emit the rest of the 10ms slot"
        );
        assert_eq!(report.dropped_audio_frames, 0);
        std::fs::remove_file(config.output_path).unwrap();
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
        drain_click_observations(&receiver, &clock, false, started_at, &mut compositor);
        assert_eq!(compositor.clicks.len(), CLICK_QUEUE_DEPTH);
        assert_eq!(
            compositor.clicks.front().map(|click| click.x),
            Some((CLICK_QUEUE_DEPTH * 2) as i32)
        );
    }

    #[test]
    fn resumed_clicks_reject_paused_and_pre_resume_observations() {
        let (sender, receiver) = crossbeam_channel::unbounded();
        let start = Instant::now();
        let clock = RecordingClock::new(start);
        clock
            .controller()
            .mark_pause(start + Duration::from_millis(100));
        clock
            .controller()
            .mark_resume(start + Duration::from_millis(300));
        for ms in [50, 150, 299, 301] {
            sender
                .send(MouseClickObservation {
                    at: start + Duration::from_millis(ms),
                    x: ms as i32,
                    y: 0,
                    button: ObservedMouseButton::Left,
                })
                .unwrap();
        }
        let mut compositor = VisualCompositor::new((4, 4));
        drain_click_observations(
            &receiver,
            &clock,
            false,
            start + Duration::from_millis(300),
            &mut compositor,
        );
        assert_eq!(compositor.clicks.len(), 1);
        assert_eq!(compositor.clicks[0].timestamp_ms, 101);
    }

    #[cfg(feature = "bench-compositor-timing")]
    #[test]
    fn compositor_records_every_stage_once_per_frame() {
        let mut value = config();
        value.mouse_trail_rgba = [255, 0, 0, 255];
        value.mouse_click_rgba = [0, 255, 0, 128];
        let size = (320, 180);
        let original = [20, 40, 60, 255].repeat(size.0 as usize * size.1 as usize);
        let frame: CapturedFrame = snow_capture::frame::Frame::from_rgba8(size.0, size.1, original)
            .unwrap()
            .into();
        let mut compositor = VisualCompositor::new(size);
        compositor.keyboard = Some(KeyboardOverlay::new(size, Box::new(KeyboardTestRasterizer)));
        compositor.compose(&value, &frame, 100).unwrap();
        let snapshot = compositor.timings.snapshot();
        for name in [
            "compose.resize",
            "compose.trail_observe",
            "compose.trail_draw",
            "compose.clicks",
            "compose.cursor",
            "compose.keyboard",
        ] {
            assert_eq!(
                snapshot.get(name).map(|stats| stats.count),
                Some(1),
                "{name} must be recorded exactly once per composited frame"
            );
        }
    }

    #[cfg(feature = "bench-synthetic-input")]
    mod bench_synthetic_input_tests {
        use super::*;

        struct Synthetic {
            input: BenchSyntheticInput,
            clicks: crossbeam_channel::Receiver<MouseClickObservation>,
            keys: crossbeam_channel::Receiver<KeyObservation>,
            cursor: crossbeam_channel::Receiver<(Instant, i32, i32)>,
        }

        fn synthetic() -> Synthetic {
            let (click_tx, click_rx) = crossbeam_channel::bounded(CLICK_QUEUE_DEPTH);
            let (key_tx, key_rx) = crossbeam_channel::bounded(64);
            let (cursor_tx, cursor_rx) = crossbeam_channel::bounded(BENCH_CURSOR_QUEUE_DEPTH);
            Synthetic {
                input: BenchSyntheticInput {
                    clicks: click_tx,
                    keys: Some(key_tx),
                    generation: None,
                    cursor: cursor_tx,
                    pressed: [0; 256],
                    layout: bench_keyboard_layout(),
                },
                clicks: click_rx,
                keys: key_rx,
                cursor: cursor_rx,
            }
        }

        #[test]
        fn synthetic_keys_track_modifier_state_for_overlay_chords() {
            let mut synthetic = synthetic();
            let chord = [
                (0xa2u16, true),
                (0xa0, true),
                (u16::from(b'S'), true),
                (u16::from(b'S'), false),
                (0xa0, false),
                (0xa2, false),
            ];
            for (key, down) in chord {
                synthetic.input.observe_key(key, down).unwrap();
            }
            let config = KeyboardOverlayConfig {
                keycap_size: 64,
                background_rgba: [0; 4],
                text_rgba: [0; 4],
                border_rgba: [0; 4],
                labels: [
                    (0x11, "Ctrl".into()),
                    (0x10, "Shift".into()),
                    (u16::from(b'S'), "S".into()),
                ]
                .into(),
            };
            // Drain without blocking: the sender stays alive inside `synthetic`.
            let events: Vec<_> = std::iter::from_fn(|| synthetic.keys.try_recv().ok()).collect();
            assert_eq!(events.len(), chord.len());
            assert!(events.iter().all(|event| event.generation == 0));
            let press = events[2].event(0, &config);
            assert_eq!(press.key, u16::from(b'S'));
            assert!(press.down);
            assert_eq!(
                press
                    .modifiers
                    .iter()
                    .map(|(key, _)| *key)
                    .collect::<Vec<_>>(),
                [0xa2, 0xa0]
            );
            assert!(events[5].pressed.iter().all(|&state| state == 0));
        }

        #[test]
        fn synthetic_keys_without_an_overlay_are_rejected() {
            let mut synthetic = synthetic();
            synthetic.input.keys = None;
            assert!(synthetic.input.observe_key(u16::from(b'A'), true).is_err());
        }

        #[test]
        fn synthetic_clicks_and_cursor_carry_region_relative_positions() {
            let synthetic = synthetic();
            synthetic
                .input
                .clicks
                .try_send(MouseClickObservation {
                    at: Instant::now(),
                    x: 12,
                    y: 34,
                    button: ObservedMouseButton::Left,
                })
                .unwrap();
            synthetic
                .input
                .cursor
                .try_send((Instant::now(), 56, 78))
                .unwrap();
            let click = synthetic.clicks.try_recv().unwrap();
            assert_eq!((click.x, click.y), (12, 34));
            let (_, x, y) = synthetic.cursor.try_recv().unwrap();
            assert_eq!((x, y), (56, 78));
        }

        #[test]
        fn synthetic_cursor_shape_has_opaque_and_transparent_pixels() {
            let shape = synthetic_arrow_cursor_shape();
            assert_eq!((shape.width, shape.height), (24, 24));
            assert_eq!(shape.shape_rgba.len(), 24 * 24 * 4);
            assert!(shape.shape_rgba.chunks(4).any(|pixel| pixel[3] == 255));
            assert!(shape.shape_rgba.chunks(4).any(|pixel| pixel[3] == 0));
        }
    }
}

#[cfg(test)]
mod scheduling_tests {
    use super::*;
    fn frame(value: u8) -> CapturedFrame {
        snow_capture::frame::Frame::from_rgba8(2, 2, vec![value; 16])
            .unwrap()
            .into()
    }

    #[test]
    fn capture_selection_keeps_future_frames_and_discards_only_superseded_history() {
        let start = Instant::now();
        let clock = RecordingClock::new(start);
        let mut inbox = CaptureInbox::default();
        inbox
            .frames
            .push_back((frame(1), start, start + Duration::from_millis(30)));
        inbox
            .frames
            .push_back((frame(2), start, start + Duration::from_millis(35)));
        assert_eq!(
            inbox
                .select(&clock, Duration::from_millis(33))
                .unwrap()
                .0
                .as_rgba_bytes()[0],
            1
        );
        assert!(inbox.select(&clock, Duration::from_millis(33)).is_none());
        assert_eq!(
            inbox
                .select(&clock, Duration::from_millis(66))
                .unwrap()
                .0
                .as_rgba_bytes()[0],
            2
        );
    }

    #[test]
    fn capture_backlog_remains_bounded_and_newest_eligible_frame_wins() {
        let start = Instant::now();
        let clock = RecordingClock::new(start);
        let mut inbox = CaptureInbox::default();
        for value in 0..20 {
            inbox.push(frame(value), start);
        }
        assert_eq!(inbox.frames.len(), 2);
        assert_eq!(inbox.superseded, 18);
        assert_eq!(
            inbox
                .select(&clock, Duration::from_secs(1))
                .unwrap()
                .0
                .as_rgba_bytes()[0],
            19
        );
        assert_eq!(inbox.superseded, 19);
        inbox.clear();
        assert!(inbox.select(&clock, Duration::from_secs(2)).is_none());
    }
    #[test]
    fn cursor_sampling_after_a_desktop_deadline_does_not_delay_the_desktop() {
        let start = Instant::now();
        let clock = RecordingClock::new(start);
        let mut desktop = CaptureInbox::default();
        desktop.frames.push_back((
            frame(42),
            start + Duration::from_millis(35),
            start + Duration::from_millis(30),
        ));
        let mut cursor = CursorInbox::default();
        cursor.push(
            start + Duration::from_millis(35),
            AttachedCursorSample {
                x: 20,
                y: 30,
                visible: true,
                shape: CursorShapeState::Unavailable,
            },
        );
        assert_eq!(
            desktop
                .select(&clock, Duration::from_millis(33))
                .unwrap()
                .0
                .as_rgba_bytes()[0],
            42
        );
        assert!(cursor.select(&clock, Duration::from_millis(33)).is_none());
        assert_eq!(
            cursor.select(&clock, Duration::from_millis(66)).unwrap().x,
            20
        );
    }
}

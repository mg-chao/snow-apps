//! Clean recording followed by a retryable, deterministic rendering task.
#[cfg(not(target_os = "macos"))]
use snow_memory::RasterBuffer;
use std::collections::VecDeque;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, AtomicU8, Ordering},
};
use std::thread::JoinHandle;
#[cfg(not(target_os = "macos"))]
use std::time::Duration;
use std::time::Instant;

use crate::keyboard_hook::{KeyObservation, KeyboardInput};
use crate::mouse_hook::{
    MouseClickObservation, MouseHookObserver, MouseMovement, ObservedMouseButton,
};
use crate::{DirectRecordingConfig, RecordingState, ScreenRecorderError, error::Result};
use crossbeam_channel::{Receiver, Sender};
use snow_audio_recorder::{
    AudioControlHandle, AudioRecordingConfig, AudioRecordingSession, AudioSourceKind,
    AudioTrackConfig, RecordedAudioTrack,
};
use snow_core::recording_clock::RecordingClock;
use snow_media::geometry::{
    DesktopRect, DesktopSpace, DesktopTransform, PixelRect, PixelSize, aspect_fit,
};
#[cfg(not(target_os = "macos"))]
use snow_recording_export::StreamingEncoder;
#[cfg(any(not(target_os = "macos"), test))]
use snow_recording_export::{ExportExecutionMode, ExportFormat, SoftwareH264Priority};
use snow_recording_export::{StreamingEncoderConfig, write_deferred_output_settings};
#[cfg(not(target_os = "macos"))]
use snow_recording_model::CursorShapeCompositionMode;
use snow_recording_model::{
    BundleAssetKind, CursorFrameRecord, CursorShapeRecord, FinalizedTimeline, InputMouseButton,
    InputStoreWriter, KeyEventRecord, PlaybackOverlay, RecordedInput, RecordedInputEvent,
    RecordedMouseClick, RecordingBundleAsset, RenderConfig, RenderMetadata, SessionManifest,
    VideoCodec, VideoEncodeConfig, VideoEncodingSpeed, write_render_metadata,
};

pub use snow_recording_export::{DeferredRenderProgress, DeferredRenderState, DeferredRenderTask};

#[derive(Clone, Debug)]
pub struct DeferredRecordingOptions {
    pub working_directory: Option<PathBuf>,
    pub playback_overlay: PlaybackOverlay,
}
impl Default for DeferredRecordingOptions {
    fn default() -> Self {
        Self {
            working_directory: None,
            playback_overlay: PlaybackOverlay::None,
        }
    }
}

struct SourceInner {
    path: PathBuf,
    duration_ms: u64,
    config: StreamingEncoderConfig,
    active: Arc<AtomicBool>,
    discarded: AtomicBool,
    capture_report: snow_recording_export::StreamingEncoderReport,
}

/// A finalized self-contained bundle. Dropping handles preserves it for Keep/recovery.
#[derive(Clone)]
pub struct DeferredRecordingSource {
    inner: Arc<SourceInner>,
}
impl DeferredRecordingSource {
    pub fn path(&self) -> &Path {
        &self.inner.path
    }
    pub fn duration_ms(&self) -> u64 {
        self.inner.duration_ms
    }
    /// Actual source codec/fallback statistics, useful for performance diagnostics.
    pub fn capture_report(&self) -> &snow_recording_export::StreamingEncoderReport {
        &self.inner.capture_report
    }
    pub fn render_async(&self) -> Result<DeferredRenderTask> {
        if self.inner.discarded.load(Ordering::Acquire) {
            return Err(invalid("recording source was discarded"));
        }
        Ok(DeferredRenderTask::start(
            self.inner.path.clone(),
            self.inner.config.clone(),
            Arc::clone(&self.inner.active),
        )?)
    }
    pub fn discard(&self) -> Result<()> {
        if self
            .inner
            .active
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return Err(invalid("recording source is in use by a render task"));
        }
        let result = if self.inner.discarded.load(Ordering::Acquire) {
            Ok(())
        } else {
            match std::fs::remove_file(&self.inner.path) {
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
                result => result,
            }
        };
        if result.is_ok() {
            self.inner.discarded.store(true, Ordering::Release);
        }
        self.inner.active.store(false, Ordering::Release);
        result.map_err(Into::into)
    }
}

pub(crate) enum Command {
    Pause(Instant),
    Resume(Instant),
    Stop(Instant),
    Cancel,
}
#[derive(Clone)]
pub(crate) struct DeferredCaptureControls {
    pub audio: AudioControlHandle,
    #[cfg(target_os = "macos")]
    pub exclusions: snow_macos::desktop::ExclusionControl,
}
pub struct DeferredRecordingSession {
    config: DirectRecordingConfig,
    options: DeferredRecordingOptions,
    state: Arc<AtomicU8>,
    commands: Option<Sender<Command>>,
    stop_requested: AtomicBool,
    stop_boundary: Arc<Mutex<Option<Instant>>>,
    control_clock: Arc<Mutex<Option<RecordingClock>>>,
    worker: Option<JoinHandle<Result<DeferredRecordingSource>>>,
    capture_controls: DeferredCaptureControls,
    #[cfg(not(target_os = "macos"))]
    exclusion_generation: std::sync::atomic::AtomicU64,
}
impl DeferredRecordingSession {
    pub fn create(
        config: DirectRecordingConfig,
        options: DeferredRecordingOptions,
    ) -> Result<Self> {
        config
            .validate()
            .map_err(ScreenRecorderError::InvalidConfig)?;
        if options
            .working_directory
            .as_ref()
            .is_some_and(|p| p.as_os_str().is_empty())
        {
            return Err(invalid(
                "deferred recording working directory must not be empty",
            ));
        }
        render_config(
            &config,
            config.output_dimensions(),
            options.playback_overlay,
        )
        .validate()
        .map_err(ScreenRecorderError::InvalidConfig)?;
        let audio = AudioControlHandle::new();
        audio.set_gain_db(AudioSourceKind::System, config.system_audio_gain_db)?;
        audio.set_gain_db(AudioSourceKind::Microphone, config.microphone_gain_db)?;
        Ok(Self {
            config,
            options,
            state: Arc::new(AtomicU8::new(0)),
            commands: None,
            worker: None,
            stop_requested: AtomicBool::new(false),
            stop_boundary: Arc::new(Mutex::new(None)),
            control_clock: Arc::new(Mutex::new(None)),
            capture_controls: DeferredCaptureControls {
                audio,
                #[cfg(target_os = "macos")]
                exclusions: Default::default(),
            },
            #[cfg(not(target_os = "macos"))]
            exclusion_generation: std::sync::atomic::AtomicU64::new(0),
        })
    }
    pub fn start(&mut self) -> Result<()> {
        if self.state() != RecordingState::Created {
            return Err(invalid("deferred recording already started"));
        }
        // Control transitions are rare, but automated callers must not grow an
        // unbounded queue while a driver stalls. Stop has an independent signal.
        let (commands, receiver) = crossbeam_channel::bounded(64);
        let (ready, ready_rx) = crossbeam_channel::bounded(1);
        let config = self.config.clone();
        let options = self.options.clone();
        let state = Arc::clone(&self.state);
        let stop_boundary = Arc::clone(&self.stop_boundary);
        let control_clock = Arc::clone(&self.control_clock);
        let capture_controls = self.capture_controls.clone();
        let audio_control = capture_controls.audio.clone();
        let worker = std::thread::Builder::new()
            .name("snow-deferred-recording".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                let result = run_recording(
                    config,
                    options,
                    receiver,
                    Arc::clone(&state),
                    ready.clone(),
                    stop_boundary,
                    control_clock,
                    capture_controls,
                );
                if let Err(error) = &result {
                    let _ = ready.try_send(Err(error.to_string()));
                }
                state.store(3, Ordering::Release);
                audio_control.mark_stopped();
                result
            })?;
        self.commands = Some(commands);
        self.worker = Some(worker);
        match ready_rx.recv() {
            Ok(Ok(())) => Ok(()),
            Ok(Err(message)) => {
                let _ = self.worker.take().expect("startup worker").join();
                self.commands = None;
                Err(ScreenRecorderError::Encode(message))
            }
            Err(_) => {
                let result = self
                    .worker
                    .take()
                    .expect("startup worker")
                    .join()
                    .map_err(|_| {
                        ScreenRecorderError::Encode("deferred recording worker panicked".into())
                    })?;
                result.map(|_| ())
            }
        }
    }
    pub fn state(&self) -> RecordingState {
        match self.state.load(Ordering::Acquire) {
            1 => RecordingState::Running,
            2 => RecordingState::Paused,
            3 => RecordingState::Stopped,
            _ => RecordingState::Created,
        }
    }
    pub fn audio_control(&self) -> AudioControlHandle {
        self.capture_controls.audio.clone()
    }
    pub fn request_exclusions(
        &self,
        windows: Vec<u32>,
        processes: Vec<i32>,
        required: Vec<u32>,
    ) -> Result<u64> {
        #[cfg(target_os = "macos")]
        {
            self.capture_controls
                .exclusions
                .request(windows, processes, required)
                .map_err(crate::macos::native_error)
        }
        #[cfg(not(target_os = "macos"))]
        {
            // Windows uses native display affinity for application-owned surfaces.
            if windows.len() > 4096
                || processes.len() > 4096
                || required.len() > 4096
                || required.iter().any(|id| !windows.contains(id))
            {
                return Err(invalid("invalid capture exclusions"));
            }
            Ok(self.exclusion_generation.fetch_add(1, Ordering::AcqRel) + 1)
        }
    }
    pub fn exclusion_status(&self) -> (u64, u64, u32) {
        #[cfg(target_os = "macos")]
        {
            self.capture_controls.exclusions.status()
        }
        #[cfg(not(target_os = "macos"))]
        {
            let generation = self.exclusion_generation.load(Ordering::Acquire);
            (generation, generation, 0)
        }
    }
    pub fn pause(&self) -> Result<()> {
        self.transition(1, 2)
    }
    pub fn resume(&self) -> Result<()> {
        self.transition(2, 1)
    }
    fn transition(&self, from: u8, to: u8) -> Result<()> {
        let control = self.control_clock.lock().unwrap_or_else(|e| e.into_inner());
        if self.stop_requested.load(Ordering::Acquire) {
            return Err(invalid("deferred recording is stopping"));
        }
        let sender = self
            .commands
            .as_ref()
            .ok_or_else(|| invalid("deferred recording has not started"))?;
        if sender.is_full() {
            return Err(invalid("deferred recording control queue is full"));
        }
        if self
            .state
            .compare_exchange(from, to, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return Err(invalid("invalid deferred recording state transition"));
        }
        let at = Instant::now();
        let command = if to == 2 {
            Command::Pause(at)
        } else {
            Command::Resume(at)
        };
        if let Some(clock) = control.as_ref() {
            match command {
                Command::Pause(at) => clock.controller().mark_pause(at),
                Command::Resume(at) => clock.controller().mark_resume(at),
                _ => {}
            }
        }
        self.send(command)
    }
    fn send(&self, command: Command) -> Result<()> {
        self.commands
            .as_ref()
            .ok_or_else(|| invalid("deferred recording has not started"))?
            .try_send(command)
            .map_err(|_| ScreenRecorderError::Encode("deferred recording worker stopped".into()))
    }
    pub fn request_stop(&self) -> Result<()> {
        let control = self.control_clock.lock().unwrap_or_else(|e| e.into_inner());
        if self.commands.is_none() {
            return Err(invalid("deferred recording has not started"));
        }
        if !self.stop_requested.swap(true, Ordering::AcqRel) {
            let at = Instant::now();
            if let Some(clock) = control.as_ref() {
                clock.controller().mark_pause(at);
            }
            *self.stop_boundary.lock().unwrap_or_else(|e| e.into_inner()) = Some(at);
            let _ = self.send(Command::Stop(at));
        }
        Ok(())
    }
    pub fn stop(mut self) -> Result<DeferredRecordingSource> {
        self.request_stop()?;
        self.worker
            .take()
            .ok_or_else(|| invalid("deferred recording has not started"))?
            .join()
            .map_err(|_| ScreenRecorderError::Encode("deferred recording worker panicked".into()))?
    }
}
impl Drop for DeferredRecordingSession {
    fn drop(&mut self) {
        if let Some(worker) = self.worker.take() {
            self.state.store(3, Ordering::Release);
            let _ = self.send(Command::Cancel);
            let _ = worker.join();
        }
    }
}
fn invalid(message: impl Into<String>) -> ScreenRecorderError {
    ScreenRecorderError::InvalidConfig(message.into())
}

fn render_config(
    config: &DirectRecordingConfig,
    size: (u32, u32),
    overlay: PlaybackOverlay,
) -> RenderConfig {
    config.render_config(size, overlay)
}

pub(crate) struct CaptureProduct {
    pub duration_ms: u64,
    pub logical: (u32, u32),
    pub coded: (u32, u32),
    pub media: snow_recording_model::media::RecordedMedia,
    pub clock: RecordingClock,
    pub tracks: Vec<RecordedAudioTrack>,
    pub video: PathBuf,
    pub index: PathBuf,
    pub inputs: PathBuf,
    pub source_codec: VideoCodec,
    pub encoder_report: snow_recording_export::StreamingEncoderReport,
}

#[allow(clippy::too_many_arguments)]
fn run_recording(
    config: DirectRecordingConfig,
    options: DeferredRecordingOptions,
    commands: Receiver<Command>,
    state: Arc<AtomicU8>,
    ready: Sender<std::result::Result<(), String>>,
    stop_boundary: Arc<Mutex<Option<Instant>>>,
    control_clock: Arc<Mutex<Option<RecordingClock>>>,
    capture_controls: DeferredCaptureControls,
) -> Result<DeferredRecordingSource> {
    let parent = options.working_directory.unwrap_or_else(|| {
        config
            .output_path
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(Path::new("."))
            .to_path_buf()
    });
    std::fs::create_dir_all(&parent)?;
    let directory = tempfile::Builder::new()
        .prefix(".snow-deferred-")
        .tempdir_in(&parent)?;
    #[cfg(not(target_os = "macos"))]
    let product = run_capture(
        &config,
        directory.path(),
        commands,
        state,
        ready,
        stop_boundary,
        control_clock,
        capture_controls,
    )?;
    #[cfg(target_os = "macos")]
    let product = crate::macos::deferred::run_capture(
        &config,
        directory.path(),
        commands,
        state,
        ready,
        stop_boundary,
        control_clock,
        capture_controls,
    )?;
    finalize_source(directory, product, config, parent, options.playback_overlay)
}

fn finalize_source(
    directory: tempfile::TempDir,
    product: CaptureProduct,
    config: DirectRecordingConfig,
    parent: PathBuf,
    overlay: PlaybackOverlay,
) -> Result<DeferredRecordingSource> {
    let result = pack_source(directory.path(), product, config, parent, overlay);
    match result {
        Ok(source) => Ok(source),
        Err(error) => {
            let path = directory.keep();
            Err(ScreenRecorderError::Export(format!(
                "{error}; recorded source files are retained in {}",
                path.display()
            )))
        }
    }
}

fn pack_source(
    directory: &Path,
    product: CaptureProduct,
    config: DirectRecordingConfig,
    parent: PathBuf,
    overlay: PlaybackOverlay,
) -> Result<DeferredRecordingSource> {
    let timeline =
        FinalizedTimeline::new(product.duration_ms, config.output_fps).map_err(invalid)?;
    let render = render_config(&config, product.logical, overlay);
    let metadata = RenderMetadata {
        render,
        timeline,
        coded_width: product.coded.0,
        coded_height: product.coded.1,
    };
    let render_path = directory.join("render.bin");
    write_render_metadata(&render_path, &metadata)?;
    let mut output = config.streaming_config();
    output.width = product.logical.0;
    output.height = product.logical.1;
    let output_path = directory.join("output.bin");
    write_deferred_output_settings(&output_path, &output)?;
    // Empty legacy store makes the bundle structurally readable by v2-aware editors.
    // Newly recorded input is exclusively streamed through the bounded v3 asset.
    let mouse = directory.join("mouse.bin");
    snow_recording_model::write_mouse_records(&mouse, &snow_recording_model::MouseStore::new())?;
    let mut assets = vec![
        RecordingBundleAsset {
            kind: BundleAssetKind::VideoIndex,
            asset_id: None,
            path: &product.index,
        },
        RecordingBundleAsset {
            kind: BundleAssetKind::MouseStore,
            asset_id: None,
            path: &mouse,
        },
        RecordingBundleAsset {
            kind: BundleAssetKind::InputEvents,
            asset_id: None,
            path: &product.inputs,
        },
        RecordingBundleAsset {
            kind: BundleAssetKind::RenderMetadata,
            asset_id: None,
            path: &render_path,
        },
        RecordingBundleAsset {
            kind: BundleAssetKind::OutputSettings,
            asset_id: None,
            path: &output_path,
        },
    ];
    for track in &product.tracks {
        assets.push(RecordingBundleAsset {
            kind: BundleAssetKind::AudioTrack,
            asset_id: Some(&track.manifest.asset_id),
            path: &track.path,
        });
    }
    let id = uuid::Uuid::new_v4().to_string();
    let path = parent.join(format!("{id}.snowrec"));
    let manifest = SessionManifest {
        media: product.media,
        video_codec: product.source_codec,
        session_id: id,
        output_dir: parent,
        keep_temp_files: false,
        fps: config.output_fps,
        intermediate_profile: snow_recording_model::IntermediateRecordingProfile::EditFast,
        recording_video: VideoEncodeConfig {
            quality: config.quality.max(95),
            speed: VideoEncodingSpeed::VeryFast,
        },
        width: product.logical.0,
        height: product.logical.1,
        capture_origin_x: 0,
        capture_origin_y: 0,
        audio_tracks: product.tracks.iter().map(|t| t.manifest.clone()).collect(),
        pause_intervals: product
            .clock
            .pause_intervals()
            .into_iter()
            .map(|p| snow_recording_model::PauseInterval {
                start_ms: p.start_ms,
                end_ms: p.end_ms,
            })
            .collect(),
    };
    // Append directly to the source file, then rename on the same filesystem.
    snow_recording_model::write_recording_bundle(&product.video, &manifest, &assets)?;
    std::fs::rename(&product.video, &path)?;
    Ok(DeferredRecordingSource {
        inner: Arc::new(SourceInner {
            path,
            duration_ms: product.duration_ms,
            config: output,
            active: Arc::new(AtomicBool::new(false)),
            discarded: AtomicBool::new(false),
            capture_report: product.encoder_report,
        }),
    })
}

pub(crate) fn start_audio(
    config: &DirectRecordingConfig,
    path: &Path,
    clock: &RecordingClock,
    audio_control: AudioControlHandle,
) -> Result<Option<AudioRecordingSession>> {
    if config.format.is_animated_image() {
        return Ok(None);
    }
    let mut tracks = Vec::new();
    if config.enable_system_audio {
        let mut track = AudioTrackConfig::system_default("system");
        track.required = cfg!(target_os = "macos");
        tracks.push(track);
    }
    if config.enable_microphone {
        let mut track = AudioTrackConfig::microphone_default("microphone");
        track.required = cfg!(target_os = "macos");
        tracks.push(track);
    }
    if tracks.is_empty() {
        return Ok(None);
    }
    Ok(Some(AudioRecordingSession::start_with_controls(
        AudioRecordingConfig {
            output_dir: path.join("audio"),
            tracks,
            ..Default::default()
        },
        clock.clone(),
        audio_control,
    )?))
}

#[cfg(not(target_os = "macos"))]
#[allow(clippy::too_many_arguments)]
fn run_capture(
    config: &DirectRecordingConfig,
    path: &Path,
    commands: Receiver<Command>,
    state: Arc<AtomicU8>,
    ready: Sender<std::result::Result<(), String>>,
    stop_boundary: Arc<Mutex<Option<Instant>>>,
    control_clock: Arc<Mutex<Option<RecordingClock>>>,
    capture_controls: DeferredCaptureControls,
) -> Result<CaptureProduct> {
    use crate::direct::capture::{DirectCapture, DirectCaptureEvent, DirectFrame};
    let logical = config.output_dimensions();
    let coded = (logical.0.div_ceil(2) * 2, logical.1.div_ceil(2) * 2);
    let video = path.join("source.mp4");
    let inputs = path.join("inputs.bin");
    let index = path.join("video.index");
    let mut source = config.streaming_config();
    source.output_path = video.clone();
    source.format = ExportFormat::Mp4;
    source.width = coded.0;
    source.height = coded.1;
    source.codec = VideoCodec::H264;
    source.audio.clear();
    source.prefer_hardware_h264 = true;
    source.execution_mode = ExportExecutionMode::HardwarePreferred;
    source.software_h264_priority = SoftwareH264Priority::X264First;
    source.encode_threads = 0;
    let mut encoder = StreamingEncoder::builder(source)
        .recording_source()
        .create()?;
    let capture = DirectCapture::cpu(config, true)?;
    let clock = RecordingClock::new(Instant::now());
    clock.controller().mark_pause(clock.started_at());
    let mut audio = start_audio(config, path, &clock, capture_controls.audio)?;
    let mut input = InputRecorder::new(config, &inputs, logical)?;
    let origin = Instant::now();
    clock.controller().mark_resume(origin);
    input.reset(&clock, origin)?;
    let mut paused = false;
    let mut fresh_since = origin;
    let mut stop = None;
    let mut queue = VecDeque::new();
    let mut latest = None;
    let mut schedule = crate::output_schedule::OutputSchedule::new(config.output_fps);
    let mut canvas = CleanCanvas::new(logical, coded);
    let mut previous_generation = None;
    let mut admitted = false;
    let mut frame_index = FrameIndexWriter::new(&index, config.output_fps)?;
    let mut geometry = Vec::new();
    let mut geometry_generation = None;
    #[cfg(feature = "bench-pipeline-timing")]
    let mut source_trace = std::env::var_os("SNOW_BENCH_SOURCE_TRACE")
        .map(|path| -> std::io::Result<_> {
            let mut file = BufWriter::new(std::fs::File::create(path)?);
            writeln!(
                file,
                "pts,generation,duplicate,width,height,raw_marker,canvas_marker,raw_rgb,canvas_rgb"
            )?;
            Ok(file)
        })
        .transpose()?;
    *control_clock.lock().unwrap_or_else(|e| e.into_inner()) = Some(clock.clone());
    state.store(1, Ordering::Release);
    let _ = ready.send(Ok(()));
    while stop.is_none() {
        while let Ok(command) = commands.try_recv() {
            match command {
                Command::Pause(at) if !paused => {
                    capture.pause();
                    if let Some(audio) = &audio {
                        audio.pause();
                    }
                    input.reset(&clock, at)?;
                    queue.clear();
                    latest = None;
                    paused = true;
                }
                Command::Resume(at) if paused => {
                    fresh_since = at;
                    capture.resume();
                    if let Some(audio) = &audio {
                        audio.resume();
                    }
                    input.reset(&clock, at)?;
                    paused = false;
                    previous_generation = None;
                }
                Command::Stop(at) => {
                    stop = Some(at);
                    break;
                }
                Command::Cancel => {
                    capture.stop();
                    return Err(ScreenRecorderError::ExportCanceled);
                }
                _ => {}
            }
        }
        if stop.is_some() {
            break;
        }
        // Stop is independent of the bounded control queue and must also wake
        // a source left paused by its last admitted command.
        if let Some(at) = *stop_boundary.lock().unwrap_or_else(|e| e.into_inner()) {
            stop = Some(at);
            break;
        }
        if commands.is_empty() && state.load(Ordering::Acquire) == 3 {
            return Err(ScreenRecorderError::ExportCanceled);
        }
        if paused {
            input.drain(&clock, true, fresh_since, None)?;
        }
        let wait = if paused {
            Duration::from_millis(10)
        } else {
            schedule.wait(clock.active_elapsed_duration(Instant::now()))
        };
        let first = match capture.recv_timeout(wait) {
            Ok(event) => Some(event),
            Err(snow_core::error::RecvTimeoutError::Timeout) => None,
            Err(snow_core::error::RecvTimeoutError::Disconnected) => {
                return Err(ScreenRecorderError::Encode(
                    "clean capture worker disconnected".into(),
                ));
            }
        };
        for event in first
            .into_iter()
            .chain(std::iter::from_fn(|| capture.try_recv().ok()))
        {
            match event {
                DirectCaptureEvent::Frame(DirectFrame::Cpu(frame))
                    if !paused
                        && frame
                            .metadata()
                            .observation_started_at()
                            .unwrap_or_else(|| {
                                frame
                                    .metadata()
                                    .stream_timestamp()
                                    .map(|t| t.instant)
                                    .unwrap_or_else(Instant::now)
                            })
                            >= fresh_since =>
                {
                    let observed = frame
                        .metadata()
                        .observation_started_at()
                        .unwrap_or_else(|| {
                            frame
                                .metadata()
                                .stream_timestamp()
                                .map(|t| t.instant)
                                .unwrap_or_else(Instant::now)
                        });
                    if !clock.is_active_at(observed) {
                        continue;
                    }
                    let at = frame.metadata().queued_at().unwrap_or_else(Instant::now);
                    if queue.len() == 4 {
                        queue.pop_front();
                    }
                    queue.push_back((at, frame));
                }
                DirectCaptureEvent::Error(error) => return Err(error.into()),
                DirectCaptureEvent::StreamEnded => {
                    return Err(ScreenRecorderError::Encode(
                        "clean capture stream ended unexpectedly".into(),
                    ));
                }
                _ => {}
            }
        }
        if paused {
            continue;
        }
        if let Some(at) = *stop_boundary.lock().unwrap_or_else(|e| e.into_inner()) {
            stop = Some(at);
            break;
        }
        let Some(slot) = admit_source_slot(&mut schedule, &clock, &stop_boundary, &control_clock)
        else {
            continue;
        };
        let mut changed = false;
        while queue
            .front()
            .is_some_and(|(at, _)| clock.active_elapsed_duration(*at) <= slot.at)
        {
            let (_, frame) = queue.pop_front().expect("queued source");
            let generation = frame.metadata().content_generation();
            changed |=
                !admitted || !frame.metadata().is_duplicate() || previous_generation != generation;
            previous_generation = generation;
            latest = Some(frame);
        }
        if let Some(frame) = &latest {
            let now = Instant::now();
            let slot_ms = (u128::from(slot.pts) * 1000 / u128::from(config.output_fps)) as u64;
            input.begin_slot(slot_ms);
            input.update_geometry(
                &clock,
                now,
                frame.dimensions(),
                frame.metadata().capture_transform(),
            )?;
            if geometry_generation != Some(frame.metadata().configuration_generation()) {
                geometry_generation = Some(frame.metadata().configuration_generation());
                if let Some(transform) = frame.metadata().capture_transform() {
                    geometry.push(snow_recording_model::media::GeometryChange {
                        timestamp_ms: slot_ms,
                        generation: frame.metadata().configuration_generation(),
                        transform,
                        destination: fitted_rect(frame.dimensions(), logical)?,
                    });
                }
            }
            input.drain(&clock, false, fresh_since, None)?;
            if let Some(cursor) = frame.metadata().cursor() {
                input.cursor(&clock, now, cursor, frame.dimensions())?;
                let position = cursor
                    .visible
                    .then(|| input.point((cursor.x, cursor.y), frame.dimensions()));
                input.pointer(&clock, now, Some(position), slot_ms)?;
            } else {
                input.pointer(&clock, now, Some(None), slot_ms)?;
            }
            input.end_slot();
        }
        if changed && let Some(frame) = &latest {
            let pixels = canvas.compose(frame)?;
            #[cfg(feature = "bench-pipeline-timing")]
            if let Some(trace) = &mut source_trace {
                let raw = source_pixel_probe(
                    frame.as_bytes(),
                    frame.dimensions(),
                    frame.pixel_format() == snow_capture::CapturePixelFormat::Bgra8,
                );
                let clean = source_pixel_probe(&pixels, coded, false);
                writeln!(
                    trace,
                    "{},{:?},{},{},{},{:?},{:?},\"{:?}\",\"{:?}\"",
                    slot.pts,
                    frame.metadata().content_generation(),
                    frame.metadata().is_duplicate(),
                    frame.dimensions().0,
                    frame.dimensions().1,
                    raw.0,
                    clean.0,
                    raw.1,
                    clean.1,
                )?;
            }
            canvas.pixels = encoder.push_raster_rgba_frame_at_pts(slot.pts, pixels)?;
            frame_index.push(slot.pts)?;
            admitted = true;
        }
    }
    let stop = stop.expect("stop boundary");
    let duration_ms = clock
        .active_elapsed_duration(stop)
        .as_nanos()
        .div_ceil(1_000_000)
        .max(1)
        .min(u128::from(u64::MAX)) as u64;
    let timeline = FinalizedTimeline::new(duration_ms, config.output_fps).map_err(invalid)?;
    clock.controller().mark_pause(stop);
    capture.stop();
    input.drain(&clock, true, fresh_since, Some(stop))?;
    if !admitted {
        return Err(ScreenRecorderError::Encode(
            "no screen frame was captured".into(),
        ));
    }
    let _ = timeline;
    let encoder_report = encoder.finish_at_duration_ms(duration_ms)?;
    let tracks = audio
        .take()
        .map(AudioRecordingSession::finish)
        .transpose()?
        .map(|a| a.tracks)
        .unwrap_or_default();
    input.finish()?;
    frame_index.finish(duration_ms)?;
    clock.controller().finalize(stop);
    let mut media = snow_recording_model::media::RecordedMedia::new(
        snow_media::ColorDescription::SRGB,
        snow_media::CursorMode::Separate,
        geometry,
    );
    media.discontinuities.extend(
        tracks
            .iter()
            .flat_map(|t| t.discontinuities.iter().cloned()),
    );
    Ok(CaptureProduct {
        duration_ms,
        logical,
        coded,
        media,
        clock,
        tracks,
        video,
        index,
        inputs,
        source_codec: VideoCodec::H264,
        encoder_report,
    })
}

#[cfg(all(not(target_os = "macos"), feature = "bench-pipeline-timing"))]
fn source_pixel_probe(pixels: &[u8], size: (u32, u32), bgra: bool) -> (Option<u32>, [u8; 3]) {
    let rgb_at = |x: u32, y: u32| -> [u8; 3] {
        let offset = (u64::from(y) * u64::from(size.0) + u64::from(x)) as usize * 4;
        let Some(pixel) = pixels.get(offset..offset + 4) else {
            return [0; 3];
        };
        if bgra {
            [pixel[2], pixel[1], pixel[0]]
        } else {
            [pixel[0], pixel[1], pixel[2]]
        }
    };
    let mut code = 0u32;
    if size.0 >= 528 && size.1 >= 96 {
        for bit in 0..32 {
            if rgb_at(24 + bit * 16, 80)[0] >= 128 {
                code |= 1 << bit;
            }
        }
    }
    (
        (code & 255 == 0xa5).then_some(code >> 8),
        rgb_at(size.0 / 2, size.1 / 2),
    )
}

/// Serialize only slot admission with accepted controls. A Stop that freezes
/// the clock at an exact grid boundary must not create an image at its exclusive end.
pub(crate) fn admit_source_slot(
    schedule: &mut crate::output_schedule::OutputSchedule,
    clock: &RecordingClock,
    stop_boundary: &Mutex<Option<Instant>>,
    control_clock: &Mutex<Option<RecordingClock>>,
) -> Option<crate::output_schedule::OutputSlot> {
    let _admission = control_clock.lock().unwrap_or_else(|e| e.into_inner());
    if stop_boundary
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .is_some()
    {
        return None;
    }
    schedule.poll(clock.active_elapsed_duration(Instant::now()))
}

pub(crate) struct FrameIndexWriter {
    writer: BufWriter<std::fs::File>,
    fps: u32,
    last: Option<(u64, u64)>,
    count: u64,
}
impl FrameIndexWriter {
    pub fn new(path: &Path, fps: u32) -> Result<Self> {
        let mut writer = BufWriter::new(std::fs::File::create(path)?);
        writer.write_all(snow_recording_model::VIDEO_INDEX_MAGIC)?;
        Ok(Self {
            writer,
            fps,
            last: None,
            count: 0,
        })
    }
    fn write(&mut self, pts: u64, timestamp: u64, end: u64) -> Result<()> {
        self.writer.write_all(&self.count.to_le_bytes())?;
        self.writer.write_all(&timestamp.to_le_bytes())?;
        self.writer.write_all(
            &(end
                .saturating_sub(timestamp)
                .max(1)
                .min(u64::from(u32::MAX)) as u32)
                .to_le_bytes(),
        )?;
        self.count += 1;
        let _ = pts;
        Ok(())
    }
    pub fn push(&mut self, pts: u64) -> Result<()> {
        let timestamp = (u128::from(pts) * 1000 / u128::from(self.fps)) as u64;
        if let Some((last, at)) = self.last.replace((pts, timestamp)) {
            self.write(last, at, timestamp)?;
        }
        Ok(())
    }
    pub fn finish(mut self, duration: u64) -> Result<()> {
        if let Some((pts, at)) = self.last.take() {
            self.write(pts, at, duration)?;
        }
        self.writer.flush()?;
        Ok(())
    }
}

#[cfg(not(target_os = "macos"))]
struct CleanCanvas {
    logical: (u32, u32),
    coded: (u32, u32),
    pixels: RasterBuffer,
    rgba: RasterBuffer,
    scaled: RasterBuffer,
    resize: Option<snow_recording_export::resize::NearestResizePlan>,
}
#[cfg(not(target_os = "macos"))]
impl CleanCanvas {
    fn new(logical: (u32, u32), coded: (u32, u32)) -> Self {
        Self {
            logical,
            coded,
            pixels: RasterBuffer::new(),
            rgba: RasterBuffer::new(),
            scaled: RasterBuffer::new(),
            resize: None,
        }
    }
    fn compose(&mut self, frame: &snow_capture::CapturedFrame) -> Result<RasterBuffer> {
        use snow_media::geometry::{PixelSize, aspect_fit};
        let size = frame.dimensions();
        let destination = aspect_fit(
            PixelSize::new(size.0, size.1).map_err(|e| invalid(e.to_string()))?,
            PixelSize::new(self.logical.0, self.logical.1).map_err(|e| invalid(e.to_string()))?,
        )
        .map_err(|e| invalid(e.to_string()))?;
        self.rgba.clear();
        self.rgba.extend_from_slice(frame.as_bytes());
        if frame.pixel_format() == snow_capture::CapturePixelFormat::Bgra8 {
            for pixel in self.rgba.chunks_exact_mut(4) {
                pixel.swap(0, 2);
            }
        }
        if self
            .resize
            .as_ref()
            .is_none_or(|p| !p.matches(size, (destination.width, destination.height)))
        {
            self.resize = Some(snow_recording_export::resize::NearestResizePlan::new(
                size.0,
                size.1,
                destination.width,
                destination.height,
            ));
        }
        self.scaled.resize(
            destination.width as usize * destination.height as usize * 4,
            0,
        );
        self.resize
            .as_ref()
            .expect("source resize")
            .resize_into(&self.rgba, &mut self.scaled);
        self.pixels
            .resize(self.coded.0 as usize * self.coded.1 as usize * 4, 0);
        self.pixels.fill(0);
        for pixel in self.pixels.chunks_exact_mut(4) {
            pixel[3] = 255;
        }
        for y in 0..destination.height as usize {
            let to =
                ((y + destination.y as usize) * self.coded.0 as usize + destination.x as usize) * 4;
            let len = destination.width as usize * 4;
            self.pixels[to..to + len].copy_from_slice(&self.scaled[y * len..(y + 1) * len]);
        }
        // 4:2:0 source padding replicates the last logical edge; final rendering crops it away.
        if self.coded.0 > self.logical.0 {
            for y in 0..self.logical.1 as usize {
                let edge = (y * self.coded.0 as usize + self.logical.0 as usize - 1) * 4;
                let color: [u8; 4] = self.pixels[edge..edge + 4].try_into().expect("pixel");
                self.pixels[edge + 4..edge + 8].copy_from_slice(&color);
            }
        }
        if self.coded.1 > self.logical.1 {
            let row = self.coded.0 as usize * 4;
            let from = (self.logical.1 as usize - 1) * row;
            self.pixels.copy_within(from..from + row, from + row);
        }
        Ok(std::mem::take(&mut self.pixels))
    }
}

/// Admission ordering is deterministic; original timestamps remain in each payload.
#[derive(Clone, Copy, PartialEq)]
struct InputGeometry {
    source: (u32, u32),
    desktop: DesktopRect,
    destination: PixelRect,
}
fn fitted_rect(source: (u32, u32), logical: (u32, u32)) -> Result<PixelRect> {
    aspect_fit(
        PixelSize::new(source.0, source.1).map_err(|e| invalid(e.to_string()))?,
        PixelSize::new(logical.0, logical.1).map_err(|e| invalid(e.to_string()))?,
    )
    .map_err(|e| invalid(e.to_string()))
}
fn project_canvas_point(
    point: (i32, i32),
    source: (u32, u32),
    destination: PixelRect,
) -> (i32, i32) {
    let (x, y) = snow_recording_effects::mouse_effects::scale_point(
        point.0,
        point.1,
        source,
        (destination.width, destination.height),
    );
    (
        x.saturating_add(destination.x as i32),
        y.saturating_add(destination.y as i32),
    )
}
pub(crate) struct InputRecorder {
    writer: InputStoreWriter,
    sequence: u64,
    last_ms: u64,
    logical: (u32, u32),
    region_origin: (i32, i32),
    geometry: InputGeometry,
    mouse: MouseHookObserver,
    clicks: Receiver<MouseClickObservation>,
    movement: Receiver<MouseMovement>,
    keyboard: Option<KeyboardInput>,
    style: Option<crate::KeyboardOverlayConfig>,
    include_modifiers: bool,
    mouse_generation: u64,
    key_generation: u64,
    shapes: VecDeque<CursorShapeRecord>,
    last_shape: Option<u64>,
    #[cfg(not(target_os = "macos"))]
    raw_shapes: VecDeque<snow_cursor::CursorShape>,
    admission_ms: Option<u64>,
    latest_movement: Option<MouseMovement>,
    pointer_break: bool,
}
impl InputRecorder {
    pub fn new(config: &DirectRecordingConfig, path: &Path, logical: (u32, u32)) -> Result<Self> {
        let (click_tx, clicks) = crossbeam_channel::bounded(128);
        let (movement_tx, movement) = crossbeam_channel::bounded(64);
        let mouse = MouseHookObserver::start_with_movement(
            (
                config.region.x,
                config.region.y,
                config.region.width,
                config.region.height,
            ),
            click_tx,
            Some((movement_tx, movement.clone())),
        )
        .map_err(ScreenRecorderError::Encode)?;
        let keyboard = (config.show_keyboard && config.keyboard.is_some())
            .then(KeyboardInput::start)
            .transpose()
            .map_err(ScreenRecorderError::Encode)?;
        Ok(Self {
            writer: InputStoreWriter::new(path)?,
            sequence: 0,
            last_ms: 0,
            logical,
            region_origin: (config.region.x, config.region.y),
            geometry: InputGeometry {
                source: (config.region.width, config.region.height),
                desktop: DesktopRect {
                    space: if cfg!(target_os = "macos") {
                        DesktopSpace::Points
                    } else {
                        DesktopSpace::PhysicalPixels
                    },
                    x: f64::from(config.region.x),
                    y: f64::from(config.region.y),
                    width: f64::from(config.region.width),
                    height: f64::from(config.region.height),
                },
                destination: fitted_rect((config.region.width, config.region.height), logical)?,
            },
            mouse,
            clicks,
            movement,
            keyboard,
            style: config.keyboard.clone(),
            include_modifiers: config.show_keyboard,
            mouse_generation: 0,
            key_generation: 0,
            shapes: VecDeque::new(),
            last_shape: None,
            #[cfg(not(target_os = "macos"))]
            raw_shapes: VecDeque::new(),
            admission_ms: None,
            latest_movement: None,
            pointer_break: false,
        })
    }
    pub fn push(
        &mut self,
        clock: &RecordingClock,
        at: Instant,
        event: RecordedInput,
    ) -> Result<()> {
        let timestamp_ms = self
            .admission_ms
            .unwrap_or_else(|| clock.active_elapsed_ms(at))
            .max(self.last_ms);
        self.writer.push(&RecordedInputEvent {
            timestamp_ms,
            sequence: self.sequence,
            event,
        })?;
        self.sequence = self
            .sequence
            .checked_add(1)
            .ok_or_else(|| invalid("input sequence overflow"))?;
        self.last_ms = timestamp_ms;
        Ok(())
    }
    pub fn reset(&mut self, clock: &RecordingClock, at: Instant) -> Result<()> {
        while self.clicks.try_recv().is_ok() {}
        while self.movement.try_recv().is_ok() {}
        if let Some(keyboard) = &self.keyboard {
            keyboard.reset();
        }
        self.last_shape = None;
        self.latest_movement = None;
        self.pointer_break = false;
        self.push(clock, at, RecordedInput::Reset)
    }
    pub fn begin_slot(&mut self, timestamp_ms: u64) {
        self.admission_ms = Some(timestamp_ms);
    }
    pub fn end_slot(&mut self) {
        self.admission_ms = None;
    }
    pub fn drain(
        &mut self,
        clock: &RecordingClock,
        paused: bool,
        since: Instant,
        end: Option<Instant>,
    ) -> Result<()> {
        let now = Instant::now();
        let generation = self.mouse.generation();
        let key_generation = self
            .keyboard
            .as_ref()
            .map_or(0, |k| k.generation.load(Ordering::Acquire));
        if generation != self.mouse_generation || key_generation != self.key_generation {
            self.mouse_generation = generation;
            self.key_generation = key_generation;
            self.push(clock, now, RecordedInput::KeyboardReset)?;
        }
        let accept = |at: Instant| {
            !paused && at >= since && end.is_none_or(|end| at <= end) && clock.is_active_at(at)
        };
        while let Ok(event) = self.clicks.try_recv() {
            if !accept(event.at) {
                continue;
            }
            let at_ms = clock.active_elapsed_ms(event.at);
            let key = if let Some(style) = &self.style {
                key_record(event.event(at_ms, style, self.include_modifiers))
            } else {
                KeyEventRecord {
                    at_ms,
                    key: event.button.key(),
                    down: event.down,
                    label: event.button.label().into(),
                    modifiers: Vec::new(),
                }
            };
            let (x, y) = self.hook_point((event.x, event.y));
            let button = match event.button {
                ObservedMouseButton::Left => InputMouseButton::Left,
                ObservedMouseButton::Right => InputMouseButton::Right,
                ObservedMouseButton::Middle => InputMouseButton::Middle,
                ObservedMouseButton::Button4 => InputMouseButton::Button4,
                ObservedMouseButton::Button5 => InputMouseButton::Button5,
            };
            self.push(
                clock,
                now,
                RecordedInput::Click(RecordedMouseClick {
                    x,
                    y,
                    button,
                    down: event.down,
                    key,
                }),
            )?;
        }
        let events = self
            .keyboard
            .as_ref()
            .map(|k| k.receiver.try_iter().collect::<Vec<KeyObservation>>())
            .unwrap_or_default();
        for event in events {
            if !accept(event.at) || event.generation != self.key_generation {
                continue;
            }
            if let Some(style) = &self.style {
                let key = key_record(event.event(clock.active_elapsed_ms(event.at), style));
                self.push(clock, now, RecordedInput::Key(key))?;
            }
        }
        while let Ok(event) = self.movement.try_recv() {
            if accept(event.at) {
                self.pointer_break |= event.position.is_none()
                    || self
                        .latest_movement
                        .as_ref()
                        .is_some_and(|last| last.continuity != event.continuity);
                self.latest_movement = Some(event);
            }
        }
        Ok(())
    }
    /// Exactly one coalesced pointer is admitted per output slot. A native
    /// capture sample wins over hooks; hook timestamps remain useful on macOS.
    pub fn pointer(
        &mut self,
        clock: &RecordingClock,
        at: Instant,
        captured: Option<Option<(i32, i32)>>,
        captured_at_ms: u64,
    ) -> Result<()> {
        let continuity = self.latest_movement.as_ref().map_or(0, |m| m.continuity);
        if self.pointer_break {
            self.push(
                clock,
                at,
                RecordedInput::Pointer {
                    position: None,
                    continuity,
                    at_ms: captured_at_ms,
                },
            )?;
            self.pointer_break = false;
        }
        let (position, at_ms) = captured.map_or_else(
            || {
                self.latest_movement
                    .as_ref()
                    .map_or((None, captured_at_ms), |m| {
                        (
                            m.position.map(|p| self.hook_point(p)),
                            clock.active_elapsed_ms(m.at),
                        )
                    })
            },
            |p| (p, captured_at_ms),
        );
        self.push(
            clock,
            at,
            RecordedInput::Pointer {
                position,
                continuity,
                at_ms,
            },
        )
    }
    fn point(&self, point: (i32, i32), source: (u32, u32)) -> (i32, i32) {
        project_canvas_point(point, source, self.geometry.destination)
    }
    fn hook_point(&self, point: (i32, i32)) -> (i32, i32) {
        let desktop = self.geometry.desktop;
        let x = (f64::from(point.0) + f64::from(self.region_origin.0) - desktop.x)
            * f64::from(self.geometry.source.0)
            / desktop.width;
        let y = (f64::from(point.1) + f64::from(self.region_origin.1) - desktop.y)
            * f64::from(self.geometry.source.1)
            / desktop.height;
        self.point((x.round() as i32, y.round() as i32), self.geometry.source)
    }
    pub fn update_geometry(
        &mut self,
        clock: &RecordingClock,
        at: Instant,
        source: (u32, u32),
        transform: Option<DesktopTransform>,
    ) -> Result<bool> {
        let geometry = InputGeometry {
            source,
            desktop: transform.map_or(self.geometry.desktop, |t| t.source),
            destination: fitted_rect(source, self.logical)?,
        };
        if self.geometry != geometry {
            self.geometry = geometry;
            self.last_shape = None;
            self.shapes.clear();
            self.push(clock, at, RecordedInput::Reset)?;
            return Ok(true);
        }
        Ok(false)
    }
    #[cfg(not(target_os = "macos"))]
    pub fn cursor(
        &mut self,
        clock: &RecordingClock,
        at: Instant,
        cursor: &snow_cursor::AttachedCursorSample,
        source: (u32, u32),
    ) -> Result<()> {
        let shape_id = cursor.shape_id().map(|id| id.get());
        if let Some(shape) = cursor.shape.embedded_shape()
            && !self.raw_shapes.iter().any(|s| s.shape_id == shape.shape_id)
        {
            if self.raw_shapes.len() == 8 {
                self.raw_shapes.pop_front();
            }
            self.raw_shapes.push_back(shape.clone());
        }
        if let Some(id) = shape_id
            && !self.shapes.iter().any(|shape| shape.shape_id == id)
        {
            let shape = self
                .raw_shapes
                .iter()
                .find(|s| s.shape_id.get() == id)
                .ok_or_else(|| {
                    ScreenRecorderError::Encode("captured cursor shape is unavailable".into())
                })?;
            let scale = (
                self.geometry.destination.width as f64 / source.0 as f64,
                self.geometry.destination.height as f64 / source.1 as f64,
            );
            let width = (u64::from(shape.width) * u64::from(self.geometry.destination.width)
                / u64::from(source.0))
            .max(1) as u32;
            let height = (u64::from(shape.height) * u64::from(self.geometry.destination.height)
                / u64::from(source.1))
            .max(1) as u32;
            let mut pixels = vec![0; width as usize * height as usize * 4];
            snow_recording_export::resize::NearestResizePlan::new(
                shape.width,
                shape.height,
                width,
                height,
            )
            .resize_into(&shape.shape_rgba, &mut pixels);
            let record = CursorShapeRecord {
                shape_id: shape.shape_id.get(),
                hotspot_x: ((shape.hotspot_x as f64 * scale.0).round() as u32).min(width - 1),
                hotspot_y: ((shape.hotspot_y as f64 * scale.1).round() as u32).min(height - 1),
                width,
                height,
                mode: match shape.composition_mode {
                    snow_cursor::CursorCompositionMode::AlphaBlend => {
                        CursorShapeCompositionMode::AlphaBlend
                    }
                    snow_cursor::CursorCompositionMode::MaskedColor => {
                        CursorShapeCompositionMode::MaskedColor
                    }
                },
                shape_rgba: pixels,
            };
            self.cache_shape(record);
        }
        if shape_id != self.last_shape
            && let Some(id) = shape_id
        {
            let shape = self
                .shapes
                .iter()
                .find(|s| s.shape_id == id)
                .cloned()
                .ok_or_else(|| {
                    ScreenRecorderError::Encode("captured cursor shape is unavailable".into())
                })?;
            self.push(clock, at, RecordedInput::CursorShape(shape))?;
        }
        self.last_shape = shape_id;
        let (x, y) = self.point((cursor.x, cursor.y), source);
        self.push(
            clock,
            at,
            RecordedInput::Cursor(CursorFrameRecord {
                timestamp_ms: self
                    .admission_ms
                    .unwrap_or_else(|| clock.active_elapsed_ms(at)),
                x,
                y,
                visible: cursor.visible,
                shape_id,
            }),
        )
    }
    pub fn cache_shape(&mut self, shape: CursorShapeRecord) {
        if let Some(index) = self
            .shapes
            .iter()
            .position(|s| s.shape_id == shape.shape_id)
        {
            self.shapes.remove(index);
        }
        if self.shapes.len() == 8 {
            self.shapes.pop_front();
        }
        self.shapes.push_back(shape);
    }
    #[cfg(target_os = "macos")]
    pub fn native_cursor(
        &mut self,
        clock: &RecordingClock,
        at: Instant,
        shape: Option<CursorShapeRecord>,
        frame: CursorFrameRecord,
    ) -> Result<()> {
        if let Some(shape) = shape {
            self.cache_shape(shape);
        }
        if frame.shape_id != self.last_shape
            && let Some(id) = frame.shape_id
        {
            let shape = self
                .shapes
                .iter()
                .find(|s| s.shape_id == id)
                .cloned()
                .ok_or_else(|| {
                    ScreenRecorderError::Encode("native cursor shape is unavailable".into())
                })?;
            self.push(clock, at, RecordedInput::CursorShape(shape))?;
        }
        self.last_shape = frame.shape_id;
        self.push(clock, at, RecordedInput::Cursor(frame))
    }
    pub fn finish(self) -> Result<()> {
        self.writer.finish()?;
        Ok(())
    }
}
fn key_record(event: snow_recording_effects::keyboard_overlay::KeyEvent) -> KeyEventRecord {
    KeyEventRecord {
        at_ms: event.at_ms,
        key: event.key,
        down: event.down,
        label: event.label,
        modifiers: event.modifiers,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(all(not(target_os = "macos"), feature = "bench-pipeline-timing"))]
    #[test]
    fn source_trace_reads_marker_and_rgb_before_compression() {
        let size = (528, 96);
        let mut pixels = vec![0; size.0 as usize * size.1 as usize * 4];
        let code = (123u32 << 8) | 0xa5;
        for bit in 0..32 {
            if code & (1 << bit) != 0 {
                let at = (80 * size.0 as usize + 24 + bit * 16) * 4;
                pixels[at..at + 4].fill(255);
            }
        }
        let center = (size.1 as usize / 2 * size.0 as usize + size.0 as usize / 2) * 4;
        pixels[center..center + 4].copy_from_slice(&[10, 20, 30, 255]);
        assert_eq!(
            source_pixel_probe(&pixels, size, false),
            (Some(123), [10, 20, 30])
        );
        assert_eq!(
            source_pixel_probe(&pixels, size, true),
            (Some(123), [30, 20, 10])
        );
        pixels.fill(0);
        assert_eq!(source_pixel_probe(&pixels, size, false), (None, [0; 3]));
    }
    fn test_config(output_path: PathBuf) -> DirectRecordingConfig {
        DirectRecordingConfig {
            loop_animated_images: false,
            region: crate::RecordingRegion::new(0, 0, 16, 16),
            capture_backend: crate::CaptureBackendKind::Auto,
            output_path,
            format: ExportFormat::Mp4,
            capture_fps: 30,
            output_fps: 30,
            maximum_width: None,
            maximum_height: None,
            codec: VideoCodec::H264,
            preset: VideoEncodingSpeed::VeryFast,
            quality: 80,
            prefer_hardware_encoder: false,
            enable_microphone: false,
            enable_system_audio: false,
            audio_mode: Default::default(),
            system_audio_gain_db: 0,
            microphone_gain_db: 0,
            show_cursor: false,
            keyboard: None,
            mouse_trail_rgba: [0; 4],
            mouse_trail_duration_ms: 500,
            mouse_click_rgba: [0; 4],
            mouse_highlight_rgba: [0; 4],
            record_mouse_clicks: false,
            show_keyboard: false,
            excluded_windows: Default::default(),
            excluded_processes: Default::default(),
        }
    }
    #[test]
    fn deferred_audio_controls_share_initial_and_live_gains_with_capture() {
        let mut config = test_config(PathBuf::from("output.mp4"));
        config.system_audio_gain_db = -9;
        config.microphone_gain_db = 6;
        let session = DeferredRecordingSession::create(config, Default::default()).unwrap();
        let capture = session.capture_controls.clone();
        assert_eq!(capture.audio.gain_db(AudioSourceKind::System), -9);
        assert_eq!(capture.audio.gain_db(AudioSourceKind::Microphone), 6);
        let control = session.audio_control();
        control.set_gain_db(AudioSourceKind::System, 12).unwrap();
        control
            .set_gain_db(AudioSourceKind::Microphone, -12)
            .unwrap();
        assert_eq!(capture.audio.gain_db(AudioSourceKind::System), 12);
        assert_eq!(capture.audio.gain_db(AudioSourceKind::Microphone), -12);
        let mut invalid = test_config(PathBuf::from("output.mp4"));
        invalid.system_audio_gain_db = 25;
        assert!(DeferredRecordingSession::create(invalid, Default::default()).is_err());
    }
    #[test]
    fn full_control_queue_rejects_transitions_without_changing_clock_and_cannot_block_stop() {
        let directory = tempfile::tempdir().unwrap();
        let mut session = DeferredRecordingSession::create(
            test_config(directory.path().join("output.mp4")),
            Default::default(),
        )
        .unwrap();
        let (sender, receiver) = crossbeam_channel::bounded(64);
        let origin = Instant::now();
        let clock = RecordingClock::new(origin);
        *session.control_clock.lock().unwrap() = Some(clock.clone());
        session.commands = Some(sender);
        session.state.store(1, Ordering::Release);
        for _ in 0..64 {
            session.send(Command::Pause(origin)).unwrap();
        }
        assert!(session.pause().is_err());
        assert_eq!(session.state(), RecordingState::Running);
        assert!(clock.is_active_at(Instant::now()));
        session.request_stop().unwrap();
        let stop = session.stop_boundary.lock().unwrap().unwrap();
        assert!(!clock.is_active_at(stop));
        assert_eq!(receiver.len(), 64);
        assert!(session.resume().is_err());
    }

    #[test]
    fn concurrent_control_admissions_have_monotonic_accepted_timestamps() {
        let directory = tempfile::tempdir().unwrap();
        let mut session = DeferredRecordingSession::create(
            test_config(directory.path().join("output.mp4")),
            Default::default(),
        )
        .unwrap();
        let (sender, receiver) = crossbeam_channel::bounded(64);
        session.commands = Some(sender);
        session.state.store(1, Ordering::Release);
        *session.control_clock.lock().unwrap() = Some(RecordingClock::new(Instant::now()));
        std::thread::scope(|scope| {
            for _ in 0..8 {
                let session = &session;
                scope.spawn(move || {
                    for _ in 0..16 {
                        let _ = session.pause();
                        let _ = session.resume();
                    }
                });
            }
        });
        session.request_stop().unwrap();
        let mut previous = None;
        for command in receiver.try_iter() {
            let at = match command {
                Command::Pause(at) | Command::Resume(at) | Command::Stop(at) => at,
                Command::Cancel => unreachable!(),
            };
            assert!(previous.is_none_or(|previous| at >= previous));
            previous = Some(at);
        }
        assert!(previous.is_some());
        assert!(session.stop_boundary.lock().unwrap().unwrap() >= previous.unwrap());
    }

    #[test]
    fn accepted_stop_excludes_its_endpoint_from_source_admission_and_index() {
        for duration_ms in [1000, 1001] {
            let directory = tempfile::tempdir().unwrap();
            let path = directory.path().join("video.index");
            let origin = Instant::now() - std::time::Duration::from_secs(2);
            let clock = RecordingClock::new(origin);
            let control = Mutex::new(Some(clock.clone()));
            let stop = Mutex::new(None);
            let mut schedule = crate::output_schedule::OutputSchedule::new(30);
            let mut index = FrameIndexWriter::new(&path, 30).unwrap();
            for at in [0, duration_ms - 1] {
                let slot = schedule.poll(std::time::Duration::from_millis(at)).unwrap();
                index.push(slot.pts).unwrap();
            }
            let admission = control.lock().unwrap();
            std::thread::scope(|scope| {
                let waiting =
                    scope.spawn(|| admit_source_slot(&mut schedule, &clock, &stop, &control));
                let at = origin + std::time::Duration::from_millis(duration_ms);
                clock.controller().mark_pause(at);
                *stop.lock().unwrap() = Some(at);
                drop(admission);
                assert!(waiting.join().unwrap().is_none());
            });
            index.finish(duration_ms).unwrap();
            let bytes = std::fs::read(path).unwrap();
            let records = &bytes[snow_recording_model::VIDEO_INDEX_MAGIC.len()..];
            assert_eq!(
                records.len(),
                2 * snow_recording_model::VIDEO_INDEX_RECORD_BYTES
            );
            let timeline = FinalizedTimeline::new(duration_ms, 30).unwrap();
            for record in records.chunks_exact(snow_recording_model::VIDEO_INDEX_RECORD_BYTES) {
                let timestamp = u64::from_le_bytes(record[8..16].try_into().unwrap());
                let duration = u32::from_le_bytes(record[16..].try_into().unwrap());
                assert!(timestamp + u64::from(duration) <= duration_ms);
                assert!(
                    (u128::from(timestamp) * 30).div_ceil(1000)
                        < u128::from(timeline.frame_count())
                );
            }
        }
    }

    #[test]
    fn packing_failure_preserves_completed_source_files_for_recovery() {
        let parent = tempfile::tempdir().unwrap();
        let directory = tempfile::tempdir_in(parent.path()).unwrap();
        let retained = directory.path().to_path_buf();
        let video = retained.join("source.mp4");
        std::fs::write(&video, b"completed source video").unwrap();
        let config = test_config(parent.path().join("output.mp4"));
        let product = CaptureProduct {
            logical: (16, 16),
            coded: (16, 16),
            duration_ms: 1001,
            media: snow_recording_model::media::RecordedMedia::new(
                snow_media::ColorDescription::SRGB,
                snow_media::CursorMode::Hidden,
                Vec::new(),
            ),
            clock: RecordingClock::new(Instant::now()),
            tracks: Vec::new(),
            video: video.clone(),
            index: retained.join("missing.index"),
            inputs: retained.join("missing.inputs"),
            source_codec: VideoCodec::H264,
            encoder_report: Default::default(),
        };
        let error = finalize_source(
            directory,
            product,
            config,
            parent.path().to_path_buf(),
            PlaybackOverlay::None,
        )
        .err()
        .expect("missing asset must fail packing");
        assert!(error.to_string().contains(&retained.display().to_string()));
        assert_eq!(std::fs::read(video).unwrap(), b"completed source video");
        assert!(retained.join("render.bin").is_file());
        assert!(retained.join("output.bin").is_file());
    }

    #[test]
    fn discard_is_explicit_and_rejects_an_active_render() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("recording.snowrec");
        std::fs::write(&path, b"source").unwrap();
        let config = StreamingEncoderConfig {
            loop_animated_images: false,
            output_path: directory.path().join("final.mp4"),
            format: ExportFormat::Mp4,
            width: 16,
            height: 16,
            fps: 30,
            codec: VideoCodec::H264,
            prefer_hardware_h264: false,
            execution_mode: ExportExecutionMode::SoftwareOnly,
            software_h264_priority: SoftwareH264Priority::X264First,
            video: Default::default(),
            encode_threads: 1,
            audio: Vec::new(),
        };
        let source = DeferredRecordingSource {
            inner: Arc::new(SourceInner {
                path: path.clone(),
                duration_ms: 100,
                config,
                active: Arc::new(AtomicBool::new(true)),
                discarded: AtomicBool::new(false),
                capture_report: Default::default(),
            }),
        };
        assert!(source.discard().is_err());
        assert!(path.exists());
        source.inner.active.store(false, Ordering::Release);
        let clone = source.clone();
        drop(source);
        assert!(path.exists());
        clone.discard().unwrap();
        assert!(!path.exists());
        clone.discard().unwrap();
    }
    #[cfg(not(target_os = "macos"))]
    #[test]
    fn odd_source_canvas_pads_edges_without_stretching_logical_pixels() {
        let frame = snow_capture::frame::Frame::from_rgba8(
            3,
            1,
            vec![1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255],
        )
        .unwrap()
        .into();
        let mut canvas = CleanCanvas::new((3, 1), (4, 2));
        let pixels = canvas.compose(&frame).unwrap();
        assert_eq!(&pixels[..12], &[1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255]);
        assert_eq!(&pixels[12..16], &[7, 8, 9, 255]);
        assert_eq!(&pixels[..16], &pixels[16..]);
    }
    #[test]
    fn geometry_changes_project_input_into_the_same_fitted_canvas_as_video() {
        let landscape = fitted_rect((1920, 1080), (1920, 1080)).unwrap();
        assert_eq!(
            project_canvas_point((960, 540), (1920, 1080), landscape),
            (960, 540)
        );
        let portrait = fitted_rect((1080, 1920), (1920, 1080)).unwrap();
        assert_eq!(
            portrait,
            PixelRect {
                x: 656,
                y: 0,
                width: 607,
                height: 1080
            }
        );
        assert_eq!(
            project_canvas_point((0, 0), (1080, 1920), portrait),
            (656, 0)
        );
        let center = project_canvas_point((540, 960), (1080, 1920), portrait);
        assert!((center.0 - 960).abs() <= 1);
        assert_eq!(center.1, 540);
    }
}

//! The application session facade. All native objects live and die on one worker.
use super::*;
use crate::{DirectRecordingConfig, RecordingState};
use snow_macos::desktop::DesktopTarget;
use snow_media::geometry::{DesktopRect, DesktopSpace};
use std::sync::{
    Arc,
    atomic::{AtomicU8, Ordering},
    mpsc,
};
use std::thread::JoinHandle;

fn capture_config(config: &DirectRecordingConfig) -> Result<DesktopConfig> {
    if !matches!(
        config.capture_backend,
        crate::CaptureBackendKind::Auto | crate::CaptureBackendKind::ScreenCaptureKit
    ) {
        return Err(ScreenRecorderError::UnsupportedFeature(
            "macOS recording requires ScreenCaptureKit".into(),
        ));
    }
    let mut capture = DesktopConfig::new(DesktopTarget::Region(DesktopRect {
        space: DesktopSpace::Points,
        x: f64::from(config.region.x),
        y: f64::from(config.region.y),
        width: f64::from(config.region.width),
        height: f64::from(config.region.height),
    }));
    capture.fps = config.capture_fps;
    capture.cursor = if config.show_cursor {
        snow_media::CursorMode::Embedded
    } else {
        snow_media::CursorMode::Hidden
    };
    capture.excluded_windows = config.excluded_windows.to_vec();
    capture.excluded_processes = config.excluded_processes.to_vec();
    capture.opaque = true;
    Ok(capture)
}

/// Must run off the main thread. Capture geometry remains in desktop points.
pub fn recording_dimensions(
    region: crate::RecordingRegion,
    maximum_width: Option<u32>,
    maximum_height: Option<u32>,
    format: ExportFormat,
) -> Result<PixelSize> {
    let capture = DesktopConfig::new(DesktopTarget::Region(DesktopRect {
        space: DesktopSpace::Points,
        x: f64::from(region.x),
        y: f64::from(region.y),
        width: f64::from(region.width),
        height: f64::from(region.height),
    }));
    resolve_dimensions(&capture, maximum_width, maximum_height, format)
}
fn resolve_dimensions(
    capture: &DesktopConfig,
    maximum_width: Option<u32>,
    maximum_height: Option<u32>,
    format: ExportFormat,
) -> Result<PixelSize> {
    let native = capture
        .capabilities()
        .map_err(native_error)?
        .transform
        .output;
    output_dimensions(native, maximum_width, maximum_height, format)
}
fn output_dimensions(
    native: PixelSize,
    maximum_width: Option<u32>,
    maximum_height: Option<u32>,
    format: ExportFormat,
) -> Result<PixelSize> {
    let (width, height) = snow_recording_export::scaled_output_dimensions(
        native.width,
        native.height,
        maximum_width,
        maximum_height,
        format,
    );
    PixelSize::new(width, height).map_err(|e| ScreenRecorderError::InvalidConfig(e.to_string()))
}
pub(super) fn native_config(
    config: DirectRecordingConfig,
    cancellation: snow_macos::CancellationToken,
) -> Result<NativeRecordingConfig> {
    let mut capture = capture_config(&config)?;
    capture.cancellation = cancellation;
    let output = resolve_dimensions(
        &capture,
        config.maximum_width,
        config.maximum_height,
        config.format,
    )?;
    let effects = config
        .render_config(
            (output.width, output.height),
            snow_recording_model::PlaybackOverlay::None,
        )
        .effects;
    let audio = (config.format == ExportFormat::Mp4
        && (config.enable_microphone || config.enable_system_audio))
        .then(|| {
            let mut audio = AudioStreamConfig::default();
            audio.system.enabled = config.enable_system_audio;
            audio.microphone.enabled = config.enable_microphone;
            audio.system.required = config.enable_system_audio;
            audio.microphone.required = config.enable_microphone;
            audio
        });
    Ok(NativeRecordingConfig {
        audio_mode: config.audio_mode,
        capture,
        output,
        output_path: config.output_path,
        fps: config.output_fps,
        format: config.format,
        loop_animated_images: config.loop_animated_images,
        video: snow_recording_model::VideoEncodeConfig {
            quality: config.quality,
            speed: config.preset,
        },
        codec: config.codec,
        execution: if config.prefer_hardware_encoder && config.format == ExportFormat::Mp4 {
            ExportExecutionMode::HardwarePreferred
        } else {
            ExportExecutionMode::SoftwareOnly
        },
        audio,
        effects: NativeEffectsConfig {
            clicks: effects.mouse_click_rgba[3] != 0,
            trail: effects.mouse_trail_rgba[3] != 0,
            click_rgba: effects.mouse_click_rgba,
            trail_rgba: effects.mouse_trail_rgba,
            trail_duration_ms: effects.mouse_trail_duration_ms,
            keyboard: effects.keyboard,
            show_keyboard: effects.show_keyboard,
            record_mouse_clicks: effects.record_mouse_clicks,
            highlight_rgba: if effects.show_cursor {
                effects.mouse_highlight_rgba
            } else {
                [0; 4]
            },
        },
    })
}
enum Command {
    Pause(Instant),
    Resume(Instant),
    Finish(Instant),
}
pub struct DirectSession {
    config: Option<DirectRecordingConfig>,
    commands: Option<crossbeam_channel::Sender<Command>>,
    worker: Option<JoinHandle<Result<NativeRecordingReport>>>,
    state: Arc<AtomicU8>,
    cancellation: snow_macos::CancellationToken,
    stop_requested: std::sync::atomic::AtomicBool,
    stop_boundary: Arc<std::sync::Mutex<Option<Instant>>>,
    control_clock: Arc<std::sync::Mutex<Option<RecordingClock>>>,
}
impl DirectSession {
    pub fn create(config: DirectRecordingConfig) -> Result<Self> {
        config
            .validate()
            .map_err(ScreenRecorderError::InvalidConfig)?;
        capture_config(&config)?;
        if config.capture_fps > 240 || config.output_fps > 240 {
            return Err(ScreenRecorderError::InvalidConfig(
                "macOS recording requires 1..240 fps".into(),
            ));
        }
        Ok(Self {
            config: Some(config),
            commands: None,
            worker: None,
            state: Arc::new(AtomicU8::new(0)),
            cancellation: Default::default(),
            stop_requested: std::sync::atomic::AtomicBool::new(false),
            stop_boundary: Arc::new(std::sync::Mutex::new(None)),
            control_clock: Arc::new(std::sync::Mutex::new(None)),
        })
    }
    pub fn start(&mut self) -> Result<()> {
        let config = self.config.take().ok_or_else(|| {
            ScreenRecorderError::InvalidConfig("recording already started".into())
        })?;
        let (tx, rx) = crossbeam_channel::bounded(64);
        let (ready_tx, ready_rx) = mpsc::sync_channel(1);
        let state = self.state.clone();
        let cancellation = self.cancellation.clone();
        let stop_boundary = Arc::clone(&self.stop_boundary);
        let control_clock = Arc::clone(&self.control_clock);
        self.worker = Some(
            std::thread::Builder::new()
                .name("snow-macos-recording".into())
                .spawn(move || {
                    snow_core::qos::apply_current_thread();
                    let result = (|| {
                        let mut recording =
                            NativeRecordingSession::start(native_config(config, cancellation)?)?;
                        recording.set_stop_boundary(Arc::clone(&stop_boundary));
                        *control_clock.lock().unwrap_or_else(|e| e.into_inner()) =
                            Some(recording.source_clock());
                        state.store(1, Ordering::Release);
                        let _ = ready_tx.send(());
                        loop {
                            if let Some(at) =
                                *stop_boundary.lock().unwrap_or_else(|e| e.into_inner())
                            {
                                recording.freeze_source(at);
                                return recording.finish();
                            }
                            let command = if recording.paused {
                                match rx.recv_timeout(Duration::from_millis(20)) {
                                    Ok(command) => Some(command),
                                    Err(crossbeam_channel::RecvTimeoutError::Timeout) => None,
                                    Err(_) => return Err(ScreenRecorderError::ExportCanceled),
                                }
                            } else {
                                match rx.try_recv() {
                                    Ok(command) => Some(command),
                                    Err(crossbeam_channel::TryRecvError::Empty) => None,
                                    Err(_) => return Err(ScreenRecorderError::ExportCanceled),
                                }
                            };
                            match command {
                                Some(Command::Pause(at)) => {
                                    recording.pause_capture_at(at);
                                }
                                Some(Command::Resume(_)) => {
                                    recording.resume_capture();
                                }
                                Some(Command::Finish(at)) => {
                                    recording.freeze_source(at);
                                    return recording.finish();
                                }
                                None => {}
                            }
                            if let Err(error) = recording.step(Duration::from_millis(20)) {
                                return Err(recording.preserve_failure(error));
                            }
                        }
                    })();
                    state.store(3, Ordering::Release);
                    result
                })
                .map_err(ScreenRecorderError::Io)?,
        );
        self.commands = Some(tx);
        if ready_rx.recv().is_err() {
            return self.join().map(|_| ());
        }
        Ok(())
    }
    fn send(&self, command: Command) -> Result<()> {
        self.commands
            .as_ref()
            .ok_or_else(|| ScreenRecorderError::InvalidConfig("recording has not started".into()))?
            .try_send(command)
            .map_err(|_| ScreenRecorderError::Encode("recording worker stopped".into()))
    }
    pub fn pause(&self) -> Result<()> {
        self.transition(1, 2)
    }
    pub fn resume(&self) -> Result<()> {
        self.transition(2, 1)
    }
    fn transition(&self, from: u8, to: u8) -> Result<()> {
        let control = self.control_clock.lock().unwrap_or_else(|e| e.into_inner());
        let sender = self.commands.as_ref().ok_or_else(|| {
            ScreenRecorderError::InvalidConfig("recording has not started".into())
        })?;
        if self.stop_requested.load(Ordering::Acquire) || self.state.load(Ordering::Acquire) != from
        {
            return Err(ScreenRecorderError::InvalidConfig(
                "invalid recording state transition".into(),
            ));
        }
        if sender.is_full() {
            return Err(ScreenRecorderError::InvalidConfig(
                "recording control queue is full".into(),
            ));
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
        self.send(command)?;
        self.state.store(to, Ordering::Release);
        Ok(())
    }
    pub fn state(&self) -> RecordingState {
        match self.state.load(Ordering::Acquire) {
            0 => RecordingState::Created,
            1 => RecordingState::Running,
            2 => RecordingState::Paused,
            _ => RecordingState::Stopped,
        }
    }
    pub fn stop(mut self) -> Result<NativeRecordingReport> {
        self.request_stop()?;
        self.join()
    }
    pub fn request_stop(&self) -> Result<()> {
        let control = self.control_clock.lock().unwrap_or_else(|e| e.into_inner());
        if self.commands.is_none() {
            return Err(ScreenRecorderError::InvalidConfig(
                "recording has not started".into(),
            ));
        }
        if !self.stop_requested.swap(true, Ordering::AcqRel) {
            let at = Instant::now();
            if let Some(clock) = control.as_ref() {
                clock.controller().mark_pause(at);
            }
            *self.stop_boundary.lock().unwrap_or_else(|e| e.into_inner()) = Some(at);
            let _ = self.send(Command::Finish(at));
        }
        Ok(())
    }
    fn join(&mut self) -> Result<NativeRecordingReport> {
        self.worker
            .take()
            .ok_or_else(|| {
                ScreenRecorderError::InvalidConfig("recording worker is unavailable".into())
            })?
            .join()
            .map_err(|_| ScreenRecorderError::Encode("recording worker panicked".into()))?
    }
}
impl Drop for DirectSession {
    fn drop(&mut self) {
        self.cancellation.cancel();
        self.commands.take();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn output_dimensions(config: &DirectRecordingConfig, native: PixelSize) -> Result<PixelSize> {
        super::output_dimensions(
            native,
            config.maximum_width,
            config.maximum_height,
            config.format,
        )
    }
    fn config() -> DirectRecordingConfig {
        DirectRecordingConfig {
            audio_mode: Default::default(),
            loop_animated_images: true,
            region: crate::RecordingRegion::new(-321, -99, 641, 359),
            capture_backend: crate::CaptureBackendKind::Auto,
            output_path: "test.mp4".into(),
            format: ExportFormat::Mp4,
            capture_fps: 60,
            output_fps: 30,
            maximum_width: None,
            maximum_height: None,
            codec: VideoCodec::H264,
            preset: snow_recording_model::VideoEncodingSpeed::VeryFast,
            quality: 80,
            prefer_hardware_encoder: true,
            enable_microphone: false,
            enable_system_audio: false,
            show_cursor: true,
            keyboard: None,
            mouse_trail_rgba: [0; 4],
            mouse_trail_duration_ms: 500,
            mouse_click_rgba: [0; 4],
            mouse_highlight_rgba: [0; 4],
            record_mouse_clicks: false,
            show_keyboard: false,
            excluded_windows: vec![123].into(),
            excluded_processes: vec![456].into(),
        }
    }
    #[test]
    fn logical_region_is_never_aligned_or_scaled() {
        let config = config();
        let capture = capture_config(&config).unwrap();
        let DesktopTarget::Region(region) = capture.target else {
            panic!("region")
        };
        assert_eq!(
            (region.x, region.y, region.width, region.height),
            (-321., -99., 641., 359.)
        );
        assert_eq!(region.space, DesktopSpace::Points);
        assert_eq!(capture.excluded_windows, [123]);
        assert_eq!(capture.excluded_processes, [456]);
        assert_eq!(capture.fps, 60);
    }
    #[test]
    fn output_uses_native_resolution_then_quality_and_codec_alignment() {
        let mut config = config();
        assert_eq!(
            output_dimensions(&config, PixelSize::new(1282, 718).unwrap()).unwrap(),
            PixelSize::new(1282, 718).unwrap()
        );
        config.maximum_width = Some(640);
        config.maximum_height = Some(480);
        let size = output_dimensions(&config, PixelSize::new(1282, 718).unwrap()).unwrap();
        assert_eq!(size.width, 640);
        assert_eq!(size.height % 2, 0);
        assert_eq!((config.region.width, config.region.height), (641, 359));
        for format in [ExportFormat::Gif, ExportFormat::Apng, ExportFormat::Webp] {
            config.format = format;
            config.maximum_width = None;
            config.maximum_height = None;
            assert_eq!(
                output_dimensions(&config, PixelSize::new(641, 359).unwrap()).unwrap(),
                PixelSize::new(641, 359).unwrap()
            );
        }
    }
    #[test]
    fn create_and_drop_does_not_start_native_capture() {
        let session = DirectSession::create(config()).unwrap();
        assert!(matches!(session.state(), RecordingState::Created));
        assert!(session.pause().is_err());
        drop(session);
        let mut config = config();
        config.capture_fps = 241;
        assert!(DirectSession::create(config).is_err());
    }
}

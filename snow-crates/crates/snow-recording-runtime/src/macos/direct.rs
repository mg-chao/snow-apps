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
fn native_config(
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
        capture,
        output,
        output_path: config.output_path,
        fps: config.output_fps,
        format: config.format,
        loop_animated_images: config.loop_animated_images,
        video: snow_recording_model::VideoEncodeConfig {
            quality: 80,
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
            clicks: config.mouse_click_rgba[3] != 0,
            trail: config.mouse_trail_rgba[3] != 0,
            click_rgba: config.mouse_click_rgba,
            trail_rgba: config.mouse_trail_rgba,
            trail_duration_ms: config.mouse_trail_duration_ms,
            keyboard: config.keyboard,
            show_keyboard: config.show_keyboard,
            record_mouse_clicks: config.record_mouse_clicks,
            highlight_rgba: if config.show_cursor {
                config.mouse_highlight_rgba
            } else {
                [0; 4]
            },
        },
    })
}
enum Command {
    Pause,
    Resume,
    Finish,
}
pub struct DirectSession {
    config: Option<DirectRecordingConfig>,
    commands: Option<mpsc::Sender<Command>>,
    worker: Option<JoinHandle<Result<NativeRecordingReport>>>,
    state: Arc<AtomicU8>,
    cancellation: snow_macos::CancellationToken,
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
        })
    }
    pub fn start(&mut self) -> Result<()> {
        let config = self.config.take().ok_or_else(|| {
            ScreenRecorderError::InvalidConfig("recording already started".into())
        })?;
        let (tx, rx) = mpsc::channel();
        let (ready_tx, ready_rx) = mpsc::sync_channel(1);
        let state = self.state.clone();
        let cancellation = self.cancellation.clone();
        self.worker = Some(
            std::thread::Builder::new()
                .name("snow-macos-recording".into())
                .spawn(move || {
                    let result = (|| {
                        let mut recording =
                            NativeRecordingSession::start(native_config(config, cancellation)?)?;
                        state.store(1, Ordering::Release);
                        let _ = ready_tx.send(());
                        loop {
                            let command = if recording.paused {
                                match rx.recv_timeout(Duration::from_millis(20)) {
                                    Ok(command) => Some(command),
                                    Err(mpsc::RecvTimeoutError::Timeout) => None,
                                    Err(_) => return Err(ScreenRecorderError::ExportCanceled),
                                }
                            } else {
                                match rx.try_recv() {
                                    Ok(command) => Some(command),
                                    Err(mpsc::TryRecvError::Empty) => None,
                                    Err(_) => return Err(ScreenRecorderError::ExportCanceled),
                                }
                            };
                            match command {
                                Some(Command::Pause) => {
                                    recording.pause();
                                    state.store(2, Ordering::Release);
                                }
                                Some(Command::Resume) => {
                                    recording.resume();
                                    state.store(1, Ordering::Release);
                                }
                                Some(Command::Finish) => return recording.finish(),
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
            .send(command)
            .map_err(|_| ScreenRecorderError::Encode("recording worker stopped".into()))
    }
    pub fn pause(&self) -> Result<()> {
        self.send(Command::Pause)
    }
    pub fn resume(&self) -> Result<()> {
        self.send(Command::Resume)
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
        let _ = self.send(Command::Finish);
        self.join()
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

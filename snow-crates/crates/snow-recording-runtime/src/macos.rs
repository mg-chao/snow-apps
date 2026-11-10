//! Native macOS recording orchestration. Drive `step` on one worker thread;
//! the host must keep its main run loop active. Video never maps to CPU in
//! hardware mode. FFmpeg owns muxing, audio encoding and finalization.
use crate::direct::{LiveAudioMixer, process_audio_event};
use crate::macos_effects::Effects;
pub use crate::macos_effects::NativeEffectsConfig;
use crate::{ScreenRecorderError, error::Result};
use snow_audio_recorder::{
    AudioControlHandle, AudioEvent, AudioSession, AudioStreamConfig, AudioStreamHandle,
};
use snow_core::{cancellation::CancellationToken, recording_clock::RecordingClock};
use snow_macos::{
    MacError,
    compositor::{Compositor, Layer},
    desktop::{DesktopConfig, DesktopEvent, DesktopFrame, DesktopSession},
};
use snow_media::{
    DynamicRange, PixelFormat,
    geometry::{DesktopTransform, PixelRect, PixelSize, aspect_fit},
};
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoder,
    StreamingEncoderConfig, StreamingEncoderReport, VideoCodec,
};
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

mod schedule;
use schedule::{ScheduledFrame, VideoSchedule};

#[derive(Clone)]
pub struct NativeRecordingConfig {
    pub capture: DesktopConfig,
    pub output: PixelSize,
    pub output_path: PathBuf,
    pub fps: u32,
    pub format: ExportFormat,
    pub loop_animated_images: bool,
    pub video: snow_recording_model::VideoEncodeConfig,
    pub codec: VideoCodec,
    pub execution: ExportExecutionMode,
    pub audio: Option<AudioStreamConfig>,
    pub audio_mode: crate::RecordingAudioMode,
    pub effects: NativeEffectsConfig,
    pub webcam: Option<snow_webcam::Config>,
}
#[derive(Clone, Debug)]
pub enum NativeRecordingEvent {
    Configuration {
        transform: DesktopTransform,
        generation: u64,
    },
    Frame {
        pts: u64,
    },
    Idle,
    Interruption {
        at_ms: u64,
        reason: String,
    },
}
#[derive(Clone, Debug)]
pub struct NativeRecordingReport {
    pub encoder: StreamingEncoderReport,
    /// In-memory metadata; editable recordings embed this in their bundle.
    /// Direct outputs do not implicitly create a standalone manifest.
    pub media: snow_recording_model::media::RecordedMedia,
    pub geometry_changes: Vec<(u64, DesktopTransform)>,
    pub interruptions: Vec<(u64, String)>,
    pub cpu_readbacks: u64,
}
pub struct NativeRecordingSession {
    config: NativeRecordingConfig,
    effects: Option<Effects>,
    webcam: snow_recording_effects::webcam::WebcamOverlay,
    _webcam_capture: crate::webcam::CaptureLease,
    capture: DesktopSession,
    compositor: Option<Compositor>,
    encoder: StreamingEncoder,
    audio: Option<AudioStreamHandle>,
    mixer: Option<LiveAudioMixer>,
    clock: RecordingClock,
    paused: bool,
    native: bool,
    interrupted: bool,
    media: snow_recording_model::media::RecordedMedia,
    video_schedule: VideoSchedule<DesktopFrame>,
    last_composition: Option<CompositionRevision>,
    cpu_pixels: snow_memory::RasterBuffer,
    accepted_transform: Option<DesktopTransform>,
    geometry_changes: Vec<(u64, DesktopTransform)>,
    interruptions: Vec<(u64, String)>,
    cpu_readbacks: u64,
    audio_ends: [Option<Instant>; 2],
    logical_output: Option<PixelSize>,
    stop_boundary: Option<std::sync::Arc<std::sync::Mutex<Option<Instant>>>>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct CompositionRevision {
    source: u64,
    effects: u64,
    webcam: Option<u64>,
}

pub(crate) fn native_error(error: MacError) -> ScreenRecorderError {
    match error {
        MacError::PermissionDenied => {
            ScreenRecorderError::PermissionDenied(crate::MediaPermission::Screen)
        }
        MacError::MicrophonePermissionDenied => {
            ScreenRecorderError::PermissionDenied(crate::MediaPermission::Microphone)
        }
        MacError::InputPermissionDenied => {
            ScreenRecorderError::PermissionDenied(crate::MediaPermission::InputMonitoring)
        }
        MacError::TargetUnavailable => {
            ScreenRecorderError::Capture(snow_capture::error::CaptureError::MonitorLost)
        }
        MacError::Canceled => {
            ScreenRecorderError::Capture(snow_capture::error::CaptureError::Canceled)
        }
        MacError::Timeout => {
            ScreenRecorderError::Capture(snow_capture::error::CaptureError::Timeout)
        }
        MacError::UnsupportedOs => {
            ScreenRecorderError::UnsupportedFeature("macOS 15 or later is required".into())
        }
        MacError::InvalidConfig(message) => ScreenRecorderError::InvalidConfig(message),
        MacError::Unsupported(message) => ScreenRecorderError::UnsupportedFeature(message),
        other => ScreenRecorderError::Capture(snow_capture::error::CaptureError::platform(other)),
    }
}
impl NativeRecordingSession {
    pub fn start(config: NativeRecordingConfig) -> Result<Self> {
        Self::start_with_controls(config, AudioControlHandle::new())
    }
    pub fn start_with_controls(
        config: NativeRecordingConfig,
        audio_control: AudioControlHandle,
    ) -> Result<Self> {
        Self::start_inner(config, false, None, audio_control)
    }
    pub(super) fn start_source(
        config: NativeRecordingConfig,
        logical_output: PixelSize,
    ) -> Result<Self> {
        Self::start_inner(
            config,
            true,
            Some(logical_output),
            AudioControlHandle::new(),
        )
    }
    fn start_inner(
        mut config: NativeRecordingConfig,
        source: bool,
        logical_output: Option<PixelSize>,
        audio_control: AudioControlHandle,
    ) -> Result<Self> {
        if let Some(webcam) = &config.webcam {
            webcam
                .style
                .validate()
                .map_err(ScreenRecorderError::InvalidConfig)?;
        }
        // Draw the pointer above both the camera and the highlight tint.
        config.capture.cursor = overlay_cursor_mode(
            config.capture.cursor,
            config.effects.highlight_rgba[3] != 0 || config.webcam.is_some(),
        );
        if config.capture.cancellation.is_canceled() {
            return Err(native_error(MacError::Canceled));
        }
        if config.fps == 0
            || config.fps > 240
            || (config.format.requires_even_dimensions()
                && (!config.output.width.is_multiple_of(2)
                    || !config.output.height.is_multiple_of(2)))
        {
            return Err(ScreenRecorderError::InvalidConfig(
                "recording requires 1..240 fps and even output dimensions".into(),
            ));
        }
        let hdr = config.capture.dynamic_range == DynamicRange::Hdr;
        if hdr
            && (config.format != ExportFormat::Mp4
                || !snow_recording_export::preserves_hdr_output(hdr, config.format, config.codec))
        {
            return Err(ScreenRecorderError::UnsupportedFeature(
                "HDR recording requires HEVC Main10 encoding".into(),
            ));
        }
        if config
            .audio
            .as_ref()
            .is_some_and(|audio| audio.microphone.enabled && audio.microphone.required)
            && !snow_macos::microphone::microphone_authorized()
        {
            return Err(ScreenRecorderError::PermissionDenied(
                crate::MediaPermission::Microphone,
            ));
        }
        if config.effects.show_keyboard {
            snow_macos::text::prepare_keyboard_layout();
        }
        config.capture.opaque = true;
        if config.capture.follow_mouse {
            // Produce the fixed recording canvas directly on GPU. A panning
            // viewport must not resize once at native scale and again at export size.
            config.capture.output = Some(logical_output.unwrap_or(config.output));
        }
        let capture = DesktopSession::new(config.capture.clone()).map_err(native_error)?;
        let format = if hdr {
            PixelFormat::P010
        } else {
            PixelFormat::Bgra8
        };
        let encode_config = StreamingEncoderConfig {
            loop_animated_images: config.loop_animated_images,
            output_path: config.output_path.clone(),
            format: config.format,
            width: config.output.width,
            height: config.output.height,
            fps: config.fps,
            codec: config.codec,
            prefer_hardware_h264: config.execution != ExportExecutionMode::SoftwareOnly,
            execution_mode: config.execution,
            software_h264_priority: SoftwareH264Priority::X264First,
            video: config.video,
            encode_threads: 0,
            audio: config
                .audio
                .as_ref()
                .map(|audio| {
                    let mode = if config.format == ExportFormat::Mp4 {
                        config.audio_mode
                    } else {
                        crate::RecordingAudioMode::Mixed
                    };
                    mode.tracks(audio.system.enabled, audio.microphone.enabled, 192)
                })
                .unwrap_or_default(),
        };
        let mut base_builder = StreamingEncoder::builder(encode_config);
        if source {
            base_builder = base_builder.recording_source();
        }
        let (mut encoder, native) = if config.execution == ExportExecutionMode::SoftwareOnly
            || config.format.is_animated_image()
        {
            let mut builder = base_builder.software_only();
            if hdr {
                builder = builder.hdr10_cpu_input();
            }
            (builder.create()?, false)
        } else {
            match base_builder.clone().native_input(format).create() {
                Ok(encoder) => (encoder, true),
                Err(_) if config.execution == ExportExecutionMode::HardwarePreferred => (
                    base_builder.native_input(format).software_only().create()?,
                    false,
                ),
                Err(error) => return Err(error.into()),
            }
        };
        encoder.retain_failed_output();
        let audio = config
            .audio
            .clone()
            .map(|mut audio| {
                audio.cancellation = config.capture.cancellation.clone();
                audio.system.output_format = snow_audio_recorder::AudioFormat::new(48_000, 2);
                audio.microphone.output_format = snow_audio_recorder::AudioFormat::new(48_000, 2);
                AudioSession::new()?.start_streaming_with_controls(audio, audio_control.clone())
            })
            .transpose()?;
        let mixer = config
            .audio
            .as_ref()
            .map(|a| LiveAudioMixer::new(a.system.enabled, a.microphone.enabled));
        let effects = Effects::new(config.effects.clone(), config.output, config.capture.cursor)?;
        // Startup (including audio-device initialization) is outside the recording timeline.
        let clock = RecordingClock::new(Instant::now());
        let webcam_capture =
            crate::webcam::CaptureLease::with_options(config.webcam.as_ref(), config.fps);
        Ok(Self {
            webcam: Default::default(),
            _webcam_capture: webcam_capture,
            media: snow_recording_model::media::RecordedMedia::new(
                if hdr {
                    snow_media::ColorDescription::HDR10
                } else {
                    snow_media::ColorDescription::SRGB
                },
                config.capture.cursor,
                Vec::new(),
            ),
            effects,
            video_schedule: VideoSchedule::new(config.fps),
            last_composition: None,
            cpu_pixels: snow_memory::RasterBuffer::new(),
            config,
            capture,
            compositor: None,
            encoder,
            audio,
            mixer,
            clock,
            paused: false,
            native,
            interrupted: false,
            accepted_transform: None,
            geometry_changes: Vec::new(),
            interruptions: Vec::new(),
            cpu_readbacks: 0,
            audio_ends: [None; 2],
            logical_output,
            stop_boundary: None,
        })
    }
    pub(crate) fn preserve_failure(&mut self, error: ScreenRecorderError) -> ScreenRecorderError {
        if self.config.capture.cancellation.is_canceled() {
            return ScreenRecorderError::ExportCanceled;
        }
        crate::capture_failure::preserve(&mut self.encoder, &self.clock, Instant::now(), error)
    }
    pub fn pause(&mut self) {
        self.pause_at(Instant::now());
    }
    pub(super) fn pause_at(&mut self, at: Instant) {
        self.clock.controller().mark_pause(at);
        self.pause_capture_at(at);
    }
    pub(super) fn pause_capture_at(&mut self, at: Instant) {
        if !self.paused {
            self.paused = true;
            if let Some(webcam) = &self.config.webcam {
                webcam.source.set_recording_active(false);
            }
            if let Some(audio) = &self.audio {
                audio.pause();
            }
            if let Some(mixer) = &mut self.mixer {
                mixer.reset_alignment(None);
            }
            if let Some(effects) = &mut self.effects {
                effects.reset(self.clock.active_elapsed_ms(at));
            }
            self.capture.release_capture_access();
            self.video_schedule.clear();
        }
    }
    pub fn resume(&mut self) {
        self.resume_at(Instant::now());
    }
    pub(super) fn resume_at(&mut self, at: Instant) {
        self.clock.controller().mark_resume(at);
        self.resume_capture();
    }
    pub(super) fn resume_capture(&mut self) {
        if self.paused {
            self.paused = false;
            if let Some(webcam) = &self.config.webcam {
                webcam.source.set_recording_active(true);
            }
            if let Some(audio) = &self.audio {
                audio.resume();
            }
        }
    }
    fn drain_audio(&mut self, flush: bool) -> Result<()> {
        if let Some(audio) = &self.audio {
            while let Ok(event) = audio.try_recv() {
                if let AudioEvent::Error(error) = event {
                    return Err(ScreenRecorderError::Audio(error));
                }
                if !self.paused
                    && let Some(discontinuity) =
                        audio_discontinuity(&event, &self.clock, &mut self.audio_ends)?
                {
                    self.media.discontinuities.push(discontinuity);
                }
                process_audio_event(event, &self.clock, self.paused, self.mixer.as_mut());
            }
        }
        if let Some(mixer) = &mut self.mixer {
            mixer.emit_ready(
                self.clock.active_elapsed_duration(Instant::now()),
                flush,
                &mut self.encoder,
            )?;
        }
        Ok(())
    }
    pub fn step(&mut self, timeout: Duration) -> Result<NativeRecordingEvent> {
        if self.config.capture.cancellation.is_canceled() {
            return Err(native_error(MacError::Canceled));
        }
        self.drain_audio(false)?;
        self.encoder.poll_native_packets()?;
        if self.paused {
            self.capture.poll_exclusions().map_err(native_error)?;
            return Ok(NativeRecordingEvent::Idle);
        }
        let wait = self.video_schedule.wait(
            self.clock.active_elapsed_duration(Instant::now()),
            timeout.min(Duration::from_millis(100)),
        );
        let observation = match self.capture.next_event(wait) {
            Ok(DesktopEvent::Configuration {
                transform,
                generation,
            }) => {
                self.video_schedule.clear();
                if let Some(effects) = &mut self.effects {
                    effects.reset(self.clock.active_elapsed_ms(Instant::now()));
                }
                self.geometry_changes.push((generation, transform));
                self.media
                    .geometry
                    .push(snow_recording_model::media::GeometryChange {
                        timestamp_ms: self.clock.active_elapsed_ms(Instant::now()),
                        generation,
                        transform,
                        destination: aspect_fit(
                            transform.output,
                            self.logical_output.unwrap_or(self.config.output),
                        )
                        .map_err(|e| ScreenRecorderError::InvalidConfig(e.to_string()))?,
                    });
                return Ok(NativeRecordingEvent::Configuration {
                    transform,
                    generation,
                });
            }
            Ok(DesktopEvent::Frame(frame)) => {
                if !frame.duplicate && self.clock.is_active_at(frame.acquired_at) {
                    Some((self.clock.active_elapsed_duration(frame.acquired_at), frame))
                } else {
                    None
                }
            }
            Err(MacError::Timeout) => None,
            Err(MacError::Inactive) => {
                self.video_schedule.clear();
                if std::mem::replace(&mut self.interrupted, true) {
                    return Ok(NativeRecordingEvent::Idle);
                }
                let at_ms = self.clock.active_elapsed_ms(Instant::now());
                let reason = "capture source interrupted".to_owned();
                self.interruptions.push((at_ms, reason.clone()));
                self.media.discontinuities.push(
                    snow_recording_model::media::TimelineDiscontinuity {
                        timestamp: snow_media::time::MediaTime {
                            value: i64::try_from(at_ms).map_err(|_| {
                                ScreenRecorderError::InvalidConfig("timeline overflow".into())
                            })?,
                            timescale: 1000,
                            domain: snow_media::time::ClockDomain::Session,
                            epoch: 0,
                        },
                        duration: None,
                        reason: snow_recording_model::media::DiscontinuityReason::SourceInterrupted,
                    },
                );
                return Ok(NativeRecordingEvent::Interruption { at_ms, reason });
            }
            Err(error) => return Err(native_error(error)),
        };
        let now = Instant::now();
        if !self.clock.is_active_at(now)
            || self.stop_boundary.as_ref().is_some_and(|boundary| {
                boundary
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .is_some_and(|stop| now >= stop)
            })
        {
            return Ok(NativeRecordingEvent::Idle);
        }
        match self
            .video_schedule
            .poll(self.clock.active_elapsed_duration(now), observation)
        {
            Some(frame) => self.submit(frame),
            None => Ok(NativeRecordingEvent::Idle),
        }
    }
    fn submit(&mut self, scheduled: ScheduledFrame<DesktopFrame>) -> Result<NativeRecordingEvent> {
        let ScheduledFrame {
            frame,
            revision,
            slot,
        } = scheduled;
        self.interrupted = false;
        let pts = slot.pts;
        let timestamp_ms = slot.at.as_millis().min(u128::from(u64::MAX)) as u64;
        let size = frame.image.size();
        let destination = aspect_fit(size, self.logical_output.unwrap_or(self.config.output))
            .map_err(|e| ScreenRecorderError::InvalidConfig(e.to_string()))?;
        let (tiles, interruption) = if let Some(effects) = &mut self.effects {
            effects.draw(&self.clock, frame.transform, destination, timestamp_ms)?
        } else {
            (Vec::new(), None)
        };
        if let Some(reason) = interruption {
            self.interruptions
                .push((self.clock.active_elapsed_ms(frame.acquired_at), reason));
        }
        if let Some(webcam) = &self.config.webcam {
            let sample = webcam.source.recording_snapshot(&self.clock, slot.at);
            self.webcam.prepare(
                sample.frame.as_deref(),
                webcam.style,
                (self.config.output.width, self.config.output.height),
            );
        }
        let webcam =
            self.webcam
                .image()
                .map(|image| snow_macos::compositor::RevisionedRgbaOverlay {
                    revision: image.generation,
                    overlay: snow_macos::compositor::RgbaOverlay {
                        x: image.rect.0,
                        y: image.rect.1,
                        width: image.rect.2,
                        height: image.rect.2,
                        stride: image.rect.2 as usize * 4,
                        bytes: image.pixels,
                    },
                });
        let composition = CompositionRevision {
            source: revision,
            effects: self.effects.as_ref().map_or(0, Effects::revision),
            webcam: self.webcam.image().map(|image| image.generation),
        };
        if self.last_composition == Some(composition) {
            self.encoder.observe_unchanged_video_frame_at_pts(pts)?;
            return Ok(NativeRecordingEvent::Idle);
        }
        if webcam.is_none()
            && let Some(compositor) = &mut self.compositor
        {
            compositor.clear_cached_background_overlay();
        }
        let overlays: Vec<_> = tiles
            .iter()
            .map(|tile| snow_macos::compositor::RgbaOverlay {
                x: tile.x,
                y: tile.y,
                width: 128,
                height: 128,
                stride: 512,
                bytes: tile.pixels.as_slice(),
            })
            .collect();
        let highlight_tiles = self
            .effects
            .as_ref()
            .map(|effects| effects.highlight_tiles())
            .unwrap_or_default();
        let highlight: Vec<_> = highlight_tiles
            .iter()
            .map(|tile| snow_macos::compositor::RgbaOverlay {
                x: tile.x,
                y: tile.y,
                width: 128,
                height: 128,
                stride: 512,
                bytes: tile.pixels.as_slice(),
            })
            .collect();
        let output_format = if self.config.capture.dynamic_range == DynamicRange::Hdr {
            PixelFormat::P010
        } else {
            PixelFormat::Bgra8
        };
        let image = if encoder_accepts_source(
            frame.image.size(),
            frame.image.format(),
            self.config.output,
            output_format,
            destination,
            overlays.is_empty() && highlight.is_empty() && webcam.is_none(),
        ) {
            frame.image.clone()
        } else {
            if self.compositor.is_none() {
                self.compositor = Some(
                    Compositor::new(self.config.output, output_format, 4).map_err(native_error)?,
                );
            }
            let mut layers = smallvec::SmallVec::<[Layer<'_>; 4]>::new();
            layers.push(Layer {
                image: &frame.image,
                source: PixelRect {
                    x: 0,
                    y: 0,
                    width: size.width,
                    height: size.height,
                },
                destination,
            });
            // A 4:2:0 clean source may have one padded row/column. Replicate
            // fitted edge pixels so chroma filtering cannot bleed black into
            // the odd logical canvas that replay restores before overlays.
            if let Some(logical) = self.logical_output {
                let right = self.config.output.width > logical.width
                    && destination.x + destination.width == logical.width;
                let bottom = self.config.output.height > logical.height
                    && destination.y + destination.height == logical.height;
                if right {
                    layers.push(Layer {
                        image: &frame.image,
                        source: PixelRect {
                            x: size.width - 1,
                            y: 0,
                            width: 1,
                            height: size.height,
                        },
                        destination: PixelRect {
                            x: logical.width,
                            y: destination.y,
                            width: 1,
                            height: destination.height,
                        },
                    });
                }
                if bottom {
                    layers.push(Layer {
                        image: &frame.image,
                        source: PixelRect {
                            x: 0,
                            y: size.height - 1,
                            width: size.width,
                            height: 1,
                        },
                        destination: PixelRect {
                            x: destination.x,
                            y: logical.height,
                            width: destination.width,
                            height: 1,
                        },
                    });
                }
                if right && bottom {
                    layers.push(Layer {
                        image: &frame.image,
                        source: PixelRect {
                            x: size.width - 1,
                            y: size.height - 1,
                            width: 1,
                            height: 1,
                        },
                        destination: PixelRect {
                            x: logical.width,
                            y: logical.height,
                            width: 1,
                            height: 1,
                        },
                    });
                }
            }
            match self
                .compositor
                .as_mut()
                .unwrap()
                .compose_with_cached_background_overlay(
                    &layers,
                    webcam.as_ref(),
                    &overlays,
                    &highlight,
                    true,
                ) {
                Ok(image) => image,
                Err(MacError::Timeout) => return Ok(NativeRecordingEvent::Idle),
                Err(error) => return Err(native_error(error)),
            }
        };
        if self.config.capture.cancellation.is_canceled() {
            return Err(native_error(MacError::Canceled));
        }
        if self.native {
            self.encoder.push_native_frame_at_pts(pts, image)?;
        } else {
            if self.config.capture.dynamic_range == DynamicRange::Hdr {
                let cpu = image
                    .to_cpu()
                    .map_err(|e| ScreenRecorderError::Encode(e.to_string()))?;
                self.encoder.push_cpu_hdr_frame_at_pts(pts, &cpu)?;
            } else {
                self.cpu_pixels.resize(
                    self.config.output.width as usize * self.config.output.height as usize * 4,
                    0,
                );
                image
                    .copy_packed(&mut self.cpu_pixels, image.format() == PixelFormat::Bgra8)
                    .map_err(|error| ScreenRecorderError::Encode(error.to_string()))?;
                self.cpu_pixels = self
                    .encoder
                    .push_raster_rgba_frame_at_pts(pts, std::mem::take(&mut self.cpu_pixels))?;
            }
            self.cpu_readbacks += 1;
        }
        self.last_composition = Some(composition);
        self.accepted_transform = Some(frame.transform);
        Ok(NativeRecordingEvent::Frame { pts })
    }
    fn prepare_finish(&mut self) -> Result<u64> {
        if self.config.capture.cancellation.is_canceled() {
            return Err(native_error(MacError::Canceled));
        }
        let stop = Instant::now();
        self.clock.controller().mark_pause(stop);
        self.capture.release_capture_access();
        self.video_schedule.clear();
        if let Some(audio) = self.audio.take() {
            for event in audio.stop_and_drain() {
                if let AudioEvent::Error(error) = event {
                    return Err(ScreenRecorderError::Audio(error));
                }
                process_audio_event(event, &self.clock, self.paused, self.mixer.as_mut());
            }
        }
        self.drain_audio(true)?;
        let duration_ms = u64::try_from(
            self.clock
                .active_elapsed_duration(stop)
                .as_nanos()
                .div_ceil(1_000_000)
                .max(1),
        )
        .map_err(|_| ScreenRecorderError::InvalidConfig("recording endpoint overflow".into()))?;
        self.media
            .validate()
            .map_err(ScreenRecorderError::InvalidConfig)?;
        Ok(duration_ms)
    }
    pub fn finish(mut self) -> Result<NativeRecordingReport> {
        self._webcam_capture.stop();
        let end = self
            .prepare_finish()
            .map_err(|error| self.preserve_failure(error))?;
        let encoder = finish_recording_output(
            self.encoder,
            end,
            self.config.execution,
            &self.config.capture.cancellation,
        )?;
        Ok(NativeRecordingReport {
            encoder,
            media: self.media,
            geometry_changes: self.geometry_changes,
            interruptions: self.interruptions,
            cpu_readbacks: self.cpu_readbacks,
        })
    }
    pub(super) fn source_clock(&self) -> RecordingClock {
        self.clock.clone()
    }
    pub(super) fn set_stop_boundary(
        &mut self,
        boundary: std::sync::Arc<std::sync::Mutex<Option<Instant>>>,
    ) {
        self.stop_boundary = Some(boundary);
    }
    pub(super) fn source_transform(&self) -> DesktopTransform {
        self.accepted_transform
            .unwrap_or_else(|| self.capture.transform())
    }
    pub(super) fn freeze_source(&mut self, at: Instant) {
        self.clock.controller().mark_pause(at);
        self.capture.release_capture_access();
        self.video_schedule.clear();
    }
    pub(super) fn finish_source(mut self, duration_ms: u64) -> Result<NativeRecordingReport> {
        self._webcam_capture.stop();
        self.capture.release_capture_access();
        self.video_schedule.clear();
        let encoder = self
            .encoder
            .finish_at_duration_ms_cancelable(duration_ms, &self.config.capture.cancellation)?;
        Ok(NativeRecordingReport {
            encoder,
            media: self.media,
            geometry_changes: self.geometry_changes,
            interruptions: self.interruptions,
            cpu_readbacks: self.cpu_readbacks,
        })
    }
}

// The encoder owns the output path. Metadata persistence belongs to the caller
// (or the editable bundle layer), never to media finalization.
fn finish_recording_output(
    encoder: StreamingEncoder,
    duration_ms: u64,
    execution: ExportExecutionMode,
    cancellation: &CancellationToken,
) -> Result<StreamingEncoderReport> {
    let mut report = encoder.finish_at_duration_ms_cancelable(duration_ms, cancellation)?;
    report.hardware_fallback |=
        execution == ExportExecutionMode::HardwarePreferred && !report.used_hardware_video_encoder;
    Ok(report)
}

fn encoder_accepts_source(
    image: PixelSize,
    image_format: PixelFormat,
    output: PixelSize,
    output_format: PixelFormat,
    destination: PixelRect,
    no_overlays: bool,
) -> bool {
    no_overlays
        && image_format == output_format
        && image == output
        && destination
            == PixelRect {
                x: 0,
                y: 0,
                width: output.width,
                height: output.height,
            }
}

fn audio_discontinuity(
    event: &AudioEvent,
    clock: &RecordingClock,
    ends: &mut [Option<Instant>; 2],
) -> Result<Option<snow_recording_model::media::TimelineDiscontinuity>> {
    use snow_media::time::{ClockDomain, MediaTime};
    use snow_recording_model::media::{DiscontinuityReason, TimelineDiscontinuity};
    let (at, duration, reason) = match event {
        AudioEvent::Packet(packet) => {
            let index =
                usize::from(packet.source == snow_audio_recorder::AudioSourceKind::Microphone);
            let previous = ends[index];
            ends[index] = packet.end_capture_time();
            if !packet.metadata.discontinuity {
                return Ok(None);
            }
            let start = packet.start_capture_time().unwrap_or_else(Instant::now);
            let gap = previous.and_then(|end| {
                clock
                    .active_elapsed_duration(start)
                    .checked_sub(clock.active_elapsed_duration(end))
            });
            (
                previous.unwrap_or(start),
                gap,
                DiscontinuityReason::AudioOverflow,
            )
        }
        AudioEvent::PacketDropped { dropped_frames, .. } => (
            Instant::now(),
            Some(Duration::from_secs_f64(*dropped_frames as f64 / 48_000.0)),
            DiscontinuityReason::AudioOverflow,
        ),
        AudioEvent::SourceRestarted { downtime, .. } => (
            Instant::now(),
            Some(*downtime),
            DiscontinuityReason::SourceInterrupted,
        ),
        _ => return Ok(None),
    };
    let time = |duration: Duration| -> Result<MediaTime> {
        Ok(MediaTime {
            value: i64::try_from(duration.as_nanos()).map_err(|_| {
                ScreenRecorderError::InvalidConfig("audio discontinuity timestamp overflow".into())
            })?,
            timescale: 1_000_000_000,
            domain: ClockDomain::Session,
            epoch: 0,
        })
    };
    Ok(Some(TimelineDiscontinuity {
        timestamp: time(clock.active_elapsed_duration(at))?,
        duration: duration.map(time).transpose()?,
        reason,
    }))
}

mod direct;
pub use direct::{DirectSession, recording_dimensions};

fn overlay_cursor_mode(cursor: snow_media::CursorMode, overlay: bool) -> snow_media::CursorMode {
    if overlay && cursor == snow_media::CursorMode::Embedded {
        snow_media::CursorMode::Separate
    } else {
        cursor
    }
}

mod cursor_rasterizer;
pub(crate) mod deferred;
#[cfg(feature = "legacy-capture")]
mod editable;
#[cfg(feature = "legacy-capture")]
mod editable_cursor;
#[cfg(feature = "legacy-capture")]
pub use editable::{NativeEditableReport, NativeEditableSession};

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn camera_and_highlight_preserve_hidden_cursors_and_draw_visible_cursors_above_them() {
        use snow_media::CursorMode::{Embedded, Hidden, Separate};
        assert_eq!(overlay_cursor_mode(Embedded, true), Separate);
        assert_eq!(overlay_cursor_mode(Embedded, false), Embedded);
        for cursor in [Hidden, Separate] {
            assert_eq!(overlay_cursor_mode(cursor, true), cursor);
            assert_eq!(overlay_cursor_mode(cursor, false), cursor);
        }
    }

    #[test]
    fn direct_outputs_do_not_create_or_touch_sidecars() {
        for format in [
            ExportFormat::Gif,
            ExportFormat::Apng,
            ExportFormat::Webp,
            ExportFormat::Mp4,
            ExportFormat::Avi,
        ] {
            for sidecar_kind in ["absent", "file", "directory", "symlink"] {
                let directory = tempfile::tempdir().unwrap();
                let output_path = directory
                    .path()
                    .join(format!("SnowShot.{}", format.file_extension()));
                let sidecar =
                    output_path.with_extension(format!("{}.snowmedia", format.file_extension()));
                let sentinel = b"caller-owned metadata";
                let symlink_target = directory.path().join("caller-metadata");
                match sidecar_kind {
                    "file" => std::fs::write(&sidecar, sentinel).unwrap(),
                    "directory" => std::fs::create_dir(&sidecar).unwrap(),
                    "symlink" => {
                        std::fs::write(&symlink_target, sentinel).unwrap();
                        std::os::unix::fs::symlink(&symlink_target, &sidecar).unwrap();
                    }
                    _ => {}
                }
                let mut encoder = StreamingEncoder::builder(StreamingEncoderConfig {
                    output_path: output_path.clone(),
                    format,
                    width: 16,
                    height: 16,
                    fps: 10,
                    codec: VideoCodec::H264,
                    prefer_hardware_h264: false,
                    execution_mode: ExportExecutionMode::SoftwareOnly,
                    software_h264_priority: SoftwareH264Priority::X264First,
                    video: Default::default(),
                    encode_threads: 1,
                    audio: Vec::new(),
                    loop_animated_images: false,
                })
                .software_only()
                .create()
                .unwrap();
                encoder
                    .push_rgba_frame_at_pts(0, &[255; 16 * 16 * 4])
                    .unwrap();
                encoder
                    .push_rgba_frame_at_pts(1, &[128; 16 * 16 * 4])
                    .unwrap();
                let report = finish_recording_output(
                    encoder,
                    200,
                    ExportExecutionMode::SoftwareOnly,
                    &CancellationToken::default(),
                )
                .unwrap();
                assert!(report.encoded_frames > 0);
                assert!(std::fs::metadata(&output_path).unwrap().len() > 0);
                match sidecar_kind {
                    "file" | "symlink" => {
                        assert_eq!(std::fs::read(&sidecar).unwrap(), sentinel);
                        if sidecar_kind == "symlink" {
                            assert_eq!(std::fs::read_link(&sidecar).unwrap(), symlink_target);
                        }
                    }
                    "directory" => {
                        assert!(sidecar.is_dir());
                        assert_eq!(std::fs::read_dir(&sidecar).unwrap().count(), 0);
                    }
                    _ => assert!(!sidecar.exists(), "{format:?}"),
                }
                let expected_count = match sidecar_kind {
                    "absent" => 1,
                    "symlink" => 3,
                    _ => 2,
                };
                assert_eq!(
                    std::fs::read_dir(directory.path()).unwrap().count(),
                    expected_count,
                    "{format:?}: {sidecar_kind}"
                );
            }
        }
    }

    #[test]
    fn audio_gap_metadata_excludes_paused_time() {
        let start = Instant::now();
        let clock = RecordingClock::new(start);
        clock
            .controller()
            .mark_pause(start + Duration::from_millis(100));
        clock
            .controller()
            .mark_resume(start + Duration::from_millis(200));
        let mut ends = [Some(start + Duration::from_millis(80)), None];
        let event = AudioEvent::Packet(snow_audio_recorder::AudioPacket {
            source: snow_audio_recorder::AudioSourceKind::System,
            format: snow_audio_recorder::AudioFormat::new(48_000, 2),
            frames: 480,
            data: vec![0; 960],
            metadata: snow_audio_recorder::AudioPacketMetadata {
                discontinuity: true,
                stream_timestamp: Some(snow_core::timestamp::StreamTimestamp {
                    instant: start + Duration::from_millis(240),
                    raw_os_ticks: None,
                    tick_format: snow_core::timestamp::TickFormat::Rational {
                        timescale: 1_000_000_000,
                        domain: snow_media::time::ClockDomain::MacHostTime,
                        epoch: 0,
                    },
                }),
                ..Default::default()
            },
        });
        let gap = audio_discontinuity(&event, &clock, &mut ends)
            .unwrap()
            .unwrap();
        assert_eq!(gap.timestamp.value, 80_000_000);
        assert_eq!(gap.duration.unwrap().value, 50_000_000);
        assert!(matches!(
            gap.reason,
            snow_recording_model::media::DiscontinuityReason::AudioOverflow
        ));
        assert_eq!(ends[0], Some(start + Duration::from_millis(240)));
    }
    #[test]
    fn fullscreen_sdr_without_overlays_skips_a_second_composite() {
        let size = PixelSize::new(1920, 1080).unwrap();
        let full = PixelRect {
            x: 0,
            y: 0,
            width: 1920,
            height: 1080,
        };
        assert!(encoder_accepts_source(
            size,
            PixelFormat::Bgra8,
            size,
            PixelFormat::Bgra8,
            full,
            true
        ));
        assert!(!encoder_accepts_source(
            size,
            PixelFormat::Rgba16Float,
            size,
            PixelFormat::P010,
            full,
            true
        ));
        assert!(!encoder_accepts_source(
            size,
            PixelFormat::Bgra8,
            size,
            PixelFormat::Bgra8,
            full,
            false
        ));
        assert!(!encoder_accepts_source(
            size,
            PixelFormat::Bgra8,
            PixelSize::new(1280, 720).unwrap(),
            PixelFormat::Bgra8,
            PixelRect {
                x: 0,
                y: 0,
                width: 1280,
                height: 720
            },
            true
        ));
    }
}

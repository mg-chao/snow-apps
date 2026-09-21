//! Native macOS recording orchestration. Drive `step` on one worker thread;
//! the host must keep its main run loop active. Video never maps to CPU in
//! hardware mode. FFmpeg owns muxing, audio encoding and finalization.
use crate::direct::{LiveAudioMixer, process_audio_event};
use crate::macos_effects::Effects;
pub use crate::macos_effects::NativeEffectsConfig;
use crate::{ScreenRecorderError, error::Result};
use snow_audio_recorder::{AudioEvent, AudioSession, AudioStreamConfig, AudioStreamHandle};
use snow_core::recording_clock::RecordingClock;
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
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingAudioConfig,
    StreamingEncoder, StreamingEncoderConfig, StreamingEncoderReport, VideoCodec,
};
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

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
    pub effects: NativeEffectsConfig,
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
    pub media: snow_recording_model::media::RecordedMedia,
    pub manifest_path: PathBuf,
    pub geometry_changes: Vec<(u64, DesktopTransform)>,
    pub interruptions: Vec<(u64, String)>,
    pub cpu_readbacks: u64,
}
pub struct NativeRecordingSession {
    config: NativeRecordingConfig,
    effects: Option<Effects>,
    capture: DesktopSession,
    compositor: Compositor,
    encoder: StreamingEncoder,
    audio: Option<AudioStreamHandle>,
    mixer: Option<LiveAudioMixer>,
    clock: RecordingClock,
    paused: bool,
    native: bool,
    interrupted: bool,
    media: snow_recording_model::media::RecordedMedia,
    last_pts: Option<u64>,
    geometry_changes: Vec<(u64, DesktopTransform)>,
    interruptions: Vec<(u64, String)>,
    cpu_readbacks: u64,
    audio_ends: [Option<Instant>; 2],
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
    pub fn start(mut config: NativeRecordingConfig) -> Result<Self> {
        // Composite the tint against clean content and draw the pointer above it.
        if config.effects.highlight_rgba[3] != 0
            && config.capture.cursor == snow_media::CursorMode::Embedded
        {
            config.capture.cursor = snow_media::CursorMode::Separate;
        }
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
        if hdr && (config.codec != VideoCodec::H265 || config.format != ExportFormat::Mp4) {
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
        let capture = DesktopSession::new(config.capture.clone()).map_err(native_error)?;
        let format = if hdr {
            PixelFormat::P010
        } else {
            PixelFormat::Bgra8
        };
        let compositor = Compositor::new(config.output, format, 4).map_err(native_error)?;
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
            audio: config.audio.as_ref().map(|_| StreamingAudioConfig {
                sample_rate_hz: 48_000,
                channels: 2,
                bitrate_kbps: 192,
            }),
        };
        let (mut encoder, native) = if config.execution == ExportExecutionMode::SoftwareOnly
            || config.format.is_animated_image()
        {
            let mut builder = StreamingEncoder::builder(encode_config).software_only();
            if hdr {
                builder = builder.hdr10_cpu_input();
            }
            (builder.create()?, false)
        } else {
            match StreamingEncoder::builder(encode_config.clone())
                .native_input(format)
                .create()
            {
                Ok(encoder) => (encoder, true),
                Err(_) if config.execution == ExportExecutionMode::HardwarePreferred => (
                    StreamingEncoder::builder(encode_config)
                        .native_input(format)
                        .software_only()
                        .create()?,
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
                AudioSession::new()?.start_streaming(audio)
            })
            .transpose()?;
        let mixer = config
            .audio
            .as_ref()
            .map(|a| LiveAudioMixer::new(a.system.enabled, a.microphone.enabled));
        let effects = Effects::new(config.effects.clone(), config.output, config.capture.cursor)?;
        // Startup (including audio-device initialization) is outside the recording timeline.
        let clock = RecordingClock::new(Instant::now());
        Ok(Self {
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
            config,
            capture,
            compositor,
            encoder,
            audio,
            mixer,
            clock,
            paused: false,
            native,
            interrupted: false,
            last_pts: None,
            geometry_changes: Vec::new(),
            interruptions: Vec::new(),
            cpu_readbacks: 0,
            audio_ends: [None; 2],
        })
    }
    pub(crate) fn preserve_failure(&mut self, error: ScreenRecorderError) -> ScreenRecorderError {
        if self.config.capture.cancellation.is_canceled() {
            return error;
        }
        self.encoder
            .preserve_staging_failure(&error.to_string())
            .map(ScreenRecorderError::from)
            .unwrap_or(error)
    }
    pub fn pause(&mut self) {
        if !self.paused {
            self.clock.controller().mark_pause(Instant::now());
            self.paused = true;
            if let Some(audio) = &self.audio {
                audio.pause();
            }
            if let Some(mixer) = &mut self.mixer {
                mixer.reset_alignment(None);
            }
            if let Some(effects) = &mut self.effects {
                effects.reset(self.clock.active_elapsed_ms(Instant::now()));
            }
            self.capture.release_capture_access();
        }
    }
    pub fn resume(&mut self) {
        if self.paused {
            self.clock.controller().mark_resume(Instant::now());
            self.paused = false;
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
            return Ok(NativeRecordingEvent::Idle);
        }
        match self
            .capture
            .next_event(timeout.min(Duration::from_millis(100)))
        {
            Ok(DesktopEvent::Configuration {
                transform,
                generation,
            }) => {
                self.geometry_changes.push((generation, transform));
                self.media
                    .geometry
                    .push(snow_recording_model::media::GeometryChange {
                        timestamp_ms: self.clock.active_elapsed_ms(Instant::now()),
                        generation,
                        transform,
                        destination: aspect_fit(transform.output, self.config.output)
                            .map_err(|e| ScreenRecorderError::InvalidConfig(e.to_string()))?,
                    });
                Ok(NativeRecordingEvent::Configuration {
                    transform,
                    generation,
                })
            }
            Ok(DesktopEvent::Frame(frame)) => self.submit(frame),
            Err(MacError::Timeout) => Ok(NativeRecordingEvent::Idle),
            Err(MacError::Inactive) => {
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
                Ok(NativeRecordingEvent::Interruption { at_ms, reason })
            }
            Err(error) => Err(native_error(error)),
        }
    }
    fn submit(&mut self, frame: DesktopFrame) -> Result<NativeRecordingEvent> {
        self.interrupted = false;
        if !self.clock.is_active_at(frame.acquired_at)
            || (frame.duplicate && self.last_pts.is_some() && self.effects.is_none())
        {
            return Ok(NativeRecordingEvent::Idle);
        }
        let frame_time = if self.effects.is_some() && frame.duplicate {
            Instant::now()
        } else {
            frame.acquired_at
        };
        let pts = u64::try_from(
            self.clock.active_elapsed_duration(frame_time).as_nanos() * u128::from(self.config.fps)
                / 1_000_000_000,
        )
        .map_err(|_| ScreenRecorderError::InvalidConfig("recording timestamp overflow".into()))?;
        if self.last_pts.is_some_and(|last| pts <= last) {
            return Ok(NativeRecordingEvent::Idle);
        }
        let size = frame.image.size();
        let destination = aspect_fit(size, self.config.output)
            .map_err(|e| ScreenRecorderError::InvalidConfig(e.to_string()))?;
        let (tiles, interruption) = if let Some(effects) = &mut self.effects {
            effects.draw(
                &self.clock,
                frame.transform,
                destination,
                self.clock.active_elapsed_ms(frame_time),
            )?
        } else {
            (Vec::new(), None)
        };
        if let Some(reason) = interruption {
            self.interruptions
                .push((self.clock.active_elapsed_ms(frame.acquired_at), reason));
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
        let image = match self.compositor.compose_with_highlight(
            &[Layer {
                image: &frame.image,
                source: PixelRect {
                    x: 0,
                    y: 0,
                    width: size.width,
                    height: size.height,
                },
                destination,
            }],
            &overlays,
            &highlight,
            true,
        ) {
            Ok(image) => image,
            Err(MacError::Timeout) => return Ok(NativeRecordingEvent::Idle),
            Err(error) => return Err(native_error(error)),
        };
        if self.config.capture.cancellation.is_canceled() {
            return Err(native_error(MacError::Canceled));
        }
        if self.native {
            self.encoder.push_native_frame_at_pts(pts, image)?;
        } else {
            let cpu = image
                .to_cpu()
                .map_err(|e| ScreenRecorderError::Encode(e.to_string()))?;
            if self.config.capture.dynamic_range == DynamicRange::Hdr {
                self.encoder.push_cpu_hdr_frame_at_pts(pts, &cpu)?;
            } else {
                let rgba = cpu
                    .into_format(PixelFormat::Rgba8)
                    .map_err(|error| ScreenRecorderError::Encode(error.to_string()))?;
                self.encoder.push_rgba_frame_at_pts(pts, &rgba.bytes)?;
            }
            self.cpu_readbacks += 1;
        }
        self.last_pts = Some(pts);
        Ok(NativeRecordingEvent::Frame { pts })
    }
    fn prepare_finish(&mut self) -> Result<u64> {
        if self.config.capture.cancellation.is_canceled() {
            return Err(native_error(MacError::Canceled));
        }
        let stop = Instant::now();
        self.clock.controller().mark_pause(stop);
        self.capture.release_capture_access();
        if let Some(audio) = self.audio.take() {
            for event in audio.stop_and_drain() {
                if let AudioEvent::Error(error) = event {
                    return Err(ScreenRecorderError::Audio(error));
                }
                process_audio_event(event, &self.clock, self.paused, self.mixer.as_mut());
            }
        }
        self.drain_audio(true)?;
        let end = u64::try_from(
            self.clock.active_elapsed_duration(stop).as_nanos() * u128::from(self.config.fps)
                / 1_000_000_000,
        )
        .map_err(|_| ScreenRecorderError::InvalidConfig("recording endpoint overflow".into()))?;
        let end = end.max(self.last_pts.unwrap_or(0).saturating_add(1));
        self.media
            .validate()
            .map_err(ScreenRecorderError::InvalidConfig)?;
        Ok(end)
    }
    pub fn finish(mut self) -> Result<NativeRecordingReport> {
        let end = self
            .prepare_finish()
            .map_err(|error| self.preserve_failure(error))?;
        let mut encoder = self
            .encoder
            .finish_at_pts_cancelable(end, &self.config.capture.cancellation)?;
        encoder.hardware_fallback |= self.config.execution
            == ExportExecutionMode::HardwarePreferred
            && !encoder.used_hardware_video_encoder;
        let mut manifest_path = self.config.output_path.as_os_str().to_os_string();
        manifest_path.push(".snowmedia");
        let manifest_path = PathBuf::from(manifest_path);
        self.media.write_to(&manifest_path)?;
        Ok(NativeRecordingReport {
            manifest_path,
            media: self.media,
            encoder,
            geometry_changes: self.geometry_changes,
            interruptions: self.interruptions,
            cpu_readbacks: self.cpu_readbacks,
        })
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

mod editable;
mod editable_cursor;
pub use editable::{NativeEditableReport, NativeEditableSession};

#[cfg(test)]
mod tests {
    use super::*;
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
}

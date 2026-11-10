//! Shared capture/encoder negotiation. A successful probe must emit real video packets.
use snow_recording_export::{StreamingEncoder, StreamingEncoderReport, VideoStartupReport};
use std::path::Path;

use crate::error::{Result, ScreenRecorderError};

#[derive(Default)]
pub(crate) struct Negotiation {
    pub adapter: Option<String>,
    pub encoder_attempts: Vec<String>,
    pub stage: Option<String>,
    reason: Option<String>,
}

impl Negotiation {
    #[cfg(windows)]
    pub fn fallback_reason(&self) -> Option<String> {
        self.reason.clone()
    }
    /// Terminal capture, permission and cancellation errors must never become fallbacks.
    pub fn fallback<T>(&mut self, result: Result<Option<T>>) -> Result<Option<T>> {
        match result {
            Err(ScreenRecorderError::Capture(error)) if !error.allows_backend_fallback() => {
                Err(error.into())
            }
            Err(
                error @ (ScreenRecorderError::PermissionDenied(_)
                | ScreenRecorderError::ExportCanceled),
            ) => Err(error),
            Err(error) => {
                self.reason = Some(error.to_string());
                Ok(None)
            }
            result => result,
        }
    }

    pub fn apply(self, report: &mut StreamingEncoderReport) {
        let mut attempts = self.encoder_attempts;
        if attempts.last() != Some(&report.video_encoder) {
            attempts.push(report.video_encoder.clone());
        }
        report.hardware_fallback |= self.reason.is_some();
        report.startup = VideoStartupReport {
            adapter: self.adapter,
            encoder_attempts: attempts,
            fallback_stage: self.reason.as_ref().and(self.stage),
            fallback_reason: self.reason,
        };
    }
}

fn probe_error(cause: impl std::fmt::Display) -> ScreenRecorderError {
    ScreenRecorderError::Encode(format!("GPU startup probe: {cause}"))
}

fn finish_probe(
    encoder: StreamingEncoder,
    path: &Path,
    diagnostics: &mut Negotiation,
) -> Result<()> {
    diagnostics.stage = Some("packet_probe".into());
    encoder.finish_at_pts(4)?;
    let mut media = ffmpeg_next::format::input(path).map_err(probe_error)?;
    if !media.packets().any(|(stream, packet)| {
        stream.parameters().medium() == ffmpeg_next::media::Type::Video && packet.size() != 0
    }) {
        return Err(probe_error(
            "hardware encoder startup probe produced no video packet",
        ));
    }
    diagnostics.stage = None;
    Ok(())
}

#[cfg(windows)]
pub(crate) enum ProbePurpose {
    LiveOutput,
    RecordingSource,
}

#[cfg(windows)]
pub(crate) fn probe_encoder(
    mut settings: snow_recording_export::StreamingEncoderConfig,
    purpose: ProbePurpose,
    device: snow_d3d11::SharedDevice,
    diagnostics: &mut Negotiation,
    mut compose: impl FnMut(u64, &snow_recording_export::gpu::GpuEncoderFrame) -> Result<()>,
) -> Result<()> {
    use snow_recording_export::gpu::{GpuInputConfig, encoder_name};

    diagnostics.stage = Some("encoder_startup".into());
    if let Some(name) = encoder_name(device.identity().vendor, settings.codec) {
        diagnostics.encoder_attempts.push(name.into());
    }
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("probe.mp4");
    settings.output_path = path.clone();
    settings.audio.clear();
    let builder = StreamingEncoder::builder(settings).gpu_input(GpuInputConfig { device });
    let mut encoder = match purpose {
        ProbePurpose::LiveOutput => builder,
        ProbePurpose::RecordingSource => builder.recording_source(),
    }
    .create()?;
    for pts in 0..4 {
        diagnostics.stage = Some("surface_allocation".into());
        let surface = encoder
            .allocate_gpu_frame()?
            .ok_or_else(|| probe_error("startup encoder exhausted its surface pool"))?;
        diagnostics.stage = Some("composition_probe".into());
        compose(pts, &surface)?;
        diagnostics.stage = Some("submission_probe".into());
        encoder.push_gpu_frame_at_pts(pts, surface)?;
    }
    finish_probe(encoder, &path, diagnostics)
}

#[cfg(windows)]
pub(crate) fn prepare_capture(
    config: &crate::DirectRecordingConfig,
    include_cursor: bool,
    diagnostics: &mut Negotiation,
) -> snow_capture::error::CaptureResult<
    Option<(
        snow_capture::gpu::GpuCaptureStream,
        snow_capture::gpu::GpuCapturedFrame,
    )>,
> {
    use snow_capture::error::CaptureError;
    use snow_capture::gpu::{GpuCaptureEvent, GpuCaptureStream};
    use std::time::{Duration, Instant};
    crate::capture_policy::try_recording_gpu_capture(config.capture_backend, |backend| {
        diagnostics.stage = Some("capture_startup".into());
        let spawn = if config.follow_mouse {
            GpuCaptureStream::spawn_follow_mouse
        } else {
            GpuCaptureStream::spawn
        };
        let stream = spawn(
            snow_capture::CaptureRegion {
                x: config.region.x,
                y: config.region.y,
                width: config.region.width,
                height: config.region.height,
            },
            backend,
            crate::direct::capture::stream_config(config, include_cursor),
        )?;
        diagnostics.adapter = Some(stream.device().identity().description.clone());
        diagnostics.stage = Some("capture_probe".into());
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            match stream.recv_timeout(deadline.saturating_duration_since(Instant::now())) {
                Ok(GpuCaptureEvent::Frame(frame)) => return Ok((stream, frame)),
                Ok(GpuCaptureEvent::Error(error)) => return Err(error),
                Ok(GpuCaptureEvent::StreamEnded) => return Err(CaptureError::WorkerDead),
                Err(_) => return Err(CaptureError::Timeout),
                _ => {}
            }
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_recording_export::{
        ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoderConfig, VideoCodec,
    };

    fn settings(path: std::path::PathBuf) -> StreamingEncoderConfig {
        StreamingEncoderConfig {
            output_path: path,
            loop_animated_images: true,
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
        }
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "requires an offscreen D3D11 VideoProcessor and a native H.264 encoder"]
    fn hardware_shared_probe_covers_direct_and_recording_source() -> anyhow::Result<()> {
        use snow_d3d11::{Rect, SharedDevice, VideoLayer, VideoProcessor};
        use windows::Win32::Graphics::Direct3D11::*;
        use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_R8G8B8A8_UNORM;
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;

        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1()? };
        let adapter = unsafe { factory.EnumAdapters1(0)? }.cast()?;
        let device = SharedDevice::create(&adapter)?;
        // The software fixture's 16x16 frames are below AMF's supported encode size.
        let size = (128, 128);
        let texture = device.texture(
            size.0,
            size.1,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET).0 as u32,
        )?;
        let processor = VideoProcessor::new(device.clone(), size, size, 30)?;
        let directory = tempfile::tempdir()?;
        for purpose in [ProbePurpose::LiveOutput, ProbePurpose::RecordingSource] {
            let mut config = settings(directory.path().join("unused.mp4"));
            config.width = size.0;
            config.height = size.1;
            config.execution_mode = ExportExecutionMode::HardwarePreferred;
            config.prefer_hardware_h264 = true;
            let mut diagnostics = Negotiation::default();
            probe_encoder(
                config,
                purpose,
                device.clone(),
                &mut diagnostics,
                |pts, output| {
                    let _lock = device.lock();
                    let pixels = vec![pts as u8 * 50; (size.0 * size.1 * 4) as usize];
                    unsafe {
                        device.context().UpdateSubresource(
                            texture.raw(),
                            0,
                            None,
                            pixels.as_ptr().cast(),
                            size.0 * 4,
                            0,
                        )
                    };
                    processor
                        .blit(
                            &[VideoLayer {
                                texture: texture.clone(),
                                source: Rect::full(size),
                                destination: Rect::full(size),
                                rotation: 0,
                                alpha: false,
                            }],
                            &output.texture().map_err(probe_error)?,
                            output.slice(),
                        )
                        .map_err(probe_error)
                },
            )?;
            assert!(diagnostics.stage.is_none());
            assert_eq!(diagnostics.encoder_attempts.len(), 1);
        }
        // Probe media never publishes to the caller's recording destination.
        assert_eq!(std::fs::read_dir(directory.path())?.count(), 0);
        Ok(())
    }

    #[test]
    fn startup_fallback_preserves_stage_adapter_and_attempts_in_capture_report() {
        let mut diagnostics = Negotiation {
            adapter: Some("test adapter".into()),
            encoder_attempts: vec!["hardware".into()],
            stage: Some("composition_probe".into()),
            ..Default::default()
        };
        assert!(
            diagnostics
                .fallback::<()>(Err(probe_error("injected composition failure")))
                .unwrap()
                .is_none()
        );
        let mut report = StreamingEncoderReport {
            video_encoder: "libx264".into(),
            ..Default::default()
        };
        diagnostics.apply(&mut report);
        assert!(report.hardware_fallback);
        assert_eq!(report.startup.adapter.as_deref(), Some("test adapter"));
        assert_eq!(report.startup.encoder_attempts, ["hardware", "libx264"]);
        assert_eq!(
            report.startup.fallback_stage.as_deref(),
            Some("composition_probe")
        );
        assert!(
            report
                .startup
                .fallback_reason
                .unwrap()
                .contains("injected composition failure")
        );
        assert_eq!(report.recovery_count, 0);
        assert!(report.recovery_reason.is_none());
    }

    #[test]
    fn terminal_errors_never_trigger_startup_fallback() {
        for cause in [
            ScreenRecorderError::ExportCanceled,
            ScreenRecorderError::PermissionDenied(crate::MediaPermission::Screen),
            snow_capture::error::CaptureError::Canceled.into(),
            snow_capture::error::CaptureError::InvalidTarget("invalid region".into()).into(),
        ] {
            let mut diagnostics = Negotiation::default();
            assert!(diagnostics.fallback::<()>(Err(cause)).is_err());
            assert!(diagnostics.reason.is_none());
        }
    }

    #[test]
    fn packet_probe_requires_encoded_media_and_clears_stage_only_after_success() {
        let directory = tempfile::tempdir().unwrap();
        for frames in [0, 4] {
            let path = directory.path().join(format!("probe-{frames}.mp4"));
            let mut encoder = StreamingEncoder::builder(settings(path.clone()))
                .software_only()
                .create()
                .unwrap();
            for pts in 0..frames {
                encoder
                    .push_owned_rgba_frame_at_pts(pts, vec![pts as u8 * 50; 16 * 16 * 4])
                    .unwrap();
            }
            let mut diagnostics = Negotiation::default();
            let result = finish_probe(encoder, &path, &mut diagnostics);
            assert_eq!(result.is_ok(), frames > 0);
            assert_eq!(
                diagnostics.stage.as_deref(),
                (frames == 0).then_some("packet_probe")
            );
        }
    }
}

//! A recording observation never implicitly downloads a GPU surface.
use super::*;

pub(super) enum DirectFrame {
    Cpu(CapturedFrame),
    #[cfg(windows)]
    Gpu(snow_capture::gpu::GpuCapturedFrame),
}

impl From<CapturedFrame> for DirectFrame {
    fn from(frame: CapturedFrame) -> Self {
        Self::Cpu(frame)
    }
}

impl DirectFrame {
    #[cfg(test)]
    pub fn expect_cpu(self) -> CapturedFrame {
        match self {
            Self::Cpu(frame) => frame,
            #[cfg(windows)]
            Self::Gpu(_) => panic!("expected CPU fixture"),
        }
    }
    pub fn metadata(&self) -> &snow_capture::FrameMetadata {
        match self {
            Self::Cpu(frame) => frame.metadata(),
            #[cfg(windows)]
            Self::Gpu(frame) => frame.metadata(),
        }
    }
    #[cfg(any(test, feature = "bench-synthetic-input"))]
    pub fn dimensions(&self) -> (u32, u32) {
        match self {
            Self::Cpu(frame) => frame.dimensions(),
            #[cfg(windows)]
            Self::Gpu(frame) => frame.dimensions(),
        }
    }
    pub fn instant(&self) -> Instant {
        match self {
            Self::Cpu(frame) => frame_instant(frame),
            #[cfg(windows)]
            Self::Gpu(frame) => frame
                .metadata()
                .stream_timestamp()
                .map(|stamp| stamp.instant)
                .unwrap_or_else(Instant::now),
        }
    }
}

pub(super) enum DirectCaptureEvent {
    Frame(DirectFrame),
    FramesDropped { count: u32 },
    Error(snow_capture::error::CaptureError),
    StreamEnded,
    Control,
}

impl From<CaptureEvent> for DirectCaptureEvent {
    fn from(event: CaptureEvent) -> Self {
        match event {
            CaptureEvent::Frame(frame) => Self::Frame(frame.into()),
            CaptureEvent::FramesDropped { count, .. } => Self::FramesDropped { count },
            CaptureEvent::Error(error) => Self::Error(error),
            CaptureEvent::StreamEnded => Self::StreamEnded,
            _ => Self::Control,
        }
    }
}

#[cfg(windows)]
impl From<snow_capture::gpu::GpuCaptureEvent> for DirectCaptureEvent {
    fn from(event: snow_capture::gpu::GpuCaptureEvent) -> Self {
        use snow_capture::gpu::GpuCaptureEvent;
        match event {
            GpuCaptureEvent::Frame(frame) => Self::Frame(DirectFrame::Gpu(frame)),
            GpuCaptureEvent::Error(error) => Self::Error(error),
            GpuCaptureEvent::StreamEnded => Self::StreamEnded,
            _ => Self::Control,
        }
    }
}

pub(super) enum DirectCapture {
    Cpu(CaptureStream),
    #[cfg(windows)]
    Gpu(snow_capture::gpu::GpuCaptureStream),
}

pub(super) fn stream_config(
    config: &DirectRecordingConfig,
    include_cursor: bool,
) -> CaptureStreamConfig {
    CaptureStreamConfig {
        target_fps: config.capture_fps,
        min_fps: config.output_fps.min(config.capture_fps).max(1),
        buffer_depth: 2,
        max_consecutive_errors: 30,
        adaptive_fps: false,
        pause_on_resolution_change: false,
        include_cursor,
    }
}

pub(super) fn capture_options(config: &DirectRecordingConfig) -> CaptureOptions {
    CaptureOptions {
        workload: CaptureWorkload::Continuous,
        excluded_windows: config.excluded_windows.clone(),
        excluded_processes: config.excluded_processes.clone(),
        #[cfg(feature = "bench-stage-timing")]
        record_stage_timings: true,
        ..CaptureOptions::default()
    }
}

impl DirectCapture {
    #[cfg(windows)]
    pub fn gpu_dropped_frames(&self) -> u64 {
        match self {
            Self::Gpu(stream) => stream.stats().snapshot().frames_dropped,
            Self::Cpu(_) => 0, // CPU drop notifications are already counted.
        }
    }
    pub fn cpu(config: &DirectRecordingConfig, include_cursor: bool) -> Result<Self> {
        let system = CaptureSystem::builder()
            .with_backend_kind(config.capture_backend)
            .with_auto_backend_policy(crate::recording::recording_auto_backend_policy(
                crate::recording::RecordingCapturePath::Direct,
            ))
            .build()?;
        let session = system.open_session(
            resolve_capture_target(&RecordingTarget::Region(config.region))?,
            capture_options(config),
        )?;
        Ok(Self::Cpu(CaptureStream::spawn(
            session,
            stream_config(config, include_cursor),
        )?))
    }
    pub fn pause(&self) {
        match self {
            Self::Cpu(stream) => stream.pause(),
            #[cfg(windows)]
            Self::Gpu(stream) => stream.pause(),
        }
    }
    pub fn resume(&self) {
        match self {
            Self::Cpu(stream) => stream.resume(),
            #[cfg(windows)]
            Self::Gpu(stream) => stream.resume(),
        }
    }
    pub fn stop(&self) {
        match self {
            Self::Cpu(stream) => stream.stop(),
            #[cfg(windows)]
            Self::Gpu(stream) => stream.stop(),
        }
    }
    pub fn set_pacing_origin(&self, origin: Option<Instant>) {
        match self {
            Self::Cpu(stream) => stream.set_pacing_origin(origin),
            #[cfg(windows)]
            Self::Gpu(stream) => stream.set_pacing_origin(origin),
        }
    }
    #[cfg(feature = "bench-pipeline-timing")]
    pub fn stats(&self) -> &Arc<snow_capture::CaptureStreamStats> {
        match self {
            Self::Cpu(stream) => stream.stats(),
            #[cfg(windows)]
            Self::Gpu(stream) => stream.stats(),
        }
    }
    pub fn recv_timeout(
        &self,
        wait: Duration,
    ) -> std::result::Result<DirectCaptureEvent, snow_core::error::RecvTimeoutError> {
        match self {
            Self::Cpu(stream) => stream.recv_timeout(wait).map(Into::into),
            #[cfg(windows)]
            Self::Gpu(stream) => stream.recv_timeout(wait).map(Into::into),
        }
    }
    pub fn try_recv(
        &self,
    ) -> std::result::Result<DirectCaptureEvent, snow_core::error::TryRecvError> {
        match self {
            Self::Cpu(stream) => stream.try_recv().map(Into::into),
            #[cfg(windows)]
            Self::Gpu(stream) => stream.try_recv().map(Into::into),
        }
    }
    pub fn stop_and_drain(self) -> Vec<DirectCaptureEvent> {
        match self {
            Self::Cpu(stream) => stream
                .stop_and_drain()
                .into_iter()
                .map(Into::into)
                .collect(),
            #[cfg(windows)]
            Self::Gpu(stream) => stream
                .stop_and_drain()
                .into_iter()
                .map(Into::into)
                .collect(),
        }
    }
}

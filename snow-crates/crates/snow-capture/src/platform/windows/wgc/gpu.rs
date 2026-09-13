use super::*;
use crate::gpu::{GpuMonitorFrame, copy_texture};
use snow_d3d11::{SharedDevice, TexturePool};

pub(crate) struct GpuWgcCapturer {
    target: WorkerTarget,
    worker: WgcWorker,
    device: SharedDevice,
    pool: TexturePool,
    latest: Option<GpuMonitorFrame>,
    delivered: Option<(u64, u64)>,
    generation: u64,
}

impl GpuWgcCapturer {
    pub(crate) fn pool_pressure(&self) -> u64 {
        self.pool.pressure_count()
    }
    pub(crate) fn new(
        monitor: &super::super::monitor::ResolvedMonitor,
        device: SharedDevice,
    ) -> CaptureResult<Self> {
        validate_support()?;
        let target = WorkerTarget::Monitor {
            adapter_luid: monitor.key.adapter_luid,
            monitor: monitor.handle.0 as usize,
            hdr_metadata: monitor.hdr_metadata,
        };
        let mut worker = WgcWorker::new_with_device(target, Some(&device))?;
        worker.capture_mode = CaptureMode::Continuous;
        worker.resynchronize()?;
        Ok(Self {
            target,
            worker,
            pool: TexturePool::new(device.clone(), 6),
            device,
            latest: None,
            delivered: None,
            generation: 0,
        })
    }

    pub(crate) fn capture(&mut self) -> CaptureResult<GpuMonitorFrame> {
        self.device.check().map_err(CaptureError::platform)?;
        let pumped = self.worker.pump_frames();
        if self.worker.closed || matches!(pumped, Err(CaptureError::AccessLost)) {
            let mut worker = WgcWorker::new_with_device(self.target, Some(&self.device))?;
            worker.capture_mode = CaptureMode::Continuous;
            worker.resynchronize()?;
            self.worker = worker;
            self.latest = None;
            self.delivered = None;
            return Err(CaptureError::AccessLost);
        }
        pumped?;
        let metadata = self
            .worker
            .canonical
            .latest()
            .cloned()
            .ok_or(CaptureError::Timeout)?;
        let identity = (metadata.epoch, metadata.generation);
        if self.delivered == Some(identity) {
            let mut frame = self.latest.clone().ok_or(CaptureError::Timeout)?;
            frame.metadata.is_duplicate = true;
            return Ok(frame);
        }
        // Frame delivery, Close and worker recreation enter WGC's own locks.
        // Only GPU operations may hold the shared immediate-context lock.
        let _lock = self.device.lock();
        let (source, _, _) = self.worker.effective_canonical_source()?;
        let texture = copy_texture(&self.device, &mut self.pool, &source)?;
        self.generation = self.generation.wrapping_add(1);
        let mut frame_metadata = crate::FrameMetadata {
            backend_kind: CaptureBackendKind::WindowsGraphicsCapture,
            content_generation: Some(self.generation),
            ..Default::default()
        };
        frame_metadata.set_timing_with_format(
            Some(metadata.capture_time),
            Some(metadata.system_relative_time_hns),
            snow_core::timestamp::TickFormat::Hns100,
        );
        let frame = GpuMonitorFrame {
            texture,
            rotation: 0,
            metadata: frame_metadata,
        };
        self.latest = Some(frame.clone());
        self.delivered = Some(identity);
        Ok(frame)
    }
}

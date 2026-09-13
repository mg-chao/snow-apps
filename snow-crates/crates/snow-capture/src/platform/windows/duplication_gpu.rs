use super::*;
use crate::gpu::{GpuMonitorFrame, copy_texture};
use snow_d3d11::{SharedDevice, TexturePool};

pub(crate) struct GpuDxgiCapturer {
    device: SharedDevice,
    output: IDXGIOutput,
    duplication: IDXGIOutputDuplication,
    native: TexturePool,
    converted: TexturePool,
    latest: Option<GpuMonitorFrame>,
    hdr: Option<HdrFrameContext>,
    tonemapper: Option<GpuTonemapper>,
    f16: Option<GpuF16Converter>,
    generation: u64,
}

impl GpuDxgiCapturer {
    pub(crate) fn pool_pressure(&self) -> u64 {
        self.native.pressure_count() + self.converted.pressure_count()
    }
    pub(crate) fn new(monitor: &ResolvedMonitor, device: SharedDevice) -> CaptureResult<Self> {
        let duplication = create_duplication(&monitor.output, device.device())?;
        Ok(Self {
            output: monitor.output.clone(),
            duplication,
            native: TexturePool::new(device.clone(), 6),
            converted: TexturePool::new(device.clone(), 6),
            hdr: hdr_to_sdr_params(monitor.hdr_metadata),
            device,
            latest: None,
            tonemapper: None,
            f16: None,
            generation: 0,
        })
    }

    pub(crate) fn capture(&mut self) -> CaptureResult<GpuMonitorFrame> {
        let device = self.device.clone();
        let _lock = device.lock();
        device.check().map_err(CaptureError::platform)?;
        let (texture, info, guard) = match try_acquire_frame(&self.duplication, 0, false)? {
            TryAcquireResult::Ok(texture, info, guard) => (texture, info, guard),
            TryAcquireResult::AccessLost => {
                self.duplication = create_duplication(&self.output, device.device())?;
                self.latest = None;
                return Err(CaptureError::AccessLost);
            }
            TryAcquireResult::Retry => return self.duplicate(),
        };
        if info.LastPresentTime == 0 && self.latest.is_some() {
            drop(guard);
            return self.duplicate();
        }
        // Never retain the duplication-owned surface after ReleaseFrame.
        let native = copy_texture(&device, &mut self.native, &texture)?;
        drop(guard);
        let desc = native.desc();
        let texture = if desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT {
            let converted = if let Some(params) = self.hdr {
                if self.tonemapper.is_none() {
                    self.tonemapper = Some(GpuTonemapper::new(device.device())?);
                }
                self.tonemapper
                    .as_mut()
                    .unwrap()
                    .tonemap(
                        device.device(),
                        device.context(),
                        native.raw(),
                        &desc,
                        params.sanitized(),
                        None,
                    )?
                    .clone()
            } else {
                if self.f16.is_none() {
                    self.f16 = Some(GpuF16Converter::new(device.device())?);
                }
                self.f16
                    .as_mut()
                    .unwrap()
                    .convert(device.device(), device.context(), native.raw(), &desc, None)?
                    .clone()
            };
            copy_texture(&device, &mut self.converted, &converted)?
        } else {
            native
        };
        let output_desc = unsafe { self.output.GetDesc() }.map_err(CaptureError::platform)?;
        let rotation = match output_desc.Rotation.0 {
            2 => 1,
            3 => 2,
            4 => 3,
            _ => 0,
        };
        self.generation = self.generation.wrapping_add(1);
        let mut metadata = crate::frame::FrameMetadata {
            backend_kind: CaptureBackendKind::DxgiDuplication,
            content_generation: Some(self.generation),
            ..Default::default()
        };
        metadata.set_timing(
            Some(Instant::now()),
            (info.LastPresentTime != 0).then_some(info.LastPresentTime),
        );
        let frame = GpuMonitorFrame {
            texture,
            rotation,
            metadata,
        };
        self.latest = Some(frame.clone());
        Ok(frame)
    }

    fn duplicate(&self) -> CaptureResult<GpuMonitorFrame> {
        let mut frame = self.latest.clone().ok_or(CaptureError::Timeout)?;
        frame.metadata.is_duplicate = true;
        Ok(frame)
    }
}

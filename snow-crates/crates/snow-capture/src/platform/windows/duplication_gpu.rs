use super::*;
use crate::gpu::{
    GpuMonitorFrame, acquire_conversion_output, copy_texture_region, follow_native_crop,
    native_crop,
};
use snow_d3d11::{Rect, SharedDevice, Texture, TexturePool};

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
    // Moving crops need pixels outside the previously delivered crop even when
    // DXGI has no newer presentation. This private image is never published.
    follow_canonical: Option<Texture>,
    follow_crop: Option<Rect>,
    follow_size: Option<(u32, u32)>,
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
            follow_canonical: None,
            follow_crop: None,
            follow_size: None,
        })
    }

    pub(crate) fn enable_following(&mut self, size: (u32, u32)) {
        self.follow_size = Some(size);
    }

    pub(crate) fn capture(&mut self, crop: Rect) -> CaptureResult<GpuMonitorFrame> {
        let device = self.device.clone();
        let _lock = device.lock();
        device.check().map_err(CaptureError::platform)?;
        let acquired = acquire_with_capacity(&self.native, &self.converted, || {
            try_acquire_frame(&self.duplication, 0, false)
        })?;
        let (texture, info, guard) = match acquired {
            TryAcquireResult::Ok(texture, info, guard) => (texture, info, guard),
            TryAcquireResult::AccessLost => {
                self.duplication = create_duplication(&self.output, device.device())?;
                self.latest = None;
                self.follow_canonical = None;
                self.follow_crop = None;
                return Err(CaptureError::AccessLost);
            }
            TryAcquireResult::Retry if self.follow_size.is_some() => {
                return self.capture_follow_crop(crop);
            }
            TryAcquireResult::Retry => return self.duplicate(),
        };
        if info.LastPresentTime == 0 && self.latest.is_some() {
            drop(guard);
            if self.follow_size.is_some() {
                return self.capture_follow_crop(crop);
            }
            return self.duplicate();
        }
        let output_desc = unsafe { self.output.GetDesc() }.map_err(CaptureError::platform)?;
        let rotation = match output_desc.Rotation.0 {
            2 => 1,
            3 => 2,
            4 => 3,
            _ => 0,
        };
        let mut desc = D3D11_TEXTURE2D_DESC::default();
        unsafe { texture.GetDesc(&mut desc) };
        if self.follow_size.is_some() {
            if self.follow_canonical.as_ref().is_none_or(|image| {
                let old = image.desc();
                old.Width != desc.Width || old.Height != desc.Height || old.Format != desc.Format
            }) {
                self.follow_canonical = Some(
                    device
                        .texture(desc.Width, desc.Height, desc.Format, 0)
                        .map_err(CaptureError::platform)?,
                );
            }
            unsafe {
                device
                    .context()
                    .CopyResource(self.follow_canonical.as_ref().unwrap().raw(), &texture);
            }
            drop(guard);
            self.follow_crop = None;
            return self.capture_follow_crop(crop);
        }
        let crop = native_crop(crop, (desc.Width, desc.Height), rotation)?;
        // Detach only the selected region before ReleaseFrame. HDR conversion
        // then operates on that same bounded surface without a full-display copy.
        let native = copy_texture_region(&device, &mut self.native, &texture, crop)?;
        drop(guard);
        let desc = native.desc();
        let texture = if desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT {
            let output = acquire_conversion_output(&mut self.converted, desc.Width, desc.Height)?;
            if let Some(params) = self.hdr {
                if self.tonemapper.is_none() {
                    self.tonemapper = Some(GpuTonemapper::new(device.device())?);
                }
                self.tonemapper.as_mut().unwrap().tonemap_into(
                    device.device(),
                    device.context(),
                    native.raw(),
                    &desc,
                    params.sanitized(),
                    None,
                    &output,
                )?;
            } else {
                if self.f16.is_none() {
                    self.f16 = Some(GpuF16Converter::new(device.device())?);
                }
                self.f16.as_mut().unwrap().convert_into(
                    device.device(),
                    device.context(),
                    native.raw(),
                    &desc,
                    None,
                    &output,
                )?;
            }
            output
        } else {
            native
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
            source: Rect::full(texture.dimensions()),
            texture,
            rotation,
            metadata,
        };
        self.latest = Some(frame.clone());
        Ok(frame)
    }

    fn capture_follow_crop(&mut self, crop: Rect) -> CaptureResult<GpuMonitorFrame> {
        let canonical = self
            .follow_canonical
            .as_ref()
            .ok_or(CaptureError::Timeout)?;
        let output_desc = unsafe { self.output.GetDesc() }.map_err(CaptureError::platform)?;
        let rotation = match output_desc.Rotation.0 {
            2 => 1,
            3 => 2,
            4 => 3,
            _ => 0,
        };
        let desc = canonical.desc();
        let native_rect = follow_native_crop(
            crop,
            (desc.Width, desc.Height),
            rotation,
            self.follow_size
                .expect("following source has prepared geometry"),
        )?;
        if self.follow_crop == Some(crop) {
            return self.duplicate();
        }
        let native =
            copy_texture_region(&self.device, &mut self.native, canonical.raw(), native_rect)?;
        let desc = native.desc();
        let texture = if desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT {
            let output = acquire_conversion_output(&mut self.converted, desc.Width, desc.Height)?;
            if let Some(params) = self.hdr {
                if self.tonemapper.is_none() {
                    self.tonemapper = Some(GpuTonemapper::new(self.device.device())?);
                }
                self.tonemapper.as_mut().unwrap().tonemap_into(
                    self.device.device(),
                    self.device.context(),
                    native.raw(),
                    &desc,
                    params.sanitized(),
                    None,
                    &output,
                )?;
            } else {
                if self.f16.is_none() {
                    self.f16 = Some(GpuF16Converter::new(self.device.device())?);
                }
                self.f16.as_mut().unwrap().convert_into(
                    self.device.device(),
                    self.device.context(),
                    native.raw(),
                    &desc,
                    None,
                    &output,
                )?;
            }
            output
        } else {
            native
        };
        self.generation = self.generation.wrapping_add(1);
        let mut metadata = crate::FrameMetadata {
            backend_kind: CaptureBackendKind::DxgiDuplication,
            content_generation: Some(self.generation),
            ..Default::default()
        };
        metadata.set_timing(Some(Instant::now()), None);
        let frame = GpuMonitorFrame {
            source: Rect::full(texture.dimensions()),
            texture,
            rotation,
            metadata,
        };
        self.follow_crop = Some(crop);
        self.latest = Some(frame.clone());
        Ok(frame)
    }

    fn duplicate(&self) -> CaptureResult<GpuMonitorFrame> {
        let mut frame = self.latest.clone().ok_or(CaptureError::Timeout)?;
        frame.metadata.is_duplicate = true;
        Ok(frame)
    }
}

/// AcquireNextFrame consumes a desktop update even if the subsequent copy
/// cannot obtain a texture. Check every destination pool first, including HDR
/// conversion: no Timeout may discard that update after ReleaseFrame.
///
/// Both pools belong exclusively to the capture thread. Until its one acquire
/// per pool below, external readers can only release leases, so available
/// capacity cannot disappear between this check and the copy/conversion.
fn acquire_with_capacity<T>(
    native: &TexturePool,
    converted: &TexturePool,
    acquire: impl FnOnce() -> CaptureResult<T>,
) -> CaptureResult<T> {
    if !native.has_capacity() || !converted.has_capacity() {
        return Err(CaptureError::Timeout);
    }
    acquire()
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows::Win32::Graphics::Dxgi::Common::{
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
    };
    use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory4};

    #[test]
    fn gpu_backpressure_does_not_consume_the_final_desktop_update() -> anyhow::Result<()> {
        let factory: IDXGIFactory4 = unsafe { CreateDXGIFactory1()? };
        let adapter = unsafe { factory.EnumWarpAdapter()? };
        let device = SharedDevice::create_compute(&adapter)?;
        for (block_native, block_converted) in [(true, false), (false, true), (true, true)] {
            let mut native = TexturePool::new(device.clone(), 2);
            let mut converted = TexturePool::new(device.clone(), 2);
            assert!(native.has_capacity());
            assert!(converted.has_capacity());
            let mut native_leases = Vec::new();
            let mut converted_leases = Vec::new();
            for (pool, leases, full) in [
                (&mut native, &mut native_leases, block_native),
                (&mut converted, &mut converted_leases, block_converted),
            ] {
                for _ in 0..if full { 2 } else { 1 } {
                    leases.push(
                        pool.acquire(8, 6, DXGI_FORMAT_R8G8B8A8_UNORM, 0)?
                            .expect("available texture"),
                    );
                }
            }
            let mut final_update = Some(42);
            let mut acquisitions = 0;
            let mut acquire = || {
                acquisitions += 1;
                Ok(final_update.take())
            };
            for _ in 0..3 {
                assert!(matches!(
                    acquire_with_capacity(&native, &converted, &mut acquire),
                    Err(CaptureError::Timeout)
                ));
            }
            native_leases.clear();
            if block_converted {
                assert!(matches!(
                    acquire_with_capacity(&native, &converted, &mut acquire),
                    Err(CaptureError::Timeout)
                ));
            }
            converted_leases.clear();
            assert_eq!(
                acquire_with_capacity(&native, &converted, &mut acquire)?,
                Some(42),
                "the last update must remain available without another desktop change"
            );
            assert_eq!(acquisitions, 1);
            assert!(final_update.is_none());
            // Free storage also admits geometry/format changes, even at capacity.
            assert!(
                native
                    .acquire(16, 12, DXGI_FORMAT_B8G8R8A8_UNORM, 0)?
                    .is_some()
            );
            assert!(matches!(
                acquire_with_capacity(&native, &converted, || Err::<(), _>(
                    CaptureError::AccessLost
                )),
                Err(CaptureError::AccessLost)
            ));
        }
        Ok(())
    }
}

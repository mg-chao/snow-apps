use super::*;
use crate::gpu::{
    GpuMonitorFrame, acquire_conversion_output, copy_texture_region, follow_native_crop,
    native_crop,
};
use snow_d3d11::{Rect, SharedDevice, Texture, TexturePool};

pub(crate) struct GpuWgcCapturer {
    target: WorkerTarget,
    worker: WgcWorker,
    device: SharedDevice,
    native: TexturePool,
    pool: TexturePool,
    latest: Option<GpuMonitorFrame>,
    delivered: Option<((u64, u64), Rect)>,
    generation: u64,
    follow_size: Option<(u32, u32)>,
}

impl GpuWgcCapturer {
    pub(crate) fn pool_pressure(&self) -> u64 {
        self.native.pressure_count() + self.pool.pressure_count()
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
            native: TexturePool::new(device.clone(), 2),
            pool: TexturePool::new(device.clone(), 6),
            device,
            latest: None,
            delivered: None,
            generation: 0,
            follow_size: None,
        })
    }

    pub(crate) fn enable_following(&mut self, size: (u32, u32)) {
        self.follow_size = Some(size);
    }

    pub(crate) fn capture(&mut self, crop: Rect) -> CaptureResult<GpuMonitorFrame> {
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
        if self.delivered == Some((identity, crop)) {
            let mut frame = self.latest.clone().ok_or(CaptureError::Timeout)?;
            frame.metadata.is_duplicate = true;
            return Ok(frame);
        }
        // Frame delivery, Close and worker recreation enter WGC's own locks.
        // Only GPU operations may hold the shared immediate-context lock.
        let _lock = self.device.lock();
        let desc = self.worker.canonical.desc().ok_or(CaptureError::Timeout)?;
        let size = (desc.Width, desc.Height);
        let crop = if let Some(expected) = self.follow_size {
            follow_native_crop(crop, size, 0, expected)?
        } else {
            native_crop(crop, size, 0)?
        };
        let (texture, source) = if desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
            && self.worker.gpu_hdr_conversion_enabled
        {
            let source = self
                .worker
                .canonical
                .texture()
                .cloned()
                .ok_or(CaptureError::Timeout)?;
            let output = convert_crop(
                &self.device,
                &mut self.native,
                &mut self.pool,
                &source,
                crop,
                |native, desc, output| {
                    self.worker
                        .effective_source_into(native.clone(), *desc, Some(output))
                        .map(|_| ())
                },
            )?;
            let source = Rect::full(output.dimensions());
            (output, source)
        } else {
            let (source, _, _) = self.worker.effective_canonical_source()?;
            let texture = copy_texture_region(&self.device, &mut self.pool, &source, crop)?;
            let region = Rect::full(texture.dimensions());
            (texture, region)
        };
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
            source,
            rotation: 0,
            metadata: frame_metadata,
        };
        self.latest = Some(frame.clone());
        self.delivered = Some((identity, crop));
        Ok(frame)
    }
}

/// Bound HDR dispatch and immutable output leases to the requested viewport.
/// The full canonical image remains available for the next pan.
fn convert_crop(
    device: &SharedDevice,
    native_pool: &mut TexturePool,
    output_pool: &mut TexturePool,
    source: &ID3D11Texture2D,
    crop: Rect,
    convert: impl FnOnce(&ID3D11Texture2D, &D3D11_TEXTURE2D_DESC, &Texture) -> CaptureResult<()>,
) -> CaptureResult<Texture> {
    let mut desc = D3D11_TEXTURE2D_DESC::default();
    unsafe { source.GetDesc(&mut desc) };
    let output = acquire_conversion_output(output_pool, crop.width, crop.height)?;
    // Full-display recordings already have a correctly sized canonical image.
    // Avoid adding an HDR copy to that existing path.
    let native = if crop == Rect::full((desc.Width, desc.Height)) {
        None
    } else {
        Some(copy_texture_region(device, native_pool, source, crop)?)
    };
    if let Some(native) = native {
        convert(native.raw(), &native.desc(), &output)?;
    } else {
        convert(source, &desc, &output)?;
    }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::convert::SurfaceConversionOptions;
    use crate::platform::windows::surface;
    use windows::Win32::Graphics::Direct3D11::*;
    use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory4};

    fn pixels(device: &SharedDevice, texture: &ID3D11Texture2D) -> CaptureResult<Frame> {
        let mut desc = D3D11_TEXTURE2D_DESC::default();
        unsafe { texture.GetDesc(&mut desc) };
        let mut staging = None;
        let staging = surface::ensure_staging_texture(
            device.device(),
            &mut staging,
            &desc,
            surface::StagingSampleDesc::SingleSample,
            "test HDR crop staging",
        )?;
        unsafe { device.context().CopyResource(staging, texture) };
        let mut frame = Frame::empty();
        surface::map_staging_to_frame_blocking(
            device.context(),
            staging,
            None,
            &desc,
            &mut frame,
            SurfaceConversionOptions::default(),
            "test HDR crop map",
        )?;
        Ok(frame)
    }

    #[test]
    fn follow_mouse_hdr_crop_matches_full_conversion_and_preserves_held_frames()
    -> anyhow::Result<()> {
        let factory: IDXGIFactory4 = unsafe { CreateDXGIFactory1()? };
        let adapter = unsafe { factory.EnumWarpAdapter()? };
        let device = SharedDevice::create_compute(&adapter)?;
        let _lock = device.lock();
        let source = device.texture(
            19,
            11,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE.0 as u32,
        )?;
        let input: Vec<[u16; 4]> = (0..19 * 11)
            .map(|i| {
                [
                    ((i % 17) as f32) / 4.0,
                    ((i % 31) as f32) / 6.0,
                    ((i % 13) as f32) / 3.0,
                    0.5,
                ]
                .map(|v| half::f16::from_f32(v).to_bits())
            })
            .collect();
        unsafe {
            device.context().UpdateSubresource(
                source.raw(),
                0,
                None,
                input.as_ptr().cast(),
                19 * 8,
                0,
            );
        }
        let mut mapper = GpuTonemapper::new(device.device())?;
        let mut converter = GpuF16Converter::new(device.device())?;
        for params in [
            None,
            Some(HdrFrameContext {
                tonemap_use_lut: false,
                ..Default::default()
            }),
            Some(HdrFrameContext {
                tonemap_use_lut: true,
                ..Default::default()
            }),
        ] {
            let reference = if let Some(params) = params {
                mapper.tonemap(
                    device.device(),
                    device.context(),
                    source.raw(),
                    &source.desc(),
                    params,
                    None,
                )?
            } else {
                converter.convert(
                    device.device(),
                    device.context(),
                    source.raw(),
                    &source.desc(),
                    None,
                )?
            }
            .clone();
            let reference = pixels(&device, &reference)?;
            let mut native_pool = TexturePool::new(device.clone(), 2);
            let mut output_pool = TexturePool::new(device.clone(), 4);
            let mut held = Vec::new();
            for crop in [
                Rect::full(source.dimensions()),
                Rect {
                    x: 0,
                    y: 0,
                    width: 5,
                    height: 3,
                },
                Rect {
                    x: 8,
                    y: 4,
                    width: 5,
                    height: 3,
                },
                Rect {
                    x: 16,
                    y: 9,
                    width: 3,
                    height: 2,
                },
            ] {
                let output = convert_crop(
                    &device,
                    &mut native_pool,
                    &mut output_pool,
                    source.raw(),
                    crop,
                    |native, desc, output| {
                        assert_eq!((desc.Width, desc.Height), (crop.width, crop.height));
                        assert_eq!(output.dimensions(), (crop.width, crop.height));
                        assert_eq!(
                            native == source.raw(),
                            crop == Rect::full(source.dimensions()),
                            "only full-display conversion should read the canonical image directly"
                        );
                        if let Some(params) = params {
                            mapper.tonemap_into(
                                device.device(),
                                device.context(),
                                native,
                                desc,
                                params,
                                None,
                                output,
                            )
                        } else {
                            converter.convert_into(
                                device.device(),
                                device.context(),
                                native,
                                desc,
                                None,
                                output,
                            )
                        }
                    },
                )?;
                let expected: Vec<u8> = (0..crop.height)
                    .flat_map(|row| {
                        let start = (((crop.y as u32 + row) * 19 + crop.x as u32) * 4) as usize;
                        reference.as_bytes()[start..start + crop.width as usize * 4]
                            .iter()
                            .copied()
                    })
                    .collect();
                assert_eq!(pixels(&device, output.raw())?.as_bytes(), expected);
                held.push((output, expected));
            }
            // Pressure must not overwrite leases or execute another conversion.
            assert!(matches!(
                convert_crop(
                    &device,
                    &mut native_pool,
                    &mut output_pool,
                    source.raw(),
                    Rect {
                        x: 1,
                        y: 1,
                        width: 5,
                        height: 3
                    },
                    |_, _, _| panic!("conversion must wait for an output lease")
                ),
                Err(CaptureError::Timeout)
            ));
            for (image, expected) in &held {
                assert_eq!(pixels(&device, image.raw())?.as_bytes(), expected);
            }
        }
        Ok(())
    }
}

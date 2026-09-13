//! FFmpeg hardware surfaces on the recorder's existing D3D11 device.
use std::ffi::c_void;
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use ffmpeg::ffi::*;
use ffmpeg_next as ffmpeg;
use snow_d3d11::SharedDevice;
use windows::Win32::Graphics::Direct3D11::{D3D11_BIND_RENDER_TARGET, ID3D11Texture2D};
use windows::core::Interface;

use crate::error::{RecordingExportError, Result};

fn failure(message: impl std::fmt::Display) -> RecordingExportError {
    RecordingExportError::Encode(format!("GPU encoder: {message}"))
}

fn check(code: i32) -> Result<()> {
    if code < 0 {
        Err(failure(ffmpeg::Error::from(code)))
    } else {
        Ok(())
    }
}

struct Buffer(*mut AVBufferRef);
impl Buffer {
    fn new(value: *mut AVBufferRef) -> Result<Self> {
        if value.is_null() {
            Err(failure("hardware context allocation failed"))
        } else {
            Ok(Self(value))
        }
    }
    fn reference(&self) -> Result<*mut AVBufferRef> {
        let value = unsafe { av_buffer_ref(self.0) };
        if value.is_null() {
            Err(failure("hardware context reference failed"))
        } else {
            Ok(value)
        }
    }
}
impl Drop for Buffer {
    fn drop(&mut self) {
        unsafe { av_buffer_unref(&mut self.0) };
    }
}

unsafe extern "C" fn lock_device(opaque: *mut c_void) {
    unsafe { (&*opaque.cast::<SharedDevice>()).enter() };
}
unsafe extern "C" fn unlock_device(opaque: *mut c_void) {
    unsafe { (&*opaque.cast::<SharedDevice>()).leave() };
}
unsafe extern "C" fn free_device(context: *mut AVHWDeviceContext) {
    unsafe { drop(Box::from_raw((*context).user_opaque.cast::<SharedDevice>())) };
}

struct Permit(Arc<AtomicUsize>);
impl Drop for Permit {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::AcqRel);
    }
}
unsafe extern "C" fn free_permit(_opaque: *mut c_void, data: *mut u8) {
    unsafe { drop(Box::from_raw(data.cast::<Permit>())) };
}

/// Owns only configuration until the encoder's worker constructs its contexts.
#[derive(Clone, Debug)]
pub struct GpuInputConfig {
    pub device: SharedDevice,
}

pub(crate) struct HardwareFrames {
    pub device: SharedDevice,
    device_ref: Buffer,
    native: Buffer,
    qsv_device: Option<Buffer>,
    mapped: Option<Buffer>,
    size: (u32, u32),
    outstanding: Arc<AtomicUsize>,
    capacity: usize,
}

impl HardwareFrames {
    #[cfg(test)]
    pub(crate) fn outstanding_tracker(&self) -> Arc<AtomicUsize> {
        Arc::clone(&self.outstanding)
    }

    /// One submitted frame of codec delay, one deferred duration frame, one
    /// frame being composed, and two driver references. Lookahead/B-frames are
    /// disabled below, so capacity cannot grow with the recording duration.
    pub(crate) const LIVE_CAPACITY: usize = 5;

    pub(crate) fn open(
        &self,
        encoder: ffmpeg::codec::encoder::video::Video,
        codec: ffmpeg::Codec,
        quality: u8,
    ) -> Result<ffmpeg::encoder::video::Encoder> {
        let qp = crate::video_quality::quality_to_h264_crf(quality).to_string();
        let mut options = ffmpeg::Dictionary::new();
        options.set("bf", "0");
        match self.codec_name() {
            "h264_nvenc" => {
                options.set("preset", "p4");
                options.set("tune", "ull");
                options.set("rc", "constqp");
                options.set("qp", &qp);
                options.set("rc-lookahead", "0");
                options.set("delay", "0");
                options.set("surfaces", &Self::LIVE_CAPACITY.to_string());
            }
            "h264_amf" => {
                options.set("usage", "ultralowlatency");
                options.set("quality", "balanced");
                options.set("rc", "cqp");
                options.set("qp_i", &qp);
                options.set("qp_p", &qp);
                options.set("preanalysis", "0");
                options.set("preencode", "0");
                options.set("query_timeout", "100");
            }
            "h264_qsv" => {
                options.set("preset", "medium");
                options.set("look_ahead", "0");
                options.set("async_depth", "1");
                options.set("global_quality", &qp);
            }
            _ => unreachable!(),
        }
        encoder.open_as_with(codec, options).map_err(failure)
    }
    pub(crate) fn new(config: GpuInputConfig, size: (u32, u32), capacity: usize) -> Result<Self> {
        let device = config.device;
        let native_encoder = snow_d3d11::h264_encoder(device.identity().vendor)
            .ok_or_else(|| failure("capture adapter has no supported hardware encoder"))?;
        let device_ref = Buffer::new(unsafe {
            av_hwdevice_ctx_alloc(AVHWDeviceType::AV_HWDEVICE_TYPE_D3D11VA)
        })?;
        unsafe {
            let context = (*device_ref.0).data.cast::<AVHWDeviceContext>();
            let d3d = (*context).hwctx.cast::<AVD3D11VADeviceContext>();
            (*d3d).device = device.device().clone().into_raw().cast();
            (*d3d).device_context = device.context().clone().into_raw().cast();
            let owner = Box::into_raw(Box::new(device.clone())).cast();
            (*context).user_opaque = owner;
            (*context).free = Some(free_device);
            (*d3d).lock_ctx = owner;
            (*d3d).lock = Some(lock_device);
            (*d3d).unlock = Some(unlock_device);
            check(av_hwdevice_ctx_init(device_ref.0))?;
        }
        let native = Buffer::new(unsafe { av_hwframe_ctx_alloc(device_ref.0) })?;
        unsafe {
            let context = (*native.0).data.cast::<AVHWFramesContext>();
            (*context).format = AVPixelFormat::AV_PIX_FMT_D3D11;
            (*context).sw_format = AVPixelFormat::AV_PIX_FMT_NV12;
            (*context).width = i32::try_from(size.0.div_ceil(16) * 16).map_err(failure)?;
            (*context).height = i32::try_from(size.1.div_ceil(16) * 16).map_err(failure)?;
            // Separate textures avoid mutable AMF array-index metadata sharing.
            (*context).initial_pool_size = 0;
            let d3d = (*context).hwctx.cast::<AVD3D11VAFramesContext>();
            (*d3d).BindFlags = D3D11_BIND_RENDER_TARGET.0 as u32;
            check(av_hwframe_ctx_init(native.0))?;
        }
        let (qsv_device, mapped) = if native_encoder == "h264_qsv" {
            let mut qsv = ptr::null_mut();
            check(unsafe {
                av_hwdevice_ctx_create_derived(
                    &mut qsv,
                    AVHWDeviceType::AV_HWDEVICE_TYPE_QSV,
                    device_ref.0,
                    0,
                )
            })?;
            let qsv = Buffer::new(qsv)?;
            let mut mapped = ptr::null_mut();
            check(unsafe {
                av_hwframe_ctx_create_derived(
                    &mut mapped,
                    AVPixelFormat::AV_PIX_FMT_QSV,
                    qsv.0,
                    native.0,
                    AV_HWFRAME_MAP_READ as i32 | AV_HWFRAME_MAP_DIRECT as i32,
                )
            })?;
            (Some(qsv), Some(Buffer::new(mapped)?))
        } else {
            (None, None)
        };
        Ok(Self {
            device,
            device_ref,
            native,
            qsv_device,
            mapped,
            size,
            outstanding: Arc::new(AtomicUsize::new(0)),
            capacity,
        })
    }

    pub(crate) fn codec_name(&self) -> &'static str {
        snow_d3d11::h264_encoder(self.device.identity().vendor).expect("validated hardware adapter")
    }

    pub(crate) fn pixel_format(&self) -> ffmpeg::format::Pixel {
        if self.mapped.is_some() {
            AVPixelFormat::AV_PIX_FMT_QSV.into()
        } else {
            AVPixelFormat::AV_PIX_FMT_D3D11.into()
        }
    }

    pub(crate) fn configure(
        &self,
        encoder: &mut ffmpeg::codec::encoder::video::Video,
    ) -> Result<()> {
        unsafe {
            let context = encoder.as_mut_ptr();
            (*context).hw_device_ctx = self
                .qsv_device
                .as_ref()
                .unwrap_or(&self.device_ref)
                .reference()?;
            (*context).hw_frames_ctx = self.mapped.as_ref().unwrap_or(&self.native).reference()?;
            (*context).max_b_frames = 0;
            (*context).color_range = AVColorRange::AVCOL_RANGE_MPEG;
            (*context).colorspace = AVColorSpace::AVCOL_SPC_BT709;
            (*context).color_primaries = AVColorPrimaries::AVCOL_PRI_BT709;
            (*context).color_trc = AVColorTransferCharacteristic::AVCOL_TRC_BT709;
        }
        Ok(())
    }

    pub(crate) fn allocate(&self) -> Result<Option<GpuEncoderFrame>> {
        if self
            .outstanding
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |count| {
                (count < self.capacity).then_some(count + 1)
            })
            .is_err()
        {
            return Ok(None);
        }
        let permit = Box::new(Permit(Arc::clone(&self.outstanding)));
        let mut native = ffmpeg::frame::Video::empty();
        check(unsafe { av_hwframe_get_buffer(self.native.0, native.as_mut_ptr(), 0) })?;
        let data = Box::into_raw(permit).cast::<u8>();
        let reference = unsafe {
            av_buffer_create(
                data,
                std::mem::size_of::<Permit>(),
                Some(free_permit),
                ptr::null_mut(),
                0,
            )
        };
        if reference.is_null() {
            unsafe { drop(Box::from_raw(data.cast::<Permit>())) };
            return Err(failure("surface lifetime reference allocation failed"));
        }
        unsafe {
            (*native.as_mut_ptr()).opaque_ref = reference;
        }
        native.set_width(self.size.0);
        native.set_height(self.size.1);
        Ok(Some(GpuEncoderFrame {
            native,
            device: self.device.clone(),
        }))
    }

    pub(crate) fn encode_frame(&self, frame: GpuEncoderFrame) -> Result<ffmpeg::frame::Video> {
        if !self.device.same_device(&frame.device) {
            return Err(failure("input surface belongs to another device"));
        }
        let source_context = unsafe { (*frame.native.as_ptr()).hw_frames_ctx };
        if source_context.is_null() || unsafe { (*source_context).data != (*self.native.0).data } {
            return Err(failure("input surface belongs to another encoder pool"));
        }
        if let Some(mapped) = &self.mapped {
            let mut output = ffmpeg::frame::Video::empty();
            unsafe {
                (*output.as_mut_ptr()).format = AVPixelFormat::AV_PIX_FMT_QSV as i32;
                (*output.as_mut_ptr()).hw_frames_ctx = mapped.reference()?;
                check(av_hwframe_map(
                    output.as_mut_ptr(),
                    frame.native.as_ptr(),
                    AV_HWFRAME_MAP_READ as i32 | AV_HWFRAME_MAP_DIRECT as i32,
                ))?;
                check(av_frame_copy_props(
                    output.as_mut_ptr(),
                    frame.native.as_ptr(),
                ))?;
            }
            Ok(output)
        } else {
            Ok(frame.native)
        }
    }
}

/// Encoder-owned NV12 storage. Its AVBuffer references retain the pool permit
/// even after submit returns and the application drops its copy.
pub struct GpuEncoderFrame {
    native: ffmpeg::frame::Video,
    device: SharedDevice,
}

impl GpuEncoderFrame {
    pub fn texture(&self) -> Result<ID3D11Texture2D> {
        let raw = unsafe { (*self.native.as_ptr()).data[0].cast::<c_void>() };
        unsafe { ID3D11Texture2D::from_raw_borrowed(&raw) }
            .cloned()
            .ok_or_else(|| failure("missing D3D11 frame texture"))
    }
    pub fn slice(&self) -> u32 {
        unsafe { (*self.native.as_ptr()).data[1] as usize as u32 }
    }
    pub fn device(&self) -> &SharedDevice {
        &self.device
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};

    #[test]
    #[ignore = "requires an offscreen hardware D3D11 device"]
    fn hardware_surface_identity_pool_pressure_and_ffmpeg_reference_lifetime() -> anyhow::Result<()>
    {
        crate::ffmpeg_util::ensure_ffmpeg_initialized()?;
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }?;
        let adapter = unsafe { factory.EnumAdapters1(0) }?.cast()?;
        let device = SharedDevice::create(&adapter)?;
        let pool = HardwareFrames::new(
            GpuInputConfig {
                device: device.clone(),
            },
            (32, 32),
            2,
        )?;
        let first = pool.allocate()?.unwrap();
        let composed_texture = first.texture()?;
        let composed_slice = first.slice();
        let native = pool.encode_frame(first)?;
        if pool.mapped.is_none() {
            assert_eq!(
                unsafe { (*native.as_ptr()).data[0] },
                composed_texture.as_raw().cast()
            );
            assert_eq!(
                unsafe { (*native.as_ptr()).data[1] as usize as u32 },
                composed_slice
            );
        }
        let mut encoder_reference = ffmpeg::frame::Video::empty();
        check(unsafe { av_frame_ref(encoder_reference.as_mut_ptr(), native.as_ptr()) })?;
        let second = pool.allocate()?.unwrap();
        assert!(pool.allocate()?.is_none());
        drop(native);
        assert!(
            pool.allocate()?.is_none(),
            "encoder still references the first surface"
        );
        drop(encoder_reference);
        assert!(pool.allocate()?.is_some());
        let other = HardwareFrames::new(GpuInputConfig { device }, (32, 32), 1)?;
        assert!(
            other.encode_frame(second).is_err(),
            "a different encoder pool must be rejected"
        );
        assert_eq!(pool.outstanding.load(Ordering::Acquire), 0);
        Ok(())
    }
}

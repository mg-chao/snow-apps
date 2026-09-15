//! FFmpeg frame ownership for CoreVideo surfaces. AVBufferRef owns the lease,
//! including while VideoToolbox asynchronously retains a submitted frame.
use crate::error::{RecordingExportError, Result};
use ffmpeg_next as ffmpeg;
use snow_media::{ColorDescription, PixelFormat, macos::PixelBuffer};

pub(crate) unsafe fn set_hdr_context(context: *mut ffmpeg::ffi::AVCodecContext) {
    unsafe {
        (*context).color_range = ffmpeg::ffi::AVColorRange::AVCOL_RANGE_MPEG;
        (*context).colorspace = ffmpeg::ffi::AVColorSpace::AVCOL_SPC_BT2020_NCL;
        (*context).color_primaries = ffmpeg::ffi::AVColorPrimaries::AVCOL_PRI_BT2020;
        (*context).color_trc = ffmpeg::ffi::AVColorTransferCharacteristic::AVCOL_TRC_SMPTE2084;
        (*context).profile = 2; // HEVC Main 10 (ISO/IEC 23008-2).
        (*context).codec_tag = u32::from_le_bytes(*b"hvc1");
    }
}

pub(crate) fn frame(image: PixelBuffer) -> Result<ffmpeg::frame::Video> {
    if image.format() == PixelFormat::P010 && image.color() != ColorDescription::HDR10 {
        return Err(RecordingExportError::InvalidConfig(
            "P010 native input must contain converted BT.2020/PQ HDR10 pixels".into(),
        ));
    }
    if image.format() == PixelFormat::Bgra8 && image.color() != ColorDescription::SRGB {
        return Err(RecordingExportError::InvalidConfig(
            "BGRA native input must be sRGB".into(),
        ));
    }
    let size = image.size();
    let hdr = image.format() == PixelFormat::P010;
    unsafe extern "C" fn release(opaque: *mut std::ffi::c_void, _data: *mut u8) {
        unsafe {
            drop(Box::from_raw(opaque.cast::<PixelBuffer>()));
        }
    }
    unsafe {
        let native = image.native_buffer() as *const _ as *mut u8;
        let owner = Box::into_raw(Box::new(image));
        let buffer = ffmpeg::ffi::av_buffer_create(
            native,
            1,
            Some(release),
            owner.cast(),
            ffmpeg::ffi::AV_BUFFER_FLAG_READONLY,
        );
        if buffer.is_null() {
            drop(Box::from_raw(owner));
            return Err(RecordingExportError::Encode(
                "native frame reference allocation failed".into(),
            ));
        }
        let mut frame = ffmpeg::frame::Video::empty();
        let raw = frame.as_mut_ptr();
        (*raw).format = ffmpeg::ffi::AVPixelFormat::AV_PIX_FMT_VIDEOTOOLBOX as i32;
        (*raw).width = size.width as i32;
        (*raw).height = size.height as i32;
        (*raw).data[3] = native;
        (*raw).buf[0] = buffer;
        (*raw).color_range = if hdr {
            ffmpeg::ffi::AVColorRange::AVCOL_RANGE_MPEG
        } else {
            ffmpeg::ffi::AVColorRange::AVCOL_RANGE_JPEG
        };
        (*raw).colorspace = if hdr {
            ffmpeg::ffi::AVColorSpace::AVCOL_SPC_BT2020_NCL
        } else {
            ffmpeg::ffi::AVColorSpace::AVCOL_SPC_BT709
        };
        (*raw).color_primaries = if hdr {
            ffmpeg::ffi::AVColorPrimaries::AVCOL_PRI_BT2020
        } else {
            ffmpeg::ffi::AVColorPrimaries::AVCOL_PRI_BT709
        };
        (*raw).color_trc = if hdr {
            ffmpeg::ffi::AVColorTransferCharacteristic::AVCOL_TRC_SMPTE2084
        } else {
            ffmpeg::ffi::AVColorTransferCharacteristic::AVCOL_TRC_IEC61966_2_1
        };
        Ok(frame)
    }
}

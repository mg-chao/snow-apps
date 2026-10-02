use crate::codec::configure_codec_threads;
use crate::config::{ExportExecutionMode, ExportPerformanceConfig};
use crate::error::{RecordingExportError as ScreenRecorderError, Result};
use ffmpeg_next as ffmpeg;
use std::{ffi::c_void, ptr};

pub(crate) fn hardware_video_decode_allowed(mode: ExportExecutionMode) -> bool {
    matches!(
        mode,
        ExportExecutionMode::HardwarePreferred | ExportExecutionMode::HardwareOnly
    )
}

pub(crate) struct HardwareDecodeSelection {
    pub(crate) hw_pix_fmt: ffmpeg::ffi::AVPixelFormat,
}

pub(crate) struct HardwareDecodeState {
    pub(crate) device_ctx: *mut ffmpeg::ffi::AVBufferRef,
    pub(crate) _selection: Box<HardwareDecodeSelection>,
    pub(crate) hw_pixel_format: ffmpeg::format::Pixel,
    pub(crate) device_name: &'static str,
}

impl Drop for HardwareDecodeState {
    fn drop(&mut self) {
        unsafe {
            if !self.device_ctx.is_null() {
                ffmpeg::ffi::av_buffer_unref(&mut self.device_ctx);
            }
        }
    }
}

pub(crate) struct SourceVideoDecoder<Decoder = ffmpeg::decoder::Video> {
    // Fields drop in declaration order. Codec shutdown joins decoding threads
    // before the hardware state's selection, referenced by AVCodecContext::opaque,
    // can be released.
    pub(crate) decoder: Decoder,
    pub(crate) hardware: Option<HardwareDecodeState>,
}

pub(crate) fn prepare_hardware_decoder_context(
    mut hardware: HardwareDecodeState,
    perf_config: &ExportPerformanceConfig,
    create_context: impl FnOnce() -> std::result::Result<ffmpeg::codec::context::Context, ffmpeg::Error>,
) -> Result<SourceVideoDecoder<ffmpeg::codec::context::Context>> {
    // Own the device before any fallible decoder setup. Both context creation and
    // codec opening can fail after native hardware resources have been allocated.
    let mut context = create_context().map_err(|err| {
        ScreenRecorderError::Export(format!(
            "failed to create source video decoder context: {err}"
        ))
    })?;
    configure_codec_threads(
        &mut context,
        perf_config.decode_threads,
        ffmpeg::codec::threading::Type::Frame,
    );
    unsafe {
        let device_ref = ffmpeg::ffi::av_buffer_ref(hardware.device_ctx);
        if device_ref.is_null() {
            return Err(ScreenRecorderError::Export(
                "failed to retain source video decoder device".into(),
            ));
        }
        let codec_ctx = context.as_mut_ptr();
        (*codec_ctx).get_format = Some(select_hardware_decoder_pixel_format);
        (*codec_ctx).opaque =
            hardware._selection.as_mut() as *mut HardwareDecodeSelection as *mut c_void;
        (*codec_ctx).hw_device_ctx = device_ref;
    }
    Ok(SourceVideoDecoder {
        decoder: context,
        hardware: Some(hardware),
    })
}

pub(crate) unsafe extern "C" fn select_hardware_decoder_pixel_format(
    codec_ctx: *mut ffmpeg::ffi::AVCodecContext,
    pixel_formats: *const ffmpeg::ffi::AVPixelFormat,
) -> ffmpeg::ffi::AVPixelFormat {
    if codec_ctx.is_null() || pixel_formats.is_null() {
        return ffmpeg::ffi::AVPixelFormat::AV_PIX_FMT_NONE;
    }

    let selection = unsafe { (*codec_ctx).opaque as *const HardwareDecodeSelection };
    if !selection.is_null() {
        let wanted = unsafe { (*selection).hw_pix_fmt };
        let mut current = pixel_formats;
        loop {
            let pixel_format = unsafe { *current };
            if pixel_format == ffmpeg::ffi::AVPixelFormat::AV_PIX_FMT_NONE {
                break;
            }
            if pixel_format == wanted {
                return wanted;
            }
            current = unsafe { current.add(1) };
        }
    }

    unsafe { ffmpeg::ffi::avcodec_default_get_format(codec_ctx, pixel_formats) }
}

pub(crate) fn preferred_hardware_decode_device_types()
-> &'static [(ffmpeg::ffi::AVHWDeviceType, &'static str)] {
    #[cfg(target_os = "windows")]
    {
        &[
            (
                ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_D3D11VA,
                "d3d11va",
            ),
            (ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_DXVA2, "dxva2"),
            (ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_QSV, "qsv"),
        ]
    }
    #[cfg(target_os = "macos")]
    {
        &[(
            ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
            "videotoolbox",
        )]
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        &[
            (ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_VAAPI, "vaapi"),
            (ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_QSV, "qsv"),
            (ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_CUDA, "cuda"),
        ]
    }
    #[cfg(not(any(target_os = "windows", target_os = "macos", unix)))]
    {
        &[]
    }
}

pub(crate) fn find_hardware_decoder_pixel_format(
    codec: ffmpeg::Codec,
    device_type: ffmpeg::ffi::AVHWDeviceType,
) -> Option<ffmpeg::ffi::AVPixelFormat> {
    let mut index = 0;
    loop {
        let config = unsafe { ffmpeg::ffi::avcodec_get_hw_config(codec.as_ptr(), index) };
        if config.is_null() {
            return None;
        }

        let config_ref = unsafe { &*config };
        let supports_device_ctx =
            (config_ref.methods & ffmpeg::ffi::AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX as i32) != 0;
        if supports_device_ctx && config_ref.device_type == device_type {
            return Some(config_ref.pix_fmt);
        }

        index += 1;
    }
}

pub(crate) fn try_open_hardware_video_decoder(
    parameters: &ffmpeg::codec::Parameters,
    perf_config: &ExportPerformanceConfig,
) -> Result<Option<SourceVideoDecoder>> {
    if !hardware_video_decode_allowed(perf_config.mode) {
        return Ok(None);
    }

    let Some(codec) = ffmpeg::codec::decoder::find(parameters.id()) else {
        return Ok(None);
    };

    for (device_type, device_name) in preferred_hardware_decode_device_types() {
        let Some(hw_pix_fmt) = find_hardware_decoder_pixel_format(codec, *device_type) else {
            continue;
        };

        let mut hardware = HardwareDecodeState {
            device_ctx: ptr::null_mut(),
            _selection: Box::new(HardwareDecodeSelection { hw_pix_fmt }),
            hw_pixel_format: ffmpeg::format::Pixel::from(hw_pix_fmt),
            device_name,
        };
        let create_status = unsafe {
            ffmpeg::ffi::av_hwdevice_ctx_create(
                &mut hardware.device_ctx,
                *device_type,
                ptr::null(),
                ptr::null_mut(),
                0,
            )
        };
        if create_status < 0 || hardware.device_ctx.is_null() {
            continue;
        }

        let SourceVideoDecoder {
            decoder: decode_context,
            hardware,
        } = prepare_hardware_decoder_context(hardware, perf_config, || {
            ffmpeg::codec::context::Context::from_parameters(parameters.clone())
        })?;

        let decoder = match decode_context
            .decoder()
            .open_as(codec)
            .and_then(|opened| opened.video())
        {
            Ok(decoder) => decoder,
            Err(_) => continue,
        };

        return Ok(Some(SourceVideoDecoder { decoder, hardware }));
    }

    Ok(None)
}

pub(crate) fn open_source_video_decoder(
    parameters: &ffmpeg::codec::Parameters,
    perf_config: &ExportPerformanceConfig,
    allow_hardware_decode: bool,
) -> Result<SourceVideoDecoder> {
    if allow_hardware_decode
        && let Some(decoder) = try_open_hardware_video_decoder(parameters, perf_config)?
    {
        return Ok(decoder);
    }

    let mut decode_context = ffmpeg::codec::context::Context::from_parameters(parameters.clone())
        .map_err(|err| {
        ScreenRecorderError::Export(format!(
            "failed to create source video decoder context: {err}"
        ))
    })?;
    configure_codec_threads(
        &mut decode_context,
        perf_config.decode_threads,
        ffmpeg::codec::threading::Type::Frame,
    );
    let decoder = decode_context.decoder().video().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to open source video decoder: {err}"))
    })?;
    Ok(SourceVideoDecoder {
        decoder,
        hardware: None,
    })
}

pub(crate) fn decoder_software_output_format(
    decoder: &ffmpeg::decoder::Video,
    hw_state: Option<&HardwareDecodeState>,
) -> ffmpeg::format::Pixel {
    if hw_state.is_some() {
        let sw_format = unsafe { ffmpeg::format::Pixel::from((*decoder.as_ptr()).sw_pix_fmt) };
        if sw_format != ffmpeg::format::Pixel::None {
            return sw_format;
        }
    }

    decoder.format()
}

pub(crate) fn normalize_decoded_video_frame<'a>(
    decoded: &'a mut ffmpeg::frame::Video,
    transferred: &'a mut ffmpeg::frame::Video,
    hw_state: Option<&HardwareDecodeState>,
) -> Result<&'a mut ffmpeg::frame::Video> {
    let Some(hw_state) = hw_state else {
        return Ok(decoded);
    };
    if decoded.format() != hw_state.hw_pixel_format {
        return Ok(decoded);
    }

    unsafe {
        ffmpeg::ffi::av_frame_unref(transferred.as_mut_ptr());
        let transfer_status =
            ffmpeg::ffi::av_hwframe_transfer_data(transferred.as_mut_ptr(), decoded.as_ptr(), 0);
        if transfer_status < 0 {
            return Err(ScreenRecorderError::Export(format!(
                "failed to transfer hardware-decoded video frame to system memory: {}",
                ffmpeg::Error::from(transfer_status)
            )));
        }

        let copy_props_status =
            ffmpeg::ffi::av_frame_copy_props(transferred.as_mut_ptr(), decoded.as_ptr());
        if copy_props_status < 0 {
            return Err(ScreenRecorderError::Export(format!(
                "failed to copy hardware-decoded video frame properties: {}",
                ffmpeg::Error::from(copy_props_status)
            )));
        }
    }

    Ok(transferred)
}

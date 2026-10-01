//! Shared codec policy and FFmpeg transport for live and replayed recordings.
use crate::config::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, VideoCodec, VideoEncodeConfig,
    VideoEncodingSpeed,
};
use crate::error::{RecordingExportError as ScreenRecorderError, Result};
use crate::ffmpeg_util::is_eagain;
use crate::video_quality::quality_to_h264_crf;
use ffmpeg_next as ffmpeg;
use std::path::Path;

pub(crate) fn choose_video_codec_id(
    format: ExportFormat,
    video_codec: VideoCodec,
) -> ffmpeg::codec::Id {
    match format {
        ExportFormat::Mp4 => match video_codec {
            VideoCodec::H264 => ffmpeg::codec::Id::H264,
            VideoCodec::H265 => ffmpeg::codec::Id::HEVC,
        },
        ExportFormat::Avi => ffmpeg::codec::Id::MPEG4,
        ExportFormat::Gif => ffmpeg::codec::Id::GIF,
        ExportFormat::Apng => ffmpeg::codec::Id::APNG,
        // FFmpeg 9's animated libwebp encoder advertises AV_CODEC_ID_WEBP;
        // AV_CODEC_ID_WEBP_ANIM is used only by the native decoder.
        ExportFormat::Webp => ffmpeg::codec::Id::WEBP,
    }
}

pub(crate) fn choose_video_pixel_format(
    format: ExportFormat,
    codec: ffmpeg::codec::Video,
    source_hint: Option<ffmpeg::format::Pixel>,
    mode: ExportExecutionMode,
) -> ffmpeg::format::Pixel {
    let preferred = match format {
        // GIF must use the adaptive palette converter even for RGB8 sources.
        ExportFormat::Gif => return ffmpeg::format::Pixel::PAL8,
        ExportFormat::Apng => [
            ffmpeg::format::Pixel::RGBA,
            ffmpeg::format::Pixel::RGB24,
            ffmpeg::format::Pixel::BGRA,
        ]
        .as_slice(),
        ExportFormat::Webp => [
            ffmpeg::format::Pixel::YUVA420P,
            ffmpeg::format::Pixel::YUV420P,
            ffmpeg::format::Pixel::RGBA,
        ]
        .as_slice(),
        ExportFormat::Mp4 | ExportFormat::Avi
            if matches!(mode, ExportExecutionMode::SoftwareOnly) =>
        {
            [
                ffmpeg::format::Pixel::YUV420P,
                ffmpeg::format::Pixel::NV12,
                ffmpeg::format::Pixel::YUV422P,
                ffmpeg::format::Pixel::RGB24,
            ]
            .as_slice()
        }
        ExportFormat::Mp4 | ExportFormat::Avi => [
            ffmpeg::format::Pixel::NV12,
            ffmpeg::format::Pixel::YUV420P,
            ffmpeg::format::Pixel::YUV422P,
            ffmpeg::format::Pixel::RGB24,
        ]
        .as_slice(),
    };

    if let Some(formats) = codec.formats() {
        let available: Vec<_> = formats.collect();
        if let Some(source_pixel) = source_hint
            && available.contains(&source_pixel)
        {
            return source_pixel;
        }
        for pixel in preferred {
            if available.contains(pixel) {
                return *pixel;
            }
        }
        if let Some(first) = available.first().copied() {
            return first;
        }
    }

    source_hint.unwrap_or(match format {
        ExportFormat::Gif => ffmpeg::format::Pixel::PAL8,
        ExportFormat::Apng => ffmpeg::format::Pixel::RGBA,
        ExportFormat::Webp => ffmpeg::format::Pixel::YUVA420P,
        ExportFormat::Mp4 | ExportFormat::Avi => ffmpeg::format::Pixel::YUV420P,
    })
}

pub(crate) fn choose_audio_codec(format: ExportFormat) -> Option<ffmpeg::Codec> {
    match format {
        ExportFormat::Mp4 => ffmpeg::encoder::find(ffmpeg::codec::Id::AAC),
        ExportFormat::Avi => ffmpeg::encoder::find_by_name("libmp3lame")
            .or_else(|| ffmpeg::encoder::find_by_name("libshine"))
            .or_else(|| ffmpeg::encoder::find(ffmpeg::codec::Id::MP3))
            .or_else(|| ffmpeg::encoder::find(ffmpeg::codec::Id::AAC)),
        ExportFormat::Gif | ExportFormat::Apng | ExportFormat::Webp => None,
    }
}

pub(crate) fn choose_audio_sample_rate(codec: ffmpeg::codec::Audio, requested_hz: u32) -> u32 {
    if let Some(rates) = codec.rates() {
        let available: Vec<u32> = rates.map(|rate| rate.max(1) as u32).collect();
        if available.is_empty() {
            return requested_hz.max(1);
        }
        if available.contains(&requested_hz) {
            return requested_hz;
        }
        return available
            .into_iter()
            .min_by_key(|rate| rate.abs_diff(requested_hz))
            .unwrap_or(requested_hz.max(1));
    }
    requested_hz.max(1)
}

pub(crate) fn choose_audio_channel_layout(
    codec: ffmpeg::codec::Audio,
    requested_channels: u16,
) -> ffmpeg::ChannelLayout {
    let requested_layout = ffmpeg::ChannelLayout::default(i32::from(requested_channels.max(1)));
    if let Some(layouts) = codec.channel_layouts() {
        let available: Vec<_> = layouts.collect();
        if available.contains(&requested_layout) {
            return requested_layout;
        }
        if requested_channels >= 2 && available.contains(&ffmpeg::ChannelLayout::STEREO) {
            return ffmpeg::ChannelLayout::STEREO;
        }
        if available.contains(&ffmpeg::ChannelLayout::MONO) {
            return ffmpeg::ChannelLayout::MONO;
        }
        if let Some(first) = available.first().copied() {
            return first;
        }
    }
    requested_layout
}

pub(crate) fn effective_audio_bitrate_kbps(requested_kbps: u16) -> u16 {
    requested_kbps.clamp(128, 192)
}

pub(crate) fn choose_audio_sample_format(codec: ffmpeg::codec::Audio) -> ffmpeg::format::Sample {
    let preferred = [
        ffmpeg::format::Sample::F32(ffmpeg::format::sample::Type::Planar),
        ffmpeg::format::Sample::F32(ffmpeg::format::sample::Type::Packed),
        ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Planar),
        ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Packed),
    ];

    if let Some(formats) = codec.formats() {
        let available: Vec<_> = formats.collect();
        for candidate in preferred {
            if available.contains(&candidate) {
                return candidate;
            }
        }
        if let Some(first) = available.first().copied() {
            return first;
        }
    }

    ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Packed)
}

pub(crate) fn open_audio_encoder(
    audio_encoder: ffmpeg::codec::encoder::audio::Audio,
    audio_codec: ffmpeg::Codec,
) -> Result<ffmpeg::encoder::audio::Encoder> {
    if audio_codec.name().eq_ignore_ascii_case("aac") {
        return audio_encoder
            .open_as_with(audio_codec, {
                let mut options = ffmpeg::Dictionary::new();
                options.set("profile", "aac_low");
                options
            })
            .map_err(|err| {
                ScreenRecorderError::Export(format!("failed to open audio encoder: {err}"))
            });
    }

    audio_encoder
        .open_as(audio_codec)
        .map_err(|err| ScreenRecorderError::Export(format!("failed to open audio encoder: {err}")))
}

pub(crate) fn select_video_codec(
    output: &ffmpeg::format::context::Output,
    output_path: &Path,
    format: ExportFormat,
    requested_codec: VideoCodec,
    prefer_hardware_h264: bool,
    mode: ExportExecutionMode,
    software_h264_priority: SoftwareH264Priority,
) -> Result<ffmpeg::Codec> {
    if matches!(format, ExportFormat::Webp) {
        return ffmpeg::encoder::find_by_name("libwebp_anim")
            .or_else(|| ffmpeg::encoder::find(ffmpeg::codec::Id::WEBP))
            .ok_or_else(|| {
                ScreenRecorderError::Export(
                    "no animated WebP encoder is available; FFmpeg must include libwebp"
                        .to_string(),
                )
            });
    }

    if matches!(format, ExportFormat::Mp4) {
        if prefer_hardware_h264
            && hardware_video_encode_allowed(mode)
            && let Some(hardware_codec) = select_hardware_codec(requested_codec)
        {
            return Ok(hardware_codec);
        }
        let (encoder_name, codec_name) = exact_mp4_encoder(requested_codec);
        if matches!(mode, ExportExecutionMode::HardwareOnly) {
            return Err(ScreenRecorderError::Export(format!(
                "HardwareOnly mode cannot satisfy the requested {codec_name} export; SnowShot requires {encoder_name}"
            )));
        }
        return ffmpeg::encoder::find_by_name(encoder_name).ok_or_else(|| {
            ScreenRecorderError::Export(format!(
                "requested {codec_name} encoder {encoder_name} is unavailable; rebuild FFmpeg with {encoder_name} support"
            ))
        });
    }

    let container_video_codec = output
        .format()
        .codec(output_path, ffmpeg::media::Type::Video);
    let preferred_video_codec = choose_video_codec_id(format, requested_codec);
    let codec = ffmpeg::encoder::find(preferred_video_codec)
        .or_else(|| ffmpeg::encoder::find(container_video_codec))
        .ok_or_else(|| {
            ScreenRecorderError::Export(format!(
                "no video encoder available for {format:?} (preferred={preferred_video_codec:?}, container={container_video_codec:?})"
            ))
        })?;

    if matches!(mode, ExportExecutionMode::SoftwareOnly) && is_hardware_video_encoder(&codec) {
        if let Some(software_codec) = select_software_h264_codec(software_h264_priority) {
            return Ok(software_codec);
        }
        return Err(ScreenRecorderError::Export(
            "SoftwareOnly mode requested, but no software H.264 encoder is available".to_string(),
        ));
    }

    if matches!(mode, ExportExecutionMode::HardwareOnly)
        && matches!(format, ExportFormat::Mp4)
        && !is_hardware_video_encoder(&codec)
    {
        return Err(ScreenRecorderError::Export(
            "HardwareOnly mode requested, but selected codec is not hardware accelerated"
                .to_string(),
        ));
    }

    Ok(codec)
}

pub(crate) fn exact_mp4_encoder(codec: VideoCodec) -> (&'static str, &'static str) {
    match codec {
        VideoCodec::H264 => ("libx264", "H.264"),
        VideoCodec::H265 => ("libx265", "H.265"),
    }
}

pub(crate) fn effective_video_config(video_config: &VideoEncodeConfig) -> VideoEncodeConfig {
    *video_config
}

pub(crate) fn configure_codec_threads(
    context: &mut ffmpeg::codec::context::Context,
    configured_threads: u8,
    kind: ffmpeg::codec::threading::Type,
) {
    let count = match configured_threads {
        0 => auto_thread_count_from_physical_cores(),
        value => usize::from(value),
    };

    let threading = ffmpeg::codec::threading::Config {
        kind,
        count: count.max(1),
    };
    context.set_threading(threading);
}

pub(crate) fn should_use_x264_options(codec: &ffmpeg::Codec) -> bool {
    codec.name().eq_ignore_ascii_case("libx264")
}

pub(crate) fn should_use_x265_options(codec: &ffmpeg::Codec) -> bool {
    codec.name().eq_ignore_ascii_case("libx265")
}

pub(crate) fn is_hardware_h264_encoder(codec: &ffmpeg::Codec) -> bool {
    let name = codec.name().to_ascii_lowercase();
    name.contains("nvenc")
        || name.contains("qsv")
        || name.contains("amf")
        || name.contains("mf")
        || name.ends_with("_videotoolbox")
}

/// Report the successfully opened mode, including MF's explicit hardware option.
pub(crate) fn opened_video_encoder_uses_hardware(
    encoder: &ffmpeg::encoder::video::Encoder,
) -> bool {
    let Some(codec) = encoder.codec() else {
        return false;
    };
    let name = codec.name();
    if name.ends_with("_mf") {
        let mut enabled = 0i64;
        // SAFETY: the opened encoder owns its live private options for this call.
        unsafe {
            let private = (*encoder.as_ptr()).priv_data;
            return !private.is_null()
                && ffmpeg::ffi::av_opt_get_int(private, c"hw_encoding".as_ptr(), 0, &mut enabled)
                    >= 0
                && enabled == 1;
        }
    }
    is_hardware_h264_encoder(&codec)
}

pub(crate) fn is_hardware_video_encoder(codec: &ffmpeg::Codec) -> bool {
    is_hardware_h264_encoder(codec)
}

pub(crate) fn select_software_h264_codec(priority: SoftwareH264Priority) -> Option<ffmpeg::Codec> {
    let preferred = match priority {
        SoftwareH264Priority::OpenH264First => ["libopenh264", "libx264"],
        SoftwareH264Priority::X264First => ["libx264", "libopenh264"],
    };
    preferred
        .into_iter()
        .find_map(ffmpeg::encoder::find_by_name)
}

pub(crate) fn select_hardware_codec(codec: VideoCodec) -> Option<ffmpeg::Codec> {
    #[cfg(target_os = "macos")]
    {
        ffmpeg::encoder::find_by_name(match codec {
            VideoCodec::H264 => "h264_videotoolbox",
            VideoCodec::H265 => "hevc_videotoolbox",
        })
    }
    #[cfg(not(target_os = "macos"))]
    {
        match codec {
            VideoCodec::H264 => select_hardware_h264_codec(),
            VideoCodec::H265 => None,
        }
    }
}

#[cfg(not(target_os = "macos"))]
pub(crate) fn select_hardware_h264_codec() -> Option<ffmpeg::Codec> {
    // Preserve the CPU-input callers' Media Foundation preference. Direct GPU
    // recording separately selects the encoder matching its D3D11 adapter.
    ["h264_mf", "h264_nvenc", "h264_qsv", "h264_amf"]
        .into_iter()
        .find_map(ffmpeg::encoder::find_by_name)
}

pub(crate) fn open_video_encoder(
    video_encoder: ffmpeg::codec::encoder::video::Video,
    codec: &ffmpeg::Codec,
    video_config: &VideoEncodeConfig,
) -> Result<ffmpeg::encoder::video::Encoder> {
    // Recording exports while capture runs, but is not a network livestream.
    // Let the selected preset use B-frames, lookahead and frame workers; stop
    // drains that finite codec tail instead of re-encoding the whole recording.
    if should_use_x264_options(codec) {
        let mut options = ffmpeg::Dictionary::new();
        options.set("preset", x264_preset_for(video_config.speed));
        options.set(
            "crf",
            &quality_to_h264_crf(video_config.quality).to_string(),
        );
        return video_encoder.open_as_with(*codec, options).map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to open video encoder with h264 options: {err}"
            ))
        });
    }
    if should_use_x265_options(codec) {
        let mut options = ffmpeg::Dictionary::new();
        options.set("preset", video_config.speed.as_x264_preset());
        options.set(
            "crf",
            &quality_to_h264_crf(video_config.quality).to_string(),
        );
        return video_encoder.open_as_with(*codec, options).map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to open video encoder with h265 options: {err}"
            ))
        });
    }
    let mut options = ffmpeg::Dictionary::new();
    if apply_hardware_encoder_options(&mut options, codec, video_config.quality) {
        return video_encoder.open_as_with(*codec, options).map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to open hardware video encoder with recording options: {err}"
            ))
        });
    }
    video_encoder
        .open_as(*codec)
        .map_err(|err| ScreenRecorderError::Export(format!("failed to open video encoder: {err}")))
}

pub(crate) fn apply_hardware_encoder_options(
    options: &mut ffmpeg::Dictionary<'_>,
    codec: &ffmpeg::Codec,
    quality: u8,
) -> bool {
    let name = codec.name().to_ascii_lowercase();
    let qp = quality_to_h264_crf(quality).to_string();
    // Preserve each live encoder's speed policy while passing the requested
    // quality. Hardware queues remain bounded and non-reordering for recovery.
    if name.ends_with("_videotoolbox") {
        options.set("allow_sw", "0");
        options.set("require_sw", "0");
        options.set("realtime", "1");
        options.set("bf", "0");
        return true;
    }
    if name.contains("nvenc") {
        options.set("preset", "p1");
        options.set("tune", "ull");
        options.set("rc", "constqp");
        options.set("qp", &qp);
        options.set("bf", "0");
        options.set("delay", "0");
        return true;
    }
    if name.contains("qsv") {
        options.set("preset", "veryfast");
        options.set("global_quality", &qp);
        options.set("look_ahead", "0");
        options.set("async_depth", "1");
        options.set("bf", "0");
        return true;
    }
    if name.contains("amf") {
        options.set("usage", "ultralowlatency");
        options.set("quality", "speed");
        options.set("rc", "cqp");
        options.set("qp_i", &qp);
        options.set("qp_p", &qp);
        options.set("bf", "0");
        return true;
    }
    if name.contains("mf") {
        // FFmpeg Media Foundation defaults to a software MFT unless requested.
        options.set("hw_encoding", "1");
        options.set("bf", "0");
        options.set("g", "60");
        return true;
    }
    false
}

pub(crate) fn x264_preset_for(speed: VideoEncodingSpeed) -> &'static str {
    speed.as_x264_preset()
}

pub(crate) fn drain_audio_packets_with_callback<F>(
    encoder: &mut ffmpeg::encoder::audio::Encoder,
    draining: bool,
    mut on_packet: F,
) -> Result<()>
where
    F: FnMut(ffmpeg::Packet) -> Result<()>,
{
    loop {
        let mut packet = ffmpeg::Packet::empty();
        match encoder.receive_packet(&mut packet) {
            Ok(()) => on_packet(packet)?,
            Err(ffmpeg::Error::Eof) => break,
            Err(err) if is_eagain(&err) && !draining => break,
            Err(err) if is_eagain(&err) && draining => continue,
            Err(err) => {
                return Err(ScreenRecorderError::Export(format!(
                    "failed to receive encoded audio packet: {err}"
                )));
            }
        }
    }
    Ok(())
}

pub(crate) fn ensure_video_frame_writable(frame: &mut ffmpeg::frame::Video) -> Result<()> {
    let status = unsafe { ffmpeg::ffi::av_frame_make_writable(frame.as_mut_ptr()) };
    if status < 0 {
        return Err(ScreenRecorderError::Export(format!(
            "failed to make video frame writable: {}",
            ffmpeg::Error::from(status)
        )));
    }
    Ok(())
}

#[inline]
pub(crate) fn hardware_video_encode_allowed(mode: ExportExecutionMode) -> bool {
    matches!(
        mode,
        ExportExecutionMode::HardwarePreferred | ExportExecutionMode::HardwareOnly
    )
}

#[inline]
pub(crate) fn auto_thread_count_from_physical_cores() -> usize {
    let logical = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1)
        .max(1);
    let physical = num_cpus::get_physical();
    if physical == 0 {
        logical
    } else {
        physical.min(logical).max(1)
    }
}

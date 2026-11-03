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
        // Standalone FFmpeg GIF encoding accepts palette indices; the streaming
        // backend instead encodes RGB24 directly without an FFmpeg codec.
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
    // Honor speech/mono budgets instead of silently promoting them to stereo rates.
    requested_kbps.clamp(8, 192)
}

/// Audio-track order, independent of video stream indices. Retain the target
/// because a file's measured average can be dominated by silence.
pub(crate) const AUDIO_BITRATES_METADATA: &str = "snow_audio_bitrates_kbps";

pub(crate) fn write_audio_bitrate_metadata(
    output: &mut ffmpeg::format::context::Output,
    configs: &[crate::StreamingAudioConfig],
) {
    let rates = configs
        .iter()
        .map(|track| effective_audio_bitrate_kbps(track.bitrate_kbps).to_string())
        .collect::<Vec<_>>()
        .join(",");
    let mut metadata = output.metadata().to_owned();
    metadata.set(AUDIO_BITRATES_METADATA, &rates);
    output.set_metadata(metadata);
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
                "no hardware encoder is available for the requested {codec_name} export in HardwareOnly mode"
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

pub(crate) fn is_hardware_video_encoder(codec: &ffmpeg::Codec) -> bool {
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
    is_hardware_video_encoder(&codec)
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
    hardware_codec_candidates(codec).into_iter().next()
}

/// CPU input can try each vendor before Media Foundation. Registration alone
/// does not establish that the machine has the hardware required to open it.
pub(crate) fn hardware_codec_candidates(codec: VideoCodec) -> Vec<ffmpeg::Codec> {
    #[cfg(target_os = "macos")]
    {
        ffmpeg::encoder::find_by_name(match codec {
            VideoCodec::H264 => "h264_videotoolbox",
            VideoCodec::H265 => "hevc_videotoolbox",
        })
        .into_iter()
        .collect()
    }
    #[cfg(not(target_os = "macos"))]
    {
        match codec {
            VideoCodec::H264 => ["h264_nvenc", "h264_amf", "h264_qsv", "h264_mf"]
                .into_iter()
                .filter_map(ffmpeg::encoder::find_by_name)
                .collect(),
            VideoCodec::H265 => ["hevc_nvenc", "hevc_amf", "hevc_qsv", "hevc_mf"]
                .into_iter()
                .filter_map(ffmpeg::encoder::find_by_name)
                .collect(),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum VideoEncoderPurpose {
    LiveOutput,
    OfflineOutput,
    RecordingSource,
}

/// Optional offline features (notably HEVC B-frames on older NVENC GPUs)
/// must not turn a usable hardware encoder into a software fallback. Retry once
/// with the bounded live profile before moving to the next device or software.
pub(crate) fn open_with_hardware_compatibility<T>(
    purpose: VideoEncoderPurpose,
    hardware: bool,
    mut open: impl FnMut(VideoEncoderPurpose) -> Result<T>,
) -> Result<T> {
    match open(purpose) {
        Err(primary) if hardware && purpose == VideoEncoderPurpose::OfflineOutput => {
            open(VideoEncoderPurpose::LiveOutput).map_err(|compatible| {
                ScreenRecorderError::Export(format!("{primary}; compatibility retry: {compatible}"))
            })
        }
        result => result,
    }
}

pub(crate) fn open_video_encoder(
    mut video_encoder: ffmpeg::codec::encoder::video::Video,
    codec: &ffmpeg::Codec,
    video_config: &VideoEncodeConfig,
    purpose: VideoEncoderPurpose,
    fps: u32,
    hardware_surface_capacity: Option<usize>,
) -> Result<ffmpeg::encoder::video::Encoder> {
    // Recording exports while capture runs, but is not a network livestream.
    // Let the selected preset use B-frames, lookahead and frame workers; stop
    // drains that finite codec tail instead of re-encoding the whole recording.
    let source = purpose == VideoEncoderPurpose::RecordingSource;
    let offline = purpose == VideoEncoderPurpose::OfflineOutput;
    let mut options = ffmpeg::Dictionary::new();
    if source || is_hardware_video_encoder(codec) {
        // Keep seeking bounded without FFmpeg's short default hardware GOP.
        let gop = fps
            .saturating_mul(if source { 1 } else { 2 })
            .min(i32::MAX as u32);
        video_encoder.set_gop(gop);
        if !offline {
            video_encoder.set_max_b_frames(0);
        }
    }
    if should_use_x264_options(codec) || should_use_x265_options(codec) {
        options.set("preset", x264_preset_for(video_config.speed));
        options.set(
            "crf",
            &quality_to_h264_crf(video_config.quality).to_string(),
        );
        if source {
            options.set("tune", "zerolatency");
            if should_use_x264_options(codec) {
                options.set("rc-lookahead", "0");
            } else {
                options.set("x265-params", "bframes=0:rc-lookahead=0");
            }
        }
    } else if apply_hardware_encoder_options(
        &mut options,
        codec.name(),
        video_config.quality,
        purpose,
    ) {
        if offline && codec.name().contains("nvenc") {
            // CQ VBR must not inherit the resolution-based average bitrate target.
            video_encoder.set_bit_rate(0);
        }
        if codec.name().ends_with("_videotoolbox") && cfg!(target_arch = "aarch64") {
            // Apple Silicon accepts normalized quality; zero disables that mode.
            // Intel VideoToolbox retains the explicit bitrate fallback.
            unsafe {
                (*video_encoder.as_mut_ptr()).global_quality =
                    i32::from(video_config.quality.clamp(1, 100));
            }
        }
        if let Some(capacity) = hardware_surface_capacity {
            if codec.name().contains("nvenc") {
                options.set("surfaces", &capacity.to_string());
            } else if codec.name().contains("amf") {
                options.set("query_timeout", "100");
            }
        }
    }
    video_encoder.open_as_with(*codec, options).map_err(|err| {
        ScreenRecorderError::Export(format!(
            "failed to open video encoder {}: {err}",
            codec.name()
        ))
    })
}

pub(crate) fn apply_hardware_encoder_options(
    options: &mut ffmpeg::Dictionary<'_>,
    name: &str,
    quality: u8,
    purpose: VideoEncoderPurpose,
) -> bool {
    let source = purpose == VideoEncoderPurpose::RecordingSource;
    let offline = purpose == VideoEncoderPurpose::OfflineOutput;
    let qp = quality_to_h264_crf(quality).to_string();
    // Offline CPU uploads can buffer and reorder. Live surfaces and recovery
    // retain their bounded, non-reordering policy.
    if name.ends_with("_videotoolbox") {
        options.set("allow_sw", "0");
        options.set("require_sw", "0");
        options.set("realtime", if offline { "0" } else { "1" });
        options.set("bf", if offline { "2" } else { "0" });
        return true;
    }
    if name.contains("nvenc") {
        if offline {
            options.set("preset", "p5");
            options.set("tune", "hq");
            options.set("rc", "vbr");
            options.set("cq", &qp);
            options.set("rc-lookahead", "16");
            options.set("bf", "3");
            return true;
        }
        options.set("preset", if source { "p1" } else { "p4" });
        options.set("tune", "ull");
        options.set("rc", "constqp");
        options.set("qp", &qp);
        options.set("rc-lookahead", "0");
        options.set("bf", "0");
        options.set("delay", "0");
        return true;
    }
    if name.contains("qsv") {
        options.set(
            "preset",
            if source {
                "veryfast"
            } else if offline {
                "slow"
            } else {
                "medium"
            },
        );
        options.set("global_quality", &qp);
        // HEVC QSV has no look_ahead option. Its look_ahead_depth applies to
        // extended VBR, not the ICQ quality mode used here. Keep ICQ on HEVC;
        // H.264 can use LA_ICQ for offline compression.
        if name == "h264_qsv" {
            options.set("look_ahead", if offline { "1" } else { "0" });
            if offline {
                options.set("look_ahead_depth", "16");
            }
        }
        options.set("async_depth", if offline { "4" } else { "1" });
        options.set("bf", if offline { "3" } else { "0" });
        return true;
    }
    if name.contains("amf") {
        options.set(
            "usage",
            if offline {
                "transcoding"
            } else {
                "ultralowlatency"
            },
        );
        options.set(
            "quality",
            if source {
                "speed"
            } else if offline {
                "quality"
            } else {
                "balanced"
            },
        );
        options.set("rc", "cqp");
        options.set("qp_i", &qp);
        options.set("qp_p", &qp);
        // CQP keeps the existing quality scale; preencode is for bitrate control.
        options.set("preanalysis", if offline { "1" } else { "0" });
        options.set("preencode", "0");
        // FFmpeg's AMF reorder-delay initialization requires a successor PTS.
        // Coalesced exports can contain one picture, so retain preanalysis
        // without either fixed or adaptive B-frames.
        options.set("bf", "0");
        if offline && name == "h264_amf" {
            options.set("pa_adaptive_mini_gop", "0");
        }
        return true;
    }
    if name.contains("mf") {
        // FFmpeg Media Foundation defaults to a software MFT unless requested.
        options.set("hw_encoding", "1");
        options.set("rate_control", "quality");
        options.set("quality", &quality.min(100).to_string());
        if offline {
            options.set("scenario", "archive");
        }
        // MF does not reliably negotiate B-frame support across vendor MFTs.
        options.set("bf", "0");
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
    // SAFETY: Codec callers use FFmpeg-allocated frames whose format, dimensions, and
    // plane layout remain consistent with their allocation or scaler output.
    unsafe { crate::ffmpeg_util::ensure_video_frame_writable(frame) }.map_err(|error| match error {
        ScreenRecorderError::Encode(message) => ScreenRecorderError::Export(message),
        other => other,
    })
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn offline_hardware_retries_bounded_options_before_software_fallback() {
        for (hardware, purpose, retries) in [
            (true, VideoEncoderPurpose::OfflineOutput, true),
            (false, VideoEncoderPurpose::OfflineOutput, false),
            (true, VideoEncoderPurpose::LiveOutput, false),
            (true, VideoEncoderPurpose::RecordingSource, false),
        ] {
            let mut attempts = Vec::new();
            let result = open_with_hardware_compatibility(purpose, hardware, |profile| {
                attempts.push(profile);
                if attempts.len() == 1 {
                    Err(ScreenRecorderError::Export("unsupported B-frames".into()))
                } else {
                    Ok(())
                }
            });
            assert_eq!(result.is_ok(), retries);
            assert_eq!(attempts[0], purpose);
            assert_eq!(attempts.len(), if retries { 2 } else { 1 });
            if retries {
                assert_eq!(attempts[1], VideoEncoderPurpose::LiveOutput);
            }
        }
        let mut calls = 0;
        open_with_hardware_compatibility(VideoEncoderPurpose::OfflineOutput, true, |_| {
            calls += 1;
            Ok(())
        })
        .unwrap();
        assert_eq!(calls, 1);
    }

    #[test]
    fn hevc_qsv_keeps_icq_without_h264_only_lookahead_options() {
        for purpose in [
            VideoEncoderPurpose::LiveOutput,
            VideoEncoderPurpose::OfflineOutput,
            VideoEncoderPurpose::RecordingSource,
        ] {
            let mut options = ffmpeg::Dictionary::new();
            assert!(apply_hardware_encoder_options(
                &mut options,
                "hevc_qsv",
                80,
                purpose
            ));
            assert_eq!(options.get("global_quality"), Some("18"));
            assert_eq!(options.get("look_ahead"), None);
            assert_eq!(options.get("look_ahead_depth"), None);
        }
    }

    #[test]
    fn hardware_candidates_never_change_the_requested_codec() {
        crate::ffmpeg_util::ensure_ffmpeg_initialized().unwrap();
        for (requested, id) in [
            (VideoCodec::H264, ffmpeg::codec::Id::H264),
            (VideoCodec::H265, ffmpeg::codec::Id::HEVC),
        ] {
            for codec in hardware_codec_candidates(requested) {
                assert_eq!(codec.id(), id);
                assert!(is_hardware_video_encoder(&codec));
            }
        }
    }

    #[test]
    fn hardware_output_quality_is_explicit_for_every_supported_encoder() {
        for (name, keys) in [
            ("h264_nvenc", &["qp"][..]),
            ("hevc_nvenc", &["qp"][..]),
            ("h264_qsv", &["global_quality"][..]),
            ("hevc_qsv", &["global_quality"][..]),
            ("h264_amf", &["qp_i", "qp_p"][..]),
            ("hevc_amf", &["qp_i", "qp_p"][..]),
            ("h264_mf", &["quality"][..]),
            ("hevc_mf", &["quality"][..]),
        ] {
            let mut lower = ffmpeg::Dictionary::new();
            let mut higher = ffmpeg::Dictionary::new();
            assert!(apply_hardware_encoder_options(
                &mut lower,
                name,
                40,
                VideoEncoderPurpose::LiveOutput
            ));
            assert!(apply_hardware_encoder_options(
                &mut higher,
                name,
                80,
                VideoEncoderPurpose::LiveOutput
            ));
            for key in keys {
                let low: u8 = lower.get(key).unwrap().parse().unwrap();
                let high: u8 = higher.get(key).unwrap().parse().unwrap();
                assert!(
                    if name.ends_with("_mf") {
                        high > low
                    } else {
                        high < low
                    },
                    "{name}: {key}"
                );
            }
            assert_eq!(higher.get("bf"), Some("0"));
        }
        let mut mf = ffmpeg::Dictionary::new();
        apply_hardware_encoder_options(&mut mf, "h264_mf", 80, VideoEncoderPurpose::LiveOutput);
        assert_eq!(mf.get("hw_encoding"), Some("1"));
        assert_eq!(mf.get("rate_control"), Some("quality"));
        assert_eq!(mf.get("quality"), Some("80"));
    }

    #[test]
    fn live_output_and_clean_sources_keep_their_bounded_presets() {
        for (name, key, output, source) in [
            ("h264_nvenc", "preset", "p4", "p1"),
            ("hevc_nvenc", "preset", "p4", "p1"),
            ("h264_amf", "quality", "balanced", "speed"),
            ("hevc_amf", "quality", "balanced", "speed"),
            ("h264_qsv", "preset", "medium", "veryfast"),
            ("hevc_qsv", "preset", "medium", "veryfast"),
        ] {
            for (purpose, expected) in [
                (VideoEncoderPurpose::LiveOutput, output),
                (VideoEncoderPurpose::RecordingSource, source),
            ] {
                let mut options = ffmpeg::Dictionary::new();
                apply_hardware_encoder_options(&mut options, name, 80, purpose);
                assert_eq!(options.get(key), Some(expected));
            }
        }
    }

    #[test]
    fn offline_hardware_options_are_valid_and_keep_quality_control() {
        crate::ffmpeg_util::ensure_ffmpeg_initialized().unwrap();
        for (name, quality_key, b_frames) in [
            ("h264_nvenc", "cq", "3"),
            ("hevc_nvenc", "cq", "3"),
            ("h264_amf", "qp_p", "0"),
            ("hevc_amf", "qp_p", "0"),
            ("h264_qsv", "global_quality", "3"),
            ("hevc_qsv", "global_quality", "3"),
            ("h264_mf", "quality", "0"),
            ("hevc_mf", "quality", "0"),
            ("h264_videotoolbox", "", "2"),
            ("hevc_videotoolbox", "", "2"),
        ] {
            let mut options = ffmpeg::Dictionary::new();
            assert!(apply_hardware_encoder_options(
                &mut options,
                name,
                80,
                VideoEncoderPurpose::OfflineOutput
            ));
            assert_eq!(options.get("bf"), Some(b_frames));
            if name == "h264_amf" {
                assert_eq!(options.get("pa_adaptive_mini_gop"), Some("0"));
                assert_eq!(options.get("preanalysis"), Some("1"));
                assert_eq!(options.get("usage"), Some("transcoding"));
                assert_eq!(options.get("quality"), Some("quality"));
            }
            if !quality_key.is_empty() {
                assert_eq!(
                    options.get(quality_key),
                    Some(if name.ends_with("_mf") { "80" } else { "18" })
                );
            }
            // Parse against the packaged FFmpeg without requiring a physical GPU.
            if let Some(codec) = ffmpeg::encoder::find_by_name(name) {
                let mut context = ffmpeg::codec::context::Context::new_with_codec(codec);
                for (key, value) in options.iter() {
                    let key = std::ffi::CString::new(key).unwrap();
                    let value = std::ffi::CString::new(value).unwrap();
                    let status = unsafe {
                        ffmpeg::ffi::av_opt_set(
                            context.as_mut_ptr().cast(),
                            key.as_ptr(),
                            value.as_ptr(),
                            ffmpeg::ffi::AV_OPT_SEARCH_CHILDREN,
                        )
                    };
                    assert!(
                        status >= 0,
                        "{name}: {key:?}={value:?}: {}",
                        ffmpeg::Error::from(status)
                    );
                }
            }
        }
    }

    #[test]
    #[ignore = "requires an available native AMD AMF H.264 encoder"]
    fn offline_amf_single_picture_drains_without_a_successor() {
        crate::ffmpeg_util::ensure_ffmpeg_initialized().unwrap();
        let codec = ffmpeg::encoder::find_by_name("h264_amf").expect("AMF encoder required");
        let mut context = ffmpeg::codec::context::Context::new_with_codec(codec)
            .encoder()
            .video()
            .unwrap();
        context.set_width(640);
        context.set_height(360);
        context.set_format(ffmpeg::format::Pixel::NV12);
        context.set_time_base((1, 30));
        context.set_frame_rate(Some((30, 1)));
        let mut encoder = open_video_encoder(
            context,
            &codec,
            &VideoEncodeConfig::default(),
            VideoEncoderPurpose::OfflineOutput,
            30,
            None,
        )
        .unwrap();
        let mut picture = ffmpeg::frame::Video::new(encoder.format(), 640, 360);
        picture.data_mut(0).fill(80);
        picture.data_mut(1).fill(128);
        picture.set_pts(Some(0));
        // One coded picture can cover an arbitrarily long static recording.
        // It must drain without an artificial successor or a duplicate sample.
        unsafe { (*picture.as_mut_ptr()).duration = 90 };
        encoder.send_frame(&picture).unwrap();
        let mut packets = Vec::new();
        let mut drain = |encoder: &mut ffmpeg::encoder::video::Encoder, flushing| loop {
            let mut packet = ffmpeg::Packet::empty();
            match encoder.receive_packet(&mut packet) {
                Ok(()) => packets.push(packet),
                Err(ffmpeg::Error::Eof) if flushing => break,
                Err(error) if !flushing && is_eagain(&error) => break,
                Err(error) => panic!("AMF single-picture packet drain failed: {error}"),
            }
        };
        drain(&mut encoder, false);
        encoder
            .send_eof()
            .expect("single-picture AMF EOF must succeed");
        drain(&mut encoder, true);
        assert_eq!(packets.len(), 1, "retain exactly the submitted picture");
        assert_eq!(packets[0].pts(), Some(0));
        assert!(packets[0].is_key());

        let mut decoder = ffmpeg::codec::context::Context::from_parameters(
            ffmpeg::codec::Parameters::from(&encoder),
        )
        .unwrap()
        .decoder()
        .video()
        .unwrap();
        decoder.send_packet(&packets[0]).unwrap();
        decoder.send_eof().unwrap();
        let mut decoded = ffmpeg::frame::Video::empty();
        decoder.receive_frame(&mut decoded).unwrap();
        assert_eq!((decoded.width(), decoded.height()), (640, 360));
        assert_eq!(decoded.pts(), Some(0));
        assert!(matches!(
            decoder.receive_frame(&mut decoded),
            Err(ffmpeg::Error::Eof)
        ));
    }
}

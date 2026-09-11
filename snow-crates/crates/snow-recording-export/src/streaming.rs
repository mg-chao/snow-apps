use std::collections::VecDeque;
use std::fs::{self, OpenOptions};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use ffmpeg_next as ffmpeg;
use snow_recording_model::{VideoCodec, VideoEncodeConfig};
use uuid::Uuid;

mod convert;

use crate::config::{ExportExecutionMode, ExportFormat, SoftwareH264Priority};
use crate::editing::{
    choose_audio_channel_layout, choose_audio_codec, choose_audio_sample_format,
    choose_audio_sample_rate, choose_video_pixel_format, configure_codec_threads,
    drain_audio_packets_with_callback, drain_video_packets_with_durations_timed,
    effective_audio_bitrate_kbps, effective_video_config, ensure_video_frame_writable,
    is_hardware_h264_encoder, open_audio_encoder, open_video_encoder, select_video_codec,
};
use crate::error::{RecordingExportError, Result};
use crate::ffmpeg_util::{copy_rgba_into_frame, ensure_ffmpeg_initialized};
use crate::video_quality::smart_quality_bitrate_bps;
use convert::{ConversionMode, RgbOrder};

pub const DIRECT_STAGING_PREFIX: &str = ".snow-recording-direct-";
const STALE_STAGING_AGE: Duration = Duration::from_secs(24 * 60 * 60);

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StreamingAudioConfig {
    pub sample_rate_hz: u32,
    pub channels: u16,
    pub bitrate_kbps: u16,
}

impl StreamingAudioConfig {
    fn validate(&self) -> std::result::Result<(), String> {
        if self.sample_rate_hz == 0 {
            return Err("streaming audio sample rate must be greater than zero".to_string());
        }
        if self.channels == 0 {
            return Err("streaming audio channel count must be greater than zero".to_string());
        }
        Ok(())
    }
}

/// Byte order of packed RGB pixels supplied to `push_rgba_frame`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum StreamingPixelOrder {
    #[default]
    Rgba,
    Bgra,
}

impl StreamingPixelOrder {
    fn kernel_order(self) -> RgbOrder {
        match self {
            Self::Rgba => RgbOrder::Rgba,
            Self::Bgra => RgbOrder::Bgra,
        }
    }

    fn ffmpeg_input(self) -> ffmpeg::format::Pixel {
        match self {
            Self::Rgba => ffmpeg::format::Pixel::RGBA,
            Self::Bgra => ffmpeg::format::Pixel::BGRA,
        }
    }
}

/// Description of a pre-encoded H.264 track muxed by a streaming encoder.
///
/// `StreamingEncoder` skips in-process video encoding and instead accepts
/// finished packets through `push_encoded_video_packet`, while audio,
/// container handling, staging-file publication, and reports stay
/// unchanged.
#[derive(Clone, Debug)]
pub struct ExternalVideoTrack {
    /// avcC record (SPS/PPS in length-prefixed form) describing the
    /// bitstream. Written into the stream's codec parameters before the
    /// container header; when empty the muxer derives the sample
    /// description from the first keyframe packet.
    pub extradata: Vec<u8>,
    /// Name reported as `video_encoder` in the streaming report.
    pub encoder_name: String,
}

#[derive(Clone, Debug)]
pub struct StreamingEncoderConfig {
    pub output_path: PathBuf,
    pub format: ExportFormat,
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub codec: VideoCodec,
    pub prefer_hardware_h264: bool,
    pub execution_mode: ExportExecutionMode,
    pub software_h264_priority: SoftwareH264Priority,
    pub video: VideoEncodeConfig,
    pub encode_threads: u8,
    pub audio: Option<StreamingAudioConfig>,
    /// Byte order of the pixels passed to `push_rgba_frame`. BGRA avoids the
    /// capture-side channel swizzle for recording streams.
    pub pixel_order: StreamingPixelOrder,
    /// Mux externally encoded H.264 packets instead of encoding raw pixels
    /// in-process. Requires MP4 output and the H.264 codec; packet timing
    /// semantics mirror `push_rgba_frame` (durations from timestamp gaps).
    pub external_video: Option<ExternalVideoTrack>,
}

pub fn scaled_output_dimensions(
    source_width: u32,
    source_height: u32,
    maximum_width: Option<u32>,
    maximum_height: Option<u32>,
    format: ExportFormat,
) -> (u32, u32) {
    crate::editing::output_dimensions(
        source_width,
        source_height,
        maximum_width,
        maximum_height,
        format.requires_even_dimensions(),
    )
}

/// Map a millisecond presentation timestamp onto encoder ticks at `fps`.
/// Shared by the pixel and external-packet push paths so both lanes derive
/// identical durations from timestamp gaps.
fn pts_from_timestamp_ms(timestamp_ms: u64, fps: u32) -> i64 {
    ((u128::from(timestamp_ms) * u128::from(fps) + 500) / 1_000).min(i64::MAX as u128) as i64
}

/// Attach an avcC record to a muxer stream's codec parameters.
///
/// ffmpeg-next exposes no extradata setter, so this writes the bytes into
/// the stream's `AVCodecParameters` directly; the container header then
/// carries the bitstream description. Must run before `write_header`.
///
/// # Safety
///
/// `stream` must belong to a live output context and must not be used
/// concurrently.
unsafe fn attach_stream_extradata(
    stream: &mut ffmpeg::format::stream::StreamMut<'_>,
    extradata: &[u8],
) -> Result<()> {
    use ffmpeg::ffi::{AV_INPUT_BUFFER_PADDING_SIZE, av_free, av_malloc};
    if extradata.is_empty() {
        return Ok(());
    }
    unsafe {
        let avstream = stream.as_mut_ptr();
        let parameters = (*avstream).codecpar;
        if parameters.is_null() {
            return Err(RecordingExportError::Encode(
                "streaming video track has no codec parameters".into(),
            ));
        }
        if !(*parameters).extradata.is_null() {
            av_free((*parameters).extradata.cast());
            (*parameters).extradata = std::ptr::null_mut();
            (*parameters).extradata_size = 0;
        }
        let padding = AV_INPUT_BUFFER_PADDING_SIZE as usize;
        let buffer = av_malloc(extradata.len() + padding).cast::<u8>();
        if buffer.is_null() {
            return Err(RecordingExportError::Encode(
                "failed to allocate external video extradata".into(),
            ));
        }
        std::ptr::copy_nonoverlapping(extradata.as_ptr(), buffer, extradata.len());
        std::ptr::write_bytes(buffer.add(extradata.len()), 0, padding);
        (*parameters).extradata = buffer.cast();
        (*parameters).extradata_size = extradata.len() as i32;
        Ok(())
    }
}

impl StreamingEncoderConfig {
    pub fn validate(&self) -> std::result::Result<(), String> {
        if self.output_path.as_os_str().is_empty() {
            return Err("output_path must not be empty".to_string());
        }
        let extension = self
            .output_path
            .extension()
            .and_then(|value| value.to_str())
            .unwrap_or_default();
        if !extension.eq_ignore_ascii_case(self.format.file_extension()) {
            return Err(format!(
                "streaming output extension must be .{}",
                self.format.file_extension()
            ));
        }
        if self.width == 0 || self.height == 0 {
            return Err("streaming output dimensions must be non-zero".to_string());
        }
        if self.format.requires_even_dimensions()
            && (!self.width.is_multiple_of(2) || !self.height.is_multiple_of(2))
        {
            return Err("selected streaming format requires even dimensions".to_string());
        }
        if self.fps == 0 {
            return Err("streaming output fps must be greater than zero".to_string());
        }
        if let Some(audio) = &self.audio {
            if self.format.is_animated_image() {
                return Err("animated streaming formats do not support audio".into());
            }
            audio.validate()?;
        }
        if self.external_video.is_some()
            && (self.format != ExportFormat::Mp4 || self.codec != VideoCodec::H264)
        {
            return Err("external video packets require MP4 output with the H.264 codec".into());
        }
        self.video.validate("video")?;
        Ok(())
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StreamingEncoderReport {
    #[cfg(feature = "stage-timing")]
    pub video_stage_timings: StreamingVideoStageTimings,
    pub encoded_frames: u64,
    pub coalesced_frames: u64,
    pub video_encoder: String,
    pub used_hardware_video_encoder: bool,
    pub audio_encoder: Option<String>,
    pub encoded_audio_frames: u64,
    pub inserted_silence_frames: u64,
    pub dropped_audio_frames: u64,
}

#[cfg(feature = "stage-timing")]
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StreamingVideoStageTimings {
    pub copy: Duration,
    pub convert: Duration,
    pub send: Duration,
    pub drain_and_mux: Duration,
    pub packet_receive: Duration,
    pub mux_write: Duration,
}

struct StreamingAudioState {
    encoder: ffmpeg::encoder::audio::Encoder,
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
    input_rate: u32,
    input_channels: usize,
    input_layout: ffmpeg::ChannelLayout,
    resampler: Option<ffmpeg::software::resampling::Context>,
    frame_samples: usize,
    variable_frame_size: bool,
    pending_samples: VecDeque<i16>,
    next_input_frame: u64,
    next_encoder_pts: i64,
}

struct PendingExternalPacket {
    pts: i64,
    is_keyframe: bool,
    data: Vec<u8>,
}

pub struct StreamingEncoder {
    output: Option<ffmpeg::format::context::Output>,
    /// In-process video encoder; `None` when the video track muxes
    /// externally encoded packets instead.
    encoder: Option<ffmpeg::encoder::video::Encoder>,
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
    /// Swscale fallback (formats the parallel kernel does not cover).
    scaler: ffmpeg::software::scaling::Context,
    /// Packed RGBA staging frame for the swscale fallback; allocated lazily
    /// so the kernel path never reserves it.
    rgba_frame: Option<ffmpeg::frame::Video>,
    encode_frame: ffmpeg::frame::Video,
    /// Input byte order accepted by `push_rgba_frame`; the kernel converts
    /// both orders natively, the fallback swizzles through the scaler.
    source_order: RgbOrder,
    /// ffmpeg pixel format matching `source_order`; tags the swscale
    /// fallback's staging frame so the scaler input stays consistent.
    source_pixel: ffmpeg::format::Pixel,
    /// Whether `encode_frame` is NV12/YUV420P and converts via the parallel
    /// kernel instead of swscale.
    use_kernel_conversion: bool,
    width: u32,
    height: u32,
    fps: u32,
    /// Presentation timestamp of the frame currently held in `encode_frame`.
    /// Pixels are converted into `encode_frame` at push time, so no caller
    /// buffer needs to be retained between pushes.
    pending_pts: Option<i64>,
    /// Replacement pixels for the pending frame, staged when a push repeats
    /// the pending presentation timestamp. Converting on every repeat would
    /// redo full-frame work for frames the encoder discards anyway; the
    /// staged copy is converted once, when the timestamp finally advances.
    coalesced_pixels: Vec<u8>,
    pending_coalesced: bool,
    pending_packet_durations: VecDeque<i64>,
    /// Whether the video track muxes externally encoded packets instead of
    /// feeding the in-process encoder.
    external: bool,
    /// Externally encoded packet awaiting its duration (the next packet's
    /// presentation-timestamp gap). External-video mode only.
    pending_external: Option<PendingExternalPacket>,
    audio: Option<StreamingAudioState>,
    staging_path: Option<PathBuf>,
    final_path: PathBuf,
    report: StreamingEncoderReport,
    finished: bool,
}

impl StreamingEncoder {
    pub fn create(config: StreamingEncoderConfig) -> Result<Self> {
        config
            .validate()
            .map_err(RecordingExportError::InvalidConfig)?;
        ensure_ffmpeg_initialized()?;

        let output_directory = config
            .output_path
            .parent()
            .filter(|path| !path.as_os_str().is_empty())
            .unwrap_or_else(|| Path::new("."));
        fs::create_dir_all(output_directory)?;
        cleanup_stale_staging_files(output_directory)?;
        let staging_path = create_staging_path(&config.output_path)?;
        match Self::create_at_path(config, staging_path.clone()) {
            Ok(mut encoder) => {
                encoder.staging_path = Some(staging_path);
                Ok(encoder)
            }
            Err(error) => {
                let _ = fs::remove_file(staging_path);
                Err(error)
            }
        }
    }

    fn create_at_path(config: StreamingEncoderConfig, staging_path: PathBuf) -> Result<Self> {
        let mut output = ffmpeg::format::output(&staging_path).map_err(|error| {
            RecordingExportError::Encode(format!(
                "failed to create streaming output context {}: {error}",
                staging_path.display()
            ))
        })?;
        let global_header = output
            .format()
            .flags()
            .contains(ffmpeg::format::Flags::GLOBAL_HEADER);
        let fps = config.fps.min(i32::MAX as u32);
        let video_time_base = ffmpeg::Rational(1, fps as i32);
        let video_frame_rate = ffmpeg::Rational(fps as i32, 1);
        let external_track = config.external_video.clone();
        let effective_video = effective_video_config(&config.video);
        // External-video mode: an unopened context only feeds the stream's
        // codec parameters; packets arrive pre-encoded via
        // `push_encoded_video_packet`.
        let mut external_parameters: Option<ffmpeg::encoder::video::Video> = None;
        let (encoder, video_codec, pixel_format) = if external_track.is_some() {
            let codec = ffmpeg::encoder::find(ffmpeg::codec::Id::H264).ok_or_else(|| {
                RecordingExportError::Encode(
                    "no H.264 codec is registered for external video muxing".into(),
                )
            })?;
            let mut parameters = ffmpeg::codec::context::Context::new_with_codec(codec)
                .encoder()
                .video()
                .map_err(|error| {
                    RecordingExportError::Encode(format!(
                        "failed to create external video stream parameters: {error}"
                    ))
                })?;
            parameters.set_width(config.width);
            parameters.set_height(config.height);
            parameters.set_format(ffmpeg::format::Pixel::NV12);
            parameters.set_time_base(video_time_base);
            parameters.set_frame_rate(Some(video_frame_rate));
            if global_header {
                parameters.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
            }
            external_parameters = Some(parameters);
            (None, codec, ffmpeg::format::Pixel::NV12)
        } else {
            let make_encoder = |codec: ffmpeg::Codec,
                                pixel: ffmpeg::format::Pixel|
             -> Result<ffmpeg::encoder::video::Encoder> {
                let mut encoder = ffmpeg::codec::context::Context::new_with_codec(codec)
                    .encoder()
                    .video()
                    .map_err(|error| {
                        RecordingExportError::Encode(format!(
                            "failed to create streaming video encoder: {error}"
                        ))
                    })?;
                encoder.set_width(config.width);
                encoder.set_height(config.height);
                encoder.set_format(pixel);
                encoder.set_time_base(video_time_base);
                encoder.set_frame_rate(Some(video_frame_rate));
                configure_codec_threads(
                    &mut encoder,
                    config.encode_threads,
                    ffmpeg::codec::threading::Type::Frame,
                );
                if !config.format.is_animated_image() {
                    encoder.set_bit_rate(smart_quality_bitrate_bps(
                        config.width,
                        config.height,
                        config.fps,
                        &effective_video,
                        false,
                    ));
                }
                if global_header {
                    encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
                }
                open_video_encoder(encoder, &codec, &effective_video)
            };
            let mut video_codec = select_video_codec(
                &output,
                &staging_path,
                config.format,
                config.codec,
                config.prefer_hardware_h264,
                config.execution_mode,
                config.software_h264_priority,
            )?;
            let mut pixel_format = choose_video_pixel_format(
                config.format,
                video_codec.video().map_err(|error| {
                    RecordingExportError::Encode(format!(
                        "selected streaming codec is not a video encoder: {error}"
                    ))
                })?,
                None,
                config.execution_mode,
            );
            let encoder = match make_encoder(video_codec, pixel_format) {
                Ok(encoder) => encoder,
                Err(primary_error)
                    if config.prefer_hardware_h264 && is_hardware_h264_encoder(&video_codec) =>
                {
                    video_codec = select_video_codec(
                        &output,
                        &staging_path,
                        config.format,
                        config.codec,
                        false,
                        ExportExecutionMode::SoftwareOnly,
                        config.software_h264_priority,
                    )?;
                    pixel_format = choose_video_pixel_format(
                        config.format,
                        video_codec.video().map_err(|error| {
                            RecordingExportError::Encode(format!(
                                "fallback streaming codec is not a video encoder: {error}"
                            ))
                        })?,
                        None,
                        ExportExecutionMode::SoftwareOnly,
                    );
                    make_encoder(video_codec, pixel_format).map_err(|_| primary_error)?
                }
                Err(error) => return Err(error),
            };
            (Some(encoder), video_codec, pixel_format)
        };

        let stream_index = {
            let mut stream = output.add_stream(video_codec).map_err(|error| {
                RecordingExportError::Encode(format!(
                    "failed to add streaming video track: {error}"
                ))
            })?;
            stream.set_time_base(video_time_base);
            stream.set_rate(video_frame_rate);
            stream.set_avg_frame_rate(video_frame_rate);
            match (encoder.as_ref(), external_parameters.as_ref()) {
                (Some(encoder), _) => stream.set_parameters(encoder),
                (_, Some(parameters)) => stream.set_parameters(parameters),
                (None, None) => unreachable!("video mode produced no parameter source"),
            }
            if let Some(track) = external_track
                .as_ref()
                .filter(|track| !track.extradata.is_empty())
            {
                // SAFETY: `stream` is a freshly added stream of this output
                // context and the parameters pointer is valid; the write
                // happens before the header is written.
                unsafe { attach_stream_extradata(&mut stream, &track.extradata)? };
            }
            stream.index()
        };
        let mut audio = create_audio_state(
            &mut output,
            &staging_path,
            config.format,
            global_header,
            config.audio.as_ref(),
            config.encode_threads,
        )?;
        output.write_header().map_err(|error| {
            RecordingExportError::Encode(format!(
                "failed to write streaming output header: {error}"
            ))
        })?;
        let stream_time_base = output
            .stream(stream_index)
            .map(|stream| stream.time_base())
            .ok_or_else(|| {
                RecordingExportError::Encode(
                    "streaming video track disappeared after header write".to_string(),
                )
            })?;
        if let Some(audio) = audio.as_mut() {
            audio.stream_time_base = output
                .stream(audio.stream_index)
                .map(|stream| stream.time_base())
                .ok_or_else(|| {
                    RecordingExportError::Encode(
                        "streaming audio track disappeared after header write".to_string(),
                    )
                })?;
        }
        let source_order = config.pixel_order.kernel_order();
        let source_pixel = config.pixel_order.ffmpeg_input();
        let scaler = ffmpeg::software::scaling::Context::get(
            config.pixel_order.ffmpeg_input(),
            config.width,
            config.height,
            pixel_format,
            config.width,
            config.height,
            ffmpeg::software::scaling::flag::Flags::BICUBIC,
        )
        .map_err(|error| {
            RecordingExportError::Encode(format!("failed to create streaming RGBA scaler: {error}"))
        })?;
        let encode_frame = ffmpeg::frame::Video::new(pixel_format, config.width, config.height);

        Ok(Self {
            output: Some(output),
            encoder,
            stream_index,
            stream_time_base,
            scaler,
            rgba_frame: None,
            encode_frame,
            source_order,
            source_pixel,
            use_kernel_conversion: matches!(
                pixel_format,
                ffmpeg::format::Pixel::NV12 | ffmpeg::format::Pixel::YUV420P
            ),
            width: config.width,
            height: config.height,
            fps,
            pending_pts: None,
            coalesced_pixels: Vec::new(),
            pending_coalesced: false,
            pending_packet_durations: VecDeque::new(),
            external: external_track.is_some(),
            pending_external: None,
            audio,
            staging_path: None,
            final_path: config.output_path,
            report: StreamingEncoderReport {
                video_encoder: external_track
                    .as_ref()
                    .map(|track| track.encoder_name.clone())
                    .unwrap_or_else(|| video_codec.name().to_string()),
                used_hardware_video_encoder: external_track.is_some()
                    || is_hardware_h264_encoder(&video_codec),
                audio_encoder: config
                    .audio
                    .as_ref()
                    .and_then(|_| choose_audio_codec(config.format))
                    .map(|codec| codec.name().to_string()),
                ..StreamingEncoderReport::default()
            },
            finished: false,
        })
    }

    /// Queue one frame for encoding. The pixels are copied into the encoder's
    /// staging frame and converted immediately, so `rgba` only needs to stay
    /// valid for the duration of this call. A push whose timestamp maps to a
    /// presentation timestamp that is not newer than the pending one replaces
    /// the pending frame's pixels (coalescing) instead of emitting two frames;
    /// replacements are staged and converted once the timestamp advances.
    pub fn push_rgba_frame(&mut self, timestamp_ms: u64, rgba: &[u8]) -> Result<()> {
        let expected = self.width as usize * self.height as usize * 4;
        if rgba.len() != expected {
            return Err(RecordingExportError::InvalidConfig(format!(
                "streaming RGBA frame has {} bytes; expected {expected}",
                rgba.len()
            )));
        }
        let pts = pts_from_timestamp_ms(timestamp_ms, self.fps);
        match self.pending_pts.take() {
            None => {}
            Some(pending_pts) if pts <= pending_pts => {
                self.pending_pts = Some(pending_pts);
                if self.coalesced_pixels.len() != expected {
                    self.coalesced_pixels.resize(expected, 0);
                }
                self.coalesced_pixels.copy_from_slice(rgba);
                self.pending_coalesced = true;
                self.report.coalesced_frames = self.report.coalesced_frames.saturating_add(1);
                return Ok(());
            }
            Some(pending_pts) => {
                if self.pending_coalesced {
                    let staged = std::mem::take(&mut self.coalesced_pixels);
                    self.convert_frame(&staged)?;
                    self.coalesced_pixels = staged;
                    self.pending_coalesced = false;
                }
                let duration = pts.saturating_sub(pending_pts).max(1);
                self.send_converted(pending_pts, duration)?;
            }
        }
        self.convert_frame(rgba)?;
        self.pending_pts = Some(pts);
        Ok(())
    }

    /// Write one externally encoded H.264 packet (Annex-B byte stream).
    ///
    /// Mirrors `push_rgba_frame`'s timing semantics: packet durations come
    /// from presentation-timestamp gaps, and `finish` flushes the final
    /// packet with a single-tick duration. A push repeating the pending
    /// timestamp is dropped (counted as coalesced) rather than replacing
    /// the pending packet: unlike pixels, encoded packets cannot be
    /// substituted without breaking reference chains. Only valid on
    /// encoders configured with `external_video`.
    pub fn push_encoded_video_packet(
        &mut self,
        timestamp_ms: u64,
        is_keyframe: bool,
        data: &[u8],
    ) -> Result<()> {
        if !self.external {
            return Err(RecordingExportError::InvalidConfig(
                "push_encoded_video_packet requires external video configuration".into(),
            ));
        }
        if data.is_empty() {
            return Err(RecordingExportError::InvalidConfig(
                "encoded video packet must not be empty".into(),
            ));
        }
        let pts = pts_from_timestamp_ms(timestamp_ms, self.fps);
        match self.pending_external.take() {
            Some(pending) if pts <= pending.pts => {
                self.pending_external = Some(pending);
                self.report.coalesced_frames = self.report.coalesced_frames.saturating_add(1);
            }
            Some(pending) => {
                let duration = pts.saturating_sub(pending.pts).max(1);
                self.write_external_packet(
                    pending.pts,
                    duration,
                    pending.is_keyframe,
                    &pending.data,
                )?;
                self.pending_external = Some(PendingExternalPacket {
                    pts,
                    is_keyframe,
                    data: data.to_vec(),
                });
            }
            None => {
                self.pending_external = Some(PendingExternalPacket {
                    pts,
                    is_keyframe,
                    data: data.to_vec(),
                });
            }
        }
        Ok(())
    }

    /// Write one external packet into the output, rescaling presentation
    /// timestamp and duration onto the video stream's time base.
    fn write_external_packet(
        &mut self,
        pts: i64,
        duration: i64,
        is_keyframe: bool,
        data: &[u8],
    ) -> Result<()> {
        let mut packet = ffmpeg::Packet::copy(data);
        packet.set_stream(self.stream_index);
        packet.set_pts(Some(pts));
        packet.set_dts(Some(pts));
        packet.set_duration(duration);
        if is_keyframe {
            packet.set_flags(ffmpeg::packet::Flags::KEY);
        }
        packet.rescale_ts(ffmpeg::Rational(1, self.fps as i32), self.stream_time_base);
        let output = self.output.as_mut().ok_or_else(|| {
            RecordingExportError::Encode("streaming output is already closed".into())
        })?;
        packet.write_interleaved(output).map_err(|error| {
            RecordingExportError::Encode(format!("failed to write external video packet: {error}"))
        })?;
        self.report.encoded_frames = self.report.encoded_frames.saturating_add(1);
        Ok(())
    }

    pub fn push_audio_pcm_i16(&mut self, timestamp_ms: u64, samples: &[i16]) -> Result<()> {
        let audio = self.audio.as_mut().ok_or_else(|| {
            RecordingExportError::InvalidConfig(
                "streaming encoder was created without an audio track".to_string(),
            )
        })?;
        let output = self.output.as_mut().ok_or_else(|| {
            RecordingExportError::Encode("streaming output is already closed".to_string())
        })?;
        audio.push_pcm(timestamp_ms, samples, output, &mut self.report)
    }

    pub fn has_audio(&self) -> bool {
        self.audio.is_some()
    }

    pub fn finish(mut self) -> Result<StreamingEncoderReport> {
        if self.external {
            if let Some(pending) = self.pending_external.take() {
                self.write_external_packet(pending.pts, 1, pending.is_keyframe, &pending.data)?;
            }
        } else if let Some(pending_pts) = self.pending_pts.take() {
            if self.pending_coalesced {
                let staged = std::mem::take(&mut self.coalesced_pixels);
                self.convert_frame(&staged)?;
                self.coalesced_pixels = staged;
                self.pending_coalesced = false;
            }
            self.send_converted(pending_pts, 1)?;
        }
        if let Some(audio) = self.audio.as_mut() {
            let output = self.output.as_mut().ok_or_else(|| {
                RecordingExportError::Encode("streaming output is already closed".to_string())
            })?;
            audio.finish(output, &mut self.report)?;
        }
        if !self.external {
            self.encoder
                .as_mut()
                .ok_or_else(|| {
                    RecordingExportError::Encode(
                        "streaming encoder is missing its video encoder".to_string(),
                    )
                })?
                .send_eof()
                .map_err(|error| {
                    RecordingExportError::Encode(format!(
                        "failed to flush streaming video encoder: {error}"
                    ))
                })?;
            let output = self.output.as_mut().ok_or_else(|| {
                RecordingExportError::Encode("streaming output is already closed".to_string())
            })?;
            let encoder = self.encoder.as_mut().ok_or_else(|| {
                RecordingExportError::Encode(
                    "streaming encoder is missing its video encoder".to_string(),
                )
            })?;
            drain_video_packets_with_durations_timed(
                encoder,
                output,
                self.stream_index,
                self.stream_time_base,
                true,
                &mut self.pending_packet_durations,
                #[cfg(feature = "stage-timing")]
                Some((
                    &mut self.report.video_stage_timings.packet_receive,
                    &mut self.report.video_stage_timings.mux_write,
                )),
            )?;
        }
        {
            let output = self.output.as_mut().ok_or_else(|| {
                RecordingExportError::Encode("streaming output is already closed".to_string())
            })?;
            output.write_trailer().map_err(|error| {
                RecordingExportError::Encode(format!(
                    "failed to write streaming output trailer: {error}"
                ))
            })?;
        }
        self.output.take();
        let staging_path = self.staging_path.take().ok_or_else(|| {
            RecordingExportError::Encode("streaming staging path is unavailable".to_string())
        })?;
        if let Err(error) = publish_staging_file(&staging_path, &self.final_path) {
            let _ = fs::remove_file(&staging_path);
            return Err(error.into());
        }
        self.finished = true;
        Ok(self.report.clone())
    }

    /// Copy `rgba` into the staging frame and convert it into `encode_frame`.
    /// The converted frame's presentation timestamp is assigned later, when
    /// the next push reveals its duration.
    fn convert_frame(&mut self, rgba: &[u8]) -> Result<()> {
        #[cfg(feature = "stage-timing")]
        let stage_started = std::time::Instant::now();
        if self.use_kernel_conversion {
            ensure_video_frame_writable(&mut self.encode_frame)?;
            let planes =
                convert::yuv420_planes_from_frame(&mut self.encode_frame).ok_or_else(|| {
                    RecordingExportError::Encode(
                        "streaming encode frame does not expose YUV 4:2:0 planes".to_string(),
                    )
                })?;
            convert::convert_rgb_to_yuv420(rgba, self.source_order, planes, ConversionMode::Auto);
            #[cfg(feature = "stage-timing")]
            {
                self.report.video_stage_timings.convert += stage_started.elapsed();
            }
            return Ok(());
        }
        let rgba_frame = self.rgba_frame.get_or_insert_with(|| {
            ffmpeg::frame::Video::new(self.source_pixel, self.width, self.height)
        });
        copy_rgba_into_frame(rgba_frame, self.width, rgba);
        #[cfg(feature = "stage-timing")]
        {
            self.report.video_stage_timings.copy += stage_started.elapsed();
        }
        #[cfg(feature = "stage-timing")]
        let stage_started = std::time::Instant::now();
        ensure_video_frame_writable(&mut self.encode_frame)?;
        self.scaler
            .run(rgba_frame, &mut self.encode_frame)
            .map_err(|error| {
                RecordingExportError::Encode(format!(
                    "failed to convert streaming RGBA frame: {error}"
                ))
            })?;
        #[cfg(feature = "stage-timing")]
        {
            self.report.video_stage_timings.convert += stage_started.elapsed();
        }
        Ok(())
    }

    /// Submit the frame currently held in `encode_frame` to the encoder and
    /// drain whatever packets become available.
    fn send_converted(&mut self, pts: i64, duration: i64) -> Result<()> {
        #[cfg(feature = "stage-timing")]
        let stage_started = std::time::Instant::now();
        self.encode_frame.set_pts(Some(pts));
        let encoder = self.encoder.as_mut().ok_or_else(|| {
            RecordingExportError::Encode("streaming encoder is missing its video encoder".into())
        })?;
        encoder.send_frame(&self.encode_frame).map_err(|error| {
            RecordingExportError::Encode(format!("failed to send streaming video frame: {error}"))
        })?;
        #[cfg(feature = "stage-timing")]
        {
            self.report.video_stage_timings.send += stage_started.elapsed();
        }
        #[cfg(feature = "stage-timing")]
        let stage_started = std::time::Instant::now();
        self.pending_packet_durations.push_back(duration.max(1));
        let output = self.output.as_mut().ok_or_else(|| {
            RecordingExportError::Encode("streaming output is already closed".to_string())
        })?;
        let encoder = self.encoder.as_mut().ok_or_else(|| {
            RecordingExportError::Encode("streaming encoder is missing its video encoder".into())
        })?;
        drain_video_packets_with_durations_timed(
            encoder,
            output,
            self.stream_index,
            self.stream_time_base,
            false,
            &mut self.pending_packet_durations,
            #[cfg(feature = "stage-timing")]
            Some((
                &mut self.report.video_stage_timings.packet_receive,
                &mut self.report.video_stage_timings.mux_write,
            )),
        )?;
        #[cfg(feature = "stage-timing")]
        {
            self.report.video_stage_timings.drain_and_mux += stage_started.elapsed();
        }
        self.report.encoded_frames = self.report.encoded_frames.saturating_add(1);
        Ok(())
    }
}

fn create_audio_state(
    output: &mut ffmpeg::format::context::Output,
    output_path: &Path,
    format: ExportFormat,
    global_header: bool,
    config: Option<&StreamingAudioConfig>,
    encode_threads: u8,
) -> Result<Option<StreamingAudioState>> {
    let Some(config) = config else {
        return Ok(None);
    };
    let container_codec = output
        .format()
        .codec(output_path, ffmpeg::media::Type::Audio);
    let codec = choose_audio_codec(format)
        .or_else(|| ffmpeg::encoder::find(container_codec))
        .ok_or_else(|| {
            RecordingExportError::Encode(format!("no audio encoder is available for {format:?}"))
        })?;
    let codec_info = codec.audio().map_err(|error| {
        RecordingExportError::Encode(format!(
            "selected streaming audio codec is not usable: {error}"
        ))
    })?;
    let output_rate = choose_audio_sample_rate(codec_info, config.sample_rate_hz);
    let output_layout = choose_audio_channel_layout(codec_info, config.channels);
    let output_format = choose_audio_sample_format(codec_info);
    let mut encoder = ffmpeg::codec::context::Context::new_with_codec(codec)
        .encoder()
        .audio()
        .map_err(|error| {
            RecordingExportError::Encode(format!(
                "failed to create streaming audio encoder: {error}"
            ))
        })?;
    encoder.set_rate(output_rate as i32);
    encoder.set_channel_layout(output_layout);
    encoder.set_format(output_format);
    encoder.set_bit_rate(usize::from(effective_audio_bitrate_kbps(config.bitrate_kbps)) * 1_000);
    encoder.set_time_base((1, output_rate as i32));
    configure_codec_threads(
        &mut encoder,
        encode_threads,
        ffmpeg::codec::threading::Type::Frame,
    );
    if global_header {
        encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
    }
    let encoder = open_audio_encoder(encoder, codec)?;
    let stream_index = {
        let mut stream = output.add_stream(codec).map_err(|error| {
            RecordingExportError::Encode(format!("failed to add streaming audio track: {error}"))
        })?;
        stream.set_time_base(ffmpeg::Rational(1, output_rate as i32));
        stream.set_rate(ffmpeg::Rational(output_rate as i32, 1));
        stream.set_parameters(&encoder);
        stream.index()
    };
    let input_layout = ffmpeg::ChannelLayout::default(i32::from(config.channels));
    let input_format = ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Packed);
    let resampler = if encoder.rate() == config.sample_rate_hz
        && encoder.channel_layout() == input_layout
        && encoder.format() == input_format
    {
        None
    } else {
        Some(
            ffmpeg::software::resampling::Context::get(
                input_format,
                input_layout,
                config.sample_rate_hz,
                encoder.format(),
                encoder.channel_layout(),
                encoder.rate(),
            )
            .map_err(|error| {
                RecordingExportError::Encode(format!(
                    "failed to create streaming audio resampler: {error}"
                ))
            })?,
        )
    };
    let variable_frame_size = encoder.frame_size() == 0;
    let frame_samples = if variable_frame_size {
        (config.sample_rate_hz as usize / 100).max(1)
    } else {
        encoder.frame_size() as usize
    };
    Ok(Some(StreamingAudioState {
        encoder,
        stream_index,
        stream_time_base: ffmpeg::Rational(1, output_rate as i32),
        input_rate: config.sample_rate_hz,
        input_channels: usize::from(config.channels),
        input_layout,
        resampler,
        frame_samples,
        variable_frame_size,
        pending_samples: VecDeque::with_capacity(frame_samples * usize::from(config.channels) * 2),
        next_input_frame: 0,
        next_encoder_pts: 0,
    }))
}

impl StreamingAudioState {
    fn push_pcm(
        &mut self,
        timestamp_ms: u64,
        samples: &[i16],
        output: &mut ffmpeg::format::context::Output,
        report: &mut StreamingEncoderReport,
    ) -> Result<()> {
        if !samples.len().is_multiple_of(self.input_channels) {
            return Err(RecordingExportError::InvalidConfig(format!(
                "streaming PCM contains {} samples for {} channels",
                samples.len(),
                self.input_channels
            )));
        }
        let timestamp_frame = ((u128::from(timestamp_ms) * u128::from(self.input_rate) + 500)
            / 1_000)
            .min(u128::from(u64::MAX)) as u64;
        if timestamp_frame > self.next_input_frame {
            let gap = timestamp_frame - self.next_input_frame;
            self.append_silence(gap, output, report)?;
            report.inserted_silence_frames = report.inserted_silence_frames.saturating_add(gap);
        }

        let packet_frames = samples.len() / self.input_channels;
        let overlap = self.next_input_frame.saturating_sub(timestamp_frame);
        let dropped = overlap.min(packet_frames as u64) as usize;
        if dropped > 0 {
            report.dropped_audio_frames =
                report.dropped_audio_frames.saturating_add(dropped as u64);
        }
        let remaining = &samples[dropped * self.input_channels..];
        self.pending_samples.extend(remaining.iter().copied());
        self.next_input_frame = self
            .next_input_frame
            .saturating_add((remaining.len() / self.input_channels) as u64);
        self.encode_ready_frames(output, report)
    }

    fn append_silence(
        &mut self,
        mut frames: u64,
        output: &mut ffmpeg::format::context::Output,
        report: &mut StreamingEncoderReport,
    ) -> Result<()> {
        while frames > 0 {
            let take = frames.min(self.frame_samples as u64) as usize;
            self.pending_samples
                .extend(std::iter::repeat_n(0, take * self.input_channels));
            self.next_input_frame = self.next_input_frame.saturating_add(take as u64);
            frames -= take as u64;
            self.encode_ready_frames(output, report)?;
        }
        Ok(())
    }

    fn encode_ready_frames(
        &mut self,
        output: &mut ffmpeg::format::context::Output,
        report: &mut StreamingEncoderReport,
    ) -> Result<()> {
        let samples_per_frame = self.frame_samples * self.input_channels;
        while self.pending_samples.len() >= samples_per_frame {
            let samples: Vec<i16> = self.pending_samples.drain(..samples_per_frame).collect();
            self.encode_chunk(&samples, self.frame_samples, output)?;
            report.encoded_audio_frames = report
                .encoded_audio_frames
                .saturating_add(self.frame_samples as u64);
        }
        Ok(())
    }

    fn encode_chunk(
        &mut self,
        samples: &[i16],
        frames: usize,
        output: &mut ffmpeg::format::context::Output,
    ) -> Result<()> {
        let input_format = ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Packed);
        let mut source = ffmpeg::frame::Audio::new(input_format, frames, self.input_layout);
        source.set_rate(self.input_rate);
        source.set_samples(frames);
        let required_bytes = std::mem::size_of_val(samples);
        let destination = source.data_mut(0);
        if destination.len() < required_bytes {
            return Err(RecordingExportError::Encode(
                "allocated streaming audio frame is too small".to_string(),
            ));
        }
        for (destination, sample) in destination[..required_bytes]
            .chunks_exact_mut(2)
            .zip(samples.iter())
        {
            destination.copy_from_slice(&sample.to_ne_bytes());
        }

        if let Some(resampler) = self.resampler.as_mut() {
            let mut converted = ffmpeg::frame::Audio::empty();
            resampler.run(&source, &mut converted).map_err(|error| {
                RecordingExportError::Encode(format!("failed to resample streaming audio: {error}"))
            })?;
            if converted.samples() > 0 {
                send_audio_frame(
                    &mut self.encoder,
                    &mut converted,
                    &mut self.next_encoder_pts,
                    output,
                    self.stream_index,
                    self.stream_time_base,
                )?;
            }
        } else {
            send_audio_frame(
                &mut self.encoder,
                &mut source,
                &mut self.next_encoder_pts,
                output,
                self.stream_index,
                self.stream_time_base,
            )?;
        }
        Ok(())
    }

    fn finish(
        &mut self,
        output: &mut ffmpeg::format::context::Output,
        report: &mut StreamingEncoderReport,
    ) -> Result<()> {
        if !self.pending_samples.is_empty() {
            let pending_frames = self.pending_samples.len() / self.input_channels;
            let encoded_frames = if self.variable_frame_size {
                pending_frames
            } else {
                self.frame_samples
            };
            self.pending_samples
                .resize(encoded_frames * self.input_channels, 0);
            let samples: Vec<i16> = self.pending_samples.drain(..).collect();
            self.encode_chunk(&samples, encoded_frames, output)?;
            report.encoded_audio_frames = report
                .encoded_audio_frames
                .saturating_add(pending_frames as u64);
        }
        if let Some(resampler) = self.resampler.as_mut() {
            loop {
                let mut converted = ffmpeg::frame::Audio::new(
                    self.encoder.format(),
                    self.frame_samples,
                    self.encoder.channel_layout(),
                );
                converted.set_rate(self.encoder.rate());
                let delay = resampler.flush(&mut converted).map_err(|error| {
                    RecordingExportError::Encode(format!(
                        "failed to flush streaming audio resampler: {error}"
                    ))
                })?;
                if converted.samples() > 0 {
                    let samples = converted.samples() as u64;
                    send_audio_frame(
                        &mut self.encoder,
                        &mut converted,
                        &mut self.next_encoder_pts,
                        output,
                        self.stream_index,
                        self.stream_time_base,
                    )?;
                    report.encoded_audio_frames =
                        report.encoded_audio_frames.saturating_add(samples);
                }
                if delay.is_none() {
                    break;
                }
            }
        }
        self.encoder.send_eof().map_err(|error| {
            RecordingExportError::Encode(format!(
                "failed to flush streaming audio encoder: {error}"
            ))
        })?;
        drain_streaming_audio_packets(
            &mut self.encoder,
            true,
            output,
            self.stream_index,
            self.stream_time_base,
        )
    }
}

fn send_audio_frame(
    encoder: &mut ffmpeg::encoder::audio::Encoder,
    frame: &mut ffmpeg::frame::Audio,
    next_pts: &mut i64,
    output: &mut ffmpeg::format::context::Output,
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
) -> Result<()> {
    let samples = frame.samples() as i64;
    frame.set_pts(Some(*next_pts));
    *next_pts = next_pts.saturating_add(samples);
    encoder.send_frame(frame).map_err(|error| {
        RecordingExportError::Encode(format!("failed to send streaming audio frame: {error}"))
    })?;
    drain_streaming_audio_packets(encoder, false, output, stream_index, stream_time_base)
}

fn drain_streaming_audio_packets(
    encoder: &mut ffmpeg::encoder::audio::Encoder,
    draining: bool,
    output: &mut ffmpeg::format::context::Output,
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
) -> Result<()> {
    let encoder_time_base = encoder.time_base();
    drain_audio_packets_with_callback(encoder, draining, |mut packet| {
        packet.set_stream(stream_index);
        packet.rescale_ts(encoder_time_base, stream_time_base);
        packet.write_interleaved(output).map_err(|error| {
            RecordingExportError::Encode(format!("failed to write streaming audio packet: {error}"))
        })
    })
}

impl Drop for StreamingEncoder {
    fn drop(&mut self) {
        if !self.finished {
            self.output.take();
            if let Some(path) = self.staging_path.take() {
                let _ = fs::remove_file(path);
            }
        }
    }
}

pub fn cleanup_stale_staging_files(directory: &Path) -> std::io::Result<usize> {
    let now = SystemTime::now();
    let mut removed = 0usize;
    for entry in fs::read_dir(directory)? {
        let entry = entry?;
        let file_name = entry.file_name();
        let Some(file_name) = file_name.to_str() else {
            continue;
        };
        if !is_direct_staging_name(file_name) {
            continue;
        }
        let modified = entry
            .metadata()?
            .modified()
            .unwrap_or(SystemTime::UNIX_EPOCH);
        if now.duration_since(modified).unwrap_or_default() < STALE_STAGING_AGE {
            continue;
        }
        if fs::remove_file(entry.path()).is_ok() {
            removed = removed.saturating_add(1);
        }
    }
    Ok(removed)
}

fn create_staging_path(final_path: &Path) -> std::io::Result<PathBuf> {
    let directory = final_path
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let extension = final_path
        .extension()
        .and_then(|value| value.to_str())
        .unwrap_or("mp4");
    for _ in 0..16 {
        let name = format!(
            "{DIRECT_STAGING_PREFIX}{}.{}",
            Uuid::new_v4().simple(),
            extension
        );
        let path = directory.join(name);
        match OpenOptions::new().write(true).create_new(true).open(&path) {
            Ok(_) => return Ok(path),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(error),
        }
    }
    Err(std::io::Error::new(
        std::io::ErrorKind::AlreadyExists,
        "could not allocate a unique direct-recording staging file",
    ))
}

fn publish_staging_file(staging_path: &Path, final_path: &Path) -> std::io::Result<()> {
    if final_path.exists() {
        return Err(std::io::Error::new(
            std::io::ErrorKind::AlreadyExists,
            format!("recording output already exists: {}", final_path.display()),
        ));
    }
    fs::rename(staging_path, final_path)
}

fn is_direct_staging_name(name: &str) -> bool {
    let Some(rest) = name.strip_prefix(DIRECT_STAGING_PREFIX) else {
        return false;
    };
    let Some((uuid, extension)) = rest.split_once('.') else {
        return false;
    };
    uuid.len() == 32
        && uuid.bytes().all(|byte| byte.is_ascii_hexdigit())
        && matches!(
            extension.to_ascii_lowercase().as_str(),
            "mp4" | "gif" | "apng" | "webp"
        )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    use snow_recording_model::VideoEncodingSpeed;

    fn encoder_config(output_path: PathBuf, format: ExportFormat) -> StreamingEncoderConfig {
        StreamingEncoderConfig {
            output_path,
            format,
            width: 16,
            height: 16,
            fps: 10,
            codec: VideoCodec::H264,
            prefer_hardware_h264: false,
            execution_mode: ExportExecutionMode::SoftwareOnly,
            software_h264_priority: SoftwareH264Priority::X264First,
            video: VideoEncodeConfig {
                quality: 80,
                speed: VideoEncodingSpeed::VeryFast,
            },
            encode_threads: 1,
            audio: None,
            pixel_order: StreamingPixelOrder::Rgba,
            external_video: None,
        }
    }

    fn write_test_frames(encoder: &mut StreamingEncoder) {
        for index in 0..3u8 {
            let mut rgba = vec![0u8; 16 * 16 * 4];
            for pixel in rgba.chunks_exact_mut(4) {
                pixel.copy_from_slice(&[index.saturating_mul(80), 40, 180, 255]);
            }
            encoder
                .push_rgba_frame(u64::from(index) * 100, &rgba)
                .unwrap();
        }
    }

    fn decoded_video_frame_count(path: &Path) -> (usize, u32, u32) {
        let mut input = ffmpeg::format::input(path).unwrap_or_else(|error| {
            panic!("failed to open generated video {}: {error}", path.display())
        });
        let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
        let stream_index = stream.index();
        let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
            .unwrap()
            .decoder()
            .video()
            .unwrap();
        let dimensions = (decoder.width(), decoder.height());
        let mut decoded = ffmpeg::frame::Video::empty();
        let mut count = 0usize;
        for (stream, packet) in input.packets() {
            if stream.index() != stream_index {
                continue;
            }
            decoder.send_packet(&packet).unwrap();
            while decoder.receive_frame(&mut decoded).is_ok() {
                count += 1;
            }
        }
        decoder.send_eof().unwrap();
        while decoder.receive_frame(&mut decoded).is_ok() {
            count += 1;
        }
        (count, dimensions.0, dimensions.1)
    }

    #[test]
    fn staging_name_match_is_exact() {
        assert!(is_direct_staging_name(
            ".snow-recording-direct-0123456789abcdef0123456789abcdef.mp4"
        ));
        assert!(!is_direct_staging_name("holiday.mp4"));
        assert!(!is_direct_staging_name(
            ".snow-recording-direct-not-a-uuid.mp4"
        ));
        assert!(!is_direct_staging_name(
            ".snow-recording-direct-0123456789abcdef0123456789abcdef.tmp"
        ));
    }

    #[test]
    fn cleanup_never_removes_ordinary_media() {
        let directory = tempfile::tempdir().unwrap();
        let ordinary = directory.path().join("recording.mp4");
        std::fs::File::create(&ordinary)
            .unwrap()
            .write_all(b"media")
            .unwrap();
        assert_eq!(cleanup_stale_staging_files(directory.path()).unwrap(), 0);
        assert!(ordinary.exists());
    }

    #[test]
    fn duplicate_pts_push_coalesces_instead_of_encoding_twice() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("recording.mp4");
        let mut encoder =
            StreamingEncoder::create(encoder_config(path.clone(), ExportFormat::Mp4)).unwrap();
        let mut first = vec![0u8; 16 * 16 * 4];
        let mut second = vec![0u8; 16 * 16 * 4];
        for pixel in first.chunks_exact_mut(4) {
            pixel.copy_from_slice(&[20, 40, 180, 255]);
        }
        for pixel in second.chunks_exact_mut(4) {
            pixel.copy_from_slice(&[220, 120, 30, 255]);
        }
        // The second push shares the first push's presentation timestamp and
        // must replace its pixels without emitting an extra frame.
        encoder.push_rgba_frame(0, &first).unwrap();
        encoder.push_rgba_frame(40, &second).unwrap();
        encoder.push_rgba_frame(100, &second).unwrap();
        let report = encoder.finish().unwrap();
        assert_eq!(report.encoded_frames, 2);
        assert_eq!(report.coalesced_frames, 1);
        let (decoded, _, _) = decoded_video_frame_count(&path);
        assert_eq!(decoded, 2);
    }

    #[test]
    fn bgra_sources_encode_for_kernel_and_swscale_targets() {
        let directory = tempfile::tempdir().unwrap();
        // NV12/YUV420P go through the parallel kernel, APNG through the
        // swscale fallback; both must accept BGRA input and produce the
        // same decoded frame count as RGBA sources.
        for format in [ExportFormat::Mp4, ExportFormat::Apng] {
            let path = directory
                .path()
                .join(format!("recording.{}", format.file_extension()));
            let mut config = encoder_config(path.clone(), format);
            config.pixel_order = StreamingPixelOrder::Bgra;
            let mut encoder = StreamingEncoder::create(config).unwrap();
            for index in 0..3u8 {
                let mut bgra = vec![0u8; 16 * 16 * 4];
                for pixel in bgra.chunks_exact_mut(4) {
                    pixel.copy_from_slice(&[180, 40, index.saturating_mul(80), 255]);
                }
                encoder
                    .push_rgba_frame(u64::from(index) * 100, &bgra)
                    .unwrap();
            }
            let report = encoder.finish().unwrap();
            assert_eq!(report.encoded_frames, 3);
            let (decoded, width, height) = decoded_video_frame_count(&path);
            assert_eq!((width, height), (16, 16));
            assert!(decoded >= 2, "{format:?} should contain changing frames");
        }
    }

    #[test]
    fn direct_formats_publish_decodable_video_without_audio() {
        let directory = tempfile::tempdir().unwrap();
        for format in [
            ExportFormat::Mp4,
            ExportFormat::Gif,
            ExportFormat::Apng,
            ExportFormat::Webp,
        ] {
            let path = directory
                .path()
                .join(format!("recording.{}", format.file_extension()));
            let mut encoder =
                StreamingEncoder::create(encoder_config(path.clone(), format)).unwrap();
            write_test_frames(&mut encoder);
            let report = encoder.finish().unwrap();
            assert_eq!(report.encoded_frames, 3);
            assert!(path.is_file());
            let (decoded, width, height) = decoded_video_frame_count(&path);
            assert_eq!((width, height), (16, 16));
            assert!(decoded >= 2, "{format:?} should contain changing frames");
            let input = ffmpeg::format::input(&path).unwrap();
            assert!(
                input
                    .streams()
                    .all(|stream| stream.parameters().medium() != ffmpeg::media::Type::Audio),
                "{format:?} should not contain an audio stream"
            );
        }
    }

    #[test]
    fn mp4_streaming_audio_inserts_silence_and_encodes_aac() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("recording.mp4");
        let mut config = encoder_config(path.clone(), ExportFormat::Mp4);
        config.audio = Some(StreamingAudioConfig {
            sample_rate_hz: 48_000,
            channels: 2,
            bitrate_kbps: 160,
        });
        let mut encoder = StreamingEncoder::create(config).unwrap();
        write_test_frames(&mut encoder);
        let packet = vec![1_000i16; 480 * 2];
        encoder.push_audio_pcm_i16(0, &packet).unwrap();
        encoder.push_audio_pcm_i16(20, &packet).unwrap();
        let report = encoder.finish().unwrap();
        assert_eq!(report.audio_encoder.as_deref(), Some("aac"));
        assert_eq!(report.inserted_silence_frames, 480);
        assert_eq!(report.dropped_audio_frames, 0);
        let input = ffmpeg::format::input(&path).unwrap();
        let audio = input.streams().best(ffmpeg::media::Type::Audio).unwrap();
        assert_eq!(audio.parameters().id(), ffmpeg::codec::Id::AAC);
    }

    #[test]
    fn animated_formats_reject_audio_configuration() {
        let mut config = encoder_config(PathBuf::from("recording.gif"), ExportFormat::Gif);
        config.audio = Some(StreamingAudioConfig {
            sample_rate_hz: 48_000,
            channels: 2,
            bitrate_kbps: 160,
        });
        assert!(config.validate().is_err());
    }

    #[test]
    fn publication_failure_removes_direct_staging_file() {
        let directory = tempfile::tempdir().unwrap();
        let output = directory.path().join("recording.mp4");
        std::fs::write(&output, b"existing").unwrap();
        let mut encoder =
            StreamingEncoder::create(encoder_config(output.clone(), ExportFormat::Mp4)).unwrap();
        write_test_frames(&mut encoder);
        assert!(encoder.finish().is_err());
        assert_eq!(std::fs::read(&output).unwrap(), b"existing");
        assert!(
            std::fs::read_dir(directory.path())
                .unwrap()
                .filter_map(std::result::Result::ok)
                .all(|entry| !entry
                    .file_name()
                    .to_string_lossy()
                    .starts_with(DIRECT_STAGING_PREFIX))
        );
    }

    // ---- external video packet muxing ----

    #[test]
    fn pts_mapping_rounds_half_up() {
        assert_eq!(pts_from_timestamp_ms(0, 10), 0);
        assert_eq!(pts_from_timestamp_ms(100, 10), 1);
        assert_eq!(pts_from_timestamp_ms(149, 10), 1);
        assert_eq!(pts_from_timestamp_ms(150, 10), 2);
        assert_eq!(pts_from_timestamp_ms(1_000, 60), 60);
        assert_eq!(pts_from_timestamp_ms(16, 60), 1);
    }

    #[test]
    fn external_video_requires_mp4_h264_output() {
        let mut config = encoder_config(PathBuf::from("recording.gif"), ExportFormat::Gif);
        config.external_video = Some(ExternalVideoTrack {
            extradata: Vec::new(),
            encoder_name: "external".into(),
        });
        assert!(config.validate().is_err());

        let mut config = encoder_config(PathBuf::from("recording.mp4"), ExportFormat::Mp4);
        config.codec = VideoCodec::H265;
        config.external_video = Some(ExternalVideoTrack {
            extradata: Vec::new(),
            encoder_name: "external".into(),
        });
        assert!(config.validate().is_err());
    }

    #[test]
    fn external_pushes_are_rejected_without_external_configuration() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("recording.mp4");
        let mut encoder =
            StreamingEncoder::create(encoder_config(path.clone(), ExportFormat::Mp4)).unwrap();
        assert!(
            encoder
                .push_encoded_video_packet(0, true, &[0, 0, 0, 1])
                .is_err()
        );
        let _ = encoder.finish().unwrap();
    }

    /// Encode synthetic YUV frames with libx264 and collect the Annex-B
    /// packets plus (optionally) the encoder's avcC extradata.
    type X264Packet = (i64, bool, Vec<u8>);

    #[allow(clippy::too_many_arguments)]
    fn x264_annexb_packets(
        width: u32,
        height: u32,
        fps: u32,
        frames: usize,
        global_header: bool,
        all_intra: bool,
    ) -> (Vec<X264Packet>, Vec<u8>) {
        let codec = ffmpeg::encoder::find_by_name("libx264")
            .expect("libx264 must be available for external packet tests");
        let mut encoder = ffmpeg::codec::context::Context::new_with_codec(codec)
            .encoder()
            .video()
            .unwrap();
        encoder.set_width(width);
        encoder.set_height(height);
        encoder.set_format(ffmpeg::format::Pixel::YUV420P);
        encoder.set_time_base(ffmpeg::Rational(1, fps as i32));
        encoder.set_frame_rate(Some(ffmpeg::Rational(fps as i32, 1)));
        if global_header {
            encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
        }
        let mut options = ffmpeg::Dictionary::new();
        options.set("preset", "ultrafast");
        options.set("tune", "zerolatency");
        if all_intra {
            options.set("x264-params", "keyint=1");
        }
        let mut encoder = encoder.open_with(options).unwrap();

        let mut packets = Vec::new();
        for index in 0..frames {
            let mut frame =
                ffmpeg::frame::Video::new(ffmpeg::format::Pixel::YUV420P, width, height);
            crate::editing::ensure_video_frame_writable(&mut frame).unwrap();
            for plane in 0..3 {
                let value = if plane == 0 { (index * 40) as u8 } else { 128 };
                let plane_width = frame.plane_width(plane).max(1) as usize;
                for row in 0..frame.plane_height(plane).max(1) {
                    let stride = frame.stride(plane);
                    let row_start = row as usize * stride;
                    let data = frame.data_mut(plane);
                    data[row_start..row_start + plane_width].fill(value);
                }
            }
            frame.set_pts(Some(index as i64));
            encoder.send_frame(&frame).unwrap();
            let mut packet = ffmpeg::Packet::empty();
            while encoder.receive_packet(&mut packet).is_ok() {
                packets.push((
                    packet.pts().unwrap_or(i64::MAX),
                    packet.is_key(),
                    packet.data().map(<[u8]>::to_vec).unwrap_or_default(),
                ));
            }
        }
        encoder.send_eof().unwrap();
        let mut packet = ffmpeg::Packet::empty();
        while encoder.receive_packet(&mut packet).is_ok() {
            packets.push((
                packet.pts().unwrap_or(i64::MAX),
                packet.is_key(),
                packet.data().map(<[u8]>::to_vec).unwrap_or_default(),
            ));
        }

        let extradata = unsafe {
            let context = encoder.as_mut_ptr();
            let size = (*context).extradata_size;
            if size > 0 && !(*context).extradata.is_null() {
                std::slice::from_raw_parts((*context).extradata.cast::<u8>(), size as usize)
                    .to_vec()
            } else {
                Vec::new()
            }
        };
        (packets, extradata)
    }

    /// Decoded presentation times in milliseconds, so assertions do not
    /// depend on the muxer's chosen track timescale.
    fn decoded_video_pts_ms(path: &Path) -> Vec<i64> {
        let mut input = ffmpeg::format::input(path).unwrap();
        let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
        let stream_index = stream.index();
        let time_base = stream.time_base();
        let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
            .unwrap()
            .decoder()
            .video()
            .unwrap();
        let mut decoded = ffmpeg::frame::Video::empty();
        let mut pts = Vec::new();
        for (stream, packet) in input.packets() {
            if stream.index() != stream_index {
                continue;
            }
            decoder.send_packet(&packet).unwrap();
            while decoder.receive_frame(&mut decoded).is_ok() {
                pts.push(decoded.pts().unwrap_or(i64::MIN));
            }
        }
        decoder.send_eof().unwrap();
        while decoder.receive_frame(&mut decoded).is_ok() {
            pts.push(decoded.pts().unwrap_or(i64::MIN));
        }
        pts.iter()
            .map(|ticks| {
                ticks * 1_000 * i64::from(time_base.numerator())
                    / i64::from(time_base.denominator())
            })
            .collect()
    }

    #[test]
    fn external_packets_mux_to_decodable_mp4() {
        // Both with in-band parameters (no extradata) and with the avcC
        // extradata attached to the stream, externally encoded packets must
        // round-trip through the muxer with pixel-push timing semantics.
        for global_header in [false, true] {
            let (packets, extradata) = x264_annexb_packets(32, 32, 10, 4, global_header, false);
            assert_eq!(
                packets.len(),
                4,
                "zerolatency x264 emits one packet per frame"
            );

            let directory = tempfile::tempdir().unwrap();
            let path = directory.path().join("recording.mp4");
            let mut config = encoder_config(path.clone(), ExportFormat::Mp4);
            config.external_video = Some(ExternalVideoTrack {
                extradata: extradata.clone(),
                encoder_name: "test-external".into(),
            });
            let mut encoder = StreamingEncoder::create(config).unwrap();
            // Presentation times 0, 100, 300, 500 ms at 10 fps -> ticks
            // 0, 1, 3, 5 with durations 1, 2, 2, 1 (final flush).
            for (ticks, (_, keyframe, data)) in [0i64, 1, 3, 5].into_iter().zip(&packets) {
                encoder
                    .push_encoded_video_packet((ticks * 100) as u64, *keyframe, data)
                    .unwrap();
            }
            let report = encoder.finish().unwrap();
            assert_eq!(report.encoded_frames, 4);
            assert_eq!(report.coalesced_frames, 0);
            assert_eq!(report.video_encoder, "test-external");
            assert!(report.used_hardware_video_encoder);

            let (decoded, width, height) = decoded_video_frame_count(&path);
            assert_eq!((width, height), (32, 32));
            assert_eq!(decoded, 4, "global_header={global_header}");
            assert_eq!(decoded_video_pts_ms(&path), vec![0, 100, 300, 500]);
        }
    }

    #[test]
    fn external_packet_repeating_pts_drops_new_packet() {
        // All-intra frames so any subset of packets decodes independently.
        let (packets, _extradata) = x264_annexb_packets(32, 32, 10, 3, false, true);
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("recording.mp4");
        let mut config = encoder_config(path.clone(), ExportFormat::Mp4);
        config.external_video = Some(ExternalVideoTrack {
            extradata: Vec::new(),
            encoder_name: "test-external".into(),
        });
        let mut encoder = StreamingEncoder::create(config).unwrap();
        let mut ticks = [0i64, 1, 2].into_iter().zip(&packets);
        let (_, (_, keyframe, data)) = ticks.next().unwrap();
        encoder
            .push_encoded_video_packet(0, *keyframe, data)
            .unwrap();
        // A push repeating the pending presentation timestamp must not
        // replace the already scheduled packet.
        let (_, (_, keyframe, data)) = ticks.next().unwrap();
        encoder
            .push_encoded_video_packet(0, *keyframe, data)
            .unwrap();
        let (_, (_, keyframe, data)) = ticks.next().unwrap();
        encoder
            .push_encoded_video_packet((2 * 100) as u64, *keyframe, data)
            .unwrap();
        let report = encoder.finish().unwrap();
        assert_eq!(report.encoded_frames, 2);
        assert_eq!(report.coalesced_frames, 1);
        let (decoded, _, _) = decoded_video_frame_count(&path);
        assert_eq!(decoded, 2);
        assert_eq!(decoded_video_pts_ms(&path), vec![0, 200]);
    }
}

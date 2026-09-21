use std::collections::VecDeque;
use std::fs::{self, OpenOptions};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use ffmpeg_next as ffmpeg;
use snow_recording_model::{VideoCodec, VideoEncodeConfig};
use uuid::Uuid;

use crate::config::{ExportExecutionMode, ExportFormat, SoftwareH264Priority};
use crate::editing::{
    choose_audio_channel_layout, choose_audio_codec, choose_audio_sample_format,
    choose_audio_sample_rate, choose_video_pixel_format, configure_codec_threads,
    drain_audio_packets_with_callback, effective_audio_bitrate_kbps, effective_video_config,
    ensure_video_frame_writable, is_hardware_h264_encoder, open_audio_encoder, open_video_encoder,
    opened_video_encoder_uses_hardware, select_video_codec,
};
use crate::error::{RecordingExportError, Result};
#[cfg(test)]
use crate::ffmpeg_util::copy_rgba_into_frame;
use crate::ffmpeg_util::ensure_ffmpeg_initialized;
use crate::video_quality::smart_quality_bitrate_bps;

pub const DIRECT_STAGING_PREFIX: &str = ".snow-recording-direct-";
const STALE_STAGING_AGE: Duration = Duration::from_secs(24 * 60 * 60);
mod recovery;

/// Failure points available only to tests and the opt-in recording benchmark.
#[cfg(any(test, feature = "bench-experiments"))]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GpuFailureStage {
    Capture,
    CaptureDisconnected,
    Composition,
    Submission,
    Drain,
    Stop,
}

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

#[derive(Clone, Debug)]
pub struct StreamingEncoderConfig {
    /// Whether animated image outputs repeat indefinitely.
    pub loop_animated_images: bool,
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
}

/// Startup policies separate from the stable streaming configuration. This
/// builder contains only Rust configuration; create it on any thread and open
/// the encoder on its owning thread with `create`.
#[derive(Clone)]
pub struct StreamingEncoderBuilder {
    config: StreamingEncoderConfig,
    software_fallback_threads: Option<u8>,
    force_hardware_failure: bool,
    recoverable: bool,
    #[cfg(windows)]
    gpu_input: Option<crate::gpu::GpuInputConfig>,
    #[cfg(target_os = "macos")]
    native_input: Option<snow_media::PixelFormat>,
    #[cfg(target_os = "macos")]
    cpu_hdr_input: bool,
}

impl From<StreamingEncoderConfig> for StreamingEncoderBuilder {
    fn from(config: StreamingEncoderConfig) -> Self {
        Self {
            config,
            software_fallback_threads: None,
            force_hardware_failure: false,
            recoverable: false,
            #[cfg(windows)]
            gpu_input: None,
            #[cfg(target_os = "macos")]
            native_input: None,
            #[cfg(target_os = "macos")]
            cpu_hdr_input: false,
        }
    }
}

impl StreamingEncoderBuilder {
    #[cfg(target_os = "macos")]
    pub fn native_input(mut self, format: snow_media::PixelFormat) -> Self {
        self.native_input = Some(format);
        self.cpu_hdr_input = false;
        self
    }
    /// CPU P010 input already rendered as BT.2020/PQ. No SDR relabeling.
    #[cfg(target_os = "macos")]
    pub fn hdr10_cpu_input(mut self) -> Self {
        self.native_input = None;
        self.cpu_hdr_input = true;
        self
    }
    /// Stage independent video segments and continuous audio for live recovery.
    pub fn recoverable(mut self) -> Self {
        self.recoverable = true;
        self
    }
    /// Select the complete CPU fallback after native-surface negotiation fails.
    pub fn software_only(mut self) -> Self {
        self.config.prefer_hardware_h264 = false;
        self.config.execution_mode = ExportExecutionMode::SoftwareOnly;
        self.config.software_h264_priority = SoftwareH264Priority::X264First;
        if let Some(threads) = self.software_fallback_threads {
            self.config.encode_threads = threads;
        }
        #[cfg(windows)]
        {
            self.gpu_input = None;
        }
        #[cfg(target_os = "macos")]
        {
            self.cpu_hdr_input |= self.native_input == Some(snow_media::PixelFormat::P010);
            self.native_input = None;
        }
        self
    }
    #[cfg(windows)]
    pub fn gpu_input(mut self, config: crate::gpu::GpuInputConfig) -> Self {
        self.gpu_input = Some(config);
        self
    }
    /// Select threads for a software encoder opened under hardware preference,
    /// including software selected when no hardware codec is available. Zero
    /// uses the exporter's physical-core policy. The initial hardware and audio
    /// settings remain those in `StreamingEncoderConfig::encode_threads`.
    pub fn software_fallback_threads(mut self, threads: u8) -> Self {
        self.software_fallback_threads = Some(threads);
        self
    }

    #[cfg(feature = "bench-experiments")]
    pub fn force_hardware_failure(mut self, enabled: bool) -> Self {
        self.force_hardware_failure = enabled;
        self
    }

    pub fn create(self) -> Result<StreamingEncoder> {
        #[cfg(target_os = "macos")]
        if self.recoverable && (self.native_input.is_some() || self.cpu_hdr_input) {
            return Err(RecordingExportError::InvalidConfig(
                "native input recovery is not available".into(),
            ));
        }
        if self.recoverable {
            return recovery::create(self);
        }
        StreamingEncoder::create_inner(
            self.config,
            self.force_hardware_failure,
            self.software_fallback_threads,
            #[cfg(windows)]
            self.gpu_input,
            #[cfg(target_os = "macos")]
            self.native_input,
            #[cfg(target_os = "macos")]
            self.cpu_hdr_input,
        )
    }
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
                return Err("animated streaming formats do not support audio".to_string());
            }
            audio.validate()?;
        }
        self.video.validate("video")?;
        Ok(())
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StreamingEncoderReport {
    pub recovery_count: u32,
    pub video_packets: u64,
    pub abandoned_frames: u64,
    pub recovery_reason: Option<String>,
    pub requested_video_encoder: String,
    pub queued_video_replacements: u64,
    pub conversion_threads: u8,
    /// Backend selection is separate from the worker count reported by FFmpeg.
    pub conversion_backend: String,
    pub effective_conversion_threads: Option<usize>,
    pub pixel_format: String,
    pub effective_encode_threads: usize,
    pub hardware_fallback: bool,
    #[cfg(feature = "bench-timing")]
    pub timings: crate::bench_timing::EncoderTimings,
    pub encoded_frames: u64,
    pub coalesced_frames: u64,
    pub video_encoder: String,
    pub used_hardware_video_encoder: bool,
    pub audio_encoder: Option<String>,
    pub encoded_audio_frames: u64,
    pub inserted_silence_frames: u64,
    pub dropped_audio_frames: u64,
}

/// Read the initialized context's worker setting. Zero/unknown automatic
/// settings remain unknown instead of being reported as zero active workers.
///
/// # Safety
/// `context` must refer to a live initialized SwsContext for the duration of this call.
pub(crate) unsafe fn swscale_thread_count(
    context: *const ffmpeg::ffi::SwsContext,
) -> Option<usize> {
    let mut count = 0i64;
    // SAFETY: the caller keeps the SwsContext alive; FFmpeg only reads options.
    let code = unsafe {
        ffmpeg::ffi::av_opt_get_int(
            context.cast_mut().cast(),
            c"threads".as_ptr(),
            0,
            &mut count,
        )
    };
    if code >= 0 && count > 0 {
        usize::try_from(count).ok()
    } else {
        None
    }
}

struct PendingFrame {
    pts: i64,
    rgba: Vec<u8>,
    #[cfg(any(windows, target_os = "macos"))]
    gpu: Option<ffmpeg::frame::Video>,
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

pub struct StreamingEncoder {
    #[cfg(any(test, feature = "bench-experiments"))]
    injected_failure: Option<GpuFailureStage>,
    recovery: Option<recovery::RecoveryState>,
    audio_output: Option<ffmpeg::format::context::Output>,
    output: Option<ffmpeg::format::context::Output>,
    encoder: ffmpeg::encoder::video::Encoder,
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
    #[cfg(any(test, feature = "bench-experiments"))]
    frame_converter: Option<crate::frame_converter::FrameConverter>,
    scaler: Option<ffmpeg::software::scaling::Context>,
    #[cfg(target_os = "macos")]
    native_input: Option<snow_media::PixelFormat>,
    #[cfg(target_os = "macos")]
    cpu_hdr_input: bool,
    #[cfg(windows)]
    hardware_frames: Option<crate::gpu::HardwareFrames>,
    #[cfg(target_os = "macos")]
    hdr_source: Option<ffmpeg::frame::Video>,
    encode_frame: ffmpeg::frame::Video,
    width: u32,
    height: u32,
    fps: u32,
    pending: Option<PendingFrame>,
    admitted_frames: u64,
    spare_rgba: Vec<u8>,
    pending_packet_durations: VecDeque<i64>,
    pending_timed_durations: VecDeque<(i64, i64)>,
    audio: Option<StreamingAudioState>,
    staging_path: Option<PathBuf>,
    retain_failed_output: bool,
    final_path: PathBuf,
    report: StreamingEncoderReport,
    finished: bool,
}

impl StreamingEncoder {
    #[cfg(target_os = "macos")]
    pub fn poll_native_packets(&mut self) -> Result<()> {
        if self.native_input.is_some() {
            self.drain_available_packets()?;
        }
        Ok(())
    }
    /// Integer frame rate defining the PTS and finalization timeline.
    pub fn fps(&self) -> u32 {
        self.fps
    }

    #[cfg(any(test, feature = "bench-experiments"))]
    pub fn inject_gpu_failure(&mut self, stage: GpuFailureStage) {
        self.injected_failure = Some(stage);
    }

    #[cfg(any(test, feature = "bench-experiments"))]
    fn check_injected_failure(&mut self, stage: GpuFailureStage) -> Result<()> {
        if self.injected_failure == Some(stage) {
            self.injected_failure = None;
            return Err(RecordingExportError::Encode(format!(
                "injected GPU {stage:?} failure"
            )));
        }
        Ok(())
    }
    /// Identity of the encoder that actually opened, including hardware fallback.
    /// This is fixed before any frame is admitted.
    pub fn opened_video_encoder(&self) -> (&str, bool) {
        (
            &self.report.video_encoder,
            self.report.used_hardware_video_encoder,
        )
    }

    pub fn create(config: StreamingEncoderConfig) -> Result<Self> {
        Self::builder(config).create()
    }

    pub fn builder(config: StreamingEncoderConfig) -> StreamingEncoderBuilder {
        config.into()
    }

    #[cfg(feature = "bench-experiments")]
    pub fn create_for_benchmark(
        config: StreamingEncoderConfig,
        force_hardware_failure: bool,
    ) -> Result<Self> {
        Self::builder(config)
            .force_hardware_failure(force_hardware_failure)
            .create()
    }

    fn create_inner(
        config: StreamingEncoderConfig,
        force_hardware_failure: bool,
        software_fallback_threads: Option<u8>,
        #[cfg(windows)] gpu_input: Option<crate::gpu::GpuInputConfig>,
        #[cfg(target_os = "macos")] native_input: Option<snow_media::PixelFormat>,
        #[cfg(target_os = "macos")] cpu_hdr_input: bool,
    ) -> Result<Self> {
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
        match Self::create_at_path(
            config,
            staging_path.clone(),
            force_hardware_failure,
            software_fallback_threads,
            #[cfg(windows)]
            gpu_input,
            #[cfg(target_os = "macos")]
            native_input,
            #[cfg(target_os = "macos")]
            cpu_hdr_input,
            false,
        ) {
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

    fn create_at_path(
        config: StreamingEncoderConfig,
        staging_path: PathBuf,
        force_hardware_failure: bool,
        software_fallback_threads: Option<u8>,
        #[cfg(windows)] gpu_input: Option<crate::gpu::GpuInputConfig>,
        #[cfg(target_os = "macos")] native_input: Option<snow_media::PixelFormat>,
        #[cfg(target_os = "macos")] cpu_hdr_input: bool,
        fragmented: bool,
    ) -> Result<Self> {
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
        #[cfg(windows)]
        let hardware_frames = gpu_input
            .map(|input| {
                crate::gpu::HardwareFrames::new(
                    input,
                    (config.width, config.height),
                    crate::gpu::HardwareFrames::LIVE_CAPACITY,
                )
            })
            .transpose()?;
        #[cfg(windows)]
        let gpu_enabled = hardware_frames.is_some();
        #[cfg(target_os = "macos")]
        let gpu_enabled = native_input.is_some();
        #[cfg(not(any(windows, target_os = "macos")))]
        let gpu_enabled = false;
        let mut video_codec = select_video_codec(
            &output,
            &staging_path,
            config.format,
            config.codec,
            config.prefer_hardware_h264,
            config.execution_mode,
            config.software_h264_priority,
        )?;
        #[cfg(windows)]
        if let Some(frames) = &hardware_frames {
            if config.format != ExportFormat::Mp4
                || config.codec != VideoCodec::H264
                || config.execution_mode == ExportExecutionMode::SoftwareOnly
            {
                return Err(RecordingExportError::InvalidConfig(
                    "GPU input requires hardware-preferred H.264 MP4".into(),
                ));
            }
            video_codec = ffmpeg::encoder::find_by_name(frames.codec_name()).ok_or_else(|| {
                RecordingExportError::Encode(format!(
                    "{} is absent from the FFmpeg build",
                    frames.codec_name()
                ))
            })?;
        }
        #[cfg(target_os = "macos")]
        if let Some(format) = native_input {
            if config.format != ExportFormat::Mp4
                || config.execution_mode == ExportExecutionMode::SoftwareOnly
                || !matches!(
                    format,
                    snow_media::PixelFormat::Bgra8 | snow_media::PixelFormat::P010
                )
                || (format == snow_media::PixelFormat::P010 && config.codec != VideoCodec::H265)
            {
                return Err(RecordingExportError::InvalidConfig(
                    "native input requires hardware MP4 with BGRA SDR or HEVC P010 HDR10".into(),
                ));
            }
            video_codec = ffmpeg::encoder::find_by_name(match config.codec {
                VideoCodec::H264 => "h264_videotoolbox",
                VideoCodec::H265 => "hevc_videotoolbox",
            })
            .ok_or_else(|| {
                RecordingExportError::Encode("VideoToolbox encoder absent from FFmpeg".into())
            })?;
        }
        #[cfg(target_os = "macos")]
        if cpu_hdr_input && (config.codec != VideoCodec::H265 || config.format != ExportFormat::Mp4)
        {
            return Err(RecordingExportError::InvalidConfig(
                "CPU HDR input requires HEVC MP4".into(),
            ));
        }
        let requested_video_encoder = video_codec.name().to_owned();
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
        #[cfg(target_os = "macos")]
        if cpu_hdr_input {
            pixel_format = crate::hdr::pixel_format(
                video_codec
                    .video()
                    .map_err(|e| RecordingExportError::Encode(e.to_string()))?,
            )?;
        }
        #[cfg(windows)]
        if let Some(frames) = &hardware_frames {
            pixel_format = frames.pixel_format();
        }
        #[cfg(target_os = "macos")]
        if native_input.is_some() {
            pixel_format = ffmpeg::format::Pixel::VIDEOTOOLBOX;
        }
        let effective_video = effective_video_config(&config.video);
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
            #[cfg(target_os = "macos")]
            if let Some(format) = native_input {
                unsafe {
                    (*encoder.as_mut_ptr()).sw_pix_fmt = match format {
                        snow_media::PixelFormat::P010 => {
                            ffmpeg::ffi::AVPixelFormat::AV_PIX_FMT_P010LE
                        }
                        _ => ffmpeg::ffi::AVPixelFormat::AV_PIX_FMT_BGRA,
                    };
                }
            }
            encoder.set_time_base(video_time_base);
            encoder.set_frame_rate(Some(video_frame_rate));
            if !config.format.is_animated_image() {
                unsafe {
                    let context = encoder.as_mut_ptr();
                    (*context).color_range = ffmpeg::ffi::AVColorRange::AVCOL_RANGE_MPEG;
                    (*context).colorspace = ffmpeg::ffi::AVColorSpace::AVCOL_SPC_BT709;
                    (*context).color_primaries = ffmpeg::ffi::AVColorPrimaries::AVCOL_PRI_BT709;
                    (*context).color_trc =
                        ffmpeg::ffi::AVColorTransferCharacteristic::AVCOL_TRC_BT709;
                }
            }
            #[cfg(target_os = "macos")]
            if native_input == Some(snow_media::PixelFormat::Bgra8) {
                unsafe {
                    (*encoder.as_mut_ptr()).color_trc =
                        ffmpeg::ffi::AVColorTransferCharacteristic::AVCOL_TRC_IEC61966_2_1;
                }
            }
            #[cfg(target_os = "macos")]
            if cpu_hdr_input || native_input == Some(snow_media::PixelFormat::P010) {
                unsafe {
                    crate::videotoolbox::set_hdr_context(encoder.as_mut_ptr());
                }
            }
            // Resolve against the codec being opened, never an already-opened
            // hardware context's effective thread count. The fallback may also
            // have been selected during codec discovery, before an open fails.
            let encode_threads = if config.prefer_hardware_h264 && !is_hardware_h264_encoder(&codec)
            {
                software_fallback_threads.unwrap_or(config.encode_threads)
            } else {
                config.encode_threads
            };
            configure_codec_threads(
                &mut encoder,
                encode_threads,
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
            #[cfg(windows)]
            if let Some(frames) = &hardware_frames {
                frames.configure(&mut encoder)?;
                return frames.open(encoder, codec, effective_video.quality);
            }
            #[cfg(target_os = "macos")]
            if cpu_hdr_input {
                return crate::editing::open_live_hdr_encoder(encoder, &codec, &effective_video);
            }
            open_video_encoder(encoder, &codec, &effective_video)
        };

        let primary = if force_hardware_failure && is_hardware_h264_encoder(&video_codec) {
            Err(RecordingExportError::Encode(
                "benchmark forced hardware initialization failure".into(),
            ))
        } else {
            make_encoder(video_codec, pixel_format)
        };
        let encoder = match primary {
            Ok(encoder) => encoder,
            Err(primary_error)
                if !gpu_enabled
                    && config.execution_mode != ExportExecutionMode::HardwareOnly
                    && config.prefer_hardware_h264
                    && is_hardware_h264_encoder(&video_codec) =>
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
                #[cfg(target_os = "macos")]
                if cpu_hdr_input {
                    pixel_format = crate::hdr::pixel_format(
                        video_codec
                            .video()
                            .map_err(|e| RecordingExportError::Encode(e.to_string()))?,
                    )?;
                }
                make_encoder(video_codec, pixel_format).map_err(|_| primary_error)?
            }
            Err(error) => return Err(error),
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
            stream.set_parameters(&encoder);
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
        let mut options = ffmpeg::Dictionary::new();
        match config.format {
            ExportFormat::Gif => options.set(
                "loop",
                if config.loop_animated_images {
                    "0"
                } else {
                    "-1"
                },
            ),
            ExportFormat::Apng => options.set(
                "plays",
                if config.loop_animated_images {
                    "0"
                } else {
                    "1"
                },
            ),
            ExportFormat::Webp => options.set(
                "loop",
                if config.loop_animated_images {
                    "0"
                } else {
                    "1"
                },
            ),
            ExportFormat::Mp4 | ExportFormat::Avi => {}
        }
        if fragmented {
            options.set("movflags", "frag_every_frame+empty_moov+default_base_moof");
        }
        output.write_header_with(options).map_err(|error| {
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
        #[cfg(target_os = "macos")]
        let cpu_format = if cpu_hdr_input {
            ffmpeg::format::Pixel::P010LE
        } else {
            ffmpeg::format::Pixel::RGBA
        };
        #[cfg(not(target_os = "macos"))]
        let cpu_format = ffmpeg::format::Pixel::RGBA;
        let mut scaler = if gpu_enabled {
            None
        } else {
            Some(
                ffmpeg::software::scaling::Context::get(
                    cpu_format,
                    config.width,
                    config.height,
                    pixel_format,
                    config.width,
                    config.height,
                    ffmpeg::software::scaling::flag::Flags::BICUBIC,
                )
                .map_err(|error| {
                    RecordingExportError::Encode(format!(
                        "failed to create streaming RGBA scaler: {error}"
                    ))
                })?,
            )
        };
        if !config.format.is_animated_image()
            && let Some(scaler) = scaler.as_mut()
        {
            // RGB capture is full-range; hardware and software use the same
            // limited-range BT.709 matrix and encoder color interpretation.
            if cpu_format == ffmpeg::format::Pixel::P010LE {
                crate::hdr::scaler_colors(scaler, true, false)?;
            } else {
                unsafe {
                    configure_bt709_scaler(scaler.as_mut_ptr())?;
                }
            }
        }

        // SAFETY: the initialized scaler exclusively owns this live context.
        let effective_conversion_threads = scaler
            .as_ref()
            .and_then(|scaler| unsafe { swscale_thread_count(scaler.as_ptr()) });
        let report = StreamingEncoderReport::default();
        #[cfg(feature = "bench-timing")]
        let report = {
            let mut report = report;
            report.timings.begin();
            report
        };
        let used_hardware_video_encoder = opened_video_encoder_uses_hardware(&encoder);
        let effective_encode_threads = unsafe { (*encoder.as_ptr()).thread_count.max(0) as usize };
        Ok(Self {
            #[cfg(any(test, feature = "bench-experiments"))]
            injected_failure: None,
            recovery: None,
            audio_output: None,
            output: Some(output),
            encoder,
            stream_index,
            stream_time_base,
            scaler,
            #[cfg(target_os = "macos")]
            native_input,
            #[cfg(target_os = "macos")]
            cpu_hdr_input,
            #[cfg(target_os = "macos")]
            hdr_source: None,
            #[cfg(windows)]
            hardware_frames,
            #[cfg(any(test, feature = "bench-experiments"))]
            frame_converter: None,
            encode_frame: if gpu_enabled {
                ffmpeg::frame::Video::empty()
            } else {
                ffmpeg::frame::Video::new(pixel_format, config.width, config.height)
            },
            width: config.width,
            height: config.height,
            fps,
            pending: None,
            admitted_frames: 0,
            spare_rgba: Vec::new(),
            pending_packet_durations: VecDeque::new(),
            pending_timed_durations: VecDeque::new(),
            audio,
            staging_path: None,
            retain_failed_output: false,
            final_path: config.output_path,
            report: StreamingEncoderReport {
                requested_video_encoder,
                video_encoder: video_codec.name().to_string(),
                pixel_format: format!("{pixel_format:?}"),
                conversion_threads: 0,
                conversion_backend: if gpu_enabled {
                    if cfg!(target_os = "macos") {
                        "native_surface"
                    } else {
                        "d3d11_video_processor"
                    }
                } else {
                    "legacy_sws_scale"
                }
                .into(),
                effective_conversion_threads,
                effective_encode_threads,
                hardware_fallback: config.prefer_hardware_h264 && !used_hardware_video_encoder,
                used_hardware_video_encoder,
                audio_encoder: config
                    .audio
                    .as_ref()
                    .and_then(|_| choose_audio_codec(config.format))
                    .map(|codec| codec.name().to_string()),
                ..report
            },
            finished: false,
        })
    }

    #[cfg(any(test, feature = "bench-experiments"))]
    pub fn set_conversion_threads(&mut self, threads: u8) -> Result<()> {
        #[cfg(target_os = "macos")]
        if self.cpu_hdr_input {
            return if threads == 0 {
                Ok(())
            } else {
                Err(RecordingExportError::InvalidConfig(
                    "RGBA conversion workers do not support P010 HDR input".into(),
                ))
            };
        }
        if self.scaler.is_none() {
            return if threads == 0 {
                Ok(())
            } else {
                Err(RecordingExportError::InvalidConfig(
                    "CPU conversion workers do not apply to GPU input".into(),
                ))
            };
        }
        if self.pending.is_some() || self.report.encoded_frames != 0 || threads > 4 {
            return Err(RecordingExportError::InvalidConfig(
                "conversion workers require an unstarted encoder and at most four threads".into(),
            ));
        }
        self.frame_converter = if threads == 0 {
            None
        } else {
            Some(crate::frame_converter::FrameConverter::new(
                self.width,
                self.height,
                self.encode_frame.format(),
                threads,
            )?)
        };
        self.report.conversion_threads = threads;
        if unsafe { (*self.encoder.as_ptr()).colorspace }
            == ffmpeg::ffi::AVColorSpace::AVCOL_SPC_BT709
            && let Some(converter) = self.frame_converter.as_mut()
        {
            converter.use_bt709()?;
        }
        self.report.conversion_backend = if self.frame_converter.is_some() {
            "legacy_sws_scale_frame"
        } else {
            "legacy_sws_scale"
        }
        .into();
        self.report.effective_conversion_threads = if let Some(converter) = &self.frame_converter {
            converter.thread_count()
        } else {
            // SAFETY: the initialized scaler owns its live context.
            unsafe { swscale_thread_count(self.scaler.as_ref().expect("CPU scaler").as_ptr()) }
        };
        Ok(())
    }

    #[cfg(feature = "bench-timing")]
    pub fn record_worker_queue_time(&mut self, duration: std::time::Duration) {
        self.report
            .timings
            .stages
            .entry("pipeline.encoder_queue")
            .or_default()
            .push(duration);
    }

    pub fn push_rgba_frame(&mut self, timestamp_ms: u64, rgba: &[u8]) -> Result<()> {
        let pts = ((u128::from(timestamp_ms) * u128::from(self.fps) + 500) / 1_000)
            .min(i64::MAX as u128) as i64;
        self.push_rgba_frame_at_pts(pts as u64, rgba)
    }

    /// Submit using the encoder's integer frame time base without millisecond rounding.
    pub fn push_rgba_frame_at_pts(&mut self, pts: u64, rgba: &[u8]) -> Result<()> {
        let expected = self.width as usize * self.height as usize * 4;
        if rgba.len() != expected {
            return Err(RecordingExportError::InvalidConfig(format!(
                "streaming RGBA frame has {} bytes; expected {expected}",
                rgba.len()
            )));
        }
        let mut owned = std::mem::take(&mut self.spare_rgba);
        owned.resize(expected, 0);
        #[cfg(feature = "bench-timing")]
        let copy_started = std::time::Instant::now();
        owned.copy_from_slice(rgba);
        #[cfg(feature = "bench-timing")]
        {
            self.report.timings.copied_bytes += rgba.len() as u64;
            self.report
                .timings
                .record("encode.pending_copy", copy_started);
        }
        self.spare_rgba = self.push_owned_rgba_frame_at_pts(pts, owned)?;
        Ok(())
    }

    /// Transfer an RGBA image at an integer frame index and return recyclable storage.
    /// Returned pixels are no longer referenced by the encoder. The first call may
    /// return an empty vector; subsequent calls return the previous image's storage.
    /// Storage may grow on first use to provide FFmpeg's SIMD tail padding.
    pub fn push_owned_rgba_frame_at_pts(&mut self, pts: u64, rgba: Vec<u8>) -> Result<Vec<u8>> {
        #[cfg(target_os = "macos")]
        if self.cpu_hdr_input {
            return Err(RecordingExportError::InvalidConfig(
                "HDR encoder requires P010 BT.2020/PQ input".into(),
            ));
        }
        if self.scaler.is_none() {
            return Err(RecordingExportError::InvalidConfig(
                "GPU encoder requires a GPU surface".into(),
            ));
        }
        let pts = i64::try_from(pts)
            .map_err(|_| RecordingExportError::InvalidConfig("video PTS overflow".into()))?;
        let expected = self.width as usize * self.height as usize * 4;
        if rgba.len() != expected {
            return Err(RecordingExportError::InvalidConfig(format!(
                "streaming RGBA frame has {} bytes; expected {expected}",
                rgba.len()
            )));
        }
        #[cfg(feature = "bench-timing")]
        {
            self.report.timings.submitted_pts.push(pts);
            self.report
                .timings
                .admissions
                .push((pts, std::time::Instant::now()));
        }
        if let Some(pending) = self.pending.as_mut() {
            if pts <= pending.pts {
                self.report.coalesced_frames = self.report.coalesced_frames.saturating_add(1);
                return Ok(std::mem::replace(&mut pending.rgba, rgba));
            }
            let duration = pts.saturating_sub(pending.pts).max(1);
            if let Err(error) = self.encode_pending(duration) {
                self.admitted_frames += 1; // The new image could not be queued either.
                return Err(error);
            }
        }
        if let Some(state) = self.recovery.as_mut() {
            state.current_start.get_or_insert(pts);
        }
        self.admitted_frames += 1;
        self.pending = Some(PendingFrame {
            pts,
            rgba,
            #[cfg(any(windows, target_os = "macos"))]
            gpu: None,
        });
        Ok(std::mem::take(&mut self.spare_rgba))
    }

    pub fn push_audio_pcm_i16(&mut self, timestamp_ms: u64, samples: &[i16]) -> Result<()> {
        let audio = self.audio.as_mut().ok_or_else(|| {
            RecordingExportError::InvalidConfig(
                "streaming encoder was created without an audio track".to_string(),
            )
        })?;
        let output = self
            .audio_output
            .as_mut()
            .or(self.output.as_mut())
            .ok_or_else(|| {
                RecordingExportError::Encode("streaming output is already closed".to_string())
            })?;
        audio.push_pcm(timestamp_ms, samples, output, &mut self.report)
    }

    pub fn has_audio(&self) -> bool {
        self.audio.is_some()
    }

    /// Poll asynchronous native encoders even when the desktop has not changed.
    /// This also releases referenced surfaces when the bounded pool is full.
    #[cfg(windows)]
    pub fn poll_gpu_packets(&mut self) -> Result<()> {
        if self.hardware_frames.is_some() {
            self.drain_available_packets()?;
        }
        Ok(())
    }

    /// Before static-frame coalescing, establish an image that survives loss of
    /// the device. Pending GPU surfaces alone cannot be recovered after removal.
    #[cfg(windows)]
    pub fn needs_recovery_image(&self) -> bool {
        self.hardware_frames.is_some() && self.recovery.is_some() && self.report.video_packets == 0
    }

    #[cfg(windows)]
    pub fn allocate_gpu_frame(&self) -> Result<Option<crate::gpu::GpuEncoderFrame>> {
        self.hardware_frames
            .as_ref()
            .ok_or_else(|| RecordingExportError::InvalidConfig("encoder has no GPU input".into()))?
            .allocate()
    }

    /// Submit an immutable CoreVideo lease without CPU readback.
    /// Explicit software HDR transport. Conversion preserves limited-range PQ
    /// code values; this never infers HDR from an SDR image or just changes tags.
    #[cfg(target_os = "macos")]
    pub fn push_cpu_hdr_frame_at_pts(
        &mut self,
        pts: u64,
        image: &snow_media::CpuFrame,
    ) -> Result<()> {
        if !self.cpu_hdr_input
            || self.finished
            || self.output.is_none()
            || image.format != snow_media::PixelFormat::P010
            || image.color != snow_media::ColorDescription::HDR10
            || image.size.width != self.width
            || image.size.height != self.height
            || image.planes.len() != 2
        {
            return Err(RecordingExportError::InvalidConfig(
                "CPU HDR frame does not match the encoder layout/color".into(),
            ));
        }
        let source = self.hdr_source.get_or_insert_with(|| {
            ffmpeg::frame::Video::new(ffmpeg::format::Pixel::P010LE, self.width, self.height)
        });
        for index in 0..2 {
            let rows = self.height as usize / if index == 0 { 1 } else { 2 };
            let row_bytes = self.width as usize * 2;
            let layout = image.planes[index];
            let bytes = image
                .plane_bytes(index)
                .ok_or_else(|| RecordingExportError::InvalidConfig("truncated HDR plane".into()))?;
            if layout.height != rows
                || layout.row_bytes != row_bytes
                || layout.width != self.width as usize / if index == 0 { 1 } else { 2 }
            {
                return Err(RecordingExportError::InvalidConfig(
                    "invalid HDR plane dimensions".into(),
                ));
            }
            let stride = source.stride(index);
            for row in 0..rows {
                source.data_mut(index)[row * stride..row * stride + row_bytes]
                    .copy_from_slice(&bytes[row * layout.stride..row * layout.stride + row_bytes]);
            }
        }
        crate::hdr::frame(source, true);
        ensure_video_frame_writable(&mut self.encode_frame)?;
        self.scaler
            .as_mut()
            .ok_or_else(|| {
                RecordingExportError::InvalidConfig("HDR CPU scaler unavailable".into())
            })?
            .run(source, &mut self.encode_frame)
            .map_err(|error| RecordingExportError::Encode(error.to_string()))?;
        crate::hdr::frame(&mut self.encode_frame, true);
        // av_frame_ref shares immutable pixels until the encoder releases them.
        // make_writable above allocates only when the previous lease is retained.
        let mut frame = ffmpeg::frame::Video::empty();
        let result =
            unsafe { ffmpeg::ffi::av_frame_ref(frame.as_mut_ptr(), self.encode_frame.as_ptr()) };
        if result < 0 {
            return Err(RecordingExportError::Encode(
                ffmpeg::Error::from(result).to_string(),
            ));
        }
        self.admit_prepared_frame(pts, frame)
    }

    #[cfg(target_os = "macos")]
    pub fn push_native_frame_at_pts(
        &mut self,
        pts: u64,
        image: snow_media::macos::PixelBuffer,
    ) -> Result<()> {
        if self.finished || self.output.is_none() {
            return Err(RecordingExportError::InvalidConfig(
                "encoder is closed".into(),
            ));
        }
        if self.native_input != Some(image.format())
            || image.size().width != self.width
            || image.size().height != self.height
        {
            return Err(RecordingExportError::InvalidConfig(
                "native frame does not match encoder layout".into(),
            ));
        }
        let native = crate::videotoolbox::frame(image)?;
        self.admit_prepared_frame(pts, native)
    }

    #[cfg(target_os = "macos")]
    fn admit_prepared_frame(&mut self, pts: u64, native: ffmpeg::frame::Video) -> Result<()> {
        let pts = i64::try_from(pts)
            .map_err(|_| RecordingExportError::InvalidConfig("video PTS overflow".into()))?;
        if let Some(pending) = &mut self.pending {
            if pts < pending.pts {
                return Err(RecordingExportError::InvalidConfig(
                    "video PTS moved backward".into(),
                ));
            }
            if pts == pending.pts {
                pending.gpu = Some(native);
                self.report.coalesced_frames += 1;
                return Ok(());
            }
            let duration = pts - pending.pts;
            self.encode_pending(duration)?;
        }
        self.admitted_frames += 1;
        self.pending = Some(PendingFrame {
            pts,
            rgba: Vec::new(),
            gpu: Some(native),
        });
        Ok(())
    }

    #[cfg(windows)]
    pub fn push_gpu_frame_at_pts(
        &mut self,
        pts: u64,
        frame: crate::gpu::GpuEncoderFrame,
    ) -> Result<()> {
        let pts = i64::try_from(pts)
            .map_err(|_| RecordingExportError::InvalidConfig("video PTS overflow".into()))?;
        let native = self
            .hardware_frames
            .as_ref()
            .ok_or_else(|| RecordingExportError::InvalidConfig("encoder has no GPU input".into()))?
            .encode_frame(frame)?;
        if let Some(pending) = &mut self.pending {
            if pts <= pending.pts {
                pending.gpu = Some(native);
                self.report.coalesced_frames += 1;
                return Ok(());
            }
            let duration = pts - pending.pts;
            if let Err(error) = self.encode_pending(duration) {
                self.admitted_frames += 1; // The new image could not be queued either.
                return Err(error);
            }
        }
        if let Some(state) = self.recovery.as_mut() {
            state.current_start.get_or_insert(pts);
        }
        self.admitted_frames += 1;
        self.pending = Some(PendingFrame {
            pts,
            rgba: Vec::new(),
            gpu: Some(native),
        });
        Ok(())
    }

    #[cfg(any(windows, target_os = "macos"))]
    fn encode_gpu_pending(
        &mut self,
        mut frame: ffmpeg::frame::Video,
        pts: i64,
        duration: i64,
    ) -> Result<()> {
        #[cfg(any(test, feature = "bench-experiments"))]
        self.check_injected_failure(GpuFailureStage::Submission)?;
        frame.set_pts(Some(pts));
        unsafe {
            (*frame.as_mut_ptr()).duration = duration;
            #[cfg(windows)]
            {
                (*frame.as_mut_ptr()).color_range = ffmpeg::ffi::AVColorRange::AVCOL_RANGE_MPEG;
                (*frame.as_mut_ptr()).colorspace = ffmpeg::ffi::AVColorSpace::AVCOL_SPC_BT709;
                (*frame.as_mut_ptr()).color_primaries =
                    ffmpeg::ffi::AVColorPrimaries::AVCOL_PRI_BT709;
                (*frame.as_mut_ptr()).color_trc =
                    ffmpeg::ffi::AVColorTransferCharacteristic::AVCOL_TRC_BT709;
            }
        }
        #[cfg(windows)]
        let device = self
            .hardware_frames
            .as_ref()
            .expect("GPU frames")
            .device
            .clone();
        #[cfg(windows)]
        {
            let _lock = device.lock();
            unsafe { device.context().Flush() };
        }
        #[cfg(feature = "bench-timing")]
        let send_started = std::time::Instant::now();
        #[cfg(feature = "bench-timing")]
        self.report.timings.submissions.push((pts, send_started));
        retry_send(
            self,
            |state| state.encoder.send_frame(&frame),
            |state| state.drain_available_packets(),
        )?;
        #[cfg(feature = "bench-timing")]
        {
            #[cfg(target_os = "macos")]
            let stage = if self.cpu_hdr_input {
                "encode.cpu_hdr_send"
            } else {
                "encode.gpu_send"
            };
            #[cfg(windows)]
            let stage = "encode.gpu_send";
            self.report.timings.record(stage, send_started);
        }
        self.pending_packet_durations.push_back(duration.max(1));
        #[cfg(feature = "bench-timing")]
        {
            #[cfg(target_os = "macos")]
            let native_submission = self.native_input.is_some();
            #[cfg(windows)]
            let native_submission = true;
            if native_submission {
                self.report.timings.gpu_surface_submissions += 1;
            }
        }
        self.pending_timed_durations
            .push_back((pts, duration.max(1)));
        self.drain_available_packets()?;
        self.report.encoded_frames += 1;
        Ok(())
    }

    fn drain_available_packets(&mut self) -> Result<()> {
        self.drain_packets(false)
    }

    fn drain_packets(&mut self, flushing: bool) -> Result<()> {
        #[cfg(any(test, feature = "bench-experiments"))]
        self.check_injected_failure(GpuFailureStage::Drain)?;
        let output = self
            .output
            .as_mut()
            .ok_or_else(|| RecordingExportError::Encode("streaming output is closed".into()))?;
        loop {
            let mut packet = ffmpeg::Packet::empty();
            match self.encoder.receive_packet(&mut packet) {
                Ok(()) => {}
                Err(ffmpeg::Error::Eof) => return Ok(()),
                Err(cause) if crate::ffmpeg_util::is_eagain(&cause) && !flushing => return Ok(()),
                // After accepted EOF the codec must progress to EOF. Never spin
                // indefinitely on a broken driver during asynchronous stop.
                Err(cause) => {
                    return Err(RecordingExportError::Encode(format!(
                        "video packet drain: {cause}"
                    )));
                }
            }
            if let Some(duration) = self.pending_packet_durations.pop_front() {
                packet.set_duration(duration);
            }
            apply_packet_duration(&mut packet, &mut self.pending_timed_durations);
            #[cfg(feature = "bench-timing")]
            self.report.timings.packet(packet.pts());
            packet.set_stream(self.stream_index);
            packet.rescale_ts(self.encoder.time_base(), self.stream_time_base);
            packet.write_interleaved(output).map_err(|cause| {
                RecordingExportError::Encode(format!("video packet mux: {cause}"))
            })?;
            self.report.video_packets += 1;
        }
    }

    /// Preserve a native recording's staging media if final muxing fails.
    /// Explicit cancellation still removes staging files.
    pub fn retain_failed_output(&mut self) {
        self.retain_failed_output = true;
    }

    pub fn preserve_staging_failure(&mut self, cause: &str) -> Option<RecordingExportError> {
        if !self.retain_failed_output || self.report.encoded_frames == 0 {
            return None;
        }
        let path = self.staging_path.as_ref()?.clone();
        // Best effort sealing also makes recordings interrupted during capture playable.
        let _ = self.encode_pending(1);
        if let (Some(audio), Some(output)) = (self.audio.as_mut(), self.output.as_mut()) {
            let _ = audio.finish(output, &mut self.report);
        }
        let _ = self.encoder.send_eof();
        let _ = self.drain_packets(true);
        if let Some(output) = self.output.as_mut() {
            let _ = output.write_trailer();
        }
        self.output.take();
        let directory = path.with_extension("recovery");
        fs::create_dir_all(&directory).ok()?;
        let destination = directory.join(self.final_path.file_name()?);
        fs::rename(&path, &destination).ok()?;
        self.staging_path.take();
        let _ = fs::write(
            directory.join("timeline.txt"),
            format!(
                "fps={}\nframes={}\nmedia={}\nerror={}\n",
                self.fps(),
                self.report.encoded_frames,
                destination.display(),
                cause
            ),
        );
        Some(RecordingExportError::Encode(format!(
            "{cause}; recoverable media is retained in {}",
            directory.display()
        )))
    }

    pub fn finish(self) -> Result<StreamingEncoderReport> {
        self.finish_inner(None, None)
    }

    /// End the last pending image at an exclusive output-frame boundary.
    pub fn finish_at_pts(self, end_pts: u64) -> Result<StreamingEncoderReport> {
        self.finish_at_pts_inner(end_pts, None)
    }

    /// Cancellation may interrupt finalization before publication. The final
    /// rename is serialized with cancel; codec calls themselves may still block.
    pub fn finish_at_pts_cancelable(
        self,
        end_pts: u64,
        cancellation: &snow_core::cancellation::CancellationToken,
    ) -> Result<StreamingEncoderReport> {
        self.finish_at_pts_inner(end_pts, Some(cancellation))
    }

    fn finish_at_pts_inner(
        self,
        end_pts: u64,
        cancellation: Option<&snow_core::cancellation::CancellationToken>,
    ) -> Result<StreamingEncoderReport> {
        let endpoint = i64::try_from(end_pts)
            .map_err(|_| RecordingExportError::InvalidConfig("video endpoint overflow".into()))?;
        if self
            .pending
            .as_ref()
            .is_some_and(|frame| endpoint <= frame.pts)
        {
            return Err(RecordingExportError::InvalidConfig(
                "video endpoint must follow the last submitted PTS".into(),
            ));
        }
        self.finish_inner(Some(endpoint), cancellation)
    }

    fn finish_inner(
        mut self,
        end_pts: Option<i64>,
        cancellation: Option<&snow_core::cancellation::CancellationToken>,
    ) -> Result<StreamingEncoderReport> {
        if cancellation.is_some_and(|token| token.is_canceled()) {
            return Err(RecordingExportError::ExportCanceled);
        }
        if self.recovery.is_some() {
            let endpoint =
                end_pts.unwrap_or_else(|| self.pending.as_ref().map_or(1, |frame| frame.pts + 1));
            return self.finish_recoverable(endpoint, cancellation);
        }
        let result = self.finish_stream(end_pts, cancellation);
        match result {
            Err(error) if !cancellation.is_some_and(|token| token.is_canceled()) => Err(self
                .preserve_staging_failure(&error.to_string())
                .unwrap_or(error)),
            result => result,
        }
    }

    fn finish_stream(
        &mut self,
        end_pts: Option<i64>,
        cancellation: Option<&snow_core::cancellation::CancellationToken>,
    ) -> Result<StreamingEncoderReport> {
        #[cfg(feature = "bench-timing")]
        let finish_started = std::time::Instant::now();
        let duration = self
            .pending
            .as_ref()
            .and_then(|frame| end_pts.map(|end| end.saturating_sub(frame.pts).max(1)))
            .unwrap_or(1);
        self.encode_pending(duration)?;
        if let Some(audio) = self.audio.as_mut() {
            let output = self.output.as_mut().ok_or_else(|| {
                RecordingExportError::Encode("streaming output is already closed".to_string())
            })?;
            audio.finish(output, &mut self.report)?;
        }
        retry_send(
            self,
            |state| state.encoder.send_eof(),
            |state| state.drain_available_packets(),
        )?;
        self.drain_packets(true)?;
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
        if let Err(error) = publish_unless_canceled(&staging_path, &self.final_path, cancellation) {
            if self.retain_failed_output && !cancellation.is_some_and(|token| token.is_canceled()) {
                self.staging_path = Some(staging_path);
            } else {
                let _ = fs::remove_file(&staging_path);
            }
            return Err(error);
        }
        self.finished = true;
        #[cfg(feature = "bench-timing")]
        self.report
            .timings
            .record("encode.finalize", finish_started);
        Ok(self.report.clone())
    }

    fn encode_pending(&mut self, duration: i64) -> Result<()> {
        let Some(mut pending) = self.pending.take() else {
            return Ok(());
        };
        #[cfg(any(windows, target_os = "macos"))]
        if let Some(frame) = pending.gpu.take() {
            return self.encode_gpu_pending(frame, pending.pts, duration);
        }
        #[cfg(feature = "bench-timing")]
        let prepare_started = std::time::Instant::now();
        #[cfg(feature = "bench-timing")]
        let original_storage = (
            pending.rgba.as_ptr(),
            pending.rgba.len(),
            pending.rgba.capacity(),
        );
        pad_owned_rgba(&mut pending.rgba);
        #[cfg(feature = "bench-timing")]
        {
            if pending.rgba.as_ptr() != original_storage.0 {
                self.report.timings.copied_bytes += original_storage.1 as u64;
            }
            if pending.rgba.capacity() != original_storage.2 {
                self.report
                    .timings
                    .record("encode.rgba_storage_growth", prepare_started);
            }
            self.report.timings.encoded_pts.push(pending.pts);
            self.report
                .timings
                .record("encode.rgba_prepare", prepare_started);
        }
        #[cfg(feature = "bench-timing")]
        let convert_started = std::time::Instant::now();
        ensure_video_frame_writable(&mut self.encode_frame)?;
        #[cfg(feature = "bench-timing")]
        self.report
            .timings
            .record("encode.writable", convert_started);
        #[cfg(any(test, feature = "bench-experiments"))]
        if let Some(converter) = self.frame_converter.as_mut() {
            #[cfg(feature = "bench-timing")]
            let writable_started = std::time::Instant::now();
            converter.ensure_input_writable()?;
            #[cfg(feature = "bench-timing")]
            self.report
                .timings
                .record("encode.input_writable", writable_started);
            #[cfg(feature = "bench-timing")]
            let copy_started = std::time::Instant::now();
            converter.copy_input(&pending.rgba)?;
            #[cfg(feature = "bench-timing")]
            {
                self.report
                    .timings
                    .record("encode.input_copy", copy_started);
                self.report.timings.copied_bytes +=
                    u64::from(self.width) * u64::from(self.height) * 4;
            }
        }
        #[cfg(feature = "bench-timing")]
        let color_started = std::time::Instant::now();
        #[cfg(any(test, feature = "bench-experiments"))]
        if let Some(converter) = self.frame_converter.as_mut() {
            converter.convert_prepared(&mut self.encode_frame)?;
        } else {
            convert_owned_rgba(
                self.scaler.as_mut().expect("CPU scaler"),
                &pending.rgba,
                &mut self.encode_frame,
            )?;
        }
        #[cfg(not(any(test, feature = "bench-experiments")))]
        convert_owned_rgba(
            self.scaler.as_mut().expect("CPU scaler"),
            &pending.rgba,
            &mut self.encode_frame,
        )?;
        #[cfg(feature = "bench-timing")]
        self.report
            .timings
            .record("encode.convert", convert_started);
        #[cfg(feature = "bench-timing")]
        self.report
            .timings
            .record("encode.color_convert", color_started);
        self.encode_frame.set_pts(Some(pending.pts));
        #[cfg(feature = "bench-timing")]
        {
            self.report.timings.cpu_conversions += 1;
        }
        #[cfg(feature = "bench-timing")]
        self.report
            .timings
            .submissions
            .push((pending.pts, std::time::Instant::now()));
        #[cfg(feature = "bench-timing")]
        let send_started = std::time::Instant::now();
        self.encoder
            .send_frame(&self.encode_frame)
            .map_err(|error| {
                RecordingExportError::Encode(format!(
                    "failed to send streaming video frame: {error}"
                ))
            })?;
        #[cfg(feature = "bench-timing")]
        self.report.timings.record("encode.send", send_started);
        #[cfg(feature = "bench-timing")]
        let drain_started = std::time::Instant::now();
        self.pending_packet_durations.push_back(duration.max(1));
        self.pending_timed_durations
            .push_back((pending.pts, duration.max(1)));
        self.drain_available_packets()?;
        #[cfg(feature = "bench-timing")]
        self.report
            .timings
            .record("encode.receive_mux", drain_started);
        pending
            .rgba
            .truncate(self.width as usize * self.height as usize * 4);
        self.spare_rgba = pending.rgba;
        self.report.encoded_frames = self.report.encoded_frames.saturating_add(1);
        Ok(())
    }
}

fn pad_owned_rgba(rgba: &mut Vec<u8>) {
    // swscale permits SIMD reads past the last plane. Reserve initialized tail
    // padding without doubling a full-size image's capacity on its first use.
    let padding = ffmpeg::ffi::AV_INPUT_BUFFER_PADDING_SIZE as usize;
    rgba.reserve_exact(padding);
    rgba.resize(rgba.len() + padding, 0);
}

// FFmpeg promises progress after receive when send returns EAGAIN. Keep the
// exact input alive across the drain and retry; a second EAGAIN is an error,
// not permission to drop or replace the scheduled image.
fn retry_send<S>(
    state: &mut S,
    mut send: impl FnMut(&mut S) -> std::result::Result<(), ffmpeg::Error>,
    drain: impl FnOnce(&mut S) -> Result<()>,
) -> Result<()> {
    match send(state) {
        Err(cause) if crate::ffmpeg_util::is_eagain(&cause) => {
            drain(state)?;
            send(state).map_err(|cause| {
                RecordingExportError::Encode(format!("video submission retry: {cause}"))
            })
        }
        result => result
            .map_err(|cause| RecordingExportError::Encode(format!("video submission: {cause}"))),
    }
}

#[cfg(test)]
mod submission_tests {
    use super::*;
    #[test]
    fn eagain_drains_then_retries_the_same_input_and_preserves_failures() {
        for drain_fails in [false, true] {
            let frame = [17u8; 8];
            let identity = frame.as_ptr();
            let mut events = Vec::new();
            let result = retry_send(
                &mut events,
                |events| {
                    assert_eq!(frame.as_ptr(), identity);
                    let first = events.is_empty();
                    events.push("send");
                    if first {
                        Err(ffmpeg::Error::Other {
                            errno: ffmpeg::error::EAGAIN,
                        })
                    } else {
                        Ok(())
                    }
                },
                |events| {
                    events.push("drain");
                    if drain_fails {
                        Err(RecordingExportError::Encode("drain failed".into()))
                    } else {
                        Ok(())
                    }
                },
            );
            assert_eq!(result.is_err(), drain_fails);
            assert_eq!(
                events,
                if drain_fails {
                    vec!["send", "drain"]
                } else {
                    vec!["send", "drain", "send"]
                }
            );
        }
    }
}

/// # Safety
/// `context` is a live, exclusively owned, initialized SwsContext.
pub(crate) unsafe fn configure_bt709_scaler(context: *mut ffmpeg::ffi::SwsContext) -> Result<()> {
    let code = unsafe {
        let matrix = ffmpeg::ffi::sws_getCoefficients(ffmpeg::ffi::SWS_CS_ITU709);
        ffmpeg::ffi::sws_setColorspaceDetails(context, matrix, 1, matrix, 0, 0, 1 << 16, 1 << 16)
    };
    if code < 0 {
        return Err(RecordingExportError::Encode(format!(
            "BT.709 conversion setup: {}",
            ffmpeg::Error::from(code)
        )));
    }
    Ok(())
}

fn convert_owned_rgba(
    scaler: &mut ffmpeg::software::scaling::Context,
    rgba: &[u8],
    output: &mut ffmpeg::frame::Video,
) -> Result<()> {
    let input = scaler.input();
    let stride = input
        .width
        .checked_mul(4)
        .and_then(|value| i32::try_from(value).ok())
        .ok_or_else(|| RecordingExportError::InvalidConfig("RGBA row size overflow".into()))?;
    let expected = stride as usize * input.height as usize;
    if input.format != ffmpeg::format::Pixel::RGBA
        || rgba.len() < expected + ffmpeg::ffi::AV_INPUT_BUFFER_PADDING_SIZE as usize
        || output.format() != scaler.output().format
        || (output.width(), output.height()) != (scaler.output().width, scaler.output().height)
    {
        return Err(RecordingExportError::InvalidConfig(
            "invalid owned RGBA conversion layout".into(),
        ));
    }
    let height = input.height;
    let mut source = [std::ptr::null(); 8];
    source[0] = rgba.as_ptr();
    let mut strides = [0; 8];
    strides[0] = stride;
    // SAFETY: sws_scale is synchronous and does not retain these source pointers.
    // The caller owns the initialized RGBA pixels and SIMD tail padding through
    // this call; each source row has exactly `stride` bytes. The destination is
    // a fully allocated, writable AVFrame matching the scaler output. Encoding
    // references only that separate destination, so RGBA storage can be recycled.
    let rows = unsafe {
        ffmpeg::ffi::sws_scale(
            scaler.as_mut_ptr(),
            source.as_ptr(),
            strides.as_ptr(),
            0,
            height as i32,
            (*output.as_mut_ptr()).data.as_ptr(),
            (*output.as_mut_ptr()).linesize.as_ptr(),
        )
    };
    if rows != output.height() as i32 {
        return Err(RecordingExportError::Encode(format!(
            "owned RGBA conversion returned {rows} rows"
        )));
    }
    Ok(())
}

// Encoders with B-frames emit packets in decode order. Duration belongs to
// the submitted image's PTS, including a prolonged static final image.
fn apply_packet_duration(packet: &mut ffmpeg::Packet, pending: &mut VecDeque<(i64, i64)>) {
    let index = packet
        .pts()
        .map_or(Some(0), |pts| pending.iter().position(|(at, _)| *at == pts));
    if let Some((_, duration)) = index.and_then(|index| pending.remove(index)) {
        packet.set_duration(duration);
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
            self.audio_output.take();
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

fn publish_unless_canceled(
    staging: &Path,
    destination: &Path,
    cancellation: Option<&snow_core::cancellation::CancellationToken>,
) -> Result<()> {
    let publish = || publish_staging_file(staging, destination);
    match cancellation {
        Some(token) => token
            .commit(publish)
            .map_err(|_| RecordingExportError::ExportCanceled)??,
        None => publish()?,
    }
    Ok(())
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

    #[test]
    fn cancellation_during_finalization_prevents_publication() {
        let directory = tempfile::tempdir().unwrap();
        let staging = directory.path().join("staging.mp4");
        let destination = directory.path().join("output.mp4");
        fs::write(&staging, b"finalized-video").unwrap();
        let cancellation = snow_core::cancellation::CancellationToken::default();
        cancellation.cancel();
        assert!(matches!(
            publish_unless_canceled(&staging, &destination, Some(&cancellation)),
            Err(RecordingExportError::ExportCanceled)
        ));
        assert!(!destination.exists());
        assert!(staging.exists());
        publish_unless_canceled(&staging, &destination, None).unwrap();
        assert_eq!(fs::read(&destination).unwrap(), b"finalized-video");
    }

    #[test]
    fn canceled_encoder_drops_its_staging_file() {
        let directory = tempfile::tempdir().unwrap();
        let destination = directory.path().join("output.mp4");
        let mut encoder =
            StreamingEncoder::builder(encoder_config(destination.clone(), ExportFormat::Mp4))
                .software_only()
                .create()
                .unwrap();
        encoder
            .push_rgba_frame_at_pts(0, &[255; 16 * 16 * 4])
            .unwrap();
        let cancellation = snow_core::cancellation::CancellationToken::default();
        cancellation.cancel();
        assert!(matches!(
            encoder.finish_at_pts_cancelable(1, &cancellation),
            Err(RecordingExportError::ExportCanceled)
        ));
        assert!(!destination.exists());
        assert_eq!(fs::read_dir(directory.path()).unwrap().count(), 0);
    }

    fn encoder_config(output_path: PathBuf, format: ExportFormat) -> StreamingEncoderConfig {
        StreamingEncoderConfig {
            loop_animated_images: true,
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
        }
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn software_policy_after_native_negotiation_accepts_cpu_frames() {
        let directory = tempfile::tempdir().unwrap();
        let mut encoder = StreamingEncoder::builder(encoder_config(
            directory.path().join("software.mp4"),
            ExportFormat::Mp4,
        ))
        .native_input(snow_media::PixelFormat::Bgra8)
        .software_only()
        .create()
        .unwrap();
        encoder
            .push_rgba_frame_at_pts(0, &[128; 16 * 16 * 4])
            .unwrap();
        let report = encoder.finish_at_pts(1).unwrap();
        assert!(!report.used_hardware_video_encoder);
        assert_eq!(report.encoded_frames, 1);
    }
    #[test]
    fn conversion_report_tracks_opened_worker_setting_and_restores_legacy_metadata() {
        let directory = tempfile::tempdir().unwrap();
        let mut encoder = StreamingEncoder::create(encoder_config(
            directory.path().join("workers.mp4"),
            ExportFormat::Mp4,
        ))
        .unwrap();
        let legacy_threads = encoder.report.effective_conversion_threads;
        assert_eq!(encoder.report.conversion_threads, 0);
        assert_eq!(encoder.report.conversion_backend, "legacy_sws_scale");
        for count in [1, 2, 4] {
            encoder.set_conversion_threads(count).unwrap();
            assert_eq!(encoder.report.conversion_backend, "legacy_sws_scale_frame");
            assert_eq!(
                encoder.report.effective_conversion_threads,
                Some(usize::from(count))
            );
        }
        encoder.set_conversion_threads(0).unwrap();
        assert_eq!(encoder.report.conversion_threads, 0);
        assert_eq!(encoder.report.conversion_backend, "legacy_sws_scale");
        assert_eq!(encoder.report.effective_conversion_threads, legacy_threads);
    }

    #[test]
    #[ignore = "Release performance benchmark; requires SNOW_CONVERSION_BENCH_OUTPUT"]
    fn benchmark_owned_rgba_conversion() {
        use std::hint::black_box;
        use std::io::Write;
        use std::time::Instant;
        if cfg!(debug_assertions) {
            panic!("use windows-msvc-performance Release");
        }
        ensure_ffmpeg_initialized().unwrap();
        let mut csv =
            fs::File::create(std::env::var("SNOW_CONVERSION_BENCH_OUTPUT").unwrap()).unwrap();
        writeln!(csv, "pair,variant,iterations,nanoseconds_per_frame").unwrap();
        let (width, height) = (1920, 1080);
        let mut pixels: Vec<u8> = (0..width * height * 4)
            .map(|index| (index % 251) as u8)
            .collect();
        let visible = pixels.len();
        pad_owned_rgba(&mut pixels);
        let mut rgba_frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height);
        let mut output = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::YUV420P, width, height);
        let mut scaler = ffmpeg::software::scaling::Context::get(
            ffmpeg::format::Pixel::RGBA,
            width,
            height,
            ffmpeg::format::Pixel::YUV420P,
            width,
            height,
            ffmpeg::software::scaling::flag::Flags::BICUBIC,
        )
        .unwrap();
        let mut run = |owned| {
            if owned {
                convert_owned_rgba(&mut scaler, black_box(&pixels), black_box(&mut output))
                    .unwrap();
            } else {
                copy_rgba_into_frame(&mut rgba_frame, width, black_box(&pixels[..visible]));
                scaler.run(&rgba_frame, black_box(&mut output)).unwrap();
            }
            black_box(&output);
        };
        let warmup = Instant::now();
        while warmup.elapsed() < Duration::from_secs(5) {
            run(false);
            run(true);
        }
        for pair in 1..=5 {
            for owned in if pair % 2 == 1 {
                [false, true]
            } else {
                [true, false]
            } {
                let started = Instant::now();
                let mut iterations = 0;
                while started.elapsed() < Duration::from_secs(2) {
                    run(owned);
                    iterations += 1;
                }
                writeln!(
                    csv,
                    "{pair},{},{iterations},{}",
                    if owned { "owned" } else { "avframe-copy" },
                    started.elapsed().as_nanos() / iterations
                )
                .unwrap();
            }
        }
    }

    #[test]
    fn owned_rgba_conversion_matches_avframe_copy_for_all_streaming_pixel_layouts() {
        ensure_ffmpeg_initialized().unwrap();
        for (width, height) in [(1, 1), (17, 19), (63, 127), (128, 64), (1920, 1080)] {
            let pixels: Vec<u8> = (0..width * height * 4)
                .map(|index| (index % 251) as u8)
                .collect();
            let mut source = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height);
            copy_rgba_into_frame(&mut source, width, &pixels);
            let mut padded = pixels.clone();
            pad_owned_rgba(&mut padded);
            for format in [
                ffmpeg::format::Pixel::YUV420P,
                ffmpeg::format::Pixel::NV12,
                ffmpeg::format::Pixel::YUV422P,
                ffmpeg::format::Pixel::YUVA420P,
                ffmpeg::format::Pixel::RGB8,
                ffmpeg::format::Pixel::RGB24,
                ffmpeg::format::Pixel::RGBA,
                ffmpeg::format::Pixel::BGRA,
            ] {
                let make_scaler = || {
                    ffmpeg::software::scaling::Context::get(
                        ffmpeg::format::Pixel::RGBA,
                        width,
                        height,
                        format,
                        width,
                        height,
                        ffmpeg::software::scaling::flag::Flags::BICUBIC,
                    )
                    .unwrap()
                };
                let mut baseline = ffmpeg::frame::Video::new(format, width, height);
                let mut candidate = ffmpeg::frame::Video::new(format, width, height);
                make_scaler().run(&source, &mut baseline).unwrap();
                convert_owned_rgba(&mut make_scaler(), &padded, &mut candidate).unwrap();
                let packed = |frame: &ffmpeg::frame::Video| {
                    // SAFETY: both frames are allocated for this exact format and
                    // geometry. FFmpeg computes and fills the packed image size;
                    // row padding is excluded from the pixel comparison.
                    unsafe {
                        let size = ffmpeg::ffi::av_image_get_buffer_size(
                            format.into(),
                            width as i32,
                            height as i32,
                            1,
                        );
                        assert!(size > 0);
                        let mut bytes = vec![0; size as usize];
                        let copied = ffmpeg::ffi::av_image_copy_to_buffer(
                            bytes.as_mut_ptr(),
                            size,
                            (*frame.as_ptr()).data.as_ptr() as *const *const u8,
                            (*frame.as_ptr()).linesize.as_ptr(),
                            format.into(),
                            width as i32,
                            height as i32,
                            1,
                        );
                        assert_eq!(copied, size);
                        bytes
                    }
                };
                assert_eq!(
                    packed(&baseline),
                    packed(&candidate),
                    "{width}x{height} {format:?}"
                );
                for threads in [1, 2, 4] {
                    let mut converter =
                        crate::frame_converter::FrameConverter::new(width, height, format, threads)
                            .unwrap();
                    converter.convert(&padded, &mut candidate).unwrap();
                    assert_eq!(
                        packed(&baseline),
                        packed(&candidate),
                        "frame API {width}x{height} {format:?} threads={threads}"
                    );
                }
                assert!(convert_owned_rgba(&mut make_scaler(), &pixels, &mut candidate).is_err());
            }
            assert_eq!(&padded[..pixels.len()], pixels);
            assert!(padded[pixels.len()..].iter().all(|byte| *byte == 0));
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
    fn animated_image_loop_metadata_matches_preference() {
        let directory = tempfile::tempdir().unwrap();
        for format in [ExportFormat::Gif, ExportFormat::Apng, ExportFormat::Webp] {
            let mut normalized_files = Vec::new();
            for enabled in [false, true] {
                let path = directory
                    .path()
                    .join(format!("loop-{enabled}.{}", format.file_extension()));
                let mut config = encoder_config(path.clone(), format);
                config.loop_animated_images = enabled;
                let mut encoder = StreamingEncoder::create(config).unwrap();
                write_test_frames(&mut encoder);
                assert_eq!(encoder.finish().unwrap().encoded_frames, 3);
                let mut bytes = fs::read(&path).unwrap();
                match format {
                    ExportFormat::Gif => {
                        let extension = bytes.windows(11).position(|v| v == b"NETSCAPE2.0");
                        if enabled {
                            let offset =
                                extension.expect("looping GIF needs a Netscape extension") + 11;
                            assert_eq!(&bytes[offset..offset + 5], &[3, 1, 0, 0, 0]);
                            bytes.drain(offset - 14..offset + 5);
                        } else {
                            assert!(
                                extension.is_none(),
                                "play-once GIF must omit the loop extension"
                            );
                        }
                    }
                    ExportFormat::Apng => {
                        let mut offset = 8;
                        let mut plays = None;
                        while offset + 12 <= bytes.len() {
                            let size =
                                u32::from_be_bytes(bytes[offset..offset + 4].try_into().unwrap())
                                    as usize;
                            if &bytes[offset + 4..offset + 8] == b"acTL" {
                                assert_eq!(
                                    u32::from_be_bytes(
                                        bytes[offset + 8..offset + 12].try_into().unwrap()
                                    ),
                                    3
                                );
                                plays = Some(u32::from_be_bytes(
                                    bytes[offset + 12..offset + 16].try_into().unwrap(),
                                ));
                                // Normalize only play count and its dependent acTL CRC.
                                bytes[offset + 12..offset + 20].fill(0);
                            }
                            offset += 12 + size;
                        }
                        assert_eq!(plays, Some(u32::from(!enabled)));
                    }
                    ExportFormat::Webp => {
                        let mut offset = 12;
                        let mut loops = None;
                        let mut frames = 0;
                        while offset + 8 <= bytes.len() {
                            let size = u32::from_le_bytes(
                                bytes[offset + 4..offset + 8].try_into().unwrap(),
                            ) as usize;
                            match &bytes[offset..offset + 4] {
                                b"ANIM" => {
                                    loops = Some(u16::from_le_bytes(
                                        bytes[offset + 12..offset + 14].try_into().unwrap(),
                                    ));
                                    bytes[offset + 12..offset + 14].fill(0);
                                }
                                b"ANMF" => frames += 1,
                                _ => {}
                            }
                            offset += 8 + size + (size & 1);
                        }
                        assert_eq!(loops, Some(u16::from(!enabled)));
                        assert_eq!(frames, 3);
                    }
                    ExportFormat::Mp4 | ExportFormat::Avi => unreachable!(),
                }
                normalized_files.push(bytes);
            }
            // Encoded frames, timing and final-frame content must be identical
            // after normalizing only the container looping metadata.
            assert_eq!(normalized_files[0], normalized_files[1], "{format:?}");
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
    #[test]
    fn retained_native_style_failure_keeps_playable_media_but_cancellation_does_not() {
        let directory = tempfile::tempdir().unwrap();
        let output = directory.path().join("recording.mp4");
        fs::write(&output, b"existing").unwrap();
        let mut encoder =
            StreamingEncoder::create(encoder_config(output.clone(), ExportFormat::Mp4)).unwrap();
        encoder.retain_failed_output();
        write_test_frames(&mut encoder);
        let error = encoder.finish().unwrap_err().to_string();
        let retained = PathBuf::from(
            error
                .split("recoverable media is retained in ")
                .nth(1)
                .unwrap(),
        );
        assert!(retained.join("timeline.txt").is_file());
        assert_eq!(
            decoded_video_frame_count(&retained.join("recording.mp4")).0,
            3
        );
        assert_eq!(fs::read(&output).unwrap(), b"existing");
        let canceled = directory.path().join("canceled.mp4");
        let mut encoder =
            StreamingEncoder::create(encoder_config(canceled.clone(), ExportFormat::Mp4)).unwrap();
        encoder.retain_failed_output();
        write_test_frames(&mut encoder);
        let token = snow_core::cancellation::CancellationToken::default();
        token.cancel();
        let count = fs::read_dir(directory.path()).unwrap().count();
        assert!(encoder.finish_at_pts_cancelable(3, &token).is_err());
        assert!(!canceled.exists());
        assert_eq!(fs::read_dir(directory.path()).unwrap().count(), count - 1);
    }

    #[test]
    fn explicit_pts_and_endpoint_preserve_variable_duration_and_decode() {
        let directory = tempfile::tempdir().unwrap();
        let output = directory.path().join("timing.mp4");
        let mut encoder =
            StreamingEncoder::create(encoder_config(output.clone(), ExportFormat::Mp4)).unwrap();
        encoder
            .push_rgba_frame_at_pts(0, &[80; 16 * 16 * 4])
            .unwrap();
        encoder
            .push_rgba_frame_at_pts(3, &[160; 16 * 16 * 4])
            .unwrap();
        let report = encoder.finish_at_pts(10).unwrap();
        assert_eq!(report.encoded_frames, 2);
        assert_eq!(decoded_video_frame_count(&output).0, 2);
        let mut input = ffmpeg::format::input(&output).unwrap();
        let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
        let index = stream.index();
        let time_base = stream.time_base();
        let packets: Vec<_> = input
            .packets()
            .filter(|(stream, _)| stream.index() == index)
            .map(|(_, packet)| (packet.pts().unwrap(), packet.duration()))
            .collect();
        assert_eq!(packets.len(), 2);
        let seconds = |ticks: i64| {
            ticks as f64 * f64::from(time_base.numerator()) / f64::from(time_base.denominator())
        };
        assert!((seconds(packets[1].0) - 0.3).abs() < 0.001);
        assert!((seconds(packets[1].0 + packets[1].1) - 1.0).abs() < 0.001);
    }

    #[test]
    fn owned_submission_recycles_only_released_pixels_and_matches_borrowed_packets() {
        let directory = tempfile::tempdir().unwrap();
        let mut packets = Vec::new();
        for owned in [false, true] {
            let path = directory.path().join(format!("owned-{owned}.mp4"));
            let mut encoder =
                StreamingEncoder::create(encoder_config(path.clone(), ExportFormat::Mp4)).unwrap();
            let mut storage = vec![0; 16 * 16 * 4];
            let mut pointers = Vec::new();
            for index in 0..12 {
                storage.resize(16 * 16 * 4, 0);
                // Reserve SIMD tail room before checking pointer identity. First-use
                // growth is permitted; adequately sized storage must recycle in place.
                storage.reserve_exact(ffmpeg::ffi::AV_INPUT_BUFFER_PADDING_SIZE as usize);
                storage.fill((index * 17) as u8);
                if owned {
                    pointers.push(storage.as_ptr());
                    storage = encoder
                        .push_owned_rgba_frame_at_pts(index, storage)
                        .unwrap();
                    if index > 0 {
                        assert_eq!(storage.as_ptr(), pointers[index as usize - 1]);
                        assert_eq!(storage, vec![((index - 1) * 17) as u8; 16 * 16 * 4]);
                    }
                    // Poison the returned allocation while encoded frames remain referenced.
                    storage.fill(255);
                } else {
                    encoder.push_rgba_frame_at_pts(index, &storage).unwrap();
                }
            }
            assert_eq!(encoder.finish_at_pts(15).unwrap().encoded_frames, 12);
            assert_eq!(decoded_video_frame_count(&path).0, 12);
            let mut input = ffmpeg::format::input(&path).unwrap();
            packets.push(
                input
                    .packets()
                    .map(|(_, p)| (p.pts(), p.duration(), p.data().unwrap().to_vec()))
                    .collect::<Vec<_>>(),
            );
        }
        assert_eq!(packets[0], packets[1]);
    }

    #[test]
    fn invalid_media_endpoint_cancels_staging_without_publishing() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("invalid-end.mp4");
        let mut encoder =
            StreamingEncoder::create(encoder_config(path.clone(), ExportFormat::Mp4)).unwrap();
        encoder
            .push_owned_rgba_frame_at_pts(3, vec![80; 16 * 16 * 4])
            .unwrap();
        assert!(encoder.finish_at_pts(3).is_err());
        assert!(!path.exists());
        assert_eq!(std::fs::read_dir(directory.path()).unwrap().count(), 0);
    }

    #[test]
    fn reordered_packets_keep_the_duration_of_their_own_image() {
        let mut pending = VecDeque::from([(0, 3), (3, 3), (6, 6)]);
        for (pts, duration) in [(0, 3), (6, 6), (3, 3)] {
            let mut packet = ffmpeg::Packet::empty();
            packet.set_pts(Some(pts));
            apply_packet_duration(&mut packet, &mut pending);
            assert_eq!(packet.duration(), duration);
        }
        assert!(pending.is_empty());
    }

    #[test]
    fn h265_reordered_static_final_frame_reaches_stop_boundary() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("h265-timing.mp4");
        let mut config = encoder_config(path.clone(), ExportFormat::Mp4);
        config.codec = VideoCodec::H265;
        config.width = 64;
        config.height = 64;
        config.encode_threads = 1;
        let mut encoder = StreamingEncoder::create(config).unwrap();
        for (pts, value) in [(0, 60), (3, 120), (6, 180)] {
            encoder
                .push_owned_rgba_frame_at_pts(pts, vec![value; 64 * 64 * 4])
                .unwrap();
        }
        assert_eq!(encoder.finish_at_pts(12).unwrap().encoded_frames, 3);
        // The distributed FFmpeg enables libx265 but not the HEVC decoder.
        // Validate demuxed timing here; the retained fixture is decoded externally.
        let mut input = ffmpeg::format::input(&path).unwrap();
        let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
        let time_base = stream.time_base();
        let endpoint = input
            .packets()
            .map(|(_, p)| p.pts().unwrap() + p.duration())
            .max()
            .unwrap();
        let seconds =
            endpoint as f64 * f64::from(time_base.numerator()) / f64::from(time_base.denominator());
        assert!((seconds - 1.2).abs() < 0.001, "{seconds}");
        if let Some(directory) = std::env::var_os("SNOW_RECORDING_TEST_ARTIFACTS") {
            std::fs::create_dir_all(&directory).unwrap();
            std::fs::copy(&path, Path::new(&directory).join("h265-timing.mp4")).unwrap();
        }
    }

    #[test]
    fn failed_audio_initialization_after_video_open_removes_staging() {
        for hardware in [false, true] {
            let directory = tempfile::tempdir().unwrap();
            let path = directory.path().join("partial-initialization.mp4");
            let mut config = encoder_config(path.clone(), ExportFormat::Mp4);
            config.width = 640;
            config.height = 360;
            config.prefer_hardware_h264 = hardware;
            config.execution_mode = ExportExecutionMode::HardwarePreferred;
            // Reaches resampler initialization after opening video and adding its
            // stream. FFmpeg cannot represent this input rate as a positive int.
            config.audio = Some(StreamingAudioConfig {
                sample_rate_hz: u32::MAX,
                channels: 2,
                bitrate_kbps: 160,
            });
            assert!(StreamingEncoder::create(config).is_err());
            assert!(!path.exists());
            assert_eq!(std::fs::read_dir(directory.path()).unwrap().count(), 0);
        }
    }

    #[test]
    fn software_execution_mode_never_silently_selects_hardware() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("software.mp4");
        let mut config = encoder_config(path, ExportFormat::Mp4);
        config.prefer_hardware_h264 = true;
        config.execution_mode = ExportExecutionMode::SoftwareOnly;
        let mut encoder = StreamingEncoder::create(config).unwrap();
        write_test_frames(&mut encoder);
        let report = encoder.finish().unwrap();
        assert_eq!(report.video_encoder, "libx264");
        assert!(!report.used_hardware_video_encoder);
    }

    #[test]
    fn forced_hardware_failure_exposes_opened_software_identity_and_keeps_explicit_threads() {
        let directory = tempfile::tempdir().unwrap();
        let mut config = encoder_config(directory.path().join("fallback.mp4"), ExportFormat::Mp4);
        config.prefer_hardware_h264 = true;
        config.execution_mode = ExportExecutionMode::HardwarePreferred;
        config.encode_threads = 2;
        // Skip hardware initialization itself, so this test needs no display/GPU.
        let mut encoder = StreamingEncoder::create_inner(
            config,
            true,
            None,
            #[cfg(windows)]
            None,
            #[cfg(target_os = "macos")]
            None,
            #[cfg(target_os = "macos")]
            false,
        )
        .unwrap();
        assert_eq!(encoder.opened_video_encoder(), ("libx264", false));
        assert_eq!(encoder.report.effective_encode_threads, 2);
        assert!(encoder.report.hardware_fallback);
        write_test_frames(&mut encoder);
        let report = encoder.finish().unwrap();
        assert_eq!(report.video_encoder, "libx264");
        assert!(!report.used_hardware_video_encoder);
    }

    #[test]
    fn software_fallback_threads_are_selected_only_for_actual_software_fallback() {
        let directory = tempfile::tempdir().unwrap();
        for preferred in [false, true] {
            let mut config = encoder_config(
                directory.path().join(format!("policy-{preferred}.mp4")),
                ExportFormat::Mp4,
            );
            config.prefer_hardware_h264 = preferred;
            config.execution_mode = if preferred {
                ExportExecutionMode::HardwarePreferred
            } else {
                ExportExecutionMode::SoftwareOnly
            };
            config.encode_threads = 1;
            let mut builder = StreamingEncoder::builder(config).software_fallback_threads(4);
            // The private test setting skips hardware initialization itself;
            // no display or GPU is needed to exercise the failure path.
            builder.force_hardware_failure = true;
            let mut encoder = builder.create().unwrap();
            assert_eq!(encoder.opened_video_encoder(), ("libx264", false));
            assert_eq!(
                encoder.report.effective_encode_threads,
                if preferred { 4 } else { 1 }
            );
            write_test_frames(&mut encoder);
            encoder.finish().unwrap();
        }
    }
}

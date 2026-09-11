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
    drain_audio_packets_with_callback, drain_video_packets_with_durations_timed,
    effective_audio_bitrate_kbps, effective_video_config, ensure_video_frame_writable,
    is_hardware_h264_encoder, open_audio_encoder, open_video_encoder, select_video_codec,
};
use crate::error::{RecordingExportError, Result};
use crate::ffmpeg_util::{copy_rgba_into_frame, ensure_ffmpeg_initialized};
use crate::video_quality::smart_quality_bitrate_bps;

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

pub struct StreamingEncoder {
    output: Option<ffmpeg::format::context::Output>,
    encoder: ffmpeg::encoder::video::Encoder,
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
    scaler: ffmpeg::software::scaling::Context,
    rgba_frame: ffmpeg::frame::Video,
    encode_frame: ffmpeg::frame::Video,
    width: u32,
    height: u32,
    fps: u32,
    /// Presentation timestamp of the frame currently held in `encode_frame`.
    /// Pixels are converted into `encode_frame` at push time, so no caller
    /// buffer needs to be retained between pushes.
    pending_pts: Option<i64>,
    pending_packet_durations: VecDeque<i64>,
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
        let scaler = ffmpeg::software::scaling::Context::get(
            ffmpeg::format::Pixel::RGBA,
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

        Ok(Self {
            output: Some(output),
            encoder,
            stream_index,
            stream_time_base,
            scaler,
            rgba_frame: ffmpeg::frame::Video::new(
                ffmpeg::format::Pixel::RGBA,
                config.width,
                config.height,
            ),
            encode_frame: ffmpeg::frame::Video::new(pixel_format, config.width, config.height),
            width: config.width,
            height: config.height,
            fps,
            pending_pts: None,
            pending_packet_durations: VecDeque::new(),
            audio,
            staging_path: None,
            final_path: config.output_path,
            report: StreamingEncoderReport {
                video_encoder: video_codec.name().to_string(),
                used_hardware_video_encoder: is_hardware_h264_encoder(&video_codec),
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
    /// the pending frame's pixels (coalescing) instead of emitting two frames.
    pub fn push_rgba_frame(&mut self, timestamp_ms: u64, rgba: &[u8]) -> Result<()> {
        let expected = self.width as usize * self.height as usize * 4;
        if rgba.len() != expected {
            return Err(RecordingExportError::InvalidConfig(format!(
                "streaming RGBA frame has {} bytes; expected {expected}",
                rgba.len()
            )));
        }
        let pts = ((u128::from(timestamp_ms) * u128::from(self.fps) + 500) / 1_000)
            .min(i64::MAX as u128) as i64;
        match self.pending_pts.take() {
            None => {}
            Some(pending_pts) if pts <= pending_pts => {
                self.pending_pts = Some(pending_pts);
                self.convert_frame(rgba)?;
                self.report.coalesced_frames = self.report.coalesced_frames.saturating_add(1);
                return Ok(());
            }
            Some(pending_pts) => {
                let duration = pts.saturating_sub(pending_pts).max(1);
                self.send_converted(pending_pts, duration)?;
            }
        }
        self.convert_frame(rgba)?;
        self.pending_pts = Some(pts);
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
        if let Some(pending_pts) = self.pending_pts.take() {
            self.send_converted(pending_pts, 1)?;
        }
        if let Some(audio) = self.audio.as_mut() {
            let output = self.output.as_mut().ok_or_else(|| {
                RecordingExportError::Encode("streaming output is already closed".to_string())
            })?;
            audio.finish(output, &mut self.report)?;
        }
        self.encoder.send_eof().map_err(|error| {
            RecordingExportError::Encode(format!(
                "failed to flush streaming video encoder: {error}"
            ))
        })?;
        {
            let output = self.output.as_mut().ok_or_else(|| {
                RecordingExportError::Encode("streaming output is already closed".to_string())
            })?;
            drain_video_packets_with_durations_timed(
                &mut self.encoder,
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
        copy_rgba_into_frame(&mut self.rgba_frame, self.width, rgba);
        #[cfg(feature = "stage-timing")]
        {
            self.report.video_stage_timings.copy += stage_started.elapsed();
        }
        #[cfg(feature = "stage-timing")]
        let stage_started = std::time::Instant::now();
        ensure_video_frame_writable(&mut self.encode_frame)?;
        self.scaler
            .run(&self.rgba_frame, &mut self.encode_frame)
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
        self.encoder
            .send_frame(&self.encode_frame)
            .map_err(|error| {
                RecordingExportError::Encode(format!(
                    "failed to send streaming video frame: {error}"
                ))
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
        drain_video_packets_with_durations_timed(
            &mut self.encoder,
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
}

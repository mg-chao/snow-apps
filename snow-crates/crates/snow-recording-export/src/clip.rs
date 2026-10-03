//! Immutable recording sources shared by preview and non-destructive trim exports.
use crate::decoder::{
    SourceVideoDecoder, normalize_decoded_video_frame, open_source_video_decoder,
};
use crate::error::{RecordingExportError as Error, Result};
use crate::{ExportPerformanceConfig, StreamingEncoder, StreamingEncoderConfig};
use ffmpeg::Rescale;
use ffmpeg_next as ffmpeg;
use snow_core::cancellation::CancellationToken;
use snow_memory::RasterBuffer;
use snow_recording_model::{RenderMetadata, TrimRange};
use std::path::{Path, PathBuf};
use std::sync::Arc;

fn media_error(error: impl std::fmt::Display) -> Error {
    Error::Decode(error.to_string())
}
pub(crate) fn check_cancel(token: &CancellationToken) -> Result<()> {
    if token.is_canceled() {
        Err(Error::ExportCanceled)
    } else {
        Ok(())
    }
}

#[derive(Clone)]
pub struct ClipSource {
    pub path: PathBuf,
    pub config: StreamingEncoderConfig,
    pub duration_us: u64,
    pub metadata: Option<RenderMetadata>,
    pub hdr: bool,
    /// Only animated containers need an explicit variable-delay frame index.
    boundaries: Arc<[u64]>,
}

impl ClipSource {
    /// None selects a self-contained deferred bundle. No source file is modified.
    pub fn open(path: &Path, config: Option<StreamingEncoderConfig>) -> Result<Self> {
        crate::ffmpeg_util::ensure_ffmpeg_initialized()?;
        let metadata = if config.is_none() {
            Some(
                snow_recording_model::read_bundle_render_metadata(path)?
                    .ok_or_else(|| media_error("recording render metadata is missing"))?,
            )
        } else {
            None
        };
        let mut config = match config {
            Some(value) => value,
            None => crate::read_deferred_output_settings(path)?,
        };
        let mut input = open_input(path)?;
        let stream = input
            .streams()
            .best(ffmpeg::media::Type::Video)
            .ok_or_else(|| media_error("recording has no video stream"))?;
        let video_index = stream.index();
        let time_base = stream.time_base();
        let video_duration_us = stream.duration().rescale(time_base, (1, 1_000_000));
        let parameters = stream.parameters();
        // Container parameters already describe a finalized recording. Opening a
        // second decoder just for metadata would allocate unnecessary worker state.
        let (width, height, hdr) = unsafe {
            let video = &*parameters.as_ptr();
            (
                video.width.max(0) as u32,
                video.height.max(0) as u32,
                video.color_trc == ffmpeg::ffi::AVColorTransferCharacteristic::AVCOL_TRC_SMPTE2084,
            )
        };
        if metadata.is_none() {
            config.width = width;
            config.height = height;
            config.audio = input
                .streams()
                .filter(|s| s.parameters().medium() == ffmpeg::media::Type::Audio)
                .enumerate()
                .map(|(index, stream)| crate::StreamingAudioConfig {
                    track_id: format!("clip-audio-{index}"),
                    title: {
                        let metadata = stream.metadata();
                        metadata
                            .get("title")
                            .or_else(|| metadata.get("handler_name"))
                            .unwrap_or("Audio")
                            .into()
                    },
                    default: stream
                        .disposition()
                        .contains(ffmpeg::format::stream::Disposition::DEFAULT),
                    ..Default::default()
                })
                .collect();
        }
        let mut boundaries = Vec::new();
        let mut duration_us = metadata.as_ref().map_or(
            if video_duration_us > 0 {
                video_duration_us as u64
            } else {
                input.duration().max(0) as u64
            },
            |value| value.timeline.duration_ms() * 1000,
        );
        if metadata.is_none() && config.format.is_animated_image() {
            let mut end = 0;
            for (stream, packet) in input.packets() {
                if stream.index() != video_index {
                    continue;
                }
                let start = packet
                    .pts()
                    .map(|pts| pts.rescale(time_base, (1, 1_000_000)).max(0) as u64)
                    .unwrap_or(end);
                let delay = packet.duration().rescale(time_base, (1, 1_000_000)).max(1) as u64;
                if boundaries.last().is_none_or(|last| *last < start) {
                    boundaries.push(start);
                }
                end = start.saturating_add(delay);
            }
            duration_us = end;
            boundaries.push(end);
        }
        if duration_us == 0 || config.width == 0 || config.height == 0 {
            return Err(media_error("recording is empty"));
        }
        config.validate().map_err(Error::InvalidConfig)?;
        Ok(Self {
            path: path.into(),
            config,
            duration_us,
            metadata,
            hdr,
            boundaries: boundaries.into(),
        })
    }

    pub fn frame_count(&self) -> u64 {
        if self.boundaries.is_empty() {
            (u128::from(self.duration_us) * u128::from(self.config.fps)).div_ceil(1_000_000) as u64
        } else {
            self.boundaries.len().saturating_sub(1) as u64
        }
    }

    pub fn boundary(&self, frame: u64) -> u64 {
        if frame >= self.frame_count() {
            return self.duration_us;
        }
        if self.boundaries.is_empty() {
            (u128::from(frame) * 1_000_000 / u128::from(self.config.fps)) as u64
        } else {
            self.boundaries[frame as usize]
        }
    }

    pub fn frame_at(&self, at: u64) -> u64 {
        if self.boundaries.is_empty() {
            ((((u128::from(at) + 1) * u128::from(self.config.fps) - 1) / 1_000_000) as u64)
                .min(self.frame_count().saturating_sub(1))
        } else {
            self.boundaries
                .partition_point(|value| *value <= at)
                .saturating_sub(1)
                .min(self.boundaries.len().saturating_sub(2)) as u64
        }
    }

    pub fn range(&self, first: u64, end: u64) -> Result<TrimRange> {
        if first >= end || end > self.frame_count() {
            return Err(Error::InvalidConfig("invalid clip frame range".into()));
        }
        TrimRange::new(self.boundary(first), self.boundary(end), self.duration_us)
            .map_err(Error::InvalidConfig)
    }

    pub fn export(
        &self,
        range: TrimRange,
        destination: &Path,
        token: &CancellationToken,
        mut progress: impl FnMut(f32),
    ) -> Result<()> {
        TrimRange::new(range.start_us, range.end_us, self.duration_us)
            .map_err(Error::InvalidConfig)?;
        if destination.exists() || destination == self.path {
            return Err(Error::InvalidConfig(
                "clip destination already exists or is the source".into(),
            ));
        }
        let mut config = self.config.clone();
        config.output_path = destination.into();
        if let Some(metadata) = &self.metadata {
            crate::deferred::render_bundle_range(
                &self.path,
                config,
                metadata.clone(),
                range,
                token,
                |_, done, total, _| progress(done as f32 * 100.0 / total.max(1) as f32),
            )?;
            return Ok(());
        }
        if range.start_us == 0 && range.end_us == self.duration_us {
            use std::io::{Read, Write};
            let mut source = std::fs::File::open(&self.path)?;
            let mut temporary =
                tempfile::NamedTempFile::new_in(destination.parent().unwrap_or(Path::new(".")))?;
            let mut buffer = vec![0; 256 * 1024];
            loop {
                check_cancel(token)?;
                let count = source.read(&mut buffer)?;
                if count == 0 {
                    break;
                }
                temporary.write_all(&buffer[..count])?;
            }
            temporary.flush()?;
            token
                .commit(|| {
                    temporary
                        .persist_noclobber(destination)
                        .map_err(|e| e.error)
                })
                .map_err(|_| Error::ExportCanceled)??;
            progress(100.0);
            return Ok(());
        }
        if config.format.is_animated_image() {
            // These containers store individual delays, not a fixed output rate.
            // Preserve both the source frame grid and millisecond/centisecond delays.
            fn gcd(a: u32, b: u32) -> u32 {
                if b == 0 { a } else { gcd(b, a % b) }
            }
            config.fps = config.fps / gcd(config.fps, 1000) * 1000;
        }
        let mut reader = VideoReader::open(self.clone(), true)?;
        let mut audio = AudioReader::open(self)?;
        let mut builder = StreamingEncoder::builder(config.clone());
        let hdr_output = crate::preserves_hdr_output(self.hdr, config.format, config.codec);
        if hdr_output {
            builder = builder.hdr10_cpu_input();
        }
        let mut encoder = builder.create()?;
        let first = self.frame_at(range.start_us);
        let end = if range.end_us == self.duration_us {
            self.frame_count()
        } else {
            self.frame_at(range.end_us)
        };
        let mut pixels = RasterBuffer::new();
        let mut hdr_conversion = None;
        let mut hdr_frame =
            ffmpeg::frame::Video::new(encoder.input_pixel_format(), config.width, config.height);
        let mut next_audio = 0;
        for index in first..end {
            check_cancel(token)?;
            let at = self.boundary(index);
            let pts = ((u128::from(at - range.start_us) * u128::from(config.fps) + 500_000)
                / 1_000_000) as u64;
            reader.read(at, token)?;
            if hdr_output {
                let decoded = reader.current_frame()?;
                if hdr_conversion.is_none() {
                    let mut scaler = ffmpeg::software::scaling::Context::get(
                        decoded.format(),
                        decoded.width(),
                        decoded.height(),
                        encoder.input_pixel_format(),
                        config.width,
                        config.height,
                        ffmpeg::software::scaling::Flags::BICUBIC,
                    )
                    .map_err(media_error)?;
                    crate::hdr::scaler_colors(&mut scaler, true, false)?;
                    hdr_conversion = Some(scaler);
                }
                crate::codec::ensure_video_frame_writable(&mut hdr_frame)?;
                hdr_conversion
                    .as_mut()
                    .unwrap()
                    .run(decoded, &mut hdr_frame)
                    .map_err(media_error)?;
                crate::hdr::frame(&mut hdr_frame, true);
                encoder.push_prepared_video_frame_ref_at_pts(pts, &hdr_frame)?;
            } else {
                reader.copy_rgba_buffer(&mut pixels)?;
                encoder.push_rgba_frame_at_pts(pts, &pixels)?;
            }
            let audio_end = (u128::from(self.boundary(index + 1).min(range.end_us)) * 48_000
                / 1_000_000) as u64
                - range.sample_offset(48_000);
            while !config.audio.is_empty() && next_audio < audio_end {
                check_cancel(token)?;
                let count = (audio_end - next_audio).min(960) as usize;
                let tracks = audio.read(range.sample_offset(48_000) + next_audio, count, token)?;
                for (track, samples) in config.audio.iter().zip(tracks.iter()) {
                    encoder.push_audio_track_pcm_i16_at_frame(
                        &track.track_id,
                        next_audio,
                        samples,
                    )?;
                }
                next_audio += count as u64;
            }
            progress((index + 1 - first) as f32 * 100.0 / (end - first).max(1) as f32);
        }
        encoder.finish_at_duration_us_cancelable(range.duration_us(), token)?;
        Ok(())
    }
}

fn open_input(path: &Path) -> Result<ffmpeg::format::context::Input> {
    let mut options = ffmpeg::Dictionary::new();
    options.set("ignore_loop", "1");
    ffmpeg::format::input_with_dictionary(path, options).map_err(media_error)
}

/// Decoder state is confined to its worker. Only owned pixels cross the UI boundary.
pub struct VideoReader {
    source: ClipSource,
    input: ffmpeg::format::context::Input,
    decoder: SourceVideoDecoder,
    stream: usize,
    time_base: ffmpeg::Rational,
    source_offset_us: i64,
    eof: bool,
    current: Option<(u64, ffmpeg::frame::Video)>,
    next: Option<(u64, ffmpeg::frame::Video)>,
    scaler: Option<ffmpeg::software::scaling::Context>,
    converted: ffmpeg::frame::Video,
    tone_mapper: crate::hdr::ToneMapper,
    hardware: bool,
}

impl VideoReader {
    pub fn open(source: ClipSource, hardware: bool) -> Result<Self> {
        // Device creation and readback dominate tiny clips; some native decoders also
        // accept undersized coded surfaces without producing valid pixels.
        let hardware = hardware
            && u64::from(source.config.width) * u64::from(source.config.height) >= 256 * 256;
        let input = open_input(&source.path)?;
        let stream = input
            .streams()
            .best(ffmpeg::media::Type::Video)
            .ok_or_else(|| media_error("missing video"))?;
        let source_offset_us = if source.metadata.is_some() {
            let mut index = snow_recording_model::VideoIndexReader::from_bundle(&source.path)?
                .ok_or_else(|| media_error("clean source has no video index"))?;
            let first = index
                .next_frame()?
                .ok_or_else(|| media_error("clean source video index is empty"))?;
            let first_slot = (u128::from(first.timestamp_ms) * u128::from(source.config.fps))
                .div_ceil(1000) as u64;
            source.boundary(first_slot) as i64
                - stream
                    .start_time()
                    .max(0)
                    .rescale(stream.time_base(), (1, 1_000_000))
        } else {
            0
        };
        let decoder = open_source_video_decoder(
            &stream.parameters(),
            &ExportPerformanceConfig {
                mode: crate::ExportExecutionMode::HardwarePreferred,
                decode_threads: 2,
                ..Default::default()
            },
            hardware,
        )?;
        Ok(Self {
            stream: stream.index(),
            time_base: stream.time_base(),
            source_offset_us,
            source,
            input,
            decoder,
            eof: false,
            current: None,
            next: None,
            scaler: None,
            converted: ffmpeg::frame::Video::empty(),
            tone_mapper: Default::default(),
            hardware,
        })
    }

    fn decode_next(
        &mut self,
        token: &CancellationToken,
    ) -> Result<Option<(u64, ffmpeg::frame::Video)>> {
        loop {
            check_cancel(token)?;
            let mut frame = ffmpeg::frame::Video::empty();
            match self.decoder.decoder.receive_frame(&mut frame) {
                Ok(()) => {
                    let at = frame
                        .timestamp()
                        .unwrap_or(0)
                        .rescale(self.time_base, (1, 1_000_000))
                        .saturating_add(self.source_offset_us)
                        .max(0) as u64;
                    return Ok(Some((at, frame)));
                }
                Err(ffmpeg::Error::Eof) => return Ok(None),
                Err(error) if crate::ffmpeg_util::is_eagain(&error) && !self.eof => {}
                Err(error) => return Err(media_error(error)),
            }
            loop {
                check_cancel(token)?;
                let mut packet = ffmpeg::Packet::empty();
                match packet.read(&mut self.input) {
                    Ok(()) if packet.stream() == self.stream => {
                        self.decoder
                            .decoder
                            .send_packet(&packet)
                            .map_err(media_error)?;
                        break;
                    }
                    Ok(()) => {}
                    Err(ffmpeg::Error::Eof) => {
                        self.decoder.decoder.send_eof().map_err(media_error)?;
                        self.eof = true;
                        break;
                    }
                    Err(error) => return Err(media_error(error)),
                }
            }
        }
    }

    pub fn read(&mut self, at: u64, token: &CancellationToken) -> Result<()> {
        let result = self.read_inner(at, token);
        if result.is_err() && self.hardware && !token.is_canceled() {
            *self = Self::open(self.source.clone(), false)?;
            return self.read_inner(at, token);
        }
        result
    }

    fn read_inner(&mut self, at: u64, token: &CancellationToken) -> Result<()> {
        let seek =
            self.current.as_ref().is_some_and(|(current, _)| {
                at < *current || at > current.saturating_add(1_000_000)
            }) || (self.current.is_none() && at > 1_000_000);
        if seek {
            // Animation frames depend on previous disposal/composition state.
            if self.source.config.format.is_animated_image() && self.source.metadata.is_none() {
                *self = Self::open(self.source.clone(), self.hardware)?;
            } else {
                self.input
                    .seek(
                        (at as i64 - self.source_offset_us).max(0),
                        ..(at as i64 - self.source_offset_us).max(0),
                    )
                    .map_err(media_error)?;
                self.decoder.decoder.flush();
                self.eof = false;
                self.current = None;
                self.next = None;
            }
        }
        if self.next.is_none() && !self.eof {
            self.next = self.decode_next(token)?;
        }
        while self
            .next
            .as_ref()
            .is_some_and(|(next, _)| *next <= at || self.current.is_none())
        {
            self.current = self.next.take();
            self.next = self.decode_next(token)?;
        }
        let frame = &mut self
            .current
            .as_mut()
            .ok_or_else(|| media_error("recording has no decoded frames"))?
            .1;
        // A checkpoint seek decodes many discarded pictures. Transfer only the
        // selected image from the GPU; readback of every discarded frame is costly.
        let mut transferred = ffmpeg::frame::Video::empty();
        normalize_decoded_video_frame(frame, &mut transferred, self.decoder.hardware.as_ref())?;
        if transferred.width() != 0 {
            *frame = transferred;
        }
        Ok(())
    }

    fn current_frame(&self) -> Result<&ffmpeg::frame::Video> {
        self.current
            .as_ref()
            .map(|(_, frame)| frame)
            .ok_or_else(|| media_error("no current frame"))
    }

    pub fn copy_rgba(&mut self, pixels: &mut Vec<u8>) -> Result<()> {
        self.prepare_rgba()?;
        pixels.resize(
            self.source.config.width as usize * self.source.config.height as usize * 4,
            0,
        );
        self.copy_prepared_rgba(pixels);
        Ok(())
    }

    pub fn copy_rgba_buffer(&mut self, pixels: &mut RasterBuffer) -> Result<()> {
        self.prepare_rgba()?;
        pixels.resize(
            self.source.config.width as usize * self.source.config.height as usize * 4,
            0,
        );
        self.copy_prepared_rgba(pixels);
        Ok(())
    }

    fn prepare_rgba(&mut self) -> Result<()> {
        let frame = &self
            .current
            .as_ref()
            .ok_or_else(|| media_error("no current frame"))?
            .1;
        let frame = if self.source.hdr {
            self.tone_mapper.convert(frame)?
        } else {
            frame
        };
        if self
            .scaler
            .as_ref()
            .is_none_or(|s| s.input().format != frame.format())
        {
            self.scaler = Some(
                ffmpeg::software::scaling::Context::get(
                    frame.format(),
                    frame.width(),
                    frame.height(),
                    ffmpeg::format::Pixel::RGBA,
                    frame.width(),
                    frame.height(),
                    ffmpeg::software::scaling::Flags::BILINEAR,
                )
                .map_err(media_error)?,
            );
            let scaler = self.scaler.as_mut().unwrap();
            unsafe {
                let coefficients = ffmpeg::ffi::sws_getCoefficients(frame.color_space() as i32);
                let status = ffmpeg::ffi::sws_setColorspaceDetails(
                    scaler.as_mut_ptr(),
                    coefficients,
                    i32::from(
                        crate::hdr::is_rgb(frame.format())
                            || frame.color_range() == ffmpeg::color::Range::JPEG,
                    ),
                    coefficients,
                    1,
                    0,
                    1 << 16,
                    1 << 16,
                );
                if status < 0 {
                    return Err(media_error(ffmpeg::Error::from(status)));
                }
            }
        }
        self.scaler
            .as_mut()
            .unwrap()
            .run(frame, &mut self.converted)
            .map_err(media_error)?;
        Ok(())
    }

    fn copy_prepared_rgba(&self, pixels: &mut [u8]) {
        let width = self.source.config.width;
        let height = self.source.config.height;
        for y in 0..height as usize {
            let stride = self.converted.stride(0);
            let len = width as usize * 4;
            pixels[y * len..(y + 1) * len]
                .copy_from_slice(&self.converted.data(0)[y * stride..y * stride + len]);
        }
    }
}

mod audio;
pub use audio::AudioReader;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ExportExecutionMode, ExportFormat, SoftwareH264Priority};
    use snow_recording_model::{VideoCodec, VideoEncodeConfig, VideoEncodingSpeed};

    fn fixture(directory: &Path, format: ExportFormat) -> StreamingEncoderConfig {
        StreamingEncoderConfig {
            output_path: directory.join(format!("source.{}", format.file_extension())),
            format,
            width: 32,
            height: 24,
            fps: 10,
            codec: VideoCodec::H264,
            prefer_hardware_h264: false,
            execution_mode: ExportExecutionMode::SoftwareOnly,
            software_h264_priority: SoftwareH264Priority::X264First,
            video: VideoEncodeConfig {
                quality: 100,
                speed: VideoEncodingSpeed::UltraFast,
            },
            encode_threads: 1,
            audio: Vec::new(),
            loop_animated_images: true,
        }
    }

    #[test]
    fn clip_trim_preserves_frames_and_sources_in_every_recording_format() {
        for format in [
            ExportFormat::Mp4,
            ExportFormat::Gif,
            ExportFormat::Apng,
            ExportFormat::Webp,
        ] {
            let directory = tempfile::tempdir().unwrap();
            let config = fixture(directory.path(), format);
            let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
            for (pts, color) in [
                (0, [220, 20, 20, 255]),
                (2, [20, 220, 20, 255]),
                (5, [20, 20, 220, 255]),
            ] {
                let pixels = color.repeat(32 * 24);
                encoder.push_rgba_frame_at_pts(pts, &pixels).unwrap();
            }
            encoder.finish_at_duration_ms(900).unwrap();
            let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
            assert!(
                source.duration_us.abs_diff(900_000) <= 10_000,
                "{format:?}: {}",
                source.duration_us
            );
            let first = source.frame_at(200_000);
            let end = source.frame_at(500_000);
            let range = source.range(first, end).unwrap();
            let path = directory
                .path()
                .join(format!("trim.{}", format.file_extension()));
            source
                .export(range, &path, &CancellationToken::default(), |_| {})
                .unwrap();
            assert!(
                config.output_path.exists(),
                "export must preserve its source"
            );
            let trimmed = ClipSource::open(&path, Some(config.clone())).unwrap();
            assert!(
                trimmed.duration_us.abs_diff(300_000) <= 10_000,
                "{format:?}: {}",
                trimmed.duration_us
            );
            let mut reader = VideoReader::open(trimmed, false).unwrap();
            reader.read(0, &CancellationToken::default()).unwrap();
            let mut pixels = Vec::new();
            reader.copy_rgba(&mut pixels).unwrap();
            assert!(
                pixels[1] > pixels[0] + 80 && pixels[1] > pixels[2] + 80,
                "first trimmed frame must be green: {:?}",
                &pixels[..4]
            );
            let second = directory
                .path()
                .join(format!("again.{}", format.file_extension()));
            source
                .export(
                    source.range(0, 1).unwrap(),
                    &second,
                    &CancellationToken::default(),
                    |_| {},
                )
                .unwrap();
            assert!(second.exists());
            let one = ClipSource::open(&second, Some(config.clone())).unwrap();
            assert!(
                one.duration_us.abs_diff(source.boundary(1)) < 2000,
                "one-frame {format:?}: {}",
                one.duration_us
            );
            if format.is_animated_image() {
                assert_eq!(one.frame_count(), 1);
            }
            let copy = directory
                .path()
                .join(format!("full.{}", format.file_extension()));
            source
                .export(
                    source.range(0, source.frame_count()).unwrap(),
                    &copy,
                    &CancellationToken::default(),
                    |_| {},
                )
                .unwrap();
            assert_eq!(
                std::fs::read(&copy).unwrap(),
                std::fs::read(&config.output_path).unwrap()
            );
        }
    }

    #[test]
    fn trim_rebases_separate_audio_tracks_and_preserves_silence() {
        let directory = tempfile::tempdir().unwrap();
        let mut config = fixture(directory.path(), ExportFormat::Mp4);
        config.audio = ["System", "Microphone"]
            .into_iter()
            .enumerate()
            .map(|(i, title)| crate::StreamingAudioConfig {
                track_id: title.into(),
                title: title.into(),
                default: i == 0,
                ..Default::default()
            })
            .collect();
        let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
        for index in 0..20 {
            encoder
                .push_rgba_frame_at_pts(index, &[20, 80, 140, 255].repeat(32 * 24))
                .unwrap();
            for (track, hz) in config.audio.iter().zip([440.0, 880.0]) {
                let samples: Vec<i16> = (index * 4800..(index + 1) * 4800)
                    .flat_map(|sample| {
                        let value = if sample >= 72_000 {
                            0
                        } else {
                            ((sample as f64 * std::f64::consts::TAU * hz / 48_000.0).sin() * 6000.0)
                                as i16
                        };
                        [value, value]
                    })
                    .collect();
                encoder
                    .push_audio_track_pcm_i16_at_frame(&track.track_id, index * 4800, &samples)
                    .unwrap();
            }
        }
        encoder.finish_at_duration_ms(2000).unwrap();
        let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
        assert_eq!(
            source.duration_us, 2_000_000,
            "audio codec padding must not extend the video timeline"
        );
        let destination = directory.path().join("audio-trim.mp4");
        let token = CancellationToken::default();
        source
            .export(source.range(3, 13).unwrap(), &destination, &token, |_| {})
            .unwrap();
        let trimmed = ClipSource::open(&destination, Some(config.clone())).unwrap();
        assert_eq!(trimmed.duration_us, 1_000_000);
        assert_eq!(trimmed.config.audio.len(), 2);
        assert_eq!(trimmed.config.audio[1].title, "Microphone");
        let mut original = AudioReader::open(&source).unwrap();
        let mut cut = AudioReader::open(&trimmed).unwrap();
        let expected = original.read(14_400 + 4000, 4000, &token).unwrap();
        let actual = cut.read(4000, 4000, &token).unwrap();
        for (a, b) in expected.iter().zip(actual) {
            let dot: f64 = a
                .iter()
                .zip(b)
                .map(|(x, y)| f64::from(*x) * f64::from(*y))
                .sum();
            let norm = (a.iter().map(|x| f64::from(*x).powi(2)).sum::<f64>()
                * b.iter().map(|x| f64::from(*x).powi(2)).sum::<f64>())
            .sqrt();
            assert!(
                dot / norm > 0.95,
                "trim audio must stay aligned with the source: {}",
                dot / norm
            );
        }
        let silence = directory.path().join("silence.mp4");
        source
            .export(source.range(17, 20).unwrap(), &silence, &token, |_| {})
            .unwrap();
        let silent = ClipSource::open(&silence, Some(config)).unwrap();
        let mut reader = AudioReader::open(&silent).unwrap();
        assert!(
            reader
                .read(2000, 960, &token)
                .unwrap()
                .iter()
                .flatten()
                .all(|sample| sample.abs() < 10)
        );
    }

    #[test]
    fn decoder_failure_reopens_software_without_reusing_stale_frames() {
        let directory = tempfile::tempdir().unwrap();
        let config = fixture(directory.path(), ExportFormat::Mp4);
        let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
        encoder
            .push_rgba_frame_at_pts(0, &[20, 220, 20, 255].repeat(32 * 24))
            .unwrap();
        encoder.finish_at_duration_ms(500).unwrap();
        let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
        let mut reader = VideoReader::open(source, false).unwrap();
        // Inject a decoder failure before its first frame, through the same recovery
        // path used when a native decoder rejects a surface or device disappears.
        reader.hardware = true;
        reader.stream = usize::MAX;
        reader.read(0, &CancellationToken::default()).unwrap();
        assert!(!reader.hardware);
        let mut pixels = Vec::new();
        reader.copy_rgba(&mut pixels).unwrap();
        assert!(pixels[1] > 180);
    }

    #[test]
    fn clip_cancellation_and_existing_destination_never_modify_originals() {
        let directory = tempfile::tempdir().unwrap();
        let config = fixture(directory.path(), ExportFormat::Mp4);
        let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
        encoder
            .push_rgba_frame_at_pts(0, &[255; 32 * 24 * 4])
            .unwrap();
        encoder.finish_at_duration_ms(500).unwrap();
        let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
        let original = std::fs::read(&config.output_path).unwrap();
        let token = CancellationToken::default();
        token.cancel();
        let path = directory.path().join("cancelled.mp4");
        assert!(matches!(
            source.export(
                source.range(0, source.frame_count()).unwrap(),
                &path,
                &token,
                |_| {}
            ),
            Err(Error::ExportCanceled)
        ));
        assert!(!path.exists());
        assert!(
            source
                .export(
                    source.range(0, 1).unwrap(),
                    &config.output_path,
                    &CancellationToken::default(),
                    |_| {}
                )
                .is_err()
        );
        assert_eq!(std::fs::read(&config.output_path).unwrap(), original);
    }
}

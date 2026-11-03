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
use snow_recording_model::{
    FixedFrameTimeline, PlaybackSpeed, RenderMetadata, RetimedFrameTiming, TrimRange,
};
use std::path::{Path, PathBuf};
use std::sync::Arc;

fn media_error(error: impl std::fmt::Display) -> Error {
    Error::Decode(error.to_string())
}
fn source_start_us(pts: i64, time_base: ffmpeg::Rational) -> i64 {
    // Presentation starts share ClipSource::boundary's floor convention. Nearest
    // rounding can put a native picture one microsecond after its editor boundary.
    pts.rescale_with(time_base, (1, 1_000_000), ffmpeg::Rounding::Down)
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
    /// Fixed presentation slots and the unrounded native media endpoint.
    fixed_timeline: Option<FixedFrameTimeline>,
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
        let native_timeline = if metadata.is_none()
            && !config.format.is_animated_image()
            && stream.duration() > 0
            && time_base.numerator() > 0
            && time_base.denominator() > 0
        {
            Some(
                FixedFrameTimeline::from_time_base(
                    stream.duration() as u64,
                    time_base.numerator() as u32,
                    time_base.denominator() as u32,
                    config.fps,
                )
                .map_err(media_error)?,
            )
        } else {
            None
        };
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
            let recorded_rates: Vec<_> = input
                .metadata()
                .get(crate::codec::AUDIO_BITRATES_METADATA)
                .map(|rates| {
                    rates
                        .split(',')
                        .take(input.nb_streams() as usize)
                        .map(|rate| {
                            rate.parse::<u16>()
                                .ok()
                                .filter(|value| (8..=192).contains(value))
                        })
                        .collect()
                })
                .unwrap_or_default();
            config.audio = input
                .streams()
                .filter(|s| s.parameters().medium() == ffmpeg::media::Type::Audio)
                .enumerate()
                .map(|(index, stream)| {
                    let parameters = stream.parameters();
                    // Prefer the recorded target; average bitrate underestimates sparse
                    // speech. Legacy files fall back to their container bitrate.
                    let (bitrate, channels) = unsafe {
                        let audio = &*parameters.as_ptr();
                        (audio.bit_rate, audio.ch_layout.nb_channels)
                    };
                    crate::StreamingAudioConfig {
                        track_id: format!("clip-audio-{index}"),
                        bitrate_kbps: recorded_rates
                            .get(index)
                            .copied()
                            .flatten()
                            .or_else(|| {
                                config.audio.get(index).map(|track| {
                                    crate::codec::effective_audio_bitrate_kbps(track.bitrate_kbps)
                                })
                            })
                            .unwrap_or_else(|| {
                                if bitrate > 0 {
                                    ((bitrate.saturating_add(500) / 1000).clamp(8, 192)) as u16
                                } else {
                                    crate::StreamingAudioConfig::default().bitrate_kbps
                                }
                            }),
                        // Replay normalizes multichannel material to stereo, but a mono
                        // recording must not become a larger stereo encode after trimming.
                        channels: if channels == 1 { 1 } else { 2 },
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
                    }
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
                    .map(|pts| source_start_us(pts, time_base).max(0) as u64)
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
        let fixed_timeline = if metadata.is_some() || !config.format.is_animated_image() {
            Some(match native_timeline {
                Some(timeline) => timeline,
                None => FixedFrameTimeline::new(duration_us, config.fps).map_err(media_error)?,
            })
        } else {
            None
        };
        Ok(Self {
            path: path.into(),
            config,
            duration_us,
            metadata,
            hdr,
            boundaries: boundaries.into(),
            fixed_timeline,
        })
    }

    pub fn frame_count(&self) -> u64 {
        if let Some(timeline) = self.fixed_timeline {
            timeline.frame_count()
        } else {
            self.boundaries.len().saturating_sub(1) as u64
        }
    }

    pub fn boundary(&self, frame: u64) -> u64 {
        if frame >= self.frame_count() {
            return self.duration_us;
        }
        if let Some(timeline) = self.fixed_timeline {
            timeline.boundary_us(frame)
        } else {
            self.boundaries[frame as usize]
        }
    }

    pub fn frame_at(&self, at: u64) -> u64 {
        if let Some(timeline) = self.fixed_timeline {
            timeline.frame_at(at)
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

    pub fn output_timing(
        &self,
        range: TrimRange,
        speed: PlaybackSpeed,
    ) -> Result<RetimedFrameTiming> {
        TrimRange::new(range.start_us, range.end_us, self.duration_us)
            .map_err(Error::InvalidConfig)?;
        match self.fixed_timeline {
            Some(timeline) => timeline.retime(range, speed),
            None => {
                RetimedFrameTiming::from_duration_us(range.duration_us(), speed, self.config.fps)
            }
        }
        .map_err(Error::InvalidConfig)
    }

    pub fn export(
        &self,
        range: TrimRange,
        destination: &Path,
        token: &CancellationToken,
        progress: impl FnMut(f32),
    ) -> Result<()> {
        self.export_with_speed(
            range,
            PlaybackSpeed::default(),
            destination,
            token,
            progress,
        )
    }

    pub fn export_with_speed(
        &self,
        range: TrimRange,
        speed: PlaybackSpeed,
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
            crate::deferred::render_bundle_range_with_speed(
                &self.path,
                config,
                metadata.clone(),
                range,
                speed,
                token,
                |_, done, total, _| progress(done as f32 * 100.0 / total.max(1) as f32),
            )?;
            return Ok(());
        }
        let unity = speed == PlaybackSpeed::default();
        if unity && range.start_us == 0 && range.end_us == self.duration_us {
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
        let animated = config.format.is_animated_image();
        if animated {
            // These containers store individual delays, not a fixed output rate.
            // Preserve both the source frame grid and millisecond/centisecond delays.
            fn gcd(a: u32, b: u32) -> u32 {
                if b == 0 { a } else { gcd(b, a % b) }
            }
            config.fps = config.fps / gcd(config.fps, 1000) * 1000;
            if !unity {
                config.fps = match config.format {
                    crate::ExportFormat::Gif => 100,
                    crate::ExportFormat::Webp => 1000,
                    _ => config
                        .fps
                        .checked_mul(speed.percent() / gcd(speed.percent(), 100))
                        .ok_or_else(|| {
                            Error::InvalidConfig("animation time base overflow".into())
                        })?,
                };
            }
        }
        let timing = self.output_timing(range, speed)?;
        let mut output_duration_us = timing.duration_us().map_err(Error::InvalidConfig)?;
        if animated && !unity {
            let endpoint = animation_pts(range.duration_us(), speed, config.fps).max(1);
            output_duration_us =
                u64::try_from((u128::from(endpoint) * 1_000_000).div_ceil(u128::from(config.fps)))
                    .map_err(|_| Error::InvalidConfig("animation duration overflow".into()))?;
        }
        let mut reader = VideoReader::open(self.clone(), true)?;
        let mut audio = crate::tempo::RetimedAudioReader::open(self, range, speed)?;
        let mut builder = StreamingEncoder::builder(config.clone())
            .offline_output()
            .audio_input_channels(2);
        let hdr_output = crate::preserves_hdr_output(self.hdr, config.format, config.codec);
        if hdr_output {
            builder = builder.hdr10_cpu_input();
        }
        let mut encoder = builder.create()?;
        let sparse_trim = unity && config.format == crate::ExportFormat::Mp4;
        if sparse_trim {
            encoder.set_frame_reuse(crate::VideoFrameReuse::Coalesce);
        }
        let prepared_sdr = !unity && !animated && !hdr_output;
        let first = self.frame_at(range.start_us);
        let end = if range.end_us == self.duration_us {
            self.frame_count()
        } else {
            self.frame_at(range.end_us)
        };
        let mut pixels = RasterBuffer::new();
        let mut hdr_conversion = None;
        let mut hdr_frame = hdr_output.then(|| {
            ffmpeg::frame::Video::new(encoder.input_pixel_format(), config.width, config.height)
        });
        let mut prepared_frame = None;
        let mut next_audio = 0;
        let mut previous_source = None;
        let mut previous_picture = None;
        let total = if animated {
            end - first
        } else {
            timing.frame_count().map_err(Error::InvalidConfig)?
        };
        for output_index in 0..total {
            check_cancel(token)?;
            let index = if unity || animated {
                first + output_index
            } else {
                // Keep the trim origin on the rational frame grid; separately
                // flooring origin and elapsed time can select the preceding frame.
                first
                    .saturating_add(
                        (u128::from(output_index) * u128::from(speed.percent()) / 100)
                            .min(u128::from(u64::MAX)) as u64,
                    )
                    .min(end - 1)
            };
            let at = self.boundary(index);
            let pts = if !unity && !animated {
                output_index
            } else if !unity {
                animation_pts(at - range.start_us, speed, config.fps)
            } else {
                ((u128::from(at - range.start_us) * u128::from(config.fps) + 500_000) / 1_000_000)
                    as u64
            };
            if animated
                && !unity
                && pts >= animation_pts(range.duration_us(), speed, config.fps).max(1)
            {
                continue;
            }
            if animated && !unity && pts == 0 && previous_source.is_some() {
                // A sub-tick initial delay must not replace the selected first picture.
                continue;
            }
            let mut submit_video = true;
            if unity || animated || previous_source != Some(index) {
                reader.read(at, token)?;
                let picture = reader.current.as_ref().map(|(pts, _)| *pts);
                // The same decoded picture can cover many slots, including HDR.
                // Audio still advances on every slot and finalization closes the hold.
                submit_video = !sparse_trim || previous_picture != picture;
                previous_picture = picture;
                if submit_video && hdr_output {
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
                    let hdr_frame = hdr_frame.as_mut().unwrap();
                    crate::codec::ensure_video_frame_writable(hdr_frame)?;
                    hdr_conversion
                        .as_mut()
                        .unwrap()
                        .run(decoded, hdr_frame)
                        .map_err(media_error)?;
                    crate::hdr::frame(hdr_frame, true);
                } else if submit_video && prepared_sdr {
                    reader.prepare_rgba()?;
                    // Release our previous lease before requesting writable storage.
                    // Encoder leases remain immutable until their packets are drained.
                    drop(prepared_frame.take());
                    prepared_frame = Some(encoder.prepare_rgba_video_frame(&reader.converted)?);
                } else if submit_video {
                    reader.copy_rgba_buffer(&mut pixels)?;
                }
                previous_source = Some(index);
            }
            // A slow export repeats pictures on the original FPS grid. Reuse
            // their prepared pixels, but retain every presentation timestamp.
            if submit_video && hdr_output {
                encoder.push_prepared_video_frame_ref_at_pts(pts, hdr_frame.as_ref().unwrap())?;
            } else if submit_video && prepared_sdr {
                encoder
                    .push_prepared_video_frame_ref_at_pts(pts, prepared_frame.as_ref().unwrap())?;
            } else if submit_video {
                encoder.push_rgba_frame_at_pts(pts, &pixels)?;
            }
            let audio_end = if unity {
                (u128::from(self.boundary(index + 1).min(range.end_us)) * 48_000 / 1_000_000) as u64
                    - range.sample_offset(48_000)
            } else {
                ((u128::from(output_index + 1) * 48_000 / u128::from(config.fps))
                    .min(u128::from(audio.total_samples()))) as u64
            };
            while !config.audio.is_empty() && next_audio < audio_end {
                check_cancel(token)?;
                let count = (audio_end - next_audio).min(960) as usize;
                let tracks = audio.read(count, token)?;
                for (track, samples) in config.audio.iter().zip(tracks.iter()) {
                    encoder.push_audio_track_pcm_i16_at_frame(
                        &track.track_id,
                        next_audio,
                        samples,
                    )?;
                }
                next_audio += count as u64;
            }
            progress(
                (output_index + 1) as f32 * if unity { 100.0 } else { 98.0 } / total.max(1) as f32,
            );
        }
        while !config.audio.is_empty() && next_audio < audio.total_samples() {
            let count = (audio.total_samples() - next_audio).min(960) as usize;
            let tracks = audio.read(count, token)?;
            for (track, samples) in config.audio.iter().zip(tracks) {
                encoder.push_audio_track_pcm_i16_at_frame(&track.track_id, next_audio, samples)?;
            }
            next_audio += count as u64;
        }
        if !animated {
            encoder.finish_at_duration_us_with_end_pts_cancelable(
                output_duration_us,
                total,
                token,
            )?;
        } else {
            encoder.finish_at_duration_us_cancelable(output_duration_us, token)?;
        }
        progress(100.0);
        Ok(())
    }
}

fn animation_pts(elapsed_us: u64, speed: PlaybackSpeed, fps: u32) -> u64 {
    let denominator = u128::from(speed.percent()) * 1_000_000;
    ((u128::from(elapsed_us) * 100 * u128::from(fps) + denominator / 2) / denominator)
        .min(u128::from(u64::MAX)) as u64
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
    webcam: Option<crate::webcam::WebcamReplay>,
    webcam_overlay: snow_recording_effects::webcam::WebcamOverlay,
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
                - source_start_us(stream.start_time().max(0), stream.time_base())
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
        let webcam = if source.metadata.is_some() {
            crate::webcam::WebcamReplay::open(&source.path, &CancellationToken::default())?
        } else {
            None
        };
        Ok(Self {
            webcam,
            webcam_overlay: Default::default(),
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
                    let at = source_start_us(frame.timestamp().unwrap_or(0), self.time_base)
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
        if let Some(webcam) = &mut self.webcam {
            let image = webcam.frame_at(at / 1000, token)?;
            self.webcam_overlay.prepare(
                image.as_deref(),
                webcam.style(),
                (self.source.config.width, self.source.config.height),
            );
        }
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
        self.webcam_overlay
            .draw_to(&mut snow_recording_effects::surface::RgbaSurface {
                pixels,
                dimensions: (width, height),
            });
    }
}

pub(crate) mod audio;
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
    fn partial_mp4_trim_preserves_sparse_pictures_and_endpoint() {
        for codec in [VideoCodec::H264, VideoCodec::H265] {
            let directory = tempfile::tempdir().unwrap();
            let mut config = fixture(directory.path(), ExportFormat::Mp4);
            config.codec = codec;
            config.fps = 30;
            config.video.speed = VideoEncodingSpeed::Medium;
            let colors = [[220, 20, 20, 255], [20, 220, 20, 255], [20, 20, 220, 255]];
            let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
            for (index, color) in colors.iter().enumerate() {
                encoder
                    .push_rgba_frame_at_pts(index as u64 * 30, &color.repeat(32 * 24))
                    .unwrap();
            }
            encoder.finish_at_pts(90).unwrap();
            let original = std::fs::read(&config.output_path).unwrap();
            let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
            let destination = directory.path().join("sparse-trim.mp4");
            let token = CancellationToken::default();
            source
                .export(source.range(15, 75).unwrap(), &destination, &token, |_| {})
                .unwrap();
            let mut input = ffmpeg::format::input(&destination).unwrap();
            let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
            let time_base = stream.time_base();
            let mut timestamps: Vec<_> = input
                .packets()
                .map(|(_, packet)| packet.pts().unwrap().rescale(time_base, (1, 30)))
                .collect();
            timestamps.sort_unstable();
            assert_eq!(
                timestamps,
                [0, 15, 45],
                "{codec:?}: trim must retain sparse picture holds"
            );
            let trimmed = ClipSource::open(&destination, Some(config.clone())).unwrap();
            assert_eq!(trimmed.duration_us, 2_000_000);
            assert_eq!(
                trimmed.frame_count(),
                60,
                "presentation slots remain seekable"
            );
            let mut reader = VideoReader::open(trimmed, false).unwrap();
            let mut pixels = Vec::new();
            for (at, color) in [
                (0, colors[0]),
                (499_999, colors[0]),
                (500_000, colors[1]),
                (1_499_999, colors[1]),
                (1_500_000, colors[2]),
                (1_999_999, colors[2]),
            ] {
                reader.read(at, &token).unwrap();
                reader.copy_rgba(&mut pixels).unwrap();
                assert!(
                    pixels[..3]
                        .iter()
                        .zip(color)
                        .all(|(a, b)| a.abs_diff(b) <= 15),
                    "{codec:?}: wrong picture at {at}: {:?}",
                    &pixels[..4]
                );
            }
            assert_eq!(std::fs::read(&config.output_path).unwrap(), original);
        }
    }

    #[test]
    fn partial_mp4_trim_preserves_audio_encoding_budget_and_mono() {
        let directory = tempfile::tempdir().unwrap();
        let mut config = fixture(directory.path(), ExportFormat::Mp4);
        config.audio = [("System", 2, 160), ("Speech", 1, 96)]
            .into_iter()
            .map(|(id, channels, bitrate_kbps)| crate::StreamingAudioConfig {
                track_id: id.into(),
                title: id.into(),
                channels,
                bitrate_kbps,
                default: id == "System",
                ..Default::default()
            })
            .collect();
        let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
        encoder
            .push_rgba_frame_at_pts(0, &[80; 32 * 24 * 4])
            .unwrap();
        for index in 0..150 {
            for track in &config.audio {
                let samples: Vec<_> = (index * 960..(index + 1) * 960)
                    .flat_map(|sample| {
                        let value = if (25..75).contains(&index) {
                            ((sample as f64 * std::f64::consts::TAU * 440.0 / 48_000.0).sin()
                                * 6000.0) as i16
                        } else {
                            0
                        };
                        std::iter::repeat_n(value, usize::from(track.channels))
                    })
                    .collect();
                encoder
                    .push_audio_track_pcm_i16_at_frame(&track.track_id, index * 960, &samples)
                    .unwrap();
            }
        }
        encoder.finish_at_duration_ms(3000).unwrap();
        let mut replay_config = config.clone();
        replay_config.audio.clear(); // Match the C API, which receives video options only.
        let source = ClipSource::open(&config.output_path, Some(replay_config.clone())).unwrap();
        let mut input = ffmpeg::format::input(&config.output_path).unwrap();
        let expected_rates: Vec<_> = input
            .streams()
            .filter(|s| s.parameters().medium() == ffmpeg::media::Type::Audio)
            .map(|s| unsafe { ((*s.parameters().as_ptr()).bit_rate + 500) / 1000 })
            .collect();
        for (track, original) in source.config.audio.iter().zip(&config.audio) {
            assert_eq!(track.bitrate_kbps, original.bitrate_kbps);
        }
        assert!(
            expected_rates[1] < 64,
            "silence must not lower the next encode target"
        );
        // Legacy recordings lack target metadata. Preserve their measured budget.
        let legacy_path = directory.path().join("legacy-audio.mp4");
        let mut legacy = ffmpeg::format::output(&legacy_path).unwrap();
        for stream in input.streams() {
            let mut target = legacy
                .add_stream(ffmpeg::encoder::find(ffmpeg::codec::Id::None))
                .unwrap();
            target.set_parameters(stream.parameters());
            target.set_time_base(stream.time_base());
        }
        legacy.write_header().unwrap();
        for (stream, mut packet) in input.packets() {
            packet.rescale_ts(
                stream.time_base(),
                legacy.stream(stream.index()).unwrap().time_base(),
            );
            packet.set_position(-1);
            packet.write_interleaved(&mut legacy).unwrap();
        }
        legacy.write_trailer().unwrap();
        drop(legacy);
        let legacy_source = ClipSource::open(&legacy_path, Some(replay_config)).unwrap();
        for (track, rate) in legacy_source.config.audio.iter().zip(expected_rates) {
            assert_eq!(i64::from(track.bitrate_kbps), rate.clamp(8, 192));
        }
        assert_eq!(source.config.audio[1].channels, 1);
        assert!(
            source.config.audio[1].bitrate_kbps < 128,
            "speech bitrate must not be raised to 128 kbps"
        );
        for percent in [100, 200] {
            let destination = directory.path().join(format!("audio-trim-{percent}.mp4"));
            source
                .export_with_speed(
                    source.range(5, 25).unwrap(),
                    PlaybackSpeed::from_percent(percent).unwrap(),
                    &destination,
                    &CancellationToken::default(),
                    |_| {},
                )
                .unwrap();
            let mut output_config = config.clone();
            output_config.audio.clear();
            let trimmed = ClipSource::open(&destination, Some(output_config)).unwrap();
            assert_eq!(trimmed.config.audio[1].channels, 1);
            assert_eq!(trimmed.config.audio[1].title, "Speech");
            assert!(trimmed.config.audio[0].default);
            assert!(!trimmed.config.audio[1].default);
            assert_eq!(trimmed.duration_us, 2_000_000 * 100 / u64::from(percent));
            for (original, output) in source.config.audio.iter().zip(&trimmed.config.audio) {
                assert_eq!(output.bitrate_kbps, original.bitrate_kbps);
            }
            let mut reader = AudioReader::open(&trimmed).unwrap();
            let audio = reader
                .read(4800, 960, &CancellationToken::default())
                .unwrap();
            assert!(
                audio
                    .iter()
                    .all(|track| track.iter().any(|sample| sample.abs() > 1000)),
                "audio must survive trim and tempo conversion"
            );
        }
    }

    #[test]
    fn fractional_microsecond_boundaries_select_the_requested_picture_at_every_speed() {
        let colors = [
            [220, 20, 20, 255],
            [20, 220, 20, 255],
            [20, 20, 220, 255],
            [20, 220, 220, 255],
            [220, 20, 220, 255],
            [220, 220, 20, 255],
            [220, 220, 220, 255],
        ];
        for (fps, first) in [(60, 1), (144, 2)] {
            let directory = tempfile::tempdir().unwrap();
            let mut config = fixture(directory.path(), ExportFormat::Mp4);
            config.fps = fps;
            let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
            for (index, color) in colors.iter().enumerate() {
                encoder
                    .push_rgba_frame_at_pts(index as u64, &color.repeat(32 * 24))
                    .unwrap();
            }
            encoder.finish_at_pts(colors.len() as u64).unwrap();
            let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
            let token = CancellationToken::default();
            let mut reader = VideoReader::open(source.clone(), false).unwrap();
            let mut pixels = Vec::new();
            for (index, expected) in colors.iter().enumerate() {
                reader.read(source.boundary(index as u64), &token).unwrap();
                reader.copy_rgba(&mut pixels).unwrap();
                assert!(
                    pixels[..3]
                        .iter()
                        .zip(expected)
                        .all(|(a, b)| a.abs_diff(*b) <= 12),
                    "{fps} FPS boundary {index} chose the preceding picture: {:?} vs {expected:?}",
                    &pixels[..4]
                );
            }
            for percent in [25, 50, 75, 100, 125, 150, 175, 200, 300] {
                let destination = directory.path().join(format!("trim-{percent}.mp4"));
                source
                    .export_with_speed(
                        source.range(first, first + 4).unwrap(),
                        PlaybackSpeed::from_percent(percent).unwrap(),
                        &destination,
                        &token,
                        |_| {},
                    )
                    .unwrap();
                let output = ClipSource::open(&destination, Some(config.clone())).unwrap();
                let mut reader = VideoReader::open(output, false).unwrap();
                reader.read(0, &token).unwrap();
                reader.copy_rgba(&mut pixels).unwrap();
                assert!(
                    pixels[..3]
                        .iter()
                        .zip(colors[first as usize])
                        .all(|(a, b)| a.abs_diff(b) <= 12),
                    "{fps} FPS {percent}% non-keyframe cut lost its first selected picture: {:?}",
                    &pixels[..4]
                );
            }
        }
    }

    #[test]
    fn legacy_matroska_input_remains_decodable_and_trimmable() {
        // Legacy recordings use this input container. Creating Matroska during
        // the test would require its intentionally omitted application muxer.
        let bytes = include_bytes!("../tests/fixtures/legacy-matroska-h264.mkv");
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("legacy.mkv");
        std::fs::write(&path, bytes).unwrap();
        let mut config = fixture(directory.path(), ExportFormat::Mp4);
        config.width = 16;
        config.height = 16;
        let source = ClipSource::open(&path, Some(config.clone())).unwrap();
        assert_eq!((source.config.width, source.config.height), (16, 16));
        assert_eq!(source.frame_count(), 3);
        let token = CancellationToken::default();
        let mut reader = VideoReader::open(source.clone(), false).unwrap();
        let mut pixels = Vec::new();
        let mut last = 0;
        for at in [0, 100_000, 200_000] {
            reader.read(at, &token).unwrap();
            reader.copy_rgba(&mut pixels).unwrap();
            assert!(pixels[0] > last + 40, "legacy source picture at {at}");
            last = pixels[0];
        }
        let destination = directory.path().join("trimmed.mp4");
        source
            .export(source.range(1, 3).unwrap(), &destination, &token, |_| {})
            .unwrap();
        let trimmed = ClipSource::open(&destination, Some(config)).unwrap();
        assert_eq!(trimmed.frame_count(), 2);
        let mut reader = VideoReader::open(trimmed, false).unwrap();
        reader.read(0, &token).unwrap();
        reader.copy_rgba(&mut pixels).unwrap();
        assert!((105..=135).contains(&pixels[0]));
        assert_eq!(std::fs::read(path).unwrap(), bytes);
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
    fn trim_speed_preserves_selected_start_and_retimes_every_container() {
        for format in [
            ExportFormat::Mp4,
            ExportFormat::Gif,
            ExportFormat::Apng,
            ExportFormat::Webp,
        ] {
            let directory = tempfile::tempdir().unwrap();
            let config = fixture(directory.path(), format);
            let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
            for index in 0..9 {
                let color = [20 + index as u8 * 20, 220 - index as u8 * 20, 40, 255];
                encoder
                    .push_rgba_frame_at_pts(index, &color.repeat(32 * 24))
                    .unwrap();
            }
            encoder.finish_at_duration_ms(900).unwrap();
            let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
            let range = source.range(1, 8).unwrap();
            for percent in [25, 50, 75, 100, 125, 150, 175, 200, 300] {
                let speed = PlaybackSpeed::from_percent(percent).unwrap();
                let path = directory
                    .path()
                    .join(format!("speed-{percent}.{}", format.file_extension()));
                let mut progress = Vec::new();
                source
                    .export_with_speed(
                        range,
                        speed,
                        &path,
                        &CancellationToken::default(),
                        |value| progress.push(value),
                    )
                    .unwrap();
                assert_eq!(progress.last(), Some(&100.0));
                assert!(progress.windows(2).all(|pair| pair[0] <= pair[1]));
                let trimmed = ClipSource::open(&path, Some(config.clone())).unwrap();
                let expected = speed.output_duration_us(range.duration_us()).unwrap();
                assert!(
                    trimmed.duration_us.abs_diff(expected) <= 10_000,
                    "{format:?} {percent}%: {} vs {expected}",
                    trimmed.duration_us
                );
                let mut reader = VideoReader::open(trimmed, false).unwrap();
                reader.read(0, &CancellationToken::default()).unwrap();
                let mut pixels = Vec::new();
                reader.copy_rgba(&mut pixels).unwrap();
                assert!(
                    pixels[1] > pixels[0] + 100,
                    "{format:?} {percent}% changed the selected first picture"
                );
            }
            assert!(source.path.exists());
        }
    }

    #[test]
    fn fast_gif_collapses_sub_tick_boundaries_without_drift_or_losing_first_picture() {
        let directory = tempfile::tempdir().unwrap();
        let mut config = fixture(directory.path(), ExportFormat::Gif);
        config.fps = 100;
        let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
        for index in 0..9 {
            let color = if index == 0 {
                [220, 20, 20, 255]
            } else {
                [20, 220, 20, 255]
            };
            encoder
                .push_rgba_frame_at_pts(index, &color.repeat(32 * 24))
                .unwrap();
        }
        encoder.finish_at_duration_ms(90).unwrap();
        let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
        let path = directory.path().join("fast.gif");
        source
            .export_with_speed(
                source.range(0, source.frame_count()).unwrap(),
                PlaybackSpeed::from_percent(300).unwrap(),
                &path,
                &CancellationToken::default(),
                |_| {},
            )
            .unwrap();
        let trimmed = ClipSource::open(&path, Some(config)).unwrap();
        assert_eq!(trimmed.duration_us, 30_000);
        let mut reader = VideoReader::open(trimmed, false).unwrap();
        reader.read(0, &CancellationToken::default()).unwrap();
        let mut pixels = Vec::new();
        reader.copy_rgba(&mut pixels).unwrap();
        assert!(pixels[0] > pixels[1] + 100);
    }

    #[test]
    fn retimed_mp4_keeps_the_recording_frame_rate_without_extra_endpoint_slots() {
        let directory = tempfile::tempdir().unwrap();
        let mut config = fixture(directory.path(), ExportFormat::Mp4);
        config.fps = 60;
        let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
        for index in 0..60 {
            encoder
                .push_rgba_frame_at_pts(
                    index,
                    &[20 + index as u8 * 3, 80, 200, 255].repeat(32 * 24),
                )
                .unwrap();
        }
        encoder.finish_at_duration_ms(1000).unwrap();
        let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
        for percent in [25, 300] {
            let path = directory.path().join(format!("retimed-{percent}.mp4"));
            source
                .export_with_speed(
                    source.range(0, source.frame_count()).unwrap(),
                    PlaybackSpeed::from_percent(percent).unwrap(),
                    &path,
                    &CancellationToken::default(),
                    |_| {},
                )
                .unwrap();
            let mut input = open_input(&path).unwrap();
            let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
            let index = stream.index();
            let rate = stream.rate();
            let average = stream.avg_frame_rate();
            let packets = input
                .packets()
                .filter(|(stream, _)| stream.index() == index)
                .count();
            assert_eq!(
                rate,
                ffmpeg::Rational(60, 1),
                "nominal={rate:?}, average={average:?}, packets={packets}"
            );
            assert_eq!(
                average,
                ffmpeg::Rational(60, 1),
                "nominal={rate:?}, average={average:?}, packets={packets}"
            );
            assert_eq!(
                packets as u64,
                PlaybackSpeed::from_percent(percent)
                    .unwrap()
                    .output_frame_count(1_000_000, 60)
                    .unwrap()
            );
            let mut config = config.clone();
            config.output_path = path.clone();
            let exported = ClipSource::open(&path, Some(config)).unwrap();
            assert_eq!(exported.frame_count(), packets as u64);
        }
        let path = directory.path().join("four-slots.mp4");
        source
            .export_with_speed(
                source.range(0, 5).unwrap(),
                PlaybackSpeed::from_percent(125).unwrap(),
                &path,
                &CancellationToken::default(),
                |_| {},
            )
            .unwrap();
        let mut output_config = config.clone();
        output_config.output_path = path.clone();
        let exported = ClipSource::open(&path, Some(output_config)).unwrap();
        assert_eq!(
            exported.frame_count(),
            4,
            "microsecond import rounding must not add a presentation slot"
        );
        assert!(exported.range(4, 5).is_err());
    }

    #[test]
    fn retimed_fractional_frame_ranges_keep_exact_slot_counts() {
        for fps in [60, 144] {
            let directory = tempfile::tempdir().unwrap();
            let mut config = fixture(directory.path(), ExportFormat::Mp4);
            config.fps = fps;
            let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
            for index in 0..12 {
                encoder
                    .push_rgba_frame_at_pts(index, &[220, 20, 40, 255].repeat(32 * 24))
                    .unwrap();
            }
            encoder.finish_at_pts(12).unwrap();
            let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
            for (first, end) in [(1, 2), (2, 4), (1, 6), (11, 12), (0, 12)] {
                for percent in [25, 50, 75, 100, 125, 150, 175, 200, 300] {
                    let destination = directory
                        .path()
                        .join(format!("exact-{first}-{end}-{percent}.mp4"));
                    source
                        .export_with_speed(
                            source.range(first, end).unwrap(),
                            PlaybackSpeed::from_percent(percent).unwrap(),
                            &destination,
                            &CancellationToken::default(),
                            |_| {},
                        )
                        .unwrap();
                    let mut input = open_input(&destination).unwrap();
                    let video = input.streams().best(ffmpeg::media::Type::Video).unwrap();
                    let id = video.index();
                    let time_base = video.time_base();
                    let duration = video.duration().rescale(time_base, (1, 1_000_000));
                    let packets: Vec<_> = input
                        .packets()
                        .filter(|(stream, _)| stream.index() == id)
                        .map(|(_, packet)| {
                            packet.pts().unwrap().rescale(time_base, (1, fps as i32))
                        })
                        .collect();
                    let expected_frames = ((end - first) * 100).div_ceil(u64::from(percent));
                    if percent == 100 && (first != 0 || end != 12) {
                        assert_eq!(packets, vec![0], "unchanged pictures share one sample");
                    } else {
                        assert_eq!(
                            packets,
                            (0..expected_frames as i64).collect::<Vec<_>>(),
                            "{fps} FPS [{first}, {end}) at {percent}% must not gain rounded slots"
                        );
                    }
                    let expected_us = ((end - first) * 100 * 1_000_000)
                        .div_ceil(u64::from(fps) * u64::from(percent));
                    assert!(duration.abs_diff(expected_us as i64) <= 100);
                    let exported = ClipSource::open(&destination, Some(config.clone())).unwrap();
                    assert_eq!(exported.frame_count(), expected_frames);
                }
            }
        }
    }

    #[test]
    fn retimed_final_native_frame_keeps_its_partial_duration() {
        let directory = tempfile::tempdir().unwrap();
        let mut config = fixture(directory.path(), ExportFormat::Mp4);
        config.fps = 60;
        let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
        for index in 0..7 {
            encoder
                .push_rgba_frame_at_pts(index, &[220, 20, 40, 255].repeat(32 * 24))
                .unwrap();
        }
        encoder
            .finish_at_duration_us_cancelable(101_000, &CancellationToken::default())
            .unwrap();
        let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
        assert_eq!(source.frame_count(), 7);
        for percent in [25, 50, 75, 100, 125, 150, 175, 200, 300] {
            let speed = PlaybackSpeed::from_percent(percent).unwrap();
            let range = source.range(6, 7).unwrap();
            let timing = source.output_timing(range, speed).unwrap();
            assert_eq!(timing.frame_count().unwrap(), 1);
            assert!(
                timing
                    .duration_us()
                    .unwrap()
                    .abs_diff((100_000u64).div_ceil(u64::from(percent)))
                    <= 100
            );
            let destination = directory.path().join(format!("partial-{percent}.mp4"));
            source
                .export_with_speed(
                    range,
                    speed,
                    &destination,
                    &CancellationToken::default(),
                    |_| {},
                )
                .unwrap();
            let output = ClipSource::open(&destination, Some(config.clone())).unwrap();
            assert_eq!(output.frame_count(), 1);
            assert!(output.duration_us.abs_diff(timing.duration_us().unwrap()) <= 100);
        }
    }

    #[test]
    fn prepared_sdr_repeats_preserve_the_existing_encoder_color_conversion() {
        let directory = tempfile::tempdir().unwrap();
        let config = fixture(directory.path(), ExportFormat::Mp4);
        let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
        for index in 0..10 {
            let pixels: Vec<u8> = (0..32 * 24)
                .flat_map(|pixel| {
                    [
                        ((pixel * 17 + index * 13) % 256) as u8,
                        ((pixel * 43 + index * 29) % 256) as u8,
                        ((pixel * 71 + index * 7) % 256) as u8,
                        255,
                    ]
                })
                .collect();
            encoder.push_rgba_frame_at_pts(index, &pixels).unwrap();
        }
        encoder.finish_at_duration_ms(1000).unwrap();
        let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
        let range = source.range(2, 8).unwrap();
        let mut expected = Vec::new();
        for percent in [100, 25, 300] {
            let output = directory.path().join(format!("colors-{percent}.mp4"));
            source
                .export_with_speed(
                    range,
                    PlaybackSpeed::from_percent(percent).unwrap(),
                    &output,
                    &CancellationToken::default(),
                    |_| {},
                )
                .unwrap();
            let mut output_config = config.clone();
            output_config.output_path = output.clone();
            let output_source = ClipSource::open(&output, Some(output_config)).unwrap();
            let mut reader = VideoReader::open(output_source, false).unwrap();
            reader.read(0, &CancellationToken::default()).unwrap();
            let mut pixels = Vec::new();
            reader.copy_rgba(&mut pixels).unwrap();
            if percent == 100 {
                expected = pixels;
            } else {
                assert_eq!(
                    pixels, expected,
                    "prepared frame changed SDR color at {percent}%"
                );
            }
        }
    }

    #[test]
    fn partial_hdr_trim_preserves_sparse_pictures_depth_and_color() {
        let directory = tempfile::tempdir().unwrap();
        let mut config = fixture(directory.path(), ExportFormat::Mp4);
        config.codec = crate::VideoCodec::H265;
        config.fps = 30;
        let mut encoder = StreamingEncoder::builder(config.clone())
            .hdr10_cpu_input()
            .software_only()
            .create()
            .unwrap();
        assert_eq!(
            encoder.input_pixel_format(),
            ffmpeg::format::Pixel::YUV420P10LE
        );
        for (pts, luma) in [(0, 256u16), (30, 512), (60, 768)] {
            let mut frame = ffmpeg::frame::Video::new(encoder.input_pixel_format(), 32, 24);
            for plane in 0..3 {
                let value = if plane == 0 { luma } else { 512 };
                for sample in frame.data_mut(plane).chunks_exact_mut(2) {
                    sample.copy_from_slice(&value.to_le_bytes());
                }
            }
            crate::hdr::frame(&mut frame, true);
            encoder
                .push_prepared_video_frame_ref_at_pts(pts, &frame)
                .unwrap();
        }
        encoder.finish_at_pts(90).unwrap();
        let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
        assert!(source.hdr);
        let destination = directory.path().join("sparse-hdr-trim.mp4");
        let token = CancellationToken::default();
        source
            .export(source.range(15, 75).unwrap(), &destination, &token, |_| {})
            .unwrap();
        let mut input = open_input(&destination).unwrap();
        let video = input.streams().best(ffmpeg::media::Type::Video).unwrap();
        let index = video.index();
        let time_base = video.time_base();
        let mut timestamps: Vec<_> = input
            .packets()
            .filter(|(stream, _)| stream.index() == index)
            .map(|(_, packet)| packet.pts().unwrap().rescale(time_base, (1, 30)))
            .collect();
        timestamps.sort_unstable();
        assert_eq!(timestamps, [0, 15, 45]);
        let output = ClipSource::open(&destination, Some(config)).unwrap();
        assert!(output.hdr);
        assert_eq!(output.duration_us, 2_000_000);
        assert_eq!(output.frame_count(), 60);
        let mut reader = VideoReader::open(output, false).unwrap();
        for (at, expected) in [
            (0, 256u16),
            (500_000, 512),
            (1_500_000, 768),
            (1_999_999, 768),
        ] {
            reader.read(at, &token).unwrap();
            let frame = reader.current_frame().unwrap();
            assert_eq!(frame.format(), ffmpeg::format::Pixel::YUV420P10LE);
            assert_eq!(
                frame.color_transfer_characteristic(),
                ffmpeg::color::TransferCharacteristic::SMPTE2084
            );
            let actual = u16::from_le_bytes(frame.data(0)[..2].try_into().unwrap());
            assert!(
                actual.abs_diff(expected) <= 4,
                "{at}: {actual} vs {expected}"
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

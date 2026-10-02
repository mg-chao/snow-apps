use super::{ClipSource, check_cancel, media_error, open_input};
use crate::error::Result;
use ffmpeg::Rescale;
use ffmpeg_next as ffmpeg;
use snow_core::cancellation::CancellationToken;
use snow_recording_model::BundleAssetKind;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};

struct PcmSource {
    file: File,
    offset: u64,
    length: u64,
    id: String,
}
struct EncodedSource {
    input: ffmpeg::format::context::Input,
    stream: usize,
    time_base: ffmpeg::Rational,
    decoder: ffmpeg::decoder::Audio,
    resampler: ffmpeg::software::resampling::Context,
    samples: Vec<i16>,
    start: i64,
    eof: bool,
    exhausted: bool,
}

pub struct AudioReader {
    pcm: Vec<PcmSource>,
    encoded: Vec<EncodedSource>,
    routes: Vec<String>,
    output: Vec<Vec<i16>>,
    scratch: Vec<u8>,
}

impl AudioReader {
    pub fn open(source: &ClipSource) -> Result<Self> {
        let mut pcm = Vec::new();
        let mut encoded = Vec::new();
        if source.metadata.is_some() {
            let footer = snow_recording_model::read_recording_bundle_footer(&source.path)?;
            for track in footer
                .manifest
                .audio_tracks
                .iter()
                .filter(|track| track.recorded)
            {
                if track.sample_rate_hz != 48_000 || track.channels != 2 {
                    return Err(media_error("unsupported source PCM layout"));
                }
                let asset = footer
                    .asset(BundleAssetKind::AudioTrack, Some(&track.asset_id))
                    .ok_or_else(|| media_error("missing source PCM"))?;
                pcm.push(PcmSource {
                    file: File::open(&source.path)?,
                    offset: asset.offset,
                    length: asset.len,
                    id: track.track_id.clone(),
                });
            }
        } else {
            let input = open_input(&source.path)?;
            for stream in input
                .streams()
                .filter(|s| s.parameters().medium() == ffmpeg::media::Type::Audio)
            {
                let mut decoder =
                    ffmpeg::codec::context::Context::from_parameters(stream.parameters())
                        .map_err(media_error)?
                        .decoder()
                        .audio()
                        .map_err(media_error)?;
                if decoder.channel_layout().is_empty() {
                    decoder.set_channel_layout(ffmpeg::ChannelLayout::default(i32::from(
                        decoder.channels(),
                    )));
                }
                let resampler = ffmpeg::software::resampling::Context::get(
                    decoder.format(),
                    decoder.channel_layout(),
                    decoder.rate(),
                    ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Packed),
                    ffmpeg::ChannelLayout::STEREO,
                    48_000,
                )
                .map_err(media_error)?;
                encoded.push(EncodedSource {
                    input: open_input(&source.path)?,
                    stream: stream.index(),
                    time_base: stream.time_base(),
                    decoder,
                    resampler,
                    samples: Vec::new(),
                    start: 0,
                    eof: false,
                    exhausted: false,
                });
            }
        }
        Ok(Self {
            pcm,
            encoded,
            routes: source
                .config
                .audio
                .iter()
                .map(|track| track.track_id.clone())
                .collect(),
            output: vec![Vec::new(); source.config.audio.len()],
            scratch: Vec::new(),
        })
    }

    /// Read bounded stereo blocks in the source sample clock, zero-padding absent audio.
    pub fn read(
        &mut self,
        start: u64,
        count: usize,
        token: &CancellationToken,
    ) -> Result<&[Vec<i16>]> {
        check_cancel(token)?;
        for output in &mut self.output {
            output.resize(count * 2, 0);
            output.fill(0);
        }
        for pcm in &mut self.pcm {
            let offset = start.saturating_mul(4).min(pcm.length);
            let len = ((pcm.length - offset) as usize).min(count * 4);
            self.scratch.resize(len, 0);
            pcm.file.seek(SeekFrom::Start(pcm.offset + offset))?;
            pcm.file.read_exact(&mut self.scratch)?;
            for (route, output) in self.routes.iter().zip(&mut self.output) {
                if route != "mixed" && route != &pcm.id {
                    continue;
                }
                for (sample, bytes) in output.iter_mut().zip(self.scratch.chunks_exact(2)) {
                    *sample = sample.saturating_add(i16::from_le_bytes([bytes[0], bytes[1]]));
                }
            }
        }
        for (source, output) in self.encoded.iter_mut().zip(&mut self.output) {
            source.read(start, count, output, token)?;
        }
        Ok(&self.output)
    }
}

impl EncodedSource {
    fn read(
        &mut self,
        start: u64,
        count: usize,
        output: &mut [i16],
        token: &CancellationToken,
    ) -> Result<()> {
        let start = i64::try_from(start).map_err(media_error)?;
        if start < self.start
            || start
                > self
                    .start
                    .saturating_add(self.samples.len() as i64 / 2 + 48_000)
        {
            let time = start.rescale((1, 48_000), (1, 1_000_000));
            self.input.seek(time, ..time).map_err(media_error)?;
            self.decoder.flush();
            self.resampler = ffmpeg::software::resampling::Context::get(
                self.decoder.format(),
                self.decoder.channel_layout(),
                self.decoder.rate(),
                ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Packed),
                ffmpeg::ChannelLayout::STEREO,
                48_000,
            )
            .map_err(media_error)?;
            self.samples.clear();
            self.eof = false;
            self.exhausted = false;
        }
        let end = start + count as i64;
        loop {
            check_cancel(token)?;
            let block_end = self.start + self.samples.len() as i64 / 2;
            let from = start.max(self.start);
            let to = end.min(block_end);
            if from < to {
                let source = (from - self.start) as usize * 2;
                let destination = (from - start) as usize * 2;
                let len = (to - from) as usize * 2;
                output[destination..destination + len]
                    .copy_from_slice(&self.samples[source..source + len]);
            }
            if block_end >= end || self.exhausted || self.start >= end {
                break;
            }
            self.decode_next(token)?;
        }
        Ok(())
    }

    fn decode_next(&mut self, token: &CancellationToken) -> Result<()> {
        loop {
            check_cancel(token)?;
            let mut decoded = ffmpeg::frame::Audio::empty();
            match self.decoder.receive_frame(&mut decoded) {
                Ok(()) => {
                    self.start = decoded
                        .timestamp()
                        .map(|pts| pts.rescale(self.time_base, (1, 48_000)))
                        .unwrap_or(self.start + self.samples.len() as i64 / 2);
                    let mut converted = ffmpeg::frame::Audio::empty();
                    self.resampler
                        .run(&decoded, &mut converted)
                        .map_err(media_error)?;
                    self.samples.clear();
                    self.samples.extend(
                        converted.data(0)[..converted.samples() * 4]
                            .chunks_exact(2)
                            .map(|bytes| i16::from_le_bytes([bytes[0], bytes[1]])),
                    );
                    return Ok(());
                }
                Err(ffmpeg::Error::Eof) => {
                    self.exhausted = true;
                    return Ok(());
                }
                Err(error) if crate::ffmpeg_util::is_eagain(&error) && !self.eof => {}
                Err(error) => return Err(media_error(error)),
            }
            loop {
                check_cancel(token)?;
                let mut packet = ffmpeg::Packet::empty();
                match packet.read(&mut self.input) {
                    Ok(()) if packet.stream() == self.stream => {
                        self.decoder.send_packet(&packet).map_err(media_error)?;
                        break;
                    }
                    Ok(()) => {}
                    Err(ffmpeg::Error::Eof) => {
                        self.decoder.send_eof().map_err(media_error)?;
                        self.eof = true;
                        break;
                    }
                    Err(error) => return Err(media_error(error)),
                }
            }
        }
    }
}

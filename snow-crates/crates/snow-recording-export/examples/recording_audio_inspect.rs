//! Independent AAC inspection for recording benchmark artifacts.
//! The production FFmpeg profile intentionally omits audio decoders.

use anyhow::{Context, Result, bail};
use std::{collections::BTreeMap, fs::File};
use symphonia::core::{
    audio::SampleBuffer,
    codecs::{CODEC_TYPE_AAC, DecoderOptions},
    errors::Error,
    formats::FormatOptions,
    io::MediaSourceStream,
    meta::MetadataOptions,
    probe::Hint,
};

fn main() -> Result<()> {
    let path = std::env::args().nth(1).context("missing media path")?;
    let source = MediaSourceStream::new(Box::new(File::open(&path)?), Default::default());
    let mut hint = Hint::new();
    hint.with_extension("mp4");
    let mut format = symphonia::default::get_probe()
        .format(
            &hint,
            source,
            &FormatOptions {
                enable_gapless: true,
                ..Default::default()
            },
            &MetadataOptions::default(),
        )?
        .format;
    let mut tracks = BTreeMap::new();
    for track in format.tracks() {
        if track.codec_params.codec != CODEC_TYPE_AAC {
            continue;
        }
        let rate = track.codec_params.sample_rate.unwrap_or_default();
        let channels = track
            .codec_params
            .channels
            .map(|channels| channels.count())
            .unwrap_or_default();
        let decoder = symphonia::default::get_codecs()
            .make(&track.codec_params, &DecoderOptions::default())?;
        tracks.insert(track.id, (decoder, rate, channels, 0u64, 0u64, 0i16));
    }
    if tracks.is_empty() {
        bail!("no AAC tracks in {path}");
    }
    loop {
        let packet = match format.next_packet() {
            Ok(packet) => packet,
            Err(Error::IoError(error)) if error.kind() == std::io::ErrorKind::UnexpectedEof => {
                break;
            }
            Err(error) => return Err(error.into()),
        };
        let Some((decoder, rate, channels, frames, nonzero, peak)) =
            tracks.get_mut(&packet.track_id())
        else {
            continue;
        };
        let decoded = decoder.decode(&packet)?;
        *rate = decoded.spec().rate;
        *channels = decoded.spec().channels.count();
        *frames += decoded.frames() as u64;
        let mut samples = SampleBuffer::<i16>::new(decoded.capacity() as u64, *decoded.spec());
        samples.copy_interleaved_ref(decoded);
        for sample in samples.samples() {
            *nonzero += u64::from(*sample != 0);
            *peak = (*peak).max(sample.saturating_abs());
        }
    }
    for (id, (_, rate, channels, frames, nonzero, peak)) in tracks {
        println!(
            "aac,{path},track={id},rate={rate},channels={channels},decoded_frames={frames},decoded_ms={:.3},nonzero_samples={nonzero},peak={peak}",
            frames as f64 * 1000.0 / f64::from(rate),
        );
    }
    Ok(())
}

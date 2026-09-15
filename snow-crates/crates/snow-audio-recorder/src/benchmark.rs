use std::time::Duration;

use crate::convert::{AudioConverter, NativeAudioFormat, NativeSampleFormat};
use crate::error::AudioResult;
use crate::format::AudioFormat;
use crate::packet::{AudioPacket, AudioSourceKind};
use crate::platform::windows::wasapi_source::{PacketAccumulator, PendingMetadata};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BenchSampleFormat {
    I16,
    F32,
    I32,
}

impl BenchSampleFormat {
    fn into_native(self) -> NativeSampleFormat {
        match self {
            Self::I16 => NativeSampleFormat::I16,
            Self::F32 => NativeSampleFormat::F32,
            Self::I32 => NativeSampleFormat::I32,
        }
    }
}

pub struct ConverterBenchHarness {
    inner: AudioConverter,
}

impl ConverterBenchHarness {
    pub fn new(
        input_rate: u32,
        input_channels: u16,
        input_format: BenchSampleFormat,
        output_rate: u32,
        output_channels: u16,
    ) -> AudioResult<Self> {
        let input = NativeAudioFormat {
            sample_rate: input_rate,
            channels: input_channels,
            sample_format: input_format.into_native(),
        };
        let output = AudioFormat::new(output_rate, output_channels);
        Ok(Self {
            inner: AudioConverter::new(input, output)?,
        })
    }

    pub fn convert(&mut self, input_bytes: &[u8], input_frames: u32) -> AudioResult<Vec<i16>> {
        self.inner.convert_chunk(input_bytes, input_frames)
    }
}

pub struct AccumulatorBenchHarness {
    inner: PacketAccumulator,
    sequence: u64,
}

impl AccumulatorBenchHarness {
    pub fn new(
        source: AudioSourceKind,
        sample_rate: u32,
        channels: u16,
        packet_duration: Duration,
    ) -> AudioResult<Self> {
        Ok(Self {
            inner: PacketAccumulator::new(
                source,
                AudioFormat::new(sample_rate, channels),
                packet_duration,
            )?,
            sequence: 0,
        })
    }

    pub fn push_chunk(&mut self, samples: Vec<i16>, frames: u32) -> AudioResult<Vec<AudioPacket>> {
        self.inner.push_chunk(
            samples,
            frames,
            PendingMetadata::default(),
            &mut self.sequence,
        )
    }
}

pub fn make_i16_bytes(samples: &[i16]) -> Vec<u8> {
    let mut out = Vec::with_capacity(std::mem::size_of_val(samples));
    for &sample in samples {
        out.extend_from_slice(&sample.to_le_bytes());
    }
    out
}

pub fn make_f32_bytes(samples: &[f32]) -> Vec<u8> {
    let mut out = Vec::with_capacity(std::mem::size_of_val(samples));
    for &sample in samples {
        out.extend_from_slice(&sample.to_le_bytes());
    }
    out
}

pub fn make_i32_bytes(samples: &[i32]) -> Vec<u8> {
    let mut out = Vec::with_capacity(std::mem::size_of_val(samples));
    for &sample in samples {
        out.extend_from_slice(&sample.to_le_bytes());
    }
    out
}

pub fn make_sine_i16_samples(
    frames: u32,
    channels: u16,
    frequency_hz: f32,
    sample_rate_hz: u32,
) -> Vec<i16> {
    let mut out = Vec::with_capacity((frames as usize) * usize::from(channels));
    let sample_rate = sample_rate_hz.max(1) as f32;
    for frame in 0..frames {
        let phase = (frame as f32 * frequency_hz * std::f32::consts::TAU) / sample_rate;
        let sample = (phase.sin() * i16::MAX as f32 * 0.5) as i16;
        for _ in 0..channels {
            out.push(sample);
        }
    }
    out
}

pub fn make_sine_f32_samples(
    frames: u32,
    channels: u16,
    frequency_hz: f32,
    sample_rate_hz: u32,
) -> Vec<f32> {
    let mut out = Vec::with_capacity((frames as usize) * usize::from(channels));
    let sample_rate = sample_rate_hz.max(1) as f32;
    for frame in 0..frames {
        let phase = (frame as f32 * frequency_hz * std::f32::consts::TAU) / sample_rate;
        let sample = phase.sin() * 0.5;
        for _ in 0..channels {
            out.push(sample);
        }
    }
    out
}

pub fn make_sine_i32_samples(
    frames: u32,
    channels: u16,
    frequency_hz: f32,
    sample_rate_hz: u32,
) -> Vec<i32> {
    let mut out = Vec::with_capacity((frames as usize) * usize::from(channels));
    let sample_rate = sample_rate_hz.max(1) as f32;
    for frame in 0..frames {
        let phase = (frame as f32 * frequency_hz * std::f32::consts::TAU) / sample_rate;
        let sample = (phase.sin() * i32::MAX as f32 * 0.5) as i32;
        for _ in 0..channels {
            out.push(sample);
        }
    }
    out
}

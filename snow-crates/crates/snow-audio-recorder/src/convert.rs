use crate::error::{AudioError, AudioResult};
use crate::format::{AudioFormat, MAX_CHANNELS};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum NativeSampleFormat {
    F32,
    #[cfg_attr(not(windows), allow(dead_code))]
    I16,
    #[cfg_attr(not(windows), allow(dead_code))]
    I32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct NativeAudioFormat {
    pub sample_rate: u32,
    pub channels: u16,
    pub sample_format: NativeSampleFormat,
}

impl NativeAudioFormat {
    pub fn bytes_per_sample(self) -> usize {
        match self.sample_format {
            NativeSampleFormat::F32 => 4,
            NativeSampleFormat::I16 => 2,
            NativeSampleFormat::I32 => 4,
        }
    }

    pub fn bytes_per_frame(self) -> AudioResult<usize> {
        usize::from(self.channels)
            .checked_mul(self.bytes_per_sample())
            .ok_or(AudioError::BufferOverflow)
    }
}

pub(crate) trait Resampler {
    fn process(&mut self, input: &[f32], out: &mut Vec<f32>);
}

pub(crate) enum ResamplerKind {
    WindowedSinc(WindowedSincResampler),
}

impl Resampler for ResamplerKind {
    fn process(&mut self, input: &[f32], out: &mut Vec<f32>) {
        match self {
            Self::WindowedSinc(inner) => inner.process(input, out),
        }
    }
}

pub(crate) struct AudioConverter {
    input: NativeAudioFormat,
    output: AudioFormat,
    f32_buffer: Vec<f32>,
    channel_buffer: Vec<f32>,
    resample_buffer: Vec<f32>,
    i16_buffer: Vec<i16>,
    output_buffer: Vec<i16>,
    resampler: Option<ResamplerKind>,
}

impl AudioConverter {
    pub fn new(input: NativeAudioFormat, output: AudioFormat) -> AudioResult<Self> {
        let resampler = if input.sample_rate == output.sample_rate {
            None
        } else {
            Some(ResamplerKind::WindowedSinc(WindowedSincResampler::new(
                input.sample_rate,
                output.sample_rate,
                output.channels,
            )))
        };
        Self::with_resampler(input, output, resampler)
    }

    pub fn with_resampler(
        input: NativeAudioFormat,
        output: AudioFormat,
        resampler: Option<ResamplerKind>,
    ) -> AudioResult<Self> {
        output.validate()?;
        Ok(Self {
            input,
            output,
            f32_buffer: Vec::new(),
            channel_buffer: Vec::new(),
            resample_buffer: Vec::new(),
            i16_buffer: Vec::new(),
            output_buffer: Vec::new(),
            resampler,
        })
    }

    pub fn convert_chunk(
        &mut self,
        input_bytes: &[u8],
        input_frames: u32,
    ) -> AudioResult<Vec<i16>> {
        let required = (input_frames as usize)
            .checked_mul(self.input.bytes_per_frame()?)
            .ok_or(AudioError::BufferOverflow)?;
        if input_bytes.len() < required {
            return Err(AudioError::BufferOverflow);
        }
        self.output_buffer.clear();
        if input_frames == 0 {
            return Ok(Vec::new());
        }

        if self.resampler.is_none() {
            match self.input.sample_format {
                NativeSampleFormat::I16 if self.input.channels == self.output.channels => {
                    copy_i16_bytes_to_samples(
                        input_bytes,
                        input_frames,
                        self.input.channels,
                        &mut self.i16_buffer,
                    )?;

                    self.output_buffer.extend_from_slice(&self.i16_buffer);

                    return Ok(std::mem::take(&mut self.output_buffer));
                }
                NativeSampleFormat::F32 if self.input.channels == self.output.channels => {
                    quantize_f32_bytes_to_i16(
                        input_bytes,
                        input_frames,
                        self.input.channels,
                        &mut self.output_buffer,
                    )?;
                    return Ok(std::mem::take(&mut self.output_buffer));
                }
                NativeSampleFormat::I32 if self.input.channels == self.output.channels => {
                    quantize_i32_bytes_to_i16(
                        input_bytes,
                        input_frames,
                        self.input.channels,
                        &mut self.output_buffer,
                    )?;
                    return Ok(std::mem::take(&mut self.output_buffer));
                }
                _ => {}
            }
        }

        decode_interleaved_to_f32(
            input_bytes,
            input_frames,
            self.input.channels,
            self.input.sample_format,
            &mut self.f32_buffer,
        )?;

        convert_channels_f32(
            &self.f32_buffer,
            self.input.channels,
            self.output.channels,
            &mut self.channel_buffer,
        );

        let output_samples: &[f32] = if let Some(resampler) = self.resampler.as_mut() {
            self.resample_buffer.clear();
            resampler.process(&self.channel_buffer, &mut self.resample_buffer);
            &self.resample_buffer
        } else {
            &self.channel_buffer
        };

        quantize_f32_samples_to_i16(output_samples, &mut self.output_buffer);
        Ok(std::mem::take(&mut self.output_buffer))
    }
}

fn sample_count(frames: u32, channels: u16) -> AudioResult<usize> {
    (frames as usize)
        .checked_mul(usize::from(channels))
        .ok_or(AudioError::BufferOverflow)
}

fn copy_i16_bytes_to_samples(
    input: &[u8],
    frames: u32,
    channels: u16,
    out: &mut Vec<i16>,
) -> AudioResult<()> {
    let sample_count = sample_count(frames, channels)?;
    let needed = sample_count
        .checked_mul(std::mem::size_of::<i16>())
        .ok_or(AudioError::BufferOverflow)?;
    if input.len() < needed {
        return Err(AudioError::BufferOverflow);
    }

    out.clear();
    out.resize(sample_count, 0);

    #[cfg(target_endian = "little")]
    unsafe {
        std::ptr::copy_nonoverlapping(input.as_ptr(), out.as_mut_ptr() as *mut u8, needed);
    }

    #[cfg(not(target_endian = "little"))]
    for (slot, chunk) in out.iter_mut().zip(input[..needed].chunks_exact(2)) {
        *slot = i16::from_le_bytes([chunk[0], chunk[1]]);
    }

    Ok(())
}

fn quantize_f32_bytes_to_i16(
    input: &[u8],
    frames: u32,
    channels: u16,
    out: &mut Vec<i16>,
) -> AudioResult<()> {
    let sample_count = sample_count(frames, channels)?;
    let needed = sample_count
        .checked_mul(4)
        .ok_or(AudioError::BufferOverflow)?;
    if input.len() < needed {
        return Err(AudioError::BufferOverflow);
    }

    out.clear();
    out.reserve(sample_count);
    for chunk in input[..needed].chunks_exact(4) {
        let sample = f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
        out.push(quantize_f32(sample));
    }
    Ok(())
}

fn quantize_i32_bytes_to_i16(
    input: &[u8],
    frames: u32,
    channels: u16,
    out: &mut Vec<i16>,
) -> AudioResult<()> {
    let sample_count = sample_count(frames, channels)?;
    let needed = sample_count
        .checked_mul(4)
        .ok_or(AudioError::BufferOverflow)?;
    if input.len() < needed {
        return Err(AudioError::BufferOverflow);
    }

    out.clear();
    out.reserve(sample_count);
    for chunk in input[..needed].chunks_exact(4) {
        let sample = i32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
        let normalized = sample as f32 / i32::MAX as f32;
        out.push(quantize_f32(normalized));
    }
    Ok(())
}

fn decode_interleaved_to_f32(
    input: &[u8],
    frames: u32,
    channels: u16,
    format: NativeSampleFormat,
    out: &mut Vec<f32>,
) -> AudioResult<()> {
    let sample_count = sample_count(frames, channels)?;
    out.clear();
    out.reserve(sample_count);

    match format {
        NativeSampleFormat::F32 => {
            let needed = sample_count
                .checked_mul(4)
                .ok_or(AudioError::BufferOverflow)?;
            if input.len() < needed {
                return Err(AudioError::BufferOverflow);
            }
            for chunk in input[..needed].chunks_exact(4) {
                out.push(f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]));
            }
        }
        NativeSampleFormat::I16 => {
            let needed = sample_count
                .checked_mul(2)
                .ok_or(AudioError::BufferOverflow)?;
            if input.len() < needed {
                return Err(AudioError::BufferOverflow);
            }
            for chunk in input[..needed].chunks_exact(2) {
                let sample = i16::from_le_bytes([chunk[0], chunk[1]]);
                out.push(sample as f32 / i16::MAX as f32);
            }
        }
        NativeSampleFormat::I32 => {
            let needed = sample_count
                .checked_mul(4)
                .ok_or(AudioError::BufferOverflow)?;
            if input.len() < needed {
                return Err(AudioError::BufferOverflow);
            }
            for chunk in input[..needed].chunks_exact(4) {
                let sample = i32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
                out.push(sample as f32 / i32::MAX as f32);
            }
        }
    }

    Ok(())
}

fn convert_channels_f32(input: &[f32], in_channels: u16, out_channels: u16, output: &mut Vec<f32>) {
    output.clear();

    if in_channels == out_channels {
        output.extend_from_slice(input);
        return;
    }

    let in_ch = usize::from(in_channels);
    let out_ch = usize::from(out_channels);
    if in_ch == 0 || out_ch == 0 {
        return;
    }

    let frame_count = input.len() / in_ch;
    output.reserve(frame_count * out_ch);

    for frame in input.chunks_exact(in_ch) {
        if out_ch == 2 && in_ch > 2 {
            let (left, right) = downmix_frame_to_stereo_f32(frame);
            output.push(left);
            output.push(right);
            continue;
        }

        if out_ch == 1 {
            let sum: f32 = frame.iter().copied().sum();
            output.push(sum / in_ch as f32);
            continue;
        }

        if in_ch == 1 {
            for _ in 0..out_ch {
                output.push(frame[0]);
            }
            continue;
        }

        if out_ch < in_ch {
            let mut accum = [0.0f32; MAX_CHANNELS as usize];
            let mut count = [0u32; MAX_CHANNELS as usize];
            for (idx, &sample) in frame.iter().enumerate() {
                let dest = idx % out_ch;
                accum[dest] += sample;
                count[dest] += 1;
            }
            for ch in 0..out_ch {
                output.push(if count[ch] > 0 {
                    accum[ch] / count[ch] as f32
                } else {
                    0.0
                });
            }
            continue;
        }

        for idx in 0..out_ch {
            output.push(frame[idx % in_ch]);
        }
    }
}

fn downmix_frame_to_stereo_f32(frame: &[f32]) -> (f32, f32) {
    const CENTER_GAIN: f32 = 0.707_106_77;
    const SURROUND_GAIN: f32 = 0.707_106_77;
    const BACK_CENTER_GAIN: f32 = 0.5;

    if frame.is_empty() {
        return (0.0, 0.0);
    }

    let mut left = frame[0];
    let mut right = *frame.get(1).unwrap_or(&frame[0]);

    match frame.len() {
        3 => {
            let center = frame[2];
            left += center * CENTER_GAIN;
            right += center * CENTER_GAIN;
        }
        4 => {
            let back_left = frame[2];
            let back_right = frame[3];
            left += back_left * SURROUND_GAIN;
            right += back_right * SURROUND_GAIN;
        }
        5 => {
            let center = frame[2];
            let back_left = frame[3];
            let back_right = frame[4];
            left += center * CENTER_GAIN + back_left * SURROUND_GAIN;
            right += center * CENTER_GAIN + back_right * SURROUND_GAIN;
        }
        6 => {
            let center = frame[2];
            let back_left = frame[4];
            let back_right = frame[5];
            left += center * CENTER_GAIN + back_left * SURROUND_GAIN;
            right += center * CENTER_GAIN + back_right * SURROUND_GAIN;
        }
        7 => {
            let center = frame[2];
            let back_center = frame[4];
            let side_left = frame[5];
            let side_right = frame[6];
            left +=
                center * CENTER_GAIN + back_center * BACK_CENTER_GAIN + side_left * SURROUND_GAIN;
            right +=
                center * CENTER_GAIN + back_center * BACK_CENTER_GAIN + side_right * SURROUND_GAIN;
        }
        8 => {
            let center = frame[2];
            let back_left = frame[4];
            let back_right = frame[5];
            let side_left = frame[6];
            let side_right = frame[7];
            left += center * CENTER_GAIN + back_left * SURROUND_GAIN + side_left * SURROUND_GAIN;
            right += center * CENTER_GAIN + back_right * SURROUND_GAIN + side_right * SURROUND_GAIN;
        }
        _ => {
            let mut extra_left = 0.0;
            let mut extra_right = 0.0;
            let mut left_count = 0u32;
            let mut right_count = 0u32;
            for (index, &sample) in frame.iter().enumerate().skip(2) {
                if index % 2 == 0 {
                    extra_left += sample;
                    left_count += 1;
                } else {
                    extra_right += sample;
                    right_count += 1;
                }
            }
            if left_count > 0 {
                left += extra_left / left_count as f32;
            }
            if right_count > 0 {
                right += extra_right / right_count as f32;
            }
        }
    }

    normalize_stereo_pair(left, right)
}

fn normalize_stereo_pair(left: f32, right: f32) -> (f32, f32) {
    let peak = left.abs().max(right.abs()).max(1.0);
    (left / peak, right / peak)
}

fn quantize_f32_samples_to_i16(samples: &[f32], out: &mut Vec<i16>) {
    out.clear();
    out.reserve(samples.len());
    for &sample in samples {
        out.push(quantize_f32(sample));
    }
}

fn quantize_f32(sample: f32) -> i16 {
    let clamped = sample.clamp(-1.0, 1.0);
    (clamped * i16::MAX as f32) as i16
}

/// Streaming band-limited resampler with a Blackman-windowed sinc kernel.
///
/// Microphones commonly expose 44.1 kHz while recordings use 48 kHz. Linear
/// interpolation audibly attenuates the upper voice band and aliases when a
/// high-rate loopback device is downsampled. This polyphase kernel keeps the
/// conversion state across WASAPI packets and applies an anti-aliasing cutoff.
pub(crate) struct WindowedSincResampler {
    channels: u16,
    step: f64,
    position_in_frames: f64,
    input_buffer: Vec<f32>,
    start_sample: usize,
    kernels: Vec<Vec<f32>>,
}

impl WindowedSincResampler {
    const HALF_TAPS: usize = 24;
    const PHASE_COUNT: usize = 1024;

    pub fn new(in_rate: u32, out_rate: u32, channels: u16) -> Self {
        let in_rate = in_rate.max(1);
        let out_rate = out_rate.max(1);
        let cutoff = (out_rate as f64 / in_rate as f64).min(1.0) * 0.94;
        Self {
            channels,
            step: in_rate as f64 / out_rate as f64,
            position_in_frames: Self::HALF_TAPS as f64,
            input_buffer: Vec::new(),
            start_sample: 0,
            kernels: Self::build_kernels(cutoff),
        }
    }

    fn build_kernels(cutoff: f64) -> Vec<Vec<f32>> {
        let tap_count = Self::HALF_TAPS * 2;
        (0..Self::PHASE_COUNT)
            .map(|phase_index| {
                let fraction = phase_index as f64 / Self::PHASE_COUNT as f64;
                let mut kernel = Vec::with_capacity(tap_count);
                let mut sum = 0.0f64;
                for tap in 0..tap_count {
                    let offset = tap as f64 - (Self::HALF_TAPS - 1) as f64 - fraction;
                    let sinc = if offset.abs() < f64::EPSILON {
                        cutoff
                    } else {
                        (std::f64::consts::PI * cutoff * offset).sin()
                            / (std::f64::consts::PI * offset)
                    };
                    let window_position = tap as f64 / (tap_count - 1) as f64;
                    let window = 0.42 - 0.5 * (std::f64::consts::TAU * window_position).cos()
                        + 0.08 * (2.0 * std::f64::consts::TAU * window_position).cos();
                    let coefficient = sinc * window;
                    kernel.push(coefficient as f32);
                    sum += coefficient;
                }
                if sum.abs() > f64::EPSILON {
                    for coefficient in &mut kernel {
                        *coefficient /= sum as f32;
                    }
                }
                kernel
            })
            .collect()
    }

    fn seed_history_if_needed(&mut self, input: &[f32], channels: usize) {
        if !self.input_buffer.is_empty() || input.len() < channels {
            return;
        }
        for _ in 0..Self::HALF_TAPS {
            self.input_buffer.extend_from_slice(&input[..channels]);
        }
    }

    fn compact_if_needed(&mut self, channels: usize) {
        if self.start_sample == 0 {
            return;
        }

        if self.start_sample >= self.input_buffer.len() {
            self.input_buffer.clear();
            self.start_sample = 0;
            return;
        }

        if self.start_sample >= channels * 4096 && self.start_sample * 2 >= self.input_buffer.len()
        {
            self.input_buffer.copy_within(self.start_sample.., 0);
            self.input_buffer
                .truncate(self.input_buffer.len() - self.start_sample);
            self.start_sample = 0;
        }
    }
}

impl Resampler for WindowedSincResampler {
    fn process(&mut self, input: &[f32], out: &mut Vec<f32>) {
        let channels = usize::from(self.channels);
        if channels == 0 {
            return;
        }

        self.seed_history_if_needed(input, channels);
        self.input_buffer.extend_from_slice(input);
        let available_samples = self.input_buffer.len().saturating_sub(self.start_sample);
        let available_frames = available_samples / channels;

        while self.position_in_frames + (Self::HALF_TAPS as f64) < available_frames as f64 {
            let center = self.position_in_frames.floor() as usize;
            let fraction = self.position_in_frames - center as f64;
            let phase_index =
                ((fraction * Self::PHASE_COUNT as f64).round() as usize).min(Self::PHASE_COUNT - 1);
            let kernel = &self.kernels[phase_index];
            let first_frame = center - (Self::HALF_TAPS - 1);
            for ch in 0..channels {
                let mut sample = 0.0f32;
                for (tap, coefficient) in kernel.iter().copied().enumerate() {
                    let sample_index = self.start_sample + (first_frame + tap) * channels + ch;
                    sample += self.input_buffer[sample_index] * coefficient;
                }
                out.push(sample);
            }

            self.position_in_frames += self.step;
        }

        let consumed_frames =
            (self.position_in_frames.floor() as usize).saturating_sub(Self::HALF_TAPS);
        if consumed_frames > 0 {
            self.start_sample += consumed_frames * channels;
            self.position_in_frames -= consumed_frames as f64;
            self.compact_if_needed(channels);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn i16_bytes(samples: &[i16]) -> Vec<u8> {
        let mut out = Vec::with_capacity(samples.len() * 2);
        for &sample in samples {
            out.extend_from_slice(&sample.to_le_bytes());
        }
        out
    }

    fn f32_bytes(samples: &[f32]) -> Vec<u8> {
        let mut out = Vec::with_capacity(samples.len() * 4);
        for &sample in samples {
            out.extend_from_slice(&sample.to_le_bytes());
        }
        out
    }

    #[test]
    fn passthrough_i16_fast_path_copies_exact_samples() {
        let input = NativeAudioFormat {
            sample_rate: 48_000,
            channels: 2,
            sample_format: NativeSampleFormat::I16,
        };
        let output = AudioFormat::new(48_000, 2);
        let mut converter = AudioConverter::new(input, output).unwrap();

        let samples = vec![1i16, -2, 3, -4, 5, -6];
        let converted = converter.convert_chunk(&i16_bytes(&samples), 3).unwrap();
        assert_eq!(converted, samples);
    }

    #[test]
    fn converts_f32_to_i16_without_intermediate_format_switches() {
        let input = NativeAudioFormat {
            sample_rate: 48_000,
            channels: 1,
            sample_format: NativeSampleFormat::F32,
        };
        let output = AudioFormat::new(48_000, 1);
        let mut converter = AudioConverter::new(input, output).unwrap();

        let samples = vec![-1.0, -0.5, 0.0, 0.5, 1.0];
        let converted = converter.convert_chunk(&f32_bytes(&samples), 5).unwrap();
        assert_eq!(converted.len(), samples.len());
        assert_eq!(converted[0], i16::MIN + 1);
        assert_eq!(converted[2], 0);
        assert_eq!(converted[4], i16::MAX);
    }

    #[test]
    fn converts_channels_before_quantizing() {
        let input = NativeAudioFormat {
            sample_rate: 48_000,
            channels: 2,
            sample_format: NativeSampleFormat::I16,
        };
        let output = AudioFormat::new(48_000, 1);
        let mut converter = AudioConverter::new(input, output).unwrap();

        let samples = vec![10i16, 30, -20, 20];
        let converted = converter.convert_chunk(&i16_bytes(&samples), 2).unwrap();
        assert_eq!(converted, vec![20, 0]);
    }

    #[test]
    fn downmixes_5_1_center_equally_to_left_and_right() {
        let input = NativeAudioFormat {
            sample_rate: 48_000,
            channels: 6,
            sample_format: NativeSampleFormat::I16,
        };
        let output = AudioFormat::new(48_000, 2);
        let mut converter = AudioConverter::new(input, output).unwrap();

        let samples = vec![0i16, 0, i16::MAX / 2, 0, 0, 0];
        let converted = converter.convert_chunk(&i16_bytes(&samples), 1).unwrap();
        assert_eq!(converted.len(), 2);
        assert!(converted[0] > 0);
        assert_eq!(converted[0], converted[1]);
    }

    #[test]
    fn downmix_5_1_ignores_lfe_in_stereo_fold_down() {
        let input = NativeAudioFormat {
            sample_rate: 48_000,
            channels: 6,
            sample_format: NativeSampleFormat::I16,
        };
        let output = AudioFormat::new(48_000, 2);
        let mut converter = AudioConverter::new(input, output).unwrap();

        let samples = vec![0i16, 0, 0, i16::MAX, 0, 0];
        let converted = converter.convert_chunk(&i16_bytes(&samples), 1).unwrap();
        assert_eq!(converted, vec![0, 0]);
    }

    #[test]
    fn resampler_preserves_state_without_head_drain() {
        let mut resampler = WindowedSincResampler::new(48_000, 24_000, 1);
        let mut out = Vec::new();
        let first: Vec<f32> = (0..128).map(|value| value as f32 / 128.0).collect();
        resampler.process(&first, &mut out);
        assert!(!out.is_empty());

        let prior_start = resampler.start_sample;
        out.clear();
        let second: Vec<f32> = (128..256).map(|value| value as f32 / 256.0).collect();
        resampler.process(&second, &mut out);
        assert!(!out.is_empty());
        assert!(resampler.start_sample >= prior_start);
    }

    #[test]
    fn resampler_preserves_voice_band_amplitude() {
        let mut resampler = WindowedSincResampler::new(44_100, 48_000, 1);
        let input: Vec<f32> = (0..4_410)
            .map(|frame| (std::f32::consts::TAU * 8_000.0 * frame as f32 / 44_100.0).sin() * 0.5)
            .collect();
        let mut output = Vec::new();
        resampler.process(&input, &mut output);

        let steady_state = &output[100..output.len() - 100];
        let rms = (steady_state
            .iter()
            .map(|sample| sample * sample)
            .sum::<f32>()
            / steady_state.len() as f32)
            .sqrt();
        assert!(rms > 0.34, "voice-band RMS was attenuated to {rms}");
        assert!(rms < 0.37, "voice-band RMS overshot to {rms}");
    }

    #[test]
    fn downsampling_rejects_frequencies_above_output_nyquist() {
        let mut resampler = WindowedSincResampler::new(96_000, 48_000, 1);
        let input: Vec<f32> = (0..9_600)
            .map(|frame| (std::f32::consts::TAU * 30_000.0 * frame as f32 / 96_000.0).sin() * 0.5)
            .collect();
        let mut output = Vec::new();
        resampler.process(&input, &mut output);

        let rms = (output.iter().map(|sample| sample * sample).sum::<f32>()
            / output.len().max(1) as f32)
            .sqrt();
        assert!(rms < 0.01, "out-of-band RMS was {rms}");
    }
}

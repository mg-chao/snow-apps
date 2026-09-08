use std::{mem::MaybeUninit, sync::OnceLock};

use rayon::prelude::*;

use crate::{Frame, PixelFormat};

#[cfg(target_arch = "x86")]
use std::arch::x86::{
    __m128i, __m256i, _mm_loadl_epi64, _mm_loadu_si128, _mm256_add_epi32, _mm256_add_ps,
    _mm256_and_si256, _mm256_cmpgt_epi16, _mm256_cvtepi32_ps, _mm256_cvtepu8_epi16,
    _mm256_cvtepu8_epi32, _mm256_cvtps_epi32, _mm256_cvttps_epi32, _mm256_i32gather_epi32,
    _mm256_loadu_ps, _mm256_loadu_si256, _mm256_movemask_epi8, _mm256_mul_ps, _mm256_mullo_epi32,
    _mm256_or_si256, _mm256_set1_epi16, _mm256_set1_epi32, _mm256_set1_ps, _mm256_setr_epi32,
    _mm256_setzero_si256, _mm256_srli_epi32, _mm256_storeu_ps, _mm256_storeu_si256,
    _mm256_sub_epi16, _mm256_sub_ps,
};
#[cfg(target_arch = "x86_64")]
use std::arch::x86_64::{
    __m128i, __m256i, _mm_cmpgt_epi16, _mm_loadl_epi64, _mm_loadu_si128, _mm_movemask_epi8,
    _mm_packs_epi16, _mm_set1_epi16, _mm256_add_epi32, _mm256_add_ps, _mm256_and_si256,
    _mm256_cmpgt_epi16, _mm256_cvtepi32_ps, _mm256_cvtepu8_epi16, _mm256_cvtepu8_epi32,
    _mm256_cvtps_epi32, _mm256_cvttps_epi32, _mm256_i32gather_epi32, _mm256_loadu_ps,
    _mm256_loadu_si256, _mm256_movemask_epi8, _mm256_mul_ps, _mm256_mullo_epi32, _mm256_or_si256,
    _mm256_set1_epi16, _mm256_set1_epi32, _mm256_set1_ps, _mm256_setr_epi32, _mm256_setzero_si256,
    _mm256_srli_epi32, _mm256_storeu_ps, _mm256_storeu_si256, _mm256_sub_epi16, _mm256_sub_ps,
};

const SCALE_FACTOR: f32 = 1.2;
const LEVELS: usize = 8;
const EDGE_THRESHOLD: usize = 31;
const PATCH_SIZE: usize = 31;
const FAST_THRESHOLD: u8 = 20;
#[cfg(test)]
const BORDER: usize = 32;
const HARRIS_K: f32 = 0.04;
const HARRIS_BLOCK_SIZE: usize = 7;
const BLUR_KERNEL: [f32; 7] = [
    f32::from_bits(0x3d8f_afb1),
    f32::from_bits(0x3e06_387e),
    f32::from_bits(0x3e43_4a39),
    f32::from_bits(0x3e5d_4ae0),
    f32::from_bits(0x3e43_4a39),
    f32::from_bits(0x3e06_387e),
    f32::from_bits(0x3d8f_afb1),
];
const ORB_PATTERN_BASE64: &str = "CP0JBQQCB/T1CfgCB/QM8wLzAgwB+QEG/vb+/PPz9fjz/fT3CgQLCfP4+Pf1B/cMBwcMBvz7/QDzAvT99wD5BQz6DP/9Bv4M+vP8+AvzDPgEBwUBBf0K/QP5Bgz4+fr+/gv/9vMM+Ar5A/v9/AL9B/b0+gsF9Ab5BfoH/wEABPsJCwvzBAcEDAL/BAT89P4H+Pv59gQLCQwA+AHz8/74Av3+/gP6Cfz3CAwKBwAJAQMH+wv28/r1AAoHDAH6/foMCvcM/PMI+PTzAPj8AwMHCAUHCvn/BwH0A/YFBgL8A/bzAPMF8/n0DPMD9Qj5DPwHBvYMCPf/+fr++wAM9AX5BQP2CPP5+fwF/f7/+QIJBfX18/vz/wYA/wX9BQL88/wM9/r3BvT2+PwKAgz9BwwMDPnz+gX8Cf0EB/8MAvkG+wHzC/QF/Qf++gf4DPnz+fX0Af0MDAL6AwD8A/7z//MBCQcBCPoB/wMMCQEMBv/3/wPz8/YFBwcKDAz7DAkGAwcLBfMGCgL0AgMDCAT6AgYM8wn0CgP4BPkJ9Qz8+gEMAvgG9wf8AgMD/gYDCwAD/Qj4BwgJA/X7+vz2C/sK+/j9DPYF9wAI/wz6BPoG9fYM+AcE/gYH/gD+DPv4+wIH+goM9/P4+Pvz+/4I+Anz9/X3AAH4Af4H/AkB/gH//Av6DPX09/oEAwcHDAUFCggA/AII9wz78wAHAgz/AgEHBQsH9wMFBvjz/PgJ+wn9/fz5/fQGBQgA+Qb6DPMG+/4B9gMKBAEI/P7+AvMC9AwM/vMA+gQBCQP69v37/fP/AQcFDPUE/gX58wn3+wcBCAYH+AcG+fz5AfgL+fjzBvT4AgQDCQr7DAP6+/oHCP0J+AL0Agj1/vYD9PP59/UA9vsF/QsI/vP/DP/4AAnz9fT79v72C/0J/vMC/QMC9/P8APwG/fb8DP75+vX8CQb9BgvzC/sFCwsMBgf7DP7/DAAH/Pj9/vkB+gfz9Pjz+f76+PgF+vf7//wF8wf4CgEFBfMBAArzCQwK/wX4Cvf/CwHz9/36Av/2AQzzAfj2CPUK+gLzA/oH8wz39vb7+fb4+PME+ggFAwwI8/wC/f0F8wr0BPMF//cJ/AMAAwP39AH6AQMCBPj29vYJCPMMDPj0+vsCAgMHCgYL+AYICPT5CvoF/ff9Cf/z/wX9+f0E+P74AwQCDAwC+wMLBvcL8wP/BwwL/wwE/QD9BgT1BAwC/AIB9vr4AfMH9QHzDPXzBgAL8wD/AQTzA/f+9wj6/fP6+P4F9wgKAgcD9//6//8JBQv+C/0M+AMAAwX/BAAKA/oEBfMA9gUFCAwLCAkJ+gf8CPT2BPYJBwMMBAn5Cv4HAAz+//oA9Q==";

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Image {
    pub(super) width: usize,
    pub(super) height: usize,
    pub(super) pixels: Vec<u8>,
}

impl Image {
    pub(crate) fn new(width: usize, height: usize, pixels: Vec<u8>) -> Self {
        assert_eq!(pixels.len(), width.saturating_mul(height));
        Self {
            width,
            height,
            pixels,
        }
    }

    fn at(&self, x: usize, y: usize) -> u8 {
        self.pixels[y * self.width + x]
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct Keypoint {
    pub(crate) x: f32,
    pub(crate) y: f32,
    pub(crate) response: f32,
    pub(crate) angle: f32,
    pub(crate) octave: usize,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct OrbFeature {
    pub(crate) keypoint: Keypoint,
    pub(crate) descriptor: [u8; 32],
}

pub(crate) struct OrbDetection {
    levels: Vec<Image>,
    keypoints: Vec<Keypoint>,
}

#[derive(Debug)]
struct ResizePlan {
    source_width: usize,
    source_height: usize,
    width: usize,
    height: usize,
    horizontal: Vec<ResizeCoefficient>,
    vertical: Vec<ResizeCoefficient>,
}

#[derive(Debug, Clone, Copy)]
struct ResizeCoefficient {
    first: u32,
    second: u32,
    first_weight: u16,
    second_weight: u16,
}

#[derive(Debug)]
pub(crate) struct PyramidPlan {
    width: usize,
    height: usize,
    levels: Vec<ResizePlan>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct FastKeypoint {
    pub(crate) x: u32,
    pub(crate) y: u32,
    pub(crate) response: u8,
}

const CIRCLE: [(i32, i32); 16] = [
    (0, 3),
    (1, 3),
    (2, 2),
    (3, 1),
    (3, 0),
    (3, -1),
    (2, -2),
    (1, -3),
    (0, -3),
    (-1, -3),
    (-2, -2),
    (-3, -1),
    (-3, 0),
    (-3, 1),
    (-2, 2),
    (-1, 3),
];

fn grayscale_pixel(pixel: &[u8]) -> u8 {
    let luminance = 0.299 * pixel[0] as f32 + 0.587 * pixel[1] as f32 + 0.114 * pixel[2] as f32;
    luminance.round().clamp(0.0, 255.0) as u8
}

fn grayscale_scalar(frame: &Frame) -> Image {
    let channels = frame.pixel_format().channels() as usize;
    let pixel_format = frame.pixel_format();
    let pixels = frame
        .pixels()
        .par_chunks_exact(channels)
        .map(|pixel| match pixel_format {
            PixelFormat::Gray8 => pixel[0],
            PixelFormat::Rgb8 | PixelFormat::Rgba8 => grayscale_pixel(pixel),
        })
        .collect();
    Image::new(frame.width() as usize, frame.height() as usize, pixels)
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn grayscale_chunk_avx2(source: &[u8], output: &mut [u8], channels: usize) {
    let offsets = _mm256_setr_epi32(0, 3, 6, 9, 12, 15, 18, 21);
    let byte_mask = _mm256_set1_epi32(0xff);
    let red_weight = _mm256_set1_ps(0.299);
    let green_weight = _mm256_set1_ps(0.587);
    let blue_weight = _mm256_set1_ps(0.114);
    let half = _mm256_set1_ps(0.5);
    let mut index = 0;
    let vector_end = if channels == 3 {
        output.len().saturating_sub(1) / 8 * 8
    } else {
        output.len() / 8 * 8
    };
    while index < vector_end {
        let packed = if channels == 3 {
            unsafe {
                _mm256_i32gather_epi32(source.as_ptr().add(index * 3).cast::<i32>(), offsets, 1)
            }
        } else {
            unsafe { _mm256_loadu_si256(source.as_ptr().add(index * 4).cast::<__m256i>()) }
        };
        let red = _mm256_cvtepi32_ps(_mm256_and_si256(packed, byte_mask));
        let green = _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(packed, 8), byte_mask));
        let blue = _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(packed, 16), byte_mask));
        let mut luminance = _mm256_mul_ps(red, red_weight);
        luminance = _mm256_add_ps(luminance, _mm256_mul_ps(green, green_weight));
        luminance = _mm256_add_ps(luminance, _mm256_mul_ps(blue, blue_weight));
        let rounded = _mm256_cvttps_epi32(_mm256_add_ps(luminance, half));
        let mut values = [0_i32; 8];
        unsafe { _mm256_storeu_si256(values.as_mut_ptr().cast::<__m256i>(), rounded) };
        for (target, value) in output[index..index + 8].iter_mut().zip(values) {
            *target = value as u8;
        }
        index += 8;
    }
    for (target, pixel) in output[index..]
        .iter_mut()
        .zip(source[index * channels..].chunks_exact(channels))
    {
        *target = grayscale_pixel(pixel);
    }
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn grayscale_avx2(frame: &Frame) -> Image {
    const OUTPUT_CHUNK: usize = 32 * 1024;
    let channels = frame.pixel_format().channels() as usize;
    if channels == 1 {
        return Image::new(
            frame.width() as usize,
            frame.height() as usize,
            frame.pixels().to_vec(),
        );
    }
    let mut pixels = vec![0_u8; frame.width() as usize * frame.height() as usize];
    pixels
        .par_chunks_mut(OUTPUT_CHUNK)
        .enumerate()
        .for_each(|(chunk_index, output)| {
            let source_start = chunk_index * OUTPUT_CHUNK * channels;
            let source = &frame.pixels()[source_start..source_start + output.len() * channels];
            unsafe { grayscale_chunk_avx2(source, output, channels) };
        });
    Image::new(frame.width() as usize, frame.height() as usize, pixels)
}

pub(crate) fn grayscale(frame: &Frame) -> Image {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    if std::arch::is_x86_feature_detected!("avx2") {
        return unsafe { grayscale_avx2(frame) };
    }
    grayscale_scalar(frame)
}

fn circle_values(width: u32, pixels: &[u8], x: u32, y: u32) -> [i16; 16] {
    let center = i16::from(pixels[(y * width + x) as usize]);
    CIRCLE.map(|(offset_x, offset_y)| {
        let x = (x as i32 + offset_x) as u32;
        let y = (y as i32 + offset_y) as u32;
        center - i16::from(pixels[(y * width + x) as usize])
    })
}

#[cfg(target_arch = "x86_64")]
#[inline]
fn fast_polarity_masks(differences: &[i16; 16], threshold: i16) -> (u32, u32) {
    unsafe {
        let first = _mm_loadu_si128(differences.as_ptr().cast::<__m128i>());
        let second = _mm_loadu_si128(differences.as_ptr().add(8).cast::<__m128i>());
        let dark_threshold = _mm_set1_epi16(threshold);
        let bright_threshold = _mm_set1_epi16(-threshold);
        let dark = _mm_packs_epi16(
            _mm_cmpgt_epi16(first, dark_threshold),
            _mm_cmpgt_epi16(second, dark_threshold),
        );
        let bright = _mm_packs_epi16(
            _mm_cmpgt_epi16(bright_threshold, first),
            _mm_cmpgt_epi16(bright_threshold, second),
        );
        (
            _mm_movemask_epi8(dark) as u32,
            _mm_movemask_epi8(bright) as u32,
        )
    }
}

fn score(differences: [i16; 16], threshold: i16) -> Option<u8> {
    #[cfg(target_arch = "x86_64")]
    let (dark, bright) = fast_polarity_masks(&differences, threshold);
    #[cfg(not(target_arch = "x86_64"))]
    let mut dark = 0_u32;
    #[cfg(not(target_arch = "x86_64"))]
    let mut bright = 0_u32;
    #[cfg(not(target_arch = "x86_64"))]
    for (index, difference) in differences.into_iter().enumerate() {
        dark |= u32::from(difference > threshold) << index;
        bright |= u32::from(-difference > threshold) << index;
    }
    let has_nine_pixel_run = |mask: u32| {
        let doubled = mask | (mask << 16);
        let mut runs = doubled;
        for shift in 1..9 {
            runs &= doubled >> shift;
        }
        runs & 0xffff != 0
    };
    if !has_nine_pixel_run(dark) && !has_nine_pixel_run(bright) {
        return None;
    }

    let mut strongest = threshold;
    for start in 0..16 {
        let mut dark = i16::MAX;
        let mut bright = i16::MIN;
        for offset in 0..9 {
            let difference = differences[(start + offset) % 16];
            dark = dark.min(difference);
            bright = bright.max(difference);
        }
        strongest = strongest.max(dark).max(-bright);
    }
    (strongest > threshold).then(|| (strongest - 1).min(i16::from(u8::MAX)) as u8)
}

#[inline]
fn may_pass_fast_9_16(width: u32, pixels: &[u8], x: u32, y: u32, threshold: i16) -> bool {
    let center = i16::from(pixels[(y * width + x) as usize]);
    let mut dark = 0_u8;
    let mut bright = 0_u8;
    for index in [0, 4, 8, 12] {
        let (offset_x, offset_y) = CIRCLE[index];
        let sample = i16::from(
            pixels[((y as i32 + offset_y) as u32 * width + (x as i32 + offset_x) as u32) as usize],
        );
        let difference = center - sample;
        dark += u8::from(difference > threshold);
        bright += u8::from(-difference > threshold);
    }
    dark >= 2 || bright >= 2
}

fn score_fast_row_scalar(width: u32, pixels: &[u8], row: &mut [u8], y: u32, threshold: i16) {
    for x in 3..width - 3 {
        if may_pass_fast_9_16(width, pixels, x, y, threshold) {
            row[x as usize] = score(circle_values(width, pixels, x, y), threshold).unwrap_or(0);
        }
    }
}

fn fast_scores_scalar(width: u32, height: u32, pixels: &[u8], threshold: i16) -> Vec<u8> {
    let mut scores = vec![0_u8; pixels.len()];
    scores
        .par_chunks_mut(width as usize)
        .enumerate()
        .for_each(|(y, row)| {
            let y = y as u32;
            if (3..height - 3).contains(&y) {
                score_fast_row_scalar(width, pixels, row, y, threshold);
            }
        });
    scores
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn score_fast_row_avx2(width: u32, pixels: &[u8], row: &mut [u8], y: u32, threshold: i16) {
    let width_usize = width as usize;
    let threshold_vector = _mm256_set1_epi16(threshold);
    let one = _mm256_set1_epi16(1);
    let mut x = 3_usize;
    while x + 16 <= width_usize - 3 {
        let center_index = y as usize * width_usize + x;
        let center = unsafe {
            _mm256_cvtepu8_epi16(_mm_loadu_si128(
                pixels.as_ptr().add(center_index).cast::<__m128i>(),
            ))
        };
        let mut dark_count = _mm256_setzero_si256();
        let mut bright_count = _mm256_setzero_si256();
        macro_rules! count_sample {
            ($index:expr) => {{
                let sample = unsafe {
                    _mm256_cvtepu8_epi16(_mm_loadu_si128(
                        pixels.as_ptr().add($index).cast::<__m128i>(),
                    ))
                };
                dark_count = _mm256_sub_epi16(
                    dark_count,
                    _mm256_cmpgt_epi16(_mm256_sub_epi16(center, sample), threshold_vector),
                );
                bright_count = _mm256_sub_epi16(
                    bright_count,
                    _mm256_cmpgt_epi16(_mm256_sub_epi16(sample, center), threshold_vector),
                );
            }};
        }
        count_sample!(center_index + width_usize * 3);
        count_sample!(center_index + 3);
        count_sample!(center_index - width_usize * 3);
        count_sample!(center_index - 3);
        let passes = _mm256_or_si256(
            _mm256_cmpgt_epi16(dark_count, one),
            _mm256_cmpgt_epi16(bright_count, one),
        );
        let mask = _mm256_movemask_epi8(passes) as u32;
        for lane in 0..16 {
            if mask & (1 << (lane * 2)) != 0 {
                let target_x = (x + lane) as u32;
                row[x + lane] =
                    score(circle_values(width, pixels, target_x, y), threshold).unwrap_or(0);
            }
        }
        x += 16;
    }
    for target_x in x as u32..width - 3 {
        if may_pass_fast_9_16(width, pixels, target_x, y, threshold) {
            row[target_x as usize] =
                score(circle_values(width, pixels, target_x, y), threshold).unwrap_or(0);
        }
    }
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
unsafe fn fast_scores_avx2(width: u32, height: u32, pixels: &[u8], threshold: i16) -> Vec<u8> {
    let mut scores = vec![0_u8; pixels.len()];
    scores
        .par_chunks_mut(width as usize)
        .enumerate()
        .for_each(|(y, row)| {
            let y = y as u32;
            if (3..height - 3).contains(&y) {
                unsafe { score_fast_row_avx2(width, pixels, row, y, threshold) };
            }
        });
    scores
}

pub(crate) fn fast_9_16(
    width: u32,
    height: u32,
    pixels: &[u8],
    threshold: u8,
) -> Vec<FastKeypoint> {
    if width < 7 || height < 7 || pixels.len() != (width as usize).saturating_mul(height as usize) {
        return Vec::new();
    }

    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    let scores = if std::arch::is_x86_feature_detected!("avx2") {
        unsafe { fast_scores_avx2(width, height, pixels, i16::from(threshold)) }
    } else {
        fast_scores_scalar(width, height, pixels, i16::from(threshold))
    };
    #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
    let scores = fast_scores_scalar(width, height, pixels, i16::from(threshold));
    const ROWS_PER_TASK: u32 = 16;
    (0..(height - 6).div_ceil(ROWS_PER_TASK))
        .into_par_iter()
        .map(|task| {
            let first_y = 3 + task * ROWS_PER_TASK;
            let last_y = (first_y + ROWS_PER_TASK).min(height - 3);
            let mut keypoints = Vec::new();
            for y in first_y..last_y {
                for x in 3..width - 3 {
                    let response = scores[(y * width + x) as usize];
                    if response == 0 {
                        continue;
                    }
                    let is_best = [-1_i32, 0, 1].into_iter().all(|offset_y| {
                        [-1_i32, 0, 1].into_iter().all(|offset_x| {
                            (offset_x == 0 && offset_y == 0)
                                || response
                                    > scores[((y as i32 + offset_y) as u32 * width
                                        + (x as i32 + offset_x) as u32)
                                        as usize]
                        })
                    });
                    if is_best {
                        keypoints.push(FastKeypoint { x, y, response });
                    }
                }
            }
            keypoints
        })
        .collect::<Vec<_>>()
        .into_iter()
        .flatten()
        .collect()
}

fn cv_round(value: f32) -> i32 {
    let floor = value.floor();
    let fraction = value - floor;
    if fraction < 0.5 {
        floor as i32
    } else if fraction > 0.5 {
        floor as i32 + 1
    } else if (floor as i32) % 2 == 0 {
        floor as i32
    } else {
        floor as i32 + 1
    }
}

fn cv_round64(value: f64) -> i64 {
    let floor = value.floor();
    let fraction = value - floor;
    if fraction < 0.5 {
        floor as i64
    } else if fraction > 0.5 {
        floor as i64 + 1
    } else if (floor as i64) % 2 == 0 {
        floor as i64
    } else {
        floor as i64 + 1
    }
}

fn resize_coefficients(source_length: usize, target_length: usize) -> Vec<ResizeCoefficient> {
    (0..target_length)
        .map(|target| {
            let value = source_length as f64 / target_length as f64 * (target as f64 + 0.5) - 0.5;
            if value < 0.0 {
                ResizeCoefficient {
                    first: 0,
                    second: 0,
                    first_weight: 256,
                    second_weight: 0,
                }
            } else if value >= (source_length - 1) as f64 {
                let last = (source_length - 1) as u32;
                ResizeCoefficient {
                    first: last,
                    second: last,
                    first_weight: 256,
                    second_weight: 0,
                }
            } else {
                let offset = value.floor() as usize;
                let right = cv_round64((value - offset as f64) * 256.0) as u16;
                ResizeCoefficient {
                    first: offset as u32,
                    second: offset as u32 + 1,
                    first_weight: 256 - right,
                    second_weight: right,
                }
            }
        })
        .collect()
}

fn resize_linear_exact(source: &Image, plan: &ResizePlan) -> Image {
    debug_assert_eq!(
        (source.width, source.height),
        (plan.source_width, plan.source_height)
    );
    if source.width == plan.width && source.height == plan.height {
        return source.clone();
    }
    let mut pixels = vec![0_u8; plan.width * plan.height];
    pixels
        .par_chunks_mut(plan.width)
        .enumerate()
        .for_each(|(target_y, output)| {
            let vertical = plan.vertical[target_y];
            let source_y = vertical.first as usize;
            let next_y = vertical.second as usize;
            for (target_x, &horizontal) in plan.horizontal.iter().enumerate() {
                let source_x = horizontal.first as usize;
                let next_x = horizontal.second as usize;
                let top_value = u32::from(source.at(source_x, source_y))
                    * u32::from(horizontal.first_weight)
                    + u32::from(source.at(next_x, source_y)) * u32::from(horizontal.second_weight);
                let bottom_value = u32::from(source.at(source_x, next_y))
                    * u32::from(horizontal.first_weight)
                    + u32::from(source.at(next_x, next_y)) * u32::from(horizontal.second_weight);
                let value = top_value * u32::from(vertical.first_weight)
                    + bottom_value * u32::from(vertical.second_weight);
                output[target_x] = ((value + (1_u32 << 15)) >> 16) as u8;
            }
        });
    Image::new(plan.width, plan.height, pixels)
}

fn reflect_101(index: isize, length: usize) -> usize {
    if length <= 1 {
        return 0;
    }
    let mut index = index;
    let length = length as isize;
    while index < 0 || index >= length {
        index = if index < 0 {
            -index
        } else {
            length * 2 - index - 2
        };
    }
    index as usize
}

#[cfg(test)]
fn padded(image: &Image) -> Image {
    let width = image.width + BORDER * 2;
    let height = image.height + BORDER * 2;
    let mut pixels = vec![0_u8; width * height];
    pixels
        .par_chunks_mut(width)
        .enumerate()
        .for_each(|(y, output)| {
            let source_y = reflect_101(y as isize - BORDER as isize, image.height);
            let source_start = source_y * image.width;
            for (x, output_pixel) in output.iter_mut().enumerate().take(BORDER) {
                let source_x = reflect_101(x as isize - BORDER as isize, image.width);
                *output_pixel = image.pixels[source_start + source_x];
            }
            output[BORDER..BORDER + image.width]
                .copy_from_slice(&image.pixels[source_start..source_start + image.width]);
            for (x, output_pixel) in output
                .iter_mut()
                .enumerate()
                .take(width)
                .skip(BORDER + image.width)
            {
                let source_x = reflect_101(x as isize - BORDER as isize, image.width);
                *output_pixel = image.pixels[source_start + source_x];
            }
        });
    Image::new(width, height, pixels)
}

fn scale_for_level(level: usize) -> f32 {
    (f64::from(SCALE_FACTOR).powi(level as i32)) as f32
}

fn level_size(width: usize, height: usize, level: usize) -> (usize, usize) {
    let inverse_scale = 1.0_f32 / scale_for_level(level);
    (
        cv_round(width as f32 * inverse_scale) as usize,
        cv_round(height as f32 * inverse_scale) as usize,
    )
}

impl PyramidPlan {
    pub(crate) fn new(width: usize, height: usize) -> Self {
        let mut source_width = width;
        let mut source_height = height;
        let levels = (1..LEVELS)
            .map(|level| {
                let (target_width, target_height) = level_size(width, height, level);
                let plan = ResizePlan {
                    source_width,
                    source_height,
                    width: target_width,
                    height: target_height,
                    horizontal: resize_coefficients(source_width, target_width),
                    vertical: resize_coefficients(source_height, target_height),
                };
                source_width = target_width;
                source_height = target_height;
                plan
            })
            .collect();
        Self {
            width,
            height,
            levels,
        }
    }
}

fn feature_quota(total: usize) -> [usize; LEVELS] {
    let factor = 1.0_f32 / SCALE_FACTOR;
    let mut desired = total as f32 * (1.0 - factor) / (1.0 - factor.powi(LEVELS as i32));
    let mut quotas = [0_usize; LEVELS];
    let mut sum = 0_usize;
    for quota in quotas.iter_mut().take(LEVELS - 1) {
        *quota = cv_round(desired) as usize;
        sum = sum.saturating_add(*quota);
        desired *= factor;
    }
    quotas[LEVELS - 1] = total.saturating_sub(sum);
    quotas
}

fn retain_best(keypoints: &mut Vec<Keypoint>, count: usize) {
    if keypoints.len() <= count {
        return;
    }
    if count == 0 {
        keypoints.clear();
        return;
    }
    let mut responses = keypoints
        .iter()
        .map(|keypoint| keypoint.response)
        .collect::<Vec<_>>();
    let threshold = responses
        .select_nth_unstable_by(count - 1, |left, right| right.total_cmp(left))
        .1
        .to_owned();
    keypoints.retain(|keypoint| keypoint.response >= threshold);
}

fn filter_border(keypoints: &mut Vec<Keypoint>, image: &Image) {
    if image.width <= EDGE_THRESHOLD * 2 || image.height <= EDGE_THRESHOLD * 2 {
        keypoints.clear();
        return;
    }
    keypoints.retain(|keypoint| {
        let x = keypoint.x as usize;
        let y = keypoint.y as usize;
        x >= EDGE_THRESHOLD
            && y >= EDGE_THRESHOLD
            && x < image.width - EDGE_THRESHOLD
            && y < image.height - EDGE_THRESHOLD
    });
}

fn harris_moments(image: &Image, x: usize, y: usize) -> (i32, i32, i32) {
    let stride = image.width;
    let radius = HARRIS_BLOCK_SIZE / 2;
    let mut horizontal = 0_i32;
    let mut vertical = 0_i32;
    let mut cross = 0_i32;
    for offset_y in 0..HARRIS_BLOCK_SIZE {
        for offset_x in 0..HARRIS_BLOCK_SIZE {
            let center = (y + offset_y - radius) * stride + x + offset_x - radius;
            let intensity_x =
                (i32::from(image.pixels[center + 1]) - i32::from(image.pixels[center - 1])) * 2
                    + i32::from(image.pixels[center - stride + 1])
                    - i32::from(image.pixels[center - stride - 1])
                    + i32::from(image.pixels[center + stride + 1])
                    - i32::from(image.pixels[center + stride - 1]);
            let intensity_y = (i32::from(image.pixels[center + stride])
                - i32::from(image.pixels[center - stride]))
                * 2
                + i32::from(image.pixels[center + stride - 1])
                - i32::from(image.pixels[center - stride - 1])
                + i32::from(image.pixels[center + stride + 1])
                - i32::from(image.pixels[center - stride + 1]);
            horizontal += intensity_x * intensity_x;
            vertical += intensity_y * intensity_y;
            cross += intensity_x * intensity_y;
        }
    }
    (horizontal, vertical, cross)
}

fn harris_response(image: &Image, keypoint: &mut Keypoint) {
    let (horizontal, vertical, cross) = harris_moments(
        image,
        cv_round(keypoint.x) as usize,
        cv_round(keypoint.y) as usize,
    );
    let scale = 1.0_f32 / ((1 << 2) as f32 * HARRIS_BLOCK_SIZE as f32 * 255.0);
    let scale_sq_sq = scale * scale * scale * scale;
    keypoint.response = ((horizontal as f32 * vertical as f32 - cross as f32 * cross as f32)
        - HARRIS_K * (horizontal as f32 + vertical as f32) * (horizontal as f32 + vertical as f32))
        * scale_sq_sq;
}

fn umax() -> [usize; PATCH_SIZE / 2 + 2] {
    let half = PATCH_SIZE / 2;
    let mut values = [0_usize; PATCH_SIZE / 2 + 2];
    let maximum = (half as f32 * 2_f32.sqrt() / 2.0 + 1.0).floor() as usize;
    let minimum = (half as f32 * 2_f32.sqrt() / 2.0).ceil() as usize;
    for (offset, value) in values.iter_mut().take(maximum + 1).enumerate() {
        *value = cv_round(((half * half - offset * offset) as f64).sqrt() as f32) as usize;
    }
    let mut lower = 0_usize;
    for upper in (minimum..=half).rev() {
        while values[lower] == values[lower + 1] {
            lower += 1;
        }
        values[upper] = lower;
        lower += 1;
    }
    values
}

fn fast_atan2(y: f32, x: f32) -> f32 {
    const DEGREES_PER_RADIAN: f32 = (180.0_f64 / std::f64::consts::PI) as f32;
    const P1: f32 = f32::from_bits(0x3f7f_f219) * DEGREES_PER_RADIAN;
    const P3: f32 = f32::from_bits(0xbea6_d05c) * DEGREES_PER_RADIAN;
    const P5: f32 = f32::from_bits(0x3e1f_5003) * DEGREES_PER_RADIAN;
    const P7: f32 = f32::from_bits(0xbd35_8fc3) * DEGREES_PER_RADIAN;
    let absolute_x = x.abs();
    let absolute_y = y.abs();
    let (ratio, mut angle) = if absolute_x >= absolute_y {
        let ratio = absolute_y / (absolute_x + f64::EPSILON as f32);
        let squared = ratio * ratio;
        (
            ratio,
            (((P7 * squared + P5) * squared + P3) * squared + P1) * ratio,
        )
    } else {
        let ratio = absolute_x / (absolute_y + f64::EPSILON as f32);
        let squared = ratio * ratio;
        (
            ratio,
            90.0 - (((P7 * squared + P5) * squared + P3) * squared + P1) * ratio,
        )
    };
    let _ = ratio;
    if x < 0.0 {
        angle = 180.0 - angle;
    }
    if y < 0.0 {
        angle = 360.0 - angle;
    }
    angle
}

fn orient(image: &Image, keypoint: &mut Keypoint, maximum_x: &[usize]) {
    let x = cv_round(keypoint.x) as usize;
    let y = cv_round(keypoint.y) as usize;
    let center = y * image.width + x;
    let half = PATCH_SIZE / 2;
    let mut moment_y = 0_i32;
    let mut moment_x = 0_i32;
    for horizontal in -(half as isize)..=half as isize {
        moment_x +=
            horizontal as i32 * i32::from(image.pixels[(center as isize + horizontal) as usize]);
    }
    for (vertical, &horizontal_maximum) in maximum_x.iter().enumerate().take(half + 1).skip(1) {
        let mut sum = 0_i32;
        for horizontal in -(horizontal_maximum as isize)..=horizontal_maximum as isize {
            let above = i32::from(
                image.pixels[(center as isize
                    + horizontal
                    + vertical as isize * image.width as isize)
                    as usize],
            );
            let below = i32::from(
                image.pixels[(center as isize + horizontal
                    - vertical as isize * image.width as isize)
                    as usize],
            );
            sum += above - below;
            moment_x += horizontal as i32 * (above + below);
        }
        moment_y += vertical as i32 * sum;
    }
    keypoint.angle = fast_atan2(moment_y as f32, moment_x as f32);
}

#[inline]
fn horizontal_blur_reflected(image: &Image, x: usize, y: usize) -> f32 {
    let mut value = 0_f32;
    for (offset, weight) in BLUR_KERNEL.iter().enumerate() {
        let source_x = reflect_101(x as isize + offset as isize - 3, image.width);
        value += f32::from(image.at(source_x, y)) * *weight;
    }
    value
}

#[inline]
fn vertical_blur_reflected(
    horizontal: &[f32],
    width: usize,
    height: usize,
    x: usize,
    y: usize,
) -> f32 {
    let mut value = horizontal[y * width + x] * BLUR_KERNEL[3];
    for offset in 1..=3 {
        let above = reflect_101(y as isize - offset as isize, height);
        let below = reflect_101(y as isize + offset as isize, height);
        value += (horizontal[above * width + x] + horizontal[below * width + x])
            * BLUR_KERNEL[3 + offset];
    }
    value
}

fn blur_scalar(image: &Image) -> Image {
    let mut horizontal = vec![0_f32; image.pixels.len()];
    horizontal
        .chunks_mut(image.width)
        .enumerate()
        .for_each(|(y, row)| {
            if image.width > 6 {
                for (x, output_pixel) in row.iter_mut().enumerate().take(3) {
                    *output_pixel = horizontal_blur_reflected(image, x, y);
                }
                for (x, output_pixel) in row.iter_mut().enumerate().take(image.width - 3).skip(3) {
                    let center = y * image.width + x;
                    let mut value = f32::from(image.pixels[center - 3]) * BLUR_KERNEL[0];
                    value += f32::from(image.pixels[center - 2]) * BLUR_KERNEL[1];
                    value += f32::from(image.pixels[center - 1]) * BLUR_KERNEL[2];
                    value += f32::from(image.pixels[center]) * BLUR_KERNEL[3];
                    value += f32::from(image.pixels[center + 1]) * BLUR_KERNEL[4];
                    value += f32::from(image.pixels[center + 2]) * BLUR_KERNEL[5];
                    value += f32::from(image.pixels[center + 3]) * BLUR_KERNEL[6];
                    *output_pixel = value;
                }
                for (x, output_pixel) in row
                    .iter_mut()
                    .enumerate()
                    .take(image.width)
                    .skip(image.width - 3)
                {
                    *output_pixel = horizontal_blur_reflected(image, x, y);
                }
            } else {
                for (x, output_pixel) in row.iter_mut().enumerate().take(image.width) {
                    *output_pixel = horizontal_blur_reflected(image, x, y);
                }
            }
        });
    let mut pixels = vec![0_u8; image.pixels.len()];
    pixels
        .chunks_mut(image.width)
        .enumerate()
        .for_each(|(y, output)| {
            if image.height > 6 && y >= 3 && y < image.height - 3 {
                for (x, output_pixel) in output.iter_mut().enumerate().take(image.width) {
                    let center = y * image.width + x;
                    let mut value = horizontal[center] * BLUR_KERNEL[3];
                    value += (horizontal[center - image.width] + horizontal[center + image.width])
                        * BLUR_KERNEL[4];
                    value += (horizontal[center - image.width * 2]
                        + horizontal[center + image.width * 2])
                        * BLUR_KERNEL[5];
                    value += (horizontal[center - image.width * 3]
                        + horizontal[center + image.width * 3])
                        * BLUR_KERNEL[6];
                    *output_pixel = cv_round(value).clamp(0, 255) as u8;
                }
            } else {
                for (x, output_pixel) in output.iter_mut().enumerate().take(image.width) {
                    let value =
                        vertical_blur_reflected(&horizontal, image.width, image.height, x, y);
                    *output_pixel = cv_round(value).clamp(0, 255) as u8;
                }
            }
        });
    Image::new(image.width, image.height, pixels)
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn horizontal_blur_row_avx2(image: &Image, row: *mut f32, y: usize) {
    if image.width <= 6 {
        for x in 0..image.width {
            unsafe { row.add(x).write(horizontal_blur_reflected(image, x, y)) };
        }
        return;
    }

    for x in 0..3 {
        unsafe { row.add(x).write(horizontal_blur_reflected(image, x, y)) };
    }
    let source = unsafe { image.pixels.as_ptr().add(y * image.width) };
    let mut x = 3;
    while x + 8 <= image.width - 3 {
        let load = |offset: usize| unsafe {
            let bytes = _mm_loadl_epi64(source.add(offset).cast::<__m128i>());
            _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(bytes))
        };
        let mut value = _mm256_mul_ps(load(x - 3), _mm256_set1_ps(BLUR_KERNEL[0]));
        value = _mm256_add_ps(
            value,
            _mm256_mul_ps(load(x - 2), _mm256_set1_ps(BLUR_KERNEL[1])),
        );
        value = _mm256_add_ps(
            value,
            _mm256_mul_ps(load(x - 1), _mm256_set1_ps(BLUR_KERNEL[2])),
        );
        value = _mm256_add_ps(
            value,
            _mm256_mul_ps(load(x), _mm256_set1_ps(BLUR_KERNEL[3])),
        );
        value = _mm256_add_ps(
            value,
            _mm256_mul_ps(load(x + 1), _mm256_set1_ps(BLUR_KERNEL[4])),
        );
        value = _mm256_add_ps(
            value,
            _mm256_mul_ps(load(x + 2), _mm256_set1_ps(BLUR_KERNEL[5])),
        );
        value = _mm256_add_ps(
            value,
            _mm256_mul_ps(load(x + 3), _mm256_set1_ps(BLUR_KERNEL[6])),
        );
        unsafe { _mm256_storeu_ps(row.add(x), value) };
        x += 8;
    }
    while x < image.width - 3 {
        let center = y * image.width + x;
        let mut value = f32::from(image.pixels[center - 3]) * BLUR_KERNEL[0];
        value += f32::from(image.pixels[center - 2]) * BLUR_KERNEL[1];
        value += f32::from(image.pixels[center - 1]) * BLUR_KERNEL[2];
        value += f32::from(image.pixels[center]) * BLUR_KERNEL[3];
        value += f32::from(image.pixels[center + 1]) * BLUR_KERNEL[4];
        value += f32::from(image.pixels[center + 2]) * BLUR_KERNEL[5];
        value += f32::from(image.pixels[center + 3]) * BLUR_KERNEL[6];
        unsafe { row.add(x).write(value) };
        x += 1;
    }
    for x in image.width.saturating_sub(3)..image.width {
        unsafe { row.add(x).write(horizontal_blur_reflected(image, x, y)) };
    }
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
unsafe fn assume_init_vec<T>(mut values: Vec<MaybeUninit<T>>) -> Vec<T> {
    let pointer = values.as_mut_ptr().cast::<T>();
    let length = values.len();
    let capacity = values.capacity();
    std::mem::forget(values);
    unsafe { Vec::from_raw_parts(pointer, length, capacity) }
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn vertical_blur_row_avx2(
    horizontal: &[f32],
    width: usize,
    height: usize,
    output: &mut [u8],
    y: usize,
) {
    if height <= 6 || y < 3 || y >= height - 3 {
        for (x, value) in output.iter_mut().enumerate() {
            *value = cv_round(vertical_blur_reflected(horizontal, width, height, x, y))
                .clamp(0, 255) as u8;
        }
        return;
    }

    let mut x = 0;
    while x + 8 <= width {
        let load =
            |row: usize| unsafe { _mm256_loadu_ps(horizontal.as_ptr().add(row * width + x)) };
        let mut value = _mm256_mul_ps(load(y), _mm256_set1_ps(BLUR_KERNEL[3]));
        let pair = _mm256_add_ps(load(y - 1), load(y + 1));
        value = _mm256_add_ps(value, _mm256_mul_ps(pair, _mm256_set1_ps(BLUR_KERNEL[4])));
        let pair = _mm256_add_ps(load(y - 2), load(y + 2));
        value = _mm256_add_ps(value, _mm256_mul_ps(pair, _mm256_set1_ps(BLUR_KERNEL[5])));
        let pair = _mm256_add_ps(load(y - 3), load(y + 3));
        value = _mm256_add_ps(value, _mm256_mul_ps(pair, _mm256_set1_ps(BLUR_KERNEL[6])));
        let mut values = [0.0_f32; 8];
        unsafe { _mm256_storeu_ps(values.as_mut_ptr(), value) };
        for (target, value) in output[x..x + 8].iter_mut().zip(values) {
            *target = cv_round(value).clamp(0, 255) as u8;
        }
        x += 8;
    }
    while x < width {
        let center = y * width + x;
        let mut value = horizontal[center] * BLUR_KERNEL[3];
        value += (horizontal[center - width] + horizontal[center + width]) * BLUR_KERNEL[4];
        value += (horizontal[center - width * 2] + horizontal[center + width * 2]) * BLUR_KERNEL[5];
        value += (horizontal[center - width * 3] + horizontal[center + width * 3]) * BLUR_KERNEL[6];
        output[x] = cv_round(value).clamp(0, 255) as u8;
        x += 1;
    }
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn blur_avx2(image: &Image) -> Image {
    let mut horizontal = vec![MaybeUninit::<f32>::uninit(); image.pixels.len()];
    horizontal
        .chunks_mut(image.width)
        .enumerate()
        .for_each(|(y, row)| unsafe {
            horizontal_blur_row_avx2(image, row.as_mut_ptr().cast::<f32>(), y)
        });
    let horizontal = unsafe { assume_init_vec(horizontal) };
    let mut pixels = vec![0_u8; image.pixels.len()];
    pixels
        .chunks_mut(image.width)
        .enumerate()
        .for_each(|(y, row)| unsafe {
            vertical_blur_row_avx2(&horizontal, image.width, image.height, row, y)
        });
    Image::new(image.width, image.height, pixels)
}

pub(crate) fn blur(image: &Image) -> Image {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    if std::arch::is_x86_feature_detected!("avx2") {
        return unsafe { blur_avx2(image) };
    }
    blur_scalar(image)
}

fn base64_value(byte: u8) -> u8 {
    match byte {
        b'A'..=b'Z' => byte - b'A',
        b'a'..=b'z' => byte - b'a' + 26,
        b'0'..=b'9' => byte - b'0' + 52,
        b'+' => 62,
        b'/' => 63,
        _ => 0,
    }
}

fn orb_pattern() -> &'static [(i8, i8)] {
    static PATTERN: OnceLock<Vec<(i8, i8)>> = OnceLock::new();
    PATTERN
        .get_or_init(|| {
            let mut bytes = Vec::with_capacity(1024);
            for chunk in ORB_PATTERN_BASE64.as_bytes().chunks_exact(4) {
                let first = base64_value(chunk[0]);
                let second = base64_value(chunk[1]);
                bytes.push((first << 2) | (second >> 4));
                if chunk[2] != b'=' {
                    let third = base64_value(chunk[2]);
                    bytes.push((second << 4) | (third >> 2));
                    if chunk[3] != b'=' {
                        let fourth = base64_value(chunk[3]);
                        bytes.push((third << 6) | fourth);
                    }
                }
            }
            assert_eq!(bytes.len(), 1024);
            bytes
                .chunks_exact(2)
                .map(|point| (point[0] as i8, point[1] as i8))
                .collect()
        })
        .as_slice()
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
fn orb_pattern_xy() -> &'static ([i32; 512], [i32; 512]) {
    static PATTERN: OnceLock<([i32; 512], [i32; 512])> = OnceLock::new();
    PATTERN.get_or_init(|| {
        let pattern = orb_pattern();
        (
            std::array::from_fn(|index| i32::from(pattern[index].0)),
            std::array::from_fn(|index| i32::from(pattern[index].1)),
        )
    })
}

fn descriptor_scalar(image: &Image, keypoint: Keypoint) -> [u8; 32] {
    const RADIANS_PER_DEGREE: f32 = (std::f64::consts::PI / 180.0) as f32;
    let scale = 1.0_f32 / scale_for_level(keypoint.octave);
    let center_x = cv_round(keypoint.x * scale) as usize;
    let center_y = cv_round(keypoint.y * scale) as usize;
    let radians = keypoint.angle * RADIANS_PER_DEGREE;
    let cosine = radians.cos();
    let sine = radians.sin();
    let pattern = orb_pattern();
    let sample = |point: (i8, i8)| {
        let x = cv_round(f32::from(point.0) * cosine - f32::from(point.1) * sine) as isize;
        let y = cv_round(f32::from(point.0) * sine + f32::from(point.1) * cosine) as isize;
        image.at(
            (center_x as isize + x) as usize,
            (center_y as isize + y) as usize,
        )
    };
    let mut bytes = [0_u8; 32];
    for (index, byte) in bytes.iter_mut().enumerate() {
        let point = &pattern[index * 16..index * 16 + 16];
        let mut value = 0_u8;
        for bit in 0..8 {
            value |= u8::from(sample(point[bit * 2]) < sample(point[bit * 2 + 1])) << bit;
        }
        *byte = value;
    }
    bytes
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn descriptor_avx2(image: &Image, keypoint: Keypoint) -> [u8; 32] {
    const RADIANS_PER_DEGREE: f32 = (std::f64::consts::PI / 180.0) as f32;
    let scale = 1.0_f32 / scale_for_level(keypoint.octave);
    let center_x = cv_round(keypoint.x * scale);
    let center_y = cv_round(keypoint.y * scale);
    let radians = keypoint.angle * RADIANS_PER_DEGREE;
    let cosine = _mm256_set1_ps(radians.cos());
    let sine = _mm256_set1_ps(radians.sin());
    let center_x = _mm256_set1_epi32(center_x);
    let center_y = _mm256_set1_epi32(center_y);
    let width = _mm256_set1_epi32(image.width as i32);
    let byte_mask = _mm256_set1_epi32(0xff);
    let (pattern_x, pattern_y) = orb_pattern_xy();
    let mut bytes = [0_u8; 32];
    for (byte_index, byte) in bytes.iter_mut().enumerate() {
        let mut samples = [0_i32; 16];
        for half in 0..2 {
            let pattern_index = byte_index * 16 + half * 8;
            let x = unsafe {
                _mm256_loadu_si256(pattern_x.as_ptr().add(pattern_index).cast::<__m256i>())
            };
            let y = unsafe {
                _mm256_loadu_si256(pattern_y.as_ptr().add(pattern_index).cast::<__m256i>())
            };
            let x = _mm256_cvtepi32_ps(x);
            let y = _mm256_cvtepi32_ps(y);
            let rotated_x = _mm256_cvtps_epi32(_mm256_sub_ps(
                _mm256_mul_ps(x, cosine),
                _mm256_mul_ps(y, sine),
            ));
            let rotated_y = _mm256_cvtps_epi32(_mm256_add_ps(
                _mm256_mul_ps(x, sine),
                _mm256_mul_ps(y, cosine),
            ));
            let x = _mm256_add_epi32(center_x, rotated_x);
            let y = _mm256_add_epi32(center_y, rotated_y);
            let offsets = _mm256_add_epi32(_mm256_mullo_epi32(y, width), x);
            let values =
                unsafe { _mm256_i32gather_epi32(image.pixels.as_ptr().cast::<i32>(), offsets, 1) };
            let values = _mm256_and_si256(values, byte_mask);
            unsafe {
                _mm256_storeu_si256(samples.as_mut_ptr().add(half * 8).cast::<__m256i>(), values)
            };
        }
        for bit in 0..8 {
            *byte |= u8::from(samples[bit * 2] < samples[bit * 2 + 1]) << bit;
        }
    }
    bytes
}

pub(crate) fn pyramid(image: &Image, plan: &PyramidPlan) -> Vec<Image> {
    debug_assert_eq!((image.width, image.height), (plan.width, plan.height));
    let mut levels = Vec::with_capacity(LEVELS);
    levels.push(image.clone());
    for resize_plan in &plan.levels {
        let previous = levels.last().expect("base pyramid level exists");
        levels.push(resize_linear_exact(previous, resize_plan));
    }
    levels
}

pub(crate) fn detect(
    image: &Image,
    max_features: usize,
    pyramid_plan: &PyramidPlan,
) -> OrbDetection {
    let quotas = feature_quota(max_features);
    let levels = pyramid(image, pyramid_plan);
    let maximum_x = umax();
    let by_level = levels
        .par_iter()
        .enumerate()
        .map(|(level, level_image)| {
            let mut keypoints = fast_9_16(
                level_image.width as u32,
                level_image.height as u32,
                &level_image.pixels,
                FAST_THRESHOLD,
            )
            .into_iter()
            .map(|keypoint| Keypoint {
                x: keypoint.x as f32,
                y: keypoint.y as f32,
                response: keypoint.response as f32,
                angle: -1.0,
                octave: level,
            })
            .collect::<Vec<_>>();
            filter_border(&mut keypoints, level_image);
            retain_best(&mut keypoints, quotas[level].saturating_mul(2));
            keypoints
                .par_iter_mut()
                .for_each(|keypoint| harris_response(level_image, keypoint));
            retain_best(&mut keypoints, quotas[level]);
            keypoints.par_iter_mut().for_each(|keypoint| {
                orient(level_image, keypoint, &maximum_x);
                let scale = scale_for_level(level);
                keypoint.x *= scale;
                keypoint.y *= scale;
            });
            keypoints
        })
        .collect::<Vec<_>>();
    OrbDetection {
        levels,
        keypoints: by_level.into_iter().flatten().collect(),
    }
}

impl OrbDetection {
    pub(crate) fn keypoints(&self) -> &[Keypoint] {
        &self.keypoints
    }

    pub(crate) fn compute(self, keypoints: impl IntoIterator<Item = Keypoint>) -> Vec<OrbFeature> {
        let blurred = self.levels.par_iter().map(blur).collect::<Vec<_>>();
        #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
        let use_avx2 = std::arch::is_x86_feature_detected!("avx2")
            && blurred
                .iter()
                .all(|image| image.pixels.len() <= i32::MAX as usize - 3);
        keypoints
            .into_iter()
            .collect::<Vec<_>>()
            .into_par_iter()
            .map(|keypoint| OrbFeature {
                descriptor: {
                    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
                    if use_avx2 {
                        unsafe { descriptor_avx2(&blurred[keypoint.octave], keypoint) }
                    } else {
                        descriptor_scalar(&blurred[keypoint.octave], keypoint)
                    }
                    #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
                    descriptor_scalar(&blurred[keypoint.octave], keypoint)
                },
                keypoint,
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    #[test]
    fn avx2_grayscale_matches_scalar_for_formats_tails_and_chunks() {
        if !std::arch::is_x86_feature_detected!("avx2") {
            return;
        }
        for (pixel_format, pixel_count) in [
            (PixelFormat::Gray8, 17),
            (PixelFormat::Rgb8, 1),
            (PixelFormat::Rgb8, 8),
            (PixelFormat::Rgb8, 9),
            (PixelFormat::Rgb8, 65_539),
            (PixelFormat::Rgba8, 7),
            (PixelFormat::Rgba8, 8),
            (PixelFormat::Rgba8, 32_771),
        ] {
            let channels = pixel_format.channels() as usize;
            let pixels = (0..pixel_count * channels)
                .map(|index| ((index * 73 + index / 7 * 19 + 11) % 256) as u8)
                .collect();
            let frame = Frame::new(pixel_count as u32, 1, pixel_format, pixels).unwrap();
            assert_eq!(unsafe { grayscale_avx2(&frame) }, grayscale_scalar(&frame));
        }

        let channels = [0_u8, 1, 2, 31, 63, 64, 127, 128, 191, 254, 255];
        let pixels = channels
            .into_iter()
            .flat_map(|red| {
                channels.into_iter().flat_map(move |green| {
                    channels
                        .into_iter()
                        .flat_map(move |blue| [red, green, blue])
                })
            })
            .collect::<Vec<_>>();
        let frame = Frame::new((pixels.len() / 3) as u32, 1, PixelFormat::Rgb8, pixels).unwrap();
        assert_eq!(unsafe { grayscale_avx2(&frame) }, grayscale_scalar(&frame));
    }

    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    #[test]
    fn avx2_descriptor_matches_scalar_for_positions_angles_and_octaves() {
        if !std::arch::is_x86_feature_detected!("avx2") {
            return;
        }
        let image = Image::new(
            128,
            112,
            (0..128 * 112)
                .map(|index| ((index * 73 + index / 11 * 29 + 7) % 256) as u8)
                .collect(),
        );
        for octave in [0, 1, 3, 7] {
            let scale = scale_for_level(octave);
            for (center_x, center_y) in [(31.0, 31.0), (63.0, 55.0), (96.0, 80.0)] {
                for angle in [
                    0.0, 0.5, 17.25, 44.999, 45.0, 89.5, 137.0, 179.999, 225.0, 315.75, 359.5,
                ] {
                    let keypoint = Keypoint {
                        x: center_x * scale,
                        y: center_y * scale,
                        response: 1.0,
                        angle,
                        octave,
                    };
                    assert_eq!(
                        unsafe { descriptor_avx2(&image, keypoint) },
                        descriptor_scalar(&image, keypoint),
                        "octave {octave}, center ({center_x}, {center_y}), angle {angle}"
                    );
                }
            }
        }
    }

    fn reference_resize_linear_exact(source: &Image, width: usize, height: usize) -> Image {
        if source.width == width && source.height == height {
            return source.clone();
        }
        let coefficients = |source_length: usize, target_length: usize| {
            (0..target_length)
                .map(|target| {
                    let value =
                        source_length as f64 / target_length as f64 * (target as f64 + 0.5) - 0.5;
                    if value < 0.0 {
                        (0, 256_u16, 0_u16)
                    } else if value >= (source_length - 1) as f64 {
                        (source_length - 1, 256_u16, 0_u16)
                    } else {
                        let offset = value.floor() as usize;
                        let right = cv_round64((value - offset as f64) * 256.0) as u16;
                        (offset, 256_u16 - right, right)
                    }
                })
                .collect::<Vec<_>>()
        };
        let horizontal = coefficients(source.width, width);
        let vertical = coefficients(source.height, height);
        let mut pixels = vec![0_u8; width * height];
        for (target_y, output) in pixels.chunks_mut(width).enumerate() {
            let (source_y, top, bottom) = vertical[target_y];
            let next_y = (source_y + 1).min(source.height - 1);
            for (target_x, &(source_x, left, right)) in horizontal.iter().enumerate() {
                let next_x = (source_x + 1).min(source.width - 1);
                let top_value = u32::from(source.at(source_x, source_y)) * u32::from(left)
                    + u32::from(source.at(next_x, source_y)) * u32::from(right);
                let bottom_value = u32::from(source.at(source_x, next_y)) * u32::from(left)
                    + u32::from(source.at(next_x, next_y)) * u32::from(right);
                let value = top_value * u32::from(top) + bottom_value * u32::from(bottom);
                output[target_x] = ((value + (1_u32 << 15)) >> 16) as u8;
            }
        }
        Image::new(width, height, pixels)
    }

    fn reference_pyramid(image: &Image) -> Vec<Image> {
        let mut levels = vec![image.clone()];
        for level in 1..LEVELS {
            let (width, height) = level_size(image.width, image.height, level);
            let previous = levels.last().expect("base pyramid level exists");
            levels.push(reference_resize_linear_exact(previous, width, height));
        }
        levels
    }

    fn reference_padded(image: &Image) -> Image {
        let width = image.width + BORDER * 2;
        let height = image.height + BORDER * 2;
        let mut pixels = vec![0_u8; width * height];
        for y in 0..height {
            let source_y = reflect_101(y as isize - BORDER as isize, image.height);
            for x in 0..width {
                let source_x = reflect_101(x as isize - BORDER as isize, image.width);
                pixels[y * width + x] = image.at(source_x, source_y);
            }
        }
        Image::new(width, height, pixels)
    }

    fn reference_blur(image: &Image) -> Image {
        let mut horizontal = vec![0_f32; image.pixels.len()];
        for y in 0..image.height {
            for x in 0..image.width {
                let mut value = 0_f32;
                for (offset, weight) in BLUR_KERNEL.iter().enumerate() {
                    let source_x = reflect_101(x as isize + offset as isize - 3, image.width);
                    value += f32::from(image.at(source_x, y)) * *weight;
                }
                horizontal[y * image.width + x] = value;
            }
        }
        let mut pixels = vec![0_u8; image.pixels.len()];
        for y in 0..image.height {
            for x in 0..image.width {
                let mut value = horizontal[y * image.width + x] * BLUR_KERNEL[3];
                for offset in 1..=3 {
                    let above = reflect_101(y as isize - offset as isize, image.height);
                    let below = reflect_101(y as isize + offset as isize, image.height);
                    value += (horizontal[above * image.width + x]
                        + horizontal[below * image.width + x])
                        * BLUR_KERNEL[3 + offset];
                }
                pixels[y * image.width + x] = cv_round(value).clamp(0, 255) as u8;
            }
        }
        Image::new(image.width, image.height, pixels)
    }

    #[test]
    fn blur_interior_matches_reflected_reference() {
        let image = Image::new(
            13,
            11,
            (0..143)
                .map(|index| ((index * 37 + 19) % 256) as u8)
                .collect(),
        );

        assert_eq!(blur(&image), reference_blur(&image));
    }

    #[test]
    fn planned_pyramid_matches_on_demand_coefficients() {
        for (width, height) in [(5, 5), (17, 13), (63, 47), (321, 225)] {
            let image = Image::new(
                width,
                height,
                (0..width * height)
                    .map(|index| ((index * 37 + 19) % 256) as u8)
                    .collect(),
            );
            let plan = PyramidPlan::new(width, height);
            assert_eq!(pyramid(&image, &plan), reference_pyramid(&image));
        }
    }

    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    #[test]
    fn avx2_blur_matches_scalar_for_borders_and_vector_tails() {
        if !std::arch::is_x86_feature_detected!("avx2") {
            return;
        }
        for (width, height) in [(1, 1), (6, 9), (7, 7), (15, 13), (31, 22), (64, 57)] {
            let image = Image::new(
                width,
                height,
                (0..width * height)
                    .map(|index| ((index * 37 + 19) % 256) as u8)
                    .collect(),
            );
            assert_eq!(unsafe { blur_avx2(&image) }, blur_scalar(&image));
        }
    }

    #[test]
    fn row_copy_padding_matches_per_pixel_reference() {
        for (width, height) in [(1, 1), (2, 3), (31, 33), (64, 57)] {
            let image = Image::new(
                width,
                height,
                (0..width * height)
                    .map(|index| ((index * 37 + 19) % 256) as u8)
                    .collect(),
            );
            assert_eq!(padded(&image), reference_padded(&image));
        }
    }

    #[test]
    fn unpadded_harris_and_orientation_match_padded_coordinates() {
        let image = Image::new(
            96,
            80,
            (0..96 * 80)
                .map(|index| ((index * 37 + 19) % 256) as u8)
                .collect(),
        );
        let padded = padded(&image);
        let maximum_x = umax();
        for (x, y) in [(31.0, 31.0), (48.0, 40.0), (64.0, 48.0)] {
            assert_eq!(
                harris_moments(&image, x as usize, y as usize),
                harris_moments(&padded, x as usize + BORDER, y as usize + BORDER)
            );
            let mut unpadded_keypoint = Keypoint {
                x,
                y,
                response: 1.0,
                angle: -1.0,
                octave: 0,
            };
            let mut padded_keypoint = Keypoint {
                x: x + BORDER as f32,
                y: y + BORDER as f32,
                ..unpadded_keypoint
            };
            orient(&image, &mut unpadded_keypoint, &maximum_x);
            orient(&padded, &mut padded_keypoint, &maximum_x);
            assert_eq!(unpadded_keypoint.angle, padded_keypoint.angle);
        }
    }

    #[test]
    fn fast_prefilter_keeps_every_nine_pixel_arc() {
        const WIDTH: u32 = 7;
        const CENTER: u32 = 3;
        for sample in [0_u8, u8::MAX] {
            for start in 0..CIRCLE.len() {
                let mut pixels = vec![128_u8; (WIDTH * WIDTH) as usize];
                for offset in 0..9 {
                    let (offset_x, offset_y) = CIRCLE[(start + offset) % CIRCLE.len()];
                    let index = ((CENTER as i32 + offset_y) as u32 * WIDTH
                        + (CENTER as i32 + offset_x) as u32)
                        as usize;
                    pixels[index] = sample;
                }
                assert!(
                    fast_9_16(WIDTH, WIDTH, &pixels, FAST_THRESHOLD)
                        .iter()
                        .any(|keypoint| keypoint.x == CENTER && keypoint.y == CENTER)
                );
            }
        }
    }

    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    #[test]
    fn avx2_fast_score_map_matches_scalar_for_rows_and_vector_tails() {
        if !std::arch::is_x86_feature_detected!("avx2") {
            return;
        }
        for seed in 0..8 {
            for (width, height) in [(7_u32, 7_u32), (21, 13), (22, 17), (37, 29), (96, 73)] {
                let pixels = (0..width as usize * height as usize)
                    .map(|index| ((index * 73 + index / 7 * 19 + seed * 31 + 11) % 256) as u8)
                    .collect::<Vec<_>>();
                assert_eq!(
                    unsafe { fast_scores_avx2(width, height, &pixels, i16::from(FAST_THRESHOLD)) },
                    fast_scores_scalar(width, height, &pixels, i16::from(FAST_THRESHOLD)),
                    "{width}x{height}, seed {seed}"
                );
            }
        }
    }

    #[test]
    fn exact_fast_prefilter_matches_every_circular_arc_mask() {
        const THRESHOLD: i16 = 20;
        for mask in 0_u32..=u16::MAX.into() {
            let expected = (0..16)
                .any(|start| (0..9).all(|offset| mask & (1 << ((start + offset) % 16)) != 0));
            for polarity in [-1_i16, 1] {
                let differences = std::array::from_fn(|index| {
                    if mask & (1 << index) != 0 {
                        polarity * (THRESHOLD + 1)
                    } else {
                        0
                    }
                });
                assert_eq!(score(differences, THRESHOLD).is_some(), expected);
            }
        }
    }
}

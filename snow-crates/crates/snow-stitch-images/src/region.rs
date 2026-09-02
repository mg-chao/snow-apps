use rayon::prelude::*;

use crate::{Frame, PixelFormat, RegionDiagnostics, StitchAxis, StitchError};

#[cfg(target_arch = "x86")]
use std::arch::x86::{
    _mm256_add_ps, _mm256_and_si256, _mm256_cvtepi32_ps, _mm256_div_ps, _mm256_i32gather_epi32,
    _mm256_loadu_ps, _mm256_min_ps, _mm256_mul_ps, _mm256_set1_epi32, _mm256_set1_ps,
    _mm256_setr_epi32, _mm256_setzero_ps, _mm256_sqrt_ps, _mm256_srli_epi32, _mm256_storeu_ps,
    _mm256_sub_ps,
};
#[cfg(target_arch = "x86_64")]
use std::arch::x86_64::{
    _mm256_add_ps, _mm256_and_si256, _mm256_cvtepi32_ps, _mm256_div_ps, _mm256_i32gather_epi32,
    _mm256_loadu_ps, _mm256_min_ps, _mm256_mul_ps, _mm256_set1_epi32, _mm256_set1_ps,
    _mm256_setr_epi32, _mm256_setzero_ps, _mm256_sqrt_ps, _mm256_srli_epi32, _mm256_storeu_ps,
    _mm256_sub_ps,
};

const DOWNSAMPLE: u32 = 4;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct TileLayout {
    width: u32,
    height: u32,
    tile_size: u32,
    columns: u32,
    rows: u32,
}

impl TileLayout {
    pub(crate) fn new(width: u32, height: u32, tile_size: u32) -> Self {
        Self {
            width,
            height,
            tile_size,
            columns: width.div_ceil(tile_size),
            rows: height.div_ceil(tile_size),
        }
    }

    pub(crate) fn len(self) -> usize {
        (self.columns as usize).saturating_mul(self.rows as usize)
    }

    pub(crate) fn index(self, x: f32, y: f32) -> Option<usize> {
        if !x.is_finite() || !y.is_finite() || x < 0.0 || y < 0.0 {
            return None;
        }
        let x = x.floor() as u32;
        let y = y.floor() as u32;
        if x >= self.width || y >= self.height {
            return None;
        }
        let column = x / self.tile_size;
        let row = y / self.tile_size;
        Some((row as usize) * (self.columns as usize) + column as usize)
    }

    fn tile_bounds(self, index: usize) -> (u32, u32, u32, u32) {
        let column = index as u32 % self.columns;
        let row = index as u32 / self.columns;
        let x0 = column * self.tile_size;
        let y0 = row * self.tile_size;
        (
            x0,
            y0,
            (x0 + self.tile_size).min(self.width),
            (y0 + self.tile_size).min(self.height),
        )
    }
}

#[derive(Debug, Clone)]
pub(crate) struct GrayImage {
    width: usize,
    height: usize,
    pixels: Vec<f32>,
    gradients: Vec<f32>,
}

impl GrayImage {
    pub(crate) fn from_frame(frame: &Frame) -> Result<Self, StitchError> {
        let width = frame.width().div_ceil(DOWNSAMPLE) as usize;
        let height = frame.height().div_ceil(DOWNSAMPLE) as usize;
        #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
        let mut pixels = if frame.pixel_format() != PixelFormat::Gray8
            && std::arch::is_x86_feature_detected!("avx2")
        {
            unsafe { downsample_pixels_avx2(frame, width, height) }
        } else {
            downsample_pixels_scalar(frame, width, height)
        };
        #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
        let mut pixels = downsample_pixels_scalar(frame, width, height);

        pixels = blur_3x3(&pixels, width, height);
        let gradients = sobel_magnitude(&pixels, width, height);
        Ok(Self {
            width,
            height,
            pixels,
            gradients,
        })
    }
}

fn downsample_row_scalar(frame: &Frame, scaled_y: usize, output: &mut [f32], first_x: usize) {
    let source_width = frame.width() as usize;
    let source_height = frame.height() as usize;
    let channels = frame.pixel_format().channels() as usize;
    let pixel_format = frame.pixel_format();
    let source = frame.pixels();
    let y0 = scaled_y * DOWNSAMPLE as usize;
    let y1 = (y0 + DOWNSAMPLE as usize).min(source_height);
    for (scaled_x, output_pixel) in output.iter_mut().enumerate().skip(first_x) {
        let x0 = scaled_x * DOWNSAMPLE as usize;
        let x1 = (x0 + DOWNSAMPLE as usize).min(source_width);
        let mut sum = 0.0;
        let mut count = 0_u32;
        for y in y0..y1 {
            for x in x0..x1 {
                let offset = (y * source_width + x) * channels;
                let sample = match pixel_format {
                    PixelFormat::Gray8 => source[offset] as f32,
                    PixelFormat::Rgb8 | PixelFormat::Rgba8 => {
                        let red = source[offset] as f32;
                        let green = source[offset + 1] as f32;
                        let blue = source[offset + 2] as f32;
                        0.299 * red + 0.587 * green + 0.114 * blue
                    }
                };
                sum += sample;
                count += 1;
            }
        }
        *output_pixel = sum / count.max(1) as f32;
    }
}

fn downsample_pixels_scalar(frame: &Frame, width: usize, height: usize) -> Vec<f32> {
    let mut pixels = vec![0.0; width.saturating_mul(height)];
    pixels
        .par_chunks_mut(width)
        .enumerate()
        .for_each(|(scaled_y, output)| downsample_row_scalar(frame, scaled_y, output, 0));
    pixels
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn downsample_pixels_avx2(frame: &Frame, width: usize, height: usize) -> Vec<f32> {
    let source_width = frame.width() as usize;
    let source_height = frame.height() as usize;
    let channels = frame.pixel_format().channels() as usize;
    let lane_stride = (DOWNSAMPLE as usize * channels) as i32;
    let offsets = _mm256_setr_epi32(
        0,
        lane_stride,
        lane_stride * 2,
        lane_stride * 3,
        lane_stride * 4,
        lane_stride * 5,
        lane_stride * 6,
        lane_stride * 7,
    );
    let byte_mask = _mm256_set1_epi32(0xff);
    let red_weight = _mm256_set1_ps(0.299);
    let green_weight = _mm256_set1_ps(0.587);
    let blue_weight = _mm256_set1_ps(0.114);
    let divisor = _mm256_set1_ps(16.0);
    let mut pixels = vec![0.0; width.saturating_mul(height)];
    pixels
        .par_chunks_mut(width)
        .enumerate()
        .for_each(|(scaled_y, output)| {
            let y0 = scaled_y * DOWNSAMPLE as usize;
            let mut scaled_x = 0;
            if y0 + DOWNSAMPLE as usize <= source_height {
                while scaled_x + 8 <= width {
                    let x0 = scaled_x * DOWNSAMPLE as usize;
                    let covered = x0 + 8 * DOWNSAMPLE as usize;
                    if covered > source_width || (channels == 3 && covered == source_width) {
                        break;
                    }
                    let mut sum = _mm256_setzero_ps();
                    for offset_y in 0..DOWNSAMPLE as usize {
                        let row = (y0 + offset_y) * source_width;
                        for offset_x in 0..DOWNSAMPLE as usize {
                            let source_index = (row + x0 + offset_x) * channels;
                            let packed = unsafe {
                                _mm256_i32gather_epi32(
                                    frame.pixels().as_ptr().add(source_index).cast::<i32>(),
                                    offsets,
                                    1,
                                )
                            };
                            let red = _mm256_cvtepi32_ps(_mm256_and_si256(packed, byte_mask));
                            let green = _mm256_cvtepi32_ps(_mm256_and_si256(
                                _mm256_srli_epi32(packed, 8),
                                byte_mask,
                            ));
                            let blue = _mm256_cvtepi32_ps(_mm256_and_si256(
                                _mm256_srli_epi32(packed, 16),
                                byte_mask,
                            ));
                            let mut sample = _mm256_mul_ps(red, red_weight);
                            sample = _mm256_add_ps(sample, _mm256_mul_ps(green, green_weight));
                            sample = _mm256_add_ps(sample, _mm256_mul_ps(blue, blue_weight));
                            sum = _mm256_add_ps(sum, sample);
                        }
                    }
                    unsafe {
                        _mm256_storeu_ps(
                            output.as_mut_ptr().add(scaled_x),
                            _mm256_div_ps(sum, divisor),
                        )
                    };
                    scaled_x += 8;
                }
            }
            downsample_row_scalar(frame, scaled_y, output, scaled_x);
        });
    pixels
}

fn blur_row_scalar(
    source: &[f32],
    width: usize,
    height: usize,
    y: usize,
    row: &mut [f32],
    first_x: usize,
    last_x: usize,
) {
    for x in first_x..last_x {
        if y > 0 && y + 1 < height && x > 0 && x + 1 < width {
            let top = (y - 1) * width;
            let middle = y * width;
            let bottom = (y + 1) * width;
            let mut sum = source[top + x - 1];
            sum += source[top + x] * 2.0;
            sum += source[top + x + 1];
            sum += source[middle + x - 1] * 2.0;
            sum += source[middle + x] * 4.0;
            sum += source[middle + x + 1] * 2.0;
            sum += source[bottom + x - 1];
            sum += source[bottom + x] * 2.0;
            sum += source[bottom + x + 1];
            row[x] = sum / 16.0;
            continue;
        }

        let mut sum = 0.0;
        let mut total = 0.0;
        for (kernel_y, weight_y) in [1.0_f32, 2.0, 1.0].into_iter().enumerate() {
            let sample_y = (y as isize + kernel_y as isize - 1)
                .clamp(0, height.saturating_sub(1) as isize) as usize;
            for (kernel_x, weight_x) in [1.0_f32, 2.0, 1.0].into_iter().enumerate() {
                let sample_x = (x as isize + kernel_x as isize - 1)
                    .clamp(0, width.saturating_sub(1) as isize)
                    as usize;
                let weight = weight_x * weight_y;
                sum += source[sample_y * width + sample_x] * weight;
                total += weight;
            }
        }
        row[x] = sum / total;
    }
}

fn blur_3x3_scalar(source: &[f32], width: usize, height: usize) -> Vec<f32> {
    let mut output = vec![0.0; source.len()];
    if width == 0 {
        return output;
    }
    output
        .par_chunks_mut(width)
        .enumerate()
        .for_each(|(y, row)| blur_row_scalar(source, width, height, y, row, 0, width));
    output
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn blur_3x3_avx2(source: &[f32], width: usize, height: usize) -> Vec<f32> {
    let mut output = vec![0.0; source.len()];
    if width == 0 {
        return output;
    }
    output
        .par_chunks_mut(width)
        .enumerate()
        .for_each(|(y, row)| {
            if width < 3 || y == 0 || y + 1 == height {
                blur_row_scalar(source, width, height, y, row, 0, width);
                return;
            }
            blur_row_scalar(source, width, height, y, row, 0, 1);
            let top = (y - 1) * width;
            let middle = y * width;
            let bottom = (y + 1) * width;
            let two = _mm256_set1_ps(2.0);
            let four = _mm256_set1_ps(4.0);
            let divisor = _mm256_set1_ps(16.0);
            let mut x = 1;
            while x + 8 < width {
                let load = |source_row: usize, offset: isize| unsafe {
                    _mm256_loadu_ps(source.as_ptr().add(source_row + x).offset(offset))
                };
                let mut sum = load(top, -1);
                sum = _mm256_add_ps(sum, _mm256_mul_ps(load(top, 0), two));
                sum = _mm256_add_ps(sum, load(top, 1));
                sum = _mm256_add_ps(sum, _mm256_mul_ps(load(middle, -1), two));
                sum = _mm256_add_ps(sum, _mm256_mul_ps(load(middle, 0), four));
                sum = _mm256_add_ps(sum, _mm256_mul_ps(load(middle, 1), two));
                sum = _mm256_add_ps(sum, load(bottom, -1));
                sum = _mm256_add_ps(sum, _mm256_mul_ps(load(bottom, 0), two));
                sum = _mm256_add_ps(sum, load(bottom, 1));
                unsafe { _mm256_storeu_ps(row.as_mut_ptr().add(x), _mm256_div_ps(sum, divisor)) };
                x += 8;
            }
            blur_row_scalar(source, width, height, y, row, x, width);
        });
    output
}

fn blur_3x3(source: &[f32], width: usize, height: usize) -> Vec<f32> {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    if std::arch::is_x86_feature_detected!("avx2") {
        return unsafe { blur_3x3_avx2(source, width, height) };
    }
    blur_3x3_scalar(source, width, height)
}

fn sobel_row_scalar(
    source: &[f32],
    width: usize,
    height: usize,
    y: usize,
    row: &mut [f32],
    first_x: usize,
) {
    if y == 0 || y + 1 == height {
        return;
    }
    for x in first_x.max(1)..width - 1 {
        let top = (y - 1) * width;
        let middle = y * width;
        let bottom = (y + 1) * width;
        let gx = -source[top + x - 1] + source[top + x + 1] - 2.0 * source[middle + x - 1]
            + 2.0 * source[middle + x + 1]
            - source[bottom + x - 1]
            + source[bottom + x + 1];
        let gy = -source[top + x - 1] - 2.0 * source[top + x] - source[top + x + 1]
            + source[bottom + x - 1]
            + 2.0 * source[bottom + x]
            + source[bottom + x + 1];
        row[x] = gx.hypot(gy).min(255.0);
    }
}

fn sobel_magnitude_scalar(source: &[f32], width: usize, height: usize) -> Vec<f32> {
    let mut output = vec![0.0; source.len()];
    if width < 3 || height < 3 {
        return output;
    }
    output
        .par_chunks_mut(width)
        .enumerate()
        .for_each(|(y, row)| sobel_row_scalar(source, width, height, y, row, 1));
    output
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn sobel_magnitude_avx2(source: &[f32], width: usize, height: usize) -> Vec<f32> {
    let mut output = vec![0.0; source.len()];
    if width < 3 || height < 3 {
        return output;
    }
    output
        .par_chunks_mut(width)
        .enumerate()
        .for_each(|(y, row)| {
            if y == 0 || y + 1 == height {
                return;
            }
            let top = (y - 1) * width;
            let middle = y * width;
            let bottom = (y + 1) * width;
            let zero = _mm256_setzero_ps();
            let two = _mm256_set1_ps(2.0);
            let maximum = _mm256_set1_ps(255.0);
            let mut x = 1;
            while x + 8 < width {
                let load = |source_row: usize, offset: isize| unsafe {
                    _mm256_loadu_ps(source.as_ptr().add(source_row + x).offset(offset))
                };
                let top_left = load(top, -1);
                let top_center = load(top, 0);
                let top_right = load(top, 1);
                let middle_left = load(middle, -1);
                let middle_right = load(middle, 1);
                let bottom_left = load(bottom, -1);
                let bottom_center = load(bottom, 0);
                let bottom_right = load(bottom, 1);

                let mut gx = _mm256_sub_ps(zero, top_left);
                gx = _mm256_add_ps(gx, top_right);
                gx = _mm256_sub_ps(gx, _mm256_mul_ps(middle_left, two));
                gx = _mm256_add_ps(gx, _mm256_mul_ps(middle_right, two));
                gx = _mm256_sub_ps(gx, bottom_left);
                gx = _mm256_add_ps(gx, bottom_right);

                let mut gy = _mm256_sub_ps(zero, top_left);
                gy = _mm256_sub_ps(gy, _mm256_mul_ps(top_center, two));
                gy = _mm256_sub_ps(gy, top_right);
                gy = _mm256_add_ps(gy, bottom_left);
                gy = _mm256_add_ps(gy, _mm256_mul_ps(bottom_center, two));
                gy = _mm256_add_ps(gy, bottom_right);

                let magnitude =
                    _mm256_sqrt_ps(_mm256_add_ps(_mm256_mul_ps(gx, gx), _mm256_mul_ps(gy, gy)));
                unsafe {
                    _mm256_storeu_ps(row.as_mut_ptr().add(x), _mm256_min_ps(magnitude, maximum))
                };
                x += 8;
            }
            sobel_row_scalar(source, width, height, y, row, x);
        });
    output
}

fn sobel_magnitude(source: &[f32], width: usize, height: usize) -> Vec<f32> {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    if std::arch::is_x86_feature_detected!("avx2") {
        return unsafe { sobel_magnitude_avx2(source, width, height) };
    }
    sobel_magnitude_scalar(source, width, height)
}

#[derive(Debug, Clone)]
pub(crate) struct SimilarityMap {
    layout: TileLayout,
    values: Vec<f32>,
    texture: Vec<f32>,
    valid: Vec<bool>,
}

fn similarity_for_tile(
    reference: &GrayImage,
    incoming: &GrayImage,
    layout: TileLayout,
    axis: StitchAxis,
    scaled_offset: f32,
    tile: usize,
) -> Option<(f32, f32)> {
    let (x0, y0, x1, y1) = layout.tile_bounds(tile);
    let scaled_x0 = (x0 / DOWNSAMPLE) as usize;
    let scaled_y0 = (y0 / DOWNSAMPLE) as usize;
    let scaled_x1 = x1.div_ceil(DOWNSAMPLE) as usize;
    let scaled_y1 = y1.div_ceil(DOWNSAMPLE) as usize;
    let expected = (scaled_x1 - scaled_x0).saturating_mul(scaled_y1 - scaled_y0);
    let mut sample_count = 0_usize;
    let mut reference_sum = 0.0;
    let mut incoming_sum = 0.0;

    for y in scaled_y0..scaled_y1.min(incoming.height) {
        let incoming_row = y * incoming.width;
        for x in scaled_x0..scaled_x1.min(incoming.width) {
            let (reference_x, reference_y) = match axis {
                StitchAxis::Vertical => (x as f32, y as f32 - scaled_offset),
                StitchAxis::Horizontal => (x as f32 - scaled_offset, y as f32),
            };
            let Some(reference_value) = bilinear_axis_sample(
                &reference.pixels,
                reference.width,
                reference.height,
                reference_x,
                reference_y,
                axis,
            ) else {
                continue;
            };
            reference_sum += reference_value;
            incoming_sum += incoming.pixels[incoming_row + x];
            sample_count += 1;
        }
    }
    if sample_count < expected.div_ceil(2).max(4) {
        return None;
    }

    let count = sample_count as f32;
    let mean_reference = reference_sum / count;
    let mean_incoming = incoming_sum / count;
    let mut reference_variance = 0.0;
    let mut incoming_variance = 0.0;
    let mut covariance = 0.0;
    let mut gradient_error = 0.0;
    let mut mean_gradient = 0.0;
    for y in scaled_y0..scaled_y1.min(incoming.height) {
        let incoming_row = y * incoming.width;
        for x in scaled_x0..scaled_x1.min(incoming.width) {
            let (reference_x, reference_y) = match axis {
                StitchAxis::Vertical => (x as f32, y as f32 - scaled_offset),
                StitchAxis::Horizontal => (x as f32 - scaled_offset, y as f32),
            };
            let Some(reference_value) = bilinear_axis_sample(
                &reference.pixels,
                reference.width,
                reference.height,
                reference_x,
                reference_y,
                axis,
            ) else {
                continue;
            };
            let reference_gradient = bilinear_axis_sample(
                &reference.gradients,
                reference.width,
                reference.height,
                reference_x,
                reference_y,
                axis,
            )?;
            let index = incoming_row + x;
            let incoming_value = incoming.pixels[index];
            let incoming_gradient = incoming.gradients[index];
            let reference_delta = reference_value - mean_reference;
            let incoming_delta = incoming_value - mean_incoming;
            reference_variance += reference_delta * reference_delta;
            incoming_variance += incoming_delta * incoming_delta;
            covariance += reference_delta * incoming_delta;
            gradient_error += (reference_gradient - incoming_gradient).abs();
            mean_gradient += reference_gradient.max(incoming_gradient);
        }
    }
    reference_variance /= count;
    incoming_variance /= count;
    covariance /= count;
    gradient_error /= count;
    mean_gradient /= count;

    let c1 = 6.5025;
    let c2 = 58.5225;
    let numerator = (2.0 * mean_reference * mean_incoming + c1) * (2.0 * covariance + c2);
    let denominator = (mean_reference * mean_reference + mean_incoming * mean_incoming + c1)
        * (reference_variance + incoming_variance + c2);
    let ssim = if denominator > f32::EPSILON {
        (numerator / denominator).clamp(0.0, 1.0)
    } else {
        1.0
    };
    let gradient_similarity = (1.0 - gradient_error / 64.0).clamp(0.0, 1.0);
    let value = 0.7 * ssim + 0.3 * gradient_similarity;
    let texture = ((reference_variance.max(incoming_variance).sqrt() / 32.0)
        + mean_gradient / 64.0)
        .mul_add(0.5, 0.0)
        .clamp(0.0, 1.0);
    Some((value, texture))
}

fn bilinear_axis_sample(
    pixels: &[f32],
    width: usize,
    height: usize,
    x: f32,
    y: f32,
    axis: StitchAxis,
) -> Option<f32> {
    if !x.is_finite()
        || !y.is_finite()
        || x < 0.0
        || y < 0.0
        || x > width.saturating_sub(1) as f32
        || y > height.saturating_sub(1) as f32
    {
        return None;
    }
    match axis {
        StitchAxis::Vertical => {
            let y0 = y.floor() as usize;
            let y1 = (y0 + 1).min(height - 1);
            let fraction = y - y0 as f32;
            Some(
                pixels[y0 * width + x as usize] * (1.0 - fraction)
                    + pixels[y1 * width + x as usize] * fraction,
            )
        }
        StitchAxis::Horizontal => {
            let x0 = x.floor() as usize;
            let x1 = (x0 + 1).min(width - 1);
            let fraction = x - x0 as f32;
            Some(
                pixels[y as usize * width + x0] * (1.0 - fraction)
                    + pixels[y as usize * width + x1] * fraction,
            )
        }
    }
}

#[inline(always)]
fn compare_swap(values: &mut [f32; 9], left: usize, right: usize) {
    if values[left].total_cmp(&values[right]).is_gt() {
        values.swap(left, right);
    }
}

#[inline]
fn median_of_nine(values: &mut [f32; 9]) -> f32 {
    compare_swap(values, 1, 2);
    compare_swap(values, 4, 5);
    compare_swap(values, 7, 8);
    compare_swap(values, 0, 1);
    compare_swap(values, 3, 4);
    compare_swap(values, 6, 7);
    compare_swap(values, 1, 2);
    compare_swap(values, 4, 5);
    compare_swap(values, 7, 8);
    compare_swap(values, 0, 3);
    compare_swap(values, 5, 8);
    compare_swap(values, 4, 7);
    compare_swap(values, 3, 6);
    compare_swap(values, 1, 4);
    compare_swap(values, 2, 5);
    compare_swap(values, 4, 7);
    compare_swap(values, 4, 2);
    compare_swap(values, 6, 4);
    compare_swap(values, 4, 2);
    values[4]
}

impl SimilarityMap {
    pub(crate) fn between(
        reference: &GrayImage,
        incoming: &GrayImage,
        layout: TileLayout,
        axis: StitchAxis,
        offset: i32,
    ) -> Self {
        let scaled_offset = offset as f32 / DOWNSAMPLE as f32;
        let tiles = (0..layout.len())
            .into_par_iter()
            .map(|tile| similarity_for_tile(reference, incoming, layout, axis, scaled_offset, tile))
            .collect::<Vec<_>>();
        let mut map = Self {
            layout,
            values: vec![0.0; layout.len()],
            texture: vec![0.0; layout.len()],
            valid: vec![false; layout.len()],
        };
        for (tile, values) in tiles.into_iter().enumerate() {
            let Some((value, texture)) = values else {
                continue;
            };
            map.values[tile] = value;
            map.texture[tile] = texture;
            map.valid[tile] = true;
        }
        map.median_filtered()
    }

    fn median_filtered(mut self) -> Self {
        let source = self.values.clone();
        let columns = self.layout.columns as usize;
        let rows = self.layout.rows as usize;
        for index in 0..self.layout.len() {
            if !self.valid[index] {
                continue;
            }
            let mut neighbors = [0.0_f32; 9];
            let mut count = 0;
            let column = index % columns;
            let row = index / columns;
            let first_column = column.saturating_sub(1);
            let last_column = (column + 1).min(columns - 1);
            let first_row = row.saturating_sub(1);
            let last_row = (row + 1).min(rows - 1);
            for neighbor_row in first_row..=last_row {
                let row_start = neighbor_row * columns;
                for neighbor_column in first_column..=last_column {
                    let neighbor = row_start + neighbor_column;
                    if self.valid[neighbor] {
                        neighbors[count] = source[neighbor];
                        count += 1;
                    }
                }
            }
            self.values[index] = if count == 9 {
                median_of_nine(&mut neighbors)
            } else {
                neighbors[..count].sort_by(f32::total_cmp);
                neighbors[count / 2]
            };
        }
        self
    }

    pub(crate) fn at_tile(&self, index: usize) -> Option<f32> {
        self.valid
            .get(index)
            .copied()
            .unwrap_or(false)
            .then(|| self.values[index])
    }

    pub(crate) fn at_point(&self, x: f32, y: f32) -> Option<f32> {
        self.layout
            .index(x, y)
            .and_then(|index| self.at_tile(index))
    }

    pub(crate) fn mean(&self) -> f32 {
        let mut weighted_sum = 0.0;
        let mut total = 0.0;
        for index in 0..self.layout.len() {
            if self.valid[index] {
                let weight = 0.1 + 0.9 * self.texture[index];
                weighted_sum += self.values[index] * weight;
                total += weight;
            }
        }
        if total > 0.0 {
            weighted_sum / total
        } else {
            0.0
        }
    }

    pub(crate) fn texture_at(&self, index: usize) -> f32 {
        self.texture.get(index).copied().unwrap_or(0.0)
    }

    pub(crate) fn layout(&self) -> TileLayout {
        self.layout
    }
}

#[derive(Debug, Clone, Copy)]
struct RegionState {
    fixed: f32,
    scrolling: f32,
    dynamic: f32,
    observations: u16,
}

impl Default for RegionState {
    fn default() -> Self {
        Self {
            fixed: 1.0 / 3.0,
            scrolling: 1.0 / 3.0,
            dynamic: 1.0 / 3.0,
            observations: 0,
        }
    }
}

#[derive(Debug, Clone)]
pub(crate) struct TemporalRegionModel {
    layout: TileLayout,
    states: Vec<RegionState>,
}

impl TemporalRegionModel {
    pub(crate) fn new(layout: TileLayout) -> Self {
        Self {
            layout,
            states: vec![RegionState::default(); layout.len()],
        }
    }

    pub(crate) fn weight_at_tile(&self, index: usize) -> f32 {
        let Some(state) = self.states.get(index) else {
            return 1.0;
        };
        let learned = 1.0 + 1.5 * (state.scrolling - 1.0 / 3.0)
            - (state.fixed - 1.0 / 3.0)
            - (state.dynamic - 1.0 / 3.0);
        let influence = (state.observations as f32 / 3.0).clamp(0.0, 1.0);
        (1.0 + influence * (learned - 1.0)).clamp(0.1, 2.0)
    }

    pub(crate) fn weight_at_point(&self, x: f32, y: f32) -> f32 {
        self.layout
            .index(x, y)
            .map(|index| self.weight_at_tile(index))
            .unwrap_or(1.0)
    }

    pub(crate) fn update(
        &mut self,
        direct: &SimilarityMap,
        compensated: &SimilarityMap,
        learning_rate: f32,
    ) {
        debug_assert_eq!(direct.layout(), self.layout);
        debug_assert_eq!(compensated.layout(), self.layout);
        for index in 0..self.states.len() {
            let (Some(direct_similarity), Some(compensated_similarity)) =
                (direct.at_tile(index), compensated.at_tile(index))
            else {
                continue;
            };
            if direct.texture_at(index).max(compensated.texture_at(index)) < 0.05 {
                continue;
            }
            let ambiguous = direct_similarity * compensated_similarity;
            if ambiguous >= 0.5 {
                continue;
            }
            let fixed = direct_similarity * (1.0 - compensated_similarity);
            let scrolling = compensated_similarity * (1.0 - direct_similarity);
            let dynamic = (1.0 - direct_similarity) * (1.0 - compensated_similarity);
            let total = fixed + scrolling + dynamic;
            if total < 0.05 {
                continue;
            }
            let observation = [fixed / total, scrolling / total, dynamic / total];
            let state = &mut self.states[index];
            state.fixed = (1.0 - learning_rate) * state.fixed + learning_rate * observation[0];
            state.scrolling =
                (1.0 - learning_rate) * state.scrolling + learning_rate * observation[1];
            state.dynamic = (1.0 - learning_rate) * state.dynamic + learning_rate * observation[2];
            state.observations = state.observations.saturating_add(1);
        }
    }

    pub(crate) fn decay_toward_neutral(&mut self, rate: f32) {
        for state in &mut self.states {
            state.fixed = (1.0 - rate) * state.fixed + rate / 3.0;
            state.scrolling = (1.0 - rate) * state.scrolling + rate / 3.0;
            state.dynamic = (1.0 - rate) * state.dynamic + rate / 3.0;
        }
    }

    pub(crate) fn reset(&mut self) {
        self.states.fill(RegionState::default());
    }

    pub(crate) fn summary(&self) -> RegionDiagnostics {
        let mut summary = RegionDiagnostics::default();
        for state in &self.states {
            if state.observations < 3 {
                summary.neutral_tiles += 1;
            } else if state.fixed >= state.scrolling && state.fixed >= state.dynamic {
                summary.fixed_tiles += 1;
            } else if state.scrolling >= state.dynamic {
                summary.scrolling_tiles += 1;
            } else {
                summary.dynamic_tiles += 1;
            }
        }
        summary
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn reference_blur_3x3(source: &[f32], width: usize, height: usize) -> Vec<f32> {
        let weights = [1.0_f32, 2.0, 1.0];
        let mut output = vec![0.0; source.len()];
        for y in 0..height {
            for x in 0..width {
                let mut sum = 0.0;
                let mut total = 0.0;
                for (kernel_y, &weight_y) in weights.iter().enumerate() {
                    let sample_y = (y as isize + kernel_y as isize - 1)
                        .clamp(0, height.saturating_sub(1) as isize)
                        as usize;
                    for (kernel_x, &weight_x) in weights.iter().enumerate() {
                        let sample_x = (x as isize + kernel_x as isize - 1)
                            .clamp(0, width.saturating_sub(1) as isize)
                            as usize;
                        let weight = weight_x * weight_y;
                        sum += source[sample_y * width + sample_x] * weight;
                        total += weight;
                    }
                }
                output[y * width + x] = sum / total;
            }
        }
        output
    }

    fn gray_frame(width: u32, height: u32, f: impl Fn(u32, u32) -> u8) -> Frame {
        let mut pixels = Vec::with_capacity((width * height) as usize);
        for y in 0..height {
            for x in 0..width {
                pixels.push(f(x, y));
            }
        }
        Frame::new(width, height, PixelFormat::Gray8, pixels).unwrap()
    }

    #[test]
    fn specialized_blur_matches_clamped_reference_exactly() {
        for (width, height) in [(1, 1), (2, 5), (7, 3), (19, 11)] {
            let source = (0..width * height)
                .map(|index| ((index * 37 + 11) % 251) as f32 / 3.0)
                .collect::<Vec<_>>();
            assert_eq!(
                blur_3x3(&source, width, height),
                reference_blur_3x3(&source, width, height)
            );
        }
    }

    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    #[test]
    fn avx2_blur_matches_scalar_for_borders_and_vector_tails() {
        if !std::arch::is_x86_feature_detected!("avx2") {
            return;
        }
        for (width, height) in [(1, 1), (2, 5), (7, 3), (10, 7), (11, 9), (19, 13), (41, 29)] {
            let source = (0..width * height)
                .map(|index| ((index * 37 + index / 7 * 19 + 11) % 1021) as f32 / 4.0)
                .collect::<Vec<_>>();
            assert_eq!(
                unsafe { blur_3x3_avx2(&source, width, height) },
                blur_3x3_scalar(&source, width, height),
                "{width}x{height}"
            );
        }
    }

    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    #[test]
    fn avx2_downsample_matches_scalar_for_formats_blocks_and_tails() {
        if !std::arch::is_x86_feature_detected!("avx2") {
            return;
        }
        for pixel_format in [PixelFormat::Rgb8, PixelFormat::Rgba8] {
            for (width, height) in [
                (31_u32, 3_u32),
                (32, 4),
                (33, 4),
                (64, 8),
                (65, 9),
                (97, 11),
            ] {
                let channels = pixel_format.channels() as usize;
                let pixels = (0..width as usize * height as usize * channels)
                    .map(|index| ((index * 73 + index / 11 * 29 + 17) % 256) as u8)
                    .collect::<Vec<_>>();
                let frame = Frame::new(width, height, pixel_format, pixels).unwrap();
                let scaled_width = width.div_ceil(DOWNSAMPLE) as usize;
                let scaled_height = height.div_ceil(DOWNSAMPLE) as usize;
                assert_eq!(
                    unsafe { downsample_pixels_avx2(&frame, scaled_width, scaled_height) },
                    downsample_pixels_scalar(&frame, scaled_width, scaled_height),
                    "{pixel_format:?}, {width}x{height}"
                );
            }
        }
    }

    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    #[test]
    fn avx2_sobel_matches_scalar_for_rows_and_vector_tails() {
        if !std::arch::is_x86_feature_detected!("avx2") {
            return;
        }
        for (width, height) in [(1, 1), (7, 5), (10, 7), (11, 9), (19, 13), (41, 29)] {
            let source = (0..width * height)
                .map(|index| ((index * 37 + index / 7 * 19 + 11) % 1021) as f32 / 4.0)
                .collect::<Vec<_>>();
            assert_eq!(
                unsafe { sobel_magnitude_avx2(&source, width, height) },
                sobel_magnitude_scalar(&source, width, height),
                "{width}x{height}"
            );
        }
    }

    #[test]
    fn identical_tiles_have_high_similarity() {
        let frame = gray_frame(64, 64, |x, y| ((x * 13 + y * 7) % 256) as u8);
        let gray = GrayImage::from_frame(&frame).unwrap();
        let similarity = SimilarityMap::between(
            &gray,
            &gray,
            TileLayout::new(64, 64, 32),
            StitchAxis::Vertical,
            0,
        );
        assert!(similarity.mean() > 0.99);
    }

    #[test]
    fn compensated_shift_restores_similarity() {
        let reference = gray_frame(64, 96, |x, y| ((x * 17 + y * 11) % 251) as u8);
        let incoming = gray_frame(64, 96, |x, y| {
            let source_y = y.saturating_add(12).min(95);
            ((x * 17 + source_y * 11) % 251) as u8
        });
        let reference = GrayImage::from_frame(&reference).unwrap();
        let incoming = GrayImage::from_frame(&incoming).unwrap();
        let layout = TileLayout::new(64, 96, 32);
        let direct = SimilarityMap::between(&reference, &incoming, layout, StitchAxis::Vertical, 0);
        let compensated =
            SimilarityMap::between(&reference, &incoming, layout, StitchAxis::Vertical, -12);
        assert!(compensated.mean() > direct.mean() + 0.25);
    }

    #[test]
    fn temporal_model_learns_all_three_region_classes() {
        let layout = TileLayout::new(96, 32, 32);
        let mut direct = SimilarityMap {
            layout,
            values: vec![0.95, 0.1, 0.1],
            texture: vec![1.0; 3],
            valid: vec![true; 3],
        };
        let mut compensated = SimilarityMap {
            layout,
            values: vec![0.1, 0.95, 0.1],
            texture: vec![1.0; 3],
            valid: vec![true; 3],
        };
        let mut model = TemporalRegionModel::new(layout);
        for _ in 0..4 {
            model.update(&direct, &compensated, 0.2);
        }
        let summary = model.summary();
        assert_eq!(summary.fixed_tiles, 1);
        assert_eq!(summary.scrolling_tiles, 1);
        assert_eq!(summary.dynamic_tiles, 1);
        assert!(model.weight_at_tile(1) > model.weight_at_tile(0));
        assert!(model.weight_at_tile(1) > model.weight_at_tile(2));

        direct.values.fill(1.0);
        compensated.values.fill(1.0);
        let before = model.summary();
        model.update(&direct, &compensated, 0.2);
        assert_eq!(model.summary(), before);
    }

    #[test]
    fn median_network_matches_total_order_sort_for_every_permutation() {
        let mut permutation = [0.0_f32, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0];
        loop {
            let mut network = permutation;
            assert_eq!(median_of_nine(&mut network), 4.0);

            let Some(pivot) = (0..8).rev().find(|&index| {
                permutation[index]
                    .total_cmp(&permutation[index + 1])
                    .is_lt()
            }) else {
                break;
            };
            let successor = (pivot + 1..9)
                .rev()
                .find(|&index| permutation[pivot].total_cmp(&permutation[index]).is_lt())
                .unwrap();
            permutation.swap(pivot, successor);
            permutation[pivot + 1..].reverse();
        }

        for mut values in [
            [
                f32::NAN,
                -0.0,
                0.0,
                f32::INFINITY,
                f32::NEG_INFINITY,
                1.0,
                1.0,
                -1.0,
                -1.0,
            ],
            [3.0, 3.0, 3.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0],
        ] {
            let mut sorted = values;
            sorted.sort_by(f32::total_cmp);
            assert_eq!(median_of_nine(&mut values).to_bits(), sorted[4].to_bits());
        }
    }
}

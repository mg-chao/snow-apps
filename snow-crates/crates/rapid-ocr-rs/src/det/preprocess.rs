use rayon::prelude::*;

use crate::{
    config::{RecImage, VisionBackend},
    error::{RapidOcrError, Result},
    vision::{
        backend::resolve_backend_strict,
        image_backend::resize_image,
        resize::{LinearResizeScratch, resize_bgr_inter_linear_into_with_scratch},
    },
};

#[derive(Debug, Clone)]
pub struct DetPreProcess {
    pub limit_side_len: usize,
    pub limit_type: String,
    pub mean: [f32; 3],
    pub std: [f32; 3],
    pub vision_backend: VisionBackend,
}

impl Default for DetPreProcess {
    fn default() -> Self {
        Self {
            limit_side_len: 736,
            limit_type: "min".to_string(),
            mean: [0.5, 0.5, 0.5],
            std: [0.5, 0.5, 0.5],
            vision_backend: VisionBackend::PureRust,
        }
    }
}

#[derive(Debug, Default)]
pub(crate) struct DetPreprocessScratch {
    pub tmp_bgr: Vec<u8>,
    pub resize: LinearResizeScratch,
}

impl DetPreProcess {
    pub(crate) fn run_into_buffer_with_scratch(
        &self,
        img: &RecImage,
        buffer: &mut Vec<f32>,
        scratch: &mut DetPreprocessScratch,
        limit_side_len: Option<usize>,
    ) -> Result<(usize, usize)> {
        let (resize_h, resize_w) =
            self.compute_resize_hw(img, limit_side_len.unwrap_or(self.limit_side_len))?;
        let expected_len = 3usize
            .checked_mul(resize_h)
            .and_then(|v| v.checked_mul(resize_w))
            .ok_or_else(|| {
                RapidOcrError::InvalidInput("det output buffer size overflow".to_string())
            })?;
        buffer.resize(expected_len, 0.0);

        let backend = resolve_backend_strict(self.vision_backend)?;
        match backend {
            VisionBackend::PureRust => {
                let src = img.as_bgr_cow();
                resize_bgr_inter_linear_into_with_scratch(
                    src.as_ref(),
                    img.width(),
                    img.height(),
                    resize_w,
                    resize_h,
                    &mut scratch.tmp_bgr,
                    &mut scratch.resize,
                )?;
                self.normalize_bgr_into_slice(
                    &scratch.tmp_bgr,
                    resize_w,
                    resize_h,
                    &mut buffer[..expected_len],
                )?;
            }
            VisionBackend::OpenCv => {
                let resized = resize_image(img, resize_w, resize_h, backend)?;
                self.normalize_resized_into_slice(&resized, &mut buffer[..expected_len])?;
            }
        }

        Ok((resize_h, resize_w))
    }

    fn compute_resize_hw(&self, img: &RecImage, limit_side_len: usize) -> Result<(usize, usize)> {
        let h = img.height();
        let w = img.width();
        if h == 0 || w == 0 {
            return Err(RapidOcrError::InvalidImage(
                "detector image width/height cannot be zero".to_string(),
            ));
        }

        let ratio = if self.limit_type == "max" {
            if h.max(w) > limit_side_len {
                limit_side_len as f32 / h.max(w) as f32
            } else {
                1.0
            }
        } else if h.min(w) < limit_side_len {
            limit_side_len as f32 / h.min(w) as f32
        } else {
            1.0
        };

        // Keep parity with Python: int(h * ratio), int(w * ratio), then round to 32-multiples.
        let mut resize_h = ((h as f32 * ratio) as usize).max(1);
        let mut resize_w = ((w as f32 * ratio) as usize).max(1);
        resize_h = ((resize_h as f32 / 32.0).round_ties_even() as usize * 32).max(32);
        resize_w = ((resize_w as f32 / 32.0).round_ties_even() as usize * 32).max(32);
        Ok((resize_h, resize_w))
    }

    fn normalize_resized_into_slice(
        &self,
        resized: &RecImage,
        out_slice: &mut [f32],
    ) -> Result<()> {
        let src = resized.as_bgr_cow();
        self.normalize_bgr_into_slice(src.as_ref(), resized.width(), resized.height(), out_slice)
    }

    fn normalize_bgr_into_slice(
        &self,
        src: &[u8],
        width: usize,
        height: usize,
        out_slice: &mut [f32],
    ) -> Result<()> {
        let expected_len = 3usize
            .checked_mul(height)
            .and_then(|v| v.checked_mul(width))
            .ok_or_else(|| {
                RapidOcrError::InvalidInput("det output buffer size overflow".to_string())
            })?;
        let expected_src_len = expected_len;
        if src.len() != expected_src_len {
            return Err(RapidOcrError::InvalidInput(format!(
                "det source BGR size mismatch: expected {expected_src_len}, got {}",
                src.len()
            )));
        }
        if out_slice.len() != expected_len {
            return Err(RapidOcrError::InvalidInput(format!(
                "det output buffer size mismatch: expected {expected_len}, got {}",
                out_slice.len()
            )));
        }

        let plane_stride = width * height;
        let row_src_stride = width * 3;
        let row_parallel = should_parallelize_rows(width, height);
        let norm_mul = [
            (1.0 / 255.0) / self.std[0],
            (1.0 / 255.0) / self.std[1],
            (1.0 / 255.0) / self.std[2],
        ];
        let norm_add = [
            -self.mean[0] / self.std[0],
            -self.mean[1] / self.std[1],
            -self.mean[2] / self.std[2],
        ];
        #[cfg(target_arch = "x86_64")]
        let use_avx2 = std::arch::is_x86_feature_detected!("avx2");

        if row_parallel {
            let out_addr = out_slice.as_mut_ptr() as usize;
            let src_addr = src.as_ptr() as usize;
            (0..height).into_par_iter().for_each(|y| {
                let out_ptr = out_addr as *mut f32;
                let src_ptr = src_addr as *const u8;
                // Safety:
                // - `y` is unique per parallel iteration, so each worker writes disjoint row ranges.
                // - `out_ptr` points to a contiguous CHW buffer of size `3 * width * height`.
                // - `src_ptr` points to a contiguous BGR buffer of size `3 * width * height`.
                unsafe {
                    let row_ptr = src_ptr.add(y * row_src_stride);
                    #[cfg(target_arch = "x86_64")]
                    if use_avx2 {
                        write_normalized_row_avx2(
                            row_ptr,
                            out_ptr,
                            y,
                            width,
                            plane_stride,
                            norm_mul,
                            norm_add,
                        );
                    } else {
                        write_normalized_row_scalar(
                            row_ptr,
                            out_ptr,
                            y,
                            width,
                            plane_stride,
                            norm_mul,
                            norm_add,
                        );
                    }
                    #[cfg(not(target_arch = "x86_64"))]
                    write_normalized_row_scalar(
                        row_ptr,
                        out_ptr,
                        y,
                        width,
                        plane_stride,
                        norm_mul,
                        norm_add,
                    );
                }
            });
        } else {
            for y in 0..height {
                // Safety:
                // - `y` stays within `[0, height)`.
                // - Source and destination pointers are derived from validated contiguous buffers.
                unsafe {
                    let row_ptr = src.as_ptr().add(y * row_src_stride);
                    #[cfg(target_arch = "x86_64")]
                    if use_avx2 {
                        write_normalized_row_avx2(
                            row_ptr,
                            out_slice.as_mut_ptr(),
                            y,
                            width,
                            plane_stride,
                            norm_mul,
                            norm_add,
                        );
                    } else {
                        write_normalized_row_scalar(
                            row_ptr,
                            out_slice.as_mut_ptr(),
                            y,
                            width,
                            plane_stride,
                            norm_mul,
                            norm_add,
                        );
                    }
                    #[cfg(not(target_arch = "x86_64"))]
                    write_normalized_row_scalar(
                        row_ptr,
                        out_slice.as_mut_ptr(),
                        y,
                        width,
                        plane_stride,
                        norm_mul,
                        norm_add,
                    );
                }
            }
        }
        Ok(())
    }
}

#[inline]
fn should_parallelize_rows(width: usize, height: usize) -> bool {
    width
        .checked_mul(height)
        .is_some_and(|v| v >= 512 * 512 && rayon::current_num_threads() > 1)
}

#[inline]
unsafe fn write_normalized_row_scalar(
    src_row_ptr: *const u8,
    out_ptr: *mut f32,
    y: usize,
    width: usize,
    plane_stride: usize,
    norm_mul: [f32; 3],
    norm_add: [f32; 3],
) {
    // Safety:
    // - Caller guarantees all pointers are valid for the computed row range.
    // - The row writes target disjoint regions when used in parallel.
    unsafe {
        let mut src_px = src_row_ptr;
        let row_base = y * width;
        for x in 0..width {
            let row_offset = row_base + x;
            let b = (*src_px) as f32 * norm_mul[0] + norm_add[0];
            let g = (*src_px.add(1)) as f32 * norm_mul[1] + norm_add[1];
            let r = (*src_px.add(2)) as f32 * norm_mul[2] + norm_add[2];

            *out_ptr.add(row_offset) = b;
            *out_ptr.add(plane_stride + row_offset) = g;
            *out_ptr.add(plane_stride * 2 + row_offset) = r;
            src_px = src_px.add(3);
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn write_normalized_row_avx2(
    src_row_ptr: *const u8,
    out_ptr: *mut f32,
    y: usize,
    width: usize,
    plane_stride: usize,
    norm_mul: [f32; 3],
    norm_add: [f32; 3],
) {
    use std::arch::x86_64::{
        __m256, __m256i, _mm256_add_ps, _mm256_cvtepi32_ps, _mm256_mul_ps, _mm256_set1_ps,
        _mm256_setr_epi32, _mm256_storeu_ps,
    };

    let row_base = y * width;
    let mut x = 0usize;
    let b_mul: __m256 = _mm256_set1_ps(norm_mul[0]);
    let g_mul: __m256 = _mm256_set1_ps(norm_mul[1]);
    let r_mul: __m256 = _mm256_set1_ps(norm_mul[2]);
    let b_add: __m256 = _mm256_set1_ps(norm_add[0]);
    let g_add: __m256 = _mm256_set1_ps(norm_add[1]);
    let r_add: __m256 = _mm256_set1_ps(norm_add[2]);

    while x + 8 <= width {
        // De-interleave 8 BGR pixels into channel vectors.
        let mut b = [0_i32; 8];
        let mut g = [0_i32; 8];
        let mut r = [0_i32; 8];
        for lane in 0..8 {
            // Safety:
            // - Caller guarantees `src_row_ptr` is valid for the current row.
            // - `x + lane < width` and each pixel has 3 channels.
            unsafe {
                let p = src_row_ptr.add((x + lane) * 3);
                b[lane] = *p as i32;
                g[lane] = *p.add(1) as i32;
                r[lane] = *p.add(2) as i32;
            }
        }

        let b_i32: __m256i = _mm256_setr_epi32(b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        let g_i32: __m256i = _mm256_setr_epi32(g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
        let r_i32: __m256i = _mm256_setr_epi32(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);

        let b_f32 = _mm256_add_ps(_mm256_mul_ps(_mm256_cvtepi32_ps(b_i32), b_mul), b_add);
        let g_f32 = _mm256_add_ps(_mm256_mul_ps(_mm256_cvtepi32_ps(g_i32), g_mul), g_add);
        let r_f32 = _mm256_add_ps(_mm256_mul_ps(_mm256_cvtepi32_ps(r_i32), r_mul), r_add);

        let row_offset = row_base + x;
        // Safety:
        // - Destination pointer is a contiguous CHW buffer sized for the full image.
        // - Row offsets are in-bounds for this row and channel planes.
        unsafe {
            _mm256_storeu_ps(out_ptr.add(row_offset), b_f32);
            _mm256_storeu_ps(out_ptr.add(plane_stride + row_offset), g_f32);
            _mm256_storeu_ps(out_ptr.add(plane_stride * 2 + row_offset), r_f32);
        }
        x += 8;
    }

    if x < width {
        // Safety:
        // - Tail starts within current source row; each iteration advances by one pixel.
        let mut src_px = unsafe { src_row_ptr.add(x * 3) };
        for px in x..width {
            let row_offset = row_base + px;
            // Safety:
            // - Source and destination pointers remain in-bounds for the tail range.
            unsafe {
                let b = (*src_px) as f32 * norm_mul[0] + norm_add[0];
                let g = (*src_px.add(1)) as f32 * norm_mul[1] + norm_add[1];
                let r = (*src_px.add(2)) as f32 * norm_mul[2] + norm_add[2];
                *out_ptr.add(row_offset) = b;
                *out_ptr.add(plane_stride + row_offset) = g;
                *out_ptr.add(plane_stride * 2 + row_offset) = r;
                src_px = src_px.add(3);
            }
        }
    }
}

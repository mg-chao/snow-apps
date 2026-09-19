use nalgebra::SMatrix;
#[cfg(feature = "opencv-backend")]
use opencv::{
    core::{self, Mat, Point2f, Scalar, Size},
    imgproc,
    prelude::*,
};
use std::sync::OnceLock;

#[cfg(feature = "opencv-backend")]
use crate::error::RapidOcrError;
#[cfg(not(feature = "opencv-backend"))]
use crate::vision::backend::OPENCV_BACKEND_DISABLED_MESSAGE;
use crate::{
    Quad,
    config::{RecImage, VisionBackend},
    error::Result,
    vision::backend::resolve_backend_strict,
};

pub fn rotate_crop_image(img: &RecImage, points: Quad, backend: VisionBackend) -> Result<RecImage> {
    let backend = resolve_backend_strict(backend)?;
    rotate_crop_image_with_resolved_backend(img, points, backend)
}

pub(crate) fn rotate_crop_image_with_resolved_backend(
    img: &RecImage,
    points: Quad,
    backend: VisionBackend,
) -> Result<RecImage> {
    match backend {
        VisionBackend::PureRust => rotate_crop_image_pure(img, points),
        VisionBackend::OpenCv => rotate_crop_image_opencv_dispatch(img, points),
    }
}

fn rotate_crop_image_pure(img: &RecImage, points: Quad) -> Result<RecImage> {
    if let Some(crop) = try_axis_aligned_crop(img, points)? {
        return Ok(crop);
    }

    let img_crop_width = l2(points[0], points[1]).max(l2(points[2], points[3]));
    let img_crop_height = l2(points[0], points[3]).max(l2(points[1], points[2]));
    let crop_w = img_crop_width.max(1.0) as usize;
    let crop_h = img_crop_height.max(1.0) as usize;

    let pts_std = [
        [0.0_f32, 0.0_f32],
        [crop_w as f32, 0.0_f32],
        [crop_w as f32, crop_h as f32],
        [0.0_f32, crop_h as f32],
    ];

    let h = homography_from_4pt(points, pts_std);
    let inv_h = h
        .try_inverse()
        .unwrap_or_else(SMatrix::<f64, 3, 3>::identity);

    let src = img.as_bgr_cow();
    let src = src.as_ref();
    let mut dst = vec![0_u8; crop_w * crop_h * 3];
    let tab = bicubic_remap_tab();
    let src_w = img.width() as i32;
    let src_h = img.height() as i32;
    let h00 = inv_h[(0, 0)];
    let h01 = inv_h[(0, 1)];
    let h02 = inv_h[(0, 2)];
    let h10 = inv_h[(1, 0)];
    let h11 = inv_h[(1, 1)];
    let h12 = inv_h[(1, 2)];
    let h20 = inv_h[(2, 0)];
    let h21 = inv_h[(2, 1)];
    let h22 = inv_h[(2, 2)];

    for y in 0..crop_h {
        let yy = y as f64;
        let x0 = h01 * yy + h02;
        let y0 = h11 * yy + h12;
        let w0 = h21 * yy + h22;
        for x in 0..crop_w {
            let xx = x as f64;
            let fw = w0 + h20 * xx;
            let inv = if fw.abs() > f64::EPSILON {
                INTER_TAB_SIZE_F64 / fw
            } else {
                0.0
            };

            let fx = (x0 + h00 * xx) * inv;
            let fy = (y0 + h10 * xx) * inv;

            let x_scaled = saturate_cast_i32_round(fx);
            let y_scaled = saturate_cast_i32_round(fy);

            let x_base = saturate_cast_i16(x_scaled >> INTER_BITS) as i32;
            let y_base = saturate_cast_i16(y_scaled >> INTER_BITS) as i32;
            let frac_x = (x_scaled & INTER_TAB_MASK) as usize;
            let frac_y = (y_scaled & INTER_TAB_MASK) as usize;
            let wtab = &tab[frac_y * INTER_TAB_SIZE + frac_x];

            let sx = x_base - 1;
            let sy = y_base - 1;

            let dst_idx = (y * crop_w + x) * 3;
            let mut sum_b = 0_i32;
            let mut sum_g = 0_i32;
            let mut sum_r = 0_i32;

            for ky in 0..4 {
                let yy_src = clamp_i32(sy + ky as i32, 0, src_h - 1) as usize;
                for kx in 0..4 {
                    let xx_src = clamp_i32(sx + kx as i32, 0, src_w - 1) as usize;
                    let src_idx = (yy_src * img.width() + xx_src) * 3;
                    let weight = wtab[ky * 4 + kx] as i32;
                    // Safety: clamped coordinates guarantee in-bounds source access.
                    unsafe {
                        sum_b += *src.get_unchecked(src_idx) as i32 * weight;
                        sum_g += *src.get_unchecked(src_idx + 1) as i32 * weight;
                        sum_r += *src.get_unchecked(src_idx + 2) as i32 * weight;
                    }
                }
            }

            let v_b = (sum_b + (1 << (INTER_REMAP_COEF_BITS - 1))) >> INTER_REMAP_COEF_BITS;
            let v_g = (sum_g + (1 << (INTER_REMAP_COEF_BITS - 1))) >> INTER_REMAP_COEF_BITS;
            let v_r = (sum_r + (1 << (INTER_REMAP_COEF_BITS - 1))) >> INTER_REMAP_COEF_BITS;
            dst[dst_idx] = v_b.clamp(0, 255) as u8;
            dst[dst_idx + 1] = v_g.clamp(0, 255) as u8;
            dst[dst_idx + 2] = v_r.clamp(0, 255) as u8;
        }
    }

    RecImage::from_bgr_u8(crop_w, crop_h, dst)
}

#[cfg(feature = "opencv-backend")]
fn rotate_crop_image_opencv(img: &RecImage, points: Quad) -> Result<RecImage> {
    if let Some(crop) = try_axis_aligned_crop(img, points)? {
        return Ok(crop);
    }

    let img_crop_width = l2(points[0], points[1])
        .max(l2(points[2], points[3]))
        .max(1.0) as i32;
    let img_crop_height = l2(points[0], points[3])
        .max(l2(points[1], points[2]))
        .max(1.0) as i32;

    let pts_std = [
        Point2f::new(0.0, 0.0),
        Point2f::new(img_crop_width as f32, 0.0),
        Point2f::new(img_crop_width as f32, img_crop_height as f32),
        Point2f::new(0.0, img_crop_height as f32),
    ];
    let src_pts = [
        Point2f::new(points[0][0], points[0][1]),
        Point2f::new(points[1][0], points[1][1]),
        Point2f::new(points[2][0], points[2][1]),
        Point2f::new(points[3][0], points[3][1]),
    ];

    let m = imgproc::get_perspective_transform_slice(src_pts, pts_std, core::DECOMP_LU).map_err(
        |e| RapidOcrError::Config(format!("opencv getPerspectiveTransform failed: {e}")),
    )?;

    let src_bgr = img.as_bgr_cow();
    let src_1d = Mat::from_slice(src_bgr.as_ref())
        .map_err(|e| RapidOcrError::Config(format!("opencv Mat::from_slice failed: {e}")))?;
    let src = src_1d
        .reshape(3, img.height() as i32)
        .map_err(|e| RapidOcrError::Config(format!("opencv Mat::reshape failed: {e}")))?;

    let mut dst = Mat::default();
    imgproc::warp_perspective(
        &src,
        &mut dst,
        &m,
        Size::new(img_crop_width, img_crop_height),
        imgproc::INTER_CUBIC,
        core::BORDER_REPLICATE,
        Scalar::all(0.0),
    )
    .map_err(|e| RapidOcrError::Config(format!("opencv warpPerspective failed: {e}")))?;

    let out = dst
        .data_bytes()
        .map_err(|e| RapidOcrError::Config(format!("opencv data_bytes failed: {e}")))?;
    RecImage::from_bgr_u8(
        img_crop_width as usize,
        img_crop_height as usize,
        out.to_vec(),
    )
}

fn try_axis_aligned_crop(img: &RecImage, points: Quad) -> Result<Option<RecImage>> {
    const EPS: f32 = 1e-3;
    let is_axis_aligned = (points[0][1] - points[1][1]).abs() <= EPS
        && (points[2][1] - points[3][1]).abs() <= EPS
        && (points[0][0] - points[3][0]).abs() <= EPS
        && (points[1][0] - points[2][0]).abs() <= EPS;
    if !is_axis_aligned {
        return Ok(None);
    }

    let img_w = img.width() as i32;
    let img_h = img.height() as i32;
    if img_w <= 0 || img_h <= 0 {
        return Ok(None);
    }

    let left = points[0][0].min(points[3][0]).round_ties_even() as i32;
    let right = points[1][0].max(points[2][0]).round_ties_even() as i32;
    let top = points[0][1].min(points[1][1]).round_ties_even() as i32;
    let bottom = points[2][1].max(points[3][1]).round_ties_even() as i32;

    // Keep parity with cv2.warpPerspective + RapidOCR dest points for axis-aligned boxes:
    // [x0, x1) and [y0, y1), where x1/y1 are the right/bottom vertices.
    let x0 = left.clamp(0, img_w - 1);
    let x1 = right.clamp(0, img_w);
    let y0 = top.clamp(0, img_h - 1);
    let y1 = bottom.clamp(0, img_h);
    if x1 <= x0 || y1 <= y0 {
        return Ok(None);
    }

    let crop_w = (x1 - x0) as usize;
    let crop_h = (y1 - y0) as usize;
    let mut out = vec![0_u8; crop_w * crop_h * 3];

    let src = img.as_bgr_cow();
    let src = src.as_ref();
    let src_row_stride = img.width() * 3;
    let dst_row_stride = crop_w * 3;
    for row in 0..crop_h {
        let src_row_start = ((y0 as usize + row) * src_row_stride) + x0 as usize * 3;
        let dst_row_start = row * dst_row_stride;
        let src_row = &src[src_row_start..src_row_start + dst_row_stride];
        let dst_row = &mut out[dst_row_start..dst_row_start + dst_row_stride];
        dst_row.copy_from_slice(src_row);
    }

    Ok(Some(RecImage::from_bgr_u8(crop_w, crop_h, out)?))
}

#[cfg(feature = "opencv-backend")]
fn rotate_crop_image_opencv_dispatch(img: &RecImage, points: Quad) -> Result<RecImage> {
    rotate_crop_image_opencv(img, points)
}

#[cfg(not(feature = "opencv-backend"))]
fn rotate_crop_image_opencv_dispatch(_img: &RecImage, _points: Quad) -> Result<RecImage> {
    Err(crate::error::RapidOcrError::Config(
        OPENCV_BACKEND_DISABLED_MESSAGE.to_string(),
    ))
}

const INTER_BITS: i32 = 5;
const INTER_TAB_SIZE: usize = 1usize << INTER_BITS;
const INTER_TAB_MASK: i32 = (INTER_TAB_SIZE as i32) - 1;
const INTER_TAB_SIZE_F64: f64 = INTER_TAB_SIZE as f64;
const INTER_REMAP_COEF_BITS: i32 = 15;
const INTER_REMAP_COEF_SCALE: i32 = 1 << INTER_REMAP_COEF_BITS;

fn clamp_i32(v: i32, min_v: i32, max_v: i32) -> i32 {
    v.max(min_v).min(max_v)
}

fn saturate_cast_i32_round(v: f64) -> i32 {
    if !v.is_finite() {
        return 0;
    }
    let r = v.round_ties_even();
    if r < i32::MIN as f64 {
        i32::MIN
    } else if r > i32::MAX as f64 {
        i32::MAX
    } else {
        r as i32
    }
}

fn saturate_cast_i16(v: i32) -> i16 {
    v.clamp(i16::MIN as i32, i16::MAX as i32) as i16
}

fn interpolate_cubic_coeffs(x: f32) -> [f32; 4] {
    const A: f32 = -0.75;
    let c0 = ((A * (x + 1.0) - 5.0 * A) * (x + 1.0) + 8.0 * A) * (x + 1.0) - 4.0 * A;
    let c1 = ((A + 2.0) * x - (A + 3.0)) * x * x + 1.0;
    let one_minus_x = 1.0 - x;
    let c2 = ((A + 2.0) * one_minus_x - (A + 3.0)) * one_minus_x * one_minus_x + 1.0;
    let c3 = 1.0 - c0 - c1 - c2;
    [c0, c1, c2, c3]
}

fn saturate_cast_i16_from_f32(v: f32) -> i16 {
    let r = v.round_ties_even();
    let i = r as i32;
    i.clamp(i16::MIN as i32, i16::MAX as i32) as i16
}

fn build_bicubic_remap_tab() -> Vec<[i16; 16]> {
    let mut tab = vec![[0_i16; 16]; INTER_TAB_SIZE * INTER_TAB_SIZE];
    for fy in 0..INTER_TAB_SIZE {
        let y_coeff = interpolate_cubic_coeffs(fy as f32 / INTER_TAB_SIZE as f32);
        for fx in 0..INTER_TAB_SIZE {
            let x_coeff = interpolate_cubic_coeffs(fx as f32 / INTER_TAB_SIZE as f32);
            let mut isum = 0_i32;
            for ky in 0..4 {
                for kx in 0..4 {
                    let v = y_coeff[ky] * x_coeff[kx];
                    let it = saturate_cast_i16_from_f32(v * INTER_REMAP_COEF_SCALE as f32);
                    tab[fy * INTER_TAB_SIZE + fx][ky * 4 + kx] = it;
                    isum += it as i32;
                }
            }

            if isum != INTER_REMAP_COEF_SCALE {
                let diff = isum - INTER_REMAP_COEF_SCALE;
                let idx = fy * INTER_TAB_SIZE + fx;
                let mut mk = 2 * 4 + 2;
                let mut mk_v = tab[idx][mk];
                let mut mk_max = mk;
                let mut mk_max_v = mk_v;
                for ky in 2..4 {
                    for kx in 2..4 {
                        let pos = ky * 4 + kx;
                        let v = tab[idx][pos];
                        if v < mk_v {
                            mk = pos;
                            mk_v = v;
                        } else if v > mk_max_v {
                            mk_max = pos;
                            mk_max_v = v;
                        }
                    }
                }

                if diff < 0 {
                    let nv =
                        (tab[idx][mk_max] as i32 - diff).clamp(i16::MIN as i32, i16::MAX as i32);
                    tab[idx][mk_max] = nv as i16;
                } else {
                    let nv = (tab[idx][mk] as i32 - diff).clamp(i16::MIN as i32, i16::MAX as i32);
                    tab[idx][mk] = nv as i16;
                }
            }
        }
    }
    tab
}

fn bicubic_remap_tab() -> &'static Vec<[i16; 16]> {
    static TAB: OnceLock<Vec<[i16; 16]>> = OnceLock::new();
    TAB.get_or_init(build_bicubic_remap_tab)
}

fn homography_from_4pt(src: Quad, dst: Quad) -> SMatrix<f64, 3, 3> {
    let mut a = [[0.0_f64; 8]; 8];
    let mut b = [0.0_f64; 8];

    for i in 0..4 {
        let x = src[i][0] as f64;
        let y = src[i][1] as f64;
        let x_cap = dst[i][0] as f64;
        let y_cap = dst[i][1] as f64;

        a[i][0] = x;
        a[i][1] = y;
        a[i][2] = 1.0;
        a[i][6] = -x * x_cap;
        a[i][7] = -y * x_cap;
        b[i] = x_cap;

        let row = i + 4;
        a[row][3] = x;
        a[row][4] = y;
        a[row][5] = 1.0;
        a[row][6] = -x * y_cap;
        a[row][7] = -y * y_cap;
        b[row] = y_cap;
    }

    if lu_solve_opencv_8x8(&mut a, &mut b) {
        SMatrix::<f64, 3, 3>::from_row_slice(&[b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], 1.0])
    } else {
        SMatrix::<f64, 3, 3>::identity()
    }
}

fn lu_solve_opencv_8x8(a: &mut [[f64; 8]; 8], b: &mut [f64; 8]) -> bool {
    const M: usize = 8;
    const EPS: f64 = f64::EPSILON * 100.0;

    for i in 0..M {
        let mut pivot = i;
        for j in i + 1..M {
            if a[j][i].abs() > a[pivot][i].abs() {
                pivot = j;
            }
        }

        if a[pivot][i].abs() < EPS {
            return false;
        }

        if pivot != i {
            let mut j = i;
            while j < M {
                let tmp = a[i][j];
                a[i][j] = a[pivot][j];
                a[pivot][j] = tmp;
                j += 1;
            }
            b.swap(i, pivot);
        }

        let d = -1.0 / a[i][i];
        for j in i + 1..M {
            let alpha = a[j][i] * d;
            let mut k = i + 1;
            while k < M {
                a[j][k] += alpha * a[i][k];
                k += 1;
            }
            b[j] += alpha * b[i];
        }
    }

    for i in (0..M).rev() {
        let mut s = b[i];
        for k in i + 1..M {
            s -= a[i][k] * b[k];
        }
        b[i] = s / a[i][i];
    }

    true
}

fn l2(a: [f32; 2], b: [f32; 2]) -> f32 {
    let dx = a[0] - b[0];
    let dy = a[1] - b[1];
    (dx * dx + dy * dy).sqrt()
}

#[cfg(all(test, feature = "opencv-backend"))]
mod tests {
    use super::rotate_crop_image;
    use crate::config::{RecImage, VisionBackend};
    use std::path::PathBuf;

    fn gradient_image(width: usize, height: usize) -> RecImage {
        let mut data = vec![0_u8; width * height * 3];
        for y in 0..height {
            for x in 0..width {
                let i = (y * width + x) * 3;
                data[i] = ((x * 3 + y * 5) % 256) as u8;
                data[i + 1] = ((x * 7 + y * 11) % 256) as u8;
                data[i + 2] = ((x * 13 + y * 17) % 256) as u8;
            }
        }
        RecImage::from_bgr_u8(width, height, data).expect("gradient image should be valid")
    }

    fn assert_same_pixels(pure: &RecImage, opcv: &RecImage) {
        let pure = pure.as_bgr_bytes();
        let opcv = opcv.as_bgr_bytes();
        assert_eq!(pure.len(), opcv.len());

        let mut max_abs = 0_u8;
        let mut diff_count = 0_usize;
        let mut first_diff = None;
        for (idx, (x, y)) in pure.iter().zip(opcv.iter()).enumerate() {
            let d = (*x as i16 - *y as i16).unsigned_abs() as u8;
            if d != 0 {
                diff_count += 1;
                first_diff.get_or_insert((idx, *x, *y));
            }
            max_abs = max_abs.max(d);
        }

        assert_eq!(
            diff_count, 0,
            "pixel mismatch: diff_count={diff_count}, max_abs={max_abs}, first_diff={first_diff:?}"
        );
    }

    #[test]
    fn pure_crop_matches_opencv_for_axis_aligned_box() {
        let img = gradient_image(320, 180);
        let box_ = [[40.0, 30.0], [280.0, 30.0], [280.0, 80.0], [40.0, 80.0]];
        let pure = rotate_crop_image(&img, box_, VisionBackend::PureRust).expect("pure crop");
        let opcv = rotate_crop_image(&img, box_, VisionBackend::OpenCv).expect("opencv crop");
        assert_eq!(pure.width(), opcv.width());
        assert_eq!(pure.height(), opcv.height());
        assert_same_pixels(&pure, &opcv);
    }

    #[test]
    fn pure_crop_matches_opencv_for_quad_box() {
        let img = gradient_image(400, 260);
        let box_ = [[30.0, 40.0], [300.0, 30.0], [320.0, 110.0], [40.0, 120.0]];
        let pure = rotate_crop_image(&img, box_, VisionBackend::PureRust).expect("pure crop");
        let opcv = rotate_crop_image(&img, box_, VisionBackend::OpenCv).expect("opencv crop");
        assert_eq!(pure.width(), opcv.width());
        assert_eq!(pure.height(), opcv.height());
        assert_same_pixels(&pure, &opcv);
    }

    #[test]
    fn pure_crop_matches_opencv_on_real_te_box() {
        let mut p = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        p.push("test");
        p.push("test_files");
        p.push("te.png");
        let img = RecImage::from_path(&p).expect("te image should load");
        let box_ = [[0.0, 2.0], [348.0, 5.0], [348.0, 37.0], [0.0, 35.0]];
        let pure = rotate_crop_image(&img, box_, VisionBackend::PureRust).expect("pure crop");
        let opcv = rotate_crop_image(&img, box_, VisionBackend::OpenCv).expect("opencv crop");
        assert_eq!(pure.width(), opcv.width());
        assert_eq!(pure.height(), opcv.height());
        assert_same_pixels(&pure, &opcv);
    }

    #[test]
    fn pure_crop_matches_opencv_on_real_en_line_box() {
        let mut p = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        p.push("test");
        p.push("test_files");
        p.push("en.jpg");
        let img = RecImage::from_path(&p).expect("en image should load");
        let box_ = [[5.0, 53.0], [701.0, 53.0], [701.0, 75.0], [5.0, 75.0]];
        let pure = rotate_crop_image(&img, box_, VisionBackend::PureRust).expect("pure crop");
        let opcv = rotate_crop_image(&img, box_, VisionBackend::OpenCv).expect("opencv crop");
        assert_eq!(pure.width(), opcv.width());
        assert_eq!(pure.height(), opcv.height());
        assert_same_pixels(&pure, &opcv);
    }

    #[test]
    fn pure_crop_matches_opencv_on_check_return_word_len_box2() {
        let mut p = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        p.push("test");
        p.push("test_files");
        p.push("check_return_word_len.jpeg");
        let img = RecImage::from_path(&p).expect("check_return_word_len image should load");
        let box_ = [[237.0, 50.0], [279.0, 48.0], [279.0, 61.0], [238.0, 62.0]];
        let pure = rotate_crop_image(&img, box_, VisionBackend::PureRust).expect("pure crop");
        let opcv = rotate_crop_image(&img, box_, VisionBackend::OpenCv).expect("opencv crop");
        assert_eq!(pure.width(), opcv.width());
        assert_eq!(pure.height(), opcv.height());
        assert_same_pixels(&pure, &opcv);
    }
}

#[cfg(test)]
use ndarray::{Array3, Array4, s};
use std::sync::OnceLock;

use crate::{
    config::{RecImage, VisionBackend},
    error::{RapidOcrError, Result},
    vision::{
        backend::resolve_backend_strict,
        image_backend::resize_image,
        resize::{LinearResizeScratch, resize_bgr_inter_linear_into_with_scratch},
    },
};

#[cfg(test)]
pub fn resize_norm_img(
    img: &RecImage,
    max_wh_ratio: f64,
    rec_image_shape: [usize; 3],
) -> Result<Array3<f32>> {
    resize_norm_img_with_backend(img, max_wh_ratio, rec_image_shape, VisionBackend::PureRust)
}

#[cfg(test)]
pub fn resize_norm_img_with_backend(
    img: &RecImage,
    max_wh_ratio: f64,
    rec_image_shape: [usize; 3],
    backend: VisionBackend,
) -> Result<Array3<f32>> {
    let backend = resolve_backend_strict(backend)?;
    resize_norm_img_impl(img, max_wh_ratio, rec_image_shape, backend)
}

#[cfg(test)]
pub fn make_batch(
    images: &[RecImage],
    max_wh_ratio: f64,
    rec_image_shape: [usize; 3],
) -> Result<Array4<f32>> {
    make_batch_with_backend(
        images,
        max_wh_ratio,
        rec_image_shape,
        VisionBackend::PureRust,
    )
}

#[cfg(test)]
pub fn make_batch_with_backend(
    images: &[RecImage],
    max_wh_ratio: f64,
    rec_image_shape: [usize; 3],
    backend: VisionBackend,
) -> Result<Array4<f32>> {
    let backend = resolve_backend_strict(backend)?;
    let (img_channel, img_height, dst_width) = validate_common(max_wh_ratio, rec_image_shape)?;

    let batch_size = images.len();
    let mut batch = Array4::<f32>::zeros((batch_size, img_channel, img_height, dst_width));
    for (idx, image) in images.iter().enumerate() {
        let one = resize_norm_img_impl(image, max_wh_ratio, rec_image_shape, backend)?;
        batch.slice_mut(s![idx, .., .., ..]).assign(&one);
    }
    Ok(batch)
}

#[cfg(test)]
pub fn make_batch_with_backend_by_indices(
    images: &[RecImage],
    indices: &[usize],
    max_wh_ratio: f64,
    rec_image_shape: [usize; 3],
    backend: VisionBackend,
) -> Result<Array4<f32>> {
    let backend = resolve_backend_strict(backend)?;
    let (img_channel, img_height, dst_width) = validate_common(max_wh_ratio, rec_image_shape)?;

    let batch_size = indices.len();
    let mut batch = Array4::<f32>::zeros((batch_size, img_channel, img_height, dst_width));
    let sample_len = img_channel
        .checked_mul(img_height)
        .and_then(|v| v.checked_mul(dst_width))
        .ok_or_else(|| RapidOcrError::InvalidInput("rec batch sample size overflow".to_string()))?;
    let batch_slice = batch.as_slice_mut().ok_or_else(|| {
        RapidOcrError::InvalidInput("rec batch buffer is not contiguous".to_string())
    })?;
    for (idx, image_idx) in indices.iter().copied().enumerate() {
        let image = images.get(image_idx).ok_or_else(|| {
            RapidOcrError::InvalidInput(format!(
                "batch index {image_idx} out of bounds for image count {}",
                images.len()
            ))
        })?;
        let start = idx * sample_len;
        let end = start + sample_len;
        write_resize_norm_img_into_slice_impl(
            image,
            max_wh_ratio,
            rec_image_shape,
            backend,
            &mut batch_slice[start..end],
        )?;
    }
    Ok(batch)
}

pub(crate) fn batch_shape_for(
    max_wh_ratio: f64,
    rec_image_shape: [usize; 3],
) -> Result<(usize, usize, usize)> {
    validate_common(max_wh_ratio, rec_image_shape)
}

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) fn write_resize_norm_img_into_slice(
    img: &RecImage,
    max_wh_ratio: f64,
    rec_image_shape: [usize; 3],
    backend: VisionBackend,
    dst: &mut [f32],
) -> Result<()> {
    write_resize_norm_img_into_slice_impl(img, max_wh_ratio, rec_image_shape, backend, dst)
}

pub(crate) fn write_resize_norm_img_into_slice_with_scratch(
    img: &RecImage,
    max_wh_ratio: f64,
    rec_image_shape: [usize; 3],
    backend: VisionBackend,
    dst: &mut [f32],
    tmp_bgr: &mut Vec<u8>,
    resize_scratch: &mut LinearResizeScratch,
) -> Result<()> {
    let backend = resolve_backend_strict(backend)?;
    if backend != VisionBackend::PureRust {
        return write_resize_norm_img_into_slice_impl(
            img,
            max_wh_ratio,
            rec_image_shape,
            backend,
            dst,
        );
    }

    let (img_channel, img_height, dst_width) = validate_common(max_wh_ratio, rec_image_shape)?;
    let sample_len = img_channel
        .checked_mul(img_height)
        .and_then(|v| v.checked_mul(dst_width))
        .ok_or_else(|| RapidOcrError::InvalidInput("rec sample size overflow".to_string()))?;
    if dst.len() != sample_len {
        return Err(RapidOcrError::InvalidInput(format!(
            "rec destination slice size mismatch: expected {sample_len}, got {}",
            dst.len()
        )));
    }
    dst.fill(0.0);

    let resized_w = calc_resized_width(img, img_height, dst_width);
    let src = img.as_bgr_cow();
    resize_bgr_inter_linear_into_with_scratch(
        src.as_ref(),
        img.width(),
        img.height(),
        resized_w,
        img_height,
        tmp_bgr,
        resize_scratch,
    )?;

    let plane_stride = img_height * dst_width;
    let plane_stride2 = plane_stride * 2;
    let row_src_stride = resized_w * img_channel;
    for y in 0..img_height {
        let row_base = y * dst_width;
        // Safety:
        // - `tmp_bgr` contains `resized_w * img_height * img_channel` bytes.
        // - destination writes stay within validated CHW destination slice.
        unsafe {
            let mut src_px = tmp_bgr.as_ptr().add(y * row_src_stride);
            let dst_ptr = dst.as_mut_ptr();
            for x in 0..resized_w {
                let row_offset = row_base + x;
                let b = normalize_to_signed_unit(*src_px);
                let g = normalize_to_signed_unit(*src_px.add(1));
                let r = normalize_to_signed_unit(*src_px.add(2));
                *dst_ptr.add(row_offset) = b;
                *dst_ptr.add(plane_stride + row_offset) = g;
                *dst_ptr.add(plane_stride2 + row_offset) = r;
                src_px = src_px.add(img_channel);
            }
        }
    }

    Ok(())
}

#[cfg(test)]
fn resize_norm_img_impl(
    img: &RecImage,
    max_wh_ratio: f64,
    rec_image_shape: [usize; 3],
    backend: VisionBackend,
) -> Result<Array3<f32>> {
    let (img_channel, img_height, dst_width) = validate_common(max_wh_ratio, rec_image_shape)?;
    let sample_len = img_channel
        .checked_mul(img_height)
        .and_then(|v| v.checked_mul(dst_width))
        .ok_or_else(|| RapidOcrError::InvalidInput("rec sample size overflow".to_string()))?;
    let mut out = vec![0.0_f32; sample_len];
    write_resize_norm_img_into_slice_impl(img, max_wh_ratio, rec_image_shape, backend, &mut out)?;
    Array3::from_shape_vec((img_channel, img_height, dst_width), out).map_err(|e| {
        RapidOcrError::InvalidInput(format!("failed to build rec normalized tensor: {e}"))
    })
}

fn write_resize_norm_img_into_slice_impl(
    img: &RecImage,
    max_wh_ratio: f64,
    rec_image_shape: [usize; 3],
    backend: VisionBackend,
    dst: &mut [f32],
) -> Result<()> {
    let (img_channel, img_height, dst_width) = validate_common(max_wh_ratio, rec_image_shape)?;
    let sample_len = img_channel
        .checked_mul(img_height)
        .and_then(|v| v.checked_mul(dst_width))
        .ok_or_else(|| RapidOcrError::InvalidInput("rec sample size overflow".to_string()))?;
    if dst.len() != sample_len {
        return Err(RapidOcrError::InvalidInput(format!(
            "rec destination slice size mismatch: expected {sample_len}, got {}",
            dst.len()
        )));
    }

    dst.fill(0.0);

    let resized_w = calc_resized_width(img, img_height, dst_width);
    let resized = resize_image(img, resized_w, img_height, backend)?;
    let src = resized.as_bgr_cow();
    let src = src.as_ref();
    let plane_stride = img_height * dst_width;
    let plane_stride2 = plane_stride * 2;
    debug_assert_eq!(src.len(), resized.width() * resized.height() * img_channel);

    let row_src_stride = resized.width() * img_channel;
    for y in 0..img_height {
        let row_base = y * dst_width;
        // Safety:
        // - `y < img_height`, and `row_src_stride = resized.width() * img_channel`.
        // - source row pointer remains within the resized image backing buffer.
        // - destination pointer remains within the validated CHW buffer.
        unsafe {
            let mut src_px = src.as_ptr().add(y * row_src_stride);
            let dst_ptr = dst.as_mut_ptr();
            for x in 0..resized_w {
                let row_offset = row_base + x;
                let b = normalize_to_signed_unit(*src_px);
                let g = normalize_to_signed_unit(*src_px.add(1));
                let r = normalize_to_signed_unit(*src_px.add(2));

                *dst_ptr.add(row_offset) = b;
                *dst_ptr.add(plane_stride + row_offset) = g;
                *dst_ptr.add(plane_stride2 + row_offset) = r;
                src_px = src_px.add(img_channel);
            }
        }
    }

    Ok(())
}

#[inline]
fn normalize_to_signed_unit(v: u8) -> f32 {
    signed_unit_lut()[v as usize]
}

fn signed_unit_lut() -> &'static [f32; 256] {
    static LUT: OnceLock<[f32; 256]> = OnceLock::new();
    LUT.get_or_init(|| {
        let mut out = [0.0_f32; 256];
        let mut i = 0usize;
        while i < 256 {
            let mut v = i as f32 * (2.0 / 255.0) - 1.0;
            if v == -0.0 {
                v = 0.0;
            }
            out[i] = v;
            i += 1;
        }
        out
    })
}

fn validate_common(
    max_wh_ratio: f64,
    rec_image_shape: [usize; 3],
) -> Result<(usize, usize, usize)> {
    let [img_channel, img_height, img_width] = rec_image_shape;
    if img_channel != 3 {
        return Err(RapidOcrError::Config(format!(
            "rec_image_shape[0] must be 3, got {img_channel}"
        )));
    }
    if img_height == 0 {
        return Err(RapidOcrError::Config(
            "rec_image_shape[1] must be greater than zero".to_string(),
        ));
    }
    if max_wh_ratio <= 0.0 {
        return Err(RapidOcrError::InvalidInput(format!(
            "max_wh_ratio must be greater than zero, got {max_wh_ratio}"
        )));
    }

    // Keep parity with Python: avoid f32 precision underflow (e.g. 48 * 6.6666665 -> 319.99997)
    // by keeping at least the configured rec width.
    let dst_width = ((img_height as f64) * max_wh_ratio) as usize;
    let dst_width = dst_width.max(img_width);
    if dst_width == 0 {
        return Err(RapidOcrError::InvalidInput(format!(
            "computed destination width is zero (img_height={img_height}, max_wh_ratio={max_wh_ratio})"
        )));
    }

    Ok((img_channel, img_height, dst_width))
}

fn calc_resized_width(img: &RecImage, img_height: usize, dst_width: usize) -> usize {
    let ratio = img.width() as f64 / img.height() as f64;
    let estimate = ((img_height as f64) * ratio).ceil() as usize;
    estimate.min(dst_width)
}

#[cfg(test)]
mod tests {
    use super::{
        VisionBackend, make_batch, make_batch_with_backend_by_indices, resize_norm_img,
        write_resize_norm_img_into_slice,
    };
    use crate::config::RecImage;
    #[cfg(all(feature = "opencv-backend", feature = "image-io"))]
    use std::path::PathBuf;

    #[test]
    fn resize_norm_img_rejects_zero_height() {
        let image = RecImage::from_bgr_u8(10, 10, vec![0; 10 * 10 * 3]).expect("valid image");
        let err = resize_norm_img(&image, 1.0, [3, 0, 320]).expect_err("must reject");
        assert!(
            err.to_string()
                .contains("rec_image_shape[1] must be greater than zero")
        );
    }

    #[test]
    fn make_batch_rejects_non_positive_ratio() {
        let image = RecImage::from_bgr_u8(10, 10, vec![0; 10 * 10 * 3]).expect("valid image");
        let err = make_batch(&[image], 0.0, [3, 48, 320]).expect_err("must reject");
        assert!(
            err.to_string()
                .contains("max_wh_ratio must be greater than zero")
        );
    }

    #[test]
    fn open_cv_backend_rejected_without_feature() {
        #[cfg(not(feature = "opencv-backend"))]
        {
            let image = RecImage::from_bgr_u8(10, 10, vec![0; 10 * 10 * 3]).expect("valid image");
            let err =
                super::make_batch_with_backend(&[image], 1.0, [3, 48, 320], VisionBackend::OpenCv)
                    .expect_err("must reject open cv backend when feature is disabled");
            assert!(
                err.to_string()
                    .contains("feature `opencv-backend` is not enabled")
            );
        }
    }

    #[test]
    fn make_batch_by_indices_rejects_out_of_bounds_index() {
        let image = RecImage::from_bgr_u8(10, 10, vec![0; 10 * 10 * 3]).expect("valid image");
        let err = make_batch_with_backend_by_indices(
            &[image],
            &[1],
            1.0,
            [3, 48, 320],
            VisionBackend::PureRust,
        )
        .expect_err("must reject out-of-bounds index");
        assert!(err.to_string().contains("out of bounds"));
    }

    #[test]
    fn write_into_slice_matches_array_output() {
        let image = RecImage::from_bgr_u8(
            7,
            5,
            (0..7 * 5 * 3).map(|v| (v % 255) as u8).collect::<Vec<_>>(),
        )
        .expect("valid image");

        let max_wh_ratio = 2.5;
        let shape = [3, 48, 320];
        let arr = resize_norm_img(&image, max_wh_ratio, shape).expect("array output should work");

        let mut buf = vec![0.0_f32; arr.len()];
        write_resize_norm_img_into_slice(
            &image,
            max_wh_ratio,
            shape,
            VisionBackend::PureRust,
            &mut buf,
        )
        .expect("slice writer should work");

        let arr_slice = arr.as_slice().expect("array should be contiguous");
        assert_eq!(arr_slice.len(), buf.len());
        for (a, b) in arr_slice.iter().zip(buf.iter()) {
            assert!((*a - *b).abs() <= f32::EPSILON);
        }
    }

    #[cfg(all(feature = "opencv-backend", feature = "image-io"))]
    #[test]
    fn pure_rec_preprocess_matches_opencv_on_test_images() {
        let mut root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        root.push("test");
        root.push("test_files");
        let images = [
            "arabic.png",
            "black_font_color_transparent.png",
            "check_return_word_len.jpeg",
            "ch_doc_server.png",
            "ch_en_num.jpg",
            "cyrillic.png",
            "devanagari.jpg",
            "devanagari_rec.png",
            "el_rec.jpg",
            "empty_black.jpg",
            "en.jpg",
            "en_rec.jpg",
            "eslav.jpg",
            "img_exif_orientation.jpg",
            "issue_170.png",
            "japan.jpg",
            "korean.jpg",
            "latin.jpg",
            "return_word_debug.jpg",
            "short.png",
            "ta.png",
            "te.png",
            "test_letterbox_like.jpg",
            "test_without_det.jpg",
            "text_cls.jpg",
            "text_det.jpg",
            "text_rec.jpg",
            "text_vertical_words.png",
            "th_rec.jpg",
            "white_font_color_transparent.png",
        ];

        for name in images {
            let mut path = root.clone();
            path.push(name);
            let image = RecImage::from_path(&path).expect("fixture image should load");
            let max_wh_ratio =
                (320.0_f64 / 48.0_f64).max(image.width() as f64 / image.height() as f64);
            let pure = super::resize_norm_img_with_backend(
                &image,
                max_wh_ratio,
                [3, 48, 320],
                VisionBackend::PureRust,
            )
            .expect("pure preprocess should work");
            let opencv = super::resize_norm_img_with_backend(
                &image,
                max_wh_ratio,
                [3, 48, 320],
                VisionBackend::OpenCv,
            )
            .expect("opencv preprocess should work");

            assert_eq!(pure.shape(), opencv.shape(), "shape mismatch for {name}");
            let pure = pure.as_slice().expect("pure tensor should be contiguous");
            let opencv = opencv
                .as_slice()
                .expect("opencv tensor should be contiguous");
            assert_eq!(
                pure.len(),
                opencv.len(),
                "tensor length mismatch for {name}"
            );
            for (idx, (a, b)) in pure.iter().zip(opencv.iter()).enumerate() {
                assert_eq!(*a, *b, "tensor mismatch for {name} at flat index {idx}");
            }
        }
    }
}

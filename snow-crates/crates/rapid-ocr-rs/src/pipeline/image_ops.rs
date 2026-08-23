use crate::{
    Quad,
    config::{RecImage, VisionBackend},
    error::{RapidOcrError, Result},
    vision::{
        image_backend::resize_image as resize_image_with_backend, rotate_crop::rotate_crop_image,
    },
};
use rayon::prelude::*;

#[derive(Debug, Clone, Copy, Default)]
pub struct PreprocessRecord {
    pub ratio_h: f32,
    pub ratio_w: f32,
    pub pad_top: usize,
    pub pad_left: usize,
}

pub fn resize_image_within_bounds(
    img: RecImage,
    min_side_len: usize,
    max_side_len: usize,
    backend: VisionBackend,
) -> Result<(RecImage, f32, f32)> {
    let mut current = img;
    let mut ratio_h = 1.0_f32;
    let mut ratio_w = 1.0_f32;

    if current.width().max(current.height()) > max_side_len {
        let (resized, rh, rw) = resize_with_bound(&current, max_side_len, true, backend)?;
        current = resized;
        ratio_h = rh;
        ratio_w = rw;
    }
    if current.width().min(current.height()) < min_side_len {
        let (resized, rh, rw) = resize_with_bound(&current, min_side_len, false, backend)?;
        current = resized;
        ratio_h = rh;
        ratio_w = rw;
    }
    Ok((current, ratio_h, ratio_w))
}

pub fn apply_vertical_padding_in_place(
    img: &mut RecImage,
    width_height_ratio: f32,
    min_height: usize,
) -> Result<usize> {
    let h = img.height();
    let w = img.width();
    if h == 0 || w == 0 {
        return Err(RapidOcrError::InvalidImage(
            "image width/height cannot be zero".to_string(),
        ));
    }
    let use_limit_ratio = if width_height_ratio < 0.0 {
        false
    } else {
        w as f32 / h as f32 > width_height_ratio
    };
    if h > min_height && !use_limit_ratio {
        return Ok(0);
    }

    let target_h = if width_height_ratio > 0.0 {
        ((w as f32 / width_height_ratio).max(min_height as f32) as usize) * 2
    } else {
        min_height * 2
    };
    let padding_h = target_h.abs_diff(h) / 2;
    let padded = pad_image(img, padding_h, padding_h, 0, 0)?;
    *img = padded;
    Ok(padding_h)
}

pub fn crop_text_regions(
    img: &RecImage,
    det_boxes: &[Quad],
    backend: VisionBackend,
) -> Result<Vec<RecImage>> {
    let crops: Vec<Result<RecImage>> = det_boxes
        .par_iter()
        .map(|box_| {
            let mut pts = *box_;
            for p in &mut pts {
                p[0] = p[0].clamp(0.0, img.width().saturating_sub(1) as f32);
                p[1] = p[1].clamp(0.0, img.height().saturating_sub(1) as f32);
            }
            let mut crop = rotate_crop_image(img, pts, backend)?;
            if crop.height() as f32 / crop.width().max(1) as f32 >= 1.5 {
                crop = rotate_90(crop)?;
            }
            Ok(crop)
        })
        .collect();

    let mut out = Vec::with_capacity(crops.len());
    for crop in crops {
        out.push(crop?);
    }
    Ok(out)
}

pub fn map_boxes_to_original(
    boxes: &mut [Quad],
    record: PreprocessRecord,
    ori_h: usize,
    ori_w: usize,
) {
    for box_ in boxes {
        for p in box_ {
            p[0] -= record.pad_left as f32;
            p[1] -= record.pad_top as f32;
            p[0] *= record.ratio_w;
            p[1] *= record.ratio_h;
            p[0] = p[0].clamp(0.0, ori_w as f32);
            p[1] = p[1].clamp(0.0, ori_h as f32);
        }
    }
}

pub fn map_img_to_original(
    imgs: &[RecImage],
    ratio_h: f32,
    ratio_w: f32,
    backend: VisionBackend,
) -> Result<Vec<RecImage>> {
    let mapped: Vec<Result<RecImage>> = imgs
        .par_iter()
        .map(|img| {
            let ori_h = (img.height() as f32 * ratio_h).round_ties_even().max(1.0) as usize;
            let ori_w = (img.width() as f32 * ratio_w).round_ties_even().max(1.0) as usize;
            resize_image(img, ori_w, ori_h, backend)
        })
        .collect();

    let mut out = Vec::with_capacity(mapped.len());
    for item in mapped {
        out.push(item?);
    }
    Ok(out)
}

pub fn resize_image(
    img: &RecImage,
    new_w: usize,
    new_h: usize,
    backend: VisionBackend,
) -> Result<RecImage> {
    resize_image_with_backend(img, new_w, new_h, backend)
}

fn resize_with_bound(
    img: &RecImage,
    side_len: usize,
    use_max: bool,
    backend: VisionBackend,
) -> Result<(RecImage, f32, f32)> {
    let h = img.height();
    let w = img.width();
    let ratio = if use_max {
        side_len as f32 / h.max(w) as f32
    } else {
        side_len as f32 / h.min(w) as f32
    };
    let mut resize_h = (h as f32 * ratio) as usize;
    let mut resize_w = (w as f32 * ratio) as usize;
    resize_h = ((resize_h as f32 / 32.0).round_ties_even() as usize * 32).max(32);
    resize_w = ((resize_w as f32 / 32.0).round_ties_even() as usize * 32).max(32);

    let resized = resize_image(img, resize_w, resize_h, backend)?;
    let ratio_h = h as f32 / resize_h as f32;
    let ratio_w = w as f32 / resize_w as f32;
    Ok((resized, ratio_h, ratio_w))
}

fn pad_image(
    img: &RecImage,
    top: usize,
    bottom: usize,
    left: usize,
    right: usize,
) -> Result<RecImage> {
    let new_w = img.width() + left + right;
    let new_h = img.height() + top + bottom;
    let mut out = vec![0_u8; new_w * new_h * 3];
    let src = img.as_bgr_cow();
    let src = src.as_ref();
    let copy_bytes_per_row = img.width() * 3;
    for y in 0..img.height() {
        let src_row_start = y * copy_bytes_per_row;
        let dst_row_start = ((y + top) * new_w + left) * 3;
        let src_row = &src[src_row_start..src_row_start + copy_bytes_per_row];
        let dst_row = &mut out[dst_row_start..dst_row_start + copy_bytes_per_row];
        dst_row.copy_from_slice(src_row);
    }
    RecImage::from_bgr_u8(new_w, new_h, out)
}

fn rotate_90(img: RecImage) -> Result<RecImage> {
    let src = img.as_bgr_cow();
    let src = src.as_ref();
    let mut out = vec![0_u8; src.len()];
    let new_w = img.height();
    let new_h = img.width();
    for y in 0..img.height() {
        for x in 0..img.width() {
            let src_idx = (y * img.width() + x) * 3;
            let dst_x = y;
            let dst_y = new_h - 1 - x;
            let dst_idx = (dst_y * new_w + dst_x) * 3;
            out[dst_idx] = src[src_idx];
            out[dst_idx + 1] = src[src_idx + 1];
            out[dst_idx + 2] = src[src_idx + 2];
        }
    }
    RecImage::from_bgr_u8(new_w, new_h, out)
}

#[cfg(test)]
mod tests {
    use super::apply_vertical_padding_in_place;
    use crate::config::RecImage;

    fn image(width: usize, height: usize) -> RecImage {
        RecImage::from_bgr_u8(width, height, vec![0; width * height * 3])
            .expect("test image should be valid")
    }

    #[test]
    fn padding_keeps_buffer_when_no_padding_is_needed() {
        let mut img = image(100, 50);
        let original_ptr = img.as_bytes().as_ptr();

        let pad_top =
            apply_vertical_padding_in_place(&mut img, 8.0, 30).expect("padding should succeed");

        assert_eq!(pad_top, 0);
        assert_eq!(img.height(), 50);
        assert_eq!(img.as_bytes().as_ptr(), original_ptr);
    }

    #[test]
    fn padding_updates_image_and_reports_top_padding() {
        let mut img = image(100, 10);

        let pad_top =
            apply_vertical_padding_in_place(&mut img, 8.0, 30).expect("padding should succeed");

        assert_eq!(pad_top, 25);
        assert_eq!(img.width(), 100);
        assert_eq!(img.height(), 60);
    }
}

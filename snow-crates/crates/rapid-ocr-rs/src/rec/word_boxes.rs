use nalgebra::{SMatrix, SVector};
#[cfg(feature = "opencv-backend")]
use opencv::{
    core::{self, Mat, Point2f},
    imgproc,
    prelude::*,
};

#[cfg(test)]
use crate::vision::backend::default_backend;
use crate::{
    Quad,
    config::{RecImage, VisionBackend},
    error::{RapidOcrError, Result},
    types::{LineResult, WordBox, WordInfo, WordType},
    vision::backend::resolve_backend_strict,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Direction {
    Horizontal,
    Vertical,
}

#[cfg(test)]
pub fn compute_word_boxes(
    imgs: &[RecImage],
    dt_boxes: &[Quad],
    lines: &[LineResult],
    return_single_char_box: bool,
) -> Result<Vec<Vec<WordBox>>> {
    compute_word_boxes_with_backend(
        imgs,
        dt_boxes,
        lines,
        return_single_char_box,
        default_backend(),
    )
}

pub fn compute_word_boxes_with_backend(
    imgs: &[RecImage],
    dt_boxes: &[Quad],
    lines: &[LineResult],
    return_single_char_box: bool,
    backend: VisionBackend,
) -> Result<Vec<Vec<WordBox>>> {
    let backend = resolve_backend_strict(backend)?;
    if imgs.len() != dt_boxes.len() || imgs.len() != lines.len() {
        return Err(RapidOcrError::InvalidInput(format!(
            "compute_word_boxes input length mismatch: imgs={}, dt_boxes={}, lines={}",
            imgs.len(),
            dt_boxes.len(),
            lines.len()
        )));
    }

    Ok(compute_word_boxes_unchecked(
        imgs,
        dt_boxes,
        lines,
        return_single_char_box,
        backend,
    ))
}

fn compute_word_boxes_unchecked(
    imgs: &[RecImage],
    dt_boxes: &[Quad],
    lines: &[LineResult],
    return_single_char_box: bool,
    backend: VisionBackend,
) -> Vec<Vec<WordBox>> {
    let mut out = Vec::new();

    for ((img, det_box), line) in imgs.iter().zip(dt_boxes.iter()).zip(lines.iter()) {
        let Some(word_info) = &line.word_info else {
            out.push(Vec::new());
            continue;
        };

        if line.text.is_empty() || word_info.line_txt_len == 0.0 {
            out.push(Vec::new());
            continue;
        }

        let img_box = [
            [0.0_f32, 0.0_f32],
            [img.width() as f32, 0.0_f32],
            [img.width() as f32, img.height() as f32],
            [0.0_f32, img.height() as f32],
        ];

        let (word_content, mut word_boxes, confs) =
            cal_ocr_word_box(&line.text, img_box, word_info, return_single_char_box);

        adjust_box_overlap(&mut word_boxes);
        let direction = get_box_direction(*det_box);
        let mapped =
            reverse_rotate_crop_image_with_backend(*det_box, &word_boxes, direction, backend);

        let item = word_content
            .into_iter()
            .zip(confs.into_iter().chain(std::iter::repeat(0.0)))
            .zip(mapped)
            .map(|((text, score), bbox)| WordBox { text, score, bbox })
            .collect::<Vec<_>>();

        out.push(item);
    }

    out
}

fn get_box_direction(box_: Quad) -> Direction {
    let edge_lengths = [
        l2(box_[0], box_[1]),
        l2(box_[1], box_[2]),
        l2(box_[2], box_[3]),
        l2(box_[3], box_[0]),
    ];

    let width = edge_lengths[0].max(edge_lengths[2]);
    let height = edge_lengths[1].max(edge_lengths[3]);

    if width < 1e-6 {
        return Direction::Vertical;
    }

    let aspect_ratio = (height / width * 100.0).round() / 100.0;
    if aspect_ratio >= 1.5 {
        Direction::Vertical
    } else {
        Direction::Horizontal
    }
}

fn cal_ocr_word_box(
    rec_txt: &str,
    bbox: Quad,
    word_info: &WordInfo,
    return_single_char_box: bool,
) -> (Vec<String>, Vec<Quad>, Vec<f32>) {
    if rec_txt.is_empty() || word_info.line_txt_len == 0.0 {
        return (Vec::new(), Vec::new(), Vec::new());
    }

    let (x0, y0, x1, y1) = quad_to_rect_bbox(&bbox);
    let avg_col_width = (x1 - x0) / word_info.line_txt_len;

    let is_all_en_num = word_info
        .word_types
        .iter()
        .all(|v| matches!(v, WordType::EnNum));

    let mut line_cols: Vec<Vec<usize>> = Vec::new();
    let mut char_widths = Vec::new();
    let mut word_contents = Vec::new();

    for (word, word_col) in word_info.words.iter().zip(word_info.word_cols.iter()) {
        if is_all_en_num && !return_single_char_box {
            line_cols.push(word_col.clone());
            word_contents.push(word.join(""));
        } else {
            for col in word_col {
                line_cols.push(vec![*col]);
            }
            for token in word {
                word_contents.push(token.clone());
            }
        }

        if word_col.len() > 1 {
            char_widths.push(calc_avg_char_width(word_col, avg_col_width));
        }
    }

    let avg_char_width = calc_all_char_avg_width(&char_widths, x0, x1, rec_txt.chars().count());

    let boxes = if is_all_en_num && !return_single_char_box {
        calc_en_num_box(&line_cols, avg_char_width, avg_col_width, (x0, y0, x1, y1))
    } else {
        line_cols
            .iter()
            .flat_map(|cols| calc_box(cols, avg_char_width, avg_col_width, (x0, y0, x1, y1)))
            .collect::<Vec<_>>()
    };

    (word_contents, boxes, word_info.confs.clone())
}

fn calc_en_num_box(
    line_cols: &[Vec<usize>],
    avg_char_width: f32,
    avg_col_width: f32,
    bbox_points: (f32, f32, f32, f32),
) -> Vec<Quad> {
    let mut results = Vec::new();
    for cols in line_cols {
        let boxes = calc_box(cols, avg_char_width, avg_col_width, bbox_points);
        if boxes.is_empty() {
            continue;
        }
        let (x0, y0, x1, y1) = quad_vec_to_rect_bbox(&boxes);
        results.push([[x0, y0], [x1, y0], [x1, y1], [x0, y1]]);
    }
    results
}

fn calc_box(
    cols: &[usize],
    avg_char_width: f32,
    avg_col_width: f32,
    bbox_points: (f32, f32, f32, f32),
) -> Vec<Quad> {
    let (x0, y0, x1, y1) = bbox_points;
    let mut results = Vec::new();

    for col_idx in cols {
        let center_x = (*col_idx as f32 + 0.5) * avg_col_width;

        let char_x0 = ((center_x - avg_char_width / 2.0).max(0.0)).floor() + x0;
        let char_x1 = ((center_x + avg_char_width / 2.0).min(x1 - x0)).floor() + x0;

        results.push([[char_x0, y0], [char_x1, y0], [char_x1, y1], [char_x0, y1]]);
    }

    results.sort_by(|a, b| {
        a[0][0]
            .partial_cmp(&b[0][0])
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    results
}

fn calc_avg_char_width(word_col: &[usize], each_col_width: f32) -> f32 {
    let total = (word_col[word_col.len() - 1] - word_col[0]) as f32 * each_col_width;
    total / (word_col.len() as f32 - 1.0)
}

fn calc_all_char_avg_width(width_list: &[f32], bbox_x0: f32, bbox_x1: f32, txt_len: usize) -> f32 {
    if txt_len == 0 {
        return 0.0;
    }
    if !width_list.is_empty() {
        return width_list.iter().sum::<f32>() / width_list.len() as f32;
    }
    (bbox_x1 - bbox_x0) / txt_len as f32
}

fn adjust_box_overlap(word_box_list: &mut [Quad]) {
    if word_box_list.len() < 2 {
        return;
    }

    for i in 0..(word_box_list.len() - 1) {
        let cur_right = word_box_list[i][1][0];
        let next_left = word_box_list[i + 1][0][0];

        if cur_right > next_left {
            let distance = (cur_right - next_left).abs();
            word_box_list[i][1][0] -= distance / 2.0;
            word_box_list[i][2][0] -= distance / 2.0;
            word_box_list[i + 1][0][0] += distance / 2.0;
            word_box_list[i + 1][3][0] += distance / 2.0;
        }
    }
}

fn reverse_rotate_crop_image(
    bbox_points: Quad,
    word_points_list: &[Quad],
    direction: Direction,
) -> Vec<Quad> {
    let left = bbox_points
        .iter()
        .map(|p| p[0])
        .fold(f32::INFINITY, |acc, v| acc.min(v));
    let top = bbox_points
        .iter()
        .map(|p| p[1])
        .fold(f32::INFINITY, |acc, v| acc.min(v));

    let mut local_bbox = bbox_points;
    for p in &mut local_bbox {
        p[0] -= left;
        p[1] -= top;
    }

    let img_crop_width = l2(local_bbox[0], local_bbox[1]);
    let img_crop_height = l2(local_bbox[0], local_bbox[3]);

    let pts_std = [
        [0.0_f32, 0.0_f32],
        [img_crop_width, 0.0_f32],
        [img_crop_width, img_crop_height],
        [0.0_f32, img_crop_height],
    ];

    let h = homography_from_4pt(local_bbox, pts_std);
    let inv = h
        .try_inverse()
        .unwrap_or_else(SMatrix::<f32, 3, 3>::identity);

    let mut out = Vec::with_capacity(word_points_list.len());
    for word_points in word_points_list {
        let mut mapped = [[0.0_f32; 2]; 4];

        for (i, point) in word_points.iter().enumerate() {
            let mut p = *point;
            if matches!(direction, Direction::Vertical) {
                p = s_rotate(-90.0_f32.to_radians(), p[0], p[1], 0.0, 0.0);
                p[0] += img_crop_width;
            }

            let v = inv * SVector::<f32, 3>::new(p[0], p[1], 1.0);
            let x = if v[2] != 0.0 { v[0] / v[2] } else { v[0] };
            let y = if v[2] != 0.0 { v[1] / v[2] } else { v[1] };

            mapped[i] = [x + left, y + top];
        }

        out.push(order_points(mapped));
    }

    out
}

fn reverse_rotate_crop_image_with_backend(
    bbox_points: Quad,
    word_points_list: &[Quad],
    direction: Direction,
    backend: VisionBackend,
) -> Vec<Quad> {
    match backend {
        VisionBackend::PureRust => {
            reverse_rotate_crop_image(bbox_points, word_points_list, direction)
        }
        VisionBackend::OpenCv => {
            #[cfg(feature = "opencv-backend")]
            {
                reverse_rotate_crop_image_opencv(bbox_points, word_points_list, direction)
                    .unwrap_or_else(|_| {
                        reverse_rotate_crop_image(bbox_points, word_points_list, direction)
                    })
            }
            #[cfg(not(feature = "opencv-backend"))]
            {
                reverse_rotate_crop_image(bbox_points, word_points_list, direction)
            }
        }
    }
}

#[cfg(feature = "opencv-backend")]
fn reverse_rotate_crop_image_opencv(
    bbox_points: Quad,
    word_points_list: &[Quad],
    direction: Direction,
) -> Result<Vec<Quad>> {
    let left = bbox_points
        .iter()
        .map(|p| p[0])
        .fold(f32::INFINITY, |acc, v| acc.min(v)) as i32;
    let top = bbox_points
        .iter()
        .map(|p| p[1])
        .fold(f32::INFINITY, |acc, v| acc.min(v)) as i32;

    let mut local_bbox = bbox_points;
    for p in &mut local_bbox {
        p[0] -= left as f32;
        p[1] -= top as f32;
    }

    let img_crop_width = l2(local_bbox[0], local_bbox[1]) as i32;
    let img_crop_height = l2(local_bbox[0], local_bbox[3]) as i32;

    let src = [
        Point2f::new(local_bbox[0][0], local_bbox[0][1]),
        Point2f::new(local_bbox[1][0], local_bbox[1][1]),
        Point2f::new(local_bbox[2][0], local_bbox[2][1]),
        Point2f::new(local_bbox[3][0], local_bbox[3][1]),
    ];
    let dst = [
        Point2f::new(0.0, 0.0),
        Point2f::new(img_crop_width as f32, 0.0),
        Point2f::new(img_crop_width as f32, img_crop_height as f32),
        Point2f::new(0.0, img_crop_height as f32),
    ];

    let m = imgproc::get_perspective_transform_slice(&src, &dst, core::DECOMP_LU).map_err(|e| {
        RapidOcrError::Config(format!("opencv getPerspectiveTransform failed: {e}"))
    })?;

    let mut inv = Mat::default();
    core::invert(&m, &mut inv, core::DECOMP_LU)
        .map_err(|e| RapidOcrError::Config(format!("opencv invert failed: {e}")))?;

    let mut inv64 = Mat::default();
    inv.convert_to(&mut inv64, core::CV_64F, 1.0, 0.0)
        .map_err(|e| RapidOcrError::Config(format!("opencv convert_to(CV_64F) failed: {e}")))?;

    let h00 = *inv64
        .at_2d::<f64>(0, 0)
        .map_err(|e| RapidOcrError::Config(format!("opencv mat access failed: {e}")))?;
    let h01 = *inv64
        .at_2d::<f64>(0, 1)
        .map_err(|e| RapidOcrError::Config(format!("opencv mat access failed: {e}")))?;
    let h02 = *inv64
        .at_2d::<f64>(0, 2)
        .map_err(|e| RapidOcrError::Config(format!("opencv mat access failed: {e}")))?;
    let h10 = *inv64
        .at_2d::<f64>(1, 0)
        .map_err(|e| RapidOcrError::Config(format!("opencv mat access failed: {e}")))?;
    let h11 = *inv64
        .at_2d::<f64>(1, 1)
        .map_err(|e| RapidOcrError::Config(format!("opencv mat access failed: {e}")))?;
    let h12 = *inv64
        .at_2d::<f64>(1, 2)
        .map_err(|e| RapidOcrError::Config(format!("opencv mat access failed: {e}")))?;
    let h20 = *inv64
        .at_2d::<f64>(2, 0)
        .map_err(|e| RapidOcrError::Config(format!("opencv mat access failed: {e}")))?;
    let h21 = *inv64
        .at_2d::<f64>(2, 1)
        .map_err(|e| RapidOcrError::Config(format!("opencv mat access failed: {e}")))?;
    let h22 = *inv64
        .at_2d::<f64>(2, 2)
        .map_err(|e| RapidOcrError::Config(format!("opencv mat access failed: {e}")))?;

    let mut out = Vec::with_capacity(word_points_list.len());
    for word_points in word_points_list {
        let mut mapped = [[0.0_f32; 2]; 4];

        for (idx, point) in word_points.iter().enumerate() {
            let mut p = *point;
            if matches!(direction, Direction::Vertical) {
                p = s_rotate(-90.0_f32.to_radians(), p[0], p[1], 0.0, 0.0);
                p[0] += img_crop_width as f32;
            }

            let x = p[0] as f64;
            let y = p[1] as f64;
            let px = h00 * x + h01 * y + h02;
            let py = h10 * x + h11 * y + h12;
            let pz = h20 * x + h21 * y + h22;

            let mapped_x = if pz.abs() > f64::EPSILON { px / pz } else { px };
            let mapped_y = if pz.abs() > f64::EPSILON { py / pz } else { py };

            mapped[idx] = [
                (mapped_x + left as f64) as i32 as f32,
                (mapped_y + top as f64) as i32 as f32,
            ];
        }

        out.push(order_points(mapped));
    }

    Ok(out)
}

fn homography_from_4pt(src: Quad, dst: Quad) -> SMatrix<f32, 3, 3> {
    let mut a = SMatrix::<f32, 8, 8>::zeros();
    let mut b = SVector::<f32, 8>::zeros();

    for i in 0..4 {
        let x = src[i][0];
        let y = src[i][1];
        let x_cap = dst[i][0];
        let y_cap = dst[i][1];

        let r0 = i * 2;
        let r1 = r0 + 1;

        a[(r0, 0)] = x;
        a[(r0, 1)] = y;
        a[(r0, 2)] = 1.0;
        a[(r0, 6)] = -x * x_cap;
        a[(r0, 7)] = -y * x_cap;
        b[r0] = x_cap;

        a[(r1, 3)] = x;
        a[(r1, 4)] = y;
        a[(r1, 5)] = 1.0;
        a[(r1, 6)] = -x * y_cap;
        a[(r1, 7)] = -y * y_cap;
        b[r1] = y_cap;
    }

    if let Some(h) = a.lu().solve(&b) {
        SMatrix::<f32, 3, 3>::from_row_slice(&[h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], 1.0])
    } else {
        SMatrix::<f32, 3, 3>::identity()
    }
}

fn s_rotate(angle: f32, valuex: f32, valuey: f32, pointx: f32, pointy: f32) -> [f32; 2] {
    let x = (valuex - pointx) * angle.cos() + (valuey - pointy) * angle.sin() + pointx;
    let y = (valuey - pointy) * angle.cos() - (valuex - pointx) * angle.sin() + pointy;
    [x, y]
}

fn order_points(ori_box: Quad) -> Quad {
    let mut points = ori_box.to_vec();
    points.sort_by(|a, b| {
        a[1].partial_cmp(&b[1])
            .unwrap_or(std::cmp::Ordering::Equal)
            .then(a[0].partial_cmp(&b[0]).unwrap_or(std::cmp::Ordering::Equal))
    });

    let mut top = [points[0], points[1]];
    let mut bottom = [points[2], points[3]];
    top.sort_by(|a, b| a[0].partial_cmp(&b[0]).unwrap_or(std::cmp::Ordering::Equal));
    bottom.sort_by(|a, b| a[0].partial_cmp(&b[0]).unwrap_or(std::cmp::Ordering::Equal));

    [top[0], top[1], bottom[1], bottom[0]]
}

fn quad_to_rect_bbox(quad: &Quad) -> (f32, f32, f32, f32) {
    let mut x_min = f32::INFINITY;
    let mut y_min = f32::INFINITY;
    let mut x_max = f32::NEG_INFINITY;
    let mut y_max = f32::NEG_INFINITY;

    for point in quad {
        x_min = x_min.min(point[0]);
        y_min = y_min.min(point[1]);
        x_max = x_max.max(point[0]);
        y_max = y_max.max(point[1]);
    }

    (x_min, y_min, x_max, y_max)
}

fn quad_vec_to_rect_bbox(quads: &[Quad]) -> (f32, f32, f32, f32) {
    let mut x_min = f32::INFINITY;
    let mut y_min = f32::INFINITY;
    let mut x_max = f32::NEG_INFINITY;
    let mut y_max = f32::NEG_INFINITY;

    for quad in quads {
        for point in quad {
            x_min = x_min.min(point[0]);
            y_min = y_min.min(point[1]);
            x_max = x_max.max(point[0]);
            y_max = y_max.max(point[1]);
        }
    }

    (x_min, y_min, x_max, y_max)
}

fn l2(a: [f32; 2], b: [f32; 2]) -> f32 {
    let dx = a[0] - b[0];
    let dy = a[1] - b[1];
    (dx * dx + dy * dy).sqrt()
}

#[cfg(test)]
mod tests {
    use crate::{
        config::RecImage,
        error::RapidOcrError,
        types::{LineResult, WordInfo, WordType},
    };

    use super::compute_word_boxes;

    #[test]
    fn compute_word_boxes_smoke() {
        let img = RecImage::from_bgr_u8(100, 20, vec![0; 100 * 20 * 3]).expect("valid image");
        let line = LineResult {
            text: "AB".to_string(),
            score: 0.9,
            word_info: Some(WordInfo {
                words: vec![vec!["A".to_string(), "B".to_string()]],
                word_cols: vec![vec![1, 2]],
                word_types: vec![WordType::EnNum],
                line_txt_len: 4.0,
                confs: vec![0.8, 0.9],
            }),
        };
        let det = [[[0.0, 0.0], [100.0, 0.0], [100.0, 20.0], [0.0, 20.0]]];
        let out =
            compute_word_boxes(&[img], &det, &[line], false).expect("word boxes should compute");
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].len(), 1);
        assert_eq!(out[0][0].text, "AB");
    }

    #[test]
    fn compute_word_boxes_rejects_mismatch() {
        let img = RecImage::from_bgr_u8(100, 20, vec![0; 100 * 20 * 3]).expect("valid image");
        let line = LineResult {
            text: "AB".to_string(),
            score: 0.9,
            word_info: None,
        };
        let det = [[[0.0, 0.0], [100.0, 0.0], [100.0, 20.0], [0.0, 20.0]]];

        let err = compute_word_boxes(&[img], &det, &[line.clone(), line], false)
            .expect_err("should reject mismatched lengths");
        assert!(matches!(err, RapidOcrError::InvalidInput(_)));
    }

    #[test]
    fn checked_backend_rejects_opencv_without_feature() {
        #[cfg(not(feature = "opencv-backend"))]
        {
            let img = RecImage::from_bgr_u8(20, 20, vec![0; 20 * 20 * 3]).expect("valid image");
            let line = LineResult {
                text: "A".to_string(),
                score: 1.0,
                word_info: None,
            };
            let det = [[[0.0, 0.0], [20.0, 0.0], [20.0, 20.0], [0.0, 20.0]]];
            let err = super::compute_word_boxes_with_backend(
                &[img],
                &det,
                &[line],
                false,
                crate::config::VisionBackend::OpenCv,
            )
            .expect_err("must reject opencv backend without feature");
            assert!(matches!(err, RapidOcrError::Config(_)));
        }
    }
}

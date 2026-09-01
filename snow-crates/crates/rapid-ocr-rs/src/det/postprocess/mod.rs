use geo_clipper::{ClipperInt, EndType as ClipperEndType, JoinType as ClipperJoinType};
use geo_types::{Coord, LineString, Polygon};
#[cfg(test)]
use ndarray::Array2;
use ndarray::ArrayView2;
#[cfg(feature = "opencv-backend")]
use opencv::{
    core::{self, Mat, Point, Point2f, Scalar, Vector},
    imgproc,
    prelude::*,
};
use rayon::prelude::*;
#[cfg(target_arch = "x86_64")]
use std::sync::OnceLock;

use crate::{Quad, config::VisionBackend, vision::backend::resolve_backend_or_pure_rust};

#[derive(Debug, Clone)]
pub struct DbPostProcess {
    pub thresh: f32,
    pub box_thresh: f32,
    pub max_candidates: usize,
    pub unclip_ratio: f32,
    pub min_size: usize,
    pub use_dilation: bool,
    pub score_mode: String,
    pub vision_backend: VisionBackend,
}

impl Default for DbPostProcess {
    fn default() -> Self {
        Self {
            thresh: 0.3,
            box_thresh: 0.5,
            max_candidates: 1000,
            unclip_ratio: 1.6,
            min_size: 3,
            use_dilation: true,
            score_mode: "fast".to_string(),
            vision_backend: VisionBackend::PureRust,
        }
    }
}

impl DbPostProcess {
    #[cfg(test)]
    pub fn run(&self, pred: &Array2<f32>, src_w: usize, src_h: usize) -> (Vec<Quad>, Vec<f32>) {
        self.run_view(pred.view(), src_w, src_h)
    }

    pub(crate) fn run_view(
        &self,
        pred: ArrayView2<'_, f32>,
        src_w: usize,
        src_h: usize,
    ) -> (Vec<Quad>, Vec<f32>) {
        let backend = resolve_backend_or_pure_rust(self.vision_backend);
        match backend {
            VisionBackend::PureRust => self.run_pure(pred, src_w, src_h),
            VisionBackend::OpenCv => {
                #[cfg(feature = "opencv-backend")]
                {
                    self.run_with_opencv(pred, src_w, src_h)
                }
                #[cfg(not(feature = "opencv-backend"))]
                {
                    unreachable!("backend resolver should reject unsupported OpenCV backend");
                }
            }
        }
    }

    fn run_pure(
        &self,
        pred: ArrayView2<'_, f32>,
        src_w: usize,
        src_h: usize,
    ) -> (Vec<Quad>, Vec<f32>) {
        let height = pred.nrows();
        let width = pred.ncols();
        if height == 0 || width == 0 {
            return (Vec::new(), Vec::new());
        }

        // Keep parity with RapidOCR Python:
        // 1) threshold => bitmap
        // 2) optional 2x2 dilation
        // 3) findContours
        // 4) boxes_from_bitmap with mini-box score / slow contour score
        let mut bitmap = build_threshold_bitmap(pred, self.thresh);
        if self.use_dilation {
            bitmap = dilate_mask_2x2(&bitmap, width, height);
        }
        let contours = find_contours_from_mask_pure(bitmap, width, height);
        let (boxes, scores) =
            self.boxes_from_bitmap_pure(pred, &contours, width, height, src_w, src_h);
        let (mut boxes, mut scores) = filter_det_res(boxes, scores, src_h, src_w);
        sort_boxes_like_python(&mut boxes, &mut scores, 10.0);
        (boxes, scores)
    }

    #[cfg(feature = "opencv-backend")]
    fn run_with_opencv(
        &self,
        pred: ArrayView2<'_, f32>,
        src_w: usize,
        src_h: usize,
    ) -> (Vec<Quad>, Vec<f32>) {
        let height = pred.nrows();
        let width = pred.ncols();
        if height == 0 || width == 0 {
            return (Vec::new(), Vec::new());
        }

        let mut bitmap = build_threshold_bitmap(pred, self.thresh);
        if self.use_dilation {
            bitmap = dilate_mask_2x2(&bitmap, width, height);
        }
        let contours = match find_contours_from_mask_opencv(&bitmap, height) {
            Ok(v) => v,
            Err(_) => return (Vec::new(), Vec::new()),
        };

        let pred_vec: Vec<f32> = if let Some(s) = pred.as_slice_memory_order() {
            s.to_vec()
        } else {
            pred.iter().copied().collect()
        };

        let pred_1d = match Mat::from_slice(&pred_vec) {
            Ok(v) => v,
            Err(_) => return (Vec::new(), Vec::new()),
        };
        let pred_ref = match pred_1d.reshape(1, height as i32) {
            Ok(v) => v,
            Err(_) => return (Vec::new(), Vec::new()),
        };
        let mut pred_mat = Mat::default();
        if pred_ref.copy_to(&mut pred_mat).is_err() {
            return (Vec::new(), Vec::new());
        }

        let (boxes, scores) =
            self.boxes_from_bitmap_opencv(&pred_mat, &contours, width, height, src_w, src_h);
        let (mut boxes, mut scores) = filter_det_res(boxes, scores, src_h, src_w);
        sort_boxes_like_python(&mut boxes, &mut scores, 10.0);
        (boxes, scores)
    }

    fn boxes_from_bitmap_pure(
        &self,
        pred: ArrayView2<'_, f32>,
        contours: &[Vec<[i32; 2]>],
        bitmap_w: usize,
        bitmap_h: usize,
        dest_w: usize,
        dest_h: usize,
    ) -> (Vec<Quad>, Vec<f32>) {
        let num_candidates = if self.max_candidates == 0 {
            contours.len()
        } else {
            contours.len().min(self.max_candidates)
        };

        let mut boxes = Vec::new();
        let mut scores = Vec::new();
        if num_candidates == 0 {
            return (boxes, scores);
        }
        let scale_target = ScaleTarget {
            bitmap_w,
            bitmap_h,
            dest_w,
            dest_h,
        };
        #[cfg(feature = "opencv-backend")]
        let pred_mat = pred_view_to_mat(pred);

        const PARALLEL_CANDIDATE_THRESHOLD: usize = 12;
        let run_parallel =
            num_candidates >= PARALLEL_CANDIDATE_THRESHOLD && rayon::current_num_threads() > 1;
        #[cfg(feature = "opencv-backend")]
        let run_parallel = run_parallel && pred_mat.is_none();
        if !run_parallel {
            let mut scratch = CandidateScratch::default();
            for (i, contour) in contours.iter().take(num_candidates).enumerate() {
                if let Some((box1, score)) = self.process_contour_candidate_pure(
                    pred,
                    #[cfg(feature = "opencv-backend")]
                    pred_mat.as_ref(),
                    contour,
                    i,
                    &scale_target,
                    &mut scratch,
                ) {
                    boxes.push(box1);
                    scores.push(score);
                }
            }
            return (boxes, scores);
        }

        let candidate_results: Vec<Option<(Quad, f32)>> = contours[..num_candidates]
            .par_iter()
            .enumerate()
            .map_init(CandidateScratch::default, |scratch, (i, contour)| {
                self.process_contour_candidate_pure(
                    pred,
                    #[cfg(feature = "opencv-backend")]
                    None,
                    contour,
                    i,
                    &scale_target,
                    scratch,
                )
            })
            .collect();

        for (box1, score) in candidate_results.into_iter().flatten() {
            boxes.push(box1);
            scores.push(score);
        }

        (boxes, scores)
    }

    fn process_contour_candidate_pure(
        &self,
        pred: ArrayView2<'_, f32>,
        #[cfg(feature = "opencv-backend")] pred_mat: Option<&Mat>,
        contour: &[[i32; 2]],
        _contour_idx: usize,
        scale_target: &ScaleTarget,
        scratch: &mut CandidateScratch,
    ) -> Option<(Quad, f32)> {
        if contour.len() < 3 {
            return None;
        }

        scratch.contour_f.clear();
        scratch
            .contour_f
            .extend(contour.iter().map(|p| [p[0] as f32, p[1] as f32]));
        let (points, sside) = mini_box_from_points_pure(&scratch.contour_f)?;
        #[cfg(feature = "opencv-backend")]
        let (points, sside) = if let Ok(Some((opencv_points, opencv_sside))) =
            mini_box_from_points_opencv(&scratch.contour_f)
        {
            (opencv_points, opencv_sside)
        } else {
            (points, sside)
        };
        if sside < self.min_size as f32 {
            return None;
        }

        let score = if self.score_mode.eq_ignore_ascii_case("slow") {
            #[cfg(feature = "opencv-backend")]
            {
                if let Some(mat) = pred_mat {
                    let mut contour_cv = Vector::<Point>::new();
                    for p in contour {
                        contour_cv.push(Point::new(p[0], p[1]));
                    }
                    match contour_score_opencv(mat, &contour_cv) {
                        Ok(v) => v,
                        Err(_) => contour_score_pure_with_scratch(pred, contour, scratch),
                    }
                } else {
                    contour_score_pure_with_scratch(pred, contour, scratch)
                }
            }
            #[cfg(not(feature = "opencv-backend"))]
            {
                contour_score_pure_with_scratch(pred, contour, scratch)
            }
        } else {
            #[cfg(feature = "opencv-backend")]
            {
                if let Some(mat) = pred_mat {
                    match box_score_fast_opencv(mat, &points) {
                        Ok(v) => v,
                        Err(_) => box_score_fast_pure_with_scratch(pred, &points, scratch),
                    }
                } else {
                    box_score_fast_pure_with_scratch(pred, &points, scratch)
                }
            }
            #[cfg(not(feature = "opencv-backend"))]
            {
                box_score_fast_pure_with_scratch(pred, &points, scratch)
            }
        };
        if self.box_thresh > score {
            return None;
        }

        unclip_polygon_pyclipper_into(&points, self.unclip_ratio, &mut scratch.expanded);
        if scratch.expanded.len() < 3 {
            return None;
        }

        let (box1, sside2) = mini_box_from_points_pure(&scratch.expanded)?;
        #[cfg(feature = "opencv-backend")]
        let (mut box1, sside2) = if let Ok(Some((opencv_box, opencv_sside2))) =
            mini_box_from_points_opencv(&scratch.expanded)
        {
            (opencv_box, opencv_sside2)
        } else {
            (box1, sside2)
        };
        #[cfg(not(feature = "opencv-backend"))]
        let mut box1 = box1;
        if sside2 < self.min_size as f32 + 2.0 {
            return None;
        }
        scale_box_to_dest(
            &mut box1,
            scale_target.bitmap_w,
            scale_target.bitmap_h,
            scale_target.dest_w,
            scale_target.dest_h,
        );
        Some((box1, score))
    }

    #[cfg(feature = "opencv-backend")]
    fn boxes_from_bitmap_opencv(
        &self,
        pred: &Mat,
        contours: &Vector<Vector<Point>>,
        bitmap_w: usize,
        bitmap_h: usize,
        dest_w: usize,
        dest_h: usize,
    ) -> (Vec<Quad>, Vec<f32>) {
        let num_candidates = if self.max_candidates == 0 {
            contours.len()
        } else {
            contours.len().min(self.max_candidates)
        };

        let mut boxes = Vec::new();
        let mut scores = Vec::new();

        for i in 0..num_candidates {
            let contour = match contours.get(i) {
                Ok(v) => v,
                Err(_) => continue,
            };
            if contour.len() < 3 {
                continue;
            }

            let (points, sside) = match mini_box_from_contour_opencv(&contour) {
                Ok(Some(v)) => v,
                _ => continue,
            };
            if sside < self.min_size as f32 {
                continue;
            }

            let score = if self.score_mode.eq_ignore_ascii_case("slow") {
                match contour_score_opencv(pred, &contour) {
                    Ok(v) => v,
                    Err(_) => continue,
                }
            } else {
                match box_score_fast_opencv(pred, &points) {
                    Ok(v) => v,
                    Err(_) => continue,
                }
            };
            if self.box_thresh > score {
                continue;
            }

            let mut expanded = Vec::new();
            unclip_polygon_pyclipper_into(&points, self.unclip_ratio, &mut expanded);
            if expanded.len() < 3 {
                continue;
            }

            let (mut box1, sside2) = match mini_box_from_points_opencv(&expanded) {
                Ok(Some(v)) => v,
                _ => continue,
            };
            if sside2 < self.min_size as f32 + 2.0 {
                continue;
            }

            scale_box_to_dest(&mut box1, bitmap_w, bitmap_h, dest_w, dest_h);
            boxes.push(box1);
            scores.push(score);
        }

        (boxes, scores)
    }
}

#[derive(Default)]
struct CandidateScratch {
    contour_f: Vec<[f32; 2]>,
    shifted_poly: Vec<[f32; 2]>,
    mask: Vec<u8>,
    expanded: Vec<[f32; 2]>,
}

struct ScaleTarget {
    bitmap_w: usize,
    bitmap_h: usize,
    dest_w: usize,
    dest_h: usize,
}

fn build_threshold_bitmap(pred: ArrayView2<'_, f32>, thresh: f32) -> Vec<u8> {
    let mut out = vec![0_u8; pred.len()];
    if out.is_empty() {
        return out;
    }

    if let Some(src) = pred.as_slice_memory_order() {
        threshold_slice_to_bitmap(src, thresh, &mut out);
    } else {
        for (dst, src) in out.iter_mut().zip(pred.iter()) {
            *dst = if *src > thresh { 255 } else { 0 };
        }
    }

    out
}

fn threshold_slice_to_bitmap(src: &[f32], thresh: f32, dst: &mut [u8]) {
    debug_assert_eq!(src.len(), dst.len());
    if src.is_empty() {
        return;
    }

    const PAR_THRESHOLD: usize = 8 * 1024;
    if src.len() < PAR_THRESHOLD {
        threshold_chunk_dispatch(src, thresh, dst);
        return;
    }

    let chunk_size = src.len().div_ceil(rayon::current_num_threads()).max(1024);
    dst.par_chunks_mut(chunk_size)
        .enumerate()
        .for_each(|(chunk_idx, dst_chunk)| {
            let start = chunk_idx * chunk_size;
            let end = start + dst_chunk.len();
            threshold_chunk_dispatch(&src[start..end], thresh, dst_chunk);
        });
}

fn threshold_chunk_dispatch(src: &[f32], thresh: f32, dst: &mut [u8]) {
    debug_assert_eq!(src.len(), dst.len());

    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx2") {
            // Safety: AVX2 path is gated by runtime feature detection and slice bounds.
            unsafe {
                threshold_chunk_avx2(src, thresh, dst);
            }
            return;
        }
        if std::arch::is_x86_feature_detected!("sse4.1") {
            // Safety: SSE4.1 path is gated by runtime feature detection and slice bounds.
            unsafe {
                threshold_chunk_sse41(src, thresh, dst);
            }
            return;
        }
    }

    threshold_chunk_scalar(src, thresh, dst);
}

fn threshold_chunk_scalar(src: &[f32], thresh: f32, dst: &mut [u8]) {
    debug_assert_eq!(src.len(), dst.len());
    for i in 0..src.len() {
        // Safety: loop bounds guarantee in-range access for both slices.
        unsafe {
            let value = *src.get_unchecked(i);
            *dst.get_unchecked_mut(i) = if value > thresh { 255 } else { 0 };
        }
    }
}

#[cfg(target_arch = "x86_64")]
fn threshold_mask_lut8() -> &'static [u64; 256] {
    static LUT: OnceLock<[u64; 256]> = OnceLock::new();
    LUT.get_or_init(|| {
        let mut out = [0_u64; 256];
        let mut mask = 0usize;
        while mask < 256 {
            let mut packed = 0_u64;
            let mut lane = 0usize;
            while lane < 8 {
                if (mask >> lane) & 1 == 1 {
                    packed |= (255_u64) << (lane * 8);
                }
                lane += 1;
            }
            out[mask] = packed;
            mask += 1;
        }
        out
    })
}

#[cfg(target_arch = "x86_64")]
fn threshold_mask_lut4() -> &'static [u32; 16] {
    static LUT: OnceLock<[u32; 16]> = OnceLock::new();
    LUT.get_or_init(|| {
        let mut out = [0_u32; 16];
        let mut mask = 0usize;
        while mask < 16 {
            let mut packed = 0_u32;
            let mut lane = 0usize;
            while lane < 4 {
                if (mask >> lane) & 1 == 1 {
                    packed |= (255_u32) << (lane * 8);
                }
                lane += 1;
            }
            out[mask] = packed;
            mask += 1;
        }
        out
    })
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn threshold_chunk_avx2(src: &[f32], thresh: f32, dst: &mut [u8]) {
    use std::arch::x86_64::{
        __m256, _CMP_GT_OQ, _mm256_cmp_ps, _mm256_loadu_ps, _mm256_movemask_ps, _mm256_set1_ps,
    };

    debug_assert_eq!(src.len(), dst.len());
    let mut i = 0usize;
    let thresh_vec: __m256 = _mm256_set1_ps(thresh);
    let simd_len = src.len() / 8 * 8;
    let lut = threshold_mask_lut8();

    while i < simd_len {
        let mask = unsafe {
            let src_ptr = src.as_ptr().add(i);
            let values = _mm256_loadu_ps(src_ptr);
            let cmp = _mm256_cmp_ps(values, thresh_vec, _CMP_GT_OQ);
            _mm256_movemask_ps(cmp) as usize
        };
        unsafe {
            std::ptr::write_unaligned(dst.as_mut_ptr().add(i) as *mut u64, lut[mask]);
        };
        i += 8;
    }

    if i < src.len() {
        threshold_chunk_scalar(&src[i..], thresh, &mut dst[i..]);
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse4.1")]
unsafe fn threshold_chunk_sse41(src: &[f32], thresh: f32, dst: &mut [u8]) {
    use std::arch::x86_64::{__m128, _mm_cmpgt_ps, _mm_loadu_ps, _mm_movemask_ps, _mm_set1_ps};

    debug_assert_eq!(src.len(), dst.len());
    let mut i = 0usize;
    let thresh_vec: __m128 = _mm_set1_ps(thresh);
    let simd_len = src.len() / 4 * 4;
    let lut = threshold_mask_lut4();

    while i < simd_len {
        let mask = unsafe {
            let src_ptr = src.as_ptr().add(i);
            let values = _mm_loadu_ps(src_ptr);
            let cmp = _mm_cmpgt_ps(values, thresh_vec);
            _mm_movemask_ps(cmp) as usize
        };
        unsafe {
            std::ptr::write_unaligned(dst.as_mut_ptr().add(i) as *mut u32, lut[mask]);
        };
        i += 4;
    }

    if i < src.len() {
        threshold_chunk_scalar(&src[i..], thresh, &mut dst[i..]);
    }
}

fn find_contours_from_mask_pure(mask: Vec<u8>, width: usize, height: usize) -> Vec<Vec<[i32; 2]>> {
    if width == 0 || height == 0 {
        return Vec::new();
    }

    // Mirror cv::findContours preprocess path:
    // 1) copyMakeBorder(..., 1,1,1,1, BORDER_CONSTANT=0)
    // 2) threshold(..., 0, 1, THRESH_BINARY)
    let pad_w = width + 2;
    let pad_h = height + 2;
    let mut image = vec![0_i8; pad_w * pad_h];
    for y in 0..height {
        let src = &mask[y * width..(y + 1) * width];
        let dst_row = &mut image[(y + 1) * pad_w + 1..(y + 1) * pad_w + 1 + width];
        for (dst, src_v) in dst_row.iter_mut().zip(src.iter()) {
            *dst = if *src_v > 0 { MASK8_BLACK as i8 } else { 0_i8 };
        }
    }

    let mut scanner = CvContourScanner8::new(image, pad_w, pad_h, -1, -1);
    let mut out = Vec::new();
    while let Some(contour) = scanner.find_next_contour() {
        out.push(contour);
    }
    out
}

const MASK8_RIGHT: i32 = -128; // 0x80 as signed char
const MASK8_NEW: i32 = 2; // 0x02
const MASK8_FLAGS: i32 = -2; // 0xFE as signed char
const MASK8_BLACK: i32 = 1; // 0x01

const CHAIN_DELTAS: [[i32; 2]; 8] = [
    [1, 0],
    [1, -1],
    [0, -1],
    [-1, -1],
    [-1, 0],
    [-1, 1],
    [0, 1],
    [1, 1],
];

#[inline]
fn chain_delta_index(dir: i32, step: usize) -> isize {
    let d = CHAIN_DELTAS[(dir & 7) as usize];
    d[0] as isize + d[1] as isize * step as isize
}

#[derive(Debug)]
struct CvContourScanner8 {
    image: Vec<i8>,
    width: usize,
    height: usize,
    offset_x: i32,
    offset_y: i32,
    pt_x: usize,
    pt_y: usize,
    lnbd_x: usize,
    lnbd_y: usize,
    nbd: i32,
}

impl CvContourScanner8 {
    fn new(image: Vec<i8>, width: usize, height: usize, offset_x: i32, offset_y: i32) -> Self {
        Self {
            image,
            width,
            height,
            offset_x,
            offset_y,
            pt_x: 1,
            pt_y: 1,
            lnbd_x: 0,
            lnbd_y: 1,
            nbd: 2,
        }
    }

    #[inline]
    fn idx(&self, x: usize, y: usize) -> usize {
        y * self.width + x
    }

    #[inline]
    fn at_i32(&self, x: usize, y: usize) -> i32 {
        self.image[self.idx(x, y)] as i32
    }

    fn find_next_x(
        &self,
        mut x: usize,
        y: usize,
        prev: i32,
        p_out: &mut i32,
        width_bound: usize,
    ) -> usize {
        while x < width_bound {
            let p = self.at_i32(x, y);
            *p_out = p;
            if p != prev {
                return x;
            }
            x += 1;
        }
        x
    }

    fn find_next_contour(&mut self) -> Option<Vec<[i32; 2]>> {
        if self.width < 2 || self.height < 2 {
            return None;
        }

        let width_bound = self.width - 1;
        let height_bound = self.height - 1;
        let mut x = self.pt_x;
        let mut y = self.pt_y;
        if y >= self.height || x >= self.width {
            return None;
        }

        let mut last_pos_x = self.lnbd_x as i32;
        let mut last_pos_y = self.lnbd_y as i32;
        let mut prev = self.at_i32(x.saturating_sub(1), y);

        while y < height_bound {
            let mut p = 0_i32;
            while x < width_bound {
                x = self.find_next_x(x, y, prev, &mut p, width_bound);
                if x >= width_bound {
                    break;
                }

                if let Some(contour) = self.contour_scan(prev, p, &mut last_pos_x, x, y) {
                    self.lnbd_x = last_pos_x.max(0) as usize;
                    self.lnbd_y = last_pos_y.max(0) as usize;
                    return Some(contour);
                }

                prev = p;
                if (prev & MASK8_FLAGS) != 0 {
                    last_pos_x = x as i32;
                }
                x += 1;
            }

            y += 1;
            if y >= height_bound {
                break;
            }
            x = 1;
            prev = 0;
            last_pos_x = 0;
            last_pos_y = y as i32;
        }

        None
    }

    fn contour_scan(
        &mut self,
        prev: i32,
        p: i32,
        last_pos_x: &mut i32,
        x: usize,
        y: usize,
    ) -> Option<Vec<[i32; 2]>> {
        let mut is_hole = false;

        // RETR_LIST + 8-bit logic from contours_new.cpp::contourScan
        if !(prev == 0 && p == MASK8_BLACK) {
            if p != 0 || prev < MASK8_BLACK {
                return None;
            }
            if (prev & MASK8_FLAGS) != 0 {
                *last_pos_x = x as i32 - 1;
            }
            is_hole = true;
        }

        *last_pos_x = x as i32 - if is_hole { 1 } else { 0 };
        let mut nbd_ = self.nbd;
        let contour = self.make_contour(&mut nbd_, is_hole, x as i32, y as i32);
        self.pt_x = x + 1;
        self.pt_y = y;
        self.nbd = nbd_;
        Some(contour)
    }

    fn make_contour(&mut self, _nbd: &mut i32, is_hole: bool, x: i32, y: i32) -> Vec<[i32; 2]> {
        let start_x = x - if is_hole { 1 } else { 0 };
        let start_y = y;
        let origin_x = start_x + self.offset_x;
        let origin_y = start_y + self.offset_y;
        self.fetch_contour_ex(
            [start_x as usize, start_y as usize],
            is_hole,
            false,
            [origin_x, origin_y],
            MASK8_NEW,
        )
    }

    fn fetch_contour_ex(
        &mut self,
        start: [usize; 2],
        is_hole: bool,
        is_direct: bool,
        pt: [i32; 2],
        nbd: i32,
    ) -> Vec<[i32; 2]> {
        let start_x = start[0];
        let start_y = start[1];
        let mut pt_x = pt[0];
        let mut pt_y = pt[1];
        let step = self.width;
        let i0 = self.idx(start_x, start_y) as isize;
        let mut points = Vec::<[i32; 2]>::new();

        let mut s_end: i32 = if is_hole { 0 } else { 4 };
        let mut s = s_end;
        let i1: isize;
        loop {
            s = (s - 1) & 7;
            let ni = i0 + chain_delta_index(s, step);
            if self.image[ni as usize] != 0 || s == s_end {
                i1 = ni;
                break;
            }
        }

        if s == s_end {
            self.image[i0 as usize] = (nbd | MASK8_RIGHT) as i8;
            points.push([pt_x, pt_y]);
            return points;
        }

        let mut i3 = i0;
        let mut prev_s = s ^ 4;
        loop {
            s_end = s;
            s = s.min(15);
            let i4: isize;
            loop {
                if s >= 15 {
                    i4 = i3 + chain_delta_index(s, step);
                    break;
                }
                s += 1;
                let ni = i3 + chain_delta_index(s, step);
                if self.image[ni as usize] != 0 {
                    i4 = ni;
                    break;
                }
            }
            s &= 7;

            if ((s - 1) as u32) < (s_end as u32) {
                self.image[i3 as usize] = (nbd | MASK8_RIGHT) as i8;
            } else if self.image[i3 as usize] as i32 == MASK8_BLACK {
                self.image[i3 as usize] = nbd as i8;
            }

            if s != prev_s || is_direct {
                points.push([pt_x, pt_y]);
            }

            prev_s = s;
            let d = CHAIN_DELTAS[s as usize];
            pt_x += d[0];
            pt_y += d[1];

            if i4 == i0 && i3 == i1 {
                break;
            }

            i3 = i4;
            s = (s + 4) & 7;
        }

        points
    }
}

fn dilate_mask_2x2(mask: &[u8], width: usize, height: usize) -> Vec<u8> {
    if width == 0 || height == 0 {
        return Vec::new();
    }

    let mut out = vec![0_u8; width * height];
    #[cfg(target_arch = "x86_64")]
    let use_avx2 = std::arch::is_x86_feature_detected!("avx2");
    out.par_chunks_mut(width).enumerate().for_each(|(y, row)| {
        let row_start = y * width;
        let cur = &mask[row_start..row_start + width];
        let prev = if y == 0 {
            None
        } else {
            Some(&mask[row_start - width..row_start])
        };

        #[cfg(target_arch = "x86_64")]
        if use_avx2 {
            // Safety: AVX2 path is guarded by runtime feature detection and slice bounds.
            unsafe {
                dilate_row_2x2_avx2(cur, prev, row);
            }
            return;
        }

        dilate_row_2x2_scalar(cur, prev, row);
    });

    out
}

#[inline]
fn dilate_row_2x2_scalar(cur: &[u8], prev: Option<&[u8]>, out: &mut [u8]) {
    let width = cur.len();
    if width == 0 {
        return;
    }

    let prev_row = prev.unwrap_or(&[]);
    out[0] = if let Some(p) = prev {
        cur[0] | p[0]
    } else {
        cur[0]
    };

    for x in 1..width {
        let top = if prev_row.is_empty() { 0 } else { prev_row[x] };
        let top_left = if prev_row.is_empty() {
            0
        } else {
            prev_row[x - 1]
        };
        out[x] = cur[x] | cur[x - 1] | top | top_left;
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn dilate_row_2x2_avx2(cur: &[u8], prev: Option<&[u8]>, out: &mut [u8]) {
    use std::arch::x86_64::{
        __m256i, _mm256_loadu_si256, _mm256_or_si256, _mm256_setzero_si256, _mm256_storeu_si256,
    };

    let width = cur.len();
    if width == 0 {
        return;
    }

    let cur_ptr = cur.as_ptr();
    let out_ptr = out.as_mut_ptr();
    let prev_ptr = prev.map_or(std::ptr::null(), |p| p.as_ptr());

    // Safety: x=0 is in-bounds for non-empty rows.
    unsafe {
        *out_ptr = if prev_ptr.is_null() {
            *cur_ptr
        } else {
            *cur_ptr | *prev_ptr
        };
    }

    if width == 1 {
        return;
    }

    let mut x = 1usize;
    let simd_end = width.saturating_sub(31);
    let zero = _mm256_setzero_si256();
    while x < simd_end {
        // Safety:
        // - x >= 1, so x-1 access is valid.
        // - x + 31 < width by loop bound, so 32-byte loads/stores are in-bounds.
        unsafe {
            let cur_v = _mm256_loadu_si256(cur_ptr.add(x) as *const __m256i);
            let cur_l_v = _mm256_loadu_si256(cur_ptr.add(x - 1) as *const __m256i);
            let prev_v = if prev_ptr.is_null() {
                zero
            } else {
                _mm256_loadu_si256(prev_ptr.add(x) as *const __m256i)
            };
            let prev_l_v = if prev_ptr.is_null() {
                zero
            } else {
                _mm256_loadu_si256(prev_ptr.add(x - 1) as *const __m256i)
            };
            let out_v = _mm256_or_si256(
                _mm256_or_si256(cur_v, cur_l_v),
                _mm256_or_si256(prev_v, prev_l_v),
            );
            _mm256_storeu_si256(out_ptr.add(x) as *mut __m256i, out_v);
        }
        x += 32;
    }

    while x < width {
        // Safety: scalar tail stays within row bounds.
        unsafe {
            let top = if prev_ptr.is_null() {
                0
            } else {
                *prev_ptr.add(x)
            };
            let top_left = if prev_ptr.is_null() {
                0
            } else {
                *prev_ptr.add(x - 1)
            };
            *out_ptr.add(x) = *cur_ptr.add(x) | *cur_ptr.add(x - 1) | top | top_left;
        }
        x += 1;
    }
}

fn order_min_box_points_like_python(points: Quad) -> Quad {
    let mut points = points.to_vec();
    points.sort_by(|a, b| a[0].total_cmp(&b[0]));

    let (index_1, index_4) = if points[1][1] > points[0][1] {
        (0_usize, 1_usize)
    } else {
        (1_usize, 0_usize)
    };

    let (index_2, index_3) = if points[3][1] > points[2][1] {
        (2_usize, 3_usize)
    } else {
        (3_usize, 2_usize)
    };

    [
        points[index_1],
        points[index_2],
        points[index_3],
        points[index_4],
    ]
}

fn mini_box_from_points_pure(points: &[[f32; 2]]) -> Option<(Quad, f32)> {
    if points.len() < 3 {
        return None;
    }
    let rect = min_area_rect_from_points_pure(points)?;
    let raw = rotated_rect_to_points_pure(rect);
    let ordered = order_min_box_points_like_python(raw);
    let sside = rect.size[0].min(rect.size[1]);
    Some((ordered, sside))
}

#[cfg(feature = "opencv-backend")]
fn mini_box_from_rotated_rect_opencv(
    rect: core::RotatedRect,
) -> opencv::Result<Option<(Quad, f32)>> {
    if rect.size.width <= 0.0 || rect.size.height <= 0.0 {
        return Ok(None);
    }

    let mut cv_pts = [Point2f::new(0.0, 0.0); 4];
    rect.points(&mut cv_pts)?;
    let raw = [
        [cv_pts[0].x, cv_pts[0].y],
        [cv_pts[1].x, cv_pts[1].y],
        [cv_pts[2].x, cv_pts[2].y],
        [cv_pts[3].x, cv_pts[3].y],
    ];
    let ordered = order_min_box_points_like_python(raw);
    let sside = rect.size.width.min(rect.size.height);
    Ok(Some((ordered, sside)))
}

#[cfg(feature = "opencv-backend")]
fn mini_box_from_contour_opencv(contour: &Vector<Point>) -> opencv::Result<Option<(Quad, f32)>> {
    if contour.len() < 3 {
        return Ok(None);
    }
    let rect = imgproc::min_area_rect(contour)?;
    mini_box_from_rotated_rect_opencv(rect)
}

#[cfg(feature = "opencv-backend")]
fn mini_box_from_points_opencv(points: &[[f32; 2]]) -> opencv::Result<Option<(Quad, f32)>> {
    if points.len() < 3 {
        return Ok(None);
    }
    let Some(rect) = min_area_rect_from_points_opencv(points)? else {
        return Ok(None);
    };
    mini_box_from_rotated_rect_opencv(rect)
}

#[cfg(test)]
fn box_score_fast_pure(bitmap: ArrayView2<'_, f32>, box_points: &[[f32; 2]]) -> f32 {
    let mut scratch = CandidateScratch::default();
    box_score_fast_pure_with_scratch(bitmap, box_points, &mut scratch)
}

#[cfg(feature = "opencv-backend")]
fn pred_view_to_mat(pred: ArrayView2<'_, f32>) -> Option<Mat> {
    let height = pred.nrows();
    let values: Vec<f32> = if let Some(src) = pred.as_slice_memory_order() {
        src.to_vec()
    } else {
        pred.iter().copied().collect()
    };
    let pred_1d = Mat::from_slice(&values).ok()?;
    let pred_ref = pred_1d.reshape(1, height as i32).ok()?;
    let mut pred_mat = Mat::default();
    pred_ref.copy_to(&mut pred_mat).ok()?;
    Some(pred_mat)
}

fn box_score_fast_pure_with_scratch(
    bitmap: ArrayView2<'_, f32>,
    box_points: &[[f32; 2]],
    scratch: &mut CandidateScratch,
) -> f32 {
    let h = bitmap.nrows() as i32;
    let w = bitmap.ncols() as i32;
    if h <= 0 || w <= 0 || box_points.is_empty() {
        return 0.0;
    }

    let mut xmin_f = f32::INFINITY;
    let mut xmax_f = f32::NEG_INFINITY;
    let mut ymin_f = f32::INFINITY;
    let mut ymax_f = f32::NEG_INFINITY;
    for p in box_points {
        xmin_f = xmin_f.min(p[0]);
        xmax_f = xmax_f.max(p[0]);
        ymin_f = ymin_f.min(p[1]);
        ymax_f = ymax_f.max(p[1]);
    }

    let xmin = xmin_f.floor().clamp(0.0, (w - 1) as f32) as i32;
    let xmax = xmax_f.ceil().clamp(0.0, (w - 1) as f32) as i32;
    let ymin = ymin_f.floor().clamp(0.0, (h - 1) as f32) as i32;
    let ymax = ymax_f.ceil().clamp(0.0, (h - 1) as f32) as i32;

    if xmin > xmax || ymin > ymax {
        return 0.0;
    }

    let local_w = (xmax - xmin + 1) as usize;
    let local_h = (ymax - ymin + 1) as usize;

    scratch.shifted_poly.clear();
    scratch.shifted_poly.reserve(box_points.len());
    for p in box_points {
        // Match cv2.fillPoly behavior: truncate float vertices toward zero.
        let x = (p[0] - xmin as f32) as i32;
        let y = (p[1] - ymin as f32) as i32;
        scratch.shifted_poly.push([x as f32, y as f32]);
    }

    let mask_len = local_w * local_h;
    if scratch.mask.len() < mask_len {
        scratch.mask.resize(mask_len, 0);
    }
    let mask = &mut scratch.mask[..mask_len];
    fill_polygon_mask(mask, local_w, local_h, &scratch.shifted_poly);
    masked_mean_in_roi(bitmap, xmin as usize, ymin as usize, local_w, local_h, mask)
}

#[cfg(feature = "opencv-backend")]
fn box_score_fast_opencv(bitmap: &Mat, box_points: &[[f32; 2]]) -> opencv::Result<f32> {
    if box_points.is_empty() || bitmap.rows() <= 0 || bitmap.cols() <= 0 {
        return Ok(0.0);
    }

    let h = bitmap.rows();
    let w = bitmap.cols();

    let mut xmin_f = f32::INFINITY;
    let mut xmax_f = f32::NEG_INFINITY;
    let mut ymin_f = f32::INFINITY;
    let mut ymax_f = f32::NEG_INFINITY;
    for p in box_points {
        xmin_f = xmin_f.min(p[0]);
        xmax_f = xmax_f.max(p[0]);
        ymin_f = ymin_f.min(p[1]);
        ymax_f = ymax_f.max(p[1]);
    }

    let xmin = xmin_f.floor().clamp(0.0, (w - 1) as f32) as i32;
    let xmax = xmax_f.ceil().clamp(0.0, (w - 1) as f32) as i32;
    let ymin = ymin_f.floor().clamp(0.0, (h - 1) as f32) as i32;
    let ymax = ymax_f.ceil().clamp(0.0, (h - 1) as f32) as i32;

    if xmin > xmax || ymin > ymax {
        return Ok(0.0);
    }

    let roi = Mat::roi(
        bitmap,
        core::Rect::new(xmin, ymin, xmax - xmin + 1, ymax - ymin + 1),
    )?;
    let mut mask = Mat::zeros(ymax - ymin + 1, xmax - xmin + 1, core::CV_8UC1)?.to_mat()?;

    let mut poly = Vector::<Point>::new();
    for p in box_points {
        poly.push(Point::new(
            (p[0] - xmin as f32) as i32,
            (p[1] - ymin as f32) as i32,
        ));
    }
    let mut polys = Vector::<Vector<Point>>::new();
    polys.push(poly);

    imgproc::fill_poly(
        &mut mask,
        &polys,
        Scalar::all(1.0),
        imgproc::LINE_8,
        0,
        Point::new(0, 0),
    )?;

    let mean = core::mean(&roi, &mask)?;
    Ok(mean[0] as f32)
}

fn unclip_polygon_pyclipper_into(in_poly: &[[f32; 2]], unclip_ratio: f32, out: &mut Vec<[f32; 2]>) {
    out.clear();
    if in_poly.len() < 3 {
        return;
    }

    // Match Python/shapely numeric behavior (double precision) when computing
    // offset distance for pyclipper.
    let area = polygon_area_f64(in_poly).abs();
    let length = polygon_perimeter_f64(in_poly);
    if !area.is_finite() || !length.is_finite() || length <= 1e-6 {
        return;
    }

    let distance = area * f64::from(unclip_ratio) / length;
    if !distance.is_finite() {
        return;
    }

    // Pyclipper consumes integer coordinates. Float points are truncated toward zero.
    let mut ring: Vec<Coord<i64>> = in_poly
        .iter()
        .map(|p| Coord {
            x: (p[0] as f64).trunc() as i64,
            y: (p[1] as f64).trunc() as i64,
        })
        .collect();
    if ring.len() < 3 {
        return;
    }

    // geo_clipper expects closed rings.
    if ring.first() != ring.last() {
        ring.push(*ring.first().unwrap_or(&ring[0]));
    }

    let poly = Polygon::new(LineString::from(ring), vec![]);
    let expanded = poly.offset(
        distance,
        ClipperJoinType::Round(0.25),
        ClipperEndType::ClosedPolygon,
    );

    // Match RapidOCR Python behavior:
    // np.array(offset.Execute(distance)).reshape((-1, 1, 2))
    // which flattens all returned paths into one contour point list.
    for polygon in expanded.0 {
        let ext = polygon.exterior();
        let ext_len = ext.0.len();
        let ext_end = if ext_len > 1 && ext.0.first() == ext.0.last() {
            ext_len - 1
        } else {
            ext_len
        };
        for c in ext.0.iter().take(ext_end) {
            out.push([c.x as f32, c.y as f32]);
        }

        for hole in polygon.interiors() {
            let h_len = hole.0.len();
            let h_end = if h_len > 1 && hole.0.first() == hole.0.last() {
                h_len - 1
            } else {
                h_len
            };
            for c in hole.0.iter().take(h_end) {
                out.push([c.x as f32, c.y as f32]);
            }
        }
    }
}

fn scale_box_to_dest(
    box_points: &mut Quad,
    bitmap_w: usize,
    bitmap_h: usize,
    dest_w: usize,
    dest_h: usize,
) {
    if bitmap_w == 0 || bitmap_h == 0 {
        return;
    }

    let bw = bitmap_w as f32;
    let bh = bitmap_h as f32;
    let dw = dest_w as f32;
    let dh = dest_h as f32;

    for p in box_points {
        p[0] = (p[0] / bw * dw).round_ties_even().clamp(0.0, dw);
        p[1] = (p[1] / bh * dh).round_ties_even().clamp(0.0, dh);
    }
}

#[cfg(test)]
fn contour_score_pure(bitmap: ArrayView2<'_, f32>, contour: &[[i32; 2]]) -> f32 {
    let mut scratch = CandidateScratch::default();
    contour_score_pure_with_scratch(bitmap, contour, &mut scratch)
}

fn contour_score_pure_with_scratch(
    bitmap: ArrayView2<'_, f32>,
    contour: &[[i32; 2]],
    scratch: &mut CandidateScratch,
) -> f32 {
    if contour.len() < 3 {
        return 0.0;
    }

    let h = bitmap.nrows() as i32;
    let w = bitmap.ncols() as i32;
    if h <= 0 || w <= 0 {
        return 0.0;
    }

    let mut min_x = i32::MAX;
    let mut max_x = i32::MIN;
    let mut min_y = i32::MAX;
    let mut max_y = i32::MIN;

    for p in contour {
        min_x = min_x.min(p[0]);
        max_x = max_x.max(p[0]);
        min_y = min_y.min(p[1]);
        max_y = max_y.max(p[1]);
    }

    let xmin = min_x.max(0);
    let xmax = (max_x + 1).min(w - 1);
    let ymin = min_y.max(0);
    let ymax = (max_y + 1).min(h - 1);

    if xmin > xmax || ymin > ymax {
        return 0.0;
    }

    let local_w = (xmax - xmin + 1) as usize;
    let local_h = (ymax - ymin + 1) as usize;

    scratch.shifted_poly.clear();
    scratch.shifted_poly.reserve(contour.len());
    for p in contour {
        scratch
            .shifted_poly
            .push([(p[0] - xmin) as f32, (p[1] - ymin) as f32]);
    }

    let mask_len = local_w * local_h;
    if scratch.mask.len() < mask_len {
        scratch.mask.resize(mask_len, 0);
    }
    let mask = &mut scratch.mask[..mask_len];
    fill_polygon_mask(mask, local_w, local_h, &scratch.shifted_poly);
    masked_mean_in_roi(bitmap, xmin as usize, ymin as usize, local_w, local_h, mask)
}

fn masked_mean_in_roi(
    bitmap: ArrayView2<'_, f32>,
    xmin: usize,
    ymin: usize,
    local_w: usize,
    local_h: usize,
    mask: &[u8],
) -> f32 {
    debug_assert_eq!(mask.len(), local_w * local_h);
    if local_w == 0 || local_h == 0 || mask.is_empty() {
        return 0.0;
    }

    if let Some(src) = bitmap.as_slice_memory_order() {
        return masked_mean_in_roi_contiguous(
            src,
            bitmap.ncols(),
            xmin,
            ymin,
            local_w,
            local_h,
            mask,
        );
    }

    let mut sum = 0.0_f64;
    let mut count = 0_usize;
    for y in 0..local_h {
        let src_y = ymin + y;
        let row_off = y * local_w;
        for x in 0..local_w {
            if mask[row_off + x] == 0 {
                continue;
            }
            sum += f64::from(bitmap[[src_y, xmin + x]]);
            count += 1;
        }
    }

    if count == 0 {
        0.0
    } else {
        (sum / count as f64) as f32
    }
}

fn masked_mean_in_roi_contiguous(
    bitmap: &[f32],
    bitmap_w: usize,
    xmin: usize,
    ymin: usize,
    local_w: usize,
    local_h: usize,
    mask: &[u8],
) -> f32 {
    let mut sum = 0.0_f64;
    let mut count = 0_usize;
    for y in 0..local_h {
        let src_row_off = (ymin + y) * bitmap_w + xmin;
        let mask_row_off = y * local_w;
        // Safety:
        // - `src_row_off + local_w <= bitmap.len()` because ROI was clamped to bitmap bounds.
        // - `mask_row_off + local_w <= mask.len()` by construction.
        unsafe {
            let src_ptr = bitmap.as_ptr().add(src_row_off);
            let mask_ptr = mask.as_ptr().add(mask_row_off);
            let mut x = 0usize;
            while x < local_w {
                while x < local_w && *mask_ptr.add(x) == 0 {
                    x += 1;
                }
                if x >= local_w {
                    break;
                }
                let run_start = x;
                while x < local_w && *mask_ptr.add(x) != 0 {
                    x += 1;
                }
                let run_len = x - run_start;
                sum += sum_f32_slice(src_ptr.add(run_start), run_len);
                count += run_len;
            }
        }
    }

    if count == 0 {
        0.0
    } else {
        (sum / count as f64) as f32
    }
}

#[inline]
unsafe fn sum_f32_slice(ptr: *const f32, len: usize) -> f64 {
    #[cfg(target_arch = "x86_64")]
    if std::arch::is_x86_feature_detected!("avx2") {
        return unsafe { sum_f32_slice_avx2(ptr, len) };
    }

    let mut sum = 0.0_f64;
    let mut i = 0usize;
    while i < len {
        unsafe {
            sum += f64::from(*ptr.add(i));
        }
        i += 1;
    }
    sum
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn sum_f32_slice_avx2(ptr: *const f32, len: usize) -> f64 {
    use std::arch::x86_64::{_mm256_add_ps, _mm256_loadu_ps, _mm256_setzero_ps, _mm256_storeu_ps};

    let mut i = 0usize;
    let simd_len = len / 8 * 8;
    let mut acc = _mm256_setzero_ps();
    while i < simd_len {
        let v = unsafe { _mm256_loadu_ps(ptr.add(i)) };
        acc = _mm256_add_ps(acc, v);
        i += 8;
    }

    let mut lanes = [0.0_f32; 8];
    unsafe {
        _mm256_storeu_ps(lanes.as_mut_ptr(), acc);
    }
    let mut sum = lanes.iter().map(|v| f64::from(*v)).sum::<f64>();

    while i < len {
        unsafe {
            sum += f64::from(*ptr.add(i));
        }
        i += 1;
    }

    sum
}

#[cfg(feature = "opencv-backend")]
fn find_contours_from_mask_opencv(
    mask: &[u8],
    height: usize,
) -> opencv::Result<Vector<Vector<Point>>> {
    let src_1d = Mat::from_slice(mask)?;
    let src = src_1d.reshape(1, height as i32)?;

    let mut work = Mat::default();
    src.copy_to(&mut work)?;

    let mut contours = Vector::<Vector<Point>>::new();
    imgproc::find_contours(
        &work,
        &mut contours,
        imgproc::RETR_LIST,
        imgproc::CHAIN_APPROX_SIMPLE,
        Point::new(0, 0),
    )?;

    Ok(contours)
}

#[cfg(feature = "opencv-backend")]
fn contour_score_opencv(binary_map: &Mat, contour: &Vector<Point>) -> opencv::Result<f32> {
    if contour.len() < 3 {
        return Ok(0.0);
    }

    let rect = imgproc::bounding_rect(contour)?;
    let xmin = rect.x.max(0);
    let xmax = (rect.x + rect.width).min(binary_map.cols() - 1);
    let ymin = rect.y.max(0);
    let ymax = (rect.y + rect.height).min(binary_map.rows() - 1);

    if xmin > xmax || ymin > ymax {
        return Ok(0.0);
    }

    let roi = Mat::roi(
        binary_map,
        core::Rect::new(xmin, ymin, xmax - xmin + 1, ymax - ymin + 1),
    )?;

    let mut mask = Mat::zeros(ymax - ymin + 1, xmax - xmin + 1, core::CV_8UC1)?.to_mat()?;
    let mut shifted = Vector::<Point>::new();
    for p in contour.iter() {
        shifted.push(Point::new(p.x - xmin, p.y - ymin));
    }
    let mut polys = Vector::<Vector<Point>>::new();
    polys.push(shifted);

    imgproc::fill_poly(
        &mut mask,
        &polys,
        Scalar::all(1.0),
        imgproc::LINE_8,
        0,
        Point::new(0, 0),
    )?;

    let mean = core::mean(&roi, &mask)?;
    Ok(mean[0] as f32)
}

#[derive(Clone, Copy, Debug)]
struct PureRotatedRect {
    center: [f32; 2],
    size: [f32; 2], // width, height
    angle: f32,     // degrees in [-90, 0)
}

fn min_area_rect_from_points_pure(points: &[[f32; 2]]) -> Option<PureRotatedRect> {
    if points.is_empty() {
        return None;
    }

    let hull = convex_hull_like_opencv(points, false);
    if hull.is_empty() {
        return None;
    }

    let mut angle = -std::f64::consts::FRAC_PI_2;

    if hull.len() > 2 {
        let (corner, vec1, vec2) = rotating_calipers_min_area_rect(&hull)?;
        let center = [
            corner[0] + (vec1[0] + vec2[0]) * 0.5,
            corner[1] + (vec1[1] + vec2[1]) * 0.5,
        ];

        // OpenCV parity: compute lengths/angle in double precision and cast to float.
        let mut width =
            (((vec2[0] as f64 * vec2[0] as f64) + (vec2[1] as f64 * vec2[1] as f64)).sqrt()) as f32;
        let mut height =
            (((vec1[0] as f64 * vec1[0] as f64) + (vec1[1] as f64 * vec1[1] as f64)).sqrt()) as f32;
        let special_case_vertical = vec1[0] == 0.0 && vec1[1] > 0.0;
        if special_case_vertical {
            std::mem::swap(&mut width, &mut height);
        } else {
            angle = -f64::atan2(vec1[0] as f64, vec1[1] as f64);
        }

        let rect = PureRotatedRect {
            center,
            size: [width, height],
            angle: (angle * 180.0 / std::f64::consts::PI) as f32,
        };
        return Some(rect);
    }

    if hull.len() == 2 {
        let p0 = hull[0];
        let p1 = hull[1];

        let center = [(p0[0] + p1[0]) * 0.5, (p0[1] + p1[1]) * 0.5];
        let dx = p0[0] as f64 - p1[0] as f64;
        let dy = p0[1] as f64 - p1[1] as f64;

        let mut width = 0.0_f32;
        let mut height = (dx * dx + dy * dy).sqrt() as f32;
        if dx == 0.0 {
            std::mem::swap(&mut width, &mut height);
        } else if dy < 0.0 {
            angle = f64::atan2(dy, dx);
            std::mem::swap(&mut width, &mut height);
        } else if dy > 0.0 {
            angle = -f64::atan2(dx, dy);
        }

        return Some(PureRotatedRect {
            center,
            size: [width, height],
            angle: (angle * 180.0 / std::f64::consts::PI) as f32,
        });
    }

    Some(PureRotatedRect {
        center: hull[0],
        size: [0.0, 0.0],
        angle: (angle * 180.0 / std::f64::consts::PI) as f32,
    })
}

fn rotating_calipers_min_area_rect(points: &[[f32; 2]]) -> Option<([f32; 2], [f32; 2], [f32; 2])> {
    let n = points.len();
    if n < 3 {
        return None;
    }

    let mut inv_vect_length = vec![0.0_f32; n];
    let mut vect = vec![[0.0_f32; 2]; n];

    let mut left = 0_usize;
    let mut bottom = 0_usize;
    let mut right = 0_usize;
    let mut top = 0_usize;

    let mut left_x = points[0][0];
    let mut right_x = points[0][0];
    let mut top_y = points[0][1];
    let mut bottom_y = points[0][1];

    for i in 0..n {
        let p0 = points[i];
        if p0[0] < left_x {
            left_x = p0[0];
            left = i;
        }
        if p0[0] > right_x {
            right_x = p0[0];
            right = i;
        }
        if p0[1] > top_y {
            top_y = p0[1];
            top = i;
        }
        if p0[1] < bottom_y {
            bottom_y = p0[1];
            bottom = i;
        }

        let p1 = points[(i + 1) % n];
        let dx = p1[0] - p0[0];
        let dy = p1[1] - p0[1];
        vect[i] = [dx, dy];
        let norm = (dx * dx + dy * dy).sqrt();
        if norm <= 1e-12 {
            return None;
        }
        inv_vect_length[i] = 1.0 / norm;
    }

    let mut seq = [bottom, right, top, left];
    let mut min_area = f32::MAX;

    let mut best_left = 0_usize;
    let mut best_bottom = 0_usize;
    let mut best_a1 = 0.0_f32;
    let mut best_b1 = 0.0_f32;
    let mut best_width = 0.0_f32;
    let mut best_height = 0.0_f32;
    let mut found = false;

    for _ in 0..n {
        let rot_vect = [
            vect[seq[0]],
            rotate90_cw(vect[seq[1]]),
            rotate180(vect[seq[2]]),
            rotate90_ccw(vect[seq[3]]),
        ];

        let mut main_element = 0_usize;
        for i in 1..4 {
            if first_vec_is_right(rot_vect[i], rot_vect[main_element]) {
                main_element = i;
            }
        }

        let pindex = seq[main_element];
        let lead_x = vect[pindex][0] * inv_vect_length[pindex];
        let lead_y = vect[pindex][1] * inv_vect_length[pindex];
        let (base_a, base_b) = match main_element {
            0 => (lead_x, lead_y),
            1 => (lead_y, -lead_x),
            2 => (-lead_x, -lead_y),
            3 => (-lead_y, lead_x),
            _ => return None,
        };

        seq[main_element] = (seq[main_element] + 1) % n;

        let mut dx = points[seq[1]][0] - points[seq[3]][0];
        let mut dy = points[seq[1]][1] - points[seq[3]][1];
        let width = dx * base_a + dy * base_b;

        dx = points[seq[2]][0] - points[seq[0]][0];
        dy = points[seq[2]][1] - points[seq[0]][1];
        let height = -dx * base_b + dy * base_a;

        let area = width * height;
        if area <= min_area {
            min_area = area;
            best_left = seq[3];
            best_bottom = seq[0];
            best_a1 = base_a;
            best_b1 = base_b;
            best_width = width;
            best_height = height;
            found = true;
        }
    }

    if !found {
        return None;
    }

    let a1 = best_a1;
    let b1 = best_b1;
    let a2 = -best_b1;
    let b2 = best_a1;

    let c1 = a1 * points[best_left][0] + points[best_left][1] * b1;
    let c2 = a2 * points[best_bottom][0] + points[best_bottom][1] * b2;

    let det = a1 * b2 - a2 * b1;
    if det.abs() <= 1e-12 {
        return None;
    }

    let px = (c1 * b2 - c2 * b1) / det;
    let py = (a1 * c2 - a2 * c1) / det;

    Some((
        [px, py],
        [a1 * best_width, b1 * best_width],
        [a2 * best_height, b2 * best_height],
    ))
}

fn rotate90_ccw(v: [f32; 2]) -> [f32; 2] {
    [-v[1], v[0]]
}

fn rotate90_cw(v: [f32; 2]) -> [f32; 2] {
    [v[1], -v[0]]
}

fn rotate180(v: [f32; 2]) -> [f32; 2] {
    [-v[0], -v[1]]
}

fn first_vec_is_right(vec1: [f32; 2], vec2: [f32; 2]) -> bool {
    let tmp = rotate90_cw(vec1);
    tmp[0] * vec2[0] + tmp[1] * vec2[1] < 0.0
}

fn rotated_rect_to_points_pure(rect: PureRotatedRect) -> Quad {
    // OpenCV RotatedRect::points uses double trigonometry then casts to float.
    let angle = f64::from(rect.angle) * std::f64::consts::PI / 180.0;
    let b = (angle.cos() as f32) * 0.5;
    let a = (angle.sin() as f32) * 0.5;

    let ah = a * rect.size[1];
    let aw = a * rect.size[0];
    let bh = b * rect.size[1];
    let bw = b * rect.size[0];

    [
        [rect.center[0] - ah - bw, rect.center[1] + bh - aw],
        [rect.center[0] + ah - bw, rect.center[1] - bh - aw],
        [rect.center[0] + ah + bw, rect.center[1] - bh + aw],
        [rect.center[0] - ah + bw, rect.center[1] + bh + aw],
    ]
}

#[inline]
fn cv_sign(v: f64) -> i32 {
    if v > 0.0 {
        1
    } else if v < 0.0 {
        -1
    } else {
        0
    }
}

#[inline]
fn normalize_vec2(v: [f32; 2]) -> [f32; 2] {
    let n = ((v[0] as f64 * v[0] as f64) + (v[1] as f64 * v[1] as f64)).sqrt();
    if n == 0.0 {
        [0.0, 0.0]
    } else {
        [(v[0] as f64 / n) as f32, (v[1] as f64 / n) as f32]
    }
}

fn sklansky_like_opencv(
    points: &[[f32; 2]],
    pointer: &[usize],
    start: i32,
    end: i32,
    stack: &mut [i32],
    nsign: i32,
    sign2: i32,
) -> usize {
    let incr = if end > start { 1 } else { -1 };
    let mut pprev = start;
    let mut pcur = pprev + incr;
    let mut pnext = pcur + incr;
    let mut stacksize = 3_usize;

    let p_start = points[pointer[start as usize]];
    let p_end = points[pointer[end as usize]];
    if start == end || (p_start[0] == p_end[0] && p_start[1] == p_end[1]) {
        stack[0] = start;
        return 1;
    }

    stack[0] = pprev;
    stack[1] = pcur;
    stack[2] = pnext;

    let end_after = end + incr;
    while pnext != end_after {
        let cury = points[pointer[pcur as usize]][1];
        let nexty = points[pointer[pnext as usize]][1];
        let by = nexty - cury;

        if cv_sign(by as f64) != nsign {
            let pcur_pt = points[pointer[pcur as usize]];
            let pprev_pt = points[pointer[pprev as usize]];
            let pnext_pt = points[pointer[pnext as usize]];

            let mut a = [pcur_pt[0] - pprev_pt[0], pcur_pt[1] - pprev_pt[1]];
            let mut b = [pnext_pt[0] - pcur_pt[0], by];
            a = normalize_vec2(a);
            b = normalize_vec2(b);

            let convexity = (a[1] as f64 * b[0] as f64) - (a[0] as f64 * b[1] as f64);
            if cv_sign(convexity) == sign2 && (a[0] != 0.0 || a[1] != 0.0) {
                pprev = pcur;
                pcur = pnext;
                pnext += incr;
                stack[stacksize] = pnext;
                stacksize += 1;
            } else if pprev == start {
                pcur = pnext;
                stack[1] = pcur;
                pnext += incr;
                stack[2] = pnext;
            } else {
                stack[stacksize - 2] = pnext;
                pcur = pprev;
                pprev = stack[stacksize - 4];
                stacksize -= 1;
            }
        } else {
            pnext += incr;
            stack[stacksize - 1] = pnext;
        }
    }

    stacksize - 1
}

fn convex_hull_like_opencv(points: &[[f32; 2]], clockwise: bool) -> Vec<[f32; 2]> {
    let total = points.len();
    if total == 0 {
        return Vec::new();
    }

    let mut pointer: Vec<usize> = (0..total).collect();
    pointer.sort_by(|&a, &b| {
        points[a][0]
            .total_cmp(&points[b][0])
            .then_with(|| points[a][1].total_cmp(&points[b][1]))
            .then_with(|| a.cmp(&b))
    });

    let mut miny_ind = 0usize;
    let mut maxy_ind = 0usize;
    for i in 1..total {
        let y = points[pointer[i]][1];
        if points[pointer[miny_ind]][1] > y {
            miny_ind = i;
        }
        if points[pointer[maxy_ind]][1] < y {
            maxy_ind = i;
        }
    }

    let mut hullbuf: Vec<i32> = Vec::with_capacity(total);
    let p0 = points[pointer[0]];
    let p_last = points[pointer[total - 1]];
    if p0[0] == p_last[0] && p0[1] == p_last[1] {
        hullbuf.push(0);
    } else {
        let mut tl_buf = vec![0_i32; total + 2];
        let tl_count =
            sklansky_like_opencv(points, &pointer, 0, maxy_ind as i32, &mut tl_buf, -1, 1);
        let mut tr_buf = vec![0_i32; total + 2];
        let tr_count = sklansky_like_opencv(
            points,
            &pointer,
            total as i32 - 1,
            maxy_ind as i32,
            &mut tr_buf,
            -1,
            -1,
        );

        let mut tl_stack = tl_buf[..tl_count].to_vec();
        let mut tr_stack = tr_buf[..tr_count].to_vec();
        if !clockwise {
            std::mem::swap(&mut tl_stack, &mut tr_stack);
        }

        if tl_stack.len() >= 2 {
            for &idx in tl_stack.iter().take(tl_stack.len() - 1) {
                hullbuf.push(idx);
            }
        }
        if tr_stack.len() >= 2 {
            for i in (1..tr_stack.len()).rev() {
                hullbuf.push(tr_stack[i]);
            }
        }
        let stop_idx = if tr_stack.len() > 2 {
            tr_stack[1]
        } else if tl_stack.len() > 2 {
            tl_stack[tl_stack.len() - 2]
        } else {
            -1
        };

        let mut bl_buf = vec![0_i32; total + 2];
        let bl_count =
            sklansky_like_opencv(points, &pointer, 0, miny_ind as i32, &mut bl_buf, 1, -1);
        let mut br_buf = vec![0_i32; total + 2];
        let br_count = sklansky_like_opencv(
            points,
            &pointer,
            total as i32 - 1,
            miny_ind as i32,
            &mut br_buf,
            1,
            1,
        );
        let mut bl_stack = bl_buf[..bl_count].to_vec();
        let mut br_stack = br_buf[..br_count].to_vec();
        if clockwise {
            std::mem::swap(&mut bl_stack, &mut br_stack);
        }

        let mut bl_emit = bl_stack.len();
        let mut br_emit = br_stack.len();
        if stop_idx >= 0 {
            let check_idx = if bl_stack.len() > 2 {
                bl_stack[1]
            } else if bl_stack.len() + br_stack.len() > 2 {
                br_stack[2 - bl_stack.len()]
            } else {
                -1
            };
            if check_idx == stop_idx
                || (check_idx >= 0
                    && stop_idx >= 0
                    && points[pointer[check_idx as usize]][0]
                        == points[pointer[stop_idx as usize]][0]
                    && points[pointer[check_idx as usize]][1]
                        == points[pointer[stop_idx as usize]][1])
            {
                bl_emit = bl_emit.min(2);
                br_emit = br_emit.min(2);
            }
        }

        if bl_emit >= 2 {
            for &idx in bl_stack.iter().take(bl_emit - 1) {
                hullbuf.push(idx);
            }
        }
        if br_emit >= 2 {
            for i in (1..br_emit).rev() {
                hullbuf.push(br_stack[i]);
            }
        }

        for idx in &mut hullbuf {
            *idx = pointer[*idx as usize] as i32;
        }

        let nout = hullbuf.len();
        if nout >= 3 {
            let mut min_idx = 0usize;
            let mut max_idx = 0usize;
            let mut lt = 0_i32;
            for i in 1..nout {
                let idx = hullbuf[i];
                lt += i32::from(hullbuf[i - 1] < idx);
                if lt > 1 && lt <= i as i32 - 2 {
                    break;
                }
                if idx < hullbuf[min_idx] {
                    min_idx = i;
                }
                if idx > hullbuf[max_idx] {
                    max_idx = i;
                }
            }

            let mmdist = (max_idx as i32 - min_idx as i32).unsigned_abs() as usize;
            if (mmdist == 1 || mmdist == nout - 1)
                && (lt <= 1 || lt >= nout.saturating_sub(2) as i32)
            {
                let ascending = (max_idx + 1) % nout == min_idx;
                let i0 = if ascending { min_idx } else { max_idx };
                if i0 > 0 {
                    let mut rotated = vec![0_i32; nout];
                    let mut j = i0;
                    let mut i = 0usize;
                    while i < nout {
                        let curr_idx = hullbuf[j];
                        rotated[i] = curr_idx;
                        let next_j = if j + 1 < nout { j + 1 } else { 0 };
                        let next_idx = hullbuf[next_j];
                        if i < nout - 1 && (ascending != (curr_idx < next_idx)) {
                            break;
                        }
                        j = next_j;
                        i += 1;
                    }
                    if i == nout {
                        hullbuf.copy_from_slice(&rotated);
                    }
                }
            }
        }
    }

    hullbuf
        .into_iter()
        .filter_map(|idx| points.get(idx as usize).copied())
        .collect()
}

#[cfg(test)]
fn unclip_polygon_like_opencv_db(in_poly: &[[f32; 2]], unclip_ratio: f32) -> Vec<[f32; 2]> {
    if in_poly.len() < 3 {
        return Vec::new();
    }

    let area = polygon_area(in_poly).abs();
    let length = polygon_perimeter(in_poly);
    if length <= 1e-6 {
        return Vec::new();
    }

    let distance = area * unclip_ratio / length;

    let n = in_poly.len();
    let mut new_lines: Vec<[[f32; 2]; 2]> = Vec::with_capacity(n);
    for i in 0..n {
        let pt1 = in_poly[i];
        let pt2 = in_poly[(i + n - 1) % n];

        let vec = [pt1[0] - pt2[0], pt1[1] - pt2[1]];
        let vec_norm = (vec[0] * vec[0] + vec[1] * vec[1]).sqrt();
        if vec_norm <= 1e-6 {
            return Vec::new();
        }

        let unclip_dis = distance / vec_norm;
        let rotate_vec = [vec[1] * unclip_dis, -vec[0] * unclip_dis];

        new_lines.push([
            [pt1[0] + rotate_vec[0], pt1[1] + rotate_vec[1]],
            [pt2[0] + rotate_vec[0], pt2[1] + rotate_vec[1]],
        ]);
    }

    let mut out = Vec::with_capacity(n);
    for i in 0..n {
        let a = new_lines[i][0];
        let b = new_lines[i][1];
        let c = new_lines[(i + 1) % n][0];
        let d = new_lines[(i + 1) % n][1];

        let v1 = [b[0] - a[0], b[1] - a[1]];
        let v2 = [d[0] - c[0], d[1] - c[1]];
        let v1n = (v1[0] * v1[0] + v1[1] * v1[1]).sqrt();
        let v2n = (v2[0] * v2[0] + v2[1] * v2[1]).sqrt();

        if v1n <= 1e-6 || v2n <= 1e-6 {
            out.push([(b[0] + c[0]) * 0.5, (b[1] + c[1]) * 0.5]);
            continue;
        }

        let cos_angle = (v1[0] * v2[0] + v1[1] * v2[1]) / (v1n * v2n);
        if cos_angle.abs() > 0.7 {
            out.push([(b[0] + c[0]) * 0.5, (b[1] + c[1]) * 0.5]);
            continue;
        }

        let denom = a[0] * (d[1] - c[1])
            + b[0] * (c[1] - d[1])
            + d[0] * (b[1] - a[1])
            + c[0] * (a[1] - b[1]);
        if denom.abs() <= 1e-6 {
            out.push([(b[0] + c[0]) * 0.5, (b[1] + c[1]) * 0.5]);
            continue;
        }

        let num = a[0] * (d[1] - c[1]) + c[0] * (a[1] - d[1]) + d[0] * (c[1] - a[1]);
        let s = num / denom;

        out.push([a[0] + s * (b[0] - a[0]), a[1] + s * (b[1] - a[1])]);
    }

    out
}

#[cfg(test)]
fn polygon_area(poly: &[[f32; 2]]) -> f32 {
    if poly.len() < 3 {
        return 0.0;
    }

    let mut sum = 0.0_f32;
    for i in 0..poly.len() {
        let p0 = poly[i];
        let p1 = poly[(i + 1) % poly.len()];
        sum += p0[0] * p1[1] - p1[0] * p0[1];
    }
    0.5 * sum
}

fn polygon_area_f64(poly: &[[f32; 2]]) -> f64 {
    if poly.len() < 3 {
        return 0.0;
    }

    let mut sum = 0.0_f64;
    for i in 0..poly.len() {
        let p0 = poly[i];
        let p1 = poly[(i + 1) % poly.len()];
        sum += f64::from(p0[0]) * f64::from(p1[1]) - f64::from(p1[0]) * f64::from(p0[1]);
    }
    0.5 * sum
}

#[cfg(test)]
fn polygon_perimeter(poly: &[[f32; 2]]) -> f32 {
    if poly.is_empty() {
        return 0.0;
    }

    let mut length = 0.0_f32;
    for i in 0..poly.len() {
        length += l2(poly[i], poly[(i + 1) % poly.len()]);
    }
    length
}

fn polygon_perimeter_f64(poly: &[[f32; 2]]) -> f64 {
    if poly.is_empty() {
        return 0.0;
    }

    let mut length = 0.0_f64;
    for i in 0..poly.len() {
        let p0 = poly[i];
        let p1 = poly[(i + 1) % poly.len()];
        let dx = f64::from(p0[0] - p1[0]);
        let dy = f64::from(p0[1] - p1[1]);
        length += (dx * dx + dy * dy).sqrt();
    }
    length
}

fn fill_polygon_mask(mask: &mut [u8], width: usize, height: usize, poly: &[[f32; 2]]) {
    if poly.len() < 3 || width == 0 || height == 0 || mask.is_empty() {
        return;
    }
    if mask.len() != width * height {
        return;
    }
    mask.fill(0);

    // Match numpy astype(np.int32) used before cv2.fillPoly: truncate toward zero.
    let mut vertices = Vec::with_capacity(poly.len());
    let mut min_y = i32::MAX;
    let mut max_y = i32::MIN;
    for p in poly {
        let x = p[0] as i32;
        let y = p[1] as i32;
        min_y = min_y.min(y);
        max_y = max_y.max(y);
        vertices.push([x, y]);
    }
    if vertices.len() < 3 {
        return;
    }

    let y_start = min_y.max(0).min(height as i32 - 1);
    let y_end = max_y.max(0).min(height as i32 - 1);
    if y_start > y_end {
        return;
    }

    let mut intersections = Vec::<f32>::with_capacity(vertices.len());
    for y in y_start..=y_end {
        intersections.clear();

        let mut prev = vertices[vertices.len() - 1];
        for &curr in &vertices {
            let (x0, y0) = (prev[0], prev[1]);
            let (x1, y1) = (curr[0], curr[1]);

            // Even-odd scanline rule with half-open edges to avoid double counting.
            if (y0 <= y && y < y1) || (y1 <= y && y < y0) {
                let dy = (y1 - y0) as f32;
                if dy.abs() > f32::EPSILON {
                    let t = (y - y0) as f32 / dy;
                    intersections.push(x0 as f32 + (x1 - x0) as f32 * t);
                }
            }
            prev = curr;
        }

        if intersections.len() < 2 {
            continue;
        }
        intersections.sort_by(|a, b| a.total_cmp(b));

        let row_off = y as usize * width;
        let mut i = 0usize;
        while i + 1 < intersections.len() {
            let xs = intersections[i].ceil() as i32;
            let xe = intersections[i + 1].floor() as i32;
            if xs <= xe {
                let x0 = xs.max(0).min(width as i32 - 1) as usize;
                let x1 = xe.max(0).min(width as i32 - 1) as usize;
                if x0 <= x1 {
                    // Safety:
                    // - `row_off + x0` and `row_off + x1` are in-bounds by clamping.
                    // - `write_bytes` writes exactly the in-row span length.
                    unsafe {
                        std::ptr::write_bytes(
                            mask.as_mut_ptr().add(row_off + x0),
                            1_u8,
                            x1 - x0 + 1,
                        );
                    }
                }
            }
            i += 2;
        }
    }
}

fn filter_det_res(
    dt_boxes: Vec<Quad>,
    scores: Vec<f32>,
    img_height: usize,
    img_width: usize,
) -> (Vec<Quad>, Vec<f32>) {
    let mut out_boxes = Vec::with_capacity(dt_boxes.len());
    let mut out_scores = Vec::with_capacity(scores.len());

    for (box_, score) in dt_boxes.into_iter().zip(scores) {
        let mut box_ = order_points_clockwise(box_);
        box_ = clip_det_res(box_, img_height, img_width);

        // Keep parity with Python-style filtering: cast norms to int before thresholding.
        let rect_width = l2(box_[0], box_[1]) as i32;
        let rect_height = l2(box_[0], box_[3]) as i32;
        if rect_width <= 3 || rect_height <= 3 {
            continue;
        }

        out_boxes.push(box_);
        out_scores.push(score);
    }

    (out_boxes, out_scores)
}

fn clip_det_res(mut points: Quad, img_height: usize, img_width: usize) -> Quad {
    let max_x = img_width.saturating_sub(1) as f32;
    let max_y = img_height.saturating_sub(1) as f32;

    for p in &mut points {
        p[0] = p[0].clamp(0.0, max_x).floor();
        p[1] = p[1].clamp(0.0, max_y).floor();
    }

    points
}

fn order_points_clockwise(pts: Quad) -> Quad {
    let mut x_sorted = pts.to_vec();
    x_sorted.sort_by(|a, b| a[0].total_cmp(&b[0]));

    let mut left = [x_sorted[0], x_sorted[1]];
    let mut right = [x_sorted[2], x_sorted[3]];
    left.sort_by(|a, b| a[1].total_cmp(&b[1]));
    right.sort_by(|a, b| a[1].total_cmp(&b[1]));

    let tl = left[0];
    let bl = left[1];
    let tr = right[0];
    let br = right[1];
    [tl, tr, br, bl]
}

fn sort_boxes_like_python(boxes: &mut Vec<Quad>, scores: &mut Vec<f32>, y_threshold: f32) {
    if boxes.is_empty() {
        return;
    }

    let n = boxes.len();
    // Python parity:
    // 1) stable sort by y (top to bottom)
    // 2) line ids via adjacent y difference threshold
    // 3) within each line id, sort by x (left to right)
    let mut y_order: Vec<usize> = (0..n).collect();
    y_order.sort_by(|&a, &b| {
        boxes[a][0][1]
            .total_cmp(&boxes[b][0][1])
            .then_with(|| a.cmp(&b))
    });

    let mut line_ids = vec![0_i32; n];
    for i in 1..n {
        let prev_y = boxes[y_order[i - 1]][0][1];
        let cur_y = boxes[y_order[i]][0][1];
        line_ids[i] = line_ids[i - 1] + if cur_y - prev_y >= y_threshold { 1 } else { 0 };
    }

    let mut final_order_in_y_sorted: Vec<usize> = (0..n).collect();
    final_order_in_y_sorted.sort_by(|&a, &b| {
        line_ids[a]
            .cmp(&line_ids[b])
            .then_with(|| boxes[y_order[a]][0][0].total_cmp(&boxes[y_order[b]][0][0]))
            .then_with(|| a.cmp(&b))
    });

    let mut new_boxes = Vec::with_capacity(n);
    let mut new_scores = Vec::with_capacity(n);
    for idx_in_y_sorted in final_order_in_y_sorted {
        let src_idx = y_order[idx_in_y_sorted];
        new_boxes.push(boxes[src_idx]);
        new_scores.push(scores[src_idx]);
    }

    *boxes = new_boxes;
    *scores = new_scores;
}

#[cfg(feature = "opencv-backend")]
fn min_area_rect_from_points_opencv(
    points: &[[f32; 2]],
) -> opencv::Result<Option<core::RotatedRect>> {
    if points.len() < 3 {
        return Ok(None);
    }

    // Match Python/OpenCV behavior in RapidOCR:
    // pyclipper returns integer vertices, and cv2.minAreaRect receives integer contour points.
    // Using integer points here avoids subtle 1px drifts after scaling/clipping.
    let mut contour = Vector::<Point>::new();
    for p in points {
        contour.push(Point::new(p[0] as i32, p[1] as i32));
    }

    let rect = imgproc::min_area_rect(&contour)?;
    Ok(Some(rect))
}

fn l2(a: [f32; 2], b: [f32; 2]) -> f32 {
    let dx = a[0] - b[0];
    let dy = a[1] - b[1];
    (dx * dx + dy * dy).sqrt()
}

#[cfg(test)]
mod tests {
    use super::{
        DbPostProcess, box_score_fast_pure, build_threshold_bitmap, contour_score_pure,
        dilate_mask_2x2, fill_polygon_mask, masked_mean_in_roi, min_area_rect_from_points_pure,
        sort_boxes_like_python, unclip_polygon_like_opencv_db,
    };
    use crate::config::VisionBackend;
    use ndarray::Array2;

    #[cfg(feature = "opencv-backend")]
    fn lcg_next(state: &mut u64) -> u64 {
        // Numerical Recipes LCG constants are deterministic and sufficient here.
        *state = state.wrapping_mul(1664525).wrapping_add(1013904223);
        *state
    }

    #[cfg(feature = "opencv-backend")]
    #[test]
    fn pure_find_contours_matches_opencv_on_seeded_masks() {
        let mut seed = 0xC0FFEE_u64;
        let mut checked = 0usize;

        for case_id in 0..1800usize {
            let width = 5usize + (lcg_next(&mut seed) % 92) as usize;
            let height = 5usize + (lcg_next(&mut seed) % 76) as usize;
            let mut mask = vec![0_u8; width * height];

            // Blend random noise with a few deterministic runs to hit corner cases.
            for y in 0..height {
                for x in 0..width {
                    let rnd = (lcg_next(&mut seed) >> 24) as u8;
                    let mut v = if rnd & 0b11 == 0 { 255_u8 } else { 0_u8 };
                    if case_id % 7 == 0 && x > width / 3 && x < width * 2 / 3 {
                        v = 255_u8;
                    }
                    if case_id % 11 == 0 && y > height / 3 && y < height * 2 / 3 {
                        v = 255_u8;
                    }
                    if case_id % 13 == 0 && (x + y) % 9 == 0 {
                        v = 255_u8;
                    }
                    if case_id % 17 == 0
                        && x > 1
                        && x + 2 < width
                        && y > 1
                        && y + 2 < height
                        && (x * 3 + y * 5) % 19 == 0
                    {
                        v = 255_u8;
                    }
                    mask[y * width + x] = v;
                }
            }

            // Ensure we cover both background and foreground.
            if !mask.contains(&255) || !mask.contains(&0) {
                continue;
            }

            let pure = super::find_contours_from_mask_pure(mask.clone(), width, height);
            let opencv = super::find_contours_from_mask_opencv(&mask, height)
                .expect("opencv findContours should succeed");
            let mut cv_contours = Vec::<Vec<[i32; 2]>>::with_capacity(opencv.len());
            for contour in opencv.iter() {
                let mut pts = Vec::with_capacity(contour.len());
                for p in contour.iter() {
                    pts.push([p.x, p.y]);
                }
                cv_contours.push(pts);
            }

            let mut pure_sorted = pure;
            let mut cv_sorted = cv_contours;
            pure_sorted.sort();
            cv_sorted.sort();

            assert_eq!(
                pure_sorted, cv_sorted,
                "contour mismatch on case_id={case_id}, size={}x{}",
                width, height
            );
            checked += 1;
        }

        assert!(
            checked >= 900,
            "not enough non-trivial contour cases were exercised"
        );
    }

    #[test]
    fn unclip_polygon_like_opencv_db_expands_square() {
        let square = [
            [10.0_f32, 10.0_f32],
            [10.0_f32, 20.0_f32],
            [20.0_f32, 20.0_f32],
            [20.0_f32, 10.0_f32],
        ];
        let out = unclip_polygon_like_opencv_db(&square, 1.6);
        assert_eq!(out.len(), 4);
        for p in out {
            assert!(p[0].is_finite());
            assert!(p[1].is_finite());
        }
    }

    #[test]
    fn contour_score_pure_non_zero_on_filled_region() {
        let mut pred = Array2::<f32>::zeros((10, 10));
        for y in 2..6 {
            for x in 2..6 {
                pred[[y, x]] = 1.0;
            }
        }

        let contour = vec![[2, 2], [2, 5], [5, 5], [5, 2]];
        let s = contour_score_pure(pred.view(), &contour);
        assert!(s > 0.5);
    }

    #[test]
    fn min_area_rect_from_points_pure_handles_simple_quad() {
        let pts = vec![
            [0.0_f32, 0.0_f32],
            [10.0_f32, 0.0_f32],
            [10.0_f32, 5.0_f32],
            [0.0_f32, 5.0_f32],
        ];
        let rect = min_area_rect_from_points_pure(&pts).expect("rect should exist");
        let max_side = rect.size[0].max(rect.size[1]);
        let min_side = rect.size[0].min(rect.size[1]);
        assert!(max_side >= 9.0);
        assert!(min_side >= 4.0);
    }

    #[test]
    fn pure_backend_detects_synthetic_text_blob() {
        let mut pred = Array2::<f32>::zeros((32, 64));
        for y in 8..24 {
            for x in 10..54 {
                pred[[y, x]] = 0.9;
            }
        }

        let post = DbPostProcess {
            vision_backend: VisionBackend::PureRust,
            thresh: 0.3,
            box_thresh: 0.5,
            ..DbPostProcess::default()
        };

        let (boxes, scores) = post.run(&pred, 640, 320);
        assert!(!boxes.is_empty());
        assert_eq!(boxes.len(), scores.len());
    }

    #[test]
    fn threshold_bitmap_matches_scalar_reference() {
        let data: Vec<f32> = (0..137)
            .map(|i| ((i as f32 * 1.37).sin() * 0.5 + 0.5) * 2.0 - 0.8)
            .collect();
        let pred = Array2::from_shape_vec((1, data.len()), data).expect("shape should match");
        let out = build_threshold_bitmap(pred.view(), 0.3);
        let expected: Vec<u8> = pred
            .iter()
            .map(|v| if *v > 0.3 { 255_u8 } else { 0_u8 })
            .collect();
        assert_eq!(out, expected);
    }

    #[test]
    fn box_score_fast_quad_matches_mask_reference() {
        let mut pred = Array2::<f32>::zeros((27, 39));
        for y in 0..pred.nrows() {
            for x in 0..pred.ncols() {
                pred[[y, x]] = ((x * 7 + y * 13) % 97) as f32 / 97.0;
            }
        }

        let quads = vec![
            vec![[4.2, 3.1], [20.7, 3.0], [21.2, 10.8], [3.8, 11.2]],
            vec![[2.0, 5.0], [14.0, 2.0], [19.0, 13.0], [6.0, 16.0]],
            vec![[0.3, 0.4], [10.8, 0.1], [11.4, 6.9], [0.5, 7.2]],
        ];

        for quad in quads {
            let fast = box_score_fast_pure(pred.view(), &quad);

            let h = pred.nrows() as i32;
            let w = pred.ncols() as i32;
            let mut xmin_f = f32::INFINITY;
            let mut xmax_f = f32::NEG_INFINITY;
            let mut ymin_f = f32::INFINITY;
            let mut ymax_f = f32::NEG_INFINITY;
            for p in &quad {
                xmin_f = xmin_f.min(p[0]);
                xmax_f = xmax_f.max(p[0]);
                ymin_f = ymin_f.min(p[1]);
                ymax_f = ymax_f.max(p[1]);
            }

            let xmin = xmin_f.floor().clamp(0.0, (w - 1) as f32) as i32;
            let xmax = xmax_f.ceil().clamp(0.0, (w - 1) as f32) as i32;
            let ymin = ymin_f.floor().clamp(0.0, (h - 1) as f32) as i32;
            let ymax = ymax_f.ceil().clamp(0.0, (h - 1) as f32) as i32;
            let local_w = (xmax - xmin + 1) as usize;
            let local_h = (ymax - ymin + 1) as usize;

            let mut shifted = Vec::with_capacity(quad.len());
            for p in &quad {
                shifted.push([p[0] - xmin as f32, p[1] - ymin as f32]);
            }
            let mut mask = vec![0_u8; local_w * local_h];
            fill_polygon_mask(&mut mask, local_w, local_h, &shifted);
            let reference = masked_mean_in_roi(
                pred.view(),
                xmin as usize,
                ymin as usize,
                local_w,
                local_h,
                &mask,
            );
            assert!(
                (fast - reference).abs() <= 1e-6,
                "quad score mismatch (mask): fast={fast} ref={reference} quad={quad:?}"
            );
        }
    }

    #[test]
    fn dilate_mask_matches_reference() {
        fn reference(mask: &[u8], width: usize, height: usize) -> Vec<u8> {
            let mut out = vec![0_u8; width * height];
            for y in 0..height {
                for x in 0..width {
                    if mask[y * width + x] == 0 {
                        continue;
                    }
                    for dy in 0..=1 {
                        let ny = y + dy;
                        if ny >= height {
                            continue;
                        }
                        for dx in 0..=1 {
                            let nx = x + dx;
                            if nx >= width {
                                continue;
                            }
                            out[ny * width + nx] = 255;
                        }
                    }
                }
            }
            out
        }

        let width = 23usize;
        let height = 17usize;
        let mut mask = vec![0_u8; width * height];
        for y in 0..height {
            for x in 0..width {
                if (x * 3 + y * 5) % 7 == 0 {
                    mask[y * width + x] = 255;
                }
            }
        }

        let expected = reference(&mask, width, height);
        let out = dilate_mask_2x2(&mask, width, height);
        assert_eq!(out, expected);
    }

    #[test]
    fn sort_boxes_like_python_orders_by_x_within_same_line() {
        let mut boxes = vec![
            [[10.0, 10.0], [20.0, 10.0], [20.0, 20.0], [10.0, 20.0]],
            [[9.5, 11.0], [19.5, 11.0], [19.5, 21.0], [9.5, 21.0]],
            [[100.0, 40.0], [120.0, 40.0], [120.0, 50.0], [100.0, 50.0]],
        ];
        let mut scores = vec![0.9, 0.8, 0.7];

        sort_boxes_like_python(&mut boxes, &mut scores, 10.0);

        // First two boxes are in the same line bucket; Python sorts by x only.
        assert!(boxes[0][0][0] <= boxes[1][0][0]);
        assert_eq!(boxes[2][0][0], 100.0);
        assert_eq!(scores.len(), boxes.len());
    }
}

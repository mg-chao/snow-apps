//! Suzuki contour tracing matching OpenCV RETR_LIST + CHAIN_APPROX_SIMPLE.

use super::canny::canny_edges;
use super::draw::draw_contours;
use crate::geometry::Rect;
use crate::grid::{Image, Mask};

const DELTAS: [(i32, i32); 8] = [
    (1, 0),
    (1, -1),
    (0, -1),
    (-1, -1),
    (-1, 0),
    (-1, 1),
    (0, 1),
    (1, 1),
];

pub(super) fn outlined_controls(input: &Image) -> (Vec<Rect>, Mask) {
    let edges = canny_edges(input);
    let contours = find_contours(&edges);
    let mut outlines = Mask::new(input.w, input.h, 0);
    let mut controls = Vec::new();
    for contour in contours {
        let Some(rect) = bounding_rect(&contour) else {
            continue;
        };
        let aspect = f64::from(rect.w) / f64::from(rect.h.max(1));
        if rect.h < 20
            || f64::from(rect.h) > 0.15 * f64::from(input.h)
            || rect.w < 50
            || f64::from(rect.w) > 0.6 * f64::from(input.w)
            || aspect <= 1.5
            || aspect >= 15.0
        {
            continue;
        }
        if contour_area(&contour) < 0.88 * f64::from(rect.w * rect.h) {
            continue;
        }
        controls.push(rect);
        draw_contours(&mut outlines, std::slice::from_ref(&contour), 3);
    }
    (controls, outlines)
}

pub(super) fn find_contours(mask: &Mask) -> Vec<Vec<(i32, i32)>> {
    if mask.w == 0 || mask.h == 0 {
        return Vec::new();
    }
    let stride = mask.w + 2;
    let height = mask.h + 2;
    let mut image = vec![0u8; (stride * height) as usize];
    for y in 0..mask.h {
        for x in 0..mask.w {
            if *mask.at(x, y) != 0 {
                image[((y + 1) * stride + (x + 1)) as usize] = 1;
            }
        }
    }

    let mut contours = Vec::new();
    let mut x = 1i32;
    let mut y = 1i32;
    let mut last_pos = (0i32, 1i32);
    let width = stride - 1;
    let scan_height = height - 1;
    let mut prev = image[(y * stride + x - 1) as usize];

    while y < scan_height {
        while x < width {
            let mut pixel = image[(y * stride + x) as usize];
            while x < width && pixel == prev {
                x += 1;
                if x < width {
                    pixel = image[(y * stride + x) as usize];
                }
            }
            if x >= width {
                break;
            }
            pixel = image[(y * stride + x) as usize];
            if let Some(contour) = try_contour(&mut image, stride, prev, pixel, &mut last_pos, x, y)
            {
                contours.push(contour);
                x += 1;
                prev = image[(y * stride + x - 1) as usize];
            } else {
                prev = pixel;
                if pixel & 0xFE != 0 {
                    last_pos.0 = x;
                }
                x += 1;
            }
        }
        y += 1;
        last_pos = (0, y);
        x = 1;
        prev = 0;
    }
    contours.reverse();
    contours
}

fn try_contour(
    image: &mut [u8],
    stride: i32,
    prev: u8,
    pixel: u8,
    last_pos: &mut (i32, i32),
    x: i32,
    y: i32,
) -> Option<Vec<(i32, i32)>> {
    let mut is_hole = false;
    if !(prev == 0 && pixel == 1) {
        if pixel != 0 || (prev as i8) < 1 {
            return None;
        }
        if prev & 0xFE != 0 {
            last_pos.0 = x - 1;
        }
        is_hole = true;
    }
    last_pos.0 = x - i32::from(is_hole);
    let start = (x - i32::from(is_hole), y);
    Some(fetch_contour(image, stride, start, is_hole))
}

fn fetch_contour(
    image: &mut [u8],
    stride: i32,
    start: (i32, i32),
    is_hole: bool,
) -> Vec<(i32, i32)> {
    let mut points = Vec::new();
    let start_index = (start.1 * stride + start.0) as usize;
    let mut s_end: i32 = if is_hole { 0 } else { 4 };
    let mut s = s_end;
    let mut i1;
    loop {
        s = (s - 1) & 7;
        i1 = neighbor(start_index, stride, s);
        if image[i1] != 0 || s == s_end {
            break;
        }
    }

    let mut pt = (start.0 - 1, start.1 - 1);
    if s == s_end {
        image[start_index] = 0x82;
        points.push(pt);
        return points;
    }

    let mut i3 = start_index;
    let mut prev_s = s ^ 4;
    loop {
        s_end = s;
        s = s.min(15);
        let mut i4 = i3;
        while s < 15 {
            s += 1;
            i4 = neighbor(i3, stride, s);
            if image[i4] != 0 {
                break;
            }
        }
        s &= 7;
        if (s.wrapping_sub(1) as u32) < s_end as u32 {
            image[i3] = 0x82;
        } else if image[i3] == 1 {
            image[i3] = 2;
        }
        if s != prev_s {
            points.push(pt);
        }
        prev_s = s;
        pt.0 += DELTAS[s as usize].0;
        pt.1 += DELTAS[s as usize].1;
        if i4 == start_index && i3 == i1 {
            break;
        }
        i3 = i4;
        s = (s + 4) & 7;
    }
    points
}

fn neighbor(index: usize, stride: i32, direction: i32) -> usize {
    let (dx, dy) = DELTAS[(direction as usize) & 7];
    (index as i32 + dx + dy * stride) as usize
}

fn bounding_rect(contour: &[(i32, i32)]) -> Option<Rect> {
    let mut points = contour.iter();
    let first = *points.next()?;
    let (mut x0, mut y0, mut x1, mut y1) = (first.0, first.1, first.0, first.1);
    for &(x, y) in points {
        x0 = x0.min(x);
        y0 = y0.min(y);
        x1 = x1.max(x);
        y1 = y1.max(y);
    }
    Some(Rect::new(x0, y0, x1 - x0 + 1, y1 - y0 + 1))
}

fn contour_area(contour: &[(i32, i32)]) -> f64 {
    if contour.is_empty() {
        return 0.0;
    }
    let mut area = 0.0;
    let mut prev = contour[contour.len() - 1];
    for &point in contour {
        area += f64::from(prev.0) * f64::from(point.1) - f64::from(prev.1) * f64::from(point.0);
        prev = point;
    }
    (area * 0.5).abs()
}

#[cfg(test)]
pub(super) fn contour_area_for_test(contour: &[(i32, i32)]) -> f64 {
    contour_area(contour)
}

#[cfg(test)]
pub(super) fn bounding_rect_for_test(contour: &[(i32, i32)]) -> Option<Rect> {
    bounding_rect(contour)
}

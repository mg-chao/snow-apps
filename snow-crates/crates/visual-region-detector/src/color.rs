//! Color statistics and global/local foreground estimation.

use crate::geometry::{Rect, expand};
use crate::grid::{Grid, Image, Mask};

fn median(values: &mut [u8]) -> f64 {
    values.sort_unstable();
    let n = values.len();
    if n.is_multiple_of(2) {
        (f64::from(values[n / 2 - 1]) + f64::from(values[n / 2])) / 2.
    } else {
        f64::from(values[n / 2])
    }
}
pub(crate) fn median_color(pixels: impl Iterator<Item = [u8; 3]>) -> [f64; 3] {
    let mut c = [Vec::new(), Vec::new(), Vec::new()];
    for p in pixels {
        for i in 0..3 {
            c[i].push(p[i]);
        }
    }
    c.map(|mut v| median(&mut v))
}
pub(crate) fn quant_key(p: &[u8; 3], step: u8) -> usize {
    let n = 256 / usize::from(step);
    (usize::from(p[0] / step) * n + usize::from(p[1] / step)) * n + usize::from(p[2] / step)
}
pub(crate) fn histogram(bgr: &Image, r: Rect, step: u8) -> Vec<usize> {
    let n = 256 / usize::from(step);
    let mut hist = vec![0; n * n * n];
    for y in r.y..r.y2() {
        for x in r.x..r.x2() {
            hist[quant_key(bgr.at(x, y), step)] += 1;
        }
    }
    hist
}
pub(crate) fn surface_fraction(bgr: &Image, r: Rect) -> f64 {
    *histogram(bgr, r, 16).iter().max().unwrap() as f64 / r.area() as f64
}
pub(crate) fn background(bgr: &Image) -> [f64; 3] {
    let (w, h) = (bgr.w, bgr.h);
    let t = 4.max(w.min(h) / 80);
    let mut hist = vec![0usize; 32768];
    for r in [
        Rect::new(0, 0, w, t.min(h)),
        Rect::new(0, (h - t).max(0), w, t.min(h)),
        Rect::new(0, 0, t.min(w), h),
        Rect::new((w - t).max(0), 0, t.min(w), h),
    ] {
        for y in r.y..r.y2() {
            for x in r.x..r.x2() {
                hist[quant_key(bgr.at(x, y), 8)] += 1;
            }
        }
    }
    let mut best = 0;
    for k in 1..hist.len() {
        if hist[k] > hist[best] {
            best = k;
        }
    }
    [
        (best / 1024 * 8 + 4) as f64,
        (best / 32 % 32 * 8 + 4) as f64,
        (best % 32 * 8 + 4) as f64,
    ]
}
pub(crate) fn foreground(bgr: &Image, bg: [f64; 3], threshold: f64) -> Mask {
    Mask {
        w: bgr.w,
        h: bgr.h,
        data: bgr
            .data
            .iter()
            .map(|p| {
                if (0..3).any(|c| (f64::from(p[c]) - bg[c]).abs() > threshold) {
                    255
                } else {
                    0
                }
            })
            .collect(),
    }
}
pub(crate) fn local_foreground(bgr: &Image, bg: &Image, threshold: i16) -> Mask {
    Mask {
        w: bgr.w,
        h: bgr.h,
        data: bgr
            .data
            .iter()
            .zip(&bg.data)
            .map(|(p, b)| {
                if (0..3).any(|c| (i16::from(p[c]) - i16::from(b[c])).abs() > threshold) {
                    255
                } else {
                    0
                }
            })
            .collect(),
    }
}

pub(crate) fn boundary_contrast(bgr: &Image, r: Rect) -> f64 {
    let outer = expand(r, 4, bgr.w, bgr.h);
    let mut ring = Vec::new();
    for y in outer.y..outer.y2() {
        for x in outer.x..outer.x2() {
            if x < r.x || x >= r.x2() || y < r.y || y >= r.y2() {
                ring.push(*bgr.at(x, y));
            }
        }
    }
    if ring.is_empty() {
        return 0.;
    }
    let inside = median_color(bgr.crop(r).data.into_iter());
    let outside = median_color(ring.into_iter());
    (0..3)
        .map(|i| (inside[i] - outside[i]).abs())
        .fold(0., f64::max)
}
pub(crate) fn roi_std(bgr: &Image, r: Rect, labels: &Grid<i32>, id: i32) -> f64 {
    let mut sum = [0.; 3];
    let mut n = 0.;
    for y in r.y..r.y2() {
        for x in r.x..r.x2() {
            if *labels.at(x, y) == id {
                n += 1.;
                for (c, total) in sum.iter_mut().enumerate() {
                    *total += f64::from(bgr.at(x, y)[c]);
                }
            }
        }
    }
    if n == 0. {
        return 0.;
    }
    let mean = sum.map(|v| v / n);
    let mut var = [0.; 3];
    for y in r.y..r.y2() {
        for x in r.x..r.x2() {
            if *labels.at(x, y) == id {
                for c in 0..3 {
                    var[c] += (f64::from(bgr.at(x, y)[c]) - mean[c]).powi(2);
                }
            }
        }
    }
    var.iter().map(|v| (v / n).sqrt()).sum::<f64>() / 3.
}

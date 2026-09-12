//! Binary mask operations, morphology, and connected components.

use crate::backend::{self, Component, KernelShape, MorphOp};
use crate::error::Result;
use crate::geometry::{Rect, clip};
use crate::grid::{Grid, Mask};

pub(crate) fn or(a: &Mask, b: &Mask) -> Mask {
    let mut out = a.clone();
    for (value, other) in out.data.iter_mut().zip(&b.data) {
        *value |= other;
    }
    out
}

pub(crate) fn suppress(a: &Mask, b: &Mask) -> Mask {
    let mut out = a.clone();
    for (value, other) in out.data.iter_mut().zip(&b.data) {
        if *other > 0 {
            *value = 0;
        }
    }
    out
}

pub(crate) fn morph(
    input: &Mask,
    op: MorphOp,
    kernel_width: i32,
    kernel_height: i32,
    shape: KernelShape,
) -> Result<Mask> {
    backend::morphology(input, op, kernel_width, kernel_height, shape)
}

pub(crate) fn dilate(input: &Mask, kernel: i32) -> Result<Mask> {
    morph(input, MorphOp::Dilate, kernel, kernel, KernelShape::Rect)
}

pub(crate) fn morphology(
    input: &Mask,
    op: MorphOp,
    kernel_width: i32,
    kernel_height: i32,
) -> Result<Mask> {
    morph(input, op, kernel_width, kernel_height, KernelShape::Rect)
}

pub(crate) fn cc(input: &Mask) -> Result<(Grid<i32>, Vec<Component>)> {
    backend::connected_components(input)
}

pub(crate) fn rasterize(rects: &[Rect], width: i32, height: i32) -> Mask {
    let mut out = Mask::new(width, height, 0);
    for rect in rects {
        for y in rect.y.max(0)..rect.y2().min(height) {
            for x in rect.x.max(0)..rect.x2().min(width) {
                out.set(x, y, 255);
            }
        }
    }
    out
}

pub(crate) fn shrink(rect: Rect, mask: &Mask, pad: i32) -> Option<Rect> {
    let (mut x0, mut y0, mut x1, mut y1) = (mask.w, mask.h, -1, -1);
    for y in rect.y.max(0)..rect.y2().min(mask.h) {
        for x in rect.x.max(0)..rect.x2().min(mask.w) {
            if *mask.at(x, y) > 0 {
                x0 = x0.min(x);
                y0 = y0.min(y);
                x1 = x1.max(x);
                y1 = y1.max(y);
            }
        }
    }
    if x1 < 0 {
        None
    } else {
        clip(
            Rect::new(
                x0 - pad,
                y0 - pad,
                x1 - x0 + 1 + 2 * pad,
                y1 - y0 + 1 + 2 * pad,
            ),
            mask.w,
            mask.h,
        )
    }
}

/// Fill zero-valued holes without relying on an image-processing library.
///
/// The implementation mirrors the flood-fill construction used by OpenCV:
/// zeros connected to the padded border are background, and all remaining
/// zeros are enclosed holes that become foreground.
pub(crate) fn fill_holes(mask: &Mask) -> Result<Mask> {
    let width = mask.w + 2;
    let height = mask.h + 2;
    let mut outside = Grid::new(width, height, false);
    let mut queue = vec![(0, 0)];
    outside.set(0, 0, true);
    let neighbors = [(-1, 0), (1, 0), (0, -1), (0, 1)];
    let mut head = 0;
    while let Some(&(x, y)) = queue.get(head) {
        head += 1;
        for (dx, dy) in neighbors {
            let nx = x + dx;
            let ny = y + dy;
            if nx < 0 || nx >= width || ny < 0 || ny >= height || *outside.at(nx, ny) {
                continue;
            }
            let is_foreground =
                nx > 0 && nx <= mask.w && ny > 0 && ny <= mask.h && *mask.at(nx - 1, ny - 1) > 0;
            if is_foreground {
                continue;
            }
            outside.set(nx, ny, true);
            queue.push((nx, ny));
        }
    }

    let mut out = mask.clone();
    for y in 0..mask.h {
        for x in 0..mask.w {
            if *mask.at(x, y) > 0 || !*outside.at(x + 1, y + 1) {
                out.set(x, y, 255);
            }
        }
    }
    Ok(out)
}

pub(crate) fn split_gaps(rect: Rect, ink: &Mask, gap_frac: f64) -> Vec<Rect> {
    let (x0, y0, x1, y1) = (
        rect.x.max(0),
        rect.y.max(0),
        rect.x2().min(ink.w),
        rect.y2().min(ink.h),
    );
    if x1 <= x0 || y1 <= y0 {
        return vec![rect];
    }
    let columns: Vec<_> = (x0..x1)
        .map(|x| (y0..y1).any(|y| *ink.at(x, y) > 0))
        .collect();
    let mut runs = Vec::new();
    let mut i = 0;
    while i < columns.len() {
        if columns[i] {
            let mut j = i;
            while j < columns.len() && columns[j] {
                j += 1;
            }
            runs.push((i as i32, j as i32));
            i = j;
        } else {
            i += 1;
        }
    }
    if runs.len() <= 1 {
        return vec![rect];
    }
    let gap = 5.max((f64::from(rect.h) * gap_frac).round_ties_even() as i32);
    let mut groups = vec![runs[0]];
    for (a, b) in runs.into_iter().skip(1) {
        let last = groups.last_mut().unwrap();
        if a - last.1 >= gap {
            groups.push((a, b));
        } else {
            last.1 = b;
        }
    }
    if groups.len() == 1 {
        vec![rect]
    } else {
        groups
            .iter()
            .map(|(a, b)| Rect::new(x0 + a, rect.y, b - a, rect.h))
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hole_filling_handles_foreground_at_all_corners() -> Result<()> {
        let mut mask = Mask::new(30, 30, 255);
        for y in 5..25 {
            for x in 5..25 {
                mask.set(x, y, 0);
            }
        }
        assert!(fill_holes(&mask)?.data.iter().all(|value| *value == 255));
        for y in 0..10 {
            for x in 13..17 {
                mask.set(x, y, 0);
            }
        }
        assert_eq!(fill_holes(&mask)?.data, mask.data);
        Ok(())
    }
}

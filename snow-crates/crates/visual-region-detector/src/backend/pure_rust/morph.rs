//! Morphology, connected components, and filtering aligned with OpenCV.

use super::{Component, KernelShape, MorphOp};
use crate::geometry::Rect;
use crate::grid::{Grid, Mask};
use std::collections::VecDeque;

pub(super) fn morphology(
    input: &Mask,
    op: MorphOp,
    width: i32,
    height: i32,
    shape: KernelShape,
) -> Mask {
    match shape {
        KernelShape::Rect => match op {
            MorphOp::Dilate => morph_rect(input, true, width, height),
            MorphOp::Open => {
                let eroded = morph_rect(input, false, width, height);
                morph_rect(&eroded, true, width, height)
            }
            MorphOp::Close => {
                let dilated = morph_rect(input, true, width, height);
                morph_rect(&dilated, false, width, height)
            }
            MorphOp::Gradient => morph_gradient_rect(input, width, height),
        },
        KernelShape::Ellipse => {
            let offsets = kernel_offsets(width, height, KernelShape::Ellipse);
            match op {
                MorphOp::Dilate => morph_once(input, &offsets, true),
                MorphOp::Open => {
                    let eroded = morph_once(input, &offsets, false);
                    morph_once(&eroded, &offsets, true)
                }
                MorphOp::Close => {
                    let dilated = morph_once(input, &offsets, true);
                    morph_once(&dilated, &offsets, false)
                }
                MorphOp::Gradient => {
                    let dilated = morph_once(input, &offsets, true);
                    let eroded = morph_once(input, &offsets, false);
                    subtract_masks(&dilated, &eroded)
                }
            }
        }
    }
}

fn subtract_masks(dilated: &Mask, eroded: &Mask) -> Mask {
    let mut output = Mask::new(dilated.w, dilated.h, 0);
    for (out, (dilate, erode)) in output
        .data
        .iter_mut()
        .zip(dilated.data.iter().zip(&eroded.data))
    {
        *out = dilate.saturating_sub(*erode);
    }
    output
}

fn morph_gradient_rect(input: &Mask, width: i32, height: i32) -> Mask {
    let dilated = morph_rect(input, true, width, height);
    let eroded = morph_rect(input, false, width, height);
    subtract_masks(&dilated, &eroded)
}

fn kernel_offsets(width: i32, height: i32, shape: KernelShape) -> Vec<(i32, i32)> {
    let width = width.max(1);
    let height = height.max(1);
    // OpenCV's normalizeAnchor(-1, ksize) uses ksize / 2, so even kernels
    // are intentionally anchored toward their lower/right half.
    let anchor_x = width / 2;
    let anchor_y = height / 2;
    let mut offsets = Vec::with_capacity((width * height) as usize);
    for ky in 0..height {
        let (x_start, x_end) = match shape {
            KernelShape::Rect => (0, width),
            KernelShape::Ellipse if width == 1 && height == 1 => (0, width),
            KernelShape::Ellipse => {
                let radius_y = height / 2;
                let radius_x = width / 2;
                let dy = ky - radius_y;
                if dy.abs() > radius_y {
                    (0, 0)
                } else {
                    let normalized = if radius_y == 0 {
                        0.0
                    } else {
                        f64::from(radius_y * radius_y - dy * dy) / f64::from(radius_y * radius_y)
                    };
                    let dx = (f64::from(radius_x) * normalized.sqrt()).round_ties_even() as i32;
                    ((radius_x - dx).max(0), (radius_x + dx + 1).min(width))
                }
            }
        };
        for kx in x_start..x_end {
            offsets.push((kx - anchor_x, ky - anchor_y));
        }
    }
    if offsets.is_empty() {
        offsets.push((0, 0));
    }
    offsets
}

fn morph_once(input: &Mask, offsets: &[(i32, i32)], dilating: bool) -> Mask {
    let mut output = Mask::new(input.w, input.h, 0);
    for y in 0..input.h {
        for x in 0..input.w {
            let mut value = if dilating { 0 } else { 255 };
            for &(dx, dy) in offsets {
                let sx = x + dx;
                let sy = y + dy;
                let sample = if sx < 0 || sx >= input.w || sy < 0 || sy >= input.h {
                    if dilating { 0 } else { 255 }
                } else {
                    *input.at(sx, sy)
                };
                if dilating {
                    value = value.max(sample);
                } else {
                    value = value.min(sample);
                }
            }
            output.set(x, y, value);
        }
    }
    output
}

fn morph_rect(input: &Mask, dilating: bool, kernel_width: i32, kernel_height: i32) -> Mask {
    let kernel_width = kernel_width.max(1);
    let kernel_height = kernel_height.max(1);
    if input.w == 0 || input.h == 0 || (kernel_width == 1 && kernel_height == 1) {
        return input.clone();
    }
    let pad = if dilating { 0 } else { 255 };
    let mut work = input.clone();
    if kernel_width > 1 {
        filter_rows(&mut work, kernel_width, kernel_width / 2, pad, dilating);
    }
    if kernel_height > 1 {
        work = transpose(&work);
        filter_rows(&mut work, kernel_height, kernel_height / 2, pad, dilating);
        work = transpose(&work);
    }
    work
}

fn transpose(input: &Mask) -> Mask {
    let mut output = Mask::new(input.h, input.w, 0);
    for y in 0..input.h {
        for x in 0..input.w {
            output.set(y, x, *input.at(x, y));
        }
    }
    output
}

fn worker_count(rows: i32) -> usize {
    if rows < 64 {
        1
    } else {
        std::thread::available_parallelism()
            .map(|n| n.get().min(8))
            .unwrap_or(1)
    }
}

fn filter_rows(mask: &mut Mask, kernel: i32, anchor: i32, pad: u8, dilating: bool) {
    let width = mask.w as usize;
    let workers = worker_count(mask.h);
    let chunk_rows = ((mask.h as usize) + workers - 1) / workers.max(1);
    std::thread::scope(|scope| {
        for chunk in mask.data.chunks_mut(width * chunk_rows.max(1)) {
            scope.spawn(move || {
                let mut row_out = vec![0u8; width];
                let mut deque = VecDeque::with_capacity(kernel.max(1) as usize);
                for row in chunk.chunks_exact_mut(width) {
                    sliding_minmax(row, kernel, anchor, pad, dilating, &mut row_out, &mut deque);
                    row.copy_from_slice(&row_out);
                }
            });
        }
    });
}

fn sliding_minmax(
    src: &[u8],
    kernel: i32,
    anchor: i32,
    pad: u8,
    dilating: bool,
    out: &mut [u8],
    deque: &mut VecDeque<i32>,
) {
    deque.clear();
    let n = src.len() as i32;
    let right = kernel - anchor - 1;
    let sample = |index: i32| -> u8 {
        if index < 0 || index >= n {
            pad
        } else {
            src[index as usize]
        }
    };
    let dominates = |left: u8, right: u8| -> bool {
        if dilating {
            left >= right
        } else {
            left <= right
        }
    };
    let start = -anchor;
    let end = n - 1 + right;
    for index in start..=end {
        let value = sample(index);
        while deque
            .back()
            .is_some_and(|&back| dominates(value, sample(back)))
        {
            deque.pop_back();
        }
        deque.push_back(index);
        let out_x = index - right;
        if out_x >= 0 && out_x < n {
            let window_start = out_x - anchor;
            while deque.front().is_some_and(|&front| front < window_start) {
                deque.pop_front();
            }
            out[out_x as usize] = sample(*deque.front().expect("window is nonempty"));
        }
    }
}

pub(super) fn connected_components(input: &Mask) -> (Grid<i32>, Vec<Component>) {
    let n = (input.w * input.h) as usize;
    if n == 0 {
        return (Grid::new(input.w, input.h, 0), Vec::new());
    }
    let mut provisional = vec![0i32; n];
    let mut parent = vec![0i32];
    let mut next_label = 1i32;
    let idx = |x: i32, y: i32| (y * input.w + x) as usize;
    let in_bounds = |x: i32, y: i32| x >= 0 && x < input.w && y >= 0 && y < input.h;

    for y in 0..input.h {
        for x in 0..input.w {
            if *input.at(x, y) == 0 {
                continue;
            }
            let mut neighbor_labels = [0i32; 4];
            let mut count = 0;
            for (dx, dy) in [(-1, -1), (0, -1), (1, -1), (-1, 0)] {
                let nx = x + dx;
                let ny = y + dy;
                if in_bounds(nx, ny) {
                    let label = provisional[idx(nx, ny)];
                    if label != 0 {
                        neighbor_labels[count] = label;
                        count += 1;
                    }
                }
            }
            if count == 0 {
                parent.push(next_label);
                provisional[idx(x, y)] = next_label;
                next_label += 1;
                continue;
            }
            let mut chosen = neighbor_labels[0];
            for &label in &neighbor_labels[1..count] {
                chosen = chosen.min(label);
            }
            provisional[idx(x, y)] = chosen;
            for &label in &neighbor_labels[..count] {
                union(&mut parent, chosen, label);
            }
        }
    }

    let mut root_to_id = vec![0i32; parent.len()];
    let mut labels = Grid::new(input.w, input.h, 0i32);
    let mut stats: Vec<(i32, i32, i32, i32, i32)> = Vec::new();
    let mut id = 0i32;
    for y in 0..input.h {
        for x in 0..input.w {
            let label = provisional[idx(x, y)];
            if label == 0 {
                continue;
            }
            let root = find(&mut parent, label);
            let mapped = if root_to_id[root as usize] == 0 {
                id += 1;
                root_to_id[root as usize] = id;
                stats.push((x, y, x, y, 0));
                id
            } else {
                root_to_id[root as usize]
            };
            labels.set(x, y, mapped);
            let stat = &mut stats[(mapped - 1) as usize];
            stat.0 = stat.0.min(x);
            stat.1 = stat.1.min(y);
            stat.2 = stat.2.max(x);
            stat.3 = stat.3.max(y);
            stat.4 += 1;
        }
    }

    let components = stats
        .into_iter()
        .enumerate()
        .map(|(index, (x0, y0, x1, y1, area))| Component {
            rect: Rect::new(x0, y0, x1 - x0 + 1, y1 - y0 + 1),
            area,
            id: (index + 1) as i32,
        })
        .collect();
    (labels, components)
}

fn find(parent: &mut [i32], mut label: i32) -> i32 {
    let mut root = label;
    while parent[root as usize] != root {
        root = parent[root as usize];
    }
    while parent[label as usize] != root {
        let next = parent[label as usize];
        parent[label as usize] = root;
        label = next;
    }
    root
}

fn union(parent: &mut [i32], a: i32, b: i32) {
    let a = find(parent, a);
    let b = find(parent, b);
    if a == b {
        return;
    }
    if a < b {
        parent[b as usize] = a;
    } else {
        parent[a as usize] = b;
    }
}

fn clamp_index(index: i32, length: i32) -> i32 {
    index.clamp(0, length - 1)
}

fn reflect_101(index: i32, length: i32) -> i32 {
    if length <= 1 {
        return 0;
    }
    let mut index = index;
    while index < 0 || index >= length {
        if index < 0 {
            index = -index;
        } else {
            index = 2 * length - index - 2;
        }
    }
    index
}

pub(super) fn blur_row(input: &[f32], kernel: i32) -> Vec<f32> {
    let kernel = kernel.max(1);
    if input.is_empty() || kernel == 1 {
        return input.to_vec();
    }
    let n = input.len() as i32;
    let anchor = kernel / 2;
    let right = kernel - anchor - 1;
    let sample = |index: i32| input[reflect_101(index, n) as usize];
    let mut sum: f32 = (-anchor..=right).map(sample).sum();
    let mut output = Vec::with_capacity(input.len());
    output.push(sum / kernel as f32);
    for x in 1..n {
        sum += sample(x + right) - sample(x - 1 - anchor);
        output.push(sum / kernel as f32);
    }
    output
}

pub(super) fn median_row(input: &[u8], kernel: i32) -> Vec<u8> {
    let kernel = if kernel % 2 == 0 { kernel + 1 } else { kernel }.max(1);
    if input.is_empty() || kernel == 1 {
        return input.to_vec();
    }
    let n = input.len() as i32;
    let anchor = (kernel - 1) / 2;
    let right = kernel - anchor - 1;
    let mut histogram = [0u16; 256];
    let sample = |index: i32| input[clamp_index(index, n) as usize];
    for offset in -anchor..=right {
        histogram[usize::from(sample(offset))] += 1;
    }
    let rank = (kernel as u16) / 2;
    let mut output = Vec::with_capacity(input.len());
    output.push(histogram_median_1d(&histogram, rank));
    for x in 1..n {
        histogram[usize::from(sample(x - 1 - anchor))] -= 1;
        histogram[usize::from(sample(x + right))] += 1;
        output.push(histogram_median_1d(&histogram, rank));
    }
    output
}

fn histogram_median_1d(histogram: &[u16; 256], rank: u16) -> u8 {
    let mut remaining = rank;
    for (value, count) in histogram.iter().enumerate() {
        if remaining < *count {
            return value as u8;
        }
        remaining -= *count;
    }
    255
}

#[cfg(test)]
pub(super) fn ellipse_kernel(width: i32, height: i32) -> Mask {
    let width = width.max(1);
    let height = height.max(1);
    let mut kernel = Mask::new(width, height, 0);
    for (dx, dy) in kernel_offsets(width, height, KernelShape::Ellipse) {
        kernel.set(dx + width / 2, dy + height / 2, 1);
    }
    kernel
}

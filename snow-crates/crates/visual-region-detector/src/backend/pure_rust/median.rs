//! 8-bit median blur aligned with OpenCV `medianBlur_8u_O1`.
//!
//! OpenCV (`E:\opencv\modules\imgproc\src\median_blur.simd.hpp`) pads only the
//! left/right with `BORDER_REPLICATE`, then walks the image in column stripes
//! of `512 / channels` so the two-tier histograms stay in cache. Each output
//! pixel is O(1) in the kernel size.

use crate::grid::Image;

pub(super) fn median_blur_image(input: &Image, kernel: i32) -> Image {
    let kernel = if kernel % 2 == 0 { kernel + 1 } else { kernel }.max(1);
    if kernel == 1 || input.w == 0 || input.h == 0 {
        return input.clone();
    }
    let radius = ((kernel - 1) / 2) as usize;
    let mut padded = Image::new(input.w + 2 * radius as i32, input.h, [0; 3]);
    for y in 0..input.h {
        for x in 0..padded.w {
            let src_x = (x - radius as i32).clamp(0, input.w - 1);
            padded.set(x, y, *input.at(src_x, y));
        }
    }
    let mut output = Image::new(input.w, input.h, [0; 3]);
    median_blur_8u_o1(&padded, &mut output, radius);
    output
}

/// Scalar port of OpenCV's `medianBlur_8u_O1` stripe loop.
fn median_blur_8u_o1(padded: &Image, output: &mut Image, radius: usize) {
    let width = output.w as usize;
    let height = output.h as usize;
    if width == 0 || height == 0 {
        return;
    }
    let stripe = (512 / 3).min(width).max(1);
    let ranges: Vec<(usize, usize)> = (0..width)
        .step_by(stripe)
        .map(|x0| (x0, (width - x0).min(stripe)))
        .collect();

    if ranges.len() == 1 || height < 48 {
        for &(x0, stripe_w) in &ranges {
            fill_stripe(
                padded,
                &mut output.data,
                x0,
                stripe_w,
                radius,
                height,
                width,
                x0,
            );
        }
        return;
    }

    let mut bands: Vec<(usize, usize, Vec<[u8; 3]>)> = ranges
        .iter()
        .map(|&(x0, stripe_w)| (x0, stripe_w, vec![[0; 3]; height * stripe_w]))
        .collect();
    std::thread::scope(|scope| {
        for (x0, stripe_w, band) in &mut bands {
            scope.spawn(move || {
                fill_stripe(padded, band, *x0, *stripe_w, radius, height, *stripe_w, 0);
            });
        }
    });
    for (x0, stripe_w, band) in bands {
        for y in 0..height {
            let src = &band[y * stripe_w..(y + 1) * stripe_w];
            output.data[y * width + x0..y * width + x0 + stripe_w].copy_from_slice(src);
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn fill_stripe(
    padded: &Image,
    dest: &mut [[u8; 3]],
    x0: usize,
    stripe_w: usize,
    radius: usize,
    height: usize,
    dest_stride: usize,
    dest_x0: usize,
) {
    const CN: usize = 3;
    let n = stripe_w + 2 * radius;
    let rank = (2 * radius * radius + 2 * radius) as u16;
    let src_width = padded.w as usize;
    let mut coarse = vec![0u16; 16 * n * CN];
    let mut fine = vec![0u16; 16 * 16 * n * CN];

    for channel in 0..CN {
        for column in 0..n {
            hop(
                &mut coarse,
                &mut fine,
                n,
                channel,
                column,
                padded.data[x0 + column][channel],
                (radius + 2) as i16,
            );
        }
        for row in 1..radius {
            let y = row.min(height.saturating_sub(1));
            let base = y * src_width + x0;
            for column in 0..n {
                hop(
                    &mut coarse,
                    &mut fine,
                    n,
                    channel,
                    column,
                    padded.data[base + column][channel],
                    1,
                );
            }
        }
    }

    for row in 0..height {
        let old_y = row.saturating_sub(radius + 1);
        let new_y = (row + radius).min(height.saturating_sub(1));
        let old_base = old_y * src_width + x0;
        let new_base = new_y * src_width + x0;
        for channel in 0..CN {
            for column in 0..n {
                hop(
                    &mut coarse,
                    &mut fine,
                    n,
                    channel,
                    column,
                    padded.data[old_base + column][channel],
                    -1,
                );
                hop(
                    &mut coarse,
                    &mut fine,
                    n,
                    channel,
                    column,
                    padded.data[new_base + column][channel],
                    1,
                );
            }

            let mut window = [0u16; 16];
            let mut luc = [0u16; 16];
            let mut fine_window = [[0u16; 16]; 16];
            for column in 0..(2 * radius) {
                let src = &coarse[16 * (n * channel + column)..];
                for bin in 0..16 {
                    window[bin] += src[bin];
                }
            }

            for local in 0..stripe_w {
                let column = local + radius;
                let add_at = (column + radius).min(n - 1);
                let add = &coarse[16 * (n * channel + add_at)..];
                for bin in 0..16 {
                    window[bin] += add[bin];
                }

                let mut sum = 0u16;
                let mut bucket = 0usize;
                while bucket < 16 {
                    sum += window[bucket];
                    if sum > rank {
                        sum -= window[bucket];
                        break;
                    }
                    bucket += 1;
                }

                if bucket < 16 {
                    update_fine_window(
                        &fine,
                        &mut fine_window,
                        &mut luc,
                        n,
                        channel,
                        bucket,
                        column,
                        radius,
                    );
                }

                let sub_at = column - radius;
                let sub = &coarse[16 * (n * channel + sub_at)..];
                for bin in 0..16 {
                    window[bin] -= sub[bin];
                }

                let mut value = 255u8;
                if bucket < 16 {
                    for (fine_bin, count) in fine_window[bucket].iter().enumerate() {
                        sum += *count;
                        if sum > rank {
                            value = (bucket * 16 + fine_bin) as u8;
                            break;
                        }
                    }
                }
                dest[row * dest_stride + dest_x0 + local][channel] = value;
            }
        }
    }
}

#[inline]
fn hop(
    coarse: &mut [u16],
    fine: &mut [u16],
    n: usize,
    channel: usize,
    column: usize,
    value: u8,
    delta: i16,
) {
    let coarse_bin = usize::from(value >> 4);
    let fine_bin = usize::from(value & 0x0f);
    let coarse_i = 16 * (n * channel + column) + coarse_bin;
    let fine_i = 16 * (n * (16 * channel + coarse_bin) + column) + fine_bin;
    if delta >= 0 {
        let add = delta as u16;
        coarse[coarse_i] += add;
        fine[fine_i] += add;
    } else {
        let sub = (-delta) as u16;
        coarse[coarse_i] -= sub;
        fine[fine_i] -= sub;
    }
}

#[allow(clippy::too_many_arguments)]
fn update_fine_window(
    fine: &[u16],
    fine_window: &mut [[u16; 16]; 16],
    luc: &mut [u16; 16],
    n: usize,
    channel: usize,
    bucket: usize,
    column: usize,
    radius: usize,
) {
    if luc[bucket] as usize <= column - radius {
        fine_window[bucket] = [0; 16];
        luc[bucket] = (column - radius) as u16;
        while (luc[bucket] as usize) < (column + radius + 1).min(n) {
            let src = &fine[16 * (n * (16 * channel + bucket) + luc[bucket] as usize)..];
            for bin in 0..16 {
                fine_window[bucket][bin] += src[bin];
            }
            luc[bucket] += 1;
        }
        if (luc[bucket] as usize) < column + radius + 1 {
            let extra = (column + radius + 1 - n) as u16;
            let src = &fine[16 * (n * (16 * channel + bucket) + (n - 1))..];
            for bin in 0..16 {
                fine_window[bucket][bin] += extra * src[bin];
            }
            luc[bucket] = (column + radius + 1) as u16;
        }
    } else {
        let base = 16 * n * (16 * channel + bucket);
        while (luc[bucket] as usize) < column + radius + 1 {
            let add_col = (luc[bucket] as usize).min(n - 1);
            let sub_col = (luc[bucket] as usize).saturating_sub(2 * radius + 1);
            let add = &fine[base + 16 * add_col..];
            let sub = &fine[base + 16 * sub_col..];
            for bin in 0..16 {
                fine_window[bucket][bin] = fine_window[bucket][bin] + add[bin] - sub[bin];
            }
            luc[bucket] += 1;
        }
    }
}

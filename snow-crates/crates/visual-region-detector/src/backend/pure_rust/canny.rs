//! Scalar Canny detector matching OpenCV's 8-bit L1 path.

use crate::grid::{Image, Mask};

pub(super) fn canny_edges(input: &Image) -> Mask {
    if input.w == 0 || input.h == 0 {
        return Mask::new(input.w, input.h, 0);
    }

    let width = input.w as usize;
    let height = input.h as usize;
    let gray: Vec<u8> = input.data.iter().copied().map(grayscale).collect();
    let mut dx = vec![0i16; gray.len()];
    let mut dy = vec![0i16; gray.len()];
    sobel(&gray, width, height, &mut dx, &mut dy);

    let mapstep = width + 2;
    let mut mag = vec![0i32; (height + 2) * mapstep];
    let mut map = vec![1u8; (height + 2) * mapstep];
    for y in 0..height {
        let row = (y + 1) * mapstep + 1;
        for x in 0..width {
            let index = y * width + x;
            mag[row + x] = i32::from(dx[index]).abs() + i32::from(dy[index]).abs();
            map[row + x] = 1;
        }
    }

    const TG22: i32 = 13_573;
    let low = 20;
    let high = 60;
    let mut stack = Vec::new();
    for y in 0..height {
        for x in 0..width {
            let index = (y + 1) * mapstep + (x + 1);
            let m = mag[index];
            if m <= low {
                map[index] = 1;
                continue;
            }

            let src = y * width + x;
            let xs = i32::from(dx[src]);
            let ys = i32::from(dy[src]);
            let abs_x = xs.abs();
            let abs_y = ys.abs() << 15;
            let tg22x = abs_x * TG22;
            let is_maximum = if abs_y < tg22x {
                m > mag[index - 1] && m >= mag[index + 1]
            } else {
                let tg67x = tg22x + (abs_x << 16);
                if abs_y > tg67x {
                    m > mag[index - mapstep] && m >= mag[index + mapstep]
                } else {
                    let step = if (xs ^ ys) < 0 { -1 } else { 1 };
                    m > mag[(index as isize - mapstep as isize - step) as usize]
                        && m > mag[(index as isize + mapstep as isize + step) as usize]
                }
            };

            if is_maximum {
                if m > high {
                    map[index] = 2;
                    stack.push(index);
                } else {
                    map[index] = 0;
                }
            } else {
                map[index] = 1;
            }
        }
    }

    while let Some(index) = stack.pop() {
        for neighbor in [
            index - mapstep - 1,
            index - mapstep,
            index - mapstep + 1,
            index - 1,
            index + 1,
            index + mapstep - 1,
            index + mapstep,
            index + mapstep + 1,
        ] {
            if map[neighbor] == 0 {
                map[neighbor] = 2;
                stack.push(neighbor);
            }
        }
    }

    let mut edges = Mask::new(input.w, input.h, 0);
    for y in 0..height {
        let row = (y + 1) * mapstep + 1;
        for x in 0..width {
            edges.data[y * width + x] = if map[row + x] == 2 { 255 } else { 0 };
        }
    }
    edges
}

pub(super) fn grayscale(pixel: [u8; 3]) -> u8 {
    // OpenCV COLOR_BGR2GRAY 8-bit coefficients (BY15, GY15, RY15).
    let value = 3_735 * i32::from(pixel[0])
        + 19_235 * i32::from(pixel[1])
        + 9_798 * i32::from(pixel[2])
        + (1 << 14);
    (value >> 15) as u8
}

fn worker_count(rows: usize) -> usize {
    if rows < 64 {
        1
    } else {
        std::thread::available_parallelism()
            .map(|n| n.get().min(8))
            .unwrap_or(1)
    }
}

/// 3x3 Sobel matching OpenCV's separable kernels with `BORDER_REPLICATE`.
fn sobel(gray: &[u8], width: usize, height: usize, dx: &mut [i16], dy: &mut [i16]) {
    let workers = worker_count(height);
    let chunk = (height + workers - 1) / workers.max(1);
    std::thread::scope(|scope| {
        for (chunk_index, (dx_chunk, dy_chunk)) in dx
            .chunks_mut(width * chunk.max(1))
            .zip(dy.chunks_mut(width * chunk.max(1)))
            .enumerate()
        {
            let start = chunk_index * chunk;
            scope.spawn(move || {
                for (local, (dx_row, dy_row)) in dx_chunk
                    .chunks_exact_mut(width)
                    .zip(dy_chunk.chunks_exact_mut(width))
                    .enumerate()
                {
                    let y = start + local;
                    let ym = y.saturating_sub(1);
                    let yp = (y + 1).min(height - 1);
                    let top = ym * width;
                    let mid = y * width;
                    let bot = yp * width;
                    for x in 0..width {
                        let xm = x.saturating_sub(1);
                        let xp = (x + 1).min(width - 1);
                        let p00 = i16::from(gray[top + xm]);
                        let p01 = i16::from(gray[top + x]);
                        let p02 = i16::from(gray[top + xp]);
                        let p10 = i16::from(gray[mid + xm]);
                        let p12 = i16::from(gray[mid + xp]);
                        let p20 = i16::from(gray[bot + xm]);
                        let p21 = i16::from(gray[bot + x]);
                        let p22 = i16::from(gray[bot + xp]);
                        dx_row[x] = (p02 + 2 * p12 + p22) - (p00 + 2 * p10 + p20);
                        dy_row[x] = (p20 + 2 * p21 + p22) - (p00 + 2 * p01 + p02);
                    }
                }
            });
        }
    });
}

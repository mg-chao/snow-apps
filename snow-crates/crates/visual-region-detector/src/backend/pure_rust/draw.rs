//! OpenCV LINE_8 thick-contour drawing for 8-bit masks.

use crate::grid::Mask;

const XY_SHIFT: i32 = 16;
const XY_ONE: i64 = 1 << XY_SHIFT;

pub(super) fn draw_contours(mask: &mut Mask, contours: &[Vec<(i32, i32)>], thickness: i32) {
    for contour in contours {
        if contour.is_empty() {
            continue;
        }
        for (index, &p0) in contour.iter().enumerate() {
            let p1 = contour[(index + 1) % contour.len()];
            thick_line(mask, p0, p1, thickness, 2);
        }
    }
}

fn cv_round(value: f64) -> i32 {
    value.round_ties_even() as i32
}

fn thick_line(mask: &mut Mask, p0: (i32, i32), p1: (i32, i32), mut thickness: i32, flags: i32) {
    let mut p0 = (i64::from(p0.0), i64::from(p0.1));
    let mut p1 = (i64::from(p1.0), i64::from(p1.1));
    if thickness > 1 {
        let size = (i64::from(mask.w), i64::from(mask.h));
        if !contains(size, p0) || !contains(size, p1) {
            let margin = i64::from(thickness);
            p0 = (p0.0 + margin, p0.1 + margin);
            p1 = (p1.0 + margin, p1.1 + margin);
            clip_line((size.0 + 2 * margin, size.1 + 2 * margin), &mut p0, &mut p1);
            p0 = (p0.0 - margin, p0.1 - margin);
            p1 = (p1.0 - margin, p1.1 - margin);
        }
    }

    p0 = (p0.0 << XY_SHIFT, p0.1 << XY_SHIFT);
    p1 = (p1.0 << XY_SHIFT, p1.1 << XY_SHIFT);

    let odd_thickness = thickness & 1;
    thickness <<= XY_SHIFT - 1;
    let inv = 1.0 / XY_ONE as f64;
    let dx = (p0.0 - p1.0) as f64 * inv;
    let dy = (p1.1 - p0.1) as f64 * inv;
    let mut r = dx * dx + dy * dy;
    if r.abs() > f64::EPSILON {
        r = (f64::from(thickness) + f64::from(odd_thickness) * XY_ONE as f64 * 0.5) / r.sqrt();
        let dpx = i64::from(cv_round(dy * r));
        let dpy = i64::from(cv_round(dx * r));
        let quad = [
            (p0.0 + dpx, p0.1 + dpy),
            (p0.0 - dpx, p0.1 - dpy),
            (p1.0 - dpx, p1.1 - dpy),
            (p1.0 + dpx, p1.1 + dpy),
        ];
        fill_convex_poly(mask, &quad);
    }

    let radius = ((i64::from(thickness) + (XY_ONE >> 1)) >> XY_SHIFT) as i32;
    let mut cursor = p0;
    for i in 0..2 {
        if flags & (i + 1) != 0 {
            let center = (
                ((cursor.0 + (XY_ONE >> 1)) >> XY_SHIFT) as i32,
                ((cursor.1 + (XY_ONE >> 1)) >> XY_SHIFT) as i32,
            );
            fill_circle(mask, center, radius);
        }
        cursor = p1;
    }
}

fn contains(size: (i64, i64), point: (i64, i64)) -> bool {
    point.0 >= 0 && point.1 >= 0 && point.0 < size.0 && point.1 < size.1
}

fn clip_line(size: (i64, i64), pt1: &mut (i64, i64), pt2: &mut (i64, i64)) -> bool {
    if size.0 <= 0 || size.1 <= 0 {
        return false;
    }
    let right = size.0 - 1;
    let bottom = size.1 - 1;
    let code = |x: i64, y: i64| {
        i32::from(x < 0)
            + i32::from(x > right) * 2
            + i32::from(y < 0) * 4
            + i32::from(y > bottom) * 8
    };
    let mut c1 = code(pt1.0, pt1.1);
    let mut c2 = code(pt2.0, pt2.1);
    if (c1 & c2) == 0 && (c1 | c2) != 0 {
        if c1 & 12 != 0 {
            let a = if c1 < 8 { 0 } else { bottom };
            pt1.0 += ((a - pt1.1) as f64 * (pt2.0 - pt1.0) as f64 / (pt2.1 - pt1.1) as f64) as i64;
            pt1.1 = a;
            c1 = i32::from(pt1.0 < 0) + i32::from(pt1.0 > right) * 2;
        }
        if c2 & 12 != 0 {
            let a = if c2 < 8 { 0 } else { bottom };
            pt2.0 += ((a - pt2.1) as f64 * (pt2.0 - pt1.0) as f64 / (pt2.1 - pt1.1) as f64) as i64;
            pt2.1 = a;
            c2 = i32::from(pt2.0 < 0) + i32::from(pt2.0 > right) * 2;
        }
        if (c1 & c2) == 0 && (c1 | c2) != 0 {
            if c1 != 0 {
                let a = if c1 == 1 { 0 } else { right };
                pt1.1 +=
                    ((a - pt1.0) as f64 * (pt2.1 - pt1.1) as f64 / (pt2.0 - pt1.0) as f64) as i64;
                pt1.0 = a;
                c1 = 0;
            }
            if c2 != 0 {
                let a = if c2 == 1 { 0 } else { right };
                pt2.1 +=
                    ((a - pt2.0) as f64 * (pt2.1 - pt1.1) as f64 / (pt2.0 - pt1.0) as f64) as i64;
                pt2.0 = a;
                c2 = 0;
            }
        }
    }
    (c1 | c2) == 0
}

fn line2(mask: &mut Mask, mut pt1: (i64, i64), mut pt2: (i64, i64)) {
    let size = (i64::from(mask.w) << XY_SHIFT, i64::from(mask.h) << XY_SHIFT);
    if !clip_line(size, &mut pt1, &mut pt2) {
        return;
    }
    let dx = pt2.0 - pt1.0;
    let dy = pt2.1 - pt1.1;
    let j = if dx < 0 { -1 } else { 0 };
    let ax = (dx ^ j) - j;
    let i = if dy < 0 { -1 } else { 0 };
    let ay = (dy ^ i) - i;
    let (x_step, y_step, mut ecount, dominant_x);
    if ax > ay {
        let dy = (dy ^ j) - j;
        if j != 0 {
            std::mem::swap(&mut pt1, &mut pt2);
        }
        let y_s = dy * XY_ONE / (ax | 1);
        ecount = ((pt2.0 - pt1.0) >> XY_SHIFT) as i32;
        x_step = XY_ONE;
        y_step = y_s;
        dominant_x = true;
    } else {
        let dx = (dx ^ i) - i;
        if i != 0 {
            std::mem::swap(&mut pt1, &mut pt2);
        }
        let x_s = dx * XY_ONE / (ay | 1);
        ecount = ((pt2.1 - pt1.1) >> XY_SHIFT) as i32;
        x_step = x_s;
        y_step = XY_ONE;
        dominant_x = false;
    }
    pt1.0 += XY_ONE >> 1;
    pt1.1 += XY_ONE >> 1;
    put(
        mask,
        ((pt2.0 + (XY_ONE >> 1)) >> XY_SHIFT) as i32,
        ((pt2.1 + (XY_ONE >> 1)) >> XY_SHIFT) as i32,
    );
    if dominant_x {
        pt1.0 >>= XY_SHIFT;
        while ecount >= 0 {
            put(mask, pt1.0 as i32, (pt1.1 >> XY_SHIFT) as i32);
            pt1.0 += 1;
            pt1.1 += y_step;
            ecount -= 1;
        }
    } else {
        pt1.1 >>= XY_SHIFT;
        while ecount >= 0 {
            put(mask, (pt1.0 >> XY_SHIFT) as i32, pt1.1 as i32);
            pt1.0 += x_step;
            pt1.1 += 1;
            ecount -= 1;
        }
    }
}

fn fill_convex_poly(mask: &mut Mask, vertices: &[(i64, i64)]) {
    let npts = vertices.len() as i32;
    if npts == 0 {
        return;
    }
    let shift = XY_SHIFT;
    let delta = 1i64 << (shift - 1);
    let delta1 = XY_ONE >> 1;
    let delta2 = XY_ONE >> 1;
    let mut p0 = vertices[npts as usize - 1];
    let mut xmin = vertices[0].0;
    let mut xmax = vertices[0].0;
    let mut ymin = vertices[0].1;
    let mut ymax = vertices[0].1;
    let mut imin = 0;
    for (i, &p) in vertices.iter().enumerate() {
        if p.1 < ymin {
            ymin = p.1;
            imin = i as i32;
        }
        ymax = ymax.max(p.1);
        xmax = xmax.max(p.0);
        xmin = xmin.min(p.0);
        line2(mask, p0, p);
        p0 = p;
    }
    xmin = (xmin + delta) >> shift;
    xmax = (xmax + delta) >> shift;
    ymin = (ymin + delta) >> shift;
    ymax = (ymax + delta) >> shift;
    if npts < 3 || xmax < 0 || ymax < 0 || xmin >= i64::from(mask.w) || ymin >= i64::from(mask.h) {
        return;
    }
    ymax = ymax.min(i64::from(mask.h - 1));
    struct Edge {
        idx: i32,
        di: i32,
        x: i64,
        dx: i64,
        ye: i32,
    }
    let mut edges = npts;
    let mut y = ymin as i32;
    let mut edge = [
        Edge {
            idx: imin,
            di: 1,
            x: -XY_ONE,
            dx: 0,
            ye: y,
        },
        Edge {
            idx: imin,
            di: npts - 1,
            x: -XY_ONE,
            dx: 0,
            ye: y,
        },
    ];
    loop {
        for item in &mut edge {
            if y >= item.ye {
                let mut idx0 = item.idx;
                let di = item.di;
                let mut idx = idx0 + di;
                if idx >= npts {
                    idx -= npts;
                }
                loop {
                    let remaining = edges;
                    edges -= 1;
                    if remaining <= 0 {
                        break;
                    }
                    let ty = ((vertices[idx as usize].1 + delta) >> shift) as i32;
                    if ty > y {
                        let xs = vertices[idx0 as usize].0;
                        let xe = vertices[idx as usize].0;
                        item.ye = ty;
                        item.dx = ((xe - xs) * 2 + i64::from(ty - y)) / (2 * i64::from(ty - y));
                        item.x = xs;
                        item.idx = idx;
                        break;
                    }
                    idx0 = idx;
                    idx += di;
                    if idx >= npts {
                        idx -= npts;
                    }
                }
            }
        }
        if edges < 0 {
            break;
        }
        if y >= 0 {
            let (left, right) = if edge[0].x > edge[1].x {
                (1, 0)
            } else {
                (0, 1)
            };
            let mut xx1 = ((edge[left].x + delta1) >> XY_SHIFT) as i32;
            let mut xx2 = ((edge[right].x + delta2) >> XY_SHIFT) as i32;
            if xx2 >= 0 && xx1 < mask.w {
                xx1 = xx1.max(0);
                xx2 = xx2.min(mask.w - 1);
                if xx1 <= xx2 {
                    let start = (y * mask.w + xx1) as usize;
                    let end = (y * mask.w + xx2) as usize;
                    mask.data[start..=end].fill(255);
                }
            }
        }
        edge[0].x += edge[0].dx;
        edge[1].x += edge[1].dx;
        y += 1;
        if y > ymax as i32 {
            break;
        }
    }
}

fn fill_circle(mask: &mut Mask, center: (i32, i32), radius: i32) {
    if radius < 0 {
        return;
    }
    let mut err: i64 = 0;
    let mut dx = i64::from(radius);
    let mut dy = 0i64;
    let mut plus = 1i64;
    let mut minus = (i64::from(radius) << 1) - 1;
    let width = i64::from(mask.w);
    let height = i64::from(mask.h);
    while dx >= dy {
        let y11 = i64::from(center.1) - dy;
        let y12 = i64::from(center.1) + dy;
        let y21 = i64::from(center.1) - dx;
        let y22 = i64::from(center.1) + dx;
        let mut x11 = i64::from(center.0) - dx;
        let mut x12 = i64::from(center.0) + dx;
        let mut x21 = i64::from(center.0) - dy;
        let mut x22 = i64::from(center.0) + dy;
        x11 = x11.max(0);
        x12 = x12.min(width - 1);
        x21 = x21.max(0);
        x22 = x22.min(width - 1);
        hline(mask, y11, x11, x12, width, height);
        hline(mask, y12, x11, x12, width, height);
        hline(mask, y21, x21, x22, width, height);
        hline(mask, y22, x21, x22, width, height);
        dy += 1;
        err += plus;
        plus += 2;
        let step_x = err > 0;
        if step_x {
            err -= minus;
            dx -= 1;
            minus -= 2;
        }
    }
}

fn hline(mask: &mut Mask, y: i64, x1: i64, x2: i64, width: i64, height: i64) {
    if y < 0 || y >= height || x2 < 0 || x1 >= width || x1 > x2 {
        return;
    }
    let start = (y * width + x1) as usize;
    let end = (y * width + x2) as usize;
    mask.data[start..=end].fill(255);
}

fn put(mask: &mut Mask, x: i32, y: i32) {
    if x >= 0 && y >= 0 && x < mask.w && y < mask.h {
        mask.set(x, y, 255);
    }
}

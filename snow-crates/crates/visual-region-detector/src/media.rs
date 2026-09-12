//! Photographs, logos, textures, and avatar candidates.

use crate::backend::MorphOp;
use crate::color::{foreground, histogram, roi_std, surface_fraction};
use crate::error::Result;
use crate::geometry::{Rect, clip, containment, expand, iou};
use crate::grid::{Image, Mask};
use crate::mask::split_gaps;
use crate::mask::{cc, fill_holes, morphology, shrink, suppress};
use crate::selection::merge;

pub(crate) fn detect_images(bgr: &Image, fg: &Mask) -> Result<Vec<Rect>> {
    let (w, h) = (bgr.w, bgr.h);
    let k = 9.max((0.008 * f64::from(h)).round_ties_even() as i32);
    let fat = fill_holes(&morphology(fg, MorphOp::Open, k, k)?)?;
    let (labels, stats) = cc(&fat)?;
    let mut out = Vec::new();
    for c in stats {
        let r = c.rect;
        if r.w > (0.55 * f64::from(w)) as i32
            || r.h > (0.45 * f64::from(h)) as i32
            || r.area() > (0.20 * f64::from(w) * f64::from(h)) as i64
            || r.w < 28
            || r.h < 24
            || c.area < 700
        {
            continue;
        }
        let fill = f64::from(c.area) / r.area() as f64;
        let min_photo = 64.max((0.040 * f64::from(h)) as i32);
        if fill >= 0.48
            && r.w >= min_photo
            && r.h >= min_photo
            && roi_std(bgr, r, &labels, c.id) >= 12.
            && let Some(t) = shrink(r, &fat, 1)
        {
            out.push(t);
        }
    }
    Ok(merge(out, 0.2))
}
pub(crate) fn detect_logos(bgr: &Image, fg: &Mask) -> Result<Vec<Rect>> {
    let (w, h) = (bgr.w, bgr.h);
    let mut band = fg.clone();
    for y in 48.max((0.14 * f64::from(h)) as i32).min(h)..h {
        for x in 0..w {
            band.set(x, y, 0);
        }
    }
    let closed = morphology(&band, MorphOp::Close, 17, 9)?;
    let (labels, stats) = cc(&closed)?;
    let mut out = Vec::new();
    for c in stats {
        let r = c.rect;
        let aspect = f64::from(r.w) / f64::from(r.h);
        if r.w > (0.42 * f64::from(w)) as i32
            || r.h > (0.11 * f64::from(h)) as i32
            || r.w < 72
            || r.h < 28
            || !(1.4..=5.5).contains(&aspect)
            || f64::from(c.area) / (r.area() as f64) < 0.18
            || roi_std(bgr, r, &labels, c.id) < 20.
        {
            continue;
        }
        if let Some(t) = shrink(r, &closed, 1) {
            out.push(t);
        }
    }
    Ok(out)
}
fn square_components(
    mask: &Mask,
    sup: &Mask,
    min: i32,
    max: i32,
    lo: f64,
    hi: f64,
) -> Result<Vec<Rect>> {
    let (_, stats) = cc(&suppress(mask, sup))?;
    let mut out = Vec::new();
    for c in stats {
        let r = c.rect;
        let aspect = f64::from(r.w) / f64::from(r.h);
        let fill = f64::from(c.area) / r.area() as f64;
        if r.w < min
            || r.h < min
            || r.w > max
            || r.h > max
            || !(0.62..=1.55).contains(&aspect)
            || fill < lo
            || fill > hi
        {
            continue;
        }
        if let Some(t) = clip(
            Rect::new(r.x - 2, r.y - 2, r.w + 4, r.h + 4),
            mask.w,
            mask.h,
        ) {
            out.push(t);
        }
    }
    Ok(out)
}
pub(crate) fn square_badges(bgr: &Image, fg: &Mask, bg: [f64; 3], sup: &Mask) -> Result<Vec<Rect>> {
    let min = 24.max((0.018 * f64::from(bgr.h)) as i32);
    let max = 90.max((0.055 * f64::from(bgr.h)) as i32);
    let solids = square_components(fg, sup, min, max, 0.28, 1.)?;
    let mut out = square_components(&foreground(bgr, bg, 12.), sup, min, max, 0.04, 0.25)?;
    for s in solids {
        if !out
            .iter()
            .any(|r| iou(s, *r) >= 0.25 || containment(s, *r) >= 0.5)
        {
            out.push(s);
        }
    }
    Ok(out)
}
pub(crate) fn textures(bgr: &Image, ink: &Mask) -> Result<Vec<Rect>> {
    let work = morphology(
        &morphology(ink, MorphOp::Close, 11, 11)?,
        MorphOp::Open,
        13,
        13,
    )?;
    let (_, stats) = cc(&work)?;
    let mut out = Vec::new();
    let mut transposed = Mask::new(ink.h, ink.w, 0);
    for y in 0..ink.h {
        for x in 0..ink.w {
            transposed.set(y, x, *ink.at(x, y));
        }
    }
    for c in stats {
        let r = c.rect;
        let aspect = f64::from(r.w) / f64::from(r.h);
        if r.w.min(r.h) < 30
            || f64::from(r.w.max(r.h)) > 0.4 * f64::from(ink.w.max(ink.h))
            || aspect <= 0.45
            || aspect >= 4.
        {
            continue;
        }
        if histogram(bgr, r, 16).iter().filter(|v| **v >= 3).count() < 45
            || f64::from(c.area) < 0.55 * r.area() as f64
            || surface_fraction(bgr, r) > 0.6
        {
            continue;
        }
        for p in split_gaps(r, ink, 0.03) {
            for s in split_gaps(Rect::new(p.y, p.x, p.h, p.w), &transposed, 0.03) {
                let rr = Rect::new(s.y, s.x, s.h, s.w);
                if rr.w.min(rr.h) >= 30 {
                    out.push(expand(rr, 2, ink.w, ink.h));
                }
            }
        }
    }
    Ok(out)
}
pub(crate) fn reject_glyphs(rects: &[Rect], ink: &Mask, surfaces: &[Rect]) -> Result<Vec<Rect>> {
    let (_, stats) = cc(ink)?;
    let components: Vec<_> = stats
        .iter()
        .filter(|c| c.area >= 8)
        .map(|c| c.rect)
        .collect();
    let mut out = Vec::new();
    for &r in rects {
        let neighbors = components.iter().any(|c| {
            f64::from(c.h) >= 0.45 * f64::from(r.h)
                && f64::from(r.y2().min(c.y2()) - r.y.max(c.y)) > 0.5 * f64::from(r.h.min(c.h))
                && containment(*c, r) < 0.3
                && r.x.max(c.x) - r.x2().min(c.x2()) >= -4
                && f64::from(r.x.max(c.x) - r.x2().min(c.x2())) < 0.4 * f64::from(r.h.min(c.h))
        });
        let density = ink.crop(r).data.iter().filter(|v| **v > 0).count() as f64 / r.area() as f64;
        if !neighbors
            || (r.w.min(r.h) >= 45 && density > 0.5)
            || surfaces.iter().any(|s| iou(r, *s) > 0.65)
        {
            out.push(r);
        }
    }
    Ok(out)
}
pub(crate) fn refine_avatars(
    cands: &[Rect],
    texts: &[Rect],
    w: i32,
    h: i32,
) -> (Vec<Rect>, Vec<Rect>) {
    let mut avatars = Vec::new();
    let mut icons = Vec::new();
    let max_icon = 36.max((0.032 * f64::from(h)) as i32);
    let header_y = (0.14 * f64::from(h)) as i32;
    for &r in cands {
        let has_text = texts.iter().any(|t| {
            f64::from(r.y2().min(t.y2()) - r.y.max(t.y)) >= 0.25 * f64::from(r.h.min(t.h))
                && t.x >= r.x2() - 8
                && f64::from(t.x) <= f64::from(r.x2()) + 5.5 * f64::from(r.w)
        });
        let left_column = f64::from(r.x) < 0.42 * f64::from(w) && r.y > header_y;
        if has_text || left_column || r.w.max(r.h) > max_icon {
            avatars.push(r);
        } else {
            icons.push(r);
        }
    }
    (avatars, icons)
}

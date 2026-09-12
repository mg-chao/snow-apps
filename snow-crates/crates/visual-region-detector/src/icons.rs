//! Dot menus and residual icon detection.

use crate::backend::{KernelShape, MorphOp};
use crate::error::Result;
use crate::geometry::{Rect, containment, expand, intersect, union};
use crate::grid::Mask;
use crate::mask::{cc, dilate, morph, morphology, suppress};

pub(crate) fn dot_icons(ink: &Mask) -> Result<Vec<Rect>> {
    let (labels, stats) = cc(ink)?;
    let mut dots = Mask::new(ink.w, ink.h, 0);
    let mut small = Vec::new();
    for c in &stats {
        let r = c.rect;
        let aspect = f64::from(r.w) / f64::from(r.h);
        if (2..=8).contains(&r.w)
            && (2..=8).contains(&r.h)
            && (0.6..=1.7).contains(&aspect)
            && f64::from(c.area) >= 0.5 * r.area() as f64
        {
            for y in r.y..r.y2() {
                for x in r.x..r.x2() {
                    if *labels.at(x, y) == c.id {
                        dots.set(x, y, 255);
                    }
                }
            }
            small.push(r);
        }
    }
    let (_, joined) = cc(&dilate(&dots, 9)?)?;
    let mut out = Vec::new();
    for c in joined {
        let r = c.rect;
        let children: Vec<_> = small
            .iter()
            .copied()
            .filter(|d| containment(*d, r) > 0.9)
            .collect();
        if ![3, 6, 9].contains(&children.len()) || r.w > 50 || r.h > 50 {
            continue;
        }
        if children.iter().map(|d| d.w).max().unwrap() - children.iter().map(|d| d.w).min().unwrap()
            > 2
            || children.iter().map(|d| d.h).max().unwrap()
                - children.iter().map(|d| d.h).min().unwrap()
                > 2
        {
            continue;
        }
        let rr = children.iter().copied().reduce(union).unwrap();
        let context = expand(rr, 5, ink.w, ink.h);
        if (context.y..context.y2())
            .any(|y| (context.x..context.x2()).any(|x| *ink.at(x, y) > 0 && *dots.at(x, y) == 0))
        {
            continue;
        }
        let near = expand(rr, 7, ink.w, ink.h);
        let max_area = small.iter().map(|d| d.area()).max().unwrap();
        if stats
            .iter()
            .any(|c| i64::from(c.area) > max_area && intersect(near, c.rect).is_some())
        {
            continue;
        }
        out.push(context);
    }
    Ok(out)
}

pub(crate) fn icons(fg: &Mask, sup: &Mask, text_mask: &Mask, texts: &[Rect]) -> Result<Vec<Rect>> {
    let residual = suppress(&suppress(fg, sup), text_mask);
    let closed = morphology(
        &morph(&residual, MorphOp::Close, 3, 3, KernelShape::Ellipse)?,
        MorphOp::Close,
        3,
        11,
    )?;
    let min = 14.max((0.012 * f64::from(fg.h)) as i32);
    let max = 28.max((0.038 * f64::from(fg.h)) as i32);
    let (_, stats) = cc(&closed)?;
    let mut out = Vec::new();
    for c in stats {
        let r = c.rect;
        if (1..=3).contains(&r.w)
            && r.h >= min
            && f64::from(r.h) <= f64::from(max) * 1.6
            && f64::from(c.area) > 0.6 * r.area() as f64
        {
            out.push(r);
            continue;
        }
        let fill = f64::from(c.area) / r.area() as f64;
        let aspect = f64::from(r.w) / f64::from(r.h);
        if r.w < min
            || r.h < min
            || f64::from(r.w) > f64::from(max) * 1.4
            || f64::from(r.h) > f64::from(max) * 1.6
            || fill < 0.22
            || c.area < 80
            || !(0.32..=2.2).contains(&aspect)
        {
            continue;
        }
        if texts
            .iter()
            .any(|t| containment(r, *t) > 0.5 || containment(*t, r) > 0.5)
        {
            continue;
        }
        out.push(r);
    }
    Ok(out)
}

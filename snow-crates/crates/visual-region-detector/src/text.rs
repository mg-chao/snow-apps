//! Text lines, paragraph grouping, and styled text splitting.

use crate::backend::{self, MorphOp};
use crate::color::local_foreground;
use crate::error::Result;
use crate::geometry::{Rect, union};
use crate::grid::{Image, Mask};
use crate::mask::split_gaps;
use crate::mask::{cc, morphology, or, shrink, suppress};
use crate::selection::merge_by;

pub(crate) fn text_lines(bgr: &Image, bg: &Image, sup: &Mask) -> Result<(Vec<Rect>, Mask, i32)> {
    let (w, h) = (bgr.w, bgr.h);
    let bw = suppress(&local_foreground(bgr, bg, 20), sup);
    let hline = morphology(&bw, MorphOp::Open, 80.max(w / 15), 1)?;
    let vline = morphology(&bw, MorphOp::Open, 1, 80.max(h / 15))?;
    let bw = suppress(&bw, &or(&hline, &vline));
    let (_, stats) = cc(&bw)?;
    let mut heights: Vec<_> = stats
        .iter()
        .filter(|c| {
            c.rect.h >= 8
                && c.rect.h <= (0.06 * f64::from(h)) as i32
                && c.rect.w >= 2
                && c.rect.w <= (0.5 * f64::from(w)) as i32
                && c.area >= 8
        })
        .map(|c| c.rect.h)
        .collect();
    heights.sort_unstable();
    let char_h = if heights.is_empty() {
        16.max((0.022 * f64::from(h)) as i32)
    } else {
        let pos = (heights.len() - 1) as f64 * 0.7;
        let lo = pos.floor() as usize;
        let hi = pos.ceil() as usize;
        (f64::from(heights[lo]) + f64::from(heights[hi] - heights[lo]) * (pos - lo as f64))
            .round_ties_even() as i32
    };
    let max_h = 96.max(3 * char_h);
    let badge_side = 36.max((0.024 * f64::from(h)) as i32);
    let kx = 5
        .max((0.85 * f64::from(char_h)).round_ties_even() as i32)
        .min(22.max((0.018 * f64::from(w)) as i32));
    let ky = 1.max((0.14 * f64::from(char_h)).round_ties_even() as i32);
    let closed = morphology(&bw, MorphOp::Close, kx, ky)?;
    let (_, stats) = cc(&closed)?;
    let mut words = Vec::new();
    for c in stats {
        let r = c.rect;
        let fill = f64::from(c.area) / r.area() as f64;
        let aspect = f64::from(r.w) / f64::from(r.h);
        if r.h < 8
            || r.h > max_h
            || r.w < 6
            || fill < 0.10
            || (r.w.min(r.h) >= badge_side && (0.65..=1.45).contains(&aspect) && fill >= 0.48)
        {
            continue;
        }
        words.push(r);
    }
    let lines = merge_by(words, |a, b| {
        if f64::from(a.y2().min(b.y2()) - a.y.max(b.y)) < 0.5 * f64::from(a.h.min(b.h)) {
            return false;
        }
        let gap = a.x.max(b.x) - a.x2().min(b.x2());
        gap < 0
            || f64::from(gap)
                <= (if a.h.min(b.h) >= 26 { 1.15 } else { 0.82 }) * f64::from(a.h.min(b.h))
    });
    let pad = 3.max((f64::from(char_h) * 0.12).round_ties_even() as i32);
    let mut out = Vec::new();
    for line in lines {
        for r in split_gaps(line, &bw, 1.05) {
            if let Some(t) = shrink(r, &bw, pad)
                && t.h >= 8
                && t.w >= 6
                && !(t.w < 10 && t.w * 2 <= t.h)
            {
                out.push(t);
            }
        }
    }
    Ok((out, bw, char_h))
}
pub(crate) fn paragraphs(texts: &[Rect]) -> Vec<Rect> {
    let mut items = texts.to_vec();
    items.sort_by_key(|r| (r.y, r.x));
    let mut groups: Vec<Vec<Rect>> = Vec::new();
    for r in items {
        let found = groups.iter().position(|g| {
            let last = *g.last().unwrap();
            let mh = f64::from(last.h.min(r.h));
            let ratio = f64::from(last.h) / f64::from(r.h);
            f64::from((last.x - r.x).abs()) < 0.35 * mh
                && r.y - last.y2() > 0
                && f64::from(r.y - last.y2()) < 0.8 * mh
                && ratio > 0.85
                && ratio < 1.18
                && last.w.min(r.w) > 8 * r.h
        });
        if let Some(i) = found {
            groups[i].push(r);
        } else {
            groups.push(vec![r]);
        }
    }
    groups
        .iter()
        .filter(|g| g.len() >= 3)
        .map(|g| g.iter().copied().reduce(union).unwrap())
        .collect()
}
pub(crate) fn split_surface_text(texts: &[Rect], surfaces: &[Rect], ink: &Mask) -> Vec<Rect> {
    let mut out = Vec::new();
    for &r in texts {
        let mut cuts = vec![r.x, r.x2()];
        for s in surfaces {
            if f64::from(s.w) < 0.7 * f64::from(r.w)
                && f64::from(s.h) > 0.7 * f64::from(r.h)
                && s.h < 2 * r.h
                && f64::from(r.y2().min(s.y2()) - r.y.max(s.y)) > 0.7 * f64::from(r.h)
            {
                if r.x < s.x && s.x < r.x2() {
                    cuts.push(s.x);
                }
                if r.x < s.x2() && s.x2() < r.x2() {
                    cuts.push(s.x2());
                }
            }
        }
        cuts.sort_unstable();
        cuts.dedup();
        for ab in cuts.windows(2) {
            if let Some(t) = shrink(Rect::new(ab[0], r.y, ab[1] - ab[0], r.h), ink, 3) {
                out.push(t);
            }
        }
    }
    out
}
pub(crate) fn split_styled_lines(texts: &[Rect], bgr: &Image, ink: &Mask) -> Result<Vec<Rect>> {
    let mut out = Vec::new();
    for &r in texts {
        let mut votes = vec![0f32; r.w as usize];
        let mut totals = votes.clone();
        for y in r.y..r.y2() {
            for x in r.x..r.x2() {
                if *ink.at(x, y) > 0 {
                    let i = (x - r.x) as usize;
                    totals[i] += 1.;
                    let p = bgr.at(x, y);
                    if p.iter().copied().max().unwrap() - p.iter().copied().min().unwrap() > 70 {
                        votes[i] += 1.;
                    }
                }
            }
        }
        let kernel = 3.max((f64::from(r.h) * 0.6) as i32);
        let v = backend::blur_row(&votes, kernel)?;
        let t = backend::blur_row(&totals, kernel)?;
        let states: Vec<u8> = v
            .iter()
            .zip(&t)
            .map(|(v, t)| u8::from(*v > 1f32.max(*t * 0.65)))
            .collect();
        let states = backend::median_row(&states, 5)?;
        let mut runs = Vec::new();
        let mut start = 0;
        let mut state = states[0];
        for (x, &value) in states.iter().enumerate().skip(1) {
            if value != state {
                runs.push((start, x as i32));
                start = x as i32;
                state = value;
            }
        }
        runs.push((start, r.w));
        if runs.len() > 1 && runs.iter().all(|(a, b)| b - a > 100.max(4 * r.h)) {
            for (a, b) in runs {
                if let Some(t) = shrink(Rect::new(r.x + a, r.y, b - a, r.h), ink, 3) {
                    out.push(t);
                }
            }
        } else {
            out.push(r);
        }
    }
    Ok(out)
}

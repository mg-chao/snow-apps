//! Stable rectangle merging, suppression, and output ordering.

use crate::geometry::{Rect, containment, iou, union};
use crate::model::Region;

pub(crate) fn merge_by(mut items: Vec<Rect>, predicate: impl Fn(Rect, Rect) -> bool) -> Vec<Rect> {
    loop {
        let mut changed = false;
        let mut out = Vec::new();
        let mut used = vec![false; items.len()];
        for (i, a) in items.iter().enumerate() {
            if used[i] {
                continue;
            }
            let mut cur = *a;
            for j in i + 1..items.len() {
                if !used[j] && predicate(cur, items[j]) {
                    cur = union(cur, items[j]);
                    used[j] = true;
                    changed = true;
                }
            }
            used[i] = true;
            out.push(cur);
        }
        items = out;
        if !changed {
            return items;
        }
    }
}
pub(crate) fn merge(items: Vec<Rect>, threshold: f64) -> Vec<Rect> {
    merge_by(items, |a, b| {
        iou(a, b) >= threshold || containment(b, a) > 0.7
    })
}
pub(crate) fn nms(mut items: Vec<Region>, threshold: f64) -> Vec<Region> {
    items.sort_by_key(|r| r.rect.area());
    let mut keep: Vec<Region> = Vec::new();
    for r in items {
        if let Some(i) = keep.iter().position(|k| iou(r.rect, k.rect) >= threshold) {
            if r.rect.area() <= keep[i].rect.area() {
                keep[i] = r;
            }
        } else {
            keep.push(r);
        }
    }
    keep
}
pub(crate) fn sort_regions(items: &mut [Region]) {
    items.sort_by_key(|r| (-r.rect.w.max(r.rect.h), -r.rect.area(), r.rect.y, r.rect.x));
}

//! Half-open pixel rectangles and geometric relationships.

use serde::Serialize;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    #[serde(rename = "width")]
    pub w: i32,
    #[serde(rename = "height")]
    pub h: i32,
}
impl Rect {
    pub fn new(x: i32, y: i32, w: i32, h: i32) -> Self {
        Self { x, y, w, h }
    }
    pub fn x2(self) -> i32 {
        self.x + self.w
    }
    pub fn y2(self) -> i32 {
        self.y + self.h
    }
    pub fn area(self) -> i64 {
        i64::from(self.w.max(0)) * i64::from(self.h.max(0))
    }
}

pub(crate) fn intersect(a: Rect, b: Rect) -> Option<Rect> {
    let (x, y, x2, y2) = (
        a.x.max(b.x),
        a.y.max(b.y),
        a.x2().min(b.x2()),
        a.y2().min(b.y2()),
    );
    (x2 > x && y2 > y).then(|| Rect::new(x, y, x2 - x, y2 - y))
}
pub(crate) fn union(a: Rect, b: Rect) -> Rect {
    let (x, y) = (a.x.min(b.x), a.y.min(b.y));
    Rect::new(x, y, a.x2().max(b.x2()) - x, a.y2().max(b.y2()) - y)
}
pub fn iou(a: Rect, b: Rect) -> f64 {
    intersect(a, b).map_or(0., |r| {
        r.area() as f64 / (a.area() + b.area() - r.area()) as f64
    })
}
pub(crate) fn containment(a: Rect, b: Rect) -> f64 {
    if a.area() == 0 {
        0.
    } else {
        intersect(a, b).map_or(0., |r| r.area() as f64 / a.area() as f64)
    }
}
pub(crate) fn clip(r: Rect, w: i32, h: i32) -> Option<Rect> {
    intersect(r, Rect::new(0, 0, w, h)).filter(|r| r.w >= 2 && r.h >= 2)
}
pub(crate) fn expand(r: Rect, p: i32, w: i32, h: i32) -> Rect {
    clip(Rect::new(r.x - p, r.y - p, r.w + 2 * p, r.h + 2 * p), w, h).unwrap_or(r)
}

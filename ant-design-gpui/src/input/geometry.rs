// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Geometry shared by text painting, pointer selection, native IME and accessibility.

use std::ops::Range;

use gpui::{Bounds, Pixels, Point, point, px, size};

/// Which visual position to use when a byte boundary belongs to two rows/runs.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CaretAffinity {
    Upstream,
    #[default]
    Downstream,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CaretLocation {
    pub offset: usize,
    pub affinity: CaretAffinity,
}

/// One visual caret stop; multiple stops can share an offset at a wrap or bidi boundary.
#[derive(Clone, Copy, Debug)]
pub struct CaretStop {
    pub offset: usize,
    pub position: Point<Pixels>,
}

/// A grapheme's visual bounds and source UTF-8 range.
#[derive(Clone, Debug)]
pub struct TextCluster {
    pub range: Range<usize>,
    pub bounds: Bounds<Pixels>,
}

/// All coordinates are relative to the unscrolled text viewport. A snapshot must
/// be discarded when its text revision no longer matches the editing buffer.
#[derive(Clone, Debug)]
pub struct InputGeometry {
    pub revision: u64,
    pub line_height: Pixels,
    pub viewport: Bounds<Pixels>,
    pub scroll: Point<Pixels>,
    pub carets: Vec<CaretStop>,
    pub clusters: Vec<TextCluster>,
    pub right_to_left: bool,
}

impl InputGeometry {
    pub(crate) fn scroll_to_caret(
        &self,
        caret: CaretLocation,
        mut scroll: Point<Pixels>,
    ) -> Point<Pixels> {
        if let Some(position) = self.caret_with_affinity(caret) {
            scroll.x = scroll
                .x
                .min(position.x)
                .max(position.x - self.viewport.size.width + px(2.))
                .max(px(0.));
            scroll.y = scroll
                .y
                .min(position.y)
                .max(position.y + self.line_height - self.viewport.size.height)
                .max(px(0.));
        }
        scroll
    }

    pub fn caret(&self, offset: usize) -> Option<Point<Pixels>> {
        self.caret_with_affinity(CaretLocation {
            offset,
            affinity: CaretAffinity::Downstream,
        })
    }

    pub fn caret_with_affinity(&self, caret: CaretLocation) -> Option<Point<Pixels>> {
        let mut matches = self
            .carets
            .iter()
            .filter(|stop| stop.offset == caret.offset);
        match caret.affinity {
            CaretAffinity::Upstream => matches.next(),
            CaretAffinity::Downstream => matches.next_back(),
        }
        .map(|stop| stop.position)
    }

    fn location(&self, index: usize) -> CaretLocation {
        let stop = self.carets[index];
        CaretLocation {
            offset: stop.offset,
            affinity: if self.carets[index + 1..]
                .iter()
                .any(|s| s.offset == stop.offset)
            {
                CaretAffinity::Upstream
            } else {
                CaretAffinity::Downstream
            },
        }
    }

    pub fn window_point(&self, local: Point<Pixels>) -> Point<Pixels> {
        self.viewport.origin + local - self.scroll
    }

    pub fn local_point(&self, window: Point<Pixels>) -> Point<Pixels> {
        window - self.viewport.origin + self.scroll
    }

    pub fn nearest(&self, position: Point<Pixels>) -> Option<usize> {
        self.hit_test(position).map(|caret| caret.offset)
    }

    pub fn hit_test(&self, position: Point<Pixels>) -> Option<CaretLocation> {
        let line_height = f32::from(self.line_height).max(1.);
        self.carets
            .iter()
            .enumerate()
            .min_by(|(_, a), (_, b)| {
                let score = |stop: &CaretStop| {
                    let row_delta = (f32::from(position.y) / line_height).floor()
                        - (f32::from(stop.position.y) / line_height).round();
                    // Compare rows before x: clicks outside a short row must not
                    // land on a longer adjacent row.
                    (
                        row_delta.abs(),
                        (f32::from(position.x - stop.position.x)).abs(),
                    )
                };
                let (ay, ax) = score(a);
                let (by, bx) = score(b);
                ay.total_cmp(&by).then_with(|| ax.total_cmp(&bx))
            })
            .map(|(index, _)| self.location(index))
    }

    pub fn row_boundary(&self, caret: CaretLocation, end: bool) -> Option<CaretLocation> {
        let position = self.caret_with_affinity(caret)?;
        self.carets
            .iter()
            .enumerate()
            .filter(|(_, stop)| stop.position.y == position.y)
            .min_by(|(_, a), (_, b)| {
                let order = f32::from(a.position.x).total_cmp(&f32::from(b.position.x));
                if end { order.reverse() } else { order }
            })
            .map(|(index, _)| self.location(index))
    }

    pub fn caret_bounds(&self, caret: CaretLocation) -> Option<Bounds<Pixels>> {
        self.caret_with_affinity(caret)
            .map(|point| Bounds::new(self.window_point(point), size(px(1.), self.line_height)))
    }

    pub fn selection_rects(&self, range: Range<usize>) -> Vec<Bounds<Pixels>> {
        if range.is_empty() {
            return self
                .caret(range.start)
                .map(|caret| Bounds::new(self.window_point(caret), size(px(1.), self.line_height)))
                .into_iter()
                .collect();
        }
        let mut rects: Vec<_> = self
            .clusters
            .iter()
            .filter(|cluster| cluster.range.start < range.end && cluster.range.end > range.start)
            .map(|cluster| {
                Bounds::new(
                    self.window_point(cluster.bounds.origin),
                    cluster.bounds.size,
                )
            })
            .collect();
        rects.sort_by(|a, b| {
            f32::from(a.top())
                .total_cmp(&f32::from(b.top()))
                .then_with(|| f32::from(a.left()).total_cmp(&f32::from(b.left())))
        });
        let mut joined: Vec<Bounds<Pixels>> = Vec::with_capacity(rects.len());
        for rect in rects {
            if let Some(previous) = joined.last_mut()
                && previous.top() == rect.top()
                && previous.bottom() == rect.bottom()
                && rect.left() <= previous.right() + px(0.5)
            {
                *previous = Bounds::from_corners(
                    previous.origin,
                    point(previous.right().max(rect.right()), previous.bottom()),
                );
            } else {
                joined.push(rect);
            }
        }
        joined
    }

    pub fn range_bounds(&self, range: Range<usize>) -> Option<Bounds<Pixels>> {
        // Native candidate windows should anchor to the first selected fragment,
        // not a union spanning several rows.
        self.selection_rects(range).into_iter().next()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn disjoint_bidi_selection_does_not_paint_unselected_text() {
        let geometry = InputGeometry {
            right_to_left: false,
            revision: 1,
            line_height: px(20.),
            viewport: Bounds::new(point(px(100.), px(50.)), size(px(80.), px(40.))),
            scroll: point(px(5.), px(0.)),
            carets: vec![CaretStop {
                offset: 0,
                position: point(px(0.), px(0.)),
            }],
            clusters: vec![
                TextCluster {
                    range: 0..1,
                    bounds: Bounds::new(point(px(0.), px(0.)), size(px(10.), px(20.))),
                },
                TextCluster {
                    range: 2..3,
                    bounds: Bounds::new(point(px(10.), px(0.)), size(px(10.), px(20.))),
                },
                TextCluster {
                    range: 1..2,
                    bounds: Bounds::new(point(px(20.), px(0.)), size(px(10.), px(20.))),
                },
            ],
        };
        let rects = geometry.selection_rects(0..2);
        assert_eq!(rects.len(), 2);
        assert_eq!(rects[0].left(), px(95.));
        assert_eq!(rects[1].left(), px(115.));
        assert_eq!(geometry.selection_rects(0..3).len(), 1);
        assert_eq!(geometry.range_bounds(0..0).unwrap().size.width, px(1.));
    }

    #[test]
    fn pointer_hit_testing_uses_visual_order_and_the_clicked_row() {
        let geometry = InputGeometry {
            right_to_left: false,
            revision: 1,
            line_height: px(20.),
            viewport: Bounds::default(),
            scroll: Point::default(),
            carets: vec![
                CaretStop {
                    offset: 0,
                    position: point(px(0.), px(0.)),
                },
                CaretStop {
                    offset: 3,
                    position: point(px(20.), px(0.)),
                },
                CaretStop {
                    offset: 1,
                    position: point(px(40.), px(0.)),
                },
                CaretStop {
                    offset: 4,
                    position: point(px(0.), px(20.)),
                },
                CaretStop {
                    offset: 5,
                    position: point(px(10.), px(20.)),
                },
            ],
            clusters: Vec::new(),
        };
        assert_eq!(geometry.nearest(point(px(19.), px(9.))), Some(3));
        assert_eq!(geometry.nearest(point(px(100.), px(21.))), Some(5));
        assert_eq!(geometry.nearest(point(px(-10.), px(-10.))), Some(0));
    }
}

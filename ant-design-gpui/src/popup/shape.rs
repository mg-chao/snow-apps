// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Rounded arrow geometry adapted from Ant Design 6.6.7 (MIT).

use super::{PopupLayout, runtime::finite_nonnegative};
use gpui::{Path, PathBuilder, Pixels, Point, point, px};

#[derive(Clone, Copy, Debug)]
pub(super) struct ArrowShape {
    pub size: f32,
    pub tip_radius: f32,
    pub outer_radius: f32,
}
impl ArrowShape {
    pub fn new(size: Pixels, tip_radius: Pixels, outer_radius: Pixels) -> Self {
        let size = f32::from(finite_nonnegative(size));
        Self {
            size,
            tip_radius: f32::from(finite_nonnegative(tip_radius)).min(size / 2.),
            outer_radius: f32::from(finite_nonnegative(outer_radius)).min(size / 2.),
        }
    }
    fn points(self) -> [(f32, f32); 6] {
        let half = self.size / 2.;
        let inv_sqrt2 = std::f32::consts::FRAC_1_SQRT_2;
        let b = (
            self.outer_radius * inv_sqrt2,
            half - self.outer_radius * (1. - inv_sqrt2),
        );
        let c = (
            half - self.tip_radius * inv_sqrt2,
            self.outer_radius * (std::f32::consts::SQRT_2 - 1.) + self.tip_radius * inv_sqrt2,
        );
        [
            (0., half),
            b,
            c,
            (self.size - c.0, c.1),
            (self.size - b.0, b.1),
            (self.size, half),
        ]
    }
    pub fn svg_path(self) -> String {
        let [a, b, c, d, e, f] = self.points();
        let outer = self.outer_radius;
        let tip = self.tip_radius;
        format!(
            "M {} {} A {outer} {outer} 0 0 0 {} {} L {} {} A {tip} {tip} 0 0 1 {} {} L {} {} A {outer} {outer} 0 0 0 {} {} Z",
            a.0, a.1, b.0, b.1, c.0, c.1, d.0, d.1, e.0, e.1, f.0, f.1
        )
    }
    pub fn path(self, layout: PopupLayout) -> Option<Path<Pixels>> {
        let edge = layout.bounds.origin + layout.arrow?;
        let transform = |(x, y): (f32, f32)| -> Point<Pixels> {
            let x = px(x - self.size / 2.);
            let y = px(y - self.size / 2.);
            let local = match (layout.placement.is_vertical(), layout.placement.before()) {
                (true, true) => point(-x, -y),
                (true, false) => point(x, y),
                (false, true) => point(-y, x),
                (false, false) => point(y, -x),
            };
            edge + local
        };
        let [a, b, c, d, e, f] = self.points().map(transform);
        let mut path = PathBuilder::fill();
        path.move_to(a);
        path.arc_to(
            point(px(self.outer_radius), px(self.outer_radius)),
            px(0.),
            false,
            false,
            b,
        );
        path.line_to(c);
        path.arc_to(
            point(px(self.tip_radius), px(self.tip_radius)),
            px(0.),
            false,
            true,
            d,
        );
        path.line_to(e);
        path.arc_to(
            point(px(self.outer_radius), px(self.outer_radius)),
            px(0.),
            false,
            false,
            f,
        );
        path.close();
        path.build().ok()
    }
}

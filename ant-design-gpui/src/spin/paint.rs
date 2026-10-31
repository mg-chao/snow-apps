// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Retained filled ring geometry. Round caps belong to the same path, avoiding
//! overlapping alpha during fades and repeated stroke tessellation on warm draws.

use gpui::{
    Bounds, FillOptions, Hsla, Path, PathBuilder, PathStyle, Pixels, Point, Window, point, px,
};

/// Cached high-precision filled circles retain moving subpixel coordinates.
/// GPUI's rounded quads intentionally snap their bounds to device pixels.
#[derive(Default)]
pub(super) struct DotCache {
    diameter: Pixels,
    circle: Option<Path<Pixels>>,
}
impl DotCache {
    pub fn paint(
        &mut self,
        center: Point<Pixels>,
        diameter: Pixels,
        color: Hsla,
        window: &mut Window,
    ) {
        if diameter <= px(0.) {
            return;
        }
        if self.diameter != diameter || self.circle.is_none() {
            self.diameter = diameter;
            let radius = diameter / 2.;
            let mut path = PathBuilder::fill()
                .with_style(PathStyle::Fill(FillOptions::default().with_tolerance(0.01)));
            path.move_to(point(radius, px(0.)));
            path.arc_to(
                point(radius, radius),
                px(0.),
                false,
                true,
                point(-radius, px(0.)),
            );
            path.arc_to(
                point(radius, radius),
                px(0.),
                false,
                true,
                point(radius, px(0.)),
            );
            path.close();
            self.circle = path.build().ok();
        }
        let Some(mut path) = self.circle.clone() else {
            return;
        };
        path.bounds.origin += center;
        for vertex in &mut path.vertices {
            vertex.xy_position += center;
        }
        window.paint_path(path, color);
    }
}

fn annulus(bounds: Bounds<Pixels>, percent: f64) -> Option<Path<Pixels>> {
    if bounds.size.width <= px(0.) || percent <= 0. {
        return None;
    }
    let holder = f32::from(bounds.size.width);
    let outer = holder / 2.;
    let inner = holder * 0.3;
    let cap = holder * 0.1;
    let center = bounds.center();
    let at = |radius: f32, angle: f32| {
        point(
            center.x + px(radius * angle.sin()),
            center.y - px(radius * angle.cos()),
        )
    };
    let radii = |radius| point(px(radius), px(radius));
    let mut path = PathBuilder::fill()
        .with_style(PathStyle::Fill(FillOptions::default().with_tolerance(0.01)));
    path.move_to(at(outer, 0.));
    if percent >= 100. {
        path.arc_to(
            radii(outer),
            px(0.),
            false,
            true,
            at(outer, std::f32::consts::PI),
        );
        path.arc_to(radii(outer), px(0.), false, true, at(outer, 0.));
        path.close();
        path.move_to(at(inner, 0.));
        path.arc_to(
            radii(inner),
            px(0.),
            false,
            false,
            at(inner, std::f32::consts::PI),
        );
        path.arc_to(radii(inner), px(0.), false, false, at(inner, 0.));
    } else {
        let angle = (percent / 100.) as f32 * std::f32::consts::TAU;
        path.arc_to(
            radii(outer),
            px(0.),
            angle > std::f32::consts::PI,
            true,
            at(outer, angle),
        );
        path.arc_to(radii(cap), px(0.), false, true, at(inner, angle));
        path.arc_to(
            radii(inner),
            px(0.),
            angle > std::f32::consts::PI,
            false,
            at(inner, 0.),
        );
        path.arc_to(radii(cap), px(0.), false, true, at(outer, 0.));
    }
    path.close();
    path.build().ok()
}

#[derive(Default)]
pub(super) struct RingCache {
    bounds: Option<Bounds<Pixels>>,
    percent: f64,
    track: Option<Path<Pixels>>,
    progress: Option<Path<Pixels>>,
    #[cfg(test)]
    pub builds: usize,
}

impl RingCache {
    pub fn paint(
        &mut self,
        bounds: Bounds<Pixels>,
        percent: f64,
        color: Hsla,
        track: Hsla,
        window: &mut Window,
    ) {
        if self.bounds != Some(bounds) {
            self.bounds = Some(bounds);
            self.percent = f64::NAN;
            self.track = annulus(bounds, 100.);
        }
        if self.percent != percent {
            self.percent = percent;
            self.progress = annulus(bounds, percent);
            #[cfg(test)]
            {
                self.builds += 1;
            }
        }
        if let Some(path) = &self.track {
            window.paint_path(path.clone(), track);
        }
        if let Some(path) = &self.progress {
            window.paint_path(path.clone(), color);
        }
    }
}

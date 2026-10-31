// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native vector painting. Geometry is local to the element and survives moves.
//! Gradient sections share the original shape's edges; no images or SVG parsing occur.

use super::{ProgressGradient, ProgressLinecap};
use gpui::{
    Background, Bounds, FillOptions, Hsla, Path, PathBuilder, PathStyle, Pixels, Point, Window,
    linear_color_stop, linear_gradient, point, px, size,
};
use std::f32::consts::{PI, TAU};
use std::time::Duration;

#[derive(Clone, Copy)]
pub(super) struct PaintMotion {
    pub now: Duration,
    pub duration: Duration,
    pub curve: crate::theme::CubicBezier,
    pub enabled: bool,
}

#[derive(Clone, Copy)]
struct ColorTransition {
    from: Hsla,
    to: Hsla,
    started: Duration,
}

fn builder() -> PathBuilder {
    PathBuilder::fill().with_style(PathStyle::Fill(
        FillOptions::default()
            .with_tolerance(0.03)
            .with_fill_rule(gpui::FillRule::NonZero),
    ))
}
fn at(radius: f32, angle: f32) -> Point<Pixels> {
    point(px(radius * angle.cos()), px(radius * angle.sin()))
}
fn radii(radius: f32) -> Point<Pixels> {
    point(px(radius), px(radius))
}

/// One filled outline includes its caps: translucent colors never double-blend at joins.
pub(super) fn arc(
    radius: f32,
    width: f32,
    start: f32,
    sweep: f32,
    cap: ProgressLinecap,
) -> Option<Path<Pixels>> {
    if width <= 0. || radius <= 0. || sweep <= 0. {
        return None;
    }
    let sweep = sweep.min(TAU);
    let outer = radius + width / 2.;
    let inner = (radius - width / 2.).max(0.);
    let union_caps =
        cap == ProgressLinecap::Round && sweep < TAU - 1e-6 && (TAU - sweep) * radius < width;
    let mut p = builder();
    let end = start + sweep;
    p.move_to(at(outer, start));
    // Split full circles: equal SVG arc endpoints denote an empty arc.
    if sweep > PI {
        p.arc_to(radii(outer), px(0.), false, true, at(outer, start + PI));
    }
    p.arc_to(radii(outer), px(0.), false, true, at(outer, end));
    if sweep >= TAU - 1e-6 {
        p.close();
        p.move_to(at(inner, start));
        p.arc_to(radii(inner), px(0.), false, false, at(inner, start + PI));
        p.arc_to(radii(inner), px(0.), false, false, at(inner, start));
    } else {
        match cap {
            ProgressLinecap::Round if !union_caps => {
                p.arc_to(radii(width / 2.), px(0.), false, true, at(inner, end))
            }
            ProgressLinecap::Round => p.line_to(at(inner, end)),
            ProgressLinecap::Butt => p.line_to(at(inner, end)),
            ProgressLinecap::Square => {
                let tangent = at(width / 2., end + PI / 2.);
                p.line_to(at(outer, end) + tangent);
                p.line_to(at(inner, end) + tangent);
                p.line_to(at(inner, end));
            }
        }
        if sweep > PI {
            p.arc_to(radii(inner), px(0.), false, false, at(inner, end - PI));
        }
        p.arc_to(radii(inner), px(0.), false, false, at(inner, start));
        match cap {
            ProgressLinecap::Round if !union_caps => {
                p.arc_to(radii(width / 2.), px(0.), false, true, at(outer, start))
            }
            ProgressLinecap::Round => p.line_to(at(outer, start)),
            ProgressLinecap::Butt => p.line_to(at(outer, start)),
            ProgressLinecap::Square => {
                let tangent = at(width / 2., start - PI / 2.);
                p.line_to(at(inner, start) + tangent);
                p.line_to(at(outer, start) + tangent);
                p.line_to(at(outer, start));
            }
        }
    }
    p.close();
    if union_caps {
        // Nonzero winding unions overlapping caps into the same mask. This also
        // prevents translucent colors from blending twice near completion.
        for angle in [start, end] {
            let center = at(radius, angle);
            let offset = point(px(width / 2.), px(0.));
            p.move_to(center + offset);
            p.arc_to(radii(width / 2.), px(0.), false, true, center - offset);
            p.arc_to(radii(width / 2.), px(0.), false, true, center + offset);
            p.close();
        }
    }
    p.build().ok()
}

pub(super) fn rounded_rect(width: f32, height: f32, radius: f32) -> Option<Path<Pixels>> {
    if width <= 0. || height <= 0. {
        return None;
    }
    let r = radius.min(width / 2.).min(height / 2.).max(0.);
    let mut p = builder();
    p.move_to(point(px(r), px(0.)));
    p.line_to(point(px(width - r), px(0.)));
    if r > 0. {
        p.arc_to(radii(r), px(0.), false, true, point(px(width), px(r)));
    }
    p.line_to(point(px(width), px(height - r)));
    if r > 0. {
        p.arc_to(
            radii(r),
            px(0.),
            false,
            true,
            point(px(width - r), px(height)),
        );
    }
    p.line_to(point(px(r), px(height)));
    if r > 0. {
        p.arc_to(radii(r), px(0.), false, true, point(px(0.), px(height - r)));
    }
    p.line_to(point(px(0.), px(r)));
    if r > 0. {
        p.arc_to(radii(r), px(0.), false, true, point(px(r), px(0.)));
    }
    p.close();
    p.build().ok()
}

pub(super) fn translate(mut path: Path<Pixels>, offset: Point<Pixels>) -> Path<Pixels> {
    path.bounds.origin += offset;
    for vertex in &mut path.vertices {
        vertex.xy_position += offset;
    }
    path
}

pub(super) fn compound<'a>(paths: impl Iterator<Item = &'a Path<Pixels>>) -> Option<Path<Pixels>> {
    let mut result: Option<Path<Pixels>> = None;
    for path in paths {
        if let Some(result) = &mut result {
            result.bounds = result.bounds.union(&path.bounds);
            result.vertices.extend_from_slice(&path.vertices);
        } else {
            result = Some(path.clone());
        }
    }
    result
}

pub(super) fn color_at(gradient: &ProgressGradient, t: f32, fallback: Hsla) -> Hsla {
    let stops = gradient.stops();
    let Some(first) = stops.first() else {
        return fallback;
    };
    if t < first.0 {
        return first.1;
    }
    let index = stops.partition_point(|(position, _)| *position <= t);
    let Some(&(b, cb)) = stops.get(index) else {
        return stops.last().unwrap().1;
    };
    let (a, ca) = stops[index.saturating_sub(1)];
    let amount = ((t - a) / (b - a)).clamp(0., 1.);
    mix(ca, cb, amount)
}

fn mix(ca: Hsla, cb: Hsla, amount: f32) -> Hsla {
    let ca = gpui::Rgba::from(ca);
    let cb = gpui::Rgba::from(cb);
    let alpha = ca.a + (cb.a - ca.a) * amount;
    let channel = |a: f32, b: f32| {
        if alpha > 0. {
            (a * ca.a * (1. - amount) + b * cb.a * amount) / alpha
        } else {
            0.
        }
    };
    gpui::Rgba {
        r: channel(ca.r, cb.r),
        g: channel(ca.g, cb.g),
        b: channel(ca.b, cb.b),
        a: alpha,
    }
    .into()
}

#[derive(Clone, Debug, PartialEq)]
pub(super) enum Brush {
    Native(Background),
    Linear(ProgressGradient, f32),
    Conic(ProgressGradient, f32, f32),
}
impl Brush {
    pub fn solid(color: Hsla) -> Self {
        Self::Native(color.into())
    }
}

struct Section {
    path: Path<Pixels>,
    fill: Background,
}

/// Clip a tessellated fill against a half-plane. Shared triangle edges stay exact.
fn clip(path: &Path<Pixels>, normal: Point<f32>, threshold: f32, keep_above: bool) -> Path<Pixels> {
    let mut out = Path::new(Point::default());
    let distance = |p: Point<Pixels>| {
        let d = f32::from(p.x) * normal.x + f32::from(p.y) * normal.y - threshold;
        if keep_above { d } else { -d }
    };
    for triangle in path.vertices.as_chunks::<3>().0 {
        let mut points = [Point::default(); 4];
        let mut count = 0;
        for i in 0..3 {
            let a = triangle[i].xy_position;
            let b = triangle[(i + 1) % 3].xy_position;
            let da = distance(a);
            let db = distance(b);
            if da >= 0. {
                points[count] = a;
                count += 1;
            }
            if (da >= 0.) != (db >= 0.) {
                points[count] = a + (b - a) * (da / (da - db));
                count += 1;
            }
        }
        for i in 1..count.saturating_sub(1) {
            out.push_triangle(
                (points[0], points[i], points[i + 1]),
                (point(0., 1.), point(0., 1.), point(0., 1.)),
            );
        }
    }
    out
}

fn linear_sections(path: &Path<Pixels>, gradient: &ProgressGradient, angle: f32) -> Vec<Section> {
    let bounds = path.bounds;
    let (sin, cos) = (angle - 90.).to_radians().sin_cos();
    let direction = point(cos, sin);
    let center = bounds.center();
    let half =
        (f32::from(bounds.size.width) * cos.abs() + f32::from(bounds.size.height) * sin.abs()) / 2.;
    if half <= 0. {
        return Vec::new();
    }
    let middle = f32::from(center.x) * cos + f32::from(center.y) * sin;
    let mut positions: Vec<f32> = vec![0.];
    positions.extend(gradient.stops().iter().map(|s| s.0));
    if gradient.stops().windows(2).any(|s| s[0].1.a != s[1].1.a) {
        // GPUI interpolates straight RGBA; CSS interpolates premultiplied color.
        // Short linear spans approximate that curve without raster assets.
        positions.extend((1..64).map(|i| i as f32 / 64.));
    }
    positions.push(1.);
    positions.sort_by(f32::total_cmp);
    positions.dedup();
    positions
        .windows(2)
        .filter_map(|range| {
            let a = range[0];
            let b = range[1];
            if b <= a {
                return None;
            }
            let section = clip(
                &clip(path, direction, middle - half + 2. * half * a, true),
                direction,
                middle - half + 2. * half * b,
                false,
            );
            if section.vertices.is_empty() {
                return None;
            }
            // A square bounds makes GPUI's projected gradient equal CSS's gradient line.
            let mut section = section;
            section.bounds = Bounds::new(
                center - point(px(half), px(half)),
                size(px(2. * half), px(2. * half)),
            );
            Some(Section {
                path: section,
                fill: linear_gradient(
                    angle,
                    linear_color_stop(color_at(gradient, a, gpui::black()), a),
                    linear_color_stop(color_at(gradient, b - 1e-7, gpui::black()), b),
                ),
            })
        })
        .collect()
}

fn conic_sections(
    path: &Path<Pixels>,
    gradient: &ProgressGradient,
    from: f32,
    span: f32,
) -> Vec<Section> {
    // The reference floors mapped conic stops to whole percentages of a turn.
    let mapped = ProgressGradient::new(
        gradient
            .stops()
            .iter()
            .map(|(t, c)| (((t * span / TAU * 100.).floor() / 100.).clamp(0., 1.), *c)),
    );
    let (inner, outer) = path
        .vertices
        .iter()
        .fold((f32::INFINITY, 0_f32), |(inner, outer), v| {
            let radius = f32::from(v.xy_position.x).hypot(f32::from(v.xy_position.y));
            (inner.min(radius), outer.max(radius))
        });
    if !inner.is_finite() || outer <= 0. {
        return Vec::new();
    }
    let radius = (inner + outer) / 2.;
    let mut full_bounds = path.clone();
    full_bounds.bounds = Bounds::new(
        point(px(-outer), px(-outer)),
        size(px(outer * 2.), px(outer * 2.)),
    );
    // rc-progress paints a linear background below its conic background. Both
    // participate in alpha compositing; this is observable for translucent stops.
    let translucent = gradient.stops().iter().any(|(_, color)| color.a < 1.);
    let mut result = if translucent {
        linear_sections(&full_bounds, gradient, if span < TAU { 180. } else { 0. })
    } else {
        Vec::new()
    };
    // Split at stops and at eight degrees. Tangential gradients remove banding
    // while bounding the angular approximation and scene primitive count.
    let mut divisions: Vec<f32> = (0..=48).map(|i| i as f32 / 48.).collect();
    divisions.extend(mapped.stops().iter().map(|s| s.0));
    divisions.sort_by(f32::total_cmp);
    divisions.dedup();
    for range in divisions.windows(2) {
        let a = from + range[0] * TAU;
        let b = from + range[1] * TAU;
        let n1 = point(-a.sin(), a.cos());
        let n2 = point(-b.sin(), b.cos());
        let mut section = clip(&clip(path, n1, 0., true), n2, 0., false);
        if section.vertices.is_empty() {
            continue;
        }
        let mid = (a + b) / 2.;
        let extent = outer.max(1.);
        section.bounds = Bounds::new(
            point(px(-extent), px(-extent)),
            size(px(extent * 2.), px(extent * 2.)),
        );
        let delta = (b - a) / 2.;
        let t0 = 0.5 - radius / extent * delta.sin() / 2.;
        let t1 = 0.5 + radius / extent * delta.sin() / 2.;
        result.push(Section {
            path: section,
            fill: linear_gradient(
                mid.to_degrees() + 180.,
                linear_color_stop(color_at(&mapped, range[0], gpui::black()), t0),
                linear_color_stop(color_at(&mapped, range[1] - 1e-7, gpui::black()), t1),
            ),
        });
    }
    result
}

#[derive(Default)]
pub(super) struct ShapeCache {
    initialized: bool,
    key: Vec<f32>,
    path: Option<Path<Pixels>>,
    brush: Option<Brush>,
    target_brush: Option<Brush>,
    color_transition: Option<ColorTransition>,
    sections: Vec<Section>,
    pub builds: usize,
}
impl ShapeCache {
    pub fn path(&self) -> Option<&Path<Pixels>> {
        self.path.as_ref()
    }
    pub fn brush(&self) -> Option<&Brush> {
        self.brush.as_ref()
    }
    pub fn prepare_motion(
        &mut self,
        key: &[f32],
        build: impl FnOnce() -> Option<Path<Pixels>>,
        target: Brush,
        motion: PaintMotion,
    ) -> bool {
        let solid = |brush: &Brush| match brush {
            Brush::Native(fill) => fill.as_solid(),
            _ => None,
        };
        if self.target_brush.as_ref() != Some(&target) {
            self.color_transition = self
                .brush
                .as_ref()
                .and_then(solid)
                .zip(solid(&target))
                .filter(|(from, to)| from != to)
                .map(|(from, to)| ColorTransition {
                    from,
                    to,
                    started: motion.now,
                });
            self.target_brush = Some(target.clone());
        }
        let mut brush = target;
        if let Some(transition) = self.color_transition {
            let elapsed = motion.now.saturating_sub(transition.started);
            if motion.enabled && elapsed < motion.duration {
                let amount = crate::behavior::evaluate_cubic_bezier(
                    motion.curve,
                    elapsed.as_secs_f64() / motion.duration.as_secs_f64(),
                );
                brush = Brush::solid(mix(transition.from, transition.to, amount as f32));
            } else {
                self.color_transition = None;
            }
        }
        self.prepare(key, build, brush);
        self.color_transition.is_some()
    }
    pub fn prepare(
        &mut self,
        key: &[f32],
        build: impl FnOnce() -> Option<Path<Pixels>>,
        brush: Brush,
    ) {
        let changed = !self.initialized || self.key != key;
        if changed {
            self.initialized = true;
            self.key.clear();
            self.key.extend_from_slice(key);
            self.path = build();
            self.builds += 1;
        }
        if changed || self.brush.as_ref() != Some(&brush) {
            self.sections.clear();
            if let Some(path) = &self.path {
                self.sections = match &brush {
                    Brush::Native(_) => Vec::new(),
                    Brush::Linear(gradient, angle) => linear_sections(path, gradient, *angle),
                    Brush::Conic(gradient, from, span) => {
                        conic_sections(path, gradient, *from, *span)
                    }
                };
            }
            self.brush = Some(brush);
        }
    }
    pub fn paint(&self, origin: Point<Pixels>, opacity: f32, window: &mut Window) {
        if opacity <= 0. {
            return;
        }
        if let (Some(path), Some(Brush::Native(fill))) = (&self.path, &self.brush) {
            window.paint_path(translate(path.clone(), origin), fill.opacity(opacity));
        } else {
            for section in &self.sections {
                window.paint_path(
                    translate(section.path.clone(), origin),
                    section.fill.opacity(opacity),
                );
            }
        }
    }
}

// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native vector color surfaces. Only gradient geometry is retained; color
//! edits update brushes without creating images, uploading textures or laying
//! out a grid of child elements.

use super::{ColorPickerValue, ColorStop, PickerColor};
use crate::theme::Color;
use gpui::{
    Bounds, Hsla, Path, PathBuilder, Pixels, Point, Rgba, Window, checkerboard, fill,
    linear_color_stop, linear_gradient, outline, point, px, size,
};
use std::{cell::RefCell, sync::Arc};

#[derive(Clone, Copy)]
pub(super) struct SurfaceAppearance {
    pub radius: f32,
    pub checker: Color,
    pub inset: Color,
}

pub(super) fn paint_color(
    bounds: Bounds<Pixels>,
    value: &ColorPickerValue,
    appearance: SurfaceAppearance,
    window: &mut Window,
) {
    let SurfaceAppearance {
        radius,
        checker,
        inset,
    } = appearance;
    if bounds.is_empty() {
        return;
    }
    window.paint_quad(
        fill(
            bounds,
            checkerboard(
                checker,
                checker_cell_physical(f32::from(bounds.size.width) / 4., window.scale_factor()),
            ),
        )
        .corner_radii(px(radius)),
    );
    paint_surface_inset(bounds, radius, inset, window);
    match value {
        ColorPickerValue::Cleared => {}
        ColorPickerValue::Single(color) => {
            window.paint_quad(fill(bounds, color.to_rgba()).corner_radii(px(radius)))
        }
        ColorPickerValue::Gradient(stops) => paint_gradient(bounds, stops, radius, window),
    }
    paint_inner_border(bounds, radius, checker, window);
}

pub(super) fn paint_gradient_rail(
    bounds: Bounds<Pixels>,
    value: &ColorPickerValue,
    appearance: SurfaceAppearance,
    window: &mut Window,
) {
    let SurfaceAppearance {
        radius,
        checker,
        inset,
    } = appearance;
    if bounds.is_empty() {
        return;
    }
    window.paint_quad(
        fill(
            bounds,
            checkerboard(
                checker,
                checker_cell_physical(f32::from(bounds.size.height) / 2., window.scale_factor()),
            ),
        )
        .corner_radii(px(radius)),
    );
    match value {
        ColorPickerValue::Cleared => {}
        ColorPickerValue::Single(color) => {
            window.paint_quad(fill(bounds, color.to_rgba()).corner_radii(px(radius)))
        }
        ColorPickerValue::Gradient(stops) => paint_gradient(bounds, stops, radius, window),
    }
    paint_surface_inset(bounds, radius, inset, window);
}

fn checker_cell_physical(logical: f32, scale: f32) -> f32 {
    // Unlike quad bounds/radii, GPUI Background pattern sizes are passed to the
    // shader unchanged. Its position is already measured in physical pixels.
    logical * scale
}

pub(super) fn paint_palette(
    bounds: Bounds<Pixels>,
    color: PickerColor,
    appearance: SurfaceAppearance,
    window: &mut Window,
) {
    let SurfaceAppearance { radius, inset, .. } = appearance;
    if bounds.is_empty() {
        return;
    }
    let hue = color.with_saturation(1.).with_brightness(1.).with_alpha(1.);
    window.paint_quad(fill(bounds, hue.to_rgba()).corner_radii(px(radius)));
    window.paint_quad(
        fill(
            bounds,
            linear_gradient(
                90.,
                linear_color_stop(gpui::white(), 0.),
                linear_color_stop(gpui::white().opacity(0.), 1.),
            ),
        )
        .corner_radii(px(radius)),
    );
    window.paint_quad(
        fill(
            bounds,
            linear_gradient(
                180.,
                linear_color_stop(gpui::black().opacity(0.), 0.),
                linear_color_stop(gpui::black(), 1.),
            ),
        )
        .corner_radii(px(radius)),
    );
    paint_surface_inset(bounds, radius, inset, window);
}

pub(super) fn paint_hue(
    bounds: Bounds<Pixels>,
    appearance: SurfaceAppearance,
    window: &mut Window,
) {
    let SurfaceAppearance { radius, inset, .. } = appearance;
    let values = [
        (0., 0.),
        (60., 17.),
        (120., 33.),
        (180., 50.),
        (240., 67.),
        (300., 83.),
        (360., 100.),
    ];
    let stops = values.map(|(h, percent)| ColorStop {
        color: PickerColor::new(h, 1., 1., 1.).unwrap(),
        percent,
    });
    paint_gradient(bounds, &stops, radius, window);
    paint_surface_inset(bounds, radius, inset, window);
}

pub(super) fn paint_alpha(
    bounds: Bounds<Pixels>,
    color: PickerColor,
    appearance: SurfaceAppearance,
    window: &mut Window,
) {
    let SurfaceAppearance {
        radius,
        checker,
        inset,
    } = appearance;
    if bounds.is_empty() {
        return;
    }
    window.paint_quad(
        fill(
            bounds,
            checkerboard(checker, checker_cell_physical(4., window.scale_factor())),
        )
        .corner_radii(px(radius)),
    );
    window.paint_quad(
        fill(
            bounds,
            linear_gradient(
                90.,
                linear_color_stop(Hsla::from(color.with_alpha(0.)), 0.),
                linear_color_stop(Hsla::from(color.with_alpha(1.)), 1.),
            ),
        )
        .corner_radii(px(radius)),
    );
    paint_surface_inset(bounds, radius, inset, window);
}

#[derive(Clone, Copy)]
pub(super) struct ThumbAppearance {
    pub rim: Color,
    pub outline: Color,
    pub active_outline: Color,
    pub active: bool,
    pub scale: f32,
}

pub(super) fn paint_thumb(
    center: Point<Pixels>,
    diameter: f32,
    color: PickerColor,
    appearance: ThumbAppearance,
    window: &mut Window,
) {
    let ThumbAppearance {
        rim,
        outline: outline_color,
        active_outline,
        active,
        scale,
    } = appearance;
    let Some(geometry) = ThumbGeometry::new(center, diameter, scale) else {
        return;
    };
    let bounds = geometry.bounds;
    let diameter = f32::from(bounds.size.width);
    let shadow = if active {
        active_outline
    } else {
        outline_color
    };
    window.paint_quad(
        outline(
            bounds.dilate(geometry.outline_width),
            shadow,
            Default::default(),
        )
        .border_widths(geometry.outline_width)
        .corner_radii(px(diameter / 2.) + geometry.outline_width),
    );
    window.paint_quad(
        fill(bounds, color.to_rgba())
            .corner_radii(px(diameter / 2.))
            .border_widths(geometry.rim_width)
            .border_color(rim),
    );
}

struct ThumbGeometry {
    bounds: Bounds<Pixels>,
    rim_width: Pixels,
    outline_width: Pixels,
}
impl ThumbGeometry {
    fn new(center: Point<Pixels>, diameter: f32, scale: f32) -> Option<Self> {
        let diameter = diameter * scale;
        if diameter <= 0. || !diameter.is_finite() || scale <= 0. || !scale.is_finite() {
            return None;
        }
        Some(Self {
            bounds: Bounds::new(
                center - point(px(diameter / 2.), px(diameter / 2.)),
                size(px(diameter), px(diameter)),
            ),
            rim_width: px(2. * scale),
            outline_width: px(scale),
        })
    }
}

pub(super) fn paint_clear(
    bounds: Bounds<Pixels>,
    radius: f32,
    border: Color,
    red: Color,
    window: &mut Window,
) {
    if bounds.is_empty() {
        return;
    }
    let Some(shape) = rounded_rect(
        f32::from(bounds.size.width),
        f32::from(bounds.size.height),
        radius,
    ) else {
        return;
    };
    let width = f32::from(bounds.size.width);
    // The source uses a fixed 40×2px line rotated -45 degrees at the top-right.
    // Intersect the diagonal band with the actual rounded outer outline.
    let normal = std::f32::consts::FRAC_1_SQRT_2;
    let offset = width * normal;
    let slash = clip(
        &clip(&shape, point(normal, normal), offset - 1., true),
        point(normal, normal),
        offset + 1.,
        false,
    );
    if !slash.vertices.is_empty() {
        window.paint_path(translate(slash, bounds.origin), red.to_hsla());
    }
    // Both the slash and outline share the same round clipping geometry.
    window.paint_quad(outline(bounds, border, Default::default()).corner_radii(px(radius)));
}

pub(super) fn paint_surface_inset(
    bounds: Bounds<Pixels>,
    radius: f32,
    color: Color,
    window: &mut Window,
) {
    paint_surface_inset_corners(bounds, gpui::Corners::all(px(radius)), color, window);
}

pub(super) fn paint_surface_inset_corners(
    bounds: Bounds<Pixels>,
    radii: gpui::Corners<Pixels>,
    color: Color,
    window: &mut Window,
) {
    window.paint_inset_shadows(
        bounds,
        radii,
        &[gpui::BoxShadow {
            color: color.to_hsla(),
            offset: Point::default(),
            blur_radius: px(1.),
            spread_radius: px(0.),
            inset: true,
        }],
    );
}

fn paint_inner_border(bounds: Bounds<Pixels>, radius: f32, color: Color, window: &mut Window) {
    window.paint_quad(outline(bounds, color, Default::default()).corner_radii(px(radius)));
}

#[derive(Clone, Debug, PartialEq)]
struct GeometryKey {
    width: f32,
    height: f32,
    radius: f32,
    positions: Vec<f32>,
}
#[derive(Clone)]
struct Section {
    path: Path<Pixels>,
    start: f32,
    end: f32,
}
struct CachedGeometry {
    key: GeometryKey,
    sections: Arc<[Section]>,
}
thread_local! {static GEOMETRY:RefCell<Vec<CachedGeometry>>=const {RefCell::new(Vec::new())};}

fn paint_gradient(bounds: Bounds<Pixels>, stops: &[ColorStop], radius: f32, window: &mut Window) {
    if bounds.is_empty() || stops.is_empty() {
        return;
    }
    if stops.len() == 1 {
        window.paint_quad(fill(bounds, stops[0].color.to_rgba()).corner_radii(px(radius)));
        return;
    }
    let width = f32::from(bounds.size.width);
    let height = f32::from(bounds.size.height);
    let mut positions = vec![0., 1.];
    // At most 512 sampled boundaries for unusually large externally supplied
    // gradients; ordinary editable gradients preserve every hard-stop edge.
    if stops.len() <= 128 {
        positions.extend(stops.iter().map(|s| (s.percent / 100.) as f32));
    } else {
        positions.extend((1..512).map(|i| i as f32 / 512.));
    }
    if stops
        .windows(2)
        .any(|s| s[0].color.to_hsb().3 != s[1].color.to_hsb().3)
    {
        positions.extend((1..64).map(|i| i as f32 / 64.));
    }
    positions.sort_by(f32::total_cmp);
    positions.dedup();
    let key = GeometryKey {
        width,
        height,
        radius,
        positions,
    };
    let sections = GEOMETRY.with(|cache| {
        let mut cache = cache.borrow_mut();
        if let Some(index) = cache.iter().position(|c| c.key == key) {
            let entry = cache.remove(index);
            let sections = entry.sections.clone();
            cache.push(entry);
            return sections;
        }
        let sections: Arc<[Section]> = make_sections(&key).into();
        if cache.len() >= 32 {
            cache.remove(0);
        }
        cache.push(CachedGeometry {
            key,
            sections: sections.clone(),
        });
        sections
    });
    for section in sections.iter() {
        let start = color_at(stops, section.start as f64 * 100., false);
        let end = color_at(stops, section.end as f64 * 100., true);
        let background = linear_gradient(
            90.,
            linear_color_stop(Hsla::from(start), section.start),
            linear_color_stop(Hsla::from(end), section.end),
        );
        window.paint_path(translate(section.path.clone(), bounds.origin), background);
    }
}

fn make_sections(key: &GeometryKey) -> Vec<Section> {
    let Some(shape) = rounded_rect(key.width, key.height, key.radius) else {
        return Vec::new();
    };
    key.positions
        .windows(2)
        .filter_map(|p| {
            if p[1] <= p[0] {
                return None;
            }
            let mut path = clip(
                &clip(&shape, point(1., 0.), key.width * p[0], true),
                point(1., 0.),
                key.width * p[1],
                false,
            );
            if path.vertices.is_empty() {
                return None;
            }
            // A square projection keeps GPUI's gradient line equal to CSS's width,
            // including very short (8px) rails.
            path.bounds = Bounds::new(
                point(px(0.), px((key.height - key.width) / 2.)),
                size(px(key.width), px(key.width)),
            );
            Some(Section {
                path,
                start: p[0],
                end: p[1],
            })
        })
        .collect()
}

fn color_at(stops: &[ColorStop], percent: f64, left: bool) -> Rgba {
    let index = stops.partition_point(|s| {
        if left {
            s.percent < percent
        } else {
            s.percent <= percent
        }
    });
    if index == 0 {
        return stops[0].color.to_rgba();
    }
    if index >= stops.len() {
        return stops[stops.len() - 1].color.to_rgba();
    }
    let (a, b) = (stops[index - 1], stops[index]);
    let span = b.percent - a.percent;
    if span <= 0. {
        return b.color.to_rgba();
    }
    let t = ((percent - a.percent) / span).clamp(0., 1.) as f32;
    let (a, b) = (a.color.to_rgba(), b.color.to_rgba());
    let alpha = a.a + (b.a - a.a) * t;
    let channel = |x: f32, y: f32| {
        if alpha > 0. {
            (x * a.a * (1. - t) + y * b.a * t) / alpha
        } else {
            0.
        }
    };
    Rgba {
        r: channel(a.r, b.r),
        g: channel(a.g, b.g),
        b: channel(a.b, b.b),
        a: alpha,
    }
}

fn rounded_rect(width: f32, height: f32, radius: f32) -> Option<Path<Pixels>> {
    if width <= 0. || height <= 0. || !width.is_finite() || !height.is_finite() {
        return None;
    }
    let r = radius.min(width / 2.).min(height / 2.).max(0.);
    let mut p = PathBuilder::fill();
    p.move_to(point(px(r), px(0.)));
    p.line_to(point(px(width - r), px(0.)));
    if r > 0. {
        p.arc_to(
            point(px(r), px(r)),
            px(0.),
            false,
            true,
            point(px(width), px(r)),
        );
    }
    p.line_to(point(px(width), px(height - r)));
    if r > 0. {
        p.arc_to(
            point(px(r), px(r)),
            px(0.),
            false,
            true,
            point(px(width - r), px(height)),
        );
    }
    p.line_to(point(px(r), px(height)));
    if r > 0. {
        p.arc_to(
            point(px(r), px(r)),
            px(0.),
            false,
            true,
            point(px(0.), px(height - r)),
        );
    }
    p.line_to(point(px(0.), px(r)));
    if r > 0. {
        p.arc_to(
            point(px(r), px(r)),
            px(0.),
            false,
            true,
            point(px(r), px(0.)),
        );
    }
    p.close();
    p.build().ok()
}
fn translate(mut path: Path<Pixels>, offset: Point<Pixels>) -> Path<Pixels> {
    path.bounds.origin += offset;
    for vertex in &mut path.vertices {
        vertex.xy_position += offset;
    }
    path
}
fn clip(path: &Path<Pixels>, normal: Point<f32>, threshold: f32, above: bool) -> Path<Pixels> {
    let mut out = Path::new(Point::default());
    let distance = |p: Point<Pixels>| {
        let d = f32::from(p.x) * normal.x + f32::from(p.y) * normal.y - threshold;
        if above { d } else { -d }
    };
    for triangle in path.vertices.as_chunks::<3>().0 {
        let mut points = [Point::default(); 4];
        let mut count = 0;
        for i in 0..3 {
            let (a, b) = (triangle[i].xy_position, triangle[(i + 1) % 3].xy_position);
            let (da, db) = (distance(a), distance(b));
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

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn checkerboard_cells_follow_device_scale_for_swatches_and_rails() {
        for scale in [1., 1.5, 2.] {
            assert_eq!(checker_cell_physical(24. / 4., scale), 6. * scale);
            assert_eq!(checker_cell_physical(8. / 2., scale), 4. * scale);
        }
    }
    #[test]
    fn inactive_gradient_thumb_scales_entire_rim_and_outline() {
        let center = point(px(20.), px(30.));
        let inactive = ThumbGeometry::new(center, 12., 0.8).unwrap();
        assert!((f32::from(inactive.bounds.size.width) - 9.6).abs() < 1e-5);
        assert_eq!(inactive.bounds.center(), center);
        assert_eq!(inactive.rim_width, px(1.6));
        assert_eq!(inactive.outline_width, px(0.8));
        let active = ThumbGeometry::new(center, 12., 1.).unwrap();
        assert_eq!(active.bounds.size, size(px(12.), px(12.)));
        assert_eq!(active.rim_width, px(2.));
        assert_eq!(active.outline_width, px(1.));
        let palette = ThumbGeometry::new(center, 16., 1.).unwrap();
        assert_eq!(palette.rim_width, px(2.));
        assert!(ThumbGeometry::new(center, 12., f32::NAN).is_none());
    }
    #[test]
    fn transparent_gradient_interpolates_premultiplied_color() {
        let stops = [
            ColorStop {
                color: "#f000".parse().unwrap(),
                percent: 0.,
            },
            ColorStop {
                color: "blue".parse().unwrap(),
                percent: 100.,
            },
        ];
        let c = color_at(&stops, 50., false);
        assert_eq!(c.r, 0.);
        assert_eq!(c.b, 1.);
        assert_eq!(c.a, 0.5);
    }
    #[test]
    fn duplicate_stops_have_distinct_left_and_right_colors() {
        let stops = [
            ColorStop {
                color: "red".parse().unwrap(),
                percent: 50.,
            },
            ColorStop {
                color: "blue".parse().unwrap(),
                percent: 50.,
            },
        ];
        assert_eq!(color_at(&stops, 50., true).r, 1.);
        assert_eq!(color_at(&stops, 50., false).b, 1.);
    }
    #[test]
    fn rounded_gradient_sections_keep_global_gradient_projection() {
        let sections = make_sections(&GeometryKey {
            width: 234.,
            height: 8.,
            radius: 4.,
            positions: vec![0., 0.5, 1.],
        });
        assert_eq!(sections.len(), 2);
        assert!(
            sections
                .iter()
                .all(|s| s.path.bounds.size == size(px(234.), px(234.)))
        );
        assert!(
            sections
                .iter()
                .flat_map(|s| &s.path.vertices)
                .all(|v| v.xy_position.x >= px(0.)
                    && v.xy_position.x <= px(234.)
                    && v.xy_position.y >= px(0.)
                    && v.xy_position.y <= px(8.))
        );
    }
}

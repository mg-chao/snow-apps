// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Retained rounded border geometry fitted to the reference browser's device-pixel strokes.

use gpui::{Bounds, Hsla, Path, PathBuilder, Pixels, Point, Window, point, px};

#[derive(Clone, Copy, Debug, PartialEq)]
struct BorderGeometry {
    bounds: Bounds<Pixels>,
    radius: Pixels,
    width: Pixels,
    device_width: f32,
}

impl BorderGeometry {
    fn new(bounds: Bounds<Pixels>, radius: Pixels, width: Pixels, scale: f32) -> Option<Self> {
        if !scale.is_finite() || scale <= 0. {
            return None;
        }
        let values = [
            bounds.origin.x,
            bounds.origin.y,
            bounds.size.width,
            bounds.size.height,
            radius,
            width,
        ];
        if values.iter().any(|value| !f32::from(*value).is_finite())
            || bounds.size.width <= px(0.)
            || bounds.size.height <= px(0.)
            || width <= px(0.)
            || width >= bounds.size.width.min(bounds.size.height)
        {
            return None;
        }
        // CSS borders snap down to whole device pixels, retaining a minimum
        // one-pixel stroke. This also bounds tiny positive custom widths.
        let device_width = (f32::from(width) * scale).floor().max(1.);
        let width = px(device_width / scale);
        if !device_width.is_finite() || width >= bounds.size.width.min(bounds.size.height) {
            return None;
        }
        let right = bounds.origin.x + bounds.size.width;
        let bottom = bounds.origin.y + bounds.size.height;
        if !f32::from(right).is_finite()
            || !f32::from(bottom).is_finite()
            || right <= bounds.origin.x
            || bottom <= bounds.origin.y
        {
            return None;
        }
        // GPUI's path tessellator uses 16-bit indices. Bound malformed, almost
        // zero-width strokes before its repeated-dash walker can allocate an
        // unbounded path or lose progress through floating-point rounding.
        let perimeter = 2.
            * (f64::from(f32::from(bounds.size.width)) + f64::from(f32::from(bounds.size.height)));
        let dash_segments = perimeter / (3. * f64::from(f32::from(width)));
        if dash_segments > 8192.
            || !dash_segments.is_finite()
            || perimeter > f64::from(f32::MAX)
            || !(f32::from(width) * 3.).is_finite()
        {
            return None;
        }
        Some(Self {
            bounds,
            radius: radius
                .max(px(0.))
                .min(bounds.size.width.min(bounds.size.height) / 2.),
            width,
            device_width,
        })
    }

    fn centerline(&self) -> (Pixels, Pixels, Pixels, Pixels, Pixels) {
        let inset = self.width / 2.;
        (
            self.bounds.origin.x + inset,
            self.bounds.origin.y + inset,
            self.bounds.origin.x + self.bounds.size.width - inset,
            self.bounds.origin.y + self.bounds.size.height - inset,
            (self.radius - inset).max(px(0.)),
        )
    }

    fn dash_pattern(&self) -> (Pixels, Pixels) {
        // The measured browser reference uses longer relative dashes on thin
        // borders. Fit the spaces to an integer number of periods so that a
        // rounded closed border has no partial dash at its seam.
        let (dash_ratio, gap_ratio) = if self.device_width < 3. {
            (3., 2.)
        } else {
            (2., 1.)
        };
        let dash = self.width * dash_ratio;
        let nominal_gap = self.width * gap_ratio;
        let (left, top, right, bottom, radius) = self.centerline();
        let perimeter = 2. * f32::from(right - left + bottom - top - radius * 4.)
            + std::f32::consts::TAU * f32::from(radius);
        let dash_length = f32::from(dash);
        let gap_length = f32::from(nominal_gap);
        let count = (perimeter / (dash_length + gap_length)).floor().max(1.);
        let mut best_gap = gap_length;
        let mut deviation = f32::INFINITY;
        for count in [count, count + 1.] {
            let gap = perimeter / count - dash_length;
            if gap > 0. && (gap - gap_length).abs() < deviation {
                best_gap = gap;
                deviation = (gap - gap_length).abs();
            }
        }
        (dash, px(best_gap))
    }

    fn build(&self) -> Option<Path<Pixels>> {
        let (left, top, right, bottom, radius) = self.centerline();
        if right <= left || bottom <= top {
            return None;
        }
        let (dash, gap) = self.dash_pattern();
        let mut builder = PathBuilder::stroke(self.width).dash_array(&[dash, gap]);
        let start = point(left + radius, top);
        builder.move_to(start);
        builder.line_to(point(right - radius, top));
        corner(&mut builder, radius, point(right, top + radius));
        builder.line_to(point(right, bottom - radius));
        corner(&mut builder, radius, point(right - radius, bottom));
        builder.line_to(point(left + radius, bottom));
        corner(&mut builder, radius, point(left, bottom - radius));
        builder.line_to(point(left, top + radius));
        corner(&mut builder, radius, start);
        builder.close();
        builder
            .build()
            .ok()
            .filter(|path| !path.vertices.is_empty())
    }
}

fn corner(builder: &mut PathBuilder, radius: Pixels, end: Point<Pixels>) {
    if radius > px(0.) {
        builder.arc_to(point(radius, radius), px(0.), false, true, end);
    } else {
        builder.line_to(end);
    }
}

struct CachedBorder {
    geometry: BorderGeometry,
    path: Option<Path<Pixels>>,
}

/// One tessellated border per mounted Button. Foreground transitions reuse its
/// geometry; moving or resizing the control replaces this single cache entry.
#[derive(Default)]
pub(super) struct DashedBorderCache {
    cached: Option<CachedBorder>,
    #[cfg(test)]
    builds: u64,
}

impl DashedBorderCache {
    fn path(
        &mut self,
        bounds: Bounds<Pixels>,
        radius: Pixels,
        width: Pixels,
        scale: f32,
    ) -> Option<&Path<Pixels>> {
        let geometry = BorderGeometry::new(bounds, radius, width, scale)?;
        if self
            .cached
            .as_ref()
            .is_none_or(|cached| cached.geometry != geometry)
        {
            #[cfg(test)]
            {
                self.builds += 1;
            }
            self.cached = Some(CachedBorder {
                geometry,
                path: geometry.build(),
            });
        }
        self.cached.as_ref()?.path.as_ref()
    }

    pub(super) fn paint(
        &mut self,
        bounds: Bounds<Pixels>,
        radius: Pixels,
        width: Pixels,
        color: Hsla,
        window: &mut Window,
    ) {
        if color.a <= 0.
            || [color.h, color.s, color.l, color.a]
                .iter()
                .any(|value| !value.is_finite())
        {
            return;
        }
        if let Some(path) = self.path(bounds, radius, width, window.scale_factor()) {
            window.paint_path(path.clone(), color);
        }
    }

    #[cfg(test)]
    pub(super) fn builds(&self) -> u64 {
        self.builds
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::size;

    fn bounds() -> Bounds<Pixels> {
        Bounds::new(point(px(20.), px(30.)), size(px(80.), px(32.)))
    }

    #[test]
    fn dashed_border_cache_reuses_geometry_and_rebuilds_for_geometry_changes() {
        let mut cache = DashedBorderCache::default();
        let original = cache
            .path(bounds(), px(6.), px(1.), 1.)
            .unwrap()
            .vertices
            .as_ptr();
        assert_eq!(cache.builds(), 1);
        assert_eq!(
            cache
                .path(bounds(), px(6.), px(1.), 1.)
                .unwrap()
                .vertices
                .as_ptr(),
            original
        );
        assert_eq!(cache.builds(), 1);
        cache.path(bounds(), px(8.), px(1.), 1.).unwrap();
        assert_eq!(cache.builds(), 2);
        cache.path(bounds(), px(8.), px(2.), 1.).unwrap();
        assert_eq!(cache.builds(), 3);
        let mut moved = bounds();
        moved.origin.x += px(1.);
        cache.path(moved, px(8.), px(2.), 1.).unwrap();
        assert_eq!(cache.builds(), 4);
        moved.size.width += px(1.);
        cache.path(moved, px(8.), px(2.), 1.).unwrap();
        assert_eq!(cache.builds(), 5);
    }

    #[test]
    fn dashed_border_centerline_preserves_outer_bounds_and_clamps_round_radius() {
        let geometry = BorderGeometry::new(bounds(), px(6.), px(1.), 1.).unwrap();
        assert_eq!(
            geometry.centerline(),
            (px(20.5), px(30.5), px(99.5), px(61.5), px(5.5))
        );
        let circle = BorderGeometry::new(bounds(), px(1000.), px(2.), 1.).unwrap();
        assert_eq!(circle.radius, px(16.));
        assert_eq!(circle.centerline().4, px(15.));
        let path = geometry.build().unwrap();
        for vertex in &path.vertices {
            assert!(vertex.xy_position.x >= px(19.99) && vertex.xy_position.x <= px(100.01));
            assert!(vertex.xy_position.y >= px(29.99) && vertex.xy_position.y <= px(62.01));
        }
    }

    fn covers(path: &Path<Pixels>, x: f32, y: f32) -> bool {
        path.vertices.as_chunks::<3>().0.iter().any(|triangle| {
            let origin = triangle[0].xy_position;
            let second = triangle[1].xy_position;
            let third = triangle[2].xy_position;
            let area = f32::from(second.x - origin.x) * f32::from(third.y - origin.y)
                - f32::from(second.y - origin.y) * f32::from(third.x - origin.x);
            if area.abs() < 0.00001 {
                return false;
            }
            let side = |a: Point<Pixels>, b: Point<Pixels>| {
                f32::from(b.x - a.x) * (y - f32::from(a.y))
                    - f32::from(b.y - a.y) * (x - f32::from(a.x))
            };
            let a = side(triangle[0].xy_position, triangle[1].xy_position);
            let b = side(triangle[1].xy_position, triangle[2].xy_position);
            let c = side(triangle[2].xy_position, triangle[0].xy_position);
            let positive = a >= -0.0001 && b >= -0.0001 && c >= -0.0001;
            let negative = a <= 0.0001 && b <= 0.0001 && c <= 0.0001;
            positive || negative
        })
    }

    #[test]
    fn dashed_border_mesh_matches_thin_and_thick_reference_patterns() {
        for width in [1., 2., 3., 4.] {
            let rectangle = Bounds::new(point(px(0.), px(0.)), size(px(120.), px(40.)));
            let geometry = BorderGeometry::new(rectangle, px(0.), px(width), 1.).unwrap();
            let (dash, gap) = geometry.dash_pattern();
            let (dash, gap) = (f32::from(dash), f32::from(gap));
            assert_eq!(dash, width * if width < 3. { 3. } else { 2. });
            assert!((gap - width * if width < 3. { 2. } else { 1. }).abs() < 0.3);
            let path = geometry.build().unwrap();
            let inset = width / 2.;
            for index in 0..4 {
                let dash_center = inset + index as f32 * (dash + gap) + dash / 2.;
                let gap_center = inset + index as f32 * (dash + gap) + dash + gap / 2.;
                assert!(
                    covers(&path, dash_center, inset),
                    "missing dash at {dash_center}"
                );
                assert!(
                    !covers(&path, gap_center, inset),
                    "gap filled at {gap_center}"
                );
            }
            assert!(
                !covers(&path, 30., width + 0.5),
                "stroke exceeds requested width"
            );
        }
    }

    #[test]
    fn dashed_border_rejects_invalid_or_degenerate_geometry_without_losing_cache() {
        let mut cache = DashedBorderCache::default();
        cache.path(bounds(), px(6.), px(1.), 1.).unwrap();
        for width in [0., -1., f32::NAN, f32::INFINITY, 32.] {
            assert!(cache.path(bounds(), px(6.), px(width), 1.).is_none());
        }
        for invalid in [f32::NAN, f32::INFINITY, -1., 0.] {
            let mut rectangle = bounds();
            rectangle.size.width = px(invalid);
            assert!(cache.path(rectangle, px(6.), px(1.), 1.).is_none());
        }
        assert!(cache.path(bounds(), px(f32::NAN), px(1.), 1.).is_none());
        cache.path(bounds(), px(6.), px(1.), 1.).unwrap();
        assert_eq!(cache.builds(), 1);
    }

    #[test]
    fn dashed_border_snaps_to_device_pixels_and_reselects_pattern_at_display_scale() {
        for (width, scale, physical_width, physical_dash, nominal_gap) in [
            (1., 1., 1., 3., 2.),
            (1., 1.5, 1., 3., 2.),
            (2., 1., 2., 6., 4.),
            (3., 1., 3., 6., 3.),
            (2., 1.5, 3., 6., 3.),
            (3., 1.5, 4., 8., 4.),
        ] {
            let geometry = BorderGeometry::new(bounds(), px(6.), px(width), scale).unwrap();
            let (dash, gap) = geometry.dash_pattern();
            assert_eq!(geometry.device_width, physical_width);
            assert!((f32::from(dash) * scale - physical_dash).abs() < 0.001);
            assert!((f32::from(gap) * scale - nominal_gap).abs() < 0.3);
        }
        let mut cache = DashedBorderCache::default();
        cache.path(bounds(), px(6.), px(1.), 1.).unwrap();
        cache.path(bounds(), px(6.), px(1.), 3.).unwrap();
        assert_eq!(
            cache.builds(),
            2,
            "a device-pixel pattern change invalidates the path"
        );
        let tiny = BorderGeometry::new(bounds(), px(6.), px(f32::MIN_POSITIVE), 1.).unwrap();
        assert_eq!(tiny.width, px(1.));
        for scale in [0., -1., f32::NAN, f32::INFINITY] {
            assert!(BorderGeometry::new(bounds(), px(6.), px(1.), scale).is_none());
        }
    }
}

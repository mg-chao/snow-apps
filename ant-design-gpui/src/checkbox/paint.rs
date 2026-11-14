// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! The Checkbox tick is geometry, not a font glyph or a raster icon. Retain its
//! tiny mesh across idle paints and color changes; no icon cache or asset lookup
//! participates in the control's hot path.

use gpui::{Bounds, Hsla, Path, PathBuilder, Pixels, Point, Window, point, px};

#[derive(Clone, Copy, Debug, PartialEq)]
struct CheckGeometry {
    bounds: Bounds<Pixels>,
    width: Pixels,
    progress: f32,
}

impl CheckGeometry {
    fn new(bounds: Bounds<Pixels>, width: Pixels, progress: f32) -> Option<Self> {
        let values = [
            bounds.origin.x,
            bounds.origin.y,
            bounds.size.width,
            bounds.size.height,
            width,
        ];
        if values.iter().any(|value| !f32::from(*value).is_finite())
            || !progress.is_finite()
            || progress <= 0.
            || width <= px(0.)
            || bounds.size.width <= px(0.)
            || bounds.size.height <= px(0.)
        {
            return None;
        }
        Some(Self {
            bounds,
            width,
            // The easing curve may overshoot slightly when the tick appears.
            progress: progress.min(2.),
        })
    }

    fn points(self) -> [Point<Pixels>; 6] {
        let size = f32::from(self.bounds.size.width.min(self.bounds.size.height));
        // Ant's ::after is a 5/14 by 8/14 rectangle with only its right and
        // bottom borders, rotated 45deg. Reproduce the CSS transform origin and
        // translation exactly rather than approximating it with an icon.
        let width = size * 5. / 14.;
        let height = size * 8. / 14.;
        let stroke = f32::from(self.width).min(width.min(height));
        let anchor = point(
            self.bounds.origin.x + px(size / 4. + width / 2.),
            self.bounds.origin.y + px(size / 2. + height / 2.),
        );
        let transform = |x: f32, y: f32| {
            let x = (x - width) * self.progress;
            let y = (y - height) * self.progress;
            anchor
                + point(
                    px((x - y) * std::f32::consts::FRAC_1_SQRT_2),
                    px((x + y) * std::f32::consts::FRAC_1_SQRT_2),
                )
        };
        [
            transform(width - stroke, 0.),
            transform(width, 0.),
            transform(width, height),
            transform(0., height),
            transform(0., height - stroke),
            transform(width - stroke, height - stroke),
        ]
    }

    fn build(self) -> Option<Path<Pixels>> {
        let points = self.points();
        if points
            .iter()
            .any(|point| !f32::from(point.x).is_finite() || !f32::from(point.y).is_finite())
        {
            return None;
        }
        let mut builder = PathBuilder::fill();
        builder.move_to(points[0]);
        for point in points.into_iter().skip(1) {
            builder.line_to(point);
        }
        builder.close();
        builder
            .build()
            .ok()
            .filter(|path| !path.vertices.is_empty())
    }
}

struct CachedCheck {
    geometry: CheckGeometry,
    path: Option<Path<Pixels>>,
}

#[derive(Default)]
pub(super) struct CheckmarkCache {
    cached: Option<CachedCheck>,
    #[cfg(test)]
    builds: u64,
}

impl CheckmarkCache {
    fn path(
        &mut self,
        bounds: Bounds<Pixels>,
        width: Pixels,
        progress: f32,
    ) -> Option<&Path<Pixels>> {
        // Layout motion and scrolling only translate the mesh. Cache local
        // geometry so they do not rerun the path tessellator.
        let mut geometry = CheckGeometry::new(bounds, width, progress)?;
        geometry.bounds.origin = Point::default();
        if self
            .cached
            .as_ref()
            .is_none_or(|cached| cached.geometry != geometry)
        {
            #[cfg(test)]
            {
                self.builds += 1;
            }
            self.cached = Some(CachedCheck {
                geometry,
                path: geometry.build(),
            });
        }
        self.cached.as_ref()?.path.as_ref()
    }

    pub(super) fn paint(
        &mut self,
        bounds: Bounds<Pixels>,
        width: Pixels,
        progress: f32,
        color: Hsla,
        window: &mut Window,
    ) {
        if color.a <= 0.
            || [color.h, color.s, color.l, color.a]
                .iter()
                .any(|channel| !channel.is_finite())
        {
            return;
        }
        if let Some(path) = self.path(bounds, width, progress) {
            let mut path = path.clone();
            path.bounds.origin += bounds.origin;
            for vertex in &mut path.vertices {
                vertex.xy_position += bounds.origin;
            }
            window.paint_path(path, color);
        }
    }
}

#[cfg(test)]
pub(super) fn checkmark_points(
    bounds: Bounds<Pixels>,
    width: Pixels,
    progress: f32,
) -> Option<[Point<Pixels>; 6]> {
    Some(CheckGeometry::new(bounds, width, progress)?.points())
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::size;

    fn bounds() -> Bounds<Pixels> {
        Bounds::new(point(px(0.), px(0.)), size(px(16.), px(16.)))
    }

    #[test]
    fn checkbox_checkmark_matches_css_border_transform() {
        let geometry = CheckGeometry::new(bounds(), px(2.), 1.).unwrap();
        let points = geometry.points();
        // Independently evaluated CSS transform for the reference 16px box.
        for (point, (x, y)) in points.iter().zip([
            (11.907906, 4.692239),
            (13.322119, 6.106452),
            (6.857143, 12.571428),
            (2.816533, 8.530819),
            (4.230746, 7.116605),
            (6.857143, 9.742999),
        ]) {
            assert!((f32::from(point.x) - x).abs() < 0.001, "{point:?}");
            assert!((f32::from(point.y) - y).abs() < 0.001, "{point:?}");
        }
        assert!(geometry.build().is_some());
    }

    #[test]
    fn checkbox_checkmark_cache_retains_mesh_for_unchanged_geometry() {
        let mut cache = CheckmarkCache::default();
        let original = cache.path(bounds(), px(2.), 1.).unwrap().vertices.as_ptr();
        assert_eq!(cache.builds, 1);
        assert_eq!(
            cache.path(bounds(), px(2.), 1.).unwrap().vertices.as_ptr(),
            original
        );
        assert_eq!(cache.builds, 1);
        cache.path(bounds(), px(2.), 0.5).unwrap();
        assert_eq!(cache.builds, 2);
        cache.path(bounds(), px(3.), 0.5).unwrap();
        assert_eq!(cache.builds, 3);
        let moved = Bounds::new(point(px(3.), px(8.)), bounds().size);
        cache.path(moved, px(3.), 0.5).unwrap();
        assert_eq!(cache.builds, 3, "moving a checkbox reuses its local mesh");
    }

    #[test]
    fn checkbox_checkmark_rejects_invalid_geometry_and_reuses_last_valid_mesh() {
        let mut cache = CheckmarkCache::default();
        cache.path(bounds(), px(2.), 1.).unwrap();
        for invalid in [0., -1., f32::NAN, f32::INFINITY] {
            assert!(cache.path(bounds(), px(invalid), 1.).is_none());
            assert!(cache.path(bounds(), px(2.), invalid).is_none());
            let mut rectangle = bounds();
            rectangle.size.width = px(invalid);
            assert!(cache.path(rectangle, px(2.), 1.).is_none());
        }
        cache.path(bounds(), px(2.), 1.).unwrap();
        assert_eq!(cache.builds, 1);
    }
}

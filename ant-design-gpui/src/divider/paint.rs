// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! A bounded, color-independent rail cache. Scrolling translates cached vertices
//! instead of tessellating again. Solid rails bypass paths entirely.

use gpui::{Bounds, Hsla, Path, PathBuilder, Pixels, Point, Window, fill, point, px, size};

use super::DividerVariant;

// Stay well below GPUI's u16 path index limit, including round-dot tessellation.
const MAX_MARKS: usize = 2048;

#[derive(Clone, Copy, Debug, PartialEq)]
struct Geometry {
    length: f32,
    width: f32,
    vertical: bool,
    variant: DividerVariant,
    scale: f32,
}

impl Geometry {
    fn new(
        bounds: Bounds<Pixels>,
        vertical: bool,
        variant: DividerVariant,
        width: Pixels,
        scale: f32,
    ) -> Option<Self> {
        let length = f32::from(if vertical {
            bounds.size.height
        } else {
            bounds.size.width
        });
        let width = f32::from(width);
        if !scale.is_finite()
            || scale <= 0.
            || !length.is_finite()
            || length <= 0.
            || !width.is_finite()
            || width <= 0.
            || !f32::from(bounds.origin.x).is_finite()
            || !f32::from(bounds.origin.y).is_finite()
        {
            return None;
        }
        let device_width = (width * scale).floor().max(1.);
        let device_length = (length * scale).round().max(1.);
        if !device_width.is_finite() || !device_length.is_finite() || device_length <= 0. {
            return None;
        }
        Some(Self {
            length: device_length / scale,
            width: device_width / scale,
            vertical,
            variant,
            scale,
        })
    }

    fn point(self, along: f32, across: f32) -> Point<Pixels> {
        if self.vertical {
            point(px(across), px(along))
        } else {
            point(px(along), px(across))
        }
    }

    fn dash_pattern(self) -> (f32, f32) {
        if self.width * self.scale < 3. {
            (3. * self.width, 2. * self.width)
        } else {
            (2. * self.width, self.width)
        }
    }

    fn marks(self) -> (usize, f32, f32) {
        let (mark, gap) = if self.variant == DividerVariant::Dotted {
            (self.width, self.width)
        } else {
            self.dash_pattern()
        };
        let round_dots = self.variant == DividerVariant::Dotted && self.width * self.scale > 3.;
        if !round_dots {
            // Browser pixel probes: short strokes become a solid mark, then
            // two proportionally fitted marks before reaching the normal pattern.
            if self.length <= 2. * mark {
                return (1, self.length, 0.);
            }
            if self.length <= 2. * mark + gap {
                let factor = self.length / (2. * mark + gap);
                return (2, mark * factor, gap * factor);
            }
            if self.variant == DividerVariant::Dotted {
                let count = (self.length / (mark + gap)).ceil() as usize;
                if count > MAX_MARKS {
                    return (
                        MAX_MARKS,
                        mark,
                        (self.length - mark * MAX_MARKS as f32) / (MAX_MARKS - 1) as f32,
                    );
                }
                return (count.max(1), mark, gap);
            }
        }
        // Keep the nominal mark length and select the neighboring mark count
        // whose fitted gaps deviate least. Both ends then have a complete mark.
        let lower = (((self.length + gap) / (mark + gap)).floor() as usize).clamp(1, MAX_MARKS);
        let upper = lower.saturating_add(1).min(MAX_MARKS);
        let mut result = (1, mark.min(self.length), 0.);
        let mut deviation = f32::INFINITY;
        for count in [lower, upper] {
            if count <= 1 {
                continue;
            }
            let fitted = (self.length - mark * count as f32) / (count - 1) as f32;
            if fitted > 0. && (fitted - gap).abs() <= deviation {
                deviation = (fitted - gap).abs();
                result = (count, mark, fitted);
            }
        }
        result
    }

    fn build(self) -> Option<Path<Pixels>> {
        let (count, mark, gap) = self.marks();
        let mut builder = PathBuilder::fill();
        for index in 0..count {
            let start = index as f32 * (mark + gap);
            let end = (start + mark).min(self.length);
            if end <= start {
                break;
            }
            if self.variant == DividerVariant::Dotted && self.width * self.scale > 3. {
                let radius = self.width.min(end - start) / 2.;
                let center = start + (end - start) / 2.;
                builder.move_to(self.point(center - radius, self.width / 2.));
                builder.arc_to(
                    point(px(radius), px(radius)),
                    px(0.),
                    false,
                    true,
                    self.point(center + radius, self.width / 2.),
                );
                builder.arc_to(
                    point(px(radius), px(radius)),
                    px(0.),
                    false,
                    true,
                    self.point(center - radius, self.width / 2.),
                );
                builder.close();
            } else {
                builder.add_polygon(
                    &[
                        self.point(start, 0.),
                        self.point(end, 0.),
                        self.point(end, self.width),
                        self.point(start, self.width),
                    ],
                    true,
                );
            }
        }
        builder
            .build()
            .ok()
            .filter(|path| !path.vertices.is_empty())
    }
}

#[derive(Default)]
pub(super) struct RailCache {
    geometry: Option<Geometry>,
    path: Option<Path<Pixels>>,
    #[cfg(test)]
    builds: usize,
}

impl RailCache {
    fn path(&mut self, geometry: Geometry) -> Option<&Path<Pixels>> {
        if self.geometry != Some(geometry) {
            self.geometry = Some(geometry);
            self.path = geometry.build();
            #[cfg(test)]
            {
                self.builds += 1;
            }
        }
        self.path.as_ref()
    }

    pub(super) fn paint(
        &mut self,
        bounds: Bounds<Pixels>,
        vertical: bool,
        variant: DividerVariant,
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
        let Some(geometry) = Geometry::new(bounds, vertical, variant, width, window.scale_factor())
        else {
            return;
        };
        let origin = bounds
            .origin
            .map(|value| px((f32::from(value) * geometry.scale).round() / geometry.scale));
        if variant == DividerVariant::Solid {
            let size = if vertical {
                size(px(geometry.width), px(geometry.length))
            } else {
                size(px(geometry.length), px(geometry.width))
            };
            window.paint_quad(fill(Bounds::new(origin, size), color));
        } else if let Some(path) = self.path(geometry) {
            let mut path = path.clone();
            path.bounds.origin += origin;
            for vertex in &mut path.vertices {
                vertex.xy_position += origin;
            }
            window.paint_path(path, color);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn geometry(variant: DividerVariant, scale: f32) -> Geometry {
        Geometry::new(
            Bounds::new(point(px(10.), px(20.)), size(px(480.), px(1.))),
            false,
            variant,
            px(1.),
            scale,
        )
        .unwrap()
    }

    #[test]
    fn cached_rails_reuse_geometry_and_invalidate_on_length_width_variant_or_scale() {
        let mut cache = RailCache::default();
        let initial = geometry(DividerVariant::Dashed, 1.5);
        let vertices = cache.path(initial).unwrap().vertices.clone();
        assert_eq!(cache.path(initial).unwrap().vertices, vertices);
        let moved = Geometry::new(
            Bounds::new(point(px(25.), px(70.)), size(px(480.), px(1.))),
            false,
            DividerVariant::Dashed,
            px(1.),
            1.5,
        )
        .unwrap();
        assert_eq!(
            cache.path(moved).unwrap().vertices,
            vertices,
            "scrolling reuses local rail geometry"
        );
        assert_eq!(cache.builds, 1);
        for changed in [
            Geometry {
                length: 240.,
                ..initial
            },
            Geometry {
                width: 2.,
                ..initial
            },
            Geometry {
                vertical: true,
                ..initial
            },
            geometry(DividerVariant::Dotted, 1.5),
            geometry(DividerVariant::Dashed, 2.),
        ] {
            assert!(cache.path(changed).is_some());
        }
        assert_eq!(cache.builds, 6);
    }

    #[test]
    fn rail_geometry_rejects_degenerate_and_nonfinite_inputs_and_snaps_thickness() {
        let bounds = Bounds::new(point(px(0.), px(0.)), size(px(50.), px(1.)));
        for width in [0., -1., f32::NAN, f32::INFINITY] {
            assert!(Geometry::new(bounds, false, DividerVariant::Dashed, px(width), 1.).is_none());
        }
        for scale in [0., -1., f32::NAN, f32::INFINITY] {
            assert!(Geometry::new(bounds, false, DividerVariant::Dotted, px(1.), scale).is_none());
        }
        assert_eq!(geometry(DividerVariant::Dashed, 1.5).width, 1. / 1.5);
        assert_eq!(geometry(DividerVariant::Dashed, 2.).width, 1.);
    }

    #[test]
    fn dotted_and_dashed_paths_are_bounded_even_for_extreme_rails() {
        for variant in [DividerVariant::Dashed, DividerVariant::Dotted] {
            let initial = geometry(variant, 1.);
            for length in [0.25, 1., 12.6, 480., 1_000_000.] {
                let geometry = Geometry { length, ..initial };
                assert!(geometry.marks().0 <= MAX_MARKS);
                let path = geometry.build().expect("bounded nonempty path");
                assert!(path.vertices.len() < u16::MAX as usize);
            }
        }
    }

    #[test]
    fn short_rail_dash_runs_match_independent_browser_pixel_probes() {
        // Chromium measurements at 1x: (stroke, rail length, painted runs).
        // The 12.6px vertical rail rasterizes to 13 device pixels.
        for (width, length, expected) in [
            (1., 13., vec![(0., 3.), (5., 8.), (10., 13.)]),
            (2., 13., vec![(0., 5.), (8., 13.)]),
            (3., 13., vec![(0., 5.), (8., 13.)]),
            (5., 13., vec![(0., 13.)]),
            (5., 24., vec![(0., 10.), (14., 24.)]),
        ] {
            let geometry = Geometry {
                width,
                length,
                ..geometry(DividerVariant::Dashed, 1.)
            };
            let (count, mark, gap) = geometry.marks();
            let runs: Vec<_> = (0..count)
                .map(|index| {
                    let start = index as f32 * (mark + gap);
                    (start.round(), (start + mark).min(length).round())
                })
                .collect();
            assert_eq!(runs, expected, "stroke {width}, length {length}");
        }
        let tiny = Geometry::new(
            Bounds::new(point(px(0.), px(0.)), size(px(0.25), px(1.))),
            false,
            DividerVariant::Dashed,
            px(1.),
            1.,
        )
        .unwrap();
        assert_eq!(tiny.length, 1.);
        assert!(tiny.build().is_some());
    }
}

// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Cached alpha-shape shadows. Sequential SVG filters match CSS drop-shadow;
//! body and arrow remain native vector primitives. Moving a surface reuses its
//! raster, and scene leases protect atlas entries across cached view replays.

use super::{PopupLayout, shape::ArrowShape};
use crate::theme::ShadowList;
use gpui::{
    App, BorrowAppContext, Bounds, DevicePixels, Global, Pixels, RenderImage, SvgSize, Window,
    point, px, size,
};
use std::{
    collections::HashMap,
    fmt::Write,
    hash::{Hash, Hasher},
    sync::{Arc, Weak},
};

const MAX_ENTRIES: usize = 64;
const MAX_BYTES: usize = 16 * 1024 * 1024;
const MAX_IMAGE_PIXELS: f32 = 4. * 1024. * 1024.;
// A blurred mask has far less spatial detail than the native body and arrow.
// 2.5 samples per Gaussian standard deviation preserve that detail while
// avoiding full-DPI convolution of large transparent margins. Sharp custom
// shadows retain the device resolution.
const SAMPLES_PER_SIGMA: f32 = 2.5;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PopupShadowStatistics {
    pub entries: usize,
    pub bytes: usize,
    pub hits: u64,
    pub misses: u64,
}

pub fn popup_shadow_statistics(cx: &App) -> PopupShadowStatistics {
    cx.try_global::<ShadowCache>()
        .map_or_else(PopupShadowStatistics::default, |cache| cache.stats.clone())
}

#[derive(Clone, PartialEq)]
struct Key {
    // Dimensions, radius, alpha, scale, arrow size/radii/coordinate/side.
    shape: [u32; 11],
    shadows: ShadowList,
}
impl Eq for Key {}
impl Hash for Key {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.shape.hash(state);
        for shadow in self.shadows.iter() {
            for value in [
                shadow.x,
                shadow.y,
                shadow.blur,
                shadow.spread,
                shadow.color.a,
            ] {
                // Token validation excludes NaNs; normalize signed zero for Eq.
                if value == 0. { 0_u64 } else { value.to_bits() }.hash(state);
            }
            (shadow.color.r, shadow.color.g, shadow.color.b, shadow.inset).hash(state);
        }
    }
}
struct Resource {
    key: Key,
    image: Arc<RenderImage>,
    bounds: Bounds<Pixels>,
    bytes: usize,
}
struct Entry {
    resource: Arc<Resource>,
    used: u64,
}
#[derive(Default)]
struct ShadowCache {
    entries: HashMap<Key, Entry>,
    resident: HashMap<Key, Weak<Resource>>,
    tick: u64,
    stats: PopupShadowStatistics,
}
impl Global for ShadowCache {}

struct Lease(Option<Arc<Resource>>);
fn lease(resource: &Arc<Resource>, window: &mut Window, cx: &mut App) {
    let view = window.current_view();
    window.with_id(("__adgpui_popup_shadow_view", view), |window| {
        window.use_keyed_state(
            ("__adgpui_popup_shadow", resource.image.id.0),
            cx,
            |_, cx| {
                cx.on_release(|lease: &mut Lease, cx| {
                    if let Some(resource) = lease.0.take()
                        && Arc::strong_count(&resource) == 1
                    {
                        let image = resource.image.clone();
                        let key = resource.key.clone();
                        cx.defer(move |cx| {
                            cx.drop_image(image, None);
                            if cx.has_global::<ShadowCache>() {
                                cx.update_global::<ShadowCache, _>(|cache, _| {
                                    if cache
                                        .resident
                                        .get(&key)
                                        .is_some_and(|entry| entry.strong_count() == 0)
                                    {
                                        cache.resident.remove(&key);
                                    }
                                });
                            }
                        });
                    }
                })
                .detach();
                Lease(Some(resource.clone()))
            },
        );
    });
}

pub(super) fn paint(
    layout: PopupLayout,
    radius: Pixels,
    arrow: ArrowShape,
    alpha: f32,
    shadows: ShadowList,
    window: &mut Window,
    cx: &mut App,
) {
    if shadows.is_empty()
        || alpha <= 0.
        || layout.bounds.is_empty()
        || shadows.iter().any(|shadow| {
            [
                shadow.x,
                shadow.y,
                shadow.blur,
                shadow.spread,
                shadow.color.a,
            ]
            .iter()
            .any(|value| !value.is_finite())
                || shadow.blur < 0.
        })
    {
        return;
    }
    let side = if layout.arrow.is_none() {
        0
    } else {
        match (layout.placement.is_vertical(), layout.placement.before()) {
            (true, true) => 1,
            (true, false) => 2,
            (false, true) => 3,
            (false, false) => 4,
        }
    };
    let coordinate = layout.arrow.map_or(0., |point| {
        if layout.placement.is_vertical() {
            point.x.into()
        } else {
            point.y.into()
        }
    });
    let numbers = [
        f32::from(layout.bounds.size.width),
        f32::from(layout.bounds.size.height),
        f32::from(radius),
        alpha,
        window.scale_factor(),
        arrow.size,
        arrow.tip_radius,
        arrow.outer_radius,
        coordinate,
        side as f32,
        0.,
    ];
    if numbers.iter().any(|number| !number.is_finite()) {
        return;
    }
    let key = Key {
        shape: numbers.map(|number| if number == 0. { 0 } else { number.to_bits() }),
        shadows,
    };
    if !cx.has_global::<ShadowCache>() {
        cx.set_global(ShadowCache::default());
    }
    let resource = cx.update_global::<ShadowCache, _>(|cache, cx| cache.get(key, cx));
    if let Some(resource) = resource {
        lease(&resource, window, cx);
        let bounds = Bounds::new(
            layout.bounds.origin + resource.bounds.origin,
            resource.bounds.size,
        );
        let _ = window.paint_image(
            bounds,
            bounds,
            gpui::Corners::default(),
            resource.image.clone(),
            0,
            false,
        );
    }
}

impl ShadowCache {
    fn get(&mut self, key: Key, cx: &mut App) -> Option<Arc<Resource>> {
        self.tick = self.tick.wrapping_add(1);
        if let Some(entry) = self.entries.get_mut(&key) {
            entry.used = self.tick;
            self.stats.hits += 1;
            return Some(entry.resource.clone());
        }
        if let Some(resource) = self.resident.get(&key).and_then(Weak::upgrade) {
            self.stats.hits += 1;
            return Some(resource);
        }
        self.stats.misses += 1;
        let (svg, bounds, dimensions) = svg(&key)?;
        let renderer = cx.svg_renderer();
        let parsed = renderer.parse_svg(svg.as_bytes()).ok()?;
        let image = renderer
            .render_parsed(&parsed, SvgSize::ExactSize(dimensions))
            .ok()?;
        let bytes = image.as_bytes(0).map_or(0, <[u8]>::len);
        let resource = Arc::new(Resource {
            key: key.clone(),
            image,
            bounds,
            bytes,
        });
        self.resident.retain(|_, entry| entry.strong_count() > 0);
        self.resident.insert(key.clone(), Arc::downgrade(&resource));
        self.stats.bytes += bytes;
        self.entries.insert(
            key,
            Entry {
                resource: resource.clone(),
                used: self.tick,
            },
        );
        while self.entries.len() > MAX_ENTRIES || self.stats.bytes > MAX_BYTES {
            let key = self
                .entries
                .iter()
                .min_by_key(|(_, entry)| entry.used)
                .map(|(key, _)| key.clone())?;
            let entry = self.entries.remove(&key)?;
            self.stats.bytes -= entry.resource.bytes;
            if Arc::strong_count(&entry.resource) == 1 {
                let image = entry.resource.image.clone();
                cx.defer(move |cx| cx.drop_image(image, None));
            }
        }
        self.stats.entries = self.entries.len();
        Some(resource)
    }
}

fn svg(key: &Key) -> Option<(String, Bounds<Pixels>, gpui::Size<DevicePixels>)> {
    let [
        width,
        height,
        radius,
        alpha,
        scale,
        arrow_size,
        tip,
        outer,
        coordinate,
        side,
        _,
    ] = key.shape.map(f32::from_bits);
    let arrow = ArrowShape::new(px(arrow_size), px(tip), px(outer));
    // Three standard deviations preserve even the last, very soft filter. Each
    // CSS drop-shadow sees the output of the previous filter, so pads accumulate.
    let pad = arrow_size / 2.
        + key
            .shadows
            .iter()
            .map(|shadow| (shadow.blur * 3. + shadow.x.abs().max(shadow.y.abs())) as f32)
            .sum::<f32>();
    let full_width = width + 2. * pad;
    let full_height = height + 2. * pad;
    if !full_width.is_finite() || !full_height.is_finite() || full_width <= 0. || full_height <= 0.
    {
        return None;
    }
    let minimum_blur = key
        .shadows
        .iter()
        .map(|shadow| shadow.blur as f32)
        .fold(f32::INFINITY, f32::min);
    let shadow_scale = if minimum_blur > 0. {
        SAMPLES_PER_SIGMA / minimum_blur
    } else {
        scale
    };
    let effective_scale = scale
        .min(shadow_scale)
        .min((MAX_IMAGE_PIXELS / (full_width * full_height)).sqrt())
        .min(4096. / full_width.max(full_height));
    let dimensions = size(
        DevicePixels((full_width * effective_scale).ceil() as i32),
        DevicePixels((full_height * effective_scale).ceil() as i32),
    );
    let bounds = Bounds::new(
        point(px(-pad), px(-pad)),
        size(px(full_width), px(full_height)),
    );
    let mut svg = format!(
        r#"<svg xmlns="http://www.w3.org/2000/svg" width="{full_width}" height="{full_height}" viewBox="0 0 {full_width} {full_height}"><defs><filter id="shadow" filterUnits="userSpaceOnUse" x="0" y="0" width="{full_width}" height="{full_height}" color-interpolation-filters="sRGB">"#
    );
    for (i, shadow) in key.shadows.iter().enumerate() {
        let previous = if i == 0 {
            "SourceGraphic".to_string()
        } else {
            format!("combined{}", i - 1)
        };
        let _ = write!(
            svg,
            r#"<feGaussianBlur in="{previous}" stdDeviation="{}" result="blur{i}"/><feOffset in="blur{i}" dx="{}" dy="{}" result="offset{i}"/><feFlood flood-color="rgb({},{},{})" flood-opacity="{}" result="color{i}"/><feComposite in="color{i}" in2="offset{i}" operator="in" result="shadow{i}"/><feMerge result="combined{i}"><feMergeNode in="shadow{i}"/><feMergeNode in="{previous}"/></feMerge>"#,
            shadow.blur,
            shadow.x,
            shadow.y,
            shadow.color.r,
            shadow.color.g,
            shadow.color.b,
            shadow.color.a
        );
    }
    svg.push_str("<feMerge>");
    for i in (0..key.shadows.len()).rev() {
        let _ = write!(svg, r#"<feMergeNode in="shadow{i}"/>"#);
    }
    let _ = write!(
        svg,
        r#"</feMerge></filter></defs><g filter="url(#shadow)"><g fill="black" opacity="{alpha}"><rect x="{pad}" y="{pad}" width="{width}" height="{height}" rx="{radius}"/>"#
    );
    if side != 0. {
        let (x, y, angle) = match side as u8 {
            1 => (coordinate, height, 180),
            2 => (coordinate, 0., 0),
            3 => (width, coordinate, 90),
            _ => (0., coordinate, -90),
        };
        let half = arrow_size / 2.;
        let _ = write!(
            svg,
            r#"<path d="{}" transform="translate({} {}) rotate({angle}) translate(-{half} -{half})"/>"#,
            arrow.svg_path(),
            x + pad,
            y + pad
        );
    }
    svg.push_str("</g></g></svg>");
    Some((svg, bounds, dimensions))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::theme::{Color, Shadow};

    #[test]
    fn blurred_shadow_sampling_is_bounded_but_sharp_edges_keep_device_resolution() {
        let key = |blur| Key {
            shape: [280., 104., 8., 1., 2., 16., 2., 4., 140., 1., 0.].map(f32::to_bits),
            shadows: Arc::from([Shadow {
                blur,
                color: Color::BLACK.with_alpha(0.12),
                ..Default::default()
            }]),
        };
        let (_, soft_bounds, soft_size) = svg(&key(6.)).unwrap();
        let full_soft_pixels =
            f32::from(soft_bounds.size.width) * f32::from(soft_bounds.size.height) * 4.;
        assert!(((soft_size.width.0 * soft_size.height.0) as f32) < full_soft_pixels / 4.);
        let (_, sharp_bounds, sharp_size) = svg(&key(0.)).unwrap();
        assert_eq!(
            sharp_size.width.0,
            (f32::from(sharp_bounds.size.width) * 2.).ceil() as i32
        );
        assert_eq!(
            sharp_size.height.0,
            (f32::from(sharp_bounds.size.height) * 2.).ceil() as i32
        );
    }

    #[gpui::test]
    fn popup_shadow_cache_is_bounded_and_reuses_a_scene_retained_eviction(
        cx: &mut gpui::TestAppContext,
    ) {
        cx.update(|cx| {
            let mut cache = ShadowCache::default();
            let shadows: ShadowList = Arc::from([Shadow {
                x: 1.,
                y: 1.,
                color: Color::BLACK.with_alpha(0.1),
                ..Default::default()
            }]);
            let key = |width: f32| Key {
                shape: [width, 24., 6., 1., 1., 16., 2., 4., 16., 2., 0.].map(f32::to_bits),
                shadows: shadows.clone(),
            };
            let held = cache.get(key(32.), cx).unwrap();
            let identity = held.image.id;
            for width in 33..99 {
                cache.get(key(width as f32), cx).unwrap();
            }
            assert_eq!(cache.stats.entries, MAX_ENTRIES);
            assert!(cache.stats.bytes <= MAX_BYTES);
            assert!(!cache.entries.contains_key(&key(32.)));
            let misses = cache.stats.misses;
            assert_eq!(cache.get(key(32.), cx).unwrap().image.id, identity);
            assert_eq!(cache.stats.misses, misses);
        });
    }
}

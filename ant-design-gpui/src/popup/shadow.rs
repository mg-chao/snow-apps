// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Alpha-shape shadows shared only by live native scenes. The bounded weak
//! lookup keeps lightweight fingerprints, never unused raster data. Scene leases
//! protect atlas entries across cached replays and retire the last visible use.

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
    sync::{
        Arc, Weak,
        atomic::{AtomicUsize, Ordering},
    },
};

const MAX_ENTRIES: usize = 64;
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
    /// Raster resources currently owned by native scenes, including shared windows.
    pub live_entries: usize,
}

pub fn popup_shadow_statistics(cx: &App) -> PopupShadowStatistics {
    cx.try_global::<ShadowCache>()
        .map_or_else(PopupShadowStatistics::default, ShadowCache::statistics)
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
#[derive(Default)]
struct Counters {
    entries: AtomicUsize,
    bytes: AtomicUsize,
}
struct Resource {
    key: Key,
    image: Arc<RenderImage>,
    bounds: Bounds<Pixels>,
    bytes: usize,
    counters: Arc<Counters>,
}
impl Drop for Resource {
    fn drop(&mut self) {
        self.counters.entries.fetch_sub(1, Ordering::Relaxed);
        self.counters.bytes.fetch_sub(self.bytes, Ordering::Relaxed);
    }
}
struct Entry {
    resource: Weak<Resource>,
    used: u64,
}
#[derive(Default)]
struct ShadowCache {
    // Hash collisions only lose sharing: a hit must match the live full key.
    // Fingerprints do not retain arbitrarily long custom shadow arrays.
    entries: HashMap<u64, Entry>,
    tick: u64,
    stats: PopupShadowStatistics,
    counters: Arc<Counters>,
}
impl Global for ShadowCache {}

fn retire(resource: &Arc<Resource>, cx: &mut App) {
    let image = resource.image.clone();
    cx.defer(move |cx| cx.drop_image(image, None));
}
fn scene_resource(key: Key, window: &mut Window, cx: &mut App) -> Option<Arc<Resource>> {
    if !cx.has_global::<ShadowCache>() {
        cx.set_global(ShadowCache::default());
    }
    let fingerprint = crate::behavior::resources::resource_fingerprint(&key);
    let mut looked_up = false;
    let resource = crate::behavior::resources::try_scene_resource(
        ("__adgpui_popup_shadow", fingerprint),
        |resource: &Arc<Resource>| resource.key == key,
        |_, cx| {
            looked_up = true;
            cx.update_global::<ShadowCache, _>(|cache, cx| cache.get(key.clone(), cx))
                .ok_or(())
        },
        |resource| Arc::strong_count(resource) == 1,
        retire,
        window,
        cx,
    )
    .ok();
    if resource.is_some() && !looked_up {
        cx.global_mut::<ShadowCache>().stats.hits += 1;
    }
    resource
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
    if let Some(resource) = scene_resource(key, window, cx) {
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
    fn statistics(&self) -> PopupShadowStatistics {
        PopupShadowStatistics {
            entries: self.entries.len(),
            bytes: self.counters.bytes.load(Ordering::Relaxed),
            live_entries: self.counters.entries.load(Ordering::Relaxed),
            ..self.stats.clone()
        }
    }
    fn get(&mut self, key: Key, cx: &mut App) -> Option<Arc<Resource>> {
        self.tick = self.tick.wrapping_add(1);
        let mut hash = std::collections::hash_map::DefaultHasher::new();
        key.hash(&mut hash);
        let fingerprint = hash.finish();
        if let Some(entry) = self.entries.get_mut(&fingerprint)
            && let Some(resource) = entry.resource.upgrade()
            && resource.key == key
        {
            entry.used = self.tick;
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
        self.counters.entries.fetch_add(1, Ordering::Relaxed);
        self.counters.bytes.fetch_add(bytes, Ordering::Relaxed);
        let resource = Arc::new(Resource {
            key,
            image,
            bounds,
            bytes,
            counters: self.counters.clone(),
        });
        self.entries
            .retain(|_, entry| entry.resource.strong_count() > 0);
        self.entries.insert(
            fingerprint,
            Entry {
                resource: Arc::downgrade(&resource),
                used: self.tick,
            },
        );
        while self.entries.len() > MAX_ENTRIES {
            let oldest = *self.entries.iter().min_by_key(|(_, entry)| entry.used)?.0;
            self.entries.remove(&oldest);
        }
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
    use gpui::{AppContext, IntoElement, Styled};
    use std::{cell::RefCell, rc::Rc};

    #[test]
    fn popup_shadow_scene_recovery_survives_lookup_pressure_and_releases_hidden_rasters() {
        struct Consumer {
            shown: bool,
            revision: usize,
            images: Rc<RefCell<Vec<usize>>>,
        }
        impl gpui::Render for Consumer {
            fn render(&mut self, _: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
                if !self.shown {
                    return gpui::div().into_any_element();
                }
                let revision = self.revision;
                let images = self.images.clone();
                gpui::canvas(
                    |_, _, _| (),
                    move |_, _, window, cx| {
                        images.borrow_mut().clear();
                        for index in 0..=MAX_ENTRIES {
                            window.with_id(("shadow-consumer", index as u64), |window| {
                                let width = (index + 1 + usize::from(index == 0) * revision) as f32;
                                let key = Key {
                                    shape: [width, 2., 0., 1., 1., 0., 0., 0., 0., 0., 0.]
                                        .map(f32::to_bits),
                                    shadows: Arc::from([Shadow {
                                        y: 1.,
                                        color: Color::BLACK.with_alpha(0.1),
                                        ..Default::default()
                                    }]),
                                };
                                let resource = scene_resource(key, window, cx).unwrap();
                                images.borrow_mut().push(resource.image.id.0);
                                let _ = window.paint_image(
                                    resource.bounds,
                                    resource.bounds,
                                    gpui::Corners::default(),
                                    resource.image.clone(),
                                    0,
                                    false,
                                );
                            });
                        }
                    },
                )
                .size_full()
                .into_any_element()
            }
        }
        struct CachedConsumer(gpui::Entity<Consumer>);
        impl gpui::Render for CachedConsumer {
            fn render(&mut self, _: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
                self.0
                    .clone()
                    .cached(gpui::StyleRefinement::default().size_full())
            }
        }
        let mut cx = gpui::TestAppContext::single();
        let images = Rc::new(RefCell::new(Vec::new()));
        let mut consumer = None;
        let window = cx.add_window(|_, cx| {
            let view = cx.new(|_| Consumer {
                shown: true,
                revision: 0,
                images: images.clone(),
            });
            consumer = Some(view.clone());
            CachedConsumer(view)
        });
        let consumer = consumer.unwrap();
        let draw = |cx: &mut gpui::TestAppContext| {
            cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
                .unwrap();
            cx.run_until_parked();
        };
        draw(&mut cx);
        let first = images.borrow().clone();
        let warm = cx.update(|cx| popup_shadow_statistics(cx));
        assert_eq!(first.len(), MAX_ENTRIES + 1);
        assert_eq!(warm.entries, MAX_ENTRIES);
        assert_eq!(warm.misses, (MAX_ENTRIES + 1) as u64);
        assert_eq!(warm.live_entries, MAX_ENTRIES + 1);
        draw(&mut cx);
        assert_eq!(*images.borrow(), first, "cached scenes keep their leases");
        consumer.update(&mut cx, |_, cx| cx.notify());
        draw(&mut cx);
        assert_eq!(
            *images.borrow(),
            first,
            "repaints recover evicted live records"
        );
        assert_eq!(
            cx.update(|cx| popup_shadow_statistics(cx).misses),
            warm.misses
        );

        consumer.update(&mut cx, |consumer, cx| {
            consumer.revision = 128;
            cx.notify();
        });
        draw(&mut cx);
        let changed = images.borrow().clone();
        assert_ne!(changed[0], first[0]);
        assert_eq!(changed[1..], first[1..]);
        assert_eq!(
            cx.update(|cx| popup_shadow_statistics(cx).misses),
            warm.misses + 1
        );
        assert_eq!(
            cx.update(|cx| popup_shadow_statistics(cx).live_entries),
            MAX_ENTRIES + 1
        );

        consumer.update(&mut cx, |consumer, cx| {
            consumer.shown = false;
            cx.notify();
        });
        draw(&mut cx);
        assert_eq!(cx.update(|cx| popup_shadow_statistics(cx).live_entries), 0);
        assert_eq!(cx.update(|cx| popup_shadow_statistics(cx).bytes), 0);
        consumer.update(&mut cx, |consumer, cx| {
            consumer.shown = true;
            cx.notify();
        });
        draw(&mut cx);
        assert!(images.borrow().iter().zip(changed).all(|(a, b)| *a != b));
        assert_eq!(
            cx.update(|cx| popup_shadow_statistics(cx).live_entries),
            MAX_ENTRIES + 1
        );
    }

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
    fn popup_shadow_lookup_is_bounded_and_does_not_keep_unused_rasters(
        cx: &mut gpui::TestAppContext,
    ) {
        cx.update(|cx| {
            let mut cache = ShadowCache::default();
            let key = |width: f32| Key {
                shape: [width, 24., 6., 1., 1., 16., 2., 4., 16., 2., 0.].map(f32::to_bits),
                shadows: Arc::from([Shadow {
                    x: 1.,
                    y: 1.,
                    color: Color::BLACK.with_alpha(0.1),
                    ..Default::default()
                }]),
            };
            let held = cache.get(key(32.), cx).unwrap();
            assert!(Arc::ptr_eq(&held, &cache.get(key(32.), cx).unwrap()));
            let mut visible = Vec::new();
            for width in 33..99 {
                visible.push(cache.get(key(width as f32), cx).unwrap());
            }
            assert_eq!(cache.entries.len(), MAX_ENTRIES);
            assert_eq!(cache.statistics().live_entries, 67);
            // Evicting lookup metadata cannot invalidate a still-visible resource.
            assert!(held.image.as_bytes(0).is_some());
            drop(visible);
            drop(held);
            assert_eq!(cache.statistics().bytes, 0);
            assert_eq!(cache.statistics().live_entries, 0);
        });
    }
}

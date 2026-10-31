// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Ink colors and motion adapted from Ant Design 6.6.7 (MIT).

//! Low-detail blurred masks are rasterized once, then moved and faded using
//! native GPUI image primitives. No convolution runs on an animation frame.

use super::picture::Preparation;
use crate::{behavior::evaluate_cubic_bezier, theme::CubicBezier};
use gpui::{
    AnyElement, App, BorrowAppContext, DevicePixels, Global, IntoElement, RenderImage, StyledImage,
    SvgSize, Window, div, img, prelude::*, px, size,
};
use std::{
    collections::HashMap,
    rc::{Rc, Weak},
    sync::Arc,
    time::Duration,
};

/// Paint the reference's four-stop, repeating 200% gradient with at most a few
/// native quads. Only its position changes; no raster preparation runs per frame.
pub(super) fn rail_fill(
    percent: f32,
    radius: f32,
    elapsed: Duration,
    duration: Duration,
    motion: bool,
) -> AnyElement {
    let phase = if motion {
        (elapsed.as_secs_f64() / duration.as_secs_f64().max(0.01)).fract() as f32
    } else {
        0.
    };
    div()
        .h_full()
        .w(gpui::relative(percent / 100.))
        .child(
            gpui::canvas(
                |_, _, _| (),
                move |bounds, _, w, _| {
                    let width = f32::from(bounds.size.width);
                    if width <= 0. {
                        return;
                    }
                    let period = width * 2.;
                    let shift = (width * 4. * phase).rem_euclid(period);
                    let mut points = [0.; 16];
                    let mut count = 1;
                    points[0] = 0.;
                    for repeat in -2..=1 {
                        for stop in [0., 0.4, 0.6, 1.] {
                            let x = shift + repeat as f32 * period + stop * period;
                            if x > 0. && x < width {
                                points[count] = x;
                                count += 1;
                            }
                        }
                    }
                    points[count] = width;
                    count += 1;
                    points[..count].sort_by(f32::total_cmp);
                    for pair in points[..count].windows(2) {
                        if pair[1] - pair[0] < 0.001 {
                            continue;
                        }
                        let a = rail_color(((pair[0] - shift) / period).rem_euclid(1.));
                        let b = rail_color(((pair[1] - shift) / period).rem_euclid(1.));
                        let rect = gpui::Bounds::new(
                            bounds.origin + gpui::point(px(pair[0]), px(0.)),
                            gpui::size(px(pair[1] - pair[0]), bounds.size.height),
                        );
                        let left = if pair[0] == 0. { radius } else { 0. };
                        let right = if pair[1] == width {
                            if percent >= 100. { radius } else { radius / 2. }
                        } else {
                            0.
                        };
                        w.paint_quad(
                            gpui::fill(
                                rect,
                                gpui::linear_gradient(
                                    90.,
                                    gpui::linear_color_stop(a, 0.),
                                    gpui::linear_color_stop(b, 1.),
                                ),
                            )
                            .corner_radii(gpui::Corners {
                                top_left: px(left),
                                bottom_left: px(left),
                                top_right: px(right),
                                bottom_right: px(right),
                            }),
                        );
                    }
                },
            )
            .size_full(),
        )
        .into_any_element()
}
fn rail_color(position: f32) -> gpui::Rgba {
    let (left, right, t): ([f32; 3], [f32; 3], f32) = if position <= 0.4 {
        ([120., 170., 255.], [160., 150., 245.], position / 0.4)
    } else if position <= 0.6 {
        (
            [160., 150., 245.],
            [130., 200., 220.],
            (position - 0.4) / 0.2,
        )
    } else {
        (
            [130., 200., 220.],
            [120., 170., 255.],
            (position - 0.6) / 0.4,
        )
    };
    let channel = |i| (left[i] + (right[i] - left[i]) * t) / 255.;
    gpui::Rgba {
        r: channel(0),
        g: channel(1),
        b: channel(2),
        a: 0.85,
    }
}

const MAX_ENTRIES: usize = 16;
const MAX_BYTES: usize = 8 * 1024 * 1024;
const CURVE: CubicBezier = CubicBezier::new(0.645, 0.045, 0.355, 1.);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct Key {
    width: u32,
    height: u32,
    scale: u32,
}
#[derive(Clone, Copy)]
struct Layer {
    width: f32,
    height: f32,
    blur: f32,
    cx: f32,
    cy: f32,
    rx: f32,
    ry: f32,
    stop: f32,
    color: [u8; 3],
    alpha: f32,
}
struct Masks {
    images: Vec<Arc<RenderImage>>,
    layers: [Layer; 5],
    bytes: usize,
}
struct Resource {
    masks: Arc<Masks>,
}
struct Entry {
    preparation: Option<Preparation<Option<Arc<Masks>>>>,
    resource: Option<Rc<Resource>>,
    used: u64,
}
#[derive(Default)]
struct Cache {
    entries: HashMap<Key, Entry>,
    resident: HashMap<Key, Weak<Resource>>,
    tick: u64,
}
impl Global for Cache {}

#[derive(Clone, Copy, Debug)]
struct Pose {
    x: f32,
    y: f32,
    scale: f32,
    opacity: f32,
}

/// Return the cloud-only layer. Its parent supplies colorBgBase, clipping,
/// progress text/rail, and the animation clock. Reduced motion never needs a timer.
pub(super) fn clouds(
    width: f32,
    height: f32,
    elapsed: Duration,
    duration: Duration,
    motion: bool,
    window: &mut Window,
    cx: &mut App,
) -> AnyElement {
    let root = div()
        .relative()
        .w(px(width.max(0.)))
        .h(px(height.max(0.)))
        .overflow_hidden();
    if !width.is_finite() || !height.is_finite() || width <= 0. || height <= 0. {
        return root.into_any_element();
    }
    let key = Key {
        width: width.ceil() as u32,
        height: height.ceil() as u32,
        scale: window.scale_factor().to_bits(),
    };
    if !cx.has_global::<Cache>() {
        cx.set_global(Cache::default());
    }
    let resource = cx.update_global::<Cache, _>(|cache, cx| cache.get(key, window, cx));
    let Some(resource) = resource else {
        return root.into_any_element();
    };
    let elapsed = if motion { elapsed.as_secs_f64() } else { 0. };
    let duration = duration.as_secs_f64().max(0.01);
    let first = flow(0, elapsed, duration, 0.);
    let fourth = flow(2, elapsed, duration + 1.5, 3.);
    let poses = [
        first,
        flow(1, elapsed, duration + 2., 1.),
        flow(2, elapsed, duration + 0.5, 2.),
        fourth,
        flow(0, elapsed, duration + 2.5, 2.5),
    ];
    let masks = &resource.masks;
    let children = masks.images.iter().enumerate().map(|(index, image)| {
        let layer = masks.layers[index];
        let pose = poses[index];
        let child = matches!(index, 1 | 2 | 4);
        let parent = if index == 4 { fourth } else { first };
        let parent_width = width * 1.5;
        let parent_height = height * 1.5;
        let (x, y, scale, opacity) = if child {
            let child_x = -0.25 * parent_width + layer.width * (0.5 * (1. - pose.scale) + pose.x);
            let child_y = -0.25 * parent_height + layer.height * (0.5 * (1. - pose.scale) + pose.y);
            (
                -0.25 * width
                    + parent_width * (0.5 * (1. - parent.scale) + parent.x)
                    + child_x * parent.scale,
                -0.25 * height
                    + parent_height * (0.5 * (1. - parent.scale) + parent.y)
                    + child_y * parent.scale,
                parent.scale * pose.scale,
                parent.opacity * pose.opacity,
            )
        } else {
            (
                -0.25 * width + layer.width * (0.5 * (1. - pose.scale) + pose.x),
                -0.25 * height + layer.height * (0.5 * (1. - pose.scale) + pose.y),
                pose.scale,
                pose.opacity,
            )
        };
        let pad = layer.blur * 3.;
        img(image.clone())
            .id(("image-ink", index))
            .absolute()
            .left(px(x - pad * scale))
            .top(px(y - pad * scale))
            .w(px((layer.width + pad * 2.) * scale))
            .h(px((layer.height + pad * 2.) * scale))
            .object_fit(gpui::ObjectFit::Fill)
            .opacity(opacity)
    });
    root.children(children).into_any_element()
}

fn flow(kind: usize, elapsed: f64, duration: f64, delay: f64) -> Pose {
    let progress = (elapsed + delay).rem_euclid(duration) / duration;
    let phase = if progress < 0.5 {
        progress * 2.
    } else {
        (1. - progress) * 2.
    };
    let t = evaluate_cubic_bezier(CURVE, phase) as f32;
    let (x, y, start_scale, end_scale, start_opacity, end_opacity) = match kind {
        1 => (-0.18, 0.15, 1.1, 0.85, 0.7, 0.9),
        2 => (0.08, 0.10, 0.85, 1.15, 0.65, 0.8),
        _ => (0.15, -0.20, 1., 1.25, 0.8, 0.5),
    };
    Pose {
        x: x * t,
        y: y * t,
        scale: start_scale + (end_scale - start_scale) * t,
        opacity: start_opacity + (end_opacity - start_opacity) * t,
    }
}

fn layers(width: f32, height: f32) -> [Layer; 5] {
    let main = |blur, cx, cy, rx, ry, stop, color, alpha| Layer {
        width: width * 1.5,
        height: height * 1.5,
        blur,
        cx,
        cy,
        rx,
        ry,
        stop,
        color,
        alpha,
    };
    let child = |blur, cx, cy, rx, ry, stop, color, alpha| Layer {
        width: width * 2.25,
        height: height * 2.25,
        blur,
        cx,
        cy,
        rx,
        ry,
        stop,
        color,
        alpha,
    };
    [
        main(40., 0.25, 0.30, 0.65, 0.55, 0.55, [100, 180, 255], 0.98),
        child(
            (45f32.powi(2) + 40f32.powi(2)).sqrt(),
            0.75,
            0.45,
            0.60,
            0.65,
            0.50,
            [180, 140, 255],
            0.95,
        ),
        child(
            (38f32.powi(2) + 40f32.powi(2)).sqrt(),
            0.50,
            0.70,
            0.55,
            0.50,
            0.45,
            [100, 220, 220],
            0.9,
        ),
        main(42., 0.60, 0.20, 0.45, 0.40, 0.45, [255, 150, 200], 0.88),
        child(
            (35f32.powi(2) + 42f32.powi(2)).sqrt(),
            0.20,
            0.75,
            0.50,
            0.55,
            0.50,
            [160, 190, 255],
            0.88,
        ),
    ]
}

fn rasterize(key: Key, renderer: gpui::SvgRenderer) -> Option<Arc<Masks>> {
    let layers = layers(key.width as f32, key.height as f32);
    let mut images = Vec::with_capacity(5);
    let mut bytes = 0;
    for layer in layers {
        let pad = layer.blur * 3.;
        let width = layer.width + pad * 2.;
        let height = layer.height + pad * 2.;
        let cx = pad + layer.width * layer.cx;
        let cy = pad + layer.height * layer.cy;
        let rx = layer.width * layer.rx;
        let ry = layer.height * layer.ry;
        let [r, g, b] = layer.color;
        // 0.25 samples/logical pixel resolves these 35–60 px Gaussian masks
        // comfortably, independently of monitor DPI and sharp image detail.
        let scale = f32::from_bits(key.scale)
            .min(0.25)
            .min(512. / width.max(height));
        let dimensions = size(
            DevicePixels((width * scale).ceil().max(1.) as i32),
            DevicePixels((height * scale).ceil().max(1.) as i32),
        );
        let svg = format!(
            r#"<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}"><defs><radialGradient id="ink" gradientUnits="userSpaceOnUse" cx="0" cy="0" r="1" gradientTransform="translate({cx} {cy}) scale({rx} {ry})"><stop stop-color="rgb({r},{g},{b})" stop-opacity="{}"/><stop offset="{}" stop-color="rgb({r},{g},{b})" stop-opacity="0"/></radialGradient><filter id="blur" x="0" y="0" width="{width}" height="{height}" filterUnits="userSpaceOnUse" color-interpolation-filters="sRGB"><feGaussianBlur stdDeviation="{}"/></filter></defs><rect x="{pad}" y="{pad}" width="{}" height="{}" fill="url(#ink)" filter="url(#blur)"/></svg>"#,
            layer.alpha, layer.stop, layer.blur, layer.width, layer.height
        );
        let parsed = renderer.parse_svg(svg.as_bytes()).ok()?;
        let image = renderer
            .render_parsed(&parsed, SvgSize::ExactSize(dimensions))
            .ok()?;
        bytes += image.as_bytes(0).map_or(0, <[u8]>::len);
        images.push(image);
    }
    Some(Arc::new(Masks {
        images,
        layers,
        bytes,
    }))
}

impl Cache {
    fn get(&mut self, key: Key, window: &mut Window, cx: &mut App) -> Option<Rc<Resource>> {
        self.tick = self.tick.wrapping_add(1);
        let entry = self.entries.entry(key).or_insert_with(|| {
            let resource = self.resident.get(&key).and_then(Weak::upgrade);
            let renderer = cx.svg_renderer();
            Entry {
                preparation: resource
                    .is_none()
                    .then(|| Preparation::new(async move { rasterize(key, renderer) }, cx)),
                resource,
                used: self.tick,
            }
        });
        entry.used = self.tick;
        if entry.resource.is_none()
            && let Some(prepared) = entry
                .preparation
                .as_ref()
                .and_then(|task| task.use_value(window))
        {
            if let Some(masks) = prepared.as_ref() {
                entry.resource = Some(Rc::new(Resource {
                    masks: masks.clone(),
                }));
            }
            entry.preparation = None;
        }
        let result = entry.resource.clone();
        if let Some(resource) = &result {
            self.resident.retain(|_, weak| weak.strong_count() > 0);
            self.resident.insert(key, Rc::downgrade(resource));
            lease(resource, window, cx);
        }
        let mut bytes: usize = self
            .entries
            .values()
            .filter_map(|entry| entry.resource.as_ref())
            .map(|resource| resource.masks.bytes)
            .sum();
        while self.entries.len() > MAX_ENTRIES || bytes > MAX_BYTES {
            let Some(key) = self
                .entries
                .iter()
                .min_by_key(|(_, entry)| entry.used)
                .map(|(key, _)| *key)
            else {
                break;
            };
            if let Some(entry) = self.entries.remove(&key)
                && let Some(resource) = entry.resource
            {
                bytes = bytes.saturating_sub(resource.masks.bytes);
                retire(&resource, cx);
            }
        }
        result
    }
}

struct Lease(Rc<Resource>);
fn retire(resource: &Rc<Resource>, cx: &mut App) {
    if Rc::strong_count(resource) == 1 {
        let images = resource.masks.images.clone();
        cx.defer(move |cx| {
            for image in images {
                cx.drop_image(image, None);
            }
        });
    }
}
fn lease(resource: &Rc<Resource>, window: &mut Window, cx: &mut App) {
    window.with_id(("ant-image-ink-view", window.current_view()), |window| {
        window.use_keyed_state(
            ("ant-image-ink", resource.masks.images[0].id.0),
            cx,
            |_, cx| {
                cx.on_release(|lease: &mut Lease, cx| retire(&lease.0, cx))
                    .detach();
                Lease(resource.clone())
            },
        );
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ink_flow_matches_reference_endpoints_and_negative_delays() {
        let start = flow(0, 0., 3., 0.);
        let middle = flow(0, 1.5, 3., 0.);
        assert_eq!(
            (start.x, start.y, start.scale, start.opacity),
            (0., 0., 1., 0.8)
        );
        assert_eq!(
            (middle.x, middle.y, middle.scale, middle.opacity),
            (0.15, -0.2, 1.25, 0.5)
        );
        assert_eq!(flow(1, 0., 5., 1.).scale, flow(1, 1., 5., 0.).scale);
        assert_eq!(flow(2, 0., 4.5, 3.).opacity, flow(2, 4.5, 4.5, 3.).opacity);
    }
    #[test]
    fn watercolor_masks_are_bounded_nonempty_and_translucent() {
        let masks = rasterize(
            Key {
                width: 200,
                height: 200,
                scale: 1f32.to_bits(),
            },
            gpui::SvgRenderer::new(Arc::new(())),
        )
        .unwrap();
        assert_eq!(masks.images.len(), 5);
        assert!(masks.bytes < 2 * 1024 * 1024);
        for image in &masks.images {
            assert!(image.size(0).width.0 <= 512);
            assert!(
                image
                    .as_bytes(0)
                    .unwrap()
                    .as_chunks::<4>()
                    .0
                    .iter()
                    .any(|pixel| pixel[3] > 0 && pixel[3] < 255)
            );
        }
    }
}

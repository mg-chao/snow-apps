// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Transform-only frames reuse immutable textures. CPU orientation variants are
//! prepared in the background when a platform has no affine GPU compositor.

use super::{PreviewTransform, affine, loader::Preparation};
use gpui::{
    App, BorrowAppContext, Bounds, Global, GpuDamage, GpuRect, GpuService, GpuTexture,
    GpuTextureOptions, Pixels, RenderImage, Window, point, px, size,
};
use std::{
    collections::HashMap,
    rc::{Rc, Weak},
    sync::Arc,
};

const MAX_TEXTURE_BYTES: usize = 64 * 1024 * 1024;
const MAX_CACHE_ENTRIES: usize = 32;
#[derive(Clone, Copy)]
struct PreparationLimits {
    side: u32,
    bytes: u64,
}
const PORTABLE_LIMITS: PreparationLimits = PreparationLimits {
    side: 4096,
    bytes: MAX_TEXTURE_BYTES as u64,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct FrameKey {
    image: usize,
    frame: usize,
    grayscale: u8,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct GpuKey {
    frame: FrameKey,
    service: u64,
    epoch: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct RasterKey {
    frame: FrameKey,
    quarter: u8,
    flip_y: bool,
}
struct Upload {
    dimensions: [u32; 2],
    pixels: Vec<u8>,
}
struct GpuEntry {
    preparation: Option<Preparation<Option<Upload>>>,
    texture: Option<Arc<dyn GpuTexture>>,
    failed: bool,
    bytes: usize,
    used: u64,
}
struct RasterResource {
    image: Arc<RenderImage>,
    bytes: usize,
}
struct RasterEntry {
    preparation: Option<Preparation<Option<Arc<RenderImage>>>>,
    resource: Option<Rc<RasterResource>>,
    used: u64,
}
#[derive(Default)]
struct Cache {
    gpu: HashMap<GpuKey, GpuEntry>,
    raster: HashMap<RasterKey, RasterEntry>,
    raster_resident: HashMap<RasterKey, Weak<RasterResource>>,
    tick: u64,
}
impl Global for Cache {}

pub(super) struct PlainGeometry {
    pub bounds: Bounds<Pixels>,
    pub fitted: Bounds<Pixels>,
    pub corners: gpui::Corners<Pixels>,
}

/// Thumbnails retain native object-fit clipping and corner radii. Fractional
/// grayscale is prepared once instead of recoloring pixels during every paint.
pub(super) fn paint_plain(
    image: Arc<RenderImage>,
    frame: usize,
    geometry: PlainGeometry,
    grayscale: f32,
    window: &mut Window,
    cx: &mut App,
) -> bool {
    if frame >= image.frame_count() {
        return false;
    }
    let grayscale = grayscale_key(grayscale);
    let (image, frame) = if grayscale == 0 && !needs_portable_resample(&image, frame) {
        (image, frame)
    } else {
        if !cx.has_global::<Cache>() {
            cx.set_global(Cache::default());
        }
        let Some(raster) = cx.update_global::<Cache, _>(|cache, cx| {
            cache.raster(image, frame, grayscale, (0, false), window, cx)
        }) else {
            return false;
        };
        (raster, 0)
    };
    window
        .paint_image(
            geometry.bounds,
            geometry.fitted,
            geometry.corners,
            image,
            frame,
            false,
        )
        .is_ok()
}

fn grayscale_key(grayscale: f32) -> u8 {
    if grayscale.is_finite() {
        (grayscale.clamp(0., 1.) * 255.).round() as u8
    } else {
        0
    }
}

/// `bounds` is the fitted, untransformed image rectangle. Apply opacity to the
/// owning canvas, so native and GPU paths receive identical inherited opacity.
pub(super) fn paint(
    image: Arc<RenderImage>,
    frame: usize,
    bounds: Bounds<Pixels>,
    transform: PreviewTransform,
    grayscale: f32,
    window: &mut Window,
    cx: &mut App,
) -> bool {
    let matrix = affine(
        transform,
        point(bounds.size.width / 2., bounds.size.height / 2.),
    );
    paint_affine(image, frame, bounds, matrix, grayscale, window, cx)
}

/// The matrix is relative to `bounds.origin`. A false result means a background
/// orientation is pending; its completion invalidates this canvas's owning view.
pub(super) fn paint_affine(
    image: Arc<RenderImage>,
    frame: usize,
    bounds: Bounds<Pixels>,
    matrix: [f32; 6],
    grayscale: f32,
    window: &mut Window,
    cx: &mut App,
) -> bool {
    if frame >= image.frame_count()
        || !valid_bounds(bounds)
        || matrix.iter().any(|n| !n.is_finite())
    {
        return false;
    }
    let grayscale = grayscale_key(grayscale);
    if !cx.has_global::<Cache>() {
        cx.set_global(Cache::default());
    }
    if let Ok(service) = window.gpu_service() {
        let texture = cx.update_global::<Cache, _>(|cache, cx| {
            cache.gpu(image.clone(), frame, grayscale, service.clone(), window, cx)
        });
        if let Some(texture) = texture {
            let options = GpuTextureOptions {
                transform: matrix,
                damage: GpuDamage::None,
                ..Default::default()
            };
            if window.paint_gpu_texture(bounds, texture, options).is_ok() {
                return true;
            }
        }
    }
    let orientation = decompose(matrix, bounds);
    if orientation.width <= 0.0001 || orientation.height <= 0.0001 {
        return true;
    }
    let (raster, raster_frame) = if orientation.quarter == 0
        && !orientation.flip_y
        && grayscale == 0
        && !needs_portable_resample(&image, frame)
    {
        (Some(image.clone()), frame)
    } else {
        let raster = cx.update_global::<Cache, _>(|cache, cx| {
            cache.raster(
                image.clone(),
                frame,
                grayscale,
                (orientation.quarter, orientation.flip_y),
                window,
                cx,
            )
        });
        (raster, 0)
    };
    let Some(raster) = raster else { return false };
    let drawn = Bounds::new(
        point(
            orientation.center[0] - px(orientation.width / 2.),
            orientation.center[1] - px(orientation.height / 2.),
        ),
        size(px(orientation.width), px(orientation.height)),
    );
    window
        .paint_image(
            drawn,
            drawn,
            gpui::Corners::default(),
            raster,
            raster_frame,
            false,
        )
        .is_ok()
}

fn valid_bounds(bounds: Bounds<Pixels>) -> bool {
    [
        f32::from(bounds.origin.x),
        f32::from(bounds.origin.y),
        f32::from(bounds.size.width),
        f32::from(bounds.size.height),
    ]
    .iter()
    .all(|n| n.is_finite())
        && bounds.size.width > px(0.)
        && bounds.size.height > px(0.)
}

struct Orientation {
    quarter: u8,
    flip_y: bool,
    width: f32,
    height: f32,
    center: [Pixels; 2],
}
fn decompose(matrix: [f32; 6], bounds: Bounds<Pixels>) -> Orientation {
    let [a, b, c, d, tx, ty] = matrix;
    let sx = a.hypot(c);
    let sy = b.hypot(d);
    let quarter = ((-c).atan2(a) / std::f32::consts::FRAC_PI_2)
        .round()
        .rem_euclid(4.) as u8;
    let width = f32::from(bounds.size.width);
    let height = f32::from(bounds.size.height);
    Orientation {
        quarter,
        flip_y: a * d - b * c < 0.,
        width: if quarter.is_multiple_of(2) {
            width * sx
        } else {
            height * sx
        },
        height: if quarter.is_multiple_of(2) {
            height * sy
        } else {
            width * sy
        },
        center: [
            bounds.origin.x + px(a * width / 2. + c * height / 2. + tx),
            bounds.origin.y + px(b * width / 2. + d * height / 2. + ty),
        ],
    }
}

impl Cache {
    fn next(&mut self) -> u64 {
        self.tick = self.tick.wrapping_add(1);
        self.tick
    }

    fn gpu(
        &mut self,
        image: Arc<RenderImage>,
        frame: usize,
        grayscale: u8,
        service: Arc<dyn GpuService>,
        window: &Window,
        cx: &mut App,
    ) -> Option<Arc<dyn GpuTexture>> {
        let used = self.next();
        let key = GpuKey {
            frame: FrameKey {
                image: image.id.0,
                frame,
                grayscale,
            },
            service: service.service_id(),
            epoch: service.epoch(),
        };
        self.gpu
            .retain(|other, _| other.service != key.service || other.epoch == key.epoch);
        let entry = self.gpu.entry(key).or_insert_with(|| {
            let dimensions = image.size(frame);
            let capabilities = service.capabilities();
            let limits = PreparationLimits {
                side: capabilities.max_texture_size,
                bytes: capabilities.max_upload_bytes.min(MAX_TEXTURE_BYTES as u64),
            };
            let prepared = bounded_dimensions(
                [dimensions.width.0 as u32, dimensions.height.0 as u32],
                limits,
            );
            let bytes = prepared.map_or(0, |[width, height]| {
                u64::from(width) * u64::from(height) * 4
            });
            GpuEntry {
                preparation: prepared.map(|_| {
                    Preparation::new(
                        async move { prepare_upload(&image, frame, grayscale, limits) },
                        cx,
                    )
                }),
                texture: None,
                failed: prepared.is_none(),
                bytes: bytes as usize,
                used,
            }
        });
        entry.used = used;
        if !entry.failed
            && entry.texture.is_none()
            && let Some(upload) = entry
                .preparation
                .as_ref()
                .and_then(|preparation| preparation.use_value(window))
        {
            if let Some(upload) = upload.as_ref() {
                let texture =
                    service
                        .create_rgba8_texture(upload.dimensions)
                        .and_then(|mut texture| {
                            service.upload_rgba8_region(
                                &mut texture,
                                GpuRect {
                                    x: 0,
                                    y: 0,
                                    width: upload.dimensions[0],
                                    height: upload.dimensions[1],
                                },
                                &upload.pixels,
                                upload.dimensions[0] * 4,
                            )?;
                            Ok(texture)
                        });
                match texture {
                    Ok(texture) => entry.texture = Some(texture),
                    Err(_) => entry.failed = true,
                }
            } else {
                entry.failed = true;
            }
            entry.preparation = None;
            if entry.failed {
                entry.bytes = 0;
            }
        }
        let result = entry.texture.clone();
        self.trim_gpu();
        result
    }

    fn trim_gpu(&mut self) {
        let mut bytes: usize = self.gpu.values().map(|entry| entry.bytes).sum();
        while self.gpu.len() > MAX_CACHE_ENTRIES || bytes > MAX_TEXTURE_BYTES {
            let Some(key) = self
                .gpu
                .iter()
                .min_by_key(|(_, entry)| entry.used)
                .map(|(key, _)| *key)
            else {
                break;
            };
            if let Some(entry) = self.gpu.remove(&key) {
                bytes = bytes.saturating_sub(entry.bytes);
            }
        }
    }

    fn raster(
        &mut self,
        image: Arc<RenderImage>,
        frame: usize,
        grayscale: u8,
        orientation: (u8, bool),
        window: &mut Window,
        cx: &mut App,
    ) -> Option<Arc<RenderImage>> {
        let used = self.next();
        let (quarter, flip_y) = orientation;
        let key = RasterKey {
            frame: FrameKey {
                image: image.id.0,
                frame,
                grayscale,
            },
            quarter,
            flip_y,
        };
        let entry = self.raster.entry(key).or_insert_with(|| {
            let resource = self.raster_resident.get(&key).and_then(Weak::upgrade);
            RasterEntry {
                preparation: resource.is_none().then(|| {
                    Preparation::new(
                        async move { orient(&image, frame, quarter, flip_y, grayscale) },
                        cx,
                    )
                }),
                resource,
                used,
            }
        });
        entry.used = used;
        if entry.resource.is_none()
            && let Some(prepared) = entry
                .preparation
                .as_ref()
                .and_then(|preparation| preparation.use_value(window))
        {
            if let Some(image) = prepared.as_ref() {
                entry.resource = Some(Rc::new(RasterResource {
                    image: image.clone(),
                    bytes: image.as_bytes(0).map_or(0, <[u8]>::len),
                }));
            }
            entry.preparation = None;
        }
        let result = entry.resource.clone();
        if let Some(resource) = &result {
            self.raster_resident
                .retain(|_, resident| resident.strong_count() > 0);
            self.raster_resident.insert(key, Rc::downgrade(resource));
            raster_lease(resource, window, cx);
        }
        let mut bytes: usize = self
            .raster
            .values()
            .filter_map(|entry| entry.resource.as_ref())
            .map(|entry| entry.bytes)
            .sum();
        while self.raster.len() > MAX_CACHE_ENTRIES || bytes > MAX_TEXTURE_BYTES {
            let Some(key) = self
                .raster
                .iter()
                .min_by_key(|(_, entry)| entry.used)
                .map(|(key, _)| *key)
            else {
                break;
            };
            if let Some(entry) = self.raster.remove(&key)
                && let Some(resource) = entry.resource
            {
                bytes = bytes.saturating_sub(resource.bytes);
                retire_raster(&resource, cx);
            }
        }
        result.map(|resource| resource.image.clone())
    }
}

struct RasterLease(Rc<RasterResource>);
fn retire_raster(resource: &Rc<RasterResource>, cx: &mut App) {
    if Rc::strong_count(resource) == 1 {
        let image = resource.image.clone();
        cx.defer(move |cx| cx.drop_image(image, None));
    }
}
fn raster_lease(resource: &Rc<RasterResource>, window: &mut Window, cx: &mut App) {
    window.with_id(("ant-image-raster-view", window.current_view()), |window| {
        window.use_keyed_state(
            ("ant-image-oriented-raster", resource.image.id.0),
            cx,
            |_, cx| {
                cx.on_release(|lease: &mut RasterLease, cx| retire_raster(&lease.0, cx))
                    .detach();
                RasterLease(resource.clone())
            },
        );
    });
}

fn prepare_upload(
    image: &RenderImage,
    frame: usize,
    grayscale: u8,
    limits: PreparationLimits,
) -> Option<Upload> {
    let (original, pixels) = frame_data(image, frame)?;
    let dimensions = bounded_dimensions(original, limits)?;
    let resized = if original != dimensions {
        Some(resize_bgra(pixels, original, dimensions)?)
    } else {
        None
    };
    let pixels = resized.as_ref().map_or(pixels, |buffer| buffer.as_raw());
    let mut rgba = Vec::with_capacity(pixels.len());
    for pixel in pixels.as_chunks::<4>().0 {
        let pixel = gray_pixel(pixel, grayscale);
        let alpha = u16::from(pixel[3]);
        let channel = |channel: u8| ((u16::from(channel) * alpha + 127) / 255) as u8;
        rgba.extend_from_slice(&[
            channel(pixel[2]),
            channel(pixel[1]),
            channel(pixel[0]),
            pixel[3],
        ]);
    }
    Some(Upload {
        dimensions,
        pixels: rgba,
    })
}

fn orient(
    image: &RenderImage,
    frame: usize,
    quarter: u8,
    flip_y: bool,
    grayscale: u8,
) -> Option<Arc<RenderImage>> {
    orient_with_limits(image, frame, quarter, flip_y, grayscale, PORTABLE_LIMITS)
}

fn orient_with_limits(
    image: &RenderImage,
    frame: usize,
    quarter: u8,
    flip_y: bool,
    grayscale: u8,
    limits: PreparationLimits,
) -> Option<Arc<RenderImage>> {
    let (original, pixels) = frame_data(image, frame)?;
    let dimensions = bounded_dimensions(original, limits)?;
    let mut original = resize_bgra(pixels, original, dimensions)?;
    if grayscale != 0 {
        for pixel in original.as_chunks_mut::<4>().0 {
            let transformed = gray_pixel(pixel, grayscale);
            pixel.copy_from_slice(&transformed);
        }
    }
    // The buffer intentionally contains BGRA. Geometric operations preserve the
    // channels; RenderImage consumes the same BGRA representation afterward.
    let mut output = match quarter % 4 {
        0 => original,
        1 => ::image::imageops::rotate90(&original),
        2 => ::image::imageops::rotate180(&original),
        _ => ::image::imageops::rotate270(&original),
    };
    if flip_y {
        ::image::imageops::flip_vertical_in_place(&mut output);
    }
    Some(Arc::new(RenderImage::new(vec![::image::Frame::new(
        output,
    )])))
}

fn frame_data(image: &RenderImage, frame: usize) -> Option<([u32; 2], &[u8])> {
    let dimensions = image.size(frame);
    let width = u32::try_from(dimensions.width.0).ok()?;
    let height = u32::try_from(dimensions.height.0).ok()?;
    if width == 0 || height == 0 {
        return None;
    }
    let pixels = image.as_bytes(frame)?;
    let expected = usize::try_from(u64::from(width) * u64::from(height) * 4).ok()?;
    (pixels.len() == expected).then_some(([width, height], pixels))
}

fn bounded_dimensions([width, height]: [u32; 2], limits: PreparationLimits) -> Option<[u32; 2]> {
    if width == 0 || height == 0 || limits.side == 0 || limits.bytes < 4 {
        return None;
    }
    let pixels = limits.bytes / 4;
    let ratio = (f64::from(limits.side) / f64::from(width))
        .min(f64::from(limits.side) / f64::from(height))
        .min((pixels as f64 / (f64::from(width) * f64::from(height))).sqrt())
        .min(1.);
    let mut target = [
        (f64::from(width) * ratio).floor().max(1.) as u32,
        (f64::from(height) * ratio).floor().max(1.) as u32,
    ];
    // Extremely wide/tall sources can round their minor axis up to one pixel.
    if u64::from(target[0]) * u64::from(target[1]) > pixels {
        if target[0] >= target[1] {
            target[0] = (pixels / u64::from(target[1])).max(1) as u32;
        } else {
            target[1] = (pixels / u64::from(target[0])).max(1) as u32;
        }
    }
    Some(target)
}

fn resize_bgra(
    pixels: &[u8],
    [width, height]: [u32; 2],
    target: [u32; 2],
) -> Option<::image::RgbaImage> {
    if [width, height] == target {
        return ::image::RgbaImage::from_raw(width, height, pixels.to_vec());
    }
    // Borrow the decoded frame: downsampling allocates only bounded output,
    // avoiding a full-size source copy or an unbounded float intermediate.
    let original = ::image::ImageBuffer::<::image::Rgba<u8>, _>::from_raw(width, height, pixels)?;
    Some(::image::imageops::thumbnail(
        &original, target[0], target[1],
    ))
}

fn needs_portable_resample(image: &RenderImage, frame: usize) -> bool {
    let dimensions = image.size(frame);
    let original = [dimensions.width.0 as u32, dimensions.height.0 as u32];
    bounded_dimensions(original, PORTABLE_LIMITS) != Some(original)
}

fn gray_pixel(pixel: &[u8], amount: u8) -> [u8; 4] {
    if amount == 0 {
        return [pixel[0], pixel[1], pixel[2], pixel[3]];
    }
    let luminance =
        0.0722 * f32::from(pixel[0]) + 0.7152 * f32::from(pixel[1]) + 0.2126 * f32::from(pixel[2]);
    let fraction = f32::from(amount) / 255.;
    let channel =
        |value: u8| (f32::from(value) + (luminance - f32::from(value)) * fraction).round() as u8;
    [
        channel(pixel[0]),
        channel(pixel[1]),
        channel(pixel[2]),
        pixel[3],
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::DevicePixels;
    fn asymmetric() -> Arc<RenderImage> {
        Arc::new(RenderImage::new(vec![::image::Frame::new(
            ::image::RgbaImage::from_raw(
                2,
                3,
                vec![
                    1, 0, 0, 255, 2, 0, 0, 255, 3, 0, 0, 255, 4, 0, 0, 255, 5, 0, 0, 255, 6, 0, 0,
                    255,
                ],
            )
            .unwrap(),
        )]))
    }
    #[test]
    fn upload_converts_bgra_and_premultiplies_alpha() {
        let image = RenderImage::new(vec![::image::Frame::new(
            ::image::RgbaImage::from_raw(1, 1, vec![20, 40, 80, 128]).unwrap(),
        )]);
        assert_eq!(
            prepare_upload(&image, 0, 0, PORTABLE_LIMITS)
                .unwrap()
                .pixels,
            [40, 20, 10, 128]
        );
    }
    #[test]
    fn cached_orientation_preserves_channels_and_rotates_before_flipping() {
        let source = asymmetric();
        let result = orient(&source, 0, 1, true, 0).unwrap();
        assert_eq!(result.size(0), size(DevicePixels(3), DevicePixels(2)));
        assert_eq!(
            result
                .as_bytes(0)
                .unwrap()
                .as_chunks::<4>()
                .0
                .iter()
                .map(|p| p[0])
                .collect::<Vec<_>>(),
            [6, 4, 2, 5, 3, 1]
        );
        assert_eq!(source.as_bytes(0).unwrap()[0], 1);
    }
    #[test]
    fn decomposition_preserves_center_and_viewport_axis_reflection() {
        let bounds = Bounds::new(point(px(10.), px(20.)), size(px(200.), px(100.)));
        let transform = PreviewTransform {
            rotate: 90.,
            flip_x: true,
            x: 13.,
            y: -9.,
            ..Default::default()
        };
        let value = decompose(affine(transform, point(px(100.), px(50.))), bounds);
        assert!((f32::from(value.center[0]) - 123.).abs() < 0.001);
        assert!((f32::from(value.center[1]) - 61.).abs() < 0.001);
        assert_eq!((value.width, value.height), (100., 200.));
        assert!(value.flip_y);
    }

    #[test]
    fn oversized_frames_downsample_within_adapter_and_cache_limits_instead_of_disappearing() {
        let image = RenderImage::new(vec![::image::Frame::new(::image::RgbaImage::from_pixel(
            8,
            4,
            ::image::Rgba([20, 40, 80, 128]),
        ))]);
        let prepared =
            prepare_upload(&image, 0, 0, PreparationLimits { side: 3, bytes: 24 }).unwrap();
        assert_eq!(prepared.dimensions, [3, 1]);
        assert_eq!(prepared.pixels.len(), 12);
        assert!(
            prepared
                .pixels
                .as_chunks::<4>()
                .0
                .iter()
                .all(|pixel| *pixel == [40, 20, 10, 128])
        );
        let portable = orient_with_limits(
            &image,
            0,
            1,
            true,
            0,
            PreparationLimits { side: 3, bytes: 24 },
        )
        .unwrap();
        assert_eq!(portable.size(0), size(DevicePixels(1), DevicePixels(3)));
        assert_eq!(portable.as_bytes(0).unwrap().len(), 12);
        assert_eq!(
            image.size(0),
            size(DevicePixels(8), DevicePixels(4)),
            "original natural metadata is retained"
        );
        assert_eq!(
            bounded_dimensions([48_000, 24_000], PORTABLE_LIMITS),
            Some([4096, 2048])
        );
        assert_eq!(
            bounded_dimensions(
                [u32::MAX, 1],
                PreparationLimits {
                    side: u32::MAX,
                    bytes: 16
                }
            ),
            Some([4, 1])
        );
        assert_eq!(
            bounded_dimensions([1, 1], PreparationLimits { side: 1, bytes: 3 }),
            None
        );
    }
}

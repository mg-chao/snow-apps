//! Metal-backed Core Image composition with a bounded IOSurface output pool.
//! Rendering completes before publishing an immutable lease. No CPU mapping occurs.
use crate::{MacError, MacResult};
use objc2::rc::Retained;
use objc2::runtime::{AnyObject, ProtocolObject};
use objc2_core_foundation::{
    CFBoolean, CFDictionary, CFNumber, CFRetained, CFType, CGAffineTransform, CGPoint, CGRect,
    CGSize,
};
use objc2_core_graphics::*;
use objc2_core_image::{CIColor, CIContext, CIImage, kCIContextCacheIntermediates};
use objc2_core_video::*;
use objc2_foundation::{NSDictionary, NSNumber};
use objc2_metal::{
    MTLBlitCommandEncoder, MTLCommandBuffer, MTLCommandEncoder, MTLCommandQueue, MTLDevice,
    MTLPixelFormat, MTLTextureUsage,
};
use snow_media::{
    ColorDescription, PixelFormat, TransferFunction,
    geometry::{PixelRect, PixelSize},
    macos::PixelBuffer,
};
use std::ptr::NonNull;

mod batch;
use batch::OverlayBatcher;

struct MetalCopy {
    queue: Retained<ProtocolObject<dyn MTLCommandQueue>>,
    cache: CFRetained<CVMetalTextureCache>,
}

/// Source/destination rectangles use top-left pixel coordinates.
pub struct Layer<'a> {
    pub image: &'a PixelBuffer,
    pub source: PixelRect,
    pub destination: PixelRect,
}

/// Small, premultiplied sRGB effect tile. Uploads do not read capture pixels.
pub struct RgbaOverlay<'a> {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
    pub stride: usize,
    pub bytes: &'a [u8],
}

/// A single camera image. The revision must change whenever its pixels change.
pub struct RevisionedRgbaOverlay<'a> {
    pub revision: u64,
    pub overlay: RgbaOverlay<'a>,
}

enum Background<'a, 'b> {
    Transient(&'a [RgbaOverlay<'b>]),
    Cached(Option<&'a RevisionedRgbaOverlay<'b>>),
}
impl Background<'_, '_> {
    fn overlays(&self) -> &[RgbaOverlay<'_>] {
        match self {
            Self::Transient(overlays) => overlays,
            Self::Cached(Some(overlay)) => std::slice::from_ref(&overlay.overlay),
            Self::Cached(None) => &[],
        }
    }
}

struct CachedOverlay {
    key: (u64, u32, u32, usize),
    image: Retained<CIImage>,
}

pub struct Compositor {
    context: Retained<CIContext>,
    pool: CFRetained<CVPixelBufferPool>,
    threshold: CFRetained<CFDictionary>,
    metal: Option<MetalCopy>,
    size: PixelSize,
    format: PixelFormat,
    color: ColorDescription,
    color_space: CFRetained<CGColorSpace>,
    overlay_color_space: CFRetained<CGColorSpace>,
    overlay_batcher: OverlayBatcher,
    background_overlay: Option<CachedOverlay>,
}
// SAFETY: methods require exclusive access; Core Image contexts and CoreVideo
// pools support use on worker threads. No main-thread Cocoa objects are held.
unsafe impl Send for Compositor {}

impl Compositor {
    pub fn new(size: PixelSize, format: PixelFormat, maximum_leases: u32) -> MacResult<Self> {
        // Turning intermediate caching off was slower at 1080p across repeated
        // runs, and only sometimes faster at 4K. Production keeps the default.
        Self::with_intermediate_cache(size, format, maximum_leases, true)
    }

    pub fn with_intermediate_cache(
        size: PixelSize,
        format: PixelFormat,
        maximum_leases: u32,
        cache_intermediates: bool,
    ) -> MacResult<Self> {
        size.byte_len(8)
            .map_err(|e| MacError::InvalidConfig(e.to_string()))?;
        if !(2..=16).contains(&maximum_leases) {
            return Err(MacError::InvalidConfig(
                "GPU pool capacity must be 2..=16".into(),
            ));
        }
        // Native constructors autorelease internal objects even when their
        // returned handles are retained. Drain those references here so replacing
        // a compositor does not retain its context until the capture worker exits.
        objc2::rc::autoreleasepool(|_| unsafe {
            let (fourcc, color, space) = match format {
                PixelFormat::Bgra8 => (
                    kCVPixelFormatType_32BGRA,
                    ColorDescription::SRGB,
                    kCGColorSpaceSRGB,
                ),
                PixelFormat::Rgba16Float => (
                    kCVPixelFormatType_64RGBAHalf,
                    ColorDescription {
                        transfer: TransferFunction::Linear,
                        ..ColorDescription::SRGB
                    },
                    kCGColorSpaceExtendedLinearSRGB,
                ),
                PixelFormat::P010
                    if size.width.is_multiple_of(2) && size.height.is_multiple_of(2) =>
                {
                    (
                        kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
                        ColorDescription::HDR10,
                        kCGColorSpaceITUR_2100_PQ,
                    )
                }
                _ => {
                    return Err(MacError::Unsupported(
                        "Metal output requires BGRA8, RGBA16Float, or even-sized P010".into(),
                    ));
                }
            };
            let device = objc2_metal::MTLCreateSystemDefaultDevice()
                .ok_or_else(|| MacError::Unsupported("Metal device unavailable".into()))?;
            let disabled = NSNumber::numberWithBool(false);
            let options = NSDictionary::from_slices(
                &[kCIContextCacheIntermediates],
                &[&*disabled as &AnyObject],
            );
            let context = if cache_intermediates {
                CIContext::contextWithMTLDevice(&device)
            } else {
                CIContext::contextWithMTLDevice_options(&device, Some(&options))
            };
            let metal = metal_copy(&device);
            let attrs = CFDictionary::<CFType, CFType>::from_slices(
                &[
                    kCVPixelBufferWidthKey.as_ref(),
                    kCVPixelBufferHeightKey.as_ref(),
                    kCVPixelBufferPixelFormatTypeKey.as_ref(),
                    kCVPixelBufferMetalCompatibilityKey.as_ref(),
                    kCVPixelBufferIOSurfacePropertiesKey.as_ref(),
                ],
                &[
                    CFNumber::new_i64(i64::from(size.width)).as_ref(),
                    CFNumber::new_i64(i64::from(size.height)).as_ref(),
                    CFNumber::new_i64(i64::from(fourcc)).as_ref(),
                    CFBoolean::new(true).as_ref(),
                    CFDictionary::<CFType, CFType>::empty().as_ref(),
                ],
            );
            let mut pool = std::ptr::null_mut();
            let status = CVPixelBufferPool::create(
                None,
                None,
                Some(attrs.as_opaque()),
                NonNull::from(&mut pool),
            );
            if status != 0 {
                return Err(MacError::Unsupported(format!(
                    "CoreVideo pool creation failed: {status}"
                )));
            }
            let pool = CFRetained::from_raw(NonNull::new(pool).ok_or(MacError::Inactive)?);
            let threshold = CFDictionary::from_slices(
                &[kCVPixelBufferPoolAllocationThresholdKey],
                &[&*CFNumber::new_i64(i64::from(maximum_leases))],
            );
            let threshold = CFRetained::retain(NonNull::from(threshold.as_opaque()));
            let color_space = CGColorSpace::with_name(Some(space))
                .ok_or_else(|| MacError::Unsupported("output color space unavailable".into()))?;
            let overlay_color_space =
                CGColorSpace::with_name(Some(kCGColorSpaceSRGB)).ok_or(MacError::Inactive)?;
            Ok(Self {
                context,
                pool,
                threshold,
                metal,
                size,
                format,
                color,
                color_space,
                overlay_color_space,
                overlay_batcher: OverlayBatcher::default(),
                background_overlay: None,
            })
        })
    }

    /// Bit-exact detach of a same-size buffer into the output pool.
    /// Callers fall back to [`Self::compose`] when this returns [`MacError::Unsupported`].
    pub fn copy_identical(&mut self, image: &PixelBuffer) -> MacResult<PixelBuffer> {
        if image.size() != self.size || image.format() != self.format {
            return Err(MacError::Unsupported(
                "identical copy requires the compositor size and format".into(),
            ));
        }
        let Some(metal_format) = metal_pixel_format(self.format) else {
            return Err(MacError::Unsupported(
                "identical copy does not cover this pixel format".into(),
            ));
        };
        if self.metal.is_none() {
            return Err(MacError::Unsupported(
                "Metal texture copy is unavailable".into(),
            ));
        }
        objc2::rc::autoreleasepool(|_| unsafe {
            let buffer = self.allocate_output()?;
            let metal = self
                .metal
                .as_ref()
                .ok_or_else(|| MacError::Unsupported("Metal texture copy is unavailable".into()))?;
            let width = self.size.width as usize;
            let height = self.size.height as usize;
            let source = metal_texture(
                &metal.cache,
                image.native_buffer(),
                metal_format,
                width,
                height,
            )?;
            let destination = metal_texture(&metal.cache, &buffer, metal_format, width, height)?;
            let source_texture = CVMetalTextureGetTexture(&source)
                .ok_or_else(|| MacError::Unsupported("source has no Metal texture".into()))?;
            let destination_texture = CVMetalTextureGetTexture(&destination)
                .ok_or_else(|| MacError::Unsupported("destination has no Metal texture".into()))?;
            let command = metal
                .queue
                .commandBuffer()
                .ok_or_else(|| MacError::Unsupported("Metal command buffer unavailable".into()))?;
            let encoder = command
                .blitCommandEncoder()
                .ok_or_else(|| MacError::Unsupported("Metal blit encoder unavailable".into()))?;
            encoder.copyFromTexture_toTexture(&source_texture, &destination_texture);
            // Discrete GPUs keep the blit in managed storage until this runs.
            // Apple silicon treats it as a no-op.
            encoder.synchronizeTexture_slice_level(&destination_texture, 0, 0);
            encoder.endEncoding();
            command.commit();
            command.waitUntilCompleted();
            if command.status() == objc2_metal::MTLCommandBufferStatus::Error {
                return Err(MacError::Unsupported("Metal identical copy failed".into()));
            }
            drop(destination_texture);
            drop(source_texture);
            drop(destination);
            drop(source);
            metal.cache.flush(0);
            PixelBuffer::from_retained(buffer, self.color)
                .map_err(|error| MacError::Unsupported(error.to_string()))
        })
    }

    /// Backpressure returns `Timeout`; callers should drop superseded observations.
    pub fn compose(&mut self, layers: &[Layer<'_>], opaque: bool) -> MacResult<PixelBuffer> {
        self.compose_with_overlays(layers, &[], opaque)
    }
    /// Fractional source pixels preserve a proportional viewport on smaller or
    /// mixed-scale displays. The immutable full-monitor lease remains on GPU.
    pub(crate) fn compose_viewport(
        &mut self,
        image: &PixelBuffer,
        source: [f64; 4],
    ) -> MacResult<PixelBuffer> {
        let [x, y, width, height] = source;
        let input = image.size();
        if source.into_iter().any(|value| !value.is_finite())
            || x < 0.0
            || y < 0.0
            || width <= 0.0
            || height <= 0.0
            || x + width > f64::from(input.width) + 1e-7
            || y + height > f64::from(input.height) + 1e-7
        {
            return Err(MacError::InvalidConfig("invalid GPU viewport".into()));
        }
        objc2::rc::autoreleasepool(|_| unsafe {
            let buffer = self.allocate_output()?;
            let source_y = (f64::from(input.height) - y - height).max(0.0);
            let sx = f64::from(self.size.width) / width;
            let sy = f64::from(self.size.height) / height;
            let source = CIImage::imageWithCVPixelBuffer(image.native_buffer())
                .imageByCroppingToRect(rect(x, source_y, width, height));
            let result = source.imageByApplyingTransform_highQualityDownsample(
                CGAffineTransform {
                    a: sx,
                    b: 0.0,
                    c: 0.0,
                    d: sy,
                    tx: -x * sx,
                    ty: -source_y * sy,
                },
                true,
            );
            let bounds = rect(
                0.0,
                0.0,
                f64::from(self.size.width),
                f64::from(self.size.height),
            );
            self.context.render_toCVPixelBuffer_bounds_colorSpace(
                &result,
                &buffer,
                bounds,
                Some(&self.color_space),
            );
            PixelBuffer::from_retained(buffer, self.color)
                .map_err(|error| MacError::Unsupported(error.to_string()))
        })
    }
    pub fn compose_with_overlays(
        &mut self,
        layers: &[Layer<'_>],
        overlays: &[RgbaOverlay<'_>],
        opaque: bool,
    ) -> MacResult<PixelBuffer> {
        self.compose_with_highlight(layers, overlays, &[], opaque)
    }
    pub fn compose_with_highlight(
        &mut self,
        layers: &[Layer<'_>],
        overlays: &[RgbaOverlay<'_>],
        highlight: &[RgbaOverlay<'_>],
        opaque: bool,
    ) -> MacResult<PixelBuffer> {
        self.compose_with_background_overlays(layers, &[], overlays, highlight, opaque)
    }
    pub fn compose_with_background_overlays(
        &mut self,
        layers: &[Layer<'_>],
        background_overlays: &[RgbaOverlay<'_>],
        overlays: &[RgbaOverlay<'_>],
        highlight: &[RgbaOverlay<'_>],
        opaque: bool,
    ) -> MacResult<PixelBuffer> {
        self.compose_impl(
            layers,
            Background::Transient(background_overlays),
            overlays,
            highlight,
            opaque,
            true,
        )
    }

    /// Retain only the current immutable camera image. Core Image can reuse its
    /// source upload while faster desktop/effect frames continue to be composed.
    /// Passing None releases it; a geometry change also invalidates the cache.
    pub fn compose_with_cached_background_overlay(
        &mut self,
        layers: &[Layer<'_>],
        background: Option<&RevisionedRgbaOverlay<'_>>,
        overlays: &[RgbaOverlay<'_>],
        highlight: &[RgbaOverlay<'_>],
        opaque: bool,
    ) -> MacResult<PixelBuffer> {
        self.compose_impl(
            layers,
            Background::Cached(background),
            overlays,
            highlight,
            opaque,
            true,
        )
    }

    /// Also release an invisible camera when callers can bypass composition.
    pub fn clear_cached_background_overlay(&mut self) {
        self.background_overlay = None;
    }

    fn compose_impl(
        &mut self,
        layers: &[Layer<'_>],
        background: Background<'_, '_>,
        overlays: &[RgbaOverlay<'_>],
        highlight: &[RgbaOverlay<'_>],
        opaque: bool,
        batch_overlays: bool,
    ) -> MacResult<PixelBuffer> {
        if !matches!(&background, Background::Cached(Some(_))) {
            self.clear_cached_background_overlay();
        }
        for overlay in background
            .overlays()
            .iter()
            .chain(highlight)
            .chain(overlays)
        {
            let needed = overlay.stride.checked_mul(overlay.height as usize);
            if overlay.width == 0
                || overlay.height == 0
                || overlay.stride < overlay.width as usize * 4
                || needed.is_none_or(|len| len > overlay.bytes.len())
                || overlay.x >= self.size.width
                || overlay.y >= self.size.height
            {
                return Err(MacError::InvalidConfig("invalid RGBA effect tile".into()));
            }
        }
        for layer in layers {
            validate_rect(layer.source, layer.image.size())?;
            validate_rect(layer.destination, self.size)?;
        }
        objc2::rc::autoreleasepool(|_| unsafe {
            let buffer = self.allocate_output()?;
            let bounds = rect(0.0, 0.0, self.size.width as f64, self.size.height as f64);
            let background_color = CIColor::colorWithRed_green_blue_alpha(
                0.0,
                0.0,
                0.0,
                if opaque { 1.0 } else { 0.0 },
            );
            let mut result =
                CIImage::imageWithColor(&background_color).imageByCroppingToRect(bounds);
            for layer in layers {
                let s = layer.source;
                let d = layer.destination;
                let source_y = f64::from(layer.image.size().height - s.y - s.height);
                let destination_y = f64::from(self.size.height - d.y - d.height);
                let sx = f64::from(d.width) / f64::from(s.width);
                let sy = f64::from(d.height) / f64::from(s.height);
                let source = CIImage::imageWithCVPixelBuffer(layer.image.native_buffer())
                    .imageByCroppingToRect(rect(
                        f64::from(s.x),
                        source_y,
                        f64::from(s.width),
                        f64::from(s.height),
                    ));
                let transformed = source.imageByApplyingTransform_highQualityDownsample(
                    CGAffineTransform {
                        a: sx,
                        b: 0.0,
                        c: 0.0,
                        d: sy,
                        tx: f64::from(d.x) - f64::from(s.x) * sx,
                        ty: destination_y - source_y * sy,
                    },
                    true,
                );
                result = transformed.imageByCompositingOverImage(&result);
            }
            let srgb = &self.overlay_color_space;
            let background_overlays = match background {
                Background::Transient(overlays) => overlays,
                Background::Cached(overlay) => {
                    if let Some(revisioned) = overlay {
                        let overlay = &revisioned.overlay;
                        let key = (
                            revisioned.revision,
                            overlay.width,
                            overlay.height,
                            overlay.stride,
                        );
                        if self
                            .background_overlay
                            .as_ref()
                            .is_none_or(|cached| cached.key != key)
                        {
                            self.background_overlay = Some(CachedOverlay {
                                key,
                                image: bitmap_image(overlay, srgb),
                            });
                        }
                        let positioned = position_overlay(
                            &self.background_overlay.as_ref().unwrap().image,
                            overlay,
                            self.size,
                        );
                        result = positioned.imageByCompositingOverImage(&result);
                    }
                    &[]
                }
            };
            for (mut group, multiply) in [
                (background_overlays, false),
                (highlight, true),
                (overlays, false),
            ] {
                while !group.is_empty() {
                    let (packed, count);
                    let overlay = if batch_overlays {
                        (packed, count) = self.overlay_batcher.prepare(group, self.size);
                        &packed
                    } else {
                        count = 1;
                        &group[0]
                    };
                    let image = bitmap_image(overlay, srgb);
                    let positioned = position_overlay(&image, overlay, self.size);
                    result = if multiply {
                        let parameters = objc2_foundation::NSDictionary::from_slices(
                            &[objc2_foundation::ns_string!("inputBackgroundImage")],
                            &[&*result as &objc2::runtime::AnyObject],
                        );
                        positioned.imageByApplyingFilter_withInputParameters(
                            objc2_foundation::ns_string!("CIMultiplyBlendMode"),
                            &parameters,
                        )
                    } else {
                        positioned.imageByCompositingOverImage(&result)
                    };
                    // NSData copied the packed bytes, so the scratch can be reused
                    // by the next group while the native image graph retains its data.
                    group = &group[count..];
                }
            }
            if self.color == ColorDescription::HDR10 {
                for (key, value) in [
                    (
                        kCVImageBufferColorPrimariesKey,
                        kCVImageBufferColorPrimaries_ITU_R_2020,
                    ),
                    (
                        kCVImageBufferTransferFunctionKey,
                        kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ,
                    ),
                    (
                        kCVImageBufferYCbCrMatrixKey,
                        kCVImageBufferYCbCrMatrix_ITU_R_2020,
                    ),
                ] {
                    buffer.set_attachment(key, value.as_ref(), CVAttachmentMode::ShouldPropagate);
                }
            }
            // Configure the destination YCbCr matrix before rendering. Setting
            // it afterward labels pixels already converted with the default
            // matrix and produces incorrect HDR colors on the first pool use.
            // Core Image converts from each source's color attachments into the
            // requested output color space (including linear HDR -> BT.2020/PQ).
            self.context.render_toCVPixelBuffer_bounds_colorSpace(
                &result,
                &buffer,
                bounds,
                Some(&self.color_space),
            );
            PixelBuffer::from_retained(buffer, self.color)
                .map_err(|e| MacError::Unsupported(e.to_string()))
        })
    }

    fn allocate_output(&self) -> MacResult<CFRetained<CVPixelBuffer>> {
        unsafe {
            let mut buffer = std::ptr::null_mut();
            let status = CVPixelBufferPool::create_pixel_buffer_with_aux_attributes(
                None,
                &self.pool,
                Some(&self.threshold),
                NonNull::from(&mut buffer),
            );
            if status == kCVReturnWouldExceedAllocationThreshold {
                return Err(MacError::Timeout);
            }
            if status != 0 {
                return Err(MacError::Unsupported(format!(
                    "GPU output allocation failed: {status}"
                )));
            }
            Ok(CFRetained::from_raw(
                NonNull::new(buffer).ok_or(MacError::Inactive)?,
            ))
        }
    }
}

fn bitmap_image(overlay: &RgbaOverlay<'_>, srgb: &CGColorSpace) -> Retained<CIImage> {
    let data = objc2_foundation::NSData::with_bytes(overlay.bytes);
    unsafe {
        CIImage::imageWithBitmapData_bytesPerRow_size_format_colorSpace(
            &data,
            overlay.stride,
            CGSize {
                width: f64::from(overlay.width),
                height: f64::from(overlay.height),
            },
            objc2_core_image::kCIFormatRGBA8,
            Some(srgb),
        )
    }
}

fn position_overlay(
    image: &CIImage,
    overlay: &RgbaOverlay<'_>,
    size: PixelSize,
) -> Retained<CIImage> {
    unsafe {
        image.imageByApplyingTransform(CGAffineTransform {
            a: 1.0,
            b: 0.0,
            c: 0.0,
            d: 1.0,
            tx: f64::from(overlay.x),
            ty: f64::from(size.height) - f64::from(overlay.y) - f64::from(overlay.height),
        })
    }
}
fn rect(x: f64, y: f64, width: f64, height: f64) -> CGRect {
    CGRect {
        origin: CGPoint { x, y },
        size: CGSize { width, height },
    }
}
fn validate_rect(rect: PixelRect, size: PixelSize) -> MacResult<()> {
    if rect.width == 0
        || rect.height == 0
        || rect
            .x
            .checked_add(rect.width)
            .is_none_or(|x| x > size.width)
        || rect
            .y
            .checked_add(rect.height)
            .is_none_or(|y| y > size.height)
    {
        return Err(MacError::InvalidConfig(
            "composition rectangle outside image".into(),
        ));
    }
    Ok(())
}

fn metal_copy(device: &ProtocolObject<dyn MTLDevice>) -> Option<MetalCopy> {
    let queue = device.newCommandQueue()?;
    let usage = CFNumber::new_i64(
        (MTLTextureUsage::ShaderRead | MTLTextureUsage::ShaderWrite).bits() as i64,
    );
    let attrs = CFDictionary::<CFType, CFType>::from_slices(
        &[unsafe { kCVMetalTextureUsage }.as_ref()],
        &[usage.as_ref()],
    );
    let mut cache = std::ptr::null_mut();
    let status = unsafe {
        CVMetalTextureCache::create(
            None,
            Some(attrs.as_opaque()),
            device,
            None,
            NonNull::from(&mut cache),
        )
    };
    if status != 0 {
        return None;
    }
    NonNull::new(cache).map(|cache| MetalCopy {
        queue,
        cache: unsafe { CFRetained::from_raw(cache) },
    })
}

fn metal_pixel_format(format: PixelFormat) -> Option<MTLPixelFormat> {
    match format {
        PixelFormat::Bgra8 => Some(MTLPixelFormat::BGRA8Unorm),
        PixelFormat::Rgba16Float => Some(MTLPixelFormat::RGBA16Float),
        _ => None,
    }
}

fn metal_texture(
    cache: &CVMetalTextureCache,
    buffer: &CVPixelBuffer,
    format: MTLPixelFormat,
    width: usize,
    height: usize,
) -> MacResult<CFRetained<CVMetalTexture>> {
    let mut texture = std::ptr::null_mut();
    let status = unsafe {
        CVMetalTextureCache::create_texture_from_image(
            None,
            cache,
            buffer,
            None,
            format,
            width,
            height,
            0,
            NonNull::from(&mut texture),
        )
    };
    if status != 0 {
        return Err(MacError::Unsupported(format!(
            "Metal texture allocation failed: {status}"
        )));
    }
    NonNull::new(texture)
        .map(|texture| unsafe { CFRetained::from_raw(texture) })
        .ok_or(MacError::Inactive)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn follow_viewport_crops_a_held_monitor_surface_at_fixed_output_size() {
        let mut monitor =
            Compositor::new(PixelSize::new(16, 8).unwrap(), PixelFormat::Bgra8, 2).unwrap();
        let red = [255u8, 0, 0, 255].repeat(8 * 8);
        let blue = [0u8, 0, 255, 255].repeat(8 * 8);
        let source = monitor
            .compose_with_overlays(
                &[],
                &[
                    RgbaOverlay {
                        x: 0,
                        y: 0,
                        width: 8,
                        height: 8,
                        stride: 32,
                        bytes: &red,
                    },
                    RgbaOverlay {
                        x: 8,
                        y: 0,
                        width: 8,
                        height: 8,
                        stride: 32,
                        bytes: &blue,
                    },
                ],
                true,
            )
            .unwrap();
        let mut viewport =
            Compositor::new(PixelSize::new(8, 8).unwrap(), PixelFormat::Bgra8, 3).unwrap();
        for (x, color) in [(0.0, [255, 0, 0, 255]), (8.0, [0, 0, 255, 255])] {
            let image = viewport
                .compose_viewport(&source, [x, 0.0, 8.0, 8.0])
                .unwrap();
            assert_eq!(image.size(), PixelSize::new(8, 8).unwrap());
            let cpu = image.to_cpu_format(PixelFormat::Rgba8).unwrap();
            for y in 1..7 {
                for x in 1..7 {
                    let offset = cpu.planes[0].offset + y * cpu.planes[0].stride + x * 4;
                    assert_eq!(&cpu.bytes[offset..offset + 4], &color);
                }
            }
        }
        assert!(
            viewport
                .compose_viewport(&source, [8.0, 0.0, 8.01, 8.0])
                .is_err()
        );
        assert!(
            viewport
                .compose_viewport(&source, [0.0, f64::NAN, 8.0, 8.0])
                .is_err()
        );
        // A fractional backing-pixel crop does not round or change output extent.
        assert_eq!(
            viewport
                .compose_viewport(&source, [8.5, 0.5, 7.5, 7.5])
                .unwrap()
                .size(),
            PixelSize::new(8, 8).unwrap()
        );
    }

    #[test]
    fn cached_camera_owns_one_immutable_image_and_refreshes_on_revision_geometry_and_reappearance()
    {
        let mut compositor =
            Compositor::new(PixelSize::new(16, 16).unwrap(), PixelFormat::Bgra8, 2).unwrap();
        let red = [220, 10, 20, 255].repeat(4 * 4);
        let blue = [10, 20, 220, 255].repeat(4 * 4);
        let camera = |revision, width, bytes| RevisionedRgbaOverlay {
            revision,
            overlay: RgbaOverlay {
                x: 2,
                y: 3,
                width,
                height: 4,
                stride: 16,
                bytes,
            },
        };
        fn render(
            compositor: &mut Compositor,
            overlay: Option<&RevisionedRgbaOverlay<'_>>,
        ) -> snow_media::CpuFrame {
            compositor
                .compose_with_cached_background_overlay(&[], overlay, &[], &[], true)
                .unwrap()
                .to_cpu_format(PixelFormat::Rgba8)
                .unwrap()
        }
        let first = render(&mut compositor, Some(&camera(1, 4, &red)));
        let original = compositor
            .background_overlay
            .as_ref()
            .unwrap()
            .image
            .clone();
        // A different borrowed buffer with the same revision must reuse the owned
        // immutable source, rather than copy/upload it again or borrow its bytes.
        let repeated = render(&mut compositor, Some(&camera(1, 4, &blue)));
        assert_eq!(first.bytes, repeated.bytes);
        assert!(std::ptr::eq(
            &*original,
            &*compositor.background_overlay.as_ref().unwrap().image
        ));
        let changed = render(&mut compositor, Some(&camera(2, 4, &blue)));
        assert_ne!(first.bytes, changed.bytes);
        assert!(!std::ptr::eq(
            &*original,
            &*compositor.background_overlay.as_ref().unwrap().image
        ));
        let old = compositor
            .background_overlay
            .as_ref()
            .unwrap()
            .image
            .clone();
        render(&mut compositor, Some(&camera(2, 3, &blue)));
        assert!(!std::ptr::eq(
            &*old,
            &*compositor.background_overlay.as_ref().unwrap().image
        ));
        let hidden = render(&mut compositor, None);
        assert!(compositor.background_overlay.is_none());
        assert!(
            hidden
                .bytes
                .chunks_exact(4)
                .all(|pixel| pixel == [0, 0, 0, 255])
        );
        assert_eq!(
            render(&mut compositor, Some(&camera(1, 4, &red))).bytes,
            first.bytes
        );
        compositor.clear_cached_background_overlay();
        assert!(compositor.background_overlay.is_none());
        // A later input's revision can restart after explicit retirement.
        assert_eq!(
            render(&mut compositor, Some(&camera(1, 4, &blue))).bytes,
            changed.bytes
        );
        let overlay = camera(1, 4, &blue);
        let first_lease = compositor
            .compose_with_cached_background_overlay(&[], Some(&overlay), &[], &[], true)
            .unwrap();
        let second_lease = compositor
            .compose_with_cached_background_overlay(&[], Some(&overlay), &[], &[], true)
            .unwrap();
        assert!(matches!(
            compositor.compose_with_cached_background_overlay(&[], None, &[], &[], true,),
            Err(MacError::Timeout)
        ));
        assert!(
            compositor.background_overlay.is_none(),
            "pool backpressure cannot retain an invisible camera"
        );
        drop((first_lease, second_lease));
    }

    #[test]
    fn cached_camera_matches_uncached_pixels_with_padding_clipping_alpha_layering_and_hdr() {
        let size = PixelSize::new(16, 16).unwrap();
        let pixels = [
            200, 20, 10, 255, 100, 10, 5, 128, 0, 0, 0, 0, 10, 220, 20, 255, 5, 110, 10, 128, 0, 0,
            0, 0, 20, 10, 240, 255, 10, 5, 120, 128, 0, 0, 0, 0,
        ];
        let foreground = [255; 4 * 4 * 4];
        for format in [
            PixelFormat::Bgra8,
            PixelFormat::Rgba16Float,
            PixelFormat::P010,
        ] {
            let mut cached = Compositor::new(size, format, 2).unwrap();
            let mut reference = Compositor::new(size, format, 2).unwrap();
            for (x, y) in [(2, 3), (15, 15), (7, 5)] {
                let camera = RevisionedRgbaOverlay {
                    revision: 1,
                    overlay: RgbaOverlay {
                        x,
                        y,
                        width: 2,
                        height: 3,
                        stride: 12,
                        bytes: &pixels,
                    },
                };
                let overlays = [RgbaOverlay {
                    x: 3,
                    y: 4,
                    width: 4,
                    height: 4,
                    stride: 16,
                    bytes: &foreground,
                }];
                for opaque in [false, true] {
                    let actual = cached
                        .compose_with_cached_background_overlay(
                            &[],
                            Some(&camera),
                            &overlays,
                            &overlays,
                            opaque,
                        )
                        .unwrap()
                        .to_cpu()
                        .unwrap();
                    let expected = reference
                        .compose_with_background_overlays(
                            &[],
                            std::slice::from_ref(&camera.overlay),
                            &overlays,
                            &overlays,
                            opaque,
                        )
                        .unwrap()
                        .to_cpu()
                        .unwrap();
                    assert_eq!(
                        actual.bytes, expected.bytes,
                        "{format:?}, ({x}, {y}), opaque={opaque}"
                    );
                }
            }
            let invalid = RevisionedRgbaOverlay {
                revision: 2,
                overlay: RgbaOverlay {
                    x: 0,
                    y: 0,
                    width: 4,
                    height: 4,
                    stride: usize::MAX,
                    bytes: &pixels,
                },
            };
            assert!(
                cached
                    .compose_with_cached_background_overlay(&[], Some(&invalid), &[], &[], true,)
                    .is_err()
            );
        }
    }

    #[test]
    fn batched_overlays_match_individual_layers_with_alpha_highlights_hdr_and_clipping() {
        let size = PixelSize::new(256, 192).unwrap();
        let rectangles = [
            (4, 3, 48, 40, 8),
            (60, 7, 40, 41, 12),
            (110, 9, 30, 48, 0),
            (135, 15, 50, 33, 4),
            (200, 160, 30, 30, 8),
            (240, 170, 32, 32, 12),
        ];
        let storage: Vec<_> = rectangles
            .iter()
            .enumerate()
            .map(|(index, &(_, _, w, h, padding))| {
                let stride = w as usize * 4 + padding;
                let mut bytes = vec![0xa5; stride * h as usize];
                for y in 0..h as usize {
                    for x in 0..w as usize {
                        let alpha = ((x * 13 + y * 7 + index * 31) % 256) as u8;
                        let from = y * stride + x * 4;
                        bytes[from..from + 4].copy_from_slice(&[
                            (u16::from(alpha) * 220 / 255) as u8,
                            (u16::from(alpha) * 70 / 255) as u8,
                            (u16::from(alpha) * 130 / 255) as u8,
                            alpha,
                        ]);
                    }
                }
                bytes
            })
            .collect();
        let overlays: Vec<_> = rectangles
            .iter()
            .zip(&storage)
            .map(|(&(x, y, width, height, padding), bytes)| RgbaOverlay {
                x,
                y,
                width,
                height,
                stride: width as usize * 4 + padding,
                bytes,
            })
            .collect();
        for format in [
            PixelFormat::Bgra8,
            PixelFormat::Rgba16Float,
            PixelFormat::P010,
        ] {
            let mut compositor = Compositor::new(size, format, 4).unwrap();
            for opaque in [false, true] {
                for mixed_layers in [false, true] {
                    let background = if mixed_layers { &overlays[..2] } else { &[] };
                    let highlight = if mixed_layers { &overlays[1..4] } else { &[] };
                    let expected = compositor
                        .compose_impl(
                            &[],
                            Background::Transient(background),
                            &overlays,
                            highlight,
                            opaque,
                            false,
                        )
                        .unwrap()
                        .to_cpu()
                        .unwrap();
                    let actual = compositor
                        .compose_impl(
                            &[],
                            Background::Transient(background),
                            &overlays,
                            highlight,
                            opaque,
                            true,
                        )
                        .unwrap()
                        .to_cpu()
                        .unwrap();
                    assert_eq!(
                        actual.bytes, expected.bytes,
                        "format={format:?}, opaque={opaque}, mixed_layers={mixed_layers}"
                    );
                    // Reusing the batch for a smaller scene cannot retain earlier pixels.
                    let expected = compositor
                        .compose_impl(
                            &[],
                            Background::Transient(&[]),
                            &overlays[..2],
                            &[],
                            opaque,
                            false,
                        )
                        .unwrap()
                        .to_cpu()
                        .unwrap();
                    let actual = compositor
                        .compose_impl(
                            &[],
                            Background::Transient(&[]),
                            &overlays[..2],
                            &[],
                            opaque,
                            true,
                        )
                        .unwrap()
                        .to_cpu()
                        .unwrap();
                    assert_eq!(
                        actual.bytes, expected.bytes,
                        "reused batch format={format:?}"
                    );
                }
            }
        }
    }

    #[test]
    fn recreated_contexts_are_released_before_worker_pool_drains() {
        std::thread::spawn(|| {
            // A capture worker can have a pool spanning multiple compositor
            // replacements. Each constructor must drain its own autoreleases.
            objc2::rc::autoreleasepool(|_| {
                let size = PixelSize::new(32, 18).unwrap();
                for cache_intermediates in [true, false] {
                    for _ in 0..3 {
                        let mut compositor = Compositor::with_intermediate_cache(
                            size,
                            PixelFormat::Bgra8,
                            4,
                            cache_intermediates,
                        )
                        .expect("metal compositor");
                        let context = objc2::rc::Weak::from_retained(&compositor.context);
                        let frame = compositor
                            .compose(&[], true)
                            .expect("retained context renders");
                        assert_eq!(frame.size(), size);
                        drop(frame);
                        drop(compositor);
                        assert!(
                            context.load().is_none(),
                            "dropped compositor context outlived its owner"
                        );
                    }
                }
            });
        })
        .join()
        .unwrap();
    }

    #[test]
    fn rejects_overflowing_and_empty_layers() {
        let size = PixelSize::new(10, 10).unwrap();
        assert!(
            validate_rect(
                PixelRect {
                    x: u32::MAX,
                    y: 0,
                    width: 2,
                    height: 2
                },
                size
            )
            .is_err()
        );
        assert!(
            validate_rect(
                PixelRect {
                    x: 0,
                    y: 0,
                    width: 0,
                    height: 2
                },
                size
            )
            .is_err()
        );
        assert!(
            validate_rect(
                PixelRect {
                    x: 0,
                    y: 0,
                    width: 10,
                    height: 10
                },
                size
            )
            .is_ok()
        );
    }

    #[test]
    fn bitmap_overlays_preserve_top_left_rows_and_edge_clipping() {
        let size = PixelSize::new(10, 8).unwrap();
        let mut compositor = Compositor::new(size, PixelFormat::Bgra8, 4).unwrap();
        // Padded rows with distinct colors also detect accidental stride assumptions.
        let pixels = [
            255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 0, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 0, 0, 0,
            0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0,
        ];
        for (x, y) in [(2, 1), (9, 7)] {
            // Exercise both ordinary effects and the multiply highlight uploader.
            for highlight in [false, true] {
                let background = compositor
                    .compose_with_overlays(
                        &[],
                        &[RgbaOverlay {
                            x: 0,
                            y: 0,
                            width: 10,
                            height: 8,
                            stride: 40,
                            bytes: &[255; 320],
                        }],
                        true,
                    )
                    .unwrap();
                let layer = Layer {
                    image: &background,
                    source: PixelRect {
                        x: 0,
                        y: 0,
                        width: 10,
                        height: 8,
                    },
                    destination: PixelRect {
                        x: 0,
                        y: 0,
                        width: 10,
                        height: 8,
                    },
                };
                let overlays = [RgbaOverlay {
                    x,
                    y,
                    width: 2,
                    height: 3,
                    stride: 12,
                    bytes: &pixels,
                }];
                let frame = compositor
                    .compose_with_highlight(
                        &[layer],
                        if highlight { &[] } else { &overlays },
                        if highlight { &overlays } else { &[] },
                        true,
                    )
                    .unwrap()
                    .to_cpu_format(PixelFormat::Rgba8)
                    .unwrap();
                for py in 0..8usize {
                    for px in 0..10usize {
                        let offset = frame.planes[0].offset + py * frame.planes[0].stride + px * 4;
                        let expected = if px >= x as usize
                            && px < x as usize + 2
                            && py >= y as usize
                            && py < y as usize + 3
                        {
                            let row = py - y as usize;
                            &pixels[row * 12..row * 12 + 4]
                        } else {
                            &[255; 4]
                        };
                        assert_eq!(
                            &frame.bytes[offset..offset + 4],
                            expected,
                            "overlay ({x}, {y}), pixel ({px}, {py}), highlight={highlight}"
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn identical_metal_copy_preserves_packed_bytes() {
        use objc2_core_video::{
            CVPixelBufferGetBaseAddress, CVPixelBufferGetBytesPerRow, CVPixelBufferLockBaseAddress,
            CVPixelBufferLockFlags, CVPixelBufferUnlockBaseAddress,
        };
        let size = PixelSize::new(32, 18).unwrap();
        let mut compositor =
            Compositor::new(size, snow_media::PixelFormat::Bgra8, 4).expect("metal compositor");
        let source = compositor.compose(&[], true).unwrap();
        unsafe {
            assert_eq!(
                CVPixelBufferLockBaseAddress(
                    source.native_buffer(),
                    CVPixelBufferLockFlags::empty()
                ),
                0
            );
            let stride = CVPixelBufferGetBytesPerRow(source.native_buffer());
            let ptr = CVPixelBufferGetBaseAddress(source.native_buffer()).cast::<u8>();
            for y in 0..18 {
                for x in 0..32 {
                    let pixel = ptr.add(y * stride + x * 4);
                    pixel.write((x * 3) as u8);
                    pixel.add(1).write((y * 5) as u8);
                    pixel.add(2).write(9);
                    pixel.add(3).write(255);
                }
            }
            CVPixelBufferUnlockBaseAddress(source.native_buffer(), CVPixelBufferLockFlags::empty());
        }
        let copied = compositor.copy_identical(&source).unwrap();
        assert_eq!(
            source.to_cpu().unwrap().bytes.as_ref(),
            copied.to_cpu().unwrap().bytes.as_ref()
        );
    }

    #[test]
    fn identical_metal_copy_recycles_small_pools_without_changing_retained_frames() {
        let size = PixelSize::new(32, 18).unwrap();
        let producer = Compositor::new(size, PixelFormat::Bgra8, 3).unwrap();
        let mut copy = Compositor::new(size, PixelFormat::Bgra8, 4).unwrap();
        let mut held = Vec::new();
        for value in 0u8..128 {
            let source = unsafe {
                PixelBuffer::from_retained(
                    producer.allocate_output().unwrap(),
                    ColorDescription::SRGB,
                )
            }
            .unwrap();
            unsafe {
                assert_eq!(
                    CVPixelBufferLockBaseAddress(
                        source.native_buffer(),
                        CVPixelBufferLockFlags::empty()
                    ),
                    0
                );
                let stride = CVPixelBufferGetBytesPerRow(source.native_buffer());
                CVPixelBufferGetBaseAddress(source.native_buffer())
                    .cast::<u8>()
                    .write_bytes(value, stride * size.height as usize);
                CVPixelBufferUnlockBaseAddress(
                    source.native_buffer(),
                    CVPixelBufferLockFlags::empty(),
                );
            }
            let image = copy.copy_identical(&source).unwrap();
            assert!(
                image
                    .to_cpu()
                    .unwrap()
                    .bytes
                    .iter()
                    .all(|byte| *byte == value)
            );
            if held.len() < 3 {
                held.push(image);
            }
            for (value, frame) in held.iter().enumerate() {
                assert!(
                    frame
                        .to_cpu()
                        .unwrap()
                        .bytes
                        .iter()
                        .all(|byte| *byte == value as u8)
                );
            }
        }
        let fourth = copy.copy_identical(&held[0]).unwrap();
        assert!(matches!(
            copy.copy_identical(&held[0]),
            Err(MacError::Timeout)
        ));
        drop(fourth);
        assert!(copy.copy_identical(&held[0]).is_ok());
    }
}

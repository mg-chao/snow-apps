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
pub struct Compositor {
    context: Retained<CIContext>,
    pool: CFRetained<CVPixelBufferPool>,
    threshold: CFRetained<CFDictionary>,
    metal: Option<MetalCopy>,
    size: PixelSize,
    format: PixelFormat,
    color: ColorDescription,
    color_space: CFRetained<CGColorSpace>,
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
        unsafe {
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
            Ok(Self {
                context,
                pool,
                threshold,
                metal,
                size,
                format,
                color,
                color_space,
            })
        }
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
        for overlay in highlight.iter().chain(overlays) {
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
            let background = CIColor::colorWithRed_green_blue_alpha(
                0.0,
                0.0,
                0.0,
                if opaque { 1.0 } else { 0.0 },
            );
            let mut result = CIImage::imageWithColor(&background).imageByCroppingToRect(bounds);
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
            let srgb =
                CGColorSpace::with_name(Some(kCGColorSpaceSRGB)).ok_or(MacError::Inactive)?;
            for (overlay, multiply) in highlight
                .iter()
                .map(|tile| (tile, true))
                .chain(overlays.iter().map(|tile| (tile, false)))
            {
                let data = objc2_foundation::NSData::with_bytes(overlay.bytes);
                let image = CIImage::imageWithBitmapData_bytesPerRow_size_format_colorSpace(
                    &data,
                    overlay.stride,
                    CGSize {
                        width: f64::from(overlay.width),
                        height: f64::from(overlay.height),
                    },
                    objc2_core_image::kCIFormatRGBA8,
                    Some(&srgb),
                );
                let positioned = image.imageByApplyingTransform(CGAffineTransform {
                    a: 1.0,
                    b: 0.0,
                    c: 0.0,
                    d: 1.0,
                    tx: f64::from(overlay.x),
                    ty: f64::from(self.size.height)
                        - f64::from(overlay.y)
                        - f64::from(overlay.height),
                });
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
}

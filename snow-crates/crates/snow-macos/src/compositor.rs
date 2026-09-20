//! Metal-backed Core Image composition with a bounded IOSurface output pool.
//! Rendering completes before publishing an immutable lease. No CPU mapping occurs.
use crate::{MacError, MacResult};
use objc2::rc::Retained;
use objc2_core_foundation::{
    CFBoolean, CFDictionary, CFNumber, CFRetained, CFType, CGAffineTransform, CGPoint, CGRect,
    CGSize,
};
use objc2_core_graphics::*;
use objc2_core_image::{CIColor, CIContext, CIImage};
use objc2_core_video::*;
use snow_media::{
    ColorDescription, PixelFormat, TransferFunction,
    geometry::{PixelRect, PixelSize},
    macos::PixelBuffer,
};
use std::ptr::NonNull;

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
    size: PixelSize,
    color: ColorDescription,
    color_space: CFRetained<CGColorSpace>,
}
// SAFETY: methods require exclusive access; Core Image contexts and CoreVideo
// pools support use on worker threads. No main-thread Cocoa objects are held.
unsafe impl Send for Compositor {}

impl Compositor {
    pub fn new(size: PixelSize, format: PixelFormat, maximum_leases: u32) -> MacResult<Self> {
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
            let context = CIContext::contextWithMTLDevice(&device);
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
                size,
                color,
                color_space,
            })
        }
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
            let buffer = CFRetained::from_raw(NonNull::new(buffer).ok_or(MacError::Inactive)?);
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
}

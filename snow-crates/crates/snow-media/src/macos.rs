//! Immutable CoreVideo leases. Native consumers borrow the buffer for the lease lifetime.
use crate::{
    ColorDescription, ColorPrimaries, ColorRange, CpuFrame, PixelFormat, PlaneLayout,
    TransferFunction, geometry::PixelSize,
};
use objc2_core_foundation::{CFRetained, CFString};
use objc2_core_video::*;
use std::sync::{Arc, Mutex};

#[derive(Debug, thiserror::Error)]
pub enum PixelBufferError {
    #[error("unsupported CoreVideo pixel format: {0:#x}")]
    Unsupported(u32),
    #[error("CoreVideo buffer lock failed: {0}")]
    Lock(i32),
    #[error("invalid CoreVideo buffer layout")]
    Layout,
    #[error("unsupported CoreVideo color attachment: {0}")]
    Color(String),
    #[error(transparent)]
    Conversion(#[from] crate::CpuFormatError),
}

struct Storage {
    buffer: CFRetained<CVPixelBuffer>,
    mapping: Mutex<()>,
}
// SAFETY: Ownership is retained, acquisition has completed, and callers promise
// no further writes. CPU access is serialized and only read-only locks are exposed.
unsafe impl Send for Storage {}
unsafe impl Sync for Storage {}

#[derive(Clone)]
pub struct PixelBuffer {
    storage: Arc<Storage>,
    size: PixelSize,
    format: PixelFormat,
    color: ColorDescription,
}

impl PixelBuffer {
    pub fn size(&self) -> PixelSize {
        self.size
    }
    pub fn format(&self) -> PixelFormat {
        self.format
    }
    pub fn color(&self) -> ColorDescription {
        self.color
    }
    /// Explicit CPU readback with requested channel order; HDR remains float.
    pub fn to_cpu_format(&self, format: PixelFormat) -> Result<CpuFrame, PixelBufferError> {
        if self.format != format
            && !matches!(
                (self.format, format),
                (PixelFormat::Bgra8, PixelFormat::Rgba8) | (PixelFormat::Rgba8, PixelFormat::Bgra8)
            )
        {
            return Err(crate::CpuFormatError::Unsupported.into());
        }
        Ok(self.to_cpu()?.into_format(format)?)
    }
    /// # Safety
    /// The producer must have completed its writes. Neither the caller nor any
    /// native consumer may modify the buffer until every clone of this lease is dropped.
    #[allow(non_upper_case_globals)]
    pub unsafe fn from_retained(
        buffer: CFRetained<CVPixelBuffer>,
        mut color: ColorDescription,
    ) -> Result<Self, PixelBufferError> {
        let format = match CVPixelBufferGetPixelFormatType(&buffer) {
            kCVPixelFormatType_32BGRA => PixelFormat::Bgra8,
            kCVPixelFormatType_32RGBA => PixelFormat::Rgba8,
            kCVPixelFormatType_64RGBAHalf => PixelFormat::Rgba16Float,
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            | kCVPixelFormatType_420YpCbCr8BiPlanarFullRange => PixelFormat::Nv12,
            kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
            | kCVPixelFormatType_420YpCbCr10BiPlanarFullRange => PixelFormat::P010,
            other => return Err(PixelBufferError::Unsupported(other)),
        };
        color = read_color(&buffer, color)?;
        let size = PixelSize::new(
            u32::try_from(CVPixelBufferGetWidth(&buffer)).map_err(|_| PixelBufferError::Layout)?,
            u32::try_from(CVPixelBufferGetHeight(&buffer)).map_err(|_| PixelBufferError::Layout)?,
        )
        .map_err(|_| PixelBufferError::Layout)?;
        Ok(Self {
            storage: Arc::new(Storage {
                buffer,
                mapping: Mutex::new(()),
            }),
            size,
            format,
            color,
        })
    }

    /// # Safety
    /// Do not mutate the buffer. Retain this lease until asynchronous native reads complete.
    pub unsafe fn native_buffer(&self) -> &CVPixelBuffer {
        &self.storage.buffer
    }

    /// Copy packed planes into owned CPU memory, omitting native row padding.
    pub fn to_cpu(&self) -> Result<CpuFrame, PixelBufferError> {
        let _guard = self
            .storage
            .mapping
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let buffer = &self.storage.buffer;
        let result =
            unsafe { CVPixelBufferLockBaseAddress(buffer, CVPixelBufferLockFlags::ReadOnly) };
        if result != 0 {
            return Err(PixelBufferError::Lock(result));
        }
        struct Unlock<'a>(&'a CVPixelBuffer);
        impl Drop for Unlock<'_> {
            fn drop(&mut self) {
                unsafe {
                    CVPixelBufferUnlockBaseAddress(self.0, CVPixelBufferLockFlags::ReadOnly);
                }
            }
        }
        let _unlock = Unlock(buffer);
        let planar = CVPixelBufferIsPlanar(buffer);
        let count = if planar {
            CVPixelBufferGetPlaneCount(buffer)
        } else {
            1
        };
        let expected = if matches!(self.format, PixelFormat::Nv12 | PixelFormat::P010) {
            2
        } else {
            1
        };
        if count != expected {
            return Err(PixelBufferError::Layout);
        }
        let mut bytes = Vec::new();
        let mut planes = Vec::with_capacity(count);
        for index in 0..count {
            let (width, height, stride, ptr) = if planar {
                (
                    CVPixelBufferGetWidthOfPlane(buffer, index),
                    CVPixelBufferGetHeightOfPlane(buffer, index),
                    CVPixelBufferGetBytesPerRowOfPlane(buffer, index),
                    CVPixelBufferGetBaseAddressOfPlane(buffer, index),
                )
            } else {
                (
                    self.size.width as usize,
                    self.size.height as usize,
                    CVPixelBufferGetBytesPerRow(buffer),
                    CVPixelBufferGetBaseAddress(buffer),
                )
            };
            let unit = match self.format {
                PixelFormat::Rgba8 | PixelFormat::Bgra8 => 4,
                PixelFormat::Rgba16Float => 8,
                PixelFormat::Nv12 => {
                    if index == 0 {
                        1
                    } else {
                        2
                    }
                }
                PixelFormat::P010 => {
                    if index == 0 {
                        2
                    } else {
                        4
                    }
                }
            };
            let row_bytes = width.checked_mul(unit).ok_or(PixelBufferError::Layout)?;
            let plane = PlaneLayout {
                offset: bytes.len(),
                width,
                height,
                stride,
                row_bytes,
            };
            let readable = plane.required_bytes().ok_or(PixelBufferError::Layout)?;
            if ptr.is_null() {
                return Err(PixelBufferError::Layout);
            }
            let source = unsafe { std::slice::from_raw_parts(ptr.cast::<u8>(), readable) };
            let length = row_bytes
                .checked_mul(height)
                .ok_or(PixelBufferError::Layout)?;
            bytes
                .try_reserve(length)
                .map_err(|_| PixelBufferError::Layout)?;
            for row in 0..height {
                bytes.extend_from_slice(&source[row * stride..row * stride + row_bytes]);
            }
            planes.push(PlaneLayout {
                stride: row_bytes,
                ..plane
            });
        }
        Ok(CpuFrame {
            size: self.size,
            format: self.format,
            color: self.color,
            planes,
            bytes: bytes.into(),
        })
    }
}

fn read_color(
    buffer: &CVPixelBuffer,
    mut fallback: ColorDescription,
) -> Result<ColorDescription, PixelBufferError> {
    unsafe {
        let string = |key: &CFString| -> Result<Option<CFRetained<CFString>>, PixelBufferError> {
            buffer
                .attachment(key, std::ptr::null_mut())
                .map(|v| {
                    v.downcast::<CFString>()
                        .map_err(|_| PixelBufferError::Color(key.to_string()))
                })
                .transpose()
        };
        if let Some(value) = string(kCVImageBufferColorPrimariesKey)? {
            fallback.primaries = if &*value == kCVImageBufferColorPrimaries_ITU_R_709_2 {
                ColorPrimaries::Bt709
            } else if &*value == kCVImageBufferColorPrimaries_P3_D65 {
                ColorPrimaries::DisplayP3
            } else if &*value == kCVImageBufferColorPrimaries_ITU_R_2020 {
                ColorPrimaries::Bt2020
            } else {
                return Err(PixelBufferError::Color(value.to_string()));
            };
        }
        if let Some(value) = string(kCVImageBufferTransferFunctionKey)? {
            fallback.transfer = if &*value == kCVImageBufferTransferFunction_sRGB {
                TransferFunction::Srgb
            } else if &*value == kCVImageBufferTransferFunction_Linear {
                TransferFunction::Linear
            } else if &*value == kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ {
                TransferFunction::Pq
            } else if &*value == kCVImageBufferTransferFunction_ITU_R_2100_HLG {
                TransferFunction::Hlg
            } else if &*value == kCVImageBufferTransferFunction_ITU_R_709_2 {
                TransferFunction::Bt709
            } else {
                return Err(PixelBufferError::Color(value.to_string()));
            };
        }
        let format = CVPixelBufferGetPixelFormatType(buffer);
        if format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
            || format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
        {
            fallback.range = ColorRange::Full;
        }
        if format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            || format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
        {
            fallback.range = ColorRange::Video;
        }
        Ok(fallback)
    }
}

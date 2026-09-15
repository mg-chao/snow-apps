use crate::geometry::PixelSize;
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum DynamicRange {
    #[default]
    Sdr,
    Hdr,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum CursorMode {
    Hidden,
    #[default]
    Embedded,
    Separate,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[repr(u32)]
pub enum PixelFormat {
    Rgba8,
    Bgra8,
    Rgba16Float,
    Nv12,
    P010,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[repr(u32)]
pub enum ColorPrimaries {
    Bt709,
    DisplayP3,
    Bt2020,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[repr(u32)]
pub enum TransferFunction {
    Srgb,
    Linear,
    Pq,
    Hlg,
    Bt709,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[repr(u32)]
pub enum ColorRange {
    Full,
    Video,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[repr(u32)]
pub enum AlphaMode {
    Opaque,
    Straight,
    Premultiplied,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ColorDescription {
    pub primaries: ColorPrimaries,
    pub transfer: TransferFunction,
    pub range: ColorRange,
    pub alpha: AlphaMode,
}
impl ColorDescription {
    pub const SRGB: Self = Self {
        primaries: ColorPrimaries::Bt709,
        transfer: TransferFunction::Srgb,
        range: ColorRange::Full,
        alpha: AlphaMode::Premultiplied,
    };
    pub const HDR10: Self = Self {
        primaries: ColorPrimaries::Bt2020,
        transfer: TransferFunction::Pq,
        range: ColorRange::Video,
        alpha: AlphaMode::Opaque,
    };
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PlaneLayout {
    /// Byte offset into the owning CPU frame allocation.
    pub offset: usize,
    pub width: usize,
    pub height: usize,
    pub stride: usize,
    pub row_bytes: usize,
}
impl PlaneLayout {
    pub fn required_bytes(self) -> Option<usize> {
        if self.width == 0
            || self.height == 0
            || self.row_bytes == 0
            || self.stride < self.row_bytes
        {
            return None;
        }
        self.stride
            .checked_mul(self.height - 1)?
            .checked_add(self.row_bytes)
            .filter(|&len| len <= isize::MAX as usize)
    }
}
#[derive(Clone, Debug)]
pub struct CpuFrame {
    pub size: PixelSize,
    pub format: PixelFormat,
    pub color: ColorDescription,
    pub planes: Vec<PlaneLayout>,
    pub bytes: std::sync::Arc<[u8]>,
}
impl CpuFrame {
    /// Return only readable plane bytes, excluding trailing row padding.
    pub fn plane_bytes(&self, index: usize) -> Option<&[u8]> {
        let plane = self.planes.get(index)?;
        let end = plane.offset.checked_add(plane.required_bytes()?)?;
        self.bytes.get(plane.offset..end)
    }
    /// Convert packed SDR channel order, preserving alpha and padding. Other
    /// color/depth conversions require an explicit rendering policy.
    pub fn into_format(mut self, format: PixelFormat) -> Result<Self, CpuFormatError> {
        if self.format == format {
            return Ok(self);
        }
        if !matches!(
            (self.format, format),
            (PixelFormat::Bgra8, PixelFormat::Rgba8) | (PixelFormat::Rgba8, PixelFormat::Bgra8)
        ) {
            return Err(CpuFormatError::Unsupported);
        }
        let [plane] = self.planes.as_slice() else {
            return Err(CpuFormatError::Layout);
        };
        let plane = *plane;
        if plane.width != self.size.width as usize
            || plane.height != self.size.height as usize
            || plane.width.checked_mul(4) != Some(plane.row_bytes)
            || self.plane_bytes(0).is_none()
        {
            return Err(CpuFormatError::Layout);
        }
        let bytes = std::sync::Arc::make_mut(&mut self.bytes);
        for row in 0..plane.height {
            let offset = plane.offset + row * plane.stride;
            crate::convert::swap_red_blue(&mut bytes[offset..offset + plane.row_bytes]);
        }
        self.format = format;
        Ok(self)
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
pub enum CpuFormatError {
    #[error("unsupported CPU pixel conversion; explicit color rendering is required")]
    Unsupported,
    #[error("invalid CPU frame layout")]
    Layout,
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cpu_channel_conversion_preserves_padding_alpha_and_other_leases() {
        let source = CpuFrame {
            size: PixelSize::new(1, 2).unwrap(),
            format: PixelFormat::Bgra8,
            color: ColorDescription::SRGB,
            planes: vec![PlaneLayout {
                offset: 1,
                width: 1,
                height: 2,
                stride: 5,
                row_bytes: 4,
            }],
            bytes: std::sync::Arc::from([99, 10, 20, 30, 40, 98, 50, 60, 70, 80]),
        };
        let converted = source.clone().into_format(PixelFormat::Rgba8).unwrap();
        assert_eq!(&*converted.bytes, &[99, 30, 20, 10, 40, 98, 70, 60, 50, 80]);
        assert_eq!(source.bytes[1], 10);
        assert_eq!(converted.color, source.color);
        assert_eq!(
            converted.into_format(PixelFormat::Bgra8).unwrap().bytes,
            source.bytes
        );
        assert!(matches!(
            source.clone().into_format(PixelFormat::Rgba16Float),
            Err(CpuFormatError::Unsupported)
        ));
        let mut truncated = source;
        truncated.planes[0].offset = 2;
        assert!(matches!(
            truncated.into_format(PixelFormat::Rgba8),
            Err(CpuFormatError::Layout)
        ));
    }
    #[test]
    fn planar_offsets_and_truncated_allocations_are_checked() {
        let mut frame = CpuFrame {
            size: PixelSize::new(2, 2).unwrap(),
            format: PixelFormat::Nv12,
            color: ColorDescription::SRGB,
            planes: vec![
                PlaneLayout {
                    offset: 0,
                    width: 2,
                    height: 2,
                    stride: 4,
                    row_bytes: 2,
                },
                PlaneLayout {
                    offset: 8,
                    width: 1,
                    height: 1,
                    stride: 2,
                    row_bytes: 2,
                },
            ],
            bytes: std::sync::Arc::from([1, 2, 0, 0, 3, 4, 0, 0, 128, 128]),
        };
        assert_eq!(frame.plane_bytes(0).unwrap(), &[1, 2, 0, 0, 3, 4]);
        assert_eq!(frame.plane_bytes(1).unwrap(), &[128, 128]);
        frame.planes[1].offset = 9;
        assert!(frame.plane_bytes(1).is_none());
        frame.planes[1].offset = usize::MAX;
        assert!(frame.plane_bytes(1).is_none());
    }
    #[test]
    fn padded_plane_excludes_unreadable_trailing_padding() {
        assert_eq!(
            PlaneLayout {
                offset: 0,
                width: 3,
                height: 2,
                stride: 64,
                row_bytes: 12
            }
            .required_bytes(),
            Some(76)
        );
        assert_eq!(
            PlaneLayout {
                offset: 0,
                width: 3,
                height: 2,
                stride: 8,
                row_bytes: 12
            }
            .required_bytes(),
            None
        );
    }
}

use std::{fmt, path::Path};

use serde::{Deserialize, Serialize};
use snow_memory::RasterBuffer;

use crate::StitchError;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PixelFormat {
    Gray8,
    Rgb8,
    Rgba8,
}

impl PixelFormat {
    pub const fn channels(self) -> u32 {
        match self {
            Self::Gray8 => 1,
            Self::Rgb8 => 3,
            Self::Rgba8 => 4,
        }
    }
}

impl fmt::Display for PixelFormat {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Gray8 => f.write_str("gray8"),
            Self::Rgb8 => f.write_str("rgb8"),
            Self::Rgba8 => f.write_str("rgba8"),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Geometry {
    pub width: u32,
    pub height: u32,
    pub pixel_format: PixelFormat,
}

impl fmt::Display for Geometry {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}x{} {}", self.width, self.height, self.pixel_format)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    width: u32,
    height: u32,
    pixel_format: PixelFormat,
    pixels: RasterBuffer,
}

impl Frame {
    pub fn new(
        width: u32,
        height: u32,
        pixel_format: PixelFormat,
        pixels: Vec<u8>,
    ) -> Result<Self, StitchError> {
        Self::validate_buffer(width, height, pixel_format, pixels.len())?;
        Self::from_buffer(width, height, pixel_format, pixels.into())
    }

    /// Preserve raster ownership without converting mapped pixels to a Vec.
    pub fn from_buffer(
        width: u32,
        height: u32,
        pixel_format: PixelFormat,
        pixels: RasterBuffer,
    ) -> Result<Self, StitchError> {
        Self::validate_buffer(width, height, pixel_format, pixels.len())?;
        Ok(Self {
            width,
            height,
            pixel_format,
            pixels,
        })
    }

    fn validate_buffer(
        width: u32,
        height: u32,
        pixel_format: PixelFormat,
        length: usize,
    ) -> Result<(), StitchError> {
        let expected = Self::buffer_len(width, height, pixel_format)?;
        if length != expected {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "{}x{} {} requires {expected} bytes, got {}",
                    width, height, pixel_format, length
                ),
            });
        }
        Ok(())
    }

    pub fn from_strided(
        width: u32,
        height: u32,
        pixel_format: PixelFormat,
        row_stride: usize,
        storage: &[u8],
    ) -> Result<Self, StitchError> {
        let packed_row = usize::try_from(width)
            .ok()
            .and_then(|value| value.checked_mul(pixel_format.channels() as usize))
            .ok_or(StitchError::Arithmetic {
                operation: "calculating packed strided row length",
            })?;
        let storage_len =
            row_stride
                .checked_mul(height as usize)
                .ok_or(StitchError::Arithmetic {
                    operation: "calculating strided storage length",
                })?;
        if row_stride < packed_row || storage.len() < storage_len {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "strided {}x{} {} needs stride >= {packed_row} and {storage_len} storage bytes",
                    width, height, pixel_format
                ),
            });
        }
        let mut pixels = RasterBuffer::zeroed(Self::buffer_len(width, height, pixel_format)?);
        if packed_row == 0 {
            return Self::from_buffer(width, height, pixel_format, pixels);
        }
        for (source, target) in storage[..storage_len]
            .chunks_exact(row_stride)
            .zip(pixels.as_mut_slice().chunks_exact_mut(packed_row))
        {
            target.copy_from_slice(&source[..packed_row]);
        }
        Self::from_buffer(width, height, pixel_format, pixels)
    }

    pub fn decode(path: impl AsRef<Path>) -> Result<Self, StitchError> {
        let path = path.as_ref();
        let decoded = image::open(path).map_err(|source| StitchError::Decode {
            path: path.to_path_buf(),
            source,
        })?;
        let rgb = decoded.into_rgb8();
        Self::new(rgb.width(), rgb.height(), PixelFormat::Rgb8, rgb.into_raw())
    }

    pub fn encode(&self, path: impl AsRef<Path>) -> Result<(), StitchError> {
        let path = path.as_ref();
        let color = match self.pixel_format {
            PixelFormat::Gray8 => image::ColorType::L8,
            PixelFormat::Rgb8 => image::ColorType::Rgb8,
            PixelFormat::Rgba8 => image::ColorType::Rgba8,
        };
        image::save_buffer(path, &self.pixels, self.width, self.height, color).map_err(|source| {
            StitchError::Encode {
                path: path.to_path_buf(),
                source,
            }
        })
    }

    pub const fn width(&self) -> u32 {
        self.width
    }

    pub const fn height(&self) -> u32 {
        self.height
    }

    pub const fn pixel_format(&self) -> PixelFormat {
        self.pixel_format
    }

    pub const fn geometry(&self) -> Geometry {
        Geometry {
            width: self.width,
            height: self.height,
            pixel_format: self.pixel_format,
        }
    }

    pub fn pixels(&self) -> &[u8] {
        &self.pixels
    }

    pub fn into_pixels(self) -> Vec<u8> {
        self.pixels.into_vec()
    }

    pub fn into_buffer(self) -> RasterBuffer {
        self.pixels
    }

    pub(crate) fn pixels_mut(&mut self) -> &mut RasterBuffer {
        &mut self.pixels
    }

    #[cfg(test)]
    pub(crate) fn set_height(&mut self, height: u32) {
        self.height = height;
    }

    pub fn row(&self, y: u32) -> Result<&[u8], StitchError> {
        if y >= self.height {
            return Err(StitchError::CheckedCrop {
                x: 0,
                y,
                width: self.width,
                height: 1,
                image_width: self.width,
                image_height: self.height,
            });
        }
        let row_len = self.row_len()?;
        let start = usize::try_from(y)
            .ok()
            .and_then(|value| value.checked_mul(row_len))
            .ok_or(StitchError::Arithmetic {
                operation: "calculating a row offset",
            })?;
        Ok(&self.pixels[start..start + row_len])
    }

    pub fn crop(&self, x: u32, y: u32, width: u32, height: u32) -> Result<Self, StitchError> {
        let x_end = x.checked_add(width);
        let y_end = y.checked_add(height);
        if width == 0
            || height == 0
            || x_end.is_none_or(|end| end > self.width)
            || y_end.is_none_or(|end| end > self.height)
        {
            return Err(StitchError::CheckedCrop {
                x,
                y,
                width,
                height,
                image_width: self.width,
                image_height: self.height,
            });
        }

        let channels =
            usize::try_from(self.pixel_format.channels()).map_err(|_| StitchError::Arithmetic {
                operation: "converting channel count",
            })?;
        let source_row_len = self.row_len()?;
        let output_row_len = usize::try_from(width)
            .ok()
            .and_then(|value| value.checked_mul(channels))
            .ok_or(StitchError::Arithmetic {
                operation: "calculating cropped row length",
            })?;
        let x_bytes = usize::try_from(x)
            .ok()
            .and_then(|value| value.checked_mul(channels))
            .ok_or(StitchError::Arithmetic {
                operation: "calculating horizontal crop offset",
            })?;
        let capacity =
            output_row_len
                .checked_mul(height as usize)
                .ok_or(StitchError::Arithmetic {
                    operation: "allocating cropped image",
                })?;
        let mut output = RasterBuffer::zeroed(capacity);
        let source = self.pixels.as_slice();
        for (row, target) in output
            .as_mut_slice()
            .chunks_exact_mut(output_row_len)
            .enumerate()
        {
            let row_start = (y as usize + row) * source_row_len + x_bytes;
            target.copy_from_slice(&source[row_start..row_start + output_row_len]);
        }
        Self::from_buffer(width, height, self.pixel_format, output)
    }

    pub fn visible_pixels_equal(&self, other: &Self) -> bool {
        let _perf = crate::perf::Scope::new(crate::perf::Stage::DuplicateCheck);
        if self.geometry() != other.geometry() {
            return false;
        }
        match self.pixel_format {
            PixelFormat::Gray8 | PixelFormat::Rgb8 => self.pixels == other.pixels,
            PixelFormat::Rgba8 => self
                .pixels
                .chunks_exact(4)
                .zip(other.pixels.chunks_exact(4))
                .all(|(left, right)| left[..3] == right[..3]),
        }
    }

    pub(crate) fn visible_interior_pixels_equal(&self, other: &Self) -> bool {
        let _perf = crate::perf::Scope::new(crate::perf::Stage::DuplicateCheck);
        if self.geometry() != other.geometry() || self.width < 3 || self.height < 3 {
            return false;
        }
        let channels = self.pixel_format.channels() as usize;
        let row_len = self.width as usize * channels;
        let interior_start = channels;
        let interior_end = row_len - channels;
        match self.pixel_format {
            PixelFormat::Gray8 | PixelFormat::Rgb8 => (1..self.height as usize - 1).all(|y| {
                let row_start = y * row_len;
                self.pixels[row_start + interior_start..row_start + interior_end]
                    == other.pixels[row_start + interior_start..row_start + interior_end]
            }),
            PixelFormat::Rgba8 => (1..self.height as usize - 1).all(|y| {
                let row_start = y * row_len;
                self.pixels[row_start + interior_start..row_start + interior_end]
                    .chunks_exact(4)
                    .zip(
                        other.pixels[row_start + interior_start..row_start + interior_end]
                            .chunks_exact(4),
                    )
                    .all(|(left, right)| left[..3] == right[..3])
            }),
        }
    }

    pub(crate) fn from_row_ranges(
        width: u32,
        pixel_format: PixelFormat,
        ranges: &[(&Frame, std::ops::Range<u32>)],
    ) -> Result<Self, StitchError> {
        let height = ranges.iter().try_fold(0_u32, |total, (_, range)| {
            total.checked_add(range.end.checked_sub(range.start)?)
        });
        let height = height.ok_or(StitchError::Arithmetic {
            operation: "calculating composed height",
        })?;
        let capacity = Self::buffer_len(width, height, pixel_format)?;
        let mut pixels = RasterBuffer::with_capacity(capacity);
        for (frame, range) in ranges {
            if frame.width != width
                || frame.pixel_format != pixel_format
                || range.end > frame.height
            {
                return Err(StitchError::CheckedCrop {
                    x: 0,
                    y: range.start,
                    width,
                    height: range.end.saturating_sub(range.start),
                    image_width: frame.width,
                    image_height: frame.height,
                });
            }
            let row_len = frame.row_len()?;
            let start = range.start as usize * row_len;
            let end = range.end as usize * row_len;
            pixels.extend_from_slice(&frame.pixels[start..end]);
        }
        Self::from_buffer(width, height, pixel_format, pixels)
    }

    fn row_len(&self) -> Result<usize, StitchError> {
        usize::try_from(self.width)
            .ok()
            .and_then(|width| width.checked_mul(self.pixel_format.channels() as usize))
            .ok_or(StitchError::Arithmetic {
                operation: "calculating packed row length",
            })
    }

    fn buffer_len(
        width: u32,
        height: u32,
        pixel_format: PixelFormat,
    ) -> Result<usize, StitchError> {
        usize::try_from(width)
            .ok()
            .and_then(|value| value.checked_mul(height as usize))
            .and_then(|value| value.checked_mul(pixel_format.channels() as usize))
            .ok_or(StitchError::Arithmetic {
                operation: "calculating frame buffer length",
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn visible_equality_preserves_color() {
        use image::Pixel;

        assert_eq!(
            image::Rgb([255, 0, 0]).to_luma(),
            image::Rgb([0, 76, 0]).to_luma()
        );
        let left = Frame::new(1, 1, PixelFormat::Rgb8, vec![255, 0, 0]).unwrap();
        let right = Frame::new(1, 1, PixelFormat::Rgb8, vec![0, 76, 0]).unwrap();
        assert!(!left.visible_pixels_equal(&right));
    }

    #[test]
    fn rgba_equality_ignores_only_alpha() {
        let base = Frame::new(2, 1, PixelFormat::Rgba8, vec![1, 2, 3, 4, 5, 6, 7, 8]).unwrap();
        let alpha = Frame::new(2, 1, PixelFormat::Rgba8, vec![1, 2, 3, 9, 5, 6, 7, 10]).unwrap();
        let visible = Frame::new(2, 1, PixelFormat::Rgba8, vec![1, 2, 4, 4, 5, 6, 7, 8]).unwrap();
        assert!(base.visible_pixels_equal(&alpha));
        assert!(!base.visible_pixels_equal(&visible));
    }

    #[test]
    fn checked_crop_copies_packed_rows() {
        let frame = Frame::new(3, 2, PixelFormat::Gray8, vec![1, 2, 3, 4, 5, 6]).unwrap();
        assert_eq!(frame.crop(1, 0, 2, 2).unwrap().pixels(), &[2, 3, 5, 6]);
    }

    #[test]
    fn equality_is_independent_of_source_stride_and_padding() {
        let packed = Frame::new(2, 2, PixelFormat::Rgb8, (1..=12).collect()).unwrap();
        let strided = Frame::from_strided(
            2,
            2,
            PixelFormat::Rgb8,
            8,
            &[1, 2, 3, 4, 5, 6, 99, 98, 7, 8, 9, 10, 11, 12, 97, 96],
        )
        .unwrap();
        assert!(packed.visible_pixels_equal(&strided));
    }

    #[test]
    fn interior_equality_ignores_edges_and_rgba_alpha() {
        let left = Frame::new(4, 4, PixelFormat::Rgba8, [10, 20, 30, 40].repeat(16)).unwrap();
        let mut pixels = left.pixels().to_vec();
        pixels[0] = 99;
        let last = pixels.len() - 1;
        pixels[last] = 99;
        let right = Frame::new(4, 4, PixelFormat::Rgba8, pixels).unwrap();
        assert!(left.visible_interior_pixels_equal(&right));

        let mut pixels = right.pixels().to_vec();
        pixels[(4 + 1) * 4] = 99;
        let changed = Frame::new(4, 4, PixelFormat::Rgba8, pixels).unwrap();
        assert!(!left.visible_interior_pixels_equal(&changed));
    }
}

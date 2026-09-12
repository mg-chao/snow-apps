//! Owned pixel grids and the portable image boundary.

use crate::error::{Error, Result};
use image::{ImageBuffer, Rgb, RgbImage};

/// A compact row-major grid used by the detector's internal stages.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Grid<T> {
    pub(crate) w: i32,
    pub(crate) h: i32,
    pub(crate) data: Vec<T>,
}

pub(crate) type Mask = Grid<u8>;
pub(crate) type Image = Grid<[u8; 3]>;

impl<T: Clone> Grid<T> {
    pub(crate) fn new(w: i32, h: i32, value: T) -> Self {
        debug_assert!(w >= 0 && h >= 0);
        Self {
            w,
            h,
            data: vec![value; (w * h) as usize],
        }
    }

    pub(crate) fn at(&self, x: i32, y: i32) -> &T {
        &self.data[(y * self.w + x) as usize]
    }

    pub(crate) fn set(&mut self, x: i32, y: i32, value: T) {
        self.data[(y * self.w + x) as usize] = value;
    }

    pub(crate) fn crop(&self, r: crate::geometry::Rect) -> Self {
        let mut data = Vec::with_capacity(r.area() as usize);
        for y in r.y..r.y2() {
            data.extend_from_slice(
                &self.data[(y * self.w + r.x) as usize..(y * self.w + r.x2()) as usize],
            );
        }
        Self {
            w: r.w,
            h: r.h,
            data,
        }
    }
}

/// An owned 8-bit, three-channel BGR image.
///
/// The detector uses BGR internally so that the portable representation has
/// the same channel order as OpenCV's `CV_8UC3` images. `from_rgb8` and
/// `to_rgb8` are the convenient boundary for the `image` crate and file I/O.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BgrImage {
    pub(crate) image: Image,
}

impl BgrImage {
    /// Construct an image from tightly packed interleaved BGR bytes.
    pub fn from_bgr_bytes(width: u32, height: u32, bytes: Vec<u8>) -> Result<Self> {
        let width_i32 = i32::try_from(width)
            .map_err(|_| Error::InvalidImage("image width exceeds i32::MAX".into()))?;
        let height_i32 = i32::try_from(height)
            .map_err(|_| Error::InvalidImage("image height exceeds i32::MAX".into()))?;
        let expected = usize::try_from(width)
            .ok()
            .and_then(|w| usize::try_from(height).ok().and_then(|h| w.checked_mul(h)))
            .and_then(|pixels| pixels.checked_mul(3))
            .ok_or_else(|| Error::InvalidImage("image dimensions overflow".into()))?;
        if width == 0 || height == 0 {
            return Err(Error::InvalidImage(
                "expected a non-empty H x W x 3 uint8 BGR image".into(),
            ));
        }
        if bytes.len() != expected {
            return Err(Error::InvalidImage(format!(
                "expected {expected} BGR bytes, got {}",
                bytes.len()
            )));
        }
        let data = bytes
            .chunks_exact(3)
            .map(|pixel| [pixel[0], pixel[1], pixel[2]])
            .collect();
        Ok(Self {
            image: Image {
                w: width_i32,
                h: height_i32,
                data,
            },
        })
    }

    /// Construct an image from owned BGR pixels in row-major order.
    pub fn from_bgr_pixels(width: u32, height: u32, pixels: Vec<[u8; 3]>) -> Result<Self> {
        let expected = usize::try_from(width)
            .ok()
            .and_then(|w| usize::try_from(height).ok().and_then(|h| w.checked_mul(h)))
            .ok_or_else(|| Error::InvalidImage("image dimensions overflow".into()))?;
        if pixels.len() != expected {
            return Err(Error::InvalidImage(format!(
                "expected {expected} BGR pixels, got {}",
                pixels.len()
            )));
        }
        let bytes = pixels
            .iter()
            .flat_map(|pixel| pixel.iter().copied())
            .collect();
        Self::from_bgr_bytes(width, height, bytes)
    }

    /// Construct a BGR image from an RGB image owned by the `image` crate.
    pub fn from_rgb8(rgb: &RgbImage) -> Self {
        let data = rgb
            .pixels()
            .map(|pixel| [pixel[2], pixel[1], pixel[0]])
            .collect();
        Self {
            image: Image {
                w: rgb.width() as i32,
                h: rgb.height() as i32,
                data,
            },
        }
    }

    /// Build a solid-color BGR image, useful for callers and tests.
    pub fn solid(width: u32, height: u32, bgr: [u8; 3]) -> Result<Self> {
        let count = usize::try_from(width)
            .ok()
            .and_then(|w| usize::try_from(height).ok().and_then(|h| w.checked_mul(h)))
            .ok_or_else(|| Error::InvalidImage("image dimensions overflow".into()))?;
        Self::from_bgr_pixels(width, height, vec![bgr; count])
    }

    /// Image width in pixels.
    pub fn width(&self) -> u32 {
        self.image.w as u32
    }

    /// Image height in pixels.
    pub fn height(&self) -> u32 {
        self.image.h as u32
    }

    /// Borrow row-major BGR pixels.
    pub fn pixels(&self) -> &[[u8; 3]] {
        &self.image.data
    }

    /// Return tightly packed interleaved BGR bytes.
    pub fn to_bgr_bytes(&self) -> Vec<u8> {
        self.image
            .data
            .iter()
            .flat_map(|pixel| pixel.iter().copied())
            .collect()
    }

    /// Convert to an RGB image for encoding or display.
    pub fn to_rgb8(&self) -> RgbImage {
        ImageBuffer::from_fn(self.width(), self.height(), |x, y| {
            let pixel = self.image.at(x as i32, y as i32);
            Rgb([pixel[2], pixel[1], pixel[0]])
        })
    }

    pub(crate) fn from_grid(image: Image) -> Self {
        Self { image }
    }
}

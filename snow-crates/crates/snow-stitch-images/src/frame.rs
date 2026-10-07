use std::{fmt, ops::Deref, path::Path, sync::Arc};

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
    pixels: FramePixels,
}

#[derive(Clone)]
enum FramePixels {
    Owned(RasterBuffer),
    Shared(SharedFramePixels),
}

#[derive(Clone)]
struct SharedFramePixels {
    bytes: *const u8,
    length: usize,
    _owner: Arc<dyn AsRef<[u8]> + Send + Sync>,
}

impl SharedFramePixels {
    fn new(owner: Arc<dyn AsRef<[u8]> + Send + Sync>) -> Self {
        let bytes = owner.as_ref().as_ref();
        Self {
            bytes: bytes.as_ptr(),
            length: bytes.len(),
            _owner: owner,
        }
    }
}

// The slice borrows immutable storage from a Send + Sync owner. Every clone
// retains that owner, and the pointer is never exposed for mutation.
unsafe impl Send for SharedFramePixels {}
unsafe impl Sync for SharedFramePixels {}

impl AsRef<[u8]> for SharedFramePixels {
    fn as_ref(&self) -> &[u8] {
        // Retaining the Arc keeps the original AsRef borrow valid. Cache the
        // slice so per-pixel sampling never dispatches through the owner's vtable.
        unsafe { std::slice::from_raw_parts(self.bytes, self.length) }
    }
}

impl FramePixels {
    fn into_buffer(self) -> RasterBuffer {
        match self {
            Self::Owned(pixels) => pixels,
            Self::Shared(pixels) => RasterBuffer::from(pixels.as_ref()),
        }
    }

    fn make_owned(&mut self) -> &mut RasterBuffer {
        if let Self::Shared(pixels) = self {
            *self = Self::Owned(RasterBuffer::from(pixels.as_ref()));
        }
        match self {
            Self::Owned(pixels) => pixels,
            Self::Shared(_) => unreachable!("shared pixels were detached"),
        }
    }
}

impl Deref for FramePixels {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        match self {
            Self::Owned(pixels) => pixels,
            Self::Shared(pixels) => pixels.as_ref(),
        }
    }
}

impl fmt::Debug for FramePixels {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("FramePixels")
            .field("len", &self.len())
            .finish()
    }
}

impl PartialEq for FramePixels {
    fn eq(&self, other: &Self) -> bool {
        **self == **other
    }
}

impl Eq for FramePixels {}

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
            pixels: FramePixels::Owned(pixels),
        })
    }

    /// Retain immutable pixel storage without copying it. Mutation detaches into
    /// an owned raster, so references and input leases cannot modify one another.
    pub fn from_shared_buffer(
        width: u32,
        height: u32,
        pixel_format: PixelFormat,
        pixels: Arc<dyn AsRef<[u8]> + Send + Sync>,
    ) -> Result<Self, StitchError> {
        let pixels = SharedFramePixels::new(pixels);
        Self::validate_buffer(width, height, pixel_format, pixels.length)?;
        Ok(Self {
            width,
            height,
            pixel_format,
            pixels: FramePixels::Shared(pixels),
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
        self.encode_file(path)
            .map_err(|source| StitchError::Encode {
                path: path.to_path_buf(),
                source,
            })
    }

    fn encode_file(&self, path: &Path) -> image::ImageResult<()> {
        let format = image::ImageFormat::from_path(path)?;
        // full-image-io guarantees the concrete JPEG image-view API is available.
        #[cfg(feature = "full-image-io")]
        if format == image::ImageFormat::Jpeg {
            use std::{fs::File, io::BufWriter, io::Write};

            let mut output = BufWriter::new(File::create(path)?);
            match self.pixel_format {
                PixelFormat::Gray8 => self.encode_jpeg::<image::Luma<u8>>(&mut output)?,
                PixelFormat::Rgb8 => self.encode_jpeg::<image::Rgb<u8>>(&mut output)?,
                PixelFormat::Rgba8 => self.encode_jpeg::<image::Rgba<u8>>(&mut output)?,
            }
            return Ok(output.flush()?);
        }
        #[cfg(not(feature = "full-image-io"))]
        if format == image::ImageFormat::Jpeg
            && format.writing_enabled()
            && self.pixel_format == PixelFormat::Rgba8
        {
            // Cargo feature unification can enable JPEG without full-image-io. Its concrete API
            // cannot be named here; allocate only the RGB destination, preserving raster policy.
            let mut rgb = RasterBuffer::zeroed(self.pixels.len() / 4 * 3);
            for (source, target) in self.pixels.chunks_exact(4).zip(rgb.chunks_exact_mut(3)) {
                target.copy_from_slice(&source[..3]);
            }
            return image::save_buffer_with_format(
                path,
                &rgb,
                self.width,
                self.height,
                image::ColorType::Rgb8,
                format,
            );
        }
        let color = match self.pixel_format {
            PixelFormat::Gray8 => image::ColorType::L8,
            PixelFormat::Rgb8 => image::ColorType::Rgb8,
            PixelFormat::Rgba8 => image::ColorType::Rgba8,
        };
        image::save_buffer_with_format(path, &self.pixels, self.width, self.height, color, format)
    }

    #[cfg(feature = "full-image-io")]
    fn encode_jpeg<P>(&self, output: &mut impl std::io::Write) -> image::ImageResult<()>
    where
        P: image::PixelWithColorType<Subpixel = u8>,
    {
        let borrowed = image::ImageBuffer::<P, _>::from_raw(self.width, self.height, self.pixels())
            .expect("frame storage was validated against its geometry");
        // The JPEG image-view API drops alpha one block at a time, without copying the raster.
        image::codecs::jpeg::JpegEncoder::new(output).encode_image(&borrowed)
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
        self.into_buffer().into_vec()
    }

    pub fn into_buffer(self) -> RasterBuffer {
        self.pixels.into_buffer()
    }

    pub(crate) fn pixels_mut(&mut self) -> &mut RasterBuffer {
        self.pixels.make_owned()
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
        let source = self.pixels();
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
    fn shared_input_resolves_its_owner_once_for_all_pixel_reads() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        struct Pixels {
            bytes: Vec<u8>,
            reads: Arc<AtomicUsize>,
        }
        impl AsRef<[u8]> for Pixels {
            fn as_ref(&self) -> &[u8] {
                self.reads.fetch_add(1, Ordering::Relaxed);
                &self.bytes
            }
        }
        let reads = Arc::new(AtomicUsize::new(0));
        let owner = Arc::new(Pixels {
            bytes: vec![0x5a; 16],
            reads: reads.clone(),
        });
        let frame = Frame::from_shared_buffer(2, 2, PixelFormat::Rgba8, owner).unwrap();
        let mut clone = frame.clone();
        assert_eq!(frame, clone);
        assert_eq!(frame.row(1).unwrap(), &[0x5a; 8]);
        assert_eq!(frame.crop(0, 1, 2, 1).unwrap().pixels(), &[0x5a; 8]);
        clone.pixels_mut()[0] = 0x7f;
        assert_eq!(frame.into_pixels(), vec![0x5a; 16]);
        assert_eq!(reads.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn shared_input_retains_ownership_and_detaches_only_for_mutation() {
        let input = Arc::new(vec![0x5a; 512 * 512 * 4]);
        let lease = Arc::downgrade(&input);
        let pointer = input.as_ptr();
        let mut frame = Frame::from_shared_buffer(512, 512, PixelFormat::Rgba8, input).unwrap();
        let cloned = frame.clone();
        assert_eq!(frame.pixels().as_ptr(), pointer);
        assert_eq!(cloned.pixels().as_ptr(), pointer);
        frame.pixels_mut()[0] = 0x7f;
        assert_ne!(frame.pixels().as_ptr(), pointer);
        assert_eq!(cloned.pixels()[0], 0x5a);
        assert!(lease.upgrade().is_some());
        drop(cloned);
        assert!(lease.upgrade().is_none());
        let owned = frame.into_buffer();
        assert_eq!(owned[0], 0x7f);
        assert!(owned.is_page_backed());
    }

    #[test]
    fn shared_input_validates_geometry_and_materializes_owned_pixels() {
        let pixels = Arc::new(vec![0x5a; 16]);
        assert!(Frame::from_shared_buffer(3, 2, PixelFormat::Rgba8, pixels.clone()).is_err());
        let frame = Frame::from_shared_buffer(2, 2, PixelFormat::Rgba8, pixels.clone()).unwrap();
        assert_eq!(frame.row(1).unwrap(), &[0x5a; 8]);
        assert_eq!(frame.crop(0, 1, 2, 1).unwrap().pixels(), &[0x5a; 8]);
        assert_eq!(frame.into_pixels(), *pixels);
    }

    fn sample_frame(pixel_format: PixelFormat) -> Frame {
        let pixels = match pixel_format {
            PixelFormat::Gray8 => vec![0, 45, 123, 255],
            PixelFormat::Rgb8 => vec![255, 0, 0, 0, 255, 0, 0, 0, 255, 45, 123, 200],
            PixelFormat::Rgba8 => {
                vec![
                    255, 0, 0, 0, 0, 255, 0, 64, 0, 0, 255, 128, 45, 123, 200, 255,
                ]
            }
        };
        Frame::new(2, 2, pixel_format, pixels).unwrap()
    }

    #[test]
    fn encode_png_preserves_all_frame_formats() {
        let directory = tempfile::tempdir().unwrap();
        for pixel_format in [PixelFormat::Gray8, PixelFormat::Rgb8, PixelFormat::Rgba8] {
            let frame = sample_frame(pixel_format);
            let path = directory.path().join(format!("{pixel_format}.png"));
            frame.encode(&path).unwrap();
            let decoded = image::open(&path).unwrap();
            assert_eq!((decoded.width(), decoded.height()), (2, 2));
            assert_eq!(decoded.as_bytes(), frame.pixels());
        }
    }

    #[test]
    fn encode_png_preserves_page_backed_rgba_pixels() {
        let directory = tempfile::tempdir().unwrap();
        let mut pixels = RasterBuffer::zeroed(snow_memory::MIN_PAGE_BUFFER_BYTES);
        for pixel in pixels.chunks_exact_mut(4) {
            pixel.copy_from_slice(&[45, 123, 200, 64]);
        }
        let pointer = pixels.as_ptr();
        let page_backed = pixels.is_page_backed();
        let frame = Frame::from_buffer(512, 512, PixelFormat::Rgba8, pixels).unwrap();
        let path = directory.path().join("pages.png");
        frame.encode(&path).unwrap();
        assert_eq!(image::open(path).unwrap().as_bytes(), frame.pixels());
        let pixels = frame.into_buffer();
        assert_eq!(pixels.as_ptr(), pointer);
        assert_eq!(pixels.is_page_backed(), page_backed);
    }

    #[test]
    fn encode_jpeg_matches_dynamic_image_color_conversion() {
        if !image::ImageFormat::Jpeg.writing_enabled() {
            return;
        }
        let directory = tempfile::tempdir().unwrap();
        for pixel_format in [PixelFormat::Gray8, PixelFormat::Rgb8, PixelFormat::Rgba8] {
            let frame = sample_frame(pixel_format);
            let reference = match pixel_format {
                PixelFormat::Gray8 => image::DynamicImage::ImageLuma8(
                    image::GrayImage::from_raw(2, 2, frame.pixels().to_vec()).unwrap(),
                ),
                PixelFormat::Rgb8 => image::DynamicImage::ImageRgb8(
                    image::RgbImage::from_raw(2, 2, frame.pixels().to_vec()).unwrap(),
                ),
                PixelFormat::Rgba8 => image::DynamicImage::ImageRgba8(
                    image::RgbaImage::from_raw(2, 2, frame.pixels().to_vec()).unwrap(),
                ),
            };
            let expected_path = directory
                .path()
                .join(format!("expected-{pixel_format}.jpg"));
            reference.save(&expected_path).unwrap();
            let path = directory.path().join(format!("{pixel_format}.jpg"));
            frame.encode(&path).unwrap();
            assert_eq!(
                std::fs::read(path).unwrap(),
                std::fs::read(expected_path).unwrap()
            );
        }
    }

    #[test]
    fn encode_jpeg_discards_alpha_without_changing_page_backed_rgb() {
        if !image::ImageFormat::Jpeg.writing_enabled() {
            return;
        }
        let directory = tempfile::tempdir().unwrap();
        let mut pixels = RasterBuffer::zeroed(snow_memory::MIN_PAGE_BUFFER_BYTES);
        for pixel in pixels.chunks_exact_mut(4) {
            pixel.copy_from_slice(&[45, 123, 200, 0]);
        }
        let pointer = pixels.as_ptr();
        let page_backed = pixels.is_page_backed();
        let frame = Frame::from_buffer(512, 512, PixelFormat::Rgba8, pixels).unwrap();
        let path = directory.path().join("transparent.jpg");
        frame.encode(&path).unwrap();
        let opaque = Frame::new(
            512,
            512,
            PixelFormat::Rgb8,
            [45, 123, 200].repeat(512 * 512),
        )
        .unwrap();
        let opaque_path = directory.path().join("opaque.jpg");
        opaque.encode(&opaque_path).unwrap();
        assert_eq!(
            std::fs::read(path).unwrap(),
            std::fs::read(opaque_path).unwrap()
        );
        let pixels = frame.into_buffer();
        assert_eq!(pixels.as_ptr(), pointer);
        assert_eq!(pixels.is_page_backed(), page_backed);
        assert!(
            pixels
                .chunks_exact(4)
                .all(|pixel| pixel == [45, 123, 200, 0])
        );
    }

    #[test]
    #[cfg(feature = "full-image-io")]
    fn encode_webp_preserves_all_frame_colors() {
        let directory = tempfile::tempdir().unwrap();
        for pixel_format in [PixelFormat::Gray8, PixelFormat::Rgb8, PixelFormat::Rgba8] {
            let frame = sample_frame(pixel_format);
            let path = directory.path().join(format!("{pixel_format}.webp"));
            frame.encode(&path).unwrap();
            let decoded = image::open(path).unwrap();
            assert_eq!((decoded.width(), decoded.height()), (2, 2));
            match pixel_format {
                PixelFormat::Gray8 => {
                    let expected: Vec<u8> =
                        frame.pixels().iter().flat_map(|&v| [v, v, v]).collect();
                    assert_eq!(decoded.to_rgb8().as_raw(), &expected);
                }
                PixelFormat::Rgb8 => assert_eq!(decoded.to_rgb8().as_raw(), frame.pixels()),
                PixelFormat::Rgba8 => assert_eq!(decoded.to_rgba8().as_raw(), frame.pixels()),
            }
        }
    }

    #[test]
    fn encode_errors_keep_requested_path_and_cause() {
        let directory = tempfile::tempdir().unwrap();
        let frame = sample_frame(PixelFormat::Rgba8);
        let invalid_extension = directory.path().join("output.unsupported");
        assert!(matches!(
            frame.encode(&invalid_extension),
            Err(StitchError::Encode { path, source: image::ImageError::Unsupported(_) })
                if path == invalid_extension
        ));
        let missing_directory = directory.path().join("missing").join("output.png");
        assert!(matches!(
            frame.encode(&missing_directory),
            Err(StitchError::Encode { path, source: image::ImageError::IoError(_) })
                if path == missing_directory
        ));
    }

    #[test]
    fn encode_jpeg_errors_keep_requested_path_and_cause() {
        if !image::ImageFormat::Jpeg.writing_enabled() {
            return;
        }
        let directory = tempfile::tempdir().unwrap();
        let frame = sample_frame(PixelFormat::Rgba8);
        let missing_directory = directory.path().join("missing").join("output.JPG");
        assert!(matches!(
            frame.encode(&missing_directory),
            Err(StitchError::Encode { path, source: image::ImageError::IoError(_) })
                if path == missing_directory
        ));
        for (width, height) in [(0, 0), (65_536, 1)] {
            let frame = Frame::new(
                width,
                height,
                PixelFormat::Rgba8,
                vec![0; width as usize * height as usize * 4],
            )
            .unwrap();
            let path = directory.path().join(format!("{width}x{height}.jpg"));
            assert!(matches!(
                frame.encode(&path),
                Err(StitchError::Encode { path: actual, source: image::ImageError::Encoding(_) })
                    if actual == path
            ));
        }
    }

    #[test]
    #[cfg(feature = "full-image-io")]
    fn encode_unsupported_frame_colors_remain_errors() {
        let directory = tempfile::tempdir().unwrap();
        let frame = sample_frame(PixelFormat::Gray8);
        for extension in ["gif", "qoi", "hdr", "ff"] {
            let path = directory.path().join(format!("gray.{extension}"));
            assert!(matches!(
                frame.encode(&path),
                Err(StitchError::Encode { path: actual, source: image::ImageError::Unsupported(_) })
                    if actual == path
            ));
        }
    }

    #[test]
    fn encode_jpeg_respects_encoder_availability() {
        let directory = tempfile::tempdir().unwrap();
        for pixel_format in [PixelFormat::Gray8, PixelFormat::Rgb8, PixelFormat::Rgba8] {
            let frame = sample_frame(pixel_format);
            let path = directory.path().join(format!("{pixel_format}.jpg"));
            if image::ImageFormat::Jpeg.writing_enabled() {
                frame.encode(&path).unwrap();
                let decoded = image::open(path).unwrap();
                assert_eq!((decoded.width(), decoded.height()), (2, 2));
                continue;
            }
            assert!(matches!(
                frame.encode(&path),
                Err(StitchError::Encode { path: actual, source: image::ImageError::Unsupported(_) })
                    if actual == path
            ));
        }
    }

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

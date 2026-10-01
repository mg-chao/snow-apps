#[cfg(feature = "image-io")]
use std::{fs, io::Cursor, path::PathBuf};

#[cfg(feature = "image-io")]
use exif::{In, Reader as ExifReader, Tag};
#[cfg(feature = "image-io")]
use image::{DynamicImage, GrayImage, ImageBuffer, LumaA, RgbImage, RgbaImage};
#[cfg(feature = "turbojpeg-decode")]
use turbojpeg::{PixelFormat, decompress};

use crate::{
    config::RecImage,
    error::{RapidOcrError, Result},
};

#[derive(Debug, Clone)]
pub enum OcrInput {
    #[cfg(feature = "image-io")]
    Path(PathBuf),
    #[cfg(feature = "remote-input")]
    Url(String),
    #[cfg(feature = "image-io")]
    Bytes(Vec<u8>),
    Bgr {
        width: usize,
        height: usize,
        data: Vec<u8>,
    },
    Rgb {
        width: usize,
        height: usize,
        data: Vec<u8>,
    },
    BgrU8 {
        width: usize,
        height: usize,
        data: Vec<u8>,
    },
    RgbU8 {
        width: usize,
        height: usize,
        data: Vec<u8>,
    },
    GrayU8 {
        width: usize,
        height: usize,
        data: Vec<u8>,
    },
    GrayAlphaU8 {
        width: usize,
        height: usize,
        data: Vec<u8>,
    },
    RgbaU8 {
        width: usize,
        height: usize,
        data: Vec<u8>,
    },
    Image(RecImage),
}

#[derive(Debug, Default)]
pub struct LoadImage;

impl LoadImage {
    pub fn load(&self, input: OcrInput) -> Result<RecImage> {
        match input {
            #[cfg(feature = "image-io")]
            OcrInput::Path(path) => self.load_path(path),
            #[cfg(feature = "remote-input")]
            OcrInput::Url(url) => self.load_url(&url),
            #[cfg(feature = "image-io")]
            OcrInput::Bytes(bytes) => self.decode_bytes_with_exif(&bytes, false),
            OcrInput::Bgr {
                width,
                height,
                data,
            } => RecImage::from_bgr_u8(width, height, data),
            OcrInput::Rgb {
                width,
                height,
                data,
            } => RecImage::from_rgb_u8(width, height, data),
            OcrInput::BgrU8 {
                width,
                height,
                data,
            } => RecImage::from_bgr_u8(width, height, data),
            OcrInput::RgbU8 {
                width,
                height,
                data,
            } => RecImage::from_rgb_u8(width, height, data),
            OcrInput::GrayU8 {
                width,
                height,
                data,
            } => {
                ensure_len(width, height, 1, data.len())?;
                let mut bgr = vec![0_u8; width * height * 3];
                for (src, dst) in data.iter().zip(bgr.chunks_exact_mut(3)) {
                    dst[0] = *src;
                    dst[1] = *src;
                    dst[2] = *src;
                }
                RecImage::from_bgr_u8(width, height, bgr)
            }
            OcrInput::GrayAlphaU8 {
                width,
                height,
                data,
            } => {
                ensure_len(width, height, 2, data.len())?;
                RecImage::from_bgr_u8(width, height, gray_alpha_to_bgr(width, height, &data))
            }
            OcrInput::RgbaU8 {
                width,
                height,
                data,
            } => {
                ensure_len(width, height, 4, data.len())?;
                RecImage::from_bgr_u8(width, height, rgba_to_bgr(width, height, &data))
            }
            OcrInput::Image(image) => Ok(image),
        }
    }

    #[cfg(feature = "image-io")]
    fn load_path(&self, path: PathBuf) -> Result<RecImage> {
        if !path.exists() {
            return Err(RapidOcrError::FileNotFound(path));
        }
        let bytes = fs::read(path)?;
        self.decode_bytes_with_exif(&bytes, true)
    }

    #[cfg(feature = "remote-input")]
    fn load_url(&self, url: &str) -> Result<RecImage> {
        let response = reqwest::blocking::get(url)?;
        if !response.status().is_success() {
            return Err(RapidOcrError::Download(format!(
                "failed to fetch image from url {url}: HTTP {}",
                response.status()
            )));
        }
        let bytes = response.bytes()?;
        self.decode_bytes_with_exif(bytes.as_ref(), true)
    }

    #[cfg(feature = "image-io")]
    fn decode_bytes_with_exif(&self, bytes: &[u8], apply_exif_transpose: bool) -> Result<RecImage> {
        let orientation = if apply_exif_transpose {
            exif_orientation_from_bytes(bytes)
        } else {
            None
        };

        // Match Python PIL JPEG decode path as closely as possible for non-oriented images.
        #[cfg(feature = "turbojpeg-decode")]
        if orientation.unwrap_or(1) == 1
            && let Ok(img) = decode_bytes_with_turbojpeg(bytes)
        {
            return Ok(img);
        }

        let mut dyn_img = image::load_from_memory(bytes)
            .map_err(|e| RapidOcrError::InvalidImage(e.to_string()))?;
        if apply_exif_transpose {
            dyn_img = apply_exif_orientation(dyn_img, orientation);
        }
        dynamic_to_rec_image(dyn_img)
    }
}

#[cfg(feature = "image-io")]
fn exif_orientation_from_bytes(bytes: &[u8]) -> Option<u32> {
    let mut cursor = Cursor::new(bytes);
    let exif = ExifReader::new().read_from_container(&mut cursor).ok()?;
    let field = exif.get_field(Tag::Orientation, In::PRIMARY)?;
    field.value.get_uint(0)
}

#[cfg(all(test, feature = "image-io"))]
fn exif_transpose_from_bytes(img: DynamicImage, bytes: &[u8]) -> DynamicImage {
    apply_exif_orientation(img, exif_orientation_from_bytes(bytes))
}

#[cfg(feature = "image-io")]
fn apply_exif_orientation(img: DynamicImage, orientation: Option<u32>) -> DynamicImage {
    let orientation = orientation.unwrap_or(1);

    // Keep parity with PIL.ImageOps.exif_transpose orientation mapping.
    match orientation {
        2 => img.fliph(),
        3 => img.rotate180(),
        4 => img.flipv(),
        5 => img.fliph().rotate90(),
        6 => img.rotate90(),
        7 => img.fliph().rotate270(),
        8 => img.rotate270(),
        _ => img,
    }
}

#[cfg(feature = "turbojpeg-decode")]
fn decode_bytes_with_turbojpeg(bytes: &[u8]) -> Result<RecImage> {
    if !looks_like_jpeg(bytes) {
        return Err(RapidOcrError::InvalidImage("not a jpeg stream".to_string()));
    }

    let decoded = decompress(bytes, PixelFormat::BGR)
        .map_err(|e| RapidOcrError::InvalidImage(format!("turbojpeg decode failed: {e}")))?;

    let width = decoded.width;
    let height = decoded.height;
    if width == 0 || height == 0 {
        return Err(RapidOcrError::InvalidImage(
            "decoded image width/height cannot be zero".to_string(),
        ));
    }

    let row_bytes = width * 3;
    let bgr = if decoded.pitch == row_bytes {
        decoded.pixels
    } else {
        let mut compact = vec![0_u8; row_bytes * height];
        for y in 0..height {
            let src_start = y * decoded.pitch;
            let src_end = src_start + row_bytes;
            let dst_start = y * row_bytes;
            compact[dst_start..dst_start + row_bytes]
                .copy_from_slice(&decoded.pixels[src_start..src_end]);
        }
        compact
    };

    RecImage::from_bgr_u8(width, height, bgr)
}

#[cfg(feature = "turbojpeg-decode")]
fn looks_like_jpeg(bytes: &[u8]) -> bool {
    bytes.len() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF
}

#[cfg(feature = "image-io")]
fn dynamic_to_rec_image(img: DynamicImage) -> Result<RecImage> {
    match img {
        DynamicImage::ImageLuma8(gray) => RecImage::from_bgr_u8(
            gray.width() as usize,
            gray.height() as usize,
            gray_to_bgr(gray),
        ),
        DynamicImage::ImageLumaA8(gray_alpha) => {
            let width = gray_alpha.width() as usize;
            let height = gray_alpha.height() as usize;
            RecImage::from_bgr_u8(width, height, gray_alpha_to_bgr_img(gray_alpha))
        }
        DynamicImage::ImageRgba8(rgba) => {
            let width = rgba.width() as usize;
            let height = rgba.height() as usize;
            RecImage::from_bgr_u8(width, height, rgba_to_bgr_img(rgba))
        }
        other => {
            let rgb = other.to_rgb8();
            RecImage::from_bgr_u8(rgb.width() as usize, rgb.height() as usize, rgb_to_bgr(rgb))
        }
    }
}

#[cfg(feature = "image-io")]
fn gray_to_bgr(gray: GrayImage) -> Vec<u8> {
    let mut bgr = vec![0_u8; gray.width() as usize * gray.height() as usize * 3];
    for (src, dst) in gray.as_raw().iter().zip(bgr.chunks_exact_mut(3)) {
        dst[0] = *src;
        dst[1] = *src;
        dst[2] = *src;
    }
    bgr
}

#[cfg(feature = "image-io")]
fn rgb_to_bgr(rgb: RgbImage) -> Vec<u8> {
    let mut bgr = vec![0_u8; rgb.width() as usize * rgb.height() as usize * 3];
    for (src, dst) in rgb.as_raw().chunks_exact(3).zip(bgr.chunks_exact_mut(3)) {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
    }
    bgr
}

#[cfg(feature = "image-io")]
fn gray_alpha_to_bgr_img(gray_alpha: ImageBuffer<LumaA<u8>, Vec<u8>>) -> Vec<u8> {
    gray_alpha_to_bgr(
        gray_alpha.width() as usize,
        gray_alpha.height() as usize,
        gray_alpha.as_raw(),
    )
}

fn gray_alpha_to_bgr(width: usize, height: usize, data: &[u8]) -> Vec<u8> {
    let mut out = vec![0_u8; width * height * 3];
    for (src, dst) in data.chunks_exact(2).zip(out.chunks_exact_mut(3)) {
        let gray = src[0] as f32;
        let alpha = src[1] as f32 / 255.0;
        // Same behavior as Python's bitwise approach with white background.
        let value = (gray * alpha + 255.0 * (1.0 - alpha))
            .round()
            .clamp(0.0, 255.0) as u8;
        dst[0] = value;
        dst[1] = value;
        dst[2] = value;
    }
    out
}

#[cfg(feature = "image-io")]
fn rgba_to_bgr_img(rgba: RgbaImage) -> Vec<u8> {
    rgba_to_bgr(rgba.width() as usize, rgba.height() as usize, rgba.as_raw())
}

fn rgba_to_bgr(width: usize, height: usize, data: &[u8]) -> Vec<u8> {
    let bg = auto_background_for_rgba(data);
    let mut out = vec![0_u8; width * height * 3];

    for (src, dst) in data.chunks_exact(4).zip(out.chunks_exact_mut(3)) {
        let r = src[0] as f32;
        let g = src[1] as f32;
        let b = src[2] as f32;
        let a = src[3] as f32 / 255.0;

        let blended_r = (r * a + bg[0] as f32 * (1.0 - a)).round().clamp(0.0, 255.0) as u8;
        let blended_g = (g * a + bg[1] as f32 * (1.0 - a)).round().clamp(0.0, 255.0) as u8;
        let blended_b = (b * a + bg[2] as f32 * (1.0 - a)).round().clamp(0.0, 255.0) as u8;

        dst[0] = blended_b;
        dst[1] = blended_g;
        dst[2] = blended_r;
    }
    out
}

fn auto_background_for_rgba(data: &[u8]) -> [u8; 3] {
    let mut sum = 0.0_f64;
    let mut count = 0_u64;
    for px in data.chunks_exact(4) {
        let alpha = px[3];
        if alpha == 0 {
            continue;
        }
        let r = px[0] as f64;
        let g = px[1] as f64;
        let b = px[2] as f64;
        let luminance = 0.299_f64 * r + 0.587_f64 * g + 0.114_f64 * b;
        sum += luminance;
        count += 1;
    }

    if count == 0 {
        return [255, 255, 255];
    }
    let avg = sum / count as f64;
    if avg < 128.0 {
        [255, 255, 255]
    } else {
        [0, 0, 0]
    }
}

fn ensure_len(width: usize, height: usize, channels: usize, actual_len: usize) -> Result<()> {
    let expected = width
        .checked_mul(height)
        .and_then(|v| v.checked_mul(channels))
        .ok_or_else(|| RapidOcrError::InvalidImage("image dimensions overflow".to_string()))?;
    if expected != actual_len {
        return Err(RapidOcrError::InvalidImage(format!(
            "raw input size mismatch: expected {expected}, got {actual_len}"
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    #[cfg(feature = "image-io")]
    use image::{DynamicImage, RgbImage};

    #[cfg(feature = "image-io")]
    use super::exif_transpose_from_bytes;
    use super::{LoadImage, OcrInput};

    #[test]
    fn gray_alpha_input_is_supported() {
        let loader = LoadImage;
        let image = loader
            .load(OcrInput::GrayAlphaU8 {
                width: 2,
                height: 1,
                data: vec![0, 0, 255, 255],
            })
            .expect("gray alpha should load");
        assert_eq!(image.width(), 2);
        assert_eq!(image.height(), 1);
    }

    #[test]
    fn rgba_input_is_supported() {
        let loader = LoadImage;
        let image = loader
            .load(OcrInput::RgbaU8 {
                width: 1,
                height: 1,
                data: vec![0, 0, 0, 255],
            })
            .expect("rgba should load");
        assert_eq!(image.width(), 1);
        assert_eq!(image.height(), 1);
    }

    #[test]
    fn bgr_contract_input_is_supported() {
        let loader = LoadImage;
        let image = loader
            .load(OcrInput::Bgr {
                width: 1,
                height: 1,
                data: vec![0, 0, 0],
            })
            .expect("bgr contract input should load");
        assert_eq!(image.width(), 1);
        assert_eq!(image.height(), 1);
    }

    #[test]
    fn raw_worker_pixels_preserve_bgr_bytes_without_reallocating() {
        let pixels = vec![7, 19, 41, 53, 67, 83];
        let original_buffer = pixels.as_ptr();
        let image = LoadImage
            .load(OcrInput::BgrU8 {
                width: 2,
                height: 1,
                data: pixels,
            })
            .expect("raw worker input should load without encoded image features");
        assert_eq!(image.as_bytes(), &[7, 19, 41, 53, 67, 83]);
        assert_eq!(image.as_bytes().as_ptr(), original_buffer);
        assert_eq!(image.color_order(), crate::ColorOrder::Bgr);
    }

    #[test]
    fn raw_worker_pixels_reject_invalid_dimensions_and_length() {
        for (width, height, data) in [(0, 1, vec![]), (2, 1, vec![0; 5])] {
            assert!(
                LoadImage
                    .load(OcrInput::BgrU8 {
                        width,
                        height,
                        data
                    })
                    .is_err()
            );
        }
    }

    #[cfg(feature = "image-io")]
    #[test]
    fn encoded_png_bytes_and_file_preserve_pixel_conversion() {
        let rgb = RgbImage::from_raw(2, 1, vec![255, 0, 0, 0, 255, 0]).expect("valid rgb image");
        let mut bytes = std::io::Cursor::new(Vec::new());
        DynamicImage::ImageRgb8(rgb)
            .write_to(&mut bytes, image::ImageFormat::Png)
            .expect("PNG should encode");
        let bytes = bytes.into_inner();
        let file = tempfile::NamedTempFile::new().expect("temporary image");
        std::fs::write(file.path(), &bytes).expect("PNG fixture should write");
        for input in [OcrInput::Bytes(bytes), OcrInput::Path(file.path().into())] {
            let image = LoadImage.load(input).expect("encoded input should load");
            assert_eq!(image.as_bytes(), &[0, 0, 255, 0, 255, 0]);
            assert_eq!((image.width(), image.height()), (2, 1));
        }
    }

    #[cfg(feature = "image-io")]
    #[test]
    fn exif_transpose_is_noop_when_exif_is_missing() {
        let rgb = RgbImage::from_raw(2, 1, vec![255, 0, 0, 0, 255, 0]).expect("valid rgb image");
        let img = DynamicImage::ImageRgb8(rgb);

        // Minimal EXIF payload with orientation=6 would be parsed in integration tests.
        // Here we only verify helper is a no-op when EXIF payload is absent.
        let out = exif_transpose_from_bytes(img.clone(), &[]);
        assert_eq!(out.width(), img.width());
        assert_eq!(out.height(), img.height());
    }
}

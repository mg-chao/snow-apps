//! Bounded, content-adaptive GIF quantization for recording and edited exports.
//!
//! A full-clip palette requires retaining/replaying the recording at stop. Instead,
//! keep a palette while it represents the current screen, and rebuild on color
//! changes. Stable indices and ordered dithering let the GIF encoder retain its
//! transparent-difference and offset optimizations.

use ffmpeg_next as ffmpeg;

use crate::error::{RecordingExportError, Result};

/// GIF compares pixel indices for delta compression. Across a palette change,
/// equal indices do not mean equal colors. FFmpeg compares palettes against its
/// initial global table, so returning to that table also needs a complete frame.
#[derive(Default)]
pub(crate) struct GifDeltaState {
    previous_palette: Vec<u8>,
}

impl GifDeltaState {
    pub(crate) fn prepare(
        &mut self,
        encoder: &mut ffmpeg::encoder::video::Encoder,
        frame: &ffmpeg::frame::Video,
    ) -> Result<()> {
        if frame.format() != ffmpeg::format::Pixel::PAL8 {
            return Ok(());
        }
        // SAFETY: PAL8 frames have a 1024-byte palette in data[1]. The opened
        // GIF encoder owns the private AVOptions context throughout this call.
        let palette = unsafe { std::slice::from_raw_parts((*frame.as_ptr()).data[1], 1024) };
        let flags = if palette == self.previous_palette {
            3
        } else {
            0
        };
        let result = unsafe {
            ffmpeg::ffi::av_opt_set_int(
                (*encoder.as_mut_ptr()).priv_data,
                c"gifflags".as_ptr(),
                flags,
                0,
            )
        };
        if result < 0 {
            return Err(RecordingExportError::Encode(format!(
                "failed to configure GIF palette transition: {}",
                ffmpeg::Error::from(result)
            )));
        }
        self.previous_palette.clear();
        self.previous_palette.extend_from_slice(palette);
        Ok(())
    }
}

const BINS: usize = 32 * 32 * 32;
const COLORS: usize = 255; // Leave one entry for the encoder's transparent differences.
const PALETTE_RMS_LIMIT: u64 = 8;
const BIN_RMS_LIMIT: u32 = 32;
const BAYER_SCALE: usize = 3;

#[derive(Clone, Copy, Default)]
struct Bin {
    count: u64,
    sum: [u64; 3],
}

impl Bin {
    fn color(self) -> [u8; 3] {
        self.sum
            .map(|sum| ((sum + self.count / 2) / self.count) as u8)
    }
}

fn key(rgb: [u8; 3]) -> usize {
    (usize::from(rgb[0] >> 3) << 10) | (usize::from(rgb[1] >> 3) << 5) | usize::from(rgb[2] >> 3)
}

fn distance(a: [u8; 3], b: [u8; 3]) -> u32 {
    a.into_iter()
        .zip(b)
        .map(|(a, b)| (i32::from(a) - i32::from(b)).pow(2) as u32)
        .sum()
}

#[derive(Default)]
pub(crate) struct GifPalette {
    histogram: Vec<Bin>,
    colors: Vec<[u8; 3]>,
    lookup: Vec<u16>,
}

impl GifPalette {
    fn nearest(&self, rgb: [u8; 3]) -> (usize, u32) {
        self.colors
            .iter()
            .enumerate()
            .map(|(index, color)| (index, distance(rgb, *color)))
            .min_by_key(|&(_, error)| error)
            .expect("initialized GIF palette")
    }

    fn rebuild(&mut self) {
        let mut boxes = vec![
            self.histogram
                .iter()
                .copied()
                .filter(|bin| bin.count != 0)
                .collect::<Vec<_>>(),
        ];
        while boxes.len() < COLORS {
            // Split the box with the largest population-weighted color range.
            let choice = boxes
                .iter()
                .enumerate()
                .filter(|(_, bins)| bins.len() > 1)
                .map(|(index, bins)| {
                    let mut low = [255u8; 3];
                    let mut high = [0u8; 3];
                    let mut count = 0u64;
                    for bin in bins {
                        let color = bin.color();
                        count += bin.count;
                        for c in 0..3 {
                            low[c] = low[c].min(color[c]);
                            high[c] = high[c].max(color[c]);
                        }
                    }
                    let axis = (0..3).max_by_key(|&c| high[c] - low[c]).unwrap();
                    (
                        index,
                        axis,
                        count * u64::from(high[axis] - low[axis]).pow(2),
                    )
                })
                .max_by_key(|&(_, _, score)| score);
            let Some((index, axis, _)) = choice else {
                break;
            };
            let bins = &mut boxes[index];
            bins.sort_unstable_by_key(|bin| bin.color()[axis]);
            let half = bins.iter().map(|bin| bin.count).sum::<u64>().div_ceil(2);
            let mut count = 0;
            let split = bins
                .iter()
                .position(|bin| {
                    count += bin.count;
                    count >= half
                })
                .unwrap()
                + 1;
            let tail = bins.split_off(split.min(bins.len() - 1));
            boxes.push(tail);
        }
        self.colors = boxes
            .iter()
            .map(|bins| {
                let mut combined = Bin::default();
                for bin in bins {
                    combined.count += bin.count;
                    for c in 0..3 {
                        combined.sum[c] += bin.sum[c];
                    }
                }
                combined.color()
            })
            .collect();
        self.colors.sort_unstable();
        self.lookup.fill(u16::MAX);
    }

    pub(crate) fn convert(
        &mut self,
        rgb: &ffmpeg::frame::Video,
        output: &mut ffmpeg::frame::Video,
    ) {
        assert_eq!(rgb.format(), ffmpeg::format::Pixel::RGB24);
        assert_eq!(output.format(), ffmpeg::format::Pixel::PAL8);
        assert_eq!(
            (rgb.width(), rgb.height()),
            (output.width(), output.height())
        );
        self.histogram.resize(BINS, Bin::default());
        self.histogram.fill(Bin::default());
        self.lookup.resize(BINS, u16::MAX);
        let width = rgb.width() as usize;
        let height = rgb.height() as usize;
        for row in rgb.data(0).chunks(rgb.stride(0)).take(height) {
            for pixel in row[..width * 3].chunks_exact(3) {
                let color = [pixel[0], pixel[1], pixel[2]];
                let bin = &mut self.histogram[key(color)];
                bin.count += 1;
                for (sum, channel) in bin.sum.iter_mut().zip(color) {
                    *sum += u64::from(channel);
                }
            }
        }
        // Retain a palette below eight RGB levels RMS, but refresh even for a
        // small newly introduced UI color if any occupied bin is badly represented.
        let mut error = 0u64;
        let mut worst = 0;
        if !self.colors.is_empty() {
            for bin in self.histogram.iter().filter(|bin| bin.count != 0) {
                let (_, distance) = self.nearest(bin.color());
                error += u64::from(distance) * bin.count;
                worst = worst.max(distance);
            }
        }
        if self.colors.is_empty()
            || error > (width * height) as u64 * 3 * PALETTE_RMS_LIMIT.pow(2)
            || worst > 3 * BIN_RMS_LIMIT.pow(2)
        {
            self.rebuild();
        }

        let mut palette = [0xff00_0000u32; 256];
        for (entry, color) in palette.iter_mut().zip(&self.colors) {
            *entry |=
                (u32::from(color[0]) << 16) | (u32::from(color[1]) << 8) | u32::from(color[2]);
        }
        palette[255] = 0; // Reserved transparent entry, never emitted for opaque input.
        // SAFETY: av_frame_get_buffer allocates AVPALETTE_SIZE bytes in PAL8
        // data[1], even though its linesize is zero. The caller made it writable.
        unsafe {
            std::ptr::copy_nonoverlapping(
                palette.as_ptr().cast::<u8>(),
                (*output.as_mut_ptr()).data[1],
                1024,
            );
        }
        let stride = output.stride(0);
        for y in 0..height {
            let row = &rgb.data(0)[y * rgb.stride(0)..][..width * 3];
            let dst = &mut output.data_mut(0)[y * stride..][..width];
            for (x, pixel) in row.chunks_exact(3).enumerate() {
                let color = [pixel[0], pixel[1], pixel[2]];
                let index = self.index(color);
                // Preserve exact flat colors; dither only quantization error.
                dst[x] = if self.colors[usize::from(index)] == color {
                    index
                } else {
                    let offset = (bayer(x, y) >> BAYER_SCALE) as i16 - (1 << (5 - BAYER_SCALE));
                    self.index(color.map(|c| (i16::from(c) + offset).clamp(0, 255) as u8))
                };
            }
        }
    }

    fn index(&mut self, color: [u8; 3]) -> u8 {
        let key = key(color);
        if self.lookup[key] == u16::MAX {
            let center = [
                ((key >> 10) * 8 + 4) as u8,
                (((key >> 5) & 31) * 8 + 4) as u8,
                ((key & 31) * 8 + 4) as u8,
            ];
            self.lookup[key] = self.nearest(center).0 as u16;
        }
        self.lookup[key] as u8
    }
}

// 8x8 Bayer matrix, equivalent to ordered dithering with bayer_scale=3.
fn bayer(x: usize, y: usize) -> usize {
    let mut value = 0;
    for bit in 0..3 {
        value |= (((x >> bit) ^ (y >> bit)) & 1) << (5 - bit * 2);
        value |= ((y >> bit) & 1) << (4 - bit * 2);
    }
    value
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gif_palette_preserves_flat_colors_and_refreshes_after_scene_change() {
        crate::ffmpeg_util::ensure_ffmpeg_initialized().unwrap();
        // Odd widths exercise AVFrame stride padding.
        let mut rgb = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB24, 17, 9);
        let mut output = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::PAL8, 17, 9);
        let mut quantizer = GifPalette::default();
        for colors in [
            [[23, 37, 51], [209, 151, 77]],
            [[250, 17, 180], [12, 239, 221]],
        ] {
            let stride = rgb.stride(0);
            for y in 0..9 {
                for x in 0..17 {
                    rgb.data_mut(0)[y * stride + x * 3..][..3].copy_from_slice(&colors[x % 2]);
                }
            }
            quantizer.convert(&rgb, &mut output);
            let palette = quantizer.colors.clone();
            for y in 0..9 {
                for x in 0..17 {
                    assert_eq!(
                        palette[output.data(0)[y * output.stride(0) + x] as usize],
                        colors[x % 2]
                    );
                }
            }
            let indices = output.data(0).to_vec();
            quantizer.convert(&rgb, &mut output);
            assert_eq!(quantizer.colors, palette);
            assert_eq!(output.data(0), indices);
            assert_eq!(quantizer.histogram.len(), BINS);
            assert_eq!(quantizer.lookup.len(), BINS);
        }
    }

    #[test]
    fn gif_bayer_pattern_is_spatially_stable_and_balanced() {
        let mut values = (0..8)
            .flat_map(|y| (0..8).map(move |x| bayer(x, y)))
            .collect::<Vec<_>>();
        values.sort_unstable();
        assert_eq!(values, (0..64).collect::<Vec<_>>());
        assert_eq!(bayer(3, 5), bayer(11, 13));
    }
}

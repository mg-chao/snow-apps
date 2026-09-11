//! Streaming RGB → YUV conversion for the encoder hot path.
//!
//! swscale converts single-threaded; at 4K its RGBA→NV12 pass dominates the
//! recording budget (~33 ms per frame measured). This module implements the
//! BT.709 limited-range matrix that swscale selects automatically for
//! HD-sized conversions, splits row pairs across a thread pool, and writes
//! straight into the encoder's planar frame so no packed RGBA staging copy
//! is needed at all.

use std::sync::OnceLock;

use ffmpeg_next as ffmpeg;
use rayon::prelude::*;

/// Byte order of the packed 8-bit RGB source pixels.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum RgbOrder {
    Rgba,
    /// Constructed by the BGRA capture transport.
    #[allow(dead_code)]
    Bgra,
}

impl RgbOrder {
    #[inline(always)]
    fn offsets(self) -> (usize, usize, usize) {
        match self {
            RgbOrder::Rgba => (0, 1, 2),
            RgbOrder::Bgra => (2, 1, 0),
        }
    }
}

/// Chroma planes of an interleaved (NV12) or planar (YUV420P) 4:2:0 target.
pub(crate) enum ChromaPlanes<'a> {
    Nv12 {
        uv: &'a mut [u8],
        uv_stride: usize,
    },
    Planar {
        u: &'a mut [u8],
        u_stride: usize,
        v: &'a mut [u8],
        v_stride: usize,
    },
}

/// Writable YUV 4:2:0 planes of one frame, honoring encoder strides.
pub(crate) struct Yuv420Planes<'a> {
    width: usize,
    height: usize,
    y: &'a mut [u8],
    y_stride: usize,
    chroma: ChromaPlanes<'a>,
}

/// Frames below this pixel count convert on the calling thread; the pool
/// scheduling overhead outweighs the work. Mirrors snow-capture's BGRA
/// threshold.
const PARALLEL_MIN_PIXELS: usize = 524_288;

/// Execution mode selector; tests pin serial and parallel runs explicitly.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ConversionMode {
    Auto,
    /// Test-only: pin single-threaded execution.
    #[allow(dead_code)]
    Serial,
    /// Test-only: pin multi-threaded execution.
    #[allow(dead_code)]
    Parallel,
}

// BT.601 limited range: Y in [16, 235], Cb/Cr in [16, 240] centered on 128.
// swscale's default colorspace for RGB conversions is BT.601 in this FFmpeg
// build regardless of frame size, so the kernel reproduces the shipped
// recording colors rather than shifting them to BT.709.
const KR: f32 = 0.299;
const KG: f32 = 0.587;
const KB: f32 = 0.114;
const Y_SCALE: f32 = 219.0 / 255.0;
const C_SCALE: f32 = 224.0 / 255.0;

#[inline(always)]
fn rgb_to_y(r: f32, g: f32, b: f32) -> u8 {
    let luma = KR * r + KG * g + KB * b;
    (16.0 + luma * Y_SCALE).round_ties_even().clamp(0.0, 255.0) as u8
}

#[inline(always)]
fn rgb_to_cb(r: f32, g: f32, b: f32) -> u8 {
    let luma = KR * r + KG * g + KB * b;
    (128.0 + (0.5 * (b - luma) / (1.0 - KB)) * C_SCALE)
        .round_ties_even()
        .clamp(0.0, 255.0) as u8
}

#[inline(always)]
fn rgb_to_cr(r: f32, g: f32, b: f32) -> u8 {
    let luma = KR * r + KG * g + KB * b;
    (128.0 + (0.5 * (r - luma) / (1.0 - KR)) * C_SCALE)
        .round_ties_even()
        .clamp(0.0, 255.0) as u8
}

#[inline(always)]
fn channel(bytes: &[u8], x: usize, offset: usize) -> f32 {
    f32::from(bytes[x * 4 + offset])
}

fn convert_luma_row(source: &[u8], order: RgbOrder, destination: &mut [u8], width: usize) {
    let (ro, go, bo) = order.offsets();
    for (x, output) in destination.iter_mut().enumerate().take(width) {
        *output = rgb_to_y(
            channel(source, x, ro),
            channel(source, x, go),
            channel(source, x, bo),
        );
    }
}

/// Convert one pair of source rows into one chroma row. The 2×2 block is
/// averaged in RGB first and converted once, which keeps chroma cost at a
/// quarter of the luma work.
fn convert_chroma_pair(
    top: &[u8],
    bottom: &[u8],
    order: RgbOrder,
    width: usize,
    mut write: impl FnMut(usize, u8, u8),
) {
    let (ro, go, bo) = order.offsets();
    let chroma_width = width.div_ceil(2);
    for cx in 0..chroma_width {
        let x0 = cx * 2;
        let x1 = (x0 + 1).min(width - 1);
        let mut sums = [0u32; 3];
        for (row, x) in [(top, x0), (top, x1), (bottom, x0), (bottom, x1)] {
            for (sum, offset) in sums.iter_mut().zip([ro, go, bo]) {
                *sum += u32::from(row[x * 4 + offset]);
            }
        }
        let [rs, gs, bs] = sums;
        let r = f32::from(((rs + 2) >> 2) as u16);
        let g = f32::from(((gs + 2) >> 2) as u16);
        let b = f32::from(((bs + 2) >> 2) as u16);
        write(cx, rgb_to_cb(r, g, b), rgb_to_cr(r, g, b));
    }
}

/// One parallelizable slice of row pairs with its plane views.
struct BandPlanes<'a> {
    first_pair: usize,
    pair_count: usize,
    y: &'a mut [u8],
    y_stride: usize,
    chroma: ChromaPlanes<'a>,
}

fn convert_band(source: &[u8], order: RgbOrder, width: usize, band: &mut BandPlanes) {
    let row_bytes = width * 4;
    for p in 0..band.pair_count {
        let top_row = (band.first_pair + p) * 2;
        let bottom_row = top_row + 1;
        let top = &source[top_row * row_bytes..][..row_bytes];
        let bottom = &source[bottom_row * row_bytes..][..row_bytes];

        for (row, src) in [(0usize, top), (1usize, bottom)] {
            let start = (2 * p + row) * band.y_stride;
            if let Some(destination) = band.y.get_mut(start..start + width) {
                convert_luma_row(src, order, destination, width);
            }
        }

        match &mut band.chroma {
            ChromaPlanes::Nv12 { uv, uv_stride } => {
                let uv_stride = *uv_stride;
                let start = p * uv_stride;
                let chroma_width = width.div_ceil(2);
                let Some(row) = uv.get_mut(start..start.saturating_add(chroma_width * 2)) else {
                    continue;
                };
                convert_chroma_pair(top, bottom, order, width, |cx, cb, cr| {
                    row[cx * 2] = cb;
                    row[cx * 2 + 1] = cr;
                });
            }
            ChromaPlanes::Planar {
                u,
                u_stride,
                v,
                v_stride,
            } => {
                let u_stride = *u_stride;
                let v_stride = *v_stride;
                let chroma_width = width.div_ceil(2);
                let u_start = p * u_stride;
                let v_start = p * v_stride;
                let u_row = u.get_mut(u_start..u_start + chroma_width);
                let v_row = v.get_mut(v_start..v_start + chroma_width);
                if let (Some(u_row), Some(v_row)) = (u_row, v_row) {
                    convert_chroma_pair(top, bottom, order, width, |cx, cb, cr| {
                        u_row[cx] = cb;
                        v_row[cx] = cr;
                    });
                }
            }
        }
    }
}

fn split_by_cuts<'a>(slice: &'a mut [u8], cuts: &[usize]) -> Vec<&'a mut [u8]> {
    let mut parts = Vec::with_capacity(cuts.len() + 1);
    let mut rest = slice;
    let mut previous = 0;
    for &cut in cuts {
        let (head, tail) = rest.split_at_mut(cut - previous);
        parts.push(head);
        rest = tail;
        previous = cut;
    }
    parts.push(rest);
    parts
}

fn split_chroma_bands<'a>(chroma: ChromaPlanes<'a>, row_cuts: &[usize]) -> Vec<ChromaPlanes<'a>> {
    match chroma {
        ChromaPlanes::Nv12 { uv, uv_stride } => {
            let byte_cuts: Vec<usize> = row_cuts.iter().map(|cut| cut * uv_stride).collect();
            split_by_cuts(uv, &byte_cuts)
                .into_iter()
                .map(|uv| ChromaPlanes::Nv12 { uv, uv_stride })
                .collect()
        }
        ChromaPlanes::Planar {
            u,
            u_stride,
            v,
            v_stride,
        } => {
            let u_cuts: Vec<usize> = row_cuts.iter().map(|cut| cut * u_stride).collect();
            let v_cuts: Vec<usize> = row_cuts.iter().map(|cut| cut * v_stride).collect();
            let u_parts = split_by_cuts(u, &u_cuts);
            let v_parts = split_by_cuts(v, &v_cuts);
            u_parts
                .into_iter()
                .zip(v_parts)
                .map(|(u, v)| ChromaPlanes::Planar {
                    u,
                    u_stride,
                    v,
                    v_stride,
                })
                .collect()
        }
    }
}

/// Convert `source` (packed `width * height * 4` bytes) into `planes`,
/// consuming the plane views.
pub(crate) fn convert_rgb_to_yuv420(
    source: &[u8],
    order: RgbOrder,
    planes: Yuv420Planes,
    mode: ConversionMode,
) {
    let width = planes.width;
    let height = planes.height;
    debug_assert_eq!(source.len(), width * height * 4);
    let pair_count = height / 2;
    let parallel = match mode {
        ConversionMode::Serial => false,
        ConversionMode::Parallel => pair_count >= 1 && worker_count() > 1,
        ConversionMode::Auto => {
            width * height >= PARALLEL_MIN_PIXELS && pair_count >= 2 && worker_count() > 1
        }
    };

    // Odd trailing row: luma only; chroma has no matching pair. Handled
    // first, before the plane views are consumed by band splitting.
    if height % 2 == 1 {
        let row_bytes = width * 4;
        let last = height - 1;
        let src = &source[last * row_bytes..][..row_bytes];
        let start = last * planes.y_stride;
        if let Some(destination) = planes.y.get_mut(start..start + width) {
            convert_luma_row(src, order, destination, width);
        }
    }
    if parallel {
        let workers = worker_count().min(pair_count.max(1));
        let pairs_per_band = pair_count.div_ceil(workers).max(1);
        let band_count = pair_count.div_ceil(pairs_per_band).max(1);
        let mut pair_cuts = Vec::with_capacity(band_count.saturating_sub(1));
        for band in 1..band_count {
            pair_cuts.push((band * pairs_per_band).min(pair_count));
        }
        let y_cuts: Vec<usize> = pair_cuts.iter().map(|c| c * 2 * planes.y_stride).collect();
        let y_parts = split_by_cuts(planes.y, &y_cuts);
        let chroma_parts = split_chroma_bands(planes.chroma, &pair_cuts);
        let mut first_pair = 0usize;
        let mut bands: Vec<BandPlanes> = y_parts
            .into_iter()
            .zip(chroma_parts)
            .filter_map(|(y, chroma)| {
                let count = pair_count.saturating_sub(first_pair).min(pairs_per_band);
                if count == 0 {
                    return None;
                }
                let band = BandPlanes {
                    first_pair,
                    pair_count: count,
                    y,
                    y_stride: planes.y_stride,
                    chroma,
                };
                first_pair += count;
                Some(band)
            })
            .collect();
        if let Some(pool) = conversion_pool() {
            pool.install(|| {
                bands.par_iter_mut().for_each(|band| {
                    convert_band(source, order, width, band);
                });
            });
        } else {
            for band in bands.iter_mut() {
                convert_band(source, order, width, band);
            }
        }
    } else if pair_count > 0 {
        let mut band = BandPlanes {
            first_pair: 0,
            pair_count,
            y: planes.y,
            y_stride: planes.y_stride,
            chroma: planes.chroma,
        };
        convert_band(source, order, width, &mut band);
    }
}

fn worker_count() -> usize {
    static WORKERS: OnceLock<usize> = OnceLock::new();
    *WORKERS.get_or_init(|| {
        let logical = std::thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(1);
        num_cpus::get_physical().min(logical).max(1)
    })
}

fn conversion_pool() -> Option<&'static rayon::ThreadPool> {
    static POOL: OnceLock<Option<rayon::ThreadPool>> = OnceLock::new();
    POOL.get_or_init(|| {
        let threads = worker_count();
        if threads <= 1 {
            return None;
        }
        rayon::ThreadPoolBuilder::new()
            .num_threads(threads)
            .thread_name(|index| format!("snow-yuv-convert-{index}"))
            .build()
            .ok()
    })
    .as_ref()
}

/// Extract writable YUV 4:2:0 planes from an ffmpeg frame in NV12 or
/// YUV420P format. Returns `None` for any other layout.
pub(crate) fn yuv420_planes_from_frame(
    frame: &mut ffmpeg::frame::Video,
) -> Option<Yuv420Planes<'_>> {
    let width = frame.width() as usize;
    let height = frame.height() as usize;
    if width == 0 || height == 0 {
        return None;
    }
    let y_stride = frame.stride(0);
    let chroma_stride = frame.stride(1);
    let chroma_height = height.div_ceil(2);
    if y_stride < width || chroma_stride < width.div_ceil(2) {
        return None;
    }
    // SAFETY: `data[i]` points at the frame's own plane buffers, which stay
    // live and exclusively borrowed for the returned lifetime; plane lengths
    // follow from stride * plane height, matching ffmpeg's allocation.
    unsafe {
        let raw = frame.as_mut_ptr();
        if (*raw).data[0].is_null() || (*raw).data[1].is_null() {
            return None;
        }
        let y = std::slice::from_raw_parts_mut((*raw).data[0], y_stride * height);
        let chroma: ChromaPlanes<'_> = match frame.format() {
            ffmpeg::format::Pixel::NV12 => ChromaPlanes::Nv12 {
                uv: std::slice::from_raw_parts_mut((*raw).data[1], chroma_stride * chroma_height),
                uv_stride: chroma_stride,
            },
            ffmpeg::format::Pixel::YUV420P => {
                if (*raw).data[2].is_null() {
                    return None;
                }
                let v_stride = frame.stride(2);
                ChromaPlanes::Planar {
                    u: std::slice::from_raw_parts_mut(
                        (*raw).data[1],
                        chroma_stride * chroma_height,
                    ),
                    u_stride: chroma_stride,
                    v: std::slice::from_raw_parts_mut((*raw).data[2], v_stride * chroma_height),
                    v_stride,
                }
            }
            _ => return None,
        };
        Some(Yuv420Planes {
            width,
            height,
            y,
            y_stride,
            chroma,
        })
    }
}

/// Build planes over caller-owned buffers; used by tests.
#[cfg(test)]
fn yuv420_planes_from_parts<'a>(
    width: usize,
    height: usize,
    y: &'a mut [u8],
    y_stride: usize,
    chroma: ChromaPlanes<'a>,
) -> Yuv420Planes<'a> {
    Yuv420Planes {
        width,
        height,
        y,
        y_stride,
        chroma,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn gradient_source(width: usize, height: usize, order: RgbOrder) -> Vec<u8> {
        let mut source = vec![0u8; width * height * 4];
        let (ro, go, bo) = order.offsets();
        for y in 0..height {
            for x in 0..width {
                let pixel = &mut source[(y * width + x) * 4..][..4];
                pixel[ro] = (x * 255 / width.max(1)) as u8;
                pixel[go] = (y * 255 / height.max(1)) as u8;
                pixel[bo] = ((x + y) * 255 / (width + height).max(1)) as u8;
                pixel[3] = 255;
            }
        }
        source
    }

    fn convert_into_buffers(
        source: &[u8],
        order: RgbOrder,
        width: usize,
        height: usize,
        mode: ConversionMode,
        nv12: bool,
    ) -> (Vec<u8>, Vec<u8>, Vec<u8>) {
        let chroma_len = width.div_ceil(2) * height.div_ceil(2);
        let mut y = vec![0u8; width * height];
        let mut second = vec![0u8; if nv12 { chroma_len * 2 } else { chroma_len }];
        let mut third = vec![0u8; if nv12 { 0 } else { chroma_len }];
        let chroma_width = width.div_ceil(2);
        if nv12 {
            let (y_plane, uv) = (y.as_mut_slice(), second.as_mut_slice());
            convert_rgb_to_yuv420(
                source,
                order,
                yuv420_planes_from_parts(
                    width,
                    height,
                    y_plane,
                    width,
                    ChromaPlanes::Nv12 {
                        uv,
                        uv_stride: chroma_width * 2,
                    },
                ),
                mode,
            );
        } else {
            let (y_plane, u, v) = (
                y.as_mut_slice(),
                second.as_mut_slice(),
                third.as_mut_slice(),
            );
            convert_rgb_to_yuv420(
                source,
                order,
                yuv420_planes_from_parts(
                    width,
                    height,
                    y_plane,
                    width,
                    ChromaPlanes::Planar {
                        u,
                        u_stride: chroma_width,
                        v,
                        v_stride: chroma_width,
                    },
                ),
                mode,
            );
        }
        (y, second, third)
    }

    #[test]
    fn bt601_reference_colors_match_double_precision() {
        let colors: [(u8, u8, u8, &str); 5] = [
            (255, 255, 255, "white"),
            (0, 0, 0, "black"),
            (255, 0, 0, "red"),
            (0, 255, 0, "green"),
            (0, 0, 255, "blue"),
        ];
        for (r, g, b, name) in colors {
            let (rf, gf, bf) = (f64::from(r), f64::from(g), f64::from(b));
            let luma = 0.299 * rf + 0.587 * gf + 0.114 * bf;
            let expect_y = 16.0 + luma * (219.0 / 255.0);
            let expect_cb = 128.0 + (0.5 * (bf - luma) / (1.0 - 0.114)) * (224.0 / 255.0);
            let expect_cr = 128.0 + (0.5 * (rf - luma) / (1.0 - 0.299)) * (224.0 / 255.0);
            let (y, cb, cr) = (
                rgb_to_y(f32::from(r), f32::from(g), f32::from(b)),
                rgb_to_cb(f32::from(r), f32::from(g), f32::from(b)),
                rgb_to_cr(f32::from(r), f32::from(g), f32::from(b)),
            );
            assert!(
                (f64::from(y) - expect_y).abs() <= 1.0,
                "{name}: Y {y} vs {expect_y:.3}"
            );
            assert!(
                (f64::from(cb) - expect_cb).abs() <= 1.0,
                "{name}: Cb {cb} vs {expect_cb:.3}"
            );
            assert!(
                (f64::from(cr) - expect_cr).abs() <= 1.0,
                "{name}: Cr {cr} vs {expect_cr:.3}"
            );
        }
    }

    #[test]
    fn parallel_bands_match_serial_conversion() {
        for (width, height) in [(1920, 1080), (1281, 721), (64, 64)] {
            let source = gradient_source(width, height, RgbOrder::Rgba);
            let serial = convert_into_buffers(
                &source,
                RgbOrder::Rgba,
                width,
                height,
                ConversionMode::Serial,
                false,
            );
            let parallel = convert_into_buffers(
                &source,
                RgbOrder::Rgba,
                width,
                height,
                ConversionMode::Parallel,
                false,
            );
            assert_eq!(serial.0, parallel.0, "luma {width}x{height}");
            assert_eq!(serial.1, parallel.1, "cb {width}x{height}");
            assert_eq!(serial.2, parallel.2, "cr {width}x{height}");
        }
    }

    #[test]
    fn bgra_input_matches_swapped_rgba() {
        let (width, height) = (33, 17);
        let mut rgba = gradient_source(width, height, RgbOrder::Rgba);
        let mut bgra = rgba.clone();
        for pixel in bgra.chunks_exact_mut(4) {
            pixel.swap(0, 2);
        }
        let rgba_out = convert_into_buffers(
            &rgba,
            RgbOrder::Rgba,
            width,
            height,
            ConversionMode::Serial,
            false,
        );
        let bgra_out = convert_into_buffers(
            &bgra,
            RgbOrder::Bgra,
            width,
            height,
            ConversionMode::Serial,
            false,
        );
        assert_eq!(rgba_out.0, bgra_out.0);
        assert_eq!(rgba_out.1, bgra_out.1);
        assert_eq!(rgba_out.2, bgra_out.2);
        rgba.clear();
    }

    #[test]
    fn nv12_and_planar_layouts_share_luma_and_split_chroma() {
        let (width, height) = (16, 8);
        let source = gradient_source(width, height, RgbOrder::Rgba);
        let planar = convert_into_buffers(
            &source,
            RgbOrder::Rgba,
            width,
            height,
            ConversionMode::Serial,
            false,
        );
        let nv12 = convert_into_buffers(
            &source,
            RgbOrder::Rgba,
            width,
            height,
            ConversionMode::Serial,
            true,
        );
        assert_eq!(planar.0, nv12.0);
        let chroma_len = width / 2 * height / 2;
        for chroma_index in 0..chroma_len {
            assert_eq!(nv12.1[chroma_index * 2], planar.1[chroma_index]);
            assert_eq!(nv12.1[chroma_index * 2 + 1], planar.2[chroma_index]);
        }
    }

    #[test]
    fn padded_strides_leave_padding_bytes_untouched() {
        let (width, height) = (12, 6);
        let source = gradient_source(width, height, RgbOrder::Rgba);
        let y_stride = width + 8;
        let chroma_width = width / 2;
        let uv_stride = chroma_width * 2 + 8;
        let mut y = vec![0xA5; y_stride * height];
        let mut uv = vec![0xA5; uv_stride * height / 2];
        {
            let (y_plane, uv_plane) = (y.as_mut_slice(), uv.as_mut_slice());
            convert_rgb_to_yuv420(
                &source,
                RgbOrder::Rgba,
                yuv420_planes_from_parts(
                    width,
                    height,
                    y_plane,
                    y_stride,
                    ChromaPlanes::Nv12 {
                        uv: uv_plane,
                        uv_stride,
                    },
                ),
                ConversionMode::Serial,
            );
        }
        for row in 0..height {
            for pad in width..y_stride {
                assert_eq!(y[row * y_stride + pad], 0xA5, "y pad row {row}");
            }
            if row < height / 2 {
                for pad in chroma_width * 2..uv_stride {
                    assert_eq!(uv[row * uv_stride + pad], 0xA5, "uv pad row {row}");
                }
            }
        }
    }

    #[test]
    fn odd_dimensions_convert_with_edge_replication() {
        let (width, height) = (7, 5);
        let source = gradient_source(width, height, RgbOrder::Rgba);
        let (y, u, v) = convert_into_buffers(
            &source,
            RgbOrder::Rgba,
            width,
            height,
            ConversionMode::Serial,
            false,
        );
        assert!(y.iter().any(|byte| *byte != 0));
        // The odd trailing row must have luma output.
        assert!(
            y[(height - 1) * width..].iter().any(|byte| *byte != 0),
            "odd trailing row needs luma"
        );
        assert!(u.iter().any(|byte| *byte != 0));
        assert!(v.iter().any(|byte| *byte != 0));
    }

    #[test]
    fn kernel_matches_swscale_within_tolerance_for_hd_frame() {
        // swscale's default RGB colorspace is BT.601 in this build, matching
        // the kernel, so a 1920x1080 gradient must agree closely. Chroma
        // differs more than luma because swscale filters rather than box
        // averages. A wrong color matrix shows up as 10+ units.
        let (width, height) = (1920, 1080);
        let source = gradient_source(width, height, RgbOrder::Rgba);
        let (y, u, v) = convert_into_buffers(
            &source,
            RgbOrder::Rgba,
            width,
            height,
            ConversionMode::Parallel,
            false,
        );

        let mut input =
            ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width as u32, height as u32);
        let stride = input.stride(0);
        let packed = width * 4;
        let dst = input.data_mut(0);
        for row in 0..height {
            dst[row * stride..row * stride + packed]
                .copy_from_slice(&source[row * packed..(row + 1) * packed]);
        }
        let mut reference =
            ffmpeg::frame::Video::new(ffmpeg::format::Pixel::YUV420P, width as u32, height as u32);
        let mut scaler = ffmpeg::software::scaling::Context::get(
            ffmpeg::format::Pixel::RGBA,
            width as u32,
            height as u32,
            ffmpeg::format::Pixel::YUV420P,
            width as u32,
            height as u32,
            ffmpeg::software::scaling::flag::Flags::BICUBIC,
        )
        .unwrap();
        scaler.run(&input, &mut reference).unwrap();

        let compare_rows = |name: &str,
                            ours: &[u8],
                            theirs: &[u8],
                            stride: usize,
                            row_width: usize,
                            rows: usize,
                            tolerance: i32| {
            let mut worst_interior: i32 = 0;
            let mut worst_edge: i32 = 0;
            for row in 0..rows {
                let theirs_row = &theirs[row * stride..row * stride + row_width];
                let ours_row = &ours[row * row_width..(row + 1) * row_width];
                for (left, right) in ours_row.iter().zip(theirs_row) {
                    let delta = (i32::from(*left) - i32::from(*right)).abs();
                    let edge = row == 0 || row + 1 == height;
                    if edge {
                        worst_edge = worst_edge.max(delta);
                    } else {
                        worst_interior = worst_interior.max(delta);
                    }
                }
            }
            assert!(
                worst_interior <= tolerance,
                "{name} interior exceeded tolerance {tolerance}: {worst_interior}"
            );
            assert!(
                worst_edge <= tolerance + 4,
                "{name} edge exceeded tolerance {}: {worst_edge}",
                tolerance + 4
            );
        };
        compare_rows(
            "luma",
            &y,
            reference.data(0),
            reference.stride(0),
            width,
            height,
            2,
        );
        compare_rows(
            "cb",
            &u,
            reference.data(1),
            reference.stride(1),
            width / 2,
            height / 2,
            8,
        );
        compare_rows(
            "cr",
            &v,
            reference.data(2),
            reference.stride(2),
            width / 2,
            height / 2,
            8,
        );
    }
}

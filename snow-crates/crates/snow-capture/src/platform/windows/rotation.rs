use rayon::prelude::*;
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_MODE_ROTATION, DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180,
    DXGI_MODE_ROTATION_ROTATE270,
};

use crate::backend::CaptureBlitRegion;
use crate::error::{CaptureError, CaptureResult};
use crate::frame::Frame;

pub(super) fn is_rotated(rotation: DXGI_MODE_ROTATION) -> bool {
    matches!(
        rotation,
        DXGI_MODE_ROTATION_ROTATE90 | DXGI_MODE_ROTATION_ROTATE180 | DXGI_MODE_ROTATION_ROTATE270
    )
}

pub(super) fn oriented_size(width: u32, height: u32, rotation: DXGI_MODE_ROTATION) -> (u32, u32) {
    match rotation {
        DXGI_MODE_ROTATION_ROTATE90 | DXGI_MODE_ROTATION_ROTATE270 => (height, width),
        _ => (width, height),
    }
}

pub(super) fn hdr_gpu_rotation(
    format: Option<windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT>,
    hdr: bool,
    gpu_conversion: bool,
    rotation: DXGI_MODE_ROTATION,
) -> DXGI_MODE_ROTATION {
    use windows::Win32::Graphics::Dxgi::Common::{
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_MODE_ROTATION_IDENTITY,
    };
    if format == Some(DXGI_FORMAT_R16G16B16A16_FLOAT) && hdr && gpu_conversion {
        rotation
    } else {
        DXGI_MODE_ROTATION_IDENTITY
    }
}

pub(super) fn oriented_blit(
    blit: CaptureBlitRegion,
    source_width: u32,
    source_height: u32,
    rotation: DXGI_MODE_ROTATION,
) -> CaptureResult<CaptureBlitRegion> {
    let inverse = match rotation {
        DXGI_MODE_ROTATION_ROTATE90 => DXGI_MODE_ROTATION_ROTATE270,
        DXGI_MODE_ROTATION_ROTATE270 => DXGI_MODE_ROTATION_ROTATE90,
        _ => rotation,
    };
    let mapped = native_blit(blit, source_width, source_height, inverse)?;
    Ok(CaptureBlitRegion {
        dst_x: blit.dst_x,
        dst_y: blit.dst_y,
        ..mapped
    })
}

pub(super) fn has_native_region_history(
    frame: Option<&Frame>,
    previous_blit: Option<CaptureBlitRegion>,
    blit: CaptureBlitRegion,
    format: crate::frame::CapturePixelFormat,
    opened_capture_access: bool,
) -> bool {
    !opened_capture_access
        && previous_blit == Some(blit)
        && frame.is_some_and(|frame| {
            frame.width() == blit.width
                && frame.height() == blit.height
                && frame.pixel_format() == format
                && !frame.as_bytes().is_empty()
        })
}

const ROTATION_TILE: usize = 32;
const PARALLEL_ROTATION_MIN_PIXELS: usize = 1024 * 1024;

fn rotate_quarter_turn<const CLOCKWISE: bool>(
    input: &[[u8; 4]],
    output: &mut [[u8; 4]],
    source_width: usize,
    source_height: usize,
    destination_width: usize,
    dst_x: usize,
) {
    // Each task owns complete destination rows. Tiling keeps both sides of
    // the transpose local to cache, without unsafe writes or scratch copies.
    let rotate_band = |(band_index, band): (usize, &mut [[u8; 4]])| {
        for tile_x in (0..source_height).step_by(ROTATION_TILE) {
            let end_x = (tile_x + ROTATION_TILE).min(source_height);
            for (row_index, row) in band.chunks_exact_mut(destination_width).enumerate() {
                let y = band_index * ROTATION_TILE + row_index;
                let sx = if CLOCKWISE { y } else { source_width - 1 - y };
                for (offset, pixel) in row[dst_x + tile_x..dst_x + end_x].iter_mut().enumerate() {
                    let x = tile_x + offset;
                    let sy = if CLOCKWISE { source_height - 1 - x } else { x };
                    *pixel = input[sy * source_width + sx];
                }
            }
        }
    };
    let band_pixels = destination_width * ROTATION_TILE;
    if input.len() >= PARALLEL_ROTATION_MIN_PIXELS {
        output
            .par_chunks_mut(band_pixels)
            .enumerate()
            .for_each(rotate_band);
    } else {
        output
            .chunks_mut(band_pixels)
            .enumerate()
            .for_each(rotate_band);
    }
}

/// Map desktop coordinates back into DXGI's unrotated surface. The resulting
/// crop is read into a separate native buffer before being oriented for output.
pub(super) fn native_blit(
    blit: CaptureBlitRegion,
    desktop_width: u32,
    desktop_height: u32,
    rotation: DXGI_MODE_ROTATION,
) -> CaptureResult<CaptureBlitRegion> {
    let right = blit
        .src_x
        .checked_add(blit.width)
        .ok_or(CaptureError::BufferOverflow)?;
    let bottom = blit
        .src_y
        .checked_add(blit.height)
        .ok_or(CaptureError::BufferOverflow)?;
    if right > desktop_width || bottom > desktop_height {
        return Err(CaptureError::BufferOverflow);
    }
    let (src_x, src_y) = match rotation {
        DXGI_MODE_ROTATION_ROTATE90 => (blit.src_y, desktop_width - right),
        DXGI_MODE_ROTATION_ROTATE180 => (desktop_width - right, desktop_height - bottom),
        DXGI_MODE_ROTATION_ROTATE270 => (desktop_height - bottom, blit.src_x),
        _ => (blit.src_x, blit.src_y),
    };
    let (width, height) = oriented_size(blit.width, blit.height, rotation);
    Ok(CaptureBlitRegion {
        src_x,
        src_y,
        width,
        height,
        dst_x: 0,
        dst_y: 0,
    })
}

pub(super) fn orient_into(
    source: &Frame,
    destination: &mut Frame,
    dst_x: u32,
    dst_y: u32,
    rotation: DXGI_MODE_ROTATION,
) -> CaptureResult<()> {
    let (width, height) = oriented_size(source.width(), source.height(), rotation);
    let right = dst_x
        .checked_add(width)
        .ok_or(CaptureError::BufferOverflow)?;
    let bottom = dst_y
        .checked_add(height)
        .ok_or(CaptureError::BufferOverflow)?;
    if right > destination.width()
        || bottom > destination.height()
        || source.pixel_format() != destination.pixel_format()
    {
        return Err(CaptureError::BufferOverflow);
    }
    if width == 0 || height == 0 {
        return Ok(());
    }
    let source_width = source.width() as usize;
    let source_height = source.height() as usize;
    let destination_width = destination.width() as usize;
    let input = source.as_bytes().as_chunks::<4>().0;
    let output = destination.as_mut_bytes().as_chunks_mut::<4>().0;
    let output =
        &mut output[dst_y as usize * destination_width..bottom as usize * destination_width];
    match rotation {
        DXGI_MODE_ROTATION_ROTATE90 => rotate_quarter_turn::<true>(
            input,
            output,
            source_width,
            source_height,
            destination_width,
            dst_x as usize,
        ),
        DXGI_MODE_ROTATION_ROTATE270 => rotate_quarter_turn::<false>(
            input,
            output,
            source_width,
            source_height,
            destination_width,
            dst_x as usize,
        ),
        DXGI_MODE_ROTATION_ROTATE180 => {
            for (y, row) in output.chunks_exact_mut(destination_width).enumerate() {
                let src = (source_height - 1 - y) * source_width;
                for (destination, source) in row[dst_x as usize..right as usize]
                    .iter_mut()
                    .zip(input[src..src + source_width].iter().rev())
                {
                    *destination = *source;
                }
            }
        }
        _ => {
            for (y, row) in output.chunks_exact_mut(destination_width).enumerate() {
                let src = y * source_width;
                row[dst_x as usize..right as usize]
                    .copy_from_slice(&input[src..src + source_width]);
            }
        }
    }
    Ok(())
}

pub(super) fn orient_frame(
    source: &Frame,
    mut destination: Frame,
    rotation: DXGI_MODE_ROTATION,
) -> CaptureResult<Frame> {
    let (width, height) = oriented_size(source.width(), source.height(), rotation);
    destination.ensure_capacity(width, height, source.pixel_format())?;
    orient_into(source, &mut destination, 0, 0, rotation)?;
    destination.metadata = source.metadata.clone();
    // Native damage rectangles must not escape into desktop coordinates.
    // An empty list denotes full-frame damage in the capture contract.
    destination.metadata.dirty_rects.clear();
    Ok(destination)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frame::CapturePixelFormat;
    use windows::Win32::Graphics::Dxgi::Common::{
        DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180,
        DXGI_MODE_ROTATION_ROTATE270,
    };

    #[test]
    fn hdr_gpu_rotation_requires_the_existing_hdr_conversion_path() {
        use windows::Win32::Graphics::Dxgi::Common::{
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
        };
        for format in [
            None,
            Some(DXGI_FORMAT_B8G8R8A8_UNORM),
            Some(DXGI_FORMAT_R16G16B16A16_FLOAT),
        ] {
            for hdr in [false, true] {
                for gpu in [false, true] {
                    for rotation in [
                        DXGI_MODE_ROTATION_IDENTITY,
                        DXGI_MODE_ROTATION_ROTATE90,
                        DXGI_MODE_ROTATION_ROTATE180,
                        DXGI_MODE_ROTATION_ROTATE270,
                    ] {
                        let actual = hdr_gpu_rotation(format, hdr, gpu, rotation);
                        if format == Some(DXGI_FORMAT_R16G16B16A16_FLOAT) && hdr && gpu {
                            assert_eq!(actual, rotation);
                        } else {
                            assert_eq!(actual, DXGI_MODE_ROTATION_IDENTITY);
                        }
                    }
                }
            }
        }
    }

    #[test]
    #[ignore = "Release-only rotation benchmark; run scripts/run-capture-rotation-perf.ps1"]
    fn rotation_performance_benchmark() -> CaptureResult<()> {
        use std::hint::black_box;
        use std::time::Instant;
        if cfg!(debug_assertions) {
            return Err(CaptureError::InvalidConfig(
                "rotation benchmarks require Release".into(),
            ));
        }
        println!("native_size,rotation,first_call_ms,median_ms,p95_ms");
        for (width, height) in [(320, 180), (1920, 1080), (3840, 2160)] {
            let mut source = Frame::empty();
            source.ensure_rgba_capacity(width, height)?;
            for (index, pixel) in source.as_mut_bytes().chunks_exact_mut(4).enumerate() {
                pixel.copy_from_slice(&(index as u32).to_ne_bytes());
            }
            for rotation in [
                DXGI_MODE_ROTATION_ROTATE90,
                DXGI_MODE_ROTATION_ROTATE180,
                DXGI_MODE_ROTATION_ROTATE270,
            ] {
                let (out_width, out_height) = oriented_size(width, height, rotation);
                let mut destination = Frame::empty();
                destination.ensure_rgba_capacity(out_width, out_height)?;
                let first_start = Instant::now();
                orient_into(&source, &mut destination, 0, 0, rotation)?;
                let first_call_ms = first_start.elapsed().as_secs_f64() * 1000.0;
                for _ in 0..10 {
                    orient_into(&source, &mut destination, 0, 0, rotation)?;
                }
                let mut samples = Vec::with_capacity(100);
                for _ in 0..100 {
                    let start = Instant::now();
                    orient_into(
                        black_box(&source),
                        black_box(&mut destination),
                        0,
                        0,
                        rotation,
                    )?;
                    samples.push(start.elapsed().as_secs_f64() * 1000.0);
                    black_box(destination.as_bytes());
                }
                samples.sort_by(f64::total_cmp);
                println!(
                    "{width}x{height},{},{first_call_ms:.4},{:.4},{:.4}",
                    (rotation.0 - 1) * 90,
                    samples[50],
                    samples[95]
                );
            }
        }
        Ok(())
    }

    #[test]
    fn dxgi_rotation_preserves_pixels_in_desktop_orientation() -> CaptureResult<()> {
        for format in [CapturePixelFormat::Rgba8, CapturePixelFormat::Bgra8] {
            let mut source = Frame::empty();
            source.ensure_capacity(3, 2, format)?;
            for (pixel, id) in source.as_mut_bytes().chunks_exact_mut(4).zip(1..=6) {
                pixel.copy_from_slice(&[id, id + 10, id + 20, 255]);
            }
            for (rotation, width, height, expected) in [
                (DXGI_MODE_ROTATION_IDENTITY, 3, 2, [1, 2, 3, 4, 5, 6]),
                (DXGI_MODE_ROTATION_ROTATE90, 2, 3, [4, 1, 5, 2, 6, 3]),
                (DXGI_MODE_ROTATION_ROTATE180, 3, 2, [6, 5, 4, 3, 2, 1]),
                (DXGI_MODE_ROTATION_ROTATE270, 2, 3, [3, 6, 2, 5, 1, 4]),
            ] {
                let frame = orient_frame(&source, Frame::empty(), rotation)?;
                assert_eq!(
                    (frame.width(), frame.height()),
                    (width, height),
                    "{rotation:?}"
                );
                assert_eq!(frame.pixel_format(), format);
                let bytes: Vec<u8> = expected
                    .into_iter()
                    .flat_map(|id| [id, id + 10, id + 20, 255])
                    .collect();
                assert_eq!(frame.as_bytes(), bytes, "{rotation:?}");
            }
        }
        Ok(())
    }

    #[test]
    fn rotated_region_matches_desktop_crop_at_every_edge() -> CaptureResult<()> {
        for (rotation, desktop_width, desktop_height, desktop) in [
            (DXGI_MODE_ROTATION_IDENTITY, 3, 2, [1, 2, 3, 4, 5, 6]),
            (DXGI_MODE_ROTATION_ROTATE90, 2, 3, [4, 1, 5, 2, 6, 3]),
            (DXGI_MODE_ROTATION_ROTATE180, 3, 2, [6, 5, 4, 3, 2, 1]),
            (DXGI_MODE_ROTATION_ROTATE270, 2, 3, [3, 6, 2, 5, 1, 4]),
        ] {
            for y in 0..desktop_height {
                for x in 0..desktop_width {
                    for height in 1..=desktop_height - y {
                        for width in 1..=desktop_width - x {
                            let blit = CaptureBlitRegion {
                                src_x: x,
                                src_y: y,
                                width,
                                height,
                                dst_x: 1,
                                dst_y: 1,
                            };
                            let native =
                                native_blit(blit, desktop_width, desktop_height, rotation)?;
                            assert_eq!((native.dst_x, native.dst_y), (0, 0));
                            let mut crop = Frame::empty();
                            crop.ensure_rgba_capacity(native.width, native.height)?;
                            for cy in 0..native.height {
                                for cx in 0..native.width {
                                    let id =
                                        ((native.src_y + cy) * 3 + native.src_x + cx + 1) as u8;
                                    let offset = ((cy * native.width + cx) * 4) as usize;
                                    crop.as_mut_bytes()[offset..offset + 4].fill(id);
                                }
                            }
                            let mut output = Frame::empty();
                            output.ensure_rgba_capacity(width + 2, height + 2)?;
                            output.as_mut_bytes().fill(99);
                            orient_into(&crop, &mut output, 1, 1, rotation)?;
                            for oy in 0..height + 2 {
                                for ox in 0..width + 2 {
                                    let expected =
                                        if ox > 0 && ox <= width && oy > 0 && oy <= height {
                                            desktop[((y + oy - 1) * desktop_width + x + ox - 1)
                                                as usize]
                                        } else {
                                            99
                                        };
                                    let offset = ((oy * (width + 2) + ox) * 4) as usize;
                                    assert_eq!(
                                        &output.as_bytes()[offset..offset + 4],
                                        &[expected; 4],
                                        "{rotation:?}, {blit:?}"
                                    );
                                }
                            }
                        }
                    }
                }
            }
        }
        Ok(())
    }

    #[test]
    fn rotation_uses_metadata_even_for_square_frames_and_reused_buffers() -> CaptureResult<()> {
        use crate::frame::{ColorSpace, DirtyRect};
        let mut source = Frame::empty();
        source.ensure_rgba_capacity(2, 2)?;
        for (pixel, id) in source.as_mut_bytes().chunks_exact_mut(4).zip(1..=4) {
            pixel.fill(id);
        }
        source
            .metadata
            .set_timing(Some(std::time::Instant::now()), Some(123));
        source.metadata.color_space = ColorSpace::Srgb;
        source.metadata.dirty_rects.push(DirtyRect {
            x: 0,
            y: 0,
            width: 1,
            height: 1,
        });
        let mut output = Frame::empty();
        for (rotation, expected) in [
            (DXGI_MODE_ROTATION_ROTATE90, [3, 1, 4, 2]),
            (DXGI_MODE_ROTATION_ROTATE270, [2, 4, 1, 3]),
            (DXGI_MODE_ROTATION_ROTATE180, [4, 3, 2, 1]),
            (DXGI_MODE_ROTATION_IDENTITY, [1, 2, 3, 4]),
        ] {
            output = orient_frame(&source, output, rotation)?;
            let pixels: Vec<u8> = output.as_bytes().chunks_exact(4).map(|p| p[0]).collect();
            assert_eq!(pixels, expected);
            assert!(output.metadata.dirty_rects.is_empty());
            assert_eq!(
                output
                    .metadata
                    .stream_timestamp
                    .as_ref()
                    .unwrap()
                    .raw_os_ticks,
                Some(123)
            );
            assert_eq!(output.metadata.color_space, source.metadata.color_space);
        }
        assert_eq!(
            source.metadata.dirty_rects.len(),
            1,
            "native history must remain intact"
        );
        Ok(())
    }

    #[test]
    fn rotation_rejects_out_of_bounds_regions_before_writing() -> CaptureResult<()> {
        let blit = CaptureBlitRegion {
            src_x: u32::MAX,
            src_y: 0,
            width: 2,
            height: 1,
            dst_x: 0,
            dst_y: 0,
        };
        assert!(native_blit(blit, 2, 3, DXGI_MODE_ROTATION_ROTATE90).is_err());
        assert!(
            native_blit(
                CaptureBlitRegion { src_x: 1, ..blit },
                2,
                3,
                DXGI_MODE_ROTATION_ROTATE90
            )
            .is_err()
        );
        let mut source = Frame::empty();
        source.ensure_rgba_capacity(3, 2)?;
        let mut destination = Frame::empty();
        destination.ensure_rgba_capacity(2, 3)?;
        destination.as_mut_bytes().fill(99);
        assert!(orient_into(&source, &mut destination, 1, 0, DXGI_MODE_ROTATION_ROTATE90).is_err());
        assert!(
            orient_into(
                &source,
                &mut destination,
                u32::MAX,
                0,
                DXGI_MODE_ROTATION_ROTATE90
            )
            .is_err()
        );
        assert!(destination.as_bytes().iter().all(|&byte| byte == 99));
        Ok(())
    }

    #[test]
    fn tiled_rotation_matches_reference_across_tiles_and_parallel_bands() -> CaptureResult<()> {
        for (width, height) in [(1, 65), (65, 1), (31, 33), (33, 65), (1025, 1025)] {
            let mut source = Frame::empty();
            source.ensure_rgba_capacity(width, height)?;
            for (index, pixel) in source.as_mut_bytes().chunks_exact_mut(4).enumerate() {
                pixel.copy_from_slice(&(index as u32).to_ne_bytes());
            }
            for rotation in [
                DXGI_MODE_ROTATION_ROTATE90,
                DXGI_MODE_ROTATION_ROTATE180,
                DXGI_MODE_ROTATION_ROTATE270,
            ] {
                let (out_width, out_height) = oriented_size(width, height, rotation);
                let mut destination = Frame::empty();
                destination.ensure_rgba_capacity(out_width + 3, out_height + 4)?;
                destination.as_mut_bytes().fill(255);
                orient_into(&source, &mut destination, 1, 2, rotation)?;
                for sy in 0..height {
                    for sx in 0..width {
                        let (dx, dy) = match rotation {
                            DXGI_MODE_ROTATION_ROTATE90 => (height - 1 - sy, sx),
                            DXGI_MODE_ROTATION_ROTATE180 => (width - 1 - sx, height - 1 - sy),
                            _ => (sy, width - 1 - sx),
                        };
                        let offset = (((dy + 2) * (out_width + 3) + dx + 1) * 4) as usize;
                        assert_eq!(
                            &destination.as_bytes()[offset..offset + 4],
                            &(sy * width + sx).to_ne_bytes()
                        );
                    }
                }
                for (index, pixel) in destination.as_bytes().chunks_exact(4).enumerate() {
                    let x = index as u32 % (out_width + 3);
                    let y = index as u32 / (out_width + 3);
                    if x == 0 || x > out_width || y < 2 || y >= out_height + 2 {
                        assert_eq!(pixel, &[255; 4]);
                    }
                }
            }
        }
        Ok(())
    }

    #[test]
    fn native_region_history_requires_matching_pixels_crop_and_capture_access() -> CaptureResult<()>
    {
        let blit = CaptureBlitRegion {
            src_x: 12,
            src_y: 34,
            width: 3,
            height: 2,
            dst_x: 0,
            dst_y: 0,
        };
        let mut frame = Frame::empty();
        assert!(!has_native_region_history(
            Some(&frame),
            Some(blit),
            blit,
            CapturePixelFormat::Rgba8,
            false
        ));
        frame.ensure_rgba_capacity(3, 2)?;
        assert!(has_native_region_history(
            Some(&frame),
            Some(blit),
            blit,
            CapturePixelFormat::Rgba8,
            false
        ));
        for previous in [
            None,
            Some(CaptureBlitRegion { src_x: 13, ..blit }),
            Some(CaptureBlitRegion { src_y: 35, ..blit }),
        ] {
            assert!(!has_native_region_history(
                Some(&frame),
                previous,
                blit,
                CapturePixelFormat::Rgba8,
                false
            ));
        }
        assert!(!has_native_region_history(
            None,
            Some(blit),
            blit,
            CapturePixelFormat::Rgba8,
            false
        ));
        assert!(!has_native_region_history(
            Some(&frame),
            Some(blit),
            blit,
            CapturePixelFormat::Bgra8,
            false
        ));
        assert!(!has_native_region_history(
            Some(&frame),
            Some(blit),
            blit,
            CapturePixelFormat::Rgba8,
            true
        ));
        frame.ensure_rgba_capacity(2, 3)?;
        assert!(!has_native_region_history(
            Some(&frame),
            Some(blit),
            blit,
            CapturePixelFormat::Rgba8,
            false
        ));
        Ok(())
    }
}

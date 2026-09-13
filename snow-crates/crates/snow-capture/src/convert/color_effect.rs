use super::{
    BGRA_NT_PARALLEL_MIN_CHUNK_PIXELS, BGRA_NT_PARALLEL_MIN_PIXELS, ParallelConfig, SurfaceLayout,
    SurfacePixelFormat, maybe_parallel_row_chunks, run_rows_parallel_with,
};
use crate::{CapturePixelFormat, color_effect::ScreenColorTransform};

pub(super) unsafe fn convert_f16_surface(
    layout: SurfaceLayout,
    output: CapturePixelFormat,
    opaque: bool,
    transform: ScreenColorTransform,
    hdr: Option<super::HdrFrameContext>,
    allow_parallel: bool,
) {
    layout.assert_pitches(8);
    let prepared = hdr.map(|params| {
        let mut prepared = super::prepare_hdr_context_cached(params);
        prepared.screen_color_rows = Some(transform.linear_rows());
        prepared
    });
    let rows = transform.linear_rows();
    let config = super::surface_format_plan(
        SurfacePixelFormat::Rgba16Float,
        super::SurfaceConversionOptions::default(),
    )
    .parallel;
    let convert = |src, dst, count| unsafe {
        if let Some(prepared) = &prepared {
            let kernel = if opaque {
                super::f16_hdr_prepared_opaque_kernel()
            } else {
                super::f16_hdr_prepared_kernel()
            };
            kernel(src, dst, count, prepared);
            if output == CapturePixelFormat::Bgra8 {
                super::swap_rgba_to_bgra_in_place(
                    std::slice::from_raw_parts_mut(dst, count * 4),
                    count,
                );
            }
        } else {
            super::f16::convert_corrected_row(src, dst, count, rows, output, opaque);
        }
    };
    if allow_parallel
        && let Some(chunks) = maybe_parallel_row_chunks(layout, config, layout.total_pixels())
    {
        unsafe { run_rows_parallel_with(layout, chunks, config.max_workers, convert) };
    } else {
        unsafe { super::run_rows_serial_with(layout, convert) };
    }
}

pub(super) unsafe fn convert_surface(
    layout: SurfaceLayout,
    format: SurfacePixelFormat,
    output: CapturePixelFormat,
    opaque: bool,
    transform: ScreenColorTransform,
    allow_parallel: bool,
) {
    layout.assert_pitches(4);
    let config = ParallelConfig {
        min_pixels: BGRA_NT_PARALLEL_MIN_PIXELS,
        min_chunk_pixels: BGRA_NT_PARALLEL_MIN_CHUNK_PIXELS,
        max_workers: usize::MAX,
    };
    if allow_parallel
        && let Some(chunks) = maybe_parallel_row_chunks(layout, config, layout.total_pixels())
    {
        unsafe {
            run_rows_parallel_with(
                layout,
                chunks,
                config.max_workers,
                move |src, dst, count| {
                    convert_row(src, dst, count, format, output, opaque, transform);
                },
            );
        }
    } else {
        for row in 0..layout.height {
            unsafe {
                convert_row(
                    layout.src.add(row * layout.src_pitch),
                    layout.dst.add(row * layout.dst_pitch),
                    layout.width,
                    format,
                    output,
                    opaque,
                    transform,
                );
            }
        }
    }
}

unsafe fn convert_row(
    src: *const u8,
    dst: *mut u8,
    count: usize,
    format: SurfacePixelFormat,
    output: CapturePixelFormat,
    opaque: bool,
    transform: ScreenColorTransform,
) {
    let source_bgra = format == SurfacePixelFormat::Bgra8;
    let target_bgra = output == CapturePixelFormat::Bgra8;
    #[cfg(target_arch = "x86_64")]
    let mut start = 0;
    #[cfg(not(target_arch = "x86_64"))]
    let start = 0;
    #[cfg(target_arch = "x86_64")]
    if transform.inverted {
        use std::arch::x86_64::*;
        unsafe {
            let rgb = _mm_set1_epi32(0x00ffffff);
            let alpha = _mm_set1_epi32(0xff000000u32 as i32);
            let green_alpha = _mm_set1_epi32(0xff00ff00u32 as i32);
            let red = _mm_set1_epi32(0x000000ff);
            let blue = _mm_set1_epi32(0x00ff0000);
            while start + 4 <= count {
                let mut pixels = _mm_xor_si128(_mm_loadu_si128(src.add(start * 4).cast()), rgb);
                if source_bgra != target_bgra {
                    pixels = _mm_or_si128(
                        _mm_and_si128(pixels, green_alpha),
                        _mm_or_si128(
                            _mm_slli_epi32::<16>(_mm_and_si128(pixels, red)),
                            _mm_srli_epi32::<16>(_mm_and_si128(pixels, blue)),
                        ),
                    );
                }
                if opaque {
                    pixels = _mm_or_si128(pixels, alpha);
                }
                _mm_storeu_si128(dst.add(start * 4).cast(), pixels);
                start += 4;
            }
        }
    }
    #[cfg(target_arch = "x86_64")]
    if !transform.inverted {
        use std::arch::x86_64::*;
        unsafe {
            let mask = _mm_set1_epi32(255);
            let alpha = _mm_set1_epi32(0xff000000u32 as i32);
            let coefficients = transform.rows.map(|row| row.map(|v| _mm_set1_ps(v)));
            let zero = _mm_setzero_ps();
            let maximum = _mm_set1_ps(255.);
            let half = _mm_set1_ps(0.5);
            while start + 4 <= count {
                let pixels = _mm_loadu_si128(src.add(start * 4).cast());
                let low = _mm_cvtepi32_ps(_mm_and_si128(pixels, mask));
                let green = _mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32::<8>(pixels), mask));
                let high = _mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32::<16>(pixels), mask));
                let (red, blue) = if source_bgra {
                    (high, low)
                } else {
                    (low, high)
                };
                let channels = coefficients.map(|row| {
                    let value = _mm_add_ps(
                        _mm_add_ps(
                            _mm_add_ps(_mm_mul_ps(row[0], red), _mm_mul_ps(row[1], green)),
                            _mm_mul_ps(row[2], blue),
                        ),
                        row[3],
                    );
                    _mm_cvttps_epi32(_mm_add_ps(
                        _mm_min_ps(maximum, _mm_max_ps(zero, value)),
                        half,
                    ))
                });
                let (low, high) = if target_bgra {
                    (channels[2], channels[0])
                } else {
                    (channels[0], channels[2])
                };
                let result = _mm_or_si128(
                    _mm_or_si128(low, _mm_slli_epi32::<8>(channels[1])),
                    _mm_or_si128(
                        _mm_slli_epi32::<16>(high),
                        if opaque {
                            alpha
                        } else {
                            _mm_and_si128(pixels, alpha)
                        },
                    ),
                );
                _mm_storeu_si128(dst.add(start * 4).cast(), result);
                start += 4;
            }
        }
    }
    for index in start..count {
        unsafe {
            let input = src.add(index * 4);
            let pixel = [*input, *input.add(1), *input.add(2), *input.add(3)];
            let rgb = if source_bgra {
                [pixel[2], pixel[1], pixel[0]]
            } else {
                [pixel[0], pixel[1], pixel[2]]
            };
            let result: [u8; 3] = if transform.inverted {
                rgb.map(|v| 255 - v)
            } else {
                transform.rows.map(|row| {
                    (row[0] * rgb[0] as f32
                        + row[1] * rgb[1] as f32
                        + row[2] * rgb[2] as f32
                        + row[3])
                        .round()
                        .clamp(0., 255.) as u8
                })
            };
            let output = dst.add(index * 4);
            *output = result[if target_bgra { 2 } else { 0 }];
            *output.add(1) = result[1];
            *output.add(2) = result[if target_bgra { 0 } else { 2 }];
            *output.add(3) = if opaque { 255 } else { pixel[3] };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::convert::{SurfaceConversionOptions, convert_surface_to_rgba};
    #[test]
    fn hdr_sources_restore_sdr_before_tone_mapping_and_clamping() {
        let transform = ScreenColorTransform {
            inverted: true,
            rows: [
                [-1., 0., 0., 255.],
                [0., -1., 0., 255.],
                [0., 0., -1., 255.],
            ],
        };
        for boost in [1., 2., 3.5] {
            let original =
                [[0.02, 0.1, 0.3, 0.5], [0.8, 0.6, 0.4, 1.], [2., 3., 4., 1.]].repeat(11);
            let pack = |filtered: bool| {
                original
                    .iter()
                    .flat_map(|pixel| {
                        pixel.iter().enumerate().flat_map(move |(c, value)| {
                            let value = if c < 3 { value * boost } else { *value };
                            half::f16::from_f32(if filtered && c < 3 { 1. - value } else { value })
                                .to_bits()
                                .to_ne_bytes()
                        })
                    })
                    .collect::<Vec<_>>()
            };
            for output in [CapturePixelFormat::Rgba8, CapturePixelFormat::Bgra8] {
                for opaque in [true, false] {
                    let options = SurfaceConversionOptions {
                        hdr_to_sdr: Some(super::super::HdrFrameContext {
                            sdr_white_nits: 80. * boost,
                            ..Default::default()
                        }),
                        output_pixel_format: output,
                        force_opaque_alpha: opaque,
                        ..Default::default()
                    };
                    let mut expected = [0u8; 132];
                    let mut actual = [0u8; 132];
                    convert_surface_to_rgba(
                        SurfacePixelFormat::Rgba16Float,
                        &pack(false),
                        264,
                        &mut expected,
                        132,
                        33,
                        1,
                        options,
                    );
                    convert_surface_to_rgba(
                        SurfacePixelFormat::Rgba16Float,
                        &pack(true),
                        264,
                        &mut actual,
                        132,
                        33,
                        1,
                        SurfaceConversionOptions {
                            screen_color_transform: Some(transform),
                            ..options
                        },
                    );
                    for (a, b) in actual.into_iter().zip(expected) {
                        assert!(
                            a.abs_diff(b) <= 1,
                            "HDR correction {actual:?} != {expected:?}"
                        );
                    }
                }
            }
        }
    }
    #[test]
    fn general_matrix_simd_matches_scalar_including_clipping_and_alpha() {
        let transform = ScreenColorTransform {
            inverted: false,
            rows: [
                [1.25, -0.5, 0.25, -30.],
                [0., 2., 0., 40.],
                [-0.25, 0.5, 0.75, 10.],
            ],
        };
        let source = (0..1031 * 4).map(|i| (i % 251) as u8).collect::<Vec<_>>();
        for format in [SurfacePixelFormat::Bgra8, SurfacePixelFormat::Rgba8] {
            for output in [CapturePixelFormat::Bgra8, CapturePixelFormat::Rgba8] {
                for opaque in [true, false] {
                    let mut vector = vec![0; source.len()];
                    let mut scalar = vec![0; source.len()];
                    unsafe {
                        convert_row(
                            source.as_ptr(),
                            vector.as_mut_ptr(),
                            1031,
                            format,
                            output,
                            opaque,
                            transform,
                        );
                        for i in 0..1031 {
                            convert_row(
                                source.as_ptr().add(i * 4),
                                scalar.as_mut_ptr().add(i * 4),
                                1,
                                format,
                                output,
                                opaque,
                                transform,
                            );
                        }
                    }
                    assert_eq!(vector, scalar);
                }
            }
        }
    }
    #[test]
    fn inversion_handles_layouts_alpha_strides_and_vector_tails() {
        let mut matrix = [0.; 25];
        for i in 0..5 {
            matrix[i * 6] = if i < 3 { -1. } else { 1. };
        }
        matrix[20..23].fill(1.);
        let transform = ScreenColorTransform::from_magnifier_matrix(&matrix);
        for width in [1, 3, 4, 5, 31, 1024] {
            for format in [SurfacePixelFormat::Bgra8, SurfacePixelFormat::Rgba8] {
                for output in [CapturePixelFormat::Bgra8, CapturePixelFormat::Rgba8] {
                    for opaque in [false, true] {
                        let pitch = width * 4 + 13;
                        let source = (0..pitch * 3).map(|i| (i % 251) as u8).collect::<Vec<_>>();
                        let mut result = vec![0x5a; pitch * 3];
                        convert_surface_to_rgba(
                            format,
                            &source,
                            pitch,
                            &mut result,
                            pitch,
                            width,
                            3,
                            SurfaceConversionOptions {
                                screen_color_transform: transform,
                                force_opaque_alpha: opaque,
                                output_pixel_format: output,
                                ..Default::default()
                            },
                        );
                        let swap = (format == SurfacePixelFormat::Bgra8)
                            != (output == CapturePixelFormat::Bgra8);
                        for y in 0..3 {
                            for x in 0..width {
                                let i = y * pitch + x * 4;
                                assert_eq!(
                                    &result[i..i + 4],
                                    &[
                                        255 - source[i + if swap { 2 } else { 0 }],
                                        255 - source[i + 1],
                                        255 - source[i + if swap { 0 } else { 2 }],
                                        if opaque { 255 } else { source[i + 3] }
                                    ]
                                );
                            }
                            assert!(
                                result[y * pitch + width * 4..(y + 1) * pitch]
                                    .iter()
                                    .all(|v| *v == 0x5a)
                            );
                        }
                    }
                }
            }
        }
    }
}

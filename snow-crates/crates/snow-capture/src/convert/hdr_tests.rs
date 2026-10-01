use super::{HdrFrameContext, HdrPreparedPixelKernel, f16};

pub(crate) const HIGHLIGHT_RAMP_WIDTH: usize = 1027;
pub(crate) const HIGHLIGHT_RAMP_HEIGHT: usize = 4;

// Neutral and tinted highlights crossing normalized SDR white. The odd width
// exercises SIMD tails and partial GPU thread groups as well as vector batches.
pub(crate) fn highlight_ramp(sdr_white_nits: f32) -> Vec<[u16; 4]> {
    let boost = sdr_white_nits / 80.0;
    [
        [1.0, 1.0, 1.0],
        [1.0, 0.85, 0.7],
        [0.7, 1.0, 0.85],
        [0.85, 0.7, 1.0],
    ]
    .into_iter()
    .flat_map(|rgb| {
        (0..HIGHLIGHT_RAMP_WIDTH).map(move |x| {
            let intensity = 0.5 + x as f32 / 1024.0;
            [
                rgb[0] * intensity * boost,
                rgb[1] * intensity * boost,
                rgb[2] * intensity * boost,
                0.5,
            ]
            .map(|v| half::f16::from_f32(v).to_bits())
        })
    })
    .collect()
}

pub(crate) fn assert_smooth_highlight_rows(rgba: &[u8], context: &str) {
    assert_eq!(rgba.len(), HIGHLIGHT_RAMP_WIDTH * HIGHLIGHT_RAMP_HEIGHT * 4);
    // The legacy HDR curve can be darker than SDR identity near white. Preserve
    // that curve while removing abrupt steps; requiring every channel to rise
    // would instead force a change to highlight brightness or saturation.
    for (row, bytes) in rgba.chunks_exact(HIGHLIGHT_RAMP_WIDTH * 4).enumerate() {
        for x in 1..HIGHLIGHT_RAMP_WIDTH {
            for channel in 0..3 {
                let before = bytes[(x - 1) * 4 + channel];
                let after = bytes[x * 4 + channel];
                assert!(
                    before.abs_diff(after) <= 2,
                    "{context}: highlight contour at row {row}, x={x}, channel={channel}: {before} -> {after}"
                );
            }
        }
    }
}

fn cpu_hdr_kernels() -> Vec<(&'static str, HdrPreparedPixelKernel, bool)> {
    let mut kernels: Vec<(&str, HdrPreparedPixelKernel, bool)> = vec![(
        "scalar",
        f16::convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked,
        false,
    )];
    #[cfg(target_arch = "x86_64")]
    {
        use super::simd_x86::*;
        if std::arch::is_x86_feature_detected!("avx2")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            kernels.extend([
                (
                    "AVX2",
                    convert_f16_rgba_to_srgb_hdr_f16c_prepared_unchecked as HdrPreparedPixelKernel,
                    false,
                ),
                (
                    "AVX2 opaque",
                    convert_f16_rgba_to_srgb_hdr_f16c_prepared_opaque_unchecked
                        as HdrPreparedPixelKernel,
                    true,
                ),
            ]);
            if std::arch::is_x86_feature_detected!("fma") {
                kernels.extend([
                    (
                        "AVX2 FMA",
                        convert_f16_rgba_to_srgb_hdr_f16c_fma_prepared_unchecked
                            as HdrPreparedPixelKernel,
                        false,
                    ),
                    (
                        "AVX2 FMA opaque",
                        convert_f16_rgba_to_srgb_hdr_f16c_fma_prepared_opaque_unchecked
                            as HdrPreparedPixelKernel,
                        true,
                    ),
                ]);
            }
            if std::arch::is_x86_feature_detected!("avx512f")
                && std::arch::is_x86_feature_detected!("avx512bw")
            {
                kernels.extend([
                    (
                        "AVX512",
                        convert_f16_rgba_to_srgb_hdr_avx512_prepared_unchecked
                            as HdrPreparedPixelKernel,
                        false,
                    ),
                    (
                        "AVX512 opaque",
                        convert_f16_rgba_to_srgb_hdr_avx512_prepared_opaque_unchecked
                            as HdrPreparedPixelKernel,
                        true,
                    ),
                ]);
                if std::arch::is_x86_feature_detected!("fma") {
                    kernels.extend([
                        (
                            "AVX512 FMA",
                            convert_f16_rgba_to_srgb_hdr_avx512_fma_prepared_unchecked
                                as HdrPreparedPixelKernel,
                            false,
                        ),
                        (
                            "AVX512 FMA opaque",
                            convert_f16_rgba_to_srgb_hdr_avx512_fma_prepared_opaque_unchecked
                                as HdrPreparedPixelKernel,
                            true,
                        ),
                    ]);
                }
            }
        }
    }
    kernels.push((
        "scalar opaque",
        f16::convert_f16_rgba_to_srgb_hdr_scalar_prepared_opaque_unchecked,
        true,
    ));
    kernels
}

// Start from encoded SDR bytes, independently decode sRGB, and emulate DWM's
// documented scRGB white-level scale. The oracle remains the original bytes,
// rather than another HDR converter that could share the same color error.
pub(crate) fn boosted_sdr_palette(sdr_white_nits: f32) -> (Vec<[u16; 4]>, Vec<u8>) {
    let decode = |byte: u8| {
        let encoded = f32::from(byte) / 255.0;
        if encoded <= 0.04045 {
            encoded / 12.92
        } else {
            ((encoded + 0.055) / 1.055).powf(2.4)
        }
    };
    let mut colors: Vec<[u8; 3]> = (0..=255).map(|v| [v; 3]).collect();
    let levels = [0, 1, 8, 16, 32, 64, 96, 128, 160, 192, 224, 254, 255];
    for r in levels {
        for g in levels {
            for b in levels {
                colors.push([r, g, b]);
            }
        }
    }
    let pixels = colors
        .iter()
        .map(|color| {
            [
                decode(color[0]) * sdr_white_nits / 80.0,
                decode(color[1]) * sdr_white_nits / 80.0,
                decode(color[2]) * sdr_white_nits / 80.0,
                0.5,
            ]
            .map(|v| half::f16::from_f32(v).to_bits())
        })
        .collect();
    let expected = colors
        .into_iter()
        .flat_map(|[r, g, b]| [r, g, b, 128])
        .collect();
    (pixels, expected)
}

pub(crate) fn assert_color_bytes(actual: &[u8], expected: &[u8], opaque: bool, context: &str) {
    assert_eq!(actual.len(), expected.len());
    for (i, (&a, &b)) in actual.iter().zip(expected).enumerate() {
        if i % 4 == 3 {
            assert_eq!(a, if opaque { 255 } else { b }, "{context}: alpha");
        } else {
            assert!(a.abs_diff(b) <= 1, "{context}: color byte {i}: {a} != {b}");
        }
    }
}

#[test]
fn hdr_surface_restores_original_sdr_colors_across_cpu_kernels() {
    for sdr_white_nits in [80.0, 160.0, 203.0, 280.0, 480.0] {
        let (pixels, expected) = boosted_sdr_palette(sdr_white_nits);
        let src: Vec<u8> = pixels
            .iter()
            .flat_map(|px| px.iter().flat_map(|v| v.to_ne_bytes()))
            .collect();
        for hdr_peak_nits in [400.0, 1000.0, 4000.0] {
            for tonemap_use_lut in [false, true] {
                let prepared = f16::prepare_hdr_context(HdrFrameContext {
                    sdr_white_nits,
                    hdr_peak_nits,
                    tonemap_use_lut,
                    ..Default::default()
                });
                for (name, kernel, opaque) in cpu_hdr_kernels() {
                    let mut actual = vec![0; expected.len()];
                    unsafe { kernel(src.as_ptr(), actual.as_mut_ptr(), pixels.len(), &prepared) };
                    assert_color_bytes(
                        &actual,
                        &expected,
                        opaque,
                        &format!(
                            "{name}, white={sdr_white_nits}, peak={hdr_peak_nits}, LUT={tonemap_use_lut}"
                        ),
                    );
                }
            }
        }
    }
}

// Golden RGB bytes generated by the scalar converter at 87fb21f8^, for both
// analytic and LUT modes. These colors are outside the SDR-white handoff band.
// Keep this oracle independent of the current curve and gamut implementation.
pub(crate) fn legacy_highlight_fixture(
    sdr_white_nits: f32,
    peak_nits: f32,
) -> (Vec<[u16; 4]>, Vec<u8>) {
    let colors = [
        [1.25, 1.25, 1.25],
        [2.0, 2.0, 2.0],
        [4.0, 4.0, 4.0],
        [8.0, 8.0, 8.0],
        [1.25, 1.0625, 0.875],
        [2.0, 1.5, 1.0],
        [4.0, 3.0, 2.0],
        [8.0, 6.0, 4.0],
        [0.875, 1.25, 1.0625],
        [1.0625, 0.875, 1.25],
        [4.0, 0.5, 0.125],
        [1.25, 0.0, 0.0],
        [0.0, 1.25, 0.5],
        [0.5, 0.0, 1.25],
    ];
    let expected_rgb = match peak_nits as u32 {
        400 => [
            [255, 255, 255],
            [255, 255, 255],
            [255, 255, 255],
            [255, 255, 255],
            [255, 237, 218],
            [255, 225, 188],
            [255, 225, 188],
            [255, 225, 188],
            [218, 255, 237],
            [237, 218, 255],
            [255, 99, 49],
            [255, 0, 0],
            [0, 255, 170],
            [170, 0, 255],
        ],
        1000 => [
            [241, 241, 241],
            [255, 255, 255],
            [255, 255, 255],
            [255, 255, 255],
            [249, 231, 212],
            [255, 225, 188],
            [255, 225, 188],
            [255, 225, 188],
            [209, 245, 228],
            [237, 218, 255],
            [255, 99, 49],
            [255, 0, 0],
            [0, 255, 170],
            [170, 0, 255],
        ],
        4000 => [
            [220, 220, 220],
            [239, 239, 239],
            [255, 255, 255],
            [255, 255, 255],
            [227, 212, 194],
            [255, 225, 188],
            [255, 225, 188],
            [255, 225, 188],
            [191, 224, 209],
            [219, 201, 235],
            [255, 99, 49],
            [255, 0, 0],
            [0, 236, 156],
            [170, 0, 255],
        ],
        _ => panic!("unsupported golden peak"),
    };
    let boost = sdr_white_nits / 80.0;
    let pixels = colors
        .into_iter()
        .map(|rgb| {
            [rgb[0] * boost, rgb[1] * boost, rgb[2] * boost, 0.5]
                .map(|v| half::f16::from_f32(v).to_bits())
        })
        .collect();
    let expected = expected_rgb
        .into_iter()
        .flat_map(|[r, g, b]| [r, g, b, 128])
        .collect();
    (pixels, expected)
}

#[test]
fn hdr_conversion_preserves_legacy_highlights_outside_white_transition() {
    for sdr_white_nits in [80.0, 160.0, 280.0, 480.0] {
        for hdr_peak_nits in [400.0, 1000.0, 4000.0] {
            let (pixels, expected) = legacy_highlight_fixture(sdr_white_nits, hdr_peak_nits);
            let src: Vec<u8> = pixels
                .iter()
                .flat_map(|px| px.iter().flat_map(|v| v.to_ne_bytes()))
                .collect();
            for tonemap_use_lut in [false, true] {
                let prepared = f16::prepare_hdr_context(HdrFrameContext {
                    sdr_white_nits,
                    hdr_peak_nits,
                    tonemap_use_lut,
                    ..Default::default()
                });
                for (name, kernel, opaque) in cpu_hdr_kernels() {
                    let mut actual = vec![0; expected.len()];
                    unsafe { kernel(src.as_ptr(), actual.as_mut_ptr(), pixels.len(), &prepared) };
                    if name == "scalar" {
                        assert_eq!(actual, expected, "legacy scalar highlight bytes changed");
                    }
                    assert_color_bytes(
                        &actual,
                        &expected,
                        opaque,
                        &format!(
                            "legacy highlights: {name}, white={sdr_white_nits}, peak={hdr_peak_nits}, LUT={tonemap_use_lut}"
                        ),
                    );
                }
            }
        }
    }
}

#[test]
fn hdr_highlight_gradients_are_continuous_across_cpu_kernels() {
    let kernels = cpu_hdr_kernels();
    println!(
        "HDR kernels tested: {:?}",
        kernels.iter().map(|(name, _, _)| name).collect::<Vec<_>>()
    );
    for sdr_white_nits in [80.0, 160.0, 280.0, 480.0] {
        let pixels = highlight_ramp(sdr_white_nits);
        let src: Vec<u8> = pixels
            .iter()
            .flat_map(|px| px.iter().flat_map(|v| v.to_ne_bytes()))
            .collect();
        for hdr_peak_nits in [400.0, 1000.0, 4000.0] {
            for tonemap_use_lut in [false, true] {
                let prepared = f16::prepare_hdr_context(HdrFrameContext {
                    sdr_white_nits,
                    hdr_peak_nits,
                    tonemap_use_lut,
                    ..Default::default()
                });
                let mut reference = vec![0; pixels.len() * 4];
                unsafe {
                    f16::convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked(
                        src.as_ptr(),
                        reference.as_mut_ptr(),
                        pixels.len(),
                        &prepared,
                    );
                }
                for (name, kernel, opaque) in &kernels {
                    let mut actual = vec![0; reference.len()];
                    unsafe { kernel(src.as_ptr(), actual.as_mut_ptr(), pixels.len(), &prepared) };
                    assert_smooth_highlight_rows(
                        &actual,
                        &format!(
                            "{name}, white={sdr_white_nits}, peak={hdr_peak_nits}, LUT={tonemap_use_lut}"
                        ),
                    );
                    for (i, (a, b)) in actual.iter().zip(&reference).enumerate() {
                        if i % 4 == 3 {
                            assert_eq!(*a, if *opaque { 255 } else { 128 });
                        } else {
                            assert!(
                                a.abs_diff(*b) <= 1,
                                "{name} at byte {i}: {a} != {b}, white={sdr_white_nits}, peak={hdr_peak_nits}, LUT={tonemap_use_lut}"
                            );
                        }
                    }
                }
            }
        }
    }
}

#[test]
fn hdr_cpu_kernels_match_scalar_for_dark_saturated_and_corrected_colors() {
    let kernels = cpu_hdr_kernels();
    let mut state = 0x1234_abcd_u32;
    let mut colors = vec![
        [0.0, 0.0, 0.0, 0.5],
        [f32::NAN, f32::INFINITY, f32::NEG_INFINITY, 0.5],
        [65504.0, 1.0, 0.0, 0.5],
    ];
    for i in 0..8192 {
        let rgb = if i < 4096 {
            let t = i as f32 / 4095.0;
            let y = 1e-8 * 1e12_f32.powf(t);
            [y, y * 0.7, y * 0.3, 0.5]
        } else {
            std::array::from_fn(|channel| {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                if channel == 3 {
                    0.5
                } else {
                    let t = (state & 0xffff) as f32 / 65535.0;
                    match i % 3 {
                        0 => t * 80.0 - 20.0,
                        1 => t * 2.0,
                        _ => 1e-8 * 1e12_f32.powf(t),
                    }
                }
            })
        };
        colors.push(rgb);
    }
    let src: Vec<u8> = colors
        .iter()
        .flat_map(|pixel| {
            pixel
                .iter()
                .flat_map(|&value| half::f16::from_f32(value).to_bits().to_ne_bytes())
        })
        .collect();
    let transforms = [
        None,
        Some([
            [-1.0, 0.0, 0.0, 1.0],
            [0.0, -1.0, 0.0, 1.0],
            [0.0, 0.0, -1.0, 1.0],
        ]),
        Some([
            [1.15, -0.125, 0.025, 0.05],
            [0.03, 0.95, 0.02, -0.1],
            [-0.02, 0.1, 1.1, 0.025],
        ]),
    ];
    for sdr_white_nits in [80.0, 160.0, 280.0] {
        for hdr_peak_nits in [400.0, 1000.0, 4000.0] {
            for tonemap_use_lut in [false, true] {
                let mut prepared = f16::prepare_hdr_context(HdrFrameContext {
                    sdr_white_nits,
                    hdr_peak_nits,
                    tonemap_use_lut,
                    ..Default::default()
                });
                for screen_color_rows in transforms {
                    prepared.screen_color_rows = screen_color_rows;
                    let mut reference = vec![0; colors.len() * 4];
                    unsafe {
                        f16::convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked(
                            src.as_ptr(),
                            reference.as_mut_ptr(),
                            colors.len(),
                            &prepared,
                        );
                    }
                    for &(name, kernel, opaque) in &kernels {
                        let mut actual = vec![0; reference.len()];
                        unsafe {
                            kernel(src.as_ptr(), actual.as_mut_ptr(), colors.len(), &prepared)
                        };
                        for (index, (&a, &b)) in actual.iter().zip(&reference).enumerate() {
                            if index % 4 == 3 {
                                assert_eq!(a, if opaque { 255 } else { b }, "{name} alpha");
                            } else {
                                assert!(
                                    a.abs_diff(b) <= 1,
                                    "{name} at byte {index}: {a} != {b}, white={sdr_white_nits}, peak={hdr_peak_nits}, LUT={tonemap_use_lut}, rows={screen_color_rows:?}"
                                );
                            }
                        }
                    }
                }
            }
        }
    }
}

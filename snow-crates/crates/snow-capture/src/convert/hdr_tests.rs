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
    for (row, bytes) in rgba.chunks_exact(HIGHLIGHT_RAMP_WIDTH * 4).enumerate() {
        for x in 1..HIGHLIGHT_RAMP_WIDTH {
            for channel in 0..3 {
                let before = bytes[(x - 1) * 4 + channel];
                let after = bytes[x * 4 + channel];
                assert!(
                    before.abs_diff(after) <= 2 && u16::from(after) + 1 >= u16::from(before),
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

#[test]
fn hdr_highlight_gradients_are_continuous_across_cpu_kernels() {
    let kernels = cpu_hdr_kernels();
    println!(
        "HDR kernels tested: {:?}",
        kernels.iter().map(|(name, _, _)| name).collect::<Vec<_>>()
    );
    for sdr_white_nits in [80.0, 160.0, 280.0] {
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
    for sdr_white_nits in [1.0, 80.0, 280.0, f32::MAX] {
        for hdr_peak_nits in [1.0, 101.0, 400.0, 1000.0, 4000.0, 10000.0, f32::MAX] {
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

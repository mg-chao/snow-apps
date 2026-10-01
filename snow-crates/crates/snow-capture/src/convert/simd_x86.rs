use super::f16::{
    HdrPreparedContext, convert_f16_rgba_to_srgb_hdr_scalar_prepared_opaque_unchecked,
    convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked,
    convert_f16_rgba_to_srgb_scalar_opaque_unchecked, convert_f16_rgba_to_srgb_scalar_unchecked,
};
use super::scalar::{
    convert_bgra_to_rgba_scalar_opaque_unchecked, convert_bgra_to_rgba_scalar_unchecked,
};

#[inline(always)]
fn nt_prefix_pixels(dst: *mut u8, pixel_count: usize, alignment: usize) -> usize {
    if pixel_count == 0 || alignment <= 1 {
        return 0;
    }
    let misalign = (dst as usize) & (alignment - 1);
    if misalign == 0 {
        return 0;
    }
    let bytes_to_align = alignment - misalign;
    if !bytes_to_align.is_multiple_of(4) {
        return pixel_count;
    }
    (bytes_to_align / 4).min(pixel_count)
}

#[target_feature(enable = "avx512f,avx512bw")]
pub(crate) unsafe fn convert_bgra_to_rgba_avx512_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { avx512_bgra_core::<false>(src, dst, pixel_count, false, false) }
}

#[target_feature(enable = "avx512f,avx512bw")]
pub(crate) unsafe fn convert_bgra_to_rgba_avx512_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { avx512_bgra_core::<true>(src, dst, pixel_count, false, false) }
}

/// Streaming-store variant - uses non-temporal writes to bypass the cache.
/// Caller must ensure `dst` will not be read back immediately (or issue an
/// `_mm_sfence` afterwards).
#[target_feature(enable = "avx512f,avx512bw")]
pub(crate) unsafe fn convert_bgra_to_rgba_avx512_nt_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { avx512_bgra_core::<false>(src, dst, pixel_count, true, true) }
}

#[target_feature(enable = "avx512f,avx512bw")]
pub(crate) unsafe fn convert_bgra_to_rgba_avx512_nt_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { avx512_bgra_core::<true>(src, dst, pixel_count, true, true) }
}

#[target_feature(enable = "avx512f,avx512bw")]
pub(crate) unsafe fn convert_bgra_to_rgba_avx512_nt_nofence_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { avx512_bgra_core::<false>(src, dst, pixel_count, true, false) }
}

#[target_feature(enable = "avx512f,avx512bw")]
pub(crate) unsafe fn convert_bgra_to_rgba_avx512_nt_nofence_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { avx512_bgra_core::<true>(src, dst, pixel_count, true, false) }
}

#[target_feature(enable = "avx512f,avx512bw")]
unsafe fn avx512_bgra_core<const FORCE_OPAQUE_ALPHA: bool>(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    nontemporal: bool,
    fence: bool,
) {
    use std::arch::x86_64::{
        __m512i, _MM_HINT_T0, _mm_prefetch, _mm_sfence, _mm512_loadu_si512, _mm512_or_si512,
        _mm512_set1_epi32, _mm512_shuffle_epi8, _mm512_storeu_si512, _mm512_stream_si512,
    };

    let shuffle = unsafe {
        let pattern: [i8; 64] = [
            2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15, 2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8,
            11, 14, 13, 12, 15, 2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15, 2, 1, 0, 3,
            6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
        ];
        _mm512_loadu_si512(pattern.as_ptr() as *const __m512i)
    };
    let alpha_mask = if FORCE_OPAQUE_ALPHA {
        Some(_mm512_set1_epi32(i32::from_ne_bytes([0, 0, 0, u8::MAX])))
    } else {
        None
    };

    macro_rules! store512 {
        ($ptr:expr, $val:expr) => {
            if nontemporal {
                _mm512_stream_si512($ptr as *mut __m512i, $val);
            } else {
                _mm512_storeu_si512($ptr as *mut __m512i, $val);
            }
        };
    }

    let mut x = 0usize;
    if nontemporal {
        let prefix = nt_prefix_pixels(dst, pixel_count, 64);
        if prefix > 0 {
            unsafe {
                if FORCE_OPAQUE_ALPHA {
                    convert_bgra_to_rgba_scalar_opaque_unchecked(src, dst, prefix);
                } else {
                    convert_bgra_to_rgba_scalar_unchecked(src, dst, prefix);
                }
            }
            x = prefix;
            if x == pixel_count {
                return;
            }
        }
    }

    // Process 128 pixels (8x16) per iteration to better amortise loop
    // overhead and improve instruction-level parallelism.
    while x + 128 <= pixel_count {
        let offset = x * 4;
        // Prefetch source data 1 iteration ahead (512 bytes).
        if x + 256 <= pixel_count {
            unsafe {
                _mm_prefetch(src.add(offset + 512) as *const i8, _MM_HINT_T0);
                _mm_prefetch(src.add(offset + 576) as *const i8, _MM_HINT_T0);
                _mm_prefetch(src.add(offset + 640) as *const i8, _MM_HINT_T0);
                _mm_prefetch(src.add(offset + 704) as *const i8, _MM_HINT_T0);
            }
        }
        let input0 = unsafe { _mm512_loadu_si512(src.add(offset) as *const __m512i) };
        let input1 = unsafe { _mm512_loadu_si512(src.add(offset + 64) as *const __m512i) };
        let input2 = unsafe { _mm512_loadu_si512(src.add(offset + 128) as *const __m512i) };
        let input3 = unsafe { _mm512_loadu_si512(src.add(offset + 192) as *const __m512i) };
        let input4 = unsafe { _mm512_loadu_si512(src.add(offset + 256) as *const __m512i) };
        let input5 = unsafe { _mm512_loadu_si512(src.add(offset + 320) as *const __m512i) };
        let input6 = unsafe { _mm512_loadu_si512(src.add(offset + 384) as *const __m512i) };
        let input7 = unsafe { _mm512_loadu_si512(src.add(offset + 448) as *const __m512i) };
        let mut output0 = _mm512_shuffle_epi8(input0, shuffle);
        let mut output1 = _mm512_shuffle_epi8(input1, shuffle);
        let mut output2 = _mm512_shuffle_epi8(input2, shuffle);
        let mut output3 = _mm512_shuffle_epi8(input3, shuffle);
        let mut output4 = _mm512_shuffle_epi8(input4, shuffle);
        let mut output5 = _mm512_shuffle_epi8(input5, shuffle);
        let mut output6 = _mm512_shuffle_epi8(input6, shuffle);
        let mut output7 = _mm512_shuffle_epi8(input7, shuffle);
        if let Some(mask) = alpha_mask {
            output0 = _mm512_or_si512(output0, mask);
            output1 = _mm512_or_si512(output1, mask);
            output2 = _mm512_or_si512(output2, mask);
            output3 = _mm512_or_si512(output3, mask);
            output4 = _mm512_or_si512(output4, mask);
            output5 = _mm512_or_si512(output5, mask);
            output6 = _mm512_or_si512(output6, mask);
            output7 = _mm512_or_si512(output7, mask);
        }
        unsafe {
            store512!(dst.add(offset), output0);
            store512!(dst.add(offset + 64), output1);
            store512!(dst.add(offset + 128), output2);
            store512!(dst.add(offset + 192), output3);
            store512!(dst.add(offset + 256), output4);
            store512!(dst.add(offset + 320), output5);
            store512!(dst.add(offset + 384), output6);
            store512!(dst.add(offset + 448), output7);
        }
        x += 128;
    }

    while x + 16 <= pixel_count {
        let offset = x * 4;
        let input = unsafe { _mm512_loadu_si512(src.add(offset) as *const __m512i) };
        let mut output = _mm512_shuffle_epi8(input, shuffle);
        if let Some(mask) = alpha_mask {
            output = _mm512_or_si512(output, mask);
        }
        unsafe {
            store512!(dst.add(offset), output);
        }
        x += 16;
    }

    if nontemporal && fence {
        _mm_sfence();
    }

    if x < pixel_count {
        unsafe {
            if FORCE_OPAQUE_ALPHA {
                convert_bgra_to_rgba_scalar_opaque_unchecked(
                    src.add(x * 4),
                    dst.add(x * 4),
                    pixel_count - x,
                );
            } else {
                convert_bgra_to_rgba_scalar_unchecked(
                    src.add(x * 4),
                    dst.add(x * 4),
                    pixel_count - x,
                );
            }
        }
    }
}

#[target_feature(enable = "avx2")]
pub(crate) unsafe fn convert_bgra_to_rgba_avx2_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { avx2_bgra_core::<false>(src, dst, pixel_count, false, false) }
}

#[target_feature(enable = "avx2")]
pub(crate) unsafe fn convert_bgra_to_rgba_avx2_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { avx2_bgra_core::<true>(src, dst, pixel_count, false, false) }
}

/// Streaming-store variant for AVX2.
#[target_feature(enable = "avx2")]
pub(crate) unsafe fn convert_bgra_to_rgba_avx2_nt_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { avx2_bgra_core::<false>(src, dst, pixel_count, true, true) }
}

#[target_feature(enable = "avx2")]
pub(crate) unsafe fn convert_bgra_to_rgba_avx2_nt_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { avx2_bgra_core::<true>(src, dst, pixel_count, true, true) }
}

#[target_feature(enable = "avx2")]
pub(crate) unsafe fn convert_bgra_to_rgba_avx2_nt_nofence_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { avx2_bgra_core::<false>(src, dst, pixel_count, true, false) }
}

#[target_feature(enable = "avx2")]
pub(crate) unsafe fn convert_bgra_to_rgba_avx2_nt_nofence_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { avx2_bgra_core::<true>(src, dst, pixel_count, true, false) }
}

#[target_feature(enable = "avx2")]
unsafe fn avx2_bgra_core<const FORCE_OPAQUE_ALPHA: bool>(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    nontemporal: bool,
    fence: bool,
) {
    use std::arch::x86_64::{
        __m256i, _MM_HINT_T0, _mm_prefetch, _mm_sfence, _mm256_loadu_si256, _mm256_or_si256,
        _mm256_set1_epi32, _mm256_setr_epi8, _mm256_shuffle_epi8, _mm256_storeu_si256,
        _mm256_stream_si256,
    };

    let shuffle = _mm256_setr_epi8(
        2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15, 2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11,
        14, 13, 12, 15,
    );
    let alpha_mask = if FORCE_OPAQUE_ALPHA {
        Some(_mm256_set1_epi32(i32::from_ne_bytes([0, 0, 0, u8::MAX])))
    } else {
        None
    };

    macro_rules! store256 {
        ($ptr:expr, $val:expr) => {
            if nontemporal {
                _mm256_stream_si256($ptr as *mut __m256i, $val);
            } else {
                _mm256_storeu_si256($ptr as *mut __m256i, $val);
            }
        };
    }

    let mut x = 0usize;
    if nontemporal {
        let prefix = nt_prefix_pixels(dst, pixel_count, 32);
        if prefix > 0 {
            unsafe {
                if FORCE_OPAQUE_ALPHA {
                    convert_bgra_to_rgba_scalar_opaque_unchecked(src, dst, prefix);
                } else {
                    convert_bgra_to_rgba_scalar_unchecked(src, dst, prefix);
                }
            }
            x = prefix;
            if x == pixel_count {
                return;
            }
        }
    }
    while x + 32 <= pixel_count {
        let offset = x * 4;
        // Prefetch source data ~4 iterations ahead (512 bytes) to hide
        // memory latency on modern out-of-order cores.
        if x + 128 <= pixel_count {
            unsafe {
                _mm_prefetch(src.add(offset + 256) as *const i8, _MM_HINT_T0);
                _mm_prefetch(src.add(offset + 320) as *const i8, _MM_HINT_T0);
                _mm_prefetch(src.add(offset + 384) as *const i8, _MM_HINT_T0);
                _mm_prefetch(src.add(offset + 448) as *const i8, _MM_HINT_T0);
            }
        }
        let input0 = unsafe { _mm256_loadu_si256(src.add(offset) as *const __m256i) };
        let input1 = unsafe { _mm256_loadu_si256(src.add(offset + 32) as *const __m256i) };
        let input2 = unsafe { _mm256_loadu_si256(src.add(offset + 64) as *const __m256i) };
        let input3 = unsafe { _mm256_loadu_si256(src.add(offset + 96) as *const __m256i) };
        let mut output0 = _mm256_shuffle_epi8(input0, shuffle);
        let mut output1 = _mm256_shuffle_epi8(input1, shuffle);
        let mut output2 = _mm256_shuffle_epi8(input2, shuffle);
        let mut output3 = _mm256_shuffle_epi8(input3, shuffle);
        if let Some(mask) = alpha_mask {
            output0 = _mm256_or_si256(output0, mask);
            output1 = _mm256_or_si256(output1, mask);
            output2 = _mm256_or_si256(output2, mask);
            output3 = _mm256_or_si256(output3, mask);
        }
        unsafe {
            store256!(dst.add(offset), output0);
            store256!(dst.add(offset + 32), output1);
            store256!(dst.add(offset + 64), output2);
            store256!(dst.add(offset + 96), output3);
        }
        x += 32;
    }

    while x + 8 <= pixel_count {
        let offset = x * 4;
        let input = unsafe { _mm256_loadu_si256(src.add(offset) as *const __m256i) };
        let mut output = _mm256_shuffle_epi8(input, shuffle);
        if let Some(mask) = alpha_mask {
            output = _mm256_or_si256(output, mask);
        }
        unsafe {
            store256!(dst.add(offset), output);
        }
        x += 8;
    }

    if nontemporal && fence {
        _mm_sfence();
    }

    if x < pixel_count {
        unsafe {
            if FORCE_OPAQUE_ALPHA {
                convert_bgra_to_rgba_scalar_opaque_unchecked(
                    src.add(x * 4),
                    dst.add(x * 4),
                    pixel_count - x,
                );
            } else {
                convert_bgra_to_rgba_scalar_unchecked(
                    src.add(x * 4),
                    dst.add(x * 4),
                    pixel_count - x,
                );
            }
        }
    }
}

#[target_feature(enable = "ssse3")]
pub(crate) unsafe fn convert_bgra_to_rgba_ssse3_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { ssse3_bgra_core::<false>(src, dst, pixel_count, false, false) }
}

#[target_feature(enable = "ssse3")]
pub(crate) unsafe fn convert_bgra_to_rgba_ssse3_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { ssse3_bgra_core::<true>(src, dst, pixel_count, false, false) }
}

/// Streaming-store variant for SSSE3.
#[target_feature(enable = "ssse3")]
pub(crate) unsafe fn convert_bgra_to_rgba_ssse3_nt_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { ssse3_bgra_core::<false>(src, dst, pixel_count, true, true) }
}

#[target_feature(enable = "ssse3")]
pub(crate) unsafe fn convert_bgra_to_rgba_ssse3_nt_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { ssse3_bgra_core::<true>(src, dst, pixel_count, true, true) }
}

#[target_feature(enable = "ssse3")]
pub(crate) unsafe fn convert_bgra_to_rgba_ssse3_nt_nofence_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { ssse3_bgra_core::<false>(src, dst, pixel_count, true, false) }
}

#[target_feature(enable = "ssse3")]
pub(crate) unsafe fn convert_bgra_to_rgba_ssse3_nt_nofence_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { ssse3_bgra_core::<true>(src, dst, pixel_count, true, false) }
}

#[target_feature(enable = "ssse3")]
unsafe fn ssse3_bgra_core<const FORCE_OPAQUE_ALPHA: bool>(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    nontemporal: bool,
    fence: bool,
) {
    use std::arch::x86_64::{
        __m128i, _mm_loadu_si128, _mm_or_si128, _mm_set1_epi32, _mm_setr_epi8, _mm_sfence,
        _mm_shuffle_epi8, _mm_storeu_si128, _mm_stream_si128,
    };

    let shuffle = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
    let alpha_mask = if FORCE_OPAQUE_ALPHA {
        Some(_mm_set1_epi32(i32::from_ne_bytes([0, 0, 0, u8::MAX])))
    } else {
        None
    };

    macro_rules! store128 {
        ($ptr:expr, $val:expr) => {
            if nontemporal {
                _mm_stream_si128($ptr as *mut __m128i, $val);
            } else {
                _mm_storeu_si128($ptr as *mut __m128i, $val);
            }
        };
    }

    let mut x = 0usize;
    if nontemporal {
        let prefix = nt_prefix_pixels(dst, pixel_count, 16);
        if prefix > 0 {
            unsafe {
                if FORCE_OPAQUE_ALPHA {
                    convert_bgra_to_rgba_scalar_opaque_unchecked(src, dst, prefix);
                } else {
                    convert_bgra_to_rgba_scalar_unchecked(src, dst, prefix);
                }
            }
            x = prefix;
            if x == pixel_count {
                return;
            }
        }
    }
    while x + 16 <= pixel_count {
        let offset = x * 4;
        let input0 = unsafe { _mm_loadu_si128(src.add(offset) as *const __m128i) };
        let input1 = unsafe { _mm_loadu_si128(src.add(offset + 16) as *const __m128i) };
        let input2 = unsafe { _mm_loadu_si128(src.add(offset + 32) as *const __m128i) };
        let input3 = unsafe { _mm_loadu_si128(src.add(offset + 48) as *const __m128i) };
        let mut output0 = _mm_shuffle_epi8(input0, shuffle);
        let mut output1 = _mm_shuffle_epi8(input1, shuffle);
        let mut output2 = _mm_shuffle_epi8(input2, shuffle);
        let mut output3 = _mm_shuffle_epi8(input3, shuffle);
        if let Some(mask) = alpha_mask {
            output0 = _mm_or_si128(output0, mask);
            output1 = _mm_or_si128(output1, mask);
            output2 = _mm_or_si128(output2, mask);
            output3 = _mm_or_si128(output3, mask);
        }
        unsafe {
            store128!(dst.add(offset), output0);
            store128!(dst.add(offset + 16), output1);
            store128!(dst.add(offset + 32), output2);
            store128!(dst.add(offset + 48), output3);
        }
        x += 16;
    }

    while x + 4 <= pixel_count {
        let offset = x * 4;
        let input = unsafe { _mm_loadu_si128(src.add(offset) as *const __m128i) };
        let mut output = _mm_shuffle_epi8(input, shuffle);
        if let Some(mask) = alpha_mask {
            output = _mm_or_si128(output, mask);
        }
        unsafe {
            store128!(dst.add(offset), output);
        }
        x += 4;
    }

    if nontemporal && fence {
        _mm_sfence();
    }

    if x < pixel_count {
        unsafe {
            if FORCE_OPAQUE_ALPHA {
                convert_bgra_to_rgba_scalar_opaque_unchecked(
                    src.add(x * 4),
                    dst.add(x * 4),
                    pixel_count - x,
                );
            } else {
                convert_bgra_to_rgba_scalar_unchecked(
                    src.add(x * 4),
                    dst.add(x * 4),
                    pixel_count - x,
                );
            }
        }
    }
}

// F16->sRGB via AVX2 + F16C
// Uses `vcvtph2ps` (F16C) to convert half-floats to f32, then applies
// a SIMD sRGB approximation and preserves the source alpha channel.
// The sRGB transfer function (IEC 61966-2-1:1999, Section 4.7) is:
//   srgb(x) = 1.055 * x^(1/2.4) - 0.055   for x > 0.0031308
//   srgb(x) = 12.92 * x                     for x <= 0.0031308
// We approximate x^(1/2.4) via the classic "fast-pow" IEEE 754 bit trick:
//   reinterpret_as_int(x^p) ~= p * reinterpret_as_int(x) + 0x3F800000 * (1 - p)
// This exploits the fact that the integer representation of an IEEE 754
// float is roughly proportional to its log2.  The technique is described
// in:
//   - Schraudolph, N. N. (1999). "A Fast, Compact Approximation of the
//     Exponential Function." Neural Computation, 11(4), 853-62.
// This is a speed/accuracy tradeoff; exact error depends on the input
// distribution and platform math behavior.

#[target_feature(enable = "avx2,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_f16c_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_f16_rgba_to_srgb_f16c_inner::<false>(src, dst, pixel_count, false, false);
    }
}

#[target_feature(enable = "avx2,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_f16c_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_f16_rgba_to_srgb_f16c_inner::<true>(src, dst, pixel_count, false, false);
    }
}

/// Non-temporal store variant - uses streaming writes to bypass the cache.
#[target_feature(enable = "avx2,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_f16c_nt_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_f16_rgba_to_srgb_f16c_inner::<false>(src, dst, pixel_count, true, true);
    }
}

#[target_feature(enable = "avx2,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_f16c_nt_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_f16_rgba_to_srgb_f16c_inner::<true>(src, dst, pixel_count, true, true);
    }
}

#[target_feature(enable = "avx2,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_f16c_nt_nofence_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_f16_rgba_to_srgb_f16c_inner::<false>(src, dst, pixel_count, true, false);
    }
}

#[target_feature(enable = "avx2,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_f16c_nt_nofence_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_f16_rgba_to_srgb_f16c_inner::<true>(src, dst, pixel_count, true, false);
    }
}

#[target_feature(enable = "avx2,f16c")]
unsafe fn convert_f16_rgba_to_srgb_f16c_inner<const FORCE_OPAQUE_ALPHA: bool>(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    nontemporal: bool,
    fence: bool,
) {
    use std::arch::x86_64::*;

    unsafe {
        let threshold = _mm256_set1_ps(0.003_130_8_f32);
        let linear_scale = _mm256_set1_ps(12.92);
        let a = _mm256_set1_ps(1.055);
        let b = _mm256_set1_ps(-0.055);
        let zero = _mm256_setzero_ps();
        let one = _mm256_set1_ps(1.0);
        let scale255 = _mm256_set1_ps(255.0);
        let half = _mm256_set1_ps(0.5);

        //   as_int(x^p) ~ p * as_int(x) + 0x3F800000 * (1 - p)
        let pow_scale = _mm256_set1_ps(HDR_SRGB_GAMMA_EXP);
        let pow_bias_f = 0x3F80_0000u32 as f32;
        let pow_offset = _mm256_set1_ps(pow_bias_f * (1.0 - HDR_SRGB_GAMMA_EXP));
        let pow_offset_i = _mm256_cvtps_epi32(pow_offset);

        let alpha_scale = _mm256_set1_ps(255.0);
        let alpha_half = _mm256_set1_ps(0.5);
        let alpha_opaque = _mm256_set1_epi32(i32::from_ne_bytes([0, 0, 0, u8::MAX]));

        // Permutation index for AoS->SoA transpose final step
        let perm = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);

        let mut src_ptr = src as *const u16;
        let mut dst_ptr = dst;
        let mut remaining = pixel_count;

        macro_rules! srgb_gamma_ps {
            ($v:expr) => {{
                let clamped = _mm256_min_ps(_mm256_max_ps($v, zero), one);

                let lin = _mm256_mul_ps(clamped, linear_scale);

                let xi = _mm256_castps_si256(clamped);
                let pow_i = _mm256_add_epi32(
                    _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_cvtepi32_ps(xi), pow_scale)),
                    pow_offset_i,
                );
                let pow_approx = _mm256_castsi256_ps(pow_i);
                let gamma = _mm256_add_ps(_mm256_mul_ps(a, pow_approx), b);

                // Select: linear for small values, gamma for the rest
                let mask = _mm256_cmp_ps(clamped, threshold, _CMP_GT_OQ);
                let result = _mm256_blendv_ps(lin, gamma, mask);

                _mm256_add_ps(_mm256_mul_ps(result, scale255), half)
            }};
        }

        while remaining >= 8 {
            // Prefetch source data for the next iteration (64 bytes ahead)
            if remaining >= 16 {
                _mm_prefetch(src_ptr.add(32) as *const i8, _MM_HINT_T0);
            }

            // Load 32 half-floats as 4 groups of 8, convert to f32
            let h0 = _mm_loadu_si128(src_ptr as *const __m128i);
            let h1 = _mm_loadu_si128(src_ptr.add(8) as *const __m128i);
            let h2 = _mm_loadu_si128(src_ptr.add(16) as *const __m128i);
            let h3 = _mm_loadu_si128(src_ptr.add(24) as *const __m128i);

            // f0 = [R0 G0 B0 A0 | R1 G1 B1 A1]  (AoS layout, 128-bit lanes)
            let f0 = _mm256_cvtph_ps(h0);
            let f1 = _mm256_cvtph_ps(h1);
            let f2 = _mm256_cvtph_ps(h2);
            let f3 = _mm256_cvtph_ps(h3);

            // Transpose AoS -> SoA to get 8-wide R, G, B vectors.
            // Step 1: interleave within 128-bit lanes
            let t0 = _mm256_unpacklo_ps(f0, f1); // [R0 R2 G0 G2 | R1 R3 G1 G3]
            let t1 = _mm256_unpackhi_ps(f0, f1); // [B0 B2 A0 A2 | B1 B3 A1 A3]
            let t2 = _mm256_unpacklo_ps(f2, f3); // [R4 R6 G4 G6 | R5 R7 G5 G7]
            let t3 = _mm256_unpackhi_ps(f2, f3); // [B4 B6 A4 A6 | B5 B7 A5 A7]

            // Step 2: shuffle to collect channel pairs
            let rr = _mm256_shuffle_ps(t0, t2, 0b01_00_01_00);
            let gg = _mm256_shuffle_ps(t0, t2, 0b11_10_11_10);
            let bb = _mm256_shuffle_ps(t1, t3, 0b01_00_01_00);
            let aa = _mm256_shuffle_ps(t1, t3, 0b11_10_11_10);

            // Step 3: final permute to sequential order
            let r_vals = _mm256_permutevar8x32_ps(rr, perm);
            let g_vals = _mm256_permutevar8x32_ps(gg, perm);
            let b_vals = _mm256_permutevar8x32_ps(bb, perm);
            let a_vals = _mm256_permutevar8x32_ps(aa, perm);

            let r_srgb = srgb_gamma_ps!(r_vals);
            let g_srgb = srgb_gamma_ps!(g_vals);
            let b_srgb = srgb_gamma_ps!(b_vals);

            // Convert to i32 and pack: pixel = R | (G << 8) | (B << 16) | (A << 24)
            let r_i = _mm256_cvttps_epi32(r_srgb);
            let g_i = _mm256_cvttps_epi32(g_srgb);
            let b_i = _mm256_cvttps_epi32(b_srgb);

            let g_shifted = _mm256_slli_epi32(g_i, 8);
            let b_shifted = _mm256_slli_epi32(b_i, 16);
            let rgba_rgb = _mm256_or_si256(_mm256_or_si256(r_i, g_shifted), b_shifted);
            let rgba = if FORCE_OPAQUE_ALPHA {
                _mm256_or_si256(rgba_rgb, alpha_opaque)
            } else {
                let a_clamped = _mm256_min_ps(_mm256_max_ps(a_vals, zero), one);
                let a_srgb = _mm256_add_ps(_mm256_mul_ps(a_clamped, alpha_scale), alpha_half);
                let a_i = _mm256_cvttps_epi32(a_srgb);
                let a_shifted = _mm256_slli_epi32(a_i, 24);
                _mm256_or_si256(rgba_rgb, a_shifted)
            };

            if nontemporal {
                _mm256_stream_si256(dst_ptr as *mut __m256i, rgba);
            } else {
                _mm256_storeu_si256(dst_ptr as *mut __m256i, rgba);
            }

            src_ptr = src_ptr.add(32); // 8 pixels * 4 channels
            dst_ptr = dst_ptr.add(32); // 8 pixels * 4 bytes
            remaining -= 8;
        }

        if nontemporal && fence {
            _mm_sfence();
        }

        if remaining > 0 {
            if FORCE_OPAQUE_ALPHA {
                convert_f16_rgba_to_srgb_scalar_opaque_unchecked(
                    src_ptr as *const u8,
                    dst_ptr,
                    remaining,
                );
            } else {
                convert_f16_rgba_to_srgb_scalar_unchecked(src_ptr as *const u8, dst_ptr, remaining);
            }
        }
    } // unsafe
}

const HDR_PQ_M1: f32 = 0.159_301_76;
const HDR_PQ_M2: f32 = 78.843_75;
const HDR_PQ_INV_M1: f32 = 1.0 / HDR_PQ_M1;
const HDR_PQ_INV_M2: f32 = 1.0 / HDR_PQ_M2;
const HDR_PQ_C1: f32 = 0.835_937_5;
const HDR_PQ_C2: f32 = 18.851_563;
const HDR_PQ_C3: f32 = 18.687_5;
const HDR_PQ_ABSOLUTE_NITS: f32 = 10_000.0;
const HDR_SDR_REFERENCE_WHITE_NITS: f32 = 80.0;
const HDR_INPUT_BLACK_NITS: f32 = 0.001;
const HDR_EPSILON: f32 = 1e-6;
const HDR_SRGB_GAMMA_EXP: f32 = 1.0 / 2.4;
// 32px unrolling improves AVX-512 throughput on large frames but can hurt
// smaller surfaces due to extra front-end/register pressure.
const AVX512_UNROLL_MIN_PIXELS: usize = 3_000_000;

#[target_feature(enable = "fma")]
unsafe fn fmadd_ps(
    a: std::arch::x86_64::__m256,
    b: std::arch::x86_64::__m256,
    c: std::arch::x86_64::__m256,
) -> std::arch::x86_64::__m256 {
    use std::arch::x86_64::*;
    _mm256_fmadd_ps(a, b, c)
}

// Evaluate the four PQ powers without leaving SIMD. PQ supplies positive normal
// arguments (the smallest forward input is 0.001/10000) or zero after inverse
// black subtraction; finite results stay in the normal f32 range. This is not a
// general-purpose powf implementation for subnormal arguments/results.
//
// Reduce the mantissa to [1/sqrt(2), sqrt(2)] so |z| <= 0.172 in
// ln(m) = 2*z*(1 + z^2/3 + ... + z^8/9), z = (m-1)/(m+1). Reduce exp's
// argument to [-ln(2)/2, ln(2)/2] for its seventh-degree Taylor polynomial.
// Range reduction bounds truncation error before f32 rounding; HDR kernel tests
// enforce the final <=1 encoded RGB byte budget against the scalar curve.
#[target_feature(enable = "avx2")]
unsafe fn hdr_pow_ps(value: std::arch::x86_64::__m256, exponent: f32) -> std::arch::x86_64::__m256 {
    use std::arch::x86_64::*;
    let one = _mm256_set1_ps(1.0);
    let x = _mm256_min_ps(
        _mm256_max_ps(value, _mm256_set1_ps(f32::MIN_POSITIVE)),
        _mm256_set1_ps(f32::MAX),
    );
    let bits = _mm256_castps_si256(x);
    let e = _mm256_sub_epi32(_mm256_srli_epi32(bits, 23), _mm256_set1_epi32(127));
    let m = _mm256_castsi256_ps(_mm256_or_si256(
        _mm256_and_si256(bits, _mm256_set1_epi32(0x7fffff)),
        _mm256_set1_epi32(0x3f800000),
    ));
    let adjust = _mm256_cmp_ps(m, _mm256_set1_ps(std::f32::consts::SQRT_2), _CMP_GT_OQ);
    let m = _mm256_blendv_ps(m, _mm256_mul_ps(m, _mm256_set1_ps(0.5)), adjust);
    let e = _mm256_cvtepi32_ps(_mm256_add_epi32(
        e,
        _mm256_and_si256(_mm256_castps_si256(adjust), _mm256_set1_epi32(1)),
    ));
    let z = _mm256_div_ps(_mm256_sub_ps(m, one), _mm256_add_ps(m, one));
    let z2 = _mm256_mul_ps(z, z);
    let mut poly = _mm256_set1_ps(1.0 / 9.0);
    for coefficient in [1.0 / 7.0, 1.0 / 5.0, 1.0 / 3.0, 1.0] {
        poly = _mm256_add_ps(_mm256_mul_ps(poly, z2), _mm256_set1_ps(coefficient));
    }
    let log2 = _mm256_add_ps(
        e,
        _mm256_mul_ps(
            _mm256_mul_ps(z, poly),
            _mm256_set1_ps(2.0 * std::f32::consts::LOG2_E),
        ),
    );
    let power = _mm256_max_ps(
        _mm256_min_ps(
            _mm256_mul_ps(log2, _mm256_set1_ps(exponent)),
            _mm256_set1_ps(127.0),
        ),
        _mm256_set1_ps(-126.0),
    );
    let integer = _mm256_round_ps::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(power);
    let fraction = _mm256_mul_ps(
        _mm256_sub_ps(power, integer),
        _mm256_set1_ps(std::f32::consts::LN_2),
    );
    let mut exp = _mm256_set1_ps(1.0 / 5040.0);
    for coefficient in [
        1.0 / 720.0,
        1.0 / 120.0,
        1.0 / 24.0,
        1.0 / 6.0,
        0.5,
        1.0,
        1.0,
    ] {
        exp = _mm256_add_ps(_mm256_mul_ps(exp, fraction), _mm256_set1_ps(coefficient));
    }
    let scale = _mm256_castsi256_ps(_mm256_slli_epi32(
        _mm256_add_epi32(_mm256_cvttps_epi32(integer), _mm256_set1_epi32(127)),
        23,
    ));
    let result = _mm256_mul_ps(exp, scale);
    // Preserve infinity through forward PQ. Nonpositive/NaN arguments become
    // zero, matching the scalar max(0).powf behavior for these positive powers.
    let infinity = _mm256_set1_ps(f32::INFINITY);
    let result = _mm256_blendv_ps(result, infinity, _mm256_cmp_ps(value, infinity, _CMP_EQ_OQ));
    _mm256_and_ps(
        result,
        _mm256_cmp_ps(value, _mm256_setzero_ps(), _CMP_GT_OQ),
    )
}

#[target_feature(enable = "avx2,f16c")]
unsafe fn hdr_srgb_bytes_ps(
    value: std::arch::x86_64::__m256,
    lut: *const i32,
) -> std::arch::x86_64::__m256i {
    use std::arch::x86_64::*;
    unsafe {
        let clamped = _mm256_min_ps(
            _mm256_max_ps(value, _mm256_setzero_ps()),
            _mm256_set1_ps(1.0),
        );
        // Half rounding contributes at most one encoded byte of error, including
        // dark values. Bit-pattern power approximations are much less accurate.
        let half = _mm256_cvtps_ph::<_MM_FROUND_TO_NEAREST_INT>(clamped);
        let indices = _mm256_cvtepu16_epi32(half);
        let words = _mm256_i32gather_epi32(lut, _mm256_srli_epi32(indices, 2), 4);
        let shifts = _mm256_slli_epi32(_mm256_and_si256(indices, _mm256_set1_epi32(3)), 3);
        _mm256_and_si256(_mm256_srlv_epi32(words, shifts), _mm256_set1_epi32(255))
    }
}

#[target_feature(enable = "avx2")]
unsafe fn scale_hdr_rgb_ps(
    rgb: [std::arch::x86_64::__m256; 3],
    y_in: std::arch::x86_64::__m256,
    y_out: std::arch::x86_64::__m256,
) -> [std::arch::x86_64::__m256; 3] {
    use std::arch::x86_64::*;
    let one = _mm256_set1_ps(1.0);
    let eps = _mm256_set1_ps(HDR_EPSILON);
    let max_rgb = _mm256_max_ps(rgb[0], _mm256_max_ps(rgb[1], rgb[2]));
    // Combine exposure y_out/y_in and gamut compression 1/max_rgb into one
    // division. Luma is <= max_rgb, so y_out >= 1 always needs compression;
    // cap it at 1 first to avoid overflowing max_rgb*y_out for large inputs.
    let y_out = _mm256_min_ps(y_out, one);
    let denom = _mm256_max_ps(_mm256_max_ps(y_in, eps), _mm256_mul_ps(max_rgb, y_out));
    let scale = _mm256_div_ps(y_out, denom);
    let scale = _mm256_andnot_ps(_mm256_cmp_ps(y_in, eps, _CMP_LE_OQ), scale);
    let t = _mm256_min_ps(
        _mm256_max_ps(
            _mm256_mul_ps(
                _mm256_sub_ps(
                    max_rgb,
                    _mm256_set1_ps(super::f16::HDR_SDR_TRANSITION_START),
                ),
                _mm256_set1_ps(super::f16::HDR_SDR_TRANSITION_INV_WIDTH),
            ),
            _mm256_setzero_ps(),
        ),
        one,
    );
    let weight = _mm256_mul_ps(
        _mm256_mul_ps(t, t),
        _mm256_sub_ps(_mm256_set1_ps(3.0), _mm256_mul_ps(_mm256_set1_ps(2.0), t)),
    );
    rgb.map(|channel| {
        let hdr = _mm256_mul_ps(channel, scale);
        let blended = _mm256_add_ps(channel, _mm256_mul_ps(_mm256_sub_ps(hdr, channel), weight));
        let blended = _mm256_blendv_ps(
            blended,
            channel,
            _mm256_cmp_ps(t, _mm256_setzero_ps(), _CMP_LE_OQ),
        );
        _mm256_blendv_ps(blended, hdr, _mm256_cmp_ps(t, one, _CMP_GE_OQ))
    })
}

// F16 HDR->sRGB via AVX2 + F16C.
// This path keeps the entire per-pixel hot loop in SIMD (except scalar tail),
// including inverse boost, the smooth SDR/BT.2390 handoff, and sRGB encode.

#[target_feature(enable = "avx2,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_hdr_f16c_prepared_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    // Specialize by LUT usage so the hot loop doesn't carry a runtime branch.
    if prepared.use_lut() {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_f16c_inner::<true, false, false>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    } else {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_f16c_inner::<false, false, false>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    }
}

#[target_feature(enable = "avx2,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_hdr_f16c_prepared_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    if prepared.use_lut() {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_f16c_inner::<true, false, true>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    } else {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_f16c_inner::<false, false, true>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    }
}

#[target_feature(enable = "avx2,f16c,fma")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_hdr_f16c_fma_prepared_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    // Specialize by LUT usage so the hot loop doesn't carry a runtime branch.
    if prepared.use_lut() {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_f16c_inner::<true, true, false>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    } else {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_f16c_inner::<false, true, false>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    }
}

#[target_feature(enable = "avx2,f16c,fma")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_hdr_f16c_fma_prepared_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    if prepared.use_lut() {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_f16c_inner::<true, true, true>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    } else {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_f16c_inner::<false, true, true>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    }
}

#[target_feature(enable = "avx2")]
unsafe fn restore_hdr_screen_colors(
    rgb: [std::arch::x86_64::__m256; 3],
    rows: Option<[[f32; 4]; 3]>,
) -> [std::arch::x86_64::__m256; 3] {
    use std::arch::x86_64::*;
    let restored = if let Some(rows) = rows {
        rows.map(|row| {
            _mm256_add_ps(
                _mm256_add_ps(
                    _mm256_add_ps(
                        _mm256_mul_ps(_mm256_set1_ps(row[0]), rgb[0]),
                        _mm256_mul_ps(_mm256_set1_ps(row[1]), rgb[1]),
                    ),
                    _mm256_mul_ps(_mm256_set1_ps(row[2]), rgb[2]),
                ),
                _mm256_set1_ps(row[3]),
            )
        })
    } else {
        rgb
    };
    restored.map(|channel| _mm256_max_ps(channel, _mm256_setzero_ps()))
}

#[target_feature(enable = "avx2,f16c")]
unsafe fn convert_f16_rgba_to_srgb_hdr_f16c_inner<
    const USE_LUT: bool,
    const USE_FMA: bool,
    const FORCE_OPAQUE_ALPHA: bool,
>(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    use std::arch::x86_64::*;

    unsafe {
        let (lut_ptr, lut_input_max, lut_inv_step) = if USE_LUT {
            let lut = prepared
                .luma_lut
                .as_ref()
                .expect("LUT-specialized HDR kernel requires prepared LUT context");
            (lut.values_ptr(), lut.input_max(), lut.inv_step())
        } else {
            (std::ptr::null(), 0.0, 0.0)
        };
        let inv_boost = prepared.inv_boost;
        let ow = prepared.curve.ow;
        let ob = prepared.curve.ob;
        let ib = prepared.curve.ib;
        let bt_denom = prepared.curve.denom;
        let bt_min_lum = prepared.curve.min_lum;
        let bt_max_lum = prepared.curve.max_lum;
        let bt_ks = prepared.curve.ks;

        let v_zero = _mm256_setzero_ps();
        let v_one = _mm256_set1_ps(1.0);
        let v_two = _mm256_set1_ps(2.0);
        let v_three = _mm256_set1_ps(3.0);
        let v_half = _mm256_set1_ps(0.5);
        let v_255 = _mm256_set1_ps(255.0);
        let v_inv_boost = _mm256_set1_ps(inv_boost);
        let v_eps = _mm256_set1_ps(HDR_EPSILON);
        let v_pq_abs_inv = _mm256_set1_ps(1.0 / HDR_PQ_ABSOLUTE_NITS);
        let v_pq_abs = _mm256_set1_ps(HDR_PQ_ABSOLUTE_NITS);
        let v_c1 = _mm256_set1_ps(HDR_PQ_C1);
        let v_c2 = _mm256_set1_ps(HDR_PQ_C2);
        let v_c3 = _mm256_set1_ps(HDR_PQ_C3);
        let v_bt_ib = _mm256_set1_ps(ib);
        let v_bt_ob = _mm256_set1_ps(ob);
        let v_bt_ow = _mm256_set1_ps(ow);
        let v_bt_denom = _mm256_set1_ps(bt_denom);
        let v_bt_ks = _mm256_set1_ps(bt_ks);
        let v_bt_min_lum = _mm256_set1_ps(bt_min_lum);
        let v_bt_max_lum = _mm256_set1_ps(bt_max_lum);
        let v_bt_one_minus_ks = _mm256_sub_ps(v_one, v_bt_ks);
        let v_bt_one_minus_ks_safe = _mm256_max_ps(v_bt_one_minus_ks, v_eps);
        let v_neg_two = _mm256_set1_ps(-2.0);
        let v_hdr_input_black = _mm256_set1_ps(HDR_INPUT_BLACK_NITS);
        let v_sdr_ref_white = _mm256_set1_ps(HDR_SDR_REFERENCE_WHITE_NITS);

        let v_luma_r = _mm256_set1_ps(0.2126);
        let v_luma_g = _mm256_set1_ps(0.7152);
        let v_luma_b = _mm256_set1_ps(0.0722);
        let v_lut_input_max = _mm256_set1_ps(lut_input_max);
        let v_lut_inv_step = _mm256_set1_ps(lut_inv_step);
        let v_lut_last_idx = _mm256_set1_epi32((super::f16::HDR_LUMA_LUT_SIZE - 1) as i32);
        let v_i32_one = _mm256_set1_epi32(1);

        let srgb_lut_ptr = super::f16::hdr_srgb_lut().as_ptr();
        let perm = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);

        macro_rules! mul_add_ps {
            ($a:expr, $b:expr, $c:expr) => {{
                if USE_FMA {
                    fmadd_ps($a, $b, $c)
                } else {
                    _mm256_add_ps(_mm256_mul_ps($a, $b), $c)
                }
            }};
        }

        macro_rules! nits_to_pq_ps {
            ($nits:expr) => {{
                let norm = _mm256_mul_ps(_mm256_max_ps($nits, v_zero), v_pq_abs_inv);
                let p = hdr_pow_ps(norm, HDR_PQ_M1);
                let num = mul_add_ps!(v_c2, p, v_c1);
                let den = mul_add_ps!(v_c3, p, v_one);
                let ratio = _mm256_div_ps(num, den);
                hdr_pow_ps(ratio, HDR_PQ_M2)
            }};
        }

        macro_rules! pq_to_nits_ps {
            ($v:expr) => {{
                let p = hdr_pow_ps(_mm256_max_ps($v, v_zero), HDR_PQ_INV_M2);
                let num = _mm256_max_ps(_mm256_sub_ps(p, v_c1), v_zero);
                let den = _mm256_max_ps(_mm256_sub_ps(v_c2, _mm256_mul_ps(v_c3, p)), v_eps);
                let ratio = _mm256_div_ps(num, den);
                _mm256_mul_ps(
                    hdr_pow_ps(_mm256_max_ps(ratio, v_zero), HDR_PQ_INV_M1),
                    v_pq_abs,
                )
            }};
        }

        macro_rules! pack_store_rgba_ps {
            ($r:expr, $g:expr, $b:expr, $a:expr, $dst:expr) => {{
                let r_i = hdr_srgb_bytes_ps($r, srgb_lut_ptr);
                let g_i = hdr_srgb_bytes_ps($g, srgb_lut_ptr);
                let b_i = hdr_srgb_bytes_ps($b, srgb_lut_ptr);

                let g_shifted = _mm256_slli_epi32(g_i, 8);
                let b_shifted = _mm256_slli_epi32(b_i, 16);
                let rgba_rgb = _mm256_or_si256(_mm256_or_si256(r_i, g_shifted), b_shifted);
                let rgba = if FORCE_OPAQUE_ALPHA {
                    _mm256_or_si256(
                        rgba_rgb,
                        _mm256_set1_epi32(i32::from_ne_bytes([0, 0, 0, u8::MAX])),
                    )
                } else {
                    let a_srgb = mul_add_ps!($a, v_255, v_half);
                    let a_i = _mm256_cvttps_epi32(a_srgb);
                    let a_shifted = _mm256_slli_epi32(a_i, 24);
                    _mm256_or_si256(rgba_rgb, a_shifted)
                };

                _mm256_storeu_si256($dst as *mut __m256i, rgba);
            }};
        }

        let mut src_ptr = src as *const u16;
        let mut dst_ptr = dst;
        let mut remaining = pixel_count;

        while remaining >= 8 {
            if remaining >= 16 {
                _mm_prefetch(src_ptr.add(32) as *const i8, _MM_HINT_T0);
            }

            let h0 = _mm_loadu_si128(src_ptr as *const __m128i);
            let h1 = _mm_loadu_si128(src_ptr.add(8) as *const __m128i);
            let h2 = _mm_loadu_si128(src_ptr.add(16) as *const __m128i);
            let h3 = _mm_loadu_si128(src_ptr.add(24) as *const __m128i);

            let f0 = _mm256_cvtph_ps(h0);
            let f1 = _mm256_cvtph_ps(h1);
            let f2 = _mm256_cvtph_ps(h2);
            let f3 = _mm256_cvtph_ps(h3);

            let t0 = _mm256_unpacklo_ps(f0, f1);
            let t1 = _mm256_unpackhi_ps(f0, f1);
            let t2 = _mm256_unpacklo_ps(f2, f3);
            let t3 = _mm256_unpackhi_ps(f2, f3);

            let rr = _mm256_shuffle_ps(t0, t2, 0b01_00_01_00);
            let gg = _mm256_shuffle_ps(t0, t2, 0b11_10_11_10);
            let bb = _mm256_shuffle_ps(t1, t3, 0b01_00_01_00);
            let aa = _mm256_shuffle_ps(t1, t3, 0b11_10_11_10);

            let [mut r, mut g, mut b] = restore_hdr_screen_colors(
                [
                    _mm256_permutevar8x32_ps(rr, perm),
                    _mm256_permutevar8x32_ps(gg, perm),
                    _mm256_permutevar8x32_ps(bb, perm),
                ],
                prepared.screen_color_rows,
            );
            let a = _mm256_min_ps(
                _mm256_max_ps(_mm256_permutevar8x32_ps(aa, perm), v_zero),
                v_one,
            );

            r = _mm256_mul_ps(r, v_inv_boost);
            g = _mm256_mul_ps(g, v_inv_boost);
            b = _mm256_mul_ps(b, v_inv_boost);

            let rg_luma = mul_add_ps!(v_luma_g, g, _mm256_mul_ps(r, v_luma_r));
            let y_in = mul_add_ps!(v_luma_b, b, rg_luma);
            let y_in_pos = _mm256_max_ps(y_in, v_zero);
            let y_out = if USE_LUT {
                let lut_idx_f =
                    _mm256_mul_ps(_mm256_min_ps(y_in_pos, v_lut_input_max), v_lut_inv_step);
                let lut_idx = _mm256_cvttps_epi32(lut_idx_f);
                let lut_idx_next =
                    _mm256_min_epi32(_mm256_add_epi32(lut_idx, v_i32_one), v_lut_last_idx);
                let lut_frac = _mm256_sub_ps(lut_idx_f, _mm256_cvtepi32_ps(lut_idx));
                let y0 = _mm256_i32gather_ps(lut_ptr, lut_idx, 4);
                let y1 = _mm256_i32gather_ps(lut_ptr, lut_idx_next, 4);
                _mm256_add_ps(y0, _mm256_mul_ps(lut_frac, _mm256_sub_ps(y1, y0)))
            } else {
                let l_in_nits =
                    _mm256_max_ps(_mm256_mul_ps(y_in_pos, v_sdr_ref_white), v_hdr_input_black);

                let x = nits_to_pq_ps!(l_in_nits);
                let mut y = _mm256_div_ps(_mm256_sub_ps(x, v_bt_ib), v_bt_denom);

                let tb = _mm256_div_ps(_mm256_sub_ps(y, v_bt_ks), v_bt_one_minus_ks_safe);
                let tb2 = _mm256_mul_ps(tb, tb);
                let tb3 = _mm256_mul_ps(tb2, tb);
                let poly = _mm256_add_ps(
                    _mm256_add_ps(
                        _mm256_mul_ps(
                            _mm256_add_ps(
                                _mm256_sub_ps(
                                    _mm256_mul_ps(v_two, tb3),
                                    _mm256_mul_ps(v_three, tb2),
                                ),
                                v_one,
                            ),
                            v_bt_ks,
                        ),
                        _mm256_mul_ps(
                            _mm256_add_ps(_mm256_sub_ps(tb3, _mm256_mul_ps(v_two, tb2)), tb),
                            v_bt_one_minus_ks,
                        ),
                    ),
                    _mm256_mul_ps(
                        _mm256_add_ps(_mm256_mul_ps(v_neg_two, tb3), _mm256_mul_ps(v_three, tb2)),
                        v_bt_max_lum,
                    ),
                );
                y = _mm256_blendv_ps(y, poly, _mm256_cmp_ps(y, v_bt_ks, _CMP_GE_OQ));

                let one_minus_y = _mm256_max_ps(_mm256_sub_ps(v_one, y), v_zero);
                let one_minus_y2 = _mm256_mul_ps(one_minus_y, one_minus_y);
                let one_minus_y4 = _mm256_mul_ps(one_minus_y2, one_minus_y2);
                let y_black = mul_add_ps!(v_bt_min_lum, one_minus_y4, y);
                y = _mm256_blendv_ps(y, y_black, _mm256_cmp_ps(y, v_zero, _CMP_GE_OQ));

                let mapped_pq = _mm256_min_ps(
                    _mm256_max_ps(mul_add_ps!(y, v_bt_denom, v_bt_ib), v_bt_ob),
                    v_bt_ow,
                );
                _mm256_max_ps(
                    _mm256_div_ps(pq_to_nits_ps!(mapped_pq), v_sdr_ref_white),
                    v_zero,
                )
            };

            let [r_hdr, g_hdr, b_hdr] = scale_hdr_rgb_ps([r, g, b], y_in_pos, y_out);
            pack_store_rgba_ps!(r_hdr, g_hdr, b_hdr, a, dst_ptr);

            src_ptr = src_ptr.add(32);
            dst_ptr = dst_ptr.add(32);
            remaining -= 8;
        }

        if remaining > 0 {
            if FORCE_OPAQUE_ALPHA {
                convert_f16_rgba_to_srgb_hdr_scalar_prepared_opaque_unchecked(
                    src_ptr as *const u8,
                    dst_ptr,
                    remaining,
                    prepared,
                );
            } else {
                convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked(
                    src_ptr as *const u8,
                    dst_ptr,
                    remaining,
                    prepared,
                );
            }
        }
    } // unsafe
}

// F16->sRGB via AVX-512 + F16C (16 pixels per iteration)
// Processes 16 RGBA f16 pixels at a time by splitting into two 8-wide
// batches and packing both halves into one 512-bit store.

#[target_feature(enable = "avx512f,avx512bw,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_avx512_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { convert_f16_rgba_to_srgb_avx512_inner::<false>(src, dst, pixel_count, false, false) }
}

#[target_feature(enable = "avx512f,avx512bw,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_avx512_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { convert_f16_rgba_to_srgb_avx512_inner::<true>(src, dst, pixel_count, false, false) }
}

/// Non-temporal store variant for AVX-512 F16->sRGB.
#[target_feature(enable = "avx512f,avx512bw,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_avx512_nt_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { convert_f16_rgba_to_srgb_avx512_inner::<false>(src, dst, pixel_count, true, true) }
}

#[target_feature(enable = "avx512f,avx512bw,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_avx512_nt_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { convert_f16_rgba_to_srgb_avx512_inner::<true>(src, dst, pixel_count, true, true) }
}

#[target_feature(enable = "avx512f,avx512bw,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_avx512_nt_nofence_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { convert_f16_rgba_to_srgb_avx512_inner::<false>(src, dst, pixel_count, true, false) }
}

#[target_feature(enable = "avx512f,avx512bw,f16c")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_avx512_nt_nofence_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { convert_f16_rgba_to_srgb_avx512_inner::<true>(src, dst, pixel_count, true, false) }
}

#[target_feature(enable = "avx512f,avx512bw,f16c")]
unsafe fn convert_f16_rgba_to_srgb_avx512_inner<const FORCE_OPAQUE_ALPHA: bool>(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    nontemporal: bool,
    fence: bool,
) {
    use std::arch::x86_64::*;

    unsafe {
        let threshold = _mm256_set1_ps(0.003_130_8_f32);
        let linear_scale = _mm256_set1_ps(12.92);
        let a = _mm256_set1_ps(1.055);
        let b = _mm256_set1_ps(-0.055);
        let zero = _mm256_setzero_ps();
        let one = _mm256_set1_ps(1.0);
        let scale255 = _mm256_set1_ps(255.0);
        let half = _mm256_set1_ps(0.5);

        let pow_scale = _mm256_set1_ps(HDR_SRGB_GAMMA_EXP);
        let pow_bias_f = 0x3F80_0000u32 as f32;
        let pow_offset = _mm256_set1_ps(pow_bias_f * (1.0 - HDR_SRGB_GAMMA_EXP));
        let pow_offset_i = _mm256_cvtps_epi32(pow_offset);

        let perm = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);

        macro_rules! srgb_gamma_ps {
            ($v:expr) => {{
                let clamped = _mm256_min_ps(_mm256_max_ps($v, zero), one);
                let lin = _mm256_mul_ps(clamped, linear_scale);
                let xi = _mm256_castps_si256(clamped);
                let pow_i = _mm256_add_epi32(
                    _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_cvtepi32_ps(xi), pow_scale)),
                    pow_offset_i,
                );
                let pow_approx = _mm256_castsi256_ps(pow_i);
                let gamma = _mm256_add_ps(_mm256_mul_ps(a, pow_approx), b);
                let mask = _mm256_cmp_ps(clamped, threshold, _CMP_GT_OQ);
                let result = _mm256_blendv_ps(lin, gamma, mask);
                _mm256_add_ps(_mm256_mul_ps(result, scale255), half)
            }};
        }

        // Process 8 RGBA f16 pixels -> 8 RGBA u8 pixels (256-bit output)
        macro_rules! convert_8px {
            ($src_ptr:expr) => {{
                let h0 = _mm_loadu_si128($src_ptr as *const __m128i);
                let h1 = _mm_loadu_si128(($src_ptr).add(8) as *const __m128i);
                let h2 = _mm_loadu_si128(($src_ptr).add(16) as *const __m128i);
                let h3 = _mm_loadu_si128(($src_ptr).add(24) as *const __m128i);

                let f0 = _mm256_cvtph_ps(h0);
                let f1 = _mm256_cvtph_ps(h1);
                let f2 = _mm256_cvtph_ps(h2);
                let f3 = _mm256_cvtph_ps(h3);

                let t0 = _mm256_unpacklo_ps(f0, f1);
                let t1 = _mm256_unpackhi_ps(f0, f1);
                let t2 = _mm256_unpacklo_ps(f2, f3);
                let t3 = _mm256_unpackhi_ps(f2, f3);

                let rr = _mm256_shuffle_ps(t0, t2, 0b01_00_01_00);
                let gg = _mm256_shuffle_ps(t0, t2, 0b11_10_11_10);
                let bb = _mm256_shuffle_ps(t1, t3, 0b01_00_01_00);
                let aa = _mm256_shuffle_ps(t1, t3, 0b11_10_11_10);

                let r_vals = _mm256_permutevar8x32_ps(rr, perm);
                let g_vals = _mm256_permutevar8x32_ps(gg, perm);
                let b_vals = _mm256_permutevar8x32_ps(bb, perm);
                let a_vals = _mm256_permutevar8x32_ps(aa, perm);

                let r_srgb = srgb_gamma_ps!(r_vals);
                let g_srgb = srgb_gamma_ps!(g_vals);
                let b_srgb = srgb_gamma_ps!(b_vals);

                let r_i = _mm256_cvttps_epi32(r_srgb);
                let g_i = _mm256_cvttps_epi32(g_srgb);
                let b_i = _mm256_cvttps_epi32(b_srgb);

                let g_shifted = _mm256_slli_epi32(g_i, 8);
                let b_shifted = _mm256_slli_epi32(b_i, 16);
                let rgba_rgb = _mm256_or_si256(_mm256_or_si256(r_i, g_shifted), b_shifted);
                if FORCE_OPAQUE_ALPHA {
                    _mm256_or_si256(
                        rgba_rgb,
                        _mm256_set1_epi32(i32::from_ne_bytes([0, 0, 0, u8::MAX])),
                    )
                } else {
                    let a_clamped = _mm256_min_ps(_mm256_max_ps(a_vals, zero), one);
                    let a_srgb = _mm256_add_ps(_mm256_mul_ps(a_clamped, scale255), half);
                    let a_i = _mm256_cvttps_epi32(a_srgb);
                    let a_shifted = _mm256_slli_epi32(a_i, 24);
                    _mm256_or_si256(rgba_rgb, a_shifted)
                }
            }};
        }

        let mut src_ptr = src as *const u16;
        let mut dst_ptr = dst;
        let mut remaining = pixel_count;

        if pixel_count >= AVX512_UNROLL_MIN_PIXELS {
            // Process 32 pixels per iteration to increase ILP in the heavy F16C+gamma path.
            while remaining >= 32 {
                if remaining >= 64 {
                    _mm_prefetch(src_ptr.add(128) as *const i8, _MM_HINT_T0);
                    _mm_prefetch(src_ptr.add(160) as *const i8, _MM_HINT_T0);
                }

                let lo0 = convert_8px!(src_ptr);
                let hi0 = convert_8px!(src_ptr.add(32));
                let combined0 = _mm512_inserti64x4(_mm512_castsi256_si512(lo0), hi0, 1);
                if nontemporal {
                    _mm512_stream_si512(dst_ptr as *mut __m512i, combined0);
                } else {
                    _mm512_storeu_si512(dst_ptr as *mut __m512i, combined0);
                }

                let lo1 = convert_8px!(src_ptr.add(64));
                let hi1 = convert_8px!(src_ptr.add(96));
                let combined1 = _mm512_inserti64x4(_mm512_castsi256_si512(lo1), hi1, 1);
                if nontemporal {
                    _mm512_stream_si512(dst_ptr.add(64) as *mut __m512i, combined1);
                } else {
                    _mm512_storeu_si512(dst_ptr.add(64) as *mut __m512i, combined1);
                }

                src_ptr = src_ptr.add(128); // 32 pixels * 4 channels
                dst_ptr = dst_ptr.add(128); // 32 pixels * 4 bytes
                remaining -= 32;
            }
        }

        // Process 16 pixels per iteration (two 8-wide batches -> one 512-bit store).
        while remaining >= 16 {
            if remaining >= 32 {
                _mm_prefetch(src_ptr.add(64) as *const i8, _MM_HINT_T0);
                _mm_prefetch(src_ptr.add(96) as *const i8, _MM_HINT_T0);
            }

            let lo = convert_8px!(src_ptr);
            let hi = convert_8px!(src_ptr.add(32));

            // Combine two 256-bit results into one 512-bit register and store
            let combined = _mm512_inserti64x4(_mm512_castsi256_si512(lo), hi, 1);
            if nontemporal {
                _mm512_stream_si512(dst_ptr as *mut __m512i, combined);
            } else {
                _mm512_storeu_si512(dst_ptr as *mut __m512i, combined);
            }

            src_ptr = src_ptr.add(64); // 16 pixels * 4 channels
            dst_ptr = dst_ptr.add(64); // 16 pixels * 4 bytes
            remaining -= 16;
        }

        if remaining >= 8 {
            let result = convert_8px!(src_ptr);
            if nontemporal {
                _mm256_stream_si256(dst_ptr as *mut __m256i, result);
            } else {
                _mm256_storeu_si256(dst_ptr as *mut __m256i, result);
            }
            src_ptr = src_ptr.add(32);
            dst_ptr = dst_ptr.add(32);
            remaining -= 8;
        }

        if nontemporal && fence {
            _mm_sfence();
        }

        if remaining > 0 {
            if FORCE_OPAQUE_ALPHA {
                convert_f16_rgba_to_srgb_scalar_opaque_unchecked(
                    src_ptr as *const u8,
                    dst_ptr,
                    remaining,
                );
            } else {
                convert_f16_rgba_to_srgb_scalar_unchecked(src_ptr as *const u8, dst_ptr, remaining);
            }
        }
    } // unsafe
}

// F16 HDR->sRGB entry point for AVX-512+F16C-capable systems.
// The AVX-512 path uses the prepared HDR context and bypasses runtime
// feature checks in the hot loop.

#[target_feature(enable = "avx512f,avx512bw,f16c,avx2")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_hdr_avx512_prepared_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    // Specialize by LUT usage so the hot loop doesn't carry a runtime branch.
    if prepared.use_lut() {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_avx512_inner::<true, false, false>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    } else {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_avx512_inner::<false, false, false>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    }
}

#[target_feature(enable = "avx512f,avx512bw,f16c,avx2")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_hdr_avx512_prepared_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    if prepared.use_lut() {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_avx512_inner::<true, false, true>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    } else {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_avx512_inner::<false, false, true>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    }
}

#[target_feature(enable = "avx512f,avx512bw,f16c,avx2,fma")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_hdr_avx512_fma_prepared_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    // Specialize by LUT usage so the hot loop doesn't carry a runtime branch.
    if prepared.use_lut() {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_avx512_inner::<true, true, false>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    } else {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_avx512_inner::<false, true, false>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    }
}

#[target_feature(enable = "avx512f,avx512bw,f16c,avx2,fma")]
pub(crate) unsafe fn convert_f16_rgba_to_srgb_hdr_avx512_fma_prepared_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    if prepared.use_lut() {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_avx512_inner::<true, true, true>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    } else {
        unsafe {
            convert_f16_rgba_to_srgb_hdr_avx512_inner::<false, true, true>(
                src,
                dst,
                pixel_count,
                prepared,
            )
        }
    }
}

#[target_feature(enable = "avx512f,avx512bw,f16c,avx2")]
unsafe fn convert_f16_rgba_to_srgb_hdr_avx512_inner<
    const USE_LUT: bool,
    const USE_FMA: bool,
    const FORCE_OPAQUE_ALPHA: bool,
>(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    use std::arch::x86_64::*;

    unsafe {
        let (lut_ptr, lut_input_max, lut_inv_step) = if USE_LUT {
            let lut = prepared
                .luma_lut
                .as_ref()
                .expect("LUT-specialized HDR kernel requires prepared LUT context");
            (lut.values_ptr(), lut.input_max(), lut.inv_step())
        } else {
            (std::ptr::null(), 0.0, 0.0)
        };
        let inv_boost = prepared.inv_boost;
        let ow = prepared.curve.ow;
        let ob = prepared.curve.ob;
        let ib = prepared.curve.ib;
        let bt_denom = prepared.curve.denom;
        let bt_min_lum = prepared.curve.min_lum;
        let bt_max_lum = prepared.curve.max_lum;
        let bt_ks = prepared.curve.ks;

        let v_zero = _mm256_setzero_ps();
        let v_one = _mm256_set1_ps(1.0);
        let v_two = _mm256_set1_ps(2.0);
        let v_three = _mm256_set1_ps(3.0);
        let v_half = _mm256_set1_ps(0.5);
        let v_255 = _mm256_set1_ps(255.0);
        let v_inv_boost = _mm256_set1_ps(inv_boost);
        let v_eps = _mm256_set1_ps(HDR_EPSILON);
        let v_pq_abs_inv = _mm256_set1_ps(1.0 / HDR_PQ_ABSOLUTE_NITS);
        let v_pq_abs = _mm256_set1_ps(HDR_PQ_ABSOLUTE_NITS);
        let v_c1 = _mm256_set1_ps(HDR_PQ_C1);
        let v_c2 = _mm256_set1_ps(HDR_PQ_C2);
        let v_c3 = _mm256_set1_ps(HDR_PQ_C3);
        let v_bt_ib = _mm256_set1_ps(ib);
        let v_bt_ob = _mm256_set1_ps(ob);
        let v_bt_ow = _mm256_set1_ps(ow);
        let v_bt_denom = _mm256_set1_ps(bt_denom);
        let v_bt_ks = _mm256_set1_ps(bt_ks);
        let v_bt_min_lum = _mm256_set1_ps(bt_min_lum);
        let v_bt_max_lum = _mm256_set1_ps(bt_max_lum);
        let v_bt_one_minus_ks = _mm256_sub_ps(v_one, v_bt_ks);
        let v_bt_one_minus_ks_safe = _mm256_max_ps(v_bt_one_minus_ks, v_eps);
        let v_neg_two = _mm256_set1_ps(-2.0);
        let v_hdr_input_black = _mm256_set1_ps(HDR_INPUT_BLACK_NITS);
        let v_sdr_ref_white = _mm256_set1_ps(HDR_SDR_REFERENCE_WHITE_NITS);

        let v_luma_r = _mm256_set1_ps(0.2126);
        let v_luma_g = _mm256_set1_ps(0.7152);
        let v_luma_b = _mm256_set1_ps(0.0722);
        let v_lut_input_max = _mm256_set1_ps(lut_input_max);
        let v_lut_inv_step = _mm256_set1_ps(lut_inv_step);
        let v_lut_last_idx = _mm256_set1_epi32((super::f16::HDR_LUMA_LUT_SIZE - 1) as i32);
        let v_i32_one = _mm256_set1_epi32(1);

        let srgb_lut_ptr = super::f16::hdr_srgb_lut().as_ptr();
        let perm = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);

        macro_rules! mul_add_ps {
            ($a:expr, $b:expr, $c:expr) => {{
                if USE_FMA {
                    fmadd_ps($a, $b, $c)
                } else {
                    _mm256_add_ps(_mm256_mul_ps($a, $b), $c)
                }
            }};
        }

        macro_rules! nits_to_pq_ps {
            ($nits:expr) => {{
                let norm = _mm256_mul_ps(_mm256_max_ps($nits, v_zero), v_pq_abs_inv);
                let p = hdr_pow_ps(norm, HDR_PQ_M1);
                let num = mul_add_ps!(v_c2, p, v_c1);
                let den = mul_add_ps!(v_c3, p, v_one);
                let ratio = _mm256_div_ps(num, den);
                hdr_pow_ps(ratio, HDR_PQ_M2)
            }};
        }

        macro_rules! pq_to_nits_ps {
            ($v:expr) => {{
                let p = hdr_pow_ps(_mm256_max_ps($v, v_zero), HDR_PQ_INV_M2);
                let num = _mm256_max_ps(_mm256_sub_ps(p, v_c1), v_zero);
                let den = _mm256_max_ps(_mm256_sub_ps(v_c2, _mm256_mul_ps(v_c3, p)), v_eps);
                let ratio = _mm256_div_ps(num, den);
                _mm256_mul_ps(
                    hdr_pow_ps(_mm256_max_ps(ratio, v_zero), HDR_PQ_INV_M1),
                    v_pq_abs,
                )
            }};
        }

        macro_rules! pack_rgba_ps {
            ($r:expr, $g:expr, $b:expr, $a:expr) => {{
                let r_i = hdr_srgb_bytes_ps($r, srgb_lut_ptr);
                let g_i = hdr_srgb_bytes_ps($g, srgb_lut_ptr);
                let b_i = hdr_srgb_bytes_ps($b, srgb_lut_ptr);
                let a_i = if FORCE_OPAQUE_ALPHA {
                    _mm256_set1_epi32(255)
                } else {
                    _mm256_cvttps_epi32(mul_add_ps!($a, v_255, v_half))
                };

                let g_shifted = _mm256_slli_epi32(g_i, 8);
                let b_shifted = _mm256_slli_epi32(b_i, 16);
                let a_shifted = _mm256_slli_epi32(a_i, 24);
                _mm256_or_si256(
                    _mm256_or_si256(r_i, g_shifted),
                    _mm256_or_si256(b_shifted, a_shifted),
                )
            }};
        }

        macro_rules! process_hdr_8px {
            ($src_words:expr) => {{
                let h0 = _mm_loadu_si128($src_words as *const __m128i);
                let h1 = _mm_loadu_si128(($src_words).add(8) as *const __m128i);
                let h2 = _mm_loadu_si128(($src_words).add(16) as *const __m128i);
                let h3 = _mm_loadu_si128(($src_words).add(24) as *const __m128i);

                let f0 = _mm256_cvtph_ps(h0);
                let f1 = _mm256_cvtph_ps(h1);
                let f2 = _mm256_cvtph_ps(h2);
                let f3 = _mm256_cvtph_ps(h3);

                let t0 = _mm256_unpacklo_ps(f0, f1);
                let t1 = _mm256_unpackhi_ps(f0, f1);
                let t2 = _mm256_unpacklo_ps(f2, f3);
                let t3 = _mm256_unpackhi_ps(f2, f3);

                let rr = _mm256_shuffle_ps(t0, t2, 0b01_00_01_00);
                let gg = _mm256_shuffle_ps(t0, t2, 0b11_10_11_10);
                let bb = _mm256_shuffle_ps(t1, t3, 0b01_00_01_00);
                let aa = _mm256_shuffle_ps(t1, t3, 0b11_10_11_10);

                let [mut r, mut g, mut b] = restore_hdr_screen_colors(
                    [
                        _mm256_permutevar8x32_ps(rr, perm),
                        _mm256_permutevar8x32_ps(gg, perm),
                        _mm256_permutevar8x32_ps(bb, perm),
                    ],
                    prepared.screen_color_rows,
                );
                let a = _mm256_min_ps(
                    _mm256_max_ps(_mm256_permutevar8x32_ps(aa, perm), v_zero),
                    v_one,
                );

                r = _mm256_mul_ps(r, v_inv_boost);
                g = _mm256_mul_ps(g, v_inv_boost);
                b = _mm256_mul_ps(b, v_inv_boost);

                let rg_luma = mul_add_ps!(v_luma_g, g, _mm256_mul_ps(r, v_luma_r));
                let y_in = mul_add_ps!(v_luma_b, b, rg_luma);
                let y_in_pos = _mm256_max_ps(y_in, v_zero);
                let y_out = if USE_LUT {
                    let lut_idx_f =
                        _mm256_mul_ps(_mm256_min_ps(y_in_pos, v_lut_input_max), v_lut_inv_step);
                    let lut_idx = _mm256_cvttps_epi32(lut_idx_f);
                    let lut_idx_next =
                        _mm256_min_epi32(_mm256_add_epi32(lut_idx, v_i32_one), v_lut_last_idx);
                    let lut_frac = _mm256_sub_ps(lut_idx_f, _mm256_cvtepi32_ps(lut_idx));
                    let y0 = _mm256_i32gather_ps(lut_ptr, lut_idx, 4);
                    let y1 = _mm256_i32gather_ps(lut_ptr, lut_idx_next, 4);
                    _mm256_add_ps(y0, _mm256_mul_ps(lut_frac, _mm256_sub_ps(y1, y0)))
                } else {
                    let l_in_nits =
                        _mm256_max_ps(_mm256_mul_ps(y_in_pos, v_sdr_ref_white), v_hdr_input_black);

                    let x = nits_to_pq_ps!(l_in_nits);
                    let mut y = _mm256_div_ps(_mm256_sub_ps(x, v_bt_ib), v_bt_denom);

                    let tb = _mm256_div_ps(_mm256_sub_ps(y, v_bt_ks), v_bt_one_minus_ks_safe);
                    let tb2 = _mm256_mul_ps(tb, tb);
                    let tb3 = _mm256_mul_ps(tb2, tb);
                    let poly = _mm256_add_ps(
                        _mm256_add_ps(
                            _mm256_mul_ps(
                                _mm256_add_ps(
                                    _mm256_sub_ps(
                                        _mm256_mul_ps(v_two, tb3),
                                        _mm256_mul_ps(v_three, tb2),
                                    ),
                                    v_one,
                                ),
                                v_bt_ks,
                            ),
                            _mm256_mul_ps(
                                _mm256_add_ps(_mm256_sub_ps(tb3, _mm256_mul_ps(v_two, tb2)), tb),
                                v_bt_one_minus_ks,
                            ),
                        ),
                        _mm256_mul_ps(
                            _mm256_add_ps(
                                _mm256_mul_ps(v_neg_two, tb3),
                                _mm256_mul_ps(v_three, tb2),
                            ),
                            v_bt_max_lum,
                        ),
                    );
                    y = _mm256_blendv_ps(y, poly, _mm256_cmp_ps(y, v_bt_ks, _CMP_GE_OQ));

                    let one_minus_y = _mm256_max_ps(_mm256_sub_ps(v_one, y), v_zero);
                    let one_minus_y2 = _mm256_mul_ps(one_minus_y, one_minus_y);
                    let one_minus_y4 = _mm256_mul_ps(one_minus_y2, one_minus_y2);
                    let y_black = mul_add_ps!(v_bt_min_lum, one_minus_y4, y);
                    y = _mm256_blendv_ps(y, y_black, _mm256_cmp_ps(y, v_zero, _CMP_GE_OQ));

                    let mapped_pq = _mm256_min_ps(
                        _mm256_max_ps(mul_add_ps!(y, v_bt_denom, v_bt_ib), v_bt_ob),
                        v_bt_ow,
                    );
                    _mm256_max_ps(
                        _mm256_div_ps(pq_to_nits_ps!(mapped_pq), v_sdr_ref_white),
                        v_zero,
                    )
                };

                let [r_hdr, g_hdr, b_hdr] = scale_hdr_rgb_ps([r, g, b], y_in_pos, y_out);
                pack_rgba_ps!(r_hdr, g_hdr, b_hdr, a)
            }};
        }

        let mut src_ptr = src as *const u16;
        let mut dst_ptr = dst;
        let mut remaining = pixel_count;

        if pixel_count >= AVX512_UNROLL_MIN_PIXELS {
            while remaining >= 32 {
                if remaining >= 64 {
                    _mm_prefetch(src_ptr.add(128) as *const i8, _MM_HINT_T0);
                    _mm_prefetch(src_ptr.add(160) as *const i8, _MM_HINT_T0);
                }

                let lo0 = process_hdr_8px!(src_ptr);
                let hi0 = process_hdr_8px!(src_ptr.add(32));
                let combined0 = _mm512_inserti64x4(_mm512_castsi256_si512(lo0), hi0, 1);
                _mm512_storeu_si512(dst_ptr as *mut __m512i, combined0);

                let lo1 = process_hdr_8px!(src_ptr.add(64));
                let hi1 = process_hdr_8px!(src_ptr.add(96));
                let combined1 = _mm512_inserti64x4(_mm512_castsi256_si512(lo1), hi1, 1);
                _mm512_storeu_si512(dst_ptr.add(64) as *mut __m512i, combined1);

                src_ptr = src_ptr.add(128);
                dst_ptr = dst_ptr.add(128);
                remaining -= 32;
            }
        }

        while remaining >= 16 {
            if remaining >= 32 {
                _mm_prefetch(src_ptr.add(64) as *const i8, _MM_HINT_T0);
                _mm_prefetch(src_ptr.add(96) as *const i8, _MM_HINT_T0);
            }

            let lo = process_hdr_8px!(src_ptr);
            let hi = process_hdr_8px!(src_ptr.add(32));
            let combined = _mm512_inserti64x4(_mm512_castsi256_si512(lo), hi, 1);
            _mm512_storeu_si512(dst_ptr as *mut __m512i, combined);

            src_ptr = src_ptr.add(64);
            dst_ptr = dst_ptr.add(64);
            remaining -= 16;
        }

        if remaining >= 8 {
            let rgba = process_hdr_8px!(src_ptr);
            _mm256_storeu_si256(dst_ptr as *mut __m256i, rgba);
            src_ptr = src_ptr.add(32);
            dst_ptr = dst_ptr.add(32);
            remaining -= 8;
        }

        if remaining > 0 {
            if FORCE_OPAQUE_ALPHA {
                convert_f16_rgba_to_srgb_hdr_scalar_prepared_opaque_unchecked(
                    src_ptr as *const u8,
                    dst_ptr,
                    remaining,
                    prepared,
                );
            } else {
                convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked(
                    src_ptr as *const u8,
                    dst_ptr,
                    remaining,
                    prepared,
                );
            }
        }
    }
}

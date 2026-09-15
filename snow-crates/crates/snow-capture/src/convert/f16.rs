use super::HdrFrameContext;
use half::f16;
use std::cell::RefCell;
use std::sync::{Arc, Mutex, OnceLock};

/// Convert a linear-light value in [0, 1] to an sRGB-encoded byte in [0, 255].
///
/// Implements the sRGB electro-optical transfer function (EOTF/OETF) defined in
/// IEC 61966-2-1:1999, Section 4.7:
///
///   - Linear segment:  C_srgb = 12.92 * C_linear          when C_linear <= 0.0031308
///   - Gamma segment:   C_srgb = 1.055 * C_linear^(1/2.4) - 0.055   otherwise
///
/// The threshold 0.0031308 and the constants 12.92, 1.055, 0.055, and the
/// exponent 1/2.4 are all specified by the standard to ensure a smooth
/// transition between the two segments at the junction point.
pub(crate) fn linear_to_srgb_u8(v: f32) -> u8 {
    let c = v.clamp(0.0, 1.0);
    let srgb = if c <= 0.003_130_8 {
        c * 12.92
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    };
    (srgb * 255.0 + 0.5).floor().clamp(0.0, 255.0) as u8
}

/// Constants for the SMPTE ST 2084 Perceptual Quantizer (PQ) transfer function.
/// These are exact rational values from SMPTE ST 2084.
const PQ_M1: f32 = 0.159_301_76;
const PQ_M2: f32 = 78.843_75;
const PQ_C1: f32 = 0.835_937_5;
const PQ_C2: f32 = 18.851_563;
const PQ_C3: f32 = 18.687_5;

const PQ_ABSOLUTE_NITS: f32 = 10_000.0;
const SDR_REFERENCE_WHITE_NITS: f32 = 80.0;
const SDR_OUTPUT_WHITE_NITS: f32 = 100.0;
const SDR_OUTPUT_BLACK_NITS: f32 = 0.1;
const HDR_INPUT_BLACK_NITS: f32 = 0.001;
const SDR_IDENTITY_EPS: f32 = 1e-3;
const EPSILON: f32 = 1e-6;
pub(crate) const HDR_LUMA_LUT_SIZE: usize = 2048;
const HDR_LUMA_LUT_CACHE_SIZE: usize = 4;

#[derive(Clone)]
pub(crate) struct HdrLumaLut {
    values: Arc<[f32; HDR_LUMA_LUT_SIZE]>,
    input_max: f32,
    inv_step: f32,
}

impl HdrLumaLut {
    #[inline(always)]
    pub(crate) fn map_linear_luma(&self, linear_luma: f32) -> f32 {
        let clamped = linear_luma.clamp(0.0, self.input_max);
        let pos = clamped * self.inv_step;
        let idx = pos.floor() as usize;
        let idx0 = idx.min(HDR_LUMA_LUT_SIZE - 1);
        let idx1 = (idx0 + 1).min(HDR_LUMA_LUT_SIZE - 1);
        let t = pos - idx0 as f32;
        let y0 = self.values[idx0];
        let y1 = self.values[idx1];
        y0 + (y1 - y0) * t
    }

    #[inline(always)]
    #[cfg(any(windows, target_arch = "x86_64"))]
    pub(crate) fn values_ptr(&self) -> *const f32 {
        self.values.as_ptr()
    }

    #[inline(always)]
    #[cfg(any(windows, target_arch = "x86_64"))]
    pub(crate) fn input_max(&self) -> f32 {
        self.input_max
    }

    #[inline(always)]
    #[cfg(any(windows, target_arch = "x86_64"))]
    pub(crate) fn inv_step(&self) -> f32 {
        self.inv_step
    }
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct HdrBt2390Curve {
    pub(crate) ow: f32,
    pub(crate) ob: f32,
    pub(crate) ib: f32,
    pub(crate) denom: f32,
    pub(crate) min_lum: f32,
    pub(crate) max_lum: f32,
    pub(crate) ks: f32,
}

#[derive(Clone)]
pub(crate) struct HdrPreparedContext {
    pub(crate) screen_color_rows: Option<[[f32; 4]; 3]>,
    pub(crate) inv_boost: f32,
    pub(crate) curve: HdrBt2390Curve,
    pub(crate) luma_lut: Option<HdrLumaLut>,
}

impl HdrPreparedContext {
    #[inline(always)]
    #[cfg(any(windows, target_arch = "x86_64"))]
    pub(crate) fn use_lut(&self) -> bool {
        self.luma_lut.is_some()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct HdrLumaLutKey {
    hdr_peak_bits: u32,
    input_model: u8,
}

#[derive(Clone)]
struct HdrLumaLutCacheEntry {
    key: HdrLumaLutKey,
    lut: HdrLumaLut,
}

#[derive(Clone)]
struct HdrPreparedContextCacheEntry {
    params: HdrFrameContext,
    prepared: HdrPreparedContext,
}

thread_local! {
    static HDR_PREPARED_CONTEXT_CACHE: RefCell<Option<HdrPreparedContextCacheEntry>> = const {
        RefCell::new(None)
    };
}

/// Encode absolute luminance (nits) to PQ signal value.
#[inline(always)]
fn nits_to_pq(nits: f32) -> f32 {
    let p = (nits.max(0.0) / PQ_ABSOLUTE_NITS).powf(PQ_M1);
    ((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p)).powf(PQ_M2)
}

/// Decode PQ signal value to absolute luminance (nits).
#[inline(always)]
fn pq_to_nits(v: f32) -> f32 {
    let p = v.max(0.0).powf(1.0 / PQ_M2);
    let numerator = (p - PQ_C1).max(0.0);
    let denominator = (PQ_C2 - PQ_C3 * p).max(EPSILON);
    (numerator / denominator).powf(1.0 / PQ_M1) * PQ_ABSOLUTE_NITS
}

/// ITU-R BT.2390 EETF polynomial segment (PQ domain).
#[inline(always)]
fn bt2390_eetf_pq_with_curve(x: f32, curve: HdrBt2390Curve) -> f32 {
    let mut y = (x - curve.ib) / curve.denom;

    if y >= curve.ks {
        let tb = (y - curve.ks) / (1.0 - curve.ks).max(EPSILON);
        let tb2 = tb * tb;
        let tb3 = tb2 * tb;
        y = (2.0 * tb3 - 3.0 * tb2 + 1.0) * curve.ks
            + (tb3 - 2.0 * tb2 + tb) * (1.0 - curve.ks)
            + (-2.0 * tb3 + 3.0 * tb2) * curve.max_lum;
    }

    if y >= 0.0 {
        y += curve.min_lum * (1.0 - y).max(0.0).powi(4);
    }

    (y * curve.denom + curve.ib).clamp(curve.ob, curve.ow)
}

#[inline(always)]
pub(crate) fn bt2390_curve(peak_nits: f32) -> HdrBt2390Curve {
    let ow = nits_to_pq(SDR_OUTPUT_WHITE_NITS);
    let ob = nits_to_pq(SDR_OUTPUT_BLACK_NITS);
    let iw = nits_to_pq(peak_nits.max(SDR_OUTPUT_WHITE_NITS + 1e-3));
    let ib = nits_to_pq(HDR_INPUT_BLACK_NITS).min(ob - 1e-3);
    let denom = (iw - ib).max(EPSILON);
    let min_lum = (ob - ib) / denom;
    let max_lum = (ow - ib) / denom;
    let ks = 1.5 * max_lum - 0.5;
    HdrBt2390Curve {
        ow,
        ob,
        ib,
        denom,
        min_lum,
        max_lum,
        ks,
    }
}

#[inline(always)]
pub(crate) fn bt2390_map_linear_luma_with_curve(linear_luma: f32, curve: HdrBt2390Curve) -> f32 {
    let l_in = (linear_luma.max(0.0) * SDR_REFERENCE_WHITE_NITS).max(HDR_INPUT_BLACK_NITS);
    let x = nits_to_pq(l_in);
    let mapped = bt2390_eetf_pq_with_curve(x, curve);
    (pq_to_nits(mapped) / SDR_REFERENCE_WHITE_NITS).max(0.0)
}

#[inline(always)]
fn bt2390_map_linear_luma(linear_luma: f32, peak_nits: f32) -> f32 {
    bt2390_map_linear_luma_with_curve(linear_luma, bt2390_curve(peak_nits))
}

#[inline(always)]
fn hdr_luma_lut_key(params: HdrFrameContext) -> HdrLumaLutKey {
    HdrLumaLutKey {
        hdr_peak_bits: params.hdr_peak_nits.to_bits(),
        input_model: params.input_model as u8,
    }
}

fn hdr_luma_lut_cache() -> &'static Mutex<Vec<HdrLumaLutCacheEntry>> {
    static CACHE: OnceLock<Mutex<Vec<HdrLumaLutCacheEntry>>> = OnceLock::new();
    CACHE.get_or_init(|| Mutex::new(Vec::new()))
}

pub(crate) fn build_bt2390_luma_lut(params: HdrFrameContext) -> HdrLumaLut {
    let params = params.sanitized();
    let input_max = (params.hdr_peak_nits / SDR_REFERENCE_WHITE_NITS).max(1.0);
    let step = input_max / ((HDR_LUMA_LUT_SIZE - 1) as f32);
    let inv_step = if step > EPSILON { 1.0 / step } else { 0.0 };
    let mut values = [0.0f32; HDR_LUMA_LUT_SIZE];
    for (idx, value) in values.iter_mut().enumerate() {
        let linear_luma = (idx as f32) * step;
        *value = bt2390_map_linear_luma(linear_luma, params.hdr_peak_nits);
    }
    HdrLumaLut {
        values: Arc::new(values),
        input_max,
        inv_step,
    }
}

pub(crate) fn hdr_luma_lut_for_context(params: HdrFrameContext) -> HdrLumaLut {
    let params = params.sanitized();
    let key = hdr_luma_lut_key(params);
    let cache = hdr_luma_lut_cache();
    let mut guard = cache
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    if let Some(idx) = guard.iter().position(|entry| entry.key == key) {
        let entry = guard.remove(idx);
        let lut = entry.lut.clone();
        guard.push(entry);
        return lut;
    }

    let lut = build_bt2390_luma_lut(params);
    guard.push(HdrLumaLutCacheEntry {
        key,
        lut: lut.clone(),
    });
    if guard.len() > HDR_LUMA_LUT_CACHE_SIZE {
        guard.remove(0);
    }
    lut
}

#[inline(always)]
pub(crate) fn prepare_hdr_context(params: HdrFrameContext) -> HdrPreparedContext {
    let params = params.sanitized();
    let inv_boost = (SDR_REFERENCE_WHITE_NITS / params.sdr_white_nits.max(EPSILON)).max(EPSILON);
    let curve = bt2390_curve(params.hdr_peak_nits);
    let luma_lut = if params.tonemap_use_lut {
        Some(hdr_luma_lut_for_context(params))
    } else {
        None
    };
    HdrPreparedContext {
        screen_color_rows: None,
        inv_boost,
        curve,
        luma_lut,
    }
}

#[inline]
pub(crate) fn prepare_hdr_context_cached(params: HdrFrameContext) -> HdrPreparedContext {
    let params = params.sanitized();

    HDR_PREPARED_CONTEXT_CACHE.with(|cache| {
        let mut cache = cache.borrow_mut();
        if let Some(entry) = cache.as_ref()
            && entry.params == params
        {
            return entry.prepared.clone();
        }

        let prepared = prepare_hdr_context(params);
        *cache = Some(HdrPreparedContextCacheEntry {
            params,
            prepared: prepared.clone(),
        });
        prepared
    })
}

#[inline(always)]
fn inverse_windows_sdr_boost(rgb: &mut [f32; 3], inv_boost: f32) {
    rgb[0] *= inv_boost;
    rgb[1] *= inv_boost;
    rgb[2] *= inv_boost;
}

#[inline(always)]
fn is_sdr_identity_pixel(rgb: [f32; 3]) -> bool {
    rgb[0].max(rgb[1]).max(rgb[2]) <= 1.0 + SDR_IDENTITY_EPS
}

#[inline(always)]
fn tone_map_hdr_pixel_bt2390(rgb: &mut [f32; 3], curve: HdrBt2390Curve) {
    let y_in = (0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]).max(0.0);
    if y_in <= EPSILON {
        rgb[0] = 0.0;
        rgb[1] = 0.0;
        rgb[2] = 0.0;
        return;
    }

    let y_out = bt2390_map_linear_luma_with_curve(y_in, curve);
    tone_map_hdr_pixel_with_luma(rgb, y_in, y_out);
}

#[inline(always)]
fn tone_map_hdr_pixel_bt2390_lut(rgb: &mut [f32; 3], lut: &HdrLumaLut) {
    let y_in = (0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]).max(0.0);
    if y_in <= EPSILON {
        rgb[0] = 0.0;
        rgb[1] = 0.0;
        rgb[2] = 0.0;
        return;
    }
    let y_out = lut.map_linear_luma(y_in);
    tone_map_hdr_pixel_with_luma(rgb, y_in, y_out);
}

#[inline(always)]
fn tone_map_hdr_pixel_with_luma(rgb: &mut [f32; 3], y_in: f32, y_out: f32) {
    let scale = y_out / y_in.max(EPSILON);
    rgb[0] *= scale;
    rgb[1] *= scale;
    rgb[2] *= scale;

    // Hue-preserving compression to SDR gamut bounds.
    let max_channel = rgb[0].max(rgb[1]).max(rgb[2]);
    if max_channel > 1.0 {
        let inv = 1.0 / max_channel;
        rgb[0] *= inv;
        rgb[1] *= inv;
        rgb[2] *= inv;
    }
}

/// 64 KB lookup table mapping every possible IEEE 754 binary16 bit pattern
/// (0x0000..0xFFFF) to its sRGB-encoded u8 value.
///
/// For each 16-bit index `i`, the table stores:
///   `linear_to_srgb_u8(f16::from_bits(i).to_f32())`
///
/// This trades memory for speed: a single table lookup replaces the
/// per-channel `powf(1/2.4)` call in the scalar F16->sRGB path, turning
/// the conversion into three byte loads per pixel (plus one for alpha).
fn f16_to_srgb_lut() -> &'static [u8; 65_536] {
    static LUT: OnceLock<[u8; 65_536]> = OnceLock::new();
    LUT.get_or_init(|| {
        let mut lut = [0u8; 65_536];
        let mut i = 0usize;
        while i < lut.len() {
            let linear = f16::from_bits(i as u16).to_f32();
            lut[i] = linear_to_srgb_u8(linear);
            i += 1;
        }
        lut
    })
}

/// pay the ~1-2 ms build cost.
pub(crate) fn warmup_lut() {
    let _ = f16_to_srgb_lut();
    let _ = hdr_luma_lut_for_context(HdrFrameContext::default());
}

#[inline(always)]
unsafe fn pack_f16_rgba_to_srgb<const FORCE_OPAQUE_ALPHA: bool>(
    src_words: *const u16,
    lut: &[u8; 65_536],
) -> u32 {
    let packed = unsafe { std::ptr::read_unaligned(src_words as *const u64) };
    #[cfg(target_endian = "big")]
    let packed = packed.swap_bytes();

    let r_bits = (packed & 0xFFFF) as usize;
    let g_bits = ((packed >> 16) & 0xFFFF) as usize;
    let b_bits = ((packed >> 32) & 0xFFFF) as usize;
    let a_bits = ((packed >> 48) & 0xFFFF) as usize;

    let a_byte = if FORCE_OPAQUE_ALPHA {
        u32::from(u8::MAX)
    } else {
        let a = f16::from_bits(a_bits as u16).to_f32().clamp(0.0, 1.0);
        (a * 255.0 + 0.5) as u32
    };

    unsafe {
        (*lut.get_unchecked(r_bits) as u32)
            | ((*lut.get_unchecked(g_bits) as u32) << 8)
            | ((*lut.get_unchecked(b_bits) as u32) << 16)
            | (a_byte << 24)
    }
}

pub(crate) unsafe fn convert_f16_rgba_to_srgb_scalar_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_f16_rgba_to_srgb_scalar_impl::<false>(src, dst, pixel_count);
    }
}

pub(crate) unsafe fn convert_f16_rgba_to_srgb_scalar_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_f16_rgba_to_srgb_scalar_impl::<true>(src, dst, pixel_count);
    }
}

unsafe fn convert_f16_rgba_to_srgb_scalar_impl<const FORCE_OPAQUE_ALPHA: bool>(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    let lut = f16_to_srgb_lut();
    #[cfg(target_arch = "x86_64")]
    let lut_ptr = lut.as_ptr();
    let mut src_words = src as *const u16;
    let mut dst_px = dst as *mut u32;
    let mut remaining = pixel_count;

    #[cfg(target_arch = "x86_64")]
    macro_rules! prefetch_lut_entries {
        ($base:expr) => {
            #[cfg(target_arch = "x86_64")]
            {
                use std::arch::x86_64::{_MM_HINT_NTA, _mm_prefetch};
                let packed = std::ptr::read_unaligned($base as *const u64);
                let r_bits = (packed & 0xFFFF) as usize;
                let g_bits = ((packed >> 16) & 0xFFFF) as usize;
                let b_bits = ((packed >> 32) & 0xFFFF) as usize;
                _mm_prefetch(lut_ptr.add(r_bits) as *const i8, _MM_HINT_NTA);
                _mm_prefetch(lut_ptr.add(g_bits) as *const i8, _MM_HINT_NTA);
                _mm_prefetch(lut_ptr.add(b_bits) as *const i8, _MM_HINT_NTA);
            }
        };
    }

    while remaining >= 16 {
        // Prefetch LUT entries for the *next* batch of 16 pixels
        // (4 channels * 2 bytes = 8 bytes per pixel, so 16 pixels ahead
        // is 64 u16 words = 128 bytes of source data).
        #[cfg(target_arch = "x86_64")]
        if remaining >= 32 {
            unsafe {
                prefetch_lut_entries!(src_words.add(64));
                prefetch_lut_entries!(src_words.add(68));
                prefetch_lut_entries!(src_words.add(72));
                prefetch_lut_entries!(src_words.add(76));
            }
        }

        unsafe {
            let c0 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words, lut);
            let c1 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(4), lut);
            let c2 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(8), lut);
            let c3 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(12), lut);
            let c4 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(16), lut);
            let c5 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(20), lut);
            let c6 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(24), lut);
            let c7 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(28), lut);
            let c8 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(32), lut);
            let c9 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(36), lut);
            let c10 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(40), lut);
            let c11 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(44), lut);
            let c12 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(48), lut);
            let c13 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(52), lut);
            let c14 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(56), lut);
            let c15 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(60), lut);

            std::ptr::write_unaligned(dst_px, c0);
            std::ptr::write_unaligned(dst_px.add(1), c1);
            std::ptr::write_unaligned(dst_px.add(2), c2);
            std::ptr::write_unaligned(dst_px.add(3), c3);
            std::ptr::write_unaligned(dst_px.add(4), c4);
            std::ptr::write_unaligned(dst_px.add(5), c5);
            std::ptr::write_unaligned(dst_px.add(6), c6);
            std::ptr::write_unaligned(dst_px.add(7), c7);
            std::ptr::write_unaligned(dst_px.add(8), c8);
            std::ptr::write_unaligned(dst_px.add(9), c9);
            std::ptr::write_unaligned(dst_px.add(10), c10);
            std::ptr::write_unaligned(dst_px.add(11), c11);
            std::ptr::write_unaligned(dst_px.add(12), c12);
            std::ptr::write_unaligned(dst_px.add(13), c13);
            std::ptr::write_unaligned(dst_px.add(14), c14);
            std::ptr::write_unaligned(dst_px.add(15), c15);
        }

        src_words = unsafe { src_words.add(64) };
        dst_px = unsafe { dst_px.add(16) };
        remaining -= 16;
    }

    while remaining >= 8 {
        unsafe {
            let c0 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words, lut);
            let c1 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(4), lut);
            let c2 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(8), lut);
            let c3 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(12), lut);
            let c4 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(16), lut);
            let c5 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(20), lut);
            let c6 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(24), lut);
            let c7 = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words.add(28), lut);

            std::ptr::write_unaligned(dst_px, c0);
            std::ptr::write_unaligned(dst_px.add(1), c1);
            std::ptr::write_unaligned(dst_px.add(2), c2);
            std::ptr::write_unaligned(dst_px.add(3), c3);
            std::ptr::write_unaligned(dst_px.add(4), c4);
            std::ptr::write_unaligned(dst_px.add(5), c5);
            std::ptr::write_unaligned(dst_px.add(6), c6);
            std::ptr::write_unaligned(dst_px.add(7), c7);
        }

        src_words = unsafe { src_words.add(32) };
        dst_px = unsafe { dst_px.add(8) };
        remaining -= 8;
    }

    while remaining != 0 {
        unsafe {
            let color = pack_f16_rgba_to_srgb::<FORCE_OPAQUE_ALPHA>(src_words, lut);
            std::ptr::write_unaligned(dst_px, color);
        }

        src_words = unsafe { src_words.add(4) };
        dst_px = unsafe { dst_px.add(1) };
        remaining -= 1;
    }
}

pub(crate) unsafe fn convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    unsafe {
        convert_f16_rgba_to_srgb_hdr_scalar_prepared_impl::<false>(src, dst, pixel_count, prepared);
    }
}

pub(crate) unsafe fn convert_f16_rgba_to_srgb_hdr_scalar_prepared_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    unsafe {
        convert_f16_rgba_to_srgb_hdr_scalar_prepared_impl::<true>(src, dst, pixel_count, prepared);
    }
}

unsafe fn convert_f16_rgba_to_srgb_hdr_scalar_prepared_impl<const FORCE_OPAQUE_ALPHA: bool>(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    prepared: &HdrPreparedContext,
) {
    let mut src_words = src as *const u16;
    let mut dst_px = dst as *mut u32;
    let mut remaining = pixel_count;
    let inv_boost = prepared.inv_boost;
    let curve = prepared.curve;
    let hdr_lut = prepared.luma_lut.as_ref();

    while remaining != 0 {
        let packed = unsafe { std::ptr::read_unaligned(src_words as *const u64) };
        #[cfg(target_endian = "big")]
        let packed = packed.swap_bytes();

        let r = f16::from_bits((packed & 0xFFFF) as u16).to_f32();
        let g = f16::from_bits(((packed >> 16) & 0xFFFF) as u16).to_f32();
        let b = f16::from_bits(((packed >> 32) & 0xFFFF) as u16).to_f32();
        let a = if FORCE_OPAQUE_ALPHA {
            1.0
        } else {
            f16::from_bits(((packed >> 48) & 0xFFFF) as u16)
                .to_f32()
                .clamp(0.0, 1.0)
        };

        let mut rgb = [r, g, b];
        if let Some(rows) = prepared.screen_color_rows {
            rgb = rows.map(|row| row[0] * r + row[1] * g + row[2] * b + row[3]);
        }
        rgb = rgb.map(|v| v.max(0.0));
        inverse_windows_sdr_boost(&mut rgb, inv_boost);
        if !is_sdr_identity_pixel(rgb) {
            if let Some(lut) = hdr_lut {
                tone_map_hdr_pixel_bt2390_lut(&mut rgb, lut);
            } else {
                tone_map_hdr_pixel_bt2390(&mut rgb, curve);
            }
        }

        let a_byte = ((a * 255.0 + 0.5) as u32) & 0xFF;
        let color = u32::from(linear_to_srgb_u8(rgb[0]))
            | (u32::from(linear_to_srgb_u8(rgb[1])) << 8)
            | (u32::from(linear_to_srgb_u8(rgb[2])) << 16)
            | (a_byte << 24);
        unsafe { std::ptr::write_unaligned(dst_px, color) };

        src_words = unsafe { src_words.add(4) };
        dst_px = unsafe { dst_px.add(1) };
        remaining -= 1;
    }
}

pub(super) unsafe fn convert_corrected_row(
    src: *const u8,
    dst: *mut u8,
    count: usize,
    rows: [[f32; 4]; 3],
    output: crate::CapturePixelFormat,
    opaque: bool,
) {
    for i in 0..count {
        let pixel = unsafe { std::ptr::read_unaligned(src.add(i * 8).cast::<[u16; 4]>()) };
        let input = pixel.map(|bits| f16::from_bits(bits).to_f32());
        // Undo the scRGB effect before clamping negative values or removing SDR white boost.
        let rgb = rows.map(|row| {
            (row[0] * input[0] + row[1] * input[1] + row[2] * input[2] + row[3]).max(0.0)
        });
        let mut encoded = [
            linear_to_srgb_u8(rgb[0]),
            linear_to_srgb_u8(rgb[1]),
            linear_to_srgb_u8(rgb[2]),
            if opaque {
                255
            } else {
                (input[3].clamp(0., 1.) * 255. + 0.5) as u8
            },
        ];
        if output == crate::CapturePixelFormat::Bgra8 {
            encoded.swap(0, 2);
        }
        unsafe { std::ptr::write_unaligned(dst.add(i * 4).cast::<[u8; 4]>(), encoded) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pack_half_rgba(px: [f32; 4]) -> [u8; 8] {
        let r = f16::from_f32(px[0]).to_bits();
        let g = f16::from_f32(px[1]).to_bits();
        let b = f16::from_f32(px[2]).to_bits();
        let a = f16::from_f32(px[3]).to_bits();
        let mut out = [0u8; 8];
        out[0..2].copy_from_slice(&r.to_le_bytes());
        out[2..4].copy_from_slice(&g.to_le_bytes());
        out[4..6].copy_from_slice(&b.to_le_bytes());
        out[6..8].copy_from_slice(&a.to_le_bytes());
        out
    }

    #[test]
    fn sdr_pixels_are_restored_exactly_after_inverse_boost() {
        let k = 2.0f32; // Equivalent to SDR white level = 160 nits
        let context = HdrFrameContext {
            sdr_white_nits: SDR_REFERENCE_WHITE_NITS * k,
            hdr_peak_nits: 1000.0,
            ..HdrFrameContext::default()
        }
        .sanitized();

        let src_pixels = [
            [0.0, 0.0, 0.0, 1.0],
            [0.18, 0.18, 0.18, 1.0],
            [0.5, 0.25, 0.75, 1.0],
            [1.0, 1.0, 1.0, 1.0],
        ];

        let mut src = vec![0u8; src_pixels.len() * 8];
        for (idx, px) in src_pixels.iter().enumerate() {
            let boosted = [px[0] * k, px[1] * k, px[2] * k, px[3]];
            let packed = pack_half_rgba(boosted);
            src[idx * 8..idx * 8 + 8].copy_from_slice(&packed);
        }

        let mut dst = vec![0u8; src_pixels.len() * 4];
        let prepared = prepare_hdr_context(context);
        unsafe {
            convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked(
                src.as_ptr(),
                dst.as_mut_ptr(),
                src_pixels.len(),
                &prepared,
            );
        }

        for (idx, px) in src_pixels.iter().enumerate() {
            let off = idx * 4;
            assert_eq!(dst[off], linear_to_srgb_u8(px[0]));
            assert_eq!(dst[off + 1], linear_to_srgb_u8(px[1]));
            assert_eq!(dst[off + 2], linear_to_srgb_u8(px[2]));
            assert_eq!(dst[off + 3], 255);
        }
    }

    #[test]
    fn hdr_pixel_is_tonemapped_while_sdr_pixel_stays_identity() {
        let context = HdrFrameContext::default();
        let src = [
            pack_half_rgba([0.5, 0.25, 0.75, 1.0]),
            pack_half_rgba([4.0, 2.0, 1.0, 1.0]),
        ]
        .concat();

        let mut dst = vec![0u8; 8];
        let prepared = prepare_hdr_context(context);
        unsafe {
            convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked(
                src.as_ptr(),
                dst.as_mut_ptr(),
                2,
                &prepared,
            );
        }

        // SDR pixel must stay byte-identical to the baseline conversion.
        assert_eq!(dst[0], linear_to_srgb_u8(0.5));
        assert_eq!(dst[1], linear_to_srgb_u8(0.25));
        assert_eq!(dst[2], linear_to_srgb_u8(0.75));
        assert_eq!(dst[3], 255);

        // HDR pixel should still be valid SDR output and not trivially zero.
        assert!(dst[4] > 0 || dst[5] > 0 || dst[6] > 0);
    }

    #[test]
    fn lut_tonemap_stays_within_error_budget() {
        let precise_context = HdrFrameContext {
            tonemap_use_lut: false,
            ..HdrFrameContext::default()
        };
        let lut_context = HdrFrameContext {
            tonemap_use_lut: true,
            ..HdrFrameContext::default()
        };

        let pixel_count = 2048usize;
        let mut src = vec![0u8; pixel_count * 8];
        for i in 0..pixel_count {
            let t = i as f32 / (pixel_count - 1) as f32;
            let r = 0.05 + 8.0 * t.powf(1.3);
            let g = 0.03 + 6.0 * t.powf(1.1);
            let b = 0.02 + 5.0 * t.powf(1.5);
            let packed = pack_half_rgba([r, g, b, 1.0]);
            src[i * 8..i * 8 + 8].copy_from_slice(&packed);
        }

        let mut precise = vec![0u8; pixel_count * 4];
        let mut approx = vec![0u8; pixel_count * 4];
        let precise_prepared = prepare_hdr_context(precise_context);
        let approx_prepared = prepare_hdr_context(lut_context);
        unsafe {
            convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked(
                src.as_ptr(),
                precise.as_mut_ptr(),
                pixel_count,
                &precise_prepared,
            );
            convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked(
                src.as_ptr(),
                approx.as_mut_ptr(),
                pixel_count,
                &approx_prepared,
            );
        }

        let mut max_abs_diff = 0u8;
        for i in 0..pixel_count {
            for ch in 0..3 {
                let idx = i * 4 + ch;
                let diff = precise[idx].abs_diff(approx[idx]);
                max_abs_diff = max_abs_diff.max(diff);
            }
        }
        assert!(
            max_abs_diff <= 6,
            "LUT max abs diff exceeded budget: {max_abs_diff}"
        );
    }

    #[test]
    fn prepared_context_cache_reuses_same_lut_for_same_context() {
        let context = HdrFrameContext::default();
        let prepared_a = prepare_hdr_context_cached(context);
        let prepared_b = prepare_hdr_context_cached(context);

        let lut_a = prepared_a
            .luma_lut
            .as_ref()
            .expect("expected LUT-backed prepared context");
        let lut_b = prepared_b
            .luma_lut
            .as_ref()
            .expect("expected LUT-backed prepared context");

        assert!(Arc::ptr_eq(&lut_a.values, &lut_b.values));
        assert_eq!(prepared_a.inv_boost, prepared_b.inv_boost);
        assert_eq!(prepared_a.curve.ow, prepared_b.curve.ow);
        assert_eq!(prepared_a.curve.ks, prepared_b.curve.ks);
    }
}

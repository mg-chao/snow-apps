//! Explicit HDR10 → SDR conversion. PQ is absolute luminance in nits.
//! The SDR rendering uses a luminance-preserving Reinhard shoulder with 203-nit
//! reference white and 1000-nit display peak, then clips the BT.709 gamut and
//! applies the sRGB OETF. This is a rendering policy, not a metadata relabel.
use std::sync::OnceLock;

pub fn pq_to_nits(encoded: f32) -> f32 {
    let p = encoded.clamp(0.0, 1.0).powf(1.0 / 78.84375);
    let numerator = (p - 0.8359375).max(0.0);
    let denominator = 18.851563 - 18.6875 * p;
    10_000.0 * (numerator / denominator).powf(1.0 / 0.15930176)
}
/// Encode absolute luminance using SMPTE ST 2084.
pub fn nits_to_pq(nits: f32) -> f32 {
    let y = (nits / 10_000.0).clamp(0.0, 1.0).powf(0.15930176);
    ((0.8359375 + 18.851563 * y) / (1.0 + 18.6875 * y)).powf(78.84375)
}
fn pq_table() -> &'static [f32] {
    static PQ: OnceLock<Box<[f32]>> = OnceLock::new();
    PQ.get_or_init(|| {
        (0..=u16::MAX)
            .map(|x| pq_to_nits(f32::from(x) / 65535.0))
            .collect()
    })
}
/// Composite a straight-alpha sRGB effect into a full-range RGB48LE HDR10
/// pixel in linear BT.2020 light, using 203-nit SDR reference white.
/// Transparent pixels remain byte-for-byte unchanged.
pub fn blend_srgb_into_pq(destination: &mut [u8; 6], source: [u8; 4]) {
    if source[3] == 0 {
        return;
    }
    static SRGB: OnceLock<[f32; 256]> = OnceLock::new();
    let table = SRGB.get_or_init(|| {
        std::array::from_fn(|i| {
            let v = i as f32 / 255.0;
            if v <= 0.04045 {
                v / 12.92
            } else {
                ((v + 0.055) / 1.055).powf(2.4)
            }
        })
    });
    let [r, g, b] = [source[0], source[1], source[2]].map(|x| table[x as usize]);
    let linear = [
        0.627404 * r + 0.329283 * g + 0.043313 * b,
        0.069097 * r + 0.919540 * g + 0.011363 * b,
        0.016391 * r + 0.088013 * g + 0.895596 * b,
    ];
    let alpha = f32::from(source[3]) / 255.0;
    let pq = pq_table();
    for (dst, src) in destination.chunks_exact_mut(2).zip(linear) {
        let background = pq[u16::from_le_bytes([dst[0], dst[1]]) as usize];
        let value = nits_to_pq(src * 203.0 * alpha + background * (1.0 - alpha));
        dst.copy_from_slice(&((value * 65535.0).round() as u16).to_le_bytes());
    }
}
fn srgb(linear: f32) -> u8 {
    let x = linear.clamp(0.0, 1.0);
    let encoded = if x <= 0.0031308 {
        12.92 * x
    } else {
        1.055 * x.powf(1.0 / 2.4) - 0.055
    };
    (encoded * 255.0).round() as u8
}
fn map_nits(rgb: [f32; 3]) -> [u8; 4] {
    let [r, g, b] = rgb;
    let y = 0.2627 * r + 0.6780 * g + 0.0593 * b;
    let scale = if y > 0.0 {
        (1203.0 / 1000.0) / (y + 203.0)
    } else {
        0.0
    };
    // Linear-light BT.2020 D65 → BT.709 D65, before transfer encoding.
    [
        srgb((1.660491 * r - 0.587641 * g - 0.072850 * b) * scale),
        srgb((-0.124550 * r + 1.132_9 * g - 0.008350 * b) * scale),
        srgb((-0.018151 * r - 0.100579 * g + 1.118_73 * b) * scale),
        255,
    ]
}
pub fn tone_map_pq(rgb: [f32; 3]) -> [u8; 4] {
    map_nits(rgb.map(pq_to_nits))
}
/// Full-range packed RGB48LE PQ row → RGBA8 sRGB. Padding is handled by the
/// caller; mismatched row lengths are rejected before writing output.
pub fn tone_map_rgb48_row(source: &[u8], destination: &mut [u8]) -> Result<(), &'static str> {
    if !source.len().is_multiple_of(6)
        || source.len() / 6 != destination.len() / 4
        || !destination.len().is_multiple_of(4)
    {
        return Err("HDR tone-map row lengths do not match");
    }
    let table = pq_table();
    for (src, dst) in source.chunks_exact(6).zip(destination.chunks_exact_mut(4)) {
        let rgb = [0, 2, 4].map(|i| table[u16::from_le_bytes([src[i], src[i + 1]]) as usize]);
        dst.copy_from_slice(&map_nits(rgb));
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn hdr_effects_use_linear_light_and_preserve_untouched_highlights() {
        let encode = |nits| ((nits_to_pq(nits) * 65535.0).round() as u16).to_le_bytes();
        let decode = |p: &[u8]| pq_to_nits(f32::from(u16::from_le_bytes([p[0], p[1]])) / 65535.0);
        for nits in [0.0, 100.0, 203.0, 1000.0, 10000.0] {
            assert!((pq_to_nits(nits_to_pq(nits)) - nits).abs() < 1.0);
        }
        let mut pixel: [u8; 6] = encode(1000.0).repeat(3).try_into().unwrap();
        let original = pixel;
        blend_srgb_into_pq(&mut pixel, [255, 0, 0, 0]);
        assert_eq!(pixel, original);
        blend_srgb_into_pq(&mut pixel, [255; 4]);
        for c in pixel.chunks_exact(2) {
            assert!((decode(c) - 203.0).abs() < 0.1);
        }
        pixel = original;
        blend_srgb_into_pq(&mut pixel, [255, 255, 255, 128]);
        let expected = 203.0 * (128.0 / 255.0) + 1000.0 * (127.0 / 255.0);
        for c in pixel.chunks_exact(2) {
            assert!((decode(c) - expected).abs() < 0.3);
        }
        blend_srgb_into_pq(&mut pixel, [255, 0, 0, 255]);
        for (c, expected) in pixel.chunks_exact(2).zip([0.627404, 0.069097, 0.016391]) {
            assert!((decode(c) - 203.0 * expected).abs() < 0.1);
        }
    }
    #[test]
    fn pq_absolute_luminance_and_sdr_rendering() {
        assert_eq!(pq_to_nits(0.0), 0.0);
        assert!((pq_to_nits(0.5080784) - 100.0).abs() < 0.1);
        assert!((pq_to_nits(0.7518271) - 1000.0).abs() < 0.2);
        assert!((pq_to_nits(1.0) - 10_000.0).abs() < 1.0);
        assert_eq!(tone_map_pq([0.0; 3]), [0, 0, 0, 255]);
        assert_eq!(tone_map_pq([1.0; 3]), [255; 4]);
        let mut previous = 0;
        for x in 0..=1000 {
            let pixel = tone_map_pq([x as f32 / 1000.0; 3]);
            assert!(pixel[0] >= previous);
            assert!(pixel[0].abs_diff(pixel[1]) <= 1);
            previous = pixel[0];
        }
    }
    #[test]
    fn lookup_matches_scalar_and_validates_rows() {
        let mut bytes = Vec::new();
        for i in (0..=u16::MAX).step_by(37) {
            for v in [i, i / 2, u16::MAX - i] {
                bytes.extend(v.to_le_bytes());
            }
        }
        let mut output = vec![0; bytes.len() / 6 * 4];
        tone_map_rgb48_row(&bytes, &mut output).unwrap();
        for (src, dst) in bytes.chunks_exact(6).zip(output.chunks_exact(4)) {
            let rgb =
                [0, 2, 4].map(|i| f32::from(u16::from_le_bytes([src[i], src[i + 1]])) / 65535.0);
            assert_eq!(dst, tone_map_pq(rgb));
        }
        assert!(tone_map_rgb48_row(&[0; 7], &mut [0; 4]).is_err());
    }
}

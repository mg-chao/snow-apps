//! Packed RGBA/BGRA conversion. Row padding is the caller's responsibility.
/// Swap red and blue in complete four-byte pixels, preserving alpha exactly.
/// The final incomplete pixel is untouched.
pub fn swap_red_blue(bytes: &mut [u8]) {
    #[cfg(target_arch = "aarch64")]
    {
        // AArch64 macOS guarantees Advanced SIMD. Unaligned loads are supported.
        unsafe {
            swap_neon(bytes);
        }
    }
    #[cfg(not(target_arch = "aarch64"))]
    swap_scalar(bytes);
}
fn swap_scalar(bytes: &mut [u8]) {
    for pixel in bytes.chunks_exact_mut(4) {
        pixel.swap(0, 2);
    }
}
#[cfg(target_arch = "aarch64")]
unsafe fn swap_neon(bytes: &mut [u8]) {
    use std::arch::aarch64::*;
    let mut chunks = bytes.chunks_exact_mut(64);
    for chunk in &mut chunks {
        unsafe {
            let rgba = vld4q_u8(chunk.as_ptr());
            vst4q_u8(
                chunk.as_mut_ptr(),
                uint8x16x4_t(rgba.2, rgba.1, rgba.0, rgba.3),
            );
        }
    }
    swap_scalar(chunks.into_remainder());
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn vector_and_scalar_agree_for_unaligned_rows_and_tails() {
        for length in 0..260 {
            for offset in 0..16 {
                let mut native: Vec<u8> = (0..length + offset + 16)
                    .map(|i| (i * 71 + 29) as u8)
                    .collect();
                let mut scalar = native.clone();
                swap_red_blue(&mut native[offset..offset + length]);
                swap_scalar(&mut scalar[offset..offset + length]);
                assert_eq!(native, scalar);
                swap_red_blue(&mut native[offset..offset + length]);
                assert_eq!(
                    native,
                    (0..length + offset + 16)
                        .map(|i| (i * 71 + 29) as u8)
                        .collect::<Vec<_>>()
                );
            }
        }
    }
}

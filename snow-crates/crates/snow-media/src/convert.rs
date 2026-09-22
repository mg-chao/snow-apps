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

/// Copy packed pixels, exchanging red and blue in one pass.
pub fn copy_swap_red_blue(source: &[u8], destination: &mut [u8]) {
    let length = source.len().min(destination.len());
    let length = length - length % 4;
    #[cfg(target_arch = "aarch64")]
    unsafe {
        copy_swap_neon(source, destination, length);
    }
    #[cfg(not(target_arch = "aarch64"))]
    copy_swap_scalar(&source[..length], &mut destination[..length]);
    if length < destination.len() && length < source.len() {
        let tail = source.len().min(destination.len());
        destination[length..tail].copy_from_slice(&source[length..tail]);
    }
}

fn copy_swap_scalar(source: &[u8], destination: &mut [u8]) {
    for (src, dst) in source.chunks_exact(4).zip(destination.chunks_exact_mut(4)) {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = src[3];
    }
}

#[cfg(target_arch = "aarch64")]
unsafe fn copy_swap_neon(source: &[u8], destination: &mut [u8], length: usize) {
    use std::arch::aarch64::*;
    let mut offset = 0;
    while offset + 64 <= length {
        unsafe {
            let rgba = vld4q_u8(source.as_ptr().add(offset));
            vst4q_u8(
                destination.as_mut_ptr().add(offset),
                uint8x16x4_t(rgba.2, rgba.1, rgba.0, rgba.3),
            );
        }
        offset += 64;
    }
    copy_swap_scalar(&source[offset..length], &mut destination[offset..length]);
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
    #[test]
    fn copy_swap_matches_copy_then_swap_including_tails() {
        for length in 0..200 {
            let source: Vec<u8> = (0..length).map(|i| (i * 13 + 7) as u8).collect();
            let mut direct = vec![0; length];
            let mut reference = source.clone();
            copy_swap_red_blue(&source, &mut direct);
            swap_red_blue(&mut reference);
            assert_eq!(direct, reference);
        }
    }
}

use crate::config::VideoEncodeConfig;

/// Convert a user-facing quality slider (0..=100) into an H.264 CRF target.
/// Higher quality means lower CRF and larger output bitrate.
pub(crate) fn quality_to_h264_crf(quality: u8) -> u8 {
    let q = quality.min(100) as f64 / 100.0;
    let curved = q.powf(1.20);
    (35.0 - 22.0 * curved).round().clamp(10.0, 35.0) as u8
}

pub(crate) fn smart_quality_bitrate_kbps(
    width: u32,
    height: u32,
    fps: u32,
    quality: u8,
    lossless_mode: bool,
) -> u32 {
    let w = width.max(1) as f64;
    let h = height.max(1) as f64;
    let f = fps.max(1) as f64;
    let q = quality.min(100) as f64 / 100.0;
    let q_curve = q.powf(1.35);

    // 1080p30 reference ladder roughly aligned with mainstream screen recorders:
    // q=20 ~5 Mbps, q=50 ~9 Mbps, q=80 ~14 Mbps, q=100 ~18 Mbps.
    let base_1080p30_kbps = 3_500.0 + 14_500.0 * q_curve;
    let pixels = w * h;
    let pixel_scale = (pixels / (1920.0 * 1080.0)).powf(0.92);
    let fps_scale = (f / 30.0).powf(0.85);

    let mut kbps = base_1080p30_kbps * pixel_scale * fps_scale;

    // Keep very low resolution / low fps outputs from starving.
    let minimum_kbps = ((pixels * f * 0.028) / 1000.0).clamp(600.0, 6_000.0);
    kbps = kbps.max(minimum_kbps);

    // Lossless mode should not be constrained too aggressively by nominal quality.
    if lossless_mode {
        let lossless_floor = ((pixels * f * 0.20) / 1000.0).clamp(12_000.0, 180_000.0);
        kbps = kbps.max(lossless_floor);
    }

    kbps.round().clamp(600.0, 240_000.0) as u32
}

pub fn smart_quality_bitrate_bps(
    width: u32,
    height: u32,
    fps: u32,
    video: &VideoEncodeConfig,
    lossless_mode: bool,
) -> usize {
    let kbps = smart_quality_bitrate_kbps(width, height, fps, video.quality, lossless_mode);
    u64::from(kbps).saturating_mul(1_000).min(usize::MAX as u64) as usize
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quality_to_crf_is_monotonic() {
        assert!(quality_to_h264_crf(10) > quality_to_h264_crf(50));
        assert!(quality_to_h264_crf(50) > quality_to_h264_crf(90));
    }

    #[test]
    fn bitrate_grows_with_quality_and_fps() {
        let low = smart_quality_bitrate_kbps(1920, 1080, 30, 30, false);
        let high = smart_quality_bitrate_kbps(1920, 1080, 30, 90, false);
        assert!(high > low);

        let slow_fps = smart_quality_bitrate_kbps(1920, 1080, 30, 80, false);
        let fast_fps = smart_quality_bitrate_kbps(1920, 1080, 60, 80, false);
        assert!(fast_fps > slow_fps);
    }
}

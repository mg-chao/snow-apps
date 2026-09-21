//! Native keycap geometry in pixels at unit scale, shared by all text adapters.
pub const HEIGHT: u32 = 64;
pub const FONT_SIZE: f32 = 32.0;

/// Compact long legends before rasterizing, preserving equal horizontal padding.
/// Native adapters measure at FONT_SIZE, then apply the returned font size.
pub fn fit(measured: f32) -> (u32, f32) {
    let measured = measured.max(1.0);
    let content = if measured <= FONT_SIZE {
        measured
    } else {
        FONT_SIZE * (measured / FONT_SIZE).powf(0.85)
    };
    (
        (content + 40.0).ceil() as u32,
        FONT_SIZE * content / measured,
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn short_legends_keep_font_size_and_long_legends_compact() {
        assert_eq!(fit(24.0), (64, 32.0));
        assert_eq!(fit(32.0), (72, 32.0));
        let (width, font) = fit(128.0);
        assert_eq!(width, 144);
        assert!(font < FONT_SIZE);
        assert!((128.0 * font / FONT_SIZE + 40.0 - width as f32).abs() < 1.0);
    }
}

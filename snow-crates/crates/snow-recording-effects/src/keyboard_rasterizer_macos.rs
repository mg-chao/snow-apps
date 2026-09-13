use std::ffi::{CString, c_char};

use super::*;
use crate::keyboard_overlay::{KEYCAP_SIZE, Keycap};

unsafe extern "C" {
    fn snow_recording_keycap(
        label: *const c_char,
        scale: f32,
        background: *const u8,
        foreground: *const u8,
        border: *const u8,
        width: *mut u32,
        height: *mut u32,
        pixels: *mut u8,
        capacity: usize,
    ) -> i32;
}

struct Rasterizer {
    config: KeyboardOverlayConfig,
}

pub fn create(config: &KeyboardOverlayConfig) -> Result<Box<dyn KeycapRasterizer>, String> {
    let mut rasterizer = Rasterizer {
        config: config.clone(),
    };
    rasterizer.rasterize("M", config.keycap_size as f32 / KEYCAP_SIZE as f32)?;
    Ok(Box::new(rasterizer))
}

impl KeycapRasterizer for Rasterizer {
    fn rasterize(&mut self, label: &str, scale: f32) -> Result<Keycap, String> {
        if !scale.is_finite() {
            return Err("Invalid keyboard keycap scale".into());
        }
        let scale = scale.clamp(0.5, 2.0);
        let label = CString::new(label).map_err(|_| "Invalid keyboard label")?;
        let mut width = 0;
        let mut height = 0;
        let measure = unsafe {
            snow_recording_keycap(
                label.as_ptr(),
                scale,
                self.config.background_rgba.as_ptr(),
                self.config.text_rgba.as_ptr(),
                self.config.border_rgba.as_ptr(),
                &mut width,
                &mut height,
                std::ptr::null_mut(),
                0,
            )
        };
        if measure == 0 || width == 0 || height == 0 || width > 16384 || height > 128 {
            return Err("Cannot measure keyboard keycap".into());
        }
        let length = width as usize * height as usize * 4;
        let mut pixels = vec![0; length];
        let render = unsafe {
            snow_recording_keycap(
                label.as_ptr(),
                scale,
                self.config.background_rgba.as_ptr(),
                self.config.text_rgba.as_ptr(),
                self.config.border_rgba.as_ptr(),
                &mut width,
                &mut height,
                pixels.as_mut_ptr(),
                pixels.len(),
            )
        };
        if render == 0 || width as usize * height as usize * 4 != length {
            return Err("Cannot render keyboard keycap".into());
        }
        Ok(Keycap {
            width,
            height,
            pixels,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config() -> KeyboardOverlayConfig {
        KeyboardOverlayConfig {
            keycap_size: 64,
            background_rgba: [10, 20, 30, 180],
            text_rgba: [250, 250, 250, 255],
            border_rgba: [80, 90, 100, 180],
            labels: Default::default(),
        }
    }

    #[test]
    fn macos_keycaps_have_bounded_dimensions_and_premultiplied_rgba() {
        let mut rasterizer = create(&config()).unwrap();
        for size in [32, 64, 96, 128] {
            for label in ["A", "Cmd", "Option", "Backspace", "Й", "中"] {
                let cap = rasterizer.rasterize(label, size as f32 / 64.0).unwrap();
                assert_eq!(cap.height, size);
                assert_eq!(
                    cap.pixels.len(),
                    cap.width as usize * cap.height as usize * 4
                );
                assert_eq!(cap.pixels[3], 0, "rounded corner must stay transparent");
                assert!(cap.pixels.chunks_exact(4).any(|pixel| pixel[0] > 100));
                assert!(
                    cap.pixels
                        .chunks_exact(4)
                        .all(|pixel| pixel[..3].iter().all(|&channel| channel <= pixel[3]))
                );
            }
        }
    }

    #[test]
    fn macos_keycap_labels_are_rendered_and_long_legends_grow_sublinearly() {
        let mut rasterizer = create(&config()).unwrap();
        let widths: Vec<_> = ["WW", "WWWW", "WWWWWW"]
            .into_iter()
            .map(|label| rasterizer.rasterize(label, 1.0).unwrap().width)
            .collect();
        assert!(widths[0] < widths[1] && widths[1] < widths[2]);
        assert!(widths[2] - widths[1] < widths[1] - widths[0]);
        assert_ne!(
            rasterizer.rasterize("O", 1.0).unwrap().pixels,
            rasterizer.rasterize("Q", 1.0).unwrap().pixels
        );
        assert!(rasterizer.rasterize("A", f32::NAN).is_err());
        assert!(rasterizer.rasterize("A\0B", 1.0).is_err());
    }

    #[test]
    fn macos_keycap_glyphs_use_top_left_pixel_row_order() {
        let mut rasterizer = create(&config()).unwrap();
        let cap = rasterizer.rasterize("L", 2.0).unwrap();
        let rows: Vec<_> = cap
            .pixels
            .chunks_exact(cap.width as usize * 4)
            .map(|row| row.chunks_exact(4).filter(|pixel| pixel[0] > 200).count())
            .collect();
        let top = rows.iter().position(|&count| count > 0).unwrap();
        let bottom = rows.iter().rposition(|&count| count > 0).unwrap();
        let quarter = ((bottom - top + 1) / 4).max(1);
        let top_ink: usize = rows[top..top + quarter].iter().sum();
        let bottom_ink: usize = rows[bottom + 1 - quarter..=bottom].iter().sum();
        assert!(
            bottom_ink > top_ink * 2,
            "L's horizontal foot must be below its vertical stem"
        );
    }
}

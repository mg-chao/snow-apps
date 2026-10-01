//! Sparse playback overlays in final output pixels. Text glyphs are rasterized once.
use std::collections::BTreeMap;

use snow_recording_model::{FinalizedTimeline, PlaybackOverlay, RenderFrame};

use crate::keyboard_overlay::{Keycap, KeycapRasterizer, blend_keycap_to};
use crate::surface::Surface;

const PROGRESS_BAR_HEIGHT: u32 = 12;
const TIMER_FONT_SIZE: f32 = 24.0;
const TIMER_MARGIN: u32 = 20;

pub fn progress_bar_height(height: u32) -> u32 {
    PROGRESS_BAR_HEIGHT.min(height)
}

/// The filled width is also the cache key for a static source with a progress overlay.
pub fn progress_bar_width(width: u32, progress: f64) -> u32 {
    if !progress.is_finite() {
        return 0;
    }
    (f64::from(width) * progress.clamp(0.0, 1.0)).floor() as u32
}

pub fn draw_progress_bar(surface: &mut impl Surface, progress: f64, rgba: [u8; 4]) {
    let (width, height) = surface.size();
    let top = height - progress_bar_height(height);
    let filled = progress_bar_width(width, progress);
    // Only the uncovered track is blended: translucent fill retains its chosen opacity.
    for y in top..height {
        fill_span(surface, 0, y, filled, rgba);
        fill_span(surface, filled, y, width - filled, [0, 0, 0, 64]);
    }
}

fn fill_span<S: Surface>(surface: &mut S, x: u32, y: u32, width: u32, color: [u8; 4]) {
    if width == 0 || color[3] == 0 {
        return;
    }
    surface.span(x, y, width, |pixels, _| {
        let alpha = u32::from(color[3]);
        for pixel in pixels.chunks_exact_mut(4) {
            for channel in 0..3 {
                pixel[channel] = ((u32::from(color[channel]) * alpha
                    + u32::from(pixel[channel]) * (255 - alpha)
                    + 127)
                    / 255) as u8;
            }
            pixel[3] = if S::OPAQUE {
                255
            } else {
                (alpha + (u32::from(pixel[3]) * (255 - alpha) + 127) / 255) as u8
            };
        }
    });
}

pub fn format_playback_time(elapsed_ms: u64, total_ms: u64) -> String {
    let total = total_ms / 1000;
    let elapsed = elapsed_ms.min(total_ms) / 1000;
    if total >= 3600 {
        format!(
            "{}:{:02}:{:02} / {}:{:02}:{:02}",
            elapsed / 3600,
            elapsed / 60 % 60,
            elapsed % 60,
            total / 3600,
            total / 60 % 60,
            total % 60
        )
    } else {
        format!(
            "{:02}:{:02} / {:02}:{:02}",
            elapsed / 60,
            elapsed % 60,
            total / 60,
            total % 60
        )
    }
}

pub struct PlaybackRenderer {
    overlay: PlaybackOverlay,
    output: (u32, u32),
    glyphs: BTreeMap<char, Keycap>,
    glyph_top: u32,
    glyph_height: u32,
    glyph_y: u32,
    advance: u32,
    horizontal_padding: u32,
    margin: u32,
    badge_height: u32,
    radius: f32,
    badge: Option<Keycap>,
    caption: String,
    caption_key: Option<(u64, u64)>,
}

impl PlaybackRenderer {
    pub fn new(
        overlay: PlaybackOverlay,
        output: (u32, u32),
        mut rasterizer: Option<Box<dyn KeycapRasterizer>>,
    ) -> Result<Self, String> {
        let font = TIMER_FONT_SIZE;
        let padding = (font / 3.0).round() as u32;
        let mut result = Self {
            overlay,
            output,
            glyphs: BTreeMap::new(),
            glyph_top: u32::MAX,
            glyph_height: 0,
            glyph_y: 0,
            advance: 0,
            horizontal_padding: (font / 2.0).round() as u32,
            margin: TIMER_MARGIN,
            badge_height: font as u32 + 2 * padding,
            radius: (font / 3.0).round(),
            badge: None,
            caption: String::new(),
            caption_key: None,
        };
        if matches!(overlay, PlaybackOverlay::PlaybackTime { .. }) {
            let rasterizer = rasterizer
                .as_mut()
                .ok_or("playback time requires a text rasterizer")?;
            let mut bottom = 0;
            let mut digit_top = u32::MAX;
            let mut digit_bottom = 0;
            for character in "0123456789:/".chars() {
                let cap = rasterizer.rasterize_glyph(&character.to_string(), font)?;
                validate_cap(&cap)?;
                let mut left = cap.width;
                let mut right = 0;
                for (index, pixel) in cap.pixels.chunks_exact(4).enumerate() {
                    if pixel[3] != 0 {
                        let x = index as u32 % cap.width;
                        let y = index as u32 / cap.width;
                        left = left.min(x);
                        right = right.max(x + 1);
                        result.glyph_top = result.glyph_top.min(y);
                        bottom = bottom.max(y + 1);
                        if character.is_ascii_digit() {
                            digit_top = digit_top.min(y);
                            digit_bottom = digit_bottom.max(y + 1);
                        }
                    }
                }
                if left == cap.width {
                    return Err("playback time glyph is empty".into());
                }
                let width = right - left;
                result.advance = result.advance.max(width);
                let mut pixels = vec![0; width as usize * cap.height as usize * 4];
                for y in 0..cap.height as usize {
                    let source = (y * cap.width as usize + left as usize) * 4;
                    let destination = y * width as usize * 4;
                    pixels[destination..destination + width as usize * 4]
                        .copy_from_slice(&cap.pixels[source..source + width as usize * 4]);
                }
                result.glyphs.insert(
                    character,
                    Keycap {
                        width,
                        height: cap.height,
                        pixels,
                    },
                );
            }
            result.glyph_height = bottom - result.glyph_top;
            result.advance += (font / 12.0).round().max(1.0) as u32;
            // Center the numeral body, keeping a stable baseline as the timer ticks.
            // A slash can extend below that body; its overhang needs padding without
            // pulling the numerals upward. Keep every glyph on the same raster origin.
            let digit_height = digit_bottom - digit_top;
            let above = digit_top - result.glyph_top;
            let below = bottom - digit_bottom;
            result.badge_height = result
                .badge_height
                .max(digit_height + 2 * (padding + above.max(below)));
            result.glyph_y = (result.badge_height - digit_height) / 2 - above;
        }
        Ok(result)
    }

    pub fn top_reservation(&self) -> u32 {
        if matches!(self.overlay, PlaybackOverlay::PlaybackTime { .. }) {
            let gap = (self.radius / 2.0).round() as u32;
            (self.margin + self.badge_height + gap).min(self.output.1)
        } else {
            0
        }
    }

    pub fn caption(&self) -> &str {
        &self.caption
    }

    pub fn draw_to(
        &mut self,
        surface: &mut impl Surface,
        frame: RenderFrame,
        timeline: Option<FinalizedTimeline>,
    ) -> Result<(), String> {
        match self.overlay {
            PlaybackOverlay::None => {}
            PlaybackOverlay::ProgressBar { rgba } => {
                draw_progress_bar(surface, frame.progress, rgba)
            }
            PlaybackOverlay::PlaybackTime { .. } => {
                let timeline = timeline.ok_or("playback time requires a finalized timeline")?;
                let elapsed = if frame.index + 1 >= timeline.frame_count() {
                    timeline.duration_ms()
                } else {
                    frame.timestamp_ms.min(timeline.duration_ms())
                };
                let key = (elapsed / 1000, timeline.duration_ms() / 1000);
                if self.caption_key != Some(key) {
                    self.caption = format_playback_time(elapsed, timeline.duration_ms());
                    self.update_badge()?;
                    self.caption_key = Some(key);
                }
                let badge = self.badge.as_ref().expect("prepared playback badge");
                // Keep pixel geometry even on narrow recordings; the surface clips the badge.
                let x = self.output.0 as f32 - self.margin as f32 - badge.width as f32;
                blend_keycap_to(surface, badge, x, self.margin as f32, 1.0, 1.0);
            }
        }
        Ok(())
    }

    fn update_badge(&mut self) -> Result<(), String> {
        let width =
            self.caption
                .chars()
                .try_fold(2 * self.horizontal_padding, |width, character| {
                    width
                        .checked_add(if character == ' ' {
                            self.advance / 2
                        } else {
                            self.advance
                        })
                        .ok_or_else(|| "playback badge width overflow".to_string())
                })?;
        let badge = self.badge.get_or_insert_with(|| Keycap {
            width,
            height: self.badge_height,
            pixels: Vec::new(),
        });
        badge.width = width;
        badge.height = self.badge_height;
        badge
            .pixels
            .resize(width as usize * badge.height as usize * 4, 0);
        let border = (self.radius / 8.0).max(1.0);
        for y in 0..badge.height {
            for x in 0..width {
                let outer = rounded_coverage(
                    x as f32 + 0.5,
                    y as f32 + 0.5,
                    width as f32,
                    badge.height as f32,
                    self.radius,
                );
                let inner = rounded_coverage(
                    x as f32 + 0.5 - border,
                    y as f32 + 0.5 - border,
                    width as f32 - 2.0 * border,
                    badge.height as f32 - 2.0 * border,
                    self.radius - border,
                );
                let offset = ((y * width + x) * 4) as usize;
                let pixel = &mut badge.pixels[offset..offset + 4];
                pixel.fill(0);
                composite_pixel(pixel, [24, 27, 33, 216], outer);
                composite_pixel(pixel, [255, 255, 255, 40], outer.saturating_sub(inner));
            }
        }
        let PlaybackOverlay::PlaybackTime { rgba } = self.overlay else {
            return Err("playback badge requires a time overlay".into());
        };
        let mut x = self.horizontal_padding;
        let top = self.glyph_y;
        let mut secondary = false;
        for character in self.caption.chars() {
            if character == ' ' {
                x += self.advance / 2;
                continue;
            }
            let glyph = self
                .glyphs
                .get(&character)
                .ok_or("unsupported playback-time glyph")?;
            let glyph_x = x + (self.advance - glyph.width) / 2;
            let rows = glyph
                .height
                .saturating_sub(self.glyph_top)
                .min(self.glyph_height);
            // Keep the changing elapsed time prominent; the total is quiet context.
            let emphasis = if character == '/' {
                secondary = true;
                128
            } else if secondary {
                180
            } else {
                255
            };
            let foreground = [rgba[0], rgba[1], rgba[2], multiply_alpha(rgba[3], emphasis)];
            for y in 0..rows {
                for column in 0..glyph.width {
                    let source = (((y + self.glyph_top) * glyph.width + column) * 4) as usize;
                    let destination = (((y + top) * width + glyph_x + column) * 4) as usize;
                    composite_pixel(
                        &mut badge.pixels[destination..destination + 4],
                        foreground,
                        glyph.pixels[source + 3],
                    );
                }
            }
            x += self.advance;
        }
        Ok(())
    }
}

fn multiply_alpha(alpha: u8, coverage: u8) -> u8 {
    ((u32::from(alpha) * u32::from(coverage) + 127) / 255) as u8
}

/// Compose a straight RGBA color over a premultiplied badge pixel.
fn composite_pixel(destination: &mut [u8], color: [u8; 4], coverage: u8) {
    let alpha = u32::from(multiply_alpha(color[3], coverage));
    for channel in 0..3 {
        destination[channel] = ((u32::from(color[channel]) * alpha
            + u32::from(destination[channel]) * (255 - alpha)
            + 127)
            / 255) as u8;
    }
    destination[3] = (alpha + (u32::from(destination[3]) * (255 - alpha) + 127) / 255) as u8;
}

fn rounded_coverage(x: f32, y: f32, width: f32, height: f32, radius: f32) -> u8 {
    let dx = (x - width / 2.0).abs() - (width / 2.0 - radius);
    let dy = (y - height / 2.0).abs() - (height / 2.0 - radius);
    let distance = dx.max(0.0).hypot(dy.max(0.0)) + dx.max(dy).min(0.0) - radius;
    ((0.5 - distance).clamp(0.0, 1.0) * 255.0).round() as u8
}

fn validate_cap(cap: &Keycap) -> Result<(), String> {
    if cap.width == 0
        || cap.height == 0
        || cap.width > 4096
        || cap.height > 512
        || cap.pixels.len() != cap.width as usize * cap.height as usize * 4
    {
        return Err("invalid playback glyph bitmap".into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::surface::{RgbaSurface, TILE_SIZE, TileSurface};

    struct Glyph;
    impl KeycapRasterizer for Glyph {
        fn rasterize(&mut self, label: &str, _: f32) -> Result<Keycap, String> {
            let width = if label == "1" { 2 } else { 4 };
            Ok(Keycap {
                width,
                height: 12,
                pixels: [255; 4].repeat(width as usize * 12),
            })
        }
    }

    fn time_renderer(size: (u32, u32)) -> PlaybackRenderer {
        PlaybackRenderer::new(
            PlaybackOverlay::PlaybackTime { rgba: [255; 4] },
            size,
            Some(Box::new(Glyph)),
        )
        .unwrap()
    }

    fn opaque_numeral_rows(badge: &Keycap) -> (u32, u32) {
        let mut top = badge.height;
        let mut bottom = 0;
        for (index, pixel) in badge.pixels.chunks_exact(4).enumerate() {
            if pixel == [255; 4] {
                let y = index as u32 / badge.width;
                top = top.min(y);
                bottom = bottom.max(y + 1);
            }
        }
        assert!(top < bottom, "elapsed numerals must be visible");
        (top, bottom)
    }

    #[test]
    fn numeral_centering_preserves_separator_baseline_and_padding() {
        struct BaselineGlyphs {
            separator: (u32, u32),
        }
        impl KeycapRasterizer for BaselineGlyphs {
            fn rasterize(&mut self, label: &str, _: f32) -> Result<Keycap, String> {
                let (top, bottom) = match label {
                    "/" => self.separator,
                    ":" => (24, 30),
                    _ => (20, 32),
                };
                let mut cap = Keycap {
                    width: 8,
                    height: 56,
                    pixels: vec![0; 8 * 56 * 4],
                };
                for y in top..bottom {
                    for x in 2..6 {
                        let offset = ((y * cap.width + x) * 4) as usize;
                        cap.pixels[offset..offset + 4].fill(255);
                    }
                }
                Ok(cap)
            }
        }

        // Transparent font leading is asymmetric; slash extents can lie on either
        // side of the numeral body. Neither should move the numeral center.
        for separator in [(20, 40), (12, 32), (12, 40)] {
            let timeline = FinalizedTimeline::new(3_665_000, 30).unwrap();
            let mut renderer = PlaybackRenderer::new(
                PlaybackOverlay::PlaybackTime { rgba: [255; 4] },
                (320, 240),
                Some(Box::new(BaselineGlyphs { separator })),
            )
            .unwrap();
            let mut previous_rows = None;
            for index in [0, 30, 65 * 30, timeline.frame_count() - 1] {
                renderer
                    .draw_to(
                        &mut TileSurface::new(renderer.output),
                        timeline.frame(index).unwrap(),
                        Some(timeline),
                    )
                    .unwrap();
                let badge = renderer.badge.as_ref().unwrap();
                let rows = opaque_numeral_rows(badge);
                assert!(
                    rows.0.abs_diff(badge.height - rows.1) <= 1,
                    "numerals must be vertically centered with separator {separator:?}: {rows:?}"
                );
                if let Some(previous) = previous_rows {
                    assert_eq!(rows, previous, "ticking must preserve the numeral baseline");
                }
                previous_rows = Some(rows);

                let separator_top = (i64::from(rows.0) + i64::from(separator.0) - 20) as u32;
                let separator_bottom = separator_top + separator.1 - separator.0;
                assert!(separator_top >= 8 && separator_bottom <= badge.height - 8);
                let separator_x = renderer.horizontal_padding
                    + renderer
                        .caption
                        .chars()
                        .take_while(|c| *c != '/')
                        .map(|c| {
                            if c == ' ' {
                                renderer.advance / 2
                            } else {
                                renderer.advance
                            }
                        })
                        .sum::<u32>()
                    + (renderer.advance - 4) / 2;
                for y in separator_top..separator_bottom {
                    let offset = ((y * badge.width + separator_x) * 4) as usize;
                    assert_eq!(
                        &badge.pixels[offset..offset + 4],
                        &[138, 139, 142, 236],
                        "the complete separator must retain its baseline and coverage"
                    );
                }
            }
        }
    }

    #[cfg(any(windows, target_os = "macos"))]
    #[test]
    fn native_playback_numerals_are_vertically_centered() {
        let style = crate::keyboard_overlay::KeyboardOverlayConfig {
            font: None,
            keycap_size: 64,
            background_rgba: [0; 4],
            text_rgba: [255; 4],
            border_rgba: [0; 4],
            labels: Default::default(),
        };
        let rasterizer = crate::keyboard_rasterizer::create(&style).unwrap();
        let mut renderer = PlaybackRenderer::new(
            PlaybackOverlay::PlaybackTime { rgba: [255; 4] },
            (320, 240),
            Some(rasterizer),
        )
        .unwrap();
        for duration in [125_000, 3_665_000] {
            let timeline = FinalizedTimeline::new(duration, 30).unwrap();
            for index in [0, 30, 65 * 30, timeline.frame_count() - 1] {
                renderer
                    .draw_to(
                        &mut TileSurface::new(renderer.output),
                        timeline.frame(index).unwrap(),
                        Some(timeline),
                    )
                    .unwrap();
                let badge = renderer.badge.as_ref().unwrap();
                let (top, bottom) = opaque_numeral_rows(badge);
                assert!(
                    top.abs_diff(badge.height - bottom) <= 1,
                    "native numerals in {} must have equal vertical padding: {top} vs {}",
                    renderer.caption(),
                    badge.height - bottom
                );
            }
        }
    }

    #[test]
    fn fixed_pixel_playback_styles_do_not_depend_on_output_dimensions() {
        struct FixedFont;
        impl KeycapRasterizer for FixedFont {
            fn rasterize(&mut self, _: &str, scale: f32) -> Result<Keycap, String> {
                assert_eq!(scale, 24.0 / 32.0, "timer font must remain 24 pixels");
                Ok(Keycap {
                    width: 12,
                    height: 24,
                    pixels: [255; 4].repeat(12 * 24),
                })
            }
        }
        let timeline = FinalizedTimeline::new(125_000, 30).unwrap();
        let mut reference = None;
        for size in [
            (640, 480),
            (1280, 720),
            (1920, 1080),
            (3840, 2160),
            (1080, 1920),
        ] {
            assert_eq!(progress_bar_height(size.1), 12);
            let mut renderer = PlaybackRenderer::new(
                PlaybackOverlay::PlaybackTime { rgba: [255; 4] },
                size,
                Some(Box::new(FixedFont)),
            )
            .unwrap();
            let mut tiles = TileSurface::new(size);
            renderer
                .draw_to(&mut tiles, timeline.frame(0).unwrap(), Some(timeline))
                .unwrap();
            let badge = renderer.badge.as_ref().unwrap();
            assert_eq!(renderer.margin, 20);
            assert_eq!(badge.height, 40);
            let style = (badge.width, badge.height, badge.pixels.clone());
            if let Some(reference) = &reference {
                assert_eq!(&style, reference);
            } else {
                reference = Some(style);
            }
        }
    }

    #[test]
    fn time_badge_keeps_fixed_top_right_inset_and_clips_small_frames() {
        let timeline = FinalizedTimeline::new(3_600_000, 30).unwrap();
        for size in [
            (1920, 1080),
            (3840, 2160),
            (100, 240),
            (8, 24),
            (16, 22),
            (1, 1),
        ] {
            let mut renderer = time_renderer(size);
            let mut pixels = [48, 96, 144, 255].repeat(size.0 as usize * size.1 as usize);
            renderer
                .draw_to(
                    &mut RgbaSurface {
                        pixels: &mut pixels,
                        dimensions: size,
                    },
                    timeline.frame(0).unwrap(),
                    Some(timeline),
                )
                .unwrap();
            let changed: Vec<_> = pixels
                .chunks_exact(4)
                .enumerate()
                .filter(|(_, pixel)| *pixel != [48, 96, 144, 255])
                .map(|(index, _)| (index as u32 % size.0, index as u32 / size.0))
                .collect();
            let badge = renderer.badge.as_ref().unwrap();
            if size.0 <= 20 || size.1 <= 20 {
                assert!(changed.is_empty(), "fixed inset is outside {size:?}");
                continue;
            }
            assert!(!changed.is_empty(), "visible badge at {size:?}");
            let left = changed.iter().map(|(x, _)| *x).min().unwrap();
            let right = changed.iter().map(|(x, _)| *x).max().unwrap();
            let top = changed.iter().map(|(_, y)| *y).min().unwrap();
            let bottom = changed.iter().map(|(_, y)| *y).max().unwrap();
            assert_eq!(right, size.0 - 20 - 1);
            assert_eq!(left, size.0.saturating_sub(20 + badge.width));
            assert_eq!(top, 20, "top-right anchor at {size:?}");
            assert_eq!(bottom, (20 + badge.height).min(size.1) - 1);
            // Compare the cropped pixels to the full badge so fitting cannot resize the glyphs.
            let badge_x = i64::from(size.0) - 20 - i64::from(badge.width);
            for (x, y) in changed {
                let source =
                    (((y - 20) * badge.width + (i64::from(x) - badge_x) as u32) * 4) as usize;
                let destination = ((y * size.0 + x) * 4) as usize;
                let alpha = u32::from(badge.pixels[source + 3]);
                for (channel, background) in [48, 96, 144].into_iter().enumerate() {
                    let expected = u32::from(badge.pixels[source + channel])
                        + (background * (255 - alpha) + 127) / 255;
                    assert_eq!(u32::from(pixels[destination + channel]), expected);
                }
            }
        }
    }

    #[test]
    fn rounded_badge_has_transparent_corners_and_quieter_total_time() {
        let timeline = FinalizedTimeline::new(125_000, 30).unwrap();
        let mut renderer = time_renderer((1920, 1080));
        renderer
            .draw_to(
                &mut TileSurface::new(renderer.output),
                timeline.frame(0).unwrap(),
                Some(timeline),
            )
            .unwrap();
        let badge = renderer.badge.as_ref().unwrap();
        assert_eq!(&badge.pixels[..4], &[0; 4]);
        assert!(
            badge
                .pixels
                .chunks_exact(4)
                .any(|pixel| pixel[3] > 0 && pixel[3] < 216)
        );
        assert!(
            badge
                .pixels
                .chunks_exact(4)
                .all(|pixel| { pixel[..3].iter().all(|channel| *channel <= pixel[3]) })
        );
        let text_pixel = |character_index: u32| {
            let advance_before = renderer
                .caption
                .chars()
                .take(character_index as usize)
                .map(|character| {
                    if character == ' ' {
                        renderer.advance / 2
                    } else {
                        renderer.advance
                    }
                })
                .sum::<u32>();
            let x = renderer.horizontal_padding + advance_before + (renderer.advance - 4) / 2;
            let y = (badge.height - renderer.glyph_height) / 2;
            let offset = ((y * badge.width + x) * 4) as usize;
            &badge.pixels[offset..offset + 4]
        };
        let elapsed = text_pixel(0);
        let separator = text_pixel(6);
        let total = text_pixel(8);
        assert_eq!(elapsed, [255; 4]);
        assert!(elapsed[0] > total[0] && total[0] > separator[0]);
        let center = ((badge.height / 2 * badge.width) * 4) as usize;
        let edge = &badge.pixels[center..center + 4];
        let background = &badge.pixels[center + 2 * 4..center + 3 * 4];
        assert!(
            edge[0] > background[0],
            "subtle border separates badge from dark content"
        );
    }

    #[test]
    fn antialiased_glyphs_blend_over_background_and_sparse_output_matches_video() {
        struct AntialiasedGlyph;
        impl KeycapRasterizer for AntialiasedGlyph {
            fn rasterize(&mut self, _: &str, _: f32) -> Result<Keycap, String> {
                let mut pixels = [128; 4].repeat(4 * 12);
                pixels[20..24].fill(0);
                Ok(Keycap {
                    width: 4,
                    height: 12,
                    pixels,
                })
            }
        }
        let size = (320, 240);
        let timeline = FinalizedTimeline::new(125_000, 30).unwrap();
        let mut renderer = PlaybackRenderer::new(
            PlaybackOverlay::PlaybackTime { rgba: [255; 4] },
            size,
            Some(Box::new(AntialiasedGlyph)),
        )
        .unwrap();
        let frame = timeline.frame(0).unwrap();
        let mut tiles = TileSurface::new(size);
        renderer.draw_to(&mut tiles, frame, Some(timeline)).unwrap();
        let badge = renderer.badge.as_ref().unwrap();
        let x = renderer.horizontal_padding + (renderer.advance - 4) / 2;
        let y = (badge.height - renderer.glyph_height) / 2;
        let offset = ((y * badge.width + x) * 4) as usize;
        assert_eq!(&badge.pixels[offset..offset + 4], &[138, 139, 142, 236]);
        let transparent = (((y + 1) * badge.width + x + 1) * 4) as usize;
        assert_eq!(
            &badge.pixels[transparent..transparent + 4],
            &[20, 23, 28, 216]
        );
        for background in [[0, 0, 0, 255], [255; 4]] {
            let mut pixels = background.repeat(size.0 as usize * size.1 as usize);
            renderer
                .draw_to(
                    &mut RgbaSurface {
                        pixels: &mut pixels,
                        dimensions: size,
                    },
                    frame,
                    Some(timeline),
                )
                .unwrap();
            for tile in tiles.snapshot() {
                for (index, source) in tile.pixels.chunks_exact(4).enumerate() {
                    let x = tile.x + index as u32 % TILE_SIZE;
                    let y = tile.y + index as u32 / TILE_SIZE;
                    if x >= size.0 || y >= size.1 {
                        continue;
                    }
                    let destination = ((y * size.0 + x) * 4) as usize;
                    for channel in 0..3 {
                        let expected = u32::from(source[channel])
                            + (u32::from(background[channel]) * (255 - u32::from(source[3])) + 127)
                                / 255;
                        assert_eq!(u32::from(pixels[destination + channel]), expected);
                    }
                }
            }
        }
    }

    #[test]
    fn caption_cache_and_digit_spacing_keep_the_badge_steady() {
        let timeline = FinalizedTimeline::new(125_000, 30).unwrap();
        let mut renderer = time_renderer((320, 240));
        let mut tiles = TileSurface::new(renderer.output);
        renderer
            .draw_to(&mut tiles, timeline.frame(0).unwrap(), Some(timeline))
            .unwrap();
        let first_badge = renderer.badge.as_ref().unwrap();
        let first_pixels = first_badge.pixels.clone();
        let allocation = first_badge.pixels.as_ptr();
        let width = first_badge.width;
        renderer
            .draw_to(&mut tiles, timeline.frame(15).unwrap(), Some(timeline))
            .unwrap();
        assert_eq!(renderer.badge.as_ref().unwrap().pixels, first_pixels);
        assert_eq!(renderer.badge.as_ref().unwrap().pixels.as_ptr(), allocation);
        renderer
            .draw_to(&mut tiles, timeline.frame(30).unwrap(), Some(timeline))
            .unwrap();
        assert_eq!(renderer.caption(), "00:01 / 02:05");
        assert_eq!(renderer.badge.as_ref().unwrap().width, width);
        assert_eq!(renderer.badge.as_ref().unwrap().pixels.as_ptr(), allocation);
        assert_ne!(renderer.badge.as_ref().unwrap().pixels, first_pixels);
        renderer
            .draw_to(
                &mut tiles,
                timeline.frame(timeline.frame_count() - 1).unwrap(),
                Some(timeline),
            )
            .unwrap();
        assert_eq!(renderer.caption(), "02:05 / 02:05");
    }

    #[test]
    fn progress_has_exact_endpoints_and_preserves_custom_alpha() {
        let mut pixels = vec![200; 12 * 8 * 4];
        draw_progress_bar(
            &mut RgbaSurface {
                pixels: &mut pixels,
                dimensions: (12, 8),
            },
            0.5,
            [100, 0, 0, 128],
        );
        for y in 0..8 {
            for x in 0..12 {
                let expected = if x < 6 {
                    [150, 100, 100, 255]
                } else {
                    [150, 150, 150, 255]
                };
                let offset = (y * 12 + x) * 4;
                assert_eq!(&pixels[offset..offset + 4], &expected, "pixel ({x}, {y})");
            }
        }
        assert_eq!(progress_bar_width(1920, 0.0), 0);
        assert_eq!(progress_bar_width(1920, 1.0), 1920);
        for (height, expected) in [
            (0, 0),
            (1, 1),
            (5, 5),
            (8, 8),
            (480, 12),
            (720, 12),
            (1080, 12),
            (1440, 12),
            (2160, 12),
            (8000, 12),
            (u32::MAX, 12),
        ] {
            assert_eq!(
                progress_bar_height(height),
                expected,
                "output height {height}"
            );
        }
    }
    #[test]
    fn playback_time_uses_total_duration_to_select_hour_format() {
        assert_eq!(format_playback_time(65_000, 125_000), "01:05 / 02:05");
        assert_eq!(format_playback_time(65_000, 3_600_000), "0:01:05 / 1:00:00");
        assert_eq!(format_playback_time(u64::MAX, 125_000), "02:05 / 02:05");
    }

    #[test]
    fn caption_accepts_valid_glyphs_with_different_bitmap_heights() {
        struct VariableGlyphs;
        impl KeycapRasterizer for VariableGlyphs {
            fn rasterize(&mut self, label: &str, _: f32) -> Result<Keycap, String> {
                let height = if label == "0" { 12 } else { 6 };
                Ok(Keycap {
                    width: 4,
                    height,
                    pixels: [255; 4].repeat(4 * height as usize),
                })
            }
        }
        let timeline = FinalizedTimeline::new(2_000, 30).unwrap();
        let mut renderer = PlaybackRenderer::new(
            PlaybackOverlay::PlaybackTime { rgba: [255; 4] },
            (120, 100),
            Some(Box::new(VariableGlyphs)),
        )
        .unwrap();
        let mut pixels = [0, 0, 0, 255].repeat(120 * 100);
        renderer
            .draw_to(
                &mut RgbaSurface {
                    pixels: &mut pixels,
                    dimensions: (120, 100),
                },
                timeline.frame(0).unwrap(),
                Some(timeline),
            )
            .unwrap();
        assert_eq!(renderer.caption(), "00:00 / 00:02");
        assert!(pixels.chunks_exact(4).any(|pixel| pixel[0] != 0));
    }
}

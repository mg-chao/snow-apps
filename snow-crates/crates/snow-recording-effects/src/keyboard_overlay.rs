//! Recording-time key state and bounded, output-pixel overlay composition.
//! Native input and font APIs are adapters; animation uses only explicit timestamps.
use crate::surface::{RgbaSurface, Surface};
use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::sync::Arc;

pub const MOVE_MS: u64 = 180;
pub const HOLD_MS: u64 = 1_200;
pub const FADE_MS: u64 = 400;
/// Native keycaps use fixed output-pixel dimensions in both video and live preview.
pub const KEYCAP_SIZE: u32 = 64;
const KEYCAP_GAP: f32 = 10.0;
const ROW_PITCH: f32 = KEYCAP_SIZE as f32 + 12.0;
const MAX_ROWS: usize = 4;
const MAX_CACHE: usize = 256;
const MAX_CACHE_BYTES: usize = 32 * 1024 * 1024;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct KeyboardOverlayConfig {
    pub background_rgba: [u8; 4],
    pub text_rgba: [u8; 4],
    pub border_rgba: [u8; 4],
    pub labels: BTreeMap<u16, String>,
}

#[derive(Clone, Debug)]
pub struct KeyEvent {
    pub at_ms: u64,
    pub key: u16,
    pub down: bool,
    pub label: String,
    pub modifiers: Vec<(u16, String)>,
}

pub fn modifier(key: u16) -> bool {
    matches!(key, 0x10..=0x12 | 0x5b..=0x5c | 0xa0..=0xa5)
}

fn modifier_order(key: u16) -> u16 {
    match key {
        0x11 | 0xa2 | 0xa3 => 0,
        0x12 | 0xa4 | 0xa5 => 1,
        0x10 | 0xa0 | 0xa1 => 2,
        _ => 3,
    }
}

#[derive(Debug)]
struct Row {
    keys: Vec<(u16, String)>,
    born: u64,
    released: Option<u64>,
    from: f32,
    target: f32,
    moved: u64,
}

impl Row {
    fn y(&self, now: u64) -> f32 {
        let t = (now.saturating_sub(self.moved) as f32 / MOVE_MS as f32).min(1.0);
        self.from + (self.target - self.from) * (1.0 - (1.0 - t).powi(3))
    }

    fn opacity(&self, now: u64) -> f32 {
        self.released.map_or(1.0, |released| {
            1.0 - (now.saturating_sub(released + HOLD_MS) as f32 / FADE_MS as f32).min(1.0)
        })
    }
}

#[derive(Default)]
pub struct KeyboardModel {
    rows: VecDeque<Row>,
    held: BTreeSet<u16>,
    dirty: bool,
}

impl KeyboardModel {
    pub fn reset(&mut self, now: u64) {
        self.held.clear();
        for row in &mut self.rows {
            if row.released.is_none() {
                row.released = Some(now);
            }
        }
        self.dirty = true;
    }

    fn retarget(&mut self, now: u64) {
        for (index, row) in self.rows.iter_mut().rev().enumerate() {
            let target = index as f32;
            if row.target != target {
                row.from = row.y(now);
                row.target = target;
                row.moved = now;
            }
        }
    }

    pub fn event(&mut self, event: KeyEvent) {
        let now = event.at_ms;
        if !event.down {
            self.held.remove(&event.key);
            if let Some(row) = self.rows.back_mut().filter(|row| row.released.is_none()) {
                let non_modifiers: Vec<_> =
                    row.keys.iter().filter(|(key, _)| !modifier(*key)).collect();
                let active = if non_modifiers.is_empty() {
                    !event.modifiers.is_empty()
                } else {
                    non_modifiers.iter().any(|(key, _)| self.held.contains(key))
                };
                if !active {
                    row.released = Some(now);
                    self.dirty = true;
                }
            }
            return;
        }
        if !self.held.insert(event.key) {
            return;
        }
        let mut modifiers = event.modifiers;
        modifiers.sort_by_key(|(key, _)| modifier_order(*key));
        modifiers.dedup_by(|a, b| a.1 == b.1);
        let extend = self.rows.back().is_some_and(|row| row.released.is_none());
        if !extend {
            if self.rows.len() == MAX_ROWS {
                self.rows.pop_front();
            }
            self.rows.push_back(Row {
                keys: vec![],
                born: now,
                released: None,
                from: -0.3,
                target: 0.0,
                moved: now,
            });
        }
        let row = self.rows.back_mut().expect("row was created");
        // Modifier snapshots also collapse Windows' synthetic Ctrl + right Alt into AltGr.
        row.keys.retain(|(key, _)| !modifier(*key));
        modifiers.append(&mut row.keys);
        row.keys = modifiers;
        if !modifier(event.key) && !row.keys.iter().any(|(key, _)| *key == event.key) {
            row.keys.push((event.key, event.label));
        }
        self.dirty = true;
        self.retarget(now);
    }

    pub fn needs_frame(&self, now: u64) -> bool {
        self.dirty
            || self.rows.iter().any(|row| {
                now.saturating_sub(row.moved) < MOVE_MS
                    || row.released.is_some_and(|at| now >= at + HOLD_MS)
            })
    }

    /// Absolute deadline; stable held keys require no timer.
    pub fn next_frame_at(&self, now: u64) -> Option<u64> {
        if self.needs_frame(now) {
            return Some(now.saturating_add(16));
        }
        self.rows
            .iter()
            .filter_map(|row| row.released.map(|at| at.saturating_add(HOLD_MS)))
            .min()
    }

    fn advance(&mut self, now: u64) {
        let previous = self.rows.len();
        self.rows.retain(|row| row.opacity(now) > 0.0);
        if previous != self.rows.len() {
            self.retarget(now);
        }
        self.dirty = false;
    }
}

pub struct Keycap {
    pub width: u32,
    pub height: u32,
    /// Premultiplied RGBA, including rounded background, border and glyphs.
    pub pixels: Vec<u8>,
}

pub trait KeycapRasterizer {
    /// The overlay requests scale 1.0 at every output resolution. The native rasterizer
    /// retains this argument for compatibility, but always produces 64 × 64 keycaps.
    fn rasterize(&mut self, label: &str, scale: f32) -> Result<Keycap, String>;
}

pub struct KeyboardOverlay {
    pub model: KeyboardModel,
    rasterizer: Box<dyn KeycapRasterizer>,
    cache: BTreeMap<String, (Arc<Keycap>, u64)>,
    cache_bytes: usize,
    cache_clock: u64,
    output: (u32, u32),
}

impl KeyboardOverlay {
    pub fn new(output: (u32, u32), rasterizer: Box<dyn KeycapRasterizer>) -> Self {
        Self {
            model: KeyboardModel::default(),
            rasterizer,
            cache: BTreeMap::new(),
            cache_bytes: 0,
            cache_clock: 0,
            output,
        }
    }

    fn keycap(&mut self, label: &str) -> Result<Arc<Keycap>, String> {
        self.cache_clock = self.cache_clock.wrapping_add(1);
        if let Some((cap, used)) = self.cache.get_mut(label) {
            *used = self.cache_clock;
            return Ok(Arc::clone(cap));
        }
        let cap = Arc::new(self.rasterizer.rasterize(label, 1.0)?);
        let bytes = cap.pixels.len();
        if bytes <= MAX_CACHE_BYTES {
            while !self.cache.is_empty()
                && (self.cache.len() >= MAX_CACHE || self.cache_bytes + bytes > MAX_CACHE_BYTES)
            {
                let key = self
                    .cache
                    .iter()
                    .min_by_key(|(_, (_, used))| *used)
                    .unwrap()
                    .0
                    .clone();
                self.cache_bytes -= self.cache.remove(&key).unwrap().0.pixels.len();
            }
            self.cache_bytes += bytes;
            self.cache
                .insert(label.to_owned(), (Arc::clone(&cap), self.cache_clock));
        }
        Ok(cap)
    }

    pub fn draw(&mut self, rgba: &mut [u8], now: u64) -> Result<(), String> {
        self.draw_to(
            &mut RgbaSurface {
                pixels: rgba,
                dimensions: self.output,
            },
            now,
        )
    }

    pub fn draw_to(&mut self, surface: &mut impl Surface, now: u64) -> Result<(), String> {
        self.model.advance(now);
        // Preserve the recording overlay's inset while keeping keys and spacing fixed.
        let margin = (48.0 * (self.output.1 as f32 / 1080.0).clamp(0.5, 4.0))
            .min(self.output.0.min(self.output.1) as f32 * 0.05);
        let row_limit = ((self.output.1 as f32 - 2.0 * margin + ROW_PITCH - KEYCAP_SIZE as f32)
            / ROW_PITCH)
            .floor()
            .max(1.0) as usize;
        for index in (0..self.model.rows.len()).rev().take(row_limit) {
            let row = &self.model.rows[index];
            if row.born > now {
                continue;
            }
            let row_y = row.y(now);
            let opacity = row.opacity(now);
            let labels: Vec<_> = row.keys.iter().map(|(_, label)| label.clone()).collect();
            let caps: Vec<_> = labels
                .iter()
                .map(|label| self.keycap(label))
                .collect::<Result<_, _>>()?;
            let width = caps.iter().map(|cap| cap.width as f32).sum::<f32>()
                + KEYCAP_GAP * caps.len().saturating_sub(1) as f32;
            // Keep the most recent key at the right edge. Narrow outputs clip the chord
            // through the surface instead of shrinking every key below its fixed size.
            let mut x = self.output.0 as f32 - margin - width;
            let bottom = self.output.1 as f32 - margin - row_y * ROW_PITCH;
            for cap in caps {
                blend_keycap_to(surface, &cap, x, bottom - cap.height as f32, 1.0, opacity);
                x += cap.width as f32 + KEYCAP_GAP;
            }
        }
        Ok(())
    }
}

#[cfg(test)]
fn blend_keycap(
    rgba: &mut [u8],
    size: (u32, u32),
    cap: &Keycap,
    x: f32,
    y: f32,
    scale: f32,
    opacity: f32,
) {
    blend_keycap_to(
        &mut RgbaSurface {
            pixels: rgba,
            dimensions: size,
        },
        cap,
        x,
        y,
        scale,
        opacity,
    );
}

fn blend_keycap_to<S: Surface>(
    surface: &mut S,
    cap: &Keycap,
    x: f32,
    y: f32,
    scale: f32,
    opacity: f32,
) {
    let size = surface.size();
    let width = (cap.width as f32 * scale).round().max(1.0) as i32;
    let height = (cap.height as f32 * scale).round().max(1.0) as i32;
    let origin = (x.round() as i32, y.round() as i32);
    let opacity = (opacity * 255.0).round().clamp(0.0, 255.0) as u32;
    let left = (-origin.0).max(0).min(width);
    let right = (size.0 as i32 - origin.0).clamp(left, width);
    let top = (-origin.1).max(0).min(height);
    let bottom = (size.1 as i32 - origin.1).clamp(top, height);
    if left == right || top == bottom {
        return;
    }
    for dy in top..bottom {
        let target_y = origin.1 + dy;
        surface.span(
            (origin.0 + left) as u32,
            target_y as u32,
            (right - left) as u32,
            |destination, offset| {
                if width == cap.width as i32 && height == cap.height as i32 {
                    let start = (dy as usize * cap.width as usize + left as usize + offset) * 4;
                    let source = &cap.pixels[start..start + destination.len()];
                    if S::OPAQUE {
                        blend_span::<true>(destination, source, opacity);
                    } else {
                        blend_span::<false>(destination, source, opacity);
                    }
                } else {
                    for (index, dst) in destination.chunks_exact_mut(4).enumerate() {
                        let dx = left + (offset + index) as i32;
                        let sx = (dx as u64 * u64::from(cap.width) / width as u64) as usize;
                        let sy = (dy as u64 * u64::from(cap.height) / height as u64) as usize;
                        let source = (sy * cap.width as usize + sx) * 4;
                        if S::OPAQUE {
                            blend_faded_pixel::<true>(
                                dst,
                                &cap.pixels[source..source + 4],
                                opacity,
                            );
                        } else {
                            blend_faded_pixel::<false>(
                                dst,
                                &cap.pixels[source..source + 4],
                                opacity,
                            );
                        }
                    }
                }
            },
        );
    }
}

fn blend_span<const OPAQUE: bool>(destination: &mut [u8], source: &[u8], opacity: u32) {
    assert_eq!(destination.len(), source.len());
    #[cfg(target_arch = "x86_64")]
    let mut offset = 0;
    #[cfg(not(target_arch = "x86_64"))]
    let offset = 0;
    // SSE2 is part of the x86-64 baseline. Process four premultiplied pixels together;
    // use unaligned loads because rounded keycap widths need not be multiples of four.
    #[cfg(target_arch = "x86_64")]
    unsafe {
        use std::arch::x86_64::*;
        let zero = _mm_setzero_si128();
        let full = _mm_set1_epi16(255);
        let round = _mm_set1_epi16(127);
        let one = _mm_set1_epi16(1);
        let fade = _mm_set1_epi16(opacity as i16);
        let alpha_mask = _mm_set1_epi32(0xff000000u32 as i32);
        while offset + 16 <= destination.len() {
            let src = _mm_loadu_si128(source.as_ptr().add(offset).cast());
            let dst = _mm_loadu_si128(destination.as_ptr().add(offset).cast());
            let blend = |src16, dst16| {
                let alpha = _mm_shufflehi_epi16::<255>(_mm_shufflelo_epi16::<255>(src16));
                // Exact unsigned division by 255 for the rounded 16-bit product.
                let divide = |product| {
                    _mm_srli_epi16::<8>(_mm_add_epi16(
                        _mm_add_epi16(product, one),
                        _mm_srli_epi16::<8>(product),
                    ))
                };
                if opacity == 255 {
                    let product =
                        _mm_add_epi16(_mm_mullo_epi16(dst16, _mm_sub_epi16(full, alpha)), round);
                    _mm_add_epi16(src16, divide(product))
                } else {
                    let alpha = divide(_mm_add_epi16(_mm_mullo_epi16(alpha, fade), round));
                    let product = _mm_add_epi16(
                        _mm_mullo_epi16(src16, fade),
                        _mm_mullo_epi16(dst16, _mm_sub_epi16(full, alpha)),
                    );
                    divide(_mm_add_epi16(product, round))
                }
            };
            let low = blend(_mm_unpacklo_epi8(src, zero), _mm_unpacklo_epi8(dst, zero));
            let high = blend(_mm_unpackhi_epi8(src, zero), _mm_unpackhi_epi8(dst, zero));
            let packed = _mm_packus_epi16(low, high);
            let result = if OPAQUE {
                let rgb = _mm_andnot_si128(alpha_mask, packed);
                _mm_or_si128(rgb, _mm_and_si128(alpha_mask, dst))
            } else {
                packed
            };
            _mm_storeu_si128(destination.as_mut_ptr().add(offset).cast(), result);
            offset += 16;
        }
    }
    for (dst, src) in destination[offset..]
        .chunks_exact_mut(4)
        .zip(source[offset..].chunks_exact(4))
    {
        blend_faded_pixel::<OPAQUE>(dst, src, opacity);
    }
}

fn blend_faded_pixel<const OPAQUE: bool>(dst: &mut [u8], src: &[u8], opacity: u32) {
    let inverse = 255 - (u32::from(src[3]) * opacity + 127) / 255;
    for channel in 0..if OPAQUE { 3 } else { 4 } {
        dst[channel] =
            ((u32::from(src[channel]) * opacity + u32::from(dst[channel]) * inverse + 127) / 255)
                .min(255) as u8;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn event(at: u64, key: u16, down: bool, mods: &[u16]) -> KeyEvent {
        KeyEvent {
            at_ms: at,
            key,
            down,
            label: format!("{key}"),
            modifiers: mods.iter().map(|key| (*key, format!("{key}"))).collect(),
        }
    }
    #[test]
    fn shortcuts_are_combined_but_successive_actions_are_separate() {
        let mut m = KeyboardModel::default();
        m.event(event(0, 0xa2, true, &[0xa2]));
        m.event(event(10, 0xa0, true, &[0xa2, 0xa0]));
        m.event(event(20, 83, true, &[0xa2, 0xa0]));
        assert_eq!(m.rows.len(), 1);
        assert_eq!(
            m.rows[0].keys.iter().map(|k| k.0).collect::<Vec<_>>(),
            [0xa2, 0xa0, 83]
        );
        m.event(event(30, 83, false, &[0xa2, 0xa0]));
        m.event(event(40, 86, true, &[0xa2, 0xa0]));
        assert_eq!(m.rows.len(), 2);
        assert_eq!(m.rows[0].released, Some(30));
        assert_eq!(m.rows[1].keys.last().unwrap().0, 86);
    }
    #[test]
    fn held_keys_ignore_repeats_and_overlapping_keys_form_a_chord() {
        let mut m = KeyboardModel::default();
        m.event(event(0, 65, true, &[]));
        m.event(event(50, 65, true, &[]));
        m.event(event(100, 66, true, &[]));
        m.event(event(150, 65, false, &[]));
        assert_eq!(m.rows.len(), 1);
        assert_eq!(m.rows[0].keys.len(), 2);
        assert_eq!(m.rows[0].opacity(20_000), 1.0);
        m.event(event(200, 66, false, &[]));
        assert_eq!(m.rows[0].opacity(1400), 1.0);
        assert_eq!(m.rows[0].opacity(1600), 0.5);
        assert!(m.needs_frame(1800));
        m.advance(1800);
        assert!(m.rows.is_empty());
        assert!(!m.needs_frame(1800));
    }
    #[test]
    fn movement_retargets_continuously_and_history_is_bounded() {
        let mut m = KeyboardModel::default();
        for index in 0..20 {
            m.event(event(index * 30, 65, true, &[]));
            m.event(event(index * 30 + 1, 65, false, &[]));
        }
        assert_eq!(m.rows.len(), MAX_ROWS);
        let at = 580;
        let position = m.rows.back().unwrap().y(at);
        m.event(event(at, 66, true, &[]));
        assert!((m.rows[m.rows.len() - 2].y(at) - position).abs() < 0.001);
        m.reset(590);
        assert!(m.held.is_empty());
        assert!(m.rows.iter().all(|row| row.released.is_some()));
    }
    struct Solid;
    impl KeycapRasterizer for Solid {
        fn rasterize(&mut self, _: &str, _: f32) -> Result<Keycap, String> {
            Ok(Keycap {
                width: 40,
                height: 20,
                pixels: [100, 0, 0, 128].repeat(800),
            })
        }
    }

    struct FixedSquare;
    impl KeycapRasterizer for FixedSquare {
        fn rasterize(&mut self, label: &str, scale: f32) -> Result<Keycap, String> {
            assert_eq!(scale, 1.0, "output resolution must not scale keycaps");
            Ok(Keycap {
                width: 64,
                height: 64,
                pixels: [label.parse().unwrap(), 0, 0, 255].repeat(64 * 64),
            })
        }
    }

    #[test]
    fn keycaps_remain_64_square_with_fixed_chord_spacing_at_every_resolution() {
        for size in [
            (320, 240),
            (1280, 720),
            (1920, 1080),
            (3840, 2160),
            (1080, 1920),
        ] {
            let mut overlay = KeyboardOverlay::new(size, Box::new(FixedSquare));
            for key in 65..=67 {
                overlay.model.event(event(0, key, true, &[]));
            }
            let mut pixels = [0, 0, 0, 255].repeat(size.0 as usize * size.1 as usize);
            overlay.draw(&mut pixels, MOVE_MS).unwrap();
            let rows: Vec<_> = pixels
                .chunks_exact(size.0 as usize * 4)
                .filter(|row| row.chunks_exact(4).any(|p| p[0] != 0))
                .collect();
            assert_eq!(rows.len(), 64, "key height at {size:?}");
            for row in rows {
                let mut previous_end = None;
                for key in 65..=67 {
                    let columns: Vec<_> = row
                        .chunks_exact(4)
                        .enumerate()
                        .filter_map(|(x, p)| (p[0] == key).then_some(x))
                        .collect();
                    assert_eq!(columns.len(), 64, "key width at {size:?}");
                    assert_eq!(columns[63] - columns[0], 63);
                    if let Some(end) = previous_end {
                        assert_eq!(columns[0] - end - 1, 10, "fixed gap at {size:?}");
                    }
                    previous_end = Some(columns[63]);
                }
            }
        }
    }

    #[test]
    fn narrow_outputs_clip_chords_instead_of_shrinking_keys() {
        for size in [(100, 100), (40, 100), (2, 2)] {
            let mut overlay = KeyboardOverlay::new(size, Box::new(FixedSquare));
            overlay.model.event(event(0, 65, true, &[]));
            overlay.model.event(event(0, 66, true, &[]));
            let mut pixels = [0, 0, 0, 255].repeat(size.0 as usize * size.1 as usize);
            overlay.draw(&mut pixels, MOVE_MS).unwrap();
            let rows: Vec<_> = pixels
                .chunks_exact(size.0 as usize * 4)
                .filter(|row| row.chunks_exact(4).any(|p| p[0] == 66))
                .collect();
            assert_eq!(rows.len(), 64.min(size.1 as usize));
            // The right inset is 5, 2 and 0 pixels respectively; the latest key is
            // either a complete square or a crop of it, never a scaled-down chord.
            let visible_width = match size.0 {
                100 => 64,
                40 => 38,
                _ => 2,
            };
            for row in rows {
                assert_eq!(
                    row.chunks_exact(4).filter(|p| p[0] == 66).count(),
                    visible_width
                );
            }
        }
    }

    #[test]
    fn fixed_height_history_rows_do_not_overlap_at_low_resolution() {
        let size = (320, 324);
        let mut overlay = KeyboardOverlay::new(size, Box::new(FixedSquare));
        for key in 65..=68 {
            let at = u64::from(key - 65) * 20;
            overlay.model.event(event(at, key, true, &[]));
            overlay.model.event(event(at + 1, key, false, &[]));
        }
        let mut pixels = [0, 0, 0, 255].repeat(size.0 as usize * size.1 as usize);
        overlay.draw(&mut pixels, 300).unwrap();
        let mut previous_bottom = None;
        for key in 65..=68 {
            let rows: Vec<_> = pixels
                .chunks_exact(size.0 as usize * 4)
                .enumerate()
                .filter_map(|(y, row)| row.chunks_exact(4).any(|p| p[0] == key).then_some(y))
                .collect();
            assert_eq!(rows.len(), 64);
            if let Some(bottom) = previous_bottom {
                assert_eq!(rows[0] - bottom - 1, 12);
            }
            previous_bottom = Some(rows[63]);
        }
    }

    #[test]
    fn keycap_cache_is_bounded_and_keeps_recent_labels() {
        let mut overlay = KeyboardOverlay::new((3840, 2160), Box::new(Solid));
        for index in 0..1000 {
            overlay.keycap(&format!("Label {index}")).unwrap();
        }
        assert_eq!(overlay.cache.len(), MAX_CACHE);
        assert!(overlay.cache_bytes <= MAX_CACHE_BYTES);
        assert!(overlay.cache.contains_key("Label 999"));
        assert!(!overlay.cache.contains_key("Label 0"));
    }

    #[test]
    fn cache_evicts_by_bytes_and_does_not_retain_oversized_keycaps() {
        struct Large;
        impl KeycapRasterizer for Large {
            fn rasterize(&mut self, label: &str, _: f32) -> Result<Keycap, String> {
                let height = if label == "oversized" { 8193 } else { 4096 };
                Ok(Keycap {
                    width: 1024,
                    height,
                    pixels: vec![0; (1024 * height * 4) as usize],
                })
            }
        }
        let mut overlay = KeyboardOverlay::new((3840, 2160), Box::new(Large));
        overlay.keycap("a").unwrap();
        overlay.keycap("b").unwrap();
        overlay.keycap("a").unwrap();
        overlay.keycap("c").unwrap();
        assert_eq!(overlay.cache_bytes, MAX_CACHE_BYTES);
        assert_eq!(overlay.cache.len(), 2);
        assert!(overlay.cache.contains_key("a"));
        assert!(!overlay.cache.contains_key("b"));
        let cap = overlay.keycap("oversized").unwrap();
        assert!(cap.pixels.len() > MAX_CACHE_BYTES);
        assert!(!overlay.cache.contains_key("oversized"));
        assert_eq!(overlay.cache_bytes, MAX_CACHE_BYTES);
    }

    #[test]
    fn transparent_simd_blending_updates_alpha_and_matches_scalar() {
        let src = [80, 20, 10, 128].repeat(37);
        for opacity in [0, 1, 127, 128, 254, 255] {
            let mut pixels = [15, 30, 45, 64].repeat(37);
            let mut expected = pixels.clone();
            for (dst, src) in expected.chunks_exact_mut(4).zip(src.chunks_exact(4)) {
                blend_faded_pixel::<false>(dst, src, opacity);
            }
            blend_span::<false>(&mut pixels, &src, opacity);
            assert_eq!(pixels, expected);
        }
    }
    #[test]
    fn offscreen_composition_is_bounded_blended_and_cleared() {
        for size in [(1920, 1080), (40, 100), (2, 2), (1080, 1920)] {
            let mut overlay = KeyboardOverlay::new(size, Box::new(Solid));
            overlay.model.event(event(0, 65, true, &[]));
            overlay.model.event(event(1, 65, false, &[]));
            let original = [20, 40, 60, 255].repeat(size.0 as usize * size.1 as usize);
            let mut pixels = original.clone();
            overlay.draw(&mut pixels, 200).unwrap();
            assert_ne!(pixels, original);
            let mut cleared = original.clone();
            overlay.draw(&mut cleared, 1601).unwrap();
            assert_eq!(cleared, original);
        }
    }

    #[test]
    fn modifier_release_does_not_dismantle_chord_and_sides_release_independently() {
        let mut m = KeyboardModel::default();
        m.event(event(0, 0xa2, true, &[0xa2]));
        m.event(event(10, 0xa3, true, &[0xa2]));
        m.event(event(20, 0xa2, false, &[0xa3]));
        assert!(m.rows[0].released.is_none());
        m.event(event(30, 83, true, &[0xa3]));
        m.event(event(40, 0xa3, false, &[]));
        assert_eq!(m.rows[0].keys.len(), 2);
        assert!(m.rows[0].released.is_none());
        m.event(event(50, 83, false, &[]));
        assert_eq!(m.rows[0].released, Some(50));
    }

    #[test]
    fn premultiplied_blend_handles_clipping_and_fades_without_dark_fringe() {
        let cap = Solid.rasterize("A", 1.0).unwrap();
        let mut pixels = [20, 40, 60, 255].repeat(100);
        let original = pixels.clone();
        blend_keycap(&mut pixels, (10, 10), &cap, -100.0, 0.0, 1.0, 1.0);
        blend_keycap(&mut pixels, (10, 10), &cap, 100.0, 0.0, 1.0, 1.0);
        assert_eq!(pixels, original);
        blend_keycap(&mut pixels, (10, 10), &cap, 0.0, 0.0, 1.0, 1.0);
        assert_eq!(&pixels[..4], &[110, 20, 30, 255]);
        pixels.copy_from_slice(&original);
        blend_keycap(&mut pixels, (10, 10), &cap, 0.0, 0.0, 1.0, 0.5);
        assert_eq!(&pixels[..4], &[65, 30, 45, 255]);
    }

    #[test]
    fn wide_keycap_blending_matches_scalar_alpha_contract_including_unaligned_tails() {
        let mut source = Vec::new();
        let mut destination = Vec::new();
        for alpha in 0..=255u8 {
            source.extend_from_slice(&[alpha, alpha / 2, alpha / 3, alpha]);
            destination.extend_from_slice(&[17, 230, 91, alpha]);
        }
        for (start, opacity) in
            (0..4).flat_map(|start| [0, 1, 127, 128, 254, 255].map(|opacity| (start, opacity)))
        {
            let mut actual = destination[start * 4..destination.len() - 4].to_vec();
            let source = &source[start * 4..source.len() - 4];
            let mut expected = actual.clone();
            for (dst, src) in expected.chunks_exact_mut(4).zip(source.chunks_exact(4)) {
                let inverse = 255 - (u32::from(src[3]) * opacity + 127) / 255;
                for channel in 0..3 {
                    dst[channel] = ((u32::from(src[channel]) * opacity
                        + u32::from(dst[channel]) * inverse
                        + 127)
                        / 255) as u8;
                }
            }
            blend_span::<true>(&mut actual, source, opacity);
            assert_eq!(actual, expected);
        }
    }
}

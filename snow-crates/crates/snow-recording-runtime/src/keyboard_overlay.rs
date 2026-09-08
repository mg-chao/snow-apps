//! Recording-time key state and bounded, output-pixel overlay composition.
//! Native input and font APIs are adapters; animation uses only explicit timestamps.
use std::collections::{BTreeMap, BTreeSet, VecDeque};

pub const MOVE_MS: u64 = 180;
pub const HOLD_MS: u64 = 1_200;
pub const FADE_MS: u64 = 400;
const MAX_ROWS: usize = 4;
const MAX_CACHE: usize = 256;

#[derive(Clone, Debug)]
pub struct KeyboardOverlayConfig {
    pub background_rgba: [u8; 4],
    pub text_rgba: [u8; 4],
    pub border_rgba: [u8; 4],
    pub labels: BTreeMap<u16, String>,
}

#[derive(Clone, Debug)]
pub(crate) struct KeyEvent {
    pub at_ms: u64,
    pub key: u16,
    pub down: bool,
    pub label: String,
    pub modifiers: Vec<(u16, String)>,
}

pub(crate) fn modifier(key: u16) -> bool {
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
pub(crate) struct KeyboardModel {
    rows: VecDeque<Row>,
    held: BTreeSet<u16>,
    dirty: bool,
}

impl KeyboardModel {
    pub(crate) fn reset(&mut self, now: u64) {
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

    pub(crate) fn event(&mut self, event: KeyEvent) {
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

    pub(crate) fn needs_frame(&self, now: u64) -> bool {
        self.dirty
            || self.rows.iter().any(|row| {
                now.saturating_sub(row.moved) < MOVE_MS
                    || row.released.is_some_and(|at| now >= at + HOLD_MS)
            })
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

pub(crate) struct Keycap {
    pub width: u32,
    pub height: u32,
    /// Premultiplied RGBA, including rounded background, border and glyphs.
    pub pixels: Vec<u8>,
}

pub(crate) trait KeycapRasterizer {
    fn rasterize(&mut self, label: &str, scale: f32) -> Result<Keycap, String>;
}

pub(crate) struct KeyboardOverlay {
    pub model: KeyboardModel,
    rasterizer: Box<dyn KeycapRasterizer>,
    cache: BTreeMap<String, Keycap>,
    output: (u32, u32),
    scale: f32,
}

impl KeyboardOverlay {
    pub(crate) fn new(output: (u32, u32), rasterizer: Box<dyn KeycapRasterizer>) -> Self {
        Self {
            model: KeyboardModel::default(),
            rasterizer,
            cache: BTreeMap::new(),
            output,
            scale: 2.0 * (output.1 as f32 / 1080.0).clamp(0.5, 4.0),
        }
    }

    pub(crate) fn draw(&mut self, rgba: &mut [u8], now: u64) -> Result<(), String> {
        self.model.advance(now);
        // Populate only on input changes. Cache size is independent of recording duration.
        if self.cache.len() >= MAX_CACHE {
            self.cache.retain(|label, _| {
                self.model
                    .rows
                    .iter()
                    .any(|row| row.keys.iter().any(|(_, key_label)| key_label == label))
            });
        }
        for row in &self.model.rows {
            for (_, label) in &row.keys {
                if !self.cache.contains_key(label) {
                    self.cache
                        .insert(label.clone(), self.rasterizer.rasterize(label, self.scale)?);
                }
            }
        }
        let margin = (24.0 * self.scale).min(self.output.0.min(self.output.1) as f32 * 0.05);
        let available = (self.output.0 as f32 - 2.0 * margin).max(1.0);
        let gap = 6.0 * self.scale;
        let pitch = 48.0 * self.scale;
        let row_limit = ((self.output.1 as f32 - 2.0 * margin) / pitch)
            .floor()
            .max(1.0) as usize;
        for row in self.model.rows.iter().rev().take(row_limit) {
            if row.born > now {
                continue;
            }
            let caps: Vec<_> = row
                .keys
                .iter()
                .filter_map(|(_, label)| self.cache.get(label))
                .collect();
            let width = caps.iter().map(|cap| cap.width as f32).sum::<f32>()
                + gap * caps.len().saturating_sub(1) as f32;
            let fit = (available / width.max(1.0)).min(1.0);
            let mut x = self.output.0 as f32 - margin - width * fit;
            let bottom = self.output.1 as f32 - margin - row.y(now) * pitch;
            for cap in caps {
                blend_keycap(
                    rgba,
                    self.output,
                    cap,
                    x,
                    bottom - cap.height as f32 * fit,
                    fit,
                    row.opacity(now),
                );
                x += (cap.width as f32 + gap) * fit;
            }
        }
        Ok(())
    }
}

fn blend_keycap(
    rgba: &mut [u8],
    size: (u32, u32),
    cap: &Keycap,
    x: f32,
    y: f32,
    scale: f32,
    opacity: f32,
) {
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
        let target = (target_y as usize * size.0 as usize + (origin.0 + left) as usize) * 4;
        let destination = &mut rgba[target..target + (right - left) as usize * 4];
        // Normal output-size rendering is a contiguous blit: no divisions, floating-point
        // rounding, or per-pixel bounds checks in the hot path.
        if width == cap.width as i32 && height == cap.height as i32 {
            let start = (dy as usize * cap.width as usize + left as usize) * 4;
            let source = &cap.pixels[start..start + destination.len()];
            blend_span(destination, source, opacity);
            continue;
        }
        for (index, dst) in destination.chunks_exact_mut(4).enumerate() {
            let dx = left + index as i32;
            let sx = (dx as u64 * u64::from(cap.width) / width as u64) as usize;
            let sy = (dy as u64 * u64::from(cap.height) / height as u64) as usize;
            let source = (sy * cap.width as usize + sx) * 4;
            blend_faded_pixel(dst, &cap.pixels[source..source + 4], opacity);
        }
    }
}

fn blend_span(destination: &mut [u8], source: &[u8], opacity: u32) {
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
            let rgb = _mm_andnot_si128(alpha_mask, _mm_packus_epi16(low, high));
            let result = _mm_or_si128(rgb, _mm_and_si128(alpha_mask, dst));
            _mm_storeu_si128(destination.as_mut_ptr().add(offset).cast(), result);
            offset += 16;
        }
    }
    for (dst, src) in destination[offset..]
        .chunks_exact_mut(4)
        .zip(source[offset..].chunks_exact(4))
    {
        blend_faded_pixel(dst, src, opacity);
    }
}

fn blend_faded_pixel(dst: &mut [u8], src: &[u8], opacity: u32) {
    let inverse = 255 - (u32::from(src[3]) * opacity + 127) / 255;
    for channel in 0..3 {
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
            blend_span(&mut actual, source, opacity);
            assert_eq!(actual, expected);
        }
    }
}

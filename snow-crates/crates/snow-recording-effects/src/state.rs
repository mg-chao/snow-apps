//! Clocked input state used by live CPU/GPU adapters, previews and recorded replay.
use std::collections::VecDeque;

use snow_recording_model::EffectsConfig;

use crate::keyboard_overlay::{KeyEvent, KeyboardOverlay};
use crate::laser_trail::LaserTrail;
use crate::mouse_effects::{CLICK_ANIMATION_MS, CLICK_QUEUE_DEPTH, RenderClick, draw_clicks_to};
use crate::surface::Surface;

pub const MAX_PENDING_KEYS: usize = 256;

/// Converts high-rate native observations into one clocked trail observation.
/// An exit stays visible to the state even when followed by re-entry in one slot.
#[derive(Default)]
pub struct CoalescedPointer {
    position: Option<(i32, i32)>,
    pending: Option<(Option<(i32, i32)>, u64)>,
    path_broken: bool,
}

impl CoalescedPointer {
    pub fn reproject(&mut self, transform: snow_recording_model::PointerTransform) {
        self.position = self.position.map(|point| transform.project(point));
        self.pending = self
            .pending
            .map(|(point, at)| (point.map(|p| transform.project(p)), at));
    }
    pub fn new() -> Self {
        Self::default()
    }

    pub fn clear(&mut self) {
        *self = Self::default();
    }

    pub fn break_path(&mut self) {
        self.path_broken = true;
    }

    /// Returns whether the latest admitted position changed. Stationary samples
    /// still update admission time, but do not renew the trail or its pixels.
    pub fn observe(&mut self, position: Option<(i32, i32)>, at_ms: u64) -> bool {
        let changed = self.pending.map_or(self.position, |(pending, _)| pending) != position;
        self.path_broken |= position.is_none();
        self.pending = Some((position, at_ms));
        changed
    }

    pub(crate) fn has_due_observation(&self, now: u64) -> bool {
        self.pending.is_some_and(|(position, at_ms)| {
            at_ms <= now && (position != self.position || (self.path_broken && position.is_some()))
        })
    }

    pub fn advance(
        &mut self,
        effects: &mut InputEffectsState,
        source: (u32, u32),
        output: (u32, u32),
        now: u64,
    ) {
        if let Some((position, at_ms)) = self.pending.filter(|(_, at_ms)| *at_ms <= now) {
            self.pending = None;
            if std::mem::take(&mut self.path_broken) {
                effects.observe_pointer(None, source, output, at_ms);
            }
            self.position = position;
            // Delayed composition and stationary samples must never renew the
            // original observation age or add extra mouse-polling taper points.
            effects.observe_pointer(position, source, output, at_ms);
        }
        effects.observe_pointer(self.position, source, output, now);
    }
}

#[derive(Default)]
pub struct InputEffectsState {
    pub trail: LaserTrail,
    pub clicks: VecDeque<RenderClick>,
    pub keyboard: Option<KeyboardOverlay>,
    pub pending_keys: VecDeque<KeyEvent>,
    pub(crate) precise_clicks: PreciseClicks,
}

#[derive(Clone, Copy)]
struct PreciseClick {
    at: u64,
    button: crate::mouse_hook::ObservedMouseButton,
    rounded: (i32, i32),
    point: (f64, f64),
}
pub(crate) struct PreciseClicks([Option<PreciseClick>; CLICK_QUEUE_DEPTH]);
impl Default for PreciseClicks {
    fn default() -> Self {
        Self([None; CLICK_QUEUE_DEPTH])
    }
}

impl InputEffectsState {
    /// Move spatial history with the desktop; do not renew ages or release held keys.
    pub fn reproject_pointer(
        &mut self,
        source: snow_recording_model::PointerTransform,
        output: snow_recording_model::PointerTransform,
    ) {
        self.trail.reproject(source, output);
        let previous = self.precise_clicks.0;
        self.precise_clicks.0.fill(None);
        for (index, click) in self.clicks.iter_mut().enumerate() {
            let point = previous
                .iter()
                .flatten()
                .find(|old| {
                    old.at == click.timestamp_ms
                        && old.button == click.button
                        && old.rounded == (click.x, click.y)
                })
                .map_or((f64::from(click.x), f64::from(click.y)), |old| old.point);
            let point = (
                point.0 * source.scale[0] + source.offset[0],
                point.1 * source.scale[1] + source.offset[1],
            );
            (click.x, click.y) = (point.0.round() as i32, point.1.round() as i32);
            if let Some(slot) = self.precise_clicks.0.get_mut(index) {
                *slot = Some(PreciseClick {
                    at: click.timestamp_ms,
                    button: click.button,
                    rounded: (click.x, click.y),
                    point,
                });
            }
        }
    }
    pub fn new(lifetime_ms: u64) -> Self {
        Self {
            trail: LaserTrail::new(lifetime_ms),
            ..Self::default()
        }
    }

    pub fn with_keyboard(mut self, keyboard: Option<KeyboardOverlay>) -> Self {
        self.keyboard = keyboard;
        self
    }

    pub fn queue_key(&mut self, key: KeyEvent) {
        if self.pending_keys.len() == MAX_PENDING_KEYS {
            self.reset_keyboard(key.at_ms);
        }
        // Equal-time observations keep admission order. Mouse and keyboard observers
        // can deliver different timestamp orders, normalized once before composition.
        self.pending_keys.push_back(key);
    }

    pub fn click(&mut self, click: RenderClick) {
        if self.clicks.len() == CLICK_QUEUE_DEPTH {
            self.clicks.pop_front();
        }
        self.clicks.push_back(click);
    }

    pub fn observe_pointer(
        &mut self,
        position: Option<(i32, i32)>,
        source: (u32, u32),
        output: (u32, u32),
        at_ms: u64,
    ) {
        self.trail.observe(position, source, output, at_ms);
    }

    pub fn reset_keyboard(&mut self, now: u64) {
        self.pending_keys.clear();
        if let Some(keyboard) = &mut self.keyboard {
            keyboard.model.reset(now);
        }
    }

    pub fn reset_inputs(&mut self, now: u64) {
        self.trail.clear();
        self.clicks.clear();
        self.precise_clicks.0.fill(None);
        self.reset_keyboard(now);
    }

    pub fn advance_keys(&mut self, now: u64) {
        let keys = self.pending_keys.make_contiguous();
        if !keys.is_sorted_by_key(|event| event.at_ms) {
            keys.sort_by_key(|event| event.at_ms);
        }
        if let Some(keyboard) = &mut self.keyboard {
            while self
                .pending_keys
                .front()
                .is_some_and(|event| event.at_ms <= now)
            {
                keyboard
                    .model
                    .event(self.pending_keys.pop_front().expect("pending input key"));
            }
        }
    }

    /// Commit one admission batch before deciding whether its keycaps need pixels.
    /// All input sources share this boundary, including scroll observations that
    /// only extend a visible indicator's lifetime without invalidating its image.
    pub fn keyboard_needs_frame(&mut self, now: u64) -> bool {
        self.advance_keys(now);
        self.keyboard
            .as_ref()
            .is_some_and(|keyboard| keyboard.model.needs_frame(now))
    }

    pub fn next_key_frame_at(&mut self, now: u64) -> Option<u64> {
        self.advance_keys(now);
        let keyboard = self.keyboard.as_ref()?;
        keyboard
            .model
            .next_frame_at(now)
            .into_iter()
            .chain(self.pending_keys.front().map(|event| event.at_ms))
            .min()
    }

    pub fn draw_mouse_to(
        &mut self,
        surface: &mut impl Surface,
        now: u64,
        source: (u32, u32),
        config: &EffectsConfig,
    ) {
        self.draw_mouse_layers_to(
            surface,
            now,
            source,
            config.mouse_trail_rgba,
            config.mouse_trail_duration_ms,
            config.mouse_click_rgba,
        );
    }

    pub fn draw_mouse_layers_to(
        &mut self,
        surface: &mut impl Surface,
        now: u64,
        source: (u32, u32),
        trail_rgba: [u8; 4],
        lifetime_ms: u64,
        click_rgba: [u8; 4],
    ) {
        self.trail.set_lifetime_ms(lifetime_ms);
        self.trail.draw_to(surface, now, trail_rgba);
        self.clicks
            .retain(|click| now.saturating_sub(click.timestamp_ms) < CLICK_ANIMATION_MS);
        draw_clicks_to(surface, &self.clicks, now, click_rgba, source);
    }

    pub fn draw_keyboard_to(&mut self, surface: &mut impl Surface, now: u64) -> Result<(), String> {
        self.advance_keys(now);
        if let Some(keyboard) = &mut self.keyboard {
            keyboard.draw_to(surface, now)?;
        }
        Ok(())
    }

    pub fn has_active_animation(&mut self, config: &EffectsConfig, now: u64) -> bool {
        let keyboard = self.keyboard_needs_frame(now);
        keyboard
            || (config.mouse_trail_rgba[3] != 0 && self.trail.has_active_animation(now))
            || (config.mouse_click_rgba[3] != 0
                && self.clicks.iter().any(|click| {
                    now.checked_sub(click.timestamp_ms)
                        .is_some_and(|age| age <= CLICK_ANIMATION_MS)
                }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::preview::{EffectsPreview, PreviewConfig};
    use crate::surface::TileSurface;

    #[test]
    fn follow_mouse_repeated_shrinking_keeps_original_click_precision() {
        let mut state = InputEffectsState::default();
        state.click(RenderClick {
            timestamp_ms: 10,
            x: 47,
            y: 63,
            button: crate::mouse_hook::ObservedMouseButton::Left,
        });
        let half = snow_recording_model::PointerTransform {
            scale: [0.5; 2],
            offset: [0.0; 2],
        };
        let double = snow_recording_model::PointerTransform {
            scale: [2.0; 2],
            offset: [0.0; 2],
        };
        for _ in 0..500 {
            state.reproject_pointer(half, half);
            state.reproject_pointer(double, double);
        }
        assert_eq!(
            (
                state.clicks[0].x,
                state.clicks[0].y,
                state.clicks[0].timestamp_ms
            ),
            (47, 63, 10)
        );
    }

    struct Solid;
    impl crate::keyboard_overlay::KeycapRasterizer for Solid {
        fn rasterize(
            &mut self,
            _: &str,
            _: f32,
        ) -> Result<crate::keyboard_overlay::Keycap, String> {
            Ok(crate::keyboard_overlay::Keycap {
                width: 120,
                height: 64,
                pixels: [160, 0, 0, 200].repeat(120 * 64),
            })
        }
    }

    #[test]
    fn keycap_queue_preserves_equal_time_admission_order_and_reset_discards_future_input() {
        let mut effects = InputEffectsState {
            keyboard: Some(KeyboardOverlay::new((320, 240), Box::new(Solid))),
            ..Default::default()
        };
        let key = |at_ms, down| KeyEvent {
            at_ms,
            key: 65,
            down,
            label: "A".into(),
            modifiers: vec![],
        };
        let events = [key(20, true), key(10, true), key(10, false)];
        for event in &events {
            effects.queue_key(event.clone());
        }
        let mut reference = KeyboardOverlay::new((320, 240), Box::new(Solid));
        for event in [&events[1], &events[2], &events[0]] {
            reference.model.event(event.clone());
        }
        let mut expected = TileSurface::new((320, 240));
        reference.draw_to(&mut expected, 200).unwrap();
        let mut actual = TileSurface::new((320, 240));
        effects.draw_keyboard_to(&mut actual, 200).unwrap();
        let expected = expected.snapshot();
        let actual = actual.snapshot();
        assert_eq!(actual.len(), expected.len());
        for (actual, expected) in actual.iter().zip(expected) {
            assert_eq!(actual.pixels, expected.pixels);
        }

        effects.queue_key(key(1000, true));
        effects.reset_keyboard(210);
        effects
            .draw_keyboard_to(&mut TileSurface::new((320, 240)), 2000)
            .unwrap();
        assert_eq!(effects.next_key_frame_at(2000), None);
        assert!(!effects.keyboard_needs_frame(2000));
        effects.queue_key(key(2010, true));
        assert!(
            effects.keyboard_needs_frame(2010),
            "reset must release held keys"
        );
    }

    #[test]
    fn scroll_deadlines_coalesce_before_invalidation_and_future_reversals_wait() {
        use crate::keyboard_overlay::{FADE_MS, HOLD_MS, SCROLL_GAP_MS};
        let mut effects = InputEffectsState {
            keyboard: Some(KeyboardOverlay::new((320, 240), Box::new(Solid))),
            ..Default::default()
        };
        let scroll = |at_ms, key| KeyEvent {
            at_ms,
            key,
            down: true,
            label: "Scroll".into(),
            modifiers: vec![],
        };
        let config = EffectsConfig::default();
        effects.queue_key(scroll(100, 0x205));
        assert_eq!(effects.next_key_frame_at(0), Some(100));
        assert!(!effects.has_active_animation(&config, 99));
        assert!(effects.has_active_animation(&config, 100));
        effects
            .draw_keyboard_to(&mut TileSurface::new((320, 240)), 280)
            .unwrap();
        for at in [300, 500, 1000] {
            effects.queue_key(scroll(at, 0x205));
            assert!(!effects.has_active_animation(&config, at));
            assert_eq!(
                effects.next_key_frame_at(at),
                Some(at + SCROLL_GAP_MS + HOLD_MS)
            );
        }
        effects.queue_key(scroll(1200, 0x206));
        assert_eq!(effects.next_key_frame_at(1100), Some(1200));
        assert!(!effects.has_active_animation(&config, 1199));
        assert!(effects.has_active_animation(&config, 1200));
        effects
            .draw_keyboard_to(&mut TileSurface::new((320, 240)), 1200)
            .unwrap();
        let fade = 1200 + SCROLL_GAP_MS + HOLD_MS;
        assert!(!effects.has_active_animation(&config, fade - 1));
        assert!(effects.has_active_animation(&config, fade));
        effects
            .draw_keyboard_to(&mut TileSurface::new((320, 240)), fade + FADE_MS)
            .unwrap();
        assert_eq!(effects.next_key_frame_at(fade + FADE_MS), None);
    }

    #[test]
    fn queued_keys_wake_composition_only_at_their_observation_time() {
        struct UnusedRasterizer;
        impl crate::keyboard_overlay::KeycapRasterizer for UnusedRasterizer {
            fn rasterize(
                &mut self,
                _: &str,
                _: f32,
            ) -> Result<crate::keyboard_overlay::Keycap, String> {
                Err("this test does not rasterize".into())
            }
        }
        let mut effects = InputEffectsState {
            keyboard: Some(KeyboardOverlay::new((100, 100), Box::new(UnusedRasterizer))),
            ..Default::default()
        };
        let config = EffectsConfig::default();
        effects.queue_key(KeyEvent {
            at_ms: 100,
            key: 65,
            down: true,
            label: "A".into(),
            modifiers: Vec::new(),
        });
        assert!(!effects.has_active_animation(&config, 99));
        assert!(effects.has_active_animation(&config, 100));
        effects.reset_keyboard(100);
        assert!(effects.pending_keys.is_empty());
        effects.keyboard = None;
        assert!(!effects.has_active_animation(&config, 100));
    }

    #[test]
    fn native_high_rate_coalescing_matches_live_preview_cadence_and_decay() {
        const SIZE: (u32, u32) = (1024, 128);
        const COLOR: [u8; 4] = [255, 64, 80, 230];
        for lifetime in [100, 500, 2000] {
            for interval in [1, 2, 8] {
                let mut pointer = CoalescedPointer::new();
                let mut effects = InputEffectsState::new(lifetime);
                let mut preview = EffectsPreview::new(
                    PreviewConfig {
                        keyboard_filter: Default::default(),
                        webcam: None,
                        region: (0, 0, SIZE.0, SIZE.1),
                        canvas: SIZE,
                        output: SIZE,
                        trail: COLOR,
                        trail_duration_ms: lifetime,
                        click: [0; 4],
                        highlight_radius:
                            snow_recording_model::render::DEFAULT_MOUSE_HIGHLIGHT_RADIUS,
                        highlight: [0; 4],
                        record_mouse_clicks: false,
                        record_mouse_scrolling: false,
                        show_keyboard: false,
                        keyboard: None,
                        generation: 0,
                    },
                    None,
                );
                for frame in 0..46 {
                    let now = if frame < 40 {
                        frame * 17
                    } else {
                        39 * 17 + (frame - 39) * lifetime / 3
                    };
                    if frame < 40 {
                        let first = if frame == 0 { 0 } else { now - 16 };
                        let mut latest = None;
                        for at in first..=now {
                            if at % interval == 0 {
                                let point = Some((20 + at as i32, 64));
                                pointer.observe(point, at);
                                latest = Some((point, at));
                            }
                        }
                        if let Some((point, at)) = latest {
                            preview.observe(point, at);
                        }
                    }
                    let mut surface = TileSurface::new(SIZE);
                    pointer.advance(&mut effects, SIZE, SIZE, now);
                    effects.trail.draw_to(&mut surface, now, COLOR);
                    let actual = surface.snapshot();
                    let expected = preview.render(now).unwrap().mouse;
                    assert_eq!(actual.len(), expected.len());
                    for (actual, expected) in actual.iter().zip(expected) {
                        assert_eq!((actual.x, actual.y), (expected.x, expected.y));
                        assert_eq!(actual.pixels, expected.pixels);
                    }
                }
            }
        }
    }
}

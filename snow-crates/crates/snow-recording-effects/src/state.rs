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
    pub fn new() -> Self {
        Self::default()
    }

    pub fn clear(&mut self) {
        *self = Self::default();
    }

    pub fn break_path(&mut self) {
        self.path_broken = true;
    }

    pub fn observe(&mut self, position: Option<(i32, i32)>, at_ms: u64) {
        self.path_broken |= position.is_none();
        self.pending = Some((position, at_ms));
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
}

impl InputEffectsState {
    pub fn new(lifetime_ms: u64) -> Self {
        Self {
            trail: LaserTrail::new(lifetime_ms),
            ..Self::default()
        }
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

    pub fn has_active_animation(&self, config: &EffectsConfig, now: u64) -> bool {
        (config.mouse_trail_rgba[3] != 0 && self.trail.has_active_animation(now))
            || (config.mouse_click_rgba[3] != 0
                && self.clicks.iter().any(|click| {
                    now.checked_sub(click.timestamp_ms)
                        .is_some_and(|age| age <= CLICK_ANIMATION_MS)
                }))
            || self
                .keyboard
                .as_ref()
                .is_some_and(|keyboard| keyboard.model.needs_frame(now))
            || (self.keyboard.is_some() && self.pending_keys.iter().any(|event| event.at_ms <= now))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::preview::{EffectsPreview, PreviewConfig};
    use crate::surface::TileSurface;

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
                        region: (0, 0, SIZE.0, SIZE.1),
                        canvas: SIZE,
                        output: SIZE,
                        trail: COLOR,
                        trail_duration_ms: lifetime,
                        click: [0; 4],
                        highlight: [0; 4],
                        record_mouse_clicks: false,
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

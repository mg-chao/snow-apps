use snow_recording_effects::{laser_trail::LaserTrail, surface::Surface};

/// Adapts native movement events to the frame observations used by preview and
/// the other recording compositors. LaserTrail's 50-point taper counts rendered
/// movements, not mouse polling events.
pub(super) struct FrameTrail {
    trail: LaserTrail,
    position: Option<(i32, i32)>,
    pending: Option<Movement>,
    path_broken: bool,
}

struct Movement {
    position: Option<(i32, i32)>,
    at_ms: u64,
}

impl FrameTrail {
    pub fn new(lifetime_ms: u64) -> Self {
        Self {
            trail: LaserTrail::new(lifetime_ms),
            position: None,
            pending: None,
            path_broken: false,
        }
    }

    pub fn clear(&mut self) {
        self.trail.clear();
        self.position = None;
        self.pending = None;
        self.path_broken = false;
    }

    pub fn observe(&mut self, position: Option<(i32, i32)>, at_ms: u64) {
        // Keep exits even if the mouse re-enters before the next video frame.
        // Older visible geometry must fade naturally, without bridging the gap.
        self.path_broken |= position.is_none();
        self.pending = Some(Movement { position, at_ms });
    }

    pub fn draw_to(&mut self, surface: &mut impl Surface, now: u64, color: [u8; 4]) {
        let size = surface.size();
        if let Some(movement) = self.pending.take() {
            if std::mem::take(&mut self.path_broken) {
                self.trail.observe(None, size, size, movement.at_ms);
            }
            self.position = movement.position;
            // Observation time belongs to the input, not the encoder. A delayed
            // frame or an idle cursor must never renew the trail's lifetime.
            self.trail
                .observe(self.position, size, size, movement.at_ms);
        }
        self.trail.observe(self.position, size, size, now);
        self.trail.draw_to(surface, now, color);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_recording_effects::{
        preview::{EffectsPreview, PreviewConfig},
        surface::{Tile, TileSurface},
    };

    const SIZE: (u32, u32) = (1024, 128);
    const COLOR: [u8; 4] = [255, 64, 80, 230];

    fn preview(lifetime_ms: u64) -> EffectsPreview {
        EffectsPreview::new(
            PreviewConfig {
                region: (0, 0, SIZE.0, SIZE.1),
                output: SIZE,
                trail: COLOR,
                trail_duration_ms: lifetime_ms,
                click: [0; 4],
                highlight: [0; 4],
                record_mouse_clicks: false,
                show_keyboard: false,
                keyboard: None,
                generation: 0,
            },
            None,
        )
    }

    fn render(trail: &mut FrameTrail, now: u64) -> Vec<Tile> {
        let mut surface = TileSurface::new(SIZE);
        trail.draw_to(&mut surface, now, COLOR);
        surface.snapshot()
    }

    fn assert_same_pixels(actual: &[Tile], expected: &[Tile], now: u64) {
        assert_eq!(actual.len(), expected.len(), "tile count at {now} ms");
        for (actual, expected) in actual.iter().zip(expected) {
            assert_eq!((actual.x, actual.y), (expected.x, expected.y));
            assert!(actual.pixels == expected.pixels, "trail pixels at {now} ms");
        }
    }

    #[test]
    fn high_rate_native_movement_matches_preview_length_and_decay() {
        for lifetime in [100, 500, 2000] {
            for event_interval in [1, 2, 8] {
                let mut native = FrameTrail::new(lifetime);
                let mut preview = preview(lifetime);
                // A 1,000 Hz mouse, a 500 Hz mouse and a 125 Hz mouse follow
                // the same path while frames are rendered at preview cadence.
                for frame in 0..40 {
                    let now = frame * 17;
                    let start = if frame == 0 { 0 } else { now - 16 };
                    let mut latest = None;
                    for at in start..=now {
                        if at % event_interval == 0 {
                            let point = Some((20 + at as i32, 64));
                            native.observe(point, at);
                            latest = Some((point, at));
                        }
                    }
                    if let Some((point, at)) = latest {
                        preview.observe(point, at);
                    }
                    assert_same_pixels(
                        &render(&mut native, now),
                        &preview.render(now).unwrap().mouse,
                        now,
                    );
                }
                for age in [1, lifetime / 2, lifetime - 1, lifetime, lifetime + 17] {
                    let now = 39 * 17 + age;
                    assert_same_pixels(
                        &render(&mut native, now),
                        &preview.render(now).unwrap().mouse,
                        now,
                    );
                }
                assert!(render(&mut native, 39 * 17 + lifetime).is_empty());
            }
        }
    }

    #[test]
    fn coalesced_exit_and_reentry_preserve_the_tail_without_connecting_the_gap() {
        let mut native = FrameTrail::new(500);
        let mut preview = preview(500);
        for (at, x) in [(0, 20), (17, 60)] {
            native.observe(Some((x, 64)), at);
            preview.observe_input(Some((x, 64)), at, 0);
            assert_same_pixels(
                &render(&mut native, at),
                &preview.render(at).unwrap().mouse,
                at,
            );
        }
        native.observe(None, 20);
        native.observe(Some((500, 64)), 25);
        native.observe(Some((520, 64)), 30);
        preview.observe_input(Some((520, 64)), 30, 1);
        assert_same_pixels(
            &render(&mut native, 34),
            &preview.render(34).unwrap().mouse,
            34,
        );
        native.observe(Some((560, 64)), 45);
        preview.observe_input(Some((560, 64)), 45, 1);
        assert_same_pixels(
            &render(&mut native, 51),
            &preview.render(51).unwrap().mouse,
            51,
        );
        assert!(!render(&mut native, 100).is_empty());
        assert!(render(&mut native, 545).is_empty());
    }

    #[test]
    fn delayed_frames_and_stationary_events_do_not_refresh_observation_time() {
        let mut native = FrameTrail::new(100);
        let mut preview = preview(100);
        for (at, x, rendered_at) in [(0, 20, 0), (20, 80, 50), (60, 80, 70), (100, 80, 120)] {
            native.observe(Some((x, 64)), at);
            preview.observe(Some((x, 64)), at);
            assert_same_pixels(
                &render(&mut native, rendered_at),
                &preview.render(rendered_at).unwrap().mouse,
                rendered_at,
            );
        }
        assert!(render(&mut native, 120).is_empty());
        native.observe(Some((120, 64)), 140);
        assert!(
            render(&mut native, 140).is_empty(),
            "do not bridge expired geometry"
        );
        native.observe(Some((140, 64)), 150);
        assert!(!render(&mut native, 150).is_empty());
    }

    #[test]
    fn reset_discards_both_visible_and_pending_movement() {
        let mut native = FrameTrail::new(500);
        native.observe(Some((20, 64)), 0);
        render(&mut native, 0);
        native.observe(Some((60, 64)), 17);
        assert!(!render(&mut native, 17).is_empty());
        native.observe(Some((80, 64)), 20);
        native.clear();
        assert!(render(&mut native, 34).is_empty());
        native.observe(Some((500, 64)), 40);
        assert!(render(&mut native, 51).is_empty());
        native.observe(Some((540, 64)), 60);
        assert!(!render(&mut native, 68).is_empty());
    }
}

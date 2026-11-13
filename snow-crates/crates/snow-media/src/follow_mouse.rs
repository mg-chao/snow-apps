//! Motion of a monitor-local recording viewport, independent of frame acquisition.
use std::time::Instant;

use crate::geometry::{DesktopRect, DesktopSpace};

const HALF_LIFE_SECONDS: f64 = 0.05;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FollowMouseOptions {
    pub smooth_movement: bool,
    pub reduce_jitter: bool,
}

struct Axis {
    position: f64,
    target: f64,
    extent: f64,
    min_origin: f64,
    max_origin: f64,
}

impl Axis {
    fn new(origin: f64, extent: f64, min_origin: f64, end: f64) -> Self {
        Self {
            position: origin,
            target: origin,
            extent,
            min_origin,
            max_origin: end - extent,
        }
    }

    fn center_target(&mut self, cursor: f64, space: DesktopSpace) {
        let half = match space {
            DesktopSpace::Points => self.extent * 0.5,
            DesktopSpace::PhysicalPixels => (self.extent * 0.5).floor(),
        };
        self.target = (cursor - half).clamp(self.min_origin, self.max_origin);
    }

    fn enters_margin(&self, cursor: f64) -> bool {
        // The destination remains the reference while the viewport catches up.
        let margin = self.extent / 12.0;
        (self.target > self.min_origin && cursor <= self.target + margin)
            || (self.target < self.max_origin && cursor >= self.target + self.extent - margin)
    }

    fn ease(&mut self, fraction: f64, settle_distance: f64) {
        let delta = self.target - self.position;
        self.position += delta * fraction;
        if (self.target - self.position).abs() <= settle_distance {
            self.position = self.target;
        }
    }

    fn keep_cursor_in_view(&mut self, cursor: f64, space: DesktopSpace) {
        // Leave room at the exclusive far edge, including after pixel rounding.
        let edge_inset = match space {
            DesktopSpace::Points => (self.extent * 0.5).min(1.0),
            DesktopSpace::PhysicalPixels => 1.0,
        };
        let min_position = cursor - (self.extent - edge_inset).max(0.0);
        self.position = self
            .position
            .clamp(min_position, cursor)
            .clamp(self.min_origin, self.max_origin);
    }

    fn origin(&self, space: DesktopSpace) -> f64 {
        match space {
            DesktopSpace::Points => self.position,
            DesktopSpace::PhysicalPixels => self.position.round(),
        }
    }
}

pub struct FollowMouseMotion {
    options: FollowMouseOptions,
    space: DesktopSpace,
    x: Axis,
    y: Axis,
    sampled_at: Option<Instant>,
}

impl FollowMouseMotion {
    /// Start with an already fitted crop and bounds in the same desktop space.
    pub fn new(
        viewport: DesktopRect,
        bounds: DesktopRect,
        options: FollowMouseOptions,
        now: Instant,
    ) -> Self {
        Self {
            options,
            space: viewport.space,
            x: Axis::new(viewport.x, viewport.width, bounds.x, bounds.right()),
            y: Axis::new(viewport.y, viewport.height, bounds.y, bounds.bottom()),
            sampled_at: Some(now),
        }
    }

    /// Center an unpublished viewport on its latest cursor observation.
    pub fn center(&mut self, cursor: (f64, f64), now: Instant) -> DesktopRect {
        self.sampled_at = Some(now);
        self.center_target(cursor);
        self.apply_target();
        self.viewport()
    }

    /// Resume from the same position without integrating paused time.
    pub fn restart_clock(&mut self) {
        self.sampled_at = None;
    }

    pub fn is_moving(&self) -> bool {
        self.x.position != self.x.target || self.y.position != self.y.target
    }

    pub fn advance(&mut self, cursor: (f64, f64), now: Instant) -> DesktopRect {
        let previous = self.sampled_at.replace(now);
        if !self.options.reduce_jitter
            || self.x.enters_margin(cursor.0)
            || self.y.enters_margin(cursor.1)
        {
            self.center_target(cursor);
        }
        if !self.options.smooth_movement {
            self.apply_target();
        } else {
            if self.is_moving()
                && let Some(previous) = previous
            {
                let elapsed = now.saturating_duration_since(previous).as_secs_f64();
                let fraction = 1.0 - (-elapsed / HALF_LIFE_SECONDS).exp2();
                let settle_distance = match self.space {
                    DesktopSpace::Points => 0.25,
                    DesktopSpace::PhysicalPixels => 0.5,
                };
                self.x.ease(fraction, settle_distance);
                self.y.ease(fraction, settle_distance);
            }
            self.x.keep_cursor_in_view(cursor.0, self.space);
            self.y.keep_cursor_in_view(cursor.1, self.space);
        }
        self.viewport()
    }

    fn center_target(&mut self, cursor: (f64, f64)) {
        self.x.center_target(cursor.0, self.space);
        self.y.center_target(cursor.1, self.space);
    }

    fn apply_target(&mut self) {
        self.x.position = self.x.target;
        self.y.position = self.y.target;
    }

    fn viewport(&self) -> DesktopRect {
        DesktopRect {
            space: self.space,
            x: self.x.origin(self.space),
            y: self.y.origin(self.space),
            width: self.x.extent,
            height: self.y.extent,
        }
    }
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use super::*;

    fn rect(space: DesktopSpace, x: f64, y: f64, width: f64, height: f64) -> DesktopRect {
        DesktopRect {
            space,
            x,
            y,
            width,
            height,
        }
    }

    fn motion(smooth_movement: bool, reduce_jitter: bool, now: Instant) -> FollowMouseMotion {
        let space = DesktopSpace::Points;
        FollowMouseMotion::new(
            rect(space, 150.0, 160.0, 100.0, 80.0),
            rect(space, -200.0, -400.0, 1400.0, 1800.0),
            FollowMouseOptions {
                smooth_movement,
                reduce_jitter,
            },
            now,
        )
    }

    #[test]
    fn immediate_follow_preserves_pixel_centering_and_bounds() {
        let now = Instant::now();
        let space = DesktopSpace::PhysicalPixels;
        let viewport = rect(space, -600.0, 200.0, 101.0, 79.0);
        let bounds = rect(space, -800.0, 0.0, 600.0, 400.0);
        let mut follow =
            FollowMouseMotion::new(viewport, bounds, FollowMouseOptions::default(), now);
        assert_eq!(follow.advance((-550.0, 239.0), now), viewport);
        assert_eq!(
            follow.advance((-799.0, 1.0), now),
            rect(space, -800.0, 0.0, 101.0, 79.0)
        );
        assert_eq!(
            follow.advance((-201.0, 399.0), now),
            rect(space, -301.0, 321.0, 101.0, 79.0)
        );
    }

    #[test]
    fn jitter_reduction_recenters_at_each_outer_twelfth() {
        let now = Instant::now();
        let space = DesktopSpace::Points;
        let initial = rect(space, 150.0, 160.0, 120.0, 84.0);
        let bounds = rect(space, -200.0, -400.0, 1400.0, 1800.0);
        for (inside, boundary) in [
            ((160.1, 201.0), (160.0, 201.0)),
            ((259.9, 201.0), (260.0, 201.0)),
            ((211.0, 167.1), (211.0, 167.0)),
            ((211.0, 236.9), (211.0, 237.0)),
        ] {
            let mut follow = FollowMouseMotion::new(
                initial,
                bounds,
                FollowMouseOptions {
                    smooth_movement: false,
                    reduce_jitter: true,
                },
                now,
            );
            assert_eq!(follow.advance(inside, now), initial);
            let changed = follow.advance(boundary, now);
            assert_eq!(
                (changed.x, changed.y),
                (boundary.0 - 60.0, boundary.1 - 42.0)
            );
            assert_eq!(follow.advance(boundary, now), changed);
        }
    }

    #[test]
    fn clamped_edge_does_not_trigger_movement_on_the_other_axis() {
        let now = Instant::now();
        let mut follow = motion(false, true, now);
        let initial = follow.center((-195.0, 200.0), now);
        assert_eq!(initial.x, -200.0);
        assert_eq!(follow.advance((-195.0, 212.0), now), initial);
        let changed = follow.advance((-195.0, 234.0), now);
        assert_eq!((changed.x, changed.y), (-200.0, 194.0));
    }

    #[test]
    fn smooth_movement_catches_up_in_real_time_and_settles() {
        let now = Instant::now();
        for fps in [15, 30, 60, 120] {
            let mut follow = motion(true, false, now);
            let initial = follow.viewport();
            for frame in 1..=fps {
                let t = f64::from(frame) / f64::from(fps);
                let crop = follow.advance((240.0, 200.0), now + Duration::from_secs_f64(t));
                if frame == 1 {
                    assert!(initial.x < crop.x && crop.x < 190.0);
                }
                if t == 0.2 {
                    assert!((crop.x - 187.5).abs() < 1e-6);
                }
            }
            assert_eq!(follow.viewport().x, 190.0);
            assert_eq!(
                follow
                    .advance((240.0, 200.0), now + Duration::from_secs(2))
                    .x,
                190.0
            );
        }
    }

    #[test]
    fn smooth_follow_catches_large_jumps_on_the_first_sample() {
        let now = Instant::now();
        for space in [DesktopSpace::Points, DesktopSpace::PhysicalPixels] {
            for reduce_jitter in [false, true] {
                let initial = rect(space, 150.0, 160.0, 100.0, 80.0);
                let bounds = rect(space, -400.0, -300.0, 1400.0, 1000.0);
                let mut follow = FollowMouseMotion::new(
                    initial,
                    bounds,
                    FollowMouseOptions {
                        smooth_movement: true,
                        reduce_jitter,
                    },
                    now,
                );
                let jumped = follow.advance((750.0, -200.0), now + Duration::from_millis(16));
                assert_eq!(jumped, rect(space, 651.0, -200.0, 100.0, 80.0));
                // This sample leaves the current crop while staying inside the target's dead zone.
                let caught = follow.advance((760.0, -200.0), now + Duration::from_millis(16));
                assert_eq!(caught, rect(space, 661.0, -200.0, 100.0, 80.0));
                let eased = follow.advance((760.0, -200.0), now + Duration::from_millis(66));
                assert!(eased.x > caught.x && eased.x < 700.0);
                assert!(eased.y < jumped.y && eased.y > -240.0);

                let reversed = follow.advance((-400.0, 699.0), now + Duration::from_millis(82));
                assert_eq!(reversed, rect(space, -400.0, 620.0, 100.0, 80.0));
                assert!(!follow.is_moving());
            }
        }
    }

    #[test]
    fn combined_options_latch_and_retarget_against_the_destination() {
        let now = Instant::now();
        let half_life = Duration::from_millis(50);
        let mut follow = motion(true, true, now);
        assert_eq!(follow.advance((241.0, 200.0), now).x, 150.0);
        assert_eq!(follow.advance((242.0, 200.0), now + half_life).x, 171.0);
        // The pointer is already in the moving viewport's right margin but is
        // still inside the destination's central five-sixths.
        assert_eq!(follow.advance((263.0, 200.0), now + 2 * half_life).x, 181.5);
        assert_eq!(
            follow.advance((284.0, 200.0), now + 3 * half_life).x,
            207.75
        );
        assert_eq!(
            follow.advance((284.0, 200.0), now + 12 * half_life).x,
            234.0
        );
    }

    #[test]
    fn easing_resumes_from_the_frozen_position() {
        let now = Instant::now();
        let mut follow = motion(true, false, now);
        let frozen = follow.advance((250.0, 200.0), now + Duration::from_millis(50));
        assert_eq!(frozen.x, 175.0);
        follow.restart_clock();
        let resumed_at = now + Duration::from_secs(3);
        assert_eq!(follow.advance((265.0, 200.0), resumed_at), frozen);
        assert_eq!(
            follow
                .advance((265.0, 200.0), resumed_at + Duration::from_millis(50))
                .x,
            195.0
        );
    }
}

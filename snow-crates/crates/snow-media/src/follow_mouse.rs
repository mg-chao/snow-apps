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
        let margin = self.extent * 0.25;
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
        } else if self.is_moving()
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
    fn jitter_reduction_uses_each_axis_quarter_and_recenters_on_crossing() {
        let now = Instant::now();
        for (inside, boundary) in [
            ((175.1, 201.0), (175.0, 201.0)),
            ((224.9, 201.0), (225.0, 201.0)),
            ((201.0, 180.1), (201.0, 180.0)),
            ((201.0, 219.9), (201.0, 220.0)),
        ] {
            let mut follow = motion(false, true, now);
            let initial = follow.viewport();
            assert_eq!(follow.advance(inside, now), initial);
            let changed = follow.advance(boundary, now);
            assert_eq!(
                (changed.x, changed.y),
                (boundary.0 - 50.0, boundary.1 - 40.0)
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
        let changed = follow.advance((-195.0, 220.0), now);
        assert_eq!((changed.x, changed.y), (-200.0, 180.0));
    }

    #[test]
    fn smooth_movement_catches_up_in_real_time_and_settles() {
        let now = Instant::now();
        for fps in [15, 30, 60, 120] {
            let mut follow = motion(true, false, now);
            let initial = follow.viewport();
            for frame in 1..=fps {
                let t = f64::from(frame) / f64::from(fps);
                let crop = follow.advance((1000.0, 200.0), now + Duration::from_secs_f64(t));
                if frame == 1 {
                    assert!(initial.x < crop.x && crop.right() < 1000.0);
                }
                if t == 0.2 {
                    assert!((crop.x - 900.0).abs() < 1e-6);
                }
            }
            assert_eq!(follow.viewport().x, 950.0);
            assert_eq!(
                follow
                    .advance((1000.0, 200.0), now + Duration::from_secs(2))
                    .x,
                950.0
            );
        }
    }

    #[test]
    fn combined_options_latch_and_retarget_against_the_destination() {
        let now = Instant::now();
        let half_life = Duration::from_millis(50);
        let mut follow = motion(true, true, now);
        assert_eq!(follow.advance((224.0, 200.0), now).x, 150.0);
        assert_eq!(follow.advance((225.0, 200.0), now + half_life).x, 162.5);
        // The pointer is already in the moving viewport's right margin but is
        // still inside the destination's central half.
        assert_eq!(
            follow.advance((240.0, 200.0), now + 2 * half_life).x,
            168.75
        );
        assert_eq!(
            follow.advance((250.0, 200.0), now + 3 * half_life).x,
            184.375
        );
        assert_eq!(
            follow.advance((250.0, 200.0), now + 12 * half_life).x,
            200.0
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
        assert_eq!(follow.advance((350.0, 200.0), resumed_at), frozen);
        assert_eq!(
            follow
                .advance((350.0, 200.0), resumed_at + Duration::from_millis(50))
                .x,
            237.5
        );
    }
}

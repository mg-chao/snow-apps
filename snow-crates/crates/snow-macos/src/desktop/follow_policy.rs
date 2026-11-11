//! Resource-free follow geometry; desktop points stay independent of backing pixels.
use std::time::{Duration, Instant};

use snow_media::geometry::{DesktopRect, GeometryError};

pub(super) struct MotionCadence {
    interval: Duration,
    next: Option<Instant>,
}

impl MotionCadence {
    pub(super) fn new(fps: u32) -> Self {
        Self {
            interval: Duration::from_nanos(1_000_000_000u64.div_ceil(u64::from(fps))),
            next: None,
        }
    }

    pub(super) fn take_frame(&mut self, now: Instant) -> bool {
        let scheduled = self.next.unwrap_or(now);
        if now < scheduled {
            return false;
        }
        let next = scheduled + self.interval;
        self.next = Some(if next > now {
            next
        } else {
            now + self.interval
        });
        true
    }

    pub(super) fn wait_limit(&self, now: Instant, timeout: Duration) -> Duration {
        // A missing cursor sample can leave an elapsed deadline pending.
        self.next
            .filter(|&next| next > now)
            .map_or(timeout, |next| timeout.min(next.duration_since(now)))
    }

    pub(super) fn restart(&mut self) {
        self.next = None;
    }
}

pub(super) fn contains(bounds: DesktopRect, cursor: (f64, f64)) -> bool {
    cursor.0.is_finite()
        && cursor.1.is_finite()
        && cursor.0 >= bounds.x
        && cursor.1 >= bounds.y
        && cursor.0 < bounds.right()
        && cursor.1 < bounds.bottom()
}

pub(super) fn viewport(
    nominal: DesktopRect,
    bounds: DesktopRect,
    cursor: (f64, f64),
) -> Result<DesktopRect, GeometryError> {
    nominal.validate()?;
    bounds.validate()?;
    if nominal.space != bounds.space || !cursor.0.is_finite() || !cursor.1.is_finite() {
        return Err(GeometryError);
    }
    let scale = (bounds.width / nominal.width)
        .min(bounds.height / nominal.height)
        .min(1.0);
    let width = (nominal.width * scale).min(bounds.width);
    let height = (nominal.height * scale).min(bounds.height);
    DesktopRect {
        space: nominal.space,
        x: (cursor.0 - width * 0.5).clamp(bounds.x, bounds.right() - width),
        y: (cursor.1 - height * 0.5).clamp(bounds.y, bounds.bottom() - height),
        width,
        height,
    }
    .validate()
}

/// Readiness alone cannot authorize publication after newer observations.
pub(super) fn can_commit(
    prepared_topology: u64,
    current_topology: u64,
    prepared_exclusions: u64,
    current_exclusions: u64,
    filters_settled: bool,
    destination: DesktopRect,
    cursor: (f64, f64),
) -> bool {
    prepared_topology == current_topology
        && prepared_exclusions == current_exclusions
        && filters_settled
        && contains(destination, cursor)
}

/// Initial native startup may finish after the pointer has crossed a display.
/// Once a frame is published, the active display can continue during handoff.
pub(super) fn can_publish(
    published: bool,
    active_topology: u64,
    current_topology: u64,
    healthy: bool,
    active_bounds: DesktopRect,
    cursor: (f64, f64),
) -> bool {
    published || active_topology == current_topology && healthy && contains(active_bounds, cursor)
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_media::geometry::DesktopSpace;

    fn rect(x: f64, y: f64, width: f64, height: f64) -> DesktopRect {
        DesktopRect {
            space: DesktopSpace::Points,
            x,
            y,
            width,
            height,
        }
    }

    #[test]
    fn centered_viewport_clamps_every_monitor_edge_without_backing_scale() {
        let nominal = rect(0.0, 0.0, 641.0, 359.0);
        for bounds in [
            rect(0.0, 0.0, 1920.0, 1080.0),
            rect(-1920.0, -300.0, 1920.0, 1080.0),
        ] {
            for x in [bounds.x, bounds.x + 800.25, bounds.right() - 0.5] {
                for y in [bounds.y, bounds.y + 500.5, bounds.bottom() - 0.5] {
                    let crop = viewport(nominal, bounds, (x, y)).unwrap();
                    assert_eq!((crop.width, crop.height), (641.0, 359.0));
                    assert!(crop.x >= bounds.x && crop.y >= bounds.y);
                    assert!(crop.right() <= bounds.right() && crop.bottom() <= bounds.bottom());
                    if x > bounds.x + 320.5 && x < bounds.right() - 320.5 {
                        assert_eq!(crop.x + crop.width * 0.5, x);
                    }
                }
            }
        }
    }

    #[test]
    fn smaller_display_shrinks_proportionally_and_next_display_restores_nominal_extent() {
        let nominal = rect(0.0, 0.0, 641.0, 359.0);
        let small = rect(-300.0, 50.0, 301.0, 200.0);
        let crop = viewport(nominal, small, (-50.0, 90.0)).unwrap();
        assert_eq!(crop.width, 301.0);
        assert!((crop.width / crop.height - 641.0 / 359.0).abs() < 1e-12);
        assert!(crop.right() <= small.right() && crop.bottom() <= small.bottom());
        let large = rect(0.0, 0.0, 1920.0, 1080.0);
        let restored = viewport(nominal, large, (1000.0, 500.0)).unwrap();
        assert_eq!((restored.width, restored.height), (641.0, 359.0));
        assert!(viewport(nominal, large, (f64::NAN, 0.0)).is_err());
    }

    #[test]
    fn ready_destination_requires_latest_pointer_topology_and_filters() {
        let bounds = rect(100.0, -100.0, 200.0, 300.0);
        assert!(can_commit(2, 2, 4, 4, true, bounds, (100.0, -100.0)));
        assert!(!can_commit(1, 2, 4, 4, true, bounds, (150.0, 0.0)));
        assert!(!can_commit(2, 2, 3, 4, true, bounds, (150.0, 0.0)));
        assert!(!can_commit(2, 2, 4, 4, false, bounds, (150.0, 0.0)));
        assert!(!can_commit(2, 2, 4, 4, true, bounds, (300.0, 0.0)));
        assert!(!can_commit(2, 2, 4, 4, true, bounds, (150.0, 200.0)));
    }

    #[test]
    fn cursor_crossing_during_first_monitor_warmup_cannot_publish_the_old_monitor() {
        let left = rect(-1920.0, 0.0, 1920.0, 1080.0);
        let right = rect(0.0, 0.0, 1920.0, 1080.0);
        assert!(can_publish(false, 1, 1, true, left, (-500.0, 400.0)));
        assert!(!can_publish(false, 1, 1, true, left, (500.0, 400.0)));
        assert!(can_publish(false, 1, 1, true, right, (500.0, 400.0)));
        assert!(!can_publish(false, 1, 2, true, right, (500.0, 400.0)));
        assert!(!can_publish(false, 1, 1, false, right, (500.0, 400.0)));
        assert!(can_publish(true, 1, 2, false, left, (500.0, 400.0)));
    }

    #[test]
    fn motion_cadence_bounds_frequent_polls_and_skips_missed_frames() {
        let now = Instant::now();
        let mut cadence = MotionCadence::new(20);
        let at = |millis| now + Duration::from_millis(millis);
        let timeout = Duration::from_millis(20);
        assert!(cadence.take_frame(at(0)));
        assert!(!cadence.take_frame(at(49)));
        assert_eq!(
            cadence.wait_limit(at(49), timeout),
            Duration::from_millis(1)
        );
        assert_eq!(cadence.wait_limit(at(50), timeout), timeout);
        assert_eq!(cadence.wait_limit(at(54), timeout), timeout);
        assert!(cadence.take_frame(at(55)));
        assert!(!cadence.take_frame(at(99)));
        assert!(cadence.take_frame(at(100)));
        assert!(cadence.take_frame(at(270)));
        assert!(!cadence.take_frame(at(271)));
        cadence.restart();
        assert!(cadence.take_frame(at(271)));
    }
}

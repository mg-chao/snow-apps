use std::time::Duration;

/// Output PTS ownership is independent of capture arrival and source timestamps.
pub(crate) struct OutputSchedule {
    fps: u32,
    next: u64,
    pub missed_slots: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct OutputSlot {
    pub pts: u64,
    pub at: Duration,
}

impl OutputSchedule {
    pub fn new(fps: u32) -> Self {
        Self {
            fps: fps.max(1),
            next: 0,
            missed_slots: 0,
        }
    }

    pub fn endpoint(&self, elapsed: Duration) -> u64 {
        (elapsed.as_nanos() * u128::from(self.fps))
            .div_ceil(1_000_000_000)
            .min(u128::from(u64::MAX)) as u64
    }

    fn at(&self, index: u64) -> Duration {
        Duration::from_nanos(
            (u128::from(index) * 1_000_000_000)
                .div_ceil(u128::from(self.fps))
                .min(u128::from(u64::MAX)) as u64,
        )
    }

    pub fn wait(&self, elapsed: Duration) -> Duration {
        self.at(self.next)
            .saturating_sub(elapsed)
            .min(Duration::from_millis(10))
    }

    pub fn poll(&mut self, elapsed: Duration) -> Option<OutputSlot> {
        let index = (elapsed.as_nanos() * u128::from(self.fps) / 1_000_000_000)
            .min(u128::from(u64::MAX)) as u64;
        if index < self.next {
            return None;
        }
        self.missed_slots = self
            .missed_slots
            .saturating_add(index.saturating_sub(self.next));
        self.next = index.saturating_add(1);
        Some(OutputSlot {
            pts: index,
            at: self.at(index),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn late_worker_skips_obsolete_slots_without_a_catch_up_burst() {
        let mut schedule = OutputSchedule::new(30);
        assert_eq!(schedule.poll(Duration::ZERO).unwrap().pts, 0);
        assert_eq!(schedule.poll(Duration::from_millis(105)).unwrap().pts, 3);
        assert_eq!(schedule.missed_slots, 2);
        assert!(schedule.poll(Duration::from_millis(105)).is_none());
        assert_eq!(schedule.poll(Duration::from_millis(134)).unwrap().pts, 4);
    }

    #[test]
    fn rational_deadlines_do_not_drift_over_an_hour() {
        for fps in [10, 24, 30, 60, 144] {
            let schedule = OutputSchedule::new(fps);
            assert_eq!(
                schedule.at(u64::from(fps) * 3600),
                Duration::from_secs(3600)
            );
            assert_eq!(
                schedule.endpoint(Duration::from_millis(1001)),
                u64::from(fps) + 1
            );
        }
    }

    #[test]
    fn frozen_active_clock_does_not_advance_output_during_pause() {
        let mut schedule = OutputSchedule::new(30);
        schedule.poll(Duration::from_secs(1));
        assert!(schedule.poll(Duration::from_secs(1)).is_none());
        assert_eq!(schedule.poll(Duration::from_millis(1034)).unwrap().pts, 31);
    }
}

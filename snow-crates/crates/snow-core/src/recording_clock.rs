use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PauseInterval {
    pub start_ms: u64,
    pub end_ms: u64,
}

#[derive(Debug)]
struct RecordingClockState {
    started_at: Instant,
    pause_started_at: Option<Instant>,
    intervals: Vec<PauseInterval>,
    precise_intervals: Vec<(Instant, Instant)>,
    total_paused: Duration,
}

impl RecordingClockState {
    fn new(started_at: Instant) -> Self {
        Self {
            started_at,
            pause_started_at: None,
            intervals: Vec::new(),
            precise_intervals: Vec::new(),
            total_paused: Duration::ZERO,
        }
    }

    fn mark_pause(&mut self, at: Instant) {
        if self.pause_started_at.is_none() {
            self.pause_started_at = Some(at);
        }
    }

    fn mark_resume(&mut self, at: Instant) {
        if let Some(start) = self.pause_started_at.take() {
            let start_ms = start.saturating_duration_since(self.started_at).as_millis() as u64;
            let end_ms = at.saturating_duration_since(self.started_at).as_millis() as u64;
            self.total_paused += at.saturating_duration_since(start);
            self.precise_intervals.push((start, at));
            self.intervals.push(PauseInterval { start_ms, end_ms });
        }
    }

    fn finalize(&mut self, at: Instant) {
        if self.pause_started_at.is_some() {
            self.mark_resume(at);
        }
    }

    fn active_elapsed_duration(&self, at: Instant) -> Duration {
        let elapsed = at.saturating_duration_since(self.started_at);
        // Queued observations can precede the latest resume. Subtract only
        // the portion of each pause that occurred before this observation.
        let mut paused = if self
            .precise_intervals
            .last()
            .is_none_or(|(_, end)| at >= *end)
        {
            self.total_paused
        } else {
            self.precise_intervals
                .iter()
                .map(|(start, end)| at.min(*end).saturating_duration_since(*start))
                .sum()
        };
        if let Some(paused_from) = self.pause_started_at {
            paused += at.saturating_duration_since(paused_from);
        }
        elapsed.saturating_sub(paused)
    }
}

#[derive(Clone, Debug)]
pub struct RecordingClock {
    inner: Arc<Mutex<RecordingClockState>>,
}

impl RecordingClock {
    pub fn new(started_at: Instant) -> Self {
        Self {
            inner: Arc::new(Mutex::new(RecordingClockState::new(started_at))),
        }
    }

    pub fn controller(&self) -> RecordingClockController {
        RecordingClockController {
            inner: Arc::clone(&self.inner),
        }
    }

    pub fn started_at(&self) -> Instant {
        self.inner.lock().unwrap().started_at
    }

    pub fn active_elapsed_duration(&self, at: Instant) -> Duration {
        self.inner.lock().unwrap().active_elapsed_duration(at)
    }

    pub fn active_elapsed_ms(&self, at: Instant) -> u64 {
        self.active_elapsed_duration(at).as_millis() as u64
    }

    /// Whether an observation belongs to active recording rather than a pause.
    pub fn is_active_at(&self, at: Instant) -> bool {
        let state = self.inner.lock().unwrap();
        at >= state.started_at
            && state.pause_started_at.is_none_or(|start| at < start)
            && !state
                .precise_intervals
                .iter()
                .any(|(start, end)| at >= *start && at < *end)
    }

    pub fn active_elapsed_from_stream_offset(&self, offset: Duration) -> Duration {
        let started_at = self.started_at();
        let at = started_at.checked_add(offset).unwrap_or(started_at);
        self.active_elapsed_duration(at)
    }

    pub fn pause_intervals(&self) -> Vec<PauseInterval> {
        self.inner.lock().unwrap().intervals.clone()
    }
}

#[derive(Clone, Debug)]
pub struct RecordingClockController {
    inner: Arc<Mutex<RecordingClockState>>,
}

impl RecordingClockController {
    pub fn mark_pause(&self, at: Instant) {
        self.inner.lock().unwrap().mark_pause(at);
    }

    pub fn mark_resume(&self, at: Instant) {
        self.inner.lock().unwrap().mark_resume(at);
    }

    pub fn finalize(&self, at: Instant) {
        self.inner.lock().unwrap().finalize(at);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clock_tracks_pause_intervals_and_active_elapsed() {
        let started_at = Instant::now();
        let clock = RecordingClock::new(started_at);
        let controller = clock.controller();

        controller.mark_pause(started_at + Duration::from_millis(100));
        controller.mark_resume(started_at + Duration::from_millis(250));

        assert_eq!(
            clock.pause_intervals(),
            vec![PauseInterval {
                start_ms: 100,
                end_ms: 250,
            }]
        );
        assert_eq!(
            clock.active_elapsed_ms(started_at + Duration::from_millis(400)),
            250
        );
    }

    #[test]
    fn finalize_closes_open_pause_interval() {
        let started_at = Instant::now();
        let clock = RecordingClock::new(started_at);
        let controller = clock.controller();

        controller.mark_pause(started_at + Duration::from_millis(50));
        controller.finalize(started_at + Duration::from_millis(100));

        assert_eq!(
            clock.pause_intervals(),
            vec![PauseInterval {
                start_ms: 50,
                end_ms: 100,
            }]
        );
    }

    #[test]
    fn queued_observations_keep_their_original_time_after_later_pauses() {
        let start = Instant::now();
        let clock = RecordingClock::new(start);
        let controller = clock.controller();
        controller.mark_pause(start + Duration::from_millis(100));
        controller.mark_resume(start + Duration::from_millis(250));
        controller.mark_pause(start + Duration::from_millis(350));
        controller.mark_resume(start + Duration::from_millis(450));
        for (wall, media, active) in [
            (50, 50, true),
            (100, 100, false),
            (200, 100, false),
            (250, 100, true),
            (300, 150, true),
            (400, 200, false),
            (500, 250, true),
        ] {
            let at = start + Duration::from_millis(wall);
            assert_eq!(clock.active_elapsed_ms(at), media);
            assert_eq!(clock.is_active_at(at), active);
        }
    }
}

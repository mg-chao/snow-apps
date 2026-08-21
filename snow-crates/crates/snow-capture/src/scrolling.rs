//! Stitch-throughput pacing primitives used by scrolling capture.
//!
//! Scrolling capture has one serialized consumer: the stitch worker.  Its
//! sustainable capture rate is therefore derived from the time that worker
//! spends processing representative (non-duplicate) frames.  Viewport motion
//! and frame-change heuristics are deliberately absent from this policy.

use std::time::Duration;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ScrollingGovernorConfig {
    pub min_fps: f64,
    pub max_fps: f64,
    pub initial_fps: f64,
    pub target_utilization: f64,
    pub slower_latency_alpha: f64,
    pub faster_latency_alpha: f64,
    pub recovery_fps_per_second: f64,
    pub overload_backoff_factor: f64,
}

impl Default for ScrollingGovernorConfig {
    fn default() -> Self {
        Self {
            min_fps: 4.0,
            max_fps: 30.0,
            initial_fps: 15.0,
            target_utilization: 0.80,
            slower_latency_alpha: 0.50,
            faster_latency_alpha: 0.10,
            recovery_fps_per_second: 6.0,
            overload_backoff_factor: 0.75,
        }
    }
}

impl ScrollingGovernorConfig {
    pub fn validate(self) -> Result<Self, &'static str> {
        if !self.min_fps.is_finite()
            || !self.max_fps.is_finite()
            || !self.initial_fps.is_finite()
            || self.min_fps <= 0.0
            || self.max_fps < self.min_fps
            || self.initial_fps < self.min_fps
            || self.initial_fps > self.max_fps
            || !self.target_utilization.is_finite()
            || self.target_utilization <= 0.0
            || self.target_utilization > 1.0
            || !self.slower_latency_alpha.is_finite()
            || self.slower_latency_alpha <= 0.0
            || self.slower_latency_alpha > 1.0
            || !self.faster_latency_alpha.is_finite()
            || self.faster_latency_alpha <= 0.0
            || self.faster_latency_alpha > 1.0
            || !self.recovery_fps_per_second.is_finite()
            || self.recovery_fps_per_second < 0.0
            || !self.overload_backoff_factor.is_finite()
            || self.overload_backoff_factor <= 0.0
            || self.overload_backoff_factor >= 1.0
        {
            return Err("invalid scrolling stitch-throughput governor configuration");
        }
        Ok(self)
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct ScrollingGovernorSignal {
    /// Full service time of the stitch worker for the slowest representative
    /// completion observed since the previous capture-loop iteration.
    pub stitch_service_time: Option<Duration>,
    /// Number of frames waiting in the stitch mailbox at feedback time.
    pub stitch_pending_depth: usize,
    /// Number of frames replaced/coalesced while the stitch worker was busy.
    pub stitch_replaced_frames: u32,
    /// Capture stream queue pressure, normalized to `[0, 1]`.
    pub queue_fill: f64,
    /// Whether the capture queue dropped a frame during this observation.
    pub capture_dropped: bool,
}

#[derive(Clone, Copy, Debug)]
pub struct ScrollingGovernor {
    config: ScrollingGovernorConfig,
    current_fps: f64,
    estimated_stitch_service_time: Option<Duration>,
}

impl ScrollingGovernor {
    pub fn new(config: ScrollingGovernorConfig) -> Result<Self, &'static str> {
        let config = config.validate()?;
        Ok(Self {
            current_fps: config.initial_fps,
            config,
            estimated_stitch_service_time: None,
        })
    }

    pub fn config(&self) -> ScrollingGovernorConfig {
        self.config
    }

    pub fn current_fps(&self) -> f64 {
        self.current_fps
    }

    pub fn estimated_stitch_service_time(&self) -> Option<Duration> {
        self.estimated_stitch_service_time
    }

    pub fn interval(&self) -> Duration {
        Duration::from_secs_f64(1.0 / self.current_fps.max(self.config.min_fps))
    }

    pub fn observe(&mut self, signal: ScrollingGovernorSignal, elapsed: Duration) -> f64 {
        let elapsed_seconds = elapsed.as_secs_f64().clamp(0.0, 0.5);

        if let Some(sample) = signal.stitch_service_time.filter(|value| !value.is_zero()) {
            let had_estimate = self.estimated_stitch_service_time.is_some();
            let estimate = match self.estimated_stitch_service_time {
                None => sample,
                Some(previous) => {
                    let alpha = if sample > previous {
                        self.config.slower_latency_alpha
                    } else {
                        self.config.faster_latency_alpha
                    };
                    let estimate_ns = (alpha * sample.as_nanos() as f64
                        + (1.0 - alpha) * previous.as_nanos() as f64)
                        .clamp(1.0, u64::MAX as f64);
                    Duration::from_nanos(estimate_ns.round() as u64)
                }
            };
            self.estimated_stitch_service_time = Some(estimate);

            let sustainable_fps = self.config.target_utilization / estimate.as_secs_f64();
            let sustainable_fps = sustainable_fps.clamp(self.config.min_fps, self.config.max_fps);
            if sustainable_fps < self.current_fps {
                self.current_fps = sustainable_fps;
            } else if had_estimate && self.config.recovery_fps_per_second > 0.0 {
                self.current_fps = (self.current_fps
                    + self.config.recovery_fps_per_second * elapsed_seconds)
                    .min(sustainable_fps);
            } else {
                self.current_fps = self.current_fps.min(sustainable_fps);
            }
        }

        let queue_pressure = signal.queue_fill.clamp(0.0, 1.0);
        let stitch_pressure = if signal.stitch_pending_depth >= 2 {
            1.0
        } else {
            signal.stitch_pending_depth as f64 / 2.0
        };
        let overloaded = signal.capture_dropped
            || signal.stitch_replaced_frames > 0
            || queue_pressure > 0.75
            || stitch_pressure >= 1.0;
        if overloaded {
            self.current_fps *= self.config.overload_backoff_factor;
        }

        self.current_fps = self
            .current_fps
            .clamp(self.config.min_fps, self.config.max_fps);
        self.current_fps
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample(milliseconds: u64) -> ScrollingGovernorSignal {
        ScrollingGovernorSignal {
            stitch_service_time: Some(Duration::from_millis(milliseconds)),
            ..Default::default()
        }
    }

    #[test]
    fn starts_at_fifteen_fps_without_samples() {
        let governor = ScrollingGovernor::new(ScrollingGovernorConfig::default()).unwrap();
        assert_eq!(governor.current_fps(), 15.0);
        assert_eq!(governor.estimated_stitch_service_time(), None);
    }

    #[test]
    fn throughput_capacity_is_reserved_to_eighty_percent() {
        let mut governor = ScrollingGovernor::new(ScrollingGovernorConfig::default()).unwrap();
        governor.observe(sample(50), Duration::from_secs(1));
        assert_eq!(
            governor.estimated_stitch_service_time(),
            Some(Duration::from_millis(50))
        );
        assert!((governor.current_fps() - 15.0).abs() < f64::EPSILON);
        governor.observe(sample(50), Duration::from_secs(1));
        assert!((governor.current_fps() - 16.0).abs() < f64::EPSILON);
    }

    #[test]
    fn slower_processing_downshifts_immediately_and_clamps_to_four() {
        let mut governor = ScrollingGovernor::new(ScrollingGovernorConfig::default()).unwrap();
        governor.observe(sample(20), Duration::from_secs(1));
        assert_eq!(governor.current_fps(), 15.0);
        governor.observe(sample(1000), Duration::from_secs(1));
        assert_eq!(governor.current_fps(), 4.0);
    }

    #[test]
    fn faster_processing_recovers_gradually_without_motion_input() {
        let mut governor = ScrollingGovernor::new(ScrollingGovernorConfig::default()).unwrap();
        governor.observe(sample(50), Duration::from_secs(1));
        let before = governor.current_fps();
        governor.observe(sample(20), Duration::from_secs(1));
        assert!(governor.current_fps() > before);
        assert!(governor.current_fps() <= 30.0);
    }

    #[test]
    fn duplicate_or_idle_feedback_holds_rate_and_estimate() {
        let mut governor = ScrollingGovernor::new(ScrollingGovernorConfig::default()).unwrap();
        governor.observe(sample(50), Duration::from_secs(1));
        let fps = governor.current_fps();
        let estimate = governor.estimated_stitch_service_time();
        for _ in 0..20 {
            governor.observe(ScrollingGovernorSignal::default(), Duration::from_secs(1));
        }
        assert_eq!(governor.current_fps(), fps);
        assert_eq!(governor.estimated_stitch_service_time(), estimate);
    }

    #[test]
    fn overload_backoff_does_not_require_a_stitch_sample() {
        let mut governor = ScrollingGovernor::new(ScrollingGovernorConfig::default()).unwrap();
        let initial = governor.current_fps();
        governor.observe(
            ScrollingGovernorSignal {
                stitch_pending_depth: 2,
                ..Default::default()
            },
            Duration::from_millis(1),
        );
        assert!(governor.current_fps() < initial);
    }
}

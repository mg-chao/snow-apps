//! Timing aggregation for the compile-time gated recording benchmark groups.
//!
//! The data types here are plain containers with no cost when unused. The
//! collection points in `direct.rs` are gated behind the `bench-*-timing`
//! cargo features, mirroring snow-capture's `stage-timing` contract: with the
//! features disabled (all production builds) no clock is read and no sample is
//! recorded.

use std::collections::BTreeMap;
use std::time::Duration;

/// Percentile summary over recorded [`Duration`] samples.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SampleStats {
    pub count: usize,
    pub min: Duration,
    pub p50: Duration,
    pub p95: Duration,
    pub max: Duration,
}

impl SampleStats {
    /// Summarizes `samples`, sorting it in place. Empty input yields zeros.
    pub fn from_samples(samples: &mut [Duration]) -> Self {
        if samples.is_empty() {
            return Self::default();
        }
        samples.sort_unstable();
        let pick =
            |quantile: f64| samples[(((samples.len() - 1) as f64) * quantile).round() as usize];
        Self {
            count: samples.len(),
            min: samples[0],
            p50: pick(0.50),
            p95: pick(0.95),
            max: samples[samples.len() - 1],
        }
    }
}

/// Named histogram of stage durations, e.g. `compose.resize` per frame.
#[derive(Clone, Debug, Default)]
pub struct StageHistogram {
    samples: BTreeMap<&'static str, Vec<Duration>>,
}

impl StageHistogram {
    pub fn record(&mut self, name: &'static str, duration: Duration) {
        self.samples.entry(name).or_default().push(duration);
    }

    /// Combines `other` into `self`, preserving every sample.
    pub fn merge(&mut self, other: &StageHistogram) {
        for (name, samples) in &other.samples {
            self.samples
                .entry(name)
                .or_default()
                .extend(samples.iter().copied());
        }
    }

    pub fn is_empty(&self) -> bool {
        self.samples.values().all(Vec::is_empty)
    }

    /// Per-stage summaries, ordered by stage name.
    pub fn snapshot(&self) -> BTreeMap<&'static str, SampleStats> {
        self.samples
            .iter()
            .map(|(name, samples)| (*name, SampleStats::from_samples(&mut samples.clone())))
            .collect()
    }
}

/// Per-frame pipeline latency measured on the recording worker.
#[cfg(feature = "bench-pipeline-timing")]
#[derive(Default)]
pub struct PipelineTimings {
    queue_dwell: Vec<Duration>,
    compose: Vec<Duration>,
    encode_push: Vec<Duration>,
    end_to_end: Vec<Duration>,
    started: Option<std::time::Instant>,
    first_encoded: Option<Duration>,
    pub synthetic_overlay_frames: u64,
}

#[cfg(feature = "bench-pipeline-timing")]
impl PipelineTimings {
    /// Marks the session start used for the time-to-first-encoded-frame.
    pub fn begin(&mut self) {
        if self.started.is_none() {
            self.started = Some(std::time::Instant::now());
        }
    }

    /// Records one captured frame's journey through the worker.
    pub fn observe_frame(
        &mut self,
        captured_at: std::time::Instant,
        received: std::time::Instant,
        composed: std::time::Instant,
        pushed: std::time::Instant,
    ) {
        self.queue_dwell
            .push(received.saturating_duration_since(captured_at));
        self.compose
            .push(composed.saturating_duration_since(received));
        self.encode_push
            .push(pushed.saturating_duration_since(composed));
        self.end_to_end
            .push(pushed.saturating_duration_since(captured_at));
        let started = self.started.unwrap_or(received);
        self.first_encoded
            .get_or_insert(pushed.saturating_duration_since(started));
    }

    /// Summarizes the samples together with a final capture stream snapshot.
    pub fn stats(&self, stream: &snow_capture::CaptureStreamStatsSnapshot) -> CapturePipelineStats {
        let mut queue_dwell = self.queue_dwell.clone();
        let mut compose = self.compose.clone();
        let mut encode_push = self.encode_push.clone();
        let mut end_to_end = self.end_to_end.clone();
        CapturePipelineStats {
            queue_dwell: SampleStats::from_samples(&mut queue_dwell),
            compose: SampleStats::from_samples(&mut compose),
            encode_push: SampleStats::from_samples(&mut encode_push),
            end_to_end: SampleStats::from_samples(&mut end_to_end),
            time_to_first_encoded_frame: self.first_encoded,
            synthetic_overlay_frames: self.synthetic_overlay_frames,
            stream_frames_captured: stream.frames_captured,
            stream_frames_dropped: stream.frames_dropped,
            stream_errors_recovered: stream.errors_recovered,
            stream_capture_fps: stream.current_fps,
            stream_target_fps: stream.target_fps,
            stream_buffer_fill: stream.buffer_fill,
            stream_capture_latency_avg: stream.capture_latency_avg,
        }
    }
}

/// Pipeline latency summary attached to [`crate::DirectRecordingReport`].
#[cfg(feature = "bench-pipeline-timing")]
#[derive(Clone, Copy, Debug, Default)]
pub struct CapturePipelineStats {
    /// Capture stream timestamp to the worker receiving the frame.
    pub queue_dwell: SampleStats,
    /// Overlay compositing inside the worker.
    pub compose: SampleStats,
    /// `StreamingEncoder::push_rgba_frame` duration.
    pub encode_push: SampleStats,
    /// Capture stream timestamp to the frame being handed to the encoder.
    pub end_to_end: SampleStats,
    pub time_to_first_encoded_frame: Option<Duration>,
    /// Frames the overlay scheduler composited from the cached latest frame
    /// while the desktop itself was static.
    pub synthetic_overlay_frames: u64,
    pub stream_frames_captured: u64,
    pub stream_frames_dropped: u64,
    pub stream_errors_recovered: u64,
    pub stream_capture_fps: f64,
    pub stream_target_fps: u32,
    pub stream_buffer_fill: u64,
    pub stream_capture_latency_avg: Duration,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sample_stats_reports_ordered_percentiles() {
        let mut samples: Vec<_> = (0..100).map(Duration::from_millis).collect();
        // Reverse so sorting is observable.
        samples.reverse();
        let stats = SampleStats::from_samples(&mut samples);
        assert_eq!(stats.count, 100);
        assert_eq!(stats.min, Duration::from_millis(0));
        // (len - 1) * quantile rounds half away from zero: 99 * 0.5 -> 50, 99 * 0.95 -> 94.
        assert_eq!(stats.p50, Duration::from_millis(50));
        assert_eq!(stats.p95, Duration::from_millis(94));
        assert_eq!(stats.max, Duration::from_millis(99));
    }

    #[test]
    fn sample_stats_of_nothing_is_zero() {
        assert_eq!(SampleStats::from_samples(&mut []), SampleStats::default());
    }

    #[test]
    fn histogram_records_merges_and_snapshots_by_name() {
        let mut histogram = StageHistogram::default();
        assert!(histogram.is_empty());
        histogram.record("compose.cursor", Duration::from_millis(2));
        histogram.record("compose.cursor", Duration::from_millis(4));
        let mut other = StageHistogram::default();
        other.record("compose.resize", Duration::from_millis(1));
        histogram.merge(&other);

        let snapshot = histogram.snapshot();
        assert_eq!(snapshot.len(), 2);
        assert_eq!(snapshot["compose.cursor"].count, 2);
        assert_eq!(snapshot["compose.cursor"].p50, Duration::from_millis(4));
        assert_eq!(snapshot["compose.resize"].count, 1);
        assert!(!histogram.is_empty());
    }

    #[cfg(feature = "bench-pipeline-timing")]
    #[test]
    fn pipeline_timings_derive_latency_breakdowns_from_frame_instants() {
        use std::time::Duration;
        let mut timings = PipelineTimings::default();
        let start = std::time::Instant::now();
        timings.begin();
        let captured = start + Duration::from_millis(100);
        let received = captured + Duration::from_millis(5);
        let composed = received + Duration::from_millis(7);
        let pushed = composed + Duration::from_millis(3);
        timings.observe_frame(captured, received, composed, pushed);
        timings.synthetic_overlay_frames = 2;

        let stats = timings.stats(&snow_capture::CaptureStreamStatsSnapshot::default());
        assert_eq!(stats.queue_dwell.max, Duration::from_millis(5));
        assert_eq!(stats.compose.max, Duration::from_millis(7));
        assert_eq!(stats.encode_push.max, Duration::from_millis(3));
        assert_eq!(stats.end_to_end.max, Duration::from_millis(15));
        assert_eq!(stats.synthetic_overlay_frames, 2);
        // The session start and the test's `start` are two adjacent clock reads, so
        // the first-frame span is 115 ms minus a few nanoseconds.
        assert!(stats.time_to_first_encoded_frame.unwrap() >= Duration::from_millis(100));
    }
}

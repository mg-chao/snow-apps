use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use snow_core::error::{RecvError, RecvTimeoutError, TryRecvError};
use snow_core::event::{DeliveryLane, StreamEvent};
use snow_core::stream_queue::StreamQueue;
use snow_core::timestamp::{StreamTimestamp, TickFormat};

use crate::{CursorCaptureError, CursorSampler, CursorSnapshot};

#[derive(Debug)]
pub enum CursorStreamEvent {
    Sample {
        sample: CursorSnapshot,
        stream_timestamp: StreamTimestamp,
    },
    Paused {
        at: Instant,
    },
    Resumed {
        at: Instant,
        gap: Duration,
    },
    StreamEnded,
    Error(CursorCaptureError),
}

impl StreamEvent for CursorStreamEvent {
    fn is_paused(&self) -> bool {
        matches!(self, Self::Paused { .. })
    }

    fn is_resumed(&self) -> bool {
        matches!(self, Self::Resumed { .. })
    }

    fn is_stream_ended(&self) -> bool {
        matches!(self, Self::StreamEnded)
    }

    fn is_error(&self) -> bool {
        matches!(self, Self::Error(_))
    }

    fn delivery_lane(&self) -> DeliveryLane {
        match self {
            Self::Sample { .. } => DeliveryLane::Data,
            Self::Paused { .. } | Self::Resumed { .. } | Self::StreamEnded | Self::Error(_) => {
                DeliveryLane::Control
            }
        }
    }

    fn timestamp(&self) -> Option<&StreamTimestamp> {
        match self {
            Self::Sample {
                stream_timestamp, ..
            } => Some(stream_timestamp),
            _ => None,
        }
    }
}

#[derive(Clone, Debug)]
pub struct CursorStreamConfig {
    pub poll_interval: Duration,
    pub buffer_depth: usize,
    pub max_consecutive_errors: usize,
}

impl Default for CursorStreamConfig {
    fn default() -> Self {
        Self {
            poll_interval: Duration::from_millis(50),
            buffer_depth: 8,
            max_consecutive_errors: 30,
        }
    }
}

#[derive(Debug)]
pub struct CursorStreamStats {
    pub samples_captured: AtomicU64,
    pub samples_dropped: AtomicU64,
    pub errors_observed: AtomicU64,
    pub buffer_fill: AtomicU64,
}

impl Default for CursorStreamStats {
    fn default() -> Self {
        Self {
            samples_captured: AtomicU64::new(0),
            samples_dropped: AtomicU64::new(0),
            errors_observed: AtomicU64::new(0),
            buffer_fill: AtomicU64::new(0),
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct CursorStreamStatsSnapshot {
    pub samples_captured: u64,
    pub samples_dropped: u64,
    pub errors_observed: u64,
    pub buffer_fill: u64,
}

impl CursorStreamStats {
    pub fn snapshot(&self) -> CursorStreamStatsSnapshot {
        CursorStreamStatsSnapshot {
            samples_captured: self.samples_captured.load(Ordering::Relaxed),
            samples_dropped: self.samples_dropped.load(Ordering::Relaxed),
            errors_observed: self.errors_observed.load(Ordering::Relaxed),
            buffer_fill: self.buffer_fill.load(Ordering::Relaxed),
        }
    }
}

/// A streaming handle that polls [`CursorSampler`] on a dedicated thread.
pub struct CursorStreamHandle {
    queue: Arc<StreamQueue<CursorStreamEvent>>,
    stop_flag: Arc<AtomicBool>,
    pause_flag: Arc<AtomicBool>,
    stats: Arc<CursorStreamStats>,
    join_handle: Option<JoinHandle<()>>,
    buffer_depth: usize,
}

impl CursorStreamHandle {
    pub fn start(config: CursorStreamConfig) -> Result<Self, CursorCaptureError> {
        let sampler = CursorSampler::new()?;
        let buffer_depth = config.buffer_depth.max(1);
        let queue = Arc::new(StreamQueue::new(buffer_depth));
        let stop_flag = Arc::new(AtomicBool::new(false));
        let pause_flag = Arc::new(AtomicBool::new(false));
        let stats = Arc::new(CursorStreamStats::default());

        let worker_queue = Arc::clone(&queue);
        let worker_stop = Arc::clone(&stop_flag);
        let worker_pause = Arc::clone(&pause_flag);
        let worker_stats = Arc::clone(&stats);
        let poll_interval = config.poll_interval;
        let max_consecutive_errors = config.max_consecutive_errors.max(1);

        let join_handle = std::thread::Builder::new()
            .name("snow-cursor-stream".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                poll_loop(
                    sampler,
                    poll_interval,
                    max_consecutive_errors,
                    &worker_queue,
                    &worker_stop,
                    &worker_pause,
                    &worker_stats,
                );
            })
            .map_err(|error| {
                CursorCaptureError::platform(format!(
                    "failed to spawn cursor stream thread: {error}"
                ))
            })?;

        Ok(Self {
            queue,
            stop_flag,
            pause_flag,
            stats,
            join_handle: Some(join_handle),
            buffer_depth,
        })
    }

    pub fn recv(&self) -> Result<CursorStreamEvent, RecvError> {
        self.map_recv_outcome(self.queue.recv())
    }

    pub fn try_recv(&self) -> Result<CursorStreamEvent, TryRecvError> {
        self.map_recv_outcome(self.queue.try_recv())
    }

    pub fn recv_timeout(&self, timeout: Duration) -> Result<CursorStreamEvent, RecvTimeoutError> {
        self.map_recv_outcome(self.queue.recv_timeout(timeout))
    }

    pub fn stop(&self) {
        self.stop_flag.store(true, Ordering::Release);
    }

    pub fn pause(&self) {
        self.pause_flag.store(true, Ordering::Release);
    }

    pub fn resume(&self) {
        self.pause_flag.store(false, Ordering::Release);
    }

    pub fn is_paused(&self) -> bool {
        self.pause_flag.load(Ordering::Acquire)
    }

    pub fn is_running(&self) -> bool {
        self.join_handle
            .as_ref()
            .is_some_and(|join| !join.is_finished())
    }

    pub fn stats(&self) -> &Arc<CursorStreamStats> {
        &self.stats
    }

    pub fn buffer_fill_percent(&self) -> f64 {
        if self.buffer_depth == 0 {
            return 0.0;
        }
        let fill = self.stats.buffer_fill.load(Ordering::Relaxed);
        (fill as f64 / self.buffer_depth as f64).min(1.0)
    }

    fn update_buffer_fill(&self, len: usize) {
        self.stats.buffer_fill.store(len as u64, Ordering::Release);
    }

    fn map_recv_outcome<E>(
        &self,
        outcome: Result<(CursorStreamEvent, usize), E>,
    ) -> Result<CursorStreamEvent, E> {
        outcome.map(|(event, len)| {
            self.update_buffer_fill(len);
            event
        })
    }
}

impl Drop for CursorStreamHandle {
    fn drop(&mut self) {
        self.stop_flag.store(true, Ordering::Release);
        if let Some(join_handle) = self.join_handle.take() {
            let _ = join_handle.join();
        }
        self.queue.close();
    }
}

impl snow_core::streaming::StreamHandle<CursorStreamEvent> for CursorStreamHandle {
    type RecvError = RecvError;
    type TryRecvError = TryRecvError;
    type RecvTimeoutError = RecvTimeoutError;

    fn recv(&self) -> Result<CursorStreamEvent, Self::RecvError> {
        self.recv()
    }

    fn try_recv(&self) -> Result<CursorStreamEvent, Self::TryRecvError> {
        self.try_recv()
    }

    fn recv_timeout(&self, timeout: Duration) -> Result<CursorStreamEvent, Self::RecvTimeoutError> {
        self.recv_timeout(timeout)
    }

    fn stop(&self) {
        self.stop()
    }

    fn pause(&self) {
        self.pause()
    }

    fn resume(&self) {
        self.resume()
    }

    fn is_paused(&self) -> bool {
        self.is_paused()
    }

    fn is_running(&self) -> bool {
        self.is_running()
    }
}

impl snow_core::streaming::StreamStats for CursorStreamHandle {
    fn snapshot(&self) -> snow_core::streaming::StreamStatsSnapshot {
        snow_core::streaming::StreamStatsSnapshot {
            total_events: self.stats.samples_captured.load(Ordering::Relaxed),
            dropped_events: self.stats.samples_dropped.load(Ordering::Relaxed),
            buffer_fill_ratio: self.buffer_fill_percent(),
        }
    }
}

fn poll_loop(
    mut sampler: CursorSampler,
    poll_interval: Duration,
    max_consecutive_errors: usize,
    queue: &StreamQueue<CursorStreamEvent>,
    stop: &AtomicBool,
    pause: &AtomicBool,
    stats: &CursorStreamStats,
) {
    let mut pause_started = None;
    let mut consecutive_errors = 0usize;

    loop {
        if stop.load(Ordering::Acquire) {
            store_buffer_fill(stats, queue.push(CursorStreamEvent::StreamEnded).data_len);
            queue.close();
            return;
        }

        if pause.load(Ordering::Acquire) {
            if pause_started.is_none() {
                let now = Instant::now();
                pause_started = Some(now);
                store_buffer_fill(
                    stats,
                    queue.push(CursorStreamEvent::Paused { at: now }).data_len,
                );
            }
            std::thread::sleep(poll_interval);
            continue;
        }

        if let Some(started) = pause_started.take() {
            let now = Instant::now();
            let gap = now.duration_since(started);
            store_buffer_fill(
                stats,
                queue
                    .push(CursorStreamEvent::Resumed { at: now, gap })
                    .data_len,
            );
        }

        match sampler.sample() {
            Ok(sample) => {
                consecutive_errors = 0;
                let stream_timestamp = StreamTimestamp {
                    instant: Instant::now(),
                    raw_os_ticks: None,
                    tick_format: TickFormat::RawQpc,
                };
                stats.samples_captured.fetch_add(1, Ordering::Relaxed);
                let outcome = queue.push(CursorStreamEvent::Sample {
                    sample,
                    stream_timestamp,
                });
                store_buffer_fill(stats, outcome.data_len);
                if matches!(outcome.dropped, Some(CursorStreamEvent::Sample { .. })) {
                    stats.samples_dropped.fetch_add(1, Ordering::Relaxed);
                }
            }
            Err(error) => {
                consecutive_errors += 1;
                stats.errors_observed.fetch_add(1, Ordering::Relaxed);
                if consecutive_errors >= max_consecutive_errors {
                    store_buffer_fill(stats, queue.push(CursorStreamEvent::Error(error)).data_len);
                    store_buffer_fill(stats, queue.push(CursorStreamEvent::StreamEnded).data_len);
                    queue.close();
                    return;
                }
            }
        }

        std::thread::sleep(poll_interval);
    }
}

fn store_buffer_fill(stats: &CursorStreamStats, len: usize) {
    stats.buffer_fill.store(len as u64, Ordering::Release);
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use snow_core::event::{DeliveryLane, StreamEvent};
    use snow_core::timestamp::TickFormat;

    use super::*;

    fn platform_supported() -> bool {
        CursorSampler::new().is_ok()
    }

    #[test]
    fn sample_events_use_data_lane_and_expose_timestamp() {
        let event = CursorStreamEvent::Sample {
            sample: CursorSnapshot {
                absolute_x: 10,
                absolute_y: 20,
                visible: true,
                shape: crate::CursorShapeCapture::Unavailable,
            },
            stream_timestamp: StreamTimestamp {
                instant: Instant::now(),
                raw_os_ticks: None,
                tick_format: TickFormat::RawQpc,
            },
        };

        assert_eq!(event.delivery_lane(), DeliveryLane::Data);
        assert!(event.timestamp().is_some());
        assert!(!event.is_error());
    }

    #[test]
    fn control_events_use_control_lane() {
        let events = [
            CursorStreamEvent::Paused { at: Instant::now() },
            CursorStreamEvent::Resumed {
                at: Instant::now(),
                gap: Duration::from_millis(5),
            },
            CursorStreamEvent::StreamEnded,
            CursorStreamEvent::Error(CursorCaptureError::UnsupportedPlatform),
        ];

        for event in events {
            assert_eq!(event.delivery_lane(), DeliveryLane::Control);
            assert!(event.timestamp().is_none());
        }
    }

    #[test]
    fn stream_handle_stops_cleanly() {
        let handle = match CursorStreamHandle::start(CursorStreamConfig {
            poll_interval: Duration::from_millis(20),
            buffer_depth: 4,
            max_consecutive_errors: 30,
        }) {
            Ok(handle) => handle,
            Err(CursorCaptureError::UnsupportedPlatform) => return,
            Err(error) => panic!("unexpected stream start failure: {error}"),
        };

        assert!(handle.is_running());
        handle.stop();

        let deadline = Instant::now() + Duration::from_secs(2);
        let mut saw_end = false;
        while Instant::now() < deadline {
            match handle.recv_timeout(Duration::from_millis(100)) {
                Ok(CursorStreamEvent::StreamEnded) => {
                    saw_end = true;
                    break;
                }
                Ok(_) => {}
                Err(_) => break,
            }
        }

        assert!(saw_end);
    }

    #[test]
    fn stream_samples_include_raw_qpc_timestamp() {
        if !platform_supported() {
            return;
        }

        let handle = CursorStreamHandle::start(CursorStreamConfig {
            poll_interval: Duration::from_millis(20),
            buffer_depth: 8,
            max_consecutive_errors: 30,
        })
        .expect("cursor streaming should start on supported platforms");

        let deadline = Instant::now() + Duration::from_secs(2);
        while Instant::now() < deadline {
            match handle.recv_timeout(Duration::from_millis(200)) {
                Ok(CursorStreamEvent::Sample {
                    stream_timestamp, ..
                }) => {
                    assert_eq!(stream_timestamp.tick_format, TickFormat::RawQpc);
                    assert!(stream_timestamp.raw_os_ticks.is_none());
                    return;
                }
                Ok(CursorStreamEvent::Error(error)) => {
                    panic!("unexpected stream error: {error}");
                }
                Ok(_) => {}
                Err(error) => panic!("timed out waiting for sample: {error:?}"),
            }
        }

        panic!("timed out waiting for cursor sample");
    }
}

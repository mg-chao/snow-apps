//! Continuous capture streaming with frame pacing, backpressure, and
//! adaptive rate control.
//!
//! The streaming module runs a capture loop on a dedicated thread,
//! delivering `CaptureEvent`s through a dual-lane stream queue. The caller
//! consumes events from the receiver at its own pace (e.g. feeding
//! an encoder). Data-plane frame events are bounded and droppable,
//! while control-plane lifecycle events remain reliable.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, Instant};

use crossbeam_channel as mpsc;
use snow_core::error::{RecvError, RecvTimeoutError, TryRecvError};
use snow_core::stream_queue::StreamQueue;

use crate::backend::CaptureWorkload;
use crate::capture_session::CaptureSession;
use crate::error::{CaptureError, CaptureResult};
use crate::frame::{CaptureEvent, CapturedFrame, Frame, FrameRecycleSender};
use crate::scrolling::{ScrollingGovernor, ScrollingGovernorConfig, ScrollingGovernorSignal};

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum CaptureRateControl {
    Fixed,
    Backpressure { min_fps: u32 },
    Scrolling(ScrollingGovernorConfig),
}

/// Completion feedback from the serialized scrolling stitch worker.
///
/// Duplicate stitch fast-path completions set `representative` to `false`.
/// Their service time is intentionally excluded from capacity estimation, but
/// their pending/replacement pressure is still retained by the stream.
#[derive(Clone, Copy, Debug, Default)]
pub struct ScrollingStitchFeedback {
    pub service_time: Duration,
    pub representative: bool,
    pub pending_depth: usize,
    pub replaced_frames: u32,
}

/// Configuration for a continuous capture stream.
#[derive(Clone, Debug)]
pub struct CaptureStreamConfig {
    /// Target frames per second. The stream thread will pace captures
    /// to approximate this rate. Set to `0` for uncapped (capture as
    /// fast as the backend allows).
    pub target_fps: u32,
    /// Maximum number of frames buffered in the channel before the
    /// stream starts dropping the oldest frames. Higher values add
    /// latency but tolerate encoder stalls better.
    pub buffer_depth: usize,
    /// Maximum number of consecutive transient errors before the
    /// stream thread gives up and exits.
    pub max_consecutive_errors: usize,
    /// Pacing policy. Recording normally uses `Backpressure`, while scrolling
    /// uses the stitch-throughput governor.
    pub rate_control: CaptureRateControl,
    /// Optional limit for recyclable RGBA frame storage. The pool contains at
    /// least three slots; once all slots are leased, capture waits for a frame
    /// to return instead of allocating beyond the pool.
    pub frame_pool_budget_bytes: Option<usize>,
    /// When `true`, the stream automatically pauses after sending a
    /// `ResolutionChanged` event, giving the consumer time to
    /// reconfigure its encoder before calling `resume()`.
    pub pause_on_resolution_change: bool,
}

impl Default for CaptureStreamConfig {
    fn default() -> Self {
        Self {
            target_fps: 60,
            buffer_depth: 6,
            max_consecutive_errors: 30,
            rate_control: CaptureRateControl::Fixed,
            frame_pool_budget_bytes: None,
            pause_on_resolution_change: false,
        }
    }
}

/// Live statistics about the running stream, updated atomically by the
/// capture thread. Read these from any thread via `CaptureStream::stats()`.
#[derive(Debug)]
pub struct CaptureStreamStats {
    /// Total frames captured since the stream started.
    pub frames_captured: AtomicU64,
    /// Total frames dropped due to backpressure (receiver too slow).
    pub frames_dropped: AtomicU64,
    /// Total transient errors encountered and recovered from.
    pub errors_recovered: AtomicU64,
    /// Current effective FPS (updated once per second).
    pub current_fps: AtomicU64,
    /// Current target selected by the active rate controller.
    pub effective_target_fps: AtomicU64,
    /// EWMA of representative full stitch-worker service time in nanoseconds.
    pub stitch_service_latency_avg_ns: AtomicU64,
    /// Number of representative stitch samples incorporated by the governor.
    pub stitch_samples: AtomicU64,
    /// Number of emergency overload backoffs applied by the governor.
    pub stitch_overload_backoffs: AtomicU64,
    /// Number of frames currently sitting in the channel buffer.
    /// Stored as a plain u64; compare against `CaptureStreamConfig::buffer_depth`
    /// to get a fill percentage.
    pub buffer_fill: AtomicU64,
    /// Exponentially-weighted moving average of per-frame capture
    /// latency in nanoseconds. Useful for detecting GPU readback
    /// bottlenecks. Stored as `f64` bits.
    pub capture_latency_avg_ns: AtomicU64,
    /// Exponentially-weighted moving average of per-frame cursor attach
    /// latency in nanoseconds. Stored as `f64` bits.
    pub cursor_latency_avg_ns: AtomicU64,
    /// Frames that attached cursor data via a backend-native path.
    pub cursor_native_frames: AtomicU64,
    /// Frames that attached cursor data via the GDI fallback path.
    pub cursor_fallback_frames: AtomicU64,
    /// Frames that reused a cached cursor shape.
    pub cursor_shape_cache_hits: AtomicU64,
    /// Frames that emitted a new cursor shape payload.
    pub cursor_shape_cache_misses: AtomicU64,
}

impl Default for CaptureStreamStats {
    fn default() -> Self {
        Self {
            frames_captured: AtomicU64::new(0),
            frames_dropped: AtomicU64::new(0),
            errors_recovered: AtomicU64::new(0),
            current_fps: AtomicU64::new(0),
            effective_target_fps: AtomicU64::new(0),
            stitch_service_latency_avg_ns: AtomicU64::new(0),
            stitch_samples: AtomicU64::new(0),
            stitch_overload_backoffs: AtomicU64::new(0),
            buffer_fill: AtomicU64::new(0),
            capture_latency_avg_ns: AtomicU64::new(0),
            cursor_latency_avg_ns: AtomicU64::new(0),
            cursor_native_frames: AtomicU64::new(0),
            cursor_fallback_frames: AtomicU64::new(0),
            cursor_shape_cache_hits: AtomicU64::new(0),
            cursor_shape_cache_misses: AtomicU64::new(0),
        }
    }
}

impl CaptureStreamStats {
    /// Snapshot the current stats into plain values.
    pub fn snapshot(&self) -> CaptureStreamStatsSnapshot {
        CaptureStreamStatsSnapshot {
            frames_captured: self.frames_captured.load(Ordering::Relaxed),
            frames_dropped: self.frames_dropped.load(Ordering::Relaxed),
            errors_recovered: self.errors_recovered.load(Ordering::Relaxed),
            current_fps: f64::from_bits(self.current_fps.load(Ordering::Relaxed)),
            effective_target_fps: f64::from_bits(self.effective_target_fps.load(Ordering::Relaxed)),
            stitch_service_latency_avg: Duration::from_nanos(f64::from_bits(
                self.stitch_service_latency_avg_ns.load(Ordering::Relaxed),
            ) as u64),
            stitch_samples: self.stitch_samples.load(Ordering::Relaxed),
            stitch_overload_backoffs: self.stitch_overload_backoffs.load(Ordering::Relaxed),
            buffer_fill: self.buffer_fill.load(Ordering::Relaxed),
            capture_latency_avg: Duration::from_nanos(f64::from_bits(
                self.capture_latency_avg_ns.load(Ordering::Relaxed),
            ) as u64),
            cursor_latency_avg: Duration::from_nanos(f64::from_bits(
                self.cursor_latency_avg_ns.load(Ordering::Relaxed),
            ) as u64),
            cursor_native_frames: self.cursor_native_frames.load(Ordering::Relaxed),
            cursor_fallback_frames: self.cursor_fallback_frames.load(Ordering::Relaxed),
            cursor_shape_cache_hits: self.cursor_shape_cache_hits.load(Ordering::Relaxed),
            cursor_shape_cache_misses: self.cursor_shape_cache_misses.load(Ordering::Relaxed),
        }
    }
}

/// A point-in-time copy of stream statistics.
#[derive(Clone, Debug, Default)]
pub struct CaptureStreamStatsSnapshot {
    pub frames_captured: u64,
    pub frames_dropped: u64,
    pub errors_recovered: u64,
    pub current_fps: f64,
    pub effective_target_fps: f64,
    pub stitch_service_latency_avg: Duration,
    pub stitch_samples: u64,
    pub stitch_overload_backoffs: u64,
    /// Number of frames currently buffered in the channel.
    pub buffer_fill: u64,
    /// Exponentially-weighted moving average of per-frame capture latency.
    pub capture_latency_avg: Duration,
    /// Exponentially-weighted moving average of per-frame cursor attach latency.
    pub cursor_latency_avg: Duration,
    pub cursor_native_frames: u64,
    pub cursor_fallback_frames: u64,
    pub cursor_shape_cache_hits: u64,
    pub cursor_shape_cache_misses: u64,
}

/// Handle to a running capture stream. Dropping the handle stops the
/// background capture thread.
pub struct CaptureStream {
    queue: Arc<StreamQueue<CaptureEvent>>,
    stop_flag: Arc<AtomicBool>,
    pause_flag: Arc<AtomicBool>,
    stats: Arc<CaptureStreamStats>,
    feedback: Arc<CaptureStreamFeedback>,
    join_handle: Option<std::thread::JoinHandle<()>>,
    buffer_depth: usize,
}

#[derive(Debug, Default)]
struct CaptureStreamFeedback {
    stitch_service_time_max_ns: AtomicU64,
    stitch_pending_depth: AtomicU64,
    stitch_replaced_frames: AtomicU64,
}

/// Cheap cloneable handle used by native consumers to report stitch work
/// without sending commands through the delivery thread.
#[derive(Clone, Debug)]
pub struct CaptureStreamFeedbackSender {
    feedback: Arc<CaptureStreamFeedback>,
}

impl CaptureStreamFeedbackSender {
    pub fn report_stitch_feedback(&self, feedback: ScrollingStitchFeedback) {
        if feedback.representative && !feedback.service_time.is_zero() {
            atomic_max(
                &self.feedback.stitch_service_time_max_ns,
                feedback.service_time.as_nanos().min(u64::MAX as u128) as u64,
            );
        }
        atomic_max(
            &self.feedback.stitch_pending_depth,
            feedback.pending_depth.min(u64::MAX as usize) as u64,
        );
        if feedback.replaced_frames != 0 {
            self.feedback
                .stitch_replaced_frames
                .fetch_add(feedback.replaced_frames as u64, Ordering::AcqRel);
        }
    }
}

impl CaptureStream {
    fn request_stop_and_join(&mut self) {
        self.stop_flag.store(true, Ordering::Release);
        if let Some(handle) = self.join_handle.take() {
            let _ = handle.join();
        }
    }

    fn update_buffer_fill(&self, len: usize) {
        self.stats.buffer_fill.store(len as u64, Ordering::Release);
    }

    fn map_recv_outcome<E>(
        &self,
        outcome: Result<(CaptureEvent, usize), E>,
    ) -> Result<CaptureEvent, E> {
        outcome.map(|(event, len)| {
            self.update_buffer_fill(len);
            event
        })
    }

    /// Start the streaming capture loop on a background thread.
    pub fn spawn(mut capture: CaptureSession, config: CaptureStreamConfig) -> CaptureResult<Self> {
        if capture.workload() != CaptureWorkload::Continuous {
            return Err(CaptureError::InvalidConfig(
                "streaming requires CaptureWorkload::Continuous".into(),
            ));
        }
        match config.rate_control {
            CaptureRateControl::Fixed => {}
            CaptureRateControl::Backpressure { min_fps } => {
                if min_fps == 0 || config.target_fps == 0 || min_fps > config.target_fps {
                    return Err(CaptureError::InvalidConfig(
                        "backpressure rate control requires 0 < min_fps <= target_fps".into(),
                    ));
                }
            }
            CaptureRateControl::Scrolling(governor) => {
                governor
                    .validate()
                    .map_err(|message| CaptureError::InvalidConfig(message.to_owned()))?;
                if config.target_fps == 0 || governor.max_fps > config.target_fps as f64 {
                    return Err(CaptureError::InvalidConfig(
                        "scrolling rate control must not exceed target_fps".into(),
                    ));
                }
            }
        }
        capture.prepare_target()?;

        let buffer_depth = config.buffer_depth.max(1);
        let queue = Arc::new(StreamQueue::new(buffer_depth));
        let initial_target_info = capture.target_info().ok();
        let bounded_frame_pool =
            config.frame_pool_budget_bytes.is_some() && initial_target_info.is_some();
        let desired_recycle_depth = buffer_depth.saturating_mul(3).max(8);
        let recycle_depth = initial_target_info
            .and_then(|target| {
                config.frame_pool_budget_bytes.map(|budget| {
                    let frame_bytes = (target.width as usize)
                        .checked_mul(target.height as usize)
                        .and_then(|pixels| pixels.checked_mul(4));
                    if frame_bytes.is_none_or(|bytes| bytes == 0) {
                        desired_recycle_depth
                    } else {
                        (budget / frame_bytes.unwrap()).clamp(3, desired_recycle_depth)
                    }
                })
            })
            .unwrap_or(desired_recycle_depth);
        let (recycle_tx, recycle_rx) = mpsc::bounded::<Frame>(recycle_depth);
        let recycler = FrameRecycleSender::new(recycle_tx.clone());

        if let Some(target_info) = initial_target_info {
            for _ in 0..recycle_depth {
                let mut frame = Frame::empty();
                if frame
                    .ensure_rgba_capacity(target_info.width, target_info.height)
                    .is_ok()
                {
                    let _ = recycle_tx.try_send(frame);
                }
            }
        }

        let stop_flag = Arc::new(AtomicBool::new(false));
        let pause_flag = Arc::new(AtomicBool::new(false));
        let stats = Arc::new(CaptureStreamStats::default());
        let feedback = Arc::new(CaptureStreamFeedback::default());

        let worker_queue = Arc::clone(&queue);
        let stop = stop_flag.clone();
        let pause = pause_flag.clone();
        let stats_clone = stats.clone();
        let feedback_clone = feedback.clone();

        let join_handle = std::thread::Builder::new()
            .name("snow-capture-stream".to_string())
            .spawn(move || {
                stream_loop(
                    &mut capture,
                    &config,
                    &worker_queue,
                    &recycle_rx,
                    &recycler,
                    &stop,
                    &pause,
                    &stats_clone,
                    &feedback_clone,
                    bounded_frame_pool,
                );
                worker_queue.close();
            })
            .map_err(|e| {
                CaptureError::platform(anyhow::anyhow!(
                    "failed to spawn capture stream thread: {e}"
                ))
            })?;

        Ok(Self {
            queue,
            stop_flag,
            pause_flag,
            stats,
            feedback,
            join_handle: Some(join_handle),
            buffer_depth,
        })
    }

    /// Receive the next capture event, blocking until one is available
    /// or the channel disconnects. Automatically updates `buffer_fill`
    /// when a `Frame` event is consumed.
    pub fn recv(&self) -> Result<CaptureEvent, RecvError> {
        self.map_recv_outcome(self.queue.recv())
    }

    /// Try to receive a capture event without blocking. Automatically
    /// updates `buffer_fill` when a `Frame` event is consumed.
    pub fn try_recv(&self) -> Result<CaptureEvent, TryRecvError> {
        self.map_recv_outcome(self.queue.try_recv())
    }

    /// Receive a capture event with a timeout. Automatically updates
    /// `buffer_fill` when a `Frame` event is consumed.
    pub fn recv_timeout(&self, timeout: Duration) -> Result<CaptureEvent, RecvTimeoutError> {
        self.map_recv_outcome(self.queue.recv_timeout(timeout))
    }

    /// exit on its next loop iteration.
    pub fn stop(&self) {
        self.stop_flag.store(true, Ordering::Release);
    }

    /// Pause the capture stream. The capture thread idles without
    /// releasing the underlying OS capture resources, so resume is
    /// near-instant.
    pub fn pause(&self) {
        self.pause_flag.store(true, Ordering::Release);
    }

    /// Resume a paused capture stream.
    pub fn resume(&self) {
        self.pause_flag.store(false, Ordering::Release);
    }

    /// Whether the stream is currently paused.
    pub fn is_paused(&self) -> bool {
        self.pause_flag.load(Ordering::Acquire)
    }

    /// Check whether the stream thread is still running.
    pub fn is_running(&self) -> bool {
        self.join_handle.as_ref().is_some_and(|h| !h.is_finished())
    }

    /// Get a reference to the live stream statistics.
    pub fn stats(&self) -> &Arc<CaptureStreamStats> {
        &self.stats
    }

    /// Return a cloneable sender for stitch-worker timing feedback.
    pub fn feedback_sender(&self) -> CaptureStreamFeedbackSender {
        CaptureStreamFeedbackSender {
            feedback: Arc::clone(&self.feedback),
        }
    }

    /// Current buffer fill level as a fraction in `[0.0, 1.0]`.
    /// Useful for proactive quality adjustment before drops occur.
    pub fn buffer_fill_percent(&self) -> f64 {
        if self.buffer_depth == 0 {
            return 0.0;
        }
        let fill = self.stats.buffer_fill.load(Ordering::Relaxed);
        (fill as f64 / self.buffer_depth as f64).min(1.0)
    }

    /// Signal the stream to stop and drain all remaining buffered
    /// events so the recorder can flush its encoder without losing
    /// the tail frames.
    pub fn stop_and_drain(mut self) -> Vec<CaptureEvent> {
        self.request_stop_and_join();
        let events = self.queue.drain();
        self.queue.close();
        events
    }
}

impl Drop for CaptureStream {
    fn drop(&mut self) {
        self.request_stop_and_join();
        self.queue.close();
    }
}

impl snow_core::streaming::StreamHandle<CaptureEvent> for CaptureStream {
    type RecvError = RecvError;
    type TryRecvError = TryRecvError;
    type RecvTimeoutError = RecvTimeoutError;

    fn recv(&self) -> Result<CaptureEvent, Self::RecvError> {
        self.recv()
    }

    fn try_recv(&self) -> Result<CaptureEvent, Self::TryRecvError> {
        self.try_recv()
    }

    fn recv_timeout(&self, timeout: Duration) -> Result<CaptureEvent, Self::RecvTimeoutError> {
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

impl snow_core::streaming::StreamStats for CaptureStream {
    fn snapshot(&self) -> snow_core::streaming::StreamStatsSnapshot {
        snow_core::streaming::StreamStatsSnapshot {
            total_events: self.stats.frames_captured.load(Ordering::Relaxed),
            dropped_events: self.stats.frames_dropped.load(Ordering::Relaxed),
            buffer_fill_ratio: self.buffer_fill_percent(),
        }
    }
}

fn stream_loop(
    capture: &mut CaptureSession,
    config: &CaptureStreamConfig,
    queue: &StreamQueue<CaptureEvent>,
    recycle_rx: &mpsc::Receiver<Frame>,
    recycler: &FrameRecycleSender,
    stop: &AtomicBool,
    pause: &AtomicBool,
    stats: &CaptureStreamStats,
    feedback: &CaptureStreamFeedback,
    bounded_frame_pool: bool,
) {
    let base_interval = if config.target_fps > 0 {
        Some(Duration::from_secs_f64(1.0 / config.target_fps as f64))
    } else {
        None
    };

    let mut rate_controller = RuntimeRateController::new(config.rate_control, base_interval);
    if let Some(interval) = rate_controller.interval() {
        stats.effective_target_fps.store(
            (1.0 / interval.as_secs_f64().max(f64::EPSILON)).to_bits(),
            Ordering::Relaxed,
        );
    }

    let mut reuse_frame: Option<Frame> = None;
    let mut consecutive_errors: usize = 0;
    let mut last_width: u32 = 0;
    let mut last_height: u32 = 0;

    let mut latency_avg_ns: f64 = 0.0;
    const LATENCY_ALPHA: f64 = 0.1;
    let mut cursor_latency_avg_ns: f64 = 0.0;

    let mut fps_counter: u64 = 0;
    let mut fps_epoch = Instant::now();

    let mut was_paused = false;
    let mut pause_started: Option<Instant> = None;

    loop {
        if stop.load(Ordering::Acquire) {
            break;
        }

        if pause.load(Ordering::Acquire) {
            if !was_paused {
                let now = Instant::now();
                pause_started = Some(now);
                store_queue_fill(stats, queue.push(CaptureEvent::Paused { at: now }).data_len);
                was_paused = true;
            }
            std::thread::sleep(Duration::from_millis(50));
            fps_counter = 0;
            fps_epoch = Instant::now();
            continue;
        } else if was_paused {
            let now = Instant::now();
            let gap = pause_started
                .map(|s| now.saturating_duration_since(s))
                .unwrap_or(Duration::ZERO);
            store_queue_fill(
                stats,
                queue.push(CaptureEvent::Resumed { at: now, gap }).data_len,
            );
            was_paused = false;
            pause_started = None;
        }

        let frame_start = Instant::now();

        drain_recycled_frames(recycle_rx, &mut reuse_frame);
        if bounded_frame_pool && reuse_frame.is_none() {
            match recycle_rx.recv_timeout(Duration::from_millis(5)) {
                Ok(frame) => reuse_frame = Some(frame),
                Err(mpsc::RecvTimeoutError::Timeout) => continue,
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            }
        }

        let capture_result = match reuse_frame.take() {
            Some(f) => capture.capture_with_cursor(Some(f)),
            None => capture.capture_with_cursor(None),
        };

        let capture_elapsed = frame_start.elapsed();

        match capture_result {
            Ok((mut frame, cursor_outcome)) => {
                consecutive_errors = 0;

                if let Some(cursor_outcome) = cursor_outcome {
                    let cursor_stats = cursor_outcome.stats;
                    let cursor_elapsed_ns = cursor_outcome.elapsed.as_nanos() as f64;
                    cursor_latency_avg_ns = LATENCY_ALPHA * cursor_elapsed_ns
                        + (1.0 - LATENCY_ALPHA) * cursor_latency_avg_ns;
                    stats
                        .cursor_latency_avg_ns
                        .store(cursor_latency_avg_ns.to_bits(), Ordering::Relaxed);
                    if cursor_stats.used_native {
                        stats.cursor_native_frames.fetch_add(1, Ordering::Relaxed);
                    }
                    if cursor_stats.used_fallback {
                        stats.cursor_fallback_frames.fetch_add(1, Ordering::Relaxed);
                    }
                    if cursor_stats.shape_cache_hit {
                        stats
                            .cursor_shape_cache_hits
                            .fetch_add(1, Ordering::Relaxed);
                    }
                    if cursor_stats.shape_cache_miss {
                        stats
                            .cursor_shape_cache_misses
                            .fetch_add(1, Ordering::Relaxed);
                    }
                }

                frame.metadata.capture_duration = Some(capture_elapsed);

                let sample_ns = capture_elapsed.as_nanos() as f64;
                latency_avg_ns = LATENCY_ALPHA * sample_ns + (1.0 - LATENCY_ALPHA) * latency_avg_ns;
                stats
                    .capture_latency_avg_ns
                    .store(latency_avg_ns.to_bits(), Ordering::Relaxed);

                let (w, h) = frame.dimensions();
                if last_width != 0 && last_height != 0 && (w != last_width || h != last_height) {
                    let event = CaptureEvent::ResolutionChanged {
                        old_width: last_width,
                        old_height: last_height,
                        new_width: w,
                        new_height: h,
                    };
                    store_queue_fill(stats, queue.push(event).data_len);

                    if config.pause_on_resolution_change {
                        pause.store(true, Ordering::Release);
                    }
                }
                last_width = w;
                last_height = h;

                stats.frames_captured.fetch_add(1, Ordering::Relaxed);
                let outcome = queue.push(CaptureEvent::Frame(
                    CapturedFrame::from_frame_with_recycler(frame, recycler.clone()),
                ));
                store_queue_fill(stats, outcome.data_len);
                let was_dropped = match outcome.dropped {
                    Some(CaptureEvent::Frame(dropped)) => {
                        stats.frames_dropped.fetch_add(1, Ordering::Relaxed);
                        store_queue_fill(
                            stats,
                            queue
                                .push(CaptureEvent::FramesDropped { count: 1 })
                                .data_len,
                        );
                        if let Ok(frame) = dropped.into_owned() {
                            reuse_frame = Some(frame);
                        }
                        true
                    }
                    Some(_) => false,
                    None => false,
                };

                let stitch_service_time = feedback
                    .stitch_service_time_max_ns
                    .swap(0, Ordering::AcqRel)
                    .checked_into_duration();
                let stitch_pending_depth =
                    feedback.stitch_pending_depth.swap(0, Ordering::AcqRel) as usize;
                let stitch_replaced_frames = feedback
                    .stitch_replaced_frames
                    .swap(0, Ordering::AcqRel)
                    .min(u32::MAX as u64) as u32;
                let current_interval = rate_controller.observe(
                    RateObservation {
                        dropped: was_dropped,
                        queue_fill: outcome.data_len as f64 / config.buffer_depth.max(1) as f64,
                        stitch_service_time,
                        stitch_pending_depth,
                        stitch_replaced_frames,
                    },
                    frame_start.elapsed(),
                );
                let target_fps = current_interval.map_or(0.0, |interval| {
                    1.0 / interval.as_secs_f64().max(f64::EPSILON)
                });
                stats
                    .effective_target_fps
                    .store(target_fps.to_bits(), Ordering::Relaxed);
                if let Some(estimated) = rate_controller.stitch_service_time() {
                    stats
                        .stitch_service_latency_avg_ns
                        .store((estimated.as_nanos() as f64).to_bits(), Ordering::Relaxed);
                }
                if rate_controller.last_sample_was_representative() {
                    stats.stitch_samples.fetch_add(1, Ordering::Relaxed);
                }
                if rate_controller.last_observation_overloaded() {
                    stats
                        .stitch_overload_backoffs
                        .fetch_add(1, Ordering::Relaxed);
                }
            }
            Err(ref e) if e.is_retryable() => {
                consecutive_errors += 1;
                stats.errors_recovered.fetch_add(1, Ordering::Relaxed);
                if consecutive_errors >= config.max_consecutive_errors {
                    store_queue_fill(stats, queue.push(CaptureEvent::Error(e.clone())).data_len);
                    break;
                }
                std::thread::sleep(Duration::from_millis(16));
                continue;
            }
            Err(e) => {
                store_queue_fill(stats, queue.push(CaptureEvent::Error(e.clone())).data_len);
                break;
            }
        }

        fps_counter += 1;
        let fps_elapsed = fps_epoch.elapsed();
        if fps_elapsed >= Duration::from_secs(1) {
            let fps = fps_counter as f64 / fps_elapsed.as_secs_f64();
            stats.current_fps.store(fps.to_bits(), Ordering::Relaxed);
            fps_counter = 0;
            fps_epoch = Instant::now();
        }

        if let Some(interval) = rate_controller.interval() {
            let elapsed = frame_start.elapsed();
            if elapsed < interval {
                spin_sleep(interval - elapsed);
            }
        }
    }

    // Send StreamEnded sentinel so the consumer knows no more events
    // will arrive and can flush its encoder.
    store_queue_fill(stats, queue.push(CaptureEvent::StreamEnded).data_len);
}

#[derive(Clone, Copy)]
struct RateObservation {
    dropped: bool,
    queue_fill: f64,
    stitch_service_time: Option<Duration>,
    stitch_pending_depth: usize,
    stitch_replaced_frames: u32,
}

enum RuntimeRateController {
    Fixed {
        interval: Option<Duration>,
    },
    Backpressure {
        base: Option<Duration>,
        maximum: Option<Duration>,
        current: Option<Duration>,
        drops: u32,
        total: u32,
    },
    Scrolling {
        governor: ScrollingGovernor,
        last_observation: Instant,
        last_sample_representative: bool,
        last_overloaded: bool,
    },
}

impl RuntimeRateController {
    fn new(policy: CaptureRateControl, base: Option<Duration>) -> Self {
        match policy {
            CaptureRateControl::Fixed => Self::Fixed { interval: base },
            CaptureRateControl::Backpressure { min_fps } => Self::Backpressure {
                base,
                maximum: (min_fps > 0).then(|| Duration::from_secs_f64(1.0 / min_fps as f64)),
                current: base,
                drops: 0,
                total: 0,
            },
            CaptureRateControl::Scrolling(config) => Self::Scrolling {
                governor: ScrollingGovernor::new(config)
                    .expect("validated capture stream scrolling rate control"),
                last_observation: Instant::now(),
                last_sample_representative: false,
                last_overloaded: false,
            },
        }
    }

    fn interval(&self) -> Option<Duration> {
        match self {
            Self::Fixed { interval } => *interval,
            Self::Backpressure { current, .. } => *current,
            Self::Scrolling { governor, .. } => Some(governor.interval()),
        }
    }

    fn observe(
        &mut self,
        observation: RateObservation,
        fallback_elapsed: Duration,
    ) -> Option<Duration> {
        match self {
            Self::Fixed { interval } => *interval,
            Self::Backpressure {
                base,
                maximum,
                current,
                drops,
                total,
            } => {
                const WINDOW: u32 = 30;
                *total += 1;
                *drops += u32::from(observation.dropped);
                if *total >= WINDOW {
                    if let (Some(value), Some(base), Some(maximum)) = (*current, *base, *maximum) {
                        let drop_ratio = *drops as f64 / *total as f64;
                        let value_ns = value.as_nanos() as f64;
                        let target_ns = if drop_ratio > 0.10 {
                            (value_ns * 1.5).min(maximum.as_nanos() as f64)
                        } else {
                            (value_ns * 0.8).max(base.as_nanos() as f64)
                        };
                        let smoothed = 0.15 * target_ns + 0.85 * value_ns;
                        *current = Some(Duration::from_nanos(smoothed as u64));
                    }
                    *drops = 0;
                    *total = 0;
                }
                *current
            }
            Self::Scrolling {
                governor,
                last_observation,
                last_sample_representative,
                last_overloaded,
            } => {
                let now = Instant::now();
                let elapsed = now
                    .checked_duration_since(*last_observation)
                    .unwrap_or(fallback_elapsed);
                *last_observation = now;
                *last_sample_representative = observation.stitch_service_time.is_some();
                *last_overloaded = observation.dropped
                    || observation.stitch_replaced_frames > 0
                    || observation.queue_fill > 0.75
                    || observation.stitch_pending_depth >= 2;
                governor.observe(
                    ScrollingGovernorSignal {
                        stitch_service_time: observation.stitch_service_time,
                        stitch_pending_depth: observation.stitch_pending_depth,
                        stitch_replaced_frames: observation.stitch_replaced_frames,
                        queue_fill: observation.queue_fill,
                        capture_dropped: observation.dropped,
                    },
                    elapsed,
                );
                Some(governor.interval())
            }
        }
    }

    fn stitch_service_time(&self) -> Option<Duration> {
        match self {
            Self::Scrolling { governor, .. } => governor.estimated_stitch_service_time(),
            _ => None,
        }
    }

    fn last_sample_was_representative(&self) -> bool {
        matches!(
            self,
            Self::Scrolling {
                last_sample_representative: true,
                ..
            }
        )
    }

    fn last_observation_overloaded(&self) -> bool {
        matches!(
            self,
            Self::Scrolling {
                last_overloaded: true,
                ..
            }
        )
    }
}

fn atomic_max(target: &AtomicU64, value: u64) {
    let mut current = target.load(Ordering::Acquire);
    while current < value {
        match target.compare_exchange_weak(current, value, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => break,
            Err(observed) => current = observed,
        }
    }
}

trait CheckedDuration {
    fn checked_into_duration(self) -> Option<Duration>;
}

impl CheckedDuration for u64 {
    fn checked_into_duration(self) -> Option<Duration> {
        (!self.eq(&0)).then(|| Duration::from_nanos(self))
    }
}

fn drain_recycled_frames(recycle_rx: &mpsc::Receiver<Frame>, reuse_frame: &mut Option<Frame>) {
    if reuse_frame.is_none()
        && let Ok(candidate) = recycle_rx.try_recv()
    {
        *reuse_frame = Some(candidate);
    }
}

fn store_queue_fill(stats: &CaptureStreamStats, len: usize) {
    stats.buffer_fill.store(len as u64, Ordering::Release);
}

/// High-precision sleep that uses spin-waiting for the final sub-millisecond
/// portion to avoid Windows timer resolution issues.
fn spin_sleep(duration: Duration) {
    const SPIN_THRESHOLD: Duration = Duration::from_micros(1500);
    let target = Instant::now() + duration;

    if duration > SPIN_THRESHOLD {
        std::thread::sleep(duration - SPIN_THRESHOLD);
    }

    while Instant::now() < target {
        std::hint::spin_loop();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recycle_acquisition_preserves_spare_pool_slots() {
        let (tx, rx) = mpsc::bounded(3);
        for _ in 0..3 {
            tx.send(Frame::empty()).unwrap();
        }
        let mut reuse = None;
        drain_recycled_frames(&rx, &mut reuse);
        assert!(reuse.is_some());
        assert_eq!(rx.len(), 2);

        drain_recycled_frames(&rx, &mut reuse);
        assert_eq!(rx.len(), 2, "an owned reuse frame must not drain spares");
    }
}

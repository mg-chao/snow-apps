//! Continuous capture streaming with frame pacing, backpressure, and
//! adaptive rate control.
//!
//! The streaming module runs a capture loop on a dedicated thread,
//! delivering `CaptureEvent`s through a dual-lane stream queue. The caller
//! consumes events from the receiver at its own pace (e.g. feeding
//! an encoder). Data-plane frame events are bounded and droppable,
//! while control-plane lifecycle events remain reliable.

use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crossbeam_channel as mpsc;
use snow_core::cancellation::CancellationToken;
use snow_core::error::{RecvError, RecvTimeoutError, TryRecvError};
use snow_core::stream_queue::StreamQueue;
#[cfg(windows)]
use snow_media::follow_mouse::FollowMouseOptions;

use crate::backend::CaptureWorkload;
use crate::capture_session::CaptureSession;
use crate::error::{CaptureError, CaptureResult, DisplayRefreshRetry};
use crate::frame::{CaptureEvent, CapturedFrame, Frame, FrameRecycleSender};

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
    /// Maximum number of consecutive failed transient attempts before the
    /// stream thread gives up and exits. Repeated reads of a cached display
    /// refresh failure count as one attempt.
    pub max_consecutive_errors: usize,
    /// Enable adaptive frame rate reduction under sustained
    /// backpressure. When the receiver can't keep up, the capture
    /// rate is temporarily halved (down to `min_fps`), then ramped
    /// back up when the consumer catches up.
    pub adaptive_fps: bool,
    /// Minimum FPS when adaptive rate reduction is active.
    /// Ignored when `adaptive_fps` is `false`.
    pub min_fps: u32,
    /// When `true`, the stream automatically pauses after sending a
    /// `ResolutionChanged` event, giving the consumer time to
    /// reconfigure its encoder before calling `resume()`.
    pub pause_on_resolution_change: bool,
    /// Attach the current cursor sample to every frame. Recording keeps this
    /// enabled by default; screenshot consumers can disable it to preserve
    /// the captured desktop pixels exactly.
    pub include_cursor: bool,
}

impl Default for CaptureStreamConfig {
    fn default() -> Self {
        Self {
            target_fps: 60,
            buffer_depth: 6,
            max_consecutive_errors: 30,
            adaptive_fps: false,
            min_fps: 15,
            pause_on_resolution_change: false,
            include_cursor: true,
        }
    }
}

/// Live statistics about the running stream, updated atomically by the
/// capture thread. Read these from any thread via `CaptureStream::stats()`.
#[derive(Debug)]
pub struct CaptureStreamStats {
    /// Mouse-follow destination preparations requested during this stream.
    pub follow_prepare_requests: AtomicU64,
    pub follow_monitor_switches: AtomicU64,
    pub follow_preparation_failures: AtomicU64,
    /// Maximum simultaneously owned active/preparing/retiring native sources.
    pub follow_peak_sources: AtomicU64,
    pub follow_prepare_latency_max_ns: AtomicU64,
    /// Total frames captured since the stream started.
    pub frames_captured: AtomicU64,
    /// Total frames dropped due to backpressure (receiver too slow).
    pub frames_dropped: AtomicU64,
    /// Total transient errors encountered and recovered from.
    pub errors_recovered: AtomicU64,
    /// Current effective FPS (updated once per second).
    pub current_fps: AtomicU64,
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
    /// Only present in builds with the `stage-timing` feature.
    #[cfg(feature = "stage-timing")]
    pub cursor_latency_avg_ns: AtomicU64,
    /// Frames that attached cursor data via a backend-native path.
    /// Only present in builds with the `stage-timing` feature.
    #[cfg(feature = "stage-timing")]
    pub cursor_native_frames: AtomicU64,
    /// Frames that attached cursor data via the GDI fallback path.
    /// Only present in builds with the `stage-timing` feature.
    #[cfg(feature = "stage-timing")]
    pub cursor_fallback_frames: AtomicU64,
    /// Frames that reused a cached cursor shape.
    /// Only present in builds with the `stage-timing` feature.
    #[cfg(feature = "stage-timing")]
    pub cursor_shape_cache_hits: AtomicU64,
    /// Frames that emitted a new cursor shape payload.
    /// Only present in builds with the `stage-timing` feature.
    #[cfg(feature = "stage-timing")]
    pub cursor_shape_cache_misses: AtomicU64,
    /// Current externally requested target FPS. Zero means uncapped.
    pub target_fps: AtomicU64,
}

impl Default for CaptureStreamStats {
    fn default() -> Self {
        Self {
            follow_prepare_requests: AtomicU64::new(0),
            follow_monitor_switches: AtomicU64::new(0),
            follow_preparation_failures: AtomicU64::new(0),
            follow_peak_sources: AtomicU64::new(0),
            follow_prepare_latency_max_ns: AtomicU64::new(0),
            frames_captured: AtomicU64::new(0),
            frames_dropped: AtomicU64::new(0),
            errors_recovered: AtomicU64::new(0),
            current_fps: AtomicU64::new(0),
            buffer_fill: AtomicU64::new(0),
            capture_latency_avg_ns: AtomicU64::new(0),
            #[cfg(feature = "stage-timing")]
            cursor_latency_avg_ns: AtomicU64::new(0),
            #[cfg(feature = "stage-timing")]
            cursor_native_frames: AtomicU64::new(0),
            #[cfg(feature = "stage-timing")]
            cursor_fallback_frames: AtomicU64::new(0),
            #[cfg(feature = "stage-timing")]
            cursor_shape_cache_hits: AtomicU64::new(0),
            #[cfg(feature = "stage-timing")]
            cursor_shape_cache_misses: AtomicU64::new(0),
            target_fps: AtomicU64::new(0),
        }
    }
}

impl CaptureStreamStats {
    /// Snapshot the current stats into plain values.
    pub fn snapshot(&self) -> CaptureStreamStatsSnapshot {
        CaptureStreamStatsSnapshot {
            follow_prepare_requests: self.follow_prepare_requests.load(Ordering::Relaxed),
            follow_monitor_switches: self.follow_monitor_switches.load(Ordering::Relaxed),
            follow_preparation_failures: self.follow_preparation_failures.load(Ordering::Relaxed),
            follow_peak_sources: self.follow_peak_sources.load(Ordering::Relaxed),
            follow_prepare_latency_max: Duration::from_nanos(
                self.follow_prepare_latency_max_ns.load(Ordering::Relaxed),
            ),
            frames_captured: self.frames_captured.load(Ordering::Relaxed),
            frames_dropped: self.frames_dropped.load(Ordering::Relaxed),
            errors_recovered: self.errors_recovered.load(Ordering::Relaxed),
            current_fps: f64::from_bits(self.current_fps.load(Ordering::Relaxed)),
            buffer_fill: self.buffer_fill.load(Ordering::Relaxed),
            capture_latency_avg: Duration::from_nanos(f64::from_bits(
                self.capture_latency_avg_ns.load(Ordering::Relaxed),
            ) as u64),
            #[cfg(feature = "stage-timing")]
            cursor_latency_avg: Duration::from_nanos(f64::from_bits(
                self.cursor_latency_avg_ns.load(Ordering::Relaxed),
            ) as u64),
            #[cfg(feature = "stage-timing")]
            cursor_native_frames: self.cursor_native_frames.load(Ordering::Relaxed),
            #[cfg(feature = "stage-timing")]
            cursor_fallback_frames: self.cursor_fallback_frames.load(Ordering::Relaxed),
            #[cfg(feature = "stage-timing")]
            cursor_shape_cache_hits: self.cursor_shape_cache_hits.load(Ordering::Relaxed),
            #[cfg(feature = "stage-timing")]
            cursor_shape_cache_misses: self.cursor_shape_cache_misses.load(Ordering::Relaxed),
            target_fps: self.target_fps.load(Ordering::Relaxed) as u32,
        }
    }
}

/// A point-in-time copy of stream statistics.
#[derive(Clone, Debug, Default)]
pub struct CaptureStreamStatsSnapshot {
    pub follow_prepare_requests: u64,
    pub follow_monitor_switches: u64,
    pub follow_preparation_failures: u64,
    pub follow_peak_sources: u64,
    pub follow_prepare_latency_max: Duration,
    pub frames_captured: u64,
    pub frames_dropped: u64,
    pub errors_recovered: u64,
    pub current_fps: f64,
    /// Number of frames currently buffered in the channel.
    pub buffer_fill: u64,
    /// Exponentially-weighted moving average of per-frame capture latency.
    pub capture_latency_avg: Duration,
    /// Exponentially-weighted moving average of per-frame cursor attach latency.
    /// Only present in builds with the `stage-timing` feature.
    #[cfg(feature = "stage-timing")]
    pub cursor_latency_avg: Duration,
    /// Only present in builds with the `stage-timing` feature.
    #[cfg(feature = "stage-timing")]
    pub cursor_native_frames: u64,
    /// Only present in builds with the `stage-timing` feature.
    #[cfg(feature = "stage-timing")]
    pub cursor_fallback_frames: u64,
    /// Only present in builds with the `stage-timing` feature.
    #[cfg(feature = "stage-timing")]
    pub cursor_shape_cache_hits: u64,
    /// Only present in builds with the `stage-timing` feature.
    #[cfg(feature = "stage-timing")]
    pub cursor_shape_cache_misses: u64,
    pub target_fps: u32,
}

/// Handle to a running capture stream. Dropping the handle stops the
/// background capture thread.
pub struct CaptureStream {
    queue: Arc<StreamQueue<CaptureEvent>>,
    stop_flag: Arc<AtomicBool>,
    cancellation: CancellationToken,
    wake: mpsc::Sender<()>,
    pause_flag: Arc<AtomicBool>,
    stats: Arc<CaptureStreamStats>,
    join_handle: Option<std::thread::JoinHandle<()>>,
    buffer_depth: usize,
    target_fps: Arc<AtomicU32>,
    pacing_origin: Arc<Mutex<Option<Instant>>>,
    maximum_fps: u32,
    minimum_fps: u32,
}

impl CaptureStream {
    /// Recording-only mouse following. Native monitor sources are warmed on
    /// independent owner threads while the previous source continues delivering.
    #[cfg(windows)]
    pub fn spawn_follow_mouse(
        nominal_region: crate::CaptureRegion,
        preferred_backend: crate::backend::CaptureBackendKind,
        options: crate::CaptureOptions,
        config: CaptureStreamConfig,
    ) -> CaptureResult<Self> {
        Self::spawn_follow_mouse_with_motion(
            nominal_region,
            preferred_backend,
            options,
            config,
            FollowMouseOptions::default(),
        )
    }

    #[cfg(windows)]
    pub fn spawn_follow_mouse_with_motion(
        nominal_region: crate::CaptureRegion,
        preferred_backend: crate::backend::CaptureBackendKind,
        options: crate::CaptureOptions,
        config: CaptureStreamConfig,
        motion: FollowMouseOptions,
    ) -> CaptureResult<Self> {
        if config.target_fps == 0
            || nominal_region.width == 0
            || nominal_region.height == 0
            || options.workload != CaptureWorkload::Continuous
        {
            return Err(CaptureError::InvalidConfig(
                "following requires continuous capture, nonempty geometry and positive FPS".into(),
            ));
        }
        let buffer_depth = config.buffer_depth.max(1);
        let queue = Arc::new(StreamQueue::new(buffer_depth));
        let stop_flag = Arc::new(AtomicBool::new(false));
        let pause_flag = Arc::new(AtomicBool::new(false));
        let stats = Arc::new(CaptureStreamStats::default());
        let maximum_fps = config.target_fps;
        let minimum_fps = if config.adaptive_fps {
            config.min_fps
        } else {
            0
        };
        let target_fps = Arc::new(AtomicU32::new(maximum_fps));
        stats
            .target_fps
            .store(u64::from(maximum_fps), Ordering::Relaxed);
        let pacing_origin = Arc::new(Mutex::new(None));
        let cancellation = CancellationToken::default();
        let (wake, commands) = mpsc::bounded(1);
        let (ready_tx, ready_rx) = mpsc::bounded(1);
        let (recycle_tx, recycle_rx) = mpsc::bounded(buffer_depth.saturating_add(4));
        let recycler = FrameRecycleSender::new(recycle_tx);
        let worker_queue = queue.clone();
        let worker_stop = stop_flag.clone();
        let worker_pause = pause_flag.clone();
        let worker_stats = stats.clone();
        let worker_target = target_fps.clone();
        let worker_origin = pacing_origin.clone();
        let join_handle = std::thread::Builder::new()
            .name("snow-follow-capture".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                let result = crate::follow_mouse::run(
                    crate::follow_mouse::Settings {
                        nominal: nominal_region,
                        preferred: preferred_backend,
                        options,
                        config,
                        motion,
                        gpu: false,
                        recycled: Some(recycle_rx),
                    },
                    &worker_stop,
                    &worker_pause,
                    &worker_target,
                    &worker_origin,
                    &commands,
                    &worker_stats,
                    ready_tx.clone(),
                    |frame| {
                        if let crate::follow_mouse::FollowFrame::Cpu(frame) = frame {
                            let _ = publish_cpu_frame(
                                &worker_queue,
                                &worker_stats,
                                CapturedFrame::from_frame_with_recycler(frame, recycler.clone()),
                            );
                        }
                    },
                );
                if let Err(error) = result {
                    let _ = ready_tx.try_send(Err(error.clone()));
                    worker_queue.push(CaptureEvent::Error(error));
                }
                worker_queue.push(CaptureEvent::StreamEnded);
                worker_queue.close();
            })
            .map_err(CaptureError::platform)?;
        match ready_rx.recv() {
            Ok(Ok(_)) => Ok(Self {
                queue,
                stop_flag,
                cancellation,
                wake,
                pause_flag,
                stats,
                join_handle: Some(join_handle),
                buffer_depth,
                target_fps,
                pacing_origin,
                maximum_fps,
                minimum_fps,
            }),
            result => {
                stop_flag.store(true, Ordering::Release);
                let _ = wake.try_send(());
                let _ = join_handle.join();
                Err(match result {
                    Ok(Err(error)) => error,
                    _ => CaptureError::WorkerDead,
                })
            }
        }
    }

    fn request_stop_and_join(&mut self) {
        self.stop();
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
        let cancellation = CancellationToken::default();
        capture.set_stream_cancellation(cancellation.clone());
        capture.prepare_target()?;

        let buffer_depth = config.buffer_depth.max(1);
        let queue = Arc::new(StreamQueue::new(buffer_depth));
        let recycle_depth = buffer_depth.saturating_add(2);
        let initial_target_info = capture.target_info().ok();
        let (recycle_tx, recycle_rx) = mpsc::bounded::<Frame>(recycle_depth);
        let recycler = FrameRecycleSender::new(recycle_tx.clone());

        if let Some(target_info) = initial_target_info {
            let mut frame = Frame::empty();
            if frame
                .ensure_rgba_capacity(target_info.width, target_info.height)
                .is_ok()
            {
                let _ = recycle_tx.try_send(frame);
            }
        }

        let stop_flag = Arc::new(AtomicBool::new(false));
        let pause_flag = Arc::new(AtomicBool::new(false));
        let stats = Arc::new(CaptureStreamStats::default());
        let maximum_fps = config.target_fps;
        let minimum_fps = if config.adaptive_fps {
            config.min_fps
        } else {
            0
        };
        let target_fps = Arc::new(AtomicU32::new(config.target_fps));
        stats
            .target_fps
            .store(u64::from(config.target_fps), Ordering::Release);

        let worker_queue = Arc::clone(&queue);
        let stop = stop_flag.clone();
        let pause = pause_flag.clone();
        let stats_clone = stats.clone();
        let requested_target = Arc::clone(&target_fps);
        let pacing_origin = Arc::new(Mutex::new(None));
        let worker_origin = Arc::clone(&pacing_origin);
        let (wake, wake_rx) = mpsc::bounded(1);
        let worker_cancellation = cancellation.clone();

        let join_handle = std::thread::Builder::new()
            .name("snow-capture-stream".to_string())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                stream_loop(
                    &mut capture,
                    &config,
                    &worker_queue,
                    &recycle_rx,
                    &recycler,
                    &stop,
                    &pause,
                    &stats_clone,
                    &requested_target,
                    &worker_origin,
                    &worker_cancellation,
                    &wake_rx,
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
            cancellation,
            wake,
            pause_flag,
            stats,
            join_handle: Some(join_handle),
            buffer_depth,
            target_fps,
            pacing_origin,
            maximum_fps,
            minimum_fps,
        })
    }

    /// Align acquisition to an external clock while preserving the requested rate.
    /// Captures begin a quarter interval after each clock boundary, leaving time
    /// for readback before the following output boundary. None restores free pacing.
    pub fn set_pacing_origin(&self, origin: Option<Instant>) {
        *self
            .pacing_origin
            .lock()
            .unwrap_or_else(|error| error.into_inner()) = origin;
        let _ = self.wake.try_send(());
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

    /// Cancel native acquisition and wake pacing or a paused supervisor so the
    /// worker can release capture access before joining.
    pub fn stop(&self) {
        self.stop_flag.store(true, Ordering::Release);
        self.cancellation.cancel();
        // Follow supervisors wait on control changes while paused; they do not
        // observe the native-acquisition cancellation receiver.
        let _ = self.wake.try_send(());
    }

    /// Update the pacing target without restarting the capture session.
    /// The requested rate is clamped to the stream's configured ceiling and,
    /// when adaptive pacing is enabled, its configured floor. A value of zero
    /// requests uncapped capture when the stream was created uncapped.
    pub fn set_target_fps(&self, target_fps: u32) {
        let clamped = clamp_target_fps(target_fps, self.maximum_fps, self.minimum_fps);
        let previous = self.target_fps.swap(clamped, Ordering::AcqRel);
        self.stats
            .target_fps
            .store(u64::from(clamped), Ordering::Release);
        if previous != clamped {
            let _ = self.wake.try_send(());
        }
    }

    /// Return the current externally requested pacing target. Zero means
    /// uncapped capture.
    pub fn target_fps(&self) -> u32 {
        self.target_fps.load(Ordering::Acquire)
    }

    /// Pause the capture stream. The capture thread idles without
    /// releasing the underlying OS capture resources, so resume is
    /// near-instant.
    pub fn pause(&self) {
        self.pause_flag.store(true, Ordering::Release);
        let _ = self.wake.try_send(());
    }

    /// Resume a paused capture stream.
    pub fn resume(&self) {
        self.pause_flag.store(false, Ordering::Release);
        let _ = self.wake.try_send(());
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

#[derive(Default)]
struct StreamRetryState {
    failures: usize,
    last_display_refresh: Option<Arc<DisplayRefreshRetry>>,
}

impl StreamRetryState {
    /// Return whether this is a new failed attempt, including when control
    /// messages wake a stream before the display cache's retry deadline.
    fn record(&mut self, error: &CaptureError) -> bool {
        if let CaptureError::DisplayRefreshPending(retry) = error {
            if self
                .last_display_refresh
                .as_ref()
                .is_some_and(|last| Arc::ptr_eq(last, retry))
            {
                return false;
            }
            self.last_display_refresh = Some(Arc::clone(retry));
        }
        self.failures = self.failures.saturating_add(1);
        true
    }

    fn delay(error: &CaptureError, now: Instant) -> Duration {
        match error {
            CaptureError::DisplayRefreshPending(retry) => {
                retry.retry_at().saturating_duration_since(now)
            }
            _ => Duration::from_millis(16),
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
    requested_target: &AtomicU32,
    pacing_origin: &Mutex<Option<Instant>>,
    cancellation: &CancellationToken,
    wake: &mpsc::Receiver<()>,
) {
    let min_interval = if config.adaptive_fps && config.min_fps > 0 {
        Some(Duration::from_secs_f64(1.0 / config.min_fps as f64))
    } else {
        None
    };

    let mut reuse_frame: Option<Frame> = None;
    let mut cadence: Option<AlignedCaptureCadence> = None;
    let mut retry_state = StreamRetryState::default();
    let mut last_width: u32 = 0;
    let mut last_height: u32 = 0;

    let mut pressure_interval = target_interval(config.target_fps);
    const ADAPTIVE_ALPHA: f64 = 0.15;
    const DROP_RATIO_THRESHOLD: f64 = 0.10;
    const ADAPTIVE_WINDOW: u32 = 30;
    let mut window_drops: u32 = 0;
    let mut window_total: u32 = 0;

    let mut latency_avg_ns: f64 = 0.0;
    const LATENCY_ALPHA: f64 = 0.1;
    #[cfg(feature = "stage-timing")]
    let mut cursor_latency_avg_ns: f64 = 0.0;

    let mut fps_counter: u64 = 0;
    let mut content_generation = 0u64;
    let mut fps_epoch = Instant::now();

    let mut was_paused = false;
    let mut source_fps = None;
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
            wait_for_control(Duration::from_millis(50), cancellation, wake);
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

        let origin = *pacing_origin
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let fps = requested_target.load(Ordering::Acquire);
        let effective_fps = effective_target_fps(fps, pressure_interval);
        if source_fps != Some(effective_fps) {
            if let Err(error) = capture.set_stream_target_fps(effective_fps) {
                if !stop.load(Ordering::Acquire) {
                    store_queue_fill(stats, queue.push(CaptureEvent::Error(error)).data_len);
                }
                break;
            }
            source_fps = Some(effective_fps);
        }
        let aligned = origin.is_some() && fps > 0 && !config.adaptive_fps;
        if let Some(origin) = origin.filter(|_| aligned) {
            if cadence
                .as_ref()
                .is_none_or(|cadence| cadence.origin != origin || cadence.fps != fps)
            {
                cadence = Some(AlignedCaptureCadence {
                    origin,
                    fps,
                    next: 0,
                });
            }
            if cadence
                .as_mut()
                .expect("capture cadence")
                .wait_interruptibly(Instant::now(), |delay| {
                    wait_for_control(delay, cancellation, wake)
                })
            {
                continue;
            }
            if stop.load(Ordering::Acquire) || pause.load(Ordering::Acquire) {
                continue;
            }
        } else {
            cadence = None;
        }
        let frame_start = Instant::now();

        drain_recycled_frames(recycle_rx, &mut reuse_frame);

        let reuse = reuse_frame.take();
        let capture_result = if config.include_cursor {
            capture.capture_with_cursor(reuse)
        } else {
            capture.capture_frame(reuse).map(|frame| (frame, None))
        };

        let capture_elapsed = frame_start.elapsed();
        if stop.load(Ordering::Acquire) && capture_result.is_err() {
            break;
        }

        match capture_result {
            Ok((mut frame, cursor_outcome)) => {
                retry_state = StreamRetryState::default();

                #[cfg(feature = "stage-timing")]
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
                #[cfg(not(feature = "stage-timing"))]
                let _ = cursor_outcome;

                #[cfg(feature = "stage-timing")]
                {
                    frame.metadata.capture_duration = Some(capture_elapsed);
                }

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
                stamp_content_generation(&mut frame, &mut content_generation);

                frame.metadata.queued_at = Some(Instant::now());
                frame.metadata.observation_started_at = Some(frame_start);

                let dropped = publish_cpu_frame(
                    queue,
                    stats,
                    CapturedFrame::from_frame_with_recycler(frame, recycler.clone()),
                );
                if let Some(dropped) = dropped {
                    if let Ok(frame) = dropped.into_owned() {
                        reuse_frame = Some(frame);
                    }
                    window_drops += 1;
                }
                window_total += 1;
                if config.adaptive_fps && window_total >= ADAPTIVE_WINDOW {
                    let drop_ratio = window_drops as f64 / window_total as f64;
                    if let (Some(cur), Some(max)) = (pressure_interval, min_interval) {
                        let requested = target_interval(requested_target.load(Ordering::Acquire));
                        let effective = requested.map_or(cur, |requested| cur.max(requested));
                        let base = requested.unwrap_or(cur);
                        let effective_ns = effective.as_nanos() as f64;
                        let target_ns = if drop_ratio > DROP_RATIO_THRESHOLD {
                            (effective_ns * 1.5).min(max.as_nanos() as f64)
                        } else {
                            (effective_ns * 0.8).max(base.as_nanos() as f64)
                        };
                        let smoothed =
                            ADAPTIVE_ALPHA * target_ns + (1.0 - ADAPTIVE_ALPHA) * effective_ns;
                        pressure_interval = Some(Duration::from_nanos(smoothed as u64));
                    }
                    window_drops = 0;
                    window_total = 0;
                }
            }
            Err(ref e) if e.is_retryable() => {
                if retry_state.record(e) {
                    stats.errors_recovered.fetch_add(1, Ordering::Relaxed);
                }
                if retry_state.failures >= config.max_consecutive_errors {
                    store_queue_fill(stats, queue.push(CaptureEvent::Error(e.clone())).data_len);
                    break;
                }
                wait_for_control(
                    StreamRetryState::delay(e, Instant::now()),
                    cancellation,
                    wake,
                );
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

        let requested = target_interval(requested_target.load(Ordering::Acquire));
        let interval = match (requested, pressure_interval) {
            (Some(requested), Some(pressure)) => Some(requested.max(pressure)),
            (Some(requested), None) => Some(requested),
            (None, Some(pressure)) => Some(pressure),
            (None, None) => None,
        };
        if let Some(interval) = interval.filter(|_| !aligned) {
            let elapsed = frame_start.elapsed();
            if elapsed < interval {
                wait_for_control(interval - elapsed, cancellation, wake);
            }
        }
    }

    // Send StreamEnded sentinel so the consumer knows no more events
    // will arrive and can flush its encoder.
    store_queue_fill(stats, queue.push(CaptureEvent::StreamEnded).data_len);
}

pub(crate) struct AlignedCaptureCadence {
    pub(crate) origin: Instant,
    pub(crate) fps: u32,
    pub(crate) next: u64,
}
impl AlignedCaptureCadence {
    pub(crate) fn wait_interruptibly(
        &mut self,
        now: Instant,
        wait: impl FnOnce(Duration) -> bool,
    ) -> bool {
        let previous = self.next;
        if wait(self.wait(now)) {
            // A control wake is not an acquisition. Preserve the pending slot
            // when its clock and rate remain unchanged on the next iteration.
            self.next = previous;
            true
        } else {
            false
        }
    }

    fn offset(index: u64, fps: u32) -> Duration {
        Duration::from_nanos(
            ((u128::from(index) * 4 + 1) * 1_000_000_000)
                .div_ceil(u128::from(fps) * 4)
                .min(u128::from(u64::MAX)) as u64,
        )
    }
    pub(crate) fn wait(&mut self, now: Instant) -> Duration {
        let elapsed = now.saturating_duration_since(self.origin);
        let current = (elapsed.as_nanos() * u128::from(self.fps) * 4).saturating_sub(1_000_000_000)
            / (4 * 1_000_000_000u128);
        let index = self.next.max(current.min(u128::from(u64::MAX)) as u64);
        self.next = index.saturating_add(1);
        Self::offset(index, self.fps).saturating_sub(elapsed)
    }
}

fn target_interval(target_fps: u32) -> Option<Duration> {
    (target_fps > 0).then(|| Duration::from_secs_f64(1.0 / target_fps as f64))
}

fn effective_target_fps(target_fps: u32, pressure: Option<Duration>) -> u32 {
    match (target_interval(target_fps), pressure) {
        (Some(requested), Some(pressure)) if pressure > requested => {
            (1_000_000_000u128 / pressure.as_nanos()).max(1) as u32
        }
        _ => target_fps,
    }
}

fn clamp_target_fps(target_fps: u32, maximum_fps: u32, minimum_fps: u32) -> u32 {
    if target_fps == 0 {
        return if maximum_fps == 0 { 0 } else { maximum_fps };
    }
    let upper = if maximum_fps == 0 {
        u32::MAX
    } else {
        maximum_fps
    };
    target_fps.clamp(minimum_fps.min(upper), upper)
}

fn drain_recycled_frames(recycle_rx: &mpsc::Receiver<Frame>, reuse_frame: &mut Option<Frame>) {
    while let Ok(candidate) = recycle_rx.try_recv() {
        let keep_candidate = match reuse_frame.as_ref() {
            Some(current) => recycled_frame_rank(&candidate) >= recycled_frame_rank(current),
            None => true,
        };
        if keep_candidate {
            *reuse_frame = Some(candidate);
        }
    }
}

fn recycled_frame_rank(frame: &Frame) -> (u64, usize) {
    (frame.metadata.sequence, frame.as_rgba_bytes().len())
}

fn store_queue_fill(stats: &CaptureStreamStats, len: usize) {
    stats.buffer_fill.store(len as u64, Ordering::Release);
}

/// Keep CPU drop statistics and reliable notifications in agreement for fixed
/// and following streams. Return the displaced lease for optional local reuse.
fn publish_cpu_frame(
    queue: &StreamQueue<CaptureEvent>,
    stats: &CaptureStreamStats,
    frame: CapturedFrame,
) -> Option<CapturedFrame> {
    let outcome = queue.push(CaptureEvent::Frame(frame));
    store_queue_fill(stats, outcome.data_len);
    if let Some(CaptureEvent::Frame(dropped)) = outcome.dropped {
        stats.frames_dropped.fetch_add(1, Ordering::Relaxed);
        store_queue_fill(
            stats,
            queue
                .push(CaptureEvent::FramesDropped { count: 1 })
                .data_len,
        );
        Some(dropped)
    } else {
        None
    }
}

/// Pacing never holds stop or a rate change behind the previous frame interval.
fn wait_for_control(
    duration: Duration,
    cancellation: &CancellationToken,
    wake: &mpsc::Receiver<()>,
) -> bool {
    #[cfg(windows)]
    let sleep_duration = duration.saturating_sub(Duration::from_micros(1500));
    #[cfg(not(windows))]
    let sleep_duration = duration;
    #[cfg(windows)]
    let target = Instant::now() + duration;
    mpsc::select_biased! {
        recv(cancellation.receiver()) -> _ => return true,
        recv(wake) -> _ => return true,
        default(sleep_duration) => {},
    }
    // Windows needs precise timer boundaries for recording. macOS scheduling
    // already supports the required granularity without a busy spin.
    #[cfg(windows)]
    while Instant::now() < target {
        if cancellation.is_canceled() || wake.try_recv().is_ok() {
            return true;
        }
        std::hint::spin_loop();
    }
    false
}

fn stamp_content_generation(frame: &mut Frame, generation: &mut u64) {
    if *generation == 0 || !frame.metadata.is_duplicate {
        *generation = generation.saturating_add(1);
    }
    frame.metadata.content_generation = Some(*generation);
}

#[cfg(test)]
mod tests {
    use super::{
        CaptureStream, CaptureStreamConfig, CaptureStreamStats, clamp_target_fps, target_interval,
    };
    use crossbeam_channel as mpsc;
    use snow_core::cancellation::CancellationToken;
    use snow_core::stream_queue::StreamQueue;
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
    use std::time::Duration;

    #[test]
    fn follow_mouse_cpu_queue_reports_every_displaced_frame() {
        use crate::frame::{CaptureEvent, CapturedFrame, Frame};
        let queue = StreamQueue::new(1);
        let stats = CaptureStreamStats::default();
        for value in 0..3u8 {
            let frame = CapturedFrame::from(Frame::from_rgba8(1, 1, vec![value; 4]).unwrap());
            let dropped = super::publish_cpu_frame(&queue, &stats, frame);
            assert_eq!(dropped.is_some(), value != 0);
            if let Some(dropped) = dropped {
                assert_eq!(dropped.as_bytes(), &[value - 1; 4]);
            }
        }
        assert_eq!(stats.snapshot().frames_dropped, 2);
        assert_eq!(stats.snapshot().buffer_fill, 1);
        for _ in 0..2 {
            assert!(matches!(
                queue.try_recv().unwrap().0,
                CaptureEvent::FramesDropped { count: 1 }
            ));
        }
        let CaptureEvent::Frame(latest) = queue.try_recv().unwrap().0 else {
            panic!("latest frame")
        };
        assert_eq!(latest.as_bytes(), &[2; 4]);
        assert!(queue.try_recv().is_err());
    }

    #[test]
    fn retry_budget_counts_distinct_display_attempts_even_with_identical_deadlines() {
        use crate::error::{CaptureError, DisplayRefreshRetry};
        use std::time::Instant;

        let now = Instant::now();
        let retry_at = now + Duration::from_secs(1);
        let error = CaptureError::DisplayRefreshPending(Arc::new(DisplayRefreshRetry::new(
            CaptureError::AccessLost,
            retry_at,
        )));
        let mut state = super::StreamRetryState::default();
        assert!(state.record(&error));
        for _ in 0..100 {
            assert!(!state.record(&error.clone()));
        }
        assert_eq!(state.failures, 1);
        assert_eq!(
            super::StreamRetryState::delay(&error, now),
            Duration::from_secs(1)
        );
        assert_eq!(
            super::StreamRetryState::delay(&error, retry_at),
            Duration::ZERO
        );
        assert_eq!(
            super::StreamRetryState::delay(&error, retry_at + Duration::from_secs(1)),
            Duration::ZERO
        );

        let next = CaptureError::DisplayRefreshPending(Arc::new(DisplayRefreshRetry::new(
            CaptureError::AccessLost,
            retry_at,
        )));
        assert!(state.record(&next));
        assert_eq!(state.failures, 2);
        // Ordinary capture failures keep their original count and wait behavior.
        assert!(state.record(&CaptureError::Timeout));
        assert!(state.record(&CaptureError::Timeout));
        assert_eq!(state.failures, 4);
        assert_eq!(
            super::StreamRetryState::delay(&CaptureError::Timeout, now),
            Duration::from_millis(16)
        );
        state = super::StreamRetryState::default();
        assert!(state.record(&next));
        assert_eq!(state.failures, 1);
    }

    #[test]
    fn target_fps_is_clamped_to_configured_ceiling_and_floor() {
        assert_eq!(clamp_target_fps(1, 30, 10), 10);
        assert_eq!(clamp_target_fps(24, 30, 10), 24);
        assert_eq!(clamp_target_fps(120, 30, 10), 30);
    }

    #[test]
    fn uncapped_target_remains_uncapped_only_for_uncapped_streams() {
        assert_eq!(clamp_target_fps(0, 0, 15), 0);
        assert_eq!(clamp_target_fps(0, 30, 15), 30);
    }

    #[test]
    fn invalid_floor_does_not_panic_when_ceiling_is_lower() {
        assert_eq!(clamp_target_fps(1, 30, 60), 30);
        assert_eq!(clamp_target_fps(u32::MAX, 30, 60), 30);
    }

    #[test]
    fn target_interval_handles_capped_and_uncapped_rates() {
        assert_eq!(target_interval(0), None);
        let interval = target_interval(30).expect("30 fps has an interval");
        assert!(interval >= Duration::from_millis(33));
        assert!(interval <= Duration::from_millis(34));
    }

    #[test]
    fn default_stream_config_keeps_cursor_enabled_for_recording() {
        assert!(CaptureStreamConfig::default().include_cursor);
    }

    #[test]
    fn stats_expose_runtime_target_fps() {
        let stats = CaptureStreamStats::default();
        stats.target_fps.store(24, Ordering::Release);
        assert_eq!(stats.snapshot().target_fps, 24);
    }

    #[test]
    fn native_rate_follows_requested_and_adaptive_pacing() {
        assert_eq!(super::effective_target_fps(30, target_interval(30)), 30);
        assert_eq!(super::effective_target_fps(12, target_interval(30)), 12);
        assert_eq!(super::effective_target_fps(30, target_interval(10)), 10);
        assert_eq!(super::effective_target_fps(30, target_interval(1)), 1);
        assert_eq!(super::effective_target_fps(0, None), 0);
    }

    #[test]
    fn stop_and_control_changes_interrupt_pacing() {
        let token = CancellationToken::default();
        let (wake, receiver) = mpsc::bounded(1);
        wake.send(()).unwrap();
        assert!(super::wait_for_control(
            Duration::from_secs(60),
            &token,
            &receiver
        ));
        token.cancel();
        assert!(super::wait_for_control(
            Duration::from_secs(60),
            &token,
            &receiver
        ));
    }

    struct NativeStreamProbe {
        observations: mpsc::Sender<u32>,
        changes: mpsc::Sender<u32>,
        permits: mpsc::Receiver<()>,
        cancellation: Option<CancellationToken>,
        fps: u32,
    }

    impl crate::backend::MonitorCapturer for NativeStreamProbe {
        fn set_cancellation(&mut self, token: CancellationToken) {
            self.cancellation = Some(token);
        }

        fn set_target_fps(&mut self, fps: u32) -> crate::error::CaptureResult<()> {
            self.fps = fps;
            self.changes.send(fps).unwrap();
            Ok(())
        }

        fn capture(
            &mut self,
            _reuse: Option<crate::frame::Frame>,
        ) -> crate::error::CaptureResult<crate::frame::Frame> {
            self.observations.send(self.fps).unwrap();
            let token = self
                .cancellation
                .as_ref()
                .expect("stream cancellation is assigned");
            mpsc::select_biased! {
                recv(token.receiver()) -> _ => Err(crate::error::CaptureError::Canceled),
                recv(self.permits) -> permit => {
                    permit.unwrap();
                    crate::frame::Frame::from_rgba8(2, 2, vec![0; 16])
                },
            }
        }
    }

    struct NativeStreamBackend {
        observations: mpsc::Sender<u32>,
        changes: mpsc::Sender<u32>,
        permits: mpsc::Receiver<()>,
    }

    impl crate::backend::CaptureBackend for NativeStreamBackend {
        fn create_target_capturer(
            &self,
            _target: &crate::CaptureTarget,
            _options: crate::CaptureOptions,
        ) -> crate::error::CaptureResult<Option<Box<dyn crate::backend::MonitorCapturer>>> {
            Ok(Some(Box::new(NativeStreamProbe {
                observations: self.observations.clone(),
                changes: self.changes.clone(),
                permits: self.permits.clone(),
                cancellation: None,
                fps: 0,
            })))
        }

        fn enumerate_monitors(&self) -> crate::error::CaptureResult<Vec<crate::MonitorId>> {
            Ok(Vec::new())
        }

        fn primary_monitor(&self) -> crate::error::CaptureResult<crate::MonitorId> {
            Err(crate::error::CaptureError::NoPrimaryMonitor)
        }

        fn monitor_layout(&self) -> crate::error::CaptureResult<crate::MonitorLayout> {
            Err(crate::error::CaptureError::NoPrimaryMonitor)
        }

        fn create_monitor_capturer(
            &self,
            _monitor: &crate::MonitorId,
        ) -> crate::error::CaptureResult<Box<dyn crate::backend::MonitorCapturer>> {
            Err(crate::error::CaptureError::NoPrimaryMonitor)
        }

        fn inspect_window(
            &self,
            _window: &crate::WindowId,
        ) -> crate::error::CaptureResult<crate::CaptureTargetInfo> {
            Ok(crate::CaptureTargetInfo {
                origin_x: 0,
                origin_y: 0,
                width: 2,
                height: 2,
            })
        }
    }

    #[test]
    fn stream_updates_native_rate_and_cancels_blocked_acquisition() {
        let (observed, observations) = mpsc::unbounded();
        let (changed, changes) = mpsc::unbounded();
        let (permit, permits) = mpsc::bounded(1);
        let session = crate::CaptureSession::builder()
            .target(crate::CaptureTarget::Window(
                crate::WindowId::from_macos_id(1),
            ))
            .with_backend(Arc::new(NativeStreamBackend {
                observations: observed,
                changes: changed,
                permits,
            }))
            .with_options(crate::CaptureOptions {
                workload: crate::CaptureWorkload::Continuous,
                ..Default::default()
            })
            .build()
            .unwrap();
        let stream = CaptureStream::spawn(
            session,
            CaptureStreamConfig {
                target_fps: 30,
                include_cursor: false,
                adaptive_fps: false,
                ..Default::default()
            },
        )
        .unwrap();
        let deadline = Duration::from_secs(5);
        assert_eq!(observations.recv_timeout(deadline).unwrap(), 30);
        assert_eq!(changes.recv_timeout(deadline).unwrap(), 30);
        permit.send(()).unwrap();
        assert_eq!(observations.recv_timeout(deadline).unwrap(), 30);
        assert!(
            changes.try_recv().is_err(),
            "unchanged native rate is not reapplied"
        );
        stream.set_target_fps(12);
        permit.send(()).unwrap();
        assert_eq!(observations.recv_timeout(deadline).unwrap(), 12);
        assert_eq!(changes.recv_timeout(deadline).unwrap(), 12);
        // The third native acquisition has no permit and can finish only when
        // stop broadcasts cancellation, independently of the consumer queue.
        let (done, finished) = mpsc::bounded(1);
        std::thread::spawn(move || {
            done.send(stream.stop_and_drain()).unwrap();
        });
        let events = finished.recv_timeout(deadline).unwrap();
        assert!(
            events
                .iter()
                .any(|event| matches!(event, crate::frame::CaptureEvent::StreamEnded))
        );
        assert!(
            !events
                .iter()
                .any(|event| matches!(event, crate::frame::CaptureEvent::Error(_)))
        );
    }

    fn test_stream(maximum_fps: u32, minimum_fps: u32) -> CaptureStream {
        CaptureStream {
            queue: Arc::new(StreamQueue::new(1)),
            stop_flag: Arc::new(AtomicBool::new(false)),
            cancellation: CancellationToken::default(),
            wake: mpsc::bounded(1).0,
            pause_flag: Arc::new(AtomicBool::new(false)),
            stats: Arc::new(CaptureStreamStats::default()),
            join_handle: None,
            buffer_depth: 1,
            target_fps: Arc::new(AtomicU32::new(maximum_fps)),
            pacing_origin: Arc::new(std::sync::Mutex::new(None)),
            maximum_fps,
            minimum_fps,
        }
    }

    #[test]
    fn follow_mouse_paused_cpu_stream_stop_drain_and_drop_wake_supervisor() {
        use crate::frame::CaptureEvent;

        #[derive(Clone, Copy, Debug)]
        enum Shutdown {
            Stop,
            Drain,
            Drop,
        }

        let deadline = Duration::from_secs(5);
        for shutdown in [Shutdown::Stop, Shutdown::Drain, Shutdown::Drop] {
            let mut stream = test_stream(30, 1);
            let (wake, commands) = mpsc::bounded(1);
            stream.wake = wake.clone();
            stream.pause();
            let stop = stream.stop_flag.clone();
            let paused = stream.pause_flag.clone();
            let queue = stream.queue.clone();
            let (entered, ready) = mpsc::bounded(0);
            stream.join_handle = Some(std::thread::spawn(move || {
                // Consume the pause notification before parking, as the follow
                // supervisor does. Cancellation alone cannot wake this receiver.
                commands.recv().unwrap();
                assert!(paused.load(Ordering::Acquire));
                entered.send(()).unwrap();
                commands.recv().unwrap();
                assert!(stop.load(Ordering::Acquire));
                queue.push(CaptureEvent::StreamEnded);
                queue.close();
            }));
            ready.recv_timeout(deadline).unwrap();
            let (completed, finished) = mpsc::bounded(1);
            let teardown = std::thread::spawn(move || {
                let events = match shutdown {
                    Shutdown::Stop => {
                        stream.stop();
                        let event = stream.recv_timeout(deadline);
                        drop(stream);
                        event.into_iter().collect()
                    }
                    Shutdown::Drain => stream.stop_and_drain(),
                    Shutdown::Drop => {
                        drop(stream);
                        Vec::new()
                    }
                };
                completed.send(events).unwrap();
            });
            let result = finished.recv_timeout(deadline);
            // A regression must fail with a bounded timeout instead of leaving
            // the test process hung in Drop while joining the parked worker.
            if result.is_err() {
                let _ = wake.try_send(());
            }
            teardown.join().unwrap();
            let events = result.unwrap_or_else(|error| {
                panic!("{shutdown:?} must wake the paused follow supervisor: {error}")
            });
            if !matches!(shutdown, Shutdown::Drop) {
                assert!(matches!(events.as_slice(), [CaptureEvent::StreamEnded]));
            }
        }
    }

    #[test]
    fn stop_broadcasts_cancellation_and_coalesces_control_wakes() {
        let mut stream = test_stream(30, 1);
        let (wake, commands) = mpsc::bounded(1);
        stream.wake = wake;
        stream.pause();
        // A pending pause notification must not block stop or hide its flag.
        stream.stop();
        stream.stop();
        assert!(stream.stop_flag.load(Ordering::Acquire));
        assert!(stream.cancellation.is_canceled());
        assert!(matches!(
            stream.cancellation.receiver().try_recv(),
            Err(mpsc::TryRecvError::Disconnected)
        ));
        assert!(commands.try_recv().is_ok());
        assert!(commands.try_recv().is_err());
        // Repeating stop after the previous wake was consumed remains safe.
        stream.stop();
        assert!(commands.try_recv().is_ok());
    }

    #[test]
    fn runtime_target_changes_do_not_require_stream_restart() {
        let stream = test_stream(30, 1);
        stream.set_target_fps(12);
        assert_eq!(stream.target_fps(), 12);
        assert_eq!(stream.stats().snapshot().target_fps, 12);
        stream.set_target_fps(120);
        assert_eq!(stream.target_fps(), 30);
        stream.set_target_fps(0);
        assert_eq!(stream.target_fps(), 30);
    }

    #[test]
    fn runtime_target_respects_adaptive_floor_for_uncapped_streams() {
        let stream = test_stream(0, 15);
        stream.set_target_fps(1);
        assert_eq!(stream.target_fps(), 15);
        stream.set_target_fps(0);
        assert_eq!(stream.target_fps(), 0);
    }
    #[test]
    fn image_generation_survives_superseded_changes_followed_by_duplicates() {
        let mut frame = crate::frame::Frame::from_rgba8(2, 2, vec![0; 16]).unwrap();
        let mut generation = 0;
        super::stamp_content_generation(&mut frame, &mut generation);
        let displayed = frame.metadata.content_generation();
        frame.as_mut_bytes().fill(42);
        super::stamp_content_generation(&mut frame, &mut generation);
        // The changed capture is dropped; only its duplicate reaches the consumer.
        frame.metadata.is_duplicate = true;
        super::stamp_content_generation(&mut frame, &mut generation);
        assert_eq!(frame.metadata.content_generation(), Some(2));
        assert_ne!(frame.metadata.content_generation(), displayed);
        super::stamp_content_generation(&mut frame, &mut generation);
        assert_eq!(frame.metadata.content_generation(), Some(2));
    }

    #[test]
    fn aligned_capture_deadlines_do_not_drift_or_replay_missed_slots() {
        use super::AlignedCaptureCadence;
        let start = std::time::Instant::now();
        for fps in [10, 30, 60, 144] {
            assert_eq!(
                AlignedCaptureCadence::offset(u64::from(fps) * 3600, fps)
                    - AlignedCaptureCadence::offset(0, fps),
                Duration::from_secs(3600)
            );
        }
        let mut cadence = AlignedCaptureCadence {
            origin: start,
            fps: 30,
            next: 0,
        };
        assert_eq!(cadence.wait(start), Duration::from_nanos(8_333_334));
        assert_eq!(
            cadence.wait(start + Duration::from_millis(110)),
            Duration::ZERO
        );
        assert!(cadence.wait(start + Duration::from_millis(110)) > Duration::from_millis(20));
    }

    #[test]
    fn control_wakes_do_not_consume_aligned_capture_slots() {
        let start = std::time::Instant::now();
        let mut cadence = super::AlignedCaptureCadence {
            origin: start,
            fps: 30,
            next: 0,
        };
        for elapsed in [Duration::ZERO, Duration::from_millis(4)] {
            assert!(cadence.wait_interruptibly(start + elapsed, |delay| {
                assert_eq!(delay + elapsed, Duration::from_nanos(8_333_334));
                true
            }));
            assert_eq!(cadence.next, 0);
        }
        assert!(
            !cadence.wait_interruptibly(start + Duration::from_millis(4), |delay| {
                assert_eq!(delay, Duration::from_nanos(4_333_334));
                false
            })
        );
        assert_eq!(cadence.next, 1);
        assert_eq!(
            cadence.wait(start + Duration::from_millis(9)),
            Duration::from_nanos(32_666_667)
        );
    }
}

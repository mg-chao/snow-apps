use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use snow_core::error::{RecvTimeoutError, TryRecvError};
use snow_core::event::{DeliveryLane, StreamEvent};
use snow_core::stream_queue::StreamQueue;
use snow_d3d11::SharedDevice;

use super::{GpuCaptureSession, GpuCapturedFrame};
use crate::backend::CaptureBackendKind;
use crate::error::{CaptureError, CaptureResult};
use crate::streaming::AlignedCaptureCadence;
use crate::{CaptureRegion, CaptureStreamConfig, CaptureStreamStats};

pub enum GpuCaptureEvent {
    Frame(GpuCapturedFrame),
    Error(CaptureError),
    Paused,
    Resumed,
    StreamEnded,
}

impl StreamEvent for GpuCaptureEvent {
    fn delivery_lane(&self) -> DeliveryLane {
        if matches!(self, Self::Frame(_)) {
            DeliveryLane::Data
        } else {
            DeliveryLane::Control
        }
    }
    fn is_paused(&self) -> bool {
        matches!(self, Self::Paused)
    }
    fn is_resumed(&self) -> bool {
        matches!(self, Self::Resumed)
    }
    fn is_stream_ended(&self) -> bool {
        matches!(self, Self::StreamEnded)
    }
    fn is_error(&self) -> bool {
        matches!(self, Self::Error(_))
    }
    fn timestamp(&self) -> Option<&snow_core::timestamp::StreamTimestamp> {
        if let Self::Frame(frame) = self {
            frame.metadata().stream_timestamp()
        } else {
            None
        }
    }
}

pub struct GpuCaptureStream {
    queue: Arc<StreamQueue<GpuCaptureEvent>>,
    stopped: Arc<AtomicBool>,
    paused: Arc<AtomicBool>,
    origin: Arc<Mutex<Option<Instant>>>,
    wake: crossbeam_channel::Sender<()>,
    worker: Option<JoinHandle<()>>,
    device: SharedDevice,
    stats: Arc<CaptureStreamStats>,
}

impl GpuCaptureStream {
    pub fn spawn(
        region: CaptureRegion,
        backend: CaptureBackendKind,
        config: CaptureStreamConfig,
    ) -> CaptureResult<Self> {
        if config.target_fps == 0 {
            return Err(CaptureError::InvalidConfig(
                "GPU recording requires a positive capture FPS".into(),
            ));
        }
        let queue = Arc::new(StreamQueue::new(config.buffer_depth));
        let stopped = Arc::new(AtomicBool::new(false));
        let paused = Arc::new(AtomicBool::new(false));
        let origin = Arc::new(Mutex::new(None));
        let stats = Arc::new(CaptureStreamStats::default());
        stats
            .target_fps
            .store(u64::from(config.target_fps), Ordering::Relaxed);
        let (wake, commands) = crossbeam_channel::bounded(1);
        let (ready_tx, ready_rx) = crossbeam_channel::bounded(1);
        let worker_queue = Arc::clone(&queue);
        let worker_stopped = Arc::clone(&stopped);
        let worker_paused = Arc::clone(&paused);
        let worker_origin = Arc::clone(&origin);
        let worker_stats = Arc::clone(&stats);
        let worker = std::thread::Builder::new()
            .name("snow-gpu-capture".into())
            .spawn(move || {
                let mut session = match GpuCaptureSession::open(region, backend) {
                    Ok(session) => session,
                    Err(error) => {
                        let _ = ready_tx.send(Err(error));
                        worker_queue.close();
                        return;
                    }
                };
                if ready_tx.send(Ok(session.device().clone())).is_err() {
                    return;
                }
                let mut cadence = AlignedCaptureCadence {
                    origin: Instant::now(),
                    fps: config.target_fps,
                    next: 0,
                };
                let mut previous_origin = None;
                let mut was_paused = false;
                let mut failures = 0;
                let mut epoch = Instant::now();
                let mut frames = 0u64;
                while !worker_stopped.load(Ordering::Acquire) {
                    let paused = worker_paused.load(Ordering::Acquire);
                    if paused != was_paused {
                        worker_queue.push(if paused {
                            GpuCaptureEvent::Paused
                        } else {
                            GpuCaptureEvent::Resumed
                        });
                        was_paused = paused;
                    }
                    if paused {
                        let _ = commands.recv_timeout(Duration::from_millis(10));
                        continue;
                    }
                    let requested_origin = *worker_origin
                        .lock()
                        .unwrap_or_else(|error| error.into_inner());
                    if requested_origin != previous_origin {
                        cadence.origin = requested_origin.unwrap_or_else(Instant::now);
                        cadence.next = 0;
                        previous_origin = requested_origin;
                    }
                    let wait = cadence.wait(Instant::now());
                    if !wait.is_zero() {
                        let _ = commands.recv_timeout(wait);
                    }
                    if worker_stopped.load(Ordering::Acquire)
                        || worker_paused.load(Ordering::Acquire)
                    {
                        continue;
                    }
                    let started = Instant::now();
                    let pressure = session.pool_pressure();
                    match session.capture() {
                        Ok(mut frame) => {
                            if !config.include_cursor {
                                Arc::make_mut(&mut frame.metadata).cursor = None;
                            }
                            if worker_paused.load(Ordering::Acquire) {
                                continue;
                            }
                            failures = 0;
                            let outcome = worker_queue.push(GpuCaptureEvent::Frame(frame));
                            if outcome.dropped.is_some() {
                                worker_stats.frames_dropped.fetch_add(1, Ordering::Relaxed);
                            }
                            worker_stats
                                .buffer_fill
                                .store(outcome.data_len as u64, Ordering::Relaxed);
                            worker_stats.frames_captured.fetch_add(1, Ordering::Relaxed);
                            worker_stats.capture_latency_avg_ns.store(
                                (started.elapsed().as_nanos() as f64).to_bits(),
                                Ordering::Relaxed,
                            );
                            frames += 1;
                        }
                        Err(CaptureError::Timeout) => {
                            worker_stats.frames_dropped.fetch_add(
                                session.pool_pressure().saturating_sub(pressure),
                                Ordering::Relaxed,
                            );
                        }
                        Err(CaptureError::AccessLost)
                            if failures < config.max_consecutive_errors =>
                        {
                            failures += 1;
                            worker_stats
                                .errors_recovered
                                .fetch_add(1, Ordering::Relaxed);
                        }
                        Err(error) => {
                            worker_queue.push(GpuCaptureEvent::Error(error));
                            break;
                        }
                    }
                    if epoch.elapsed() >= Duration::from_secs(1) {
                        worker_stats.current_fps.store(
                            (frames as f64 / epoch.elapsed().as_secs_f64()).to_bits(),
                            Ordering::Relaxed,
                        );
                        epoch = Instant::now();
                        frames = 0;
                    }
                }
                worker_queue.push(GpuCaptureEvent::StreamEnded);
                worker_queue.close();
            })
            .map_err(CaptureError::platform)?;
        match ready_rx.recv() {
            Ok(Ok(device)) => Ok(Self {
                queue,
                stopped,
                paused,
                origin,
                wake,
                worker: Some(worker),
                device,
                stats,
            }),
            result => {
                stopped.store(true, Ordering::Release);
                let _ = wake.try_send(());
                let _ = worker.join();
                Err(match result {
                    Ok(Err(error)) => error,
                    Err(error) => CaptureError::platform(error),
                    Ok(Ok(_)) => unreachable!(),
                })
            }
        }
    }

    pub fn device(&self) -> &SharedDevice {
        &self.device
    }
    pub fn stats(&self) -> &Arc<CaptureStreamStats> {
        &self.stats
    }
    pub fn set_pacing_origin(&self, origin: Option<Instant>) {
        *self
            .origin
            .lock()
            .unwrap_or_else(|error| error.into_inner()) = origin;
        let _ = self.wake.try_send(());
    }
    pub fn pause(&self) {
        self.paused.store(true, Ordering::Release);
        let _ = self.wake.try_send(());
    }
    // The consumer rejects pre-resume observations by their acquisition time.
    // Do not drain here: errors and lifecycle notifications are reliable events.
    pub fn resume(&self) {
        self.paused.store(false, Ordering::Release);
        let _ = self.wake.try_send(());
    }
    pub fn stop(&self) {
        self.stopped.store(true, Ordering::Release);
        let _ = self.wake.try_send(());
    }
    pub fn try_recv(&self) -> Result<GpuCaptureEvent, TryRecvError> {
        self.queue.try_recv().map(|(event, len)| {
            self.stats.buffer_fill.store(len as u64, Ordering::Relaxed);
            event
        })
    }
    pub fn recv_timeout(&self, wait: Duration) -> Result<GpuCaptureEvent, RecvTimeoutError> {
        self.queue.recv_timeout(wait).map(|(event, len)| {
            self.stats.buffer_fill.store(len as u64, Ordering::Relaxed);
            event
        })
    }
    pub fn stop_and_drain(mut self) -> Vec<GpuCaptureEvent> {
        self.stop();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
        self.queue.drain()
    }
}

impl Drop for GpuCaptureStream {
    fn drop(&mut self) {
        self.stop();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

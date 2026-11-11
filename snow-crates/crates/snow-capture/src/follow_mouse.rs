//! Recording-only moving viewports. Native sources stay on their owner threads.
//! One active source and one preparing/retiring source bound native resources.
use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, Sender};
use snow_cursor::{CursorProjector, CursorSampler, CursorShape, CursorSnapshot, CursorTargetInfo};
use snow_d3d11::{Rect, SharedDevice, TexturePool, VideoLayer};
use snow_media::follow_mouse::{FollowMouseMotion, FollowMouseOptions};
use snow_media::geometry::{DesktopRect, DesktopSpace, DesktopTransform, PixelSize};
use windows::Win32::Graphics::Direct3D11::*;
use windows::Win32::Graphics::Dxgi::Common::*;

use crate::backend::CaptureBackendKind;
use crate::convert::SurfaceConversionOptions;
use crate::error::{CaptureError, CaptureResult};
use crate::frame::{Frame, FrameMetadata};
use crate::gpu::{GpuCaptureSession, GpuCapturedFrame, PreparedMonitor};
use crate::monitor::MonitorKey;
use crate::platform::windows::{rotation, surface};
use crate::region::{CaptureRegion, MonitorGeometry};
use crate::streaming::AlignedCaptureCadence;
use crate::{
    CaptureOptions, CapturePixelFormat, CaptureStreamConfig, CaptureStreamStats, CaptureSystem,
    CaptureTarget,
};

const PREPARE_TIMEOUT: Duration = Duration::from_secs(5);
const BACKEND_TIMEOUT: Duration = Duration::from_millis(1200);
const RETRY_DELAY: Duration = Duration::from_secs(1);
const SOURCE_DEPTH: usize = 1;

mod discovery;
use discovery::Discovery;

fn source_geometry_changed(error: &CaptureError) -> bool {
    matches!(
        error,
        CaptureError::ResolutionChanged(..) | CaptureError::MonitorLost
    )
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Monitor {
    key: MonitorKey,
    bounds: CaptureRegion,
    topology: u64,
}

impl Monitor {
    fn from_geometry(monitor: &MonitorGeometry, topology: u64) -> Self {
        Self {
            key: monitor.monitor.key(),
            bounds: CaptureRegion {
                x: monitor.x,
                y: monitor.y,
                width: monitor.width,
                height: monitor.height,
            },
            topology,
        }
    }
    fn contains(&self, x: i32, y: i32) -> bool {
        i64::from(x) >= i64::from(self.bounds.x)
            && i64::from(y) >= i64::from(self.bounds.y)
            && i64::from(x) < i64::from(self.bounds.x) + i64::from(self.bounds.width)
            && i64::from(y) < i64::from(self.bounds.y) + i64::from(self.bounds.height)
    }
}

/// Floor a uniform fit using integer arithmetic so large/negative desktop
/// coordinates never affect the size calculation or escape the monitor.
fn crop(nominal: CaptureRegion, bounds: CaptureRegion, x: i32, y: i32) -> CaptureRegion {
    let (nw, nh) = (u64::from(nominal.width), u64::from(nominal.height));
    let (bw, bh) = (u64::from(bounds.width), u64::from(bounds.height));
    let (width, height) = if nw <= bw && nh <= bh {
        (nw, nh)
    } else if bw * nh <= bh * nw {
        (bw, (nh * bw / nw).max(1))
    } else {
        ((nw * bh / nh).max(1), bh)
    };
    let left = (i64::from(x) - width as i64 / 2).clamp(
        i64::from(bounds.x),
        i64::from(bounds.x) + bounds.width as i64 - width as i64,
    );
    let top = (i64::from(y) - height as i64 / 2).clamp(
        i64::from(bounds.y),
        i64::from(bounds.y) + bounds.height as i64 - height as i64,
    );
    CaptureRegion {
        x: left as i32,
        y: top as i32,
        width: width as u32,
        height: height as u32,
    }
}

fn desktop_rect(region: CaptureRegion) -> DesktopRect {
    DesktopRect {
        space: DesktopSpace::PhysicalPixels,
        x: f64::from(region.x),
        y: f64::from(region.y),
        width: f64::from(region.width),
        height: f64::from(region.height),
    }
}

// CPU observations stay inline, just like CaptureEvent, to avoid allocating a
// box for every captured frame. The queue depth bounds their aggregate size.
#[allow(clippy::large_enum_variant)]
#[derive(Clone)]
pub(crate) enum FollowFrame {
    Cpu(Frame),
    Gpu(GpuCapturedFrame),
}
impl FollowFrame {
    fn metadata_mut(&mut self) -> &mut FrameMetadata {
        match self {
            Self::Cpu(frame) => &mut frame.metadata,
            Self::Gpu(frame) => Arc::make_mut(&mut frame.metadata),
        }
    }
    fn region(&self) -> CaptureRegion {
        let metadata = match self {
            Self::Cpu(frame) => frame.metadata(),
            Self::Gpu(frame) => frame.metadata(),
        };
        let source = metadata
            .capture_transform()
            .expect("following frame has a transform")
            .source;
        CaptureRegion {
            x: source.x as i32,
            y: source.y as i32,
            width: source.width as u32,
            height: source.height as u32,
        }
    }
    fn device(&self) -> Option<SharedDevice> {
        match self {
            Self::Gpu(frame) => frame
                .layers
                .first()
                .map(|layer| layer.texture.device().clone()),
            Self::Cpu(_) => None,
        }
    }
    fn with_metadata(&self, metadata: FrameMetadata) -> Self {
        let mut frame = self.clone();
        *frame.metadata_mut() = metadata;
        frame
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct ViewKey {
    content: u64,
    region: CaptureRegion,
}
impl ViewKey {
    fn from_metadata(metadata: &FrameMetadata) -> Option<Self> {
        let source = metadata.capture_transform()?.source;
        Some(Self {
            content: metadata.content_generation()?,
            region: CaptureRegion {
                x: source.x as i32,
                y: source.y as i32,
                width: source.width as u32,
                height: source.height as u32,
            },
        })
    }
}

#[derive(Default)]
struct Revisions {
    previous: Option<(Monitor, CaptureRegion)>,
    native_content: Option<u64>,
    content: u64,
    geometry: u64,
    sequence: u64,
}
impl Revisions {
    fn apply(&mut self, metadata: &mut FrameMetadata, monitor: Monitor, region: CaptureRegion) {
        self.apply_at(metadata, monitor, region, Instant::now());
    }
    fn apply_at(
        &mut self,
        metadata: &mut FrameMetadata,
        monitor: Monitor,
        region: CaptureRegion,
        admitted: Instant,
    ) {
        let changed = self.previous != Some((monitor, region));
        // The source's changed observation may have been replaced by a later
        // duplicate in its one-frame queue. Duplicate is relative to acquisition,
        // so compare native identity before assigning the admitted revision.
        let content_changed =
            changed || !metadata.is_duplicate || self.native_content != metadata.content_generation;
        self.native_content = metadata.content_generation;
        if content_changed {
            self.content = self.content.wrapping_add(1);
            metadata.is_duplicate = false;
            metadata.dirty_rects.clear();
        }
        if changed {
            self.geometry = self.geometry.wrapping_add(1);
        }
        self.sequence = self.sequence.wrapping_add(1);
        metadata.sequence = self.sequence;
        metadata.content_generation = Some(self.content);
        metadata.configuration_generation = self.geometry;
        // A prepared destination may have completed before the last active
        // image was admitted. Its handoff belongs to this admission boundary,
        // while raw native presentation ticks and acquisition start survive.
        if let Some(timestamp) = &mut metadata.stream_timestamp {
            timestamp.instant = admitted;
        } else {
            metadata.set_timing(Some(admitted), None);
        }
        metadata.queued_at = Some(admitted);
        self.previous = Some((monitor, region));
    }
}

/// Give unversioned CPU observations an identity before the droppable queue.
/// Native GPU generations already survive duplicates and asynchronous readback.
fn publish_source_frame(
    mut frame: FollowFrame,
    unversioned_content: &mut u64,
    send: &Sender<CaptureResult<FollowFrame>>,
    discard: &Receiver<CaptureResult<FollowFrame>>,
) {
    let metadata = frame.metadata_mut();
    if metadata.content_generation.is_none() {
        if *unversioned_content == 0 || !metadata.is_duplicate {
            *unversioned_content = unversioned_content.wrapping_add(1);
        }
        metadata.content_generation = Some(*unversioned_content);
    }
    let _ = discard.try_recv();
    let _ = send.try_send(Ok(frame));
}

/// A replacement monitor may recover independently, but it cannot repair the
/// recording's output device. Let the recording worker handle that fallback.
fn recover_source_error(
    error: CaptureError,
    output_health: impl FnOnce() -> CaptureResult<()>,
) -> CaptureResult<()> {
    if error.allows_backend_fallback() || error.is_retryable() {
        output_health()
    } else {
        Err(error)
    }
}

fn check_output_device(output: Option<&SharedDevice>) -> CaptureResult<()> {
    output.map_or(Ok(()), |device| {
        device.check().map_err(CaptureError::platform)
    })
}

#[derive(Clone)]
pub(crate) struct Settings {
    pub nominal: CaptureRegion,
    pub preferred: CaptureBackendKind,
    pub options: CaptureOptions,
    pub config: CaptureStreamConfig,
    pub motion: FollowMouseOptions,
    pub gpu: bool,
    pub recycled: Option<Receiver<Frame>>,
}

#[cfg(feature = "bench-follow-mouse")]
static BENCH_CURSOR: Mutex<Option<(i32, i32)>> = Mutex::new(None);

/// Override desktop position without moving the user's pointer. This state is
/// process-local; benchmark scenarios must be serial and clear it afterwards.
#[cfg(feature = "bench-follow-mouse")]
pub fn set_bench_follow_mouse_position(point: Option<(i32, i32)>) {
    *BENCH_CURSOR
        .lock()
        .unwrap_or_else(|error| error.into_inner()) = point;
}

fn pointer_position() -> CaptureResult<(i32, i32)> {
    #[cfg(feature = "bench-follow-mouse")]
    if let Some(point) = *BENCH_CURSOR
        .lock()
        .unwrap_or_else(|error| error.into_inner())
    {
        return Ok(point);
    }
    let mut point = windows::Win32::Foundation::POINT::default();
    unsafe { windows::Win32::UI::WindowsAndMessaging::GetPhysicalCursorPos(&mut point) }
        .map_err(CaptureError::platform)?;
    Ok((point.x, point.y))
}

fn sample_cursor(cursor: &mut CursorSampler, settings: &Settings) -> CaptureResult<CursorSnapshot> {
    if !settings.config.include_cursor {
        let (absolute_x, absolute_y) = pointer_position()?;
        return Ok(CursorSnapshot {
            absolute_x,
            absolute_y,
            visible: false,
            shape: snow_cursor::CursorShapeCapture::Unavailable,
        });
    }
    let sample = cursor.sample().map_err(CaptureError::platform)?;
    #[cfg(feature = "bench-follow-mouse")]
    {
        let mut sample = sample;
        if let Some((x, y)) = *BENCH_CURSOR
            .lock()
            .unwrap_or_else(|error| error.into_inner())
        {
            sample.absolute_x = x;
            sample.absolute_y = y;
        }
        Ok(sample)
    }
    #[cfg(not(feature = "bench-follow-mouse"))]
    {
        Ok(sample)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum HandoffSlot<K> {
    Idle,
    Preparing(K),
    Retiring,
}

/// Resource ownership transitions are independent of native timing. In
/// particular, cancellation never permits a third producer during teardown.
struct Handoff<K> {
    active: K,
    desired: Option<K>,
    valid: bool,
    slot: HandoffSlot<K>,
    retry_at: Instant,
}
impl<K: Copy + Eq> Handoff<K> {
    fn new(active: K, now: Instant) -> Self {
        Self {
            active,
            desired: Some(active),
            valid: true,
            slot: HandoffSlot::Idle,
            retry_at: now,
        }
    }
    fn observe(&mut self, desired: Option<K>) -> bool {
        self.desired = desired;
        if matches!(self.slot, HandoffSlot::Preparing(target) if Some(target) != desired) {
            self.slot = HandoffSlot::Retiring;
            return true;
        }
        false
    }
    fn start(&mut self, now: Instant) -> Option<K> {
        if self.slot != HandoffSlot::Idle || now < self.retry_at {
            return None;
        }
        let desired = self
            .desired
            .filter(|desired| !self.valid || *desired != self.active)?;
        self.slot = HandoffSlot::Preparing(desired);
        Some(desired)
    }
    fn commit(&mut self, prepared: K) -> bool {
        if self.slot != HandoffSlot::Preparing(prepared) || self.desired != Some(prepared) {
            return false;
        }
        self.active = prepared;
        self.valid = true;
        self.slot = HandoffSlot::Retiring;
        true
    }
    fn failed(&mut self, now: Instant) {
        self.slot = HandoffSlot::Retiring;
        self.retry_at = now + RETRY_DELAY;
    }
    fn retired(&mut self) {
        self.slot = HandoffSlot::Idle;
    }
}

fn backend_order(preferred: CaptureBackendKind) -> Vec<CaptureBackendKind> {
    let defaults = [
        CaptureBackendKind::WindowsGraphicsCapture,
        CaptureBackendKind::DxgiDuplication,
        CaptureBackendKind::Gdi,
    ];
    let mut order = Vec::with_capacity(3);
    if preferred != CaptureBackendKind::Auto {
        order.push(preferred);
    }
    order.extend(defaults.into_iter().filter(|backend| *backend != preferred));
    order
}

struct SourceControl {
    canceled: AtomicBool,
    // Odd revisions are paused. The transition and its revision are observed
    // together so the source never integrates across a pause/resume race.
    pause_revision: AtomicU32,
    admitted: AtomicBool,
    timing: Mutex<(Option<Instant>, u32)>,
    wake: Sender<()>,
    changes: Receiver<()>,
}
impl SourceControl {
    fn new(fps: u32) -> Self {
        let (wake, changes) = crossbeam_channel::bounded(1);
        Self {
            canceled: AtomicBool::new(false),
            pause_revision: AtomicU32::new(0),
            admitted: AtomicBool::new(false),
            timing: Mutex::new((None, fps)),
            wake,
            changes,
        }
    }
    fn cancel(&self) {
        self.canceled.store(true, Ordering::Release);
        let _ = self.wake.try_send(());
    }
    fn is_canceled(&self) -> bool {
        self.canceled.load(Ordering::Acquire)
    }
    fn set_paused(&self, paused: bool) {
        if self
            .pause_revision
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |revision| {
                ((revision & 1 != 0) != paused).then(|| revision.wrapping_add(1))
            })
            .is_ok()
        {
            let _ = self.wake.try_send(());
        }
    }
    fn set_timing(&self, origin: Option<Instant>, fps: u32) {
        let mut timing = self
            .timing
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if *timing != (origin, fps) {
            *timing = (origin, fps);
            drop(timing);
            let _ = self.wake.try_send(());
        }
    }
    fn wait(&self, timeout: Duration) -> bool {
        // Channel timeouts use coarse Windows timer boundaries. Keep native
        // sleep precision for active capture; only paused workers block on the
        // channel. Short sleeps bound control latency without a busy spin.
        let deadline = Instant::now() + timeout;
        loop {
            if self.is_canceled() || self.changes.try_recv().is_ok() {
                return true;
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return false;
            }
            std::thread::sleep(remaining.min(Duration::from_millis(5)));
        }
    }
    fn wait_until_resumed(&self) -> bool {
        while self.pause_revision.load(Ordering::Acquire) & 1 != 0 && !self.is_canceled() {
            if self.changes.recv().is_err() {
                return false;
            }
        }
        !self.is_canceled()
    }
}

struct SourceWorker {
    monitor: Monitor,
    control: Arc<SourceControl>,
    frames: Receiver<CaptureResult<FollowFrame>>,
    thread: Option<JoinHandle<()>>,
}
impl SourceWorker {
    fn spawn(
        settings: Settings,
        monitor: Monitor,
        output: Option<SharedDevice>,
    ) -> CaptureResult<Self> {
        let control = Arc::new(SourceControl::new(settings.config.target_fps));
        let (send, frames) = crossbeam_channel::bounded(SOURCE_DEPTH);
        let discard = frames.clone();
        let worker_control = control.clone();
        let thread = std::thread::Builder::new()
            .name("snow-follow-source".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                let result =
                    source_loop(&settings, monitor, output, &worker_control, &send, &discard);
                if let Err(error) = result {
                    let _ = discard.try_recv();
                    let _ = send.try_send(Err(error));
                }
            })
            .map_err(CaptureError::platform)?;
        Ok(Self {
            monitor,
            control,
            frames,
            thread: Some(thread),
        })
    }
    fn cancel(&self) {
        self.control.cancel();
    }
    fn is_finished(&self) -> bool {
        self.thread.as_ref().is_none_or(JoinHandle::is_finished)
    }
    fn latest(&self) -> Option<CaptureResult<FollowFrame>> {
        self.frames.try_iter().last()
    }
}
impl Drop for SourceWorker {
    fn drop(&mut self) {
        self.cancel();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

/// Startup has no admitted image or encoder device to preserve. Retire stale
/// work before selecting a fresh monitor, under one deadline for all attempts.
fn prepare_initial(
    stop: &AtomicBool,
    wake: &Receiver<()>,
    stats: &CaptureStreamStats,
    deadline: Instant,
    mut discover: impl FnMut(bool) -> CaptureResult<Option<Monitor>>,
    mut spawn: impl FnMut(Monitor) -> CaptureResult<SourceWorker>,
) -> CaptureResult<(SourceWorker, FollowFrame)> {
    let mut source: Option<SourceWorker> = None;
    let mut refresh = false;
    let mut retry_at = Instant::now();
    loop {
        if stop.load(Ordering::Acquire) {
            return Err(CaptureError::Canceled);
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(CaptureError::Timeout);
        }
        let desired = discover(std::mem::take(&mut refresh))?;
        if source
            .as_ref()
            .is_some_and(|worker| Some(worker.monitor) != desired)
        {
            source.take();
        }
        if source.is_none()
            && Instant::now() >= retry_at
            && let Some(desired) = desired
        {
            stats
                .follow_prepare_requests
                .fetch_add(1, Ordering::Relaxed);
            stats.follow_peak_sources.fetch_max(1, Ordering::Relaxed);
            source = Some(spawn(desired)?);
        }
        let wait = remaining.min(Duration::from_millis(10));
        let Some(worker) = &source else {
            let _ = wake.recv_timeout(wait);
            continue;
        };
        match worker.frames.recv_timeout(wait) {
            Ok(Ok(frame)) => {
                // A valid frame can arrive after another display transition.
                // Revalidate before publishing or selecting the output device.
                if discover(false)? == Some(worker.monitor) {
                    return Ok((source.take().unwrap(), frame));
                }
                source.take();
            }
            Ok(Err(error)) if source_geometry_changed(&error) => {
                source.take();
                refresh = true;
                retry_at = Instant::now() + RETRY_DELAY;
                stats
                    .follow_preparation_failures
                    .fetch_add(1, Ordering::Relaxed);
            }
            Ok(Err(error)) => return Err(error),
            Err(crossbeam_channel::RecvTimeoutError::Disconnected) => {
                return Err(CaptureError::WorkerDead);
            }
            Err(crossbeam_channel::RecvTimeoutError::Timeout) => {}
        }
    }
}

/// Shared supervisor for CPU and GPU stream handles. The source first-frame
/// barrier also covers CPU readback and a cross-adapter upload, when necessary.
pub(crate) fn run(
    settings: Settings,
    stop: &AtomicBool,
    pause: &AtomicBool,
    target_fps: &AtomicU32,
    origin: &Mutex<Option<Instant>>,
    wake: &Receiver<()>,
    stats: &CaptureStreamStats,
    ready: Sender<CaptureResult<Option<SharedDevice>>>,
    mut publish: impl FnMut(FollowFrame),
) -> CaptureResult<()> {
    if settings.nominal.width == 0
        || settings.nominal.height == 0
        || settings.config.target_fps == 0
    {
        return Err(CaptureError::InvalidConfig(
            "following requires nonempty geometry and positive FPS".into(),
        ));
    }
    let mut discovery = Discovery::new(CaptureSystem::builder().build()?)?;
    let (mut active, first) = prepare_initial(
        stop,
        wake,
        stats,
        Instant::now() + PREPARE_TIMEOUT,
        |refresh| {
            if refresh {
                discovery.invalidate();
            }
            Ok(if discovery.poll() {
                discovery.target(pointer_position()?)
            } else {
                None
            })
        },
        |monitor| SourceWorker::spawn(settings.clone(), monitor, None),
    )?;
    let output_device = first.device();
    let _ = ready.send(Ok(output_device.clone()));
    let mut revisions = Revisions::default();
    let mut emit = |mut frame: FollowFrame, monitor: Monitor| {
        let region = frame.region();
        if let Some(started) = frame.metadata_mut().observation_started_at {
            stats.capture_latency_avg_ns.store(
                (started.elapsed().as_nanos() as f64).to_bits(),
                Ordering::Relaxed,
            );
        }
        revisions.apply(frame.metadata_mut(), monitor, region);
        stats.frames_captured.fetch_add(1, Ordering::Relaxed);
        publish(frame);
    };
    emit(first, active.monitor);
    active.control.admitted.store(true, Ordering::Release);
    let mut pending: Option<SourceWorker> = None;
    let mut retiring: Option<SourceWorker> = None;
    let mut handoff = Handoff::new(active.monitor, Instant::now());
    let mut pending_since = Instant::now();
    let mut received = None;
    let mut samples = 0u64;
    let mut sample_epoch = Instant::now();
    while !stop.load(Ordering::Acquire) {
        let requested_origin = *origin.lock().unwrap_or_else(|error| error.into_inner());
        let fps = target_fps.load(Ordering::Acquire).max(1);
        active.control.set_timing(requested_origin, fps);
        let paused = pause.load(Ordering::Acquire);
        active.control.set_paused(paused);
        if let Some(worker) = &pending {
            worker.control.set_paused(paused);
            worker.control.set_timing(requested_origin, fps);
        }
        if retiring.as_ref().is_some_and(SourceWorker::is_finished) {
            retiring.take();
            handoff.retired();
        }
        if !paused {
            let topology_valid = discovery.poll();
            let desired = pointer_position()
                .ok()
                .and_then(|point| discovery.target(point));
            if handoff.observe(desired)
                && let Some(worker) = pending.take()
            {
                worker.cancel();
                retiring = Some(worker);
            }
            if let Some(worker) = &pending {
                match worker.latest() {
                    Some(Ok(mut frame)) if topology_valid && handoff.commit(worker.monitor) => {
                        let old = std::mem::replace(&mut active, pending.take().unwrap());
                        received = None;
                        old.cancel();
                        retiring = Some(old);
                        stats
                            .follow_monitor_switches
                            .fetch_add(1, Ordering::Relaxed);
                        stats.follow_prepare_latency_max_ns.fetch_max(
                            pending_since.elapsed().as_nanos().min(u128::from(u64::MAX)) as u64,
                            Ordering::Relaxed,
                        );
                        // A recreated source can restart its native generation
                        // on the same monitor and crop. Always admit its first image.
                        frame.metadata_mut().is_duplicate = false;
                        emit(frame, active.monitor);
                        active.control.admitted.store(true, Ordering::Release);
                    }
                    Some(Err(error)) => {
                        if source_geometry_changed(&error) {
                            discovery.invalidate();
                        }
                        recover_source_error(error, || {
                            check_output_device(output_device.as_ref())
                        })?;
                        worker.cancel();
                        retiring = pending.take();
                        handoff.failed(Instant::now());
                        stats
                            .follow_preparation_failures
                            .fetch_add(1, Ordering::Relaxed);
                    }
                    _ if pending_since.elapsed() >= PREPARE_TIMEOUT => {
                        check_output_device(output_device.as_ref())?;
                        worker.cancel();
                        retiring = pending.take();
                        handoff.failed(Instant::now());
                        stats
                            .follow_preparation_failures
                            .fetch_add(1, Ordering::Relaxed);
                    }
                    _ => {}
                }
            }
            if let Some(desired) = topology_valid
                .then(|| handoff.start(Instant::now()))
                .flatten()
            {
                stats
                    .follow_prepare_requests
                    .fetch_add(1, Ordering::Relaxed);
                match SourceWorker::spawn(settings.clone(), desired, output_device.clone()) {
                    Ok(worker) => {
                        pending = Some(worker);
                        stats.follow_peak_sources.fetch_max(2, Ordering::Relaxed);
                    }
                    Err(_) => {
                        handoff.failed(Instant::now());
                        handoff.retired();
                    }
                }
                pending_since = Instant::now();
            }
            match active.latest().or(received.take()) {
                Some(Ok(frame)) => {
                    emit(frame, active.monitor);
                    samples += 1;
                }
                Some(Err(error)) => {
                    if source_geometry_changed(&error) {
                        discovery.invalidate();
                    }
                    recover_source_error(error, || check_output_device(output_device.as_ref()))?;
                    handoff.valid = false;
                }
                _ => {}
            }
        }
        if sample_epoch.elapsed() >= Duration::from_secs(1) {
            stats.current_fps.store(
                (samples as f64 / sample_epoch.elapsed().as_secs_f64()).to_bits(),
                Ordering::Relaxed,
            );
            samples = 0;
            sample_epoch = Instant::now();
        }
        if paused {
            stats.current_fps.store(0.0f64.to_bits(), Ordering::Relaxed);
            let _ = wake.recv();
        } else if active.is_finished() {
            let _ = wake.recv_timeout(Duration::from_millis(10));
        } else {
            crossbeam_channel::select! {
                recv(wake) -> _ => {},
                recv(active.frames) -> frame => { received = frame.ok(); },
                default(Duration::from_millis(10)) => {},
            }
        }
    }
    active.cancel();
    if let Some(worker) = &pending {
        worker.cancel();
    }
    if let Some(worker) = &retiring {
        worker.cancel();
    }
    Ok(())
}

enum NativeSource {
    Gpu {
        capture: Box<GpuCaptureSession>,
        readback: Box<Readback>,
        upload: Option<Upload>,
        completed: Option<(ViewKey, FollowFrame)>,
    },
    Cpu {
        capture: Box<crate::CaptureSession>,
        upload: Option<Upload>,
    },
    Filtered {
        capture: Box<crate::CaptureSession>,
        canonical: Frame,
        upload: Option<Upload>,
    },
}
impl NativeSource {
    fn open(
        settings: &Settings,
        monitor: Monitor,
        backend: CaptureBackendKind,
        output: Option<SharedDevice>,
        point: &CursorSnapshot,
    ) -> CaptureResult<Self> {
        let region = crop(
            settings.nominal,
            monitor.bounds,
            point.absolute_x,
            point.absolute_y,
        );
        // Explicit capture filters belong to CaptureSession. Retain its complete
        // monitor image so cropping cannot bypass filter semantics or require
        // producer reconfiguration on each pointer observation.
        if !settings.options.excluded_windows.is_empty()
            || !settings.options.excluded_processes.is_empty()
        {
            let system = CaptureSystem::builder()
                .with_backend_kind(backend)
                .build()?;
            let capture = Box::new(system.open_session(
                CaptureTarget::Region(monitor.bounds),
                settings.options.clone(),
            )?);
            return Ok(Self::Filtered {
                capture,
                canonical: Frame::empty(),
                upload: output.map(Upload::new),
            });
        }
        if backend == CaptureBackendKind::Gdi {
            if settings.gpu && output.is_none() {
                return Err(CaptureError::BackendUnavailable(
                    "GDI has no initial GPU device".into(),
                ));
            }
            let system = CaptureSystem::builder()
                .with_backend_kind(backend)
                .build()?;
            let capture = Box::new(
                system.open_session(CaptureTarget::Region(region), settings.options.clone())?,
            );
            return Ok(Self::Cpu {
                capture,
                upload: output.map(Upload::new),
            });
        }
        let capture = Box::new(GpuCaptureSession::open_follow(
            region,
            backend,
            output.clone(),
            PreparedMonitor {
                key: monitor.key,
                bounds: Rect {
                    x: monitor.bounds.x,
                    y: monitor.bounds.y,
                    width: monitor.bounds.width,
                    height: monitor.bounds.height,
                },
            },
        )?);
        let cross_adapter = output
            .as_ref()
            .is_some_and(|device| !device.same_device(capture.device()));
        let readback = Box::new(Readback::new(capture.device().clone()));
        Ok(Self::Gpu {
            capture,
            readback,
            upload: cross_adapter.then(|| Upload::new(output.unwrap())),
            completed: None,
        })
    }
    fn capture(
        &mut self,
        settings: &Settings,
        region: CaptureRegion,
        point: CursorSnapshot,
        projector: &mut CursorProjector,
        shape: &mut Option<CursorShape>,
    ) -> CaptureResult<Option<FollowFrame>> {
        let started = Instant::now();
        match self {
            Self::Gpu {
                capture,
                readback,
                upload,
                completed,
            } => {
                capture.set_follow_region(region)?;
                let mut frame = capture.capture_follow()?;
                annotate(
                    Arc::make_mut(&mut frame.metadata),
                    region,
                    point,
                    projector,
                    shape,
                    settings.config.include_cursor,
                    started,
                )?;
                if !settings.gpu || upload.is_some() {
                    let key = ViewKey::from_metadata(frame.metadata())
                        .expect("following frame has a content key");
                    if let Some((previous, cached)) = completed
                        && *previous == key
                    {
                        return Ok(Some(cached.with_metadata(frame.metadata().clone())));
                    }
                    if upload
                        .as_ref()
                        .is_some_and(|upload| !upload.pool.has_capacity())
                    {
                        return Ok(None);
                    }
                    if !readback.contains(key) {
                        readback.submit(&frame)?;
                    }
                    let pixel_format = if settings.gpu {
                        CapturePixelFormat::Bgra8
                    } else {
                        settings.options.output_pixel_format
                    };
                    let Some(cpu) = readback.poll(settings.recycled.as_ref(), pixel_format)? else {
                        return Ok(None);
                    };
                    let completed_key = ViewKey::from_metadata(cpu.metadata())
                        .expect("following CPU crop has a content key");
                    if let Some(upload) = upload {
                        let result = upload.frame(&cpu).map(|frame| frame.map(FollowFrame::Gpu));
                        readback.recycle(cpu);
                        if let Ok(Some(frame)) = &result {
                            *completed = Some((completed_key, frame.clone()));
                        }
                        return result;
                    }
                    let frame = FollowFrame::Cpu(cpu);
                    *completed = Some((completed_key, frame.clone()));
                    Ok(Some(frame))
                } else {
                    Ok(Some(FollowFrame::Gpu(frame)))
                }
            }
            Self::Cpu { capture, upload } => {
                capture.set_follow_region(region);
                let reuse = settings
                    .recycled
                    .as_ref()
                    .and_then(|receiver| receiver.try_recv().ok());
                let mut frame = capture.capture_frame(reuse)?;
                annotate(
                    &mut frame.metadata,
                    region,
                    point,
                    projector,
                    shape,
                    settings.config.include_cursor,
                    started,
                )?;
                if let Some(upload) = upload {
                    return upload
                        .frame(&frame)
                        .map(|frame| frame.map(FollowFrame::Gpu));
                }
                Ok(Some(FollowFrame::Cpu(frame)))
            }
            Self::Filtered {
                capture,
                canonical,
                upload,
            } => {
                *canonical =
                    capture.capture_frame(Some(std::mem::replace(canonical, Frame::empty())))?;
                let bounds = capture.target_info()?;
                let mut frame = settings
                    .recycled
                    .as_ref()
                    .and_then(|receiver| receiver.try_recv().ok())
                    .unwrap_or_else(Frame::empty);
                frame.ensure_capacity(region.width, region.height, canonical.pixel_format())?;
                let source_x = usize::try_from(i64::from(region.x) - i64::from(bounds.origin_x))
                    .map_err(|_| CaptureError::BufferOverflow)?;
                let source_y = usize::try_from(i64::from(region.y) - i64::from(bounds.origin_y))
                    .map_err(|_| CaptureError::BufferOverflow)?;
                let stride = canonical.width() as usize * 4;
                let row_bytes = region.width as usize * 4;
                for (row, out) in frame.as_mut_bytes().chunks_exact_mut(row_bytes).enumerate() {
                    let offset = (source_y + row) * stride + source_x * 4;
                    let input = canonical
                        .as_bytes()
                        .get(offset..offset + row_bytes)
                        .ok_or(CaptureError::BufferOverflow)?;
                    out.copy_from_slice(input);
                }
                frame.metadata = canonical.metadata().clone();
                annotate(
                    &mut frame.metadata,
                    region,
                    point,
                    projector,
                    shape,
                    settings.config.include_cursor,
                    started,
                )?;
                if let Some(upload) = upload {
                    return upload
                        .frame(&frame)
                        .map(|frame| frame.map(FollowFrame::Gpu));
                }
                Ok(Some(FollowFrame::Cpu(frame)))
            }
        }
    }
}

fn annotate(
    metadata: &mut FrameMetadata,
    region: CaptureRegion,
    point: CursorSnapshot,
    projector: &mut CursorProjector,
    shape: &mut Option<CursorShape>,
    include_cursor: bool,
    started: Instant,
) -> CaptureResult<()> {
    let now = Instant::now();
    metadata.capture_transform = Some(
        DesktopTransform::new(
            desktop_rect(region),
            PixelSize::new(region.width, region.height).map_err(CaptureError::platform)?,
        )
        .map_err(CaptureError::platform)?,
    );
    metadata.observation_started_at = Some(started);
    if let Some(timestamp) = &mut metadata.stream_timestamp {
        timestamp.instant = now;
    } else {
        metadata.set_timing(Some(now), None);
    }
    metadata.queued_at = Some(now);
    metadata.cursor = include_cursor.then(|| {
        crate::gpu::project_cursor(
            projector,
            shape,
            &CursorTargetInfo {
                origin_x: region.x,
                origin_y: region.y,
                width: region.width,
                height: region.height,
            },
            point,
        )
    });
    Ok(())
}

fn source_loop(
    settings: &Settings,
    monitor: Monitor,
    output: Option<SharedDevice>,
    control: &SourceControl,
    send: &Sender<CaptureResult<FollowFrame>>,
    discard: &Receiver<CaptureResult<FollowFrame>>,
) -> CaptureResult<()> {
    let mut cursor = CursorSampler::new().map_err(CaptureError::platform)?;
    let mut projector = CursorProjector::new();
    let mut shape = None;
    let mut last_cursor = sample_cursor(&mut cursor, settings)?;
    let mut source = None;
    let mut last_error = CaptureError::Timeout;
    let mut unversioned_content = 0;
    for backend in backend_order(settings.preferred) {
        if control.is_canceled() {
            return Ok(());
        }
        let point = sample_cursor(&mut cursor, settings).unwrap_or_else(|_| last_cursor.clone());
        last_cursor = point.clone();
        let mut candidate =
            match NativeSource::open(settings, monitor, backend, output.clone(), &point) {
                Ok(candidate) => candidate,
                Err(error @ (CaptureError::ResolutionChanged(..) | CaptureError::MonitorLost)) => {
                    return Err(error);
                }
                Err(error) if error.allows_backend_fallback() => {
                    last_error = error;
                    continue;
                }
                Err(error) => return Err(error),
            };
        let deadline = Instant::now() + BACKEND_TIMEOUT;
        loop {
            if control.is_canceled() {
                return Ok(());
            }
            let point =
                sample_cursor(&mut cursor, settings).unwrap_or_else(|_| last_cursor.clone());
            last_cursor = point.clone();
            let region = crop(
                settings.nominal,
                monitor.bounds,
                point.absolute_x,
                point.absolute_y,
            );
            match candidate.capture(settings, region, point, &mut projector, &mut shape) {
                Ok(Some(frame)) => {
                    let initial_region = frame.region();
                    publish_source_frame(frame, &mut unversioned_content, send, discard);
                    source = Some((candidate, initial_region));
                    break;
                }
                Err(error @ (CaptureError::ResolutionChanged(..) | CaptureError::MonitorLost)) => {
                    return Err(error);
                }
                Ok(None) | Err(CaptureError::Timeout | CaptureError::AccessLost)
                    if Instant::now() < deadline =>
                {
                    control.wait(Duration::from_millis(2));
                }
                Ok(None) => {
                    last_error = CaptureError::Timeout;
                    break;
                }
                Err(error) if error.allows_backend_fallback() => {
                    last_error = error;
                    break;
                }
                Err(error) => return Err(error),
            }
        }
        if source.is_some() {
            break;
        }
    }
    let (mut source, initial_region) = source.ok_or(last_error)?;
    let mut motion = FollowMouseMotion::new(
        desktop_rect(initial_region),
        desktop_rect(monitor.bounds),
        settings.motion,
        Instant::now(),
    );
    let mut cadence = AlignedCaptureCadence {
        origin: Instant::now(),
        fps: settings.config.target_fps,
        next: 0,
    };
    let mut failures = 0usize;
    let mut previous_origin = None;
    let mut last_pause_revision = 0;
    while control.wait_until_resumed() {
        let (origin, fps) = *control
            .timing
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if origin != previous_origin || cadence.fps != fps {
            cadence.origin = origin.unwrap_or_else(Instant::now);
            cadence.fps = fps;
            cadence.next = 0;
            previous_origin = origin;
        }
        if cadence.wait_interruptibly(Instant::now(), |wait| control.wait(wait))
            || control.is_canceled()
        {
            continue;
        }
        let pause_revision = control.pause_revision.load(Ordering::Acquire);
        if pause_revision & 1 != 0 {
            continue;
        }
        let point = sample_cursor(&mut cursor, settings).unwrap_or_else(|_| last_cursor.clone());
        last_cursor = point.clone();
        if pause_revision != last_pause_revision {
            motion.restart_clock();
            last_pause_revision = pause_revision;
        }
        let now = Instant::now();
        let position = (f64::from(point.absolute_x), f64::from(point.absolute_y));
        let view = if control.admitted.load(Ordering::Acquire) {
            motion.advance(position, now)
        } else {
            motion.center(position, now)
        };
        let region = CaptureRegion {
            x: view.x as i32,
            y: view.y as i32,
            ..initial_region
        };
        match source.capture(settings, region, point, &mut projector, &mut shape) {
            Ok(Some(frame)) => {
                failures = 0;
                publish_source_frame(frame, &mut unversioned_content, send, discard);
            }
            Ok(None) | Err(CaptureError::Timeout) => {}
            Err(error @ (CaptureError::ResolutionChanged(..) | CaptureError::MonitorLost)) => {
                return Err(error);
            }
            Err(error)
                if error.is_retryable() && failures < settings.config.max_consecutive_errors =>
            {
                failures += 1;
            }
            Err(error) => return Err(error),
        }
    }
    Ok(())
}

struct ReadbackSlot {
    staging: Option<ID3D11Texture2D>,
    observation: Option<(FrameMetadata, (u32, u32), u32)>,
}
struct Readback {
    device: SharedDevice,
    slots: [ReadbackSlot; 2],
    pending: VecDeque<usize>,
    pixels: Frame,
    oriented: Frame,
    last_rotation: u32,
}
impl Readback {
    fn new(device: SharedDevice) -> Self {
        Self {
            device,
            slots: std::array::from_fn(|_| ReadbackSlot {
                staging: None,
                observation: None,
            }),
            pending: VecDeque::with_capacity(2),
            pixels: Frame::empty(),
            oriented: Frame::empty(),
            last_rotation: 0,
        }
    }
    fn submit(&mut self, frame: &GpuCapturedFrame) -> CaptureResult<()> {
        let Some(index) = self
            .slots
            .iter()
            .position(|slot| slot.observation.is_none())
        else {
            return Ok(());
        };
        let layer = frame
            .layers
            .first()
            .ok_or_else(|| CaptureError::InvalidConfig("empty following GPU frame".into()))?;
        if frame.layers.len() != 1 {
            return Err(CaptureError::InvalidConfig(
                "following readback requires one layer".into(),
            ));
        }
        let mut desc = layer.texture.desc();
        desc.Width = layer.source.width;
        desc.Height = layer.source.height;
        let device = self.device.clone();
        let _lock = device.lock();
        let slot = &mut self.slots[index];
        let staging = surface::ensure_staging_texture(
            device.device(),
            &mut slot.staging,
            &desc,
            surface::StagingSampleDesc::SingleSample,
            "follow staging allocation",
        )?;
        let source = D3D11_BOX {
            left: layer.source.x as u32,
            top: layer.source.y as u32,
            front: 0,
            right: layer.source.x as u32 + layer.source.width,
            bottom: layer.source.y as u32 + layer.source.height,
            back: 1,
        };
        unsafe {
            device.context().CopySubresourceRegion(
                staging,
                0,
                0,
                0,
                0,
                layer.texture.raw(),
                0,
                Some(&source),
            );
            device.context().Flush();
        }
        slot.observation = Some((frame.metadata().clone(), frame.dimensions(), layer.rotation));
        self.pending.push_back(index);
        Ok(())
    }
    fn contains(&self, key: ViewKey) -> bool {
        self.slots.iter().any(|slot| {
            slot.observation
                .as_ref()
                .is_some_and(|(metadata, _, _)| ViewKey::from_metadata(metadata) == Some(key))
        })
    }
    fn poll(
        &mut self,
        recycled: Option<&Receiver<Frame>>,
        pixel_format: CapturePixelFormat,
    ) -> CaptureResult<Option<Frame>> {
        let Some(index) = self.pending.front().copied() else {
            return Ok(None);
        };
        let device = self.device.clone();
        let _lock = device.lock();
        let slot = &mut self.slots[index];
        let staging = slot.staging.as_ref().unwrap();
        let mut desc = D3D11_TEXTURE2D_DESC::default();
        unsafe {
            staging.GetDesc(&mut desc);
        }
        if !surface::try_map_staging_to_frame(
            device.context(),
            staging,
            &desc,
            &mut self.pixels,
            SurfaceConversionOptions {
                force_opaque_alpha: true,
                output_pixel_format: pixel_format,
                ..Default::default()
            },
        )? {
            return Ok(None);
        }
        // Mapping is finished; CPU orientation must not hold the device lock
        // needed by native frame callbacks or another source during handoff.
        drop(_lock);
        self.pending.pop_front();
        let (metadata, size, quarter_turns) = slot.observation.take().unwrap();
        let spare = || {
            recycled
                .and_then(|receiver| receiver.try_recv().ok())
                .unwrap_or_else(Frame::empty)
        };
        let mut result = if quarter_turns == 0 {
            std::mem::replace(&mut self.pixels, spare())
        } else {
            let mut output = std::mem::replace(&mut self.oriented, spare());
            output.ensure_capacity(size.0, size.1, pixel_format)?;
            let orientation = match quarter_turns {
                1 => DXGI_MODE_ROTATION_ROTATE90,
                2 => DXGI_MODE_ROTATION_ROTATE180,
                _ => DXGI_MODE_ROTATION_ROTATE270,
            };
            rotation::orient_into(&self.pixels, &mut output, 0, 0, orientation)?;
            output
        };
        self.last_rotation = quarter_turns;
        result.metadata = metadata;
        Ok(Some(result))
    }
    fn recycle(&mut self, frame: Frame) {
        if self.last_rotation == 0 {
            self.pixels = frame;
        } else {
            self.oriented = frame;
        }
    }
}

struct Upload {
    device: SharedDevice,
    pool: TexturePool,
}
impl Upload {
    fn new(device: SharedDevice) -> Self {
        Self {
            pool: TexturePool::new(device.clone(), 6),
            device,
        }
    }
    fn frame(&mut self, frame: &Frame) -> CaptureResult<Option<GpuCapturedFrame>> {
        let format = match frame.pixel_format() {
            CapturePixelFormat::Bgra8 => DXGI_FORMAT_B8G8R8A8_UNORM,
            CapturePixelFormat::Rgba8 => DXGI_FORMAT_R8G8B8A8_UNORM,
        };
        let device = self.device.clone();
        let _lock = device.lock();
        let Some(texture) = self
            .pool
            .acquire(
                frame.width(),
                frame.height(),
                format,
                D3D11_BIND_SHADER_RESOURCE.0 as u32,
            )
            .map_err(CaptureError::platform)?
        else {
            return Ok(None);
        };
        unsafe {
            device.context().UpdateSubresource(
                texture.raw(),
                0,
                None,
                frame.as_bytes().as_ptr().cast(),
                frame.width() * 4,
                0,
            );
        }
        GpuCapturedFrame::from_layers(
            vec![VideoLayer {
                texture,
                source: Rect::full(frame.dimensions()),
                destination: Rect::full(frame.dimensions()),
                rotation: 0,
                alpha: false,
            }],
            frame.dimensions(),
            frame.metadata().clone(),
        )
        .map(Some)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn region(x: i32, y: i32, width: u32, height: u32) -> CaptureRegion {
        CaptureRegion {
            x,
            y,
            width,
            height,
        }
    }

    fn prepared_source(monitor: Monitor, observation: CaptureResult<FollowFrame>) -> SourceWorker {
        let (send, frames) = crossbeam_channel::bounded(1);
        send.send(observation)
            .unwrap_or_else(|_| panic!("empty preparation queue"));
        SourceWorker {
            monitor,
            control: Arc::new(SourceControl::new(30)),
            frames,
            thread: None,
        }
    }

    #[test]
    fn startup_geometry_failure_reprepares_a_fitted_source_before_readiness() {
        let initial = Monitor {
            key: MonitorKey {
                adapter_luid: 0,
                output_id: 1,
            },
            bounds: region(0, 0, 1920, 1080),
            topology: 1,
        };
        let resized = Monitor {
            bounds: region(0, 0, 1280, 720),
            topology: 2,
            ..initial
        };
        let stop = AtomicBool::new(false);
        let (_wake, commands) = crossbeam_channel::bounded(1);
        let stats = CaptureStreamStats::default();
        let mut selected = initial;
        let mut attempts = 0;
        let (worker, frame) = prepare_initial(
            &stop,
            &commands,
            &stats,
            Instant::now() + PREPARE_TIMEOUT,
            |refresh| {
                if refresh {
                    selected = resized;
                }
                Ok(Some(selected))
            },
            |monitor| {
                attempts += 1;
                let observation = if monitor == initial {
                    Err(CaptureError::ResolutionChanged(1280, 720))
                } else {
                    let bounds = crop(region(0, 0, 1600, 900), monitor.bounds, 500, 400);
                    assert_eq!(bounds, region(0, 0, 1280, 720));
                    let mut frame = Frame::from_rgba8(1, 1, vec![42; 4]).unwrap();
                    frame.metadata.capture_transform = Some(
                        DesktopTransform::new(
                            DesktopRect {
                                space: DesktopSpace::PhysicalPixels,
                                x: 0.0,
                                y: 0.0,
                                width: 1280.0,
                                height: 720.0,
                            },
                            PixelSize::new(1, 1).unwrap(),
                        )
                        .unwrap(),
                    );
                    Ok(FollowFrame::Cpu(frame))
                };
                Ok(prepared_source(monitor, observation))
            },
        )
        .unwrap();
        assert_eq!(attempts, 2);
        assert_eq!(worker.monitor, resized);
        assert_eq!(frame.region(), resized.bounds);
        assert_eq!(stats.follow_preparation_failures.load(Ordering::Relaxed), 1);
        assert_eq!(stats.follow_peak_sources.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn startup_recovery_respects_original_deadline_and_terminal_errors() {
        let monitor = Monitor {
            key: MonitorKey {
                adapter_luid: 0,
                output_id: 1,
            },
            bounds: region(0, 0, 1920, 1080),
            topology: 1,
        };
        let stop = AtomicBool::new(false);
        let (_wake, commands) = crossbeam_channel::bounded(1);
        let stats = CaptureStreamStats::default();
        let mut attempts = 0;
        let deadline = Instant::now() + Duration::from_millis(20);
        let result = prepare_initial(
            &stop,
            &commands,
            &stats,
            deadline,
            |_| Ok(Some(monitor)),
            |monitor| {
                attempts += 1;
                Ok(prepared_source(
                    monitor,
                    Err(CaptureError::ResolutionChanged(1280, 720)),
                ))
            },
        );
        assert!(matches!(result, Err(CaptureError::Timeout)));
        assert!(Instant::now() >= deadline);
        assert_eq!(
            attempts, 1,
            "stale-target failures must respect retry backoff"
        );
        for error in [
            CaptureError::InvalidTarget("invalid crop".into()),
            CaptureError::InvalidConfig("invalid settings".into()),
            CaptureError::PermissionDenied,
            CaptureError::Canceled,
            CaptureError::BufferOverflow,
        ] {
            let expected = error.to_string();
            let result = prepare_initial(
                &stop,
                &commands,
                &stats,
                Instant::now() + PREPARE_TIMEOUT,
                |_| Ok(Some(monitor)),
                |monitor| Ok(prepared_source(monitor, Err(error.clone()))),
            );
            assert_eq!(result.err().unwrap().to_string(), expected);
        }
        stop.store(true, Ordering::Release);
        let result = prepare_initial(
            &stop,
            &commands,
            &stats,
            Instant::now() + PREPARE_TIMEOUT,
            |_| panic!("canceled startup must not discover displays"),
            |_| panic!("canceled startup must not spawn sources"),
        );
        assert!(matches!(result, Err(CaptureError::Canceled)));
    }

    #[test]
    fn geometry_failure_keeps_handoff_and_output_device_error_semantics() {
        let now = Instant::now();
        let mut handoff = Handoff::new('A', now);
        handoff.observe(Some('B'));
        assert_eq!(handoff.start(now), Some('B'));
        recover_source_error(CaptureError::ResolutionChanged(1280, 720), || Ok(())).unwrap();
        handoff.valid = false;
        assert_eq!(handoff.active, 'A');
        assert_eq!(
            handoff.start(now),
            None,
            "no third source during preparation"
        );
        assert!(handoff.commit('B'));
        handoff.retired();
        let error = recover_source_error(CaptureError::ResolutionChanged(1280, 720), || {
            Err(CaptureError::platform(anyhow::anyhow!(
                "recording device removed"
            )))
        })
        .unwrap_err();
        assert_eq!(error.to_string(), "recording device removed");
        assert!(!source_geometry_changed(&CaptureError::InvalidTarget(
            "bad crop".into()
        )));
    }
    #[test]
    fn centered_crop_clamps_at_all_edges_and_negative_origins() {
        let bounds = region(-1920, -200, 1920, 1080);
        let nominal = region(0, 0, 801, 601);
        assert_eq!(
            crop(nominal, bounds, -900, 400),
            region(-1300, 100, 801, 601)
        );
        assert_eq!(
            crop(nominal, bounds, -1920, -200),
            region(-1920, -200, 801, 601)
        );
        assert_eq!(crop(nominal, bounds, -1, 879), region(-801, 279, 801, 601));
    }
    #[test]
    fn smaller_monitor_uniformly_shrinks_without_even_expansion() {
        assert_eq!(
            crop(region(0, 0, 1920, 1080), region(0, 0, 1001, 700), 500, 350),
            region(0, 69, 1001, 563)
        );
        assert_eq!(
            crop(region(0, 0, 1080, 1920), region(-1, -1, 1, 1), 0, 0),
            region(-1, -1, 1, 1)
        );
    }
    #[test]
    fn monitor_boundaries_are_half_open() {
        let monitor = Monitor {
            key: MonitorKey {
                adapter_luid: 1,
                output_id: 2,
            },
            bounds: region(-100, 0, 100, 100),
            topology: 1,
        };
        assert!(monitor.contains(-100, 0));
        assert!(!monitor.contains(0, 0));
        assert!(!monitor.contains(-1, 100));
    }
    #[test]
    fn backend_preference_keeps_existing_fallback_order() {
        assert_eq!(
            backend_order(CaptureBackendKind::DxgiDuplication),
            vec![
                CaptureBackendKind::DxgiDuplication,
                CaptureBackendKind::WindowsGraphicsCapture,
                CaptureBackendKind::Gdi
            ]
        );
        assert_eq!(
            backend_order(CaptureBackendKind::Gdi)[0],
            CaptureBackendKind::Gdi
        );
    }

    #[test]
    fn canceled_preparation_cannot_commit_or_start_a_third_source() {
        let now = Instant::now();
        let mut handoff = Handoff::new('A', now);
        assert!(!handoff.observe(Some('B')));
        assert_eq!(handoff.start(now), Some('B'));
        assert!(handoff.observe(Some('A')));
        assert!(!handoff.commit('B'));
        assert_eq!(handoff.active, 'A');
        assert_eq!(handoff.start(now), None);
        handoff.observe(Some('C'));
        assert_eq!(handoff.start(now), None);
        handoff.retired();
        assert_eq!(handoff.start(now), Some('C'));
        assert!(handoff.commit('C'));
        assert_eq!(handoff.active, 'C');
        handoff.observe(Some('B'));
        assert_eq!(handoff.start(now), None);
        handoff.retired();
        assert_eq!(handoff.start(now), Some('B'));
    }

    #[test]
    fn failed_preparation_retains_active_and_obeys_backoff() {
        let now = Instant::now();
        let mut handoff = Handoff::new(1, now);
        handoff.observe(Some(2));
        assert_eq!(handoff.start(now), Some(2));
        handoff.failed(now);
        assert_eq!(handoff.active, 1);
        handoff.retired();
        assert_eq!(handoff.start(now + RETRY_DELAY / 2), None);
        assert_eq!(handoff.start(now + RETRY_DELAY), Some(2));
        assert!(!handoff.commit(3));
        assert!(handoff.commit(2));
    }

    #[test]
    fn topology_change_invalidates_a_ready_old_monitor_generation() {
        let now = Instant::now();
        let mut handoff = Handoff::new(('A', 1u64), now);
        handoff.observe(Some(('B', 1)));
        assert_eq!(handoff.start(now), Some(('B', 1)));
        assert!(handoff.observe(Some(('B', 2))));
        assert!(!handoff.commit(('B', 1)));
        handoff.retired();
        assert_eq!(handoff.start(now), Some(('B', 2)));
        assert!(handoff.commit(('B', 2)));
    }

    #[test]
    fn static_desktop_panning_revises_content_but_duplicates_keep_geometry() {
        let monitor = Monitor {
            key: MonitorKey {
                adapter_luid: 0,
                output_id: 1,
            },
            bounds: region(0, 0, 1920, 1080),
            topology: 1,
        };
        let mut revisions = Revisions::default();
        let mut frame = FrameMetadata {
            is_duplicate: true,
            content_generation: Some(10),
            ..Default::default()
        };
        revisions.apply(&mut frame, monitor, region(10, 10, 321, 181));
        assert!(!frame.is_duplicate());
        assert_eq!(
            (frame.content_generation(), frame.configuration_generation()),
            (Some(1), 1)
        );
        frame.is_duplicate = true;
        frame.content_generation = Some(10);
        revisions.apply(&mut frame, monitor, region(10, 10, 321, 181));
        assert!(frame.is_duplicate());
        assert_eq!(
            (frame.content_generation(), frame.configuration_generation()),
            (Some(1), 1)
        );
        frame.content_generation = Some(10);
        revisions.apply(&mut frame, monitor, region(11, 10, 321, 181));
        assert!(!frame.is_duplicate());
        assert_eq!(
            (frame.content_generation(), frame.configuration_generation()),
            (Some(2), 2)
        );
        frame.is_duplicate = false;
        frame.content_generation = Some(11);
        revisions.apply(&mut frame, monitor, region(11, 10, 321, 181));
        assert_eq!(
            (frame.content_generation(), frame.configuration_generation()),
            (Some(3), 2)
        );
    }

    #[test]
    fn overwritten_changed_observation_still_invalidates_the_admitted_desktop() {
        let monitor = Monitor {
            key: MonitorKey {
                adapter_luid: 0,
                output_id: 1,
            },
            bounds: region(0, 0, 100, 100),
            topology: 1,
        };
        // Exercise both native generations (GPU/readback) and unversioned CPU sources.
        for versioned in [false, true] {
            let (send, queue) = crossbeam_channel::bounded(SOURCE_DEPTH);
            let mut fallback_generation = 0;
            let mut revisions = Revisions::default();
            let observation = |content: u8, duplicate| {
                let mut frame = Frame::from_rgba8(1, 1, vec![content; 4]).unwrap();
                frame.metadata.content_generation = versioned.then_some(u64::from(content) + 10);
                frame.metadata.is_duplicate = duplicate;
                FollowFrame::Cpu(frame)
            };
            publish_source_frame(
                observation(1, false),
                &mut fallback_generation,
                &send,
                &queue,
            );
            let mut first = queue.recv().unwrap().unwrap();
            revisions.apply(first.metadata_mut(), monitor, region(0, 0, 1, 1));
            let first_generation = first.metadata_mut().content_generation;
            publish_source_frame(
                observation(2, false),
                &mut fallback_generation,
                &send,
                &queue,
            );
            publish_source_frame(
                observation(2, true),
                &mut fallback_generation,
                &send,
                &queue,
            );
            let mut latest = queue.recv().unwrap().unwrap();
            assert!(latest.metadata_mut().is_duplicate);
            revisions.apply(latest.metadata_mut(), monitor, region(0, 0, 1, 1));
            assert_ne!(first_generation, latest.metadata_mut().content_generation);
            assert!(!latest.metadata_mut().is_duplicate);
            assert_eq!(latest.metadata_mut().configuration_generation, 1);
            let last_generation = latest.metadata_mut().content_generation;
            let FollowFrame::Cpu(frame) = latest else {
                unreachable!()
            };
            assert_eq!(frame.as_bytes(), &[2; 4]);
            publish_source_frame(
                observation(2, true),
                &mut fallback_generation,
                &send,
                &queue,
            );
            let mut duplicate = queue.recv().unwrap().unwrap();
            revisions.apply(duplicate.metadata_mut(), monitor, region(0, 0, 1, 1));
            assert_eq!(last_generation, duplicate.metadata_mut().content_generation);
            assert!(duplicate.metadata_mut().is_duplicate);
        }
    }

    #[test]
    fn recovery_preserves_terminal_errors_and_escalates_output_device_loss() {
        for error in [
            CaptureError::AccessLost,
            CaptureError::MonitorLost,
            CaptureError::platform(anyhow::anyhow!("source adapter failed")),
        ] {
            assert!(recover_source_error(error, || Ok(())).is_ok());
        }
        for error in [
            CaptureError::PermissionDenied,
            CaptureError::Canceled,
            CaptureError::InvalidConfig("bad source".into()),
        ] {
            let expected = error.to_string();
            assert_eq!(
                recover_source_error(error, || panic!("terminal errors must not retry"))
                    .unwrap_err()
                    .to_string(),
                expected
            );
        }
        let failed_device = || {
            Err(CaptureError::platform(anyhow::anyhow!(
                "recording device removed"
            )))
        };
        for source_failure in [CaptureError::AccessLost, CaptureError::Timeout] {
            let error = recover_source_error(source_failure, failed_device).unwrap_err();
            assert_eq!(error.to_string(), "recording device removed");
            assert!(error.allows_backend_fallback());
        }
    }

    #[test]
    fn source_controls_interrupt_deadlines_without_consuming_capture_slots() {
        let now = Instant::now();
        let control = SourceControl::new(1);
        let mut cadence = AlignedCaptureCadence {
            origin: now,
            fps: 1,
            next: 0,
        };
        control.set_timing(None, 1);
        control.set_paused(false);
        assert!(
            control.changes.is_empty(),
            "unchanged observations must not wake capture"
        );
        control.set_timing(Some(now), 60);
        assert!(cadence.wait_interruptibly(now, |duration| control.wait(duration)));
        assert_eq!(cadence.next, 0);
        assert_eq!(*control.timing.lock().unwrap(), (Some(now), 60));
        control.set_paused(true);
        assert!(cadence.wait_interruptibly(now, |duration| control.wait(duration)));
        assert_eq!(cadence.next, 0);
        control.cancel();
        assert!(control.wait(Duration::from_secs(60)));
        assert!(!control.wait_until_resumed());
    }

    #[test]
    fn paused_source_blocks_until_resume_or_cancellation() {
        for cancel in [false, true] {
            let control = Arc::new(SourceControl::new(60));
            control.set_paused(true);
            let worker_control = control.clone();
            let (started, ready) = crossbeam_channel::bounded(1);
            let (completed, result) = crossbeam_channel::bounded(1);
            let worker = std::thread::spawn(move || {
                started.send(()).unwrap();
                completed.send(worker_control.wait_until_resumed()).unwrap();
            });
            ready.recv().unwrap();
            assert!(result.try_recv().is_err());
            if cancel {
                control.cancel();
            } else {
                control.set_paused(false);
            }
            assert_eq!(
                result.recv_timeout(Duration::from_secs(5)).unwrap(),
                !cancel
            );
            worker.join().unwrap();
        }
    }

    #[test]
    fn ready_destination_older_than_active_is_admitted_in_stream_order() {
        let start = Instant::now();
        let monitor = Monitor {
            key: MonitorKey {
                adapter_luid: 0,
                output_id: 1,
            },
            bounds: region(0, 0, 100, 100),
            topology: 1,
        };
        let mut revisions = Revisions::default();
        let mut active = FrameMetadata::default();
        active.set_timing(Some(start + Duration::from_millis(20)), Some(999));
        revisions.apply_at(
            &mut active,
            monitor,
            region(0, 0, 10, 10),
            start + Duration::from_millis(30),
        );
        let mut destination = FrameMetadata::default();
        destination.set_timing(Some(start + Duration::from_millis(10)), Some(777));
        destination.observation_started_at = Some(start + Duration::from_millis(9));
        revisions.apply_at(
            &mut destination,
            Monitor {
                topology: 2,
                ..monitor
            },
            region(1, 0, 10, 10),
            start + Duration::from_millis(40),
        );
        assert!(
            destination.stream_timestamp().unwrap().instant
                > active.stream_timestamp().unwrap().instant
        );
        assert_eq!(
            destination.stream_timestamp().unwrap().raw_os_ticks,
            Some(777)
        );
        assert_eq!(
            destination.observation_started_at,
            Some(start + Duration::from_millis(9))
        );
    }

    #[test]
    fn duplicate_crop_cache_shares_pixels_but_refreshes_exact_pointer_metadata() {
        let view = region(-20, 10, 3, 1);
        let mut frame = Frame::from_rgba8(3, 1, vec![7; 12]).unwrap();
        frame.metadata.content_generation = Some(9);
        let mut projector = CursorProjector::new();
        let mut shape = None;
        let point = |x| CursorSnapshot {
            absolute_x: x,
            absolute_y: 10,
            visible: true,
            shape: snow_cursor::CursorShapeCapture::Unavailable,
        };
        annotate(
            &mut frame.metadata,
            view,
            point(-20),
            &mut projector,
            &mut shape,
            true,
            Instant::now(),
        )
        .unwrap();
        let key = ViewKey::from_metadata(frame.metadata()).unwrap();
        let cached = FollowFrame::Cpu(frame.clone());
        let mut fresh = frame.metadata().clone();
        fresh.is_duplicate = true;
        annotate(
            &mut fresh,
            view,
            point(-18),
            &mut projector,
            &mut shape,
            true,
            Instant::now(),
        )
        .unwrap();
        assert_eq!(ViewKey::from_metadata(&fresh), Some(key));
        let FollowFrame::Cpu(refreshed) = cached.with_metadata(fresh.clone()) else {
            unreachable!()
        };
        assert!(Arc::ptr_eq(
            &frame.shared_bytes(),
            &refreshed.shared_bytes()
        ));
        assert_eq!(refreshed.metadata().cursor().unwrap().x, 2);
        assert!(refreshed.metadata().is_duplicate());
        annotate(
            &mut fresh,
            region(-19, 10, 3, 1),
            point(-18),
            &mut projector,
            &mut shape,
            true,
            Instant::now(),
        )
        .unwrap();
        assert_ne!(
            ViewKey::from_metadata(&fresh),
            Some(key),
            "same source generation cannot reuse pixels for a different crop"
        );
    }

    #[test]
    fn offscreen_foreign_device_bridge_preserves_odd_crops_and_all_rotations() -> anyhow::Result<()>
    {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory4};
        let factory: IDXGIFactory4 = unsafe { CreateDXGIFactory1()? };
        let adapter = unsafe { factory.EnumWarpAdapter()? };
        let source_device = SharedDevice::create_compute(&adapter)?;
        let output_device = SharedDevice::create_compute(&adapter)?;
        assert!(!source_device.same_device(&output_device));
        let pixels = (0u8..6)
            .flat_map(|y| (0u8..8).flat_map(move |x| [x * 10, y * 20, x ^ y, 255]))
            .collect::<Vec<_>>();
        let texture = source_device.texture(8, 6, DXGI_FORMAT_B8G8R8A8_UNORM, 0)?;
        {
            let _lock = source_device.lock();
            unsafe {
                source_device.context().UpdateSubresource(
                    texture.raw(),
                    0,
                    None,
                    pixels.as_ptr().cast(),
                    8 * 4,
                    0,
                );
            }
        }
        for quarter_turns in 0..4 {
            let size = if quarter_turns % 2 == 0 {
                (5, 3)
            } else {
                (3, 5)
            };
            let frame = GpuCapturedFrame::from_layers(
                vec![VideoLayer {
                    texture: texture.clone(),
                    source: Rect {
                        x: 1,
                        y: 1,
                        width: 5,
                        height: 3,
                    },
                    destination: Rect::full(size),
                    rotation: quarter_turns,
                    alpha: false,
                }],
                size,
                FrameMetadata::default(),
            )?;
            let mut download = Readback::new(source_device.clone());
            download.submit(&frame)?;
            let poll = |download: &mut Readback| -> CaptureResult<Frame> {
                let deadline = Instant::now() + Duration::from_secs(5);
                loop {
                    if let Some(frame) = download.poll(None, CapturePixelFormat::Bgra8)? {
                        return Ok(frame);
                    }
                    if Instant::now() >= deadline {
                        return Err(CaptureError::Timeout);
                    }
                    std::thread::sleep(Duration::from_millis(1));
                }
            };
            let downloaded = poll(&mut download)?;
            assert_eq!(downloaded.dimensions(), size);
            let mut upload = Upload::new(output_device.clone());
            let transferred = upload.frame(&downloaded)?.expect("fresh upload pool");
            assert!(
                transferred.layers[0]
                    .texture
                    .device()
                    .same_device(&output_device)
            );
            let mut verify = Readback::new(output_device.clone());
            verify.submit(&transferred)?;
            let verified = poll(&mut verify)?;
            assert_eq!(verified.as_bytes(), downloaded.as_bytes());
            for y in 0..size.1 {
                for x in 0..size.0 {
                    let (sx, sy) = match quarter_turns {
                        0 => (x, y),
                        1 => (y, 2 - x),
                        2 => (4 - x, 2 - y),
                        _ => (4 - y, x),
                    };
                    let source = ((sy + 1) * 8 + sx + 1) as usize * 4;
                    let output = (y * size.0 + x) as usize * 4;
                    assert_eq!(
                        &downloaded.as_bytes()[output..output + 4],
                        &pixels[source..source + 4]
                    );
                }
            }
        }
        Ok(())
    }

    proptest::proptest! {
        #[test]
        fn every_fitted_crop_is_within_one_monitor(
            nominal_width in 1u32..20000, nominal_height in 1u32..20000,
            monitor_width in 1u32..10000, monitor_height in 1u32..10000,
            left in -30000i32..30000, top in -30000i32..30000,
            x in -100000i32..100000, y in -100000i32..100000,
        ) {
            let bounds = region(left, top, monitor_width, monitor_height);
            let view = crop(region(0, 0, nominal_width, nominal_height), bounds, x, y);
            proptest::prop_assert!(view.width > 0 && view.height > 0);
            proptest::prop_assert!(view.x >= left && view.y >= top);
            proptest::prop_assert!(i64::from(view.x) + i64::from(view.width) <= i64::from(left) + i64::from(monitor_width));
            proptest::prop_assert!(i64::from(view.y) + i64::from(view.height) <= i64::from(top) + i64::from(monitor_height));
            proptest::prop_assert!(view.width <= nominal_width && view.height <= nominal_height);
        }
    }
}

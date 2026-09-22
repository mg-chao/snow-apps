//! Asynchronous selector boundary. Each worker owns all of its native state.
use snow_ui_selector::{
    AccessibilityBackend, ElementRegionService, HitTestMode, Point, QueryControl, QueryResult,
    SelectorResult, StopReason, WindowSnapshot,
};
use std::collections::VecDeque;
use std::ffi::c_void;
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, AtomicU64, Ordering},
    mpsc,
};
use std::thread;
use std::time::Instant;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowUiSelectorBackend {
    Uia,
    Msaa,
    Accessibility,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowUiSelectorHitTestMode {
    UiElement,
    Window,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowUiSelectorPhase {
    Initial,
    Refinement,
    Finished,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SnowUiSelectorRect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SnowUiSelectorQuery {
    pub epoch: u64,
    pub request_id: u64,
    pub generation: u64,
    pub x: i32,
    pub y: i32,
    pub mode: SnowUiSelectorHitTestMode,
    pub display_id: u32,
}
#[repr(C)]
pub struct SnowUiSelectorEvent {
    pub query: SnowUiSelectorQuery,
    pub phase: SnowUiSelectorPhase,
    pub reason: StopReason,
    pub ok: u8,
    pub elapsed_us: u64,
    pub rects: *const SnowUiSelectorRect,
    pub count: usize,
}
pub type EventCallback = unsafe extern "C" fn(*const SnowUiSelectorEvent, *mut c_void);
pub type RefreshCallback = unsafe extern "C" fn(u64, u8, *mut c_void);
struct Sink {
    event: EventCallback,
    refresh: RefreshCallback,
    userdata: usize,
}
struct Shared {
    sink: Mutex<Option<Sink>>,
    closed: AtomicBool,
    revision: AtomicU64,
    snapshot: Mutex<Option<(u64, u64, WindowSnapshot)>>,
    cache_revision: AtomicU64,
}
impl Shared {
    fn emit(
        &self,
        query: SnowUiSelectorQuery,
        phase: SnowUiSelectorPhase,
        result: QueryResult,
        ok: bool,
        started: Instant,
    ) {
        let rects: Vec<_> = result
            .path
            .unwrap_or_default()
            .iter()
            .map(|r| SnowUiSelectorRect {
                left: r.left(),
                top: r.top(),
                right: r.right(),
                bottom: r.bottom(),
            })
            .collect();
        let event = SnowUiSelectorEvent {
            query,
            phase,
            reason: result.reason,
            ok: u8::from(ok),
            elapsed_us: started.elapsed().as_micros().min(u128::from(u64::MAX)) as u64,
            rects: rects.as_ptr(),
            count: rects.len(),
        };
        // The sink lock is a callback-close barrier, never held across a provider call.
        // Callbacks must copy/queue only, and must not re-enter the service.
        if let Some(sink) = self.sink.lock().unwrap().as_ref() {
            unsafe { (sink.event)(&event, sink.userdata as *mut c_void) };
        }
    }
    fn cancelled(&self, query: SnowUiSelectorQuery) {
        self.emit(
            query,
            SnowUiSelectorPhase::Finished,
            empty(StopReason::Cancelled),
            true,
            Instant::now(),
        );
    }
}
fn empty(reason: StopReason) -> QueryResult {
    QueryResult { path: None, reason }
}
fn backend(value: SnowUiSelectorBackend) -> AccessibilityBackend {
    #[cfg(target_os = "macos")]
    {
        let _ = value;
        AccessibilityBackend::Accessibility
    }
    #[cfg(not(target_os = "macos"))]
    match value {
        SnowUiSelectorBackend::Uia => AccessibilityBackend::Uia,
        SnowUiSelectorBackend::Msaa => AccessibilityBackend::Msaa,
        SnowUiSelectorBackend::Accessibility => AccessibilityBackend::Accessibility,
    }
}
fn mode(value: SnowUiSelectorHitTestMode) -> HitTestMode {
    match value {
        SnowUiSelectorHitTestMode::UiElement => HitTestMode::UiElement,
        SnowUiSelectorHitTestMode::Window => HitTestMode::Window,
    }
}

mod display_layout;
pub use display_layout::*;

enum ForegroundCommand {
    Refresh {
        epoch: u64,
        backend: SnowUiSelectorBackend,
        excluded: Vec<usize>,
        displays: Option<Vec<snow_ui_selector::DisplayGeometry>>,
        revision: u64,
    },
    Query(SnowUiSelectorQuery),
    Release,
    Shutdown,
}
enum RefinementCommand {
    Query(SnowUiSelectorQuery, u64),
    Release,
}
struct ForegroundQueue {
    pending: Mutex<VecDeque<ForegroundCommand>>,
    wake: mpsc::SyncSender<()>,
}
impl ForegroundQueue {
    fn submit(&self, command: ForegroundCommand, shared: &Shared) -> bool {
        let removed = {
            let mut pending = self.pending.lock().unwrap();
            let removed = if matches!(command, ForegroundCommand::Query(_)) {
                if matches!(pending.back(), Some(ForegroundCommand::Query(_))) {
                    pending.pop_back().into_iter().collect::<Vec<_>>()
                } else {
                    Vec::new()
                }
            } else {
                pending.drain(..).collect()
            };
            pending.push_back(command);
            removed
        };
        // Every accepted request still receives a terminal callback when coalesced.
        for command in removed {
            match command {
                ForegroundCommand::Query(query) => shared.emit(
                    query,
                    SnowUiSelectorPhase::Initial,
                    empty(StopReason::Cancelled),
                    false,
                    Instant::now(),
                ),
                ForegroundCommand::Refresh { epoch, .. } => {
                    if let Some(sink) = shared.sink.lock().unwrap().as_ref() {
                        unsafe { (sink.refresh)(epoch, 0, sink.userdata as *mut c_void) };
                    }
                }
                _ => {}
            }
        }
        !matches!(
            self.wake.try_send(()),
            Err(mpsc::TrySendError::Disconnected(_))
        )
    }
}
struct RefinementQueue {
    pending: Mutex<Option<RefinementCommand>>,
    wake: mpsc::SyncSender<()>,
}
impl RefinementQueue {
    fn replace(&self, command: RefinementCommand, shared: &Shared) {
        let previous = self.pending.lock().unwrap().replace(command);
        if let Some(RefinementCommand::Query(query, _)) = previous {
            shared.cancelled(query);
        }
        let _ = self.wake.try_send(());
    }
}
pub struct SnowUiSelectorServiceImpl {
    foreground: Arc<ForegroundQueue>,
    refinement: Arc<RefinementQueue>,
    shared: Arc<Shared>,
    workers: Vec<thread::JoinHandle<()>>,
    start_refinement: Option<Box<dyn FnOnce() -> std::io::Result<thread::JoinHandle<()>> + Send>>,
}

trait WorkerService: Sized {
    fn create_with_displays(
        backend: AccessibilityBackend,
        excluded: &[usize],
        displays: Option<&[snow_ui_selector::DisplayGeometry]>,
    ) -> SelectorResult<Self> {
        let _ = displays;
        Self::create(backend, excluded)
    }
    fn refresh_with_displays(
        &mut self,
        excluded: &[usize],
        displays: Option<&[snow_ui_selector::DisplayGeometry]>,
    ) -> SelectorResult<()> {
        let _ = displays;
        self.refresh(excluded)
    }

    fn create(backend: AccessibilityBackend, excluded: &[usize]) -> SelectorResult<Self>;
    fn backend(&self) -> AccessibilityBackend;
    fn refresh(&mut self, excluded: &[usize]) -> SelectorResult<()>;
    fn snapshot(&self) -> Option<WindowSnapshot>;
    fn from_snapshot(snapshot: &WindowSnapshot) -> SelectorResult<Self>;
    fn release(&mut self);
    fn query(
        &mut self,
        point: Point,
        mode: HitTestMode,
        control: &QueryControl<'_>,
        progress: &mut dyn FnMut(&[snow_ui_selector::ElementRect]),
    ) -> SelectorResult<QueryResult>;
}
impl WorkerService for ElementRegionService {
    fn create_with_displays(
        backend: AccessibilityBackend,
        excluded: &[usize],
        displays: Option<&[snow_ui_selector::DisplayGeometry]>,
    ) -> SelectorResult<Self> {
        Self::with_backend_and_displays(backend, excluded, displays)
    }
    fn refresh_with_displays(
        &mut self,
        excluded: &[usize],
        displays: Option<&[snow_ui_selector::DisplayGeometry]>,
    ) -> SelectorResult<()> {
        ElementRegionService::refresh_with_displays(self, excluded, displays)
    }

    fn create(backend: AccessibilityBackend, excluded: &[usize]) -> SelectorResult<Self> {
        Self::with_backend_excluding_ids(backend, excluded)
    }
    fn backend(&self) -> AccessibilityBackend {
        self.backend()
    }
    fn refresh(&mut self, excluded: &[usize]) -> SelectorResult<()> {
        self.refresh_excluding_ids(excluded)
    }
    fn snapshot(&self) -> Option<WindowSnapshot> {
        self.window_snapshot()
    }
    fn from_snapshot(snapshot: &WindowSnapshot) -> SelectorResult<Self> {
        Self::from_snapshot(snapshot)
    }
    fn release(&mut self) {
        self.release_cache();
    }
    fn query(
        &mut self,
        point: Point,
        mode: HitTestMode,
        control: &QueryControl<'_>,
        progress: &mut dyn FnMut(&[snow_ui_selector::ElementRect]),
    ) -> SelectorResult<QueryResult> {
        self.query(point, mode, control, progress)
    }
}

fn foreground_worker<S: WorkerService>(
    receiver: mpsc::Receiver<()>,
    queue: Arc<ForegroundQueue>,
    shared: Arc<Shared>,
) {
    let mut service: Option<S> = None;
    let mut current_epoch = 0;
    loop {
        let command = queue.pending.lock().unwrap().pop_front();
        let Some(command) = command else {
            if receiver.recv().is_err() {
                break;
            }
            continue;
        };
        if shared.closed.load(Ordering::Acquire) {
            break;
        }
        match command {
            ForegroundCommand::Refresh {
                epoch,
                backend: selected,
                excluded,
                displays,
                revision,
            } => {
                let selected = backend(selected);
                let result =
                    if let Some(service) = service.as_mut().filter(|s| s.backend() == selected) {
                        service.refresh_with_displays(&excluded, displays.as_deref())
                    } else {
                        S::create_with_displays(selected, &excluded, displays.as_deref())
                            .map(|s| service = Some(s))
                    };
                if result.is_err() {
                    service = None;
                }
                current_epoch = epoch;
                let mut snapshot = shared.snapshot.lock().unwrap();
                if shared.cache_revision.load(Ordering::Acquire) == revision {
                    *snapshot = service
                        .as_ref()
                        .and_then(|s| s.snapshot())
                        .map(|s| (epoch, revision, s));
                }
                drop(snapshot);
                if let Some(sink) = shared.sink.lock().unwrap().as_ref() {
                    unsafe {
                        (sink.refresh)(
                            epoch,
                            u8::from(result.is_ok()),
                            sink.userdata as *mut c_void,
                        )
                    };
                }
            }
            ForegroundCommand::Query(query) => {
                let started = Instant::now();
                let revision = shared.cache_revision.load(Ordering::Acquire);
                let cancelled = || {
                    shared.closed.load(Ordering::Acquire)
                        || shared.cache_revision.load(Ordering::Acquire) != revision
                };
                let result = service
                    .as_mut()
                    .filter(|_| current_epoch == query.epoch)
                    .map(|s| {
                        s.query(
                            Point {
                                x: query.x,
                                y: query.y,
                                display_id: query.display_id,
                            },
                            mode(query.mode),
                            &QueryControl::foreground_with_cancellation(&cancelled),
                            &mut |_| {},
                        )
                    });
                let (result, ok) = match result {
                    Some(Ok(r)) => (r, true),
                    _ => (empty(StopReason::ProviderFailure), false),
                };
                shared.emit(query, SnowUiSelectorPhase::Initial, result, ok, started);
            }
            ForegroundCommand::Release => {
                if let Some(s) = &mut service {
                    s.release();
                }
                current_epoch = 0;
                *shared.snapshot.lock().unwrap() = None;
            }
            ForegroundCommand::Shutdown => break,
        }
    }
}

fn refinement_worker<S: WorkerService>(
    receiver: mpsc::Receiver<()>,
    queue: Arc<RefinementQueue>,
    shared: Arc<Shared>,
) {
    let mut service: Option<S> = None;
    let mut current_snapshot = None;
    while receiver.recv().is_ok() {
        if shared.closed.load(Ordering::Acquire) {
            break;
        }
        let command = queue.pending.lock().unwrap().take();
        match command {
            Some(RefinementCommand::Release) => {
                service = None;
                current_snapshot = None;
            }
            Some(RefinementCommand::Query(query, revision)) => {
                let started = Instant::now();
                let cancelled = || {
                    shared.closed.load(Ordering::Acquire)
                        || shared.revision.load(Ordering::Acquire) != revision
                };
                if cancelled() {
                    shared.cancelled(query);
                    continue;
                }
                let snapshot = shared
                    .snapshot
                    .lock()
                    .unwrap()
                    .as_ref()
                    .filter(|(e, _, _)| *e == query.epoch)
                    .cloned();
                let key = snapshot.as_ref().map(|(e, r, _)| (*e, *r));
                if current_snapshot != key || service.is_none() {
                    service = snapshot
                        .as_ref()
                        .and_then(|(_, _, s)| S::from_snapshot(s).ok());
                    current_snapshot = key;
                }
                let result = service.as_mut().map(|s| {
                    s.query(
                        Point {
                            x: query.x,
                            y: query.y,
                            display_id: query.display_id,
                        },
                        mode(query.mode),
                        &QueryControl::refinement(&cancelled),
                        &mut |path| {
                            if !cancelled() {
                                shared.emit(
                                    query,
                                    SnowUiSelectorPhase::Refinement,
                                    QueryResult {
                                        path: Some(path.to_vec()),
                                        reason: StopReason::DecodingPending,
                                    },
                                    true,
                                    started,
                                );
                            }
                        },
                    )
                });
                let (result, ok) = if cancelled() {
                    (empty(StopReason::Cancelled), true)
                } else {
                    match result {
                        Some(Ok(r)) => (r, true),
                        _ => (empty(StopReason::ProviderFailure), false),
                    }
                };
                // Finished carries the last path, so terminal delivery is not throttled.
                shared.emit(query, SnowUiSelectorPhase::Finished, result, ok, started);
            }
            None => {}
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_ui_selector_service_create(
    event: Option<EventCallback>,
    refresh: Option<RefreshCallback>,
    userdata: *mut c_void,
) -> *mut SnowUiSelectorServiceImpl {
    start_service::<ElementRegionService>(event, refresh, userdata)
}

fn start_service<S: WorkerService + 'static>(
    event: Option<EventCallback>,
    refresh: Option<RefreshCallback>,
    userdata: *mut c_void,
) -> *mut SnowUiSelectorServiceImpl {
    let (Some(event), Some(refresh)) = (event, refresh) else {
        return std::ptr::null_mut();
    };
    let shared = Arc::new(Shared {
        sink: Mutex::new(Some(Sink {
            event,
            refresh,
            userdata: userdata as usize,
        })),
        closed: AtomicBool::new(false),
        revision: AtomicU64::new(0),
        snapshot: Mutex::new(None),
        cache_revision: AtomicU64::new(0),
    });
    let (sender, receiver) = mpsc::sync_channel(1);
    let foreground_queue = Arc::new(ForegroundQueue {
        pending: Mutex::new(VecDeque::new()),
        wake: sender,
    });
    let (wake, refine_receiver) = mpsc::sync_channel(1);
    let refinement = Arc::new(RefinementQueue {
        pending: Mutex::new(None),
        wake,
    });
    let foreground = {
        let shared = shared.clone();
        let queue = foreground_queue.clone();
        thread::Builder::new()
            .name("selector-foreground".into())
            .spawn(move || foreground_worker::<S>(receiver, queue, shared))
    };
    let Ok(foreground) = foreground else {
        return std::ptr::null_mut();
    };
    // Creating a foreground service must not wait for a second OS thread.
    // Start refinement on first admission, then retain that worker across captures.
    let start_refinement = {
        let shared = shared.clone();
        let queue = refinement.clone();
        Box::new(move || {
            thread::Builder::new()
                .name("selector-refinement".into())
                .spawn(move || refinement_worker::<S>(refine_receiver, queue, shared))
        })
    };
    Box::into_raw(Box::new(SnowUiSelectorServiceImpl {
        foreground: foreground_queue,
        refinement,
        shared,
        workers: vec![foreground],
        start_refinement: Some(start_refinement),
    }))
}

#[unsafe(no_mangle)]
/// # Safety
/// The service must be null or a live returned handle, used exclusively by its owner.
/// Callbacks must not re-enter this service. No callbacks run after this function returns.
pub unsafe extern "C" fn snow_ui_selector_service_destroy(service: *mut SnowUiSelectorServiceImpl) {
    if service.is_null() {
        return;
    }
    let service = unsafe { Box::from_raw(service) };
    service.shared.closed.store(true, Ordering::Release);
    service.shared.sink.lock().unwrap().take();
    let _ = service
        .foreground
        .submit(ForegroundCommand::Shutdown, &service.shared);
    let _ = service.refinement.wake.try_send(());
    // Joining never holds up Qt or waits for a provider on the calling thread.
    let _ = thread::Builder::new()
        .name("selector-cleanup".into())
        .spawn(move || {
            for worker in service.workers {
                let _ = worker.join();
            }
        });
}

#[unsafe(no_mangle)]
/// # Safety
/// `service` must be a live handle or null. Called by the service owner.
pub unsafe extern "C" fn snow_ui_selector_service_invalidate_refinement(
    service: *mut SnowUiSelectorServiceImpl,
) {
    if let Some(service) = unsafe { service.as_ref() } {
        service.shared.revision.fetch_add(1, Ordering::AcqRel);
        let removed = {
            let mut pending = service.refinement.pending.lock().unwrap();
            if matches!(*pending, Some(RefinementCommand::Query(_, _))) {
                pending.take()
            } else {
                None
            }
        };
        if let Some(RefinementCommand::Query(query, _)) = removed {
            service.shared.cancelled(query);
        }
    }
}
#[unsafe(no_mangle)]
/// # Safety
/// `service` must be a live handle or null. Called by the service owner.
pub unsafe extern "C" fn snow_ui_selector_service_release_cache(
    service: *mut SnowUiSelectorServiceImpl,
) -> u8 {
    let Some(s) = (unsafe { service.as_ref() }) else {
        return 0;
    };
    unsafe { snow_ui_selector_service_invalidate_refinement(service) };
    s.refinement.replace(RefinementCommand::Release, &s.shared);
    s.shared.cache_revision.fetch_add(1, Ordering::AcqRel);
    *s.shared.snapshot.lock().unwrap() = None;
    u8::from(s.foreground.submit(ForegroundCommand::Release, &s.shared))
}
#[unsafe(no_mangle)]
/// # Safety
/// `service` must be live; `excluded` must reference `count` entries when count is nonzero.
pub unsafe extern "C" fn snow_ui_selector_service_refresh(
    service: *mut SnowUiSelectorServiceImpl,
    epoch: u64,
    backend: SnowUiSelectorBackend,
    excluded: *const usize,
    count: usize,
) -> u8 {
    unsafe { refresh_with_layout(service, epoch, backend, excluded, count, None) }
}

unsafe fn refresh_with_layout(
    service: *mut SnowUiSelectorServiceImpl,
    epoch: u64,
    backend: SnowUiSelectorBackend,
    excluded: *const usize,
    count: usize,
    displays: Option<Vec<snow_ui_selector::DisplayGeometry>>,
) -> u8 {
    let Some(s) = (unsafe { service.as_ref() }) else {
        return 0;
    };
    if count > 4096 || (count > 0 && excluded.is_null()) {
        return 0;
    }
    let excluded = if count == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(excluded, count) }.to_vec()
    };
    unsafe { snow_ui_selector_service_invalidate_refinement(service) };
    s.refinement.replace(RefinementCommand::Release, &s.shared);
    let revision = s.shared.cache_revision.fetch_add(1, Ordering::AcqRel) + 1;
    *s.shared.snapshot.lock().unwrap() = None;
    u8::from(s.foreground.submit(
        ForegroundCommand::Refresh {
            epoch,
            backend,
            excluded,
            displays,
            revision,
        },
        &s.shared,
    ))
}
#[unsafe(no_mangle)]
/// # Safety
/// Both pointers must be live or null. Query data is copied before return.
pub unsafe extern "C" fn snow_ui_selector_service_query(
    service: *mut SnowUiSelectorServiceImpl,
    query: *const SnowUiSelectorQuery,
) -> u8 {
    let (Some(s), Some(query)) = (unsafe { service.as_ref() }, unsafe { query.as_ref() }) else {
        return 0;
    };
    u8::from(
        s.foreground
            .submit(ForegroundCommand::Query(*query), &s.shared),
    )
}
#[unsafe(no_mangle)]
/// # Safety
/// Both pointers must be live or null. Query data is copied before return.
pub unsafe extern "C" fn snow_ui_selector_service_refine(
    service: *mut SnowUiSelectorServiceImpl,
    query: *const SnowUiSelectorQuery,
) -> u8 {
    let (Some(s), Some(query)) = (unsafe { service.as_mut() }, unsafe { query.as_ref() }) else {
        return 0;
    };
    if s.shared.closed.load(Ordering::Acquire) || query.mode != SnowUiSelectorHitTestMode::UiElement
    {
        return 0;
    }
    if !s
        .shared
        .snapshot
        .lock()
        .unwrap()
        .as_ref()
        .is_some_and(|(epoch, _, _)| *epoch == query.epoch)
    {
        return 0;
    }
    if let Some(start) = s.start_refinement.take() {
        let Ok(worker) = start() else {
            return 0;
        };
        s.workers.push(worker);
    }
    if s.workers.len() != 2 {
        return 0;
    }
    s.refinement.replace(
        RefinementCommand::Query(*query, s.shared.revision.load(Ordering::Acquire)),
        &s.shared,
    );
    1
}

#[cfg(test)]
mod tests;

/// Queries permission without a prompt unless explicitly requested by the user.
#[unsafe(no_mangle)]
pub extern "C" fn snow_ui_selector_accessibility_permission(prompt: u8) -> u8 {
    #[cfg(target_os = "macos")]
    {
        u8::from(snow_ui_selector::accessibility_permission(prompt != 0))
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = prompt;
        1
    }
}

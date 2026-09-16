//! Asynchronous selector boundary. Each worker owns all of its COM state.
use snow_ui_selector::{
    AccessibilityBackend, ElementRegionService, HitTestMode, QueryControl, QueryResult, StopReason,
    WindowSnapshot,
};
use std::ffi::c_void;
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, AtomicU64, Ordering},
    mpsc,
};
use std::thread;
use std::time::Instant;
use windows::Win32::Foundation::{HWND, POINT};

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowUiSelectorBackend {
    Uia,
    Msaa,
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
    snapshot: Mutex<Option<(u64, WindowSnapshot)>>,
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
    match value {
        SnowUiSelectorBackend::Uia => AccessibilityBackend::Uia,
        SnowUiSelectorBackend::Msaa => AccessibilityBackend::Msaa,
    }
}
fn mode(value: SnowUiSelectorHitTestMode) -> HitTestMode {
    match value {
        SnowUiSelectorHitTestMode::UiElement => HitTestMode::UiElement,
        SnowUiSelectorHitTestMode::Window => HitTestMode::Window,
    }
}

enum ForegroundCommand {
    Refresh {
        epoch: u64,
        backend: SnowUiSelectorBackend,
        excluded: Vec<isize>,
    },
    Query(SnowUiSelectorQuery),
    Release,
    Shutdown,
}
enum RefinementCommand {
    Query(SnowUiSelectorQuery, u64),
    Release,
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
    foreground: mpsc::Sender<ForegroundCommand>,
    refinement: Arc<RefinementQueue>,
    shared: Arc<Shared>,
    workers: Vec<thread::JoinHandle<()>>,
    start_refinement: Option<Box<dyn FnOnce() -> std::io::Result<thread::JoinHandle<()>> + Send>>,
}

trait WorkerService: Sized {
    fn create(backend: AccessibilityBackend, excluded: &[HWND]) -> windows::core::Result<Self>;
    fn backend(&self) -> AccessibilityBackend;
    fn refresh(&mut self, excluded: &[HWND]) -> windows::core::Result<()>;
    fn snapshot(&self) -> Option<WindowSnapshot>;
    fn from_snapshot(snapshot: &WindowSnapshot) -> windows::core::Result<Self>;
    fn release(&mut self);
    fn query(
        &mut self,
        point: POINT,
        mode: HitTestMode,
        control: &QueryControl<'_>,
        progress: &mut dyn FnMut(&[snow_ui_selector::ElementRect]),
    ) -> windows::core::Result<QueryResult>;
}
impl WorkerService for ElementRegionService {
    fn create(backend: AccessibilityBackend, excluded: &[HWND]) -> windows::core::Result<Self> {
        Self::with_backend_excluding_hwnds(backend, excluded)
    }
    fn backend(&self) -> AccessibilityBackend {
        self.backend()
    }
    fn refresh(&mut self, excluded: &[HWND]) -> windows::core::Result<()> {
        self.refresh_excluding_hwnds(excluded)
    }
    fn snapshot(&self) -> Option<WindowSnapshot> {
        self.window_snapshot()
    }
    fn from_snapshot(snapshot: &WindowSnapshot) -> windows::core::Result<Self> {
        Self::from_snapshot(snapshot)
    }
    fn release(&mut self) {
        self.release_cache();
    }
    fn query(
        &mut self,
        point: POINT,
        mode: HitTestMode,
        control: &QueryControl<'_>,
        progress: &mut dyn FnMut(&[snow_ui_selector::ElementRect]),
    ) -> windows::core::Result<QueryResult> {
        self.query(point, mode, control, progress)
    }
}

fn foreground_worker<S: WorkerService>(
    receiver: mpsc::Receiver<ForegroundCommand>,
    shared: Arc<Shared>,
) {
    let mut service: Option<S> = None;
    let mut current_epoch = 0;
    while let Ok(command) = receiver.recv() {
        if shared.closed.load(Ordering::Acquire) {
            break;
        }
        match command {
            ForegroundCommand::Refresh {
                epoch,
                backend: selected,
                excluded,
            } => {
                let hwnds: Vec<_> = excluded.into_iter().map(|h| HWND(h as *mut _)).collect();
                let selected = backend(selected);
                let result =
                    if let Some(service) = service.as_mut().filter(|s| s.backend() == selected) {
                        service.refresh(&hwnds)
                    } else {
                        S::create(selected, &hwnds).map(|s| service = Some(s))
                    };
                if result.is_err() {
                    service = None;
                }
                current_epoch = epoch;
                *shared.snapshot.lock().unwrap() = service
                    .as_ref()
                    .and_then(|s| s.snapshot())
                    .map(|s| (epoch, s));
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
                let result = service
                    .as_mut()
                    .filter(|_| current_epoch == query.epoch)
                    .map(|s| {
                        s.query(
                            POINT {
                                x: query.x,
                                y: query.y,
                            },
                            mode(query.mode),
                            &QueryControl::foreground(),
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
    let mut current_epoch = 0;
    while receiver.recv().is_ok() {
        if shared.closed.load(Ordering::Acquire) {
            break;
        }
        let command = queue.pending.lock().unwrap().take();
        match command {
            Some(RefinementCommand::Release) => {
                service = None;
                current_epoch = 0;
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
                if current_epoch != query.epoch {
                    let snapshot = shared
                        .snapshot
                        .lock()
                        .unwrap()
                        .as_ref()
                        .filter(|(e, _)| *e == query.epoch)
                        .map(|(_, s)| s.clone());
                    service = snapshot.as_ref().and_then(|s| S::from_snapshot(s).ok());
                    current_epoch = query.epoch;
                }
                let result = service.as_mut().map(|s| {
                    s.query(
                        POINT {
                            x: query.x,
                            y: query.y,
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
    });
    let (sender, receiver) = mpsc::channel();
    let (wake, refine_receiver) = mpsc::sync_channel(1);
    let refinement = Arc::new(RefinementQueue {
        pending: Mutex::new(None),
        wake,
    });
    let foreground = {
        let shared = shared.clone();
        thread::Builder::new()
            .name("selector-foreground".into())
            .spawn(move || foreground_worker::<S>(receiver, shared))
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
        foreground: sender,
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
    let _ = service.foreground.send(ForegroundCommand::Shutdown);
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
    u8::from(s.foreground.send(ForegroundCommand::Release).is_ok())
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
    let Some(s) = (unsafe { service.as_ref() }) else {
        return 0;
    };
    if count > 0 && excluded.is_null() {
        return 0;
    }
    let excluded = if count == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(excluded, count) }
            .iter()
            .map(|&h| h as isize)
            .collect()
    };
    unsafe { snow_ui_selector_service_invalidate_refinement(service) };
    s.refinement.replace(RefinementCommand::Release, &s.shared);
    u8::from(
        s.foreground
            .send(ForegroundCommand::Refresh {
                epoch,
                backend,
                excluded,
            })
            .is_ok(),
    )
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
    u8::from(s.foreground.send(ForegroundCommand::Query(*query)).is_ok())
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
        .is_some_and(|(epoch, _)| *epoch == query.epoch)
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

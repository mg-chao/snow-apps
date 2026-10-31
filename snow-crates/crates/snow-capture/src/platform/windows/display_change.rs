use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, mpsc};
use std::time::{Duration, Instant};

use anyhow::Context;
use windows::Win32::Foundation::{
    CloseHandle, ERROR_CLASS_ALREADY_EXISTS, GetLastError, HANDLE, HWND, LPARAM, LRESULT, RECT,
    WAIT_FAILED, WAIT_OBJECT_0, WPARAM,
};
use windows::Win32::Graphics::Dxgi::Common::DXGI_MODE_ROTATION;
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::Threading::{CreateEventW, INFINITE, SetEvent};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, MSG, MWMO_INPUTAVAILABLE,
    MsgWaitForMultipleObjectsEx, PM_REMOVE, PeekMessageW, QS_ALLINPUT, RegisterClassW,
    TranslateMessage, WM_CLOSE, WM_DISPLAYCHANGE, WM_QUIT, WNDCLASSW, WS_OVERLAPPED,
};

use crate::error::{CaptureError, CaptureResult, DisplayRefreshRetry};
use crate::monitor::{MonitorId, MonitorKey};

use super::monitor::{HdrMonitorMetadata, ResolvedMonitor};

// Bound topology staleness during active capture without polling while idle. Failed
// enumeration has its own cooldown so a 60-fps caller cannot hammer display APIs.
const FALLBACK_FRESHNESS: Duration = Duration::from_secs(1);
const REFRESH_RETRY_INTERVAL: Duration = Duration::from_secs(1);

/// Classify failures in display enumeration, without making unrelated platform
/// errors retryable or discarding the HRESULT and operation context.
fn is_transient_display_failure(error: &CaptureError) -> bool {
    use windows::Win32::Graphics::Dxgi::{
        DXGI_ERROR_ACCESS_LOST, DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET,
        DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE,
        DXGI_ERROR_SESSION_DISCONNECTED, DXGI_ERROR_WAIT_TIMEOUT,
    };

    match error {
        CaptureError::Platform(cause) => {
            cause
                .downcast_ref::<windows::core::Error>()
                .is_some_and(|error| {
                    matches!(
                        error.code(),
                        DXGI_ERROR_ACCESS_LOST
                            | DXGI_ERROR_DEVICE_REMOVED
                            | DXGI_ERROR_DEVICE_RESET
                            | DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
                            | DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE
                            | DXGI_ERROR_SESSION_DISCONNECTED
                            | DXGI_ERROR_WAIT_TIMEOUT
                    )
                })
        }
        _ => error.is_retryable(),
    }
}

struct DisplayRefreshFailure {
    generation: u64,
    retry_at: Instant,
    error: CaptureError,
}

impl DisplayRefreshFailure {
    fn new(generation: u64, cause: CaptureError, now: Instant) -> Self {
        let retry_at = now + REFRESH_RETRY_INTERVAL;
        let error = if is_transient_display_failure(&cause) {
            CaptureError::DisplayRefreshPending(Arc::new(DisplayRefreshRetry::new(cause, retry_at)))
        } else {
            cause
        };
        Self {
            generation,
            retry_at,
            error,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
struct DisplayMonitorConfiguration {
    key: MonitorKey,
    name: String,
    handle: isize,
    desktop_coordinates: RECT,
    rotation: DXGI_MODE_ROTATION,
    hdr_metadata: HdrMonitorMetadata,
    is_primary: bool,
}

#[derive(Default)]
struct DisplaySnapshot {
    monitors: Vec<MonitorId>,
    resolved: Vec<ResolvedMonitor>,
    configuration: Vec<DisplayMonitorConfiguration>,
}

impl DisplaySnapshot {
    fn enumerate() -> CaptureResult<Self> {
        let resolved = super::monitor::enumerate_resolved()?;
        let monitors = super::monitor::to_monitor_ids(&resolved);
        let configuration = resolved
            .iter()
            .zip(&monitors)
            .map(|(resolved, monitor)| DisplayMonitorConfiguration {
                key: resolved.key,
                name: resolved.name.clone(),
                handle: resolved.handle.0 as isize,
                desktop_coordinates: resolved.desktop_coordinates,
                rotation: resolved.rotation,
                hdr_metadata: resolved.hdr_metadata,
                is_primary: monitor.is_primary(),
            })
            .collect();
        Ok(Self {
            monitors,
            resolved,
            configuration,
        })
    }

    fn same_configuration(&self, other: &Self) -> bool {
        // DXGI enumeration order and newly allocated COM interfaces do not change
        // capture geometry. Compare native identity, geometry and color metadata instead.
        self.configuration.len() == other.configuration.len()
            && self
                .configuration
                .iter()
                .all(|monitor| other.configuration.contains(monitor))
    }
}

#[derive(Default)]
struct DisplayCacheState {
    snapshot: DisplaySnapshot,
    refreshed_at: Option<Instant>,
    refreshed_generation: Option<u64>,
    failure: Option<DisplayRefreshFailure>,
    successful_refreshes: u64,
}

struct DisplayCacheShared {
    state: Mutex<DisplayCacheState>,
    generation: AtomicU64,
    validated_generation: AtomicU64,
    listening: AtomicBool,
    enumerate: Box<dyn Fn() -> CaptureResult<DisplaySnapshot> + Send + Sync>,
    clock: Box<dyn Fn() -> Instant + Send + Sync>,
}

// SAFETY: Native monitor handles are only queried, never destroyed here. DXGI interfaces
// support concurrent access, and cache snapshots/publication are protected by the mutex.
unsafe impl Send for DisplayCacheShared {}
unsafe impl Sync for DisplayCacheShared {}

impl DisplayCacheShared {
    fn new(
        enumerate: impl Fn() -> CaptureResult<DisplaySnapshot> + Send + Sync + 'static,
        clock: impl Fn() -> Instant + Send + Sync + 'static,
    ) -> Self {
        Self {
            state: Mutex::new(DisplayCacheState::default()),
            generation: AtomicU64::new(0),
            validated_generation: AtomicU64::new(u64::MAX),
            listening: AtomicBool::new(false),
            enumerate: Box::new(enumerate),
            clock: Box::new(clock),
        }
    }

    fn invalidate(&self) -> u64 {
        self.generation.fetch_add(1, Ordering::AcqRel) + 1
    }

    fn listener_stopped(&self) {
        // Only the worker retires its subscription, and recovery joins it before
        // replacement. Publish invalidation before making recovery eligible.
        if self.listening.load(Ordering::Acquire) {
            self.invalidate();
            self.listening.store(false, Ordering::Release);
        }
    }

    fn read<R>(
        &self,
        force: bool,
        consume: impl FnOnce(&DisplayCacheState) -> R,
    ) -> CaptureResult<R> {
        // Serialize enumeration as well as publication. Window callbacks only invalidate
        // atomics, so an older enumeration cannot overwrite a newer refresh or block the pump.
        let mut state = self.state.lock().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("display cache mutex was poisoned"))
        })?;
        let mut generation = self.generation.load(Ordering::Acquire);
        let listening = self.listening.load(Ordering::Acquire);
        if !force
            && listening
            && state.refreshed_generation == Some(generation)
            && state.failure.is_none()
        {
            return Ok(consume(&state));
        }
        let now = (self.clock)();
        let expired = !listening
            && state
                .refreshed_at
                .is_none_or(|at| now.duration_since(at) >= FALLBACK_FRESHNESS);
        if force {
            generation = self.invalidate();
        }
        if expired || state.refreshed_generation != Some(generation) || state.failure.is_some() {
            if !force
                && let Some(failure) = &state.failure
                && failure.generation == generation
                && now < failure.retry_at
            {
                return Err(failure.error.clone());
            }
            let snapshot = match (self.enumerate)() {
                Ok(snapshot) => snapshot,
                Err(error) => {
                    let failure = DisplayRefreshFailure::new(generation, error, (self.clock)());
                    let error = failure.error.clone();
                    state.failure = Some(failure);
                    return Err(error);
                }
            };
            if state.refreshed_generation == Some(generation)
                && !state.snapshot.same_configuration(&snapshot)
            {
                // An expired fallback snapshot only invalidates capture resources when
                // its configuration changes. Do not consume a concurrent notification:
                // that newer generation must remain dirty for the next lookup.
                if self
                    .generation
                    .compare_exchange(
                        generation,
                        generation + 1,
                        Ordering::AcqRel,
                        Ordering::Acquire,
                    )
                    .is_ok()
                {
                    generation += 1;
                }
            }
            state.snapshot = snapshot;
            state.refreshed_at = Some((self.clock)());
            // A notification arriving during enumeration remains dirty for the next read.
            state.refreshed_generation = Some(generation);
            state.failure = None;
            state.successful_refreshes += 1;
            self.validated_generation
                .store(generation, Ordering::Release);
        }
        Ok(consume(&state))
    }
}

/// Event-driven while the listener is healthy; bounded, on-demand refresh otherwise.
/// The worker owns only shared cache state, never the owner of its join handle.
pub(crate) struct DisplayInfoCache {
    shared: Arc<DisplayCacheShared>,
    listener: Mutex<ListenerRecovery>,
}

struct ListenerRecovery {
    listener: Option<DisplayListener>,
    start: Box<dyn Fn(Arc<DisplayCacheShared>) -> CaptureResult<DisplayListener> + Send + Sync>,
    last_attempt: Option<(u64, Instant)>,
}

impl DisplayInfoCache {
    pub(crate) fn new() -> CaptureResult<Arc<Self>> {
        Self::with_listener(
            Arc::new(DisplayCacheShared::new(
                DisplaySnapshot::enumerate,
                Instant::now,
            )),
            |shared| DisplayListener::start(shared, run_message_loop),
        )
    }

    fn with_listener(
        shared: Arc<DisplayCacheShared>,
        start: impl Fn(Arc<DisplayCacheShared>) -> CaptureResult<DisplayListener>
        + Send
        + Sync
        + 'static,
    ) -> CaptureResult<Arc<Self>> {
        let refreshed = shared.read(true, |state| state.successful_refreshes)?;
        let cache = Arc::new(Self {
            shared,
            listener: Mutex::new(ListenerRecovery {
                listener: None,
                start: Box::new(start),
                last_attempt: None,
            }),
        });
        cache.restore_listener(refreshed);
        Ok(cache)
    }

    fn restore_listener(&self, refreshed: u64) {
        if self.shared.listening.load(Ordering::Acquire) {
            return;
        }
        let mut recovery = self
            .listener
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let now = (self.shared.clock)();
        if self.shared.listening.load(Ordering::Acquire)
            || recovery.last_attempt.is_some_and(|(attempted, at)| {
                refreshed <= attempted || now.duration_since(at) < REFRESH_RETRY_INTERVAL
            })
        {
            return;
        }
        let first_attempt = recovery.last_attempt.is_none();
        recovery.last_attempt = Some((refreshed, now));
        // Join the retired worker before starting its replacement. Otherwise its exit
        // guard could mark the new listener as unavailable. No cache lock is held here.
        recovery.listener.take();
        match (recovery.start)(Arc::clone(&self.shared)) {
            Ok(listener) => {
                recovery.listener = Some(listener);
                // Enumeration preceded subscription. Revalidate after arming so a display
                // change in that gap cannot leave a permanently stale event-driven cache.
                self.shared.invalidate();
            }
            Err(error) => {
                if first_attempt {
                    self.shared.invalidate();
                }
                // A failed restart must not dirty every successful fallback snapshot:
                // capture remains usable, and the cooldown bounds repeated startup failures.
                eprintln!("Display listener startup failed; refreshing on demand: {error}");
            }
        }
    }

    fn read<R>(
        &self,
        force: bool,
        consume: impl FnOnce(&DisplayCacheState) -> R,
    ) -> CaptureResult<R> {
        if self.shared.listening.load(Ordering::Acquire) {
            return self.shared.read(force, consume);
        }
        let refreshed = self
            .shared
            .read(force, |state| state.successful_refreshes)?;
        self.restore_listener(refreshed);
        self.shared.read(false, consume)
    }

    pub(crate) fn generation(&self) -> u64 {
        self.shared.generation.load(Ordering::Acquire)
    }

    pub(crate) fn checked_generation(&self) -> CaptureResult<u64> {
        let generation = self.generation();
        if self.shared.listening.load(Ordering::Acquire)
            && self.shared.validated_generation.load(Ordering::Acquire) == generation
        {
            // A healthy, validated subscription keeps per-frame checks on atomics:
            // no mutex, clock read, enumeration or snapshot cloning.
            return Ok(generation);
        }
        self.read(false, |state| state.refreshed_generation.unwrap())
    }

    pub(crate) fn monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        self.read(false, |state| state.snapshot.monitors.clone())
    }

    pub(crate) fn resolved(&self) -> CaptureResult<Vec<ResolvedMonitor>> {
        self.read(false, |state| state.snapshot.resolved.clone())
    }

    pub(crate) fn refresh(&self) -> CaptureResult<()> {
        self.read(true, |_| ())
    }
}

impl Drop for DisplayInfoCache {
    fn drop(&mut self) {
        // Stop/join before releasing the owner's shared state. The worker also holds an
        // Arc, including on startup failure and unwinding paths.
        self.listener
            .get_mut()
            .unwrap_or_else(|error| error.into_inner())
            .listener
            .take();
    }
}

struct StopEvent(HANDLE);
// SAFETY: The event is only signaled/waited on across threads and remains open until
// both the listener owner and the worker have released their Arc.
unsafe impl Send for StopEvent {}
unsafe impl Sync for StopEvent {}

impl StopEvent {
    fn new() -> CaptureResult<Self> {
        unsafe { CreateEventW(None, true, false, None) }
            .map(Self)
            .context("CreateEventW for display listener failed")
            .map_err(CaptureError::platform)
    }
}

impl Drop for StopEvent {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}

struct DisplayListener {
    stop: Arc<StopEvent>,
    join: Option<std::thread::JoinHandle<()>>,
    #[cfg(test)]
    hwnd: isize,
}

impl DisplayListener {
    fn start(
        shared: Arc<DisplayCacheShared>,
        pump: fn(&StopEvent) -> CaptureResult<()>,
    ) -> CaptureResult<Self> {
        let stop = Arc::new(StopEvent::new()?);
        let worker_stop = Arc::clone(&stop);
        let (ready_tx, ready_rx) = mpsc::channel();
        let join = std::thread::Builder::new()
            .name("snow-display-change-listener".to_owned())
            .spawn(move || {
                struct ListenerState(Arc<DisplayCacheShared>);
                impl Drop for ListenerState {
                    fn drop(&mut self) {
                        self.0.listener_stopped();
                    }
                }
                let _state = ListenerState(Arc::clone(&shared));
                let window = match create_listener_window(Arc::as_ptr(&shared) as usize) {
                    Ok(hwnd) => ListenerWindow(hwnd),
                    Err(error) => {
                        let _ = ready_tx.send(Err(error));
                        return;
                    }
                };
                // Publish from the worker, before readiness, so an early failure can never
                // be overwritten by the caller marking a dead listener as running.
                shared.listening.store(true, Ordering::Release);
                if ready_tx.send(Ok(window.0.0 as isize)).is_err() {
                    return;
                }
                if let Err(error) = pump(&worker_stop) {
                    eprintln!("Display listener stopped; refreshing on demand: {error}");
                }
            })
            .context("spawning display listener failed")
            .map_err(CaptureError::platform)?;
        let ready = ready_rx.recv();
        let hwnd = match ready {
            Ok(Ok(hwnd)) => hwnd,
            result => {
                let _ = join.join();
                return Err(match result {
                    Ok(Err(error)) => error,
                    _ => CaptureError::platform(anyhow::anyhow!(
                        "display listener exited before readiness"
                    )),
                });
            }
        };
        #[cfg(not(test))]
        let _ = hwnd;
        Ok(Self {
            stop,
            join: Some(join),
            #[cfg(test)]
            hwnd,
        })
    }
}

impl Drop for DisplayListener {
    fn drop(&mut self) {
        // This wakeup is independent of HWND lifetime and cannot be lost in a full queue.
        if let Err(error) = unsafe { SetEvent(self.stop.0) } {
            eprintln!("Signaling display listener shutdown failed: {error}");
        }
        if let Some(join) = self.join.take()
            && join.join().is_err()
        {
            eprintln!("Display listener panicked; refreshing on demand");
        }
    }
}

struct ListenerWindow(HWND);
impl Drop for ListenerWindow {
    fn drop(&mut self) {
        unsafe {
            let _ = DestroyWindow(self.0);
        }
    }
}

const CLASS_NAME: &str = "SnowCaptureDisplayChangeListener";

fn create_listener_window(cache_ptr: usize) -> CaptureResult<HWND> {
    let class_name_wide: Vec<u16> = CLASS_NAME
        .encode_utf16()
        .chain(std::iter::once(0))
        .collect();
    let hinstance = unsafe { GetModuleHandleW(None) }
        .context("GetModuleHandleW failed")
        .map_err(CaptureError::platform)?;
    let wc = WNDCLASSW {
        lpfnWndProc: Some(display_change_wnd_proc),
        hInstance: hinstance.into(),
        lpszClassName: windows::core::PCWSTR(class_name_wide.as_ptr()),
        ..Default::default()
    };
    if unsafe { RegisterClassW(&wc) } == 0 {
        let error = unsafe { GetLastError() };
        if error != ERROR_CLASS_ALREADY_EXISTS {
            return Err(CaptureError::platform(anyhow::anyhow!(
                "RegisterClassW failed: {}",
                error.0
            )));
        }
    }
    unsafe {
        CreateWindowExW(
            Default::default(),
            windows::core::PCWSTR(class_name_wide.as_ptr()),
            windows::core::PCWSTR::null(),
            WS_OVERLAPPED,
            0,
            0,
            0,
            0,
            // Message-only windows do not receive WM_DISPLAYCHANGE broadcasts.
            None,
            None,
            Some(hinstance.into()),
            Some(cache_ptr as *const std::ffi::c_void),
        )
    }
    .context("CreateWindowExW for display change listener failed")
    .map_err(CaptureError::platform)
}

#[derive(Clone, Copy)]
enum Wake {
    Stop,
    Messages,
}

fn run_message_loop(stop: &StopEvent) -> CaptureResult<()> {
    pump_messages(
        || {
            let result = unsafe {
                MsgWaitForMultipleObjectsEx(
                    Some(&[stop.0]),
                    INFINITE,
                    QS_ALLINPUT,
                    MWMO_INPUTAVAILABLE,
                )
            };
            if result == WAIT_OBJECT_0 {
                return Ok(Wake::Stop);
            }
            if result.0 == WAIT_OBJECT_0.0 + 1 {
                return Ok(Wake::Messages);
            }
            let error = if result == WAIT_FAILED {
                anyhow::Error::new(windows::core::Error::from_thread())
                    .context("waiting for display listener messages failed")
            } else {
                anyhow::anyhow!("unexpected display listener wait result: {}", result.0)
            };
            Err(CaptureError::platform(error))
        },
        |msg| unsafe { PeekMessageW(msg, None, 0, 0, PM_REMOVE).as_bool() },
        |msg| unsafe {
            let _ = TranslateMessage(msg);
            DispatchMessageW(msg);
        },
    )
}

fn pump_messages(
    mut wait: impl FnMut() -> CaptureResult<Wake>,
    mut next: impl FnMut(&mut MSG) -> bool,
    mut dispatch: impl FnMut(&MSG),
) -> CaptureResult<()> {
    loop {
        match wait()? {
            Wake::Stop => return Ok(()),
            Wake::Messages => {
                let mut msg = MSG::default();
                // Return to the stop-aware wait even during a message flood. System
                // wakeups can also have no queued message; never use a blocking GetMessage.
                for _ in 0..64 {
                    if !next(&mut msg) {
                        break;
                    }
                    if msg.message == WM_QUIT {
                        return Err(CaptureError::WorkerDead);
                    }
                    dispatch(&msg);
                }
            }
        }
    }
}

unsafe extern "system" fn display_change_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    use windows::Win32::UI::WindowsAndMessaging::{
        CREATESTRUCTW, GWLP_USERDATA, GetWindowLongPtrW, PostQuitMessage, SetWindowLongPtrW,
        WM_CREATE,
    };
    match msg {
        WM_DISPLAYCHANGE => {
            let ptr = unsafe { GetWindowLongPtrW(hwnd, GWLP_USERDATA) };
            if ptr != 0 {
                let cache = unsafe { &*(ptr as *const DisplayCacheShared) };
                // Keep notifications cheap. The next capture/read performs the fallible
                // enumeration and cannot silently reuse a known-stale nonempty snapshot.
                cache.invalidate();
            }
            LRESULT(0)
        }
        WM_CLOSE => {
            unsafe {
                PostQuitMessage(0);
            }
            LRESULT(0)
        }
        WM_CREATE => {
            let create_struct = unsafe { &*(lparam.0 as *const CREATESTRUCTW) };
            unsafe {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, create_struct.lpCreateParams as isize);
            }
            unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) }
        }
        _ => unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) },
    }
}

#[cfg(test)]
mod stream_tests;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::region::MonitorLayout;
    use windows::Win32::UI::WindowsAndMessaging::{
        EnumWindows, IsWindowVisible, SMTO_ABORTIFHUNG, SendMessageTimeoutW,
    };
    use windows::core::BOOL;

    use std::cell::Cell;
    use std::sync::atomic::AtomicUsize;

    fn access_lost_retry(error: CaptureError) -> Arc<DisplayRefreshRetry> {
        let CaptureError::DisplayRefreshPending(retry) = error else {
            panic!("expected a scheduled display refresh, got {error}");
        };
        assert!(matches!(retry.cause(), CaptureError::AccessLost));
        retry
    }

    #[test]
    fn cached_failure_keeps_its_attempt_identity_and_deadline() {
        let fixture = Fixture::new();
        let cache = fixture.fallback_cache();
        cache.checked_generation().unwrap();
        fixture.fail.store(true, Ordering::SeqCst);
        fixture.shared.invalidate();
        let now = *fixture.now.lock().unwrap();
        let first = access_lost_retry(cache.checked_generation().unwrap_err());
        assert_eq!(first.retry_at(), now + REFRESH_RETRY_INTERVAL);
        fixture.advance(REFRESH_RETRY_INTERVAL / 2);
        let cached = access_lost_retry(cache.monitors().unwrap_err());
        assert!(Arc::ptr_eq(&first, &cached));
        fixture.advance(REFRESH_RETRY_INTERVAL / 2);
        let next = access_lost_retry(cache.checked_generation().unwrap_err());
        assert!(!Arc::ptr_eq(&first, &next));
        assert_eq!(next.retry_at(), first.retry_at() + REFRESH_RETRY_INTERVAL);

        // An explicit refresh is a distinct attempt even at the same clock instant.
        let forced = access_lost_retry(cache.refresh().unwrap_err());
        assert!(!Arc::ptr_eq(&next, &forced));
        assert_eq!(next.retry_at(), forced.retry_at());
    }

    #[test]
    fn display_transition_errors_preserve_diagnostics_and_retry_only_known_failures() {
        use windows::Win32::Foundation::{E_ACCESSDENIED, E_INVALIDARG, E_OUTOFMEMORY};
        use windows::Win32::Graphics::Dxgi::{
            DXGI_ERROR_ACCESS_LOST, DXGI_ERROR_DEVICE_HUNG, DXGI_ERROR_DEVICE_REMOVED,
            DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_INVALID_CALL, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE,
            DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE, DXGI_ERROR_SESSION_DISCONNECTED,
            DXGI_ERROR_UNSUPPORTED, DXGI_ERROR_WAIT_TIMEOUT,
        };

        let now = Instant::now();
        for (code, transient) in [
            (DXGI_ERROR_ACCESS_LOST, true),
            (DXGI_ERROR_DEVICE_REMOVED, true),
            (DXGI_ERROR_DEVICE_RESET, true),
            (DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, true),
            (DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE, true),
            (DXGI_ERROR_SESSION_DISCONNECTED, true),
            (DXGI_ERROR_WAIT_TIMEOUT, true),
            (E_ACCESSDENIED, false),
            (E_INVALIDARG, false),
            (E_OUTOFMEMORY, false),
            (DXGI_ERROR_INVALID_CALL, false),
            (DXGI_ERROR_DEVICE_HUNG, false),
            (DXGI_ERROR_UNSUPPORTED, false),
        ] {
            let cause = CaptureError::platform(
                anyhow::Error::new(windows::core::Error::from_hresult(code))
                    .context("EnumOutputs on adapter 0 failed"),
            );
            let message = cause.to_string();
            let error = DisplayRefreshFailure::new(1, cause, now).error;
            assert_eq!(error.is_retryable(), transient, "{code:?}");
            assert!(error.to_string().contains(&message));
            if let CaptureError::DisplayRefreshPending(retry) = error {
                assert_eq!(retry.retry_at(), now + REFRESH_RETRY_INTERVAL);
                let CaptureError::Platform(source) = retry.cause() else {
                    panic!("must preserve native cause");
                };
                assert_eq!(
                    source
                        .downcast_ref::<windows::core::Error>()
                        .unwrap()
                        .code(),
                    code
                );
            }
        }
        for cause in [
            CaptureError::PermissionDenied,
            CaptureError::Canceled,
            CaptureError::InvalidConfig("bad config".into()),
            CaptureError::platform(anyhow::anyhow!("unknown driver failure")),
        ] {
            let message = cause.to_string();
            let error = DisplayRefreshFailure::new(1, cause, now).error;
            assert!(!error.is_retryable());
            assert_eq!(error.to_string(), message);
        }
    }

    struct Fixture {
        shared: Arc<DisplayCacheShared>,
        now: Arc<Mutex<Instant>>,
        calls: Arc<AtomicUsize>,
        clock_reads: Arc<AtomicUsize>,
        fail: Arc<AtomicBool>,
        configuration: Arc<Mutex<Vec<DisplayMonitorConfiguration>>>,
    }

    impl Fixture {
        fn new() -> Self {
            let now = Arc::new(Mutex::new(Instant::now()));
            let calls = Arc::new(AtomicUsize::new(0));
            let clock_reads = Arc::new(AtomicUsize::new(0));
            let fail = Arc::new(AtomicBool::new(false));
            let configuration = Arc::new(Mutex::new(Vec::<DisplayMonitorConfiguration>::new()));
            let shared = Arc::new(DisplayCacheShared::new(
                {
                    let calls = Arc::clone(&calls);
                    let fail = Arc::clone(&fail);
                    let configuration = Arc::clone(&configuration);
                    move || {
                        calls.fetch_add(1, Ordering::SeqCst);
                        if fail.load(Ordering::SeqCst) {
                            Err(CaptureError::AccessLost)
                        } else {
                            let configuration = configuration.lock().unwrap().clone();
                            let monitors = configuration
                                .iter()
                                .map(|monitor| {
                                    MonitorId::from_parts(
                                        monitor.key.adapter_luid,
                                        monitor.key.output_id,
                                        monitor.handle,
                                        &monitor.name,
                                        monitor.is_primary,
                                    )
                                })
                                .collect();
                            Ok(DisplaySnapshot {
                                monitors,
                                configuration,
                                ..Default::default()
                            })
                        }
                    }
                },
                {
                    let now = Arc::clone(&now);
                    let clock_reads = Arc::clone(&clock_reads);
                    move || {
                        clock_reads.fetch_add(1, Ordering::SeqCst);
                        *now.lock().unwrap()
                    }
                },
            ));
            Self {
                shared,
                now,
                calls,
                clock_reads,
                fail,
                configuration,
            }
        }
        fn advance(&self, duration: Duration) {
            *self.now.lock().unwrap() += duration;
        }
        fn calls(&self) -> usize {
            self.calls.load(Ordering::SeqCst)
        }
        fn fallback_cache(&self) -> Arc<DisplayInfoCache> {
            DisplayInfoCache::with_listener(Arc::clone(&self.shared), |_| {
                Err(CaptureError::WorkerDead)
            })
            .unwrap()
        }
    }

    fn monitor_configuration(output_id: u64) -> DisplayMonitorConfiguration {
        DisplayMonitorConfiguration {
            key: MonitorKey {
                adapter_luid: 1,
                output_id,
            },
            name: format!("display-{output_id}"),
            handle: output_id as isize,
            desktop_coordinates: RECT {
                left: 0,
                top: 0,
                right: 1920,
                bottom: 1080,
            },
            rotation: DXGI_MODE_ROTATION(1),
            hdr_metadata: HdrMonitorMetadata::default(),
            is_primary: true,
        }
    }

    #[test]
    fn unchanged_fallback_refresh_preserves_capture_generation() {
        let fixture = Fixture::new();
        *fixture.configuration.lock().unwrap() =
            vec![monitor_configuration(1), monitor_configuration(2)];
        let cache = fixture.fallback_cache();
        let generation = cache.checked_generation().unwrap();
        let calls = fixture.calls();
        for _ in 0..5 {
            fixture.configuration.lock().unwrap().reverse();
            fixture.advance(FALLBACK_FRESHNESS);
            assert_eq!(cache.checked_generation().unwrap(), generation);
        }
        assert_eq!(fixture.calls(), calls + 5);
    }

    #[test]
    fn fallback_configuration_changes_invalidate_capture_generation() {
        let changes: &[fn(&mut DisplayMonitorConfiguration)] = &[
            |monitor| monitor.key.adapter_luid += 1,
            |monitor| monitor.key.output_id += 1,
            |monitor| monitor.name.push_str("-renamed"),
            |monitor| monitor.handle += 1,
            |monitor| monitor.desktop_coordinates.left -= 100,
            |monitor| monitor.desktop_coordinates.top -= 100,
            |monitor| monitor.desktop_coordinates.right += 100,
            |monitor| monitor.desktop_coordinates.bottom += 100,
            |monitor| monitor.rotation = DXGI_MODE_ROTATION(2),
            |monitor| monitor.hdr_metadata.advanced_color_enabled = true,
            |monitor| monitor.hdr_metadata.hdr_enabled = true,
            |monitor| monitor.hdr_metadata.color_space += 1,
            |monitor| monitor.hdr_metadata.sdr_white_nits = Some(203.0),
            |monitor| monitor.hdr_metadata.hdr_peak_nits = Some(1600.0),
            |monitor| monitor.is_primary = false,
        ];
        for (index, change) in changes.iter().enumerate() {
            let fixture = Fixture::new();
            *fixture.configuration.lock().unwrap() = vec![monitor_configuration(1)];
            let cache = fixture.fallback_cache();
            let before = cache.checked_generation().unwrap();
            change(&mut fixture.configuration.lock().unwrap()[0]);
            fixture.advance(FALLBACK_FRESHNESS);
            let after = cache.checked_generation().unwrap();
            assert!(
                after > before,
                "configuration change {index} must invalidate capture"
            );
            fixture.advance(FALLBACK_FRESHNESS);
            assert_eq!(cache.checked_generation().unwrap(), after);
        }
    }

    #[test]
    fn fallback_monitor_addition_and_removal_invalidate_capture_generation() {
        let fixture = Fixture::new();
        let cache = fixture.fallback_cache();
        let empty = cache.checked_generation().unwrap();
        fixture
            .configuration
            .lock()
            .unwrap()
            .push(monitor_configuration(1));
        fixture.advance(FALLBACK_FRESHNESS);
        let connected = cache.checked_generation().unwrap();
        assert!(connected > empty);
        assert_eq!(cache.monitors().unwrap().len(), 1);
        fixture.configuration.lock().unwrap().clear();
        fixture.advance(FALLBACK_FRESHNESS);
        assert!(cache.checked_generation().unwrap() > connected);
        assert!(cache.monitors().unwrap().is_empty());
    }

    #[test]
    fn failed_expired_fallback_refresh_is_throttled_until_recovery() {
        let fixture = Fixture::new();
        *fixture.configuration.lock().unwrap() = vec![monitor_configuration(1)];
        let cache = fixture.fallback_cache();
        let generation = cache.checked_generation().unwrap();
        let calls = fixture.calls();
        fixture.fail.store(true, Ordering::SeqCst);
        fixture.advance(FALLBACK_FRESHNESS);
        for _ in 0..100 {
            access_lost_retry(cache.checked_generation().unwrap_err());
            assert!(cache.monitors().is_err());
        }
        assert_eq!(fixture.calls(), calls + 1);
        fixture.fail.store(false, Ordering::SeqCst);
        fixture.advance(REFRESH_RETRY_INTERVAL);
        assert_eq!(cache.checked_generation().unwrap(), generation);
        assert_eq!(fixture.calls(), calls + 2);
    }

    #[test]
    fn healthy_capture_generation_checks_do_not_read_the_clock() {
        let fixture = Fixture::new();
        let cache = DisplayInfoCache::with_listener(Arc::clone(&fixture.shared), |shared| {
            DisplayListener::start(shared, run_message_loop)
        })
        .unwrap();
        let generation = cache.checked_generation().unwrap();
        let reads = fixture.clock_reads.load(Ordering::SeqCst);
        for _ in 0..100 {
            assert_eq!(cache.checked_generation().unwrap(), generation);
        }
        assert_eq!(fixture.clock_reads.load(Ordering::SeqCst), reads);
    }

    #[test]
    fn healthy_generation_checks_revalidate_notifications_and_failed_explicit_refreshes() {
        let fixture = Fixture::new();
        let cache = DisplayInfoCache::with_listener(Arc::clone(&fixture.shared), |shared| {
            DisplayListener::start(shared, run_message_loop)
        })
        .unwrap();
        let generation = cache.checked_generation().unwrap();
        fixture.fail.store(true, Ordering::SeqCst);
        fixture.shared.invalidate();
        let calls = fixture.calls();
        for _ in 0..100 {
            access_lost_retry(cache.checked_generation().unwrap_err());
        }
        assert_eq!(fixture.calls(), calls + 1);
        fixture.fail.store(false, Ordering::SeqCst);
        fixture.shared.invalidate();
        assert!(cache.checked_generation().unwrap() > generation);
        fixture.fail.store(true, Ordering::SeqCst);
        assert!(cache.refresh().is_err());
        assert!(cache.checked_generation().is_err());
        fixture.fail.store(false, Ordering::SeqCst);
        cache.refresh().unwrap();
        cache.checked_generation().unwrap();
    }

    #[test]
    fn fallback_configuration_change_does_not_consume_a_concurrent_notification() {
        let slot = Arc::new(Mutex::new(None::<std::sync::Weak<DisplayCacheShared>>));
        let calls = Arc::new(AtomicUsize::new(0));
        let now = Arc::new(Mutex::new(Instant::now()));
        let shared = Arc::new(DisplayCacheShared::new(
            {
                let slot = Arc::clone(&slot);
                let calls = Arc::clone(&calls);
                move || {
                    let call = calls.fetch_add(1, Ordering::SeqCst);
                    if call == 1 {
                        slot.lock()
                            .unwrap()
                            .as_ref()
                            .unwrap()
                            .upgrade()
                            .unwrap()
                            .invalidate();
                    }
                    Ok(DisplaySnapshot {
                        configuration: if call == 0 {
                            Vec::new()
                        } else {
                            vec![monitor_configuration(1)]
                        },
                        ..Default::default()
                    })
                }
            },
            {
                let now = Arc::clone(&now);
                move || *now.lock().unwrap()
            },
        ));
        *slot.lock().unwrap() = Some(Arc::downgrade(&shared));
        shared.read(true, |_| ()).unwrap();
        *now.lock().unwrap() += FALLBACK_FRESHNESS;
        shared.read(false, |_| ()).unwrap();
        assert_ne!(
            shared.validated_generation.load(Ordering::Acquire),
            shared.generation.load(Ordering::Acquire)
        );
        shared.read(false, |_| ()).unwrap();
        assert_eq!(calls.load(Ordering::SeqCst), 3);
    }

    #[test]
    fn listener_startup_failure_keeps_capture_discovery_available() {
        let fixture = Fixture::new();
        let cache = fixture.fallback_cache();
        assert!(cache.listener.lock().unwrap().listener.is_none());
        assert!(!fixture.shared.listening.load(Ordering::Acquire));
        assert!(cache.monitors().unwrap().is_empty());
        assert_eq!(
            fixture.calls(),
            2,
            "the first demand after failure revalidates the snapshot"
        );
        cache.resolved().unwrap();
        cache.checked_generation().unwrap();
        assert_eq!(
            fixture.calls(),
            2,
            "related lookups share the refreshed snapshot"
        );
        fixture.advance(FALLBACK_FRESHNESS);
        assert_eq!(fixture.calls(), 2, "elapsed time alone performs no work");
        let before = cache.generation();
        assert_eq!(cache.checked_generation().unwrap(), before);
        assert_eq!(
            fixture.calls(),
            3,
            "generation-only consumers also trigger fallback refresh"
        );
    }

    #[test]
    fn initial_enumeration_failure_is_not_hidden_by_listener_fallback() {
        let fixture = Fixture::new();
        fixture.fail.store(true, Ordering::SeqCst);
        let result = DisplayInfoCache::with_listener(Arc::clone(&fixture.shared), |_| {
            panic!("do not start a listener for a failed initial snapshot")
        });
        access_lost_retry(result.err().expect("initial enumeration must fail"));
    }

    #[test]
    fn healthy_listener_reuses_cache_until_invalidated() {
        let fixture = Fixture::new();
        fixture.shared.listening.store(true, Ordering::Release);
        fixture.shared.read(true, |_| ()).unwrap();
        fixture.advance(Duration::from_secs(3600));
        fixture.shared.read(false, |_| ()).unwrap();
        assert_eq!(fixture.calls(), 1);
        fixture.shared.invalidate();
        fixture.shared.read(false, |_| ()).unwrap();
        assert_eq!(fixture.calls(), 2);
    }

    #[test]
    fn failed_refresh_never_serves_stale_nonempty_snapshot_and_recovers_on_demand() {
        let fixture = Fixture::new();
        let cache = fixture.fallback_cache();
        cache.monitors().unwrap();
        fixture
            .shared
            .state
            .lock()
            .unwrap()
            .snapshot
            .monitors
            .push(MonitorId::from_parts(99, 0, 0, "stale", true));
        fixture.fail.store(true, Ordering::SeqCst);
        fixture.shared.invalidate();
        access_lost_retry(cache.monitors().unwrap_err());
        let calls = fixture.calls();
        for _ in 0..100 {
            assert!(cache.checked_generation().is_err());
            assert!(cache.resolved().is_err());
        }
        assert_eq!(
            fixture.calls(),
            calls,
            "capture frequency must not become retry frequency"
        );
        fixture.fail.store(false, Ordering::SeqCst);
        fixture.advance(REFRESH_RETRY_INTERVAL);
        assert!(
            cache.monitors().unwrap().is_empty(),
            "a successful refresh replaces stale data"
        );
        assert_eq!(fixture.calls(), calls + 1);
    }

    #[test]
    fn new_notification_and_explicit_refresh_can_recover_before_retry_deadline() {
        let fixture = Fixture::new();
        let cache = fixture.fallback_cache();
        fixture.fail.store(true, Ordering::SeqCst);
        assert!(cache.refresh().is_err());
        fixture.fail.store(false, Ordering::SeqCst);
        fixture.shared.invalidate();
        cache.monitors().unwrap();
        fixture.fail.store(true, Ordering::SeqCst);
        assert!(cache.refresh().is_err());
        fixture.fail.store(false, Ordering::SeqCst);
        cache.refresh().unwrap();
        cache.checked_generation().unwrap();
    }

    #[test]
    fn notification_during_enumeration_is_not_cleared_by_publication() {
        let shared_slot = Arc::new(Mutex::new(None::<std::sync::Weak<DisplayCacheShared>>));
        let calls = Arc::new(AtomicUsize::new(0));
        let shared = Arc::new(DisplayCacheShared::new(
            {
                let slot = Arc::clone(&shared_slot);
                let calls = Arc::clone(&calls);
                move || {
                    if calls.fetch_add(1, Ordering::SeqCst) == 0 {
                        slot.lock()
                            .unwrap()
                            .as_ref()
                            .unwrap()
                            .upgrade()
                            .unwrap()
                            .invalidate();
                    }
                    Ok(DisplaySnapshot::default())
                }
            },
            Instant::now,
        ));
        *shared_slot.lock().unwrap() = Some(Arc::downgrade(&shared));
        shared.listening.store(true, Ordering::Release);
        shared.read(true, |_| ()).unwrap();
        shared.read(false, |_| ()).unwrap();
        assert_eq!(calls.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn listener_runtime_failure_invalidates_before_the_next_capture_lookup() {
        let fixture = Fixture::new();
        let cache = DisplayInfoCache::with_listener(Arc::clone(&fixture.shared), |shared| {
            let mut listener = DisplayListener::start(shared, |_| Err(CaptureError::AccessLost))?;
            listener.join.take().unwrap().join().unwrap();
            Ok(listener)
        })
        .unwrap();
        assert!(
            !fixture.shared.listening.load(Ordering::Acquire),
            "readiness must not revive a dead listener"
        );
        cache.checked_generation().unwrap();
        assert_eq!(fixture.calls(), 2);
        drop(cache);
    }

    #[test]
    fn successful_fallback_enumeration_restores_listening_after_startup_failure() {
        let fixture = Fixture::new();
        let attempts = Arc::new(AtomicUsize::new(0));
        let cache = DisplayInfoCache::with_listener(Arc::clone(&fixture.shared), {
            let attempts = Arc::clone(&attempts);
            move |shared| {
                if attempts.fetch_add(1, Ordering::SeqCst) == 0 {
                    Err(CaptureError::WorkerDead)
                } else {
                    DisplayListener::start(shared, run_message_loop)
                }
            }
        })
        .unwrap();
        fixture.advance(REFRESH_RETRY_INTERVAL);
        cache.monitors().unwrap();
        assert!(fixture.shared.listening.load(Ordering::Acquire));
        assert_eq!(attempts.load(Ordering::SeqCst), 2);
        let calls = fixture.calls();
        fixture.advance(Duration::from_secs(3600));
        cache.resolved().unwrap();
        cache.checked_generation().unwrap();
        assert_eq!(
            fixture.calls(),
            calls,
            "restored listening stops fallback expiration"
        );
        let hwnd = HWND(
            cache
                .listener
                .lock()
                .unwrap()
                .listener
                .as_ref()
                .unwrap()
                .hwnd as *mut std::ffi::c_void,
        );
        send_display_change(hwnd);
        cache.checked_generation().unwrap();
        assert_eq!(
            fixture.calls(),
            calls + 1,
            "the replacement receives display notifications"
        );
        assert_eq!(attempts.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn enumeration_failure_does_not_trigger_listener_restart() {
        let fixture = Fixture::new();
        let attempts = Arc::new(AtomicUsize::new(0));
        let cache = DisplayInfoCache::with_listener(Arc::clone(&fixture.shared), {
            let attempts = Arc::clone(&attempts);
            move |shared| {
                if attempts.fetch_add(1, Ordering::SeqCst) == 0 {
                    Err(CaptureError::WorkerDead)
                } else {
                    DisplayListener::start(shared, run_message_loop)
                }
            }
        })
        .unwrap();
        fixture.fail.store(true, Ordering::SeqCst);
        fixture.advance(REFRESH_RETRY_INTERVAL);
        access_lost_retry(cache.monitors().unwrap_err());
        assert_eq!(attempts.load(Ordering::SeqCst), 1);
        fixture.fail.store(false, Ordering::SeqCst);
        fixture.advance(REFRESH_RETRY_INTERVAL);
        cache.refresh().unwrap();
        assert!(fixture.shared.listening.load(Ordering::Acquire));
        assert_eq!(attempts.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn successful_fallback_enumeration_replaces_a_failed_runtime_listener() {
        let fixture = Fixture::new();
        let attempts = Arc::new(AtomicUsize::new(0));
        let cache = DisplayInfoCache::with_listener(Arc::clone(&fixture.shared), {
            let attempts = Arc::clone(&attempts);
            move |shared| {
                if attempts.fetch_add(1, Ordering::SeqCst) == 0 {
                    let mut listener =
                        DisplayListener::start(shared, |_| Err(CaptureError::AccessLost))?;
                    listener.join.take().unwrap().join().unwrap();
                    Ok(listener)
                } else {
                    DisplayListener::start(shared, run_message_loop)
                }
            }
        })
        .unwrap();
        assert!(!fixture.shared.listening.load(Ordering::Acquire));
        fixture.advance(REFRESH_RETRY_INTERVAL);
        cache.checked_generation().unwrap();
        assert!(fixture.shared.listening.load(Ordering::Acquire));
        assert_eq!(attempts.load(Ordering::SeqCst), 2);
        drop(cache);
        assert!(!fixture.shared.listening.load(Ordering::Acquire));
    }

    #[test]
    fn concurrent_capture_lookups_restore_only_one_listener() {
        let fixture = Fixture::new();
        let attempts = Arc::new(AtomicUsize::new(0));
        let cache = DisplayInfoCache::with_listener(Arc::clone(&fixture.shared), {
            let attempts = Arc::clone(&attempts);
            move |shared| {
                if attempts.fetch_add(1, Ordering::SeqCst) == 0 {
                    Err(CaptureError::WorkerDead)
                } else {
                    DisplayListener::start(shared, run_message_loop)
                }
            }
        })
        .unwrap();
        fixture.advance(REFRESH_RETRY_INTERVAL);
        let barrier = std::sync::Barrier::new(8);
        std::thread::scope(|scope| {
            for _ in 0..8 {
                scope.spawn(|| {
                    barrier.wait();
                    cache.checked_generation().unwrap();
                });
            }
        });
        assert!(fixture.shared.listening.load(Ordering::Acquire));
        assert_eq!(attempts.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn restart_revalidates_snapshot_after_subscribing() {
        let fixture = Fixture::new();
        let attempts = Arc::new(AtomicUsize::new(0));
        let cache = DisplayInfoCache::with_listener(Arc::clone(&fixture.shared), {
            let attempts = Arc::clone(&attempts);
            move |shared| {
                if attempts.fetch_add(1, Ordering::SeqCst) == 0 {
                    return Err(CaptureError::WorkerDead);
                }
                let listener = DisplayListener::start(Arc::clone(&shared), run_message_loop)?;
                // Simulate a cached display disappearing between enumeration and subscription.
                shared
                    .state
                    .lock()
                    .unwrap()
                    .snapshot
                    .monitors
                    .push(MonitorId::from_parts(99, 0, 0, "disconnected", true));
                Ok(listener)
            }
        })
        .unwrap();
        fixture.advance(REFRESH_RETRY_INTERVAL);
        assert!(cache.monitors().unwrap().is_empty());
        assert_eq!(
            fixture.calls(),
            3,
            "validate again after restoring the subscription"
        );
        assert!(fixture.shared.listening.load(Ordering::Acquire));
    }

    #[test]
    fn repeated_restart_failures_preserve_fallback_and_limit_attempts() {
        let fixture = Fixture::new();
        let attempts = Arc::new(AtomicUsize::new(0));
        let cache = DisplayInfoCache::with_listener(Arc::clone(&fixture.shared), {
            let attempts = Arc::clone(&attempts);
            move |_| {
                attempts.fetch_add(1, Ordering::SeqCst);
                Err(CaptureError::WorkerDead)
            }
        })
        .unwrap();
        fixture.advance(REFRESH_RETRY_INTERVAL);
        cache.checked_generation().unwrap();
        let calls = fixture.calls();
        for _ in 0..100 {
            cache.resolved().unwrap();
            cache.checked_generation().unwrap();
        }
        assert_eq!(attempts.load(Ordering::SeqCst), 2);
        assert_eq!(
            fixture.calls(),
            calls,
            "failed subscription must not invalidate a fresh fallback snapshot"
        );
        for _ in 0..5 {
            cache.refresh().unwrap();
        }
        assert_eq!(
            attempts.load(Ordering::SeqCst),
            2,
            "explicit refreshes also respect restart cooldown"
        );
        fixture.advance(REFRESH_RETRY_INTERVAL);
        cache.monitors().unwrap();
        assert_eq!(attempts.load(Ordering::SeqCst), 3);
    }

    #[test]
    fn replacement_dying_immediately_does_not_cause_a_restart_loop() {
        let fixture = Fixture::new();
        let attempts = Arc::new(AtomicUsize::new(0));
        let cache = DisplayInfoCache::with_listener(Arc::clone(&fixture.shared), {
            let attempts = Arc::clone(&attempts);
            move |shared| {
                attempts.fetch_add(1, Ordering::SeqCst);
                let mut listener =
                    DisplayListener::start(shared, |_| Err(CaptureError::AccessLost))?;
                listener.join.take().unwrap().join().unwrap();
                Ok(listener)
            }
        })
        .unwrap();
        fixture.advance(REFRESH_RETRY_INTERVAL);
        cache.checked_generation().unwrap();
        assert!(!fixture.shared.listening.load(Ordering::Acquire));
        let calls = fixture.calls();
        for _ in 0..100 {
            cache.monitors().unwrap();
        }
        assert_eq!(attempts.load(Ordering::SeqCst), 2);
        assert_eq!(fixture.calls(), calls);
    }

    #[test]
    fn listener_shutdown_uses_stop_event_without_posting_to_a_window() {
        let fixture = Fixture::new();
        let listener =
            DisplayListener::start(Arc::clone(&fixture.shared), run_message_loop).unwrap();
        assert!(fixture.shared.listening.load(Ordering::Acquire));
        drop(listener);
        assert!(!fixture.shared.listening.load(Ordering::Acquire));
    }

    #[test]
    fn message_wait_failure_exits_without_dispatching_stale_messages() {
        let calls = Cell::new(0);
        let dispatches = Cell::new(0);
        let mut waits = [Ok(Wake::Messages), Err(CaptureError::AccessLost)].into_iter();
        let result = pump_messages(
            || waits.next().expect("must stop after wait failure"),
            |msg| {
                calls.set(calls.get() + 1);
                msg.message = WM_DISPLAYCHANGE;
                calls.get() == 1
            },
            |_| dispatches.set(dispatches.get() + 1),
        );
        assert!(matches!(result, Err(CaptureError::AccessLost)));
        assert_eq!(dispatches.get(), 1);
        assert_eq!(calls.get(), 2);
    }

    #[test]
    fn system_wakeup_without_message_returns_to_stop_aware_wait() {
        let mut waits = [Ok(Wake::Messages), Ok(Wake::Stop)].into_iter();
        pump_messages(
            || waits.next().unwrap(),
            |_| false,
            |_| panic!("no message to dispatch"),
        )
        .unwrap();
    }

    #[test]
    fn message_flood_cannot_starve_shutdown() {
        let dispatches = Cell::new(0);
        let mut waits = [Ok(Wake::Messages), Ok(Wake::Stop)].into_iter();
        pump_messages(
            || waits.next().unwrap(),
            |_| true,
            |_| dispatches.set(dispatches.get() + 1),
        )
        .unwrap();
        assert_eq!(dispatches.get(), 64);
    }

    #[test]
    fn unexpected_quit_is_reported_without_dispatch() {
        let result = pump_messages(
            || Ok(Wake::Messages),
            |msg| {
                msg.message = WM_QUIT;
                true
            },
            |_| panic!("must not dispatch quit"),
        );
        assert!(matches!(result, Err(CaptureError::WorkerDead)));
    }

    unsafe extern "system" fn collect_window(hwnd: HWND, lparam: LPARAM) -> BOOL {
        let windows = unsafe { &mut *(lparam.0 as *mut Vec<HWND>) };
        windows.push(hwnd);
        BOOL(1)
    }

    fn top_level_windows() -> Vec<HWND> {
        let mut windows = Vec::new();
        unsafe {
            EnumWindows(
                Some(collect_window),
                LPARAM(&mut windows as *mut Vec<HWND> as isize),
            )
            .expect("enumerate broadcast recipients");
        }
        windows
    }

    fn send_display_change(hwnd: HWND) {
        let delivered = unsafe {
            SendMessageTimeoutW(
                hwnd,
                WM_DISPLAYCHANGE,
                WPARAM(32),
                LPARAM(0),
                SMTO_ABORTIFHUNG,
                5000,
                None,
            )
        };
        assert_ne!(delivered.0, 0, "display-change handler must finish");
    }

    #[test]
    fn direct_display_change_refreshes_stale_monitor_count() {
        let cache = DisplayInfoCache::new().expect("create display cache");
        let expected = cache.monitors().unwrap();
        let hwnd = HWND(
            cache
                .listener
                .lock()
                .unwrap()
                .listener
                .as_ref()
                .unwrap()
                .hwnd as *mut std::ffi::c_void,
        );
        cache
            .shared
            .state
            .lock()
            .unwrap()
            .snapshot
            .monitors
            .push(MonitorId::from_parts(
                u64::MAX,
                0,
                0,
                "disconnected-monitor",
                false,
            ));
        let generation = cache.generation();
        assert!(MonitorLayout::snapshot_from_monitors(cache.monitors().unwrap()).is_err());

        send_display_change(hwnd);

        assert!(cache.generation() > generation);
        assert_eq!(cache.monitors().unwrap(), expected);
        assert!(MonitorLayout::snapshot_from_monitors(cache.monitors().unwrap()).is_ok());
    }

    #[test]
    fn display_change_broadcast_refreshes_all_monitor_caches() {
        // Snapshot workers have independent caches; every listener must receive broadcasts.
        let caches = [
            DisplayInfoCache::new().expect("create desktop cache"),
            DisplayInfoCache::new().expect("create snapshot worker cache"),
        ];
        let expected = caches[0].monitors().unwrap();
        let recipients = top_level_windows();
        for (index, cache) in caches.iter().enumerate() {
            let hwnd = HWND(
                cache
                    .listener
                    .lock()
                    .unwrap()
                    .listener
                    .as_ref()
                    .unwrap()
                    .hwnd as *mut std::ffi::c_void,
            );
            assert!(!unsafe { IsWindowVisible(hwnd) }.as_bool());
            {
                let mut state = cache.shared.state.lock().unwrap();
                if index == 0 {
                    state.snapshot.monitors.clear();
                } else {
                    state.snapshot.monitors.push(MonitorId::from_parts(
                        u64::MAX,
                        0,
                        0,
                        "disconnected-monitor",
                        false,
                    ));
                }
            }
            let generation = cache.generation();

            // Use Windows' broadcast recipient set, but notify only our test windows.
            if recipients.contains(&hwnd) {
                send_display_change(hwnd);
            }

            assert!(
                cache.generation() > generation,
                "display-change broadcasts must invalidate every native monitor cache"
            );
            assert_eq!(cache.monitors().unwrap(), expected);
            assert_eq!(cache.resolved().unwrap().len(), expected.len());
            assert!(MonitorLayout::snapshot_from_monitors(cache.monitors().unwrap()).is_ok());
        }
    }
}

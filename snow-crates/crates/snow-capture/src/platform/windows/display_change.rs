use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex, RwLock};
use std::time::Instant;

use anyhow::Context;
use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, GetMessageW, MSG,
    PostMessageW, RegisterClassW, TranslateMessage, WM_CLOSE, WM_DISPLAYCHANGE, WM_USER, WNDCLASSW,
    WS_OVERLAPPED,
};

use crate::error::{CaptureError, CaptureResult};
use crate::monitor::MonitorId;

use super::monitor::ResolvedMonitor;

/// Sentinel message used to tell the listener thread to shut down.
const WM_QUIT_LISTENER: u32 = WM_USER + 1;

/// Shared state that the listener thread writes to and readers consume.
#[derive(Default)]
struct DisplayCacheState {
    monitors: Vec<MonitorId>,
    resolved: Vec<ResolvedMonitor>,
    refreshed_at: Option<Instant>,
}

/// A display information cache that refreshes when Windows sends
/// `WM_DISPLAYCHANGE` rather than polling on a TTL.
pub(crate) struct DisplayInfoCache {
    state: RwLock<DisplayCacheState>,
    generation: AtomicU64,
    refresh_required: AtomicBool,
    refresh_lock: Mutex<()>,
    listener_hwnd: Mutex<Option<HWND>>,
    listener_join: Mutex<Option<std::thread::JoinHandle<()>>>,
    running: AtomicBool,
}

// SAFETY: HWND is a raw pointer wrapper but we only send messages to it
// from the owning process, and the listener thread is joined on drop.
unsafe impl Send for DisplayInfoCache {}
unsafe impl Sync for DisplayInfoCache {}

impl DisplayInfoCache {
    /// Create a populated cache and spawn the listener thread.
    pub(crate) fn new() -> CaptureResult<Arc<Self>> {
        let cache = Arc::new(Self {
            state: RwLock::new(DisplayCacheState::default()),
            generation: AtomicU64::new(0),
            refresh_required: AtomicBool::new(true),
            refresh_lock: Mutex::new(()),
            listener_hwnd: Mutex::new(None),
            listener_join: Mutex::new(None),
            running: AtomicBool::new(false),
        });

        cache.refresh_pending()?;
        cache.start_listener()?;

        Ok(cache)
    }

    /// Return the current generation counter. Bumped on every display change.
    #[allow(dead_code)]
    pub(crate) fn generation(&self) -> u64 {
        self.generation.load(Ordering::Acquire)
    }

    /// Return a snapshot of the cached monitor list.
    pub(crate) fn monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        if self.refresh_required.load(Ordering::Acquire) {
            self.refresh_pending()?;
        }
        let state = self.state.read().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("display cache rwlock was poisoned"))
        })?;
        if state.monitors.is_empty() {
            drop(state);
            self.mark_refresh_required();
            return self.refresh_and_get_monitors();
        }
        Ok(state.monitors.clone())
    }

    /// Return a snapshot of the cached resolved monitors.
    pub(crate) fn resolved(&self) -> CaptureResult<Vec<ResolvedMonitor>> {
        if self.refresh_required.load(Ordering::Acquire) {
            self.refresh_pending()?;
        }
        let state = self.state.read().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("display cache rwlock was poisoned"))
        })?;
        if state.resolved.is_empty() {
            drop(state);
            self.mark_refresh_required();
            self.refresh_pending()?;
            let state = self.state.read().map_err(|_| {
                CaptureError::platform(anyhow::anyhow!("display cache rwlock was poisoned"))
            })?;
            return Ok(state.resolved.clone());
        }
        Ok(state.resolved.clone())
    }

    /// Force a refresh of the cached data.
    pub(crate) fn refresh(&self) -> CaptureResult<()> {
        self.mark_refresh_required();
        self.refresh_pending()
    }

    fn mark_refresh_required(&self) {
        let _refresh = match self.refresh_lock.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };
        self.refresh_required.store(true, Ordering::Release);
        self.generation.fetch_add(1, Ordering::AcqRel);
    }

    fn refresh_pending(&self) -> CaptureResult<()> {
        let _refresh = self.refresh_lock.lock().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("display refresh mutex was poisoned"))
        })?;
        if !self.refresh_required.load(Ordering::Acquire) {
            return Ok(());
        }

        let resolved = super::monitor::enumerate_resolved()?;
        let monitors = super::monitor::to_monitor_ids(&resolved);

        let mut state = self.state.write().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("display cache rwlock was poisoned"))
        })?;
        state.monitors = monitors;
        state.resolved = resolved;
        state.refreshed_at = Some(Instant::now());
        drop(state);

        self.refresh_required.store(false, Ordering::Release);
        Ok(())
    }

    fn refresh_and_get_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        self.refresh_pending()?;
        let state = self.state.read().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("display cache rwlock was poisoned"))
        })?;
        Ok(state.monitors.clone())
    }

    fn start_listener(&self) -> CaptureResult<()> {
        // HWND is not Send, so we transmit the raw pointer as isize.
        let (hwnd_tx, hwnd_rx) = mpsc::channel::<CaptureResult<isize>>();

        // We need a raw pointer to self for the window proc callback.
        // The listener thread will be joined in Drop, so self outlives it.
        let cache_ptr = self as *const Self as usize;

        let join = std::thread::Builder::new()
            .name("snow-display-change-listener".to_string())
            .spawn(move || {
                listener_thread_main(cache_ptr, hwnd_tx);
            })
            .map_err(|e| {
                CaptureError::platform(anyhow::anyhow!(
                    "failed to spawn display change listener thread: {e}"
                ))
            })?;

        let hwnd_raw = hwnd_rx
            .recv()
            .map_err(|_| {
                CaptureError::platform(anyhow::anyhow!(
                    "display change listener thread exited before sending HWND"
                ))
            })?
            .map_err(|e| {
                CaptureError::platform(anyhow::anyhow!(
                    "display change listener failed to create window: {e}"
                ))
            })?;

        let hwnd = HWND(hwnd_raw as *mut std::ffi::c_void);
        self.running.store(true, Ordering::Release);
        *self.listener_hwnd.lock().unwrap() = Some(hwnd);
        *self.listener_join.lock().unwrap() = Some(join);

        Ok(())
    }

    fn stop_listener(&self) {
        self.running.store(false, Ordering::Release);

        let hwnd = {
            let mut guard = match self.listener_hwnd.lock() {
                Ok(g) => g,
                Err(p) => p.into_inner(),
            };
            guard.take()
        };

        if let Some(hwnd) = hwnd {
            unsafe {
                let _ = PostMessageW(Some(hwnd), WM_QUIT_LISTENER, WPARAM(0), LPARAM(0));
            }
        }

        let join = {
            let mut guard = match self.listener_join.lock() {
                Ok(g) => g,
                Err(p) => p.into_inner(),
            };
            guard.take()
        };
        if let Some(join) = join {
            let _ = join.join();
        }
    }
}

impl Drop for DisplayInfoCache {
    fn drop(&mut self) {
        self.stop_listener();
    }
}

/// Class name for our hidden top-level broadcast listener window.
const CLASS_NAME: &str = "SnowCaptureDisplayChangeListener";

fn listener_thread_main(cache_ptr: usize, hwnd_tx: mpsc::Sender<CaptureResult<isize>>) {
    match create_listener_window(cache_ptr) {
        Ok(hwnd) => {
            let _ = hwnd_tx.send(Ok(hwnd.0 as isize));
            run_message_loop();
            unsafe {
                let _ = DestroyWindow(hwnd);
            }
        }
        Err(e) => {
            let _ = hwnd_tx.send(Err(e));
        }
    }
}

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

    // RegisterClass may fail if already registered (e.g. multiple caches);
    // that's fine, we just need the class to exist.
    unsafe {
        RegisterClassW(&wc);
    }

    let hwnd = unsafe {
        CreateWindowExW(
            Default::default(),
            windows::core::PCWSTR(class_name_wide.as_ptr()),
            windows::core::PCWSTR::null(),
            WS_OVERLAPPED,
            0,
            0,
            0,
            0,
            None,
            None,
            Some(hinstance.into()),
            Some(cache_ptr as *const std::ffi::c_void),
        )
    }
    .context("CreateWindowExW for display change listener failed")
    .map_err(CaptureError::platform)?;

    Ok(hwnd)
}

fn run_message_loop() {
    let mut msg = MSG::default();
    unsafe {
        while GetMessageW(&mut msg, None, 0, 0).as_bool() {
            if msg.message == WM_QUIT_LISTENER {
                break;
            }
            let _ = TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

unsafe extern "system" fn display_change_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    match msg {
        WM_DISPLAYCHANGE => {
            let ptr = unsafe {
                windows::Win32::UI::WindowsAndMessaging::GetWindowLongPtrW(
                    hwnd,
                    windows::Win32::UI::WindowsAndMessaging::GWLP_USERDATA,
                )
            };
            if ptr != 0 {
                let cache = unsafe { &*(ptr as *const DisplayInfoCache) };
                // Enumeration is deferred to the next reader. Marking the
                // generation here invalidates capture runtimes even if the
                // eventual refresh fails transiently.
                cache.mark_refresh_required();
            }
            LRESULT(0)
        }
        msg if msg == WM_CLOSE || msg == WM_QUIT_LISTENER => {
            unsafe {
                windows::Win32::UI::WindowsAndMessaging::PostQuitMessage(0);
            }
            LRESULT(0)
        }
        windows::Win32::UI::WindowsAndMessaging::WM_CREATE => {
            let create_struct = unsafe {
                &*(lparam.0 as *const windows::Win32::UI::WindowsAndMessaging::CREATESTRUCTW)
            };
            let cache_ptr = create_struct.lpCreateParams as isize;
            unsafe {
                windows::Win32::UI::WindowsAndMessaging::SetWindowLongPtrW(
                    hwnd,
                    windows::Win32::UI::WindowsAndMessaging::GWLP_USERDATA,
                    cache_ptr,
                );
            }
            unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) }
        }
        _ => unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn empty_cache() -> DisplayInfoCache {
        DisplayInfoCache {
            state: RwLock::new(DisplayCacheState::default()),
            generation: AtomicU64::new(0),
            refresh_required: AtomicBool::new(false),
            refresh_lock: Mutex::new(()),
            listener_hwnd: Mutex::new(None),
            listener_join: Mutex::new(None),
            running: AtomicBool::new(false),
        }
    }

    #[test]
    fn display_change_marks_refresh_pending_and_advances_generation() {
        let cache = empty_cache();

        cache.mark_refresh_required();
        assert!(cache.refresh_required.load(Ordering::Acquire));
        assert_eq!(cache.generation(), 1);

        cache.mark_refresh_required();
        assert!(cache.refresh_required.load(Ordering::Acquire));
        assert_eq!(cache.generation(), 2);
    }
}

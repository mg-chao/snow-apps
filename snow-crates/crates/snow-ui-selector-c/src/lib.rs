use std::cell::RefCell;
use std::ffi::{CString, c_char, c_void};
use std::ptr;
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

#[cfg(windows)]
use snow_ui_selector::{AccessibilityBackend, ElementRegionService, HitTestMode};
#[cfg(windows)]
use windows::Win32::Foundation::{HWND as WindowHandle, POINT as ScreenPoint};
#[cfg(target_os = "macos")]
mod macos;
#[cfg(target_os = "macos")]
use macos::{AccessibilityBackend, ElementRegionService, HitTestMode, ScreenPoint, WindowHandle};

pub struct SnowUiSelectorServiceImpl {
    sender: mpsc::Sender<WorkerRequest>,
    worker: Option<thread::JoinHandle<()>>,
}

pub struct SnowUiSelectorHitPathImpl {
    rects: Vec<SnowUiSelectorRect>,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub enum SnowUiSelectorBackend {
    Uia = 0,
    Msaa = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub enum SnowUiSelectorHitTestMode {
    UiElement = 0,
    Window = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SnowUiSelectorRect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
}

type SnowUiSelectorRefreshCallback =
    unsafe extern "C" fn(request_id: u64, ok: u8, userdata: *mut c_void);

type SnowUiSelectorHitTestPointCallback = unsafe extern "C" fn(
    request_id: u64,
    path: *mut SnowUiSelectorHitPathImpl,
    ok: u8,
    userdata: *mut c_void,
);

enum WorkerRequest {
    Refresh {
        excluded_hwnds: Vec<isize>,
        reply: mpsc::Sender<Result<(), String>>,
    },
    RefreshAsync {
        request_id: u64,
        excluded_hwnds: Vec<isize>,
        callback: SnowUiSelectorRefreshCallback,
        userdata: usize,
    },
    HitTestPoint {
        x: i32,
        y: i32,
        mode: HitTestMode,
        reply: mpsc::Sender<Result<Option<Vec<SnowUiSelectorRect>>, String>>,
    },
    HitTestPointAsync {
        request_id: u64,
        x: i32,
        y: i32,
        mode: HitTestMode,
        callback: SnowUiSelectorHitTestPointCallback,
        userdata: usize,
    },
    ReleaseCache,
    Shutdown,
}

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").expect("empty string is valid C string"));
}

fn sanitize_cstring(value: impl AsRef<str>) -> CString {
    let bytes = value
        .as_ref()
        .as_bytes()
        .iter()
        .copied()
        .filter(|byte| *byte != 0)
        .collect::<Vec<_>>();
    CString::new(bytes).expect("interior NUL bytes were filtered")
}

fn set_last_error(error: impl ToString) {
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = sanitize_cstring(error.to_string());
    });
}

fn clear_last_error() {
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = CString::new("").expect("empty string is valid C string");
    });
}

fn convert_backend(backend: SnowUiSelectorBackend) -> Result<AccessibilityBackend, String> {
    match backend {
        SnowUiSelectorBackend::Uia => Ok(AccessibilityBackend::Uia),
        SnowUiSelectorBackend::Msaa => Ok(AccessibilityBackend::Msaa),
    }
}

fn convert_hit_test_mode(mode: SnowUiSelectorHitTestMode) -> Result<HitTestMode, String> {
    match mode {
        SnowUiSelectorHitTestMode::UiElement => Ok(HitTestMode::UiElement),
        SnowUiSelectorHitTestMode::Window => Ok(HitTestMode::Window),
    }
}

fn service_ref<'a>(
    service: *const SnowUiSelectorServiceImpl,
) -> Option<&'a SnowUiSelectorServiceImpl> {
    if service.is_null() {
        set_last_error("service is null");
        None
    } else {
        Some(unsafe { &*service })
    }
}

fn hit_path_ref<'a>(
    path: *const SnowUiSelectorHitPathImpl,
) -> Option<&'a SnowUiSelectorHitPathImpl> {
    if path.is_null() {
        set_last_error("hit path is null");
        None
    } else {
        Some(unsafe { &*path })
    }
}

fn worker_call<T>(
    service: &SnowUiSelectorServiceImpl,
    make_request: impl FnOnce(mpsc::Sender<Result<T, String>>) -> WorkerRequest,
) -> Result<T, String>
where
    T: Send + 'static,
{
    worker_call_sender(service.sender.clone(), make_request, None)
}

fn worker_call_sender<T>(
    sender: mpsc::Sender<WorkerRequest>,
    make_request: impl FnOnce(mpsc::Sender<Result<T, String>>) -> WorkerRequest,
    timeout: Option<Duration>,
) -> Result<T, String>
where
    T: Send + 'static,
{
    let (reply, receiver) = mpsc::channel();
    sender
        .send(make_request(reply))
        .map_err(|_| "selector worker is not available".to_owned())?;
    match timeout {
        Some(timeout) => receiver
            .recv_timeout(timeout)
            .map_err(|error| match error {
                mpsc::RecvTimeoutError::Timeout => "selector request timed out".to_owned(),
                mpsc::RecvTimeoutError::Disconnected => {
                    "selector worker closed without replying".to_owned()
                }
            })?,
        None => receiver
            .recv()
            .map_err(|_| "selector worker closed without replying".to_owned())?,
    }
}

fn refresh_service(
    service: &mut Option<ElementRegionService>,
    backend: AccessibilityBackend,
    excluded_hwnds: Vec<isize>,
) -> Result<(), String> {
    let hwnds = excluded_hwnds
        .into_iter()
        .map(|raw| WindowHandle(raw as *mut c_void))
        .collect::<Vec<_>>();

    if let Some(existing) = service
        .as_mut()
        .filter(|service| service.backend() == backend)
    {
        let result = existing
            .refresh_excluding_hwnds(&hwnds)
            .map_err(|error| error.to_string());
        if result.is_err() {
            *service = None;
        }
        return result;
    }

    let refreshed = ElementRegionService::with_backend_excluding_hwnds(backend, &hwnds)
        .map_err(|error| error.to_string())?;
    *service = Some(refreshed);
    Ok(())
}

fn hit_test_service(
    service: &mut Option<ElementRegionService>,
    x: i32,
    y: i32,
    mode: HitTestMode,
) -> Result<Option<Vec<SnowUiSelectorRect>>, String> {
    service
        .as_mut()
        .ok_or_else(|| "selector service has not been refreshed".to_owned())
        .and_then(|service| {
            service
                .hit_test_point(ScreenPoint { x, y }, mode)
                .map(|path| {
                    path.map(|rects| {
                        rects
                            .into_iter()
                            .map(|rect| SnowUiSelectorRect {
                                left: rect.left(),
                                top: rect.top(),
                                right: rect.right(),
                                bottom: rect.bottom(),
                            })
                            .collect::<Vec<_>>()
                    })
                })
                .map_err(|error| error.to_string())
        })
}

fn run_worker(backend: AccessibilityBackend, receiver: mpsc::Receiver<WorkerRequest>) {
    let mut service: Option<ElementRegionService> = None;
    while let Ok(request) = receiver.recv() {
        match request {
            WorkerRequest::Refresh {
                excluded_hwnds,
                reply,
            } => {
                let result = refresh_service(&mut service, backend, excluded_hwnds);
                let _ = reply.send(result);
            }
            WorkerRequest::RefreshAsync {
                request_id,
                excluded_hwnds,
                callback,
                userdata,
            } => {
                let result = refresh_service(&mut service, backend, excluded_hwnds);
                unsafe {
                    callback(
                        request_id,
                        u8::from(result.is_ok()),
                        userdata as *mut c_void,
                    );
                }
            }
            WorkerRequest::HitTestPoint { x, y, mode, reply } => {
                let result = hit_test_service(&mut service, x, y, mode);
                let _ = reply.send(result);
            }
            WorkerRequest::HitTestPointAsync {
                request_id,
                x,
                y,
                mode,
                callback,
                userdata,
            } => {
                let result = hit_test_service(&mut service, x, y, mode);
                let (path, ok) = match result {
                    Ok(Some(rects)) => (
                        Box::into_raw(Box::new(SnowUiSelectorHitPathImpl { rects })),
                        1,
                    ),
                    Ok(None) => (ptr::null_mut(), 1),
                    Err(_) => (ptr::null_mut(), 0),
                };
                unsafe {
                    callback(request_id, path, ok, userdata as *mut c_void);
                }
            }
            WorkerRequest::ReleaseCache => {
                if let Some(service) = service.as_mut() {
                    service.release_cache();
                }
            }
            WorkerRequest::Shutdown => break,
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_ui_selector_service_create(
    backend: SnowUiSelectorBackend,
) -> *mut SnowUiSelectorServiceImpl {
    let backend = match convert_backend(backend) {
        Ok(backend) => backend,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };

    let (sender, receiver) = mpsc::channel();
    let worker = match thread::Builder::new()
        .name("snow-ui-selector".to_owned())
        .spawn(move || run_worker(backend, receiver))
    {
        Ok(worker) => worker,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };

    clear_last_error();
    Box::into_raw(Box::new(SnowUiSelectorServiceImpl {
        sender,
        worker: Some(worker),
    }))
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `service` must be null or a pointer returned by `snow_ui_selector_service_create` that has not
/// already been destroyed. The pointer must not be used after this call.
pub unsafe extern "C" fn snow_ui_selector_service_destroy(service: *mut SnowUiSelectorServiceImpl) {
    if service.is_null() {
        return;
    }
    let mut service = unsafe { Box::from_raw(service) };
    let _ = service.sender.send(WorkerRequest::Shutdown);
    if let Some(worker) = service.worker.take() {
        let _ = worker.join();
    }
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `service` must point to a live selector service. The cache release is
/// queued on the selector worker and does not destroy the service itself.
pub unsafe extern "C" fn snow_ui_selector_service_release_cache(
    service: *mut SnowUiSelectorServiceImpl,
) -> u8 {
    let Some(service) = service_ref(service) else {
        return 0;
    };

    match service.sender.send(WorkerRequest::ReleaseCache) {
        Ok(()) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `service` must point to a live selector service. When `excluded_count` is nonzero,
/// `excluded_hwnds` must reference an array containing at least that many entries.
pub unsafe extern "C" fn snow_ui_selector_service_refresh(
    service: *mut SnowUiSelectorServiceImpl,
    excluded_hwnds: *const usize,
    excluded_count: usize,
) -> u8 {
    if excluded_hwnds.is_null() && excluded_count > 0 {
        set_last_error("excluded_hwnds is null");
        return 0;
    }
    let Some(service) = service_ref(service) else {
        return 0;
    };

    let excluded = if excluded_count == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(excluded_hwnds, excluded_count) }
            .iter()
            .map(|hwnd| *hwnd as isize)
            .collect::<Vec<_>>()
    };

    match worker_call(service, |reply| WorkerRequest::Refresh {
        excluded_hwnds: excluded,
        reply,
    }) {
        Ok(()) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_ui_selector_service_hit_test_point(
    service: *mut SnowUiSelectorServiceImpl,
    x: i32,
    y: i32,
    mode: SnowUiSelectorHitTestMode,
) -> *mut SnowUiSelectorHitPathImpl {
    let mode = match convert_hit_test_mode(mode) {
        Ok(mode) => mode,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let Some(service) = service_ref(service) else {
        return ptr::null_mut();
    };

    match worker_call(service, |reply| WorkerRequest::HitTestPoint {
        x,
        y,
        mode,
        reply,
    }) {
        Ok(Some(rects)) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowUiSelectorHitPathImpl { rects }))
        }
        Ok(None) => {
            clear_last_error();
            ptr::null_mut()
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `service` must point to a live selector service. When `excluded_count` is nonzero,
/// `excluded_hwnds` must reference an array containing at least that many entries. `userdata` must
/// remain valid according to the callback's contract.
pub unsafe extern "C" fn snow_ui_selector_service_refresh_async(
    service: *mut SnowUiSelectorServiceImpl,
    request_id: u64,
    excluded_hwnds: *const usize,
    excluded_count: usize,
    callback: Option<SnowUiSelectorRefreshCallback>,
    userdata: *mut c_void,
) -> u8 {
    let Some(callback) = callback else {
        set_last_error("callback is null");
        return 0;
    };
    if excluded_hwnds.is_null() && excluded_count > 0 {
        set_last_error("excluded_hwnds is null");
        return 0;
    }
    let Some(service) = service_ref(service) else {
        return 0;
    };

    let excluded = if excluded_count == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(excluded_hwnds, excluded_count) }
            .iter()
            .map(|hwnd| *hwnd as isize)
            .collect::<Vec<_>>()
    };
    let userdata = userdata as usize;

    match service.sender.send(WorkerRequest::RefreshAsync {
        request_id,
        excluded_hwnds: excluded,
        callback,
        userdata,
    }) {
        Ok(()) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `service` must point to a live selector service. `userdata` must remain valid according to the
/// callback's contract.
pub unsafe extern "C" fn snow_ui_selector_service_hit_test_point_async(
    service: *mut SnowUiSelectorServiceImpl,
    request_id: u64,
    x: i32,
    y: i32,
    mode: SnowUiSelectorHitTestMode,
    callback: Option<SnowUiSelectorHitTestPointCallback>,
    userdata: *mut c_void,
) -> u8 {
    let Some(callback) = callback else {
        set_last_error("callback is null");
        return 0;
    };
    let mode = match convert_hit_test_mode(mode) {
        Ok(mode) => mode,
        Err(error) => {
            set_last_error(error);
            return 0;
        }
    };
    let Some(service) = service_ref(service) else {
        return 0;
    };
    let userdata = userdata as usize;

    match service.sender.send(WorkerRequest::HitTestPointAsync {
        request_id,
        x,
        y,
        mode,
        callback,
        userdata,
    }) {
        Ok(()) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_ui_selector_hit_path_count(path: *const SnowUiSelectorHitPathImpl) -> usize {
    hit_path_ref(path).map_or(0, |path| path.rects.len())
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must point to a live hit path returned by this library, and `out_rect` must be valid for
/// one writable `SnowUiSelectorRect`.
pub unsafe extern "C" fn snow_ui_selector_hit_path_rect(
    path: *const SnowUiSelectorHitPathImpl,
    index: usize,
    out_rect: *mut SnowUiSelectorRect,
) -> u8 {
    if out_rect.is_null() {
        set_last_error("out_rect is null");
        return 0;
    }
    let Some(path) = hit_path_ref(path) else {
        return 0;
    };
    let Some(rect) = path.rects.get(index) else {
        set_last_error("hit path index is out of range");
        return 0;
    };
    unsafe {
        *out_rect = *rect;
    }
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
/// # Safety
///
/// `path` must be null or a pointer returned by this library that has not already been destroyed.
/// The pointer must not be used after this call.
pub unsafe extern "C" fn snow_ui_selector_hit_path_destroy(path: *mut SnowUiSelectorHitPathImpl) {
    if !path.is_null() {
        drop(unsafe { Box::from_raw(path) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_ui_selector_last_error_message() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

    fn last_error_bytes() -> Vec<u8> {
        unsafe { CStr::from_ptr(snow_ui_selector_last_error_message()) }
            .to_bytes()
            .to_vec()
    }

    #[test]
    fn null_service_refresh_fails() {
        let ok = unsafe { snow_ui_selector_service_refresh(ptr::null_mut(), ptr::null(), 0) };
        assert_eq!(ok, 0);
        assert!(!last_error_bytes().is_empty());
    }

    #[test]
    fn null_service_release_cache_fails() {
        let ok = unsafe { snow_ui_selector_service_release_cache(ptr::null_mut()) };
        assert_eq!(ok, 0);
        assert!(!last_error_bytes().is_empty());
    }

    #[test]
    fn null_service_hit_test_fails() {
        let path = snow_ui_selector_service_hit_test_point(
            ptr::null_mut(),
            0,
            0,
            SnowUiSelectorHitTestMode::UiElement,
        );
        assert!(path.is_null());
        assert!(!last_error_bytes().is_empty());
    }

    #[test]
    fn async_refresh_rejects_null_callback() {
        let ok = unsafe {
            snow_ui_selector_service_refresh_async(
                ptr::null_mut(),
                1,
                ptr::null(),
                0,
                None,
                ptr::null_mut(),
            )
        };
        assert_eq!(ok, 0);
        assert!(!last_error_bytes().is_empty());
    }

    #[test]
    fn async_hit_test_rejects_null_callback() {
        let ok = unsafe {
            snow_ui_selector_service_hit_test_point_async(
                ptr::null_mut(),
                1,
                0,
                0,
                SnowUiSelectorHitTestMode::UiElement,
                None,
                ptr::null_mut(),
            )
        };
        assert_eq!(ok, 0);
        assert!(!last_error_bytes().is_empty());
    }

    #[test]
    fn hit_path_rect_reports_owned_rects() {
        let path = Box::into_raw(Box::new(SnowUiSelectorHitPathImpl {
            rects: vec![SnowUiSelectorRect {
                left: 1,
                top: 2,
                right: 30,
                bottom: 40,
            }],
        }));

        assert_eq!(snow_ui_selector_hit_path_count(path), 1);
        let mut rect = SnowUiSelectorRect::default();
        let ok = unsafe { snow_ui_selector_hit_path_rect(path, 0, &mut rect) };
        assert_eq!(ok, 1);
        assert_eq!(
            rect,
            SnowUiSelectorRect {
                left: 1,
                top: 2,
                right: 30,
                bottom: 40,
            }
        );
        unsafe {
            snow_ui_selector_hit_path_destroy(path);
        }
    }

    #[test]
    fn hit_path_rect_rejects_null_output() {
        let path = SnowUiSelectorHitPathImpl { rects: Vec::new() };
        let ok = unsafe { snow_ui_selector_hit_path_rect(&path, 0, ptr::null_mut()) };
        assert_eq!(ok, 0);
        assert!(!last_error_bytes().is_empty());
    }
}

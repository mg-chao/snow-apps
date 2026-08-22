#![cfg(target_os = "windows")]

use std::ffi::c_void;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use snow_capture::backend::CaptureBackendKind;
use snow_capture::frame::Frame;
use snow_capture::{CaptureOptions, CaptureRegion, CaptureSystem, CaptureTarget, WgcUpdateMode};
use windows::Win32::Foundation::{COLORREF, HINSTANCE, HWND, LPARAM, LRESULT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    BeginPaint, CreateSolidBrush, DeleteObject, EndPaint, FillRect, PAINTSTRUCT, UpdateWindow,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::HiDpi::{
    DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetThreadDpiAwarenessContext,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CREATESTRUCTW, CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, GWLP_USERDATA,
    GetMessageW, GetWindowRect, MSG, PostMessageW, PostQuitMessage, RegisterClassW, SW_INVALIDATE,
    ScrollWindowEx, SetCursorPos, SetWindowLongPtrW, ShowWindow, TranslateMessage, WM_APP,
    WM_CLOSE, WM_DESTROY, WM_NCCREATE, WM_PAINT, WNDCLASSW, WS_EX_TOOLWINDOW, WS_EX_TOPMOST,
    WS_POPUP, WS_VISIBLE,
};
use windows::core::PCWSTR;

const WINDOW_WIDTH: i32 = 320;
const WINDOW_HEIGHT: i32 = 240;
const WINDOW_X: i32 = 96;
const WINDOW_Y: i32 = 96;
const BAND_HEIGHT: i32 = 8;
const SCROLL_ROWS: i32 = 24;
const WM_SCROLL_TEST_WINDOW: u32 = WM_APP + 41;
const PIXEL_TOLERANCE: u8 = 3;
const REQUIRED_SHIFT_MATCH: f64 = 0.97;
const MAX_STALE_MATCH: f64 = 0.35;
static NEXT_WINDOW_CLASS_ID: AtomicU64 = AtomicU64::new(1);

struct ScrollWindowState {
    logical_top: i32,
    scroll_completed: mpsc::Sender<()>,
}

struct ScrollWindow {
    raw_handle: isize,
    scroll_completed: mpsc::Receiver<()>,
    thread: Option<JoinHandle<()>>,
}

struct ThreadDpiAwareness(DPI_AWARENESS_CONTEXT);

impl ThreadDpiAwareness {
    fn per_monitor_v2() -> Self {
        Self(unsafe { SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) })
    }
}

impl Drop for ThreadDpiAwareness {
    fn drop(&mut self) {
        unsafe {
            SetThreadDpiAwarenessContext(self.0);
        }
    }
}

impl ScrollWindow {
    fn spawn() -> Self {
        let (handle_tx, handle_rx) = mpsc::sync_channel(1);
        let (scroll_tx, scroll_rx) = mpsc::channel();
        let thread = thread::spawn(move || {
            let _dpi_awareness = ThreadDpiAwareness::per_monitor_v2();
            let instance = unsafe { GetModuleHandleW(None).expect("module handle should resolve") };
            let class_id = NEXT_WINDOW_CLASS_ID.fetch_add(1, Ordering::Relaxed);
            let class_name: Vec<u16> = format!(
                "SnowCaptureWgcScrollWindow{}_{}\0",
                std::process::id(),
                class_id
            )
            .encode_utf16()
            .collect();
            let class_name = PCWSTR(class_name.as_ptr());
            let class = WNDCLASSW {
                hInstance: HINSTANCE(instance.0),
                lpszClassName: class_name,
                lpfnWndProc: Some(scroll_window_proc),
                ..Default::default()
            };
            assert_ne!(unsafe { RegisterClassW(&class) }, 0);

            let state = Box::into_raw(Box::new(ScrollWindowState {
                logical_top: 0,
                scroll_completed: scroll_tx,
            }));
            let hwnd = unsafe {
                CreateWindowExW(
                    WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                    class_name,
                    class_name,
                    WS_POPUP | WS_VISIBLE,
                    WINDOW_X,
                    WINDOW_Y,
                    WINDOW_WIDTH,
                    WINDOW_HEIGHT,
                    None,
                    None,
                    Some(HINSTANCE(instance.0)),
                    Some(state.cast::<c_void>()),
                )
                .expect("scroll test window should be created")
            };
            unsafe {
                let _ = ShowWindow(hwnd, windows::Win32::UI::WindowsAndMessaging::SW_SHOW);
                let _ = UpdateWindow(hwnd);
            }
            handle_tx
                .send(hwnd.0 as isize)
                .expect("scroll test handle should be delivered");

            let mut message = MSG::default();
            while unsafe { GetMessageW(&mut message, None, 0, 0) }.into() {
                unsafe {
                    let _ = TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        });

        let raw_handle = handle_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("scroll test window should become ready");
        thread::sleep(Duration::from_millis(150));
        Self {
            raw_handle,
            scroll_completed: scroll_rx,
            thread: Some(thread),
        }
    }

    fn scroll_once(&self) {
        let hwnd = HWND(self.raw_handle as *mut c_void);
        unsafe { PostMessageW(Some(hwnd), WM_SCROLL_TEST_WINDOW, WPARAM(0), LPARAM(0)) }
            .expect("scroll command should post");
        self.scroll_completed
            .recv_timeout(Duration::from_secs(2))
            .expect("scroll command should repaint the exposed strip");
    }

    fn client_region(&self) -> CaptureRegion {
        let hwnd = HWND(self.raw_handle as *mut c_void);
        let mut window = RECT::default();
        unsafe { GetWindowRect(hwnd, &mut window) }.expect("test window rect should resolve");
        CaptureRegion::new(
            window.left,
            window.top,
            (window.right - window.left) as u32,
            (window.bottom - window.top) as u32,
        )
        .expect("test window should have a nonempty client region")
    }
}

impl Drop for ScrollWindow {
    fn drop(&mut self) {
        let hwnd = HWND(self.raw_handle as *mut c_void);
        let _ = unsafe { PostMessageW(Some(hwnd), WM_CLOSE, WPARAM(0), LPARAM(0)) };
        if let Some(thread) = self.thread.take() {
            thread
                .join()
                .expect("scroll test window thread should exit");
        }
    }
}

extern "system" fn scroll_window_proc(
    hwnd: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    match message {
        WM_NCCREATE => unsafe {
            let create = &*(lparam.0 as *const CREATESTRUCTW);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, create.lpCreateParams as isize);
            LRESULT(1)
        },
        WM_SCROLL_TEST_WINDOW => unsafe {
            if let Some(state) = scroll_window_state(hwnd) {
                state.logical_top += SCROLL_ROWS;
                let mut update = RECT::default();
                ScrollWindowEx(
                    hwnd,
                    0,
                    -SCROLL_ROWS,
                    None,
                    None,
                    None,
                    Some(&mut update),
                    SW_INVALIDATE,
                );
                let _ = UpdateWindow(hwnd);
                let _ = state.scroll_completed.send(());
            }
            LRESULT(0)
        },
        WM_PAINT => unsafe {
            let mut paint = PAINTSTRUCT::default();
            let dc = BeginPaint(hwnd, &mut paint);
            if let Some(state) = scroll_window_state(hwnd) {
                paint_pattern(dc, paint.rcPaint, state.logical_top);
            }
            let _ = EndPaint(hwnd, &paint);
            LRESULT(0)
        },
        WM_CLOSE => unsafe {
            DestroyWindow(hwnd).expect("scroll test window should be destroyed");
            LRESULT(0)
        },
        WM_DESTROY => unsafe {
            let state = SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if state != 0 {
                drop(Box::from_raw(state as *mut ScrollWindowState));
            }
            PostQuitMessage(0);
            LRESULT(0)
        },
        _ => unsafe { DefWindowProcW(hwnd, message, wparam, lparam) },
    }
}

unsafe fn scroll_window_state(hwnd: HWND) -> Option<&'static mut ScrollWindowState> {
    use windows::Win32::UI::WindowsAndMessaging::GetWindowLongPtrW;
    let state = unsafe { GetWindowLongPtrW(hwnd, GWLP_USERDATA) } as *mut ScrollWindowState;
    unsafe { state.as_mut() }
}

unsafe fn paint_pattern(dc: windows::Win32::Graphics::Gdi::HDC, paint: RECT, logical_top: i32) {
    let mut top = paint.top.max(0);
    let bottom = paint.bottom.min(WINDOW_HEIGHT);
    while top < bottom {
        let logical_y = logical_top + top;
        let band = logical_y.div_euclid(BAND_HEIGHT);
        let next_band = ((band + 1) * BAND_HEIGHT - logical_top).min(bottom);
        let rect = RECT {
            left: paint.left.max(0),
            top,
            right: paint.right.min(WINDOW_WIDTH),
            bottom: next_band,
        };
        let brush = unsafe { CreateSolidBrush(pattern_color(band)) };
        unsafe {
            FillRect(dc, &rect, brush);
            let _ = DeleteObject(brush.into());
        }
        top = next_band;
    }
}

fn pattern_color(band: i32) -> COLORREF {
    let band = band as u32;
    let red = 32 + (band.wrapping_mul(53) % 192);
    let green = 32 + (band.wrapping_mul(97) % 192);
    let blue = 32 + (band.wrapping_mul(149) % 192);
    COLORREF(red | (green << 8) | (blue << 16))
}

fn shifted_match_ratio(before: &Frame, after: &Frame, rows: usize) -> f64 {
    assert_eq!(before.dimensions(), after.dimensions());
    let width = before.width() as usize;
    let height = before.height() as usize;
    let margin_x = 8usize;
    let margin_y = 8usize;
    let mut matching = 0usize;
    let mut total = 0usize;
    for y in margin_y..height - rows - margin_y {
        for x in margin_x..width - margin_x {
            let before_offset = ((y + rows) * width + x) * 4;
            let after_offset = (y * width + x) * 4;
            if pixels_match(
                &before.as_rgba_bytes()[before_offset..before_offset + 3],
                &after.as_rgba_bytes()[after_offset..after_offset + 3],
            ) {
                matching += 1;
            }
            total += 1;
        }
    }
    matching as f64 / total as f64
}

fn same_position_match_ratio(before: &Frame, after: &Frame, rows: usize) -> f64 {
    let width = before.width() as usize;
    let height = before.height() as usize;
    let margin_x = 8usize;
    let margin_y = 8usize;
    let mut matching = 0usize;
    let mut total = 0usize;
    for y in margin_y..height - rows - margin_y {
        for x in margin_x..width - margin_x {
            let offset = (y * width + x) * 4;
            if pixels_match(
                &before.as_rgba_bytes()[offset..offset + 3],
                &after.as_rgba_bytes()[offset..offset + 3],
            ) {
                matching += 1;
            }
            total += 1;
        }
    }
    matching as f64 / total as f64
}

fn pixels_match(left: &[u8], right: &[u8]) -> bool {
    left.iter()
        .zip(right)
        .all(|(left, right)| left.abs_diff(*right) <= PIXEL_TOLERANCE)
}

fn assert_hot_session_scroll(mode: WgcUpdateMode) {
    let _dpi_awareness = ThreadDpiAwareness::per_monitor_v2();
    let window = ScrollWindow::spawn();
    let region = window.client_region();
    let _ = unsafe { SetCursorPos(region.x - 32, region.y - 32) };
    let system = CaptureSystem::builder()
        .with_backend_kind(CaptureBackendKind::WindowsGraphicsCapture)
        .build()
        .expect("WGC capture system should initialize");
    let mut session = system
        .open_session(
            CaptureTarget::Region(region),
            CaptureOptions {
                wgc_update_mode: mode,
                ..CaptureOptions::default()
            },
        )
        .expect("WGC scroll capture session should open");
    let mut before = Frame::empty();
    session
        .capture_into(&mut before)
        .expect("baseline WGC frame should capture");
    assert_eq!(before.width(), region.width);
    assert_eq!(before.height(), region.height);
    let physical_scroll_rows =
        ((SCROLL_ROWS as u64 * before.height() as u64).div_ceil(region.height as u64)) as usize;

    window.scroll_once();
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut after = before.clone();
    let mut best_shifted = 0.0f64;
    let mut stale_at_best = 1.0f64;
    let mut observed_ordered_damage = false;
    while Instant::now() < deadline {
        session
            .capture_into(&mut after)
            .expect("post-scroll WGC frame should capture");
        observed_ordered_damage |= !after.metadata().dirty_rects().is_empty();
        let shifted = shifted_match_ratio(&before, &after, physical_scroll_rows);
        if shifted > best_shifted {
            best_shifted = shifted;
            stale_at_best = same_position_match_ratio(&before, &after, physical_scroll_rows);
        }
        let contract_exercised =
            mode != WgcUpdateMode::OrderedIncremental || observed_ordered_damage;
        if shifted >= REQUIRED_SHIFT_MATCH && stale_at_best <= MAX_STALE_MATCH && contract_exercised
        {
            return;
        }
        thread::sleep(Duration::from_millis(5));
    }

    panic!(
        "{mode:?} never produced a complete translated WGC region frame with the requested contract; best shifted match was {best_shifted:.3}, stale same-position pixels matched {stale_at_best:.3}, ordered damage observed={observed_ordered_damage}"
    );
}

#[test]
#[ignore = "requires an interactive Windows desktop and WGC"]
fn wgc_hot_snapshot_scroll_updates_the_entire_frame() {
    assert_hot_session_scroll(WgcUpdateMode::CompleteOnly);
    assert_hot_session_scroll(WgcUpdateMode::OrderedIncremental);
}

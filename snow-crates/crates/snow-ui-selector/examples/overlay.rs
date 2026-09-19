use std::ffi::c_void;

use snow_ui_selector::{AccessibilityBackend, ElementRegionService, HitTestMode, QueryControl};
use windows::Win32::Foundation::{COLORREF, HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    BeginPaint, CreatePen, CreateSolidBrush, DeleteObject, EndPaint, FillRect, GetStockObject,
    HOLLOW_BRUSH, InvalidateRect, PAINTSTRUCT, PS_SOLID, Rectangle, SelectObject,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::Input::KeyboardAndMouse::{GetAsyncKeyState, VK_ESCAPE, VK_W};
use windows::Win32::UI::WindowsAndMessaging::{
    CREATESTRUCTW, CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, GWLP_USERDATA,
    GetCursorPos, GetMessageW, GetSystemMetrics, GetWindowLongPtrW, KillTimer, LWA_COLORKEY, MSG,
    PostQuitMessage, RegisterClassW, SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN, SM_XVIRTUALSCREEN,
    SM_YVIRTUALSCREEN, SetLayeredWindowAttributes, SetTimer, SetWindowLongPtrW, TranslateMessage,
    WM_CREATE, WM_DESTROY, WM_NCCREATE, WM_PAINT, WM_TIMER, WNDCLASSW, WS_EX_LAYERED,
    WS_EX_TOOLWINDOW, WS_EX_TOPMOST, WS_EX_TRANSPARENT, WS_POPUP, WS_VISIBLE,
};
use windows::core::w;

const TIMER_ID: usize = 1;
const TIMER_INTERVAL_MS: u32 = 30;
const OUTLINE_WIDTH: i32 = 2;
const TRANSPARENT_KEY: COLORREF = COLORREF(0);
const OUTLINE_COLOR: COLORREF = COLORREF(0x0000_FF00);

struct AppState {
    selector: ElementRegionService,
    current_rect: Option<RECT>,
    virtual_left: i32,
    virtual_top: i32,
    hit_test_mode: HitTestMode,
    /// Tracks whether the W key was held on the previous tick to detect edges.
    w_key_was_down: bool,
}

fn main() -> snow_ui_selector::SelectorResult<()> {
    let backend = parse_backend_from_args();
    run_overlay(backend)
}

fn run_overlay(backend: AccessibilityBackend) -> snow_ui_selector::SelectorResult<()> {
    let mut selector = ElementRegionService::with_backend(backend)?;
    selector.refresh()?;

    let virtual_left = unsafe { GetSystemMetrics(SM_XVIRTUALSCREEN) };
    let virtual_top = unsafe { GetSystemMetrics(SM_YVIRTUALSCREEN) };
    let virtual_width = unsafe { GetSystemMetrics(SM_CXVIRTUALSCREEN) };
    let virtual_height = unsafe { GetSystemMetrics(SM_CYVIRTUALSCREEN) };

    let initial_mode = parse_mode_from_args();

    let state = Box::new(AppState {
        selector,
        current_rect: None,
        virtual_left,
        virtual_top,
        hit_test_mode: initial_mode,
        w_key_was_down: false,
    });
    let state_ptr = Box::into_raw(state);

    let instance = unsafe { GetModuleHandleW(None)? };
    let class_name = w!("UiHitTestOverlayWindow");

    let class = WNDCLASSW {
        hInstance: instance.into(),
        lpszClassName: class_name,
        lpfnWndProc: Some(window_proc),
        ..Default::default()
    };
    unsafe {
        RegisterClassW(&class);
    }

    let hwnd_result = unsafe {
        CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            class_name,
            w!("UI Hit Test Overlay"),
            WS_POPUP | WS_VISIBLE,
            virtual_left,
            virtual_top,
            virtual_width,
            virtual_height,
            None,
            None,
            Some(HINSTANCE(instance.0)),
            Some(state_ptr as *const c_void),
        )
    };
    let hwnd = match hwnd_result {
        Ok(hwnd) => hwnd,
        Err(err) => {
            unsafe {
                drop(Box::from_raw(state_ptr));
            }
            return Err(err.into());
        }
    };

    unsafe {
        SetLayeredWindowAttributes(hwnd, TRANSPARENT_KEY, 0, LWA_COLORKEY)?;
    }

    let mut message = MSG::default();
    while unsafe { GetMessageW(&mut message, None, 0, 0) }.into() {
        unsafe {
            let _ = TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    Ok(())
}

fn parse_backend_from_args() -> AccessibilityBackend {
    let mut args = std::env::args().skip(1);

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--backend" | "-b" => {
                let Some(value) = args.next() else {
                    eprintln!("Missing value for --backend.");
                    print_usage();
                    std::process::exit(2);
                };

                if value.eq_ignore_ascii_case("msaa") {
                    return AccessibilityBackend::Msaa;
                }
                if value.eq_ignore_ascii_case("uia") {
                    return AccessibilityBackend::Uia;
                }

                eprintln!("Invalid backend '{value}'. Expected 'uia' or 'msaa'.");
                print_usage();
                std::process::exit(2);
            }
            "--msaa" => return AccessibilityBackend::Msaa,
            "--uia" => return AccessibilityBackend::Uia,
            "--help" | "-h" => {
                print_usage();
                std::process::exit(0);
            }
            _ => {}
        }
    }

    AccessibilityBackend::Uia
}

fn parse_mode_from_args() -> HitTestMode {
    let mut args = std::env::args().skip(1);

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--mode" | "-m" => {
                let Some(value) = args.next() else {
                    eprintln!("Missing value for --mode.");
                    print_usage();
                    std::process::exit(2);
                };

                if value.eq_ignore_ascii_case("window") {
                    return HitTestMode::Window;
                }
                if value.eq_ignore_ascii_case("element") {
                    return HitTestMode::UiElement;
                }

                eprintln!("Invalid mode '{value}'. Expected 'element' or 'window'.");
                print_usage();
                std::process::exit(2);
            }
            "--window" => return HitTestMode::Window,
            _ => {}
        }
    }

    HitTestMode::UiElement
}

fn print_usage() {
    eprintln!(
        "Usage: overlay [--backend uia|msaa] [--uia|--msaa] [--mode element|window] [--window]"
    );
    eprintln!("       Press W to toggle window traversal mode at runtime.");
}

extern "system" fn window_proc(hwnd: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    match msg {
        WM_NCCREATE => unsafe {
            let create = &*(lparam.0 as *const CREATESTRUCTW);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, create.lpCreateParams as isize);
            LRESULT(1)
        },
        WM_CREATE => unsafe {
            SetTimer(Some(hwnd), TIMER_ID, TIMER_INTERVAL_MS, None);
            LRESULT(0)
        },
        WM_TIMER => unsafe {
            if let Some(state) = state_mut(hwnd) {
                // Toggle window traversal mode on W key press (edge-triggered).
                let w_down = GetAsyncKeyState(VK_W.0 as i32) < 0;
                if w_down && !state.w_key_was_down {
                    state.hit_test_mode = match state.hit_test_mode {
                        HitTestMode::UiElement => {
                            eprintln!("[overlay] switched to Window traversal mode");
                            HitTestMode::Window
                        }
                        HitTestMode::Window => {
                            eprintln!("[overlay] switched to UiElement traversal mode");
                            HitTestMode::UiElement
                        }
                    };
                    // Force a redraw so the outline updates immediately.
                    state.current_rect = None;
                    let _ = InvalidateRect(Some(hwnd), None, true);
                }
                state.w_key_was_down = w_down;

                let latest_rect = {
                    let mut cursor = POINT::default();
                    if GetCursorPos(&mut cursor).is_ok() {
                        state
                            .selector
                            .query(
                                snow_ui_selector::Point {
                                    x: cursor.x,
                                    y: cursor.y,
                                    display_id: 0,
                                },
                                state.hit_test_mode,
                                &QueryControl::foreground(),
                                &mut |_| {},
                            )
                            .ok()
                            .and_then(|result| result.path)
                            .and_then(|regions| {
                                regions.first().map(|el| RECT {
                                    left: el.left(),
                                    top: el.top(),
                                    right: el.right(),
                                    bottom: el.bottom(),
                                })
                            })
                    } else {
                        None
                    }
                };

                if rect_changed(state.current_rect, latest_rect) {
                    state.current_rect = latest_rect;
                    let _ = InvalidateRect(Some(hwnd), None, true);
                }
            }

            if GetAsyncKeyState(VK_ESCAPE.0 as i32) < 0 {
                DestroyWindow(hwnd).ok();
            }

            LRESULT(0)
        },
        WM_PAINT => unsafe {
            let mut paint = PAINTSTRUCT::default();
            let dc = BeginPaint(hwnd, &mut paint);

            let clear_brush = CreateSolidBrush(TRANSPARENT_KEY);
            FillRect(dc, &paint.rcPaint, clear_brush);
            let _ = DeleteObject(clear_brush.into());

            if let Some(state) = state_mut(hwnd)
                && let Some(rect) = state.current_rect
            {
                let left = rect.left - state.virtual_left;
                let top = rect.top - state.virtual_top;
                let right = rect.right - state.virtual_left;
                let bottom = rect.bottom - state.virtual_top;

                let outline_pen = CreatePen(PS_SOLID, OUTLINE_WIDTH, OUTLINE_COLOR);
                let old_pen = SelectObject(dc, outline_pen.into());
                let old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));

                let _ = Rectangle(dc, left, top, right, bottom);

                SelectObject(dc, old_brush);
                SelectObject(dc, old_pen);
                let _ = DeleteObject(outline_pen.into());
            }

            let _ = EndPaint(hwnd, &paint);
            LRESULT(0)
        },
        WM_DESTROY => unsafe {
            let _ = KillTimer(Some(hwnd), TIMER_ID);

            let ptr = SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if ptr != 0 {
                drop(Box::from_raw(ptr as *mut AppState));
            }

            PostQuitMessage(0);
            LRESULT(0)
        },
        _ => unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) },
    }
}

unsafe fn state_mut(hwnd: HWND) -> Option<&'static mut AppState> {
    let ptr = unsafe { GetWindowLongPtrW(hwnd, GWLP_USERDATA) } as *mut AppState;
    unsafe { ptr.as_mut() }
}

fn rect_changed(left: Option<RECT>, right: Option<RECT>) -> bool {
    left.map(rect_tuple) != right.map(rect_tuple)
}

fn rect_tuple(rect: RECT) -> (i32, i32, i32, i32) {
    (rect.left, rect.top, rect.right, rect.bottom)
}

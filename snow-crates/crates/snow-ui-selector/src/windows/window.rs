use std::ffi::c_void;
use std::mem;

use windows::Win32::Foundation::{HWND, LPARAM, RECT};
use windows::Win32::Graphics::Dwm::{DWMWA_CLOAKED, DwmGetWindowAttribute};
use windows::Win32::UI::WindowsAndMessaging::{
    EnumChildWindows, EnumWindows, GetWindowInfo, GetWindowRect, IsIconic, IsWindowVisible,
    WINDOWINFO,
};
use windows::core::{BOOL, Result};

use crate::windows::geometry::{intersect_rect, is_empty, same_rect};

/// Enumerate top-level windows that pass cheap visibility checks
/// (`IsWindowVisible` + `!IsIconic`).  The more expensive DWM cloaking check
/// is deferred to [`is_window_cloaked`] so callers can batch or skip it.
pub(crate) fn enumerate_top_windows() -> Result<Vec<HWND>> {
    // Typical desktops have 50-200 top-level windows; pre-allocate to avoid
    // repeated reallocations inside the EnumWindows callback.
    let mut windows = Vec::with_capacity(128);
    unsafe {
        EnumWindows(
            Some(enum_window_proc),
            LPARAM((&mut windows as *mut Vec<HWND>) as isize),
        )?;
    }
    Ok(windows)
}

unsafe extern "system" fn enum_window_proc(hwnd: HWND, lparam: LPARAM) -> BOOL {
    // Only cheap Win32 checks here — the DWM cloaking call is deferred to the
    // cache-building phase so it can be skipped for windows that fail earlier
    // geometry checks.
    if !is_cheaply_visible(hwnd) {
        return true.into();
    }

    let windows = unsafe { &mut *(lparam.0 as *mut Vec<HWND>) };
    windows.push(hwnd);
    true.into()
}

/// Fast visibility pre-filter: only `IsWindowVisible` + `!IsIconic`.
/// Does *not* call `DwmGetWindowAttribute` (cross-process DWM round-trip).
fn is_cheaply_visible(hwnd: HWND) -> bool {
    if !unsafe { IsWindowVisible(hwnd).as_bool() } {
        return false;
    }
    if unsafe { IsIconic(hwnd).as_bool() } {
        return false;
    }
    true
}

/// Desktop-space rectangle used for intelligent selection.
///
/// This intentionally mirrors the reference app's simple Windows behavior:
/// prefer the client rect from `WINDOWINFO`, then fall back to `GetWindowRect`.
pub(crate) fn visible_window_rect(hwnd: HWND) -> Option<RECT> {
    let mut info = WINDOWINFO {
        cbSize: mem::size_of::<WINDOWINFO>() as u32,
        ..Default::default()
    };
    if unsafe { GetWindowInfo(hwnd, &mut info) }.is_ok()
        && info.rcClient.right > info.rcClient.left
        && info.rcClient.bottom > info.rcClient.top
    {
        return Some(info.rcClient);
    }

    let mut rect = RECT::default();
    unsafe { GetWindowRect(hwnd, &mut rect) }
        .is_ok()
        .then_some(rect)
        .filter(|rect| rect.right > rect.left && rect.bottom > rect.top)
}

struct ChildWindowRectContext {
    bounds: RECT,
    rects: Vec<RECT>,
}

/// Collect visible descendant HWND rectangles for one selected top-level
/// window. Callers cache this result for the lifetime of their window snapshot.
pub(crate) fn visible_child_window_rects(hwnd: HWND, bounds: RECT) -> Vec<RECT> {
    let mut context = ChildWindowRectContext {
        bounds,
        rects: Vec::new(),
    };
    unsafe {
        let _ = EnumChildWindows(
            Some(hwnd),
            Some(enum_child_window_proc),
            LPARAM((&mut context as *mut ChildWindowRectContext) as isize),
        );
    }
    normalize_child_rects(context.rects, bounds)
}

unsafe extern "system" fn enum_child_window_proc(hwnd: HWND, lparam: LPARAM) -> BOOL {
    if !unsafe { IsWindowVisible(hwnd).as_bool() } {
        return true.into();
    }

    let mut rect = RECT::default();
    if unsafe { GetWindowRect(hwnd, &mut rect) }.is_err() {
        return true.into();
    }

    let context = unsafe { &mut *(lparam.0 as *mut ChildWindowRectContext) };
    if let Some(rect) = intersect_rect(rect, context.bounds)
        && !is_empty(rect)
        && !same_rect(rect, context.bounds)
    {
        context.rects.push(rect);
    }
    true.into()
}

fn normalize_child_rects(mut rects: Vec<RECT>, bounds: RECT) -> Vec<RECT> {
    rects.retain(|rect| !is_empty(*rect) && !same_rect(*rect, bounds));
    rects.sort_unstable_by_key(|rect| {
        let width = i64::from(rect.right) - i64::from(rect.left);
        let height = i64::from(rect.bottom) - i64::from(rect.top);
        (width * height, rect.left, rect.top, rect.right, rect.bottom)
    });
    rects.dedup_by(|left, right| same_rect(*left, *right));
    rects
}

/// Expensive cloaking check — calls into DWM.  Exposed so the cache builder
/// can call it after cheaper geometry checks have already filtered windows.
pub(crate) fn is_window_cloaked(hwnd: HWND) -> bool {
    let mut cloaked = 0u32;
    let result = unsafe {
        DwmGetWindowAttribute(
            hwnd,
            DWMWA_CLOAKED,
            &mut cloaked as *mut u32 as *mut c_void,
            mem::size_of::<u32>() as u32,
        )
    };

    match result {
        Ok(()) => cloaked != 0,
        Err(_) => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rect(left: i32, top: i32, right: i32, bottom: i32) -> RECT {
        RECT {
            left,
            top,
            right,
            bottom,
        }
    }

    #[test]
    fn child_rects_are_deduplicated_and_sorted_smallest_first() {
        let bounds = rect(0, 0, 100, 100);
        let small = rect(10, 10, 20, 20);
        let medium = rect(5, 5, 50, 50);
        let rects = normalize_child_rects(vec![bounds, medium, small, small], bounds);

        assert_eq!(rects.len(), 2);
        assert!(same_rect(rects[0], small));
        assert!(same_rect(rects[1], medium));
    }
}

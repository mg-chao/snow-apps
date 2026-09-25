use std::ffi::c_void;
use std::mem;

use windows::Win32::Foundation::{HWND, LPARAM, RECT};
use windows::Win32::Graphics::Dwm::{
    DWMWA_CLOAKED, DWMWA_EXTENDED_FRAME_BOUNDS, DwmGetWindowAttribute,
};
use windows::Win32::UI::WindowsAndMessaging::{
    EnumChildWindows, EnumWindows, GWL_EXSTYLE, GetWindowLongPtrW, GetWindowRect, IsIconic,
    IsWindowVisible, WINDOW_EX_STYLE, WS_EX_LAYERED, WS_EX_TRANSPARENT,
};
use windows::core::{BOOL, Result};

use crate::windows::geometry::{intersect_rect, is_empty, same_rect};

/// Enumerate visible, non-minimized top-level selection candidates in Z order.
/// Mouse-through layered windows are intentionally ineligible, even when they
/// display visible content: automatic selection should reach the window below.
/// The more expensive DWM cloaking check
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
    if !is_top_level_selection_candidate(hwnd) {
        return true.into();
    }

    let windows = unsafe { &mut *(lparam.0 as *mut Vec<HWND>) };
    windows.push(hwnd);
    true.into()
}

/// Fast visibility and input-transparency pre-filter.
/// Does *not* call `DwmGetWindowAttribute` (cross-process DWM round-trip).
fn is_top_level_selection_candidate(hwnd: HWND) -> bool {
    if !unsafe { IsWindowVisible(hwnd).as_bool() } {
        return false;
    }
    if unsafe { IsIconic(hwnd).as_bool() } {
        return false;
    }
    let style = WINDOW_EX_STYLE(unsafe { GetWindowLongPtrW(hwnd, GWL_EXSTYLE) } as u32);
    !is_click_through_layered_window(style)
}

fn is_click_through_layered_window(style: WINDOW_EX_STYLE) -> bool {
    // Layered windows with WS_EX_TRANSPARENT pass mouse input to windows below.
    // https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features#layered-windows
    // In particular, the shell handwriting canvas can be visible and topmost
    // while covering the entire desktop with transparent pixels. Indexing it
    // would hide every real window from both the UIA and MSAA selectors.
    // WS_EX_TRANSPARENT alone only specifies paint ordering, so keep those
    // windows, as well as ordinary interactive layered windows, selectable.
    style.contains(WS_EX_LAYERED | WS_EX_TRANSPARENT)
}

/// Desktop-space rectangle used for intelligent selection.
///
/// The DWM frame includes the title bar while excluding invisible resize borders.
/// Fall back to the full Win32 window when DWM cannot supply a valid frame.
pub(crate) fn visible_window_rect(hwnd: HWND) -> Option<RECT> {
    let mut rect = RECT::default();
    let extended_frame = unsafe {
        DwmGetWindowAttribute(
            hwnd,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            &mut rect as *mut RECT as *mut c_void,
            mem::size_of::<RECT>() as u32,
        )
    };
    if extended_frame.is_ok() && !is_empty(rect) {
        return Some(rect);
    }

    let mut rect = RECT::default();
    unsafe { GetWindowRect(hwnd, &mut rect) }
        .is_ok()
        .then_some(rect)
        .filter(|rect| !is_empty(*rect))
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
    use crate::windows::geometry::rect_to_aabb;
    use crate::windows::spatial::{
        IndexedWindow, SMALL_WINDOW_LINEAR_SCAN_THRESHOLD, WindowSpatialIndex,
    };
    use windows::Win32::UI::WindowsAndMessaging::{
        CreateWindowExW, DestroyWindow, GetWindowInfo, HWND_TOP, SWP_NOACTIVATE, SWP_NOMOVE,
        SWP_NOSIZE, SetWindowPos, WINDOW_STYLE, WINDOWINFO, WS_EX_NOACTIVATE, WS_OVERLAPPEDWINDOW,
        WS_POPUP, WS_VISIBLE,
    };
    use windows::core::w;

    struct TestWindow(HWND);

    impl TestWindow {
        fn new(style: WINDOW_EX_STYLE) -> Self {
            Self::with_style(style, WS_POPUP | WS_VISIBLE)
        }

        fn with_style(style: WINDOW_EX_STYLE, window_style: WINDOW_STYLE) -> Self {
            Self(unsafe {
                CreateWindowExW(
                    style | WS_EX_NOACTIVATE,
                    w!("STATIC"),
                    w!("Snow selector transparency regression"),
                    window_style,
                    -32000,
                    -32000,
                    100,
                    100,
                    None,
                    None,
                    None,
                    None,
                )
                .unwrap()
            })
        }

        fn bring_to_front(&self) {
            unsafe {
                SetWindowPos(
                    self.0,
                    Some(HWND_TOP),
                    0,
                    0,
                    0,
                    0,
                    SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE,
                )
                .unwrap();
            }
        }
    }

    #[test]
    fn visible_window_bounds_include_a_standard_title_bar() {
        crate::windows::enable_high_dpi_support();
        let behind = TestWindow::new(WINDOW_EX_STYLE(0));
        let window = TestWindow::with_style(WINDOW_EX_STYLE(0), WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        let mut info = WINDOWINFO {
            cbSize: mem::size_of::<WINDOWINFO>() as u32,
            ..Default::default()
        };
        unsafe { GetWindowInfo(window.0, &mut info) }.unwrap();
        let bounds = visible_window_rect(window.0).unwrap();
        let caption = windows::Win32::Foundation::POINT {
            x: info.rcClient.left + 20,
            y: bounds.top + (info.rcClient.top - bounds.top) / 2,
        };

        assert!(info.rcClient.top > bounds.top);
        assert!(
            crate::windows::geometry::contains_point(bounds, caption),
            "bounds={bounds:?}, client={:?}, caption={caption:?}",
            info.rcClient
        );
        assert!(!crate::windows::geometry::contains_point(
            info.rcClient,
            caption
        ));
        assert!(crate::windows::geometry::contains_point(
            visible_window_rect(behind.0).unwrap(),
            caption
        ));

        let index = WindowSpatialIndex::build(
            [window.0, behind.0]
                .into_iter()
                .enumerate()
                .map(|(i, hwnd)| IndexedWindow {
                    envelope: rect_to_aabb(visible_window_rect(hwnd).unwrap()),
                    cache_index: i,
                    z_order: i,
                })
                .collect(),
        );
        assert_eq!(
            index
                .window_at_point([caption.x, caption.y])
                .unwrap()
                .cache_index,
            0
        );
    }

    impl Drop for TestWindow {
        fn drop(&mut self) {
            // Best-effort cleanup must not cause a second panic after a failed assertion.
            let _ = unsafe { DestroyWindow(self.0) };
        }
    }

    #[test]
    fn click_through_handwriting_canvas_is_not_a_selection_target() {
        assert!(is_click_through_layered_window(WINDOW_EX_STYLE(
            0x0a08_00a8
        )));
        assert!(!is_click_through_layered_window(WINDOW_EX_STYLE(0)));
        assert!(!is_click_through_layered_window(WS_EX_LAYERED));
        assert!(!is_click_through_layered_window(WS_EX_TRANSPARENT));
    }

    #[test]
    fn enumeration_excludes_only_the_combined_mouse_through_style() {
        let styles = [
            (WINDOW_EX_STYLE(0), true),
            (WS_EX_LAYERED, true),
            (WS_EX_TRANSPARENT, true),
            (WS_EX_LAYERED | WS_EX_TRANSPARENT, false),
        ];
        let windows = styles.map(|(style, expected)| (TestWindow::new(style), expected));
        let candidates = enumerate_top_windows().unwrap();
        for (window, expected) in windows {
            assert!(unsafe { IsWindowVisible(window.0).as_bool() });
            assert_eq!(candidates.contains(&window.0), expected);
        }
    }

    #[test]
    fn selection_reaches_underlying_window_in_both_spatial_index_paths() {
        for count in [2, SMALL_WINDOW_LINEAR_SCAN_THRESHOLD + 1] {
            let targets = (0..count)
                .map(|_| TestWindow::new(WINDOW_EX_STYLE(0)))
                .collect::<Vec<_>>();
            let target = targets.last().unwrap();
            let overlay = TestWindow::new(WS_EX_LAYERED | WS_EX_TRANSPARENT);
            target.bring_to_front();
            overlay.bring_to_front();

            // Preserve native enumeration order, but isolate these offscreen
            // fixtures from unrelated desktop windows and parallel tests.
            let candidates = enumerate_top_windows()
                .unwrap()
                .into_iter()
                .filter(|hwnd| *hwnd == overlay.0 || targets.iter().any(|w| w.0 == *hwnd))
                .collect::<Vec<_>>();
            let index = WindowSpatialIndex::build(
                candidates
                    .iter()
                    .enumerate()
                    .map(|(i, &hwnd)| IndexedWindow {
                        envelope: rect_to_aabb(visible_window_rect(hwnd).unwrap()),
                        cache_index: i,
                        z_order: i,
                    })
                    .collect(),
            );
            let bounds = visible_window_rect(target.0).unwrap();
            let hit = index
                .window_at_point([bounds.left + 50, bounds.top + 50])
                .unwrap();
            assert_eq!(candidates[hit.cache_index], target.0);
            assert_eq!(candidates.len(), count);
        }
    }

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

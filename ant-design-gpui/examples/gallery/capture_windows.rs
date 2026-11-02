// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Size only this process's hidden capture window without activating it.

use std::{ffi::c_void, io, ptr};

type Hwnd = *mut c_void;
const GWL_EXSTYLE: i32 = -20;
const WS_EX_NOACTIVATE: isize = 0x0800_0000;
const SWP_NOMOVE: u32 = 0x0002;
const SWP_NOZORDER: u32 = 0x0004;
const SWP_NOACTIVATE: u32 = 0x0010;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Rect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
}

#[link(name = "user32")]
unsafe extern "system" {
    #[link_name = "EnumWindows"]
    fn enum_windows(callback: unsafe extern "system" fn(Hwnd, isize) -> i32, context: isize)
    -> i32;
    #[link_name = "GetWindowThreadProcessId"]
    fn window_process(window: Hwnd, process: *mut u32) -> u32;
    #[link_name = "GetWindowTextW"]
    fn window_text(window: Hwnd, text: *mut u16, capacity: i32) -> i32;
    #[link_name = "GetClientRect"]
    fn client_rect(window: Hwnd, rect: *mut Rect) -> i32;
    #[link_name = "GetWindowRect"]
    fn window_rect(window: Hwnd, rect: *mut Rect) -> i32;
    #[link_name = "IsWindowVisible"]
    fn is_visible(window: Hwnd) -> i32;
    #[link_name = "GetForegroundWindow"]
    fn foreground_window() -> Hwnd;
    #[cfg_attr(target_pointer_width = "64", link_name = "GetWindowLongPtrW")]
    #[cfg_attr(target_pointer_width = "32", link_name = "GetWindowLongW")]
    fn window_long(window: Hwnd, index: i32) -> isize;
    #[cfg_attr(target_pointer_width = "64", link_name = "SetWindowLongPtrW")]
    #[cfg_attr(target_pointer_width = "32", link_name = "SetWindowLongW")]
    fn set_window_long(window: Hwnd, index: i32, value: isize) -> isize;
    #[link_name = "SetWindowPos"]
    fn set_window_pos(
        window: Hwnd,
        after: Hwnd,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        flags: u32,
    ) -> i32;
}
#[link(name = "kernel32")]
unsafe extern "system" {
    #[link_name = "SetLastError"]
    fn clear_last_error(code: u32);
}

struct Search<'a> {
    process: u32,
    title: &'a [u16],
    found: Option<Hwnd>,
    ambiguous: bool,
}
unsafe extern "system" fn find_owned(window: Hwnd, context: isize) -> i32 {
    // EnumWindows calls synchronously while Search is alive on this thread.
    let search = unsafe { &mut *(context as *mut Search<'_>) };
    let mut process = 0;
    unsafe { window_process(window, &mut process) };
    if process != search.process {
        return 1;
    }
    let mut title = [0; 256];
    let count = unsafe { window_text(window, title.as_mut_ptr(), title.len() as i32) };
    if count > 0 && title[..count as usize] == *search.title {
        search.ambiguous |= search.found.is_some();
        search.found = Some(window);
    }
    1
}

fn check(result: i32) -> io::Result<()> {
    if result == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}
fn background() -> io::Result<()> {
    let mut process = 0;
    unsafe { window_process(foreground_window(), &mut process) };
    if process == std::process::id() {
        Err(io::Error::other(
            "hidden gallery capture acquired foreground focus",
        ))
    } else {
        Ok(())
    }
}
fn outer_size(
    logical: (u32, u32),
    scale: f32,
    outer: Rect,
    client: Rect,
) -> io::Result<(i32, i32)> {
    let dimension = |logical: u32, frame: i64| {
        let pixels = (f64::from(logical) * f64::from(scale)).round();
        if logical == 0
            || !pixels.is_finite()
            || scale <= 0.
            || frame < 0
            || pixels < 1.
            || pixels + frame as f64 > f64::from(i32::MAX)
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid native capture dimensions or scale",
            ));
        }
        Ok(pixels as i32 + frame as i32)
    };
    Ok((
        dimension(
            logical.0,
            i64::from(outer.right) - i64::from(outer.left) - i64::from(client.right)
                + i64::from(client.left),
        )?,
        dimension(
            logical.1,
            i64::from(outer.bottom) - i64::from(outer.top) - i64::from(client.bottom)
                + i64::from(client.top),
        )?,
    ))
}

/// Call after title assignment, outside a GPUI update lease: SetWindowPos sends
/// synchronous WM_SIZE messages back into the native window's resize callback.
pub(super) fn resize(title: &str, logical: (u32, u32), scale: f32) -> io::Result<()> {
    background()?;
    let title: Vec<_> = title.encode_utf16().collect();
    let mut search = Search {
        process: std::process::id(),
        title: &title,
        found: None,
        ambiguous: false,
    };
    check(unsafe { enum_windows(find_owned, ptr::from_mut(&mut search) as isize) })?;
    let window = search.found.filter(|_| !search.ambiguous).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::NotFound,
            "expected one owned gallery capture window",
        )
    })?;
    if unsafe { is_visible(window) } != 0 {
        return Err(io::Error::other(
            "gallery capture window must remain hidden",
        ));
    }
    let mut outer = Rect::default();
    let mut client = Rect::default();
    check(unsafe { window_rect(window, &mut outer) })?;
    check(unsafe { client_rect(window, &mut client) })?;
    let dimensions = outer_size(logical, scale, outer, client)?;
    unsafe { clear_last_error(0) };
    let style = unsafe { window_long(window, GWL_EXSTYLE) };
    if style == 0 && io::Error::last_os_error().raw_os_error() != Some(0) {
        return Err(io::Error::last_os_error());
    }
    unsafe { clear_last_error(0) };
    let previous = unsafe { set_window_long(window, GWL_EXSTYLE, style | WS_EX_NOACTIVATE) };
    if previous == 0 && io::Error::last_os_error().raw_os_error() != Some(0) {
        return Err(io::Error::last_os_error());
    }
    // Both activation and z-order are explicit. The pinned GPUI Windows resize
    // path supplies only SWP_NOMOVE, which activates even a hidden window.
    check(unsafe {
        set_window_pos(
            window,
            ptr::null_mut(),
            0,
            0,
            dimensions.0,
            dimensions.1,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE,
        )
    })?;
    background()?;
    check(unsafe { client_rect(window, &mut client) })?;
    let actual = (client.right - client.left, client.bottom - client.top);
    let expected = (
        (logical.0 as f64 * f64::from(scale)).round() as i32,
        (logical.1 as f64 * f64::from(scale)).round() as i32,
    );
    if actual != expected {
        return Err(io::Error::other(format!(
            "native capture client size {actual:?} differs from {expected:?}"
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn capture_dimensions_use_actual_scale_and_nonclient_frame() {
        let outer = Rect {
            left: 40,
            top: 50,
            right: 1256,
            bottom: 989,
        };
        let client = Rect {
            right: 1200,
            bottom: 900,
            ..Default::default()
        };
        assert_eq!(
            outer_size((1280, 900), 1., outer, client).unwrap(),
            (1296, 939)
        );
        assert_eq!(
            outer_size((1280, 900), 1.5, outer, client).unwrap(),
            (1936, 1389)
        );
    }
    #[test]
    fn capture_dimensions_reject_invalid_scale_and_overflow() {
        for scale in [0., -1., f32::NAN, f32::INFINITY] {
            assert!(outer_size((1280, 900), scale, Rect::default(), Rect::default()).is_err());
        }
        assert!(outer_size((u32::MAX, 900), 1., Rect::default(), Rect::default()).is_err());
    }
}

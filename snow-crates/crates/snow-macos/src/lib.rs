//! Small, macOS-only native boundary shared by capture and window selection.
//! Screenshot calls must run on a worker thread while the main event loop is running.
#![cfg(target_os = "macos")]

use std::ffi::c_void;
use std::ffi::{CStr, c_char};
use std::ptr::NonNull;

#[repr(C)]
#[derive(Clone, Copy)]
struct NativeDisplay {
    id: u32,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    scale: f64,
    primary: u8,
    name: [c_char; 256],
}

#[derive(Clone, Debug)]
pub struct Display {
    pub id: u32,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub scale: f64,
    pub primary: bool,
    pub name: String,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Window {
    pub id: u32,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

impl Window {
    pub fn contains(&self, x: i32, y: i32) -> bool {
        i64::from(x) >= i64::from(self.x)
            && i64::from(y) >= i64::from(self.y)
            && i64::from(x) < i64::from(self.x) + i64::from(self.width)
            && i64::from(y) < i64::from(self.y) + i64::from(self.height)
    }
}

unsafe extern "C" {
    fn snow_macos_display_generation() -> u64;
    fn snow_macos_pointer(cursor: *mut CursorInfo) -> i32;
    fn snow_macos_cursor(cursor: *mut CursorInfo, rgba: *mut u8, capacity: usize) -> i32;
    fn snow_macos_displays(displays: *mut NativeDisplay, capacity: usize, count: *mut usize)
    -> i32;
    fn snow_macos_window_element(window_id: u32, x: i32, y: i32, element: *mut Window) -> i32;
    fn snow_macos_windows(windows: *mut Window, capacity: usize, count: *mut usize) -> i32;
    fn snow_macos_capture(
        display_id: u32,
        window_id: u32,
        width: u32,
        height: u32,
        bgra: u8,
        pixels: *mut u8,
        length: usize,
        error: *mut c_char,
        error_size: usize,
    ) -> i32;
    fn snow_macos_stream_start(
        display_id: u32,
        window_id: u32,
        source_x: u32,
        source_y: u32,
        width: u32,
        height: u32,
        error: *mut c_char,
        error_size: usize,
    ) -> *mut c_void;
    fn snow_macos_stream_read(
        handle: *mut c_void,
        bgra: u8,
        pixels: *mut u8,
        length: usize,
        sequence: *mut u64,
        error: *mut c_char,
        error_size: usize,
    ) -> i32;
    fn snow_macos_stream_stop(handle: *mut c_void);
}

/// Changes when native display geometry/configuration changes, including the desktop scale.
pub fn display_generation() -> u64 {
    unsafe { snow_macos_display_generation() }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CursorInfo {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub hotspot_x: u32,
    pub hotspot_y: u32,
    pub buttons: u8,
}

/// Read pointer position and button state without allocating or accessing cursor artwork.
pub fn pointer() -> Result<CursorInfo, String> {
    let mut info = CursorInfo::default();
    if unsafe { snow_macos_pointer(&mut info) } == 0 {
        return Err("Failed to sample the macOS pointer".into());
    }
    Ok(info)
}

/// Read the current system cursor into caller-owned scratch space (at least 256 * 256 * 4).
pub fn cursor(rgba: &mut [u8]) -> Result<CursorInfo, String> {
    let mut info = CursorInfo::default();
    if unsafe { snow_macos_cursor(&mut info, rgba.as_mut_ptr(), rgba.len()) } == 0 {
        return Err("Failed to sample the macOS cursor".into());
    }
    if image_length(info.width, info.height)? > rgba.len() {
        return Err("Invalid macOS cursor dimensions".into());
    }
    Ok(info)
}

/// A bounded continuous ScreenCaptureKit stream. Use on a worker with a running main event loop.
/// Coordinates and dimensions are backing pixels relative to the selected source.
pub struct VideoStream {
    handle: NonNull<c_void>,
    length: usize,
}

// Calls require exclusive access; native callback state is protected by an NSCondition.
unsafe impl Send for VideoStream {}

impl VideoStream {
    pub fn start(
        display_id: u32,
        window_id: u32,
        source_x: u32,
        source_y: u32,
        width: u32,
        height: u32,
    ) -> Result<Self, String> {
        let length = image_length(width, height)?;
        let mut error = [0 as c_char; 2048];
        let handle = unsafe {
            snow_macos_stream_start(
                display_id,
                window_id,
                source_x,
                source_y,
                width,
                height,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        NonNull::new(handle)
            .map(|handle| Self { handle, length })
            .ok_or_else(|| {
                unsafe { CStr::from_ptr(error.as_ptr()) }
                    .to_string_lossy()
                    .into_owned()
            })
    }

    /// Copy the newest complete surface; equal sequence numbers identify duplicate frames.
    /// Only the first read waits for a frame (at most five seconds).
    pub fn read(&mut self, bgra: bool, pixels: &mut [u8]) -> Result<u64, String> {
        if pixels.len() != self.length {
            return Err("Invalid recording buffer size".into());
        }
        let mut error = [0 as c_char; 2048];
        let mut sequence = 0;
        let success = unsafe {
            snow_macos_stream_read(
                self.handle.as_ptr(),
                u8::from(bgra),
                pixels.as_mut_ptr(),
                pixels.len(),
                &mut sequence,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        if success == 0 {
            Err(unsafe { CStr::from_ptr(error.as_ptr()) }
                .to_string_lossy()
                .into_owned())
        } else {
            Ok(sequence)
        }
    }
}

impl Drop for VideoStream {
    fn drop(&mut self) {
        unsafe { snow_macos_stream_stop(self.handle.as_ptr()) };
    }
}

pub fn displays() -> Result<Vec<Display>, String> {
    let mut capacity = 16;
    for _ in 0..4 {
        let mut native = vec![
            NativeDisplay {
                id: 0,
                x: 0,
                y: 0,
                width: 0,
                height: 0,
                scale: 1.0,
                primary: 0,
                name: [0; 256],
            };
            capacity
        ];
        let mut count = 0;
        // The native function writes at most capacity initialized records, with terminated names.
        let success = unsafe { snow_macos_displays(native.as_mut_ptr(), capacity, &mut count) };
        if success != 0 && count <= capacity {
            return Ok(native[..count]
                .iter()
                .map(|entry| Display {
                    id: entry.id,
                    x: entry.x,
                    y: entry.y,
                    width: entry.width,
                    height: entry.height,
                    scale: entry.scale,
                    primary: entry.primary != 0,
                    name: unsafe { CStr::from_ptr(entry.name.as_ptr()) }
                        .to_string_lossy()
                        .into_owned(),
                })
                .collect());
        }
        if count <= capacity || count > 1024 {
            break;
        }
        capacity = count;
    }
    Err("Failed to enumerate macOS displays".into())
}

/// Query an accessible control in a captured application window. None means
/// permission was denied or the target application does not expose that control.
pub fn window_element(window_id: u32, x: i32, y: i32) -> Option<Window> {
    let mut element = Window::default();
    let success = unsafe { snow_macos_window_element(window_id, x, y, &mut element) };
    (success != 0).then_some(element)
}

/// Front-to-back visible application windows. The current process and desktop are excluded.
pub fn windows() -> Result<Vec<Window>, String> {
    let mut capacity = 64;
    for _ in 0..4 {
        let mut windows = vec![Window::default(); capacity];
        let mut count = 0;
        let success = unsafe { snow_macos_windows(windows.as_mut_ptr(), capacity, &mut count) };
        if success != 0 && count <= capacity {
            windows.truncate(count);
            return Ok(windows);
        }
        if count <= capacity || count > 65536 {
            break;
        }
        capacity = count;
    }
    Err("Failed to enumerate macOS windows".into())
}

fn image_length(width: u32, height: u32) -> Result<usize, String> {
    if width == 0 || height == 0 || width > i32::MAX as u32 || height > i32::MAX as u32 {
        return Err("Invalid screenshot dimensions".into());
    }
    (width as usize)
        .checked_mul(height as usize)
        .and_then(|size| size.checked_mul(4))
        .filter(|size| *size <= isize::MAX as usize)
        .ok_or_else(|| "Screenshot buffer size overflow".into())
}

/// Capture a display (window_id = 0), or a desktop-independent window (display_id = 0).
pub fn capture(
    display_id: u32,
    window_id: u32,
    width: u32,
    height: u32,
    bgra: bool,
) -> Result<Vec<u8>, String> {
    let length = image_length(width, height)?;
    let mut pixels = Vec::new();
    pixels
        .try_reserve_exact(length)
        .map_err(|error| error.to_string())?;
    pixels.resize(length, 0);
    let mut error = [0 as c_char; 2048];
    let success = unsafe {
        snow_macos_capture(
            display_id,
            window_id,
            width,
            height,
            u8::from(bgra),
            pixels.as_mut_ptr(),
            length,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    if success == 0 {
        Err(unsafe { CStr::from_ptr(error.as_ptr()) }
            .to_string_lossy()
            .into_owned())
    } else {
        Ok(pixels)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validates_image_dimensions_before_allocating() {
        assert_eq!(image_length(3, 2).unwrap(), 24);
        assert!(image_length(0, 10).is_err());
        assert!(image_length(10, 0).is_err());
        assert!(image_length(u32::MAX, u32::MAX).is_err());
    }

    #[test]
    fn window_hit_test_is_half_open_and_handles_negative_origins() {
        let window = Window {
            id: 1,
            x: -200,
            y: -100,
            width: 400,
            height: 200,
        };
        assert!(window.contains(-200, -100));
        assert!(window.contains(199, 99));
        assert!(!window.contains(200, 0));
        assert!(!window.contains(0, 100));
        assert!(!window.contains(-201, 0));
        let large = Window {
            x: i32::MAX - 10,
            width: 100,
            ..window
        };
        assert!(large.contains(i32::MAX, 0));
    }
}

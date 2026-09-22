//! Owned geometry cloned from a prepared desktop session, without GPU ownership.
use super::*;

pub struct SnowCaptureDesktopLayoutImpl {
    entries: Vec<MonitorEntry>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowCaptureDisplayDescriptor {
    pub version: u32,
    pub struct_size: u32,
    pub stable_id: *const c_char,
    pub name: *const c_char,
    pub display_id: u32,
    pub coordinate_space: u32,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
    pub backing_scale: f64,
    pub pixel_width: u32,
    pub pixel_height: u32,
    pub is_primary: u32,
}

fn descriptor(entry: &MonitorEntry) -> SnowCaptureDisplayDescriptor {
    let native = entry.id.desktop_geometry();
    let (x, y, width, height, pixel_width, pixel_height) = native
        .map(|d| (d.x, d.y, d.width, d.height, d.pixel_width, d.pixel_height))
        .unwrap_or((
            f64::from(entry.x),
            f64::from(entry.y),
            f64::from(entry.expected_width),
            f64::from(entry.expected_height),
            entry.expected_width,
            entry.expected_height,
        ));
    SnowCaptureDisplayDescriptor {
        version: 1,
        struct_size: std::mem::size_of::<SnowCaptureDisplayDescriptor>() as u32,
        stable_id: entry.stable_id.as_ptr(),
        name: entry.name.as_ptr(),
        display_id: entry.id.macos_display_id().unwrap_or(0),
        coordinate_space: u32::from(native.is_some()),
        x,
        y,
        width,
        height,
        backing_scale: (f64::from(pixel_width) / width).max(f64::from(pixel_height) / height),
        pixel_width,
        pixel_height,
        is_primary: u32::from(entry.is_primary),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_desktop_session_layout_snapshot(
    session: *mut SnowCaptureDesktopSessionImpl,
    refresh: u8,
) -> *mut SnowCaptureDesktopLayoutImpl {
    let Some(session) = session_mut(session) else {
        return ptr::null_mut();
    };
    if (refresh != 0 || session.workers.is_empty())
        && let Err(error) = rebuild_workers(session)
    {
        set_last_error(error);
        return ptr::null_mut();
    }
    let entries: Vec<_> = session.workers.iter().map(|w| w.entry.clone()).collect();
    if entries.is_empty() {
        set_last_error("no active displays are available");
        return ptr::null_mut();
    }
    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureDesktopLayoutImpl { entries }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_desktop_layout_destroy(
    layout: *mut SnowCaptureDesktopLayoutImpl,
) {
    if !layout.is_null() {
        drop(unsafe { Box::from_raw(layout) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_desktop_layout_count(
    layout: *const SnowCaptureDesktopLayoutImpl,
) -> usize {
    unsafe { layout.as_ref() }.map_or(0, |l| l.entries.len())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_desktop_layout_display(
    layout: *const SnowCaptureDesktopLayoutImpl,
    index: usize,
    out: *mut SnowCaptureDisplayDescriptor,
) -> u8 {
    let Some(entry) = (unsafe { layout.as_ref() }).and_then(|l| l.entries.get(index)) else {
        return 0;
    };
    if out.is_null() {
        return 0;
    }
    let value = descriptor(entry);
    if unsafe { (*out).version != 1 || (*out).struct_size != value.struct_size } {
        return 0;
    }
    unsafe {
        out.write(value);
    }
    1
}

fn matches_frames(
    layout: &SnowCaptureDesktopLayoutImpl,
    result: &SnowCaptureScreenshotResultImpl,
) -> bool {
    layout.entries.len() == result.frames.len()
        && layout.entries.iter().all(|expected| {
            result.frames.iter().any(|actual| {
                let d = descriptor(expected);
                expected.id == actual.entry.id
                    && expected.x == actual.entry.x
                    && expected.y == actual.entry.y
                    && d.pixel_width == actual.frame.width()
                    && d.pixel_height == actual.frame.height()
                    && expected.id.desktop_geometry() == actual.entry.id.desktop_geometry()
            })
        })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_desktop_session_capture_with_layout(
    session: *mut SnowCaptureDesktopSessionImpl,
    request: *const SnowCaptureScreenshotRequest,
    layout: *const SnowCaptureDesktopLayoutImpl,
) -> *mut SnowCaptureScreenshotResultImpl {
    let (Some(session_ref), Some(layout)) =
        (unsafe { session.as_ref() }, unsafe { layout.as_ref() })
    else {
        set_last_error("desktop session or layout is null");
        return ptr::null_mut();
    };
    let entries: Vec<_> = session_ref
        .workers
        .iter()
        .map(|w| w.entry.clone())
        .collect();
    if !same_monitor_layout(&layout.entries, &entries) {
        set_last_error("desktop layout changed before capture");
        return ptr::null_mut();
    }
    let result = unsafe { snow_capture_desktop_session_capture(session, request) };
    if let Some(frames) = unsafe { result.as_ref() }
        && !matches_frames(layout, frames)
    {
        unsafe {
            snow_capture_screenshot_result_destroy(result);
        }
        set_last_error("desktop layout changed during capture");
        return ptr::null_mut();
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    fn entry() -> MonitorEntry {
        MonitorEntry {
            id: MonitorId::from_name(1, "test", true),
            stable_id: CString::new("test-id").unwrap(),
            name: CString::new("test").unwrap(),
            x: -20,
            y: 10,
            expected_width: 4,
            expected_height: 2,
            is_primary: true,
        }
    }
    #[test]
    fn snapshot_owns_strings_and_validates_descriptor_version() {
        let source = entry();
        let snapshot = SnowCaptureDesktopLayoutImpl {
            entries: vec![source.clone()],
        };
        drop(source);
        let mut output = descriptor(&snapshot.entries[0]);
        output.version = 2;
        assert_eq!(
            unsafe { snow_capture_desktop_layout_display(&snapshot, 0, &mut output) },
            0
        );
        output.version = 1;
        assert_eq!(
            unsafe { snow_capture_desktop_layout_display(&snapshot, 0, &mut output) },
            1
        );
        assert_eq!(
            unsafe { CStr::from_ptr(output.name) }.to_str().unwrap(),
            "test"
        );
        assert_eq!((output.x, output.pixel_width), (-20.0, 4));
        assert_eq!(
            unsafe { snow_capture_desktop_layout_display(&snapshot, 1, &mut output) },
            0
        );
    }
    #[test]
    fn frame_geometry_must_match_snapshot() {
        let expected = entry();
        let mut result = SnowCaptureScreenshotResultImpl {
            frames: vec![SnapshotFrame {
                entry: expected.clone(),
                frame: Arc::new(Frame::from_rgba8(4, 2, vec![0; 32]).unwrap()),
            }],
            focused_window: None,
        };
        let snapshot = SnowCaptureDesktopLayoutImpl {
            entries: vec![expected],
        };
        assert!(matches_frames(&snapshot, &result));
        result.frames[0].entry.x += 1;
        assert!(!matches_frames(&snapshot, &result));
    }
}

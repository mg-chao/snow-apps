use super::*;

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SnowCaptureFrameGeometry {
    pub version: u32,
    pub struct_size: u32,
    pub coordinate_space: u32,
    pub display_id: u32,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
    pub backing_scale: f64,
}

fn geometry(frame: &Frame, x: i32, y: i32, display_id: u32) -> SnowCaptureFrameGeometry {
    let mut result = SnowCaptureFrameGeometry {
        version: 1,
        struct_size: std::mem::size_of::<SnowCaptureFrameGeometry>() as u32,
        coordinate_space: 0,
        display_id,
        x: f64::from(x),
        y: f64::from(y),
        width: f64::from(frame.width()),
        height: f64::from(frame.height()),
        backing_scale: 1.0,
    };
    if let Some(transform) = frame.metadata().capture_transform() {
        result.coordinate_space = u32::from(cfg!(target_os = "macos"));
        result.x = transform.source.x;
        result.y = transform.source.y;
        result.width = transform.source.width;
        result.height = transform.source.height;
        result.backing_scale = (f64::from(frame.width()) / result.width)
            .max(f64::from(frame.height()) / result.height);
    }
    result
}

unsafe fn write(out: *mut SnowCaptureFrameGeometry, value: SnowCaptureFrameGeometry) -> u8 {
    if out.is_null() {
        set_last_error("frame geometry is null");
        return 0;
    }
    let version = unsafe { ptr::addr_of!((*out).version).read() };
    let size = unsafe { ptr::addr_of!((*out).struct_size).read() };
    if version != 1 || size != value.struct_size {
        set_last_error("unsupported frame geometry version or size");
        return 0;
    }
    unsafe {
        out.write(value);
    }
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_screenshot_result_display_geometry(
    result: *const SnowCaptureScreenshotResultImpl,
    index: usize,
    out: *mut SnowCaptureFrameGeometry,
) -> u8 {
    let Some(frame) = (unsafe { result.as_ref() }).and_then(|r| r.frames.get(index)) else {
        set_last_error("screenshot display is unavailable");
        return 0;
    };
    let id = frame.entry.id.macos_display_id().unwrap_or(0);
    unsafe {
        write(
            out,
            geometry(&frame.frame, frame.entry.x, frame.entry.y, id),
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_screenshot_result_focused_window_geometry(
    result: *const SnowCaptureScreenshotResultImpl,
    out: *mut SnowCaptureFrameGeometry,
) -> u8 {
    let Some(frame) = (unsafe { result.as_ref() }).and_then(|r| r.focused_window.as_ref()) else {
        set_last_error("focused window is unavailable");
        return 0;
    };
    unsafe { write(out, geometry(&frame.frame, frame.x, frame.y, 0)) }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn versioned_geometry_rejects_invalid_output_and_preserves_pixel_fallback() {
        let frame = Frame::from_rgba8(4, 2, vec![0; 32]).unwrap();
        let value = geometry(&frame, -4, 2, 17);
        assert_eq!((value.x, value.width, value.backing_scale), (-4., 4., 1.));
        let mut out = value;
        out.version = 99;
        assert_eq!(unsafe { write(&mut out, value) }, 0);
        assert_eq!(out.version, 99);
        out.version = 1;
        assert_eq!(unsafe { write(&mut out, value) }, 1);
        assert_eq!(unsafe { write(ptr::null_mut(), value) }, 0);
        assert_eq!(
            unsafe { snow_capture_screenshot_result_display_geometry(ptr::null(), 0, &mut out) },
            0
        );
    }
}

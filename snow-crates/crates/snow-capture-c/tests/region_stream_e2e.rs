#![cfg(target_os = "windows")]

use std::ffi::CStr;
use std::ffi::c_void;
use std::ptr;
use std::sync::mpsc;
use std::time::Duration;

use snow_capture_c::*;

unsafe extern "C" fn receive_frame(
    context: *mut c_void,
    kind: SnowCaptureRegionStreamEventKind,
    frame: *mut SnowCaptureRegionStreamFrameImpl,
) {
    let sender = unsafe { &*(context as *const mpsc::Sender<(u32, usize, String)>) };
    let message = if kind == SnowCaptureRegionStreamEventKind::Error {
        unsafe { CStr::from_ptr(snow_capture_last_error_message()) }
            .to_string_lossy()
            .into_owned()
    } else {
        String::new()
    };
    let _ = sender.send((kind as u32, frame as usize, message));
    if kind != SnowCaptureRegionStreamEventKind::Frame && !frame.is_null() {
        unsafe { snow_capture_region_stream_frame_destroy(frame) };
    }
}

#[test]
#[ignore = "requires an interactive Windows desktop and WGC"]
fn delivered_frame_outlives_stream_shutdown() {
    let desktop = snow_capture_desktop_session_create(ptr::null());
    assert!(!desktop.is_null());
    assert_eq!(snow_capture_desktop_session_prepare(desktop), 1);
    let snapshot = snow_capture_desktop_session_capture_all(desktop);
    assert!(!snapshot.is_null());
    let mut primary = None;
    for index in 0..snow_capture_snapshot_count(snapshot) {
        let mut info = SnowCaptureFrameInfo {
            stable_id: ptr::null(),
            name: ptr::null(),
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            is_primary: 0,
            backend_kind: 0,
            reserved0: [0; 2],
            stride_bytes: 0,
            rgba_bytes: ptr::null(),
            rgba_len: 0,
        };
        if unsafe { snow_capture_snapshot_frame_info(snapshot, index, &mut info) } != 0
            && info.is_primary != 0
        {
            primary = Some((info.x, info.y, info.width.min(320), info.height.min(240)));
            break;
        }
    }
    unsafe {
        snow_capture_snapshot_destroy(snapshot);
        snow_capture_desktop_session_destroy(desktop);
    }
    let (x, y, width, height) = primary.expect("primary monitor should be captured");

    let (sender, receiver) = mpsc::channel::<(u32, usize, String)>();
    let config = SnowCaptureRegionStreamConfig {
        version: REGION_STREAM_CONFIG_VERSION,
        struct_size: std::mem::size_of::<SnowCaptureRegionStreamConfig>() as u32,
        x,
        y,
        width,
        height,
        capture_retry_count: 1,
        capture_backend: 2,
        reserved: [0; 31],
    };
    let context = (&sender as *const mpsc::Sender<(u32, usize, String)>)
        .cast_mut()
        .cast::<c_void>();
    let stream =
        unsafe { snow_capture_region_stream_create(&config, Some(receive_frame), context) };
    assert!(!stream.is_null());
    let (kind, frame, message) = receiver
        .recv_timeout(Duration::from_secs(5))
        .expect("native stream should deliver an event");
    assert_eq!(
        kind,
        SnowCaptureRegionStreamEventKind::Frame as u32,
        "{message}"
    );
    let frame = frame as *mut SnowCaptureRegionStreamFrameImpl;

    unsafe { snow_capture_region_stream_destroy(stream) };
    let mut info = SnowCaptureRegionStreamFrameInfo {
        width: 0,
        height: 0,
        stride_bytes: 0,
        is_duplicate: 0,
        reserved0: [0; 3],
        rgba_bytes: ptr::null(),
        rgba_len: 0,
    };
    assert_eq!(
        unsafe { snow_capture_region_stream_frame_info(frame, &mut info) },
        1
    );
    assert_eq!((info.width, info.height), (width, height));
    assert!(!info.rgba_bytes.is_null());
    assert!(info.rgba_len >= width as usize * height as usize * 4);
    unsafe { snow_capture_region_stream_frame_destroy(frame) };
}

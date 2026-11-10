//! One-shot direct acquisition without opening unrelated desktop display sessions.
use super::*;
use std::ffi::c_void;

const TARGET_SCREENSHOT_REQUEST_VERSION: u32 = 1;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowCaptureTargetScreenshotRequest {
    pub version: u32,
    pub struct_size: u32,
    pub screenshot: SnowCaptureScreenshotRequest,
    pub monitor_utf8: *const c_char,
    pub pixel_format: u8,
    pub capture_backend: u8,
    pub reserved: [u8; 6],
    pub admission_context: *mut c_void,
    pub admit_raster: Option<unsafe extern "C" fn(*mut c_void, u32, u32) -> u8>,
}

#[derive(Debug, PartialEq, Eq)]
enum Target {
    Monitor(String),
    Window(isize),
}

unsafe fn plan(
    request: *const SnowCaptureTargetScreenshotRequest,
) -> Result<
    (
        SnowCaptureScreenshotRequest,
        Target,
        CapturePixelFormat,
        CaptureBackendKind,
    ),
    String,
> {
    if request.is_null() {
        return Err("target screenshot request is null".to_owned());
    }
    // Validate the fixed header before accessing any trailing fields.
    let header = unsafe { &*request.cast::<SnowCaptureScreenshotRequestHeader>() };
    if header.version != TARGET_SCREENSHOT_REQUEST_VERSION
        || (header.struct_size as usize) < std::mem::size_of::<SnowCaptureTargetScreenshotRequest>()
    {
        return Err("unsupported target screenshot request version or size".to_owned());
    }
    let request = unsafe { &*request };
    let screenshot = unsafe { read_screenshot_request(&request.screenshot)? };
    if screenshot.flags & SCREENSHOT_REQUEST_REFRESH_LAYOUT != 0 {
        return Err("target screenshots do not accept desktop layout flags".to_owned());
    }
    let monitor = if request.monitor_utf8.is_null() {
        None
    } else {
        let name = unsafe { CStr::from_ptr(request.monitor_utf8) }
            .to_str()
            .map_err(|error| error.to_string())?;
        if name.is_empty() {
            return Err("target monitor identity is empty".to_owned());
        }
        Some(name.to_owned())
    };
    let target = match (screenshot.focused_window, monitor) {
        (0, Some(monitor)) => Target::Monitor(monitor),
        (window, None) if window != 0 => Target::Window(window),
        _ => return Err("target screenshot requires exactly one monitor or window".to_owned()),
    };
    Ok((
        screenshot,
        target,
        parse_pixel_format(request.pixel_format)?,
        parse_capture_backend(request.capture_backend)?,
    ))
}

fn monitor_matches(entry: &MonitorEntry, identity: &str) -> bool {
    entry.name.to_bytes() == identity.as_bytes()
        || entry.stable_id.to_bytes() == identity.as_bytes()
        || entry
            .id
            .macos_display_id()
            .is_some_and(|id| identity == format!("display:{id}"))
}

fn capture_target(
    request: SnowCaptureScreenshotRequest,
    target: Target,
    pixel_format: CapturePixelFormat,
    preferred: CaptureBackendKind,
) -> Result<SnowCaptureScreenshotResultImpl, String> {
    let cancellation = if request.cancellation_token.is_null() {
        CancellationToken::default()
    } else {
        unsafe { &*request.cancellation_token }.canceled.clone()
    };
    if cancellation.is_canceled() {
        return Err("screenshot capture canceled".to_owned());
    }
    // Match desktop acquisition's one query per transaction, including native window alpha.
    #[cfg(windows)]
    let color_query = if request.flags & SCREENSHOT_REQUEST_RESTORE_ORIGINAL_COLORS != 0 {
        Some(
            snow_capture::color_effect::ScreenColorQuery::start_current()
                .map_err(|error| format!("failed to start screen-color query: {error}"))?,
        )
    } else {
        None
    };
    #[cfg(windows)]
    let correction = color_query.as_ref().map(|query| query.snapshot());
    #[cfg(not(windows))]
    let correction = None;
    let include_cursor = request.flags & SCREENSHOT_REQUEST_INCLUDE_CURSOR != 0;
    let options = snapshot_options(1, 0, pixel_format)?;
    let result = match target {
        Target::Window(window) => SnowCaptureScreenshotResultImpl {
            frames: Vec::new(),
            focused_window: Some(capture_window_snapshot(
                window,
                options,
                preferred,
                include_cursor,
                cancellation.clone(),
                correction,
            )?),
        },
        Target::Monitor(identity) => {
            let system = screenshot_capture_system(preferred)?;
            // Enumerating geometry opens no capture backend or worker. Only the selected
            // monitor reaches open_session, preparation and pixel acquisition.
            let entry = build_monitor_entries(&system)?
                .into_iter()
                .find(|entry| monitor_matches(entry, &identity))
                .ok_or_else(|| format!("capture monitor is unavailable: {identity}"))?;
            let mut session = system
                .open_session(CaptureTarget::Monitor(entry.id.clone()), options)
                .map_err(|error| error.to_string())?;
            let mut frame = match correction {
                Some(query) => session.capture_snapshot_with_color_query(
                    include_cursor,
                    cancellation.clone(),
                    query,
                ),
                None => session.capture_snapshot(include_cursor, cancellation.clone()),
            }
            .map_err(|error| error.to_string())?;
            if session.active_capture_access_count() != 0 {
                let _ = session.reset_to_prepared();
                return Err(
                    "capture access remained active after targeted monitor capture".to_owned(),
                );
            }
            if include_cursor {
                frame.composite_attached_cursor();
            }
            // Preserve the layout paired with acquisition. Re-inspecting a target
            // after snapshot release can reject valid pixels after unplug/movement.
            SnowCaptureScreenshotResultImpl {
                frames: vec![SnapshotFrame {
                    entry,
                    frame: Arc::new(frame),
                }],
                focused_window: None,
            }
        }
    };
    if cancellation.is_canceled() {
        return Err("screenshot capture canceled".to_owned());
    }
    Ok(result)
}

fn with_target_admission<T>(
    input: &SnowCaptureTargetScreenshotRequest,
    capture: impl FnOnce() -> Result<T, String>,
) -> Result<T, String> {
    let rejected = Arc::new(std::sync::Mutex::new(false));
    let policy = input.admit_raster.map(|callback| {
        let context = input.admission_context as usize;
        let rejected = rejected.clone();
        Arc::new(move |width, height| {
            let mut rejected = rejected.lock().unwrap();
            if *rejected {
                return false;
            }
            let accepted = unsafe { callback(context as *mut c_void, width, height) } != 0;
            if !accepted {
                *rejected = true;
            }
            accepted
        }) as snow_core::raster_admission::RasterAdmission
    });
    let result = snow_core::raster_admission::with_policy(policy, capture);
    // Every target session (including its WGC worker) has been dropped/joined.
    if *rejected.lock().unwrap() {
        Err("resource_limit".to_owned())
    } else {
        result
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// The versioned request and any monitor string/cancellation token must remain
/// readable until return. Acquisition must run on a worker thread.
pub unsafe extern "C" fn snow_capture_target_screenshot(
    request: *const SnowCaptureTargetScreenshotRequest,
) -> *mut SnowCaptureScreenshotResultImpl {
    let result = unsafe { plan(request) }.and_then(|(capture, target, format, preferred)| {
        // plan checked the complete versioned request before reading trailing fields.
        let input = unsafe { &*request };
        with_target_admission(input, || {
            with_capture_autoreleasepool(|| capture_target(capture, target, format, preferred))
        })
    });
    match result {
        Ok(result) => {
            clear_last_error();
            Box::into_raw(Box::new(result))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request() -> SnowCaptureTargetScreenshotRequest {
        SnowCaptureTargetScreenshotRequest {
            version: TARGET_SCREENSHOT_REQUEST_VERSION,
            struct_size: std::mem::size_of::<SnowCaptureTargetScreenshotRequest>() as u32,
            screenshot: SnowCaptureScreenshotRequest {
                version: SCREENSHOT_REQUEST_VERSION,
                struct_size: SCREENSHOT_REQUEST_SIZE,
                flags: SCREENSHOT_REQUEST_INCLUDE_CURSOR
                    | SCREENSHOT_REQUEST_RESTORE_ORIGINAL_COLORS,
                reserved0: 0,
                focused_window: 0,
                cancellation_token: ptr::null(),
                reserved: [0; 32],
            },
            monitor_utf8: c"display:7".as_ptr(),
            pixel_format: 1,
            capture_backend: 0,
            reserved: [0; 6],
            admission_context: ptr::null_mut(),
            admit_raster: None,
        }
    }

    #[test]
    fn target_planning_is_exclusive_sized_and_keeps_capture_policies() {
        let mut input = request();
        let (capture, target, format, backend) = unsafe { plan(&input) }.unwrap();
        assert_eq!(target, Target::Monitor("display:7".into()));
        assert_eq!(capture.flags, input.screenshot.flags);
        assert_eq!(format, CapturePixelFormat::Bgra8);
        assert_eq!(backend, CaptureBackendKind::Auto);
        input.screenshot.focused_window = 42;
        assert!(unsafe { plan(&input) }.is_err());
        input.monitor_utf8 = ptr::null();
        assert_eq!(unsafe { plan(&input) }.unwrap().1, Target::Window(42));
        input.screenshot.focused_window = 0;
        assert!(unsafe { plan(&input) }.is_err());
        input.monitor_utf8 = c"".as_ptr();
        assert!(unsafe { plan(&input) }.is_err());
        let short = SnowCaptureScreenshotRequestHeader {
            version: 1,
            struct_size: 8,
        };
        assert!(unsafe { plan((&raw const short).cast()) }.is_err());
        input = request();
        input.version += 1;
        assert!(unsafe { plan(&input) }.is_err());
        input = request();
        input.pixel_format = 255;
        assert!(unsafe { plan(&input) }.is_err());
        input = request();
        input.screenshot.flags |= SCREENSHOT_REQUEST_REFRESH_LAYOUT;
        assert!(unsafe { plan(&input) }.is_err());
    }

    #[test]
    fn target_cancellation_precedes_native_enumeration_and_acquisition() {
        let token = snow_capture_cancellation_token_create();
        unsafe { snow_capture_cancellation_token_cancel(token) };
        let mut input = request();
        input.screenshot.cancellation_token = token;
        assert!(unsafe { snow_capture_target_screenshot(&input) }.is_null());
        assert!(LAST_ERROR.with(|error| error.borrow().to_string_lossy().contains("canceled")));
        unsafe { snow_capture_cancellation_token_destroy(token) };
    }

    #[test]
    fn target_backend_preference_accepts_auto_and_rejects_invalid_values_before_capture() {
        let mut input = request();
        for (value, expected) in [
            (0, CaptureBackendKind::Auto),
            (1, CaptureBackendKind::DxgiDuplication),
            (2, CaptureBackendKind::WindowsGraphicsCapture),
            (3, CaptureBackendKind::Gdi),
            (4, CaptureBackendKind::ScreenCaptureKit),
        ] {
            input.capture_backend = value;
            assert_eq!(unsafe { plan(&input) }.unwrap().3, expected);
        }
        for value in [5, u8::MAX] {
            input.capture_backend = value;
            assert!(unsafe { snow_capture_target_screenshot(&input) }.is_null());
            assert!(LAST_ERROR.with(|error| {
                error
                    .borrow()
                    .to_string_lossy()
                    .contains("invalid capture backend")
            }));
        }
    }

    #[test]
    fn target_backend_preference_preserves_version_one_abi_and_zero_default() {
        #[repr(C)]
        struct OriginalRequest {
            version: u32,
            struct_size: u32,
            screenshot: SnowCaptureScreenshotRequest,
            monitor_utf8: *const c_char,
            pixel_format: u8,
            reserved: [u8; 7],
            admission_context: *mut c_void,
            admit_raster: Option<unsafe extern "C" fn(*mut c_void, u32, u32) -> u8>,
        }
        use std::mem::{offset_of, size_of};
        assert_eq!(
            size_of::<SnowCaptureTargetScreenshotRequest>(),
            size_of::<OriginalRequest>()
        );
        assert_eq!(
            offset_of!(SnowCaptureTargetScreenshotRequest, capture_backend),
            offset_of!(OriginalRequest, reserved)
        );
        assert_eq!(
            offset_of!(SnowCaptureTargetScreenshotRequest, admission_context),
            offset_of!(OriginalRequest, admission_context)
        );
        assert_eq!(
            offset_of!(SnowCaptureTargetScreenshotRequest, admit_raster),
            offset_of!(OriginalRequest, admit_raster)
        );
        let mut input: SnowCaptureTargetScreenshotRequest = unsafe { std::mem::zeroed() };
        let valid = request();
        input.version = valid.version;
        input.struct_size = valid.struct_size;
        input.screenshot = valid.screenshot;
        input.monitor_utf8 = valid.monitor_utf8;
        assert_eq!(unsafe { plan(&input) }.unwrap().3, CaptureBackendKind::Auto);
    }

    #[test]
    fn admission_rejects_changed_allocation_shape_and_latches_across_workers() {
        struct Context {
            calls: std::sync::atomic::AtomicUsize,
        }
        unsafe extern "C" fn admit(context: *mut c_void, width: u32, height: u32) -> u8 {
            let context = unsafe { &*(context as *const Context) };
            context
                .calls
                .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            u8::from(width <= 4 && height <= 3)
        }
        let mut context = Context {
            calls: Default::default(),
        };
        let mut input = request();
        input.admission_context = (&raw mut context).cast();
        input.admit_raster = Some(admit);
        let result = with_target_admission(&input, || {
            let mut frame = snow_capture::frame::Frame::empty();
            frame
                .prepare_for_overwrite(4, 3, CapturePixelFormat::Bgra8)
                .unwrap();
            let before = frame.as_bytes().as_ptr();
            let policy = snow_core::raster_admission::current();
            std::thread::spawn(move || {
                snow_core::raster_admission::with_policy(policy, || {
                    let mut frame = snow_capture::frame::Frame::empty();
                    assert!(
                        frame
                            .prepare_for_overwrite(3, 4, CapturePixelFormat::Bgra8)
                            .is_err()
                    );
                    assert!(frame.as_bytes().is_empty());
                })
            })
            .join()
            .unwrap();
            // Once one actual shape is denied, a smaller fallback must not recover.
            assert!(
                frame
                    .ensure_capacity(1, 1, CapturePixelFormat::Bgra8)
                    .is_err()
            );
            assert_eq!(frame.dimensions(), (4, 3));
            assert_eq!(frame.as_bytes().as_ptr(), before);
            Ok(())
        });
        assert_eq!(result.unwrap_err(), "resource_limit");
        assert_eq!(context.calls.load(std::sync::atomic::Ordering::Relaxed), 2);
        assert!(snow_core::raster_admission::current().is_none());
        let mut frame = snow_capture::frame::Frame::empty();
        frame
            .prepare_for_overwrite(3, 4, CapturePixelFormat::Bgra8)
            .unwrap();
    }

    #[test]
    fn monitor_identity_selection_never_substitutes_another_display() {
        let entry = MonitorEntry {
            id: MonitorId::from_parts(1, 2, 3, "unit-monitor", true),
            stable_id: sanitize_cstring("stable-unit-monitor"),
            name: sanitize_cstring("unit-monitor"),
            x: 0,
            y: 0,
            expected_width: 2,
            expected_height: 2,
            is_primary: true,
        };
        assert!(monitor_matches(&entry, "unit-monitor"));
        assert!(monitor_matches(&entry, "stable-unit-monitor"));
        assert!(!monitor_matches(&entry, "missing-monitor"));
    }
}

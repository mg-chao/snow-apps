//! C ABI for snow-selected-text. See include/snow_selected_text.h.
//!
//! All non-null pointers must reference live, aligned allocations of the documented
//! type and size. Handles must originate here. Destruction must not race a call.
//! Input buffers are copied during start; output views live until result destruction.

use std::ptr;
use std::sync::Arc;
use std::time::Duration;

use snow_selected_text::*;

pub struct SnowSelectedTextService(SelectedTextService);
pub struct SnowSelectedTextRequest(CaptureRequest);
pub struct SnowSelectedTextResult(Arc<CaptureResult>);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowSelectedTextBytes {
    pub data: *const u8,
    pub length: usize,
}

impl Default for SnowSelectedTextBytes {
    fn default() -> Self {
        Self {
            data: ptr::null(),
            length: 0,
        }
    }
}

impl SnowSelectedTextBytes {
    fn view(value: &str) -> Self {
        Self {
            data: value.as_ptr(),
            length: value.len(),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowSelectedTextOptions {
    pub struct_size: u32,
    pub abi_version: u32,
    pub timeout_ms: u32,
    pub copy_fallback: u32,
    pub strategy: u32,
    pub max_text_bytes: usize,
    pub excluded_windows: *const usize,
    pub excluded_window_count: usize,
    pub excluded_executables: *const SnowSelectedTextBytes,
    pub excluded_executable_count: usize,
}

impl Default for SnowSelectedTextOptions {
    fn default() -> Self {
        Self {
            struct_size: size_of::<Self>() as u32,
            abi_version: 1,
            timeout_ms: 2000,
            copy_fallback: 1,
            strategy: CaptureStrategy::Auto as u32,
            max_text_bytes: 1024 * 1024,
            excluded_windows: ptr::null(),
            excluded_window_count: 0,
            excluded_executables: ptr::null(),
            excluded_executable_count: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SnowSelectedTextError {
    pub kind: u32,
    pub native_code: i32,
    pub has_native_code: u32,
    pub clipboard_status: u32,
    pub operation: SnowSelectedTextBytes,
}

impl From<&SelectionError> for SnowSelectedTextError {
    fn from(error: &SelectionError) -> Self {
        Self {
            kind: error.kind as u32,
            native_code: error.native_code.unwrap_or(0),
            has_native_code: u32::from(error.native_code.is_some()),
            clipboard_status: error.clipboard_status as u32,
            operation: SnowSelectedTextBytes::view(error.operation),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SnowSelectedTextMetadata {
    pub window: usize,
    pub process_id: u32,
    pub focused_control: usize,
    pub method: u32,
    pub clipboard_status: u32,
    pub executable: SnowSelectedTextBytes,
    pub range_count: usize,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SnowSelectedTextRect {
    pub left: f64,
    pub top: f64,
    pub width: f64,
    pub height: f64,
}

const PENDING: u32 = 0;
const SELECTED: u32 = 1;
const NO_SELECTION: u32 = 2;
const UNSUPPORTED: u32 = 3;
const FAILED: u32 = 4;
const INVALID_ARGUMENT: u32 = 5;

fn invalid() -> SelectionError {
    SelectionError {
        kind: ErrorKind::InvalidConfiguration,
        operation: "C ABI arguments",
        native_code: None,
        clipboard_status: ClipboardStatus::Unchanged,
    }
}

unsafe fn array<'a, T>(
    pointer: *const T,
    count: usize,
    limit: usize,
) -> Result<&'a [T], SelectionError> {
    if count > limit || (count > 0 && pointer.is_null()) {
        return Err(invalid());
    }
    if count == 0 {
        Ok(&[])
    } else {
        Ok(unsafe { std::slice::from_raw_parts(pointer, count) })
    }
}

unsafe fn options(
    pointer: *const SnowSelectedTextOptions,
) -> Result<CaptureOptions, SelectionError> {
    if pointer.is_null() {
        return Ok(CaptureOptions::default());
    }
    // Read only the mandatory prefix until the caller's size/version have been checked.
    let size = unsafe { ptr::addr_of!((*pointer).struct_size).read() };
    if size != size_of::<SnowSelectedTextOptions>() as u32 {
        return Err(invalid());
    }
    let options = unsafe { &*pointer };
    if options.abi_version != 1 || options.copy_fallback > 1 {
        return Err(invalid());
    }
    let strategy = match options.strategy {
        0 => CaptureStrategy::Auto,
        1 => CaptureStrategy::Uia,
        2 => CaptureStrategy::NativeEdit,
        3 => CaptureStrategy::Clipboard,
        _ => return Err(invalid()),
    };
    let windows = unsafe {
        array(
            options.excluded_windows,
            options.excluded_window_count,
            1024,
        )
    }?
    .to_vec();
    let executables = unsafe {
        array(
            options.excluded_executables,
            options.excluded_executable_count,
            1024,
        )
    }?
    .iter()
    .map(|name| {
        let bytes = unsafe { array(name.data, name.length, 1024) }?;
        std::str::from_utf8(bytes)
            .map(str::to_owned)
            .map_err(|_| invalid())
    })
    .collect::<Result<Vec<_>, _>>()?;
    let options = CaptureOptions {
        timeout: Duration::from_millis(u64::from(options.timeout_ms)),
        copy_fallback: options.copy_fallback != 0,
        strategy,
        max_text_bytes: options.max_text_bytes,
        excluded_windows: windows,
        excluded_executables: executables,
    };
    options.validate()?;
    Ok(options)
}

unsafe fn report(error: Option<&SelectionError>, output: *mut SnowSelectedTextError) {
    if let Some(output) = unsafe { output.as_mut() } {
        *output = error.map(SnowSelectedTextError::from).unwrap_or_default();
    }
}

fn status(result: &CaptureResult) -> u32 {
    match result {
        Ok(SelectionOutcome::Selected(_)) => SELECTED,
        Ok(SelectionOutcome::NoSelection) => NO_SELECTION,
        Ok(SelectionOutcome::Unsupported) => UNSUPPORTED,
        Err(_) => FAILED,
    }
}

fn selected(result: &SnowSelectedTextResult) -> Option<&SelectedText> {
    match result.0.as_ref() {
        Ok(SelectionOutcome::Selected(text)) => Some(text),
        _ => None,
    }
}

/// Initialize a full v1 options structure. Returns 0 on null, 1 on success.
/// # Safety
/// `output` must be null or writable for a full options structure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_options_init(
    output: *mut SnowSelectedTextOptions,
) -> u8 {
    let Some(output) = (unsafe { output.as_mut() }) else {
        return 0;
    };
    *output = SnowSelectedTextOptions::default();
    1
}

/// Return 0 on success, otherwise an error kind. Error output is optional.
/// # Safety
/// Output pointers must be null or writable for their declared types.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_service_create(
    output: *mut *mut SnowSelectedTextService,
    error: *mut SnowSelectedTextError,
) -> u32 {
    let result = (|| {
        if output.is_null() {
            return Err(invalid());
        }
        unsafe {
            output.write(ptr::null_mut());
        }
        let service = SelectedTextService::new()?;
        unsafe {
            output.write(Box::into_raw(Box::new(SnowSelectedTextService(service))));
        }
        Ok(())
    })();
    unsafe {
        report(result.as_ref().err(), error);
    }
    result.err().map_or(0, |e| e.kind as u32)
}

/// Release a service handle without waiting for workers.
/// # Safety
/// `service` must be null or a live service handle, exclusively owned by this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_service_destroy(service: *mut SnowSelectedTextService) {
    if !service.is_null() {
        drop(unsafe { Box::from_raw(service) });
    }
}

/// Capture foreground context and submit work; null options selects defaults.
/// # Safety
/// Handles and input arrays must be live; output/error must be writable if non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_start(
    service: *const SnowSelectedTextService,
    config: *const SnowSelectedTextOptions,
    output: *mut *mut SnowSelectedTextRequest,
    error: *mut SnowSelectedTextError,
) -> u32 {
    let result = (|| {
        if output.is_null() {
            return Err(invalid());
        }
        unsafe {
            output.write(ptr::null_mut());
        }
        let config = unsafe { options(config) }?;
        let service = unsafe { service.as_ref() }.ok_or_else(invalid)?;
        let request = service.0.start_capture(config)?;
        unsafe {
            output.write(Box::into_raw(Box::new(SnowSelectedTextRequest(request))));
        }
        Ok(())
    })();
    unsafe {
        report(result.as_ref().err(), error);
    }
    result.err().map_or(0, |e| e.kind as u32)
}

/// Poll without allocating an output handle. Returns a request status.
/// # Safety
/// `request` must be null or a live request handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_request_poll(
    request: *const SnowSelectedTextRequest,
) -> u32 {
    let Some(request) = (unsafe { request.as_ref() }) else {
        return INVALID_ARGUMENT;
    };
    request
        .0
        .try_result()
        .map_or(PENDING, |result| status(&result))
}

/// Obtain an independently owned terminal result. Each successful call creates one handle.
/// # Safety
/// `request` must be live or null; `output` must be writable if non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_request_result(
    request: *const SnowSelectedTextRequest,
    output: *mut *mut SnowSelectedTextResult,
) -> u32 {
    if output.is_null() {
        return INVALID_ARGUMENT;
    }
    unsafe {
        output.write(ptr::null_mut());
    }
    let Some(request) = (unsafe { request.as_ref() }) else {
        return INVALID_ARGUMENT;
    };
    let Some(result) = request.0.try_result() else {
        return PENDING;
    };
    let status = status(&result);
    unsafe {
        output.write(Box::into_raw(Box::new(SnowSelectedTextResult(result))));
    }
    status
}

/// Cancellation is idempotent; a previously completed result remains unchanged.
/// # Safety
/// `request` must be null or a live request handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_request_cancel(
    request: *const SnowSelectedTextRequest,
) {
    if let Some(request) = unsafe { request.as_ref() } {
        request.0.cancel();
    }
}

/// Cancel and destroy a request without waiting for workers.
/// # Safety
/// `request` must be null or a live request handle, exclusively owned by this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_request_destroy(request: *mut SnowSelectedTextRequest) {
    if !request.is_null() {
        drop(unsafe { Box::from_raw(request) });
    }
}

/// Destroy a result and invalidate its borrowed views.
/// # Safety
/// `result` must be null or a live result handle, exclusively owned by this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_result_destroy(result: *mut SnowSelectedTextResult) {
    if !result.is_null() {
        drop(unsafe { Box::from_raw(result) });
    }
}

/// Read terminal status from an independently owned result.
/// # Safety
/// `result` must be null or a live result handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_result_status(
    result: *const SnowSelectedTextResult,
) -> u32 {
    unsafe { result.as_ref() }.map_or(INVALID_ARGUMENT, |result| status(&result.0))
}

/// Read an error, returning 1 if this is a failed result; otherwise clear output and return 0.
/// # Safety
/// `result` must be live or null; `output` must be writable if non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_result_error(
    result: *const SnowSelectedTextResult,
    output: *mut SnowSelectedTextError,
) -> u8 {
    if output.is_null() {
        return 0;
    }
    let error = unsafe { result.as_ref() }.and_then(|result| result.0.as_ref().as_ref().err());
    unsafe {
        report(error, output);
    }
    u8::from(error.is_some())
}

/// Read combined UTF-8 text, returning 1 on success. Text is not NUL-terminated.
/// # Safety
/// `result` must be live or null; `output` must be writable if non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_result_text(
    result: *const SnowSelectedTextResult,
    output: *mut SnowSelectedTextBytes,
) -> u8 {
    let Some(output) = (unsafe { output.as_mut() }) else {
        return 0;
    };
    *output = SnowSelectedTextBytes::default();
    let Some(text) = unsafe { result.as_ref() }.and_then(selected) else {
        return 0;
    };
    *output = SnowSelectedTextBytes::view(&text.text);
    1
}

/// Read source and method metadata, returning 1 on success.
/// # Safety
/// `result` must be live or null; `output` must be writable if non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_result_metadata(
    result: *const SnowSelectedTextResult,
    output: *mut SnowSelectedTextMetadata,
) -> u8 {
    let Some(output) = (unsafe { output.as_mut() }) else {
        return 0;
    };
    *output = SnowSelectedTextMetadata::default();
    let Some(text) = unsafe { result.as_ref() }.and_then(selected) else {
        return 0;
    };
    *output = SnowSelectedTextMetadata {
        window: text.source.window,
        process_id: text.source.process_id,
        focused_control: text.source.focused_control,
        method: text.method as u32,
        clipboard_status: text.clipboard_status as u32,
        executable: SnowSelectedTextBytes::view(&text.source.executable),
        range_count: text.ranges.len(),
    };
    1
}

/// Read one range's UTF-8 text and rectangle count, returning 1 on success.
/// # Safety
/// `result` must be live or null; both outputs must be writable if non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_result_range(
    result: *const SnowSelectedTextResult,
    index: usize,
    output: *mut SnowSelectedTextBytes,
    rectangle_count: *mut usize,
) -> u8 {
    if !output.is_null() {
        unsafe {
            output.write(SnowSelectedTextBytes::default());
        }
    }
    if !rectangle_count.is_null() {
        unsafe {
            rectangle_count.write(0);
        }
    }
    if output.is_null() || rectangle_count.is_null() {
        return 0;
    }
    let Some(range) = unsafe { result.as_ref() }
        .and_then(selected)
        .and_then(|text| text.ranges.get(index))
    else {
        return 0;
    };
    unsafe {
        output.write(SnowSelectedTextBytes::view(&range.text));
        rectangle_count.write(range.bounds.len());
    }
    1
}

/// Read a physical-screen rectangle, returning 1 on success.
/// # Safety
/// `result` must be live or null; `output` must be writable if non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_selected_text_result_rect(
    result: *const SnowSelectedTextResult,
    range: usize,
    index: usize,
    output: *mut SnowSelectedTextRect,
) -> u8 {
    let Some(output) = (unsafe { output.as_mut() }) else {
        return 0;
    };
    *output = SnowSelectedTextRect::default();
    let Some(rect) = unsafe { result.as_ref() }
        .and_then(selected)
        .and_then(|text| text.ranges.get(range))
        .and_then(|range| range.bounds.get(index))
    else {
        return 0;
    };
    *output = SnowSelectedTextRect {
        left: rect.left,
        top: rect.top,
        width: rect.width,
        height: rect.height,
    };
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    fn result() -> SnowSelectedTextResult {
        SnowSelectedTextResult(Arc::new(Ok(SelectionOutcome::Selected(SelectedText {
            text: "中文\0😀".into(),
            ranges: vec![SelectedRange {
                text: "中文\0😀".into(),
                bounds: vec![SelectionRect {
                    left: -100.0,
                    top: 5.0,
                    width: 20.0,
                    height: 10.0,
                }],
            }],
            source: SourceWindow {
                window: 1,
                process_id: 2,
                focused_control: 3,
                executable: "edit.exe".into(),
            },
            method: RetrievalMethod::Uia,
            clipboard_status: ClipboardStatus::Unchanged,
        }))))
    }

    #[test]
    fn options_select_only_known_strategies_before_reading_arrays() {
        assert_eq!(
            unsafe { options(ptr::null()) }.unwrap().strategy,
            CaptureStrategy::Auto
        );
        for strategy in [
            CaptureStrategy::Auto,
            CaptureStrategy::Uia,
            CaptureStrategy::NativeEdit,
            CaptureStrategy::Clipboard,
        ] {
            for copy_fallback in [0, 1] {
                let config = SnowSelectedTextOptions {
                    strategy: strategy as u32,
                    copy_fallback,
                    ..Default::default()
                };
                let parsed = unsafe { options(&config) }.unwrap();
                assert_eq!(parsed.strategy, strategy);
                assert_eq!(parsed.copy_fallback, copy_fallback != 0);
            }
        }
        for strategy in [4, u32::MAX] {
            let config = SnowSelectedTextOptions {
                strategy,
                excluded_windows: std::ptr::dangling(),
                excluded_window_count: 1,
                ..Default::default()
            };
            assert_eq!(
                unsafe { options(&config) }.unwrap_err().kind,
                ErrorKind::InvalidConfiguration
            );
        }
    }

    #[test]
    fn options_defaults_match_rust_and_validate_abi_prefix_before_arrays() {
        let mut config = SnowSelectedTextOptions::default();
        unsafe {
            assert_eq!(snow_selected_text_options_init(&mut config), 1);
            assert_eq!(snow_selected_text_options_init(ptr::null_mut()), 0);
            assert!(options(&config).unwrap().copy_fallback);
            assert_eq!(options(&config).unwrap().strategy, CaptureStrategy::Auto);
            assert_eq!(
                options(ptr::null()).unwrap().timeout,
                Duration::from_secs(2)
            );
            for (size, version, copy) in [
                (0, 1, 1),
                (config.struct_size, 2, 1),
                (config.struct_size, 1, 2),
            ] {
                let invalid_config = SnowSelectedTextOptions {
                    struct_size: size,
                    abi_version: version,
                    copy_fallback: copy,
                    ..config
                };
                assert_eq!(
                    options(&invalid_config).unwrap_err().kind,
                    ErrorKind::InvalidConfiguration
                );
            }
            config.excluded_window_count = 1;
            assert!(options(&config).is_err());
            config.excluded_window_count = 0;
            config.timeout_ms = 0;
            assert!(options(&config).is_err());
        }
    }

    #[test]
    fn options_copy_exclusion_arrays_and_reject_invalid_utf8() {
        let windows = [1usize, 2];
        let name = SnowSelectedTextBytes::view("APP.EXE");
        let config = SnowSelectedTextOptions {
            excluded_windows: windows.as_ptr(),
            excluded_window_count: windows.len(),
            excluded_executables: &name,
            excluded_executable_count: 1,
            ..Default::default()
        };
        let parsed = unsafe { options(&config) }.unwrap();
        assert_eq!(parsed.excluded_windows, windows);
        assert_eq!(parsed.excluded_executables, ["APP.EXE"]);
        let bytes = [255u8];
        let name = SnowSelectedTextBytes {
            data: bytes.as_ptr(),
            length: 1,
        };
        assert!(
            unsafe {
                options(&SnowSelectedTextOptions {
                    excluded_executables: &name,
                    ..config
                })
            }
            .is_err()
        );
    }

    #[test]
    fn result_views_preserve_utf8_lengths_nuls_metadata_and_bounds() {
        let result = result();
        let mut text = SnowSelectedTextBytes::default();
        let mut metadata = SnowSelectedTextMetadata::default();
        let mut rect = SnowSelectedTextRect::default();
        let mut count = 0;
        unsafe {
            assert_eq!(snow_selected_text_result_text(&result, &mut text), 1);
            assert_eq!(
                std::slice::from_raw_parts(text.data, text.length),
                "中文\0😀".as_bytes()
            );
            assert_eq!(
                snow_selected_text_result_metadata(&result, &mut metadata),
                1
            );
            assert_eq!(metadata.range_count, 1);
            assert_eq!(metadata.window, 1);
            assert_eq!(
                snow_selected_text_result_range(&result, 0, &mut text, &mut count),
                1
            );
            assert_eq!(count, 1);
            assert_eq!(snow_selected_text_result_rect(&result, 0, 0, &mut rect), 1);
            assert_eq!(rect.left, -100.0);
            assert_eq!(
                snow_selected_text_result_range(&result, usize::MAX, &mut text, &mut count),
                0
            );
            assert_eq!(count, 0);
            assert!(text.data.is_null());
            assert_eq!(snow_selected_text_result_rect(&result, 0, 1, &mut rect), 0);
            assert_eq!(rect.width, 0.0);
        }
    }

    #[test]
    fn null_arguments_are_explicit_and_do_not_create_workers() {
        unsafe {
            let mut error = SnowSelectedTextError::default();
            assert_eq!(
                snow_selected_text_service_create(ptr::null_mut(), &mut error),
                ErrorKind::InvalidConfiguration as u32
            );
            let mut output = std::ptr::dangling_mut::<SnowSelectedTextRequest>();
            assert_eq!(
                snow_selected_text_start(ptr::null(), ptr::null(), &mut output, &mut error),
                ErrorKind::InvalidConfiguration as u32
            );
            assert!(output.is_null());
            assert_eq!(
                snow_selected_text_request_poll(ptr::null()),
                INVALID_ARGUMENT
            );
            let mut result = std::ptr::dangling_mut::<SnowSelectedTextResult>();
            assert_eq!(
                snow_selected_text_request_result(ptr::null(), &mut result),
                INVALID_ARGUMENT
            );
            assert!(result.is_null());
            snow_selected_text_service_destroy(ptr::null_mut());
            snow_selected_text_request_destroy(ptr::null_mut());
            snow_selected_text_result_destroy(ptr::null_mut());
            snow_selected_text_request_cancel(ptr::null());
        }
    }

    #[test]
    fn error_status_mapping_and_result_ownership_are_independent() {
        let error = SelectionError {
            kind: ErrorKind::AccessDenied,
            operation: "test operation",
            native_code: Some(-1),
            clipboard_status: ClipboardStatus::RestorationFailed,
        };
        let first = SnowSelectedTextResult(Arc::new(Err(error)));
        let second = Box::into_raw(Box::new(SnowSelectedTextResult(first.0.clone())));
        drop(first);
        let mut output = SnowSelectedTextError::default();
        unsafe {
            assert_eq!(snow_selected_text_result_error(second, &mut output), 1);
            assert_eq!(output.kind, ErrorKind::AccessDenied as u32);
            assert_eq!(output.native_code, -1);
            assert_eq!(
                output.clipboard_status,
                ClipboardStatus::RestorationFailed as u32
            );
            snow_selected_text_result_destroy(second);
        }
        assert_eq!(status(&Ok(SelectionOutcome::NoSelection)), NO_SELECTION);
        assert_eq!(status(&Ok(SelectionOutcome::Unsupported)), UNSUPPORTED);
    }
}

#[cfg(all(any(windows, target_os = "macos"), feature = "crash-diagnostics"))]
unsafe extern "C" {
    fn snow_diag_attach(
        pipe: *const std::ffi::c_char,
        session: *const std::ffi::c_char,
        version: *const std::ffi::c_char,
    ) -> i32;
    fn snow_diag_panic(location: *const u8, length: usize);
    fn snow_diag_breadcrumb(record: *const std::ffi::c_char, length: usize);
}

pub fn operation_started(id: u64) {
    #[cfg(all(any(windows, target_os = "macos"), feature = "crash-diagnostics"))]
    {
        let record = format!("ocr.operation_started id={id}\n");
        unsafe { snow_diag_breadcrumb(record.as_ptr().cast(), record.len()) };
    }
    #[cfg(not(all(any(windows, target_os = "macos"), feature = "crash-diagnostics")))]
    let _ = id;
}

#[cfg(all(any(windows, target_os = "macos"), feature = "crash-diagnostics"))]
extern "C" fn panic_callback(location: *const u8, length: usize) {
    // The installed hook lends a bounded buffer for this synchronous call.
    unsafe { snow_diag_panic(location, length) };
}

pub fn initialize() {
    #[cfg(all(any(windows, target_os = "macos"), feature = "crash-diagnostics"))]
    {
        use std::ffi::CString;
        let Ok(pipe) = std::env::var("SNOW_SHOT_CRASHPAD_PIPE") else {
            return;
        };
        let session = std::env::var("SNOW_SHOT_DIAGNOSTICS_SESSION").unwrap_or_default();
        let (Ok(pipe), Ok(session)) = (CString::new(pipe), CString::new(session)) else {
            return;
        };
        let version =
            CString::new(env!("CARGO_PKG_VERSION")).expect("package version contains no NUL");
        if unsafe { snow_diag_attach(pipe.as_ptr(), session.as_ptr(), version.as_ptr()) } != 0 {
            snow_diagnostics::install_panic_hook(panic_callback);
            eprintln!("snow.diagnostics: crash capture registered");
        } else {
            eprintln!("snow.diagnostics: crash capture registration failed");
        }
    }
}

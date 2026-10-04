//! Test-only entry points, linked exclusively into isolated crash fixtures.
#[path = "../../snow-ocr-process/src/diagnostics.rs"]
mod ocr_diagnostics;

// The macOS diagnostics host applies the real worker QoS policy. Keep that
// policy in this fixture's Rust archive so the host links only one Rust runtime.
#[cfg(target_os = "macos")]
#[unsafe(no_mangle)]
pub extern "C" fn snow_application_qos_apply_current_thread() -> i32 {
    snow_core::qos::apply_current_thread_result()
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_test_rust_panic(callback: snow_diagnostics::PanicCallback) {
    snow_diagnostics::install_panic_hook(callback);
    panic!("private panic payload must not be logged");
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_test_ocr_initialize() {
    ocr_diagnostics::initialize();
    ocr_diagnostics::operation_started(1);
}

#![allow(clippy::missing_safety_doc)]

#[unsafe(no_mangle)]
pub extern "C" fn snow_diagnostics_install_panic_hook(callback: snow_diagnostics::PanicCallback) {
    snow_diagnostics::install_panic_hook(callback);
}

// Public module re-exports keep shared C-ABI entry points reachable while Cargo
// packages the FFI crates and the Rust runtime into one static archive. Full
// enables selected-text here; Mini keeps the default feature-free archive.
#[cfg(feature = "selected-text")]
pub mod selected_text {
    pub use snow_selected_text_c::*;
}

pub mod capture {
    pub use snow_capture_c::*;
}

pub mod draw_engine {
    pub use snow_draw_engine_c::*;
}

pub mod stitch_images {
    pub use snow_stitch_images_c::*;
}

#[cfg(any(windows, target_os = "macos"))]
pub mod ui_selector {
    pub use snow_ui_selector_c::*;
}

pub mod recording_effects {
    pub use snow_recording_effects_c::*;
}

pub mod visual_region_detector {
    pub use snow_visual_region_detector_c::*;
}

pub mod recording {
    pub use snow_recording_c::*;
}

// Launch-scoped policy shared by native Qt workers and Rust workers.
#[unsafe(no_mangle)]
pub extern "C" fn snow_application_qos_initialize(
    level: u32,
    report_error: Option<snow_core::qos::ErrorHandler>,
) -> i32 {
    let Some(qos) = snow_core::qos::ApplicationQos::from_code(level) else {
        return -1;
    };
    if snow_core::qos::initialize_application_qos(qos, report_error) {
        0
    } else {
        -2
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_application_qos_apply_current_thread() -> i32 {
    snow_core::qos::apply_current_thread_result()
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_application_qos_active() -> u32 {
    snow_core::qos::active_application_qos().map_or(u32::MAX, |qos| qos as u32)
}

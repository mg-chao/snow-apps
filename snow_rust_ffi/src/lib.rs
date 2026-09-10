#![allow(clippy::missing_safety_doc)]

#[unsafe(no_mangle)]
pub extern "C" fn snow_diagnostics_install_panic_hook(callback: snow_diagnostics::PanicCallback) {
    snow_diagnostics::install_panic_hook(callback);
}

// Public module re-exports keep every C-ABI entry point reachable while Cargo
// packages the FFI crates and the Rust runtime into one static archive.
pub mod capture {
    pub use snow_capture_c::*;
}

pub mod draw_engine {
    pub use snow_draw_engine_c::*;
}

pub mod stitch_images {
    pub use snow_stitch_images_c::*;
}

pub mod ui_selector {
    pub use snow_ui_selector_c::*;
}

pub mod selected_text {
    pub use snow_selected_text_c::*;
}

pub mod recording_effects {
    pub use snow_recording_effects_c::*;
}

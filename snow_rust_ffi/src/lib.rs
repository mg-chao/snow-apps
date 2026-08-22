#![allow(clippy::missing_safety_doc)]

// Public module re-exports keep every C-ABI entry point reachable while Cargo
// packages the five FFI crates and the Rust runtime into one static archive.
pub mod capture {
    pub use snow_capture_c::*;
}

pub mod draw_engine {
    pub use snow_draw_engine_c::*;
}

pub mod ocr {
    pub use snow_ocr_c::*;
}

pub mod stitch_images {
    pub use snow_stitch_images_c::*;
}

pub mod ui_selector {
    pub use snow_ui_selector_c::*;
}

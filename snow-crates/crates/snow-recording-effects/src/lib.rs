//! Timestamp-driven input effects shared by video composition and canvas previews.
pub mod keyboard_hook;
pub mod keyboard_overlay;
pub mod keyboard_rasterizer;
pub mod laser_trail;
pub mod mouse_effects;
pub mod mouse_hook;
pub mod preview;
pub mod surface;
pub use keyboard_overlay::KeyboardOverlayConfig;

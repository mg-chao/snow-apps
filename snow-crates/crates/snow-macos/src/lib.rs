//! Owned, deadline-bound macOS media acquisition. No legacy screenshot APIs.
#![cfg(target_os = "macos")]
pub mod content;
mod error;
pub mod permission;
pub use error::{MacError, MacResult};
pub mod audio;
pub mod capabilities;
pub mod capture;
pub mod compositor;
pub mod desktop;
pub mod microphone;
pub mod run_loop;
pub mod time;

pub mod cursor;
mod deadline;
pub mod input;
pub mod text;
pub use snow_core::cancellation::CancellationToken;

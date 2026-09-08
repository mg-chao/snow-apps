//! Acquire the foreground application's selected text without blocking its UI.
//!
//! Call [`SelectedTextService::start_capture`] before activating your own window.
//! Poll the returned [`CaptureRequest`] from your event loop. Copy fallback is
//! enabled by default and may change the clipboard; inspect [`ClipboardStatus`].
//! See the crate README for compatibility and timeout guarantees.

#[cfg(any(windows, test))]
mod clipboard;
mod model;
#[cfg(any(windows, test))]
mod policy;
mod runtime;

#[cfg(windows)]
mod platform;

pub use model::*;
pub use runtime::{CaptureRequest, SelectedTextService};

#[cfg(test)]
mod tests;

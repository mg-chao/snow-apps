#[cfg(target_os = "macos")]
mod macos;
#[cfg(windows)]
mod windows;

#[cfg(target_os = "macos")]
pub(crate) use macos::{Worker, capture_source};
#[cfg(windows)]
pub(crate) use windows::{Worker, capture_source};

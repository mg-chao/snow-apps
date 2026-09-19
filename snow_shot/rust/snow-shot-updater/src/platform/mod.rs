#[cfg(not(windows))]
use std::path::Path;

#[cfg(not(windows))]
use crate::error::{Result, UpdateError, io_error};

#[cfg(windows)]
mod windows_impl;
#[cfg(windows)]
mod windows_startup;

#[cfg(windows)]
pub use windows_impl::*;

#[cfg(not(windows))]
pub fn replace_file(source: &Path, destination: &Path) -> Result<()> {
    std::fs::rename(source, destination).map_err(|error| {
        io_error(
            "update_file_replace_failed",
            "Could not replace an update file; close applications using it",
            error,
        )
    })
}

#[cfg(not(windows))]
pub fn path_has_reparse(_path: &Path) -> bool {
    false
}

#[cfg(not(windows))]
pub fn write_registered_version(_root: &Path, _version: &str) -> Result<()> {
    Ok(())
}

#[cfg(not(windows))]
pub fn registered_target_matches(_root: &Path) -> bool {
    false
}

#[cfg(not(windows))]
pub fn remove_installation_startup(_root: &Path) -> Result<()> {
    Ok(())
}

#[cfg(not(windows))]
pub fn migrate_installation_startup(_previous: &Path, _root: &Path) -> Result<()> {
    Ok(())
}

#[cfg(not(windows))]
pub fn launch_on_interactive_desktop(_executable: &Path) -> Result<bool> {
    Ok(false)
}

#[cfg(not(windows))]
pub fn process_path_for_pid(_pid: u32) -> Result<std::path::PathBuf> {
    Err(UpdateError::new(
        "unsupported_platform",
        "The update target does not match its application process",
    ))
}

#[cfg(not(windows))]
pub fn launch_elevated(_executable: &Path, _arguments: &[String]) -> Result<bool> {
    Ok(false)
}

#[cfg(not(windows))]
pub fn path_eq(left: &Path, right: &Path) -> bool {
    left == right
}

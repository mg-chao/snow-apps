//! Windows platform layer: file system, processes, pipes, registry, COM.

pub mod desktop;
pub mod fsx;
pub mod peer;
pub mod pipes;
pub mod processx;
pub mod registryx;
pub mod startup;

use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::path::PathBuf;

/// NUL-terminated UTF-16 for Win32 APIs taking pointers.
pub fn wide(text: &OsStr) -> Vec<u16> {
    text.encode_wide().chain(std::iter::once(0)).collect()
}

/// NUL-terminated UTF-16 from a Rust string.
pub fn wide_str(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

/// Decodes a wide buffer (NUL-terminated or explicit length) into a path.
pub fn path_from_wide(text: &[u16]) -> PathBuf {
    let end = text.iter().position(|c| *c == 0).unwrap_or(text.len());
    use std::os::windows::ffi::OsStringExt;
    PathBuf::from(std::ffi::OsString::from_wide(&text[..end]))
}

/// `HRESULT_FROM_WIN32` used by the historical diagnostics formatting.
pub fn hresult_from_win32(code: u32) -> u32 {
    if code as i32 <= 0 {
        code
    } else {
        0x80070000u32 | (code & 0xFFFF)
    }
}

/// The raw diagnostic text the previous helper produced for failed Win32
/// calls; these strings were never part of the translation catalog.
pub fn windows_error(code: u32) -> crate::errors::Error {
    crate::errors::Error::raw(format!("Windows error 0x{code:08X}"))
}

pub fn windows_error_from_hresult(code: i32) -> crate::errors::Error {
    crate::errors::Error::raw(format!("Windows error 0x{:08X}", code as u32))
}

/// Last-error as the raw diagnostic.
pub fn last_windows_error() -> crate::errors::Error {
    windows_error(unsafe { windows::Win32::Foundation::GetLastError().0 } as u32)
}

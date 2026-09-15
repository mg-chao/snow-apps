use crate::error::{CaptureError, CaptureResult};

/// A platform-tagged, session-scoped window identity. WindowServer IDs and
/// HWND values occupy different namespaces, even when their numbers coincide.
/// Never persist this as a durable identity across sessions or target loss.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum WindowId {
    Windows { handle: isize },
    MacOS { id: u32 },
}
pub(crate) type WindowKey = WindowId;
impl WindowId {
    pub const fn from_windows_handle(handle: isize) -> Self {
        Self::Windows { handle }
    }
    pub const fn from_macos_id(id: u32) -> Self {
        Self::MacOS { id }
    }
    pub fn windows_handle(self) -> CaptureResult<isize> {
        match self {
            Self::Windows { handle } => Ok(handle),
            _ => Err(CaptureError::InvalidTarget(
                "expected a Windows window identity".into(),
            )),
        }
    }
    pub fn macos_id(self) -> CaptureResult<u32> {
        match self {
            Self::MacOS { id } => Ok(id),
            _ => Err(CaptureError::InvalidTarget(
                "expected a macOS window identity".into(),
            )),
        }
    }
    pub fn session_id(&self) -> String {
        match self {
            Self::Windows { handle } => format!("windows:{:016x}", *handle as usize as u64),
            Self::MacOS { id } => format!("macos:{id}"),
        }
    }
    pub(crate) const fn key(&self) -> WindowKey {
        *self
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn native_window_namespaces_do_not_alias() {
        let windows = WindowId::from_windows_handle(42);
        let macos = WindowId::from_macos_id(42);
        assert_ne!(windows, macos);
        assert_ne!(windows.session_id(), macos.session_id());
        assert!(windows.macos_id().is_err());
        assert!(macos.windows_handle().is_err());
        assert_eq!(macos.macos_id().unwrap(), 42);
    }
}

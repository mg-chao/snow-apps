use crate::{MacError, MacResult};
use objc2_core_graphics::{CGPreflightScreenCaptureAccess, CGRequestScreenCaptureAccess};
use objc2_foundation::{NSOperatingSystemVersion, NSProcessInfo};

pub fn supported_os() -> bool {
    NSProcessInfo::processInfo().isOperatingSystemAtLeastVersion(NSOperatingSystemVersion {
        majorVersion: 15,
        minorVersion: 0,
        patchVersion: 0,
    })
}
pub fn screen_capture_authorized() -> bool {
    CGPreflightScreenCaptureAccess()
}
/// Explicit host-initiated request; ordinary acquisition never prompts implicitly.
pub fn request_screen_capture_access() -> bool {
    CGRequestScreenCaptureAccess()
}
pub(crate) fn check_access() -> MacResult<()> {
    if objc2::MainThreadMarker::new().is_some() {
        return Err(MacError::InvalidConfig("blocking capture APIs must run on a worker while the host main run loop remains active".into()));
    }
    if !supported_os() {
        return Err(MacError::UnsupportedOs);
    }
    if !screen_capture_authorized() {
        return Err(MacError::PermissionDenied);
    }
    Ok(())
}

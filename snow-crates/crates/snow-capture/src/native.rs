//! Explicit native macOS acquisition, including floating-point HDR snapshots.
//! The returned CoreVideo lease can be read by Metal/VideoToolbox without CPU mapping.
//! Callers that need bytes request `image.to_cpu()` explicitly.
use crate::error::CaptureResult;
use crate::platform::macos::map_error;
pub use snow_macos::capture::{
    CaptureConfig as NativeCaptureConfig, NativeFrame, Target as NativeCaptureTarget,
    VideoStream as NativeCaptureStream,
};
pub use snow_macos::content::{DisplayInfo, WindowInfo};
pub use snow_macos::permission::{request_screen_capture_access, screen_capture_authorized};
pub use snow_media::{
    CursorMode, DynamicRange,
    geometry::{DesktopRect, DesktopSpace, PixelSize},
};
pub fn capture_native(config: &NativeCaptureConfig) -> CaptureResult<NativeFrame> {
    snow_macos::capture::screenshot(config).map_err(map_error)
}
pub fn enumerate_displays() -> CaptureResult<Vec<DisplayInfo>> {
    snow_macos::content::displays(std::time::Duration::from_secs(5)).map_err(map_error)
}
pub fn enumerate_windows() -> CaptureResult<Vec<WindowInfo>> {
    snow_macos::content::windows(std::time::Duration::from_secs(5)).map_err(map_error)
}

pub use snow_macos::capabilities::CaptureSupport;
pub use snow_macos::desktop::{
    DesktopConfig, DesktopEvent, DesktopFrame, DesktopSession, DesktopTarget, DisplayId,
    TargetCapabilities, WindowId,
};

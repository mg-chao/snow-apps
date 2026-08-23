use std::sync::Arc;

#[cfg(not(target_os = "windows"))]
use crate::backend::MonitorCapturer;
use crate::backend::{AutoBackendPolicy, CaptureBackend, CaptureBackendKind, CaptureMode};
#[cfg(not(target_os = "windows"))]
use crate::error::{CaptureError, CaptureResult};
#[cfg(not(target_os = "windows"))]
use crate::monitor::MonitorId;
use crate::region::MonitorLayout;
#[cfg(not(target_os = "windows"))]
use crate::window::WindowId;

#[cfg(target_os = "windows")]
pub(crate) mod windows;

#[cfg(not(target_os = "windows"))]
fn unsupported_error() -> CaptureError {
    CaptureError::platform(anyhow::anyhow!(
        "screen capture is only supported on Windows"
    ))
}

#[cfg(not(target_os = "windows"))]
struct UnsupportedBackend;

#[cfg(not(target_os = "windows"))]
impl CaptureBackend for UnsupportedBackend {
    fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        Err(unsupported_error())
    }

    fn primary_monitor(&self) -> CaptureResult<MonitorId> {
        Err(unsupported_error())
    }

    fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
        Err(unsupported_error())
    }

    fn inspect_window(
        &self,
        _window: &WindowId,
    ) -> CaptureResult<crate::capture_session::CaptureTargetInfo> {
        Err(unsupported_error())
    }

    fn create_monitor_capturer(
        &self,
        _monitor: &MonitorId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Err(unsupported_error())
    }

    fn create_window_capturer(
        &self,
        _window: &WindowId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Err(unsupported_error())
    }
}

#[cfg(target_os = "windows")]
pub(crate) fn build_backend(
    kind: CaptureBackendKind,
    auto_policy: AutoBackendPolicy,
    auto_policy_is_explicit: bool,
) -> crate::error::CaptureResult<Arc<dyn CaptureBackend>> {
    build_backend_for_mode(
        kind,
        auto_policy,
        auto_policy_is_explicit,
        CaptureMode::Continuous,
    )
}

#[cfg(target_os = "windows")]
pub(crate) fn build_backend_for_mode(
    kind: CaptureBackendKind,
    auto_policy: AutoBackendPolicy,
    auto_policy_is_explicit: bool,
    _mode: CaptureMode,
) -> crate::error::CaptureResult<Arc<dyn CaptureBackend>> {
    let backend =
        windows::WindowsBackend::with_kind_and_policy(kind, auto_policy, auto_policy_is_explicit)?;
    Ok(Arc::new(backend))
}

#[cfg(target_os = "windows")]
pub(crate) fn monitor_layout_from_monitors(
    monitors: Vec<crate::monitor::MonitorId>,
) -> crate::error::CaptureResult<MonitorLayout> {
    windows::monitor::snapshot_layout_from_monitors(monitors)
}

#[cfg(not(target_os = "windows"))]
pub(crate) fn build_backend(
    _kind: CaptureBackendKind,
    _auto_policy: AutoBackendPolicy,
    _auto_policy_is_explicit: bool,
) -> crate::error::CaptureResult<Arc<dyn CaptureBackend>> {
    build_backend_for_mode(
        _kind,
        _auto_policy,
        _auto_policy_is_explicit,
        CaptureMode::Continuous,
    )
}

#[cfg(not(target_os = "windows"))]
pub(crate) fn build_backend_for_mode(
    _kind: CaptureBackendKind,
    _auto_policy: AutoBackendPolicy,
    _auto_policy_is_explicit: bool,
    _mode: CaptureMode,
) -> crate::error::CaptureResult<Arc<dyn CaptureBackend>> {
    Ok(Arc::new(UnsupportedBackend))
}

#[cfg(not(target_os = "windows"))]
pub(crate) fn monitor_layout_from_monitors(
    _monitors: Vec<crate::monitor::MonitorId>,
) -> crate::error::CaptureResult<MonitorLayout> {
    Err(unsupported_error())
}

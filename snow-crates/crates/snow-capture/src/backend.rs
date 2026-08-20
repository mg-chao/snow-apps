use std::sync::Arc;
use std::time::Instant;

use crate::capture_session::CaptureTargetInfo;
use crate::error::CaptureError;
use crate::error::CaptureResult;
use crate::frame::{DirtyRect, Frame};
use crate::monitor::MonitorId;
use crate::region::MonitorLayout;
use crate::window::WindowId;
use snow_core::timestamp::TickFormat;
use snow_cursor::CursorSnapshot;

/// Capture workload used to tune backend behavior for latency/throughput.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CaptureWorkload {
    /// Favor low-overhead single-shot behavior for snapshots.
    #[default]
    Snapshot,
    /// Favor sustained throughput for continuous recording pipelines.
    Continuous,
}

pub(crate) type CaptureMode = CaptureWorkload;

/// WGC surface update strategy.
///
/// WGC's `ReportOnly` mode supplies complete surfaces and therefore permits
/// frame coalescing. `ReportAndRender` supplies ordered deltas and therefore
/// requires a retained complete baseline and lossless in-order processing.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum WgcUpdateMode {
    /// Select the production-safe complete-surface path. Future platform
    /// capability probes may select a faster proven contract without
    /// changing callers.
    #[default]
    Auto,
    /// Always request complete WGC surfaces. This is the deterministic
    /// compatibility and diagnostic mode.
    CompleteOnly,
    /// Prefer ordered WGC deltas and automatically resynchronize from a
    /// complete surface whenever continuity is uncertain.
    OrderedIncremental,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum CaptureBackendKind {
    #[default]
    Auto,

    DxgiDuplication,

    WindowsGraphicsCapture,

    Gdi,
}

impl CaptureBackendKind {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Auto => "auto",
            Self::DxgiDuplication => "dxgi",
            Self::WindowsGraphicsCapture => "wgc",
            Self::Gdi => "gdi",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AutoBackendPolicy {
    pub priority: Vec<CaptureBackendKind>,
}

impl AutoBackendPolicy {
    pub fn normalized_priority(&self) -> Vec<CaptureBackendKind> {
        let mut normalized = Vec::new();
        for kind in &self.priority {
            if *kind == CaptureBackendKind::Auto {
                continue;
            }
            if !normalized.contains(kind) {
                normalized.push(*kind);
            }
        }
        if normalized.is_empty() {
            normalized.extend(DEFAULT_AUTO_BACKEND_PRIORITY);
        }
        normalized
    }
}

impl Default for AutoBackendPolicy {
    fn default() -> Self {
        Self {
            priority: DEFAULT_AUTO_BACKEND_PRIORITY.to_vec(),
        }
    }
}

pub const DEFAULT_AUTO_BACKEND_PRIORITY: [CaptureBackendKind; 3] = [
    CaptureBackendKind::DxgiDuplication,
    CaptureBackendKind::WindowsGraphicsCapture,
    CaptureBackendKind::Gdi,
];

/// Source/destination rectangle pair used for partial monitor capture writes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct CaptureBlitRegion {
    pub src_x: u32,
    pub src_y: u32,
    pub width: u32,
    pub height: u32,
    pub dst_x: u32,
    pub dst_y: u32,
}

/// Timing and duplicate metadata produced by a capture operation.
#[derive(Clone, Debug)]
pub(crate) struct CaptureSampleMetadata {
    pub capture_time: Option<Instant>,
    pub raw_os_ticks: Option<i64>,
    pub tick_format: TickFormat,
    pub is_duplicate: bool,
    /// Changed rectangles in coordinates local to the captured target.
    pub dirty_rects: Vec<DirtyRect>,
}

impl Default for CaptureSampleMetadata {
    fn default() -> Self {
        Self {
            capture_time: None,
            raw_os_ticks: None,
            tick_format: TickFormat::RawQpc,
            is_duplicate: false,
            dirty_rects: Vec::new(),
        }
    }
}

pub(crate) trait MonitorCapturer: Send {
    /// Identifies the backend that will service the next capture.
    ///
    /// Automatic capturers update this value after selecting a concrete
    /// candidate, allowing callers to report the backend that actually
    /// produced a frame rather than the configured `auto` policy.
    fn backend_kind(&self) -> CaptureBackendKind {
        CaptureBackendKind::Auto
    }

    /// Prepare resources that are safe to retain while no capture is active.
    /// Implementations must not leave DXGI duplication or a WGC capture
    /// session open when this method returns.
    fn prewarm_environment(&mut self) -> CaptureResult<()> {
        Ok(())
    }

    fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame>;

    /// Capture a frame with an explicit hint about whether `reuse`
    /// contains the previous output from this same capture target.
    ///
    /// Backends can use this to safely enable incremental conversion
    /// paths that only update changed rows/tiles.
    fn capture_with_history_hint(
        &mut self,
        reuse: Option<Frame>,
        _destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        self.capture(reuse)
    }

    /// Optional accelerated path for writing only a source sub-rectangle
    /// into an already-allocated destination frame.
    ///
    /// Returns `Ok(Some(..))` when the backend handled the partial write
    /// directly, or `Ok(None)` to request the caller fall back to full-frame
    /// capture plus CPU blit.
    fn capture_region_into(
        &mut self,
        _blit: CaptureBlitRegion,
        _destination: &mut Frame,
        _destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        Ok(None)
    }

    /// Optional accelerated path for capturing a desktop-space region
    /// directly into an already-allocated destination frame.
    ///
    /// Coordinates are in virtual desktop space. The captured pixels are
    /// written starting at destination origin (0,0) for `width x height`.
    fn capture_desktop_region_into(
        &mut self,
        _x: i32,
        _y: i32,
        _width: u32,
        _height: u32,
        _destination: &mut Frame,
        _destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        Ok(None)
    }

    /// Set capture mode so backends can tune buffering/conversion policy.
    fn set_capture_mode(&mut self, _mode: CaptureMode) -> CaptureResult<()> {
        Ok(())
    }

    /// Sample cursor state associated with the most recently captured frame.
    ///
    /// Backends with native pointer metadata can return an absolute cursor
    /// snapshot here. Returning `Ok(None)` asks the caller to use a fallback
    /// path.
    fn sample_cursor(&mut self) -> CaptureResult<Option<CursorSnapshot>> {
        Ok(None)
    }

    /// Current desktop-space geometry of this capturer's source, when the
    /// backend can report it without a separate target-resolution query.
    fn target_info(&self) -> Option<CaptureTargetInfo> {
        None
    }

    /// Enable/disable GPU-assisted HDR/F16 conversion paths.
    ///
    /// When disabled, backends should prefer CPU conversion for HDR/F16
    /// surfaces when possible.
    fn set_gpu_hdr_conversion(&mut self, _enabled: bool) -> CaptureResult<()> {
        Ok(())
    }

    /// Enable/disable LUT-approximated HDR tone mapping.
    ///
    /// When disabled, backends should prefer precise HDR->SDR mapping.
    fn set_hdr_tonemap_lut(&mut self, _enabled: bool) -> CaptureResult<()> {
        Ok(())
    }

    /// Select the WGC source update contract. Other backends ignore this.
    fn set_wgc_update_mode(&mut self, _mode: WgcUpdateMode) -> CaptureResult<()> {
        Ok(())
    }

    /// Close active access to the capture source. Snapshot backends may also
    /// discard capture-time pixel surfaces that are expensive to retain while
    /// idle. This method is idempotent.
    fn release_capture_access(&mut self) {}

    /// Whether this capturer currently owns active OS capture access.
    fn capture_access_active(&self) -> bool {
        false
    }
}

pub(crate) trait CaptureBackend: Send + Sync {
    fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>>;
    fn primary_monitor(&self) -> CaptureResult<MonitorId>;
    fn monitor_layout(&self) -> CaptureResult<MonitorLayout>;
    fn inspect_primary_monitor(&self) -> CaptureResult<Option<CaptureTargetInfo>> {
        Ok(None)
    }
    fn create_primary_monitor_capturer(&self) -> CaptureResult<PreparedPrimaryCapturer> {
        for _ in 0..2 {
            let generation_before = self.display_generation();
            let layout = self.monitor_layout()?;
            let geometry = layout
                .monitors
                .into_iter()
                .find(|geometry| geometry.monitor.is_primary())
                .ok_or(CaptureError::NoPrimaryMonitor)?;
            let capturer = self.create_monitor_capturer(&geometry.monitor)?;
            let generation_after = self.display_generation();
            if generation_before == generation_after {
                return Ok(PreparedPrimaryCapturer {
                    capturer,
                    target_info: CaptureTargetInfo {
                        origin_x: geometry.x,
                        origin_y: geometry.y,
                        width: geometry.width,
                        height: geometry.height,
                    },
                    display_generation: generation_after,
                });
            }
        }
        Err(CaptureError::MonitorLost)
    }
    fn inspect_window(&self, _window: &WindowId) -> CaptureResult<CaptureTargetInfo> {
        Err(CaptureError::BackendUnavailable(
            "window inspection is not supported by this backend".into(),
        ))
    }
    fn inspect_window_for_backend(
        &self,
        window: &WindowId,
        _backend_kind: CaptureBackendKind,
    ) -> CaptureResult<CaptureTargetInfo> {
        self.inspect_window(window)
    }
    fn display_generation(&self) -> Option<u64> {
        None
    }
    fn refresh_display_configuration(&self) -> CaptureResult<()> {
        Ok(())
    }
    fn create_monitor_capturer(
        &self,
        monitor: &MonitorId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>>;
    fn create_window_capturer(
        &self,
        _window: &WindowId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Err(CaptureError::BackendUnavailable(
            "window capture is not supported by this backend".into(),
        ))
    }
}

pub(crate) struct PreparedPrimaryCapturer {
    pub capturer: Box<dyn MonitorCapturer>,
    pub target_info: CaptureTargetInfo,
    pub display_generation: Option<u64>,
}

pub(crate) fn backend_for_kind_with_auto_policy(
    kind: CaptureBackendKind,
    auto_policy: AutoBackendPolicy,
    auto_policy_is_explicit: bool,
) -> CaptureResult<Arc<dyn CaptureBackend>> {
    crate::platform::build_backend(kind, auto_policy, auto_policy_is_explicit)
}

#![allow(clippy::too_many_arguments)]

pub mod backend;
pub mod capture_session;
pub mod convert;
pub mod error;
pub mod frame;
pub mod monitor;
mod platform;
pub mod region;
pub mod streaming;
pub mod system;
pub mod window;

use error::CaptureResult;
use frame::Frame;

#[derive(Clone)]
pub enum CaptureTarget {
    PrimaryMonitor,

    Monitor(monitor::MonitorId),

    /// Capture a top-level window by native window handle.
    ///
    /// On the DXGI duplication backend, the window's monitor is captured
    /// and the result is cropped to the window's desktop bounds.  WGC
    /// captures the window directly. GDI captures the window directly
    /// via native Win32 window rendering.
    Window(window::WindowId),

    /// Capture a rectangular region in virtual desktop coordinates.
    /// The region may span multiple monitors. The layout is snapshotted
    /// once at session creation via [`MonitorLayout`](region::MonitorLayout).
    Region(region::CaptureRegion),
}

pub use backend::{CaptureWorkload, WgcUpdateMode};
pub use capture_session::{CaptureSession, CaptureTargetInfo};
pub use frame::{CaptureEvent, CapturedFrame, ColorSpace, DirtyRect, FrameMetadata};
pub use monitor::MonitorId;
pub use region::{CaptureRegion, MonitorLayout};
pub use streaming::{
    CaptureStream, CaptureStreamConfig, CaptureStreamStats, CaptureStreamStatsSnapshot,
};
pub use system::{CaptureOptions, CaptureSystem, CaptureSystemBuilder};
pub use window::WindowId;

/// Releases the process-wide Rayon pool used by pixel conversion.
///
/// Conversions that already acquired the pool keep it alive until they
/// finish. A later conversion recreates the pool on demand. Callers that need
/// the worker threads to exit promptly should first stop and join all capture
/// work that can perform conversion.
pub fn release_conversion_pool() {
    convert::release_pool();
}

pub fn capture_once(target: &CaptureTarget) -> CaptureResult<Frame> {
    let system = CaptureSystem::builder().build()?;
    let mut session = system.open_session(target.clone(), CaptureOptions::default())?;
    session.capture()
}

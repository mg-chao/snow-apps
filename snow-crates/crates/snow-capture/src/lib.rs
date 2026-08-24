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
pub mod timing;
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
#[cfg(feature = "stage-timing")]
pub use timing::StageTiming;
pub use window::WindowId;

pub fn capture_once(target: &CaptureTarget) -> CaptureResult<Frame> {
    let system = CaptureSystem::builder().build()?;
    let mut session = system.open_session(target.clone(), CaptureOptions::default())?;
    session.capture()
}

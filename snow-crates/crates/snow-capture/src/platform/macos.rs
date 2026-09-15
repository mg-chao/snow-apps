use crate::backend::{CaptureBackend, CaptureBackendKind, MonitorCapturer};
use crate::error::{CaptureError, CaptureResult};
use crate::frame::{CapturePixelFormat, Frame};
use crate::region::{MonitorGeometry, MonitorLayout};
use crate::{
    CaptureOptions, CaptureTarget, CaptureTargetInfo, CaptureWorkload, MonitorId, WindowId,
};
use snow_macos::{
    MacError,
    content::{self, DisplayInfo},
    desktop::{
        DesktopConfig, DesktopEvent, DesktopSession, DesktopTarget, DisplayId as MacDisplayId,
        WindowId as MacWindowId,
    },
};
use snow_media::{
    CursorMode,
    geometry::{DesktopRect, DesktopSpace, DesktopTransform},
};
use std::time::Duration;
const TIMEOUT: Duration = Duration::from_secs(5);
pub(crate) struct MacBackend;
pub(crate) fn map_error(error: MacError) -> CaptureError {
    match error {
        MacError::PermissionDenied | MacError::MicrophonePermissionDenied => {
            CaptureError::PermissionDenied
        }
        MacError::TargetUnavailable => CaptureError::MonitorLost,
        MacError::Timeout | MacError::Inactive => CaptureError::Timeout,
        MacError::Canceled => CaptureError::Canceled,
        MacError::UnsupportedOs | MacError::Unsupported(_) => {
            CaptureError::BackendUnavailable(error.to_string())
        }
        MacError::InvalidConfig(message) => CaptureError::InvalidConfig(message),
        other => CaptureError::platform(anyhow::anyhow!(other)),
    }
}
fn display_id(display: &DisplayInfo) -> MonitorId {
    MonitorId::from_parts(
        0,
        u64::from(display.id),
        display.id as isize,
        format!("display:{}", display.id),
        display.primary,
    )
}
fn displays() -> CaptureResult<Vec<DisplayInfo>> {
    content::displays(TIMEOUT).map_err(map_error)
}
pub(crate) fn layout(selected: Option<&[MonitorId]>) -> CaptureResult<MonitorLayout> {
    let displays = displays()?;
    let monitors: Vec<_> = displays
        .iter()
        .filter(|d| {
            selected.is_none_or(|ids| ids.iter().any(|id| id.raw_handle() == d.id as isize))
        })
        .map(|d| MonitorGeometry {
            monitor: display_id(d),
            x: d.bounds.x.round() as i32,
            y: d.bounds.y.round() as i32,
            width: d.bounds.width.round() as u32,
            height: d.bounds.height.round() as u32,
        })
        .collect();
    let left = monitors
        .iter()
        .map(|m| i64::from(m.x))
        .min()
        .ok_or(CaptureError::NoPrimaryMonitor)?;
    let top = monitors
        .iter()
        .map(|m| i64::from(m.y))
        .min()
        .ok_or(CaptureError::NoPrimaryMonitor)?;
    let right = monitors
        .iter()
        .map(|m| i64::from(m.x) + i64::from(m.width))
        .max()
        .unwrap();
    let bottom = monitors
        .iter()
        .map(|m| i64::from(m.y) + i64::from(m.height))
        .max()
        .unwrap();
    Ok(MonitorLayout {
        monitors,
        virtual_left: left as i32,
        virtual_top: top as i32,
        virtual_width: u32::try_from(right - left).map_err(|_| CaptureError::BufferOverflow)?,
        virtual_height: u32::try_from(bottom - top).map_err(|_| CaptureError::BufferOverflow)?,
    })
}
impl CaptureBackend for MacBackend {
    fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        Ok(displays()?.iter().map(display_id).collect())
    }
    fn primary_monitor(&self) -> CaptureResult<MonitorId> {
        displays()?
            .iter()
            .find(|d| d.primary)
            .map(display_id)
            .ok_or(CaptureError::NoPrimaryMonitor)
    }
    fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
        layout(None)
    }
    fn inspect_window(&self, window: &WindowId) -> CaptureResult<CaptureTargetInfo> {
        target_info(&CaptureTarget::Window(*window))
    }
    fn inspect_target(&self, target: &CaptureTarget) -> CaptureResult<Option<CaptureTargetInfo>> {
        Ok(Some(target_info(target)?))
    }
    fn create_target_capturer(
        &self,
        target: &CaptureTarget,
        options: CaptureOptions,
    ) -> CaptureResult<Option<Box<dyn MonitorCapturer>>> {
        Ok(Some(Box::new(MacCapturer::new(target.clone(), options)?)))
    }
    fn create_monitor_capturer(
        &self,
        monitor: &MonitorId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Ok(Box::new(MacCapturer::new(
            CaptureTarget::Monitor(monitor.clone()),
            CaptureOptions::default(),
        )?))
    }
    fn create_window_capturer(&self, window: &WindowId) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Ok(Box::new(MacCapturer::new(
            CaptureTarget::Window(*window),
            CaptureOptions::default(),
        )?))
    }
}
fn native_config(target: &CaptureTarget) -> CaptureResult<DesktopConfig> {
    let native_id = |id| {
        u32::try_from(id)
            .map_err(|_| CaptureError::InvalidConfig("invalid macOS target identifier".into()))
    };
    Ok(DesktopConfig::new(match target {
        CaptureTarget::PrimaryMonitor => DesktopTarget::PrimaryDisplay,
        CaptureTarget::Monitor(id) => {
            DesktopTarget::Display(MacDisplayId(native_id(id.raw_handle())?))
        }
        CaptureTarget::Window(id) => DesktopTarget::Window(MacWindowId(id.macos_id()?)),
        CaptureTarget::Region(region) => DesktopTarget::Region(DesktopRect {
            space: DesktopSpace::Points,
            x: f64::from(region.x),
            y: f64::from(region.y),
            width: f64::from(region.width),
            height: f64::from(region.height),
        }),
    }))
}
fn target_info(target: &CaptureTarget) -> CaptureResult<CaptureTargetInfo> {
    let transform = snow_macos::desktop::inspect(&native_config(target)?).map_err(map_error)?;
    Ok(CaptureTargetInfo {
        origin_x: transform.source.x.round() as i32,
        origin_y: transform.source.y.round() as i32,
        width: transform.output.width,
        height: transform.output.height,
    })
}
struct MacCapturer {
    session: DesktopSession,
    options: CaptureOptions,
    transform: DesktopTransform,
    cpu: Option<snow_media::CpuFrame>,
}
impl MacCapturer {
    fn new(target: CaptureTarget, options: CaptureOptions) -> CaptureResult<Self> {
        if !matches!(
            options.backend_tuning,
            crate::tuning::BackendTuning::Default
        ) {
            return Err(CaptureError::BackendUnavailable(
                "Windows capture tuning is unavailable on macOS".into(),
            ));
        }
        let session = DesktopSession::new(native_config(&target)?).map_err(map_error)?;
        let transform = session.transform();
        Ok(Self {
            session,
            options,
            transform,
            cpu: None,
        })
    }
}
impl MonitorCapturer for MacCapturer {
    fn set_cursor_visible(&mut self, visible: bool) -> CaptureResult<()> {
        self.session
            .set_cursor(if visible {
                CursorMode::Embedded
            } else {
                CursorMode::Hidden
            })
            .map_err(map_error)
    }
    fn backend_kind(&self) -> CaptureBackendKind {
        CaptureBackendKind::ScreenCaptureKit
    }
    fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        let native = if self.options.workload == CaptureWorkload::Continuous {
            loop {
                match self
                    .session
                    .next_event(Duration::from_millis(20))
                    .map_err(map_error)?
                {
                    DesktopEvent::Frame(frame) => break frame,
                    DesktopEvent::Configuration { transform, .. } => {
                        if self.transform != transform {
                            self.transform = transform;
                            return Err(CaptureError::ResolutionChanged(
                                transform.output.width,
                                transform.output.height,
                            ));
                        }
                    }
                }
            }
        } else {
            self.session.snapshot().map_err(map_error)?
        };
        if !native.duplicate || self.cpu.is_none() {
            self.cpu = Some(
                native
                    .image
                    .to_cpu()
                    .map_err(|e| CaptureError::platform(anyhow::anyhow!(e)))?,
            );
        }
        let cpu = self.cpu.as_ref().unwrap();
        let mut frame = reuse.unwrap_or_else(Frame::empty);
        frame.ensure_capacity(
            cpu.size.width,
            cpu.size.height,
            self.options.output_pixel_format,
        )?;
        frame.as_mut_bytes().copy_from_slice(&cpu.bytes);
        frame.metadata = Default::default();
        if self.options.output_pixel_format == CapturePixelFormat::Rgba8 {
            snow_media::convert::swap_red_blue(frame.as_mut_bytes());
        }
        if let Some(time) = native.source_times.iter().max_by(|a, b| {
            (i128::from(a.value) * i128::from(b.timescale))
                .cmp(&(i128::from(b.value) * i128::from(a.timescale)))
        }) {
            frame.metadata.set_timing_with_format(
                Some(native.acquired_at),
                Some(time.value),
                snow_core::timestamp::TickFormat::Rational {
                    timescale: time.timescale,
                    domain: time.domain,
                    epoch: time.epoch,
                },
            );
        }
        frame.metadata.source_times = native.source_times;
        frame.metadata.configuration_generation = native.generation;
        frame.metadata.capture_transform = Some(native.transform);
        frame.metadata.backend_kind = CaptureBackendKind::ScreenCaptureKit;
        frame.metadata.is_duplicate = native.duplicate;
        Ok(frame)
    }
    fn release_capture_access(&mut self) {
        self.session.release_capture_access();
        self.cpu = None;
    }
    fn capture_access_active(&self) -> bool {
        self.session.active_sources() != 0
    }
}

#[cfg(test)]
mod tuning_tests {
    use super::*;
    #[test]
    fn explicit_windows_tuning_fails_before_native_acquisition() {
        let result = MacCapturer::new(
            CaptureTarget::PrimaryMonitor,
            CaptureOptions {
                backend_tuning: crate::tuning::BackendTuning::Windows(Default::default()),
                ..Default::default()
            },
        );
        assert!(matches!(result, Err(CaptureError::BackendUnavailable(_))));
    }
}

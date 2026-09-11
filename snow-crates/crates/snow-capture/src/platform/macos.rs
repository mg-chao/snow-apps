use std::time::Instant;

use crate::backend::{CaptureBackend, MonitorCapturer};
use crate::capture_session::CaptureTargetInfo;
use crate::error::{CaptureError, CaptureResult};
use crate::frame::{CapturePixelFormat, Frame};
use crate::monitor::MonitorId;
use crate::region::{MonitorGeometry, MonitorLayout};
use crate::window::WindowId;

pub(crate) struct MacOsBackend;

fn platform_error(error: String) -> CaptureError {
    CaptureError::platform(anyhow::anyhow!(error))
}

fn monitor_id(display: &snow_macos::Display) -> MonitorId {
    MonitorId::from_parts(
        0,
        u64::from(display.id),
        display.id as isize,
        &display.name,
        display.primary,
    )
}

pub(crate) fn layout_from_monitors(monitors: Vec<MonitorId>) -> CaptureResult<MonitorLayout> {
    let displays = snow_macos::displays().map_err(platform_error)?;
    let mut geometry = Vec::with_capacity(monitors.len());
    for monitor in monitors {
        let display = displays
            .iter()
            .find(|display| display.id as isize == monitor.raw_handle())
            .ok_or(CaptureError::MonitorLost)?;
        geometry.push(MonitorGeometry {
            monitor,
            x: display.x,
            y: display.y,
            width: display.width,
            height: display.height,
        });
    }
    layout(geometry)
}

fn layout(monitors: Vec<MonitorGeometry>) -> CaptureResult<MonitorLayout> {
    let left = monitors
        .iter()
        .map(|monitor| i64::from(monitor.x))
        .min()
        .ok_or(CaptureError::NoPrimaryMonitor)?;
    let top = monitors
        .iter()
        .map(|monitor| i64::from(monitor.y))
        .min()
        .ok_or(CaptureError::NoPrimaryMonitor)?;
    let right = monitors
        .iter()
        .map(|monitor| i64::from(monitor.x) + i64::from(monitor.width))
        .max()
        .ok_or(CaptureError::NoPrimaryMonitor)?;
    let bottom = monitors
        .iter()
        .map(|monitor| i64::from(monitor.y) + i64::from(monitor.height))
        .max()
        .ok_or(CaptureError::NoPrimaryMonitor)?;
    Ok(MonitorLayout {
        monitors,
        virtual_left: left as i32,
        virtual_top: top as i32,
        virtual_width: u32::try_from(right - left).map_err(|_| CaptureError::BufferOverflow)?,
        virtual_height: u32::try_from(bottom - top).map_err(|_| CaptureError::BufferOverflow)?,
    })
}

impl CaptureBackend for MacOsBackend {
    fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        Ok(snow_macos::displays()
            .map_err(platform_error)?
            .iter()
            .map(monitor_id)
            .collect())
    }

    fn primary_monitor(&self) -> CaptureResult<MonitorId> {
        self.enumerate_monitors()?
            .into_iter()
            .find(MonitorId::is_primary)
            .ok_or(CaptureError::NoPrimaryMonitor)
    }

    fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
        layout_from_monitors(self.enumerate_monitors()?)
    }

    fn inspect_window(&self, window: &WindowId) -> CaptureResult<CaptureTargetInfo> {
        let window = snow_macos::windows()
            .map_err(platform_error)?
            .into_iter()
            .find(|candidate| candidate.id as isize == window.raw_handle())
            .ok_or_else(|| CaptureError::InvalidTarget(window.stable_id()))?;
        Ok(CaptureTargetInfo {
            origin_x: window.x,
            origin_y: window.y,
            width: window.width,
            height: window.height,
        })
    }

    fn create_monitor_capturer(
        &self,
        monitor: &MonitorId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        let display = snow_macos::displays()
            .map_err(platform_error)?
            .into_iter()
            .find(|display| display.id as isize == monitor.raw_handle())
            .ok_or(CaptureError::MonitorLost)?;
        Ok(Box::new(MacOsCapturer {
            display_id: display.id,
            window_id: 0,
            width: display.width,
            height: display.height,
            format: CapturePixelFormat::Rgba8,
        }))
    }

    fn create_window_capturer(&self, window: &WindowId) -> CaptureResult<Box<dyn MonitorCapturer>> {
        let info = self.inspect_window(window)?;
        Ok(Box::new(MacOsCapturer {
            display_id: 0,
            window_id: window.raw_handle() as u32,
            width: info.width,
            height: info.height,
            format: CapturePixelFormat::Rgba8,
        }))
    }
}

struct MacOsCapturer {
    display_id: u32,
    window_id: u32,
    width: u32,
    height: u32,
    format: CapturePixelFormat,
}

impl MonitorCapturer for MacOsCapturer {
    fn set_output_pixel_format(&mut self, format: CapturePixelFormat) -> CaptureResult<()> {
        self.format = format;
        Ok(())
    }

    fn capture(&mut self, _reuse: Option<Frame>) -> CaptureResult<Frame> {
        let now = Instant::now();
        let pixels = snow_macos::capture(
            self.display_id,
            self.window_id,
            self.width,
            self.height,
            self.format == CapturePixelFormat::Bgra8,
        )
        .map_err(platform_error)?;
        let mut frame = match self.format {
            CapturePixelFormat::Bgra8 => Frame::from_bgra8(self.width, self.height, pixels)?,
            CapturePixelFormat::Rgba8 => Frame::from_rgba8(self.width, self.height, pixels)?,
        };
        frame.metadata.set_timing(Some(now), None);
        Ok(frame)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn macos_layout_retains_backing_pixels_and_negative_origins() {
        let monitors = vec![
            MonitorGeometry {
                monitor: MonitorId::from_name(1, "left", false),
                x: -1920,
                y: -100,
                width: 1920,
                height: 1080,
            },
            MonitorGeometry {
                monitor: MonitorId::from_name(2, "retina", true),
                x: 0,
                y: 0,
                width: 3456,
                height: 2234,
            },
        ];
        let layout = layout(monitors).unwrap();
        assert_eq!((layout.virtual_left, layout.virtual_top), (-1920, -100));
        assert_eq!((layout.virtual_width, layout.virtual_height), (5376, 2334));
        assert!(layout.monitors[1].monitor.is_primary());
        assert!(super::layout(Vec::new()).is_err());
    }
}

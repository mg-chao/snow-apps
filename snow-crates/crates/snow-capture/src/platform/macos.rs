use std::time::Instant;

use crate::backend::{
    CaptureBackend, CaptureBlitRegion, CaptureMode, CaptureSampleMetadata, MonitorCapturer,
};
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
    fn display_generation(&self) -> Option<u64> {
        Some(snow_macos::display_generation())
    }

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
            mode: CaptureMode::Snapshot,
            generation: snow_macos::display_generation(),
            stream: None,
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
            mode: CaptureMode::Snapshot,
            generation: snow_macos::display_generation(),
            stream: None,
        }))
    }
}

struct MacOsCapturer {
    display_id: u32,
    window_id: u32,
    width: u32,
    height: u32,
    format: CapturePixelFormat,
    mode: CaptureMode,
    generation: u64,
    stream: Option<ContinuousCapture>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct StreamRegion {
    x: u32,
    y: u32,
    width: u32,
    height: u32,
}

struct ContinuousCapture {
    region: StreamRegion,
    native: snow_macos::VideoStream,
    sequence: u64,
    pixels: Vec<u8>,
}

impl MacOsCapturer {
    fn continuous_frame(&mut self, region: StreamRegion) -> CaptureResult<(&[u8], bool)> {
        if self.generation != snow_macos::display_generation() {
            self.stream = None;
            return Err(CaptureError::MonitorLost);
        }
        if self
            .stream
            .as_ref()
            .is_none_or(|stream| stream.region != region)
        {
            self.stream = None;
            let length = (region.width as usize)
                .checked_mul(region.height as usize)
                .and_then(|length| length.checked_mul(4))
                .ok_or(CaptureError::BufferOverflow)?;
            let mut pixels = Vec::new();
            pixels
                .try_reserve_exact(length)
                .map_err(|_| CaptureError::BufferOverflow)?;
            pixels.resize(length, 0);
            let native = snow_macos::VideoStream::start(
                self.display_id,
                self.window_id,
                region.x,
                region.y,
                region.width,
                region.height,
            )
            .map_err(platform_error)?;
            self.stream = Some(ContinuousCapture {
                region,
                native,
                sequence: 0,
                pixels,
            });
        }
        let stream = self.stream.as_mut().expect("stream was initialized");
        let sequence = stream
            .native
            .read(self.format == CapturePixelFormat::Bgra8, &mut stream.pixels)
            .map_err(platform_error)?;
        let duplicate = stream.sequence == sequence;
        stream.sequence = sequence;
        Ok((&stream.pixels, duplicate))
    }
}

impl MonitorCapturer for MacOsCapturer {
    fn set_output_pixel_format(&mut self, format: CapturePixelFormat) -> CaptureResult<()> {
        self.format = format;
        Ok(())
    }

    fn set_capture_mode(&mut self, mode: CaptureMode) -> CaptureResult<()> {
        if self.mode != mode {
            self.stream = None;
            self.mode = mode;
        }
        Ok(())
    }

    fn release_capture_access(&mut self) {
        self.stream = None;
    }

    fn capture_access_active(&self) -> bool {
        self.stream.is_some()
    }

    fn capture_region_into(
        &mut self,
        blit: CaptureBlitRegion,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        if self.mode != CaptureMode::Continuous {
            return Ok(None);
        }
        if !valid_blit(
            blit,
            self.width,
            self.height,
            destination.width(),
            destination.height(),
        ) {
            return Err(CaptureError::InvalidConfig(
                "Recording region exceeds its source or destination".into(),
            ));
        }
        let stride = destination.width() as usize * 4;
        let (pixels, duplicate) = self.continuous_frame(StreamRegion {
            x: blit.src_x,
            y: blit.src_y,
            width: blit.width,
            height: blit.height,
        })?;
        if !duplicate || !destination_has_history {
            let destination = destination.as_mut_bytes();
            let row_size = blit.width as usize * 4;
            for y in 0..blit.height as usize {
                let start = (blit.dst_y as usize + y) * stride + blit.dst_x as usize * 4;
                destination[start..start + row_size]
                    .copy_from_slice(&pixels[y * row_size..(y + 1) * row_size]);
            }
        }
        Ok(Some(CaptureSampleMetadata {
            capture_time: Some(Instant::now()),
            is_duplicate: duplicate,
            ..CaptureSampleMetadata::default()
        }))
    }

    fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        if self.mode == CaptureMode::Continuous {
            let region = StreamRegion {
                x: 0,
                y: 0,
                width: self.width,
                height: self.height,
            };
            let format = self.format;
            let (pixels, duplicate) = self.continuous_frame(region)?;
            let mut frame = if let Some(mut reuse) = reuse.filter(|frame| {
                frame.dimensions() == (region.width, region.height)
                    && frame.pixel_format() == format
            }) {
                reuse.as_mut_bytes().copy_from_slice(pixels);
                reuse.reset_metadata();
                reuse
            } else {
                match format {
                    CapturePixelFormat::Bgra8 => {
                        Frame::from_bgra8(region.width, region.height, pixels.to_vec())?
                    }
                    CapturePixelFormat::Rgba8 => {
                        Frame::from_rgba8(region.width, region.height, pixels.to_vec())?
                    }
                }
            };
            frame.metadata.set_timing(Some(Instant::now()), None);
            frame.metadata.is_duplicate = duplicate;
            return Ok(frame);
        }
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

fn valid_blit(
    blit: CaptureBlitRegion,
    source_width: u32,
    source_height: u32,
    destination_width: u32,
    destination_height: u32,
) -> bool {
    blit.width > 0
        && blit.height > 0
        && blit
            .src_x
            .checked_add(blit.width)
            .is_some_and(|right| right <= source_width)
        && blit
            .src_y
            .checked_add(blit.height)
            .is_some_and(|bottom| bottom <= source_height)
        && blit
            .dst_x
            .checked_add(blit.width)
            .is_some_and(|right| right <= destination_width)
        && blit
            .dst_y
            .checked_add(blit.height)
            .is_some_and(|bottom| bottom <= destination_height)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn macos_recording_blit_checks_both_bounds_and_overflow() {
        let blit = CaptureBlitRegion {
            src_x: 20,
            src_y: 30,
            width: 100,
            height: 50,
            dst_x: 10,
            dst_y: 5,
        };
        assert!(valid_blit(blit, 120, 80, 110, 55));
        assert!(!valid_blit(blit, 119, 80, 110, 55));
        assert!(!valid_blit(blit, 120, 79, 110, 55));
        assert!(!valid_blit(blit, 120, 80, 109, 55));
        assert!(!valid_blit(blit, 120, 80, 110, 54));
        assert!(!valid_blit(
            CaptureBlitRegion {
                src_x: u32::MAX,
                ..blit
            },
            u32::MAX,
            80,
            110,
            55
        ));
        assert!(!valid_blit(
            CaptureBlitRegion { width: 0, ..blit },
            120,
            80,
            110,
            55
        ));
    }

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

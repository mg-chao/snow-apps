use crate::{MacError, MacResult, permission};
use block2::RcBlock;
use objc2::rc::Retained;
use objc2_core_foundation::CGRect;
use objc2_core_graphics::CGMainDisplayID;
use objc2_foundation::NSError;
use objc2_screen_capture_kit::*;
use snow_media::geometry::{DesktopRect, DesktopSpace, PixelSize};
use std::time::Duration;

#[derive(Clone, Debug)]
pub struct DisplayInfo {
    pub id: u32,
    pub bounds: DesktopRect,
    pub pixels: PixelSize,
    pub primary: bool,
}
#[derive(Clone, Debug)]
pub struct WindowInfo {
    pub id: u32,
    pub process_id: i32,
    pub title: String,
    pub bounds: DesktopRect,
    pub on_screen: bool,
    pub pixels: Option<PixelSize>,
}

pub(crate) fn desktop_rect(rect: CGRect) -> DesktopRect {
    DesktopRect {
        space: DesktopSpace::Points,
        x: rect.origin.x,
        y: rect.origin.y,
        width: rect.size.width,
        height: rect.size.height,
    }
}

// Only immutable SCShareableContent snapshots cross the completion boundary.
struct ContentTransfer(Retained<SCShareableContent>);
// SAFETY: SCShareableContent and its immutable snapshot objects are documented
// for asynchronous enumeration and concurrent reads; no mutation is exposed.
unsafe impl Send for ContentTransfer {}

pub(crate) fn shareable_content(timeout: Duration) -> MacResult<Retained<SCShareableContent>> {
    shareable_content_cancelable(timeout, &crate::CancellationToken::default())
}
pub(crate) fn shareable_content_cancelable(
    timeout: Duration,
    cancellation: &crate::CancellationToken,
) -> MacResult<Retained<SCShareableContent>> {
    if cancellation.is_canceled() {
        return Err(MacError::Canceled);
    }
    permission::check_access()?;
    let (tx, rx) = crossbeam_channel::bounded(1);
    let completion = RcBlock::new(
        move |content: *mut SCShareableContent, error: *mut NSError| {
            let result = unsafe {
                if let Some(error) = error.as_ref() {
                    Err(MacError::from_native(error))
                } else {
                    Retained::retain(content)
                        .map(ContentTransfer)
                        .ok_or(MacError::TargetUnavailable)
                }
            };
            let _ = tx.try_send(result);
        },
    );
    unsafe {
        SCShareableContent::getShareableContentExcludingDesktopWindows_onScreenWindowsOnly_completionHandler(false, false, &completion);
    }
    cancellation.wait_for(&rx, timeout)?.map(|v| v.0)
}

pub fn displays(timeout: Duration) -> MacResult<Vec<DisplayInfo>> {
    displays_cancelable(timeout, &crate::CancellationToken::default())
}
pub(crate) fn displays_cancelable(
    timeout: Duration,
    cancellation: &crate::CancellationToken,
) -> MacResult<Vec<DisplayInfo>> {
    let content = shareable_content_cancelable(timeout, cancellation)?;
    let primary = CGMainDisplayID();
    unsafe {
        content
            .displays()
            .iter()
            .map(|display| {
                let filter = SCContentFilter::initWithDisplay_excludingWindows(
                    objc2::AnyThread::alloc(),
                    &display,
                    &objc2_foundation::NSArray::new(),
                );
                let bounds = desktop_rect(display.frame());
                let pixels = bounds
                    .pixels_at_scale(f64::from(filter.pointPixelScale()))
                    .map_err(|e| MacError::InvalidConfig(e.to_string()))?;
                Ok(DisplayInfo {
                    id: display.displayID(),
                    bounds,
                    pixels,
                    primary: display.displayID() == primary,
                })
            })
            .collect()
    }
}
fn window_info(window: &SCWindow) -> WindowInfo {
    unsafe {
        WindowInfo {
            id: window.windowID(),
            process_id: window.owningApplication().map_or(0, |app| app.processID()),
            title: window
                .title()
                .map_or_else(String::new, |title| title.to_string()),
            bounds: desktop_rect(window.frame()),
            on_screen: window.isOnScreen(),
            pixels: {
                let filter = SCContentFilter::initWithDesktopIndependentWindow(
                    objc2::AnyThread::alloc(),
                    window,
                );
                desktop_rect(filter.contentRect())
                    .pixels_at_scale(f64::from(filter.pointPixelScale()))
                    .ok()
            },
        }
    }
}
pub fn windows(timeout: Duration) -> MacResult<Vec<WindowInfo>> {
    let content = shareable_content(timeout)?;
    Ok(unsafe { content.windows() }
        .iter()
        .map(|window| window_info(&window))
        .collect())
}
/// Inspect only the requested window. The geometry polling hot path must not
/// allocate a content filter for every other window in the desktop session.
pub fn window(id: u32, timeout: Duration) -> MacResult<WindowInfo> {
    window_cancelable(id, timeout, &crate::CancellationToken::default())
}
pub(crate) fn window_cancelable(
    id: u32,
    timeout: Duration,
    cancellation: &crate::CancellationToken,
) -> MacResult<WindowInfo> {
    let content = shareable_content_cancelable(timeout, cancellation)?;
    unsafe { content.windows() }
        .iter()
        .find(|window| unsafe { window.windowID() == id })
        .map(|window| window_info(&window))
        .ok_or(MacError::TargetUnavailable)
}

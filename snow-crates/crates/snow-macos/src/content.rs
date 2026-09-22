use crate::{MacError, MacResult, permission};
use block2::RcBlock;
use objc2::rc::Retained;
use objc2_core_foundation::{
    CFArray, CFBoolean, CFDictionary, CFNumber, CFRetained, CFString, CFType, CGRect,
};
use objc2_core_graphics::{
    CGMainDisplayID, CGRectMakeWithDictionaryRepresentation,
    CGWindowListCreateDescriptionFromArray, kCGWindowBounds, kCGWindowIsOnscreen, kCGWindowNumber,
    kCGWindowOwnerPID,
};
use objc2_foundation::NSError;
use objc2_screen_capture_kit::*;
use snow_media::geometry::{DesktopRect, DesktopSpace, PixelSize};
use std::ptr::NonNull;
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

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

/// Immutable shareable-content snapshot that may move with a capture session.
#[derive(Clone)]
pub(crate) struct SharedSnapshot(Retained<SCShareableContent>);
// SAFETY: SCShareableContent and its immutable snapshot objects are documented
// for asynchronous enumeration and concurrent reads; no mutation is exposed.
unsafe impl Send for SharedSnapshot {}
impl SharedSnapshot {
    pub(crate) fn new(content: Retained<SCShareableContent>) -> Self {
        Self(content)
    }
}
impl std::ops::Deref for SharedSnapshot {
    type Target = SCShareableContent;
    fn deref(&self) -> &SCShareableContent {
        &self.0
    }
}

pub(crate) fn shareable_content(timeout: Duration) -> MacResult<Retained<SCShareableContent>> {
    shareable_content_cancelable(timeout, &crate::CancellationToken::default())
}
struct ContentFlight {
    token: usize,
    slot: Mutex<Option<MacResult<SharedSnapshot>>>,
    done: Condvar,
}
pub(crate) fn shareable_content_cancelable(
    timeout: Duration,
    cancellation: &crate::CancellationToken,
) -> MacResult<Retained<SCShareableContent>> {
    if cancellation.is_canceled() {
        return Err(MacError::Canceled);
    }
    // Parallel monitor workers share one token. One WindowServer snapshot
    // serves every waiter; a different token never joins that request.
    static FLIGHT: Mutex<Option<Arc<ContentFlight>>> = Mutex::new(None);
    let token = cancellation.receiver() as *const _ as usize;
    let existing = {
        let guard = FLIGHT.lock().unwrap_or_else(|error| error.into_inner());
        guard
            .as_ref()
            .filter(|flight| flight.token == token)
            .cloned()
    };
    if let Some(flight) = existing {
        return wait_for_content(&flight, timeout, cancellation);
    }
    let flight = Arc::new(ContentFlight {
        token,
        slot: Mutex::new(None),
        done: Condvar::new(),
    });
    {
        let mut guard = FLIGHT.lock().unwrap_or_else(|error| error.into_inner());
        if let Some(current) = guard.as_ref().filter(|current| current.token == token) {
            let current = current.clone();
            drop(guard);
            return wait_for_content(&current, timeout, cancellation);
        }
        *guard = Some(flight.clone());
    }
    let result = fetch_shareable_content(timeout, cancellation, false, false);
    {
        let mut slot = flight
            .slot
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        *slot = Some(result.clone().map(SharedSnapshot::new));
        flight.done.notify_all();
    }
    {
        let mut guard = FLIGHT.lock().unwrap_or_else(|error| error.into_inner());
        if guard
            .as_ref()
            .is_some_and(|current| Arc::ptr_eq(current, &flight))
        {
            *guard = None;
        }
    }
    result
}

fn wait_for_content(
    flight: &ContentFlight,
    timeout: Duration,
    cancellation: &crate::CancellationToken,
) -> MacResult<Retained<SCShareableContent>> {
    let deadline = Instant::now()
        .checked_add(timeout)
        .ok_or_else(|| MacError::InvalidConfig("deadline overflow".into()))?;
    let mut slot = flight
        .slot
        .lock()
        .unwrap_or_else(|error| error.into_inner());
    loop {
        if cancellation.is_canceled() {
            return Err(MacError::Canceled);
        }
        if let Some(result) = slot.as_ref() {
            return result.clone().map(|content| content.0.clone());
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(MacError::Timeout);
        }
        let (guard, wait) = flight
            .done
            .wait_timeout(slot, remaining.min(Duration::from_millis(50)))
            .unwrap_or_else(|error| error.into_inner());
        slot = guard;
        if wait.timed_out() && deadline <= Instant::now() {
            return Err(MacError::Timeout);
        }
    }
}

pub(crate) fn fetch_shareable_content(
    timeout: Duration,
    cancellation: &crate::CancellationToken,
    excluding_desktop_windows: bool,
    on_screen_windows_only: bool,
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
                        .map(SharedSnapshot)
                        .ok_or(MacError::TargetUnavailable)
                }
            };
            let _ = tx.try_send(result);
        },
    );
    unsafe {
        SCShareableContent::getShareableContentExcludingDesktopWindows_onScreenWindowsOnly_completionHandler(
            excluding_desktop_windows,
            on_screen_windows_only,
            &completion,
        );
    }
    cancellation.wait_for(&rx, timeout)?.map(|value| value.0)
}

pub fn displays(timeout: Duration) -> MacResult<Vec<DisplayInfo>> {
    displays_cancelable(timeout, &crate::CancellationToken::default())
}
pub(crate) fn displays_cancelable(
    timeout: Duration,
    cancellation: &crate::CancellationToken,
) -> MacResult<Vec<DisplayInfo>> {
    let content = shareable_content_cancelable(timeout, cancellation)?;
    displays_from(&content)
}
pub(crate) fn displays_from(content: &SCShareableContent) -> MacResult<Vec<DisplayInfo>> {
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
    window_from(&content, id)
}
pub(crate) fn window_from(content: &SCShareableContent, id: u32) -> MacResult<WindowInfo> {
    unsafe { content.windows() }
        .iter()
        .find(|window| unsafe { window.windowID() == id })
        .map(|window| window_info(&window))
        .ok_or(MacError::TargetUnavailable)
}

/// Geometry from WindowServer for one window, without a shareable-content snapshot.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct WindowProbe {
    pub process_id: i32,
    pub on_screen: bool,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}
pub(crate) fn probe_window(id: u32) -> Option<WindowProbe> {
    let number = CFNumber::new_i64(i64::from(id));
    let array = CFArray::<CFNumber>::from_objects(&[number.as_ref()]);
    let list = unsafe { CGWindowListCreateDescriptionFromArray(Some(array.as_ref()))? };
    if list.count() < 1 {
        return None;
    }
    let value = unsafe { list.value_at_index(0) };
    let dict = NonNull::new(value.cast_mut())?.cast::<CFDictionary>();
    let dict = unsafe { CFRetained::<CFDictionary>::retain(dict) };
    let window_id = dictionary_value::<CFNumber>(&dict, unsafe { kCGWindowNumber })?.as_i64()?;
    if window_id != i64::from(id) {
        return None;
    }
    let process_id = dictionary_value::<CFNumber>(&dict, unsafe { kCGWindowOwnerPID })?.as_i32()?;
    let on_screen = dictionary_value::<CFBoolean>(&dict, unsafe { kCGWindowIsOnscreen })?.value();
    let bounds = dictionary_value::<CFDictionary>(&dict, unsafe { kCGWindowBounds })?;
    let mut rect = CGRect {
        origin: objc2_core_foundation::CGPoint { x: 0.0, y: 0.0 },
        size: objc2_core_foundation::CGSize {
            width: 0.0,
            height: 0.0,
        },
    };
    if !unsafe { CGRectMakeWithDictionaryRepresentation(Some(&bounds), &mut rect) } {
        return None;
    }
    Some(WindowProbe {
        process_id,
        on_screen,
        x: rect.origin.x,
        y: rect.origin.y,
        width: rect.size.width,
        height: rect.size.height,
    })
}
fn dictionary_value<T: objc2_core_foundation::ConcreteType>(
    dict: &CFDictionary,
    key: &CFString,
) -> Option<CFRetained<T>> {
    let raw = unsafe { dict.value(std::ptr::from_ref(key).cast()) };
    let ptr = NonNull::new(raw.cast_mut().cast::<CFType>())?;
    unsafe { CFRetained::retain(ptr) }.downcast().ok()
}

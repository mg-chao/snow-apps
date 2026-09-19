use crate::{
    MacError, MacResult,
    content::{desktop_rect, shareable_content_cancelable},
};
use block2::RcBlock;
use crossbeam_channel::{Receiver, Sender};
use dispatch2::{DispatchQueue, DispatchRetained};
use objc2::{
    AnyThread, DefinedClass, define_class, msg_send,
    rc::Retained,
    runtime::{AnyObject, ProtocolObject},
};
use objc2_core_foundation::{CFRetained, CGPoint, CGRect, CGSize};
use objc2_core_graphics::{kCGColorSpaceExtendedLinearSRGB, kCGColorSpaceSRGB};
use objc2_core_media::{CMClock, CMSampleBuffer, CMTime, CMTimeFlags};
use objc2_core_video::{kCVPixelFormatType_32BGRA, kCVPixelFormatType_64RGBAHalf};
use objc2_foundation::{
    NSArray, NSDictionary, NSError, NSNumber, NSObject, NSObjectProtocol, NSString,
};
use objc2_screen_capture_kit::*;
use snow_media::{
    ColorDescription, CursorMode, DynamicRange, TransferFunction,
    geometry::{DesktopRect, DesktopSpace, PixelSize},
    macos::PixelBuffer,
    time::{ClockDomain, MediaTime},
};
use std::sync::{
    Arc, OnceLock,
    atomic::{AtomicBool, AtomicU64, Ordering},
};
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Target {
    Display(u32),
    Window(u32),
}

#[derive(Clone, Debug)]
pub struct CaptureConfig {
    pub cancellation: crate::CancellationToken,
    pub target: Target,
    /// Local source rectangle in points. None captures the complete target.
    pub source: Option<DesktopRect>,
    pub output: Option<PixelSize>,
    pub dynamic_range: DynamicRange,
    pub cursor: CursorMode,
    pub frames_per_second: u32,
    pub excluded_windows: Vec<u32>,
    pub excluded_processes: Vec<i32>,
    pub timeout: Duration,
}
impl CaptureConfig {
    pub fn new(target: Target) -> Self {
        Self {
            target,
            cancellation: Default::default(),
            source: None,
            output: None,
            dynamic_range: DynamicRange::Sdr,
            cursor: CursorMode::Embedded,
            frames_per_second: 60,
            excluded_windows: Vec::new(),
            excluded_processes: Vec::new(),
            timeout: Duration::from_secs(5),
        }
    }
}
#[derive(Clone)]
pub struct NativeFrame {
    pub image: PixelBuffer,
    pub timestamp: MediaTime,
    pub acquired_at: Instant,
    observation_generation: u64,
}

fn sample_frame(sample: &CMSampleBuffer, color: ColorDescription) -> MacResult<NativeFrame> {
    unsafe {
        if !sample.is_valid() || !sample.data_is_ready() {
            return Err(MacError::Inactive);
        }
        let image = sample.image_buffer().ok_or(MacError::Inactive)?;
        let time = sample.presentation_time_stamp();
        if time.timescale <= 0
            || !time.flags.contains(CMTimeFlags::Valid)
            || time.flags.intersects(
                CMTimeFlags::PositiveInfinity
                    | CMTimeFlags::NegativeInfinity
                    | CMTimeFlags::Indefinite,
            )
        {
            return Err(MacError::InvalidConfig("invalid source timestamp".into()));
        }
        let image = PixelBuffer::from_retained(image, color)
            .map_err(|e| MacError::Unsupported(e.to_string()))?;
        Ok(NativeFrame {
            observation_generation: 0,
            image,
            timestamp: MediaTime {
                value: time.value,
                timescale: time.timescale as u32,
                domain: ClockDomain::MacHostTime,
                epoch: time.epoch,
            },
            acquired_at: {
                let now = Instant::now();
                let host = CMClock::host_time_clock().time();
                let delta = MediaTime {
                    value: host.value,
                    timescale: host.timescale as u32,
                    domain: ClockDomain::MacHostTime,
                    epoch: host.epoch,
                }
                .duration_since(MediaTime {
                    value: time.value,
                    timescale: time.timescale as u32,
                    domain: ClockDomain::MacHostTime,
                    epoch: time.epoch,
                });
                delta.and_then(|d| now.checked_sub(d)).unwrap_or(now)
            },
        })
    }
}

pub(crate) struct Prepared {
    pub(crate) filter: Retained<SCContentFilter>,
    pub(crate) config: Retained<SCStreamConfiguration>,
    color: ColorDescription,
}
fn excluded_window_is_exception(window_excluded: bool, application_excluded: bool) -> bool {
    window_excluded && !application_excluded
}
pub(crate) fn prepare(options: &CaptureConfig) -> MacResult<Prepared> {
    if options.excluded_windows.len() > 4096 || options.excluded_processes.len() > 4096 {
        return Err(MacError::InvalidConfig(
            "capture exclusions exceed 4096 entries".into(),
        ));
    }
    if matches!(options.target, Target::Window(_))
        && (!options.excluded_windows.is_empty() || !options.excluded_processes.is_empty())
    {
        return Err(MacError::InvalidConfig(
            "exclusions apply only to display capture".into(),
        ));
    }
    if options.frames_per_second == 0
        || options.frames_per_second > 240
        || options.timeout.is_zero()
    {
        return Err(MacError::InvalidConfig(
            "frame rate must be 1..=240 and timeout nonzero".into(),
        ));
    }
    if options.dynamic_range == DynamicRange::Hdr && !cfg!(target_arch = "aarch64") {
        return Err(MacError::Unsupported(
            "ScreenCaptureKit HDR requires Apple Silicon".into(),
        ));
    }
    let content = shareable_content_cancelable(options.timeout, &options.cancellation)?;
    unsafe {
        let filter = match options.target {
            Target::Display(id) => {
                let display = content
                    .displays()
                    .iter()
                    .find(|v| v.displayID() == id)
                    .ok_or(MacError::TargetUnavailable)?;
                let applications: Vec<_> = content
                    .applications()
                    .iter()
                    .filter(|app| options.excluded_processes.contains(&app.processID()))
                    .collect();
                // Exceptions toggle application membership. Excluding a window
                // of an already excluded application would otherwise INCLUDE it.
                let excluded: Vec<_> = content
                    .windows()
                    .iter()
                    .filter(|window| {
                        excluded_window_is_exception(
                            options.excluded_windows.contains(&window.windowID()),
                            window.owningApplication().is_some_and(|app| {
                                options.excluded_processes.contains(&app.processID())
                            }),
                        )
                    })
                    .collect();
                SCContentFilter::initWithDisplay_excludingApplications_exceptingWindows(
                    SCContentFilter::alloc(),
                    &display,
                    &NSArray::from_retained_slice(&applications),
                    &NSArray::from_retained_slice(&excluded),
                )
            }
            Target::Window(id) => {
                if !options.excluded_windows.is_empty() || !options.excluded_processes.is_empty() {
                    return Err(MacError::InvalidConfig(
                        "exclusions apply only to display capture".into(),
                    ));
                }
                let window = content
                    .windows()
                    .iter()
                    .find(|v| v.windowID() == id && v.isOnScreen())
                    .ok_or(MacError::TargetUnavailable)?;
                SCContentFilter::initWithDesktopIndependentWindow(SCContentFilter::alloc(), &window)
            }
        };
        let scale = f64::from(filter.pointPixelScale());
        let mut bounds = desktop_rect(filter.contentRect());
        if let Some(source) = options.source {
            source
                .validate()
                .map_err(|e| MacError::InvalidConfig(e.to_string()))?;
            if source.space != DesktopSpace::Points
                || source.x < 0.0
                || source.y < 0.0
                || source.x + source.width > bounds.width
                || source.y + source.height > bounds.height
            {
                return Err(MacError::InvalidConfig(
                    "source must be inside the target, in local desktop points".into(),
                ));
            }
            bounds = source;
        }
        let output = match options.output {
            Some(size) => size,
            None => bounds
                .pixels_at_scale(scale)
                .map_err(|e| MacError::InvalidConfig(e.to_string()))?,
        };
        output
            .byte_len(8)
            .map_err(|e| MacError::InvalidConfig(e.to_string()))?;
        let config = SCStreamConfiguration::new();
        config.setWidth(output.width as usize);
        config.setHeight(output.height as usize);
        config.setQueueDepth(3);
        config.setShowsCursor(options.cursor == CursorMode::Embedded);
        config.setShowMouseClicks(false);
        config.setIgnoreShadowsSingleWindow(true);
        config.setScalesToFit(true);
        config.setMinimumFrameInterval(CMTime {
            value: 1,
            timescale: options.frames_per_second as i32,
            flags: CMTimeFlags::Valid,
            epoch: 0,
        });
        let mut color = ColorDescription::SRGB;
        if options.dynamic_range == DynamicRange::Hdr {
            config.setCaptureDynamicRange(SCCaptureDynamicRange::HDRCanonicalDisplay);
            config.setPixelFormat(kCVPixelFormatType_64RGBAHalf);
            config.setColorSpaceName(kCGColorSpaceExtendedLinearSRGB);
            color.transfer = TransferFunction::Linear;
        } else {
            config.setPixelFormat(kCVPixelFormatType_32BGRA);
            config.setColorSpaceName(kCGColorSpaceSRGB);
        }
        if let Some(source) = options.source {
            config.setSourceRect(CGRect {
                origin: CGPoint {
                    x: source.x,
                    y: source.y,
                },
                size: CGSize {
                    width: source.width,
                    height: source.height,
                },
            });
        }
        Ok(Prepared {
            filter,
            config,
            color,
        })
    }
}

pub fn screenshot(options: &CaptureConfig) -> MacResult<NativeFrame> {
    let deadline = crate::deadline::Deadline::new(options.timeout)?;
    let prepared = prepare(options)?;
    let (tx, rx) = crossbeam_channel::bounded(1);
    let color = prepared.color;
    let completion = RcBlock::new(move |sample: *mut CMSampleBuffer, error: *mut NSError| {
        let result = unsafe {
            if let Some(error) = error.as_ref() {
                Err(MacError::from_native(error))
            } else {
                sample
                    .as_ref()
                    .ok_or(MacError::Inactive)
                    .and_then(|s| sample_frame(s, color))
            }
        };
        let _ = tx.try_send(result);
    });
    unsafe {
        SCScreenshotManager::captureSampleBufferWithFilter_configuration_completionHandler(
            &prepared.filter,
            &prepared.config,
            Some(&completion),
        );
    }
    options.cancellation.wait_for(&rx, deadline.remaining()?)?
}

struct OutputState {
    frames: Sender<NativeFrame>,
    discard: Receiver<NativeFrame>,
    errors: Sender<MacError>,
    terminal: OnceLock<MacError>,
    closed: AtomicBool,
    cancellation: crate::CancellationToken,
    dropped: AtomicU64,
    color: ColorDescription,
    observation_generation: AtomicU64,
}

impl OutputState {
    // The first terminal failure is sticky and has priority over lossy video
    // observations and coalesced inactive notifications. This is also the wakeup
    // path for a blocked consumer; a full error queue already provides a wakeup.
    fn fail(&self, error: MacError) {
        let _ = self.terminal.set(error.clone());
        self.closed.store(true, Ordering::Release);
        let _ = self.errors.try_send(error);
    }
    fn check_terminal(&self) -> MacResult<()> {
        self.terminal.get().cloned().map_or(Ok(()), Err)
    }
}

define_class!(
    #[unsafe(super(NSObject))]
    #[name = "SnowCaptureStreamOutput"]
    #[ivars = Arc<OutputState>]
    struct StreamOutput;
    unsafe impl NSObjectProtocol for StreamOutput {}
    unsafe impl SCStreamOutput for StreamOutput {
        #[unsafe(method(stream:didOutputSampleBuffer:ofType:))]
        unsafe fn receive(&self, _stream: &SCStream, sample: &CMSampleBuffer, kind: SCStreamOutputType) {
            let state = self.ivars();
            if kind != SCStreamOutputType::Screen || state.closed.load(Ordering::Acquire) || state.cancellation.is_canceled() { return; }
            match frame_status(sample) {
                Some(status) if status == SCFrameStatus::Complete => {},
                Some(status) if status == SCFrameStatus::Blank || status == SCFrameStatus::Suspended || status == SCFrameStatus::Stopped => {
                    state.observation_generation.fetch_add(1, Ordering::AcqRel);
                    let _ = state.discard.try_recv();
                    let _ = state.errors.try_send(MacError::Inactive);
                    return;
                },
                _ => return,
            }
            match sample_frame(sample, state.color) {
                Ok(mut frame) => {
                    frame.observation_generation = state.observation_generation.load(Ordering::Acquire);
                    if let Err(crossbeam_channel::TrySendError::Full(frame)) = state.frames.try_send(frame) {
                        let _ = state.discard.try_recv();
                        state.dropped.fetch_add(1, Ordering::Relaxed);
                        let _ = state.frames.try_send(frame);
                    }
                }
                Err(error) => { state.fail(error); }
            }
        }
    }
    unsafe impl SCStreamDelegate for StreamOutput {
        #[unsafe(method(stream:didStopWithError:))]
        unsafe fn stopped(&self, _stream: &SCStream, error: &NSError) {
            let state = self.ivars();
            if !state.closed.load(Ordering::Acquire) { state.fail(MacError::from_native(error)); }
        }
    }
);

fn frame_status(sample: &CMSampleBuffer) -> Option<SCFrameStatus> {
    unsafe {
        let attachments = sample.sample_attachments_array(false)?;
        // ScreenCaptureKit specifies an NSArray of NSDictionary objects, with an NSNumber status.
        let array = &*(CFRetained::as_ptr(&attachments).as_ptr()
            as *const NSArray<NSDictionary<NSString, AnyObject>>);
        if array.is_empty() {
            return None;
        }
        array
            .objectAtIndex(0)
            .objectForKey(SCStreamFrameInfoStatus)
            .and_then(|value| value.downcast::<NSNumber>().ok())
            .map(|status| SCFrameStatus(status.integerValue()))
    }
}

pub struct VideoStream {
    stream: Retained<SCStream>,
    output: Retained<StreamOutput>,
    _queue: DispatchRetained<DispatchQueue>,
    frames: Receiver<NativeFrame>,
    errors: Receiver<MacError>,
    worker: Option<std::thread::JoinHandle<()>>,
    worker_done: Receiver<()>,
    stop_requested: bool,
    timeout: Duration,
    pub(crate) config: Retained<SCStreamConfiguration>,
}
// SAFETY: session methods require exclusive ownership; SCStream supports calls
// from background threads, and output callbacks access only synchronized state.
unsafe impl Send for VideoStream {}

impl VideoStream {
    pub fn start(options: &CaptureConfig) -> MacResult<Self> {
        let deadline = crate::deadline::Deadline::new(options.timeout)?;
        let prepared = prepare(options)?;
        let (tx, source_frames) = crossbeam_channel::bounded(1);
        let (delivery, frames) = crossbeam_channel::bounded(1);
        let (done, worker_done) = crossbeam_channel::bounded(1);
        let (error_tx, errors) = crossbeam_channel::bounded(1);
        let state = Arc::new(OutputState {
            frames: tx,
            discard: source_frames.clone(),
            errors: error_tx,
            terminal: OnceLock::new(),
            closed: AtomicBool::new(false),
            cancellation: options.cancellation.clone(),
            dropped: AtomicU64::new(0),
            color: prepared.color,
            observation_generation: AtomicU64::new(0),
        });
        let allocated = StreamOutput::alloc().set_ivars(state.clone());
        let output: Retained<StreamOutput> = unsafe { msg_send![super(allocated), init] };
        let queue = DispatchQueue::new("app.snow.capture.frames", None);
        let stream = unsafe {
            SCStream::initWithFilter_configuration_delegate(
                SCStream::alloc(),
                &prepared.filter,
                &prepared.config,
                Some(ProtocolObject::from_ref(&*output)),
            )
        };
        unsafe {
            stream.addStreamOutput_type_sampleHandlerQueue_error(
                ProtocolObject::from_ref(&*output),
                SCStreamOutputType::Screen,
                Some(&queue),
            )
        }
        .map_err(|e| MacError::from_native(&e))?;
        let discard = frames.clone();
        let worker = std::thread::Builder::new()
            .name("snow-capture-metal".into())
            .spawn(move || {
                copy_frames(source_frames, delivery, discard, state);
                let _ = done.try_send(());
            })
            .map_err(|e| MacError::InvalidConfig(format!("capture worker creation failed: {e}")))?;
        let result = Self {
            stream,
            output,
            _queue: queue,
            frames,
            errors,
            worker: Some(worker),
            worker_done,
            stop_requested: false,
            timeout: options.timeout,
            config: prepared.config,
        };
        let (tx, rx) = crossbeam_channel::bounded(1);
        let pending_stream = result.stream.clone();
        let pending_state = result.output.ivars().clone();
        let completion = RcBlock::new(move |error: *mut NSError| {
            let status = unsafe {
                error
                    .as_ref()
                    .map_or(Ok(()), |e| Err(MacError::from_native(e)))
            };
            // A start completion may arrive after the waiter timed out and dropped
            // the session. Stop again here: the earlier stop may have preceded start.
            let canceled = pending_state.closed.load(Ordering::Acquire);
            if tx.try_send(status).is_err() || canceled {
                pending_state.closed.store(true, Ordering::Release);
                unsafe {
                    pending_stream.stopCaptureWithCompletionHandler(None);
                }
            }
        });
        unsafe {
            result
                .stream
                .startCaptureWithCompletionHandler(Some(&completion));
        }
        options
            .cancellation
            .wait_for(&rx, deadline.remaining()?)??;
        Ok(result)
    }
    pub fn next_frame(&self, timeout: Duration) -> MacResult<NativeFrame> {
        if self.output.ivars().cancellation.is_canceled() {
            return Err(MacError::Canceled);
        }
        self.output.ivars().check_terminal()?;
        if let Ok(error) = self.errors.try_recv() {
            self.output.ivars().check_terminal()?;
            return Err(error);
        }
        if self.output.ivars().closed.load(Ordering::Acquire) {
            return Err(MacError::Canceled);
        }
        crossbeam_channel::select_biased! {
            recv(self.output.ivars().cancellation.receiver()) -> _ => Err(MacError::Canceled),
            recv(self.errors) -> error => {
                self.output.ivars().check_terminal()?;
                Err(error.unwrap_or(MacError::Canceled))
            },
            recv(self.frames) -> frame => frame.map_err(|_| MacError::Canceled).and_then(|frame| {
                self.output.ivars().check_terminal()?;
                if self.output.ivars().cancellation.is_canceled() { return Err(MacError::Canceled); }
                if frame.observation_generation == self.output.ivars().observation_generation.load(Ordering::Acquire) { Ok(frame) } else { Err(MacError::Inactive) }
            }),
            default(timeout) => Err(MacError::Timeout),
        }
    }
    pub fn set_cursor_visible(&mut self, visible: bool) -> MacResult<()> {
        let (tx, rx) = crossbeam_channel::bounded(1);
        let completion = RcBlock::new(move |error: *mut NSError| {
            let _ = tx.try_send(unsafe {
                error
                    .as_ref()
                    .map_or(Ok(()), |e| Err(MacError::from_native(e)))
            });
        });
        unsafe {
            self.config.setShowsCursor(visible);
            self.stream
                .updateConfiguration_completionHandler(&self.config, Some(&completion));
        }
        rx.recv_timeout(self.timeout)
            .map_err(|_| MacError::Timeout)?
    }
    pub fn dropped_frames(&self) -> u64 {
        self.output.ivars().dropped.load(Ordering::Relaxed)
    }
    pub fn stop(&mut self) -> MacResult<()> {
        if std::mem::replace(&mut self.stop_requested, true) {
            return Ok(());
        }
        self.output.ivars().closed.store(true, Ordering::Release);
        let (tx, rx) = crossbeam_channel::bounded(1);
        let completion = RcBlock::new(move |error: *mut NSError| {
            let result = unsafe {
                error
                    .as_ref()
                    .map_or(Ok(()), |e| Err(MacError::from_native(e)))
            };
            let _ = tx.try_send(result);
        });
        unsafe {
            self.stream
                .stopCaptureWithCompletionHandler(Some(&completion));
        }
        while self.frames.try_recv().is_ok() {}
        rx.recv_timeout(self.timeout)
            .map_err(|_| MacError::Timeout)?
    }
}
impl Drop for VideoStream {
    fn drop(&mut self) {
        let _ = self.stop();
        if self.worker_done.recv_timeout(self.timeout).is_ok()
            && let Some(worker) = self.worker.take()
        {
            let _ = worker.join();
        }
        // A stalled native render can outlive the deadline; its owned state
        // remains valid, and closed prevents it from publishing late output.
    }
}

// Copy off the native callback queue into a bounded owned GPU pool. Retaining
// public leases therefore cannot exhaust ScreenCaptureKit's three-frame pool.
fn copy_frames(
    source: Receiver<NativeFrame>,
    delivery: Sender<NativeFrame>,
    discard: Receiver<NativeFrame>,
    state: Arc<OutputState>,
) {
    let mut compositor: Option<(
        PixelSize,
        snow_media::PixelFormat,
        crate::compositor::Compositor,
    )> = None;
    while !state.closed.load(Ordering::Acquire) && !state.cancellation.is_canceled() {
        let Ok(mut frame) = source.recv_timeout(Duration::from_millis(20)) else {
            continue;
        };
        if let Ok(latest) = source.try_recv() {
            frame = latest;
            state.dropped.fetch_add(1, Ordering::Relaxed);
        }
        let size = frame.image.size();
        let format = frame.image.format();
        if compositor
            .as_ref()
            .is_none_or(|(s, f, _)| *s != size || *f != format)
        {
            match crate::compositor::Compositor::new(size, format, 4) {
                Ok(value) => compositor = Some((size, format, value)),
                Err(error) => {
                    state.fail(error);
                    break;
                }
            }
        }
        // Release the queued output before allocation so latest delivery never
        // occupies a pool slot that could have been reused for this observation.
        if discard.try_recv().is_ok() {
            state.dropped.fetch_add(1, Ordering::Relaxed);
        }
        let rect = snow_media::geometry::PixelRect {
            x: 0,
            y: 0,
            width: size.width,
            height: size.height,
        };
        match compositor.as_mut().unwrap().2.compose(
            &[crate::compositor::Layer {
                image: &frame.image,
                source: rect,
                destination: rect,
            }],
            false,
        ) {
            Ok(image) => {
                frame.image = image;
                if !state.closed.load(Ordering::Acquire)
                    && !state.cancellation.is_canceled()
                    && frame.observation_generation
                        == state.observation_generation.load(Ordering::Acquire)
                {
                    let _ = delivery.try_send(frame);
                }
            }
            Err(MacError::Timeout) => {
                state.dropped.fetch_add(1, Ordering::Relaxed);
            }
            Err(error) => {
                state.fail(error);
                break;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn combined_application_and_window_filters_never_reinclude_excluded_windows() {
        assert!(!excluded_window_is_exception(false, false));
        assert!(excluded_window_is_exception(true, false));
        assert!(!excluded_window_is_exception(false, true));
        assert!(!excluded_window_is_exception(true, true));
    }
    #[test]
    fn terminal_error_survives_saturated_control_queue_and_remains_sticky() {
        let (frames, discard) = crossbeam_channel::bounded(1);
        let (errors, receiver) = crossbeam_channel::bounded(1);
        let state = OutputState {
            frames,
            discard,
            errors,
            terminal: OnceLock::new(),
            closed: AtomicBool::new(false),
            cancellation: Default::default(),
            dropped: AtomicU64::new(0),
            color: ColorDescription::SRGB,
            observation_generation: AtomicU64::new(0),
        };
        state.errors.try_send(MacError::Inactive).unwrap();
        state.fail(MacError::PermissionDenied);
        assert!(matches!(receiver.try_recv(), Ok(MacError::Inactive)));
        assert!(matches!(
            state.check_terminal(),
            Err(MacError::PermissionDenied)
        ));
        state.fail(MacError::Canceled);
        assert!(matches!(
            state.check_terminal(),
            Err(MacError::PermissionDenied)
        ));
        assert!(state.closed.load(Ordering::Acquire));
    }
}

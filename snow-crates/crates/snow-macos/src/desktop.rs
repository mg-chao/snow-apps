//! Display/region orchestration over ScreenCaptureKit sources. Geometry is in
//! desktop points; native surfaces and per-source timestamps remain explicit.
use crate::{
    MacError, MacResult,
    capture::{CaptureConfig, NativeFrame, Target, VideoStream},
    compositor::{Compositor, Layer},
    content::{self, DisplayInfo, SharedSnapshot, WindowProbe},
};
use objc2_screen_capture_kit::SCShareableContent;
use snow_media::{
    CursorMode, DynamicRange, PixelFormat,
    geometry::{DesktopRect, DesktopSpace, DesktopTransform, PixelRect, PixelSize},
    macos::PixelBuffer,
    time::MediaTime,
};
use std::{
    sync::{
        OnceLock,
        atomic::{AtomicU64, Ordering},
    },
    time::{Duration, Instant},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct DisplayId(pub u32);
/// A WindowServer session identifier; never persist it as a durable identity.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct WindowId(pub u32);
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum DesktopTarget {
    PrimaryDisplay,
    Display(DisplayId),
    Window(WindowId),
    Region(DesktopRect),
}
#[derive(Clone, Debug)]
pub struct DesktopConfig {
    pub cancellation: crate::CancellationToken,
    pub target: DesktopTarget,
    pub output: Option<PixelSize>,
    pub dynamic_range: DynamicRange,
    pub cursor: CursorMode,
    pub fps: u32,
    pub timeout: Duration,
    pub excluded_windows: Vec<u32>,
    pub excluded_processes: Vec<i32>,
    pub opaque: bool,
}
impl DesktopConfig {
    pub fn new(target: DesktopTarget) -> Self {
        Self {
            target,
            cancellation: Default::default(),
            output: None,
            dynamic_range: DynamicRange::Sdr,
            cursor: CursorMode::Embedded,
            fps: 60,
            timeout: Duration::from_secs(5),
            excluded_windows: Vec::new(),
            excluded_processes: Vec::new(),
            opaque: false,
        }
    }
    fn validate(&self, support: crate::capabilities::CaptureSupport) -> MacResult<()> {
        if self.cancellation.is_canceled() {
            return Err(MacError::Canceled);
        }
        if self.excluded_windows.len() > 4096 || self.excluded_processes.len() > 4096 {
            return Err(geometry("capture exclusions exceed 4096 entries"));
        }
        support.native_format(self.dynamic_range)?;
        if !support.metal_composition {
            return Err(MacError::Unsupported(
                "Metal composition is unavailable".into(),
            ));
        }
        if !(1..=240).contains(&self.fps) || self.timeout.is_zero() {
            return Err(geometry("frame rate must be 1..=240 and timeout nonzero"));
        }
        if matches!(self.target, DesktopTarget::Window(_))
            && (!self.excluded_windows.is_empty() || !self.excluded_processes.is_empty())
        {
            return Err(MacError::Unsupported(
                "window capture does not support exclusion filters".into(),
            ));
        }
        Ok(())
    }
    /// Resolve the requested geometry without acquiring pixels or creating a
    /// native stream. The result is a snapshot, not a reservation of the target.
    pub fn capabilities(&self) -> MacResult<TargetCapabilities> {
        if self.cancellation.is_canceled() {
            return Err(MacError::Canceled);
        }
        let support = crate::capabilities::CaptureSupport::current();
        self.validate(support)?;
        let plan = Plan::resolve(self)?;
        Ok(TargetCapabilities {
            support,
            transform: plan.transform,
            source_count: plan.sources.len(),
            native_format: support.native_format(self.dynamic_range)?,
            cpu_formats: support.cpu_formats(self.dynamic_range)?,
            exclusion_filters: !matches!(self.target, DesktopTarget::Window(_)),
        })
    }
}
#[derive(Clone, Debug)]
pub struct TargetCapabilities {
    pub support: crate::capabilities::CaptureSupport,
    pub transform: DesktopTransform,
    /// Sources are sampled independently, even when composed into one frame.
    pub source_count: usize,
    pub native_format: PixelFormat,
    pub cpu_formats: &'static [PixelFormat],
    pub exclusion_filters: bool,
}
#[derive(Clone)]
pub struct DesktopFrame {
    pub image: PixelBuffer,
    pub transform: DesktopTransform,
    pub generation: u64,
    pub source_times: Vec<MediaTime>,
    pub acquired_at: Instant,
    pub duplicate: bool,
}
pub enum DesktopEvent {
    Configuration {
        transform: DesktopTransform,
        generation: u64,
    },
    Frame(DesktopFrame),
}
#[derive(Clone, Debug, PartialEq)]
struct SourcePlan {
    target: Target,
    process_id: Option<i32>,
    source: Option<DesktopRect>,
    destination: PixelRect,
}
#[derive(Clone, Debug, PartialEq)]
struct Plan {
    transform: DesktopTransform,
    sources: Vec<SourcePlan>,
}
fn geometry(error: impl std::fmt::Display) -> MacError {
    MacError::InvalidConfig(error.to_string())
}
fn region_plan(
    bounds: DesktopRect,
    displays: &[DisplayInfo],
    output: Option<PixelSize>,
) -> MacResult<Plan> {
    if bounds.space != DesktopSpace::Points {
        return Err(geometry("macOS regions require desktop points"));
    }
    bounds.validate().map_err(geometry)?;
    let mut intersections = Vec::new();
    let mut scale = 1.0_f64;
    for display in displays {
        if let Some(rect) = bounds.intersection(display.bounds).map_err(geometry)? {
            scale = scale.max(f64::from(display.pixels.width) / display.bounds.width);
            intersections.push((display, rect));
        }
    }
    if intersections.is_empty() {
        return Err(MacError::TargetUnavailable);
    }
    let size = output.map_or_else(|| bounds.pixels_at_scale(scale).map_err(geometry), Ok)?;
    let transform = DesktopTransform::new(bounds, size).map_err(geometry)?;
    let mut sources = Vec::new();
    for (display, rect) in intersections {
        if let Some(destination) = transform.project(rect).map_err(geometry)? {
            sources.push(SourcePlan {
                target: Target::Display(display.id),
                process_id: None,
                source: Some(DesktopRect {
                    x: rect.x - display.bounds.x,
                    y: rect.y - display.bounds.y,
                    ..rect
                }),
                destination,
            });
        }
    }
    Ok(Plan { transform, sources })
}
impl Plan {
    fn resolve(config: &DesktopConfig) -> MacResult<Self> {
        let deadline = crate::deadline::Deadline::new(config.timeout)?;
        let content =
            content::shareable_content_cancelable(deadline.remaining()?, &config.cancellation)?;
        Self::resolve_with(config, &content)
    }
    fn resolve_with(config: &DesktopConfig, content: &SCShareableContent) -> MacResult<Self> {
        let displays = if matches!(config.target, DesktopTarget::Window(_)) {
            Vec::new()
        } else {
            content::displays_from(content)?
        };
        if let DesktopTarget::Region(bounds) = config.target {
            return region_plan(bounds, &displays, config.output);
        }
        let (target, process_id, bounds, size) = match config.target {
            DesktopTarget::Window(WindowId(id)) => {
                let window = content::window_from(content, id)?;
                if !window.on_screen {
                    return Err(MacError::TargetUnavailable);
                }
                let size = window.pixels.ok_or(MacError::TargetUnavailable)?;
                (
                    Target::Window(id),
                    Some(window.process_id),
                    window.bounds,
                    size,
                )
            }
            _ => {
                let display = displays
                    .iter()
                    .find(|d| match config.target {
                        DesktopTarget::Display(DisplayId(id)) => d.id == id,
                        _ => d.primary,
                    })
                    .ok_or(MacError::TargetUnavailable)?;
                (
                    Target::Display(display.id),
                    None,
                    display.bounds,
                    display.pixels,
                )
            }
        };
        let output = config.output.unwrap_or(size);
        Ok(Self {
            transform: DesktopTransform::new(bounds, output).map_err(geometry)?,
            sources: vec![SourcePlan {
                target,
                process_id,
                source: None,
                destination: PixelRect {
                    x: 0,
                    y: 0,
                    width: output.width,
                    height: output.height,
                },
            }],
        })
    }
    fn capture_config(&self, config: &DesktopConfig, index: usize) -> CaptureConfig {
        let source = &self.sources[index];
        CaptureConfig {
            cancellation: config.cancellation.clone(),
            target: source.target,
            source: source.source,
            output: Some(PixelSize {
                width: source.destination.width,
                height: source.destination.height,
            }),
            dynamic_range: config.dynamic_range,
            cursor: config.cursor,
            frames_per_second: config.fps,
            excluded_windows: config.excluded_windows.clone(),
            excluded_processes: config.excluded_processes.clone(),
            timeout: config.timeout,
        }
    }
}
static TOPOLOGY: AtomicU64 = AtomicU64::new(1);
fn topology() -> MacResult<u64> {
    static REGISTERED: OnceLock<bool> = OnceLock::new();
    unsafe extern "C-unwind" fn changed(
        _: u32,
        _: objc2_core_graphics::CGDisplayChangeSummaryFlags,
        _: *mut std::ffi::c_void,
    ) {
        TOPOLOGY.fetch_add(1, Ordering::AcqRel);
    }
    if !REGISTERED.get_or_init(|| unsafe {
        objc2_core_graphics::CGDisplayRegisterReconfigurationCallback(
            Some(changed),
            std::ptr::null_mut(),
        ) == objc2_core_graphics::CGError::Success
    }) {
        return Err(MacError::Unsupported(
            "display reconfiguration notifications unavailable".into(),
        ));
    }
    Ok(TOPOLOGY.load(Ordering::Acquire))
}

pub struct DesktopSession {
    config: DesktopConfig,
    plan: Plan,
    streams: Vec<VideoStream>,
    latest: Vec<Option<NativeFrame>>,
    compositor: Compositor,
    previous: Option<DesktopFrame>,
    topology: u64,
    generation: u64,
    configuration_pending: bool,
    failure: Option<MacError>,
    inspected_at: Instant,
    window_probe: Option<WindowProbe>,
    pending_content: Option<SharedSnapshot>,
}
impl DesktopSession {
    pub fn new(mut config: DesktopConfig) -> MacResult<Self> {
        if config.cancellation.is_canceled() {
            return Err(MacError::Canceled);
        }
        config.validate(crate::capabilities::CaptureSupport::current())?;
        config.excluded_windows.sort_unstable();
        config.excluded_windows.dedup();
        config.excluded_processes.sort_unstable();
        config.excluded_processes.dedup();
        let topology = topology()?;
        let plan = Plan::resolve(&config)?;
        // Primary is resolved once. Removing it must never silently retarget.
        if config.target == DesktopTarget::PrimaryDisplay
            && let Target::Display(id) = plan.sources[0].target
        {
            config.target = DesktopTarget::Display(DisplayId(id));
        }
        let compositor = Compositor::new(plan.transform.output, output_format(&config), 4)?;
        let window_probe = match config.target {
            DesktopTarget::Window(WindowId(id)) => content::probe_window(id),
            _ => None,
        };
        Ok(Self {
            latest: vec![None; plan.sources.len()],
            config,
            plan,
            streams: Vec::new(),
            compositor,
            previous: None,
            topology,
            generation: 1,
            configuration_pending: true,
            failure: None,
            inspected_at: Instant::now(),
            window_probe,
            pending_content: None,
        })
    }
    /// Replace the cancellation token between one-shot operations. Streams must be stopped.
    pub fn set_snapshot_cancellation(&mut self, token: crate::CancellationToken) {
        debug_assert!(self.streams.is_empty());
        self.config.cancellation = token;
        // A new user request may follow a permission grant. Do not retry within
        // the failed request, but allow the next snapshot to check access again.
        if matches!(
            self.failure,
            Some(MacError::Canceled | MacError::PermissionDenied)
        ) {
            self.failure = None;
        }
    }
    pub fn transform(&self) -> DesktopTransform {
        self.plan.transform
    }
    pub fn set_cursor(&mut self, mode: CursorMode) -> MacResult<()> {
        if mode != self.config.cursor {
            for stream in &mut self.streams {
                stream.set_cursor_visible(mode == CursorMode::Embedded)?;
            }
            self.config.cursor = mode;
            self.previous = None;
            self.latest.fill(None);
        }
        Ok(())
    }
    fn refresh(&mut self) -> MacResult<()> {
        let current = topology()?;
        let window_check = matches!(self.config.target, DesktopTarget::Window(_))
            && self.inspected_at.elapsed() >= Duration::from_millis(250);
        if current == self.topology && !window_check {
            return Ok(());
        }
        if window_check
            && current == self.topology
            && let DesktopTarget::Window(WindowId(id)) = self.config.target
            && content::probe_window(id)
                .is_some_and(|probe| self.window_probe.as_ref() == Some(&probe))
        {
            self.inspected_at = Instant::now();
            return Ok(());
        }
        let deadline = crate::deadline::Deadline::new(self.config.timeout)?;
        let content = content::shareable_content_cancelable(
            deadline.remaining()?,
            &self.config.cancellation,
        )?;
        let next = Plan::resolve_with(&self.config, &content)?;
        self.pending_content = Some(SharedSnapshot::new(content));
        if matches!(self.config.target, DesktopTarget::Window(_))
            && self.plan.sources.first().map(|s| s.process_id)
                != next.sources.first().map(|s| s.process_id)
        {
            return Err(MacError::TargetUnavailable);
        }
        self.inspected_at = Instant::now();
        self.window_probe = match self.config.target {
            DesktopTarget::Window(WindowId(id)) => content::probe_window(id),
            _ => None,
        };
        if current != self.topology || next != self.plan {
            self.streams.clear();
            self.latest = vec![None; next.sources.len()];
            self.compositor =
                Compositor::new(next.transform.output, output_format(&self.config), 4)?;
            self.plan = next;
            self.previous = None;
            self.generation = self
                .generation
                .checked_add(1)
                .ok_or_else(|| geometry("configuration generation overflow"))?;
            self.configuration_pending = true;
        }
        self.topology = current;
        Ok(())
    }
    fn complete<T>(&mut self, mut result: MacResult<T>) -> MacResult<T> {
        if self.config.cancellation.is_canceled() && result.is_ok() {
            result = Err(MacError::Canceled);
        }
        if let Err(error) = &result
            && latch_failure(&mut self.failure, error)
        {
            self.release_capture_access();
        }
        result
    }
    pub fn next_event(&mut self, timeout: Duration) -> MacResult<DesktopEvent> {
        if self.config.cancellation.is_canceled() {
            return self.complete(Err(MacError::Canceled));
        }
        if let Some(error) = &self.failure {
            return Err(error.clone());
        }
        let result = self.next_event_inner(timeout);
        self.complete(result)
    }
    fn next_event_inner(&mut self, timeout: Duration) -> MacResult<DesktopEvent> {
        self.refresh()?;
        if std::mem::take(&mut self.configuration_pending) {
            return Ok(DesktopEvent::Configuration {
                transform: self.plan.transform,
                generation: self.generation,
            });
        }
        if self.streams.is_empty() {
            let deadline = crate::deadline::Deadline::new(self.config.timeout)?;
            let shared = if let Some(content) = self.pending_content.take() {
                Some(content)
            } else if self.plan.sources.len() > 1 {
                Some(SharedSnapshot::new(content::shareable_content_cancelable(
                    deadline.remaining()?,
                    &self.config.cancellation,
                )?))
            } else {
                None
            };
            let mut streams = Vec::with_capacity(self.plan.sources.len());
            for i in 0..self.plan.sources.len() {
                streams.push(VideoStream::start_with(
                    &self.plan.capture_config(&self.config, i),
                    shared.as_deref(),
                )?);
            }
            self.streams = streams;
        }
        let deadline = Instant::now()
            .checked_add(timeout)
            .ok_or_else(|| geometry("timeout overflow"))?;
        let mut changed = false;
        for (index, stream) in self.streams.iter().enumerate() {
            match stream.next_frame(deadline.saturating_duration_since(Instant::now())) {
                Ok(frame) => {
                    if self.latest[index]
                        .as_ref()
                        .is_none_or(|f| f.timestamp != frame.timestamp)
                    {
                        self.latest[index] = Some(frame);
                        changed = true;
                    }
                }
                Err(MacError::Timeout) if self.latest[index].is_some() => {}
                Err(error) => {
                    self.previous = None;
                    self.latest.fill(None);
                    return Err(error);
                }
            }
        }
        if topology()? != self.topology {
            return Err(MacError::Inactive);
        }
        if !changed && let Some(previous) = &self.previous {
            return Ok(DesktopEvent::Frame(DesktopFrame {
                duplicate: true,
                ..previous.clone()
            }));
        }
        let frame = self.compose()?;
        self.previous = Some(frame.clone());
        Ok(DesktopEvent::Frame(frame))
    }
    pub fn snapshot(&mut self) -> MacResult<DesktopFrame> {
        if self.config.cancellation.is_canceled() {
            return self.complete(Err(MacError::Canceled));
        }
        if let Some(error) = &self.failure {
            return Err(error.clone());
        }
        let result = self.snapshot_inner();
        self.complete(result)
    }
    fn snapshot_inner(&mut self) -> MacResult<DesktopFrame> {
        let deadline = crate::deadline::Deadline::new(self.config.timeout)?;
        self.refresh()?;
        let content = match self.pending_content.take() {
            Some(content) => content,
            None => SharedSnapshot::new(content::shareable_content_cancelable(
                deadline.remaining()?,
                &self.config.cancellation,
            )?),
        };
        for i in 0..self.plan.sources.len() {
            let mut options = self.plan.capture_config(&self.config, i);
            options.timeout = deadline.remaining()?;
            self.latest[i] = Some(crate::capture::screenshot_with(&options, &content)?);
        }
        if topology()? != self.topology {
            return Err(MacError::Inactive);
        }
        let result = self.compose()?;
        if self.config.cancellation.is_canceled() {
            return Err(MacError::Canceled);
        }
        deadline.remaining()?;
        Ok(result)
    }
    fn compose(&mut self) -> MacResult<DesktopFrame> {
        let frames: Vec<_> = self
            .latest
            .iter()
            .map(|f| f.as_ref().ok_or(MacError::Timeout))
            .collect::<MacResult<_>>()?;
        let layers: Vec<_> = frames
            .iter()
            .zip(&self.plan.sources)
            .map(|(frame, source)| Layer {
                image: &frame.image,
                source: PixelRect {
                    x: 0,
                    y: 0,
                    width: frame.image.size().width,
                    height: frame.image.size().height,
                },
                destination: source.destination,
            })
            .collect();
        let full = PixelRect {
            x: 0,
            y: 0,
            width: self.plan.transform.output.width,
            height: self.plan.transform.output.height,
        };
        let display = matches!(
            self.plan.sources.first().map(|source| source.target),
            Some(Target::Display(_))
        );
        let image = if layers.len() == 1
            && should_clone_source(
                layers[0].source,
                layers[0].destination,
                frames[0].image.size(),
                full,
                self.config.opaque,
                display,
            ) {
            frames[0].image.clone()
        } else {
            self.compositor.compose(&layers, self.config.opaque)?
        };
        Ok(DesktopFrame {
            image,
            transform: self.plan.transform,
            generation: self.generation,
            source_times: frames.iter().map(|f| f.timestamp).collect(),
            acquired_at: frames
                .iter()
                .map(|f| f.acquired_at)
                .max()
                .ok_or(MacError::Timeout)?,
            duplicate: false,
        })
    }
    pub fn release_capture_access(&mut self) {
        self.streams.clear();
        self.latest.fill(None);
        self.previous = None;
    }
    pub fn active_sources(&self) -> usize {
        self.streams.len()
    }
}
fn latch_failure(slot: &mut Option<MacError>, error: &MacError) -> bool {
    if !matches!(error, MacError::Timeout | MacError::Inactive) {
        if slot.is_none() {
            *slot = Some(error.clone());
        }
        true
    } else {
        false
    }
}
fn should_clone_source(
    source: PixelRect,
    destination: PixelRect,
    image: PixelSize,
    output: PixelRect,
    opaque: bool,
    display: bool,
) -> bool {
    source.x == 0
        && source.y == 0
        && source.width == image.width
        && source.height == image.height
        && destination == output
        && image.width == output.width
        && image.height == output.height
        && (!opaque || display)
}
fn output_format(config: &DesktopConfig) -> PixelFormat {
    match config.dynamic_range {
        DynamicRange::Sdr => PixelFormat::Bgra8,
        DynamicRange::Hdr => PixelFormat::Rgba16Float,
    }
}
pub fn inspect(config: &DesktopConfig) -> MacResult<DesktopTransform> {
    Ok(Plan::resolve(config)?.transform)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn display(id: u32, x: f64, width: f64, scale: f64) -> DisplayInfo {
        let bounds = DesktopRect {
            space: DesktopSpace::Points,
            x,
            y: 0.0,
            width,
            height: 100.0,
        };
        DisplayInfo {
            id,
            bounds,
            pixels: bounds.pixels_at_scale(scale).unwrap(),
            primary: id == 1,
        }
    }
    #[test]
    fn capability_validation_rejects_requests_before_target_enumeration() {
        let support = crate::capabilities::CaptureSupport {
            screen_capture_kit: true,
            metal_composition: true,
            hdr_capture: false,
        };
        let mut config = DesktopConfig::new(DesktopTarget::Window(WindowId(123)));
        assert!(config.validate(support).is_ok());
        config.excluded_windows.push(456);
        assert!(matches!(
            config.validate(support),
            Err(MacError::Unsupported(_))
        ));
        config.excluded_windows.clear();
        config.dynamic_range = DynamicRange::Hdr;
        assert!(matches!(
            config.validate(support),
            Err(MacError::Unsupported(_))
        ));
        config.dynamic_range = DynamicRange::Sdr;
        config.fps = 0;
        assert!(matches!(
            config.validate(support),
            Err(MacError::InvalidConfig(_))
        ));
        config.cancellation.cancel();
        assert!(matches!(config.capabilities(), Err(MacError::Canceled)));
    }
    #[test]
    fn pre_canceled_session_does_not_enter_native_acquisition() {
        let config = DesktopConfig::new(DesktopTarget::PrimaryDisplay);
        config.cancellation.cancel();
        assert!(matches!(
            DesktopSession::new(config),
            Err(MacError::Canceled)
        ));
    }
    #[test]
    fn target_loss_is_sticky_but_temporary_interruptions_are_not() {
        let mut state = None;
        assert!(!latch_failure(&mut state, &MacError::Inactive));
        assert!(!latch_failure(&mut state, &MacError::Timeout));
        assert!(state.is_none());
        assert!(latch_failure(&mut state, &MacError::TargetUnavailable));
        assert!(latch_failure(&mut state, &MacError::Canceled));
        assert!(matches!(state, Some(MacError::TargetUnavailable)));
    }
    #[test]
    fn mixed_retina_scales_and_negative_origins_share_edges() {
        let displays = [display(1, -100.0, 100.0, 1.0), display(2, 0.0, 100.0, 2.0)];
        let bounds = DesktopRect {
            width: 200.0,
            ..displays[0].bounds
        };
        let plan = region_plan(bounds, &displays, None).unwrap();
        assert_eq!(plan.transform.output, PixelSize::new(400, 200).unwrap());
        assert_eq!(
            plan.sources[0].destination.width,
            plan.sources[1].destination.x
        );
        assert_eq!(plan.sources[1].destination.width, 200);
    }
    #[test]
    fn explicit_dimensions_preserve_gaps_and_invalid_space_is_rejected() {
        let displays = [display(1, 0.0, 100.0, 1.0), display(2, 150.0, 100.0, 2.0)];
        let mut bounds = DesktopRect {
            width: 250.0,
            ..displays[0].bounds
        };
        let plan = region_plan(bounds, &displays, Some(PixelSize::new(500, 200).unwrap())).unwrap();
        assert_eq!(plan.sources[0].destination.width, 200);
        assert_eq!(plan.sources[1].destination.x, 300);
        bounds.space = DesktopSpace::PhysicalPixels;
        assert!(region_plan(bounds, &displays, None).is_err());
    }
    #[test]
    fn opaque_display_frames_reuse_the_source_and_windows_do_not() {
        let full = PixelRect {
            x: 0,
            y: 0,
            width: 20,
            height: 10,
        };
        let image = PixelSize::new(20, 10).unwrap();
        assert!(should_clone_source(full, full, image, full, true, true));
        assert!(should_clone_source(full, full, image, full, false, false));
        assert!(!should_clone_source(full, full, image, full, true, false));
        assert!(!should_clone_source(
            PixelRect {
                width: 10,
                height: 10,
                ..full
            },
            full,
            image,
            full,
            false,
            true
        ));
    }
}

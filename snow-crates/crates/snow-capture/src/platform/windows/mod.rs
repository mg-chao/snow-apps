pub(crate) mod com;
pub(crate) mod cursor;
pub(crate) mod d3d11;
pub(crate) mod dirty_rect;
pub(crate) mod display_change;
pub(crate) mod duplication;
pub(crate) mod gdi;
pub(crate) mod gpu_tonemap;
pub(crate) mod monitor;
pub(crate) mod region_pipeline;
pub(crate) mod surface;
pub(crate) mod wgc;

use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::backend::{
    AutoBackendPolicy, CaptureBackend, CaptureBackendKind, CaptureBlitRegion, CaptureMode,
    CaptureSampleMetadata, MonitorCapturer, WgcUpdateMode,
};
use crate::capture_session::CaptureTargetInfo;
use crate::error::{CaptureError, CaptureResult};
use crate::frame::{CapturePixelFormat, Frame};
use crate::monitor::MonitorId;
use crate::region::MonitorLayout;
use crate::window::WindowId;
use snow_cursor::CursorSnapshot;

use windows::Win32::Foundation::{HWND, RECT};

fn window_rect(hwnd: HWND, kind: CaptureBackendKind) -> CaptureResult<RECT> {
    // WGC captures the visible DWM frame. GetWindowRect can include invisible resize borders and
    // can be virtualized to the caller's DPI awareness, so it does not reliably locate WGC pixels.
    if matches!(
        kind,
        CaptureBackendKind::Auto | CaptureBackendKind::WindowsGraphicsCapture
    ) {
        use std::ffi::c_void;
        use std::mem;
        use windows::Win32::Graphics::Dwm::{DWMWA_EXTENDED_FRAME_BOUNDS, DwmGetWindowAttribute};

        let mut rect = RECT::default();
        let extended_frame = unsafe {
            DwmGetWindowAttribute(
                hwnd,
                DWMWA_EXTENDED_FRAME_BOUNDS,
                &mut rect as *mut RECT as *mut c_void,
                mem::size_of::<RECT>() as u32,
            )
        };
        if extended_frame.is_ok() && rect.right > rect.left && rect.bottom > rect.top {
            return Ok(rect);
        }
    }

    use windows::Win32::UI::WindowsAndMessaging::GetWindowRect;

    let mut rect = RECT::default();
    unsafe { GetWindowRect(hwnd, &mut rect) }.map_err(|error| {
        CaptureError::InvalidTarget(format!("failed to inspect window: {error}"))
    })?;
    Ok(rect)
}

const AUTO_KIND_ERROR: &str = "auto backend selection is handled separately";
const SNAPSHOT_ACQUISITION_BUDGET: Duration = Duration::from_millis(750);
const DEFAULT_WINDOW_AUTO_PRIORITY: [CaptureBackendKind; 3] = [
    CaptureBackendKind::WindowsGraphicsCapture,
    CaptureBackendKind::DxgiDuplication,
    CaptureBackendKind::Gdi,
];

fn window_auto_priority(
    policy: &AutoBackendPolicy,
    policy_is_explicit: bool,
) -> Vec<CaptureBackendKind> {
    if policy_is_explicit {
        policy.normalized_priority()
    } else {
        DEFAULT_WINDOW_AUTO_PRIORITY.to_vec()
    }
}

#[derive(Clone)]
enum AutoTarget {
    Monitor(MonitorId),
    Window(WindowId),
}

struct AutoCandidate {
    kind: CaptureBackendKind,
    capturer: Option<Box<dyn MonitorCapturer>>,
}

struct AutomaticWindowsCapturer {
    resolver: Arc<monitor::MonitorResolver>,
    target: AutoTarget,
    candidates: Vec<AutoCandidate>,
    preferred: Option<CaptureBackendKind>,
    selected: Option<CaptureBackendKind>,
    topology_generation: Option<u64>,
    capture_mode: CaptureMode,
    gpu_hdr_conversion_enabled: bool,
    hdr_tonemap_lut_enabled: bool,
    wgc_update_mode: WgcUpdateMode,
    output_pixel_format: CapturePixelFormat,
    #[cfg(feature = "stage-timing")]
    record_stage_timings: bool,
}

impl AutomaticWindowsCapturer {
    fn new(
        resolver: Arc<monitor::MonitorResolver>,
        target: AutoTarget,
        priority: Vec<CaptureBackendKind>,
    ) -> Self {
        let topology_generation = resolver.display_generation();
        Self {
            resolver,
            target,
            candidates: priority
                .into_iter()
                .map(|kind| AutoCandidate {
                    kind,
                    capturer: None,
                })
                .collect(),
            preferred: None,
            selected: None,
            topology_generation,
            capture_mode: CaptureMode::Snapshot,
            gpu_hdr_conversion_enabled: true,
            hdr_tonemap_lut_enabled: true,
            wgc_update_mode: WgcUpdateMode::Auto,
            output_pixel_format: CapturePixelFormat::Rgba8,
            #[cfg(feature = "stage-timing")]
            record_stage_timings: false,
        }
    }

    fn refresh_topology_state(&mut self) {
        let generation = self.resolver.display_generation();
        if generation != self.topology_generation {
            self.discard_all_candidates();
            self.preferred = None;
            self.selected = None;
            self.topology_generation = generation;
        }
    }

    fn create_candidate(
        &self,
        kind: CaptureBackendKind,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        match &self.target {
            AutoTarget::Monitor(monitor) => create_monitor_by_kind(&self.resolver, kind, monitor),
            AutoTarget::Window(window) => create_window_by_kind(&self.resolver, kind, window),
        }
    }

    fn ensure_candidate(&mut self, index: usize) -> CaptureResult<&mut Box<dyn MonitorCapturer>> {
        if self.candidates[index].capturer.is_none() {
            let kind = self.candidates[index].kind;
            let mut capturer = self.create_candidate(kind)?;
            capturer.set_wgc_update_mode(self.wgc_update_mode)?;
            capturer.set_gpu_hdr_conversion(self.gpu_hdr_conversion_enabled)?;
            capturer.set_hdr_tonemap_lut(self.hdr_tonemap_lut_enabled)?;
            capturer.set_output_pixel_format(self.output_pixel_format)?;
            capturer.set_capture_mode(self.capture_mode)?;
            #[cfg(feature = "stage-timing")]
            capturer.set_record_stage_timings(self.record_stage_timings)?;
            self.candidates[index].capturer = Some(capturer);
        }
        self.candidates[index]
            .capturer
            .as_mut()
            .ok_or(CaptureError::WorkerDead)
    }

    fn attempt_order(&self) -> Vec<usize> {
        let mut order = Vec::with_capacity(self.candidates.len());
        if let Some(preferred) = self.preferred
            && let Some(index) = self
                .candidates
                .iter()
                .position(|candidate| candidate.kind == preferred)
        {
            order.push(index);
        }
        for index in 0..self.candidates.len() {
            if !order.contains(&index) {
                order.push(index);
            }
        }
        order
    }

    fn budget_expired(&self, started_at: Instant) -> bool {
        self.capture_mode == CaptureMode::Snapshot
            && started_at.elapsed() >= SNAPSHOT_ACQUISITION_BUDGET
    }

    fn record_success(&mut self, index: usize) {
        let kind = self.candidates[index].kind;
        self.discard_other_candidates(index);
        self.preferred = Some(kind);
        self.selected = Some(kind);
    }

    fn record_prepared(&mut self, index: usize) {
        let kind = self.candidates[index].kind;
        self.discard_other_candidates(index);
        self.preferred = Some(kind);
        if self.selected != Some(kind) {
            self.selected = None;
        }
    }

    fn release_candidate_access(&mut self, index: usize) {
        if let Some(capturer) = self.candidates[index].capturer.as_mut() {
            capturer.release_capture_access();
        }
    }

    fn discard_candidate(&mut self, index: usize) {
        let kind = self.candidates[index].kind;
        if let Some(mut capturer) = self.candidates[index].capturer.take() {
            capturer.release_capture_access();
        }
        if self.preferred == Some(kind) {
            self.preferred = None;
        }
        if self.selected == Some(kind) {
            self.selected = None;
        }
    }

    fn discard_other_candidates(&mut self, retained_index: usize) {
        for index in 0..self.candidates.len() {
            if index != retained_index {
                self.discard_candidate(index);
            }
        }
    }

    fn discard_all_candidates(&mut self) {
        for index in 0..self.candidates.len() {
            self.discard_candidate(index);
        }
    }

    fn capture_frame(
        &mut self,
        reuse: Option<Frame>,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        self.refresh_topology_state();
        let started_at = Instant::now();
        let mut reusable = reuse;
        let mut errors = Vec::new();

        for index in self.attempt_order() {
            if self.budget_expired(started_at) {
                errors.push((self.candidates[index].kind, CaptureError::Timeout));
                break;
            }

            let kind = self.candidates[index].kind;
            let attempt_frame = reusable.take();
            let rollback = attempt_frame.as_ref().cloned();
            let result = self.ensure_candidate(index).and_then(|capturer| {
                capturer.capture_with_history_hint(attempt_frame, destination_has_history)
            });
            match result {
                Ok(frame) => {
                    self.record_success(index);
                    return Ok(frame);
                }
                Err(error) if fallback_eligible(&error) => {
                    self.discard_candidate(index);
                    reusable = rollback;
                    errors.push((kind, error));
                }
                Err(error) => {
                    self.discard_all_candidates();
                    return Err(error);
                }
            }
        }

        self.discard_all_candidates();
        Err(all_backends_failed(&self.target, &errors))
    }

    fn capture_region(
        &mut self,
        blit: CaptureBlitRegion,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        self.refresh_topology_state();
        let started_at = Instant::now();
        let mut errors = Vec::new();
        for index in self.attempt_order() {
            if self.budget_expired(started_at) {
                errors.push((self.candidates[index].kind, CaptureError::Timeout));
                break;
            }
            let kind = self.candidates[index].kind;
            let rollback = destination.clone();
            let result = self.ensure_candidate(index).and_then(|capturer| {
                capturer.capture_region_into(blit, destination, destination_has_history)
            });
            match result {
                Ok(sample) => {
                    self.record_success(index);
                    return Ok(sample);
                }
                Err(error) if fallback_eligible(&error) => {
                    *destination = rollback;
                    self.discard_candidate(index);
                    errors.push((kind, error));
                }
                Err(error) => {
                    self.discard_all_candidates();
                    return Err(error);
                }
            }
        }
        self.discard_all_candidates();
        Err(all_backends_failed(&self.target, &errors))
    }

    fn capture_desktop_region(
        &mut self,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        self.refresh_topology_state();
        let started_at = Instant::now();
        let mut errors = Vec::new();
        for index in self.attempt_order() {
            if self.budget_expired(started_at) {
                errors.push((self.candidates[index].kind, CaptureError::Timeout));
                break;
            }
            let kind = self.candidates[index].kind;
            let rollback = destination.clone();
            let result = self.ensure_candidate(index).and_then(|capturer| {
                capturer.capture_desktop_region_into(
                    x,
                    y,
                    width,
                    height,
                    destination,
                    destination_has_history,
                )
            });
            match result {
                Ok(sample) => {
                    self.record_success(index);
                    return Ok(sample);
                }
                Err(error) if fallback_eligible(&error) => {
                    *destination = rollback;
                    self.discard_candidate(index);
                    errors.push((kind, error));
                }
                Err(error) => {
                    self.discard_all_candidates();
                    return Err(error);
                }
            }
        }
        self.discard_all_candidates();
        Err(all_backends_failed(&self.target, &errors))
    }

    fn apply_to_prepared(
        &mut self,
        mut apply: impl FnMut(&mut dyn MonitorCapturer) -> CaptureResult<()>,
    ) -> CaptureResult<()> {
        for candidate in &mut self.candidates {
            if let Some(capturer) = candidate.capturer.as_mut() {
                apply(capturer.as_mut())?;
            }
        }
        Ok(())
    }
}

impl MonitorCapturer for AutomaticWindowsCapturer {
    fn backend_kind(&self) -> CaptureBackendKind {
        self.selected.unwrap_or(CaptureBackendKind::Auto)
    }

    fn prewarm_environment(&mut self) -> CaptureResult<()> {
        self.refresh_topology_state();
        let mut errors = Vec::new();
        for index in self.attempt_order() {
            let kind = self.candidates[index].kind;
            let result = self
                .ensure_candidate(index)
                .and_then(|capturer| capturer.prewarm_environment());
            match result {
                Ok(()) => {
                    self.release_candidate_access(index);
                    self.record_prepared(index);
                    return Ok(());
                }
                Err(error) if fallback_eligible(&error) => {
                    self.discard_candidate(index);
                    errors.push((kind, error));
                }
                Err(error) => {
                    self.discard_all_candidates();
                    return Err(error);
                }
            }
        }
        self.discard_all_candidates();
        Err(all_backends_failed(&self.target, &errors))
    }

    fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        self.capture_frame(reuse, false)
    }

    fn capture_with_history_hint(
        &mut self,
        reuse: Option<Frame>,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        self.capture_frame(reuse, destination_has_history)
    }

    fn capture_region_into(
        &mut self,
        blit: CaptureBlitRegion,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        self.capture_region(blit, destination, destination_has_history)
    }

    fn capture_desktop_region_into(
        &mut self,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        self.capture_desktop_region(x, y, width, height, destination, destination_has_history)
    }

    fn set_capture_mode(&mut self, mode: CaptureMode) -> CaptureResult<()> {
        self.capture_mode = mode;
        self.apply_to_prepared(|capturer| capturer.set_capture_mode(mode))
    }

    fn sample_cursor(&mut self) -> CaptureResult<Option<CursorSnapshot>> {
        let Some(selected) = self.selected else {
            return Ok(None);
        };
        let Some(candidate) = self
            .candidates
            .iter_mut()
            .find(|candidate| candidate.kind == selected)
        else {
            return Ok(None);
        };
        match candidate.capturer.as_mut() {
            Some(capturer) => capturer.sample_cursor(),
            None => Ok(None),
        }
    }

    fn set_gpu_hdr_conversion(&mut self, enabled: bool) -> CaptureResult<()> {
        self.gpu_hdr_conversion_enabled = enabled;
        self.apply_to_prepared(|capturer| capturer.set_gpu_hdr_conversion(enabled))
    }

    fn set_hdr_tonemap_lut(&mut self, enabled: bool) -> CaptureResult<()> {
        self.hdr_tonemap_lut_enabled = enabled;
        self.apply_to_prepared(|capturer| capturer.set_hdr_tonemap_lut(enabled))
    }

    fn set_wgc_update_mode(&mut self, mode: WgcUpdateMode) -> CaptureResult<()> {
        self.wgc_update_mode = mode;
        self.apply_to_prepared(|capturer| capturer.set_wgc_update_mode(mode))
    }

    fn set_output_pixel_format(&mut self, format: CapturePixelFormat) -> CaptureResult<()> {
        self.output_pixel_format = format;
        self.apply_to_prepared(|capturer| capturer.set_output_pixel_format(format))
    }

    #[cfg(feature = "stage-timing")]
    fn set_record_stage_timings(&mut self, enabled: bool) -> CaptureResult<()> {
        self.record_stage_timings = enabled;
        self.apply_to_prepared(|capturer| capturer.set_record_stage_timings(enabled))
    }

    fn release_capture_access(&mut self) {
        for candidate in &mut self.candidates {
            if let Some(capturer) = candidate.capturer.as_mut() {
                capturer.release_capture_access();
            }
        }
    }

    fn capture_access_active(&self) -> bool {
        self.candidates.iter().any(|candidate| {
            candidate
                .capturer
                .as_ref()
                .is_some_and(|capturer| capturer.capture_access_active())
        })
    }
}

fn create_monitor_by_kind(
    resolver: &Arc<monitor::MonitorResolver>,
    kind: CaptureBackendKind,
    monitor: &MonitorId,
) -> CaptureResult<Box<dyn MonitorCapturer>> {
    match kind {
        CaptureBackendKind::Auto => Err(auto_kind_error()),
        CaptureBackendKind::DxgiDuplication => Ok(Box::new(
            duplication::WindowsMonitorCapturer::new(monitor, resolver.clone())?,
        )),
        CaptureBackendKind::WindowsGraphicsCapture => Ok(Box::new(
            wgc::WindowsMonitorCapturer::new(monitor, resolver.clone())?,
        )),
        CaptureBackendKind::Gdi => Ok(Box::new(gdi::WindowsMonitorCapturer::new(
            monitor,
            resolver.clone(),
        )?)),
    }
}

fn create_window_by_kind(
    resolver: &Arc<monitor::MonitorResolver>,
    kind: CaptureBackendKind,
    window: &WindowId,
) -> CaptureResult<Box<dyn MonitorCapturer>> {
    match kind {
        CaptureBackendKind::Auto => Err(auto_kind_error()),
        CaptureBackendKind::DxgiDuplication => Ok(Box::new(
            duplication::WindowsDxgiWindowCapturer::new(window, resolver.clone())?,
        )),
        CaptureBackendKind::WindowsGraphicsCapture => Ok(Box::new(
            wgc::WindowsWindowCapturer::new(window, resolver.clone())?,
        )),
        CaptureBackendKind::Gdi => Ok(Box::new(gdi::WindowsWindowCapturer::new(window)?)),
    }
}

fn fallback_eligible(error: &CaptureError) -> bool {
    matches!(
        error,
        CaptureError::BackendUnavailable(_)
            | CaptureError::UnsupportedFormat(_)
            | CaptureError::AccessLost
            | CaptureError::Timeout
            | CaptureError::WorkerDead
            | CaptureError::Platform(_)
    )
}

fn all_backends_failed(
    target: &AutoTarget,
    errors: &[(CaptureBackendKind, CaptureError)],
) -> CaptureError {
    let target_name = match target {
        AutoTarget::Monitor(monitor) => monitor.name().to_owned(),
        AutoTarget::Window(window) => window.stable_id(),
    };
    CaptureError::BackendUnavailable(format!(
        "all screenshot backends failed for {target_name}: {}",
        format_backend_errors(errors)
    ))
}

pub(crate) struct WindowsBackend {
    resolver: Arc<monitor::MonitorResolver>,
    kind: CaptureBackendKind,
    auto_policy: AutoBackendPolicy,
    auto_policy_is_explicit: bool,
}

impl WindowsBackend {
    pub(crate) fn with_kind_and_policy(
        kind: CaptureBackendKind,
        auto_policy: AutoBackendPolicy,
        auto_policy_is_explicit: bool,
    ) -> CaptureResult<Self> {
        let display_cache = display_change::DisplayInfoCache::new()?;
        let resolver = Arc::new(monitor::MonitorResolver::with_display_cache(display_cache));
        Ok(Self {
            resolver,
            kind,
            auto_policy,
            auto_policy_is_explicit,
        })
    }

    #[allow(dead_code)]
    pub(crate) fn with_kind_and_policy_for_screenshot(
        kind: CaptureBackendKind,
        auto_policy: AutoBackendPolicy,
        auto_policy_is_explicit: bool,
    ) -> CaptureResult<Self> {
        let resolver = Arc::new(monitor::MonitorResolver::new(Duration::from_secs(60)));
        Ok(Self {
            resolver,
            kind,
            auto_policy,
            auto_policy_is_explicit,
        })
    }

    fn create_by_kind(
        &self,
        kind: CaptureBackendKind,
        monitor: &MonitorId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        create_monitor_by_kind(&self.resolver, kind, monitor)
    }

    fn create_window_by_kind(
        &self,
        kind: CaptureBackendKind,
        window: &WindowId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        create_window_by_kind(&self.resolver, kind, window)
    }

    fn create_auto_capturer(&self, monitor: &MonitorId) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Ok(Box::new(AutomaticWindowsCapturer::new(
            self.resolver.clone(),
            AutoTarget::Monitor(monitor.clone()),
            self.auto_policy.normalized_priority(),
        )))
    }

    fn create_auto_window_capturer(
        &self,
        window: &WindowId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        let priority = window_auto_priority(&self.auto_policy, self.auto_policy_is_explicit);
        Ok(Box::new(AutomaticWindowsCapturer::new(
            self.resolver.clone(),
            AutoTarget::Window(*window),
            priority,
        )))
    }
}

fn auto_kind_error() -> CaptureError {
    CaptureError::InvalidConfig(AUTO_KIND_ERROR.to_string())
}

fn format_backend_errors(errors: &[(CaptureBackendKind, CaptureError)]) -> String {
    errors
        .iter()
        .map(|(kind, error)| format!("{}: {error}", kind.as_str()))
        .collect::<Vec<_>>()
        .join("; ")
}

impl CaptureBackend for WindowsBackend {
    fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        self.resolver.enumerate_monitors()
    }

    fn primary_monitor(&self) -> CaptureResult<MonitorId> {
        self.resolver.primary_monitor()
    }

    fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
        let monitors = self.resolver.enumerate_monitors()?;
        MonitorLayout::snapshot_from_monitors(monitors)
    }

    fn inspect_window(&self, window: &WindowId) -> CaptureResult<CaptureTargetInfo> {
        self.inspect_window_for_backend(window, self.kind)
    }

    fn inspect_window_for_backend(
        &self,
        window: &WindowId,
        backend_kind: CaptureBackendKind,
    ) -> CaptureResult<CaptureTargetInfo> {
        let hwnd = HWND(window.raw_handle() as *mut std::ffi::c_void);
        let rect = window_rect(hwnd, backend_kind)?;

        let width = (rect.right - rect.left).max(0) as u32;
        let height = (rect.bottom - rect.top).max(0) as u32;
        if width == 0 || height == 0 {
            return Err(CaptureError::InvalidTarget(window.stable_id()));
        }

        Ok(CaptureTargetInfo {
            origin_x: rect.left,
            origin_y: rect.top,
            width,
            height,
        })
    }

    fn display_generation(&self) -> Option<u64> {
        self.resolver.display_generation()
    }

    fn refresh_display_configuration(&self) -> CaptureResult<()> {
        self.resolver.refresh_display_configuration()
    }

    fn create_monitor_capturer(
        &self,
        monitor: &MonitorId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        match self.kind {
            CaptureBackendKind::Auto => self.create_auto_capturer(monitor),
            other => self.create_by_kind(other, monitor),
        }
    }

    fn create_window_capturer(&self, window: &WindowId) -> CaptureResult<Box<dyn MonitorCapturer>> {
        match self.kind {
            CaptureBackendKind::Auto => self.create_auto_window_capturer(window),
            other => self.create_window_by_kind(other, window),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;
    use std::sync::Mutex;

    #[derive(Default)]
    struct CandidateState {
        calls: usize,
        prewarm_calls: usize,
        releases: usize,
        active: bool,
        outcomes: VecDeque<CaptureResult<()>>,
        prewarm_outcomes: VecDeque<CaptureResult<()>>,
    }

    struct ScriptedCapturer {
        kind: CaptureBackendKind,
        state: Arc<Mutex<CandidateState>>,
    }

    impl MonitorCapturer for ScriptedCapturer {
        fn backend_kind(&self) -> CaptureBackendKind {
            self.kind
        }

        fn prewarm_environment(&mut self) -> CaptureResult<()> {
            let mut state = self.state.lock().unwrap();
            state.prewarm_calls += 1;
            state.active = true;
            state.prewarm_outcomes.pop_front().unwrap_or(Ok(()))
        }

        fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
            let outcome = {
                let mut state = self.state.lock().unwrap();
                state.calls += 1;
                state.active = true;
                state.outcomes.pop_front().unwrap_or(Ok(()))
            };
            outcome?;
            let mut frame = reuse.unwrap_or_else(Frame::empty);
            frame.ensure_rgba_capacity(2, 2)?;
            Ok(frame)
        }

        fn release_capture_access(&mut self) {
            let mut state = self.state.lock().unwrap();
            state.active = false;
            state.releases += 1;
        }

        fn capture_access_active(&self) -> bool {
            self.state.lock().unwrap().active
        }
    }

    fn scripted_auto(
        scripts: &[(CaptureBackendKind, Arc<Mutex<CandidateState>>)],
    ) -> AutomaticWindowsCapturer {
        let monitor = MonitorId::from_parts(101, 103, 0, "auto-test-monitor", true);
        let resolver = Arc::new(monitor::MonitorResolver::new(Duration::from_secs(60)));
        let mut capturer = AutomaticWindowsCapturer::new(
            resolver,
            AutoTarget::Monitor(monitor),
            scripts.iter().map(|(kind, _)| *kind).collect(),
        );
        for (candidate, (kind, state)) in capturer.candidates.iter_mut().zip(scripts) {
            candidate.capturer = Some(Box::new(ScriptedCapturer {
                kind: *kind,
                state: Arc::clone(state),
            }));
        }
        capturer
    }

    fn retained_candidate_kinds(capturer: &AutomaticWindowsCapturer) -> Vec<CaptureBackendKind> {
        capturer
            .candidates
            .iter()
            .filter(|candidate| candidate.capturer.is_some())
            .map(|candidate| candidate.kind)
            .collect()
    }

    #[test]
    fn acquisition_failure_falls_back_and_success_is_sticky() -> CaptureResult<()> {
        let dxgi = Arc::new(Mutex::new(CandidateState {
            outcomes: VecDeque::from([Err(CaptureError::Timeout)]),
            ..CandidateState::default()
        }));
        let wgc = Arc::new(Mutex::new(CandidateState {
            outcomes: VecDeque::from([Ok(()), Ok(())]),
            ..CandidateState::default()
        }));
        let gdi = Arc::new(Mutex::new(CandidateState::default()));
        let mut capturer = scripted_auto(&[
            (CaptureBackendKind::DxgiDuplication, Arc::clone(&dxgi)),
            (CaptureBackendKind::WindowsGraphicsCapture, Arc::clone(&wgc)),
            (CaptureBackendKind::Gdi, Arc::clone(&gdi)),
        ]);

        let first = capturer.capture(None)?;
        assert_eq!((first.width(), first.height()), (2, 2));
        assert_eq!(
            capturer.backend_kind(),
            CaptureBackendKind::WindowsGraphicsCapture
        );
        assert_eq!(dxgi.lock().unwrap().calls, 1);
        assert_eq!(dxgi.lock().unwrap().releases, 1);
        assert_eq!(wgc.lock().unwrap().calls, 1);
        assert_eq!(gdi.lock().unwrap().calls, 0);
        assert_eq!(
            retained_candidate_kinds(&capturer),
            vec![CaptureBackendKind::WindowsGraphicsCapture]
        );

        capturer.release_capture_access();
        let _second = capturer.capture(None)?;
        assert_eq!(wgc.lock().unwrap().calls, 2);
        assert_eq!(dxgi.lock().unwrap().calls, 1);
        assert_eq!(gdi.lock().unwrap().calls, 0);
        capturer.release_capture_access();
        assert!(!capturer.capture_access_active());
        Ok(())
    }

    #[test]
    fn prewarm_stops_after_first_viable_backend_and_drops_the_rest() -> CaptureResult<()> {
        let dxgi = Arc::new(Mutex::new(CandidateState {
            prewarm_outcomes: VecDeque::from([Err(CaptureError::Timeout)]),
            ..CandidateState::default()
        }));
        let wgc = Arc::new(Mutex::new(CandidateState {
            prewarm_outcomes: VecDeque::from([Ok(())]),
            ..CandidateState::default()
        }));
        let gdi = Arc::new(Mutex::new(CandidateState::default()));
        let mut capturer = scripted_auto(&[
            (CaptureBackendKind::DxgiDuplication, Arc::clone(&dxgi)),
            (CaptureBackendKind::WindowsGraphicsCapture, Arc::clone(&wgc)),
            (CaptureBackendKind::Gdi, Arc::clone(&gdi)),
        ]);

        capturer.prewarm_environment()?;

        assert_eq!(dxgi.lock().unwrap().prewarm_calls, 1);
        assert_eq!(dxgi.lock().unwrap().releases, 1);
        assert_eq!(wgc.lock().unwrap().prewarm_calls, 1);
        assert_eq!(wgc.lock().unwrap().releases, 1);
        assert_eq!(gdi.lock().unwrap().prewarm_calls, 0);
        assert_eq!(
            retained_candidate_kinds(&capturer),
            vec![CaptureBackendKind::WindowsGraphicsCapture]
        );
        assert_eq!(
            capturer.preferred,
            Some(CaptureBackendKind::WindowsGraphicsCapture)
        );
        assert_eq!(capturer.selected, None);
        assert!(!capturer.capture_access_active());
        Ok(())
    }

    #[test]
    fn invalid_input_is_terminal_and_does_not_fall_back() {
        let dxgi = Arc::new(Mutex::new(CandidateState {
            outcomes: VecDeque::from([Err(CaptureError::InvalidConfig(
                "bad target configuration".to_owned(),
            ))]),
            ..CandidateState::default()
        }));
        let wgc = Arc::new(Mutex::new(CandidateState::default()));
        let mut capturer = scripted_auto(&[
            (CaptureBackendKind::DxgiDuplication, Arc::clone(&dxgi)),
            (CaptureBackendKind::WindowsGraphicsCapture, Arc::clone(&wgc)),
        ]);

        assert!(matches!(
            capturer.capture(None),
            Err(CaptureError::InvalidConfig(_))
        ));
        assert_eq!(dxgi.lock().unwrap().calls, 1);
        assert_eq!(wgc.lock().unwrap().calls, 0);
        assert_eq!(wgc.lock().unwrap().releases, 1);
        assert!(retained_candidate_kinds(&capturer).is_empty());
    }

    #[test]
    fn terminal_prewarm_error_releases_capture_access_before_returning() {
        let dxgi = Arc::new(Mutex::new(CandidateState {
            prewarm_outcomes: VecDeque::from([Err(CaptureError::InvalidConfig(
                "bad prewarm configuration".to_owned(),
            ))]),
            ..CandidateState::default()
        }));
        let mut capturer =
            scripted_auto(&[(CaptureBackendKind::DxgiDuplication, Arc::clone(&dxgi))]);

        assert!(matches!(
            capturer.prewarm_environment(),
            Err(CaptureError::InvalidConfig(_))
        ));
        let state = dxgi.lock().unwrap();
        assert_eq!(state.prewarm_calls, 1);
        assert_eq!(state.releases, 1);
        assert!(!state.active);
        drop(state);
        assert!(retained_candidate_kinds(&capturer).is_empty());
    }

    #[test]
    fn default_backend_orders_match_screenshot_policy() {
        assert_eq!(
            crate::backend::DEFAULT_AUTO_BACKEND_PRIORITY,
            [
                CaptureBackendKind::DxgiDuplication,
                CaptureBackendKind::WindowsGraphicsCapture,
                CaptureBackendKind::Gdi,
            ]
        );
        assert_eq!(
            DEFAULT_WINDOW_AUTO_PRIORITY,
            [
                CaptureBackendKind::WindowsGraphicsCapture,
                CaptureBackendKind::DxgiDuplication,
                CaptureBackendKind::Gdi,
            ]
        );
        assert_eq!(
            window_auto_priority(&AutoBackendPolicy::default(), false),
            DEFAULT_WINDOW_AUTO_PRIORITY
        );
        assert_eq!(
            window_auto_priority(&AutoBackendPolicy::default(), true),
            crate::backend::DEFAULT_AUTO_BACKEND_PRIORITY
        );
    }
}

use std::sync::Arc;
#[cfg(feature = "stage-timing")]
use std::time::{Duration, Instant};

use rustc_hash::FxHashMap;

use crate::CaptureTarget;
use crate::backend::{
    self, CaptureBackend, CaptureBlitRegion, CaptureMode, CaptureSampleMetadata, MonitorCapturer,
    WgcUpdateMode,
};
use crate::error::{CaptureError, CaptureResult};
use crate::frame::{CapturePixelFormat, DirtyRect, Frame};
use crate::monitor::{MonitorId, MonitorKey};
use crate::region::{CaptureRegion, MonitorLayout};
use crate::system::CaptureOptions;
use crate::window::{WindowId, WindowKey};
use snow_core::timestamp::TickFormat;
#[cfg(feature = "stage-timing")]
use snow_cursor::CursorShapeState;
use snow_cursor::{CursorProjector, CursorSampler, CursorSnapshot, CursorTargetInfo};

#[derive(Clone, Debug)]
struct RegionPlanEntry {
    monitor: MonitorId,
    monitor_key: MonitorKey,
    blit: CaptureBlitRegion,
}

#[derive(Clone, Debug)]
struct PreparedRegionPlan {
    region: CaptureRegion,
    output_width: u32,
    output_height: u32,
    region_fully_covered: bool,
    entries: Arc<[RegionPlanEntry]>,
}

fn copy_region_rgba(src: &Frame, blit: CaptureBlitRegion, dst: &mut Frame) -> CaptureResult<()> {
    if blit.width == 0 || blit.height == 0 {
        return Ok(());
    }

    let src_w = src.width() as usize;
    let src_h = src.height() as usize;
    let dst_w = dst.width() as usize;
    let dst_h = dst.height() as usize;

    let src_x = blit.src_x as usize;
    let src_y = blit.src_y as usize;
    let dst_x = blit.dst_x as usize;
    let dst_y = blit.dst_y as usize;
    let copy_w = blit.width as usize;
    let copy_h = blit.height as usize;

    let src_right = src_x
        .checked_add(copy_w)
        .ok_or(CaptureError::BufferOverflow)?;
    let src_bottom = src_y
        .checked_add(copy_h)
        .ok_or(CaptureError::BufferOverflow)?;
    let dst_right = dst_x
        .checked_add(copy_w)
        .ok_or(CaptureError::BufferOverflow)?;
    let dst_bottom = dst_y
        .checked_add(copy_h)
        .ok_or(CaptureError::BufferOverflow)?;

    if src_right > src_w || src_bottom > src_h || dst_right > dst_w || dst_bottom > dst_h {
        return Err(CaptureError::BufferOverflow);
    }

    let src_stride = src_w.checked_mul(4).ok_or(CaptureError::BufferOverflow)?;
    let dst_stride = dst_w.checked_mul(4).ok_or(CaptureError::BufferOverflow)?;
    let row_bytes = copy_w.checked_mul(4).ok_or(CaptureError::BufferOverflow)?;

    let src_bytes = src.as_rgba_bytes();
    let dst_bytes = dst.as_mut_rgba_bytes();
    let src_required_len = src_stride
        .checked_mul(src_h)
        .ok_or(CaptureError::BufferOverflow)?;
    if src_bytes.len() < src_required_len {
        return Err(CaptureError::BufferOverflow);
    }
    let dst_required_len = dst_stride
        .checked_mul(dst_h)
        .ok_or(CaptureError::BufferOverflow)?;
    if dst_bytes.len() < dst_required_len {
        return Err(CaptureError::BufferOverflow);
    }
    let src_start = src_y
        .checked_mul(src_stride)
        .and_then(|off| src_x.checked_mul(4).and_then(|xoff| off.checked_add(xoff)))
        .ok_or(CaptureError::BufferOverflow)?;
    let dst_start = dst_y
        .checked_mul(dst_stride)
        .and_then(|off| dst_x.checked_mul(4).and_then(|xoff| off.checked_add(xoff)))
        .ok_or(CaptureError::BufferOverflow)?;

    let mut src_row_start = src_start;
    let mut dst_row_start = dst_start;
    for _ in 0..copy_h {
        let src_row_end = src_row_start
            .checked_add(row_bytes)
            .ok_or(CaptureError::BufferOverflow)?;
        let dst_row_end = dst_row_start
            .checked_add(row_bytes)
            .ok_or(CaptureError::BufferOverflow)?;

        dst_bytes[dst_row_start..dst_row_end]
            .copy_from_slice(&src_bytes[src_row_start..src_row_end]);

        src_row_start = src_row_start
            .checked_add(src_stride)
            .ok_or(CaptureError::BufferOverflow)?;
        dst_row_start = dst_row_start
            .checked_add(dst_stride)
            .ok_or(CaptureError::BufferOverflow)?;
    }
    Ok(())
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct CaptureSessionConfig {
    pub(crate) capture_retry_count: usize,
    /// Capture intent used to tune backend behavior for screenshot
    /// vs. continuous recording workloads.
    pub(crate) mode: CaptureMode,
    /// When `true`, backends may use GPU-assisted HDR/F16 conversion paths.
    /// When `false`, backends should prefer CPU conversion paths.
    pub(crate) gpu_hdr_conversion: bool,
    /// When `true`, HDR tone mapping may use LUT-approximated BT.2390.
    /// When `false`, backends should use the precise curve implementation.
    pub(crate) hdr_tonemap_lut: bool,
    /// Packed output layout requested by the caller.
    pub(crate) output_pixel_format: CapturePixelFormat,
    /// WGC complete-surface versus ordered-delta policy.
    pub(crate) wgc_update_mode: WgcUpdateMode,
    /// Record per-stage capture timings onto frame metadata.
    #[cfg(feature = "stage-timing")]
    pub(crate) record_stage_timings: bool,
}

impl Default for CaptureSessionConfig {
    fn default() -> Self {
        Self {
            capture_retry_count: 1,
            mode: CaptureMode::Snapshot,
            gpu_hdr_conversion: true,
            hdr_tonemap_lut: true,
            output_pixel_format: CapturePixelFormat::Rgba8,
            wgc_update_mode: WgcUpdateMode::Auto,
            #[cfg(feature = "stage-timing")]
            record_stage_timings: false,
        }
    }
}

impl From<CaptureOptions> for CaptureSessionConfig {
    fn from(value: CaptureOptions) -> Self {
        Self {
            capture_retry_count: value.capture_retry_count,
            mode: value.workload,
            gpu_hdr_conversion: value.gpu_hdr_conversion,
            hdr_tonemap_lut: value.hdr_tonemap_lut,
            output_pixel_format: value.output_pixel_format,
            wgc_update_mode: value.wgc_update_mode,
            #[cfg(feature = "stage-timing")]
            record_stage_timings: value.record_stage_timings,
        }
    }
}

pub(crate) struct CaptureSessionBuilder {
    target: Option<CaptureTarget>,
    backend_override: Option<Arc<dyn CaptureBackend>>,
    config: CaptureSessionConfig,
}

impl CaptureSessionBuilder {
    pub(crate) fn new() -> Self {
        Self {
            target: None,
            backend_override: None,
            config: CaptureSessionConfig::default(),
        }
    }

    pub(crate) fn with_backend(mut self, backend: Arc<dyn CaptureBackend>) -> Self {
        self.backend_override = Some(backend);
        self
    }

    pub(crate) fn target(mut self, target: CaptureTarget) -> Self {
        self.target = Some(target);
        self
    }

    /// Configure the capture intent for backend tuning.
    #[cfg(test)]
    pub(crate) fn capture_mode(mut self, mode: CaptureMode) -> Self {
        self.config.mode = mode;
        self
    }

    pub(crate) fn with_options(mut self, options: CaptureOptions) -> Self {
        self.config = options.into();
        self
    }

    fn resolve_backend_and_config(
        self,
    ) -> CaptureResult<(CaptureTarget, Arc<dyn CaptureBackend>, CaptureSessionConfig)> {
        let CaptureSessionBuilder {
            target,
            backend_override,
            config,
        } = self;
        let target = target.ok_or_else(|| {
            CaptureError::InvalidConfig(
                "capture target must be configured before building session".into(),
            )
        })?;
        let backend = match backend_override {
            Some(b) => b,
            None => crate::platform::build_backend_for_mode(
                backend::CaptureBackendKind::Auto,
                backend::AutoBackendPolicy::default(),
                false,
                config.mode,
            )?,
        };
        Ok((target, backend, config))
    }

    fn build_uncached(self) -> CaptureResult<CaptureSession> {
        let (target, backend, config) = self.resolve_backend_and_config()?;
        if config.mode == CaptureMode::Continuous {
            crate::convert::warmup();
        }
        let runtime = CaptureSessionRuntime::for_target(&target);
        Ok(CaptureSession {
            target,
            backend,
            config,
            sequence: 0,
            runtime,
            cursor_fallback_sampler: None,
            cursor_projector: CursorProjector::new(),
        })
    }

    pub(crate) fn build(self) -> CaptureResult<CaptureSession> {
        self.build_uncached()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CaptureTargetInfo {
    pub origin_x: i32,
    pub origin_y: i32,
    pub width: u32,
    pub height: u32,
}

/// Cursor-attach diagnostics for one frame. Only populated in builds
/// with the `stage-timing` feature; all fields disappear otherwise.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct CursorAttachStats {
    #[cfg(feature = "stage-timing")]
    pub used_native: bool,
    #[cfg(feature = "stage-timing")]
    pub used_fallback: bool,
    #[cfg(feature = "stage-timing")]
    pub shape_cache_hit: bool,
    #[cfg(feature = "stage-timing")]
    pub shape_cache_miss: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct CursorAttachOutcome {
    #[cfg(feature = "stage-timing")]
    pub stats: CursorAttachStats,
    #[cfg(feature = "stage-timing")]
    pub elapsed: Duration,
}

fn cursor_target_info(target_info: &CaptureTargetInfo) -> CursorTargetInfo {
    CursorTargetInfo {
        origin_x: target_info.origin_x,
        origin_y: target_info.origin_y,
        width: target_info.width,
        height: target_info.height,
    }
}

/// Stamps a successfully captured frame with its sequence number and, in
/// stage-timing builds, the wall-clock duration of the capture call.
#[cfg(feature = "stage-timing")]
fn stamp_captured_frame(frame: &mut Frame, seq: u64, capture_duration: Duration) {
    frame.metadata.sequence = seq;
    if frame.metadata.capture_duration.is_none() {
        frame.metadata.capture_duration = Some(capture_duration);
    }
}

#[cfg(not(feature = "stage-timing"))]
fn stamp_captured_frame(frame: &mut Frame, seq: u64) {
    frame.metadata.sequence = seq;
}

#[cfg(feature = "stage-timing")]
fn observe_projected_cursor(
    stats: &mut CursorAttachStats,
    sample: &snow_cursor::AttachedCursorSample,
) {
    match sample.shape {
        CursorShapeState::Embedded(_) => stats.shape_cache_miss = true,
        CursorShapeState::Cached(_) => stats.shape_cache_hit = true,
        CursorShapeState::Unavailable => {}
    }
}

pub(crate) fn inspect_target_from_backend(
    backend: &Arc<dyn CaptureBackend>,
    target: &CaptureTarget,
) -> CaptureResult<CaptureTargetInfo> {
    match target {
        CaptureTarget::Region(region) => Ok(CaptureTargetInfo {
            origin_x: region.x,
            origin_y: region.y,
            width: region.width,
            height: region.height,
        }),
        CaptureTarget::PrimaryMonitor => {
            let layout = backend.monitor_layout()?;
            let mon = layout
                .monitors
                .iter()
                .find(|m| m.monitor.is_primary())
                .ok_or(CaptureError::NoPrimaryMonitor)?;
            Ok(CaptureTargetInfo {
                origin_x: mon.x,
                origin_y: mon.y,
                width: mon.width,
                height: mon.height,
            })
        }
        CaptureTarget::Monitor(id) => {
            let layout = backend.monitor_layout()?;
            let mon = layout
                .monitors
                .iter()
                .find(|m| m.monitor.key() == id.key())
                .ok_or_else(|| CaptureError::InvalidTarget(id.stable_id()))?;
            Ok(CaptureTargetInfo {
                origin_x: mon.x,
                origin_y: mon.y,
                width: mon.width,
                height: mon.height,
            })
        }
        CaptureTarget::Window(window) => backend.inspect_window(window),
    }
}

impl Default for CaptureSessionBuilder {
    fn default() -> Self {
        Self::new()
    }
}

pub struct CaptureSession {
    target: CaptureTarget,
    backend: Arc<dyn CaptureBackend>,
    config: CaptureSessionConfig,
    sequence: u64,
    runtime: CaptureSessionRuntime,
    cursor_fallback_sampler: Option<CursorSampler>,
    cursor_projector: CursorProjector,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum HistoryReuseSource {
    Caller,
    Cached,
    None,
}

struct HistoryReuseSelection {
    frame: Option<Frame>,
    source: HistoryReuseSource,
}

impl HistoryReuseSelection {
    #[inline]
    fn should_cache_output(&self) -> bool {
        !matches!(self.source, HistoryReuseSource::Caller)
    }

    fn select(
        reuse: Option<Frame>,
        last_history_seq: Option<u64>,
        cached_history: Option<Frame>,
    ) -> Self {
        let reuse_matches_history = reuse
            .as_ref()
            .zip(last_history_seq)
            .is_some_and(|(frame, last_seq)| frame.metadata.sequence == last_seq);
        if reuse_matches_history {
            return Self {
                frame: reuse,
                source: HistoryReuseSource::Caller,
            };
        }

        let cached_matches_history = cached_history
            .as_ref()
            .zip(last_history_seq)
            .is_some_and(|(frame, last_seq)| frame.metadata.sequence == last_seq);
        if cached_matches_history {
            return Self {
                frame: cached_history,
                source: HistoryReuseSource::Cached,
            };
        }

        if reuse.is_some() {
            return Self {
                frame: reuse,
                source: HistoryReuseSource::Caller,
            };
        }

        Self {
            frame: None,
            source: HistoryReuseSource::None,
        }
    }
}

struct CapturerStore<K> {
    capturers: FxHashMap<K, Box<dyn MonitorCapturer>>,
}

impl<K> Default for CapturerStore<K> {
    fn default() -> Self {
        Self {
            capturers: FxHashMap::default(),
        }
    }
}

impl<K> CapturerStore<K>
where
    K: Eq + std::hash::Hash + Copy,
{
    fn get_or_create<F>(
        &mut self,
        key: K,
        config: CaptureSessionConfig,
        create_capturer: F,
    ) -> CaptureResult<&mut Box<dyn MonitorCapturer>>
    where
        F: FnOnce() -> CaptureResult<Box<dyn MonitorCapturer>>,
    {
        match self.capturers.entry(key) {
            std::collections::hash_map::Entry::Occupied(entry) => Ok(entry.into_mut()),
            std::collections::hash_map::Entry::Vacant(entry) => {
                let mut capturer = create_capturer()?;
                capturer.set_wgc_update_mode(config.wgc_update_mode)?;
                capturer.set_gpu_hdr_conversion(config.gpu_hdr_conversion)?;
                capturer.set_hdr_tonemap_lut(config.hdr_tonemap_lut)?;
                capturer.set_output_pixel_format(config.output_pixel_format)?;
                capturer.set_capture_mode(config.mode)?;
                #[cfg(feature = "stage-timing")]
                capturer.set_record_stage_timings(config.record_stage_timings)?;
                Ok(entry.insert(capturer))
            }
        }
    }

    fn remove(&mut self, key: K) {
        self.capturers.remove(&key);
    }

    fn clear(&mut self) {
        self.capturers.clear();
    }

    fn release_capture_access(&mut self) {
        for capturer in self.capturers.values_mut() {
            capturer.release_capture_access();
        }
    }

    fn active_capture_access_count(&self) -> usize {
        self.capturers
            .values()
            .filter(|capturer| capturer.capture_access_active())
            .count()
    }
}

struct OutputHistoryCache<K> {
    last_output_sequence: FxHashMap<K, u64>,
    cached_frames: FxHashMap<K, Frame>,
}

impl<K> Default for OutputHistoryCache<K> {
    fn default() -> Self {
        Self {
            last_output_sequence: FxHashMap::default(),
            cached_frames: FxHashMap::default(),
        }
    }
}

impl<K> OutputHistoryCache<K>
where
    K: Eq + std::hash::Hash + Copy,
{
    fn select_reuse(
        &mut self,
        key: K,
        reuse: Option<Frame>,
    ) -> (HistoryReuseSelection, Option<u64>) {
        let last_history_seq = self.last_output_sequence.get(&key).copied();
        let selection =
            HistoryReuseSelection::select(reuse, last_history_seq, self.cached_frames.remove(&key));
        (selection, last_history_seq)
    }

    fn store_output(&mut self, key: K, seq: u64, frame: &Frame, should_cache_output: bool) {
        self.last_output_sequence.insert(key, seq);
        if should_cache_output {
            self.cached_frames.insert(key, frame.clone());
        }
    }

    fn clear_target(&mut self, key: K) {
        self.last_output_sequence.remove(&key);
        self.cached_frames.remove(&key);
    }

    fn clear(&mut self) {
        self.last_output_sequence.clear();
        self.cached_frames.clear();
    }
}

#[derive(Default)]
struct MonitorCaptureRuntime {
    capturers: CapturerStore<MonitorKey>,
    output: OutputHistoryCache<MonitorKey>,
}

impl MonitorCaptureRuntime {
    fn get_or_create_capturer(
        &mut self,
        backend: Arc<dyn CaptureBackend>,
        config: CaptureSessionConfig,
        monitor: &MonitorId,
    ) -> CaptureResult<&mut Box<dyn MonitorCapturer>> {
        self.capturers.get_or_create(monitor.key(), config, || {
            backend.create_monitor_capturer(monitor)
        })
    }

    fn clear_target(&mut self, key: MonitorKey) {
        self.capturers.remove(key);
        self.output.clear_target(key);
    }

    fn release_idle_resources(&mut self) {
        self.capturers.clear();
        self.output.clear();
    }
}

#[derive(Default)]
struct WindowCaptureRuntime {
    capturers: CapturerStore<WindowKey>,
    output: OutputHistoryCache<WindowKey>,
}

impl WindowCaptureRuntime {
    fn get_or_create_capturer(
        &mut self,
        backend: Arc<dyn CaptureBackend>,
        config: CaptureSessionConfig,
        window: &WindowId,
    ) -> CaptureResult<&mut Box<dyn MonitorCapturer>> {
        self.capturers.get_or_create(window.key(), config, || {
            backend.create_window_capturer(window)
        })
    }

    fn clear_target(&mut self, key: WindowKey) {
        self.capturers.remove(key);
        self.output.clear_target(key);
    }

    fn release_idle_resources(&mut self) {
        self.capturers.clear();
        self.output.clear();
    }
}

#[derive(Default)]
struct RegionLayoutRuntime {
    layout: Option<MonitorLayout>,
    generation: Option<u64>,
    prepared_plan: Option<PreparedRegionPlan>,
}

#[derive(Default)]
struct RegionCaptureRuntime {
    capturers: CapturerStore<MonitorKey>,
    layout: RegionLayoutRuntime,
    desktop_direct_support: FxHashMap<MonitorKey, bool>,
    fallback_frames: FxHashMap<MonitorKey, Frame>,
    output_history_seq: Option<u64>,
    output_frame: Option<Frame>,
}

impl RegionCaptureRuntime {
    fn get_or_create_capturer(
        &mut self,
        backend: Arc<dyn CaptureBackend>,
        config: CaptureSessionConfig,
        monitor: &MonitorId,
    ) -> CaptureResult<&mut Box<dyn MonitorCapturer>> {
        self.capturers.get_or_create(monitor.key(), config, || {
            backend.create_monitor_capturer(monitor)
        })
    }

    fn prepare_plan(
        &mut self,
        backend: &Arc<dyn CaptureBackend>,
        region: &CaptureRegion,
    ) -> CaptureResult<(&PreparedRegionPlan, bool)> {
        let current_generation = backend.display_generation();
        if self.layout.generation != current_generation {
            self.layout.layout = None;
            self.layout.generation = current_generation;
            self.layout.prepared_plan = None;
            self.desktop_direct_support.clear();
            self.fallback_frames.clear();
        }

        if self.layout.layout.is_none() {
            self.layout.layout = Some(backend.monitor_layout()?);
        }

        let needs_rebuild = self
            .layout
            .prepared_plan
            .as_ref()
            .is_none_or(|cached| cached.region != *region);
        if needs_rebuild {
            let layout = self.layout.layout.as_ref().unwrap();
            let overlaps = layout.overlapping_monitors(region);
            if overlaps.is_empty() {
                return Err(CaptureError::InvalidTarget(
                    "region does not overlap any monitor".into(),
                ));
            }

            let mut entries = Vec::with_capacity(overlaps.len());
            for (mon_geo, intersection) in overlaps {
                entries.push(RegionPlanEntry {
                    monitor_key: mon_geo.monitor.key(),
                    monitor: mon_geo.monitor,
                    blit: CaptureBlitRegion {
                        src_x: (intersection.x - mon_geo.x) as u32,
                        src_y: (intersection.y - mon_geo.y) as u32,
                        width: intersection.width,
                        height: intersection.height,
                        dst_x: (intersection.x - region.x) as u32,
                        dst_y: (intersection.y - region.y) as u32,
                    },
                });
            }

            let full_region_area = u64::from(region.width) * u64::from(region.height);
            let covered_region_area = entries.iter().fold(0u64, |area, entry| {
                area.saturating_add(u64::from(entry.blit.width) * u64::from(entry.blit.height))
            });

            self.layout.prepared_plan = Some(PreparedRegionPlan {
                region: *region,
                output_width: region.width,
                output_height: region.height,
                region_fully_covered: covered_region_area == full_region_area,
                entries: entries.into(),
            });
        }

        Ok((self.layout.prepared_plan.as_ref().unwrap(), needs_rebuild))
    }

    fn release_idle_resources(&mut self) {
        self.capturers.clear();
        self.layout = RegionLayoutRuntime::default();
        self.desktop_direct_support.clear();
        self.fallback_frames.clear();
        self.output_history_seq = None;
        self.output_frame = None;
    }
}

enum CaptureSessionRuntime {
    Monitor(MonitorCaptureRuntime),
    Window(WindowCaptureRuntime),
    Region(Box<RegionCaptureRuntime>),
}

impl CaptureSessionRuntime {
    fn release_capture_access(&mut self) {
        match self {
            Self::Monitor(runtime) => runtime.capturers.release_capture_access(),
            Self::Window(runtime) => runtime.capturers.release_capture_access(),
            Self::Region(runtime) => runtime.capturers.release_capture_access(),
        }
    }

    fn active_capture_access_count(&self) -> usize {
        match self {
            Self::Monitor(runtime) => runtime.capturers.active_capture_access_count(),
            Self::Window(runtime) => runtime.capturers.active_capture_access_count(),
            Self::Region(runtime) => runtime.capturers.active_capture_access_count(),
        }
    }

    fn for_target(target: &CaptureTarget) -> Self {
        match target {
            CaptureTarget::PrimaryMonitor | CaptureTarget::Monitor(_) => {
                Self::Monitor(MonitorCaptureRuntime::default())
            }
            CaptureTarget::Window(_) => Self::Window(WindowCaptureRuntime::default()),
            CaptureTarget::Region(_) => Self::Region(Box::default()),
        }
    }

    fn release_idle_resources(&mut self) {
        match self {
            Self::Monitor(runtime) => runtime.release_idle_resources(),
            Self::Window(runtime) => runtime.release_idle_resources(),
            Self::Region(runtime) => runtime.release_idle_resources(),
        }
    }
}

impl CaptureSession {
    pub(crate) fn builder() -> CaptureSessionBuilder {
        CaptureSessionBuilder::new()
    }

    pub(crate) fn open_with_backend(
        target: CaptureTarget,
        backend: Arc<dyn CaptureBackend>,
        options: CaptureOptions,
    ) -> CaptureResult<Self> {
        Self::builder()
            .target(target)
            .with_backend(backend)
            .with_options(options)
            .build()
    }

    pub fn target(&self) -> &CaptureTarget {
        &self.target
    }

    pub fn warmup_runtime(&mut self) {
        crate::convert::warmup();
    }

    /// Prepare resources that are safe to retain between one-shot captures.
    /// No active backend access may remain when this method returns.
    pub fn prewarm_environment(&mut self) -> CaptureResult<CaptureTargetInfo> {
        if self.config.mode == CaptureMode::Snapshot {
            // Snapshot warm-up must stay allocation-light. SIMD dispatch and
            // lookup tables are cheap to retain; the conversion worker pool
            // is created only when a real capture needs it.
            crate::convert::warmup_dispatch();
        } else {
            self.warmup_runtime();
        }
        let result = self.prepare_target_resources();
        self.release_capture_access();
        result
    }

    /// Close active backend access and release snapshot-only capture surfaces.
    /// Lightweight prepared state may be retained for the next request.
    pub fn release_capture_access(&mut self) {
        self.runtime.release_capture_access();
    }

    pub fn active_capture_access_count(&self) -> usize {
        self.runtime.active_capture_access_count()
    }

    /// Drop heavyweight capture resources and frame-history caches while
    /// keeping the session reusable for a later capture.
    pub fn release_idle_resources(&mut self) {
        if self.config.mode != CaptureMode::Snapshot {
            return;
        }
        self.release_capture_access();
        self.runtime.release_idle_resources();
        self.cursor_fallback_sampler = None;
        self.cursor_projector = CursorProjector::new();
    }

    /// Release all snapshot capture-time allocations and restore the
    /// lightweight state used between one-shot requests.
    ///
    /// The session remains reusable and no active backend access is retained,
    /// even when prewarming the next request fails.
    pub fn reset_to_prepared(&mut self) -> CaptureResult<()> {
        self.release_idle_resources();
        self.prewarm_environment().map(|_| ())
    }

    pub fn prepare_target(&mut self) -> CaptureResult<CaptureTargetInfo> {
        self.warmup_runtime();
        self.prepare_target_resources()
    }

    fn prepare_target_resources(&mut self) -> CaptureResult<CaptureTargetInfo> {
        let target_info = self.inspect_target(&self.target)?;
        match self.target.clone() {
            CaptureTarget::PrimaryMonitor | CaptureTarget::Monitor(_) => {
                let monitor = self.resolve_monitor_target()?;
                let backend = Arc::clone(&self.backend);
                let config = self.config;
                self.monitor_runtime_mut()
                    .get_or_create_capturer(backend, config, &monitor)?
                    .prewarm_environment()?;
            }
            CaptureTarget::Window(window) => {
                let backend = Arc::clone(&self.backend);
                let config = self.config;
                self.window_runtime_mut()
                    .get_or_create_capturer(backend, config, &window)?
                    .prewarm_environment()?;
            }
            CaptureTarget::Region(region) => {
                let entries = {
                    let backend = Arc::clone(&self.backend);
                    let (plan, _) = self.region_runtime_mut().prepare_plan(&backend, &region)?;
                    Arc::clone(&plan.entries)
                };
                for entry in entries.iter() {
                    let backend = Arc::clone(&self.backend);
                    let config = self.config;
                    self.region_runtime_mut()
                        .get_or_create_capturer(backend, config, &entry.monitor)?
                        .prewarm_environment()?;
                }
            }
        }
        Ok(target_info)
    }

    pub fn target_info(&self) -> CaptureResult<CaptureTargetInfo> {
        self.inspect_target(&self.target)
    }

    /// Inspect this session's target using the geometry convention of the
    /// concrete backend that produced a frame.
    pub fn target_info_for_backend(
        &self,
        backend_kind: crate::backend::CaptureBackendKind,
    ) -> CaptureResult<CaptureTargetInfo> {
        match &self.target {
            CaptureTarget::Window(window) => self
                .backend
                .inspect_window_for_backend(window, backend_kind),
            _ => self.target_info(),
        }
    }

    pub fn capture(&mut self) -> CaptureResult<Frame> {
        self.capture_with_cursor(None).map(|(frame, _)| frame)
    }

    pub fn capture_reuse(&mut self, frame: Frame) -> CaptureResult<Frame> {
        self.capture_with_cursor(Some(frame))
            .map(|(frame, _)| frame)
    }

    pub fn capture_into(&mut self, frame: &mut Frame) -> CaptureResult<()> {
        let reused = std::mem::replace(frame, Frame::empty());
        *frame = self.capture_reuse(reused)?;
        Ok(())
    }

    /// Capture a single snapshot with a newly owned destination frame.
    /// Unlike `capture_once_into`, callers do not need to retain a reusable
    /// destination buffer between requests.
    pub fn capture_once(&mut self) -> CaptureResult<Frame> {
        let mut frame = Frame::empty();
        self.capture_once_into(&mut frame)?;
        Ok(frame)
    }

    /// Capture one frame, release all snapshot capture-time resources, and
    /// restore the lightweight prepared state before returning. Region/
    /// scrolling and recording callers that need an operation-scoped session
    /// continue to use `capture_into`.
    pub fn capture_once_into(&mut self, frame: &mut Frame) -> CaptureResult<()> {
        let previous_dimensions = frame.dimensions();
        let previous_pixel_format = frame.pixel_format();
        let reused = std::mem::replace(frame, Frame::empty());
        let result = self
            .capture_with_cursor(Some(reused))
            .map(|(frame, _)| frame);
        let reset_result = self.reset_to_prepared();
        debug_assert_eq!(self.active_capture_access_count(), 0);
        match (result, reset_result) {
            (Ok(captured), Ok(())) => {
                *frame = captured;
                Ok(())
            }
            (Err(error), Ok(())) => {
                if previous_dimensions.0 != 0 && previous_dimensions.1 != 0 {
                    let _ = frame.ensure_capacity(
                        previous_dimensions.0,
                        previous_dimensions.1,
                        previous_pixel_format,
                    );
                }
                Err(error)
            }
            (_, Err(error)) => {
                if previous_dimensions.0 != 0 && previous_dimensions.1 != 0 {
                    let _ = frame.ensure_capacity(
                        previous_dimensions.0,
                        previous_dimensions.1,
                        previous_pixel_format,
                    );
                }
                Err(error)
            }
        }
    }

    pub(crate) fn capture_with_cursor(
        &mut self,
        reuse: Option<Frame>,
    ) -> CaptureResult<(Frame, Option<CursorAttachOutcome>)> {
        let mut frame = self.capture_frame(reuse)?;
        let cursor_outcome = self.target_info().ok().and_then(|target_info| {
            #[cfg(feature = "stage-timing")]
            let outcome = {
                let start = Instant::now();
                let stats = self.attach_cursor_to_frame(&target_info, &mut frame);
                Some(CursorAttachOutcome {
                    stats,
                    elapsed: start.elapsed(),
                })
            };
            #[cfg(not(feature = "stage-timing"))]
            let outcome = {
                self.attach_cursor_to_frame(&target_info, &mut frame);
                None
            };
            outcome
        });
        Ok((frame, cursor_outcome))
    }

    pub(crate) fn capture_frame(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        self.warmup_runtime();
        self.do_capture(reuse)
    }

    /// Returns the active capture workload.
    pub fn workload(&self) -> backend::CaptureWorkload {
        self.config.mode
    }

    /// Returns whether GPU-assisted HDR conversion is enabled.
    pub fn gpu_hdr_conversion_enabled(&self) -> bool {
        self.config.gpu_hdr_conversion
    }

    /// Returns whether LUT-approximated HDR tone mapping is enabled.
    pub fn hdr_tonemap_lut_enabled(&self) -> bool {
        self.config.hdr_tonemap_lut
    }

    pub(crate) fn inspect_target(
        &self,
        target: &CaptureTarget,
    ) -> CaptureResult<CaptureTargetInfo> {
        inspect_target_from_backend(&self.backend, target)
    }

    pub(crate) fn attach_cursor_to_frame(
        &mut self,
        target_info: &CaptureTargetInfo,
        frame: &mut Frame,
    ) -> CursorAttachStats {
        let effective_target_info = match &self.target {
            CaptureTarget::Window(window) => self
                .backend
                .inspect_window_for_backend(window, frame.metadata.backend_kind)
                .unwrap_or(*target_info),
            _ => *target_info,
        };

        #[cfg(feature = "stage-timing")]
        let mut stats = CursorAttachStats::default();
        #[cfg(not(feature = "stage-timing"))]
        let stats = CursorAttachStats::default();
        let target = cursor_target_info(&effective_target_info);
        let mut snapshot = self.sample_native_cursor().ok().flatten();
        #[cfg(feature = "stage-timing")]
        {
            stats.used_native = snapshot.is_some();
        }

        let native_has_shape = snapshot
            .as_ref()
            .and_then(|snapshot| snapshot.shape.shape())
            .is_some();
        if (snapshot.is_none() || !native_has_shape)
            && let Some(fallback) = self.sample_fallback_cursor()
        {
            // Native cursor metadata is frame-aligned and remains the
            // authority for position/visibility. The global Win32 sampler
            // only supplies the missing image when a backend has none.
            let (resolved, used_fallback) = match snapshot {
                Some(mut native) if fallback.shape.shape().is_some() => {
                    native.shape = fallback.shape;
                    (native, true)
                }
                Some(native) => (native, false),
                None => (fallback, true),
            };
            snapshot = Some(resolved);
            #[cfg(feature = "stage-timing")]
            {
                stats.used_fallback = used_fallback;
            }
            #[cfg(not(feature = "stage-timing"))]
            let _ = used_fallback;
        }

        if let Some(snapshot) = snapshot {
            let sample = self.cursor_projector.project(&target, snapshot);
            #[cfg(feature = "stage-timing")]
            observe_projected_cursor(&mut stats, &sample);
            frame.metadata.cursor = Some(sample);
        }

        stats
    }

    fn sample_native_cursor(&mut self) -> CaptureResult<Option<CursorSnapshot>> {
        match self.target.clone() {
            CaptureTarget::PrimaryMonitor | CaptureTarget::Monitor(_) => {
                self.sample_native_cursor_for_monitor_target()
            }
            CaptureTarget::Window(window) => {
                let backend = Arc::clone(&self.backend);
                let config = self.config;
                let capturer = self
                    .window_runtime_mut()
                    .get_or_create_capturer(backend, config, &window)?;
                capturer.sample_cursor()
            }
            CaptureTarget::Region(region) => self.sample_native_cursor_for_region_target(&region),
        }
    }

    fn sample_native_cursor_for_monitor_target(&mut self) -> CaptureResult<Option<CursorSnapshot>> {
        let monitor = self.resolve_monitor_target()?;
        let backend = Arc::clone(&self.backend);
        let config = self.config;
        let capturer = self
            .monitor_runtime_mut()
            .get_or_create_capturer(backend, config, &monitor)?;
        capturer.sample_cursor()
    }

    fn sample_native_cursor_for_region_target(
        &mut self,
        region: &CaptureRegion,
    ) -> CaptureResult<Option<CursorSnapshot>> {
        let entry = self
            .region_runtime()
            .layout
            .prepared_plan
            .as_ref()
            .filter(|plan| {
                plan.region == *region && plan.region_fully_covered && plan.entries.len() == 1
            })
            .and_then(|plan| plan.entries.first().cloned());
        let Some(entry) = entry else {
            return Ok(None);
        };
        let backend = Arc::clone(&self.backend);
        let config = self.config;
        let capturer =
            self.region_runtime_mut()
                .get_or_create_capturer(backend, config, &entry.monitor)?;
        capturer.sample_cursor()
    }

    fn sample_fallback_cursor(&mut self) -> Option<CursorSnapshot> {
        if self.cursor_fallback_sampler.is_none() {
            self.cursor_fallback_sampler = CursorSampler::new().ok();
        }

        self.cursor_fallback_sampler
            .as_mut()
            .and_then(|sampler| sampler.sample().ok())
    }

    fn resolve_monitor_target(&self) -> CaptureResult<MonitorId> {
        match &self.target {
            CaptureTarget::PrimaryMonitor => self.backend.primary_monitor(),
            CaptureTarget::Monitor(id) => self
                .backend
                .enumerate_monitors()?
                .into_iter()
                .find(|candidate| candidate.key() == id.key())
                .ok_or_else(|| CaptureError::InvalidTarget(id.stable_id())),
            CaptureTarget::Window(_) | CaptureTarget::Region(_) => unreachable!(),
        }
    }

    fn monitor_runtime_mut(&mut self) -> &mut MonitorCaptureRuntime {
        match &mut self.runtime {
            CaptureSessionRuntime::Monitor(runtime) => runtime,
            CaptureSessionRuntime::Window(_) | CaptureSessionRuntime::Region(_) => unreachable!(),
        }
    }

    fn window_runtime_mut(&mut self) -> &mut WindowCaptureRuntime {
        match &mut self.runtime {
            CaptureSessionRuntime::Window(runtime) => runtime,
            CaptureSessionRuntime::Monitor(_) | CaptureSessionRuntime::Region(_) => unreachable!(),
        }
    }

    fn region_runtime(&self) -> &RegionCaptureRuntime {
        match &self.runtime {
            CaptureSessionRuntime::Region(runtime) => runtime,
            CaptureSessionRuntime::Monitor(_) | CaptureSessionRuntime::Window(_) => unreachable!(),
        }
    }

    fn region_runtime_mut(&mut self) -> &mut RegionCaptureRuntime {
        match &mut self.runtime {
            CaptureSessionRuntime::Region(runtime) => runtime,
            CaptureSessionRuntime::Monitor(_) | CaptureSessionRuntime::Window(_) => unreachable!(),
        }
    }

    #[cfg(test)]
    fn set_region_layout_for_test(&mut self, layout: MonitorLayout) {
        self.region_runtime_mut().layout.layout = Some(layout);
    }

    #[cfg(test)]
    fn monitor_output_cache_is_empty(&self) -> bool {
        match &self.runtime {
            CaptureSessionRuntime::Monitor(runtime) => runtime.output.cached_frames.is_empty(),
            CaptureSessionRuntime::Window(_) | CaptureSessionRuntime::Region(_) => unreachable!(),
        }
    }

    #[cfg(test)]
    fn region_output_cache_is_none(&self) -> bool {
        match &self.runtime {
            CaptureSessionRuntime::Region(runtime) => runtime.output_frame.is_none(),
            CaptureSessionRuntime::Monitor(_) | CaptureSessionRuntime::Window(_) => unreachable!(),
        }
    }

    /// Shared capture-with-retry loop used by both monitor and window paths.
    ///
    /// Returns the captured frame (with sequence already stamped) and the
    /// assigned sequence number. The caller is responsible for inserting
    /// the sequence into the appropriate output-history map.
    fn capture_with_retry<G, R>(
        &mut self,
        reuse: Option<Frame>,
        last_history_seq: Option<u64>,
        mut get_capturer: G,
        mut remove_capturer: R,
    ) -> CaptureResult<(Frame, u64)>
    where
        G: FnMut(&mut Self) -> CaptureResult<&mut Box<dyn MonitorCapturer>>,
        R: FnMut(&mut Self),
    {
        let max_retries = self.config.capture_retry_count;
        let mut reuse = reuse;
        let reuse_sequence = reuse.as_ref().map(|f| f.metadata.sequence);
        let destination_has_history = matches!(
            (reuse_sequence, last_history_seq),
            (Some(reuse_seq), Some(last_seq)) if reuse_seq == last_seq
        );
        if !destination_has_history {
            // Backends that do not override `capture_with_history_hint`
            // may infer history from frame metadata directly. Preserve
            // allocation reuse but clear provenance metadata so stale
            // buffers cannot trigger temporal fast paths.
            if let Some(frame) = reuse.as_mut() {
                frame.reset_metadata();
            }
        }

        self.sequence = self.sequence.wrapping_add(1);
        let seq = self.sequence;

        #[cfg(feature = "stage-timing")]
        let cap_start = std::time::Instant::now();
        let first_result = {
            let capturer = get_capturer(self)?;
            let result = capturer.capture_with_history_hint(reuse, destination_has_history);
            let backend_kind = capturer.backend_kind();
            result.map(|mut frame| {
                frame.metadata.backend_kind = backend_kind;
                frame
            })
        };
        #[cfg(feature = "stage-timing")]
        let cap_dur = cap_start.elapsed();
        match first_result {
            Ok(mut frame) => {
                #[cfg(feature = "stage-timing")]
                stamp_captured_frame(&mut frame, seq, cap_dur);
                #[cfg(not(feature = "stage-timing"))]
                stamp_captured_frame(&mut frame, seq);
                return Ok((frame, seq));
            }
            Err(error) if error.requires_worker_reset() && max_retries > 0 => {
                remove_capturer(self);
            }
            Err(error) => return Err(error),
        }

        for attempt in 0..max_retries {
            #[cfg(feature = "stage-timing")]
            let retry_start = std::time::Instant::now();
            let result = {
                let capturer = get_capturer(self)?;
                let result = capturer.capture_with_history_hint(None, false);
                let backend_kind = capturer.backend_kind();
                result.map(|mut frame| {
                    frame.metadata.backend_kind = backend_kind;
                    frame
                })
            };
            #[cfg(feature = "stage-timing")]
            let retry_dur = retry_start.elapsed();
            match result {
                Ok(mut frame) => {
                    #[cfg(feature = "stage-timing")]
                    stamp_captured_frame(&mut frame, seq, retry_dur);
                    #[cfg(not(feature = "stage-timing"))]
                    stamp_captured_frame(&mut frame, seq);
                    return Ok((frame, seq));
                }
                Err(error) if error.requires_worker_reset() && attempt + 1 < max_retries => {
                    remove_capturer(self);
                }
                Err(error) => return Err(error),
            }
        }

        Err(CaptureError::WorkerDead)
    }

    fn do_capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        match self.target.clone() {
            CaptureTarget::PrimaryMonitor | CaptureTarget::Monitor(_) => {
                self.do_capture_monitor(reuse)
            }
            CaptureTarget::Window(window) => self.do_capture_window(window, reuse),
            CaptureTarget::Region(region) => self.do_capture_region(region, reuse),
        }
    }

    fn do_capture_monitor(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        let monitor = self.resolve_monitor_target()?;
        let key = monitor.key();
        let (history_reuse, last_history_seq) =
            self.monitor_runtime_mut().output.select_reuse(key, reuse);

        let cache_output_frame = history_reuse.should_cache_output();
        let monitor_for_capture = monitor.clone();
        let (frame, seq) = self.capture_with_retry(
            history_reuse.frame,
            last_history_seq,
            move |s| {
                let backend = Arc::clone(&s.backend);
                let config = s.config;
                s.monitor_runtime_mut().get_or_create_capturer(
                    backend,
                    config,
                    &monitor_for_capture,
                )
            },
            |s| {
                s.monitor_runtime_mut().clear_target(key);
            },
        )?;
        self.monitor_runtime_mut()
            .output
            .store_output(key, seq, &frame, cache_output_frame);
        Ok(frame)
    }

    fn do_capture_window(
        &mut self,
        window: WindowId,
        reuse: Option<Frame>,
    ) -> CaptureResult<Frame> {
        let key = window.key();
        let (history_reuse, last_history_seq) =
            self.window_runtime_mut().output.select_reuse(key, reuse);

        let cache_output_frame = history_reuse.should_cache_output();
        let (frame, seq) = self.capture_with_retry(
            history_reuse.frame,
            last_history_seq,
            move |s| {
                let backend = Arc::clone(&s.backend);
                let config = s.config;
                s.window_runtime_mut()
                    .get_or_create_capturer(backend, config, &window)
            },
            |s| {
                s.window_runtime_mut().clear_target(key);
            },
        )?;
        self.window_runtime_mut()
            .output
            .store_output(key, seq, &frame, cache_output_frame);
        Ok(frame)
    }

    fn do_capture_region(
        &mut self,
        region: CaptureRegion,
        reuse: Option<Frame>,
    ) -> CaptureResult<Frame> {
        let (entries, out_w, out_h, region_fully_covered, plan_changed) = {
            let backend = Arc::clone(&self.backend);
            let (plan, plan_changed) = self.region_runtime_mut().prepare_plan(&backend, &region)?;
            (
                Arc::clone(&plan.entries),
                plan.output_width,
                plan.output_height,
                plan.region_fully_covered,
                plan_changed,
            )
        };

        self.sequence = self.sequence.wrapping_add(1);
        let seq = self.sequence;

        let last_history_seq = self.region_runtime().output_history_seq;
        let history_reuse = HistoryReuseSelection::select(
            reuse,
            last_history_seq,
            self.region_runtime_mut().output_frame.take(),
        );
        let reuse_sequence = history_reuse
            .frame
            .as_ref()
            .map(|frame| frame.metadata.sequence);
        let cache_output_frame = history_reuse.should_cache_output();
        let mut out_frame = history_reuse.frame.unwrap_or_else(Frame::empty);
        let destination_has_history = matches!(
            (reuse_sequence, last_history_seq),
            (Some(reuse_seq), Some(last_seq)) if reuse_seq == last_seq
        ) && !plan_changed
            && out_frame.width() == out_w
            && out_frame.height() == out_h
            && out_frame.pixel_format() == self.config.output_pixel_format
            && !out_frame.as_rgba_bytes().is_empty();
        self.region_runtime_mut().output_history_seq = None;
        out_frame.ensure_capacity(out_w, out_h, self.config.output_pixel_format)?;
        out_frame.reset_metadata();
        if !destination_has_history && !region_fully_covered {
            out_frame.as_mut_rgba_bytes().fill(0);
        }

        if region_fully_covered
            && entries.len() == 1
            && let Some(first_entry) = entries.first()
        {
            let first_entry = first_entry.clone();
            let should_try_desktop_direct = self
                .region_runtime()
                .desktop_direct_support
                .get(&first_entry.monitor_key)
                .copied()
                .unwrap_or(true);
            if should_try_desktop_direct {
                let direct_sample = {
                    let backend = Arc::clone(&self.backend);
                    let config = self.config;
                    let capturer = self.region_runtime_mut().get_or_create_capturer(
                        backend,
                        config,
                        &first_entry.monitor,
                    )?;
                    capturer.capture_desktop_region_into(
                        region.x,
                        region.y,
                        out_w,
                        out_h,
                        &mut out_frame,
                        destination_has_history,
                    )
                };
                match direct_sample {
                    Ok(Some(sample)) => {
                        self.region_runtime_mut()
                            .desktop_direct_support
                            .insert(first_entry.monitor_key, true);
                        out_frame.metadata.sequence = seq;
                        out_frame.metadata.set_timing_with_format(
                            sample.capture_time,
                            sample.raw_os_ticks,
                            sample.tick_format,
                        );
                        out_frame.metadata.is_duplicate =
                            destination_has_history && sample.is_duplicate;
                        out_frame.metadata.dirty_rects = sample.dirty_rects;
                        self.region_runtime_mut().output_history_seq = Some(seq);
                        if cache_output_frame {
                            self.region_runtime_mut().output_frame = Some(out_frame.clone());
                        }
                        return Ok(out_frame);
                    }
                    Ok(None) => {
                        self.region_runtime_mut()
                            .desktop_direct_support
                            .insert(first_entry.monitor_key, false);
                    }
                    Err(_) => {}
                }
            }
        }

        let mut latest_capture_time = None;
        let mut latest_raw_os_ticks = None;
        let mut common_tick_format = None;
        let mut mixed_tick_formats = false;
        let mut all_duplicate = destination_has_history;
        let mut output_dirty_rects = Vec::new();
        let mut exact_damage = true;

        for entry in entries.iter() {
            let sample = {
                let backend = Arc::clone(&self.backend);
                let config = self.config;
                let capturer = self.region_runtime_mut().get_or_create_capturer(
                    backend,
                    config,
                    &entry.monitor,
                )?;
                capturer.capture_region_into(entry.blit, &mut out_frame, destination_has_history)?
            };

            let sample = if let Some(sample) = sample {
                sample
            } else {
                let reuse_frame = self
                    .region_runtime_mut()
                    .fallback_frames
                    .remove(&entry.monitor_key);
                let reuse_has_history = reuse_frame.is_some();
                let monitor_frame = {
                    let backend = Arc::clone(&self.backend);
                    let config = self.config;
                    let capturer = self.region_runtime_mut().get_or_create_capturer(
                        backend,
                        config,
                        &entry.monitor,
                    )?;
                    capturer.capture_with_history_hint(reuse_frame, reuse_has_history)?
                };

                copy_region_rgba(&monitor_frame, entry.blit, &mut out_frame)?;
                let sample = CaptureSampleMetadata {
                    capture_time: monitor_frame
                        .metadata
                        .stream_timestamp
                        .as_ref()
                        .map(|st| st.instant),
                    raw_os_ticks: monitor_frame
                        .metadata
                        .stream_timestamp
                        .as_ref()
                        .and_then(|st| st.raw_os_ticks),
                    tick_format: monitor_frame
                        .metadata
                        .stream_timestamp
                        .as_ref()
                        .map_or(TickFormat::RawQpc, |st| st.tick_format),
                    is_duplicate: monitor_frame.metadata.is_duplicate,
                    dirty_rects: Vec::new(),
                };
                self.region_runtime_mut()
                    .fallback_frames
                    .insert(entry.monitor_key, monitor_frame);
                sample
            };

            if let Some(t) = sample.capture_time {
                latest_capture_time = Some(
                    latest_capture_time.map_or(t, |current: std::time::Instant| current.max(t)),
                );
            }
            if let Some(raw_ticks) = sample.raw_os_ticks {
                match common_tick_format {
                    None => common_tick_format = Some(sample.tick_format),
                    Some(format) if format != sample.tick_format => mixed_tick_formats = true,
                    Some(_) => {}
                }
                if !mixed_tick_formats {
                    latest_raw_os_ticks = Some(
                        latest_raw_os_ticks
                            .map_or(raw_ticks, |current: i64| current.max(raw_ticks)),
                    );
                }
            }
            append_exact_sample_damage(
                &mut output_dirty_rects,
                &mut exact_damage,
                &sample,
                entry.blit.dst_x,
                entry.blit.dst_y,
            )?;
            all_duplicate &= sample.is_duplicate;
        }

        out_frame.metadata.sequence = seq;
        if mixed_tick_formats {
            latest_raw_os_ticks = None;
        }
        out_frame.metadata.set_timing_with_format(
            latest_capture_time,
            latest_raw_os_ticks,
            common_tick_format.unwrap_or(TickFormat::RawQpc),
        );
        out_frame.metadata.is_duplicate = all_duplicate;
        out_frame.metadata.dirty_rects = output_dirty_rects;
        self.region_runtime_mut().output_history_seq = Some(seq);
        if cache_output_frame {
            self.region_runtime_mut().output_frame = Some(out_frame.clone());
        }
        Ok(out_frame)
    }
}

fn append_exact_sample_damage(
    aggregate: &mut Vec<DirtyRect>,
    exact_damage: &mut bool,
    sample: &CaptureSampleMetadata,
    dst_x: u32,
    dst_y: u32,
) -> CaptureResult<()> {
    if !sample.is_duplicate && sample.dirty_rects.is_empty() {
        *exact_damage = false;
        aggregate.clear();
        return Ok(());
    }
    if !*exact_damage {
        return Ok(());
    }
    for mut rect in sample.dirty_rects.iter().copied() {
        rect.x = rect
            .x
            .checked_add(dst_x)
            .ok_or(CaptureError::BufferOverflow)?;
        rect.y = rect
            .y
            .checked_add(dst_y)
            .ok_or(CaptureError::BufferOverflow)?;
        aggregate.push(rect);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};
    use std::time::Instant;

    #[test]
    fn aggregate_damage_is_cleared_when_any_changed_source_is_inexact() {
        let mut aggregate = Vec::new();
        let mut exact_damage = true;
        let exact = CaptureSampleMetadata {
            is_duplicate: false,
            dirty_rects: vec![DirtyRect {
                x: 2,
                y: 3,
                width: 4,
                height: 5,
            }],
            ..CaptureSampleMetadata::default()
        };
        append_exact_sample_damage(&mut aggregate, &mut exact_damage, &exact, 10, 20).unwrap();
        assert_eq!(
            aggregate,
            vec![DirtyRect {
                x: 12,
                y: 23,
                width: 4,
                height: 5,
            }]
        );

        let inexact = CaptureSampleMetadata {
            is_duplicate: false,
            dirty_rects: Vec::new(),
            ..CaptureSampleMetadata::default()
        };
        append_exact_sample_damage(&mut aggregate, &mut exact_damage, &inexact, 0, 0).unwrap();
        assert!(!exact_damage);
        assert!(aggregate.is_empty());

        append_exact_sample_damage(&mut aggregate, &mut exact_damage, &exact, 0, 0).unwrap();
        assert!(aggregate.is_empty());
    }

    struct MockBackend {
        monitor: MonitorId,
        history_hints: Arc<Mutex<Vec<bool>>>,
    }

    impl MockBackend {
        fn new(history_hints: Arc<Mutex<Vec<bool>>>) -> Self {
            Self {
                monitor: MonitorId::from_parts(1, 2, 0, "mock-monitor", true),
                history_hints,
            }
        }
    }

    struct MockMonitorCapturer {
        history_hints: Arc<Mutex<Vec<bool>>>,
    }

    #[derive(Default)]
    struct LifecycleState {
        active: bool,
        prewarm_calls: usize,
        capture_calls: usize,
        release_calls: usize,
        fail_capture: bool,
    }

    struct LifecycleBackend {
        monitor: MonitorId,
        state: Arc<Mutex<LifecycleState>>,
    }

    struct LifecycleCapturer {
        state: Arc<Mutex<LifecycleState>>,
    }

    #[derive(Default)]
    struct ResolutionRetryState {
        create_calls: usize,
        capture_calls: usize,
    }

    struct ResolutionRetryBackend {
        monitor: MonitorId,
        state: Arc<Mutex<ResolutionRetryState>>,
    }

    struct ResolutionRetryCapturer {
        fail_with_resolution_change: bool,
        state: Arc<Mutex<ResolutionRetryState>>,
    }

    struct WindowInspectionBackend {
        observed_backends: Arc<Mutex<Vec<crate::backend::CaptureBackendKind>>>,
    }

    impl MonitorCapturer for LifecycleCapturer {
        fn backend_kind(&self) -> crate::backend::CaptureBackendKind {
            crate::backend::CaptureBackendKind::Gdi
        }

        fn prewarm_environment(&mut self) -> CaptureResult<()> {
            let mut state = self.state.lock().unwrap();
            assert!(!state.active);
            state.prewarm_calls += 1;
            Ok(())
        }

        fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
            let fail_capture = {
                let mut state = self.state.lock().unwrap();
                state.active = true;
                state.capture_calls += 1;
                state.fail_capture
            };
            if fail_capture {
                return Err(CaptureError::Timeout);
            }
            let mut frame = reuse.unwrap_or_else(Frame::empty);
            frame.ensure_rgba_capacity(4, 4)?;
            frame.reset_metadata();
            Ok(frame)
        }

        fn release_capture_access(&mut self) {
            let mut state = self.state.lock().unwrap();
            state.active = false;
            state.release_calls += 1;
        }

        fn capture_access_active(&self) -> bool {
            self.state.lock().unwrap().active
        }
    }

    impl CaptureBackend for LifecycleBackend {
        fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
            Ok(vec![self.monitor.clone()])
        }

        fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
            Ok(mock_layout(&self.monitor, 0, 0, 4, 4))
        }

        fn primary_monitor(&self) -> CaptureResult<MonitorId> {
            Ok(self.monitor.clone())
        }

        fn create_monitor_capturer(
            &self,
            _monitor: &MonitorId,
        ) -> CaptureResult<Box<dyn MonitorCapturer>> {
            Ok(Box::new(LifecycleCapturer {
                state: Arc::clone(&self.state),
            }))
        }
    }

    impl MonitorCapturer for ResolutionRetryCapturer {
        fn backend_kind(&self) -> crate::backend::CaptureBackendKind {
            crate::backend::CaptureBackendKind::DxgiDuplication
        }

        fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
            self.state.lock().unwrap().capture_calls += 1;
            if self.fail_with_resolution_change {
                return Err(CaptureError::ResolutionChanged(8, 6));
            }
            let mut frame = reuse.unwrap_or_else(Frame::empty);
            frame.ensure_rgba_capacity(8, 6)?;
            frame.reset_metadata();
            Ok(frame)
        }
    }

    impl CaptureBackend for ResolutionRetryBackend {
        fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
            Ok(vec![self.monitor.clone()])
        }

        fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
            Ok(mock_layout(&self.monitor, 0, 0, 8, 6))
        }

        fn primary_monitor(&self) -> CaptureResult<MonitorId> {
            Ok(self.monitor.clone())
        }

        fn create_monitor_capturer(
            &self,
            _monitor: &MonitorId,
        ) -> CaptureResult<Box<dyn MonitorCapturer>> {
            let fail_with_resolution_change = {
                let mut state = self.state.lock().unwrap();
                state.create_calls += 1;
                state.create_calls == 1
            };
            Ok(Box::new(ResolutionRetryCapturer {
                fail_with_resolution_change,
                state: Arc::clone(&self.state),
            }))
        }
    }

    impl CaptureBackend for WindowInspectionBackend {
        fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
            Ok(Vec::new())
        }

        fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
            Err(CaptureError::NoPrimaryMonitor)
        }

        fn primary_monitor(&self) -> CaptureResult<MonitorId> {
            Err(CaptureError::NoPrimaryMonitor)
        }

        fn inspect_window(&self, _window: &WindowId) -> CaptureResult<CaptureTargetInfo> {
            Ok(CaptureTargetInfo {
                origin_x: 1,
                origin_y: 2,
                width: 3,
                height: 4,
            })
        }

        fn inspect_window_for_backend(
            &self,
            _window: &WindowId,
            backend_kind: crate::backend::CaptureBackendKind,
        ) -> CaptureResult<CaptureTargetInfo> {
            self.observed_backends.lock().unwrap().push(backend_kind);
            Ok(CaptureTargetInfo {
                origin_x: 11,
                origin_y: 12,
                width: 13,
                height: 14,
            })
        }

        fn create_monitor_capturer(
            &self,
            _monitor: &MonitorId,
        ) -> CaptureResult<Box<dyn MonitorCapturer>> {
            Err(CaptureError::NoPrimaryMonitor)
        }
    }

    fn lifecycle_session(state: Arc<Mutex<LifecycleState>>) -> CaptureResult<CaptureSession> {
        let backend: Arc<dyn CaptureBackend> = Arc::new(LifecycleBackend {
            monitor: MonitorId::from_parts(41, 43, 0, "lifecycle-monitor", true),
            state,
        });
        CaptureSession::builder()
            .target(CaptureTarget::PrimaryMonitor)
            .with_backend(backend)
            .build()
    }

    impl MonitorCapturer for MockMonitorCapturer {
        fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
            self.capture_with_history_hint(reuse, false)
        }

        fn capture_with_history_hint(
            &mut self,
            reuse: Option<Frame>,
            destination_has_history: bool,
        ) -> CaptureResult<Frame> {
            self.history_hints
                .lock()
                .unwrap()
                .push(destination_has_history);
            let mut frame = reuse.unwrap_or_else(Frame::empty);
            frame.ensure_rgba_capacity(4, 4)?;
            frame.reset_metadata();
            Ok(frame)
        }
    }

    impl CaptureBackend for MockBackend {
        fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
            Ok(vec![self.monitor.clone()])
        }

        fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
            Ok(mock_layout(&self.monitor, 0, 0, 4, 4))
        }

        fn primary_monitor(&self) -> CaptureResult<MonitorId> {
            Ok(self.monitor.clone())
        }

        fn create_monitor_capturer(
            &self,
            _monitor: &MonitorId,
        ) -> CaptureResult<Box<dyn MonitorCapturer>> {
            Ok(Box::new(MockMonitorCapturer {
                history_hints: Arc::clone(&self.history_hints),
            }))
        }
    }

    struct MetadataDrivenBackend {
        monitor: MonitorId,
        inferred_history: Arc<Mutex<Vec<bool>>>,
    }

    impl MetadataDrivenBackend {
        fn new(inferred_history: Arc<Mutex<Vec<bool>>>) -> Self {
            Self {
                monitor: MonitorId::from_parts(7, 9, 0, "metadata-monitor", true),
                inferred_history,
            }
        }
    }

    struct MetadataDrivenCapturer {
        inferred_history: Arc<Mutex<Vec<bool>>>,
    }

    impl MonitorCapturer for MetadataDrivenCapturer {
        fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
            let inferred_history = reuse
                .as_ref()
                .is_some_and(|frame| frame.metadata.stream_timestamp.is_some())
                && reuse
                    .as_ref()
                    .is_some_and(|frame| !frame.as_rgba_bytes().is_empty());
            self.inferred_history.lock().unwrap().push(inferred_history);

            let mut frame = reuse.unwrap_or_else(Frame::empty);
            frame.ensure_rgba_capacity(4, 4)?;
            frame.reset_metadata();
            frame.metadata.set_timing(Some(Instant::now()), None);
            Ok(frame)
        }
    }

    impl CaptureBackend for MetadataDrivenBackend {
        fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
            Ok(vec![self.monitor.clone()])
        }

        fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
            Ok(mock_layout(&self.monitor, 0, 0, 4, 4))
        }

        fn primary_monitor(&self) -> CaptureResult<MonitorId> {
            Ok(self.monitor.clone())
        }

        fn create_monitor_capturer(
            &self,
            _monitor: &MonitorId,
        ) -> CaptureResult<Box<dyn MonitorCapturer>> {
            Ok(Box::new(MetadataDrivenCapturer {
                inferred_history: Arc::clone(&self.inferred_history),
            }))
        }
    }

    struct DirectRegionBackend {
        monitor: MonitorId,
        desktop_calls: Arc<Mutex<usize>>,
        region_calls: Arc<Mutex<usize>>,
        supports_desktop_direct: bool,
        desktop_should_error: bool,
    }

    struct DirectRegionCapturer {
        desktop_calls: Arc<Mutex<usize>>,
        region_calls: Arc<Mutex<usize>>,
        supports_desktop_direct: bool,
        desktop_should_error: bool,
    }

    impl MonitorCapturer for DirectRegionCapturer {
        fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
            Ok(reuse.unwrap_or_else(Frame::empty))
        }

        fn capture_region_into(
            &mut self,
            _blit: CaptureBlitRegion,
            _destination: &mut Frame,
            _destination_has_history: bool,
        ) -> CaptureResult<Option<CaptureSampleMetadata>> {
            *self.region_calls.lock().unwrap() += 1;
            Ok(Some(CaptureSampleMetadata {
                capture_time: Some(Instant::now()),
                raw_os_ticks: Some(0),
                tick_format: TickFormat::RawQpc,
                is_duplicate: false,
                dirty_rects: Vec::new(),
            }))
        }

        fn capture_desktop_region_into(
            &mut self,
            _x: i32,
            _y: i32,
            _width: u32,
            _height: u32,
            _destination: &mut Frame,
            _destination_has_history: bool,
        ) -> CaptureResult<Option<CaptureSampleMetadata>> {
            *self.desktop_calls.lock().unwrap() += 1;
            if self.desktop_should_error {
                return Err(CaptureError::platform(anyhow::anyhow!(
                    "mock desktop direct failure"
                )));
            }
            if !self.supports_desktop_direct {
                return Ok(None);
            }
            Ok(Some(CaptureSampleMetadata {
                capture_time: Some(Instant::now()),
                raw_os_ticks: Some(0),
                tick_format: TickFormat::RawQpc,
                is_duplicate: true,
                dirty_rects: Vec::new(),
            }))
        }
    }

    struct RegionHistoryBackend {
        monitor: MonitorId,
        history_hints: Arc<Mutex<Vec<bool>>>,
    }

    struct RegionHistoryCapturer {
        history_hints: Arc<Mutex<Vec<bool>>>,
    }

    struct RegionClearBehaviorBackend {
        monitor: MonitorId,
        observed_zero_before_write: Arc<Mutex<Vec<bool>>>,
        fill_byte: u8,
    }

    struct RegionClearBehaviorCapturer {
        observed_zero_before_write: Arc<Mutex<Vec<bool>>>,
        fill_byte: u8,
    }

    impl MonitorCapturer for RegionHistoryCapturer {
        fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
            Ok(reuse.unwrap_or_else(Frame::empty))
        }

        fn capture_region_into(
            &mut self,
            _blit: CaptureBlitRegion,
            destination: &mut Frame,
            destination_has_history: bool,
        ) -> CaptureResult<Option<CaptureSampleMetadata>> {
            self.history_hints
                .lock()
                .unwrap()
                .push(destination_has_history);
            destination.ensure_rgba_capacity(4, 4)?;
            destination.reset_metadata();
            Ok(Some(CaptureSampleMetadata {
                capture_time: Some(Instant::now()),
                raw_os_ticks: Some(0),
                tick_format: TickFormat::RawQpc,
                is_duplicate: destination_has_history,
                dirty_rects: Vec::new(),
            }))
        }
    }

    impl CaptureBackend for RegionHistoryBackend {
        fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
            Ok(vec![self.monitor.clone()])
        }

        fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
            Ok(mock_layout(&self.monitor, 0, 0, 4, 4))
        }

        fn primary_monitor(&self) -> CaptureResult<MonitorId> {
            Ok(self.monitor.clone())
        }

        fn create_monitor_capturer(
            &self,
            _monitor: &MonitorId,
        ) -> CaptureResult<Box<dyn MonitorCapturer>> {
            Ok(Box::new(RegionHistoryCapturer {
                history_hints: Arc::clone(&self.history_hints),
            }))
        }
    }

    impl CaptureBackend for DirectRegionBackend {
        fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
            Ok(vec![self.monitor.clone()])
        }

        fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
            Ok(mock_layout(&self.monitor, 0, 0, 1920, 1080))
        }

        fn primary_monitor(&self) -> CaptureResult<MonitorId> {
            Ok(self.monitor.clone())
        }

        fn create_monitor_capturer(
            &self,
            _monitor: &MonitorId,
        ) -> CaptureResult<Box<dyn MonitorCapturer>> {
            Ok(Box::new(DirectRegionCapturer {
                desktop_calls: Arc::clone(&self.desktop_calls),
                region_calls: Arc::clone(&self.region_calls),
                supports_desktop_direct: self.supports_desktop_direct,
                desktop_should_error: self.desktop_should_error,
            }))
        }
    }

    impl MonitorCapturer for RegionClearBehaviorCapturer {
        fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
            Ok(reuse.unwrap_or_else(Frame::empty))
        }

        fn capture_region_into(
            &mut self,
            blit: CaptureBlitRegion,
            destination: &mut Frame,
            _destination_has_history: bool,
        ) -> CaptureResult<Option<CaptureSampleMetadata>> {
            let was_zero = destination.as_rgba_bytes().iter().all(|&byte| byte == 0);
            self.observed_zero_before_write
                .lock()
                .unwrap()
                .push(was_zero);

            let dst_width = destination.width() as usize;
            let dst_x = blit.dst_x as usize;
            let dst_y = blit.dst_y as usize;
            let copy_w = blit.width as usize;
            let copy_h = blit.height as usize;
            let row_bytes = copy_w * 4;
            let dst_stride = dst_width * 4;
            let fill_row = vec![self.fill_byte; row_bytes];
            let bytes = destination.as_mut_rgba_bytes();
            for row in 0..copy_h {
                let offset = (dst_y + row) * dst_stride + dst_x * 4;
                bytes[offset..offset + row_bytes].copy_from_slice(&fill_row);
            }

            Ok(Some(CaptureSampleMetadata {
                capture_time: Some(Instant::now()),
                raw_os_ticks: Some(0),
                tick_format: TickFormat::RawQpc,
                is_duplicate: false,
                dirty_rects: Vec::new(),
            }))
        }
    }

    impl CaptureBackend for RegionClearBehaviorBackend {
        fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
            Ok(vec![self.monitor.clone()])
        }

        fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
            Ok(mock_layout(&self.monitor, 0, 0, 4, 4))
        }

        fn primary_monitor(&self) -> CaptureResult<MonitorId> {
            Ok(self.monitor.clone())
        }

        fn create_monitor_capturer(
            &self,
            _monitor: &MonitorId,
        ) -> CaptureResult<Box<dyn MonitorCapturer>> {
            Ok(Box::new(RegionClearBehaviorCapturer {
                observed_zero_before_write: Arc::clone(&self.observed_zero_before_write),
                fill_byte: self.fill_byte,
            }))
        }
    }

    fn mock_layout(monitor: &MonitorId, x: i32, y: i32, width: u32, height: u32) -> MonitorLayout {
        MonitorLayout {
            monitors: vec![crate::region::MonitorGeometry {
                monitor: monitor.clone(),
                x,
                y,
                width,
                height,
            }],
            virtual_left: x,
            virtual_top: y,
            virtual_width: width,
            virtual_height: height,
        }
    }

    #[test]
    fn environment_prewarm_does_not_leave_capture_access_active() -> CaptureResult<()> {
        let state = Arc::new(Mutex::new(LifecycleState::default()));
        let mut session = lifecycle_session(Arc::clone(&state))?;

        let info = session.prewarm_environment()?;

        assert_eq!((info.width, info.height), (4, 4));
        assert_eq!(session.active_capture_access_count(), 0);
        let state = state.lock().unwrap();
        assert_eq!(state.prewarm_calls, 1);
        assert_eq!(state.capture_calls, 0);
        assert!(!state.active);
        Ok(())
    }

    #[test]
    fn owned_one_shot_capture_releases_access_and_reports_concrete_backend() -> CaptureResult<()> {
        let state = Arc::new(Mutex::new(LifecycleState::default()));
        let mut session = lifecycle_session(Arc::clone(&state))?;

        let frame = session.capture_once()?;

        assert_eq!(session.active_capture_access_count(), 0);
        assert!(session.monitor_output_cache_is_empty());
        assert_eq!(
            frame.metadata().backend_kind(),
            crate::backend::CaptureBackendKind::Gdi
        );
        let state = state.lock().unwrap();
        assert_eq!(state.capture_calls, 1);
        assert_eq!(state.release_calls, 2);
        assert!(!state.active);
        Ok(())
    }

    #[test]
    fn one_shot_capture_returns_to_prepared_state_and_is_reusable() -> CaptureResult<()> {
        let state = Arc::new(Mutex::new(LifecycleState::default()));
        let mut session = lifecycle_session(Arc::clone(&state))?;

        session.prewarm_environment()?;
        let first = session.capture_once()?;
        assert_eq!(session.active_capture_access_count(), 0);
        assert!(session.monitor_output_cache_is_empty());
        let _second = session.capture_once()?;

        assert_eq!(first.dimensions(), (4, 4));

        let state = state.lock().unwrap();
        assert_eq!(state.capture_calls, 2);
        assert_eq!(state.prewarm_calls, 3);
        assert_eq!(state.release_calls, 5);
        assert!(!state.active);
        Ok(())
    }

    #[test]
    fn failed_one_shot_capture_releases_access_before_returning() -> CaptureResult<()> {
        let state = Arc::new(Mutex::new(LifecycleState {
            fail_capture: true,
            ..LifecycleState::default()
        }));
        let mut session = lifecycle_session(Arc::clone(&state))?;
        let mut frame = Frame::empty();
        frame.ensure_rgba_capacity(4, 4)?;

        assert!(matches!(
            session.capture_once_into(&mut frame),
            Err(CaptureError::Timeout)
        ));

        assert_eq!(session.active_capture_access_count(), 0);
        assert_eq!(frame.dimensions(), (4, 4));
        assert_eq!(frame.as_rgba_bytes().len(), 4 * 4 * 4);
        let state = state.lock().unwrap();
        assert_eq!(state.capture_calls, 1);
        assert_eq!(state.release_calls, 2);
        assert!(!state.active);
        Ok(())
    }

    #[test]
    fn resolution_change_recreates_worker_and_retries_same_backend() -> CaptureResult<()> {
        let state = Arc::new(Mutex::new(ResolutionRetryState::default()));
        let backend: Arc<dyn CaptureBackend> = Arc::new(ResolutionRetryBackend {
            monitor: MonitorId::from_parts(47, 53, 0, "resolution-monitor", true),
            state: Arc::clone(&state),
        });
        let options = CaptureOptions {
            capture_retry_count: 1,
            ..CaptureOptions::default()
        };
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::PrimaryMonitor)
            .with_backend(backend)
            .with_options(options)
            .build()?;

        let frame = session.capture()?;

        assert_eq!(frame.dimensions(), (8, 6));
        assert_eq!(
            frame.metadata().backend_kind(),
            crate::backend::CaptureBackendKind::DxgiDuplication
        );
        let state = state.lock().unwrap();
        assert_eq!(state.create_calls, 2);
        assert_eq!(state.capture_calls, 2);
        Ok(())
    }

    #[test]
    fn window_target_info_for_backend_forwards_concrete_backend() -> CaptureResult<()> {
        let observed_backends = Arc::new(Mutex::new(Vec::new()));
        let backend: Arc<dyn CaptureBackend> = Arc::new(WindowInspectionBackend {
            observed_backends: Arc::clone(&observed_backends),
        });
        let session = CaptureSession::builder()
            .target(CaptureTarget::Window(WindowId::from_raw_handle(101)))
            .with_backend(backend)
            .build()?;

        let info =
            session.target_info_for_backend(crate::backend::CaptureBackendKind::DxgiDuplication)?;

        assert_eq!(
            info,
            CaptureTargetInfo {
                origin_x: 11,
                origin_y: 12,
                width: 13,
                height: 14,
            }
        );
        assert_eq!(
            observed_backends.lock().unwrap().as_slice(),
            &[crate::backend::CaptureBackendKind::DxgiDuplication]
        );
        Ok(())
    }

    #[test]
    fn fully_covered_region_capture_skips_initial_clear() -> CaptureResult<()> {
        let observed_zero_before_write = Arc::new(Mutex::new(Vec::new()));
        let monitor = MonitorId::from_parts(11, 13, 0, "clear-monitor", true);
        let backend: Arc<dyn CaptureBackend> = Arc::new(RegionClearBehaviorBackend {
            monitor: monitor.clone(),
            observed_zero_before_write: Arc::clone(&observed_zero_before_write),
            fill_byte: 0x11,
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(0, 0, 4, 4)?))
            .with_backend(backend)
            .build()?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 4, 4));

        let mut reuse = Frame::empty();
        reuse.ensure_rgba_capacity(4, 4)?;
        reuse.as_mut_rgba_bytes().fill(0xAB);

        let frame = session.capture_reuse(reuse)?;
        assert!(frame.as_rgba_bytes().iter().all(|&byte| byte == 0x11));
        assert_eq!(
            observed_zero_before_write.lock().unwrap().as_slice(),
            &[false]
        );
        Ok(())
    }

    #[test]
    fn partially_covered_region_capture_still_clears_initial_output() -> CaptureResult<()> {
        let observed_zero_before_write = Arc::new(Mutex::new(Vec::new()));
        let monitor = MonitorId::from_parts(17, 19, 0, "partial-clear-monitor", true);
        let backend: Arc<dyn CaptureBackend> = Arc::new(RegionClearBehaviorBackend {
            monitor: monitor.clone(),
            observed_zero_before_write: Arc::clone(&observed_zero_before_write),
            fill_byte: 0x22,
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(0, 0, 4, 4)?))
            .with_backend(backend)
            .build()?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 2, 4));

        let mut reuse = Frame::empty();
        reuse.ensure_rgba_capacity(4, 4)?;
        reuse.as_mut_rgba_bytes().fill(0xAB);

        let frame = session.capture_reuse(reuse)?;
        let bytes = frame.as_rgba_bytes();
        assert_eq!(
            observed_zero_before_write.lock().unwrap().as_slice(),
            &[true]
        );
        for row in 0..4usize {
            let row_start = row * 16;
            assert!(
                bytes[row_start..row_start + 8]
                    .iter()
                    .all(|&byte| byte == 0x22)
            );
            assert!(
                bytes[row_start + 8..row_start + 16]
                    .iter()
                    .all(|&byte| byte == 0x00)
            );
        }
        Ok(())
    }

    #[test]
    fn screenshot_monitor_reuse_retains_history_in_hot_session() -> CaptureResult<()> {
        let history_hints = Arc::new(Mutex::new(Vec::new()));
        let backend: Arc<dyn CaptureBackend> =
            Arc::new(MockBackend::new(Arc::clone(&history_hints)));
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::PrimaryMonitor)
            .with_backend(backend)
            .build()?;

        let first = session.capture()?;
        let second = session.capture_reuse(first)?;
        let _third = session.capture_reuse(second)?;

        let recorded = history_hints.lock().unwrap().clone();
        assert_eq!(recorded, vec![false, true, true]);
        Ok(())
    }

    #[test]
    fn screenshot_monitor_capture_into_keeps_history_without_session_clone_cache()
    -> CaptureResult<()> {
        let history_hints = Arc::new(Mutex::new(Vec::new()));
        let backend: Arc<dyn CaptureBackend> =
            Arc::new(MockBackend::new(Arc::clone(&history_hints)));
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::PrimaryMonitor)
            .with_backend(backend)
            .build()?;
        let mut frame = Frame::empty();

        session.capture_into(&mut frame)?;
        assert!(session.monitor_output_cache_is_empty());
        session.capture_into(&mut frame)?;
        session.capture_into(&mut frame)?;

        let recorded = history_hints.lock().unwrap().clone();
        assert_eq!(recorded, vec![false, true, true]);
        assert!(session.monitor_output_cache_is_empty());
        Ok(())
    }

    #[test]
    fn release_idle_resources_clears_snapshot_monitor_history_and_recovers() -> CaptureResult<()> {
        let history_hints = Arc::new(Mutex::new(Vec::new()));
        let backend: Arc<dyn CaptureBackend> =
            Arc::new(MockBackend::new(Arc::clone(&history_hints)));
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::PrimaryMonitor)
            .with_backend(backend)
            .build()?;

        let first = session.capture()?;
        let _second = session.capture_reuse(first)?;
        session.release_idle_resources();
        let _third = session.capture()?;

        let recorded = history_hints.lock().unwrap().clone();
        assert_eq!(recorded, vec![false, true, false]);
        Ok(())
    }

    #[test]
    fn recording_monitor_reuse_recovers_history_from_session_cache() -> CaptureResult<()> {
        let history_hints = Arc::new(Mutex::new(Vec::new()));
        let backend: Arc<dyn CaptureBackend> =
            Arc::new(MockBackend::new(Arc::clone(&history_hints)));
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::PrimaryMonitor)
            .with_backend(backend)
            .capture_mode(CaptureMode::Continuous)
            .build()?;

        let mut first = session.capture()?;
        first.metadata.sequence = first.metadata.sequence.wrapping_add(1);
        let second = session.capture_reuse(first)?;
        let _third = session.capture_reuse(second)?;

        let recorded = history_hints.lock().unwrap().clone();
        assert_eq!(recorded, vec![false, true, true]);
        Ok(())
    }

    #[test]
    fn release_idle_resources_preserves_continuous_monitor_history() -> CaptureResult<()> {
        let history_hints = Arc::new(Mutex::new(Vec::new()));
        let backend: Arc<dyn CaptureBackend> =
            Arc::new(MockBackend::new(Arc::clone(&history_hints)));
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::PrimaryMonitor)
            .with_backend(backend)
            .capture_mode(CaptureMode::Continuous)
            .build()?;

        let mut first = session.capture()?;
        first.metadata.sequence = first.metadata.sequence.wrapping_add(1);
        session.release_idle_resources();
        let second = session.capture_reuse(first)?;
        let _third = session.capture_reuse(second)?;

        let recorded = history_hints.lock().unwrap().clone();
        assert_eq!(recorded, vec![false, true, true]);
        Ok(())
    }

    #[test]
    fn recording_invalid_reuse_sequence_falls_back_to_session_history() -> CaptureResult<()> {
        let inferred_history = Arc::new(Mutex::new(Vec::new()));
        let backend: Arc<dyn CaptureBackend> =
            Arc::new(MetadataDrivenBackend::new(Arc::clone(&inferred_history)));
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::PrimaryMonitor)
            .with_backend(backend)
            .capture_mode(CaptureMode::Continuous)
            .build()?;

        let mut first = session.capture()?;
        first.metadata.sequence = first.metadata.sequence.wrapping_add(1);
        let second = session.capture_reuse(first)?;
        let _third = session.capture_reuse(second)?;

        let recorded = inferred_history.lock().unwrap().clone();
        assert_eq!(recorded, vec![false, true, true]);
        Ok(())
    }

    #[test]
    fn screenshot_region_reuse_retains_history_in_hot_session() -> CaptureResult<()> {
        let history_hints = Arc::new(Mutex::new(Vec::new()));
        let monitor = MonitorId::from_parts(1, 2, 0, "mock-monitor", true);
        let backend: Arc<dyn CaptureBackend> = Arc::new(RegionHistoryBackend {
            monitor: monitor.clone(),
            history_hints: Arc::clone(&history_hints),
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(0, 0, 4, 4)?))
            .with_backend(backend)
            .build()?;

        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 4, 4));
        let first = session.capture()?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 4, 4));
        let second = session.capture_reuse(first)?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 4, 4));
        let _third = session.capture_reuse(second)?;

        let recorded = history_hints.lock().unwrap().clone();
        assert_eq!(recorded, vec![false, true, true]);
        Ok(())
    }

    #[test]
    fn screenshot_region_capture_into_keeps_history_without_session_clone_cache()
    -> CaptureResult<()> {
        let history_hints = Arc::new(Mutex::new(Vec::new()));
        let monitor = MonitorId::from_parts(1, 2, 0, "mock-monitor", true);
        let backend: Arc<dyn CaptureBackend> = Arc::new(RegionHistoryBackend {
            monitor: monitor.clone(),
            history_hints: Arc::clone(&history_hints),
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(0, 0, 4, 4)?))
            .with_backend(backend)
            .build()?;
        let mut frame = Frame::empty();

        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 4, 4));
        session.capture_into(&mut frame)?;
        assert!(session.region_output_cache_is_none());
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 4, 4));
        session.capture_into(&mut frame)?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 4, 4));
        session.capture_into(&mut frame)?;

        let recorded = history_hints.lock().unwrap().clone();
        assert_eq!(recorded, vec![false, true, true]);
        assert!(session.region_output_cache_is_none());
        Ok(())
    }

    #[test]
    fn release_idle_resources_clears_snapshot_region_history_and_recovers() -> CaptureResult<()> {
        let history_hints = Arc::new(Mutex::new(Vec::new()));
        let monitor = MonitorId::from_parts(1, 2, 0, "mock-monitor", true);
        let backend: Arc<dyn CaptureBackend> = Arc::new(RegionHistoryBackend {
            monitor: monitor.clone(),
            history_hints: Arc::clone(&history_hints),
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(0, 0, 4, 4)?))
            .with_backend(backend)
            .build()?;

        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 4, 4));
        let first = session.capture()?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 4, 4));
        let _second = session.capture_reuse(first)?;
        session.release_idle_resources();
        assert!(session.region_output_cache_is_none());
        let _third = session.capture()?;

        let recorded = history_hints.lock().unwrap().clone();
        assert_eq!(recorded, vec![false, true, false]);
        Ok(())
    }

    #[test]
    fn recording_region_reuse_recovers_history_from_session_cache() -> CaptureResult<()> {
        let history_hints = Arc::new(Mutex::new(Vec::new()));
        let monitor = MonitorId::from_parts(1, 2, 0, "mock-monitor", true);
        let backend: Arc<dyn CaptureBackend> = Arc::new(RegionHistoryBackend {
            monitor: monitor.clone(),
            history_hints: Arc::clone(&history_hints),
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(0, 0, 4, 4)?))
            .with_backend(backend)
            .capture_mode(CaptureMode::Continuous)
            .build()?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 4, 4));

        let mut first = session.capture()?;
        first.metadata.sequence = first.metadata.sequence.wrapping_add(1);
        let second = session.capture_reuse(first)?;
        let _third = session.capture_reuse(second)?;

        let recorded = history_hints.lock().unwrap().clone();
        assert_eq!(recorded, vec![false, true, true]);
        Ok(())
    }

    #[test]
    fn inspect_target_returns_region_geometry() -> CaptureResult<()> {
        let history_hints = Arc::new(Mutex::new(Vec::new()));
        let backend: Arc<dyn CaptureBackend> =
            Arc::new(MockBackend::new(Arc::clone(&history_hints)));
        let session = CaptureSession::builder()
            .target(CaptureTarget::PrimaryMonitor)
            .with_backend(backend)
            .build()?;

        let info =
            session.inspect_target(&CaptureTarget::Region(CaptureRegion::new(10, 20, 30, 40)?))?;
        assert_eq!(info.origin_x, 10);
        assert_eq!(info.origin_y, 20);
        assert_eq!(info.width, 30);
        assert_eq!(info.height, 40);
        Ok(())
    }

    #[test]
    fn screenshot_region_capture_uses_desktop_direct_history_on_reuse() -> CaptureResult<()> {
        let desktop_calls = Arc::new(Mutex::new(0usize));
        let region_calls = Arc::new(Mutex::new(0usize));
        let monitor = MonitorId::from_parts(1, 2, 0, "mock-monitor", true);
        let backend: Arc<dyn CaptureBackend> = Arc::new(DirectRegionBackend {
            monitor: monitor.clone(),
            desktop_calls: Arc::clone(&desktop_calls),
            region_calls: Arc::clone(&region_calls),
            supports_desktop_direct: true,
            desktop_should_error: false,
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(
                100, 50, 640, 360,
            )?))
            .with_backend(backend)
            .build()?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 1920, 1080));

        let frame = session.capture()?;
        assert!(!frame.metadata.is_duplicate);
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 1920, 1080));
        let next_frame = session.capture_reuse(frame)?;

        assert!(next_frame.metadata.is_duplicate);
        assert_eq!(*desktop_calls.lock().unwrap(), 2);
        assert_eq!(*region_calls.lock().unwrap(), 0);
        Ok(())
    }

    #[test]
    fn region_capture_falls_back_when_desktop_direct_path_is_unavailable() -> CaptureResult<()> {
        let desktop_calls = Arc::new(Mutex::new(0usize));
        let region_calls = Arc::new(Mutex::new(0usize));
        let monitor = MonitorId::from_parts(1, 2, 0, "mock-monitor", true);
        let backend: Arc<dyn CaptureBackend> = Arc::new(DirectRegionBackend {
            monitor: monitor.clone(),
            desktop_calls: Arc::clone(&desktop_calls),
            region_calls: Arc::clone(&region_calls),
            supports_desktop_direct: false,
            desktop_should_error: false,
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(
                100, 50, 640, 360,
            )?))
            .with_backend(backend)
            .build()?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 1920, 1080));

        let _frame = session.capture()?;

        assert_eq!(*desktop_calls.lock().unwrap(), 1);
        assert_eq!(*region_calls.lock().unwrap(), 1);
        Ok(())
    }

    #[test]
    fn recording_region_capture_caches_unavailable_desktop_direct_path() -> CaptureResult<()> {
        let desktop_calls = Arc::new(Mutex::new(0usize));
        let region_calls = Arc::new(Mutex::new(0usize));
        let monitor = MonitorId::from_parts(1, 2, 0, "mock-monitor", true);
        let backend: Arc<dyn CaptureBackend> = Arc::new(DirectRegionBackend {
            monitor: monitor.clone(),
            desktop_calls: Arc::clone(&desktop_calls),
            region_calls: Arc::clone(&region_calls),
            supports_desktop_direct: false,
            desktop_should_error: false,
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(
                100, 50, 640, 360,
            )?))
            .with_backend(backend)
            .capture_mode(CaptureMode::Continuous)
            .build()?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 1920, 1080));

        let frame = session.capture()?;
        let _frame = session.capture_reuse(frame)?;

        assert_eq!(*desktop_calls.lock().unwrap(), 1);
        assert_eq!(*region_calls.lock().unwrap(), 2);
        Ok(())
    }

    #[test]
    fn screenshot_region_capture_caches_unavailable_desktop_direct_path_in_hot_session()
    -> CaptureResult<()> {
        let desktop_calls = Arc::new(Mutex::new(0usize));
        let region_calls = Arc::new(Mutex::new(0usize));
        let monitor = MonitorId::from_parts(1, 2, 0, "mock-monitor", true);
        let backend: Arc<dyn CaptureBackend> = Arc::new(DirectRegionBackend {
            monitor: monitor.clone(),
            desktop_calls: Arc::clone(&desktop_calls),
            region_calls: Arc::clone(&region_calls),
            supports_desktop_direct: false,
            desktop_should_error: false,
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(
                100, 50, 640, 360,
            )?))
            .with_backend(backend)
            .build()?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 1920, 1080));

        let _first = session.capture()?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 1920, 1080));
        let _second = session.capture()?;

        assert_eq!(*desktop_calls.lock().unwrap(), 1);
        assert_eq!(*region_calls.lock().unwrap(), 2);
        Ok(())
    }

    #[test]
    fn region_capture_skips_desktop_direct_path_for_multi_monitor_regions() -> CaptureResult<()> {
        let desktop_calls = Arc::new(Mutex::new(0usize));
        let region_calls = Arc::new(Mutex::new(0usize));
        let first_monitor = MonitorId::from_parts(1, 2, 0, "mock-monitor-a", true);
        let second_monitor = MonitorId::from_parts(1, 2, 1, "mock-monitor-b", false);
        let backend: Arc<dyn CaptureBackend> = Arc::new(DirectRegionBackend {
            monitor: first_monitor.clone(),
            desktop_calls: Arc::clone(&desktop_calls),
            region_calls: Arc::clone(&region_calls),
            supports_desktop_direct: true,
            desktop_should_error: false,
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(0, 0, 1280, 480)?))
            .with_backend(backend)
            .build()?;
        session.set_region_layout_for_test(MonitorLayout {
            monitors: vec![
                crate::region::MonitorGeometry {
                    monitor: first_monitor,
                    x: 0,
                    y: 0,
                    width: 640,
                    height: 480,
                },
                crate::region::MonitorGeometry {
                    monitor: second_monitor,
                    x: 640,
                    y: 0,
                    width: 640,
                    height: 480,
                },
            ],
            virtual_left: 0,
            virtual_top: 0,
            virtual_width: 1280,
            virtual_height: 480,
        });

        let _frame = session.capture()?;

        assert_eq!(*desktop_calls.lock().unwrap(), 0);
        assert_eq!(*region_calls.lock().unwrap(), 2);
        Ok(())
    }

    #[test]
    fn region_capture_falls_back_when_desktop_direct_path_errors() -> CaptureResult<()> {
        let desktop_calls = Arc::new(Mutex::new(0usize));
        let region_calls = Arc::new(Mutex::new(0usize));
        let monitor = MonitorId::from_parts(1, 2, 0, "mock-monitor", true);
        let backend: Arc<dyn CaptureBackend> = Arc::new(DirectRegionBackend {
            monitor: monitor.clone(),
            desktop_calls: Arc::clone(&desktop_calls),
            region_calls: Arc::clone(&region_calls),
            supports_desktop_direct: true,
            desktop_should_error: true,
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(
                100, 50, 640, 360,
            )?))
            .with_backend(backend)
            .build()?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 1920, 1080));

        let _frame = session.capture()?;

        assert_eq!(*desktop_calls.lock().unwrap(), 1);
        assert_eq!(*region_calls.lock().unwrap(), 1);
        Ok(())
    }

    #[test]
    fn region_capture_skips_desktop_direct_path_for_partial_coverage() -> CaptureResult<()> {
        let desktop_calls = Arc::new(Mutex::new(0usize));
        let region_calls = Arc::new(Mutex::new(0usize));
        let monitor = MonitorId::from_parts(1, 2, 0, "mock-monitor", true);
        let backend: Arc<dyn CaptureBackend> = Arc::new(DirectRegionBackend {
            monitor: monitor.clone(),
            desktop_calls: Arc::clone(&desktop_calls),
            region_calls: Arc::clone(&region_calls),
            supports_desktop_direct: true,
            desktop_should_error: false,
        });
        let mut session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion::new(
                -16, -16, 128, 128,
            )?))
            .with_backend(backend)
            .build()?;
        session.set_region_layout_for_test(mock_layout(&monitor, 0, 0, 128, 128));

        let _frame = session.capture()?;

        assert_eq!(*desktop_calls.lock().unwrap(), 0);
        assert_eq!(*region_calls.lock().unwrap(), 1);
        Ok(())
    }
}

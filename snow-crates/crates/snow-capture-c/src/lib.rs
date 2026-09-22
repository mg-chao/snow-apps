#![allow(clippy::missing_safety_doc)]

mod desktop_layout;
mod frame_geometry;
pub use desktop_layout::*;

use snow_capture::exclusions::SnowCaptureExclusions;
use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char};
use std::ptr;
use std::sync::{Arc, mpsc};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use snow_capture::color_effect::ColorCorrection;
use snow_capture::cursor_snapshot::ScreenshotCursorSnapshot;
use snow_capture::frame::{CaptureEvent, CapturePixelFormat, CapturedFrame, Frame};
use snow_capture::{
    CaptureOptions, CaptureRegion, CaptureSession, CaptureStream, CaptureStreamConfig,
    CaptureSystem, CaptureTarget, CaptureWorkload, MonitorId, MonitorLayout, WgcUpdateMode,
    WindowId,
    backend::{AutoBackendPolicy, CaptureBackendKind},
};
use snow_core::cancellation::CancellationToken;
use snow_core::error::RecvTimeoutError;

pub struct SnowCaptureDesktopSessionImpl {
    system: CaptureSystem,
    options: CaptureOptions,
    workers: Vec<MonitorWorker>,
    prepared: bool,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowCapturePixelFormat {
    Rgba8 = 0,
    Bgra8 = 1,
}

pub struct SnowCaptureRegionSessionImpl {
    session: CaptureSession,
    frame: Frame,
}

pub struct SnowCaptureWindowSessionImpl {
    session: CaptureSession,
    frame: Frame,
}

pub struct SnowCaptureMonitorSessionImpl {
    session: CaptureSession,
    entry: MonitorEntry,
    frame: Option<SnapshotFrame>,
}

#[repr(C)]
pub struct SnowCaptureMonitorSessionConfig {
    device_name_utf8: *const c_char,
    capture_retry_count: usize,
    pixel_format: u8,
    reserved: [u8; 31],
    pub exclusions: SnowCaptureExclusions,
}

pub struct SnowCaptureCancellationTokenImpl {
    canceled: CancellationToken,
}

pub struct SnowCaptureScreenshotResultImpl {
    frames: Vec<SnapshotFrame>,
    focused_window: Option<SnapshotWindowFrame>,
}

pub struct SnowCaptureCursorSnapshotImpl {
    snapshot: ScreenshotCursorSnapshot,
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_cursor_snapshot_create() -> *mut SnowCaptureCursorSnapshotImpl {
    match ScreenshotCursorSnapshot::capture() {
        Ok(snapshot) => Box::into_raw(Box::new(SnowCaptureCursorSnapshotImpl { snapshot })),
        Err(error) => {
            set_last_error(error.to_string());
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_cursor_snapshot_destroy(
    snapshot: *mut SnowCaptureCursorSnapshotImpl,
) {
    if !snapshot.is_null() {
        drop(unsafe { Box::from_raw(snapshot) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_screenshot_result_composite_cursor(
    result: *mut SnowCaptureScreenshotResultImpl,
    snapshot: *const SnowCaptureCursorSnapshotImpl,
) -> u8 {
    let (Some(result), Some(snapshot)) = (unsafe { result.as_mut() }, unsafe { snapshot.as_ref() })
    else {
        set_last_error("screenshot result and cursor snapshot must not be null");
        return 0;
    };
    for display in &mut result.frames {
        snapshot.snapshot.composite(
            Arc::make_mut(&mut display.frame),
            display.entry.x,
            display.entry.y,
        );
    }
    if let Some(window) = &mut result.focused_window {
        snapshot
            .snapshot
            .composite(Arc::make_mut(&mut window.frame), window.x, window.y);
    }
    1
}

pub struct SnowCaptureFrameLeaseImpl {
    _frame: Arc<Frame>,
}

#[repr(C)]
pub struct SnowCaptureDesktopSessionConfig {
    capture_retry_count: usize,
    wgc_update_mode: u8,
    capture_backend: u8,
    pixel_format: u8,
    reserved: [u8; 29],
    pub exclusions: SnowCaptureExclusions,
}

#[repr(C)]
pub struct SnowCaptureDesktopSessionState {
    worker_count: usize,
    prepared: u8,
    reserved0: [u8; 3],
    active_capture_access_count: u32,
    retained_resource_bytes: u64,
    backend_kind: *const c_char,
}

#[repr(C)]
pub struct SnowCaptureFrameInfo {
    pub stable_id: *const c_char,
    pub name: *const c_char,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub is_primary: u8,
    pub backend_kind: u8,
    pub pixel_format: u8,
    pub reserved0: u8,
    pub stride_bytes: u32,
    pub rgba_bytes: *const u8,
    pub rgba_len: usize,
}

#[repr(C)]
pub struct SnowCaptureRegionSessionConfig {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub capture_retry_count: usize,
    pub wgc_update_mode: u8,
    pub capture_backend: u8,
    pub pixel_format: u8,
    pub reserved: [u8; 29],
    pub exclusions: SnowCaptureExclusions,
}

#[repr(C)]
pub struct SnowCaptureWindowSessionConfig {
    hwnd: isize,
    capture_retry_count: usize,
    wgc_update_mode: u8,
    capture_backend: u8,
    pixel_format: u8,
    reserved: [u8; 29],
}

#[repr(C)]
pub struct SnowCaptureWindowFrameInfo {
    version: u32,
    struct_size: u32,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    stride_bytes: u32,
    rgba_bytes: *const u8,
    rgba_len: usize,
    backend_kind: u8,
    pixel_format: u8,
    reserved: [u8; 6],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowCaptureScreenshotRequest {
    pub version: u32,
    pub struct_size: u32,
    pub flags: u32,
    pub reserved0: u32,
    pub focused_window: isize,
    pub cancellation_token: *const SnowCaptureCancellationTokenImpl,
    pub reserved: [u8; 32],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SnowCaptureScreenshotRequestHeader {
    version: u32,
    struct_size: u32,
}

#[repr(C)]
pub struct SnowCaptureRegionFrameInfo {
    pub width: u32,
    pub height: u32,
    pub stride_bytes: u32,
    pub is_duplicate: u8,
    pub pixel_format: u8,
    pub reserved0: [u8; 2],
    pub rgba_bytes: *const u8,
    pub rgba_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowCaptureStreamEventKind {
    Timeout = 0,
    Frame = 1,
    FramesDropped = 2,
    ResolutionChanged = 3,
    Paused = 4,
    Resumed = 5,
    Ended = 6,
    Error = 7,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowCaptureStreamConfig {
    pub version: u32,
    pub struct_size: u32,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub target_fps: u32,
    pub min_fps: u32,
    pub buffer_depth: u32,
    pub max_consecutive_errors: u32,
    pub capture_retry_count: usize,
    pub wgc_update_mode: u8,
    pub capture_backend: u8,
    pub pixel_format: u8,
    pub adaptive_fps: u8,
    pub include_cursor: u8,
    pub restore_original_colors: u8,
    pub reserved: [u8; 26],
    pub exclusions: SnowCaptureExclusions,
}

#[repr(C)]
pub struct SnowCaptureStreamEvent {
    pub kind: SnowCaptureStreamEventKind,
    pub frame: *mut SnowCaptureStreamFrameImpl,
    pub dropped_count: u64,
    pub old_width: u32,
    pub old_height: u32,
    pub new_width: u32,
    pub new_height: u32,
    pub reserved: [u8; 32],
}

#[repr(C)]
pub struct SnowCaptureStreamFrameInfo {
    pub version: u32,
    pub struct_size: u32,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub stride_bytes: u32,
    pub is_duplicate: u8,
    pub pixel_format: u8,
    pub backend_kind: u8,
    pub reserved0: [u8; 1],
    pub sequence: u64,
    pub rgba_bytes: *const u8,
    pub rgba_len: usize,
}

#[repr(C)]
pub struct SnowCaptureStreamStats {
    pub frames_captured: u64,
    pub frames_dropped: u64,
    pub errors_recovered: u64,
    pub current_fps: f64,
    pub target_fps: u32,
    pub buffer_fill: u32,
    pub capture_latency_ns: u64,
}

#[derive(Clone)]
struct MonitorEntry {
    id: MonitorId,
    stable_id: CString,
    name: CString,
    x: i32,
    y: i32,
    expected_width: u32,
    expected_height: u32,
    is_primary: bool,
}

struct MonitorWorker {
    entry: MonitorEntry,
    tx: mpsc::Sender<WorkerCommand>,
    join: Option<JoinHandle<()>>,
}

struct SnapshotFrame {
    entry: MonitorEntry,
    frame: Arc<Frame>,
}

struct SnapshotWindowFrame {
    x: i32,
    y: i32,
    frame: Arc<Frame>,
}

enum WorkerCommand {
    Prepare(mpsc::Sender<Result<(), String>>),
    Capture(
        mpsc::Sender<Result<Frame, String>>,
        ColorCorrection,
        bool,
        CancellationToken,
    ),
    ResetToPrepared(mpsc::Sender<Result<(), String>>),
    ActiveCaptureAccessCount(mpsc::Sender<Result<usize, String>>),
    Stop,
}

pub const SCREENSHOT_REQUEST_VERSION: u32 = 1;
const SCREENSHOT_REQUEST_SIZE: u32 = std::mem::size_of::<SnowCaptureScreenshotRequest>() as u32;
const SCREENSHOT_REQUEST_REFRESH_LAYOUT: u32 = 1 << 0;
const SCREENSHOT_REQUEST_RESTORE_ORIGINAL_COLORS: u32 = 1 << 1;
const SCREENSHOT_REQUEST_INCLUDE_CURSOR: u32 = 1 << 2;
pub const WINDOW_FRAME_INFO_VERSION: u32 = 1;
const WINDOW_FRAME_INFO_SIZE: u32 = std::mem::size_of::<SnowCaptureWindowFrameInfo>() as u32;
pub const STREAM_CONFIG_VERSION: u32 = 2;
const STREAM_CONFIG_SIZE: u32 = std::mem::size_of::<SnowCaptureStreamConfig>() as u32;
pub const STREAM_FRAME_INFO_VERSION: u32 = 1;
const STREAM_FRAME_INFO_SIZE: u32 = std::mem::size_of::<SnowCaptureStreamFrameInfo>() as u32;

unsafe fn read_stream_config(
    config: *const SnowCaptureStreamConfig,
) -> Result<SnowCaptureStreamConfig, String> {
    if config.is_null() {
        return Err("stream config is null".to_owned());
    }
    let header = unsafe { &*config.cast::<SnowCaptureScreenshotRequestHeader>() };
    if header.version != STREAM_CONFIG_VERSION {
        return Err(format!(
            "unsupported stream config version: {}",
            header.version
        ));
    }
    if header.struct_size < STREAM_CONFIG_SIZE {
        return Err(format!(
            "stream config is too small: {} < {}",
            header.struct_size, STREAM_CONFIG_SIZE
        ));
    }
    let config = unsafe { *config };
    if config.width == 0 || config.height == 0 {
        return Err("stream region must have non-zero width and height".to_owned());
    }
    if config.buffer_depth == 0 {
        return Err("stream buffer depth must be greater than zero".to_owned());
    }
    if config.max_consecutive_errors == 0 {
        return Err("stream max_consecutive_errors must be greater than zero".to_owned());
    }
    parse_wgc_update_mode(config.wgc_update_mode)?;
    parse_capture_backend(config.capture_backend)?;
    parse_pixel_format(config.pixel_format)?;
    Ok(config)
}

unsafe fn read_screenshot_request(
    request: *const SnowCaptureScreenshotRequest,
) -> Result<SnowCaptureScreenshotRequest, String> {
    if request.is_null() {
        return Err("screenshot request is null".to_owned());
    }
    let header = unsafe { &*request.cast::<SnowCaptureScreenshotRequestHeader>() };
    if header.version != SCREENSHOT_REQUEST_VERSION {
        return Err(format!(
            "unsupported screenshot request version: {}",
            header.version
        ));
    }
    if header.struct_size < SCREENSHOT_REQUEST_SIZE {
        return Err(format!(
            "screenshot request is too small: {} < {}",
            header.struct_size, SCREENSHOT_REQUEST_SIZE
        ));
    }
    let request = unsafe { *request };
    if request.flags
        & !(SCREENSHOT_REQUEST_REFRESH_LAYOUT
            | SCREENSHOT_REQUEST_RESTORE_ORIGINAL_COLORS
            | SCREENSHOT_REQUEST_INCLUDE_CURSOR)
        != 0
    {
        return Err(format!(
            "screenshot request contains unsupported flags: {:#x}",
            request.flags
        ));
    }
    Ok(request)
}

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").expect("empty string is valid C string"));
}

fn sanitize_cstring(value: impl AsRef<str>) -> CString {
    let bytes = value
        .as_ref()
        .as_bytes()
        .iter()
        .copied()
        .filter(|byte| *byte != 0)
        .collect::<Vec<_>>();
    CString::new(bytes).expect("interior NUL bytes were filtered")
}

fn set_last_error(error: impl ToString) {
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = sanitize_cstring(error.to_string());
    });
}

fn clear_last_error() {
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = CString::new("").expect("empty string is valid C string");
    });
}

fn parse_wgc_update_mode(value: u8) -> Result<WgcUpdateMode, String> {
    match value {
        0 => Ok(WgcUpdateMode::Auto),
        1 => Ok(WgcUpdateMode::CompleteOnly),
        2 => Ok(WgcUpdateMode::OrderedIncremental),
        _ => Err(format!("invalid WGC update mode: {value}")),
    }
}

fn parse_capture_backend(value: u8) -> Result<CaptureBackendKind, String> {
    match value {
        0 => Ok(CaptureBackendKind::Auto),
        1 => Ok(CaptureBackendKind::DxgiDuplication),
        2 => Ok(CaptureBackendKind::WindowsGraphicsCapture),
        3 => Ok(CaptureBackendKind::Gdi),
        4 => Ok(CaptureBackendKind::ScreenCaptureKit),
        _ => Err(format!("invalid capture backend: {value}")),
    }
}

fn parse_pixel_format(value: u8) -> Result<CapturePixelFormat, String> {
    match value {
        0 => Ok(CapturePixelFormat::Rgba8),
        1 => Ok(CapturePixelFormat::Bgra8),
        _ => Err(format!("invalid capture pixel format: {value}")),
    }
}

fn pixel_format_value(format: CapturePixelFormat) -> u8 {
    match format {
        CapturePixelFormat::Rgba8 => 0,
        CapturePixelFormat::Bgra8 => 1,
    }
}

fn capture_backend_value(kind: CaptureBackendKind) -> u8 {
    match kind {
        CaptureBackendKind::Auto => 0,
        CaptureBackendKind::DxgiDuplication => 1,
        CaptureBackendKind::WindowsGraphicsCapture => 2,
        CaptureBackendKind::Gdi => 3,
        CaptureBackendKind::ScreenCaptureKit => 4,
    }
}

// The struct update stays load-bearing when snow-capture is built with its
// optional `stage-timing` feature, which adds a field this literal omits.
#[allow(clippy::needless_update)]
fn platform_tuning(
    options: snow_capture::tuning::windows::WindowsCaptureOptions,
) -> snow_capture::BackendTuning {
    #[cfg(target_os = "macos")]
    {
        let _ = options;
        snow_capture::BackendTuning::Default
    }
    #[cfg(not(target_os = "macos"))]
    {
        snow_capture::BackendTuning::Windows(options)
    }
}

#[allow(clippy::needless_update)] // Optional stage-timing adds another field.
fn default_options(
    config: *const SnowCaptureDesktopSessionConfig,
) -> Result<(CaptureOptions, CaptureBackendKind), String> {
    let (
        capture_retry_count,
        wgc_update_mode,
        capture_backend,
        output_pixel_format,
        excluded_windows,
        excluded_processes,
    ) = if config.is_null() {
        (
            1,
            WgcUpdateMode::Auto,
            CaptureBackendKind::Auto,
            CapturePixelFormat::Rgba8,
            Arc::from(Vec::<u32>::new()),
            Arc::from(Vec::<i32>::new()),
        )
    } else {
        let config = unsafe { &*config };
        let (excluded_windows, excluded_processes) = unsafe { config.exclusions.to_owned()? };
        (
            config.capture_retry_count.max(1),
            parse_wgc_update_mode(config.wgc_update_mode)?,
            parse_capture_backend(config.capture_backend)?,
            parse_pixel_format(config.pixel_format)?,
            excluded_windows,
            excluded_processes,
        )
    };

    Ok((
        CaptureOptions {
            backend_tuning: platform_tuning(snow_capture::tuning::windows::WindowsCaptureOptions {
                gpu_hdr_conversion: true,
                hdr_tonemap_lut: true,
                wgc_update_mode,
                ..Default::default()
            }),
            capture_retry_count,
            workload: CaptureWorkload::Snapshot,
            output_pixel_format,
            excluded_windows,
            excluded_processes,
            // Keeps the literal valid whether or not snow-capture was built
            // with its optional `stage-timing` instrumentation feature.
            ..Default::default()
        },
        capture_backend,
    ))
}

fn desktop_backend_policy(preferred: CaptureBackendKind) -> Option<AutoBackendPolicy> {
    if preferred == CaptureBackendKind::Auto {
        return None;
    }
    let mut policy = AutoBackendPolicy::default();
    policy.priority.retain(|kind| *kind != preferred);
    policy.priority.insert(0, preferred);
    Some(policy)
}

#[allow(clippy::needless_update)] // needed when snow-capture enables its `stage-timing` feature
fn snapshot_options(
    capture_retry_count: usize,
    wgc_update_mode: u8,
    output_pixel_format: CapturePixelFormat,
) -> Result<CaptureOptions, String> {
    Ok(CaptureOptions {
        backend_tuning: platform_tuning(snow_capture::tuning::windows::WindowsCaptureOptions {
            gpu_hdr_conversion: true,
            hdr_tonemap_lut: true,
            wgc_update_mode: parse_wgc_update_mode(wgc_update_mode)?,
            ..Default::default()
        }),
        capture_retry_count: capture_retry_count.max(1),
        workload: CaptureWorkload::Snapshot,
        output_pixel_format,
        // Keeps the literal valid whether or not snow-capture was built
        // with its optional `stage-timing` instrumentation feature.
        ..Default::default()
    })
}

unsafe fn apply_exclusions(
    mut options: CaptureOptions,
    exclusions: SnowCaptureExclusions,
) -> Result<CaptureOptions, String> {
    let (windows, processes) = unsafe { exclusions.to_owned() }?;
    options.excluded_windows = windows;
    options.excluded_processes = processes;
    Ok(options)
}

fn build_monitor_entries(system: &CaptureSystem) -> Result<Vec<MonitorEntry>, String> {
    let MonitorLayout { monitors, .. } = system.monitor_layout().map_err(|err| err.to_string())?;
    Ok(monitors
        .into_iter()
        .map(|geometry| {
            let stable_id = geometry.monitor.stable_id();
            let name = geometry.monitor.name().to_owned();
            MonitorEntry {
                id: geometry.monitor.clone(),
                stable_id: sanitize_cstring(stable_id),
                name: sanitize_cstring(name),
                x: geometry.x,
                y: geometry.y,
                expected_width: geometry.width,
                expected_height: geometry.height,
                is_primary: geometry.monitor.is_primary(),
            }
        })
        .collect())
}

impl MonitorWorker {
    fn start(
        system: CaptureSystem,
        options: CaptureOptions,
        entry: MonitorEntry,
    ) -> Result<Self, String> {
        let (tx, rx) = mpsc::channel::<WorkerCommand>();
        let worker_entry = entry.clone();
        let join = thread::Builder::new()
            .name("snow-capture-monitor".to_owned())
            .spawn(move || {
                let mut session = system
                    .open_session(CaptureTarget::Monitor(worker_entry.id), options)
                    .map_err(|err| err.to_string());

                while let Ok(command) = rx.recv() {
                    match command {
                        WorkerCommand::Prepare(reply) => {
                            let result = match session.as_mut() {
                                Ok(capture_session) => capture_session
                                    .prewarm_environment()
                                    .map(|_| ())
                                    .map_err(|err| err.to_string()),
                                Err(error) => Err(error.clone()),
                            };
                            let _ = reply.send(result);
                        }
                        WorkerCommand::Capture(reply, correction, include_cursor, cancellation) => {
                            let result = match session.as_mut() {
                                Ok(session) => {
                                    session.set_windows_color_correction(correction);
                                    match session.capture_snapshot(include_cursor, cancellation) {
                                        Ok(mut frame)
                                            if session.active_capture_access_count() == 0 =>
                                        {
                                            if include_cursor {
                                                frame.composite_attached_cursor();
                                            }
                                            Ok(frame)
                                        }
                                        Ok(_) => Err(
                                            "capture access remained active after one-shot monitor capture"
                                                .to_owned(),
                                        ),
                                        Err(error) => Err(error.to_string()),
                                    }
                                }
                                Err(error) => Err(error.clone()),
                            };
                            let _ = reply.send(result);
                        }
                        WorkerCommand::ResetToPrepared(reply) => {
                            let result = match session.as_mut() {
                                Ok(capture_session) => capture_session
                                    .reset_to_prepared()
                                    .map_err(|err| err.to_string()),
                                Err(error) => Err(error.clone()),
                            };
                            let _ = reply.send(result);
                        }
                        WorkerCommand::ActiveCaptureAccessCount(reply) => {
                            let result = match session.as_ref() {
                                Ok(capture_session) => {
                                    Ok(capture_session.active_capture_access_count())
                                }
                                Err(error) => Err(error.clone()),
                            };
                            let _ = reply.send(result);
                        }
                        WorkerCommand::Stop => break,
                    }
                }
            })
            .map_err(|err| format!("failed to spawn capture monitor worker: {err}"))?;

        Ok(Self {
            entry,
            tx,
            join: Some(join),
        })
    }

    fn stop(mut self) {
        let _ = self.tx.send(WorkerCommand::Stop);
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }

    fn request_prepare(&self) -> Result<mpsc::Receiver<Result<(), String>>, String> {
        let (tx, rx) = mpsc::channel();
        self.tx
            .send(WorkerCommand::Prepare(tx))
            .map_err(|_| "capture worker is not running".to_owned())?;
        Ok(rx)
    }

    fn request_capture(
        &self,
        correction: ColorCorrection,
        include_cursor: bool,
        cancellation: CancellationToken,
    ) -> Result<mpsc::Receiver<Result<Frame, String>>, String> {
        let (tx, rx) = mpsc::channel();
        self.tx
            .send(WorkerCommand::Capture(
                tx,
                correction,
                include_cursor,
                cancellation,
            ))
            .map_err(|_| "capture worker is not running".to_owned())?;
        Ok(rx)
    }

    fn request_reset_to_prepared(&self) -> Result<mpsc::Receiver<Result<(), String>>, String> {
        let (tx, rx) = mpsc::channel();
        self.tx
            .send(WorkerCommand::ResetToPrepared(tx))
            .map_err(|_| "capture worker is not running".to_owned())?;
        Ok(rx)
    }

    fn request_active_capture_access_count(
        &self,
    ) -> Result<mpsc::Receiver<Result<usize, String>>, String> {
        let (tx, rx) = mpsc::channel();
        self.tx
            .send(WorkerCommand::ActiveCaptureAccessCount(tx))
            .map_err(|_| "capture worker is not running".to_owned())?;
        Ok(rx)
    }
}

impl Drop for SnowCaptureDesktopSessionImpl {
    fn drop(&mut self) {
        for worker in std::mem::take(&mut self.workers) {
            worker.stop();
        }
    }
}

fn session_mut<'a>(
    session: *mut SnowCaptureDesktopSessionImpl,
) -> Option<&'a mut SnowCaptureDesktopSessionImpl> {
    if session.is_null() {
        set_last_error("desktop session is null");
        None
    } else {
        Some(unsafe { &mut *session })
    }
}

fn reconcile_workers(
    session: &mut SnowCaptureDesktopSessionImpl,
    entries: Vec<MonitorEntry>,
) -> Result<(), String> {
    let old_workers = std::mem::take(&mut session.workers);
    let was_prepared = session.prepared;
    let mut unmatched = old_workers;
    let mut retained = Vec::with_capacity(entries.len());
    let mut retained_updates = Vec::with_capacity(entries.len());
    let mut created = Vec::new();

    for entry in entries {
        if let Some(index) = unmatched.iter().position(|worker| {
            worker.entry.id.stable_id() == entry.id.stable_id()
                && worker.entry.x == entry.x
                && worker.entry.y == entry.y
                && worker.entry.expected_width == entry.expected_width
                && worker.entry.expected_height == entry.expected_height
        }) {
            retained.push(unmatched.swap_remove(index));
            retained_updates.push(entry);
        } else {
            match MonitorWorker::start(session.system.clone(), session.options.clone(), entry) {
                Ok(worker) => created.push(worker),
                Err(error) => {
                    for worker in created.drain(..) {
                        worker.stop();
                    }
                    retained.extend(unmatched);
                    session.workers = retained;
                    return Err(error);
                }
            }
        }
    }

    if was_prepared {
        let receivers = match created
            .iter()
            .map(MonitorWorker::request_prepare)
            .collect::<Result<Vec<_>, _>>()
        {
            Ok(receivers) => receivers,
            Err(error) => {
                for worker in created.drain(..) {
                    worker.stop();
                }
                retained.extend(unmatched);
                session.workers = retained;
                return Err(error);
            }
        };
        for receiver in receivers {
            match receiver.recv() {
                Ok(Ok(())) => {}
                Ok(Err(error)) => {
                    for worker in created.drain(..) {
                        worker.stop();
                    }
                    retained.extend(unmatched);
                    session.workers = retained;
                    return Err(error);
                }
                Err(_) => {
                    for worker in created.drain(..) {
                        worker.stop();
                    }
                    retained.extend(unmatched);
                    session.workers = retained;
                    return Err("capture worker stopped before prepare completed".to_owned());
                }
            }
        }
    }

    for worker in unmatched {
        worker.stop();
    }
    for (worker, entry) in retained.iter_mut().zip(retained_updates) {
        worker.entry = entry;
    }
    retained.extend(created);
    session.workers = retained;
    session.prepared = was_prepared;
    Ok(())
}

fn rebuild_workers(session: &mut SnowCaptureDesktopSessionImpl) -> Result<(), String> {
    session
        .system
        .refresh_display_configuration()
        .map_err(|err| err.to_string())?;
    let entries = build_monitor_entries(&session.system)?;
    reconcile_workers(session, entries)
}

#[cfg_attr(target_os = "macos", allow(dead_code))]
fn same_monitor_layout(left: &[MonitorEntry], right: &[MonitorEntry]) -> bool {
    left.len() == right.len()
        && left.iter().all(|candidate| {
            right.iter().any(|existing| {
                candidate.id == existing.id
                    && candidate.x == existing.x
                    && candidate.y == existing.y
                    && candidate.expected_width == existing.expected_width
                    && candidate.expected_height == existing.expected_height
                    && candidate.is_primary == existing.is_primary
                    && candidate.id.desktop_geometry() == existing.id.desktop_geometry()
            })
        })
}

fn capture_all_frames(
    session: &mut SnowCaptureDesktopSessionImpl,
    correction: ColorCorrection,
    include_cursor: bool,
    cancellation: CancellationToken,
) -> Result<Vec<SnapshotFrame>, String> {
    let mut receivers = Vec::with_capacity(session.workers.len());
    let mut first_error = None;
    for worker in &session.workers {
        match worker.request_capture(correction, include_cursor, cancellation.clone()) {
            Ok(receiver) => receivers.push((worker.entry.clone(), receiver)),
            Err(error) => {
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
        }
    }

    let mut frames = Vec::with_capacity(receivers.len());
    for (entry, receiver) in receivers {
        match receiver.recv() {
            Ok(Ok(frame)) => {
                frames.push(SnapshotFrame {
                    entry,
                    frame: Arc::new(frame),
                });
            }
            Ok(Err(error)) => {
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
            Err(_) => {
                if first_error.is_none() {
                    first_error =
                        Some("capture worker stopped before capture completed".to_owned());
                }
            }
        }
    }

    match first_error {
        Some(error) => Err(error),
        None => Ok(frames),
    }
}

fn capture_all_frames_with_layout_retry(
    session: &mut SnowCaptureDesktopSessionImpl,
    correction: ColorCorrection,
    include_cursor: bool,
    cancellation: CancellationToken,
) -> Result<Vec<SnapshotFrame>, String> {
    // ScreenCaptureKit refreshes its plan per snapshot. Never retry a permission
    // failure or cancellation by re-enumerating the desktop.
    #[cfg(target_os = "macos")]
    {
        capture_all_frames(session, correction, include_cursor, cancellation)
    }
    #[cfg(not(target_os = "macos"))]
    match capture_all_frames(session, correction, include_cursor, cancellation.clone()) {
        Ok(frames) => Ok(frames),
        Err(first_error) => {
            if let Err(refresh_error) = session.system.refresh_display_configuration() {
                return Err(format!(
                    "{first_error}; layout refresh failed: {refresh_error}"
                ));
            }

            let entries = build_monitor_entries(&session.system).map_err(|refresh_error| {
                format!("{first_error}; layout refresh failed: {refresh_error}")
            })?;
            let current_entries = session
                .workers
                .iter()
                .map(|worker| worker.entry.clone())
                .collect::<Vec<_>>();
            if same_monitor_layout(&entries, &current_entries) {
                return Err(first_error);
            }
            if let Err(refresh_error) = reconcile_workers(session, entries) {
                return Err(format!(
                    "{first_error}; layout refresh failed: {refresh_error}"
                ));
            }
            capture_all_frames(session, correction, include_cursor, cancellation.clone()).map_err(
                |retry_error| {
                    format!("{first_error}; retry after layout refresh failed: {retry_error}")
                },
            )
        }
    }
}

fn active_capture_access_count(session: &SnowCaptureDesktopSessionImpl) -> Result<usize, String> {
    let mut receivers = Vec::with_capacity(session.workers.len());
    let mut first_error = None;
    for worker in &session.workers {
        match worker.request_active_capture_access_count() {
            Ok(receiver) => receivers.push(receiver),
            Err(error) => {
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
        }
    }

    let mut total = 0usize;
    for receiver in receivers {
        match receiver.recv() {
            Ok(Ok(count)) => total = total.saturating_add(count),
            Ok(Err(error)) => {
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
            Err(_) => {
                if first_error.is_none() {
                    first_error =
                        Some("capture worker stopped before lifecycle state completed".to_owned());
                }
            }
        }
    }

    match first_error {
        Some(error) => Err(error),
        None => Ok(total),
    }
}

fn reset_workers_to_prepared(session: &mut SnowCaptureDesktopSessionImpl) -> Result<(), String> {
    let receivers = session
        .workers
        .iter()
        .map(MonitorWorker::request_reset_to_prepared)
        .collect::<Result<Vec<_>, _>>()?;
    for receiver in receivers {
        match receiver.recv() {
            Ok(Ok(())) => {}
            Ok(Err(error)) => return Err(error),
            Err(_) => {
                return Err("capture worker stopped before prepared reset completed".to_owned());
            }
        }
    }
    session.prepared = true;
    Ok(())
}

fn select_monitor_entry(
    entries: Vec<MonitorEntry>,
    device_name: &str,
) -> Result<MonitorEntry, String> {
    entries
        .into_iter()
        .find(|entry| entry.id.name() == device_name)
        .ok_or_else(|| format!("capture monitor is unavailable: {device_name}"))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_monitor_session_create(
    config: *const SnowCaptureMonitorSessionConfig,
) -> *mut SnowCaptureMonitorSessionImpl {
    let result = (|| {
        let config = unsafe { config.as_ref() }.ok_or("monitor session config is null")?;
        if config.device_name_utf8.is_null() {
            return Err("monitor device name is null".to_owned());
        }
        let name = unsafe { CStr::from_ptr(config.device_name_utf8) }
            .to_str()
            .map_err(|error| error.to_string())?;
        if name.is_empty() {
            return Err("monitor device name is empty".to_owned());
        }
        let options = snapshot_options(
            config.capture_retry_count,
            0,
            parse_pixel_format(config.pixel_format)?,
        )?;
        let options = unsafe { apply_exclusions(options, config.exclusions)? };
        let system = CaptureSystem::builder()
            .build()
            .map_err(|error| error.to_string())?;
        let entry = select_monitor_entry(build_monitor_entries(&system)?, name)?;
        let session = system
            .open_session(CaptureTarget::Monitor(entry.id.clone()), options)
            .map_err(|error| error.to_string())?;
        Ok(SnowCaptureMonitorSessionImpl {
            session,
            entry,
            frame: None,
        })
    })();
    match result {
        Ok(session) => {
            clear_last_error();
            Box::into_raw(Box::new(session))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_monitor_session_destroy(
    session: *mut SnowCaptureMonitorSessionImpl,
) {
    if !session.is_null() {
        drop(unsafe { Box::from_raw(session) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_monitor_session_capture(
    session: *mut SnowCaptureMonitorSessionImpl,
    out_info: *mut SnowCaptureFrameInfo,
) -> u8 {
    let result = (|| {
        let session = unsafe { session.as_mut() }.ok_or("monitor session is null")?;
        if out_info.is_null() {
            return Err("monitor frame out_info is null".to_owned());
        }
        session.frame = None;
        let frame = session
            .session
            .capture_once()
            .map_err(|error| error.to_string())?;
        if session.session.active_capture_access_count() != 0 {
            return Err("capture access remained active after one-shot monitor capture".to_owned());
        }
        let target = session
            .session
            .target_info_for_backend(frame.metadata().backend_kind())
            .map_err(|error| error.to_string())?;
        let mut entry = session.entry.clone();
        entry.x = target.origin_x;
        entry.y = target.origin_y;
        session.frame = Some(SnapshotFrame {
            entry,
            frame: Arc::new(frame),
        });
        write_snapshot_frame_info(session.frame.as_ref().unwrap(), out_info)
    })();
    match result {
        Ok(()) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_monitor_session_frame_retain(
    session: *const SnowCaptureMonitorSessionImpl,
) -> *mut SnowCaptureFrameLeaseImpl {
    let frame = unsafe { session.as_ref() }.and_then(|session| session.frame.as_ref());
    match frame {
        Some(frame) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowCaptureFrameLeaseImpl {
                _frame: Arc::clone(&frame.frame),
            }))
        }
        None => {
            set_last_error("monitor session has no captured frame");
            ptr::null_mut()
        }
    }
}

fn backend_kind_ptr(session: &SnowCaptureDesktopSessionImpl) -> *const c_char {
    match session.system.backend_kind().as_str() {
        "auto" => c"auto".as_ptr(),
        "dxgi" => c"dxgi".as_ptr(),
        "wgc" => c"wgc".as_ptr(),
        "gdi" => c"gdi".as_ptr(),
        "sck" => c"sck".as_ptr(),
        _ => c"unknown".as_ptr(),
    }
}

fn native_window_id(value: isize) -> Result<WindowId, String> {
    #[cfg(target_os = "macos")]
    {
        u32::try_from(value)
            .ok()
            .filter(|id| *id != 0)
            .map(WindowId::from_macos_id)
            .ok_or_else(|| "invalid macOS window ID".into())
    }
    #[cfg(not(target_os = "macos"))]
    {
        Ok(WindowId::from_windows_handle(value))
    }
}

fn capture_window_snapshot(
    hwnd: isize,
    options: CaptureOptions,
    include_cursor: bool,
    cancellation: CancellationToken,
) -> Result<SnapshotWindowFrame, String> {
    let system = CaptureSystem::builder()
        .with_backend_kind(CaptureBackendKind::Auto)
        .build()
        .map_err(|error| error.to_string())?;
    let mut session = system
        .open_session(CaptureTarget::Window(native_window_id(hwnd)?), options)
        .map_err(|error| error.to_string())?;
    let mut frame = session
        .capture_snapshot(include_cursor, cancellation)
        .map_err(|error| error.to_string())?;
    if session.active_capture_access_count() != 0 {
        let _ = session.reset_to_prepared();
        return Err("capture access remained active after focused-window capture".to_owned());
    }
    if include_cursor {
        frame.composite_attached_cursor();
    }
    let (x, y) = if let Some(transform) = frame.metadata().capture_transform() {
        (
            transform.source.x.round() as i32,
            transform.source.y.round() as i32,
        )
    } else {
        let target = session
            .target_info_for_backend(frame.metadata().backend_kind())
            .map_err(|error| error.to_string())?;
        (target.origin_x, target.origin_y)
    };
    Ok(SnapshotWindowFrame {
        x,
        y,
        frame: Arc::new(frame),
    })
}

fn write_snapshot_frame_info(
    frame: &SnapshotFrame,
    out_info: *mut SnowCaptureFrameInfo,
) -> Result<(), String> {
    let rgba = frame.frame.as_bytes();
    let stride_bytes = frame
        .frame
        .width()
        .checked_mul(4)
        .ok_or_else(|| "frame stride overflow".to_owned())?;
    let required_len = usize::try_from(stride_bytes)
        .ok()
        .and_then(|stride| stride.checked_mul(frame.frame.height() as usize))
        .ok_or_else(|| "frame length overflow".to_owned())?;
    if rgba.len() < required_len {
        return Err("frame buffer is smaller than the reported dimensions".to_owned());
    }

    unsafe {
        *out_info = SnowCaptureFrameInfo {
            stable_id: frame.entry.stable_id.as_ptr(),
            name: frame.entry.name.as_ptr(),
            x: frame.entry.x,
            y: frame.entry.y,
            width: frame.frame.width(),
            height: frame.frame.height(),
            is_primary: u8::from(frame.entry.is_primary),
            backend_kind: capture_backend_value(frame.frame.metadata().backend_kind()),
            pixel_format: pixel_format_value(frame.frame.pixel_format()),
            reserved0: 0,
            stride_bytes,
            rgba_bytes: rgba.as_ptr(),
            rgba_len: rgba.len(),
        };
    }
    Ok(())
}

fn write_window_frame_info(
    frame: &SnapshotWindowFrame,
    out_info: *mut SnowCaptureWindowFrameInfo,
) -> Result<(), String> {
    let rgba = frame.frame.as_bytes();
    let stride_bytes = frame
        .frame
        .width()
        .checked_mul(4)
        .ok_or_else(|| "window frame stride overflow".to_owned())?;
    let required_len = usize::try_from(stride_bytes)
        .ok()
        .and_then(|stride| stride.checked_mul(frame.frame.height() as usize))
        .ok_or_else(|| "window frame length overflow".to_owned())?;
    if rgba.len() < required_len {
        return Err("window frame buffer is smaller than its dimensions".to_owned());
    }
    unsafe {
        *out_info = SnowCaptureWindowFrameInfo {
            version: WINDOW_FRAME_INFO_VERSION,
            struct_size: WINDOW_FRAME_INFO_SIZE,
            x: frame.x,
            y: frame.y,
            width: frame.frame.width(),
            height: frame.frame.height(),
            stride_bytes,
            rgba_bytes: rgba.as_ptr(),
            rgba_len: rgba.len(),
            backend_kind: capture_backend_value(frame.frame.metadata().backend_kind()),
            pixel_format: pixel_format_value(frame.frame.pixel_format()),
            reserved: [0; 6],
        };
    }
    Ok(())
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_desktop_session_create(
    config: *const SnowCaptureDesktopSessionConfig,
) -> *mut SnowCaptureDesktopSessionImpl {
    let (options, capture_backend) = match default_options(config) {
        Ok(parsed) => parsed,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    // Desktop screenshots treat the configured API as a preference, with per-target fallback.
    let mut builder = CaptureSystem::builder();
    if let Some(policy) = desktop_backend_policy(capture_backend) {
        builder = builder.with_auto_backend_policy(policy);
    }
    match builder.build() {
        Ok(system) => {
            let mut session = SnowCaptureDesktopSessionImpl {
                system,
                options,
                workers: Vec::new(),
                prepared: false,
            };
            if let Err(error) = rebuild_workers(&mut session) {
                set_last_error(error);
            } else {
                clear_last_error();
            }
            Box::into_raw(Box::new(session))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_desktop_session_destroy(
    session: *mut SnowCaptureDesktopSessionImpl,
) {
    if !session.is_null() {
        drop(unsafe { Box::from_raw(session) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_desktop_session_prepare(
    session: *mut SnowCaptureDesktopSessionImpl,
) -> u8 {
    let Some(session) = session_mut(session) else {
        return 0;
    };

    let receivers = match session
        .workers
        .iter()
        .map(MonitorWorker::request_prepare)
        .collect::<Result<Vec<_>, _>>()
    {
        Ok(receivers) => receivers,
        Err(error) => {
            set_last_error(error);
            return 0;
        }
    };

    let mut first_error = None;
    for receiver in receivers {
        match receiver.recv() {
            Ok(Ok(())) => {}
            Ok(Err(error)) => {
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
            Err(_) => {
                if first_error.is_none() {
                    first_error =
                        Some("capture worker stopped before prepare completed".to_owned());
                }
            }
        }
    }

    if let Some(error) = first_error {
        set_last_error(error);
        return 0;
    }

    clear_last_error();
    session.prepared = true;
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_desktop_session_state(
    session: *mut SnowCaptureDesktopSessionImpl,
    out_state: *mut SnowCaptureDesktopSessionState,
) -> u8 {
    if out_state.is_null() {
        set_last_error("out_state is null");
        return 0;
    }
    let Some(session) = session_mut(session) else {
        return 0;
    };

    let active_count = match active_capture_access_count(session) {
        Ok(count) => count,
        Err(error) => {
            set_last_error(error);
            return 0;
        }
    };
    let active_count = match u32::try_from(active_count) {
        Ok(count) => count,
        Err(_) => {
            set_last_error("active capture access count overflow");
            return 0;
        }
    };

    unsafe {
        *out_state = SnowCaptureDesktopSessionState {
            worker_count: session.workers.len(),
            prepared: u8::from(session.prepared),
            reserved0: [0; 3],
            active_capture_access_count: active_count,
            retained_resource_bytes: 0,
            backend_kind: backend_kind_ptr(session),
        };
    }
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_desktop_session_refresh_layout(
    session: *mut SnowCaptureDesktopSessionImpl,
) -> u8 {
    let Some(session) = session_mut(session) else {
        return 0;
    };

    match rebuild_workers(session) {
        Ok(()) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_desktop_session_reset_to_prepared(
    session: *mut SnowCaptureDesktopSessionImpl,
) -> u8 {
    let Some(session) = session_mut(session) else {
        return 0;
    };

    match reset_workers_to_prepared(session) {
        Ok(()) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_cancellation_token_create() -> *mut SnowCaptureCancellationTokenImpl
{
    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureCancellationTokenImpl {
        canceled: CancellationToken::default(),
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_cancellation_token_cancel(
    token: *mut SnowCaptureCancellationTokenImpl,
) {
    if !token.is_null() {
        unsafe { &*token }.canceled.cancel();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_cancellation_token_destroy(
    token: *mut SnowCaptureCancellationTokenImpl,
) {
    if !token.is_null() {
        drop(unsafe { Box::from_raw(token) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_desktop_session_capture(
    session: *mut SnowCaptureDesktopSessionImpl,
    request: *const SnowCaptureScreenshotRequest,
) -> *mut SnowCaptureScreenshotResultImpl {
    let Some(session) = session_mut(session) else {
        return ptr::null_mut();
    };
    let request = match unsafe { read_screenshot_request(request) } {
        Ok(request) => request,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };

    let canceled = if request.cancellation_token.is_null() {
        None
    } else {
        Some(unsafe { &*request.cancellation_token }.canceled.clone())
    };
    let is_canceled = || canceled.as_ref().is_some_and(|state| state.is_canceled());
    if is_canceled() {
        set_last_error("screenshot capture canceled");
        return ptr::null_mut();
    }

    if session.workers.is_empty() {
        if let Err(error) = rebuild_workers(session) {
            set_last_error(error);
            return ptr::null_mut();
        }
        if session.workers.is_empty() {
            set_last_error("no active displays are available");
            return ptr::null_mut();
        }
    }

    if !session.prepared && snow_capture_desktop_session_prepare(session as *mut _) == 0 {
        return ptr::null_mut();
    }

    if request.flags & SCREENSHOT_REQUEST_REFRESH_LAYOUT != 0
        && let Err(error) = rebuild_workers(session)
    {
        set_last_error(error);
        return ptr::null_mut();
    }

    let correction = if request.flags & SCREENSHOT_REQUEST_RESTORE_ORIGINAL_COLORS != 0 {
        ColorCorrection::snapshot_current()
    } else {
        ColorCorrection::Disabled
    };
    let include_cursor = request.flags & SCREENSHOT_REQUEST_INCLUDE_CURSOR != 0;
    let focused_window_worker = if request.focused_window != 0 {
        let hwnd = request.focused_window;
        let options = CaptureOptions {
            backend_tuning: platform_tuning(snow_capture::tuning::windows::WindowsCaptureOptions {
                color_correction: correction,
                ..session.options.backend_tuning.windows_or_default()
            }),
            ..session.options.clone()
        };
        let canceled = canceled.clone();
        match thread::Builder::new()
            .name("snow-capture-window-once".to_owned())
            .spawn(move || {
                if canceled.as_ref().is_some_and(|state| state.is_canceled()) {
                    return Err("screenshot capture canceled".to_owned());
                }
                let result = capture_window_snapshot(
                    hwnd,
                    options,
                    include_cursor,
                    canceled.clone().unwrap_or_default(),
                );
                if canceled.as_ref().is_some_and(|state| state.is_canceled()) {
                    return Err("screenshot capture canceled".to_owned());
                }
                result
            }) {
            Ok(worker) => Some(worker),
            Err(error) => {
                set_last_error(format!("failed to start focused-window capture: {error}"));
                return ptr::null_mut();
            }
        }
    } else {
        None
    };

    let frames_result = capture_all_frames_with_layout_retry(
        session,
        correction,
        include_cursor,
        canceled.clone().unwrap_or_default(),
    );
    let focused_window_result = focused_window_worker.map(|worker| {
        worker
            .join()
            .map_err(|_| "focused-window capture worker panicked".to_owned())
            .and_then(|result| result)
    });

    if is_canceled() {
        set_last_error("screenshot capture canceled");
        return ptr::null_mut();
    }
    let frames = match frames_result {
        Ok(frames) => frames,
        Err(error) => {
            let combined = match focused_window_result {
                Some(Err(window_error)) => format!(
                    "desktop capture failed: {error}; focused-window capture failed: {window_error}"
                ),
                _ => error,
            };
            set_last_error(combined);
            return ptr::null_mut();
        }
    };
    let focused_window = match focused_window_result {
        Some(Ok(frame)) => Some(frame),
        Some(Err(error)) => {
            set_last_error(format!("focused-window capture failed: {error}"));
            return ptr::null_mut();
        }
        None => None,
    };

    session.prepared = true;
    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureScreenshotResultImpl {
        frames,
        focused_window,
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_region_session_create(
    config: *const SnowCaptureRegionSessionConfig,
) -> *mut SnowCaptureRegionSessionImpl {
    if config.is_null() {
        set_last_error("region session config is null");
        return ptr::null_mut();
    }
    let config = unsafe { &*config };
    let region = match CaptureRegion::new(config.x, config.y, config.width, config.height) {
        Ok(region) => region,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let output_pixel_format = match parse_pixel_format(config.pixel_format) {
        Ok(format) => format,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let options = match snapshot_options(
        config.capture_retry_count,
        config.wgc_update_mode,
        output_pixel_format,
    ) {
        Ok(options) => options,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let options = match unsafe { apply_exclusions(options, config.exclusions) } {
        Ok(options) => options,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let capture_backend = match parse_capture_backend(config.capture_backend) {
        Ok(backend) => backend,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let system = match CaptureSystem::builder()
        .with_backend_kind(capture_backend)
        .build()
    {
        Ok(system) => system,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let session = match system.open_session(CaptureTarget::Region(region), options) {
        Ok(session) => session,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureRegionSessionImpl {
        session,
        frame: Frame::empty(),
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_region_session_destroy(
    session: *mut SnowCaptureRegionSessionImpl,
) {
    if !session.is_null() {
        drop(unsafe { Box::from_raw(session) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_region_session_prepare(
    session: *mut SnowCaptureRegionSessionImpl,
) -> u8 {
    if session.is_null() {
        set_last_error("region session is null");
        return 0;
    }
    let session = unsafe { &mut *session };
    match session.session.prepare_target() {
        Ok(_) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_region_session_capture(
    session: *mut SnowCaptureRegionSessionImpl,
    out_info: *mut SnowCaptureRegionFrameInfo,
) -> u8 {
    if out_info.is_null() {
        set_last_error("region frame out_info is null");
        return 0;
    }
    if session.is_null() {
        set_last_error("region session is null");
        return 0;
    }
    let session = unsafe { &mut *session };
    if let Err(error) = session.session.capture_into(&mut session.frame) {
        set_last_error(error);
        return 0;
    }
    let stride_bytes = match session.frame.width().checked_mul(4) {
        Some(stride) => stride,
        None => {
            set_last_error("region frame stride overflow");
            return 0;
        }
    };
    let rgba = session.frame.as_bytes();
    unsafe {
        *out_info = SnowCaptureRegionFrameInfo {
            width: session.frame.width(),
            height: session.frame.height(),
            stride_bytes,
            is_duplicate: u8::from(session.frame.metadata().is_duplicate()),
            pixel_format: pixel_format_value(session.frame.pixel_format()),
            reserved0: [0; 2],
            rgba_bytes: rgba.as_ptr(),
            rgba_len: rgba.len(),
        };
    }
    clear_last_error();
    1
}

fn empty_stream_event() -> SnowCaptureStreamEvent {
    SnowCaptureStreamEvent {
        kind: SnowCaptureStreamEventKind::Timeout,
        frame: ptr::null_mut(),
        dropped_count: 0,
        old_width: 0,
        old_height: 0,
        new_width: 0,
        new_height: 0,
        reserved: [0; 32],
    }
}

fn write_stream_frame_info(
    frame: &SnowCaptureStreamFrameImpl,
    out_info: *mut SnowCaptureStreamFrameInfo,
) -> Result<(), String> {
    let bytes = frame.frame.as_bytes();
    let width = frame.frame.width();
    let height = frame.frame.height();
    let stride_bytes = width
        .checked_mul(4)
        .ok_or_else(|| "stream frame stride overflow".to_owned())?;
    let required_len = usize::try_from(stride_bytes)
        .ok()
        .and_then(|stride| stride.checked_mul(height as usize))
        .ok_or_else(|| "stream frame length overflow".to_owned())?;
    if bytes.len() < required_len {
        return Err("stream frame buffer is smaller than its dimensions".to_owned());
    }
    unsafe {
        *out_info = SnowCaptureStreamFrameInfo {
            version: STREAM_FRAME_INFO_VERSION,
            struct_size: STREAM_FRAME_INFO_SIZE,
            x: frame.origin_x,
            y: frame.origin_y,
            width,
            height,
            stride_bytes,
            is_duplicate: u8::from(frame.frame.metadata().is_duplicate()),
            pixel_format: pixel_format_value(frame.frame.pixel_format()),
            backend_kind: capture_backend_value(frame.frame.metadata().backend_kind()),
            reserved0: [0; 1],
            sequence: frame.frame.metadata().sequence(),
            rgba_bytes: bytes.as_ptr(),
            rgba_len: bytes.len(),
        };
    }
    Ok(())
}

#[allow(clippy::needless_update)] // needed when snow-capture enables its `stage-timing` feature
fn stream_capture_options(
    config: SnowCaptureStreamConfig,
    output_pixel_format: CapturePixelFormat,
    wgc_update_mode: WgcUpdateMode,
) -> CaptureOptions {
    CaptureOptions {
        backend_tuning: platform_tuning(snow_capture::tuning::windows::WindowsCaptureOptions {
            color_correction: if config.restore_original_colors != 0 {
                ColorCorrection::snapshot_current()
            } else {
                ColorCorrection::Disabled
            },
            gpu_hdr_conversion: true,
            hdr_tonemap_lut: true,
            wgc_update_mode,
        }),
        capture_retry_count: config.capture_retry_count.max(1),
        workload: CaptureWorkload::Continuous,
        output_pixel_format,
        ..Default::default()
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_stream_create_region(
    config: *const SnowCaptureStreamConfig,
) -> *mut SnowCaptureStreamImpl {
    let config = match unsafe { read_stream_config(config) } {
        Ok(config) => config,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let region = match CaptureRegion::new(config.x, config.y, config.width, config.height) {
        Ok(region) => region,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let output_pixel_format = match parse_pixel_format(config.pixel_format) {
        Ok(format) => format,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let wgc_update_mode = match parse_wgc_update_mode(config.wgc_update_mode) {
        Ok(mode) => mode,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let capture_backend = match parse_capture_backend(config.capture_backend) {
        Ok(backend) => backend,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let options = match unsafe {
        apply_exclusions(
            stream_capture_options(config, output_pixel_format, wgc_update_mode),
            config.exclusions,
        )
    } {
        Ok(options) => options,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let system = match CaptureSystem::builder()
        .with_backend_kind(capture_backend)
        .build()
    {
        Ok(system) => system,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let session = match system.open_session(CaptureTarget::Region(region), options) {
        Ok(session) => session,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let stream_config = CaptureStreamConfig {
        target_fps: config.target_fps,
        buffer_depth: config.buffer_depth as usize,
        max_consecutive_errors: config.max_consecutive_errors as usize,
        adaptive_fps: config.adaptive_fps != 0,
        min_fps: config.min_fps,
        pause_on_resolution_change: false,
        include_cursor: config.include_cursor != 0,
    };
    let stream = match CaptureStream::spawn(session, stream_config) {
        Ok(stream) => stream,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureStreamImpl {
        stream,
        origin_x: config.x,
        origin_y: config.y,
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_stream_destroy(stream: *mut SnowCaptureStreamImpl) {
    if !stream.is_null() {
        drop(unsafe { Box::from_raw(stream) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_stream_stop(stream: *mut SnowCaptureStreamImpl) -> u8 {
    if stream.is_null() {
        set_last_error("stream is null");
        return 0;
    }
    unsafe { &*stream }.stream.stop();
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_stream_set_target_fps(
    stream: *mut SnowCaptureStreamImpl,
    target_fps: u32,
) -> u8 {
    if stream.is_null() {
        set_last_error("stream is null");
        return 0;
    }
    unsafe { &*stream }.stream.set_target_fps(target_fps);
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_stream_receive(
    stream: *mut SnowCaptureStreamImpl,
    timeout_ms: u32,
    out_event: *mut SnowCaptureStreamEvent,
) -> u8 {
    if out_event.is_null() {
        set_last_error("stream event out_event is null");
        return 0;
    }
    if stream.is_null() {
        set_last_error("stream is null");
        return 0;
    }
    unsafe { *out_event = empty_stream_event() };
    let stream = unsafe { &*stream };
    match stream
        .stream
        .recv_timeout(Duration::from_millis(u64::from(timeout_ms)))
    {
        Ok(CaptureEvent::Frame(frame)) => {
            let frame = Box::new(SnowCaptureStreamFrameImpl {
                frame,
                origin_x: stream.origin_x,
                origin_y: stream.origin_y,
            });
            unsafe {
                (*out_event).kind = SnowCaptureStreamEventKind::Frame;
                (*out_event).frame = Box::into_raw(frame);
            }
            clear_last_error();
            1
        }
        Ok(CaptureEvent::FramesDropped { count }) => unsafe {
            (*out_event).kind = SnowCaptureStreamEventKind::FramesDropped;
            (*out_event).dropped_count = u64::from(count);
            clear_last_error();
            1
        },
        Ok(CaptureEvent::ResolutionChanged {
            old_width,
            old_height,
            new_width,
            new_height,
        }) => unsafe {
            (*out_event).kind = SnowCaptureStreamEventKind::ResolutionChanged;
            (*out_event).old_width = old_width;
            (*out_event).old_height = old_height;
            (*out_event).new_width = new_width;
            (*out_event).new_height = new_height;
            clear_last_error();
            1
        },
        Ok(CaptureEvent::Paused { .. }) => unsafe {
            (*out_event).kind = SnowCaptureStreamEventKind::Paused;
            clear_last_error();
            1
        },
        Ok(CaptureEvent::Resumed { .. }) => unsafe {
            (*out_event).kind = SnowCaptureStreamEventKind::Resumed;
            clear_last_error();
            1
        },
        Ok(CaptureEvent::StreamEnded) => unsafe {
            (*out_event).kind = SnowCaptureStreamEventKind::Ended;
            clear_last_error();
            1
        },
        Ok(CaptureEvent::Error(error)) => unsafe {
            set_last_error(error.to_string());
            (*out_event).kind = SnowCaptureStreamEventKind::Error;
            1
        },
        Err(RecvTimeoutError::Timeout) => {
            clear_last_error();
            1
        }
        Err(RecvTimeoutError::Disconnected) => unsafe {
            (*out_event).kind = SnowCaptureStreamEventKind::Ended;
            clear_last_error();
            1
        },
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_stream_frame_info(
    frame: *const SnowCaptureStreamFrameImpl,
    out_info: *mut SnowCaptureStreamFrameInfo,
) -> u8 {
    if out_info.is_null() {
        set_last_error("stream frame out_info is null");
        return 0;
    }
    if frame.is_null() {
        set_last_error("stream frame is null");
        return 0;
    }
    match write_stream_frame_info(unsafe { &*frame }, out_info) {
        Ok(()) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_stream_frame_release(frame: *mut SnowCaptureStreamFrameImpl) {
    if !frame.is_null() {
        drop(unsafe { Box::from_raw(frame) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_stream_stats(
    stream: *const SnowCaptureStreamImpl,
    out_stats: *mut SnowCaptureStreamStats,
) -> u8 {
    if out_stats.is_null() {
        set_last_error("stream stats out_stats is null");
        return 0;
    }
    if stream.is_null() {
        set_last_error("stream is null");
        return 0;
    }
    let snapshot = unsafe { &*stream }.stream.stats().snapshot();
    let buffer_fill = match u32::try_from(snapshot.buffer_fill) {
        Ok(value) => value,
        Err(_) => {
            set_last_error("stream buffer fill overflow");
            return 0;
        }
    };
    unsafe {
        *out_stats = SnowCaptureStreamStats {
            frames_captured: snapshot.frames_captured,
            frames_dropped: snapshot.frames_dropped,
            errors_recovered: snapshot.errors_recovered,
            current_fps: snapshot.current_fps,
            target_fps: snapshot.target_fps,
            buffer_fill,
            capture_latency_ns: snapshot
                .capture_latency_avg
                .as_nanos()
                .min(u64::MAX as u128) as u64,
        };
    }
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_window_session_create(
    config: *const SnowCaptureWindowSessionConfig,
) -> *mut SnowCaptureWindowSessionImpl {
    if config.is_null() {
        set_last_error("window session config is null");
        return ptr::null_mut();
    }
    let config = unsafe { &*config };
    if config.hwnd == 0 {
        set_last_error("window handle is null");
        return ptr::null_mut();
    }
    let output_pixel_format = match parse_pixel_format(config.pixel_format) {
        Ok(format) => format,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let options = match snapshot_options(
        config.capture_retry_count,
        config.wgc_update_mode,
        output_pixel_format,
    ) {
        Ok(options) => options,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let capture_backend = match parse_capture_backend(config.capture_backend) {
        Ok(backend) => backend,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };

    let system = match CaptureSystem::builder()
        .with_backend_kind(capture_backend)
        .build()
    {
        Ok(system) => system,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let id = match native_window_id(config.hwnd) {
        Ok(id) => id,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let target = CaptureTarget::Window(id);
    let session = match system.open_session(target, options) {
        Ok(session) => session,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureWindowSessionImpl {
        session,
        frame: Frame::empty(),
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_window_session_destroy(
    session: *mut SnowCaptureWindowSessionImpl,
) {
    if !session.is_null() {
        drop(unsafe { Box::from_raw(session) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_window_session_prepare(
    session: *mut SnowCaptureWindowSessionImpl,
) -> u8 {
    if session.is_null() {
        set_last_error("window session is null");
        return 0;
    }
    let session = unsafe { &mut *session };
    match session.session.prewarm_environment() {
        Ok(_) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_window_session_capture(
    session: *mut SnowCaptureWindowSessionImpl,
    out_info: *mut SnowCaptureWindowFrameInfo,
) -> u8 {
    if out_info.is_null() {
        set_last_error("window frame out_info is null");
        return 0;
    }
    if session.is_null() {
        set_last_error("window session is null");
        return 0;
    }
    let session = unsafe { &mut *session };
    if let Err(error) = session.session.capture_once_into(&mut session.frame) {
        set_last_error(error);
        return 0;
    }
    if session.session.active_capture_access_count() != 0 {
        set_last_error("capture access remained active after one-shot window capture");
        return 0;
    }
    let target = match session
        .session
        .target_info_for_backend(session.frame.metadata().backend_kind())
    {
        Ok(info) => info,
        Err(error) => {
            set_last_error(error);
            return 0;
        }
    };
    let frame = SnapshotWindowFrame {
        x: target.origin_x,
        y: target.origin_y,
        frame: Arc::new(session.frame.clone()),
    };
    if let Err(error) = write_window_frame_info(&frame, out_info) {
        set_last_error(error);
        return 0;
    }
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_window_session_frame_retain(
    session: *const SnowCaptureWindowSessionImpl,
) -> *mut SnowCaptureFrameLeaseImpl {
    if session.is_null() {
        set_last_error("window session is null");
        return ptr::null_mut();
    }
    let session = unsafe { &*session };
    if session.frame.as_bytes().is_empty() {
        set_last_error("window session has no captured frame");
        return ptr::null_mut();
    }
    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureFrameLeaseImpl {
        _frame: Arc::new(session.frame.clone()),
    }))
}

/// Clears the session-owned frame and returns the window session to its
/// lightweight prepared state. Any previously retained frame lease remains
/// valid until that lease is released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_window_session_release_frame(
    session: *mut SnowCaptureWindowSessionImpl,
) -> u8 {
    if session.is_null() {
        set_last_error("window session is null");
        return 0;
    }
    let session = unsafe { &mut *session };
    session.frame = Frame::empty();
    match session.session.reset_to_prepared() {
        Ok(()) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_screenshot_result_display_count(
    result: *const SnowCaptureScreenshotResultImpl,
) -> usize {
    if result.is_null() {
        set_last_error("screenshot result is null");
        return 0;
    }
    unsafe { &*result }.frames.len()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_screenshot_result_display_info(
    result: *const SnowCaptureScreenshotResultImpl,
    index: usize,
    out_info: *mut SnowCaptureFrameInfo,
) -> u8 {
    if result.is_null() {
        set_last_error("screenshot result is null");
        return 0;
    }
    if out_info.is_null() {
        set_last_error("display frame out_info is null");
        return 0;
    }
    let result = unsafe { &*result };
    let Some(frame) = result.frames.get(index) else {
        set_last_error("screenshot display index is out of range");
        return 0;
    };
    if let Err(error) = write_snapshot_frame_info(frame, out_info) {
        set_last_error(error);
        return 0;
    }
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_screenshot_result_display_retain(
    result: *const SnowCaptureScreenshotResultImpl,
    index: usize,
) -> *mut SnowCaptureFrameLeaseImpl {
    if result.is_null() {
        set_last_error("screenshot result is null");
        return ptr::null_mut();
    }
    let Some(frame) = (unsafe { &*result }).frames.get(index) else {
        set_last_error("screenshot display index is out of range");
        return ptr::null_mut();
    };
    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureFrameLeaseImpl {
        _frame: Arc::clone(&frame.frame),
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_screenshot_result_focused_window_info(
    result: *const SnowCaptureScreenshotResultImpl,
    out_info: *mut SnowCaptureWindowFrameInfo,
) -> u8 {
    if result.is_null() {
        set_last_error("screenshot result is null");
        return 0;
    }
    if out_info.is_null() {
        set_last_error("focused-window out_info is null");
        return 0;
    }
    let Some(frame) = (unsafe { &*result }).focused_window.as_ref() else {
        set_last_error("screenshot result has no focused-window frame");
        return 0;
    };
    if let Err(error) = write_window_frame_info(frame, out_info) {
        set_last_error(error);
        return 0;
    }
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_screenshot_result_focused_window_retain(
    result: *const SnowCaptureScreenshotResultImpl,
) -> *mut SnowCaptureFrameLeaseImpl {
    if result.is_null() {
        set_last_error("screenshot result is null");
        return ptr::null_mut();
    }
    let Some(frame) = (unsafe { &*result }).focused_window.as_ref() else {
        set_last_error("screenshot result has no focused-window frame");
        return ptr::null_mut();
    };
    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureFrameLeaseImpl {
        _frame: Arc::clone(&frame.frame),
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_screenshot_result_destroy(
    result: *mut SnowCaptureScreenshotResultImpl,
) {
    if !result.is_null() {
        drop(unsafe { Box::from_raw(result) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_frame_lease_release(lease: *mut SnowCaptureFrameLeaseImpl) {
    if !lease.is_null() {
        drop(unsafe { Box::from_raw(lease) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_last_error_message() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

pub struct SnowCaptureStreamImpl {
    stream: CaptureStream,
    origin_x: i32,
    origin_y: i32,
}

pub struct SnowCaptureStreamFrameImpl {
    frame: CapturedFrame,
    origin_x: i32,
    origin_y: i32,
}

#[cfg(target_os = "macos")]
pub mod macos;

#[cfg(test)]
mod tests {

    use super::*;

    fn test_entry() -> MonitorEntry {
        MonitorEntry {
            id: MonitorId::from_parts(1, 2, 3, "unit-monitor", true),
            stable_id: sanitize_cstring("stable-unit-monitor"),
            name: sanitize_cstring("unit-monitor"),
            x: -10,
            y: 20,
            expected_width: 2,
            expected_height: 2,
            is_primary: true,
        }
    }

    fn test_result() -> *mut SnowCaptureScreenshotResultImpl {
        let frame = Frame::from_rgba8(2, 2, vec![7; 16]).expect("valid test frame");
        Box::into_raw(Box::new(SnowCaptureScreenshotResultImpl {
            frames: vec![SnapshotFrame {
                entry: test_entry(),
                frame: Arc::new(frame),
            }],
            focused_window: None,
        }))
    }

    #[test]
    fn desktop_and_stream_exclusions_are_owned_and_validated() {
        let mut windows = [9, 7, 9];
        let processes = [5, 3, 5];
        let mut config: SnowCaptureDesktopSessionConfig = unsafe { std::mem::zeroed() };
        config.exclusions = SnowCaptureExclusions {
            windows: windows.as_ptr(),
            window_count: 3,
            processes: processes.as_ptr(),
            process_count: 3,
        };
        let (options, _) = default_options(&config).unwrap();
        let stream_options =
            unsafe { apply_exclusions(CaptureOptions::default(), config.exclusions) }.unwrap();
        windows[0] = 20;
        assert_eq!(&*options.excluded_windows, &[7, 9]);
        assert_eq!(&*options.excluded_processes, &[3, 5]);
        assert_eq!(stream_options.excluded_windows, options.excluded_windows);
        assert_eq!(windows[0], 20);
        config.exclusions.windows = std::ptr::null();
        assert!(default_options(&config).is_err());
        assert!(unsafe { apply_exclusions(CaptureOptions::default(), config.exclusions) }.is_err());
        #[cfg(target_os = "macos")]
        assert!(matches!(
            options.backend_tuning,
            snow_capture::BackendTuning::Default
        ));
    }

    #[test]
    fn every_capture_entry_point_rejects_invalid_exclusions_before_capture() {
        for exclusions in [
            SnowCaptureExclusions {
                window_count: 1,
                ..Default::default()
            },
            SnowCaptureExclusions {
                process_count: 1,
                ..Default::default()
            },
            SnowCaptureExclusions {
                windows: std::ptr::dangling(),
                window_count: 4097,
                ..Default::default()
            },
            SnowCaptureExclusions {
                processes: std::ptr::dangling(),
                process_count: 4097,
                ..Default::default()
            },
        ] {
            let mut desktop: SnowCaptureDesktopSessionConfig = unsafe { std::mem::zeroed() };
            desktop.exclusions = exclusions;
            assert!(snow_capture_desktop_session_create(&desktop).is_null());
            let mut monitor: SnowCaptureMonitorSessionConfig = unsafe { std::mem::zeroed() };
            monitor.device_name_utf8 = c"test-display".as_ptr();
            monitor.exclusions = exclusions;
            assert!(unsafe { snow_capture_monitor_session_create(&monitor) }.is_null());
            let mut region: SnowCaptureRegionSessionConfig = unsafe { std::mem::zeroed() };
            region.width = 64;
            region.height = 64;
            region.exclusions = exclusions;
            assert!(unsafe { snow_capture_region_session_create(&region) }.is_null());
            let mut stream: SnowCaptureStreamConfig = unsafe { std::mem::zeroed() };
            stream.version = STREAM_CONFIG_VERSION;
            stream.struct_size = STREAM_CONFIG_SIZE;
            stream.width = 64;
            stream.height = 64;
            stream.buffer_depth = 1;
            stream.max_consecutive_errors = 1;
            stream.exclusions = exclusions;
            assert!(unsafe { snow_capture_stream_create_region(&stream) }.is_null());
            assert!(
                unsafe { CStr::from_ptr(snow_capture_last_error_message()) }
                    .to_str()
                    .unwrap()
                    .contains("exclusion")
            );
        }
    }

    #[test]
    fn null_screenshot_result_info_fails() {
        let mut info = SnowCaptureFrameInfo {
            stable_id: ptr::null(),
            name: ptr::null(),
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            is_primary: 0,
            backend_kind: 0,
            pixel_format: 0,
            reserved0: 0,
            stride_bytes: 0,
            rgba_bytes: ptr::null(),
            rgba_len: 0,
        };

        let ok = unsafe { snow_capture_screenshot_result_display_info(ptr::null(), 0, &mut info) };
        assert_eq!(ok, 0);
        assert!(!snow_capture_last_error_message().is_null());
    }

    #[test]
    fn cursor_snapshot_null_arguments_fail_without_modifying_results() {
        unsafe {
            snow_capture_cursor_snapshot_destroy(ptr::null_mut());
            assert_eq!(
                snow_capture_screenshot_result_composite_cursor(ptr::null_mut(), ptr::null()),
                0
            );
            let result = test_result();
            let before = (&*result).frames[0].frame.as_bytes().to_vec();
            assert_eq!(
                snow_capture_screenshot_result_composite_cursor(result, ptr::null()),
                0
            );
            assert_eq!((&*result).frames[0].frame.as_bytes(), before);
            snow_capture_screenshot_result_destroy(result);
        }
    }

    #[test]
    fn screenshot_result_display_info_reports_monitor_and_tight_stride() {
        let snapshot = test_result();
        let mut info = SnowCaptureFrameInfo {
            stable_id: ptr::null(),
            name: ptr::null(),
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            is_primary: 0,
            backend_kind: 0,
            pixel_format: 0,
            reserved0: 0,
            stride_bytes: 0,
            rgba_bytes: ptr::null(),
            rgba_len: 0,
        };

        let ok = unsafe { snow_capture_screenshot_result_display_info(snapshot, 0, &mut info) };
        assert_eq!(ok, 1);
        assert_eq!(info.x, -10);
        assert_eq!(info.y, 20);
        assert_eq!(info.width, 2);
        assert_eq!(info.height, 2);
        assert_eq!(info.stride_bytes, 8);
        assert_eq!(info.rgba_len, 16);
        assert!(!info.rgba_bytes.is_null());

        unsafe { snow_capture_screenshot_result_destroy(snapshot) };
    }

    #[test]
    fn frame_lease_survives_result_destroy() {
        let snapshot = test_result();
        let lease = unsafe { snow_capture_screenshot_result_display_retain(snapshot, 0) };
        assert!(!lease.is_null());
        unsafe { snow_capture_screenshot_result_destroy(snapshot) };

        let lease_ref = unsafe { &*lease };
        assert_eq!(lease_ref._frame.width(), 2);
        assert_eq!(lease_ref._frame.height(), 2);
        assert!(
            lease_ref
                ._frame
                .as_rgba_bytes()
                .iter()
                .all(|byte| *byte == 7)
        );

        unsafe { snow_capture_frame_lease_release(lease) };
    }

    #[test]
    fn reset_to_prepared_null_session_fails() {
        let ok = snow_capture_desktop_session_reset_to_prepared(ptr::null_mut());
        assert_eq!(ok, 0);
        assert!(!snow_capture_last_error_message().is_null());
    }

    #[test]
    fn window_frame_release_null_session_fails() {
        let ok = unsafe { snow_capture_window_session_release_frame(ptr::null_mut()) };
        assert_eq!(ok, 0);
        assert!(!snow_capture_last_error_message().is_null());
    }

    #[test]
    fn region_session_rejects_null_and_empty_config() {
        assert!(unsafe { snow_capture_region_session_create(ptr::null()) }.is_null());

        let config = SnowCaptureRegionSessionConfig {
            exclusions: Default::default(),
            x: 0,
            y: 0,
            width: 0,
            height: 100,
            capture_retry_count: 1,
            wgc_update_mode: 0,
            capture_backend: 0,
            pixel_format: 0,
            reserved: [0; 29],
        };
        assert!(unsafe { snow_capture_region_session_create(&config) }.is_null());
    }

    #[test]
    fn monitor_session_rejects_missing_names_without_opening_capture() {
        assert!(unsafe { snow_capture_monitor_session_create(ptr::null()) }.is_null());
        let mut config = SnowCaptureMonitorSessionConfig {
            exclusions: Default::default(),
            device_name_utf8: ptr::null(),
            capture_retry_count: 1,
            pixel_format: 0,
            reserved: [0; 31],
        };
        assert!(unsafe { snow_capture_monitor_session_create(&config) }.is_null());
        config.device_name_utf8 = c"".as_ptr();
        assert!(unsafe { snow_capture_monitor_session_create(&config) }.is_null());
        assert_eq!(
            unsafe { snow_capture_monitor_session_capture(ptr::null_mut(), ptr::null_mut()) },
            0
        );
        assert!(unsafe { snow_capture_monitor_session_frame_retain(ptr::null()) }.is_null());
    }

    #[test]
    fn monitor_selection_requires_exact_device_identity() {
        let first = test_entry();
        let mut second = test_entry();
        second.id = MonitorId::from_name(2, "secondary", false);
        assert_eq!(
            select_monitor_entry(vec![first.clone(), second.clone()], "secondary")
                .unwrap()
                .id,
            second.id
        );
        assert!(select_monitor_entry(vec![first, second], "disconnected").is_err());
    }

    #[test]
    fn region_capture_rejects_null_handles() {
        let mut info = SnowCaptureRegionFrameInfo {
            width: 0,
            height: 0,
            stride_bytes: 0,
            is_duplicate: 0,
            pixel_format: 0,
            reserved0: [0; 2],
            rgba_bytes: ptr::null(),
            rgba_len: 0,
        };
        assert_eq!(
            unsafe { snow_capture_region_session_capture(ptr::null_mut(), &mut info) },
            0
        );
        assert_eq!(
            unsafe { snow_capture_region_session_prepare(ptr::null_mut()) },
            0
        );
    }

    #[test]
    fn window_session_rejects_null_and_empty_config() {
        assert!(unsafe { snow_capture_window_session_create(ptr::null()) }.is_null());

        let config = SnowCaptureWindowSessionConfig {
            hwnd: 0,
            capture_retry_count: 1,
            wgc_update_mode: 0,
            capture_backend: 0,
            pixel_format: 0,
            reserved: [0; 29],
        };
        assert!(unsafe { snow_capture_window_session_create(&config) }.is_null());
    }

    #[test]
    fn window_capture_rejects_null_handles() {
        let mut info = SnowCaptureWindowFrameInfo {
            version: WINDOW_FRAME_INFO_VERSION,
            struct_size: std::mem::size_of::<SnowCaptureWindowFrameInfo>() as u32,
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            stride_bytes: 0,
            rgba_bytes: ptr::null(),
            rgba_len: 0,
            backend_kind: 0,
            pixel_format: 0,
            reserved: [0; 6],
        };
        assert_eq!(
            unsafe { snow_capture_window_session_capture(ptr::null_mut(), &mut info) },
            0
        );
        assert_eq!(
            unsafe { snow_capture_window_session_prepare(ptr::null_mut()) },
            0
        );
    }

    #[test]
    fn desktop_session_state_reports_worker_count_and_prepared_flag() {
        let system = CaptureSystem::builder()
            .build()
            .expect("capture system should initialize");
        let mut session = SnowCaptureDesktopSessionImpl {
            system,
            options: CaptureOptions::default(),
            workers: Vec::new(),
            prepared: true,
        };
        let mut state = SnowCaptureDesktopSessionState {
            worker_count: usize::MAX,
            prepared: 0,
            reserved0: [1; 3],
            active_capture_access_count: u32::MAX,
            retained_resource_bytes: 99,
            backend_kind: ptr::null(),
        };

        let ok = unsafe { snow_capture_desktop_session_state(&mut session, &mut state) };
        assert_eq!(ok, 1);
        assert_eq!(state.worker_count, 0);
        assert_eq!(state.prepared, 1);
        assert_eq!(state.active_capture_access_count, 0);
        assert_eq!(state.retained_resource_bytes, 0);
        assert!(!state.backend_kind.is_null());
    }

    #[test]
    fn wgc_update_mode_parser_is_strict() {
        assert_eq!(parse_wgc_update_mode(0), Ok(WgcUpdateMode::Auto));
        assert_eq!(parse_wgc_update_mode(1), Ok(WgcUpdateMode::CompleteOnly));
        assert_eq!(
            parse_wgc_update_mode(2),
            Ok(WgcUpdateMode::OrderedIncremental)
        );
        assert!(parse_wgc_update_mode(3).is_err());
        assert!(parse_wgc_update_mode(u8::MAX).is_err());
    }

    #[test]
    fn capture_backend_parser_maps_wgc_and_rejects_unknown_values() {
        assert_eq!(
            parse_capture_backend(2),
            Ok(CaptureBackendKind::WindowsGraphicsCapture)
        );
        assert_eq!(
            parse_capture_backend(4).unwrap(),
            CaptureBackendKind::ScreenCaptureKit
        );
        assert!(parse_capture_backend(5).is_err());
        assert!(parse_capture_backend(u8::MAX).is_err());
    }

    #[test]
    fn pixel_format_parser_maps_rgba_and_bgra_and_rejects_unknown_values() {
        assert_eq!(parse_pixel_format(0), Ok(CapturePixelFormat::Rgba8));
        assert_eq!(parse_pixel_format(1), Ok(CapturePixelFormat::Bgra8));
        assert!(parse_pixel_format(2).is_err());
        assert!(parse_pixel_format(u8::MAX).is_err());
    }

    #[test]
    fn stream_abi_layout_is_stable() {
        assert_eq!(
            std::mem::offset_of!(SnowCaptureStreamFrameInfo, backend_kind),
            30
        );
        assert_eq!(
            std::mem::size_of::<SnowCaptureStreamConfig>(),
            std::mem::size_of::<usize>() * 5 + 72
        );
        assert_eq!(std::mem::size_of::<SnowCaptureStreamEvent>(), 40 + 32);
        assert_eq!(
            std::mem::size_of::<SnowCaptureStreamFrameInfo>(),
            40 + std::mem::size_of::<usize>() * 2
        );
        assert_eq!(
            std::mem::offset_of!(SnowCaptureStreamFrameInfo, sequence),
            32
        );
        assert_eq!(
            std::mem::size_of::<SnowCaptureStreamStats>(),
            40 + std::mem::size_of::<usize>()
        );
    }

    #[test]
    fn stream_config_validation_checks_version_size_and_dimensions() {
        let valid = SnowCaptureStreamConfig {
            exclusions: Default::default(),
            version: STREAM_CONFIG_VERSION,
            struct_size: STREAM_CONFIG_SIZE,
            x: 0,
            y: 0,
            width: 16,
            height: 12,
            target_fps: 30,
            min_fps: 1,
            buffer_depth: 3,
            max_consecutive_errors: 30,
            capture_retry_count: 1,
            wgc_update_mode: 1,
            capture_backend: 2,
            pixel_format: 0,
            adaptive_fps: 1,
            include_cursor: 0,
            restore_original_colors: 0,
            reserved: [0; 26],
        };
        assert!(unsafe { read_stream_config(&valid) }.is_ok());

        let options = stream_capture_options(
            valid,
            CapturePixelFormat::Rgba8,
            WgcUpdateMode::CompleteOnly,
        );
        assert_eq!(
            options.backend_tuning.windows_or_default().color_correction,
            ColorCorrection::Disabled
        );
        #[cfg(windows)]
        for restore_original_colors in [1, u8::MAX] {
            let options = stream_capture_options(
                SnowCaptureStreamConfig {
                    restore_original_colors,
                    ..valid
                },
                CapturePixelFormat::Rgba8,
                WgcUpdateMode::CompleteOnly,
            );
            assert!(
                matches!(
                    options.backend_tuning.windows_or_default().color_correction,
                    ColorCorrection::Snapshot(_)
                ),
                "a scrolling stream must retain one color filter snapshot for all frames"
            );
        }

        let mut wrong_version = valid;
        wrong_version.version += 1;
        assert!(unsafe { read_stream_config(&wrong_version) }.is_err());

        let mut too_small = valid;
        too_small.struct_size = STREAM_CONFIG_SIZE - 1;
        assert!(unsafe { read_stream_config(&too_small) }.is_err());

        let mut empty = valid;
        empty.width = 0;
        assert!(unsafe { read_stream_config(&empty) }.is_err());
    }

    #[test]
    fn stream_frame_metadata_preserves_backend_and_pixels() {
        let frame = Frame::from_rgba8(2, 3, vec![17; 24]).unwrap();
        let expected_backend = capture_backend_value(frame.metadata().backend_kind());
        let owned = SnowCaptureStreamFrameImpl {
            frame: frame.into(),
            origin_x: -100,
            origin_y: 50,
        };
        let mut output = std::mem::MaybeUninit::<SnowCaptureStreamFrameInfo>::uninit();
        write_stream_frame_info(&owned, output.as_mut_ptr()).unwrap();
        let info = unsafe { output.assume_init() };
        assert_eq!(info.backend_kind, expected_backend);
        assert_eq!(info.reserved0, [0]);
        assert_eq!((info.x, info.y, info.width, info.height), (-100, 50, 2, 3));
        assert_eq!(info.stride_bytes, 8);
        assert_eq!(
            unsafe { std::slice::from_raw_parts(info.rgba_bytes, info.rgba_len) },
            &[17; 24]
        );
    }

    #[test]
    fn stream_abi_null_handles_fail_without_touching_output() {
        let mut event = empty_stream_event();
        assert_eq!(
            unsafe { snow_capture_stream_receive(ptr::null_mut(), 0, &mut event) },
            0
        );
        assert_eq!(
            unsafe { snow_capture_stream_set_target_fps(ptr::null_mut(), 30) },
            0
        );
        assert_eq!(unsafe { snow_capture_stream_stop(ptr::null_mut()) }, 0);
        let mut info = SnowCaptureStreamFrameInfo {
            version: 99,
            struct_size: 99,
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            stride_bytes: 0,
            is_duplicate: 0,
            pixel_format: 0,
            backend_kind: 0,
            reserved0: [0; 1],
            sequence: 0,
            rgba_bytes: ptr::null(),
            rgba_len: 0,
        };
        assert_eq!(
            unsafe { snow_capture_stream_frame_info(ptr::null(), &mut info) },
            0
        );
        assert_eq!(info.version, 99);
        assert_eq!(
            unsafe { snow_capture_stream_stats(ptr::null(), ptr::null_mut()) },
            0
        );
        unsafe {
            snow_capture_stream_frame_release(ptr::null_mut());
            snow_capture_stream_destroy(ptr::null_mut());
        }
    }

    #[test]
    fn desktop_config_selects_wgc() {
        let config = SnowCaptureDesktopSessionConfig {
            exclusions: Default::default(),
            capture_retry_count: 2,
            wgc_update_mode: 1,
            capture_backend: 2,
            pixel_format: 0,
            reserved: [0; 29],
        };

        let (options, backend) = default_options(&raw const config).unwrap();

        assert_eq!(options.capture_retry_count, 2);
        #[cfg(windows)]
        assert_eq!(
            options.backend_tuning.windows_or_default().wgc_update_mode,
            WgcUpdateMode::CompleteOnly
        );
        assert_eq!(options.output_pixel_format, CapturePixelFormat::Rgba8);
        assert_eq!(backend, CaptureBackendKind::WindowsGraphicsCapture);
    }

    #[test]
    #[cfg(windows)]
    fn desktop_backend_preferences_keep_all_fallbacks() {
        use CaptureBackendKind::{DxgiDuplication, Gdi, WindowsGraphicsCapture};

        for (preferred, expected) in [
            (
                DxgiDuplication,
                [DxgiDuplication, WindowsGraphicsCapture, Gdi],
            ),
            (
                WindowsGraphicsCapture,
                [WindowsGraphicsCapture, DxgiDuplication, Gdi],
            ),
            (Gdi, [Gdi, DxgiDuplication, WindowsGraphicsCapture]),
        ] {
            let policy = desktop_backend_policy(preferred).unwrap();
            assert_eq!(policy.normalized_priority(), expected);
        }
    }

    #[test]
    fn desktop_auto_preserves_default_target_priorities() {
        assert_eq!(desktop_backend_policy(CaptureBackendKind::Auto), None);
        let (_, backend) = default_options(ptr::null()).unwrap();
        assert_eq!(backend, CaptureBackendKind::Auto);
    }

    #[test]
    fn capture_config_exclusion_layouts() {
        let pointer_sized_prefix = std::mem::size_of::<usize>();
        assert_eq!(
            std::mem::size_of::<SnowCaptureExclusions>(),
            pointer_sized_prefix * 4
        );
        assert_eq!(std::mem::offset_of!(SnowCaptureExclusions, windows), 0);
        assert_eq!(
            std::mem::offset_of!(SnowCaptureExclusions, window_count),
            pointer_sized_prefix
        );
        assert_eq!(
            std::mem::offset_of!(SnowCaptureExclusions, processes),
            pointer_sized_prefix * 2
        );
        assert_eq!(
            std::mem::offset_of!(SnowCaptureExclusions, process_count),
            pointer_sized_prefix * 3
        );
        assert_eq!(
            std::mem::offset_of!(SnowCaptureDesktopSessionConfig, exclusions),
            pointer_sized_prefix + 32
        );
        assert_eq!(
            std::mem::offset_of!(SnowCaptureMonitorSessionConfig, exclusions),
            pointer_sized_prefix * 2 + 32
        );
        assert_eq!(
            std::mem::size_of::<SnowCaptureMonitorSessionConfig>(),
            pointer_sized_prefix * 6 + 32
        );
        assert_eq!(
            std::mem::offset_of!(SnowCaptureRegionSessionConfig, exclusions),
            16 + pointer_sized_prefix + 32
        );
        assert_eq!(
            std::mem::offset_of!(SnowCaptureStreamConfig, exclusions),
            pointer_sized_prefix + 72
        );
        assert_eq!(STREAM_CONFIG_VERSION, 2);
        assert_eq!(
            std::mem::size_of::<SnowCaptureDesktopSessionConfig>(),
            pointer_sized_prefix * 5 + 32
        );
        assert_eq!(
            std::mem::size_of::<SnowCaptureWindowSessionConfig>(),
            pointer_sized_prefix * 2 + 32
        );
        assert_eq!(
            std::mem::size_of::<SnowCaptureRegionSessionConfig>(),
            16 + pointer_sized_prefix * 5 + 32
        );
        assert_eq!(
            std::mem::size_of::<SnowCaptureDesktopSessionState>(),
            pointer_sized_prefix * 2 + 16
        );
        assert_eq!(
            std::mem::size_of::<SnowCaptureFrameInfo>(),
            40 + pointer_sized_prefix * 2
        );
        assert_eq!(
            std::mem::offset_of!(SnowCaptureDesktopSessionState, active_capture_access_count),
            pointer_sized_prefix + 4
        );
        assert_eq!(
            std::mem::offset_of!(SnowCaptureFrameInfo, backend_kind),
            pointer_sized_prefix * 2 + 16 + 1
        );
    }

    #[test]
    fn versioned_screenshot_abi_has_expected_layout() {
        assert_eq!(std::mem::size_of::<SnowCaptureStreamConfig>(), 112);
        assert_eq!(
            std::mem::offset_of!(SnowCaptureStreamConfig, restore_original_colors),
            std::mem::offset_of!(SnowCaptureStreamConfig, include_cursor) + 1
        );
        assert_eq!(
            SCREENSHOT_REQUEST_SIZE as usize,
            std::mem::size_of::<SnowCaptureScreenshotRequest>()
        );
        assert_eq!(
            WINDOW_FRAME_INFO_SIZE as usize,
            std::mem::size_of::<SnowCaptureWindowFrameInfo>()
        );
        assert_eq!(std::mem::size_of::<SnowCaptureScreenshotRequest>(), 64);
        assert_eq!(std::mem::size_of::<SnowCaptureWindowFrameInfo>(), 56);
        assert_eq!(
            std::mem::offset_of!(SnowCaptureScreenshotRequest, cancellation_token),
            24
        );
        assert_eq!(
            std::mem::offset_of!(SnowCaptureWindowFrameInfo, backend_kind),
            48
        );
    }

    #[test]
    fn versioned_screenshot_request_reads_only_header_before_size_validation() {
        let short = SnowCaptureScreenshotRequestHeader {
            version: SCREENSHOT_REQUEST_VERSION,
            struct_size: std::mem::size_of::<SnowCaptureScreenshotRequestHeader>() as u32,
        };
        let request = (&raw const short).cast::<SnowCaptureScreenshotRequest>();

        assert!(unsafe { read_screenshot_request(request) }.is_err());

        let unknown = SnowCaptureScreenshotRequestHeader {
            version: SCREENSHOT_REQUEST_VERSION + 1,
            struct_size: SCREENSHOT_REQUEST_SIZE,
        };
        let request = (&raw const unknown).cast::<SnowCaptureScreenshotRequest>();
        assert!(unsafe { read_screenshot_request(request) }.is_err());
    }

    #[test]
    fn versioned_screenshot_request_rejects_unsupported_flags() {
        let request = SnowCaptureScreenshotRequest {
            version: SCREENSHOT_REQUEST_VERSION,
            struct_size: SCREENSHOT_REQUEST_SIZE,
            flags: SCREENSHOT_REQUEST_REFRESH_LAYOUT | (1 << 31),
            reserved0: 0,
            focused_window: 0,
            cancellation_token: ptr::null(),
            reserved: [0; 32],
        };

        let error = match unsafe { read_screenshot_request(&request) } {
            Ok(_) => panic!("unsupported screenshot request flags must fail"),
            Err(error) => error,
        };
        assert!(error.contains("unsupported flags"));
    }

    #[test]
    fn cancellation_token_is_thread_safe_and_sticky() {
        let token = snow_capture_cancellation_token_create();
        assert!(!token.is_null());
        let state = unsafe { &*token }.canceled.clone();
        let token_address = token as usize;
        let cancel = std::thread::spawn(move || unsafe {
            snow_capture_cancellation_token_cancel(
                token_address as *mut SnowCaptureCancellationTokenImpl,
            );
        });
        cancel.join().expect("cancel thread should complete");
        assert!(state.is_canceled());
        unsafe { snow_capture_cancellation_token_destroy(token) };
    }

    #[test]
    fn color_correction_is_opt_in_and_request_flag_is_valid() {
        let (options, _) = default_options(ptr::null()).unwrap();
        assert_eq!(
            options.backend_tuning.windows_or_default().color_correction,
            ColorCorrection::Disabled
        );
        let request = SnowCaptureScreenshotRequest {
            version: SCREENSHOT_REQUEST_VERSION,
            struct_size: SCREENSHOT_REQUEST_SIZE,
            flags: SCREENSHOT_REQUEST_RESTORE_ORIGINAL_COLORS,
            reserved0: 0,
            focused_window: 0,
            cancellation_token: ptr::null(),
            reserved: [0; 32],
        };
        assert!(unsafe { read_screenshot_request(&request) }.is_ok());
    }

    #[test]
    fn cursor_compositing_request_flag_is_valid() {
        let request = SnowCaptureScreenshotRequest {
            version: SCREENSHOT_REQUEST_VERSION,
            struct_size: SCREENSHOT_REQUEST_SIZE,
            flags: SCREENSHOT_REQUEST_INCLUDE_CURSOR,
            reserved0: 0,
            focused_window: 0,
            cancellation_token: ptr::null(),
            reserved: [0; 32],
        };

        assert!(unsafe { read_screenshot_request(&request) }.is_ok());
    }

    #[test]
    fn monitor_layout_comparison_is_order_independent_and_geometry_sensitive() {
        let first = test_entry();
        let mut second = first.clone();
        second.id = MonitorId::from_parts(5, 7, 9, "second-monitor", false);
        second.stable_id = sanitize_cstring("stable-second-monitor");
        second.name = sanitize_cstring("second-monitor");
        second.x = 100;
        second.is_primary = false;

        assert!(same_monitor_layout(
            &[first.clone(), second.clone()],
            &[second.clone(), first.clone()]
        ));

        let original_second = second.clone();
        second.expected_width += 1;
        assert!(!same_monitor_layout(
            &[first.clone(), original_second],
            &[first.clone(), second]
        ));

        let mut moved = first.clone();
        moved.id = moved
            .id
            .with_desktop_geometry(snow_capture::monitor::MonitorDesktopGeometry {
                x: 1.0,
                y: 2.0,
                width: 3.0,
                height: 4.0,
                pixel_width: 6,
                pixel_height: 8,
            });
        assert!(!same_monitor_layout(&[first.clone()], &[moved.clone()]));
        assert!(same_monitor_layout(&[moved.clone()], &[moved]));
    }
}

#[cfg(all(test, target_os = "macos"))]
#[test]
fn macos_auto_has_only_screen_capture_kit() {
    assert_eq!(
        AutoBackendPolicy::default().normalized_priority(),
        vec![CaptureBackendKind::ScreenCaptureKit]
    );
}

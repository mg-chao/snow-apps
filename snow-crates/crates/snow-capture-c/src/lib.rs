#![allow(clippy::missing_safety_doc)]

use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char};
use std::path::PathBuf;
use std::ptr;
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
    mpsc,
};
use std::thread::{self, JoinHandle};

use snow_capture::frame::Frame;
use snow_capture::{
    CaptureOptions, CaptureRegion, CaptureSession, CaptureSystem, CaptureTarget, CaptureWorkload,
    MonitorId, MonitorLayout, WgcUpdateMode, WindowId, backend::CaptureBackendKind,
};
use snow_screen_recorder::{
    EditingSession, ExportFormat, ExportRequest, RecordingAudioConfig, RecordingAudioTrackConfig,
    RecordingConfig, RecordingRegion, RecordingSession, RecordingState, RecordingTarget,
    VideoCodec, VideoEncodeConfig, VideoEncodingSpeed,
};

pub struct SnowCaptureDesktopSessionImpl {
    system: CaptureSystem,
    options: CaptureOptions,
    workers: Vec<MonitorWorker>,
    prepared: bool,
}

pub struct SnowCaptureRegionSessionImpl {
    session: CaptureSession,
    frame: Frame,
}

pub struct SnowCaptureWindowSessionImpl {
    session: CaptureSession,
    frame: Frame,
}

pub struct SnowCaptureCancellationTokenImpl {
    canceled: Arc<AtomicBool>,
}

pub struct SnowCaptureSnapshotImpl {
    frames: Vec<SnapshotFrame>,
}

pub struct SnowCaptureScreenshotResultImpl {
    frames: Vec<SnapshotFrame>,
    focused_window: Option<SnapshotWindowFrame>,
}

pub struct SnowCaptureFrameLeaseImpl {
    _frame: Arc<Frame>,
}

pub struct SnowCaptureRecordingSessionImpl {
    recording: Option<RecordingSession>,
    state: RecordingState,
}

#[repr(C)]
pub struct SnowCaptureDesktopSessionConfig {
    capture_retry_count: usize,
    wgc_update_mode: u8,
    capture_backend: u8,
    reserved: [u8; 30],
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
    pub reserved0: [u8; 2],
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
    pub reserved: [u8; 30],
}

#[repr(C)]
pub struct SnowCaptureWindowSessionConfig {
    hwnd: isize,
    capture_retry_count: usize,
    wgc_update_mode: u8,
    capture_backend: u8,
    reserved: [u8; 30],
}

#[repr(C)]
pub struct SnowCaptureWindowFrameInfo {
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    stride_bytes: u32,
    rgba_bytes: *const u8,
    rgba_len: usize,
}

#[repr(C)]
pub struct SnowCaptureWindowFrameInfoV1 {
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
    reserved: [u8; 7],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowCaptureScreenshotRequestV1 {
    version: u32,
    struct_size: u32,
    flags: u32,
    reserved0: u32,
    focused_window: isize,
    cancellation_token: *const SnowCaptureCancellationTokenImpl,
    reserved: [u8; 32],
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
    pub reserved0: [u8; 3],
    pub rgba_bytes: *const u8,
    pub rgba_len: usize,
}

#[repr(C)]
pub struct SnowCaptureRecordingConfig {
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    fps: u32,
    enable_microphone: u8,
    enable_system_audio: u8,
    capture_backend: u8,
    reserved0: u8,
    working_directory_utf8: *const c_char,
    reserved: [u8; 32],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowCaptureRecordingState {
    Created = 0,
    Running = 1,
    Paused = 2,
    Stopped = 3,
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
    Capture(mpsc::Sender<Result<Frame, String>>),
    ReleaseIdleResources(mpsc::Sender<Result<(), String>>),
    ActiveCaptureAccessCount(mpsc::Sender<Result<usize, String>>),
    Stop,
}

const SCREENSHOT_REQUEST_VERSION: u32 = 1;
const SCREENSHOT_REQUEST_V1_SIZE: u32 =
    std::mem::size_of::<SnowCaptureScreenshotRequestV1>() as u32;
const SCREENSHOT_REQUEST_REFRESH_LAYOUT: u32 = 1 << 0;
const WINDOW_FRAME_INFO_VERSION: u32 = 1;
const WINDOW_FRAME_INFO_V1_SIZE: u32 = std::mem::size_of::<SnowCaptureWindowFrameInfoV1>() as u32;

unsafe fn read_screenshot_request(
    request: *const SnowCaptureScreenshotRequestV1,
) -> Result<SnowCaptureScreenshotRequestV1, String> {
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
    if header.struct_size < SCREENSHOT_REQUEST_V1_SIZE {
        return Err(format!(
            "screenshot request is too small: {} < {}",
            header.struct_size, SCREENSHOT_REQUEST_V1_SIZE
        ));
    }
    let request = unsafe { *request };
    if request.flags & !SCREENSHOT_REQUEST_REFRESH_LAYOUT != 0 {
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
        _ => Err(format!("invalid capture backend: {value}")),
    }
}

fn capture_backend_value(kind: CaptureBackendKind) -> u8 {
    match kind {
        CaptureBackendKind::Auto => 0,
        CaptureBackendKind::DxgiDuplication => 1,
        CaptureBackendKind::WindowsGraphicsCapture => 2,
        CaptureBackendKind::Gdi => 3,
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowCaptureRecordingExportConfig {
    version: u32,
    struct_size: u32,
    output_file_utf8: *const c_char,
    format: u32,
    maximum_width: u32,
    maximum_height: u32,
    target_fps: u32,
    codec: u32,
    preset: u32,
    reserved: [u8; 32],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SnowCaptureRecordingExportConfigHeader {
    version: u32,
    struct_size: u32,
}

const RECORDING_EXPORT_CONFIG_VERSION: u32 = 1;
const RECORDING_EXPORT_CONFIG_V1_SIZE: u32 =
    std::mem::size_of::<SnowCaptureRecordingExportConfig>() as u32;

fn default_options(
    config: *const SnowCaptureDesktopSessionConfig,
) -> Result<(CaptureOptions, CaptureBackendKind), String> {
    let (capture_retry_count, wgc_update_mode, capture_backend) = if config.is_null() {
        (1, WgcUpdateMode::Auto, CaptureBackendKind::Auto)
    } else {
        let config = unsafe { &*config };
        (
            config.capture_retry_count.max(1),
            parse_wgc_update_mode(config.wgc_update_mode)?,
            parse_capture_backend(config.capture_backend)?,
        )
    };

    Ok((
        CaptureOptions {
            capture_retry_count,
            workload: CaptureWorkload::Snapshot,
            gpu_hdr_conversion: true,
            hdr_tonemap_lut: true,
            wgc_update_mode,
        },
        capture_backend,
    ))
}

fn snapshot_options(
    capture_retry_count: usize,
    wgc_update_mode: u8,
) -> Result<CaptureOptions, String> {
    Ok(CaptureOptions {
        capture_retry_count: capture_retry_count.max(1),
        workload: CaptureWorkload::Snapshot,
        gpu_hdr_conversion: true,
        hdr_tonemap_lut: true,
        wgc_update_mode: parse_wgc_update_mode(wgc_update_mode)?,
    })
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
                        WorkerCommand::Capture(reply) => {
                            let result = match session.as_mut() {
                                Ok(session) => {
                                    match session.capture_once() {
                                        Ok(frame) if session.active_capture_access_count() == 0 => {
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
                        WorkerCommand::ReleaseIdleResources(reply) => {
                            let result = match session.as_mut() {
                                Ok(capture_session) => {
                                    capture_session.release_idle_resources();
                                    Ok(())
                                }
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

    fn request_capture(&self) -> Result<mpsc::Receiver<Result<Frame, String>>, String> {
        let (tx, rx) = mpsc::channel();
        self.tx
            .send(WorkerCommand::Capture(tx))
            .map_err(|_| "capture worker is not running".to_owned())?;
        Ok(rx)
    }

    fn request_release_idle_resources(&self) -> Result<mpsc::Receiver<Result<(), String>>, String> {
        let (tx, rx) = mpsc::channel();
        self.tx
            .send(WorkerCommand::ReleaseIdleResources(tx))
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

fn snapshot_ref<'a>(
    snapshot: *const SnowCaptureSnapshotImpl,
) -> Option<&'a SnowCaptureSnapshotImpl> {
    if snapshot.is_null() {
        set_last_error("snapshot is null");
        None
    } else {
        Some(unsafe { &*snapshot })
    }
}

fn replace_workers(
    session: &mut SnowCaptureDesktopSessionImpl,
    entries: Vec<MonitorEntry>,
) -> Result<(), String> {
    let old_workers = std::mem::take(&mut session.workers);
    let mut next_workers = Vec::with_capacity(entries.len());

    for entry in entries {
        match MonitorWorker::start(session.system.clone(), session.options, entry) {
            Ok(worker) => next_workers.push(worker),
            Err(error) => {
                for worker in next_workers {
                    worker.stop();
                }
                session.workers = old_workers;
                session.prepared = false;
                return Err(error);
            }
        }
    }

    for worker in old_workers {
        worker.stop();
    }

    session.workers = next_workers;
    session.prepared = false;
    Ok(())
}

fn rebuild_workers(session: &mut SnowCaptureDesktopSessionImpl) -> Result<(), String> {
    session
        .system
        .refresh_display_configuration()
        .map_err(|err| err.to_string())?;
    let entries = build_monitor_entries(&session.system)?;
    replace_workers(session, entries)
}

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
            })
        })
}

fn capture_all_frames(
    session: &mut SnowCaptureDesktopSessionImpl,
) -> Result<Vec<SnapshotFrame>, String> {
    let mut receivers = Vec::with_capacity(session.workers.len());
    let mut first_error = None;
    for worker in &session.workers {
        match worker.request_capture() {
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
) -> Result<Vec<SnapshotFrame>, String> {
    match capture_all_frames(session) {
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
            if let Err(refresh_error) = replace_workers(session, entries) {
                return Err(format!(
                    "{first_error}; layout refresh failed: {refresh_error}"
                ));
            }
            capture_all_frames(session).map_err(|retry_error| {
                format!("{first_error}; retry after layout refresh failed: {retry_error}")
            })
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

fn backend_kind_ptr(session: &SnowCaptureDesktopSessionImpl) -> *const c_char {
    match session.system.backend_kind().as_str() {
        "auto" => c"auto".as_ptr(),
        "dxgi" => c"dxgi".as_ptr(),
        "wgc" => c"wgc".as_ptr(),
        "gdi" => c"gdi".as_ptr(),
        _ => c"unknown".as_ptr(),
    }
}

fn capture_window_snapshot(
    hwnd: isize,
    options: CaptureOptions,
) -> Result<SnapshotWindowFrame, String> {
    let system = CaptureSystem::builder()
        .with_backend_kind(CaptureBackendKind::Auto)
        .build()
        .map_err(|error| error.to_string())?;
    let mut session = system
        .open_session(
            CaptureTarget::Window(WindowId::from_raw_handle(hwnd)),
            options,
        )
        .map_err(|error| error.to_string())?;
    let frame = session.capture_once().map_err(|error| error.to_string())?;
    if session.active_capture_access_count() != 0 {
        return Err("capture access remained active after focused-window capture".to_owned());
    }
    let target = session
        .target_info_for_backend(frame.metadata().backend_kind())
        .map_err(|error| error.to_string())?;
    Ok(SnapshotWindowFrame {
        x: target.origin_x,
        y: target.origin_y,
        frame: Arc::new(frame),
    })
}

fn write_snapshot_frame_info(
    frame: &SnapshotFrame,
    out_info: *mut SnowCaptureFrameInfo,
) -> Result<(), String> {
    let rgba = frame.frame.as_rgba_bytes();
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
            reserved0: [0; 2],
            stride_bytes,
            rgba_bytes: rgba.as_ptr(),
            rgba_len: rgba.len(),
        };
    }
    Ok(())
}

fn write_window_frame_info_v1(
    frame: &SnapshotWindowFrame,
    out_info: *mut SnowCaptureWindowFrameInfoV1,
) -> Result<(), String> {
    let rgba = frame.frame.as_rgba_bytes();
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
        *out_info = SnowCaptureWindowFrameInfoV1 {
            version: WINDOW_FRAME_INFO_VERSION,
            struct_size: WINDOW_FRAME_INFO_V1_SIZE,
            x: frame.x,
            y: frame.y,
            width: frame.frame.width(),
            height: frame.frame.height(),
            stride_bytes,
            rgba_bytes: rgba.as_ptr(),
            rgba_len: rgba.len(),
            backend_kind: capture_backend_value(frame.frame.metadata().backend_kind()),
            reserved: [0; 7],
        };
    }
    Ok(())
}

/// Releases the process-wide worker pool used by pixel conversion.
///
/// Conversions already in progress retain the pool until they finish. Callers
/// that need its threads to exit promptly should first stop and join capture
/// work that can perform conversion.
#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_release_conversion_pool() {
    snow_capture::release_conversion_pool();
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
    match CaptureSystem::builder()
        .with_backend_kind(capture_backend)
        .build()
    {
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
            let ok = snow_capture_desktop_session_prepare(session);
            if ok != 0 {
                clear_last_error();
            }
            ok
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_desktop_session_release_idle_resources(
    session: *mut SnowCaptureDesktopSessionImpl,
) -> u8 {
    let Some(session) = session_mut(session) else {
        return 0;
    };

    let receivers = match session
        .workers
        .iter()
        .map(MonitorWorker::request_release_idle_resources)
        .collect::<Result<Vec<_>, _>>()
    {
        Ok(receivers) => receivers,
        Err(error) => {
            set_last_error(error);
            return 0;
        }
    };

    for receiver in receivers {
        match receiver.recv() {
            Ok(Ok(())) => {}
            Ok(Err(error)) => {
                set_last_error(error);
                return 0;
            }
            Err(_) => {
                set_last_error("capture worker stopped before idle release completed");
                return 0;
            }
        }
    }

    clear_last_error();
    session.prepared = false;
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_desktop_session_capture_all(
    session: *mut SnowCaptureDesktopSessionImpl,
) -> *mut SnowCaptureSnapshotImpl {
    let Some(session) = session_mut(session) else {
        return ptr::null_mut();
    };

    let frames = match capture_all_frames_with_layout_retry(session) {
        Ok(frames) => frames,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };

    session.prepared = true;
    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureSnapshotImpl { frames }))
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_cancellation_token_create() -> *mut SnowCaptureCancellationTokenImpl
{
    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureCancellationTokenImpl {
        canceled: Arc::new(AtomicBool::new(false)),
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_cancellation_token_cancel(
    token: *mut SnowCaptureCancellationTokenImpl,
) {
    if !token.is_null() {
        unsafe { &*token }.canceled.store(true, Ordering::Release);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_cancellation_token_is_canceled(
    token: *const SnowCaptureCancellationTokenImpl,
) -> u8 {
    if token.is_null() {
        return 0;
    }
    u8::from(unsafe { &*token }.canceled.load(Ordering::Acquire))
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
pub unsafe extern "C" fn snow_capture_desktop_session_capture_v1(
    session: *mut SnowCaptureDesktopSessionImpl,
    request: *const SnowCaptureScreenshotRequestV1,
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
    let is_canceled = || {
        canceled
            .as_ref()
            .is_some_and(|state| state.load(Ordering::Acquire))
    };
    if is_canceled() {
        set_last_error("screenshot capture canceled");
        return ptr::null_mut();
    }

    if request.flags & SCREENSHOT_REQUEST_REFRESH_LAYOUT != 0
        && let Err(error) = rebuild_workers(session)
    {
        set_last_error(error);
        return ptr::null_mut();
    }

    let focused_window_worker = if request.focused_window != 0 {
        let hwnd = request.focused_window;
        let options = session.options;
        let canceled = canceled.clone();
        match thread::Builder::new()
            .name("snow-capture-window-once".to_owned())
            .spawn(move || {
                if canceled
                    .as_ref()
                    .is_some_and(|state| state.load(Ordering::Acquire))
                {
                    return Err("screenshot capture canceled".to_owned());
                }
                let result = capture_window_snapshot(hwnd, options);
                if canceled
                    .as_ref()
                    .is_some_and(|state| state.load(Ordering::Acquire))
                {
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

    let frames_result = capture_all_frames_with_layout_retry(session);
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
    let options = match snapshot_options(config.capture_retry_count, config.wgc_update_mode) {
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
    let rgba = session.frame.as_rgba_bytes();
    unsafe {
        *out_info = SnowCaptureRegionFrameInfo {
            width: session.frame.width(),
            height: session.frame.height(),
            stride_bytes,
            is_duplicate: u8::from(session.frame.metadata().is_duplicate()),
            reserved0: [0; 3],
            rgba_bytes: rgba.as_ptr(),
            rgba_len: rgba.len(),
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
    let options = match snapshot_options(config.capture_retry_count, config.wgc_update_mode) {
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
    let target = CaptureTarget::Window(WindowId::from_raw_handle(config.hwnd));
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

    let stride_bytes = match session.frame.width().checked_mul(4) {
        Some(stride) => stride,
        None => {
            set_last_error("window frame stride overflow");
            return 0;
        }
    };
    let rgba = session.frame.as_rgba_bytes();
    let target_info = match session
        .session
        .target_info_for_backend(session.frame.metadata().backend_kind())
    {
        Ok(info) => info,
        Err(error) => {
            set_last_error(error);
            return 0;
        }
    };
    unsafe {
        *out_info = SnowCaptureWindowFrameInfo {
            x: target_info.origin_x,
            y: target_info.origin_y,
            width: session.frame.width(),
            height: session.frame.height(),
            stride_bytes,
            rgba_bytes: rgba.as_ptr(),
            rgba_len: rgba.len(),
        };
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
    if session.frame.as_rgba_bytes().is_empty() {
        set_last_error("window session has no captured frame");
        return ptr::null_mut();
    }
    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureFrameLeaseImpl {
        _frame: Arc::new(session.frame.clone()),
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_snapshot_count(snapshot: *const SnowCaptureSnapshotImpl) -> usize {
    snapshot_ref(snapshot).map_or(0, |snapshot| snapshot.frames.len())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_snapshot_frame_info(
    snapshot: *const SnowCaptureSnapshotImpl,
    index: usize,
    out_info: *mut SnowCaptureFrameInfo,
) -> u8 {
    if out_info.is_null() {
        set_last_error("out_info is null");
        return 0;
    }
    let Some(snapshot) = snapshot_ref(snapshot) else {
        return 0;
    };
    let Some(frame) = snapshot.frames.get(index) else {
        set_last_error("snapshot frame index is out of range");
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
pub extern "C" fn snow_capture_snapshot_frame_retain(
    snapshot: *const SnowCaptureSnapshotImpl,
    index: usize,
) -> *mut SnowCaptureFrameLeaseImpl {
    let Some(snapshot) = snapshot_ref(snapshot) else {
        return ptr::null_mut();
    };
    let Some(frame) = snapshot.frames.get(index) else {
        set_last_error("snapshot frame index is out of range");
        return ptr::null_mut();
    };

    clear_last_error();
    Box::into_raw(Box::new(SnowCaptureFrameLeaseImpl {
        _frame: Arc::clone(&frame.frame),
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_screenshot_result_display_count(
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
pub extern "C" fn snow_capture_screenshot_result_display_retain(
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
pub unsafe extern "C" fn snow_capture_screenshot_result_focused_window_info_v1(
    result: *const SnowCaptureScreenshotResultImpl,
    out_info: *mut SnowCaptureWindowFrameInfoV1,
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
    if let Err(error) = write_window_frame_info_v1(frame, out_info) {
        set_last_error(error);
        return 0;
    }
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_screenshot_result_focused_window_retain(
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
pub unsafe extern "C" fn snow_capture_snapshot_destroy(snapshot: *mut SnowCaptureSnapshotImpl) {
    if !snapshot.is_null() {
        drop(unsafe { Box::from_raw(snapshot) });
    }
}

fn recording_session_mut<'a>(
    session: *mut SnowCaptureRecordingSessionImpl,
) -> Option<&'a mut SnowCaptureRecordingSessionImpl> {
    if session.is_null() {
        set_last_error("recording session is null");
        None
    } else {
        Some(unsafe { &mut *session })
    }
}

fn recording_session_ref<'a>(
    session: *const SnowCaptureRecordingSessionImpl,
) -> Option<&'a SnowCaptureRecordingSessionImpl> {
    if session.is_null() {
        set_last_error("recording session is null");
        None
    } else {
        Some(unsafe { &*session })
    }
}

fn path_from_utf8(value: *const c_char, label: &str) -> Result<PathBuf, String> {
    if value.is_null() {
        return Err(format!("{label} is null"));
    }
    let value = unsafe { CStr::from_ptr(value) }
        .to_str()
        .map_err(|_| format!("{label} is not valid UTF-8"))?;
    if value.is_empty() {
        return Err(format!("{label} is empty"));
    }
    Ok(PathBuf::from(value))
}

fn ffi_recording_state(state: RecordingState) -> SnowCaptureRecordingState {
    match state {
        RecordingState::Created => SnowCaptureRecordingState::Created,
        RecordingState::Running => SnowCaptureRecordingState::Running,
        RecordingState::Paused => SnowCaptureRecordingState::Paused,
        RecordingState::Stopped => SnowCaptureRecordingState::Stopped,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_recording_session_create(
    config: *const SnowCaptureRecordingConfig,
) -> *mut SnowCaptureRecordingSessionImpl {
    if config.is_null() {
        set_last_error("recording config is null");
        return ptr::null_mut();
    }
    let config = unsafe { &*config };
    if config.width == 0 || config.height == 0 {
        set_last_error("recording region must have a non-zero width and height");
        return ptr::null_mut();
    }
    if config.width % 2 != 0 || config.height % 2 != 0 {
        set_last_error("recording region width and height must be even");
        return ptr::null_mut();
    }
    if config.fps == 0 {
        set_last_error("recording fps must be greater than zero");
        return ptr::null_mut();
    }
    let capture_backend = match parse_capture_backend(config.capture_backend) {
        Ok(backend) => backend,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let output_dir = match path_from_utf8(config.working_directory_utf8, "working directory") {
        Ok(path) => path,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };

    let audio = RecordingAudioConfig {
        tracks: vec![
            RecordingAudioTrackConfig {
                enabled: config.enable_system_audio != 0,
                ..RecordingAudioTrackConfig::system_default("system")
            },
            RecordingAudioTrackConfig {
                enabled: config.enable_microphone != 0,
                ..RecordingAudioTrackConfig::microphone_default("microphone")
            },
        ],
        ..RecordingAudioConfig::default()
    };
    let recording_config = RecordingConfig {
        target: RecordingTarget::Region(RecordingRegion::new(
            config.x,
            config.y,
            config.width,
            config.height,
        )),
        capture_backend,
        output_dir,
        fps: config.fps,
        video: VideoEncodeConfig {
            quality: 80,
            speed: VideoEncodingSpeed::UltraFast,
        },
        audio,
        ..RecordingConfig::default()
    };

    match RecordingSession::create(recording_config) {
        Ok(recording) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowCaptureRecordingSessionImpl {
                recording: Some(recording),
                state: RecordingState::Created,
            }))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_recording_session_destroy(
    session: *mut SnowCaptureRecordingSessionImpl,
) {
    if session.is_null() {
        return;
    }
    let mut session = unsafe { Box::from_raw(session) };
    if matches!(
        session.state,
        RecordingState::Running | RecordingState::Paused
    ) && let Some(recording) = session.recording.take()
    {
        let _ = recording.stop();
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_recording_session_start(
    session: *mut SnowCaptureRecordingSessionImpl,
) -> u8 {
    let Some(session) = recording_session_mut(session) else {
        return 0;
    };
    let Some(recording) = session.recording.as_mut() else {
        set_last_error("recording session has already stopped");
        return 0;
    };
    match recording.start() {
        Ok(()) => {
            session.state = RecordingState::Running;
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
pub extern "C" fn snow_capture_recording_session_pause(
    session: *mut SnowCaptureRecordingSessionImpl,
) -> u8 {
    let Some(session) = recording_session_mut(session) else {
        return 0;
    };
    let Some(recording) = session.recording.as_ref() else {
        set_last_error("recording session has already stopped");
        return 0;
    };
    match recording.pause() {
        Ok(()) => {
            session.state = RecordingState::Paused;
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
pub extern "C" fn snow_capture_recording_session_resume(
    session: *mut SnowCaptureRecordingSessionImpl,
) -> u8 {
    let Some(session) = recording_session_mut(session) else {
        return 0;
    };
    let Some(recording) = session.recording.as_ref() else {
        set_last_error("recording session has already stopped");
        return 0;
    };
    match recording.resume() {
        Ok(()) => {
            session.state = RecordingState::Running;
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
pub unsafe extern "C" fn snow_capture_recording_session_state(
    session: *const SnowCaptureRecordingSessionImpl,
    out_state: *mut SnowCaptureRecordingState,
) -> u8 {
    if out_state.is_null() {
        set_last_error("recording out_state is null");
        return 0;
    }
    let Some(session) = recording_session_ref(session) else {
        return 0;
    };
    unsafe { *out_state = ffi_recording_state(session.state) };
    clear_last_error();
    1
}

#[derive(Debug)]
struct RecordingExportOptions {
    output_path: PathBuf,
    format: ExportFormat,
    maximum_width: Option<u32>,
    maximum_height: Option<u32>,
    target_fps: Option<u32>,
    codec: VideoCodec,
    preset: VideoEncodingSpeed,
}

fn legacy_recording_export_options(
    output_path: PathBuf,
    export_gif: bool,
) -> RecordingExportOptions {
    RecordingExportOptions {
        output_path,
        format: if export_gif {
            ExportFormat::Gif
        } else {
            ExportFormat::Mp4
        },
        maximum_width: None,
        maximum_height: None,
        target_fps: None,
        codec: VideoCodec::H264,
        preset: VideoEncodingSpeed::UltraFast,
    }
}

fn parse_recording_export_config(
    config: &SnowCaptureRecordingExportConfig,
) -> Result<RecordingExportOptions, String> {
    if config.version != RECORDING_EXPORT_CONFIG_VERSION {
        return Err(format!(
            "unsupported recording export config version: {}",
            config.version
        ));
    }
    if config.struct_size < RECORDING_EXPORT_CONFIG_V1_SIZE {
        return Err("recording export config is smaller than version 1".to_string());
    }
    if (config.maximum_width == 0) != (config.maximum_height == 0) {
        return Err(
            "recording export maximum_width and maximum_height must both be zero or non-zero"
                .to_string(),
        );
    }

    let output_path = path_from_utf8(config.output_file_utf8, "output file")?;
    let format = match config.format {
        0 => ExportFormat::Mp4,
        1 => ExportFormat::Gif,
        2 => ExportFormat::Apng,
        3 => ExportFormat::Webp,
        value => return Err(format!("invalid recording export format: {value}")),
    };
    let codec = match config.codec {
        0 => VideoCodec::H264,
        1 => VideoCodec::H265,
        value => return Err(format!("invalid recording video codec: {value}")),
    };
    let preset = match config.preset {
        0 => VideoEncodingSpeed::UltraFast,
        1 => VideoEncodingSpeed::VeryFast,
        2 => VideoEncodingSpeed::Medium,
        3 => VideoEncodingSpeed::VerySlow,
        4 => VideoEncodingSpeed::Placebo,
        value => return Err(format!("invalid recording encoding preset: {value}")),
    };

    Ok(RecordingExportOptions {
        output_path,
        format,
        maximum_width: (config.maximum_width != 0).then_some(config.maximum_width),
        maximum_height: (config.maximum_height != 0).then_some(config.maximum_height),
        target_fps: (config.target_fps != 0).then_some(config.target_fps),
        codec,
        preset,
    })
}

unsafe fn read_recording_export_config(
    config: *const SnowCaptureRecordingExportConfig,
) -> Result<SnowCaptureRecordingExportConfig, String> {
    if config.is_null() {
        return Err("recording export config is null".to_string());
    }

    // Read only the fixed header until the caller-provided size has been
    // validated. This keeps undersized future/foreign-language inputs from
    // being dereferenced as a complete version 1 structure.
    let header = unsafe {
        std::ptr::read_unaligned(config.cast::<SnowCaptureRecordingExportConfigHeader>())
    };
    if header.version != RECORDING_EXPORT_CONFIG_VERSION {
        return Err(format!(
            "unsupported recording export config version: {}",
            header.version
        ));
    }
    if header.struct_size < RECORDING_EXPORT_CONFIG_V1_SIZE {
        return Err("recording export config is smaller than version 1".to_string());
    }

    Ok(unsafe { std::ptr::read_unaligned(config) })
}

fn configure_recording_export_request(
    mut request: ExportRequest,
    options: RecordingExportOptions,
) -> ExportRequest {
    request.output_path = options.output_path;
    request.format = options.format;
    request.maximum_width = options.maximum_width;
    request.maximum_height = options.maximum_height;
    request.target_fps = options.target_fps;
    request.codec = options.codec;
    request.video.speed = options.preset;
    request.mouse.visible = true;
    for track in &mut request.audio_tracks {
        track.enabled = true;
    }
    request
}

fn stop_and_export_recording(
    session: *mut SnowCaptureRecordingSessionImpl,
    options: RecordingExportOptions,
) -> u8 {
    let Some(session) = recording_session_mut(session) else {
        return 0;
    };
    let Some(recording) = session.recording.take() else {
        set_last_error("recording session has already stopped");
        return 0;
    };

    let artifact = match recording.stop() {
        Ok(artifact) => artifact,
        Err(error) => {
            session.state = RecordingState::Stopped;
            set_last_error(error);
            return 0;
        }
    };
    session.state = RecordingState::Stopped;

    let bundle_path = artifact.bundle_path.clone();
    let editing = match EditingSession::open(artifact) {
        Ok(editing) => editing,
        Err(error) => {
            let _ = std::fs::remove_file(bundle_path);
            set_last_error(error);
            return 0;
        }
    };
    let request = configure_recording_export_request(editing.export_request(), options);

    let result = editing.export(request);
    let _ = std::fs::remove_file(bundle_path);
    match result {
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
pub extern "C" fn snow_capture_recording_session_stop_and_export(
    session: *mut SnowCaptureRecordingSessionImpl,
    output_file_utf8: *const c_char,
    export_gif: u8,
) -> u8 {
    let output_path = match path_from_utf8(output_file_utf8, "output file") {
        Ok(path) => path,
        Err(error) => {
            set_last_error(error);
            return 0;
        }
    };
    stop_and_export_recording(
        session,
        legacy_recording_export_options(output_path, export_gif != 0),
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_recording_session_stop_and_export_v1(
    session: *mut SnowCaptureRecordingSessionImpl,
    config: *const SnowCaptureRecordingExportConfig,
) -> u8 {
    let config = match unsafe { read_recording_export_config(config) } {
        Ok(config) => config,
        Err(error) => {
            set_last_error(error);
            return 0;
        }
    };
    let options = match parse_recording_export_config(&config) {
        Ok(options) => options,
        Err(error) => {
            set_last_error(error);
            return 0;
        }
    };
    stop_and_export_recording(session, options)
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_last_error_message() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

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

    fn test_snapshot() -> *mut SnowCaptureSnapshotImpl {
        let frame = Frame::from_rgba8(2, 2, vec![7; 16]).expect("valid test frame");
        Box::into_raw(Box::new(SnowCaptureSnapshotImpl {
            frames: vec![SnapshotFrame {
                entry: test_entry(),
                frame: Arc::new(frame),
            }],
        }))
    }

    #[test]
    fn immediate_gif_export_includes_recorded_cursor_motion() {
        let output_path = PathBuf::from("recording.gif");
        let request = configure_recording_export_request(
            ExportRequest::default(),
            legacy_recording_export_options(output_path.clone(), true),
        );

        assert_eq!(request.output_path, output_path);
        assert_eq!(request.format, ExportFormat::Gif);
        assert!(request.mouse.visible);
    }

    #[test]
    fn versioned_export_config_maps_all_fields() {
        let output = CString::new("recording.webp").unwrap();
        let config = SnowCaptureRecordingExportConfig {
            version: RECORDING_EXPORT_CONFIG_VERSION,
            struct_size: std::mem::size_of::<SnowCaptureRecordingExportConfig>() as u32,
            output_file_utf8: output.as_ptr(),
            format: 3,
            maximum_width: 1280,
            maximum_height: 720,
            target_fps: 24,
            codec: 1,
            preset: 4,
            reserved: [0; 32],
        };
        let options = parse_recording_export_config(&config).unwrap();
        let request = configure_recording_export_request(ExportRequest::default(), options);

        assert_eq!(request.output_path, PathBuf::from("recording.webp"));
        assert_eq!(request.format, ExportFormat::Webp);
        assert_eq!(request.maximum_width, Some(1280));
        assert_eq!(request.maximum_height, Some(720));
        assert_eq!(request.target_fps, Some(24));
        assert_eq!(request.codec, VideoCodec::H265);
        assert_eq!(request.video.speed, VideoEncodingSpeed::Placebo);
    }

    #[test]
    fn versioned_export_config_rejects_partial_size_caps() {
        let output = CString::new("recording.mp4").unwrap();
        let config = SnowCaptureRecordingExportConfig {
            version: RECORDING_EXPORT_CONFIG_VERSION,
            struct_size: std::mem::size_of::<SnowCaptureRecordingExportConfig>() as u32,
            output_file_utf8: output.as_ptr(),
            format: 0,
            maximum_width: 1920,
            maximum_height: 0,
            target_fps: 30,
            codec: 0,
            preset: 1,
            reserved: [0; 32],
        };

        assert!(parse_recording_export_config(&config).is_err());
    }

    #[test]
    fn versioned_export_config_rejects_unknown_version_and_short_struct() {
        let output = CString::new("recording.mp4").unwrap();
        let mut config = SnowCaptureRecordingExportConfig {
            version: RECORDING_EXPORT_CONFIG_VERSION + 1,
            struct_size: std::mem::size_of::<SnowCaptureRecordingExportConfig>() as u32,
            output_file_utf8: output.as_ptr(),
            format: 0,
            maximum_width: 1920,
            maximum_height: 1080,
            target_fps: 30,
            codec: 0,
            preset: 1,
            reserved: [0; 32],
        };
        assert!(parse_recording_export_config(&config).is_err());

        config.version = RECORDING_EXPORT_CONFIG_VERSION;
        config.struct_size -= 1;
        assert!(parse_recording_export_config(&config).is_err());
    }

    #[test]
    fn versioned_export_config_reads_only_the_header_before_size_validation() {
        let short = SnowCaptureRecordingExportConfigHeader {
            version: RECORDING_EXPORT_CONFIG_VERSION,
            struct_size: std::mem::size_of::<SnowCaptureRecordingExportConfigHeader>() as u32,
        };
        let config = (&raw const short).cast::<SnowCaptureRecordingExportConfig>();

        assert!(unsafe { read_recording_export_config(config) }.is_err());
    }

    #[test]
    fn null_snapshot_info_fails() {
        let mut info = SnowCaptureFrameInfo {
            stable_id: ptr::null(),
            name: ptr::null(),
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            is_primary: 0,
            backend_kind: 0,
            reserved0: [0; 2],
            stride_bytes: 0,
            rgba_bytes: ptr::null(),
            rgba_len: 0,
        };

        let ok = unsafe { snow_capture_snapshot_frame_info(ptr::null(), 0, &mut info) };
        assert_eq!(ok, 0);
        assert!(!snow_capture_last_error_message().is_null());
    }

    #[test]
    fn snapshot_frame_info_reports_monitor_and_tight_stride() {
        let snapshot = test_snapshot();
        let mut info = SnowCaptureFrameInfo {
            stable_id: ptr::null(),
            name: ptr::null(),
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            is_primary: 0,
            backend_kind: 0,
            reserved0: [0; 2],
            stride_bytes: 0,
            rgba_bytes: ptr::null(),
            rgba_len: 0,
        };

        let ok = unsafe { snow_capture_snapshot_frame_info(snapshot, 0, &mut info) };
        assert_eq!(ok, 1);
        assert_eq!(info.x, -10);
        assert_eq!(info.y, 20);
        assert_eq!(info.width, 2);
        assert_eq!(info.height, 2);
        assert_eq!(info.stride_bytes, 8);
        assert_eq!(info.rgba_len, 16);
        assert!(!info.rgba_bytes.is_null());

        unsafe { snow_capture_snapshot_destroy(snapshot) };
    }

    #[test]
    fn frame_lease_survives_snapshot_destroy() {
        let snapshot = test_snapshot();
        let lease = snow_capture_snapshot_frame_retain(snapshot, 0);
        assert!(!lease.is_null());
        unsafe { snow_capture_snapshot_destroy(snapshot) };

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
    fn release_idle_resources_null_session_fails() {
        let ok = snow_capture_desktop_session_release_idle_resources(ptr::null_mut());
        assert_eq!(ok, 0);
        assert!(!snow_capture_last_error_message().is_null());
    }

    #[test]
    fn region_session_rejects_null_and_empty_config() {
        assert!(unsafe { snow_capture_region_session_create(ptr::null()) }.is_null());

        let config = SnowCaptureRegionSessionConfig {
            x: 0,
            y: 0,
            width: 0,
            height: 100,
            capture_retry_count: 1,
            wgc_update_mode: 0,
            capture_backend: 0,
            reserved: [0; 30],
        };
        assert!(unsafe { snow_capture_region_session_create(&config) }.is_null());
    }

    #[test]
    fn region_capture_rejects_null_handles() {
        let mut info = SnowCaptureRegionFrameInfo {
            width: 0,
            height: 0,
            stride_bytes: 0,
            is_duplicate: 0,
            reserved0: [0; 3],
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
            reserved: [0; 30],
        };
        assert!(unsafe { snow_capture_window_session_create(&config) }.is_null());
    }

    #[test]
    fn window_capture_rejects_null_handles() {
        let mut info = SnowCaptureWindowFrameInfo {
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            stride_bytes: 0,
            rgba_bytes: ptr::null(),
            rgba_len: 0,
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
    fn odd_recording_region_is_rejected_before_session_creation() {
        let config = SnowCaptureRecordingConfig {
            x: 0,
            y: 0,
            width: 801,
            height: 451,
            fps: 60,
            enable_microphone: 0,
            enable_system_audio: 0,
            capture_backend: 0,
            reserved0: 0,
            working_directory_utf8: ptr::null(),
            reserved: [0; 32],
        };

        let session = unsafe { snow_capture_recording_session_create(&config) };
        assert!(session.is_null());
        let error = unsafe { CStr::from_ptr(snow_capture_last_error_message()) };
        assert_eq!(
            error.to_str().expect("recording error should be UTF-8"),
            "recording region width and height must be even"
        );
    }

    #[test]
    fn release_idle_resources_empty_session_succeeds() {
        let system = CaptureSystem::builder()
            .build()
            .expect("capture system should initialize");
        let mut session = SnowCaptureDesktopSessionImpl {
            system,
            options: CaptureOptions::default(),
            workers: Vec::new(),
            prepared: false,
        };

        let ok = snow_capture_desktop_session_release_idle_resources(&mut session);
        assert_eq!(ok, 1);
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
        assert!(parse_capture_backend(4).is_err());
        assert!(parse_capture_backend(u8::MAX).is_err());
    }

    #[test]
    fn desktop_config_selects_wgc() {
        let config = SnowCaptureDesktopSessionConfig {
            capture_retry_count: 2,
            wgc_update_mode: 1,
            capture_backend: 2,
            reserved: [0; 30],
        };

        let (options, backend) = default_options(&raw const config).unwrap();

        assert_eq!(options.capture_retry_count, 2);
        assert_eq!(options.wgc_update_mode, WgcUpdateMode::CompleteOnly);
        assert_eq!(backend, CaptureBackendKind::WindowsGraphicsCapture);
    }

    #[test]
    fn capture_config_extensions_reuse_reserved_bytes_without_growing_configs() {
        let pointer_sized_prefix = std::mem::size_of::<usize>();
        assert_eq!(
            std::mem::size_of::<SnowCaptureDesktopSessionConfig>(),
            pointer_sized_prefix + 32
        );
        assert_eq!(
            std::mem::size_of::<SnowCaptureWindowSessionConfig>(),
            pointer_sized_prefix * 2 + 32
        );
        assert_eq!(
            std::mem::size_of::<SnowCaptureRegionSessionConfig>(),
            16 + pointer_sized_prefix + 32
        );
        assert_eq!(
            std::mem::size_of::<SnowCaptureRecordingConfig>(),
            56 + pointer_sized_prefix
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
        assert_eq!(
            SCREENSHOT_REQUEST_V1_SIZE as usize,
            std::mem::size_of::<SnowCaptureScreenshotRequestV1>()
        );
        assert_eq!(
            WINDOW_FRAME_INFO_V1_SIZE as usize,
            std::mem::size_of::<SnowCaptureWindowFrameInfoV1>()
        );
        assert_eq!(std::mem::size_of::<SnowCaptureScreenshotRequestV1>(), 64);
        assert_eq!(std::mem::size_of::<SnowCaptureWindowFrameInfoV1>(), 56);
        assert_eq!(
            std::mem::offset_of!(SnowCaptureScreenshotRequestV1, cancellation_token),
            24
        );
        assert_eq!(
            std::mem::offset_of!(SnowCaptureWindowFrameInfoV1, backend_kind),
            48
        );
    }

    #[test]
    fn versioned_screenshot_request_reads_only_header_before_size_validation() {
        let short = SnowCaptureScreenshotRequestHeader {
            version: SCREENSHOT_REQUEST_VERSION,
            struct_size: std::mem::size_of::<SnowCaptureScreenshotRequestHeader>() as u32,
        };
        let request = (&raw const short).cast::<SnowCaptureScreenshotRequestV1>();

        assert!(unsafe { read_screenshot_request(request) }.is_err());

        let unknown = SnowCaptureScreenshotRequestHeader {
            version: SCREENSHOT_REQUEST_VERSION + 1,
            struct_size: SCREENSHOT_REQUEST_V1_SIZE,
        };
        let request = (&raw const unknown).cast::<SnowCaptureScreenshotRequestV1>();
        assert!(unsafe { read_screenshot_request(request) }.is_err());
    }

    #[test]
    fn versioned_screenshot_request_rejects_unsupported_flags() {
        let request = SnowCaptureScreenshotRequestV1 {
            version: SCREENSHOT_REQUEST_VERSION,
            struct_size: SCREENSHOT_REQUEST_V1_SIZE,
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
        assert_eq!(
            unsafe { snow_capture_cancellation_token_is_canceled(ptr::null()) },
            0
        );
        let token = snow_capture_cancellation_token_create();
        assert!(!token.is_null());
        assert_eq!(
            unsafe { snow_capture_cancellation_token_is_canceled(token) },
            0
        );
        let state = unsafe { &*token }.canceled.clone();
        let token_address = token as usize;
        let cancel = std::thread::spawn(move || unsafe {
            snow_capture_cancellation_token_cancel(
                token_address as *mut SnowCaptureCancellationTokenImpl,
            );
        });
        cancel.join().expect("cancel thread should complete");
        assert!(state.load(Ordering::Acquire));
        assert_eq!(
            unsafe { snow_capture_cancellation_token_is_canceled(token) },
            1
        );
        unsafe { snow_capture_cancellation_token_destroy(token) };
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
            &[first, second]
        ));
    }
}

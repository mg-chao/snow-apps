#![allow(clippy::missing_safety_doc)]

use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char};
use std::path::PathBuf;
use std::ptr;
use std::sync::{
    Arc,
    atomic::{AtomicBool, AtomicUsize, Ordering},
    mpsc,
};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use snow_capture::color_effect::ColorCorrection;
use snow_capture::frame::{CaptureEvent, CapturePixelFormat, CapturedFrame, Frame};
use snow_capture::{
    CaptureOptions, CaptureRegion, CaptureSession, CaptureStream, CaptureStreamConfig,
    CaptureSystem, CaptureTarget, CaptureWorkload, MonitorId, MonitorLayout, WgcUpdateMode,
    WindowId,
    backend::{AutoBackendPolicy, CaptureBackendKind},
};
use snow_core::error::RecvTimeoutError;
use snow_screen_recorder::{
    DirectRecordingConfig, DirectRecordingSession, EditingSession, ExportFormat, ExportRequest,
    RecordingAudioConfig, RecordingAudioTrackConfig, RecordingConfig, RecordingRegion,
    RecordingSession, RecordingState, RecordingTarget, ScreenRecorderError, VideoCodec,
    VideoEncodeConfig, VideoEncodingSpeed,
};

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
}

pub struct SnowCaptureCancellationTokenImpl {
    canceled: Arc<AtomicBool>,
}

pub struct SnowCaptureScreenshotResultImpl {
    frames: Vec<SnapshotFrame>,
    focused_window: Option<SnapshotWindowFrame>,
}

pub struct SnowCaptureFrameLeaseImpl {
    _frame: Arc<Frame>,
}

pub struct SnowCaptureRecordingSessionImpl {
    recording: Option<RecordingSessionKind>,
    state: RecordingState,
}

enum RecordingSessionKind {
    Legacy(Box<RecordingSession>),
    Direct(DirectRecordingSession),
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

#[repr(C)]
pub struct SnowCaptureDesktopSessionConfig {
    capture_retry_count: usize,
    wgc_update_mode: u8,
    capture_backend: u8,
    pixel_format: u8,
    reserved: [u8; 29],
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

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowCaptureResult {
    Ok = 0,
    InvalidArgument = 1,
    InvalidState = 2,
    CaptureError = 3,
    EncoderError = 4,
    IoError = 5,
    Canceled = 6,
    InternalError = 255,
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
    Capture(mpsc::Sender<Result<Frame, String>>, ColorCorrection, bool),
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
pub const STREAM_CONFIG_VERSION: u32 = 1;
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
    encoder_preference: u32,
    reserved: [u8; 32],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowCaptureKeyboardLabel {
    key_code: u32,
    utf8: *const u8,
    utf8_len: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowCaptureDirectRecordingConfig {
    version: u32,
    struct_size: u32,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    capture_backend: u32,
    output_file_utf8: *const c_char,
    output_format: u32,
    capture_fps: u32,
    output_fps: u32,
    maximum_width: u32,
    maximum_height: u32,
    codec: u32,
    preset: u32,
    encoder_preference: u32,
    enable_microphone: u8,
    enable_system_audio: u8,
    show_cursor: u8,
    reserved0: u8,
    mouse_trail_rgba: u32,
    mouse_click_rgba: u32,
    reserved: [u8; 64],
    show_keyboard: u32,
    keyboard_background_rgba: u32,
    keyboard_text_rgba: u32,
    keyboard_border_rgba: u32,
    keyboard_labels: *const SnowCaptureKeyboardLabel,
    keyboard_label_count: u32,
    mouse_trail_duration_ms: u32,
    keyboard_size: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SnowCaptureDirectRecordingConfigHeader {
    version: u32,
    struct_size: u32,
}

pub const DIRECT_RECORDING_CONFIG_VERSION: u32 = 4;
const DIRECT_RECORDING_CONFIG_V1_FIELDS_SIZE: usize =
    std::mem::offset_of!(SnowCaptureDirectRecordingConfig, show_keyboard);
// The original structure has pointer alignment and may contain tail padding. Validate its
// complete sizeof, but never reinterpret those unspecified padding bytes as v2 options.
const DIRECT_RECORDING_CONFIG_V1_SIZE: u32 = (DIRECT_RECORDING_CONFIG_V1_FIELDS_SIZE
    .div_ceil(std::mem::align_of::<SnowCaptureDirectRecordingConfig>())
    * std::mem::align_of::<SnowCaptureDirectRecordingConfig>())
    as u32;

fn direct_config_size(version: u32) -> Result<u32, String> {
    match version {
        1 => Ok(DIRECT_RECORDING_CONFIG_V1_SIZE),
        2 | 3 => Ok(std::mem::offset_of!(SnowCaptureDirectRecordingConfig, keyboard_size) as u32),
        DIRECT_RECORDING_CONFIG_VERSION => Ok(DIRECT_RECORDING_CONFIG_SIZE),
        _ => Err(format!(
            "unsupported direct recording config version: {version}"
        )),
    }
}
const DIRECT_RECORDING_CONFIG_SIZE: u32 =
    std::mem::size_of::<SnowCaptureDirectRecordingConfig>() as u32;

#[repr(C)]
#[derive(Clone, Copy)]
struct SnowCaptureRecordingExportConfigHeader {
    version: u32,
    struct_size: u32,
}

pub const RECORDING_EXPORT_CONFIG_VERSION: u32 = 1;
const RECORDING_EXPORT_CONFIG_SIZE: u32 =
    std::mem::size_of::<SnowCaptureRecordingExportConfig>() as u32;

// The struct update stays load-bearing when snow-capture is built with its
// optional `stage-timing` feature, which adds a field this literal omits.
#[allow(clippy::needless_update)]
fn default_options(
    config: *const SnowCaptureDesktopSessionConfig,
) -> Result<(CaptureOptions, CaptureBackendKind), String> {
    let (capture_retry_count, wgc_update_mode, capture_backend, output_pixel_format) =
        if config.is_null() {
            (
                1,
                WgcUpdateMode::Auto,
                CaptureBackendKind::Auto,
                CapturePixelFormat::Rgba8,
            )
        } else {
            let config = unsafe { &*config };
            (
                config.capture_retry_count.max(1),
                parse_wgc_update_mode(config.wgc_update_mode)?,
                parse_capture_backend(config.capture_backend)?,
                parse_pixel_format(config.pixel_format)?,
            )
        };

    Ok((
        CaptureOptions {
            capture_retry_count,
            workload: CaptureWorkload::Snapshot,
            gpu_hdr_conversion: true,
            hdr_tonemap_lut: true,
            output_pixel_format,
            wgc_update_mode,
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
        capture_retry_count: capture_retry_count.max(1),
        workload: CaptureWorkload::Snapshot,
        gpu_hdr_conversion: true,
        hdr_tonemap_lut: true,
        output_pixel_format,
        wgc_update_mode: parse_wgc_update_mode(wgc_update_mode)?,
        // Keeps the literal valid whether or not snow-capture was built
        // with its optional `stage-timing` instrumentation feature.
        ..Default::default()
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
                        WorkerCommand::Capture(reply, correction, include_cursor) => {
                            let result = match session.as_mut() {
                                Ok(session) => {
                                    session.set_color_correction(correction);
                                    match session.capture_once() {
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
    ) -> Result<mpsc::Receiver<Result<Frame, String>>, String> {
        let (tx, rx) = mpsc::channel();
        self.tx
            .send(WorkerCommand::Capture(tx, correction, include_cursor))
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
            match MonitorWorker::start(session.system.clone(), session.options, entry) {
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
    correction: ColorCorrection,
    include_cursor: bool,
) -> Result<Vec<SnapshotFrame>, String> {
    let mut receivers = Vec::with_capacity(session.workers.len());
    let mut first_error = None;
    for worker in &session.workers {
        match worker.request_capture(correction, include_cursor) {
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
) -> Result<Vec<SnapshotFrame>, String> {
    match capture_all_frames(session, correction, include_cursor) {
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
            capture_all_frames(session, correction, include_cursor).map_err(|retry_error| {
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
        _ => c"unknown".as_ptr(),
    }
}

fn capture_window_snapshot(
    hwnd: isize,
    options: CaptureOptions,
    include_cursor: bool,
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
    let capture_result = session.capture_once();
    let mut frame = match capture_result {
        Ok(frame) => frame,
        Err(error) => {
            let _ = session.reset_to_prepared();
            return Err(error.to_string());
        }
    };
    if session.active_capture_access_count() != 0 {
        let _ = session.reset_to_prepared();
        return Err("capture access remained active after focused-window capture".to_owned());
    }
    if include_cursor {
        frame.composite_attached_cursor();
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
    let is_canceled = || {
        canceled
            .as_ref()
            .is_some_and(|state| state.load(Ordering::Acquire))
    };
    if is_canceled() {
        set_last_error("screenshot capture canceled");
        return ptr::null_mut();
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
            color_correction: correction,
            ..session.options
        };
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
                let result = capture_window_snapshot(hwnd, options, include_cursor);
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

    let frames_result = capture_all_frames_with_layout_retry(session, correction, include_cursor);
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
        capture_retry_count: config.capture_retry_count.max(1),
        workload: CaptureWorkload::Continuous,
        color_correction: if config.restore_original_colors != 0 {
            ColorCorrection::snapshot_current()
        } else {
            ColorCorrection::Disabled
        },
        gpu_hdr_conversion: true,
        hdr_tonemap_lut: true,
        output_pixel_format,
        wgc_update_mode,
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
    let options = stream_capture_options(config, output_pixel_format, wgc_update_mode);
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

// Makes the exactly-once recording session destroy contract observable: a missed
// destroy leaves the count above its baseline and a repeated destroy wraps it far
// below, so tests can fail loudly instead of leaking capture pipelines and input
// hooks silently.
static LIVE_RECORDING_SESSIONS: AtomicUsize = AtomicUsize::new(0);

/// Live recording sessions created and not yet destroyed.
#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_recording_session_live_count() -> usize {
    LIVE_RECORDING_SESSIONS.load(Ordering::Relaxed)
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
            LIVE_RECORDING_SESSIONS.fetch_add(1, Ordering::Relaxed);
            Box::into_raw(Box::new(SnowCaptureRecordingSessionImpl {
                recording: Some(RecordingSessionKind::Legacy(Box::new(recording))),
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
    LIVE_RECORDING_SESSIONS.fetch_sub(1, Ordering::Relaxed);
    let mut session = unsafe { Box::from_raw(session) };
    if matches!(
        session.state,
        RecordingState::Running | RecordingState::Paused
    ) && let Some(recording) = session.recording.take()
    {
        match recording {
            RecordingSessionKind::Legacy(recording) => {
                let _ = (*recording).stop();
            }
            RecordingSessionKind::Direct(recording) => drop(recording),
        }
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
    let result = match recording {
        RecordingSessionKind::Legacy(recording) => recording.start(),
        RecordingSessionKind::Direct(recording) => recording.start(),
    };
    match result {
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
    let result = match recording {
        RecordingSessionKind::Legacy(recording) => recording.pause(),
        RecordingSessionKind::Direct(recording) => recording.pause(),
    };
    match result {
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
    let result = match recording {
        RecordingSessionKind::Legacy(recording) => recording.resume(),
        RecordingSessionKind::Direct(recording) => recording.resume(),
    };
    match result {
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
    let state = match session.recording.as_ref() {
        Some(RecordingSessionKind::Direct(recording)) => recording.state(),
        Some(RecordingSessionKind::Legacy(_)) | None => session.state,
    };
    unsafe { *out_state = ffi_recording_state(state) };
    clear_last_error();
    1
}

fn direct_result_for_error(error: &ScreenRecorderError) -> SnowCaptureResult {
    match error {
        ScreenRecorderError::InvalidConfig(_) | ScreenRecorderError::UnsupportedFeature(_) => {
            SnowCaptureResult::InvalidArgument
        }
        ScreenRecorderError::Capture(_) => SnowCaptureResult::CaptureError,
        ScreenRecorderError::Audio(_) | ScreenRecorderError::Encode(_) => {
            SnowCaptureResult::EncoderError
        }
        ScreenRecorderError::Io(_) => SnowCaptureResult::IoError,
        ScreenRecorderError::ExportCanceled => SnowCaptureResult::Canceled,
        ScreenRecorderError::Decode(_) | ScreenRecorderError::Export(_) => {
            SnowCaptureResult::EncoderError
        }
    }
}

fn packed_rgba(value: u32) -> [u8; 4] {
    [
        (value >> 24) as u8,
        (value >> 16) as u8,
        (value >> 8) as u8,
        value as u8,
    ]
}

fn parse_keyboard_config(
    config: &SnowCaptureDirectRecordingConfig,
) -> Result<Option<snow_screen_recorder::KeyboardOverlayConfig>, String> {
    if config.version == 1 {
        return Ok(None);
    }
    if config.show_keyboard > 1
        || (config.version == 2 && config.mouse_trail_duration_ms != 0)
        || (config.version >= 4 && !(32..=128).contains(&config.keyboard_size))
        || config.keyboard_label_count > 256
    {
        return Err("invalid keyboard recording options".into());
    }
    if config.show_keyboard == 0 {
        return Ok(None);
    }
    if config.keyboard_label_count != 0 && config.keyboard_labels.is_null() {
        return Err("keyboard labels pointer is null".into());
    }
    let mut labels = std::collections::BTreeMap::new();
    for index in 0..config.keyboard_label_count as usize {
        let label = unsafe { std::ptr::read_unaligned(config.keyboard_labels.add(index)) };
        if label.key_code > 255
            || label.utf8_len == 0
            || label.utf8_len > 128
            || label.utf8.is_null()
        {
            return Err("invalid keyboard label".into());
        }
        let bytes = unsafe { std::slice::from_raw_parts(label.utf8, label.utf8_len as usize) };
        let text = std::str::from_utf8(bytes).map_err(|_| "keyboard label is not UTF-8")?;
        if text.trim().is_empty() || text.chars().any(char::is_control) {
            return Err("invalid keyboard label text".into());
        }
        if labels
            .insert(label.key_code as u16, text.to_owned())
            .is_some()
        {
            return Err("duplicate keyboard label".into());
        }
    }
    Ok(Some(snow_screen_recorder::KeyboardOverlayConfig {
        keycap_size: if config.version < 4 {
            64
        } else {
            config.keyboard_size
        },
        background_rgba: packed_rgba(config.keyboard_background_rgba),
        text_rgba: packed_rgba(config.keyboard_text_rgba),
        border_rgba: packed_rgba(config.keyboard_border_rgba),
        labels,
    }))
}

fn parse_direct_recording_config(
    config: &SnowCaptureDirectRecordingConfig,
) -> Result<DirectRecordingConfig, String> {
    let required_size = direct_config_size(config.version)?;
    if config.struct_size < required_size {
        return Err(format!(
            "direct recording config is too small: {} < {}",
            config.struct_size, required_size
        ));
    }
    if config.reserved0 != 0 || config.reserved.iter().any(|byte| *byte != 0) {
        return Err("direct recording reserved fields must be zero".to_string());
    }
    if config.width == 0 || config.height == 0 {
        return Err("direct recording region must have non-zero dimensions".to_string());
    }
    if config.capture_fps == 0 || config.output_fps == 0 {
        return Err("direct recording frame rates must be greater than zero".to_string());
    }
    if (config.maximum_width == 0) != (config.maximum_height == 0) {
        return Err(
            "direct recording maximum dimensions must both be zero or both be non-zero".to_string(),
        );
    }
    if config.enable_microphone > 1 || config.enable_system_audio > 1 || config.show_cursor > 1 {
        return Err("direct recording boolean fields must be zero or one".to_string());
    }
    let capture_backend = parse_capture_backend(
        u8::try_from(config.capture_backend)
            .map_err(|_| format!("invalid capture backend: {}", config.capture_backend))?,
    )?;
    let output_path = path_from_utf8(config.output_file_utf8, "direct recording output file")?;
    let format = match config.output_format {
        0 => ExportFormat::Mp4,
        1 => ExportFormat::Gif,
        2 => ExportFormat::Apng,
        3 => ExportFormat::Webp,
        value => return Err(format!("invalid direct recording output format: {value}")),
    };
    let codec = match config.codec {
        0 => VideoCodec::H264,
        1 => VideoCodec::H265,
        value => return Err(format!("invalid direct recording video codec: {value}")),
    };
    let preset = match config.preset {
        0 => VideoEncodingSpeed::UltraFast,
        1 => VideoEncodingSpeed::VeryFast,
        2 => VideoEncodingSpeed::Medium,
        3 => VideoEncodingSpeed::VerySlow,
        4 => VideoEncodingSpeed::Placebo,
        value => return Err(format!("invalid direct recording encoding preset: {value}")),
    };
    let prefer_hardware_encoder = match config.encoder_preference {
        0 => false,
        1 => true,
        value => {
            return Err(format!(
                "invalid direct recording encoder preference: {value}"
            ));
        }
    };

    let direct = DirectRecordingConfig {
        region: RecordingRegion::new(config.x, config.y, config.width, config.height),
        capture_backend,
        output_path,
        format,
        capture_fps: config.capture_fps,
        output_fps: config.output_fps,
        maximum_width: (config.maximum_width != 0).then_some(config.maximum_width),
        maximum_height: (config.maximum_height != 0).then_some(config.maximum_height),
        codec,
        preset,
        prefer_hardware_encoder,
        enable_microphone: config.enable_microphone != 0 && !format.is_animated_image(),
        enable_system_audio: config.enable_system_audio != 0 && !format.is_animated_image(),
        show_cursor: config.show_cursor != 0,
        keyboard: parse_keyboard_config(config)?,
        mouse_trail_rgba: packed_rgba(config.mouse_trail_rgba),
        mouse_trail_duration_ms: if config.version < 3 {
            500
        } else {
            u64::from(config.mouse_trail_duration_ms)
        },
        mouse_click_rgba: packed_rgba(config.mouse_click_rgba),
    };
    direct.validate()?;
    Ok(direct)
}

unsafe fn read_direct_recording_config(
    config: *const SnowCaptureDirectRecordingConfig,
) -> Result<SnowCaptureDirectRecordingConfig, String> {
    if config.is_null() {
        return Err("direct recording config is null".to_string());
    }
    let header = unsafe {
        std::ptr::read_unaligned(config.cast::<SnowCaptureDirectRecordingConfigHeader>())
    };
    let required_size = direct_config_size(header.version)?;
    if header.struct_size < required_size {
        return Err(format!(
            "direct recording config is too small: {} < {}",
            header.struct_size, required_size
        ));
    }
    // Zero-extension preserves the v1 ABI without reading past the caller's allocation.
    let mut value: SnowCaptureDirectRecordingConfig = unsafe { std::mem::zeroed() };
    let copy_size = if header.version == 1 {
        DIRECT_RECORDING_CONFIG_V1_FIELDS_SIZE
    } else {
        required_size as usize
    };
    unsafe {
        std::ptr::copy_nonoverlapping(
            config.cast::<u8>(),
            (&raw mut value).cast::<u8>(),
            copy_size,
        );
    }
    Ok(value)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_recording_output_dimensions(
    width: u32,
    height: u32,
    maximum_width: u32,
    maximum_height: u32,
    format: u32,
    output_width: *mut u32,
    output_height: *mut u32,
) -> i32 {
    if width == 0
        || height == 0
        || output_width.is_null()
        || output_height.is_null()
        || (maximum_width == 0) != (maximum_height == 0)
    {
        return 0;
    }
    let format = match format {
        0 => ExportFormat::Mp4,
        1 => ExportFormat::Gif,
        2 => ExportFormat::Apng,
        3 => ExportFormat::Webp,
        _ => return 0,
    };
    let (w, h) = snow_screen_recorder::scaled_output_dimensions(
        width,
        height,
        (maximum_width != 0).then_some(maximum_width),
        (maximum_height != 0).then_some(maximum_height),
        format,
    );
    unsafe {
        *output_width = w;
        *output_height = h;
    }
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_recording_session_create_direct(
    config: *const SnowCaptureDirectRecordingConfig,
    out_session: *mut *mut SnowCaptureRecordingSessionImpl,
) -> SnowCaptureResult {
    if out_session.is_null() {
        set_last_error("direct recording out_session is null");
        return SnowCaptureResult::InvalidArgument;
    }
    unsafe { *out_session = ptr::null_mut() };
    let config = match unsafe { read_direct_recording_config(config) }
        .and_then(|config| parse_direct_recording_config(&config))
    {
        Ok(config) => config,
        Err(error) => {
            set_last_error(error);
            return SnowCaptureResult::InvalidArgument;
        }
    };
    match DirectRecordingSession::create(config) {
        Ok(recording) => {
            LIVE_RECORDING_SESSIONS.fetch_add(1, Ordering::Relaxed);
            unsafe {
                *out_session = Box::into_raw(Box::new(SnowCaptureRecordingSessionImpl {
                    recording: Some(RecordingSessionKind::Direct(recording)),
                    state: RecordingState::Created,
                }));
            }
            clear_last_error();
            SnowCaptureResult::Ok
        }
        Err(error) => {
            let result = direct_result_for_error(&error);
            set_last_error(error);
            result
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_recording_session_stop(
    session: *mut SnowCaptureRecordingSessionImpl,
) -> SnowCaptureResult {
    let Some(session) = recording_session_mut(session) else {
        return SnowCaptureResult::InvalidArgument;
    };
    if !matches!(
        session.state,
        RecordingState::Running | RecordingState::Paused
    ) {
        set_last_error("direct recording stop requires a running or paused session");
        return SnowCaptureResult::InvalidState;
    }
    if !matches!(session.recording, Some(RecordingSessionKind::Direct(_))) {
        set_last_error("recording session was not created with create_direct");
        return SnowCaptureResult::InvalidState;
    }
    let Some(RecordingSessionKind::Direct(recording)) = session.recording.take() else {
        set_last_error("direct recording session is unavailable");
        return SnowCaptureResult::InternalError;
    };
    session.state = RecordingState::Stopped;
    match recording.stop() {
        Ok(_) => {
            clear_last_error();
            SnowCaptureResult::Ok
        }
        Err(error) => {
            let result = direct_result_for_error(&error);
            set_last_error(error);
            result
        }
    }
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
    prefer_hardware_h264: bool,
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
    if config.struct_size < RECORDING_EXPORT_CONFIG_SIZE {
        return Err("recording export config is too small".to_string());
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
    let prefer_hardware_h264 = match config.encoder_preference {
        0 => false,
        1 => true,
        value => return Err(format!("invalid recording encoder preference: {value}")),
    };

    Ok(RecordingExportOptions {
        output_path,
        format,
        maximum_width: (config.maximum_width != 0).then_some(config.maximum_width),
        maximum_height: (config.maximum_height != 0).then_some(config.maximum_height),
        target_fps: (config.target_fps != 0).then_some(config.target_fps),
        codec,
        preset,
        prefer_hardware_h264,
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
    // being dereferenced as a complete structure.
    let header = unsafe {
        std::ptr::read_unaligned(config.cast::<SnowCaptureRecordingExportConfigHeader>())
    };
    if header.version != RECORDING_EXPORT_CONFIG_VERSION {
        return Err(format!(
            "unsupported recording export config version: {}",
            header.version
        ));
    }
    if header.struct_size < RECORDING_EXPORT_CONFIG_SIZE {
        return Err("recording export config is too small".to_string());
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
    request.prefer_hardware_h264 = options.prefer_hardware_h264;
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
    if !matches!(session.recording, Some(RecordingSessionKind::Legacy(_))) {
        set_last_error("direct recording sessions must be finalized with recording_session_stop");
        return 0;
    }
    let Some(RecordingSessionKind::Legacy(recording)) = session.recording.take() else {
        set_last_error("recording session has already stopped");
        return 0;
    };

    let artifact = match (*recording).stop() {
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
pub unsafe extern "C" fn snow_capture_recording_session_stop_and_export(
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
    #[test]
    fn recording_preview_output_dimensions_match_export_formats() {
        for format in 0..=3 {
            let mut width = 0;
            let mut height = 0;
            assert_eq!(
                unsafe {
                    super::snow_capture_recording_output_dimensions(
                        129,
                        131,
                        0,
                        0,
                        format,
                        &mut width,
                        &mut height,
                    )
                },
                1
            );
            assert_eq!(
                (width, height),
                if format == 0 { (128, 130) } else { (129, 131) }
            );
        }
        let mut width = 0;
        let mut height = 0;
        assert_eq!(
            unsafe {
                super::snow_capture_recording_output_dimensions(
                    3840,
                    2160,
                    1920,
                    1080,
                    0,
                    &mut width,
                    &mut height,
                )
            },
            1
        );
        assert_eq!((width, height), (1920, 1080));
        assert_eq!(
            unsafe {
                super::snow_capture_recording_output_dimensions(
                    100,
                    100,
                    1920,
                    0,
                    0,
                    &mut width,
                    &mut height,
                )
            },
            0
        );
        assert_eq!(
            unsafe {
                super::snow_capture_recording_output_dimensions(
                    100,
                    100,
                    0,
                    0,
                    9,
                    &mut width,
                    &mut height,
                )
            },
            0
        );
    }

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

    #[test]
    fn immediate_gif_export_includes_recorded_cursor_motion() {
        let output_path = PathBuf::from("recording.gif");
        let request = configure_recording_export_request(
            ExportRequest::default(),
            RecordingExportOptions {
                output_path: output_path.clone(),
                format: ExportFormat::Gif,
                maximum_width: None,
                maximum_height: None,
                target_fps: None,
                codec: VideoCodec::H264,
                preset: VideoEncodingSpeed::UltraFast,
                prefer_hardware_h264: false,
            },
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
            encoder_preference: 1,
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
        assert!(request.prefer_hardware_h264);
    }

    #[test]
    fn versioned_export_config_rejects_unknown_encoder_preference() {
        let output = CString::new("recording.mp4").unwrap();
        let config = SnowCaptureRecordingExportConfig {
            version: RECORDING_EXPORT_CONFIG_VERSION,
            struct_size: std::mem::size_of::<SnowCaptureRecordingExportConfig>() as u32,
            output_file_utf8: output.as_ptr(),
            format: 0,
            maximum_width: 0,
            maximum_height: 0,
            target_fps: 30,
            codec: 0,
            preset: 1,
            encoder_preference: 2,
            reserved: [0; 32],
        };

        assert!(parse_recording_export_config(&config).is_err());
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
            encoder_preference: 0,
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
            encoder_preference: 0,
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
    fn direct_duration_versions_and_bounds() {
        let output = CString::new("recording.mp4").unwrap();
        let mut raw = direct_config(&output);
        for duration in [100, 500, 2000] {
            raw.mouse_trail_duration_ms = duration;
            assert_eq!(
                parse_direct_recording_config(&raw)
                    .unwrap()
                    .mouse_trail_duration_ms,
                u64::from(duration)
            );
        }
        for duration in [0, 99, 2001] {
            raw.mouse_trail_duration_ms = duration;
            assert!(parse_direct_recording_config(&raw).is_err());
        }
        for version in [1, 2] {
            raw.version = version;
            raw.mouse_trail_duration_ms = 0;
            assert_eq!(
                parse_direct_recording_config(&raw)
                    .unwrap()
                    .mouse_trail_duration_ms,
                500
            );
        }
    }

    #[test]
    fn keyboard_size_round_trips_and_legacy_configs_default() {
        let output = CString::new("keyboard-size.mp4").unwrap();
        let mut raw = direct_config(&output);
        raw.show_keyboard = 1;
        raw.keyboard_size = 96;
        assert_eq!(
            parse_keyboard_config(&raw).unwrap().unwrap().keycap_size,
            96
        );
        raw.keyboard_size = 129;
        assert!(parse_keyboard_config(&raw).is_err());
        raw.version = 3;
        assert_eq!(
            parse_keyboard_config(&raw).unwrap().unwrap().keycap_size,
            64
        );
    }

    fn direct_config(output: &CStr) -> SnowCaptureDirectRecordingConfig {
        SnowCaptureDirectRecordingConfig {
            version: DIRECT_RECORDING_CONFIG_VERSION,
            struct_size: std::mem::size_of::<SnowCaptureDirectRecordingConfig>() as u32,
            x: -100,
            y: 50,
            width: 1280,
            height: 720,
            capture_backend: 2,
            output_file_utf8: output.as_ptr(),
            output_format: 0,
            capture_fps: 30,
            output_fps: 30,
            maximum_width: 1920,
            maximum_height: 1080,
            codec: 0,
            preset: 1,
            encoder_preference: 1,
            enable_microphone: 1,
            enable_system_audio: 1,
            show_cursor: 1,
            reserved0: 0,
            mouse_trail_rgba: 0x11223344,
            mouse_click_rgba: 0xAABBCC80,
            reserved: [0; 64],
            show_keyboard: 0,
            keyboard_background_rgba: 0,
            keyboard_text_rgba: 0,
            keyboard_border_rgba: 0,
            keyboard_labels: std::ptr::null(),
            keyboard_label_count: 0,
            mouse_trail_duration_ms: 500,
            keyboard_size: 64,
        }
    }

    #[test]
    fn direct_config_maps_all_fields_and_rgba_order() {
        let output = CString::new("recording.mp4").unwrap();
        let parsed = parse_direct_recording_config(&direct_config(&output)).unwrap();
        assert_eq!(parsed.region, RecordingRegion::new(-100, 50, 1280, 720));
        assert_eq!(
            parsed.capture_backend,
            CaptureBackendKind::WindowsGraphicsCapture
        );
        assert_eq!(parsed.output_path, PathBuf::from("recording.mp4"));
        assert_eq!(parsed.format, ExportFormat::Mp4);
        assert_eq!(parsed.capture_fps, 30);
        assert_eq!(parsed.output_fps, 30);
        assert_eq!(parsed.maximum_width, Some(1920));
        assert_eq!(parsed.maximum_height, Some(1080));
        assert_eq!(parsed.codec, VideoCodec::H264);
        assert_eq!(parsed.preset, VideoEncodingSpeed::VeryFast);
        assert!(parsed.prefer_hardware_encoder);
        assert!(parsed.enable_microphone && parsed.enable_system_audio && parsed.show_cursor);
        assert_eq!(parsed.mouse_trail_rgba, [0x11, 0x22, 0x33, 0x44]);
        assert_eq!(parsed.mouse_click_rgba, [0xAA, 0xBB, 0xCC, 0x80]);
    }

    #[test]
    fn animated_direct_configs_omit_audio() {
        let output = CString::new("recording.webp").unwrap();
        let mut config = direct_config(&output);
        config.output_format = 3;
        let parsed = parse_direct_recording_config(&config).unwrap();
        assert_eq!(parsed.format, ExportFormat::Webp);
        assert!(!parsed.enable_microphone);
        assert!(!parsed.enable_system_audio);
    }

    #[test]
    fn direct_config_rejects_reserved_bytes_unknown_enums_and_invalid_sizes() {
        let output = CString::new("recording.mp4").unwrap();
        let mut config = direct_config(&output);
        config.reserved[7] = 1;
        assert!(parse_direct_recording_config(&config).is_err());
        config.reserved[7] = 0;
        config.output_format = 99;
        assert!(parse_direct_recording_config(&config).is_err());
        config.output_format = 0;
        config.maximum_height = 0;
        assert!(parse_direct_recording_config(&config).is_err());
        config.maximum_height = 1080;
        config.struct_size -= 1;
        assert!(parse_direct_recording_config(&config).is_err());
    }

    #[test]
    fn direct_config_reads_only_header_before_size_validation() {
        let short = SnowCaptureDirectRecordingConfigHeader {
            version: DIRECT_RECORDING_CONFIG_VERSION,
            struct_size: std::mem::size_of::<SnowCaptureDirectRecordingConfigHeader>() as u32,
        };
        let config = (&raw const short).cast::<SnowCaptureDirectRecordingConfig>();
        assert!(unsafe { read_direct_recording_config(config) }.is_err());
    }

    #[test]
    fn direct_config_v1_reads_exact_prefix_and_defaults_keyboard_off() {
        let output = CString::new("recording.mp4").unwrap();
        let mut config = direct_config(&output);
        config.version = 1;
        config.struct_size = DIRECT_RECORDING_CONFIG_V1_SIZE;
        config.show_keyboard = 1;
        // An allocation containing only the old ABI, not a full v2 struct.
        let prefix = unsafe {
            std::slice::from_raw_parts(
                (&raw const config).cast::<u8>(),
                DIRECT_RECORDING_CONFIG_V1_SIZE as usize,
            )
        }
        .to_vec();
        let read = unsafe { read_direct_recording_config(prefix.as_ptr().cast()) }.unwrap();
        assert_eq!(read.show_keyboard, 0);
        assert!(
            parse_direct_recording_config(&read)
                .unwrap()
                .keyboard
                .is_none()
        );
        #[cfg(target_pointer_width = "64")]
        assert_eq!(DIRECT_RECORDING_CONFIG_V1_SIZE, 152);
    }

    #[test]
    fn direct_config_keyboard_copies_labels_and_validates_the_extension() {
        let output = CString::new("recording.mp4").unwrap();
        let mut config = direct_config(&output);
        config.show_keyboard = 1;
        config.keyboard_background_rgba = 0x112233cc;
        config.keyboard_text_rgba = 0xfafafaff;
        let text = String::from("空格");
        let mut label = SnowCaptureKeyboardLabel {
            key_code: 32,
            utf8: text.as_ptr(),
            utf8_len: text.len() as u32,
        };
        config.keyboard_labels = &raw const label;
        config.keyboard_label_count = 1;
        let parsed = parse_direct_recording_config(&config)
            .unwrap()
            .keyboard
            .unwrap();
        assert_eq!(parsed.background_rgba, [0x11, 0x22, 0x33, 0xcc]);
        assert_eq!(parsed.labels[&32], "空格");
        label.utf8_len = 129;
        config.keyboard_labels = &raw const label;
        assert!(parse_direct_recording_config(&config).is_err());
        config.keyboard_labels = std::ptr::null();
        assert!(parse_direct_recording_config(&config).is_err());
        config.keyboard_label_count = 257;
        assert!(parse_direct_recording_config(&config).is_err());
        config.keyboard_label_count = 0;
        config.show_keyboard = 2;
        assert!(parse_direct_recording_config(&config).is_err());
        drop(text);
        assert_eq!(parsed.labels[&32], "空格");
    }

    #[test]
    fn direct_create_validates_output_pointer_without_starting_workers() {
        let output = CString::new("recording.mp4").unwrap();
        let config = direct_config(&output);
        assert_eq!(
            unsafe { snow_capture_recording_session_create_direct(&config, ptr::null_mut()) },
            SnowCaptureResult::InvalidArgument
        );
    }

    #[test]
    fn recording_session_live_count_tracks_create_and_destroy_exactly_once() {
        let output = CString::new("recording.mp4").unwrap();
        let config = direct_config(&output);
        let baseline = snow_capture_recording_session_live_count();
        let mut first = ptr::null_mut();
        let mut second = ptr::null_mut();
        assert_eq!(
            unsafe { snow_capture_recording_session_create_direct(&config, &mut first) },
            SnowCaptureResult::Ok
        );
        assert!(!first.is_null());
        assert_eq!(
            unsafe { snow_capture_recording_session_create_direct(&config, &mut second) },
            SnowCaptureResult::Ok
        );
        assert!(!second.is_null());
        assert_eq!(snow_capture_recording_session_live_count(), baseline + 2);
        // A null destroy must not disturb the count; destroy pairs with create once.
        unsafe { snow_capture_recording_session_destroy(ptr::null_mut()) };
        assert_eq!(snow_capture_recording_session_live_count(), baseline + 2);
        unsafe { snow_capture_recording_session_destroy(first) };
        assert_eq!(snow_capture_recording_session_live_count(), baseline + 1);
        unsafe { snow_capture_recording_session_destroy(second) };
        assert_eq!(snow_capture_recording_session_live_count(), baseline);
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
            std::mem::size_of::<usize>() + 72
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
        assert_eq!(options.color_correction, ColorCorrection::Disabled);
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
                matches!(options.color_correction, ColorCorrection::Snapshot(_)),
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
            capture_retry_count: 2,
            wgc_update_mode: 1,
            capture_backend: 2,
            pixel_format: 0,
            reserved: [0; 29],
        };

        let (options, backend) = default_options(&raw const config).unwrap();

        assert_eq!(options.capture_retry_count, 2);
        assert_eq!(options.wgc_update_mode, WgcUpdateMode::CompleteOnly);
        assert_eq!(options.output_pixel_format, CapturePixelFormat::Rgba8);
        assert_eq!(backend, CaptureBackendKind::WindowsGraphicsCapture);
    }

    #[test]
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
        assert_eq!(std::mem::size_of::<SnowCaptureStreamConfig>(), 80);
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
        assert!(state.load(Ordering::Acquire));
        unsafe { snow_capture_cancellation_token_destroy(token) };
    }

    #[test]
    fn color_correction_is_opt_in_and_request_flag_is_valid() {
        let (options, _) = default_options(ptr::null()).unwrap();
        assert_eq!(options.color_correction, ColorCorrection::Disabled);
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
            &[first, second]
        ));
    }
}

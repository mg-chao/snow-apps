#![allow(clippy::missing_safety_doc)]
use snow_capture::backend::CaptureBackendKind;
use snow_screen_recorder::{
    DirectRecordingConfig, DirectRecordingSession, EditingSession, ExportFormat, ExportRequest,
    RecordingAudioConfig, RecordingAudioTrackConfig, RecordingConfig, RecordingRegion,
    RecordingSession, RecordingState, RecordingTarget, ScreenRecorderError, VideoCodec,
    VideoEncodeConfig, VideoEncodingSpeed,
};
use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char};
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};
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

pub struct SnowRecordingSessionImpl {
    recording: Option<RecordingSessionKind>,
    state: RecordingState,
}

enum RecordingSessionKind {
    Legacy(Box<RecordingSession>),
    Direct(Box<DirectRecordingSession>),
}

#[repr(C)]
pub struct SnowRecordingConfig {
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
pub enum SnowRecordingState {
    Created = 0,
    Running = 1,
    Paused = 2,
    Stopped = 3,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowRecordingResult {
    Ok = 0,
    InvalidArgument = 1,
    InvalidState = 2,
    CaptureError = 3,
    EncoderError = 4,
    IoError = 5,
    Canceled = 6,
    PermissionDenied = 7,
    Unsupported = 8,
    TargetUnavailable = 9,
    InternalError = 255,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowRecordingExportConfig {
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
    loop_animated_images: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SnowCaptureDirectRecordingConfigHeader {
    version: u32,
    struct_size: u32,
}

pub const DIRECT_RECORDING_CONFIG_VERSION: u32 = 5;
const DIRECT_RECORDING_CONFIG_V4_FIELDS_SIZE: usize =
    std::mem::offset_of!(SnowCaptureDirectRecordingConfig, loop_animated_images);
const DIRECT_RECORDING_CONFIG_V4_SIZE: u32 = (DIRECT_RECORDING_CONFIG_V4_FIELDS_SIZE
    .div_ceil(std::mem::align_of::<SnowCaptureDirectRecordingConfig>())
    * std::mem::align_of::<SnowCaptureDirectRecordingConfig>())
    as u32;
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
        4 => Ok(DIRECT_RECORDING_CONFIG_V4_SIZE),
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
struct SnowRecordingExportConfigHeader {
    version: u32,
    struct_size: u32,
}

pub const RECORDING_EXPORT_CONFIG_VERSION: u32 = 1;
const RECORDING_EXPORT_CONFIG_SIZE: u32 = std::mem::size_of::<SnowRecordingExportConfig>() as u32;

fn recording_session_mut<'a>(
    session: *mut SnowRecordingSessionImpl,
) -> Option<&'a mut SnowRecordingSessionImpl> {
    if session.is_null() {
        set_last_error("recording session is null");
        None
    } else {
        Some(unsafe { &mut *session })
    }
}

fn recording_session_ref<'a>(
    session: *const SnowRecordingSessionImpl,
) -> Option<&'a SnowRecordingSessionImpl> {
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

fn ffi_recording_state(state: RecordingState) -> SnowRecordingState {
    match state {
        RecordingState::Created => SnowRecordingState::Created,
        RecordingState::Running => SnowRecordingState::Running,
        RecordingState::Paused => SnowRecordingState::Paused,
        RecordingState::Stopped => SnowRecordingState::Stopped,
    }
}

// Makes the exactly-once recording session destroy contract observable: a missed
// destroy leaves the count above its baseline and a repeated destroy wraps it far
// below, so tests can fail loudly instead of leaking capture pipelines and input
// hooks silently.
static LIVE_RECORDING_SESSIONS: AtomicUsize = AtomicUsize::new(0);

/// Live recording sessions created and not yet destroyed.
#[unsafe(no_mangle)]
pub extern "C" fn snow_recording_session_live_count() -> usize {
    LIVE_RECORDING_SESSIONS.load(Ordering::Relaxed)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_session_create(
    config: *const SnowRecordingConfig,
) -> *mut SnowRecordingSessionImpl {
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
            Box::into_raw(Box::new(SnowRecordingSessionImpl {
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
pub unsafe extern "C" fn snow_recording_session_destroy(session: *mut SnowRecordingSessionImpl) {
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
pub extern "C" fn snow_recording_session_start(session: *mut SnowRecordingSessionImpl) -> u8 {
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
pub extern "C" fn snow_recording_session_pause(session: *mut SnowRecordingSessionImpl) -> u8 {
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
pub extern "C" fn snow_recording_session_resume(session: *mut SnowRecordingSessionImpl) -> u8 {
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
pub unsafe extern "C" fn snow_recording_session_state(
    session: *const SnowRecordingSessionImpl,
    out_state: *mut SnowRecordingState,
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

fn direct_result_for_error(error: &ScreenRecorderError) -> SnowRecordingResult {
    match error {
        ScreenRecorderError::InvalidConfig(_) => SnowRecordingResult::InvalidArgument,
        ScreenRecorderError::PermissionDenied(_)
        | ScreenRecorderError::Capture(snow_capture::error::CaptureError::PermissionDenied)
        | ScreenRecorderError::Audio(snow_audio_recorder::error::AudioError::AccessDenied) => {
            SnowRecordingResult::PermissionDenied
        }
        ScreenRecorderError::UnsupportedFeature(_) => SnowRecordingResult::Unsupported,
        ScreenRecorderError::Capture(snow_capture::error::CaptureError::MonitorLost) => {
            SnowRecordingResult::TargetUnavailable
        }
        ScreenRecorderError::Capture(_) => SnowRecordingResult::CaptureError,
        ScreenRecorderError::Audio(_) | ScreenRecorderError::Encode(_) => {
            SnowRecordingResult::EncoderError
        }
        ScreenRecorderError::Io(_) => SnowRecordingResult::IoError,
        ScreenRecorderError::ExportCanceled => SnowRecordingResult::Canceled,
        ScreenRecorderError::Decode(_) | ScreenRecorderError::Export(_) => {
            SnowRecordingResult::EncoderError
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

    let loop_animated_images = if config.version < 5 {
        true
    } else {
        match config.loop_animated_images {
            0 => false,
            1 => true,
            value => return Err(format!("invalid animated image loop flag: {value}")),
        }
    };
    let direct = DirectRecordingConfig {
        loop_animated_images,
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
    } else if header.version == 4 {
        DIRECT_RECORDING_CONFIG_V4_FIELDS_SIZE
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
pub unsafe extern "C" fn snow_recording_output_dimensions(
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
pub unsafe extern "C" fn snow_recording_session_create_direct(
    config: *const SnowCaptureDirectRecordingConfig,
    out_session: *mut *mut SnowRecordingSessionImpl,
) -> SnowRecordingResult {
    if out_session.is_null() {
        set_last_error("direct recording out_session is null");
        return SnowRecordingResult::InvalidArgument;
    }
    unsafe { *out_session = ptr::null_mut() };
    let config = match unsafe { read_direct_recording_config(config) }
        .and_then(|config| parse_direct_recording_config(&config))
    {
        Ok(config) => config,
        Err(error) => {
            set_last_error(error);
            return SnowRecordingResult::InvalidArgument;
        }
    };
    match DirectRecordingSession::create(config) {
        Ok(recording) => {
            LIVE_RECORDING_SESSIONS.fetch_add(1, Ordering::Relaxed);
            unsafe {
                *out_session = Box::into_raw(Box::new(SnowRecordingSessionImpl {
                    recording: Some(RecordingSessionKind::Direct(Box::new(recording))),
                    state: RecordingState::Created,
                }));
            }
            clear_last_error();
            SnowRecordingResult::Ok
        }
        Err(error) => {
            let result = direct_result_for_error(&error);
            set_last_error(error);
            result
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_recording_session_stop(
    session: *mut SnowRecordingSessionImpl,
) -> SnowRecordingResult {
    let Some(session) = recording_session_mut(session) else {
        return SnowRecordingResult::InvalidArgument;
    };
    if !matches!(
        session.state,
        RecordingState::Running | RecordingState::Paused
    ) {
        set_last_error("direct recording stop requires a running or paused session");
        return SnowRecordingResult::InvalidState;
    }
    if !matches!(session.recording, Some(RecordingSessionKind::Direct(_))) {
        set_last_error("recording session was not created with create_direct");
        return SnowRecordingResult::InvalidState;
    }
    let Some(RecordingSessionKind::Direct(recording)) = session.recording.take() else {
        set_last_error("direct recording session is unavailable");
        return SnowRecordingResult::InternalError;
    };
    session.state = RecordingState::Stopped;
    match recording.stop() {
        Ok(_) => {
            clear_last_error();
            SnowRecordingResult::Ok
        }
        Err(error) => {
            let result = direct_result_for_error(&error);
            set_last_error(error);
            result
        }
    }
}

/// Disposable packaged-runtime diagnostic. Uses the ordinary direct session,
/// including startup negotiation, pause/resume, audio and MP4 finalization.
///
/// # Safety
/// `config` and its strings must satisfy the same contract as create_direct.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_gpu_probe(
    config: *const SnowCaptureDirectRecordingConfig,
    recover: u32,
) -> SnowRecordingResult {
    if recover > 1 {
        set_last_error("GPU probe recovery mode must be 0 or 1");
        return SnowRecordingResult::InvalidArgument;
    }
    let config = match unsafe { read_direct_recording_config(config) }
        .and_then(|config| parse_direct_recording_config(&config))
    {
        Ok(config)
            if config.prefer_hardware_encoder
                && config.format == ExportFormat::Mp4
                && config.codec == VideoCodec::H264 =>
        {
            config
        }
        Ok(_) => {
            set_last_error("GPU probe requires hardware H.264 MP4");
            return SnowRecordingResult::InvalidArgument;
        }
        Err(error) => {
            set_last_error(error);
            return SnowRecordingResult::InvalidArgument;
        }
    };
    let result = (|| {
        let mut session = DirectRecordingSession::create(config)?;
        session.start()?;
        std::thread::sleep(std::time::Duration::from_millis(400));
        session.pause()?;
        std::thread::sleep(std::time::Duration::from_millis(100));
        session.resume()?;
        if recover == 1 {
            std::thread::sleep(std::time::Duration::from_millis(200));
            session.disconnect_gpu_capture_for_diagnostics()?;
            std::thread::sleep(std::time::Duration::from_millis(400));
        } else {
            std::thread::sleep(std::time::Duration::from_millis(600));
        }
        session.stop()
    })();
    match result {
        Ok(report) => {
            eprintln!(
                "recording_probe pipeline={} adapter={:?} encoder={} frames={} recovery={} fallback={:?}",
                report.selected_pipeline,
                report.adapter,
                report.video_encoder,
                report.encoded_frames,
                report.recovery_count,
                report.fallback_reason
            );
            let expected = if recover == 1 {
                "d3d11_to_software"
            } else {
                "d3d11"
            };
            if report.selected_pipeline == expected
                && report.recovery_count == recover
                && report.encoded_frames > 0
            {
                clear_last_error();
                SnowRecordingResult::Ok
            } else {
                set_last_error(format!(
                    "GPU probe selected {}: {:?}",
                    report.selected_pipeline, report.fallback_reason
                ));
                SnowRecordingResult::InternalError
            }
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
    config: &SnowRecordingExportConfig,
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
    config: *const SnowRecordingExportConfig,
) -> Result<SnowRecordingExportConfig, String> {
    if config.is_null() {
        return Err("recording export config is null".to_string());
    }

    // Read only the fixed header until the caller-provided size has been
    // validated. This keeps undersized future/foreign-language inputs from
    // being dereferenced as a complete structure.
    let header =
        unsafe { std::ptr::read_unaligned(config.cast::<SnowRecordingExportConfigHeader>()) };
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
    session: *mut SnowRecordingSessionImpl,
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
pub unsafe extern "C" fn snow_recording_session_stop_and_export(
    session: *mut SnowRecordingSessionImpl,
    config: *const SnowRecordingExportConfig,
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
pub extern "C" fn snow_recording_last_error_message() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn recording_preview_output_dimensions_match_export_formats() {
        for format in 0..=3 {
            let mut width = 0;
            let mut height = 0;
            assert_eq!(
                unsafe {
                    super::snow_recording_output_dimensions(
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
                super::snow_recording_output_dimensions(
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
                super::snow_recording_output_dimensions(
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
                super::snow_recording_output_dimensions(100, 100, 0, 0, 9, &mut width, &mut height)
            },
            0
        );
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
        let config = SnowRecordingExportConfig {
            version: RECORDING_EXPORT_CONFIG_VERSION,
            struct_size: std::mem::size_of::<SnowRecordingExportConfig>() as u32,
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
        let config = SnowRecordingExportConfig {
            version: RECORDING_EXPORT_CONFIG_VERSION,
            struct_size: std::mem::size_of::<SnowRecordingExportConfig>() as u32,
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
        let config = SnowRecordingExportConfig {
            version: RECORDING_EXPORT_CONFIG_VERSION,
            struct_size: std::mem::size_of::<SnowRecordingExportConfig>() as u32,
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
        let mut config = SnowRecordingExportConfig {
            version: RECORDING_EXPORT_CONFIG_VERSION + 1,
            struct_size: std::mem::size_of::<SnowRecordingExportConfig>() as u32,
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
        let short = SnowRecordingExportConfigHeader {
            version: RECORDING_EXPORT_CONFIG_VERSION,
            struct_size: std::mem::size_of::<SnowRecordingExportConfigHeader>() as u32,
        };
        let config = (&raw const short).cast::<SnowRecordingExportConfig>();

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
            loop_animated_images: 1,
        }
    }
    #[test]
    fn gpu_probe_rejects_invalid_and_software_configs_before_capture() {
        assert_eq!(
            unsafe { snow_recording_gpu_probe(ptr::null(), 0) },
            SnowRecordingResult::InvalidArgument
        );
        let output = CString::new("unused-gpu-probe.mp4").unwrap();
        let mut config = direct_config(&output);
        config.encoder_preference = 0;
        assert_eq!(
            unsafe { snow_recording_gpu_probe(&config, 0) },
            SnowRecordingResult::InvalidArgument
        );
        config.encoder_preference = 1;
        assert_eq!(
            unsafe { snow_recording_gpu_probe(&config, 2) },
            SnowRecordingResult::InvalidArgument
        );
        assert_eq!(snow_recording_session_live_count(), 0);
    }
    #[test]
    fn direct_config_loop_flag_and_legacy_padding() {
        let output = CString::new("recording.mp4").unwrap();
        for version in 1..=DIRECT_RECORDING_CONFIG_VERSION {
            for enabled in [false, true] {
                let mut config = direct_config(&output);
                config.version = version;
                if version == 2 {
                    config.mouse_trail_duration_ms = 0;
                }
                config.struct_size = direct_config_size(version).unwrap();
                config.loop_animated_images = u32::from(enabled);
                if version < 5 {
                    // This occupies v4 tail padding on x64 and is absent in older versions.
                    config.loop_animated_images = u32::MAX;
                }
                let read = unsafe { read_direct_recording_config(&config) }.unwrap();
                let parsed = parse_direct_recording_config(&read).unwrap();
                assert_eq!(parsed.loop_animated_images, version < 5 || enabled);
            }
        }
        let mut config = direct_config(&output);
        config.loop_animated_images = 2;
        assert!(parse_direct_recording_config(&config).is_err());
        config.struct_size = DIRECT_RECORDING_CONFIG_SIZE - 1;
        assert!(unsafe { read_direct_recording_config(&config) }.is_err());
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
            unsafe { snow_recording_session_create_direct(&config, ptr::null_mut()) },
            SnowRecordingResult::InvalidArgument
        );
    }
    #[test]
    fn recording_session_live_count_tracks_create_and_destroy_exactly_once() {
        let output = CString::new("recording.mp4").unwrap();
        let config = direct_config(&output);
        let baseline = snow_recording_session_live_count();
        let mut first = ptr::null_mut();
        let mut second = ptr::null_mut();
        assert_eq!(
            unsafe { snow_recording_session_create_direct(&config, &mut first) },
            SnowRecordingResult::Ok
        );
        assert!(!first.is_null());
        assert_eq!(
            unsafe { snow_recording_session_create_direct(&config, &mut second) },
            SnowRecordingResult::Ok
        );
        assert!(!second.is_null());
        assert_eq!(snow_recording_session_live_count(), baseline + 2);
        // A null destroy must not disturb the count; destroy pairs with create once.
        unsafe { snow_recording_session_destroy(ptr::null_mut()) };
        assert_eq!(snow_recording_session_live_count(), baseline + 2);
        unsafe { snow_recording_session_destroy(first) };
        assert_eq!(snow_recording_session_live_count(), baseline + 1);
        unsafe { snow_recording_session_destroy(second) };
        assert_eq!(snow_recording_session_live_count(), baseline);
    }
    #[test]
    fn odd_recording_region_is_rejected_before_session_creation() {
        let config = SnowRecordingConfig {
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

        let session = unsafe { snow_recording_session_create(&config) };
        assert!(session.is_null());
        let error = unsafe { CStr::from_ptr(snow_recording_last_error_message()) };
        assert_eq!(
            error.to_str().expect("recording error should be UTF-8"),
            "recording region width and height must be even"
        );
    }
}

#[cfg(target_os = "macos")]
pub mod macos;

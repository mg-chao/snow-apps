//! Synchronous worker-thread C interface for native macOS recording.
use snow_macos::desktop::{DesktopConfig, DesktopTarget, DisplayId, WindowId};
use snow_media::{
    CursorMode, DynamicRange,
    geometry::{DesktopRect, DesktopSpace, PixelSize},
};
use snow_recording_export::{ExportExecutionMode, VideoCodec};
use snow_screen_recorder::ScreenRecorderError;
use snow_screen_recorder::macos::{
    NativeEditableSession, NativeRecordingConfig, NativeRecordingEvent, NativeRecordingReport,
    NativeRecordingSession,
};
use std::{
    ffi::{CStr, CString, c_char, c_void},
    ptr,
    time::Duration,
};

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Ok = 0,
    InvalidArgument = 1,
    PermissionDenied = 2,
    TargetUnavailable = 3,
    Unsupported = 4,
    Timeout = 5,
    Canceled = 6,
    Failed = 255,
}
fn boundary(action: impl FnOnce() -> Result<(), ScreenRecorderError>) -> Status {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(action)) {
        Ok(Ok(())) => {
            super::clear_last_error();
            Status::Ok
        }
        Ok(Err(error)) => {
            super::set_last_error(&error);
            match error {
                ScreenRecorderError::InvalidConfig(_) => Status::InvalidArgument,
                ScreenRecorderError::PermissionDenied(_) => Status::PermissionDenied,
                ScreenRecorderError::UnsupportedFeature(_) => Status::Unsupported,
                ScreenRecorderError::Capture(
                    snow_capture::error::CaptureError::PermissionDenied,
                )
                | ScreenRecorderError::Audio(
                    snow_audio_recorder::error::AudioError::AccessDenied,
                ) => Status::PermissionDenied,
                ScreenRecorderError::Capture(snow_capture::error::CaptureError::MonitorLost) => {
                    Status::TargetUnavailable
                }
                ScreenRecorderError::Capture(snow_capture::error::CaptureError::Timeout) => {
                    Status::Timeout
                }
                ScreenRecorderError::Capture(snow_capture::error::CaptureError::Canceled) => {
                    Status::Canceled
                }
                ScreenRecorderError::ExportCanceled => Status::Canceled,
                ScreenRecorderError::Audio(snow_audio_recorder::error::AudioError::Canceled) => {
                    Status::Canceled
                }
                _ => Status::Failed,
            }
        }
        Err(_) => {
            super::set_last_error("panic at macOS recording boundary");
            Status::Failed
        }
    }
}
fn invalid() -> ScreenRecorderError {
    ScreenRecorderError::InvalidConfig("invalid macOS recording argument".into())
}
pub struct SnowMacRecordingCancellation(snow_macos::CancellationToken);
#[unsafe(no_mangle)]
pub extern "C" fn snow_recording_macos_cancellation_create() -> *mut SnowMacRecordingCancellation {
    Box::into_raw(Box::new(SnowMacRecordingCancellation(Default::default())))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_macos_cancellation_cancel(
    token: *const SnowMacRecordingCancellation,
) {
    if let Some(token) = unsafe { token.as_ref() } {
        token.0.cancel();
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_macos_cancellation_release(
    token: *mut SnowMacRecordingCancellation,
) {
    if !token.is_null() {
        unsafe {
            drop(Box::from_raw(token));
        }
    }
}
#[repr(C)]
pub struct SnowMacRecordingConfig {
    pub struct_size: u32,
    pub cancellation: *const SnowMacRecordingCancellation,
    pub target_kind: u32,
    pub target_id: u32,
    pub hdr: u32,
    pub editable: u32,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
    pub output_width: u32,
    pub output_height: u32,
    pub fps: u32,
    pub codec: u32,
    pub policy: u32,
    pub cursor: u32,
    pub system_audio: u32,
    pub microphone: u32,
    pub click_effects: u32,
    pub trail: u32,
    pub keyboard: u32,
    pub microphone_id: *const c_char,
    pub output_path: *const c_char,
    pub highlight_rgba: u32,
    pub record_mouse_clicks: u32,
}
pub struct SnowMacRecording {
    session: Option<RecordingSession>,
}
enum RecordingSession {
    Direct(Box<NativeRecordingSession>),
    Editable(Box<NativeEditableSession>),
}
impl RecordingSession {
    fn step(&mut self, timeout: Duration) -> Result<NativeRecordingEvent, ScreenRecorderError> {
        match self {
            Self::Direct(s) => s.step(timeout),
            Self::Editable(s) => s.step(timeout),
        }
    }
    fn pause(&mut self) {
        match self {
            Self::Direct(s) => s.pause(),
            Self::Editable(s) => s.pause(),
        }
    }
    fn resume(&mut self) {
        match self {
            Self::Direct(s) => s.resume(),
            Self::Editable(s) => s.resume(),
        }
    }
    fn finish(self) -> Result<NativeRecordingReport, ScreenRecorderError> {
        match self {
            Self::Direct(s) => s.finish(),
            Self::Editable(s) => s.finish().map(|r| r.recording),
        }
    }
}
#[repr(C)]
#[derive(Default)]
pub struct SnowMacRecordingEvent {
    pub kind: u32,
    pub width: u32,
    pub height: u32,
    pub generation: u64,
    pub pts: u64,
    pub x: f64,
    pub y: f64,
    pub desktop_width: f64,
    pub desktop_height: f64,
}
#[repr(C)]
#[derive(Default)]
pub struct SnowMacRecordingReport {
    pub encoded_frames: u64,
    pub cpu_readbacks: u64,
    pub interruptions: u64,
    pub configuration_changes: u64,
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_macos_create(
    config: *const SnowMacRecordingConfig,
    output: *mut *mut SnowMacRecording,
) -> Status {
    boundary(|| {
        if output.is_null() {
            return Err(invalid());
        }
        unsafe {
            output.write(ptr::null_mut());
        }
        // Validate the fixed header before constructing a reference to the full
        // versioned struct; an older caller may only have allocated the header.
        if config.is_null() {
            return Err(invalid());
        }
        let size = unsafe { config.cast::<u32>().read() } as usize;
        let legacy_size = std::mem::offset_of!(SnowMacRecordingConfig, highlight_rgba);
        if size != legacy_size && size != size_of::<SnowMacRecordingConfig>() {
            return Err(invalid());
        }
        let mut extended: SnowMacRecordingConfig = unsafe { std::mem::zeroed() };
        unsafe {
            std::ptr::copy_nonoverlapping(
                config.cast::<u8>(),
                (&raw mut extended).cast::<u8>(),
                size,
            );
        }
        let config = &extended;
        if config.record_mouse_clicks > 1
            || config.output_path.is_null()
            || config.hdr > 1
            || config.editable > 1
            || config.system_audio > 1
            || config.microphone > 1
            || config.click_effects > 1
            || config.trail > 1
            || config.keyboard > 1
        {
            return Err(invalid());
        }
        let target = match config.target_kind {
            0 => DesktopTarget::PrimaryDisplay,
            1 => DesktopTarget::Display(DisplayId(config.target_id)),
            2 => DesktopTarget::Window(WindowId(config.target_id)),
            3 => DesktopTarget::Region(DesktopRect {
                space: DesktopSpace::Points,
                x: config.x,
                y: config.y,
                width: config.width,
                height: config.height,
            }),
            _ => return Err(invalid()),
        };
        let mut capture = DesktopConfig::new(target);
        if let Some(token) = unsafe { config.cancellation.as_ref() } {
            capture.cancellation = token.0.clone();
        }
        capture.dynamic_range = if config.hdr == 1 {
            DynamicRange::Hdr
        } else {
            DynamicRange::Sdr
        };
        capture.cursor = match config.cursor {
            0 => CursorMode::Hidden,
            1 => CursorMode::Embedded,
            2 => CursorMode::Separate,
            _ => return Err(invalid()),
        };
        capture.fps = config.fps;
        let audio = if config.system_audio == 1 || config.microphone == 1 {
            let mut audio = snow_audio_recorder::AudioStreamConfig::default();
            audio.system.enabled = config.system_audio == 1;
            audio.microphone.enabled = config.microphone == 1;
            if !config.microphone_id.is_null() {
                audio.microphone.device = snow_audio_recorder::DeviceSelector::Id(
                    unsafe { CStr::from_ptr(config.microphone_id) }
                        .to_str()
                        .map_err(|_| invalid())?
                        .into(),
                );
            }
            Some(audio)
        } else {
            None
        };
        let native_config = NativeRecordingConfig {
            effects: snow_screen_recorder::macos::NativeEffectsConfig {
                clicks: config.click_effects == 1,
                trail: config.trail == 1,
                show_keyboard: config.keyboard == 1,
                record_mouse_clicks: config.record_mouse_clicks == 1,
                highlight_rgba: if config.cursor != 0 {
                    config.highlight_rgba.to_be_bytes()
                } else {
                    [0; 4]
                },
                keyboard: (config.keyboard == 1 || config.record_mouse_clicks == 1).then(|| {
                    snow_screen_recorder::KeyboardOverlayConfig {
                        keycap_size: 64,
                        background_rgba: [24, 24, 24, 220],
                        text_rgba: [255; 4],
                        border_rgba: [128, 128, 128, 255],
                        labels: Default::default(),
                    }
                }),
            },
            capture,
            output: PixelSize::new(config.output_width, config.output_height)
                .map_err(|_| invalid())?,
            output_path: unsafe { CStr::from_ptr(config.output_path) }
                .to_str()
                .map_err(|_| invalid())?
                .into(),
            fps: config.fps,
            codec: match config.codec {
                0 => VideoCodec::H264,
                1 => VideoCodec::H265,
                _ => return Err(invalid()),
            },
            execution: match config.policy {
                0 => ExportExecutionMode::HardwarePreferred,
                1 => ExportExecutionMode::HardwareOnly,
                2 => ExportExecutionMode::SoftwareOnly,
                _ => return Err(invalid()),
            },
            audio,
        };
        let session = if config.editable == 1 {
            RecordingSession::Editable(Box::new(NativeEditableSession::start(native_config)?))
        } else {
            RecordingSession::Direct(Box::new(NativeRecordingSession::start(native_config)?))
        };
        unsafe {
            output.write(Box::into_raw(Box::new(SnowMacRecording {
                session: Some(session),
            })));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_macos_step(
    recording: *mut SnowMacRecording,
    timeout_ms: u32,
    output: *mut SnowMacRecordingEvent,
) -> Status {
    boundary(|| {
        if output.is_null() || timeout_ms > 60_000 {
            return Err(invalid());
        }
        let session = unsafe { recording.as_mut() }
            .and_then(|r| r.session.as_mut())
            .ok_or_else(invalid)?;
        let mut event = SnowMacRecordingEvent::default();
        match session.step(Duration::from_millis(u64::from(timeout_ms)))? {
            NativeRecordingEvent::Configuration {
                transform,
                generation,
            } => {
                event.kind = 1;
                event.generation = generation;
                event.width = transform.output.width;
                event.height = transform.output.height;
                event.x = transform.source.x;
                event.y = transform.source.y;
                event.desktop_width = transform.source.width;
                event.desktop_height = transform.source.height;
            }
            NativeRecordingEvent::Frame { pts } => {
                event.kind = 2;
                event.pts = pts;
            }
            NativeRecordingEvent::Interruption { at_ms, .. } => {
                event.kind = 3;
                event.pts = at_ms;
            }
            NativeRecordingEvent::Idle => {}
        }
        unsafe {
            output.write(event);
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_macos_pause(
    recording: *mut SnowMacRecording,
    paused: u8,
) -> Status {
    boundary(|| {
        let session = unsafe { recording.as_mut() }
            .and_then(|r| r.session.as_mut())
            .ok_or_else(invalid)?;
        match paused {
            0 => session.resume(),
            1 => session.pause(),
            _ => return Err(invalid()),
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_macos_finish(
    recording: *mut SnowMacRecording,
    output: *mut SnowMacRecordingReport,
) -> Status {
    boundary(|| {
        if output.is_null() {
            return Err(invalid());
        }
        let session = unsafe { recording.as_mut() }
            .and_then(|r| r.session.take())
            .ok_or_else(invalid)?;
        let report = session.finish()?;
        unsafe {
            output.write(SnowMacRecordingReport {
                encoded_frames: report.encoder.encoded_frames,
                cpu_readbacks: report.cpu_readbacks,
                interruptions: report.interruptions.len() as u64,
                configuration_changes: report.geometry_changes.len() as u64,
            });
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_macos_destroy(recording: *mut SnowMacRecording) {
    if !recording.is_null() {
        unsafe {
            drop(Box::from_raw(recording));
        }
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn snow_recording_macos_input_permission_check() -> u8 {
    snow_macos::input::authorized().into()
}
#[unsafe(no_mangle)]
pub extern "C" fn snow_recording_macos_input_permission_request() -> u8 {
    snow_macos::input::request_access().into()
}
#[unsafe(no_mangle)]
pub extern "C" fn snow_recording_macos_microphone_permission_check() -> u8 {
    snow_macos::microphone::microphone_authorized().into()
}

/// Completion can run on an arbitrary OS queue. The callback and context must
/// remain valid until its single invocation; no blocking main-loop wrapper.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_macos_microphone_permission_request(
    callback: Option<unsafe extern "C" fn(u8, *mut c_void)>,
    context: *mut c_void,
) -> Status {
    boundary(|| {
        let callback = callback.ok_or_else(invalid)?;
        let context = context as usize;
        snow_macos::microphone::request_microphone_access(move |granted| unsafe {
            callback(granted.into(), context as *mut c_void);
        });
        Ok(())
    })
}
#[repr(C)]
pub struct SnowMacInputDevice {
    pub id: u32,
    pub is_default: u32,
    pub uid: *const c_char,
    pub name: *const c_char,
}
/// Device strings are borrowed only for the synchronous visitor invocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_macos_enumerate_inputs(
    visitor: Option<unsafe extern "C" fn(*const SnowMacInputDevice, *mut c_void)>,
    context: *mut c_void,
) -> Status {
    boundary(|| {
        let visitor = visitor.ok_or_else(invalid)?;
        let devices = snow_macos::microphone::input_devices().map_err(|e| {
            ScreenRecorderError::Audio(snow_audio_recorder::error::AudioError::platform(e))
        })?;
        for device in devices {
            let uid = CString::new(device.uid).map_err(|_| invalid())?;
            let name = CString::new(device.name).map_err(|_| invalid())?;
            let info = SnowMacInputDevice {
                id: device.id,
                is_default: device.is_default.into(),
                uid: uid.as_ptr(),
                name: name.as_ptr(),
            };
            unsafe {
                visitor(&info, context);
            }
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn canceled_recording_does_not_create_an_output_or_require_permissions() {
        let token = snow_recording_macos_cancellation_create();
        assert!(!token.is_null());
        let retained = unsafe { &*token }.0.clone();
        let mut config: SnowMacRecordingConfig = unsafe { std::mem::zeroed() };
        config.struct_size = size_of::<SnowMacRecordingConfig>() as u32;
        config.cancellation = token;
        config.output_width = 64;
        config.output_height = 64;
        config.fps = 30;
        config.microphone = 1;
        config.output_path = c"/nonexistent/canceled-recording.mp4".as_ptr();
        unsafe {
            snow_recording_macos_cancellation_cancel(token);
        }
        for editable in [0, 1] {
            config.editable = editable;
            let mut output = ptr::dangling_mut();
            assert_eq!(
                unsafe { snow_recording_macos_create(&config, &mut output) },
                Status::Canceled
            );
            assert!(output.is_null());
        }
        unsafe {
            snow_recording_macos_cancellation_release(token);
        }
        assert!(retained.is_canceled());
        unsafe {
            snow_recording_macos_cancellation_cancel(ptr::null());
            snow_recording_macos_cancellation_release(ptr::null_mut());
        }
    }
    #[test]
    fn input_permission_failure_keeps_its_permission_kind() {
        let result = boundary(|| {
            Err(ScreenRecorderError::PermissionDenied(
                snow_screen_recorder::MediaPermission::InputMonitoring,
            ))
        });
        assert_eq!(result, Status::PermissionDenied);
        let message = unsafe { CStr::from_ptr(super::super::snow_recording_last_error_message()) }
            .to_str()
            .unwrap();
        assert_eq!(message, "input monitoring permission is required");
    }
    #[test]
    fn permission_and_device_visitors_require_callbacks() {
        assert_eq!(
            unsafe { snow_recording_macos_microphone_permission_request(None, ptr::null_mut()) },
            Status::InvalidArgument
        );
        assert_eq!(
            unsafe { snow_recording_macos_enumerate_inputs(None, ptr::null_mut()) },
            Status::InvalidArgument
        );
        let header: u32 = 4;
        let mut output = ptr::dangling_mut();
        assert_eq!(
            unsafe { snow_recording_macos_create((&header as *const u32).cast(), &mut output) },
            Status::InvalidArgument
        );
        assert!(output.is_null());
    }
    #[test]
    fn null_recording_handles_are_rejected() {
        assert_eq!(
            unsafe { snow_recording_macos_create(ptr::null(), ptr::null_mut()) },
            Status::InvalidArgument
        );
        assert_eq!(
            unsafe { snow_recording_macos_pause(ptr::null_mut(), 0) },
            Status::InvalidArgument
        );
        assert_eq!(
            unsafe { snow_recording_macos_finish(ptr::null_mut(), ptr::null_mut()) },
            Status::InvalidArgument
        );
        unsafe {
            snow_recording_macos_destroy(ptr::null_mut());
        }
    }
}

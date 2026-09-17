//! macOS extension. All blocking acquisition calls require a worker thread.
use snow_macos::{
    MacError,
    desktop::{
        DesktopConfig, DesktopEvent, DesktopFrame, DesktopSession, DesktopTarget, DisplayId,
        WindowId,
    },
};
use snow_media::{
    CpuFrame, CursorMode, DynamicRange,
    geometry::{DesktopRect, DesktopSpace, PixelSize},
};
use std::{
    ptr,
    sync::{Arc, OnceLock},
    time::Duration,
};

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowMacStatus {
    Ok = 0,
    InvalidArgument = 1,
    PermissionDenied = 2,
    TargetUnavailable = 3,
    Unsupported = 4,
    Timeout = 5,
    Canceled = 6,
    Interrupted = 7,
    Internal = 255,
}
fn status(error: MacError) -> SnowMacStatus {
    super::set_last_error(&error);
    match error {
        MacError::PermissionDenied | MacError::MicrophonePermissionDenied => {
            SnowMacStatus::PermissionDenied
        }
        MacError::TargetUnavailable => SnowMacStatus::TargetUnavailable,
        MacError::UnsupportedOs | MacError::Unsupported(_) => SnowMacStatus::Unsupported,
        MacError::InvalidConfig(_) => SnowMacStatus::InvalidArgument,
        MacError::Timeout => SnowMacStatus::Timeout,
        MacError::Canceled => SnowMacStatus::Canceled,
        MacError::Inactive => SnowMacStatus::Interrupted,
        _ => SnowMacStatus::Internal,
    }
}
fn boundary(action: impl FnOnce() -> Result<(), MacError>) -> SnowMacStatus {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(action)) {
        Ok(Ok(())) => {
            super::clear_last_error();
            SnowMacStatus::Ok
        }
        Ok(Err(error)) => status(error),
        Err(_) => {
            super::set_last_error("panic at macOS capture boundary");
            SnowMacStatus::Internal
        }
    }
}
pub struct SnowMacCancellation(snow_core::cancellation::CancellationToken);
#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_macos_cancellation_create() -> *mut SnowMacCancellation {
    Box::into_raw(Box::new(SnowMacCancellation(Default::default())))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_cancellation_cancel(token: *const SnowMacCancellation) {
    if let Some(token) = unsafe { token.as_ref() } {
        token.0.cancel();
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_cancellation_release(token: *mut SnowMacCancellation) {
    if !token.is_null() {
        unsafe {
            drop(Box::from_raw(token));
        }
    }
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowMacCaptureConfig {
    pub struct_size: u32,
    pub cancellation: *const SnowMacCancellation,
    /// 0 = primary display, 1 = display ID, 2 = window ID, 3 = desktop region.
    pub target_kind: u32,
    pub target_id: u32,
    /// 0 = SDR BGRA8, 1 = canonical HDR RGBA16Float (Apple Silicon).
    pub dynamic_range: u32,
    /// 0 = hidden, 1 = embedded, 2 = separate (no embedded pixels).
    pub cursor_mode: u32,
    pub fps: u32,
    pub timeout_ms: u32,
    /// Desktop points, top-left origin. Used only for region targets.
    pub source_x: f64,
    pub source_y: f64,
    pub source_width: f64,
    pub source_height: f64,
    /// Both zero selects native target resolution.
    pub output_width: u32,
    pub output_height: u32,
    pub excluded_windows: *const u32,
    pub excluded_window_count: usize,
    pub excluded_processes: *const i32,
    pub excluded_process_count: usize,
}
impl SnowMacCaptureConfig {
    fn parse(&self) -> Result<DesktopConfig, MacError> {
        let invalid = || MacError::InvalidConfig("invalid macOS capture configuration".into());
        if self.struct_size as usize != size_of::<Self>()
            || self.timeout_ms == 0
            || self.timeout_ms > 60_000
            || !(1..=240).contains(&self.fps)
        {
            return Err(invalid());
        }
        let timeout = Duration::from_millis(u64::from(self.timeout_ms));
        let target = match self.target_kind {
            0 => DesktopTarget::PrimaryDisplay,
            1 => DesktopTarget::Display(DisplayId(self.target_id)),
            2 => DesktopTarget::Window(WindowId(self.target_id)),
            3 => DesktopTarget::Region(
                DesktopRect {
                    space: DesktopSpace::Points,
                    x: self.source_x,
                    y: self.source_y,
                    width: self.source_width,
                    height: self.source_height,
                }
                .validate()
                .map_err(|_| invalid())?,
            ),
            _ => return Err(invalid()),
        };
        let mut config = DesktopConfig::new(target);
        if let Some(token) = unsafe { self.cancellation.as_ref() } {
            config.cancellation = token.0.clone();
        }
        config.timeout = timeout;
        config.fps = self.fps;
        config.dynamic_range = match self.dynamic_range {
            0 => DynamicRange::Sdr,
            1 => DynamicRange::Hdr,
            _ => return Err(invalid()),
        };
        config.cursor = match self.cursor_mode {
            0 => CursorMode::Hidden,
            1 => CursorMode::Embedded,
            2 => CursorMode::Separate,
            _ => return Err(invalid()),
        };
        config.output = match (self.output_width, self.output_height) {
            (0, 0) => None,
            (w, h) => Some(PixelSize::new(w, h).map_err(|_| invalid())?),
        };
        if self.excluded_window_count > 4096
            || self.excluded_process_count > 4096
            || (self.excluded_window_count != 0 && self.excluded_windows.is_null())
            || (self.excluded_process_count != 0 && self.excluded_processes.is_null())
        {
            return Err(invalid());
        }
        // The C contract requires these arrays to be readable for the duration of create/snapshot.
        if self.excluded_window_count != 0 {
            config.excluded_windows = unsafe {
                std::slice::from_raw_parts(self.excluded_windows, self.excluded_window_count)
            }
            .to_vec();
        }
        if self.excluded_process_count != 0 {
            config.excluded_processes = unsafe {
                std::slice::from_raw_parts(self.excluded_processes, self.excluded_process_count)
            }
            .to_vec();
        }
        config.excluded_windows.sort_unstable();
        config.excluded_windows.dedup();
        config.excluded_processes.sort_unstable();
        config.excluded_processes.dedup();
        Ok(config)
    }
}
#[derive(Default)]
struct CpuMappings {
    native: OnceLock<Result<CpuFrame, String>>,
    converted: OnceLock<Result<CpuFrame, String>>,
}
impl CpuMappings {
    fn get(
        &self,
        format: snow_media::PixelFormat,
        read: impl FnOnce() -> Result<CpuFrame, String>,
    ) -> Result<&CpuFrame, MacError> {
        let native = self
            .native
            .get_or_init(read)
            .as_ref()
            .map_err(|error| MacError::Unsupported(error.clone()))?;
        if native.format == format {
            return Ok(native);
        }
        self.converted
            .get_or_init(|| {
                native
                    .clone()
                    .into_format(format)
                    .map_err(|error| error.to_string())
            })
            .as_ref()
            .map_err(|error| MacError::Unsupported(error.clone()))
    }
}
struct Lease {
    frame: DesktopFrame,
    cpu: CpuMappings,
}
pub struct SnowMacFrame {
    lease: Arc<Lease>,
}
pub struct SnowMacStream {
    stream: DesktopSession,
}
#[repr(C)]
pub struct SnowMacFrameInfo {
    pub width: u32,
    pub height: u32,
    /// 1 = BGRA8, 2 = RGBA16Float, 3 = NV12, 4 = P010, 0 = RGBA8.
    pub pixel_format: u32,
    pub plane_count: u32,
    pub timestamp_value: i64,
    pub timestamp_scale: u32,
    pub color_primaries: u32,
    pub transfer_function: u32,
    pub color_range: u32,
    pub alpha_mode: u32,
    pub timestamp_epoch: i64,
    pub generation: u64,
    pub source_count: u32,
    pub duplicate: u32,
    pub desktop_x: f64,
    pub desktop_y: f64,
    pub desktop_width: f64,
    pub desktop_height: f64,
}
#[repr(C)]
pub struct SnowMacPlane {
    pub data: *const u8,
    pub len: usize,
    pub width: usize,
    pub height: usize,
    pub stride: usize,
}
fn lease(frame: DesktopFrame) -> *mut SnowMacFrame {
    Box::into_raw(Box::new(SnowMacFrame {
        lease: Arc::new(Lease {
            frame,
            cpu: CpuMappings::default(),
        }),
    }))
}
fn require_output<T>(output: *mut T) -> Result<(), MacError> {
    if output.is_null() {
        Err(MacError::InvalidConfig("null output".into()))
    } else {
        Ok(())
    }
}
#[repr(C)]
#[derive(Default)]
pub struct SnowMacCaptureEvent {
    /// 1 = configuration, 2 = frame. Configuration precedes its first frame.
    pub kind: u32,
    pub width: u32,
    pub height: u32,
    pub generation: u64,
    pub x: f64,
    pub y: f64,
    pub desktop_width: f64,
    pub desktop_height: f64,
    pub frame: *mut SnowMacFrame,
}
#[repr(C)]
pub struct SnowMacSourceTime {
    pub value: i64,
    pub timescale: u32,
    pub epoch: i64,
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_snapshot(
    config: *const SnowMacCaptureConfig,
    output: *mut *mut SnowMacFrame,
) -> SnowMacStatus {
    boundary(|| {
        require_output(output)?;
        unsafe {
            output.write(ptr::null_mut());
        }
        if config.is_null()
            || unsafe { config.cast::<u32>().read() } as usize != size_of::<SnowMacCaptureConfig>()
        {
            return Err(MacError::InvalidConfig(
                "invalid capture configuration size".into(),
            ));
        }
        let config = unsafe { &*config };
        let frame = DesktopSession::new(config.parse()?)?.snapshot()?;
        unsafe {
            output.write(lease(frame));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_stream_create(
    config: *const SnowMacCaptureConfig,
    output: *mut *mut SnowMacStream,
) -> SnowMacStatus {
    boundary(|| {
        require_output(output)?;
        unsafe {
            output.write(ptr::null_mut());
        }
        if config.is_null()
            || unsafe { config.cast::<u32>().read() } as usize != size_of::<SnowMacCaptureConfig>()
        {
            return Err(MacError::InvalidConfig(
                "invalid capture configuration size".into(),
            ));
        }
        let config = unsafe { &*config };
        let stream = DesktopSession::new(config.parse()?)?;
        unsafe {
            output.write(Box::into_raw(Box::new(SnowMacStream { stream })));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_stream_next(
    stream: *mut SnowMacStream,
    timeout_ms: u32,
    output: *mut SnowMacCaptureEvent,
) -> SnowMacStatus {
    boundary(|| {
        require_output(output)?;
        unsafe {
            output.write(SnowMacCaptureEvent::default());
        }
        let stream = unsafe { stream.as_mut() }
            .ok_or_else(|| MacError::InvalidConfig("null stream".into()))?;
        if timeout_ms > 60_000 {
            return Err(MacError::InvalidConfig("timeout exceeds 60 seconds".into()));
        }
        let mut event = SnowMacCaptureEvent::default();
        match stream
            .stream
            .next_event(Duration::from_millis(u64::from(timeout_ms)))?
        {
            DesktopEvent::Configuration {
                transform,
                generation,
            } => {
                event.kind = 1;
                event.width = transform.output.width;
                event.height = transform.output.height;
                event.generation = generation;
                event.x = transform.source.x;
                event.y = transform.source.y;
                event.desktop_width = transform.source.width;
                event.desktop_height = transform.source.height;
            }
            DesktopEvent::Frame(frame) => {
                event.kind = 2;
                event.frame = lease(frame);
            }
        }
        unsafe {
            output.write(event);
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_frame_source_time(
    frame: *const SnowMacFrame,
    index: u32,
    output: *mut SnowMacSourceTime,
) -> SnowMacStatus {
    boundary(|| {
        require_output(output)?;
        let frame = unsafe { frame.as_ref() }
            .ok_or_else(|| MacError::InvalidConfig("null frame".into()))?;
        let time = frame
            .lease
            .frame
            .source_times
            .get(index as usize)
            .ok_or_else(|| MacError::InvalidConfig("invalid source index".into()))?;
        unsafe {
            output.write(SnowMacSourceTime {
                value: time.value,
                timescale: time.timescale,
                epoch: time.epoch,
            });
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_stream_destroy(stream: *mut SnowMacStream) {
    if !stream.is_null() {
        unsafe {
            drop(Box::from_raw(stream));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_frame_retain(
    frame: *const SnowMacFrame,
) -> *mut SnowMacFrame {
    unsafe { frame.as_ref() }.map_or(ptr::null_mut(), |frame| {
        Box::into_raw(Box::new(SnowMacFrame {
            lease: frame.lease.clone(),
        }))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_frame_release(frame: *mut SnowMacFrame) {
    if !frame.is_null() {
        unsafe {
            drop(Box::from_raw(frame));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_frame_info(
    frame: *const SnowMacFrame,
    output: *mut SnowMacFrameInfo,
) -> SnowMacStatus {
    boundary(|| {
        let frame = &unsafe { frame.as_ref() }
            .ok_or_else(|| MacError::InvalidConfig("null frame".into()))?
            .lease
            .frame;
        require_output(output)?;
        let color = frame.image.color();
        let time = frame.source_times.first().ok_or(MacError::Inactive)?;
        unsafe {
            output.write(SnowMacFrameInfo {
                width: frame.image.size().width,
                height: frame.image.size().height,
                pixel_format: frame.image.format() as u32,
                plane_count: if matches!(
                    frame.image.format(),
                    snow_media::PixelFormat::Nv12 | snow_media::PixelFormat::P010
                ) {
                    2
                } else {
                    1
                },
                timestamp_value: time.value,
                timestamp_scale: time.timescale,
                timestamp_epoch: time.epoch,
                color_primaries: color.primaries as u32,
                transfer_function: color.transfer as u32,
                color_range: color.range as u32,
                alpha_mode: color.alpha as u32,
                generation: frame.generation,
                source_count: frame.source_times.len() as u32,
                duplicate: u32::from(frame.duplicate),
                desktop_x: frame.transform.source.x,
                desktop_y: frame.transform.source.y,
                desktop_width: frame.transform.source.width,
                desktop_height: frame.transform.source.height,
            });
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_frame_map_plane(
    frame: *const SnowMacFrame,
    index: u32,
    output: *mut SnowMacPlane,
) -> SnowMacStatus {
    boundary(|| {
        let frame = unsafe { frame.as_ref() }
            .ok_or_else(|| MacError::InvalidConfig("null frame".into()))?;
        require_output(output)?;
        map_plane(frame, index, frame.lease.frame.image.format(), output)
    })
}
/// Request CPU channel order explicitly. The lease caches each conversion;
/// returned bytes remain valid until the final retained frame is released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_frame_map_format(
    frame: *const SnowMacFrame,
    format: u32,
    index: u32,
    output: *mut SnowMacPlane,
) -> SnowMacStatus {
    boundary(|| {
        require_output(output)?;
        let frame = unsafe { frame.as_ref() }
            .ok_or_else(|| MacError::InvalidConfig("null frame".into()))?;
        let format = match format {
            0 => snow_media::PixelFormat::Rgba8,
            1 => snow_media::PixelFormat::Bgra8,
            2 => snow_media::PixelFormat::Rgba16Float,
            _ => {
                return Err(MacError::Unsupported(
                    "unsupported CPU capture format".into(),
                ));
            }
        };
        map_plane(frame, index, format, output)
    })
}
fn map_plane(
    frame: &SnowMacFrame,
    index: u32,
    format: snow_media::PixelFormat,
    output: *mut SnowMacPlane,
) -> Result<(), MacError> {
    let native = frame.lease.frame.image.format();
    if native != format
        && !(native == snow_media::PixelFormat::Bgra8 && format == snow_media::PixelFormat::Rgba8)
    {
        return Err(MacError::Unsupported(
            "CPU conversion requires an explicit color rendering policy".into(),
        ));
    }
    let cpu = frame.lease.cpu.get(format, || {
        frame
            .lease
            .frame
            .image
            .to_cpu()
            .map_err(|error| error.to_string())
    })?;
    let plane = cpu
        .planes
        .get(index as usize)
        .ok_or_else(|| MacError::InvalidConfig("invalid plane index".into()))?;
    let bytes = cpu
        .plane_bytes(index as usize)
        .ok_or_else(|| MacError::InvalidConfig("invalid CPU plane layout".into()))?;
    unsafe {
        output.write(SnowMacPlane {
            data: bytes.as_ptr(),
            len: bytes.len(),
            width: plane.width,
            height: plane.height,
            stride: plane.stride,
        });
    }
    Ok(())
}
/// Borrowed CVPixelBufferRef; retain the frame lease through asynchronous reads.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_frame_pixel_buffer(
    frame: *const SnowMacFrame,
) -> *const std::ffi::c_void {
    unsafe { frame.as_ref() }.map_or(ptr::null(), |f| unsafe {
        f.lease.frame.image.native_buffer() as *const _ as *const std::ffi::c_void
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_macos_permission_check() -> u8 {
    snow_macos::permission::screen_capture_authorized().into()
}
#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_macos_permission_request() -> u8 {
    snow_macos::permission::request_screen_capture_access().into()
}
#[unsafe(no_mangle)]
pub extern "C" fn snow_capture_macos_hdr_supported() -> u8 {
    (cfg!(target_arch = "aarch64") && snow_macos::permission::supported_os()).into()
}

#[repr(C)]
pub struct SnowMacTargetInfo {
    pub kind: u32,
    pub id: u32,
    pub process_id: i32,
    pub primary: u32,
    pub available: u32,
    pub pixel_width: u32,
    pub pixel_height: u32,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_enumerate(
    kind: u32,
    timeout_ms: u32,
    visitor: Option<unsafe extern "C" fn(*const SnowMacTargetInfo, *mut std::ffi::c_void)>,
    context: *mut std::ffi::c_void,
) -> SnowMacStatus {
    boundary(|| {
        let visitor =
            visitor.ok_or_else(|| MacError::InvalidConfig("null target visitor".into()))?;
        if timeout_ms == 0 || timeout_ms > 60_000 {
            return Err(MacError::InvalidConfig(
                "invalid enumeration timeout".into(),
            ));
        }
        let timeout = Duration::from_millis(u64::from(timeout_ms));
        match kind {
            1 => {
                for d in snow_macos::content::displays(timeout)? {
                    let info = SnowMacTargetInfo {
                        kind,
                        id: d.id,
                        process_id: 0,
                        primary: u32::from(d.primary),
                        available: 1,
                        pixel_width: d.pixels.width,
                        pixel_height: d.pixels.height,
                        x: d.bounds.x,
                        y: d.bounds.y,
                        width: d.bounds.width,
                        height: d.bounds.height,
                    };
                    unsafe {
                        visitor(&info, context);
                    }
                }
            }
            2 => {
                for w in snow_macos::content::windows(timeout)? {
                    let info = SnowMacTargetInfo {
                        kind,
                        id: w.id,
                        process_id: w.process_id,
                        primary: 0,
                        available: u32::from(w.on_screen),
                        pixel_width: w.pixels.map_or(0, |p| p.width),
                        pixel_height: w.pixels.map_or(0, |p| p.height),
                        x: w.bounds.x,
                        y: w.bounds.y,
                        width: w.bounds.width,
                        height: w.bounds.height,
                    };
                    unsafe {
                        visitor(&info, context);
                    }
                }
            }
            _ => {
                return Err(MacError::InvalidConfig(
                    "target kind must be display or window".into(),
                ));
            }
        }
        Ok(())
    })
}
/// Target query resolves current geometry without creating a stream or reading pixels.
#[repr(C)]
#[derive(Default)]
pub struct SnowMacCapabilities {
    pub struct_size: u32,
    pub hdr_supported: u32,
    pub native_format: u32,
    pub cpu_format_mask: u32,
    pub exclusion_filters: u32,
    pub source_count: u32,
    pub output_width: u32,
    pub output_height: u32,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_capture_macos_query_capabilities(
    config: *const SnowMacCaptureConfig,
    output: *mut SnowMacCapabilities,
) -> SnowMacStatus {
    boundary(|| {
        require_output(output)?;
        if config.is_null()
            || unsafe { config.cast::<u32>().read() } as usize != size_of::<SnowMacCaptureConfig>()
        {
            return Err(MacError::InvalidConfig(
                "invalid capture configuration header".into(),
            ));
        }
        let parsed = unsafe { &*config }.parse()?;
        let caps = parsed.capabilities()?;
        unsafe {
            output.write(SnowMacCapabilities {
                struct_size: size_of::<SnowMacCapabilities>() as u32,
                hdr_supported: caps.support.hdr_capture.into(),
                native_format: caps.native_format as u32,
                cpu_format_mask: caps
                    .cpu_formats
                    .iter()
                    .fold(0, |mask, format| mask | (1 << *format as u32)),
                exclusion_filters: caps.exclusion_filters.into(),
                source_count: caps.source_count as u32,
                output_width: caps.transform.output.width,
                output_height: caps.transform.output.height,
                x: caps.transform.source.x,
                y: caps.transform.source.y,
                width: caps.transform.source.width,
                height: caps.transform.source.height,
            });
        }
        Ok(())
    })
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn concurrent_cpu_formats_share_one_native_readback() {
        use snow_media::{ColorDescription, PixelFormat, PlaneLayout};
        use std::sync::atomic::{AtomicUsize, Ordering};
        let mappings = CpuMappings::default();
        let reads = AtomicUsize::new(0);
        std::thread::scope(|scope| {
            for format in [
                PixelFormat::Bgra8,
                PixelFormat::Rgba8,
                PixelFormat::Bgra8,
                PixelFormat::Rgba8,
            ] {
                let mappings = &mappings;
                let reads = &reads;
                scope.spawn(move || {
                    let frame = mappings
                        .get(format, || {
                            reads.fetch_add(1, Ordering::Relaxed);
                            Ok(CpuFrame {
                                size: PixelSize::new(1, 1).unwrap(),
                                format: PixelFormat::Bgra8,
                                color: ColorDescription::SRGB,
                                planes: vec![PlaneLayout {
                                    offset: 0,
                                    width: 1,
                                    height: 1,
                                    stride: 4,
                                    row_bytes: 4,
                                }],
                                bytes: Arc::from([10, 20, 30, 40]),
                            })
                        })
                        .unwrap();
                    let expected: &[u8] = if format == PixelFormat::Bgra8 {
                        &[10, 20, 30, 40]
                    } else {
                        &[30, 20, 10, 40]
                    };
                    assert_eq!(&*frame.bytes, expected);
                    assert_eq!(frame.format, format);
                });
            }
        });
        assert_eq!(reads.load(Ordering::Relaxed), 1);
    }
    #[test]
    fn cancellation_state_survives_c_handle_release() {
        let handle = snow_capture_macos_cancellation_create();
        assert!(!handle.is_null());
        let mut config: SnowMacCaptureConfig = unsafe { std::mem::zeroed() };
        config.struct_size = size_of::<SnowMacCaptureConfig>() as u32;
        config.fps = 30;
        config.timeout_ms = 100;
        config.cancellation = handle;
        let parsed = config.parse().unwrap();
        assert!(!parsed.cancellation.is_canceled());
        unsafe {
            snow_capture_macos_cancellation_cancel(handle);
        }
        assert!(parsed.cancellation.is_canceled());
        unsafe {
            snow_capture_macos_cancellation_release(handle);
        }
        assert!(matches!(
            DesktopSession::new(parsed),
            Err(MacError::Canceled)
        ));
        unsafe {
            snow_capture_macos_cancellation_cancel(ptr::null());
            snow_capture_macos_cancellation_release(ptr::null_mut());
        }
    }
    #[test]
    fn canceled_c_acquisition_never_starts_native_work() {
        let handle = snow_capture_macos_cancellation_create();
        let mut config: SnowMacCaptureConfig = unsafe { std::mem::zeroed() };
        config.struct_size = size_of::<SnowMacCaptureConfig>() as u32;
        config.fps = 30;
        config.timeout_ms = 100;
        config.cancellation = handle;
        unsafe {
            snow_capture_macos_cancellation_cancel(handle);
        }
        let mut frame = ptr::dangling_mut();
        let mut stream = ptr::dangling_mut();
        assert_eq!(
            unsafe { snow_capture_macos_snapshot(&config, &mut frame) },
            SnowMacStatus::Canceled
        );
        assert_eq!(
            unsafe { snow_capture_macos_stream_create(&config, &mut stream) },
            SnowMacStatus::Canceled
        );
        let mut capabilities = SnowMacCapabilities::default();
        assert_eq!(
            unsafe { snow_capture_macos_query_capabilities(&config, &mut capabilities) },
            SnowMacStatus::Canceled
        );
        assert!(frame.is_null());
        assert!(stream.is_null());
        unsafe {
            snow_capture_macos_cancellation_release(handle);
        }
    }
    #[test]
    fn old_configuration_header_is_rejected_before_full_struct_access() {
        let header: u32 = 4;
        let config = (&header as *const u32).cast();
        let mut output = ptr::dangling_mut();
        assert_eq!(
            unsafe { snow_capture_macos_snapshot(config, &mut output) },
            SnowMacStatus::InvalidArgument
        );
        assert!(output.is_null());
        let mut stream = ptr::dangling_mut();
        assert_eq!(
            unsafe { snow_capture_macos_stream_create(config, &mut stream) },
            SnowMacStatus::InvalidArgument
        );
        assert!(stream.is_null());
    }
    #[test]
    fn configuration_copies_exclusions_and_checks_geometry_and_pointer_counts() {
        let mut excluded = [7, 9];
        let mut config = SnowMacCaptureConfig {
            struct_size: size_of::<SnowMacCaptureConfig>() as u32,
            cancellation: ptr::null(),
            target_kind: 3,
            target_id: 0,
            dynamic_range: 0,
            cursor_mode: 0,
            fps: 30,
            timeout_ms: 1000,
            source_x: -100.5,
            source_y: 0.25,
            source_width: 200.0,
            source_height: 100.0,
            output_width: 400,
            output_height: 200,
            excluded_windows: excluded.as_ptr(),
            excluded_window_count: excluded.len(),
            excluded_processes: ptr::null(),
            excluded_process_count: 0,
        };
        let parsed = config.parse().unwrap();
        excluded[0] = 12;
        assert_eq!(parsed.excluded_windows, [7, 9]);
        assert_ne!(parsed.excluded_windows[0], excluded[0]);
        assert_eq!(parsed.output, Some(PixelSize::new(400, 200).unwrap()));
        assert!(matches!(
            parsed.target,
            DesktopTarget::Region(DesktopRect {
                space: DesktopSpace::Points,
                x: -100.5,
                ..
            })
        ));
        config.excluded_window_count = 4097;
        assert!(config.parse().is_err());
        config.excluded_window_count = 1;
        config.excluded_windows = ptr::null();
        assert!(config.parse().is_err());
        config.excluded_window_count = 0;
        config.source_width = f64::NAN;
        assert!(config.parse().is_err());
    }
    #[test]
    fn null_arguments_produce_typed_errors() {
        assert_eq!(
            unsafe { snow_capture_macos_query_capabilities(ptr::null(), ptr::null_mut()) },
            SnowMacStatus::InvalidArgument
        );
        assert_eq!(
            unsafe { snow_capture_macos_frame_map_format(ptr::null(), 0, 0, ptr::null_mut()) },
            SnowMacStatus::InvalidArgument
        );
        assert_eq!(
            unsafe { snow_capture_macos_snapshot(ptr::null(), ptr::null_mut()) },
            SnowMacStatus::InvalidArgument
        );
        assert!(unsafe { snow_capture_macos_frame_retain(ptr::null()) }.is_null());
        unsafe {
            snow_capture_macos_frame_release(ptr::null_mut());
            snow_capture_macos_stream_destroy(ptr::null_mut());
        }
    }
}

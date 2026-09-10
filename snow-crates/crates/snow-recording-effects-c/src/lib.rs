//! Owned C ABI for live effects. Frame leases are independent of the observing session.
#![allow(clippy::missing_safety_doc)]
use snow_recording_effects::KeyboardOverlayConfig;
use snow_recording_effects::preview::{PreviewConfig, PreviewFrame, PreviewSession};
use snow_recording_effects::surface::TILE_SIZE;
use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char, c_void};
use std::sync::Arc;

thread_local! { static ERROR: RefCell<CString> = RefCell::new(CString::default()); }
fn error(message: String) {
    ERROR.with(|e| *e.borrow_mut() = CString::new(message.replace('\0', " ")).unwrap());
}

#[repr(C)]
pub struct SnowRecordingEffectsKeyLabel {
    pub virtual_key: u32,
    pub label_utf8: *const c_char,
}
#[repr(C)]
pub struct SnowRecordingEffectsConfig {
    pub version: u32,
    pub struct_size: u32,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub output_width: u32,
    pub output_height: u32,
    pub trail_rgba: u32,
    pub click_rgba: u32,
    pub show_keyboard: u32,
    pub keyboard_background_rgba: u32,
    pub keyboard_text_rgba: u32,
    pub keyboard_border_rgba: u32,
    pub labels: *const SnowRecordingEffectsKeyLabel,
    pub label_count: u32,
    pub trail_duration_ms: u32,
    pub generation: u64,
    pub keyboard_size: u32,
}
#[repr(C)]
pub struct SnowRecordingEffectsTile {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub rgba_premultiplied: *const u8,
}
#[repr(C)]
pub struct SnowRecordingEffectsFrameInfo {
    pub generation: u64,
    pub revision: u64,
    pub width: u32,
    pub height: u32,
    pub tiles: *const SnowRecordingEffectsTile,
    pub tile_count: u32,
    pub error_utf8: *const c_char,
}
pub struct SnowRecordingEffects {
    config: PreviewConfig,
    notify: Arc<dyn Fn() + Send + Sync>,
    session: Option<PreviewSession>,
}
pub struct SnowRecordingEffectsFrame {
    frame: Arc<PreviewFrame>,
    tiles: Vec<SnowRecordingEffectsTile>,
    keyboard_tiles: Vec<SnowRecordingEffectsTile>,
    error: Option<CString>,
}

unsafe fn config(raw: *const SnowRecordingEffectsConfig) -> Result<PreviewConfig, String> {
    if raw.is_null() {
        return Err("missing effects configuration".into());
    }
    // Read the fixed header before accessing the rest of a potentially older structure.
    let header = raw.cast::<u32>();
    let version = unsafe { *header };
    let size = match version {
        1 | 2 => std::mem::offset_of!(SnowRecordingEffectsConfig, keyboard_size),
        3 => std::mem::size_of::<SnowRecordingEffectsConfig>(),
        _ => return Err("unsupported effects configuration version".into()),
    };
    if unsafe { *header.add(1) } != size as u32 {
        return Err("unsupported effects configuration version or size".into());
    }
    let mut value: SnowRecordingEffectsConfig = unsafe { std::mem::zeroed() };
    unsafe {
        std::ptr::copy_nonoverlapping(raw.cast::<u8>(), (&raw mut value).cast::<u8>(), size);
    }
    let raw = &value;
    if raw.show_keyboard > 1
        || (raw.version == 1 && raw.trail_duration_ms != 0)
        || (raw.version >= 3 && !(32..=128).contains(&raw.keyboard_size))
        || raw.label_count > 256
        || (raw.label_count != 0 && raw.labels.is_null())
    {
        return Err("invalid effects options or key labels".into());
    }
    let mut labels = std::collections::BTreeMap::new();
    for index in 0..raw.label_count as usize {
        let label = unsafe { &*raw.labels.add(index) };
        if label.virtual_key > 255 || label.label_utf8.is_null() {
            return Err("invalid key label".into());
        }
        let text = unsafe { CStr::from_ptr(label.label_utf8) }
            .to_str()
            .map_err(|e| e.to_string())?;
        if text.len() > 1024 {
            return Err("key label exceeds 1024 UTF-8 bytes".into());
        }
        labels.insert(label.virtual_key as u16, text.to_owned());
    }
    let value = PreviewConfig {
        region: (raw.x, raw.y, raw.width, raw.height),
        output: (raw.output_width, raw.output_height),
        trail: raw.trail_rgba.to_be_bytes(),
        trail_duration_ms: if raw.version == 1 {
            500
        } else {
            u64::from(raw.trail_duration_ms)
        },
        click: raw.click_rgba.to_be_bytes(),
        generation: raw.generation,
        keyboard: (raw.show_keyboard != 0).then_some(KeyboardOverlayConfig {
            keycap_size: if raw.version < 3 {
                64
            } else {
                raw.keyboard_size
            },
            background_rgba: raw.keyboard_background_rgba.to_be_bytes(),
            text_rgba: raw.keyboard_text_rgba.to_be_bytes(),
            border_rgba: raw.keyboard_border_rgba.to_be_bytes(),
            labels,
        }),
    };
    value.validate()?;
    Ok(value)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_effects_create(
    raw: *const SnowRecordingEffectsConfig,
    notify: Option<unsafe extern "C" fn(*mut c_void)>,
    context: *mut c_void,
) -> *mut SnowRecordingEffects {
    let config = match unsafe { config(raw) } {
        Ok(value) => value,
        Err(e) => {
            error(e);
            return std::ptr::null_mut();
        }
    };
    let address = context as usize;
    let notify = Arc::new(move || {
        if let Some(callback) = notify {
            unsafe {
                callback(address as *mut c_void);
            }
        }
    });
    Box::into_raw(Box::new(SnowRecordingEffects {
        config,
        notify,
        session: None,
    }))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_effects_configure(
    handle: *mut SnowRecordingEffects,
    raw: *const SnowRecordingEffectsConfig,
) -> i32 {
    let Some(handle) = (unsafe { handle.as_mut() }) else {
        return 0;
    };
    let value = match unsafe { config(raw) } {
        Ok(v) => v,
        Err(e) => {
            error(e);
            return 0;
        }
    };
    if handle.config == value {
        return 1;
    }
    if let Some(session) = &handle.session
        && let Err(e) = session.configure(value.clone())
    {
        error(e);
        return 0;
    }
    handle.config = value;
    1
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_effects_set_active(
    handle: *mut SnowRecordingEffects,
    active: i32,
) -> i32 {
    let Some(handle) = (unsafe { handle.as_mut() }) else {
        return 0;
    };
    if active == 0 {
        handle.session = None;
        return 1;
    }
    if handle.session.is_none() {
        match PreviewSession::start(handle.config.clone(), Arc::clone(&handle.notify)) {
            Ok(session) => handle.session = Some(session),
            Err(e) => {
                error(e);
                return 0;
            }
        }
    }
    1
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_effects_stop(handle: *mut SnowRecordingEffects) {
    if let Some(handle) = unsafe { handle.as_mut() } {
        handle.session = None;
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_effects_destroy(handle: *mut SnowRecordingEffects) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle) });
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_effects_acquire_frame(
    handle: *mut SnowRecordingEffects,
) -> *mut SnowRecordingEffectsFrame {
    let frame = unsafe { handle.as_ref() }
        .and_then(|h| h.session.as_ref())
        .and_then(PreviewSession::latest);
    let Some(frame) = frame else {
        return std::ptr::null_mut();
    };
    let tiles = frame
        .tiles
        .iter()
        .map(|tile| SnowRecordingEffectsTile {
            x: tile.x,
            y: tile.y,
            width: TILE_SIZE.min(frame.output.0 - tile.x),
            height: TILE_SIZE.min(frame.output.1 - tile.y),
            stride: TILE_SIZE * 4,
            rgba_premultiplied: tile.pixels.as_ptr(),
        })
        .collect();
    let error = frame
        .error
        .as_ref()
        .map(|e| CString::new(e.replace('\0', " ")).unwrap());
    let keyboard_tiles = frame
        .keyboard_tiles
        .iter()
        .map(|tile| SnowRecordingEffectsTile {
            x: tile.x,
            y: tile.y,
            width: TILE_SIZE.min(frame.keyboard_output.0 - tile.x),
            height: TILE_SIZE.min(frame.keyboard_output.1 - tile.y),
            stride: TILE_SIZE * 4,
            rgba_premultiplied: tile.pixels.as_ptr(),
        })
        .collect();
    Box::into_raw(Box::new(SnowRecordingEffectsFrame {
        frame,
        tiles,
        keyboard_tiles,
        error,
    }))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_effects_frame_info(
    frame: *const SnowRecordingEffectsFrame,
    info: *mut SnowRecordingEffectsFrameInfo,
) -> i32 {
    let (Some(frame), Some(info)) = (unsafe { frame.as_ref() }, unsafe { info.as_mut() }) else {
        return 0;
    };
    *info = SnowRecordingEffectsFrameInfo {
        generation: frame.frame.generation,
        revision: frame.frame.revision,
        width: frame.frame.output.0,
        height: frame.frame.output.1,
        tiles: frame.tiles.as_ptr(),
        tile_count: frame.tiles.len() as u32,
        error_utf8: frame
            .error
            .as_ref()
            .map_or(std::ptr::null(), |s| s.as_ptr()),
    };
    1
}
/// Keyboard tiles are a separate, complete layer in physical capture coordinates.
/// Both layer views borrow the same immutable frame lease.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_effects_frame_keyboard_info(
    frame: *const SnowRecordingEffectsFrame,
    info: *mut SnowRecordingEffectsFrameInfo,
) -> i32 {
    if unsafe { snow_recording_effects_frame_info(frame, info) } == 0 {
        return 0;
    }
    let frame = unsafe { &*frame };
    let info = unsafe { &mut *info };
    info.width = frame.frame.keyboard_output.0;
    info.height = frame.frame.keyboard_output.1;
    info.tiles = frame.keyboard_tiles.as_ptr();
    info.tile_count = frame.keyboard_tiles.len() as u32;
    1
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_effects_release_frame(
    frame: *mut SnowRecordingEffectsFrame,
) {
    if !frame.is_null() {
        drop(unsafe { Box::from_raw(frame) });
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn snow_recording_effects_last_error() -> *const c_char {
    ERROR.with(|e| e.borrow().as_ptr())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;
    #[test]
    fn duration_versions_and_bounds() {
        let mut raw = valid();
        assert_eq!(unsafe { config(&raw) }.unwrap().trail_duration_ms, 500);
        raw.version = 2;
        raw.struct_size = std::mem::offset_of!(SnowRecordingEffectsConfig, keyboard_size) as u32;
        for duration in [100, 500, 2000] {
            raw.trail_duration_ms = duration;
            assert_eq!(
                unsafe { config(&raw) }.unwrap().trail_duration_ms,
                u64::from(duration)
            );
        }
        for duration in [0, 99, 2001] {
            raw.trail_duration_ms = duration;
            assert!(unsafe { config(&raw) }.is_err());
        }
        raw.version = 1;
        assert!(unsafe { config(&raw) }.is_err());
    }

    fn valid() -> SnowRecordingEffectsConfig {
        SnowRecordingEffectsConfig {
            version: 3,
            struct_size: std::mem::size_of::<SnowRecordingEffectsConfig>() as u32,
            x: -1920,
            y: -100,
            width: 1920,
            height: 1080,
            output_width: 1280,
            output_height: 720,
            trail_rgba: 0,
            click_rgba: 0,
            show_keyboard: 0,
            keyboard_background_rgba: 0,
            keyboard_text_rgba: 0,
            keyboard_border_rgba: 0,
            labels: std::ptr::null(),
            label_count: 0,
            trail_duration_ms: 500,
            generation: 12,
            keyboard_size: 64,
        }
    }
    #[test]
    fn validates_headers_dimensions_and_copies_bounded_labels() {
        assert!(unsafe { config(std::ptr::null()) }.is_err());
        let mut raw = valid();
        raw.struct_size = 8;
        assert!(unsafe { config(&raw) }.is_err());
        raw = valid();
        raw.width = 0;
        assert!(unsafe { config(&raw) }.is_err());
        raw = valid();
        raw.label_count = 1;
        assert!(unsafe { config(&raw) }.is_err());
        let text = CString::new("Control").unwrap();
        let label = SnowRecordingEffectsKeyLabel {
            virtual_key: 0x11,
            label_utf8: text.as_ptr(),
        };
        raw.labels = &label;
        raw.show_keyboard = 1;
        raw.keyboard_text_rgba = 0x12345678;
        let copied = unsafe { config(&raw) }.unwrap();
        drop(text);
        let keyboard = copied.keyboard.unwrap();
        assert_eq!(keyboard.labels[&0x11], "Control");
        assert_eq!(keyboard.text_rgba, [0x12, 0x34, 0x56, 0x78]);
    }

    #[test]
    fn keyboard_size_is_validated_and_old_configs_keep_default_size() {
        let mut raw = valid();
        raw.show_keyboard = 1;
        for size in [32, 64, 96, 128] {
            raw.keyboard_size = size;
            assert_eq!(
                unsafe { config(&raw) }
                    .unwrap()
                    .keyboard
                    .unwrap()
                    .keycap_size,
                size
            );
        }
        for size in [0, 31, 129] {
            raw.keyboard_size = size;
            assert!(unsafe { config(&raw) }.is_err());
        }
        raw.version = 2;
        raw.struct_size = std::mem::offset_of!(SnowRecordingEffectsConfig, keyboard_size) as u32;
        assert_eq!(
            unsafe { config(&raw) }
                .unwrap()
                .keyboard
                .unwrap()
                .keycap_size,
            64
        );
    }

    unsafe extern "C" fn notify(context: *mut c_void) {
        let sender = unsafe { &*context.cast::<std::sync::mpsc::Sender<()>>() };
        let _ = sender.send(());
    }
    #[test]
    fn idle_session_is_quiet_and_frame_lease_survives_join_and_destroy() {
        let (sender, receiver) = std::sync::mpsc::channel::<()>();
        let raw = valid();
        let handle = unsafe {
            snow_recording_effects_create(&raw, Some(notify), (&raw const sender).cast_mut().cast())
        };
        assert!(!handle.is_null());
        assert_eq!(unsafe { snow_recording_effects_set_active(handle, 1) }, 1);
        receiver.recv_timeout(Duration::from_secs(2)).unwrap();
        let frame = unsafe { snow_recording_effects_acquire_frame(handle) };
        assert!(!frame.is_null());
        assert!(matches!(
            receiver.recv_timeout(Duration::from_millis(75)),
            Err(std::sync::mpsc::RecvTimeoutError::Timeout)
        ));
        unsafe {
            snow_recording_effects_stop(handle);
            snow_recording_effects_destroy(handle);
        }
        let mut info = std::mem::MaybeUninit::uninit();
        assert_eq!(
            unsafe { snow_recording_effects_frame_info(frame, info.as_mut_ptr()) },
            1
        );
        let info = unsafe { info.assume_init() };
        assert_eq!(info.generation, 12);
        assert_eq!((info.width, info.height), (1280, 720));
        assert_eq!(info.tile_count, 0);
        assert!(info.error_utf8.is_null());
        let mut keyboard = std::mem::MaybeUninit::uninit();
        assert_eq!(
            unsafe { snow_recording_effects_frame_keyboard_info(frame, keyboard.as_mut_ptr()) },
            1
        );
        let keyboard = unsafe { keyboard.assume_init() };
        assert_eq!(keyboard.generation, 12);
        assert_eq!((keyboard.width, keyboard.height), (1920, 1080));
        assert_eq!(keyboard.tile_count, 0);
        assert_eq!(
            unsafe { snow_recording_effects_frame_keyboard_info(frame, std::ptr::null_mut()) },
            0
        );
        assert!(receiver.try_recv().is_err());
        unsafe {
            snow_recording_effects_release_frame(frame);
        }
    }
}

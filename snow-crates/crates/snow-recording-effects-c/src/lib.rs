//! Owned C ABI for live effects. Frame leases are independent of the observing session.
#![allow(clippy::missing_safety_doc)]
use snow_recording_effects::preview::{PreviewConfig, PreviewFrame, PreviewSession};
use snow_recording_effects::surface::TILE_SIZE;
use snow_recording_effects::{KeyboardFilterConfig, KeyboardOverlayConfig};
use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char, c_void};
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

thread_local! { static ERROR: RefCell<CString> = RefCell::new(CString::default()); }
fn error(message: String) {
    ERROR.with(|e| *e.borrow_mut() = CString::new(message.replace('\0', " ")).unwrap());
}

// Makes the exactly-once destroy contract observable: a missed destroy leaves the
// count above its baseline and a repeated destroy wraps it far below, so tests
// can fail loudly instead of leaking native observers silently.
static LIVE_HANDLES: AtomicUsize = AtomicUsize::new(0);

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
    pub reserved_v3: u32,
    pub highlight_rgba: u32,
    pub record_mouse_clicks: u32,
    pub keyboard_font_family_utf8: *const c_char,
    pub keyboard_cjk_font_family_utf8: *const c_char,
    pub keyboard_font_weight: u32,
    pub canvas_scale: f64,
    pub highlight_radius: u32,
    pub webcam_source: *const snow_webcam::Source,
    pub webcam_size: u32,
    pub webcam_shape: u32,
    pub webcam_offset_left: u32,
    pub webcam_offset_bottom: u32,
    pub record_mouse_scrolling: u32,
    pub webcam_orientation: u32,
    pub record_number_keys: u32,
    pub record_letters_and_symbols: u32,
    pub record_control_keys: u32,
    pub preserve_keyboard_shortcuts: u32,
}
const EFFECTS_CONFIG_V11_SIZE: usize =
    std::mem::offset_of!(SnowRecordingEffectsConfig, record_number_keys);
const EFFECTS_CONFIG_V10_FIELDS_SIZE: usize =
    std::mem::offset_of!(SnowRecordingEffectsConfig, webcam_orientation);
const EFFECTS_CONFIG_V10_SIZE: usize = EFFECTS_CONFIG_V10_FIELDS_SIZE
    .div_ceil(std::mem::align_of::<SnowRecordingEffectsConfig>())
    * std::mem::align_of::<SnowRecordingEffectsConfig>();
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

fn preview_canvas_dimensions(width: u32, height: u32, scale: f64) -> Result<(u32, u32), String> {
    if !scale.is_finite() || scale <= 0.0 {
        return Err("invalid preview canvas scale".into());
    }
    let canvas = (
        (f64::from(width) * scale).round() as u32,
        (f64::from(height) * scale).round() as u32,
    );
    if [width, height, canvas.0, canvas.1]
        .into_iter()
        .any(|value| value == 0 || value > 32768)
    {
        return Err("effect dimensions must be between 1 and 32768 pixels".into());
    }
    Ok(canvas)
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
        3 => std::mem::offset_of!(SnowRecordingEffectsConfig, highlight_rgba),
        4 => std::mem::offset_of!(SnowRecordingEffectsConfig, keyboard_font_family_utf8),
        5 => std::mem::offset_of!(SnowRecordingEffectsConfig, canvas_scale),
        6 => std::mem::offset_of!(SnowRecordingEffectsConfig, highlight_radius),
        7 => std::mem::offset_of!(SnowRecordingEffectsConfig, webcam_source),
        8 => std::mem::offset_of!(SnowRecordingEffectsConfig, webcam_offset_left),
        9 => std::mem::offset_of!(SnowRecordingEffectsConfig, record_mouse_scrolling),
        // v11 occupies v10 tail padding; its original sizeof is still required.
        10 => EFFECTS_CONFIG_V10_SIZE,
        11 => EFFECTS_CONFIG_V11_SIZE,
        12 => std::mem::size_of::<SnowRecordingEffectsConfig>(),
        _ => return Err("unsupported effects configuration version".into()),
    };
    if unsafe { *header.add(1) } != size as u32 {
        return Err("unsupported effects configuration version or size".into());
    }
    let mut value: SnowRecordingEffectsConfig = unsafe { std::mem::zeroed() };
    // The old caller's tail padding is not an initialized orientation field.
    let copy_size = if version == 10 {
        EFFECTS_CONFIG_V10_FIELDS_SIZE
    } else {
        size
    };
    unsafe {
        std::ptr::copy_nonoverlapping(raw.cast::<u8>(), (&raw mut value).cast::<u8>(), copy_size);
    }
    let raw = &value;
    let orientation = snow_webcam::WebcamOrientation::from_raw(if raw.version >= 11 {
        raw.webcam_orientation
    } else {
        0
    })?;
    let canvas_scale = if raw.version >= 6 {
        raw.canvas_scale
    } else {
        1.0
    };
    let canvas = preview_canvas_dimensions(raw.width, raw.height, canvas_scale)?;
    if raw.show_keyboard > 1
        || (raw.version >= 4 && raw.record_mouse_clicks > 1)
        || (raw.version >= 10 && raw.record_mouse_scrolling > 1)
        || (raw.version >= 12
            && [
                raw.record_number_keys,
                raw.record_letters_and_symbols,
                raw.record_control_keys,
                raw.preserve_keyboard_shortcuts,
            ]
            .into_iter()
            .any(|value| value > 1))
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
        if (label.virtual_key > 255 && !(0x200..=0x206).contains(&label.virtual_key))
            || label.label_utf8.is_null()
        {
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
        webcam: if raw.version >= 8 {
            unsafe {
                snow_webcam::config_from_raw(raw.webcam_source, raw.webcam_size, raw.webcam_shape)?
                    .map(|mut webcam| {
                        webcam.style.orientation = orientation;
                        if raw.version >= 9 {
                            webcam.style.offset_left = raw.webcam_offset_left;
                            webcam.style.offset_bottom = raw.webcam_offset_bottom;
                        }
                        webcam
                    })
            }
        } else {
            None
        },
        region: (raw.x, raw.y, raw.width, raw.height),
        canvas,
        output: (raw.output_width, raw.output_height),
        trail: raw.trail_rgba.to_be_bytes(),
        trail_duration_ms: if raw.version == 1 {
            500
        } else {
            u64::from(raw.trail_duration_ms)
        },
        click: raw.click_rgba.to_be_bytes(),
        highlight_radius: if raw.version >= 7 {
            raw.highlight_radius
        } else {
            snow_recording_effects::mouse_effects::HIGHLIGHT_RADIUS as u32
        },
        highlight: if raw.version >= 4 {
            raw.highlight_rgba.to_be_bytes()
        } else {
            [0; 4]
        },
        record_mouse_clicks: raw.version >= 4 && raw.record_mouse_clicks != 0,
        record_mouse_scrolling: raw.version >= 10 && raw.record_mouse_scrolling != 0,
        show_keyboard: raw.show_keyboard != 0,
        keyboard_filter: if raw.version >= 12 {
            KeyboardFilterConfig {
                record_number_keys: raw.record_number_keys != 0,
                record_letters_and_symbols: raw.record_letters_and_symbols != 0,
                record_control_keys: raw.record_control_keys != 0,
                preserve_keyboard_shortcuts: raw.preserve_keyboard_shortcuts != 0,
            }
        } else {
            Default::default()
        },
        generation: raw.generation,
        keyboard: (raw.show_keyboard != 0
            || (raw.version >= 4 && raw.record_mouse_clicks != 0)
            || (raw.version >= 10 && raw.record_mouse_scrolling != 0))
            .then_some(KeyboardOverlayConfig {
                font: if raw.version >= 5 && !raw.keyboard_font_family_utf8.is_null() {
                    if raw.keyboard_cjk_font_family_utf8.is_null() {
                        return Err("missing keyboard CJK font family".into());
                    }
                    Some(snow_recording_effects::KeyboardOverlayFont::new(
                        unsafe { CStr::from_ptr(raw.keyboard_font_family_utf8) }
                            .to_str()
                            .map_err(|e| e.to_string())?,
                        unsafe { CStr::from_ptr(raw.keyboard_cjk_font_family_utf8) }
                            .to_str()
                            .map_err(|e| e.to_string())?,
                        raw.keyboard_font_weight,
                    )?)
                } else {
                    None
                },
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
    LIVE_HANDLES.fetch_add(1, Ordering::Relaxed);
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
        LIVE_HANDLES.fetch_sub(1, Ordering::Relaxed);
    }
}

/// Live handles created by `snow_recording_effects_create` and not yet destroyed.
#[unsafe(no_mangle)]
pub extern "C" fn snow_recording_effects_live_handle_count() -> usize {
    LIVE_HANDLES.load(Ordering::Relaxed)
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
    lease_frame(frame)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_effects_playback_preview(
    width: u32,
    height: u32,
    canvas_scale: f64,
    overlay: u32,
    progress_rgba: u32,
    font_family_utf8: *const c_char,
    cjk_font_family_utf8: *const c_char,
    font_weight: u32,
) -> *mut SnowRecordingEffectsFrame {
    let render = || -> Result<_, String> {
        use snow_recording_effects::{KeyboardOverlayFont, PlaybackOverlay};
        let output = preview_canvas_dimensions(width, height, canvas_scale)?;
        let overlay = match overlay {
            1 => PlaybackOverlay::ProgressBar {
                rgba: progress_rgba.to_be_bytes(),
            },
            2 => PlaybackOverlay::PlaybackTime { rgba: [255; 4] },
            _ => return Err("invalid playback preview overlay".into()),
        };
        let rasterizer = if matches!(overlay, PlaybackOverlay::PlaybackTime { .. }) {
            let font = if font_family_utf8.is_null() {
                None
            } else {
                if cjk_font_family_utf8.is_null() {
                    return Err("missing playback CJK font family".into());
                }
                Some(KeyboardOverlayFont::new(
                    unsafe { CStr::from_ptr(font_family_utf8) }
                        .to_str()
                        .map_err(|e| e.to_string())?,
                    unsafe { CStr::from_ptr(cjk_font_family_utf8) }
                        .to_str()
                        .map_err(|e| e.to_string())?,
                    font_weight,
                )?)
            };
            Some(snow_recording_effects::keyboard_rasterizer::create(
                &KeyboardOverlayConfig {
                    font,
                    keycap_size: 64,
                    background_rgba: [0; 4],
                    text_rgba: [255; 4],
                    border_rgba: [0; 4],
                    labels: Default::default(),
                },
            )?)
        } else {
            None
        };
        let mut renderer =
            snow_recording_effects::playback::PlaybackRenderer::new(overlay, output, rasterizer)?;
        let mut surface = snow_recording_effects::surface::TileSurface::new(output);
        renderer.draw_zero_preview(&mut surface)?;
        Ok(PreviewFrame {
            generation: 0,
            revision: 0,
            output,
            tiles: surface.snapshot(),
            keyboard_output: output,
            keyboard_tiles: Vec::new(),
            error: None,
        })
    };
    match render() {
        Ok(frame) => lease_frame(Arc::new(frame)),
        Err(message) => {
            error(message);
            std::ptr::null_mut()
        }
    }
}

fn lease_frame(frame: Arc<PreviewFrame>) -> *mut SnowRecordingEffectsFrame {
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
/// Keyboard tiles are a separate, complete layer in desktop capture coordinates.
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

    #[test]
    fn keyboard_filter_flags_round_trip_and_validate() {
        for mask in 0..16 {
            let mut raw = valid();
            raw.record_number_keys = mask & 1;
            raw.record_letters_and_symbols = (mask >> 1) & 1;
            raw.record_control_keys = (mask >> 2) & 1;
            raw.preserve_keyboard_shortcuts = (mask >> 3) & 1;
            let filter = unsafe { config(&raw) }.unwrap().keyboard_filter;
            assert_eq!(filter.record_number_keys, mask & 1 != 0);
            assert_eq!(filter.record_letters_and_symbols, mask & 2 != 0);
            assert_eq!(filter.record_control_keys, mask & 4 != 0);
            assert_eq!(filter.preserve_keyboard_shortcuts, mask & 8 != 0);
        }
        for index in 0..4 {
            let mut raw = valid();
            let fields = [
                &mut raw.record_number_keys,
                &mut raw.record_letters_and_symbols,
                &mut raw.record_control_keys,
                &mut raw.preserve_keyboard_shortcuts,
            ];
            *fields.into_iter().nth(index).unwrap() = 2;
            assert!(unsafe { config(&raw) }.is_err());
        }
    }

    #[test]
    fn keyboard_filter_legacy_prefixes_enable_every_option() {
        for (version, size) in [
            (
                1,
                std::mem::offset_of!(SnowRecordingEffectsConfig, keyboard_size),
            ),
            (
                2,
                std::mem::offset_of!(SnowRecordingEffectsConfig, keyboard_size),
            ),
            (
                3,
                std::mem::offset_of!(SnowRecordingEffectsConfig, highlight_rgba),
            ),
            (
                4,
                std::mem::offset_of!(SnowRecordingEffectsConfig, keyboard_font_family_utf8),
            ),
            (
                5,
                std::mem::offset_of!(SnowRecordingEffectsConfig, canvas_scale),
            ),
            (
                6,
                std::mem::offset_of!(SnowRecordingEffectsConfig, highlight_radius),
            ),
            (
                7,
                std::mem::offset_of!(SnowRecordingEffectsConfig, webcam_source),
            ),
            (
                8,
                std::mem::offset_of!(SnowRecordingEffectsConfig, webcam_offset_left),
            ),
            (
                9,
                std::mem::offset_of!(SnowRecordingEffectsConfig, record_mouse_scrolling),
            ),
            (10, EFFECTS_CONFIG_V10_SIZE),
            (11, EFFECTS_CONFIG_V11_SIZE),
        ] {
            let mut raw = valid();
            raw.version = version;
            raw.struct_size = size as u32;
            if version == 1 {
                raw.trail_duration_ms = 0;
            }
            raw.record_number_keys = u32::MAX;
            raw.record_letters_and_symbols = u32::MAX;
            raw.record_control_keys = u32::MAX;
            raw.preserve_keyboard_shortcuts = u32::MAX;
            let prefix =
                unsafe { std::slice::from_raw_parts((&raw const raw).cast::<u8>(), size) }.to_vec();
            assert_eq!(
                unsafe { config(prefix.as_ptr().cast()) }
                    .unwrap()
                    .keyboard_filter,
                KeyboardFilterConfig::default(),
                "legacy version {version}"
            );
        }
        let mut raw = valid();
        raw.struct_size -= 1;
        assert!(unsafe { config(&raw) }.is_err());
    }

    #[test]
    fn scrolling_only_and_previous_abi_layout() {
        let mut raw = valid();
        raw.record_mouse_scrolling = 1;
        let parsed = unsafe { config(&raw) }.unwrap();
        assert!(parsed.record_mouse_scrolling && parsed.keyboard.is_some());
        assert!(!parsed.show_keyboard && !parsed.record_mouse_clicks);
        raw.record_mouse_scrolling = 2;
        assert!(unsafe { config(&raw) }.is_err());
        raw.version = 9;
        raw.struct_size =
            std::mem::offset_of!(SnowRecordingEffectsConfig, record_mouse_scrolling) as u32;
        let parsed = unsafe { config(&raw) }.unwrap();
        assert!(!parsed.record_mouse_scrolling && parsed.keyboard.is_none());
    }
    use std::time::Duration;
    #[test]
    fn playback_preview_validates_options_and_returns_an_owned_static_frame() {
        let render = |width, height, overlay| unsafe {
            snow_recording_effects_playback_preview(
                width,
                height,
                1.0,
                overlay,
                0x1677ffff,
                std::ptr::null(),
                std::ptr::null(),
                0,
            )
        };
        for (width, height, overlay) in [(0, 480, 1), (640, 0, 1), (32769, 480, 1), (640, 480, 3)] {
            assert!(render(width, height, overlay).is_null());
        }
        for overlay in [1, 2] {
            let frame = render(640, 480, overlay);
            assert!(!frame.is_null());
            let mut info = unsafe { std::mem::zeroed() };
            assert_eq!(
                unsafe { snow_recording_effects_frame_info(frame, &mut info) },
                1
            );
            assert_eq!((info.width, info.height), (640, 480));
            assert!(info.tile_count > 0);
            assert!(info.error_utf8.is_null());
            unsafe { snow_recording_effects_release_frame(frame) };
        }
    }

    #[test]
    fn playback_preview_uses_the_mouse_effects_display_canvas_and_fixed_pixel_styles() {
        let mut raw = valid();
        for (width, height) in [(640, 480), (641, 479)] {
            raw.width = width;
            raw.height = height;
            for scale in [1.0, 1.25, 1.5, 1.75, 2.0] {
                raw.canvas_scale = scale;
                let canvas = unsafe { config(&raw) }.unwrap().canvas;
                let frame = unsafe {
                    snow_recording_effects_playback_preview(
                        width,
                        height,
                        scale,
                        1,
                        0x1677ffff,
                        std::ptr::null(),
                        std::ptr::null(),
                        0,
                    )
                };
                assert!(!frame.is_null());
                let preview = unsafe { &*frame };
                assert_eq!(preview.frame.output, canvas);
                let mut visible = 0;
                for tile in &preview.frame.tiles {
                    for (index, pixel) in tile.pixels.chunks_exact(4).enumerate() {
                        if pixel[3] != 0 {
                            let y = tile.y + index as u32 / TILE_SIZE;
                            assert!((canvas.1 - 12..canvas.1).contains(&y));
                            assert_eq!(pixel, [0, 0, 0, 64]);
                            visible += 1;
                        }
                    }
                }
                assert_eq!(visible, canvas.0 * 12);
                unsafe { snow_recording_effects_release_frame(frame) };
            }
        }
        for scale in [0.0, -1.0, f64::NAN, f64::INFINITY, 100.0, 0.00001] {
            assert!(
                unsafe {
                    snow_recording_effects_playback_preview(
                        640,
                        480,
                        scale,
                        1,
                        0x1677ffff,
                        std::ptr::null(),
                        std::ptr::null(),
                        0,
                    )
                }
                .is_null()
            );
        }
    }

    #[test]
    fn preview_canvas_scale_preserves_desktop_input_units_and_legacy_configs() {
        let mut raw = valid();
        raw.width = 641;
        raw.height = 479;
        for scale in [1.0, 1.25, 1.5, 1.75, 2.0] {
            raw.canvas_scale = scale;
            let parsed = unsafe { config(&raw) }.unwrap();
            assert_eq!(parsed.region, (-1920, -100, 641, 479));
            assert_eq!(parsed.output, (1280, 720));
            assert_eq!(
                parsed.canvas,
                (
                    (641.0 * scale).round() as u32,
                    (479.0 * scale).round() as u32
                )
            );
        }
        for scale in [0.0, -1.0, f64::NAN, f64::INFINITY, 100.0, 0.00001] {
            raw.canvas_scale = scale;
            assert!(unsafe { config(&raw) }.is_err());
        }
        for (version, size) in [
            (
                1,
                std::mem::offset_of!(SnowRecordingEffectsConfig, keyboard_size),
            ),
            (
                2,
                std::mem::offset_of!(SnowRecordingEffectsConfig, keyboard_size),
            ),
            (
                3,
                std::mem::offset_of!(SnowRecordingEffectsConfig, highlight_rgba),
            ),
            (
                4,
                std::mem::offset_of!(SnowRecordingEffectsConfig, keyboard_font_family_utf8),
            ),
            (
                5,
                std::mem::offset_of!(SnowRecordingEffectsConfig, canvas_scale),
            ),
        ] {
            raw.version = version;
            raw.struct_size = size as u32;
            raw.trail_duration_ms = if version == 1 { 0 } else { 500 };
            raw.canvas_scale = f64::NAN;
            let parsed = unsafe { config(&raw) }.unwrap();
            assert_eq!(parsed.canvas, (641, 479));
        }
    }

    #[test]
    fn keyboard_font_is_owned_validated_and_legacy_compatible() {
        let mut raw = valid();
        raw.show_keyboard = 1;
        let family = CString::new("Courier New").unwrap();
        let cjk = CString::new("Microsoft JhengHei UI").unwrap();
        raw.keyboard_font_family_utf8 = family.as_ptr();
        raw.keyboard_cjk_font_family_utf8 = cjk.as_ptr();
        raw.keyboard_font_weight = 700;
        let parsed = unsafe { config(&raw) }.unwrap().keyboard.unwrap();
        drop(family);
        drop(cjk);
        let font = parsed.font.unwrap();
        assert_eq!(font.family, "Courier New");
        assert_eq!(font.cjk_family, "Microsoft JhengHei UI");
        assert_eq!(font.weight, 700);
        raw.keyboard_font_family_utf8 = c"Segoe UI".as_ptr();
        raw.keyboard_cjk_font_family_utf8 = c"Microsoft YaHei UI".as_ptr();
        raw.keyboard_font_weight = 1000;
        assert!(unsafe { config(&raw) }.is_err());
        raw.keyboard_font_weight = 400;
        raw.keyboard_cjk_font_family_utf8 = std::ptr::null();
        assert!(unsafe { config(&raw) }.is_err());
        raw.version = 4;
        raw.struct_size =
            std::mem::offset_of!(SnowRecordingEffectsConfig, keyboard_font_family_utf8) as u32;
        assert!(
            (unsafe { config(&raw) }.unwrap().keyboard.unwrap())
                .font
                .is_none()
        );
    }

    #[test]
    fn mouse_options_are_independent_and_v3_padding_is_ignored() {
        let mut raw = valid();
        raw.highlight_rgba = 0xffff0080;
        raw.record_mouse_clicks = 1;
        let parsed = unsafe { config(&raw) }.unwrap();
        assert!(parsed.record_mouse_clicks && !parsed.show_keyboard && parsed.keyboard.is_some());
        assert_eq!(parsed.highlight, [255, 255, 0, 128]);
        raw.version = 3;
        raw.struct_size = std::mem::offset_of!(SnowRecordingEffectsConfig, highlight_rgba) as u32;
        raw.reserved_v3 = u32::MAX;
        let parsed = unsafe { config(&raw) }.unwrap();
        assert!(!parsed.record_mouse_clicks && parsed.keyboard.is_none());
        assert_eq!(parsed.highlight, [0; 4]);
    }
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
            webcam_source: std::ptr::null(),
            webcam_size: 150,
            webcam_shape: 0,
            webcam_offset_left: 48,
            webcam_offset_bottom: 48,
            webcam_orientation: 0,
            record_number_keys: 1,
            record_letters_and_symbols: 1,
            record_control_keys: 1,
            preserve_keyboard_shortcuts: 1,
            version: 12,
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
            reserved_v3: 0,
            highlight_rgba: 0,
            record_mouse_clicks: 0,
            record_mouse_scrolling: 0,
            keyboard_font_family_utf8: std::ptr::null(),
            keyboard_cjk_font_family_utf8: std::ptr::null(),
            keyboard_font_weight: 0,
            canvas_scale: 1.0,
            highlight_radius: 41,
        }
    }
    #[test]
    fn webcam_preview_retains_source_and_preserves_v7_layout() {
        let source = snow_webcam::Source::with_backend(
            snow_webcam::CaptureOptions { size: 150, fps: 30 },
            |_| Err("unused".into()),
        )
        .unwrap();
        let mut raw = valid();
        raw.webcam_source = &source;
        raw.webcam_shape = 1;
        raw.webcam_offset_left = 300;
        raw.webcam_offset_bottom = 220;
        raw.webcam_orientation = 5;
        let parsed = unsafe { config(&raw) }.unwrap();
        assert_eq!(
            parsed.webcam.as_ref().unwrap().style.shape,
            snow_webcam::WebcamShape::Circle
        );
        assert_eq!(parsed.webcam.as_ref().unwrap().source, source);
        assert_eq!(parsed.webcam.as_ref().unwrap().style.offset_left, 300);
        assert_eq!(parsed.webcam.as_ref().unwrap().style.offset_bottom, 220);
        assert_eq!(parsed.webcam.as_ref().unwrap().style.orientation as u32, 5);
        raw.webcam_orientation = 8;
        assert!(unsafe { config(&raw) }.is_err());
        raw.version = 10;
        raw.struct_size = EFFECTS_CONFIG_V10_SIZE as u32;
        assert_eq!(raw.struct_size as usize, EFFECTS_CONFIG_V10_SIZE);
        assert_eq!(
            unsafe { config(&raw) }
                .unwrap()
                .webcam
                .unwrap()
                .style
                .orientation as u32,
            0
        );
        raw.version = 11;
        raw.webcam_orientation = 5;
        raw.struct_size = EFFECTS_CONFIG_V11_SIZE as u32;
        raw.struct_size -= 1;
        assert!(unsafe { config(&raw) }.is_err());
        raw.version = 8;
        raw.struct_size =
            std::mem::offset_of!(SnowRecordingEffectsConfig, webcam_offset_left) as u32;
        let legacy = unsafe { config(&raw) }.unwrap().webcam.unwrap();
        assert_eq!(
            (legacy.style.offset_left, legacy.style.offset_bottom),
            (48, 48)
        );
        for size in [49, 501] {
            raw.webcam_size = size;
            assert!(unsafe { config(&raw) }.is_err());
        }
        raw.webcam_size = 150;
        raw.webcam_shape = 2;
        assert!(unsafe { config(&raw) }.is_err());
        raw.version = 7;
        raw.struct_size = std::mem::offset_of!(SnowRecordingEffectsConfig, webcam_source) as u32;
        assert!(unsafe { config(&raw) }.unwrap().webcam.is_none());
    }
    #[test]
    fn highlight_radius_is_validated_and_legacy_configs_keep_default() {
        let mut raw = valid();
        for radius in [8, 41, 96, 128] {
            raw.highlight_radius = radius;
            assert_eq!(unsafe { config(&raw) }.unwrap().highlight_radius, radius);
        }
        for radius in [0, 7, 129, u32::MAX] {
            raw.highlight_radius = radius;
            assert!(unsafe { config(&raw) }.is_err());
        }
        raw.version = 6;
        raw.struct_size = std::mem::offset_of!(SnowRecordingEffectsConfig, highlight_radius) as u32;
        assert_eq!(unsafe { config(&raw) }.unwrap().highlight_radius, 41);
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

    // Both tests below hold native handles concurrently; the count assertions are
    // exact, so the handle-holding tests must not overlap.
    static HANDLE_TESTS: std::sync::Mutex<()> = const { std::sync::Mutex::new(()) };

    #[test]
    fn live_handle_count_tracks_create_and_destroy_exactly_once() {
        let _serial = HANDLE_TESTS
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let baseline = snow_recording_effects_live_handle_count();
        let raw = valid();
        let first = unsafe { snow_recording_effects_create(&raw, None, std::ptr::null_mut()) };
        let second = unsafe { snow_recording_effects_create(&raw, None, std::ptr::null_mut()) };
        assert!(!first.is_null());
        assert!(!second.is_null());
        assert_eq!(snow_recording_effects_live_handle_count(), baseline + 2);
        // A null destroy must not disturb the count; destroy pairs with create once.
        unsafe { snow_recording_effects_destroy(std::ptr::null_mut()) };
        assert_eq!(snow_recording_effects_live_handle_count(), baseline + 2);
        unsafe { snow_recording_effects_destroy(first) };
        assert_eq!(snow_recording_effects_live_handle_count(), baseline + 1);
        unsafe { snow_recording_effects_destroy(second) };
        assert_eq!(snow_recording_effects_live_handle_count(), baseline);
    }
    #[test]
    fn idle_session_is_quiet_and_frame_lease_survives_join_and_destroy() {
        let _serial = HANDLE_TESTS
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let (sender, receiver) = std::sync::mpsc::channel::<()>();
        let mut raw = valid();
        raw.canvas_scale = 1.5;
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
        assert_eq!((info.width, info.height), (2880, 1620));
        assert_eq!(info.tile_count, 0);
        assert!(info.error_utf8.is_null());
        let mut keyboard = std::mem::MaybeUninit::uninit();
        assert_eq!(
            unsafe { snow_recording_effects_frame_keyboard_info(frame, keyboard.as_mut_ptr()) },
            1
        );
        let keyboard = unsafe { keyboard.assume_init() };
        assert_eq!(keyboard.generation, 12);
        assert_eq!((keyboard.width, keyboard.height), (2880, 1620));
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

use super::*;
use snow_ui_selector::DisplayGeometry;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowUiSelectorDisplay {
    pub version: u32,
    pub struct_size: u32,
    pub display_id: u32,
    pub coordinate_space: u32,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
    pub pixel_width: u32,
    pub pixel_height: u32,
}

unsafe fn copy_displays(
    displays: *const SnowUiSelectorDisplay,
    count: usize,
) -> Option<Vec<DisplayGeometry>> {
    if displays.is_null() || count == 0 || count > 128 {
        return None;
    }
    unsafe { std::slice::from_raw_parts(displays, count) }
        .iter()
        .map(|d| {
            let geometry = DisplayGeometry {
                display_id: d.display_id,
                x: d.x,
                y: d.y,
                width: d.width,
                height: d.height,
                pixel_width: d.pixel_width,
                pixel_height: d.pixel_height,
            };
            (d.version == 1
                && d.struct_size == std::mem::size_of::<SnowUiSelectorDisplay>() as u32
                && d.coordinate_space == u32::from(cfg!(target_os = "macos"))
                && geometry.valid())
            .then_some(geometry)
        })
        .collect()
}

#[unsafe(no_mangle)]
/// All arrays are copied before returning. Invalid geometry never falls back to enumeration.
///
/// # Safety
/// The service must be live. Non-null arrays must reference their declared number of
/// initialized entries and remain readable for this call.
pub unsafe extern "C" fn snow_ui_selector_service_refresh_with_displays(
    service: *mut SnowUiSelectorServiceImpl,
    epoch: u64,
    backend: SnowUiSelectorBackend,
    excluded: *const usize,
    count: usize,
    displays: *const SnowUiSelectorDisplay,
    display_count: usize,
) -> u8 {
    let Some(displays) = (unsafe { copy_displays(displays, display_count) }) else {
        return 0;
    };
    unsafe { refresh_with_layout(service, epoch, backend, excluded, count, Some(displays)) }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn descriptors_are_validated_and_owned() {
        let mut d = SnowUiSelectorDisplay {
            version: 1,
            struct_size: std::mem::size_of::<SnowUiSelectorDisplay>() as u32,
            display_id: 7,
            coordinate_space: u32::from(cfg!(target_os = "macos")),
            x: -200.0,
            y: 0.0,
            width: 200.0,
            height: 100.0,
            pixel_width: 400,
            pixel_height: 200,
        };
        let copied = unsafe { copy_displays(&d, 1) }.unwrap();
        d.width = f64::NAN;
        assert_eq!(copied[0].width, 200.0);
        assert!(unsafe { copy_displays(&d, 1) }.is_none());
        assert!(unsafe { copy_displays(std::ptr::null(), 1) }.is_none());
        assert!(unsafe { copy_displays(&d, 129) }.is_none());
        d.width = 200.0;
        d.version = 2;
        assert!(unsafe { copy_displays(&d, 1) }.is_none());
    }
}

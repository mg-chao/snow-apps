use std::ffi::c_void;
use std::ptr;

use windows::Graphics::Capture::GraphicsCaptureItem;
use windows::Win32::Foundation::HWND;
use windows::Win32::Graphics::Gdi::HMONITOR;
use windows::Win32::System::WinRT::Graphics::Capture::IGraphicsCaptureItemInterop;
use windows::core::{GUID, HRESULT, Interface, Type};

unsafe extern "C" {
    fn snow_capture_wgc_create_for_monitor(
        factory: *mut c_void,
        monitor: *mut c_void,
        iid: *const GUID,
        result: *mut *mut c_void,
    ) -> HRESULT;
    fn snow_capture_wgc_create_for_window(
        factory: *mut c_void,
        window: *mut c_void,
        iid: *const GUID,
        result: *mut *mut c_void,
    ) -> HRESULT;
}

pub(in crate::platform::windows) fn create_for_monitor(
    factory: &IGraphicsCaptureItemInterop,
    monitor: HMONITOR,
) -> windows::core::Result<GraphicsCaptureItem> {
    let mut item = ptr::null_mut();
    unsafe {
        snow_capture_wgc_create_for_monitor(
            factory.as_raw(),
            monitor.0,
            &GraphicsCaptureItem::IID,
            &mut item,
        )
        .ok()?;
        GraphicsCaptureItem::from_abi(item)
    }
}

pub(in crate::platform::windows) fn create_for_window(
    factory: &IGraphicsCaptureItemInterop,
    window: HWND,
) -> windows::core::Result<GraphicsCaptureItem> {
    let mut item = ptr::null_mut();
    unsafe {
        snow_capture_wgc_create_for_window(
            factory.as_raw(),
            window.0,
            &GraphicsCaptureItem::IID,
            &mut item,
        )
        .ok()?;
        GraphicsCaptureItem::from_abi(item)
    }
}

#[cfg(test)]
pub(in crate::platform::windows) mod tests {
    use super::*;
    use windows::Win32::Foundation::{E_ACCESSDENIED, E_FAIL, E_OUTOFMEMORY, E_POINTER};
    use windows::Win32::Graphics::Dxgi::DXGI_ERROR_DEVICE_REMOVED;

    unsafe extern "C" {
        fn snow_capture_wgc_test_factory(behavior: i32) -> *mut c_void;
        fn snow_capture_wgc_test_live_items(factory: *mut c_void) -> u32;
    }

    pub(in crate::platform::windows) fn factory(behavior: i32) -> IGraphicsCaptureItemInterop {
        unsafe { IGraphicsCaptureItemInterop::from_abi(snow_capture_wgc_test_factory(behavior)) }
            .expect("native test factory")
    }

    fn create(
        factory: &IGraphicsCaptureItemInterop,
        window: bool,
    ) -> windows::core::Result<GraphicsCaptureItem> {
        let handle = 0x10003_usize as *mut c_void;
        if window {
            create_for_window(factory, HWND(handle))
        } else {
            create_for_monitor(factory, HMONITOR(handle))
        }
    }

    #[test]
    fn native_item_creation_preserves_success_and_releases_partial_results() {
        for window in [false, true] {
            let factory = factory(0);
            let item = create(&factory, window).unwrap();
            assert_eq!(
                unsafe { snow_capture_wgc_test_live_items(factory.as_raw()) },
                1
            );
            drop(item);
            assert_eq!(
                unsafe { snow_capture_wgc_test_live_items(factory.as_raw()) },
                0
            );
            for behavior in [8, 9] {
                let factory = self::factory(behavior);
                assert_eq!(create(&factory, window).unwrap_err().code(), E_FAIL);
                assert_eq!(
                    unsafe { snow_capture_wgc_test_live_items(factory.as_raw()) },
                    0
                );
            }
        }
    }

    #[test]
    fn native_item_creation_contains_cpp_exceptions_and_preserves_hresult() {
        for window in [false, true] {
            for (behavior, expected) in [
                (1, E_ACCESSDENIED),
                (2, E_FAIL),
                (3, DXGI_ERROR_DEVICE_REMOVED),
                (4, E_FAIL),
                (5, E_FAIL),
                (6, E_OUTOFMEMORY),
                (7, E_POINTER),
            ] {
                let factory = factory(behavior);
                let error = create(&factory, window).unwrap_err();
                assert_eq!(
                    error.code(),
                    expected,
                    "behavior={behavior} window={window}"
                );
                if behavior == 2 {
                    assert!(error.message().contains("synthetic WGC server failure"));
                }
            }
        }
    }
}

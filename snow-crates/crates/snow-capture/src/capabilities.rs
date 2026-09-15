//! Feature discovery is separate from permission requests and target acquisition.
use crate::backend::CaptureBackendKind;
use snow_media::{PixelFormat, geometry::DesktopSpace};
#[derive(Clone, Debug)]
pub struct CaptureCapabilities {
    pub backends: Vec<CaptureBackendKind>,
    pub desktop_space: DesktopSpace,
    pub cpu_formats: Vec<PixelFormat>,
    pub native_frames: bool,
    pub hdr_capture: bool,
    pub window_enumeration: bool,
}
impl CaptureCapabilities {
    pub fn current() -> Self {
        #[cfg(target_os = "macos")]
        {
            let native = snow_macos::capabilities::CaptureSupport::current();
            let supported = native.screen_capture_kit && native.metal_composition;
            let hdr = supported && native.hdr_capture;
            let mut formats = vec![PixelFormat::Rgba8, PixelFormat::Bgra8];
            if hdr {
                formats.push(PixelFormat::Rgba16Float);
            }
            Self {
                backends: if supported {
                    vec![CaptureBackendKind::ScreenCaptureKit]
                } else {
                    vec![]
                },
                desktop_space: DesktopSpace::Points,
                cpu_formats: if supported { formats } else { vec![] },
                native_frames: supported,
                hdr_capture: hdr,
                window_enumeration: supported,
            }
        }
        #[cfg(not(target_os = "macos"))]
        {
            let supported = cfg!(windows);
            Self {
                backends: if supported {
                    vec![
                        CaptureBackendKind::DxgiDuplication,
                        CaptureBackendKind::WindowsGraphicsCapture,
                        CaptureBackendKind::Gdi,
                    ]
                } else {
                    vec![]
                },
                desktop_space: DesktopSpace::PhysicalPixels,
                cpu_formats: if supported {
                    vec![PixelFormat::Rgba8, PixelFormat::Bgra8]
                } else {
                    vec![]
                },
                native_frames: supported,
                hdr_capture: false,
                window_enumeration: false,
            }
        }
    }
}

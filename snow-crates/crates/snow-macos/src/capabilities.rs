//! Capability discovery never requests permission or starts a stream. Target
//! queries enumerate current geometry and therefore require screen permission.
use crate::{MacError, MacResult};
use snow_media::{DynamicRange, PixelFormat};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CaptureSupport {
    pub screen_capture_kit: bool,
    pub metal_composition: bool,
    pub hdr_capture: bool,
}
impl CaptureSupport {
    pub fn current() -> Self {
        Self::from_host(
            crate::permission::supported_os(),
            cfg!(target_arch = "aarch64"),
            objc2_metal::MTLCreateSystemDefaultDevice().is_some(),
        )
    }
    fn from_host(supported_os: bool, apple_silicon: bool, metal: bool) -> Self {
        Self {
            screen_capture_kit: supported_os,
            metal_composition: supported_os && metal,
            hdr_capture: supported_os && apple_silicon,
        }
    }
    pub fn native_format(self, range: DynamicRange) -> MacResult<PixelFormat> {
        if !self.screen_capture_kit {
            return Err(MacError::UnsupportedOs);
        }
        match range {
            DynamicRange::Sdr => Ok(PixelFormat::Bgra8),
            DynamicRange::Hdr if self.hdr_capture => Ok(PixelFormat::Rgba16Float),
            DynamicRange::Hdr => Err(MacError::Unsupported(
                "ScreenCaptureKit canonical HDR requires Apple Silicon".into(),
            )),
        }
    }
    /// CPU capture conversion is intentionally separate from encoder YUV formats.
    pub fn cpu_formats(self, range: DynamicRange) -> MacResult<&'static [PixelFormat]> {
        self.native_format(range)?;
        Ok(match range {
            DynamicRange::Sdr => &[PixelFormat::Bgra8, PixelFormat::Rgba8],
            DynamicRange::Hdr => &[PixelFormat::Rgba16Float],
        })
    }
    pub fn validate_cpu_format(self, range: DynamicRange, format: PixelFormat) -> MacResult<()> {
        if !self.cpu_formats(range)?.contains(&format) {
            return Err(MacError::Unsupported(format!(
                "CPU capture format {format:?} is unavailable for {range:?}"
            )));
        }
        Ok(())
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn architecture_os_and_pixel_depth_are_independent_capabilities() {
        let intel = CaptureSupport::from_host(true, false, true);
        assert_eq!(
            intel.native_format(DynamicRange::Sdr).unwrap(),
            PixelFormat::Bgra8
        );
        assert!(matches!(
            intel.native_format(DynamicRange::Hdr),
            Err(MacError::Unsupported(_))
        ));
        let apple = CaptureSupport::from_host(true, true, true);
        assert!(
            apple
                .validate_cpu_format(DynamicRange::Hdr, PixelFormat::Rgba16Float)
                .is_ok()
        );
        assert!(
            apple
                .validate_cpu_format(DynamicRange::Hdr, PixelFormat::Bgra8)
                .is_err()
        );
        assert!(
            apple
                .validate_cpu_format(DynamicRange::Sdr, PixelFormat::P010)
                .is_err()
        );
        let old = CaptureSupport::from_host(false, true, true);
        assert!(matches!(
            old.native_format(DynamicRange::Sdr),
            Err(MacError::UnsupportedOs)
        ));
        assert!(!old.metal_composition);
        let headless = CaptureSupport::from_host(true, true, false);
        assert!(headless.hdr_capture);
        assert!(!headless.metal_composition);
    }
}

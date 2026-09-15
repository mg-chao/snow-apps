//! Platform tuning is opt-in and separate from common acquisition options.
#[derive(Clone, Copy, Debug, Default)]
pub enum BackendTuning {
    #[default]
    Default,
    Windows(windows::WindowsCaptureOptions),
}
impl BackendTuning {
    pub fn windows_or_default(self) -> windows::WindowsCaptureOptions {
        match self {
            Self::Default => Default::default(),
            Self::Windows(options) => options,
        }
    }
}
pub mod windows {
    /// Windows driver/color tuning. Selecting this explicitly on another
    /// platform is an unsupported-capability error, even for default values.
    #[derive(Clone, Copy, Debug)]
    pub struct WindowsCaptureOptions {
        /// Reverse a supported full-screen Magnifier transform.
        pub color_correction: crate::color_effect::ColorCorrection,
        pub gpu_hdr_conversion: bool,
        pub hdr_tonemap_lut: bool,
        /// Complete-surface versus ordered-delta WGC acquisition.
        pub wgc_update_mode: crate::backend::WgcUpdateMode,
    }
    impl Default for WindowsCaptureOptions {
        fn default() -> Self {
            Self {
                color_correction: crate::color_effect::ColorCorrection::Disabled,
                gpu_hdr_conversion: true,
                hdr_tonemap_lut: true,
                wgc_update_mode: crate::backend::WgcUpdateMode::Auto,
            }
        }
    }
}

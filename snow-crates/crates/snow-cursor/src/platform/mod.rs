#[cfg(target_os = "windows")]
mod windows;

#[cfg(target_os = "windows")]
pub(crate) use windows::WindowsCursorSampler as CursorSamplerImpl;

#[cfg(not(any(target_os = "windows", target_os = "macos")))]
pub(crate) struct CursorSamplerImpl;

#[cfg(not(any(target_os = "windows", target_os = "macos")))]
impl CursorSamplerImpl {
    pub(crate) fn new() -> Result<Self, crate::CursorCaptureError> {
        Err(crate::CursorCaptureError::UnsupportedPlatform)
    }

    pub(crate) fn sample_cursor(
        &mut self,
    ) -> Result<crate::sampler::CursorProbe, crate::CursorCaptureError> {
        Err(crate::CursorCaptureError::UnsupportedPlatform)
    }
}

#[cfg(target_os = "macos")]
mod macos;
#[cfg(target_os = "macos")]
pub(crate) use macos::MacOsCursorSampler as CursorSamplerImpl;

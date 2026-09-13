use std::sync::Arc;

use crate::backend::{AudioBackend, AudioBackendKind};
#[cfg(not(any(target_os = "windows", target_os = "macos")))]
use crate::device::AudioDeviceInfo;
#[cfg(not(any(target_os = "windows", target_os = "macos")))]
use crate::error::AudioError;
use crate::error::AudioResult;
#[cfg(not(any(target_os = "windows", target_os = "macos")))]
use crate::session::AudioStreamConfig;

#[cfg(target_os = "windows")]
pub(crate) mod windows;

#[cfg(not(any(target_os = "windows", target_os = "macos")))]
struct UnsupportedBackend;

#[cfg(not(any(target_os = "windows", target_os = "macos")))]
impl AudioBackend for UnsupportedBackend {
    fn enumerate_devices(
        &self,
        _flow: crate::device::DeviceFlow,
    ) -> AudioResult<Vec<AudioDeviceInfo>> {
        Err(AudioError::platform(anyhow::anyhow!(
            "audio capture is only supported on Windows and macOS"
        )))
    }

    fn create_engine(
        &self,
        _config: AudioStreamConfig,
    ) -> AudioResult<Box<dyn crate::backend::AudioRecorderEngine>> {
        Err(AudioError::platform(anyhow::anyhow!(
            "audio capture is only supported on Windows and macOS"
        )))
    }
}

#[cfg(target_os = "windows")]
pub(crate) fn build_backend(kind: AudioBackendKind) -> AudioResult<Arc<dyn AudioBackend>> {
    Ok(Arc::new(windows::WasapiBackend::new(kind)?))
}

#[cfg(not(any(target_os = "windows", target_os = "macos")))]
pub(crate) fn build_backend(_kind: AudioBackendKind) -> AudioResult<Arc<dyn AudioBackend>> {
    Ok(Arc::new(UnsupportedBackend))
}

#[cfg(target_os = "macos")]
mod macos;

#[cfg(target_os = "macos")]
pub(crate) fn build_backend(kind: AudioBackendKind) -> AudioResult<Arc<dyn AudioBackend>> {
    match kind {
        AudioBackendKind::Auto => Ok(Arc::new(macos::MacAudioBackend)),
        AudioBackendKind::Wasapi => Err(crate::error::AudioError::BackendUnavailable(
            "WASAPI is only available on Windows".into(),
        )),
    }
}

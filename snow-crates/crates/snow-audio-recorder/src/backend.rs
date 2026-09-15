use std::sync::Arc;
use std::time::Duration;

use crate::device::{AudioDeviceInfo, DeviceFlow};
use crate::error::AudioResult;
use crate::packet::AudioEvent;
use crate::session::AudioStreamConfig;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AudioBackendKind {
    Auto,
    Wasapi,
    CoreAudio,
}

impl AudioBackendKind {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Auto => "auto",
            Self::Wasapi => "wasapi",
            Self::CoreAudio => "coreaudio-sck",
        }
    }
}

pub enum EngineEvent {
    Idle,
    Events(Vec<AudioEvent>),
}

pub trait AudioRecorderEngine: Send {
    fn poll(&mut self, timeout: Duration) -> AudioResult<EngineEvent>;
}

pub trait AudioBackend: Send + Sync {
    fn enumerate_devices(&self, flow: DeviceFlow) -> AudioResult<Vec<AudioDeviceInfo>>;
    fn create_engine(&self, config: AudioStreamConfig)
    -> AudioResult<Box<dyn AudioRecorderEngine>>;
}

pub fn backend_for_kind(kind: AudioBackendKind) -> AudioResult<Arc<dyn AudioBackend>> {
    crate::platform::build_backend(kind)
}

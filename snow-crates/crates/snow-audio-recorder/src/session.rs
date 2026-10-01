use std::sync::Arc;
use std::time::Duration;

use crate::backend::{self, AudioBackend, AudioBackendKind};
use crate::device::{AudioDeviceInfo, DeviceFlow, DeviceSelector};
use crate::error::AudioResult;
use crate::format::AudioFormat;
use crate::streaming::AudioStreamHandle;

#[derive(Clone, Debug)]
pub struct SourceConfig {
    pub enabled: bool,
    pub required: bool,
    pub device: DeviceSelector,
    pub output_format: AudioFormat,
    pub packet_duration: Duration,
}

impl SourceConfig {
    fn with_defaults(device: DeviceSelector, channels: u16) -> Self {
        Self {
            enabled: true,
            required: true,
            device,
            output_format: AudioFormat::new(48_000, channels),
            packet_duration: Duration::from_millis(10),
        }
    }

    pub fn default_system() -> Self {
        Self::with_defaults(DeviceSelector::DefaultRender, 2)
    }

    pub fn default_microphone() -> Self {
        Self::with_defaults(DeviceSelector::DefaultCapture, 1)
    }

    pub fn validate(&self) -> AudioResult<()> {
        if !self.enabled {
            return Ok(());
        }

        self.output_format.validate()?;
        if self.packet_duration.is_zero() {
            return Err(crate::error::AudioError::InvalidConfig(
                "packet duration must be greater than zero".into(),
            ));
        }
        Ok(())
    }
}

#[derive(Clone, Debug)]
pub struct RestartPolicy {
    pub auto_rebind_on_default_change: bool,
    pub max_attempts: u32,
    pub initial_backoff: Duration,
    pub max_backoff: Duration,
}

impl Default for RestartPolicy {
    fn default() -> Self {
        Self {
            auto_rebind_on_default_change: true,
            max_attempts: 8,
            initial_backoff: Duration::from_millis(100),
            max_backoff: Duration::from_secs(2),
        }
    }
}

#[derive(Clone, Debug)]
pub struct AudioStreamConfig {
    pub cancellation: snow_core::cancellation::CancellationToken,
    pub system: SourceConfig,
    pub microphone: SourceConfig,
    pub event_buffer_depth: usize,
    pub max_consecutive_errors: usize,
    pub restart_policy: RestartPolicy,
}

impl Default for AudioStreamConfig {
    fn default() -> Self {
        Self {
            cancellation: Default::default(),
            system: SourceConfig::default_system(),
            microphone: SourceConfig::default_microphone(),
            event_buffer_depth: 128,
            max_consecutive_errors: 30,
            restart_policy: RestartPolicy::default(),
        }
    }
}

impl AudioStreamConfig {
    pub fn validate(&self) -> AudioResult<()> {
        if self.cancellation.is_canceled() {
            return Err(crate::error::AudioError::Canceled);
        }
        self.system.validate()?;
        self.microphone.validate()?;

        if !self.system.enabled && !self.microphone.enabled {
            return Err(crate::error::AudioError::InvalidConfig(
                "at least one audio source must be enabled".into(),
            ));
        }

        if self.event_buffer_depth == 0 {
            return Err(crate::error::AudioError::InvalidConfig(
                "event buffer depth must be greater than zero".into(),
            ));
        }

        if self.max_consecutive_errors == 0 {
            return Err(crate::error::AudioError::InvalidConfig(
                "max_consecutive_errors must be greater than zero".into(),
            ));
        }

        if self.restart_policy.max_attempts == 0 {
            return Err(crate::error::AudioError::InvalidConfig(
                "restart policy max_attempts must be greater than zero".into(),
            ));
        }

        if self.restart_policy.initial_backoff.is_zero()
            || self.restart_policy.max_backoff.is_zero()
        {
            return Err(crate::error::AudioError::InvalidConfig(
                "restart backoff durations must be greater than zero".into(),
            ));
        }

        if self.restart_policy.initial_backoff > self.restart_policy.max_backoff {
            return Err(crate::error::AudioError::InvalidConfig(
                "restart initial_backoff must be <= max_backoff".into(),
            ));
        }

        Ok(())
    }
}

pub struct AudioSessionBuilder {
    backend_override: Option<Arc<dyn AudioBackend>>,
    backend_kind: AudioBackendKind,
}

impl AudioSessionBuilder {
    pub fn new() -> Self {
        Self {
            backend_override: None,
            backend_kind: AudioBackendKind::Auto,
        }
    }

    pub fn with_backend(mut self, backend: Arc<dyn AudioBackend>) -> Self {
        self.backend_override = Some(backend);
        self
    }

    pub fn with_backend_kind(mut self, kind: AudioBackendKind) -> Self {
        self.backend_kind = kind;
        self.backend_override = None;
        self
    }

    pub fn build(self) -> AudioResult<AudioSession> {
        let backend = if let Some(backend) = self.backend_override {
            backend
        } else {
            backend::backend_for_kind(self.backend_kind)?
        };

        Ok(AudioSession { backend })
    }
}

impl Default for AudioSessionBuilder {
    fn default() -> Self {
        Self::new()
    }
}

pub struct AudioSession {
    backend: Arc<dyn AudioBackend>,
}

impl AudioSession {
    pub fn builder() -> AudioSessionBuilder {
        AudioSessionBuilder::new()
    }

    pub fn new() -> AudioResult<Self> {
        Self::builder().build()
    }

    pub fn enumerate_render_devices(&self) -> AudioResult<Vec<AudioDeviceInfo>> {
        self.backend.enumerate_devices(DeviceFlow::Render)
    }

    pub fn enumerate_capture_devices(&self) -> AudioResult<Vec<AudioDeviceInfo>> {
        self.backend.enumerate_devices(DeviceFlow::Capture)
    }

    pub fn start_streaming(&self, config: AudioStreamConfig) -> AudioResult<AudioStreamHandle> {
        config.validate()?;
        let engine = self.backend.create_engine(config.clone())?;
        AudioStreamHandle::start(engine, config)
    }

    pub fn start_streaming_with_controls(
        &self,
        config: AudioStreamConfig,
        control: crate::AudioControlHandle,
    ) -> AudioResult<AudioStreamHandle> {
        config.validate()?;
        if config
            .cancellation
            .commit(|| control.initialize_sources(config.system.enabled, config.microphone.enabled))
            .is_err()
        {
            control.mark_stopped();
            return Err(crate::AudioError::Canceled);
        }
        let engine = self
            .backend
            .create_engine_with_controls(config.clone(), &control)
            .inspect_err(|error| {
                let _ = config
                    .cancellation
                    .commit(|| control.mark_initialization_error(error));
                if config.cancellation.is_canceled() {
                    control.mark_stopped();
                }
            })?;
        AudioStreamHandle::start_with_controls(engine, config, control)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{AudioControlHandle, AudioError, AudioSourceKind, AudioSourceStatus};

    #[test]
    fn all_optional_startup_failures_keep_individual_source_statuses() {
        struct FailedBackend;
        impl AudioBackend for FailedBackend {
            fn enumerate_devices(&self, _: DeviceFlow) -> AudioResult<Vec<AudioDeviceInfo>> {
                Ok(Vec::new())
            }
            fn create_engine(
                &self,
                _: AudioStreamConfig,
            ) -> AudioResult<Box<dyn crate::backend::AudioRecorderEngine>> {
                Err(AudioError::DeviceUnavailable("no sources".into()))
            }
            fn create_engine_with_controls(
                &self,
                config: AudioStreamConfig,
                control: &AudioControlHandle,
            ) -> AudioResult<Box<dyn crate::backend::AudioRecorderEngine>> {
                assert!(!config.system.required && !config.microphone.required);
                config
                    .cancellation
                    .commit(|| {
                        control.set_source_status(
                            AudioSourceKind::System,
                            AudioSourceStatus::Unavailable,
                        );
                        control.set_source_status(
                            AudioSourceKind::Microphone,
                            AudioSourceStatus::PermissionDenied,
                        );
                    })
                    .unwrap();
                self.create_engine(config)
            }
        }
        let session = AudioSession::builder()
            .with_backend(Arc::new(FailedBackend))
            .build()
            .unwrap();
        let control = AudioControlHandle::new();
        let mut config = AudioStreamConfig::default();
        config.system.required = false;
        config.microphone.required = false;
        assert!(matches!(
            session.start_streaming_with_controls(config, control.clone()),
            Err(AudioError::DeviceUnavailable(_))
        ));
        let levels = control.take_levels();
        assert_eq!(levels.system.status, AudioSourceStatus::Unavailable);
        assert_eq!(
            levels.microphone.status,
            AudioSourceStatus::PermissionDenied
        );
    }

    #[test]
    fn successful_stream_keeps_classified_optional_source_failure() {
        struct SilentEngine;
        impl crate::backend::AudioRecorderEngine for SilentEngine {
            fn poll(&mut self, _: Duration) -> AudioResult<crate::backend::EngineEvent> {
                std::thread::sleep(Duration::from_millis(1));
                Ok(crate::backend::EngineEvent::Idle)
            }
        }
        struct PartialBackend;
        impl AudioBackend for PartialBackend {
            fn enumerate_devices(&self, _: DeviceFlow) -> AudioResult<Vec<AudioDeviceInfo>> {
                Ok(Vec::new())
            }
            fn create_engine(
                &self,
                _: AudioStreamConfig,
            ) -> AudioResult<Box<dyn crate::backend::AudioRecorderEngine>> {
                Ok(Box::new(SilentEngine))
            }
            fn create_engine_with_controls(
                &self,
                config: AudioStreamConfig,
                control: &AudioControlHandle,
            ) -> AudioResult<Box<dyn crate::backend::AudioRecorderEngine>> {
                config
                    .cancellation
                    .commit(|| {
                        control
                            .set_source_status(AudioSourceKind::System, AudioSourceStatus::Ready);
                        control.set_source_status(
                            AudioSourceKind::Microphone,
                            AudioSourceStatus::PermissionDenied,
                        );
                    })
                    .unwrap();
                self.create_engine(config)
            }
        }
        let session = AudioSession::builder()
            .with_backend(Arc::new(PartialBackend))
            .build()
            .unwrap();
        let control = AudioControlHandle::new();
        let stream = session
            .start_streaming_with_controls(AudioStreamConfig::default(), control.clone())
            .unwrap();
        let levels = control.take_levels();
        assert_eq!(levels.system.status, AudioSourceStatus::Ready);
        assert_eq!(
            levels.microphone.status,
            AudioSourceStatus::PermissionDenied
        );
        drop(stream);
    }
}

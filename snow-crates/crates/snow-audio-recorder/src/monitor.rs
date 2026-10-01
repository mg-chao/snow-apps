//! Cancelable one-source preview. PCM is processed and immediately discarded.
use std::{sync::Arc, thread::JoinHandle, time::Duration};

use crate::{
    AudioControlHandle, AudioError, AudioEvent, AudioFormat, AudioResult, AudioSourceKind,
    AudioStreamConfig,
    backend::{AudioBackend, AudioBackendKind, AudioRecorderEngine, EngineEvent},
    control::{GainProcessor, source_index},
};

pub struct AudioMonitor {
    control: AudioControlHandle,
    cancellation: snow_core::cancellation::CancellationToken,
    worker: Option<JoinHandle<()>>,
}

impl AudioMonitor {
    /// Returns before native device initialization. Read source status from control().
    pub fn start(source: AudioSourceKind, gain_db: i32) -> AudioResult<Self> {
        Self::spawn(source, gain_db, || {
            crate::backend::backend_for_kind(AudioBackendKind::Auto)
        })
    }

    fn spawn(
        source: AudioSourceKind,
        gain_db: i32,
        backend: impl FnOnce() -> AudioResult<Arc<dyn AudioBackend>> + Send + 'static,
    ) -> AudioResult<Self> {
        let control = AudioControlHandle::new();
        control.set_gain_db(source, gain_db)?;
        control.set_metering(source, true);
        let mut config = AudioStreamConfig::default();
        config.system.enabled = source == AudioSourceKind::System;
        config.microphone.enabled = source == AudioSourceKind::Microphone;
        config.system.output_format = AudioFormat::new(48_000, 2);
        config.microphone.output_format = AudioFormat::new(48_000, 2);
        config.validate()?;
        control.initialize_sources(config.system.enabled, config.microphone.enabled);
        let cancellation = config.cancellation.clone();
        let worker_control = control.clone();
        let worker = std::thread::Builder::new()
            .name("snow-audio-monitor".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                if config.cancellation.is_canceled() {
                    worker_control.mark_stopped();
                    return;
                }
                // Creation, first WASAPI poll/COM initialization, and destruction all
                // belong to this worker. No samples enter a delivery queue or a file.
                match backend().and_then(|backend| {
                    backend.create_engine_with_controls(config.clone(), &worker_control)
                }) {
                    Ok(mut engine) => run_monitor(&mut engine, &config, &worker_control),
                    Err(error) => {
                        if config
                            .cancellation
                            .commit(|| worker_control.mark_initialization_error(&error))
                            .is_err()
                        {
                            worker_control.mark_stopped();
                        }
                    }
                }
            })
            .map_err(AudioError::platform)?;
        Ok(Self {
            control,
            cancellation,
            worker: Some(worker),
        })
    }

    pub fn control(&self) -> AudioControlHandle {
        self.control.clone()
    }

    pub fn cancel(&self) {
        self.cancellation.cancel();
        self.control.mark_stopped();
    }
}

impl Drop for AudioMonitor {
    fn drop(&mut self) {
        self.cancel();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

fn run_monitor(
    engine: &mut Box<dyn AudioRecorderEngine>,
    config: &AudioStreamConfig,
    control: &AudioControlHandle,
) {
    let mut processors = [
        GainProcessor::new(control.gain_db(AudioSourceKind::System)),
        GainProcessor::new(control.gain_db(AudioSourceKind::Microphone)),
    ];
    let mut errors = 0;
    while !config.cancellation.is_canceled() {
        let polled = engine.poll(Duration::from_millis(100));
        if config.cancellation.is_canceled() {
            break;
        }
        if let Some(states) = engine.source_states()
            && config
                .cancellation
                .commit(|| {
                    for (source, status) in [AudioSourceKind::System, AudioSourceKind::Microphone]
                        .into_iter()
                        .zip(states)
                    {
                        control.set_source_status(source, status);
                    }
                })
                .is_err()
        {
            break;
        }
        match polled {
            Ok(EngineEvent::Idle) => {}
            Ok(EngineEvent::Events(events)) => {
                errors = 0;
                for event in events {
                    match event {
                        AudioEvent::Packet(mut packet) => {
                            if let Some(measurement) = processors[source_index(packet.source)]
                                .process(&mut packet, control)
                            {
                                let _ = config.cancellation.commit(|| {
                                    control.publish(
                                        packet.source,
                                        measurement.peak,
                                        measurement.clipped,
                                        measurement.at,
                                    )
                                });
                            }
                        }
                        AudioEvent::Error(error) => {
                            publish_error(config, control, &error);
                            return;
                        }
                        AudioEvent::StreamEnded => {
                            control.mark_stopped();
                            return;
                        }
                        _ => {}
                    }
                }
            }
            Err(error) if error.is_retryable() => {
                errors += 1;
                if errors >= config.max_consecutive_errors {
                    publish_error(config, control, &error);
                    return;
                }
                std::thread::sleep(Duration::from_millis(16));
            }
            Err(error) => {
                publish_error(config, control, &error);
                return;
            }
        }
    }
    control.mark_stopped();
}

fn publish_error(config: &AudioStreamConfig, control: &AudioControlHandle, error: &AudioError) {
    if config
        .cancellation
        .commit(|| control.mark_error(error))
        .is_err()
    {
        control.mark_stopped();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{AudioDeviceInfo, AudioSourceStatus, DeviceFlow};
    use std::sync::mpsc;

    struct BlockedBackend {
        entered: mpsc::Sender<()>,
        release: std::sync::Mutex<mpsc::Receiver<()>>,
        owner: mpsc::Sender<std::thread::ThreadId>,
    }
    impl AudioBackend for BlockedBackend {
        fn enumerate_devices(&self, _: DeviceFlow) -> AudioResult<Vec<AudioDeviceInfo>> {
            Ok(Vec::new())
        }
        fn create_engine(&self, _: AudioStreamConfig) -> AudioResult<Box<dyn AudioRecorderEngine>> {
            self.owner.send(std::thread::current().id()).unwrap();
            self.entered.send(()).unwrap();
            self.release.lock().unwrap().recv().unwrap();
            Err(AudioError::AccessDenied)
        }
    }

    #[test]
    fn start_returns_before_native_initialization_and_cancel_never_publishes_late_failure() {
        let (entered_tx, entered_rx) = mpsc::channel();
        let (release_tx, release_rx) = mpsc::channel();
        let (owner_tx, owner_rx) = mpsc::channel();
        let backend: Arc<dyn AudioBackend> = Arc::new(BlockedBackend {
            entered: entered_tx,
            release: std::sync::Mutex::new(release_rx),
            owner: owner_tx,
        });
        let monitor =
            AudioMonitor::spawn(AudioSourceKind::Microphone, 3, move || Ok(backend)).unwrap();
        let control = monitor.control();
        entered_rx.recv_timeout(Duration::from_secs(5)).unwrap();
        assert_ne!(owner_rx.recv().unwrap(), std::thread::current().id());
        assert_eq!(
            control.take_levels().microphone.status,
            AudioSourceStatus::Starting
        );
        monitor.cancel();
        release_tx.send(()).unwrap();
        drop(monitor);
        assert_eq!(
            control.take_levels().microphone.status,
            AudioSourceStatus::Stopped
        );
    }

    #[test]
    fn cancellation_during_backend_status_read_prevents_late_ready_and_error() {
        struct Engine {
            cancellation: snow_core::cancellation::CancellationToken,
            control: AudioControlHandle,
        }
        impl AudioRecorderEngine for Engine {
            fn poll(&mut self, _: Duration) -> AudioResult<EngineEvent> {
                Ok(EngineEvent::Events(vec![AudioEvent::Error(
                    AudioError::AccessDenied,
                )]))
            }
            fn source_states(&self) -> Option<[AudioSourceStatus; 2]> {
                // Reproduce cancellation between the post-poll check and the
                // final status publication, without depending on thread timing.
                self.cancellation.cancel();
                self.control.mark_stopped();
                Some([AudioSourceStatus::Ready, AudioSourceStatus::Disabled])
            }
        }
        let control = AudioControlHandle::new();
        control.initialize_sources(true, false);
        let config = AudioStreamConfig::default();
        let mut engine: Box<dyn AudioRecorderEngine> = Box::new(Engine {
            cancellation: config.cancellation.clone(),
            control: control.clone(),
        });
        run_monitor(&mut engine, &config, &control);
        publish_error(&config, &control, &AudioError::AccessDenied);
        assert_eq!(
            control.take_levels().system.status,
            AudioSourceStatus::Stopped
        );
    }

    #[test]
    fn monitor_applies_gain_and_destroys_native_engine_on_its_owner_thread() {
        struct Engine {
            stage: usize,
            measured: mpsc::Sender<()>,
            release: mpsc::Receiver<()>,
            destroyed: mpsc::Sender<std::thread::ThreadId>,
            owner: std::thread::ThreadId,
        }
        impl AudioRecorderEngine for Engine {
            fn poll(&mut self, _: Duration) -> AudioResult<EngineEvent> {
                assert_eq!(std::thread::current().id(), self.owner);
                self.stage += 1;
                if self.stage == 1 {
                    Ok(EngineEvent::Events(vec![AudioEvent::Packet(
                        crate::AudioPacket {
                            source: AudioSourceKind::Microphone,
                            format: AudioFormat::new(48_000, 2),
                            frames: 480,
                            data: vec![1000; 960],
                            metadata: crate::AudioPacketMetadata::default(),
                        },
                    )]))
                } else {
                    self.measured.send(()).unwrap();
                    self.release.recv().unwrap();
                    Ok(EngineEvent::Idle)
                }
            }
        }
        impl Drop for Engine {
            fn drop(&mut self) {
                self.destroyed.send(std::thread::current().id()).unwrap();
            }
        }
        struct Backend(std::sync::Mutex<Option<Engine>>);
        impl AudioBackend for Backend {
            fn enumerate_devices(&self, _: DeviceFlow) -> AudioResult<Vec<AudioDeviceInfo>> {
                Ok(Vec::new())
            }
            fn create_engine(
                &self,
                _: AudioStreamConfig,
            ) -> AudioResult<Box<dyn AudioRecorderEngine>> {
                let mut engine = self.0.lock().unwrap().take().unwrap();
                engine.owner = std::thread::current().id();
                Ok(Box::new(engine))
            }
        }
        let (measured_tx, measured_rx) = mpsc::channel();
        let (release_tx, release_rx) = mpsc::channel();
        let (destroyed_tx, destroyed_rx) = mpsc::channel();
        let backend: Arc<dyn AudioBackend> =
            Arc::new(Backend(std::sync::Mutex::new(Some(Engine {
                stage: 0,
                measured: measured_tx,
                release: release_rx,
                destroyed: destroyed_tx,
                owner: std::thread::current().id(),
            }))));
        let monitor =
            AudioMonitor::spawn(AudioSourceKind::Microphone, -6, move || Ok(backend)).unwrap();
        measured_rx.recv_timeout(Duration::from_secs(5)).unwrap();
        let control = monitor.control();
        let levels = control.take_levels();
        assert_eq!(levels.microphone.peak, 501.0 / 32768.0);
        assert_eq!(levels.system.status, AudioSourceStatus::Disabled);
        monitor.cancel();
        release_tx.send(()).unwrap();
        drop(monitor);
        assert_ne!(destroyed_rx.recv().unwrap(), std::thread::current().id());
        assert_eq!(
            control.take_levels().microphone.status,
            AudioSourceStatus::Stopped
        );
    }
}

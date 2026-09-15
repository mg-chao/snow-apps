use crate::{
    backend::{AudioBackend, AudioRecorderEngine, EngineEvent},
    convert::{AudioConverter, NativeAudioFormat, NativeSampleFormat},
    device::{AudioDeviceInfo, DeviceFlow, DeviceSelector},
    error::{AudioError, AudioResult},
    packet::{AudioEvent, AudioPacket, AudioPacketMetadata, AudioSourceKind},
    session::AudioStreamConfig,
};
use snow_macos::{
    MacError,
    audio::{AudioSamples, SystemAudioStream},
    microphone::MicrophoneStream,
};
use std::time::{Duration, Instant};
pub(crate) struct MacAudioBackend;
fn map_error(e: MacError) -> AudioError {
    match e {
        MacError::PermissionDenied | MacError::MicrophonePermissionDenied => {
            AudioError::AccessDenied
        }
        MacError::TargetUnavailable => AudioError::DeviceLost,
        MacError::Canceled => AudioError::Canceled,
        MacError::Unsupported(_) | MacError::UnsupportedOs => {
            AudioError::BackendUnavailable(e.to_string())
        }
        other => AudioError::platform(anyhow::anyhow!(other)),
    }
}
fn selected_input<'a>(
    devices: &'a [snow_macos::microphone::InputDevice],
    selector: &DeviceSelector,
) -> AudioResult<&'a snow_macos::microphone::InputDevice> {
    devices
        .iter()
        .find(|device| match selector {
            DeviceSelector::DefaultCapture => device.is_default,
            DeviceSelector::Id(uid) => device.uid == *uid,
            _ => false,
        })
        .ok_or(AudioError::DeviceLost)
}
impl AudioBackend for MacAudioBackend {
    fn enumerate_devices(&self, flow: DeviceFlow) -> AudioResult<Vec<AudioDeviceInfo>> {
        if flow == DeviceFlow::Render {
            return Ok(vec![AudioDeviceInfo {
                id: "sck:system".into(),
                name: "System audio".into(),
                is_default: true,
                is_active: true,
                flow,
            }]);
        }
        Ok(snow_macos::microphone::input_devices()
            .map_err(map_error)?
            .into_iter()
            .map(|d| AudioDeviceInfo {
                id: d.uid,
                name: d.name,
                is_default: d.is_default,
                is_active: true,
                flow,
            })
            .collect())
    }
    fn create_engine(
        &self,
        config: AudioStreamConfig,
    ) -> AudioResult<Box<dyn AudioRecorderEngine>> {
        config.validate()?;
        let system = if config.system.enabled {
            if !matches!(config.system.device, DeviceSelector::DefaultRender) {
                return Err(AudioError::InvalidConfig(
                    "macOS system audio uses desktop playback, not a render device".into(),
                ));
            }
            match SystemAudioStream::start_cancelable(
                config.cancellation.clone(),
                Duration::from_secs(5),
            ) {
                Ok(source) => Some(source),
                Err(MacError::Canceled) => return Err(AudioError::Canceled),
                Err(_) if !config.system.required => None,
                Err(e) => return Err(map_error(e)),
            }
        } else {
            None
        };
        if config.cancellation.is_canceled() {
            return Err(AudioError::Canceled);
        }
        let microphone = if config.microphone.enabled {
            let uid = match &config.microphone.device {
                DeviceSelector::DefaultCapture => None,
                DeviceSelector::Id(id) => Some(id.as_str()),
                _ => {
                    return Err(AudioError::InvalidConfig(
                        "invalid microphone selector".into(),
                    ));
                }
            };
            match MicrophoneStream::start(uid) {
                Ok(source) => Some(source),
                Err(_) if !config.microphone.required => None,
                Err(e) => return Err(map_error(e)),
            }
        } else {
            None
        };
        if system.is_none() && microphone.is_none() {
            return Err(AudioError::DeviceUnavailable(
                "no audio source started".into(),
            ));
        }
        if config.cancellation.is_canceled() {
            return Err(AudioError::Canceled);
        }
        Ok(Box::new(Engine {
            config,
            system,
            microphone,
            converters: [None, None],
            formats: [None, None],
            sequence: [0, 0],
            device_check_at: Instant::now(),
            microphone_restarted: false,
        }))
    }
}
struct Engine {
    config: AudioStreamConfig,
    system: Option<SystemAudioStream>,
    microphone: Option<MicrophoneStream>,
    converters: [Option<AudioConverter>; 2],
    formats: [Option<(u32, u16)>; 2],
    sequence: [u64; 2],
    device_check_at: Instant,
    microphone_restarted: bool,
}
impl Engine {
    fn refresh_microphone(&mut self) -> AudioResult<Option<AudioEvent>> {
        if self.device_check_at.elapsed() < Duration::from_millis(250) {
            return Ok(None);
        }
        self.device_check_at = Instant::now();
        let Some(current) = &self.microphone else {
            return Ok(None);
        };
        if !snow_macos::microphone::microphone_authorized() {
            return Err(AudioError::AccessDenied);
        }
        let devices = snow_macos::microphone::input_devices().map_err(map_error)?;
        let selected = selected_input(&devices, &self.config.microphone.device)?;
        if current.device_uid() == selected.uid && current.is_running() {
            return Ok(None);
        }
        let old_device_id = Some(current.device_uid().to_owned());
        let begin = Instant::now();
        self.microphone.take();
        self.microphone = Some(MicrophoneStream::start(Some(&selected.uid)).map_err(map_error)?);
        self.formats[1] = None;
        self.converters[1] = None;
        self.microphone_restarted = true;
        Ok(Some(AudioEvent::SourceRestarted {
            source: AudioSourceKind::Microphone,
            old_device_id,
            new_device_id: selected.uid.clone(),
            downtime: begin.elapsed(),
        }))
    }
    fn packet(&mut self, input: AudioSamples, index: usize) -> AudioResult<AudioEvent> {
        let output = if index == 0 {
            self.config.system.output_format
        } else {
            self.config.microphone.output_format
        };
        if self.formats[index] != Some((input.sample_rate, input.channels)) {
            self.converters[index] = Some(AudioConverter::new(
                NativeAudioFormat {
                    sample_rate: input.sample_rate,
                    channels: input.channels,
                    sample_format: NativeSampleFormat::F32,
                },
                output,
            )?);
            self.formats[index] = Some((input.sample_rate, input.channels));
        }
        let source_frames = input.data.len() / usize::from(input.channels);
        let bytes = unsafe {
            std::slice::from_raw_parts(input.data.as_ptr().cast::<u8>(), input.data.len() * 4)
        };
        let data = self.converters[index]
            .as_mut()
            .unwrap()
            .convert_chunk(bytes, source_frames as u32)?;
        let frames = data.len() / usize::from(output.channels);
        let source_duration =
            Duration::from_secs_f64(source_frames as f64 / f64::from(input.sample_rate));
        let capture_time = snow_macos::time::host_time_to_instant(input.timestamp)
            .and_then(|time| time.checked_add(source_duration))
            .unwrap_or_else(Instant::now);
        let end = input
            .timestamp
            .checked_add_duration(source_duration)
            .ok_or_else(|| AudioError::InvalidConfig("audio host timestamp overflow".into()))?;
        self.sequence[index] = self.sequence[index].wrapping_add(1);
        let mut metadata = AudioPacketMetadata {
            sequence: self.sequence[index],
            discontinuity: input.discontinuity
                || (index == 1 && std::mem::take(&mut self.microphone_restarted)),
            ..Default::default()
        };
        metadata.stream_timestamp = Some(snow_core::timestamp::StreamTimestamp {
            instant: capture_time,
            raw_os_ticks: Some(end.value),
            tick_format: snow_core::timestamp::TickFormat::Rational {
                timescale: end.timescale,
                domain: end.domain,
                epoch: end.epoch,
            },
        });
        Ok(AudioEvent::Packet(AudioPacket {
            source: if index == 0 {
                AudioSourceKind::System
            } else {
                AudioSourceKind::Microphone
            },
            format: output,
            frames: frames as u32,
            data,
            metadata,
        }))
    }
}
impl AudioRecorderEngine for Engine {
    fn poll(&mut self, timeout: Duration) -> AudioResult<EngineEvent> {
        let deadline = Instant::now() + timeout;
        loop {
            if self.config.cancellation.is_canceled() {
                return Err(AudioError::Canceled);
            }
            let mut events = Vec::with_capacity(3);
            if let Some(event) = self.refresh_microphone()? {
                events.push(event);
            }
            for index in 0..2 {
                let result = if index == 0 {
                    self.system.as_ref().map(|s| s.next_samples(Duration::ZERO))
                } else {
                    self.microphone
                        .as_ref()
                        .map(|s| s.next_samples(Duration::ZERO))
                };
                match result {
                    Some(Ok(sample)) => events.push(self.packet(sample, index)?),
                    Some(Err(MacError::Timeout)) | None => {}
                    Some(Err(error)) => return Err(map_error(error)),
                }
            }
            if !events.is_empty() {
                return Ok(EngineEvent::Events(events));
            }
            if Instant::now() >= deadline {
                return Ok(EngineEvent::Idle);
            }
            std::thread::sleep(
                Duration::from_millis(2).min(deadline.saturating_duration_since(Instant::now())),
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn default_device_changes_but_explicit_selection_never_retargets() {
        let devices = vec![snow_macos::microphone::InputDevice {
            id: 4,
            uid: "replacement".into(),
            name: "Input".into(),
            is_default: true,
        }];
        assert_eq!(
            selected_input(&devices, &DeviceSelector::DefaultCapture)
                .unwrap()
                .uid,
            "replacement"
        );
        assert!(matches!(
            selected_input(&devices, &DeviceSelector::Id("removed".into())),
            Err(AudioError::DeviceLost)
        ));
        assert!(matches!(
            selected_input(&[], &DeviceSelector::DefaultCapture),
            Err(AudioError::DeviceLost)
        ));
    }
}

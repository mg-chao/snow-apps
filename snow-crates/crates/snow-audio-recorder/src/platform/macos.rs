// SPDX-License-Identifier: Apache-2.0
//! Native system audio (ScreenCaptureKit) and microphone (AVCaptureSession) capture.
//!
//! ScreenCaptureKit captures the system mix, independently of the current output device. CoreAudio
//! device UIDs identify microphones, without changing the system's default input device.
//!
//! Native callbacks use bounded queues; a slow consumer gets `PacketDropped` notifications instead
//! of unbounded memory growth. Devices are selected when capture starts. A microphone disconnection
//! or capture-service failure ends the stream; restart recording after fixing the device/permission.
//! A system output-device change does not require rebinding the ScreenCaptureKit system mix.

use std::ffi::{CStr, CString, c_char, c_void};
use std::ptr::NonNull;
use std::time::{Duration, Instant};

use crate::backend::{AudioBackend, AudioRecorderEngine, EngineEvent};
use crate::device::{AudioDeviceInfo, DeviceFlow, DeviceSelector};
use crate::error::{AudioError, AudioResult};
use crate::format::AudioFormat;
use crate::packet::{AudioEvent, AudioPacket, AudioPacketMetadata, AudioSourceKind};
use crate::session::{AudioStreamConfig, SourceConfig};

const SYSTEM_DEVICE: &str = "macos-system-audio";
const MAX_DEVICES: usize = 128;
const MAX_PACKET_SAMPLES: usize = 192_000 * 2;

#[repr(C)]
#[derive(Clone)]
struct NativeDevice {
    id: [c_char; 1024],
    name: [c_char; 1024],
    is_default: u8,
}

#[repr(C)]
#[derive(Default)]
struct NativePacket {
    frames: u32,
    end_host_ns: u64,
    dropped_frames: u64,
}

unsafe extern "C" {
    fn snow_audio_devices(
        devices: *mut NativeDevice,
        capacity: usize,
        error: *mut c_char,
        error_capacity: usize,
    ) -> i32;
    fn snow_audio_start(
        microphone: u8,
        device: *const c_char,
        rate: u32,
        channels: u16,
        queue_depth: usize,
        status: *mut i32,
        error: *mut c_char,
        error_capacity: usize,
    ) -> *mut c_void;
    fn snow_audio_poll(
        handle: *mut c_void,
        samples: *mut i16,
        sample_capacity: usize,
        packet: *mut NativePacket,
        timeout_ms: u32,
        error: *mut c_char,
        error_capacity: usize,
    ) -> i32;
    fn snow_audio_stop(handle: *mut c_void);
    fn snow_audio_host_time_ns() -> u64;
}

fn native_string(bytes: &[c_char]) -> String {
    // The array is initialized with zeros before FFI and the native side always terminates strings.
    let bytes: Vec<u8> = bytes.iter().map(|byte| *byte as u8).collect();
    CStr::from_bytes_until_nul(&bytes)
        .map(|string| string.to_string_lossy().into_owned())
        .unwrap_or_else(|_| "Invalid macOS audio response".into())
}

fn native_error(status: i32, message: &[c_char]) -> AudioError {
    match status {
        -2 => AudioError::platform(
            anyhow::Error::new(AudioError::AccessDenied).context(native_string(message)),
        ),
        -3 => AudioError::DeviceUnavailable(native_string(message)),
        -4 => AudioError::UnsupportedFormat(native_string(message)),
        _ => AudioError::platform(anyhow::anyhow!(native_string(message))),
    }
}

fn device_uid(kind: AudioSourceKind, selector: &DeviceSelector) -> AudioResult<CString> {
    let uid = match (kind, selector) {
        (AudioSourceKind::System, DeviceSelector::DefaultRender) => "",
        (AudioSourceKind::System, DeviceSelector::Id(id)) if id == SYSTEM_DEVICE => "",
        (AudioSourceKind::Microphone, DeviceSelector::DefaultCapture) => "",
        (AudioSourceKind::Microphone, DeviceSelector::Id(id)) if id != SYSTEM_DEVICE => id,
        _ => {
            return Err(AudioError::DeviceUnavailable(
                "the audio device does not match the requested source".into(),
            ));
        }
    };
    CString::new(uid)
        .map_err(|_| AudioError::InvalidConfig("audio device UID contains a NUL byte".into()))
}

fn validate_source(kind: AudioSourceKind, config: &SourceConfig) -> AudioResult<()> {
    if !config.enabled {
        return Ok(());
    }
    device_uid(kind, &config.device)?;
    if config.output_format.sample_rate > 192_000 || config.output_format.channels > 2 {
        return Err(AudioError::UnsupportedFormat(
            "macOS capture supports mono or stereo audio at up to 192 kHz".into(),
        ));
    }
    Ok(())
}

pub(crate) struct MacAudioBackend;

impl AudioBackend for MacAudioBackend {
    fn enumerate_devices(&self, flow: DeviceFlow) -> AudioResult<Vec<AudioDeviceInfo>> {
        if flow == DeviceFlow::Render {
            return Ok(vec![AudioDeviceInfo {
                id: SYSTEM_DEVICE.into(),
                name: "System Audio".into(),
                is_default: true,
                is_active: true,
                flow,
            }]);
        }
        let mut native = vec![
            NativeDevice {
                id: [0; 1024],
                name: [0; 1024],
                is_default: 0,
            };
            MAX_DEVICES
        ];
        let mut error = [0; 1024];
        // SAFETY: the native API receives the capacities of the initialized output buffers.
        let count = unsafe {
            snow_audio_devices(
                native.as_mut_ptr(),
                native.len(),
                error.as_mut_ptr(),
                error.len(),
            )
        };
        let count = usize::try_from(count).map_err(|_| native_error(count, &error))?;
        if count > native.len() {
            return Err(AudioError::BufferOverflow);
        }
        Ok(native[..count]
            .iter()
            .map(|device| AudioDeviceInfo {
                id: native_string(&device.id),
                name: native_string(&device.name),
                is_default: device.is_default != 0,
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
        validate_source(AudioSourceKind::System, &config.system)?;
        validate_source(AudioSourceKind::Microphone, &config.microphone)?;
        Ok(Box::new(MacAudioEngine {
            config,
            sources: None,
        }))
    }
}

struct MacAudioSource {
    handle: NonNull<c_void>,
    kind: AudioSourceKind,
    format: AudioFormat,
    sequence: u64,
    position: u64,
    samples: Vec<i16>,
}

// SAFETY: the handle owns an Objective-C source whose queue is internally synchronized. A source is
// polled and dropped by exactly one Rust worker; native callbacks do not access Rust-owned memory.
unsafe impl Send for MacAudioSource {}

impl Drop for MacAudioSource {
    fn drop(&mut self) {
        // SAFETY: this is the sole owner of the retained native handle, released exactly once.
        unsafe { snow_audio_stop(self.handle.as_ptr()) };
    }
}

fn host_instant(end_ns: u64, now_ns: u64, now: Instant) -> Instant {
    if end_ns <= now_ns {
        now.checked_sub(Duration::from_nanos(now_ns - end_ns))
            .unwrap_or(now)
    } else {
        now.checked_add(Duration::from_nanos(end_ns - now_ns))
            .unwrap_or(now)
    }
}

impl MacAudioSource {
    fn start(kind: AudioSourceKind, config: &SourceConfig, depth: usize) -> AudioResult<Self> {
        let uid = device_uid(kind, &config.device)?;
        let mut error = [0; 1024];
        let mut status = 0;
        // SAFETY: the UID is NUL-terminated and all outputs have their explicit capacities. The
        // bridge owns asynchronous state and returns a retained handle only after successful start.
        let raw = unsafe {
            snow_audio_start(
                u8::from(kind == AudioSourceKind::Microphone),
                uid.as_ptr(),
                config.output_format.sample_rate,
                config.output_format.channels,
                depth.min(128),
                &mut status,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        let handle = NonNull::new(raw).ok_or_else(|| native_error(status, &error))?;
        Ok(Self {
            handle,
            kind,
            format: config.output_format,
            sequence: 0,
            position: 0,
            samples: vec![0; MAX_PACKET_SAMPLES],
        })
    }

    fn poll(&mut self, timeout: Duration, events: &mut Vec<AudioEvent>) -> AudioResult<()> {
        let mut packet = NativePacket::default();
        let mut error = [0; 1024];
        // SAFETY: this handle remains alive, and the native API bounds copies by sample capacity.
        let status = unsafe {
            snow_audio_poll(
                self.handle.as_ptr(),
                self.samples.as_mut_ptr(),
                self.samples.len(),
                &mut packet,
                u32::try_from(timeout.as_millis()).unwrap_or(u32::MAX),
                error.as_mut_ptr(),
                error.len(),
            )
        };
        if status < 0 {
            return Err(native_error(status, &error));
        }
        if status == 0 {
            return Ok(());
        }
        let count = self.format.samples_for_frames(packet.frames)?;
        if count > self.samples.len() {
            return Err(AudioError::BufferOverflow);
        }
        self.sequence = self.sequence.saturating_add(1);
        self.position = self
            .position
            .saturating_add(packet.dropped_frames)
            .saturating_add(u64::from(packet.frames));
        if packet.dropped_frames > 0 {
            events.push(AudioEvent::PacketDropped {
                source: self.kind,
                dropped_frames: packet.dropped_frames,
            });
        }
        // SAFETY: this is a read-only monotonic host clock query without pointer arguments.
        let now_ns = unsafe { snow_audio_host_time_ns() };
        let end = host_instant(packet.end_host_ns, now_ns, Instant::now());
        let mut metadata = AudioPacketMetadata {
            device_position_frames: Some(self.position),
            discontinuity: packet.dropped_frames > 0,
            is_silent: self.samples[..count].iter().all(|sample| *sample == 0),
            sequence: self.sequence,
            ..AudioPacketMetadata::default()
        };
        metadata.set_timing(Some(end), i64::try_from(packet.end_host_ns / 100).ok());
        events.push(AudioEvent::Packet(AudioPacket {
            source: self.kind,
            format: self.format,
            frames: packet.frames,
            data: self.samples[..count].to_vec(),
            metadata,
        }));
        Ok(())
    }
}

struct MacAudioEngine {
    config: AudioStreamConfig,
    sources: Option<Vec<MacAudioSource>>,
}

impl MacAudioEngine {
    fn initialize(&mut self) -> AudioResult<()> {
        if self.sources.is_some() {
            return Ok(());
        }
        let mut sources = Vec::with_capacity(2);
        for (kind, config) in [
            (AudioSourceKind::System, &self.config.system),
            (AudioSourceKind::Microphone, &self.config.microphone),
        ] {
            if !config.enabled {
                continue;
            }
            match MacAudioSource::start(kind, config, self.config.event_buffer_depth) {
                Ok(source) => sources.push(source),
                Err(error) if config.required => return Err(error),
                Err(_) => {}
            }
        }
        if sources.is_empty() {
            return Err(AudioError::DeviceUnavailable(
                "no audio source could be started".into(),
            ));
        }
        self.sources = Some(sources);
        Ok(())
    }
}

impl AudioRecorderEngine for MacAudioEngine {
    fn poll(&mut self, timeout: Duration) -> AudioResult<EngineEvent> {
        self.initialize()?;
        let sources = self.sources.as_mut().ok_or(AudioError::WorkerDead)?;
        let deadline = Instant::now() + timeout;
        let mut events = Vec::new();
        loop {
            for source in &mut *sources {
                source.poll(Duration::ZERO, &mut events)?;
            }
            if !events.is_empty() || Instant::now() >= deadline {
                break;
            }
            // Short bounded waits keep the microphone responsive when system audio is silent.
            sources[0].poll(
                deadline
                    .saturating_duration_since(Instant::now())
                    .min(Duration::from_millis(5)),
                &mut events,
            )?;
        }
        Ok(if events.is_empty() {
            EngineEvent::Idle
        } else {
            EngineEvent::Events(events)
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn host_clock_mapping_preserves_packet_age_and_future_end() {
        let now = Instant::now();
        assert_eq!(host_instant(100, 200, now), now - Duration::from_nanos(100));
        assert_eq!(host_instant(300, 200, now), now + Duration::from_nanos(100));
    }

    #[test]
    fn source_selectors_do_not_silently_capture_the_wrong_device() {
        assert!(device_uid(AudioSourceKind::System, &DeviceSelector::DefaultRender).is_ok());
        assert!(
            device_uid(
                AudioSourceKind::System,
                &DeviceSelector::Id("other-output".into())
            )
            .is_err()
        );
        assert!(device_uid(AudioSourceKind::System, &DeviceSelector::DefaultCapture).is_err());
        assert!(
            device_uid(
                AudioSourceKind::Microphone,
                &DeviceSelector::Id("uid\0bad".into())
            )
            .is_err()
        );
        assert!(device_uid(AudioSourceKind::Microphone, &DeviceSelector::DefaultCapture).is_ok());
    }

    #[test]
    fn unsupported_audio_formats_fail_before_permission_requests() {
        let mut config = AudioStreamConfig::default();
        config.microphone.output_format.channels = 6;
        assert!(matches!(
            MacAudioBackend.create_engine(config),
            Err(AudioError::UnsupportedFormat(_))
        ));
    }

    #[test]
    fn permission_errors_preserve_actionable_guidance_and_fatal_class() {
        let mut message = [0; 128];
        for (destination, byte) in message
            .iter_mut()
            .zip(b"Enable Microphone in System Settings")
        {
            *destination = *byte as c_char;
        }
        let error = native_error(-2, &message);
        assert_eq!(error.class(), crate::error::AudioErrorClass::Fatal);
        assert!(
            error
                .to_string()
                .contains("Enable Microphone in System Settings")
        );
        assert!(!error.is_retryable());
    }

    #[test]
    fn system_audio_is_one_mix_instead_of_an_incorrect_output_device_list() {
        let devices = MacAudioBackend
            .enumerate_devices(DeviceFlow::Render)
            .unwrap();
        assert_eq!(devices.len(), 1);
        assert_eq!(devices[0].id, SYSTEM_DEVICE);
        assert!(devices[0].is_default);
    }
}

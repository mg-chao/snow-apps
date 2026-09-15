//! CoreAudio device selection and AVAudioEngine input taps with a fixed buffer pool.
use crate::{MacError, MacResult, audio::AudioSamples};
use block2::RcBlock;
use crossbeam_channel::Receiver;
use objc2::rc::Retained;
use objc2_audio_toolbox::{
    AudioUnitSetProperty, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global,
};
use objc2_av_foundation::{AVAuthorizationStatus, AVCaptureDevice, AVMediaTypeAudio};
use objc2_avf_audio::{AVAudioEngine, AVAudioPCMBuffer, AVAudioTime};
use objc2_core_audio::*;
use objc2_core_foundation::{CFRetained, CFString};
use objc2_core_media::CMClock;
use snow_media::time::{ClockDomain, MediaTime};
use std::{
    ptr::NonNull,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    time::Duration,
};

#[derive(Clone, Debug)]
pub struct InputDevice {
    pub id: u32,
    pub uid: String,
    pub name: String,
    pub is_default: bool,
}
fn address(selector: u32, scope: u32) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress {
        mSelector: selector,
        mScope: scope,
        mElement: kAudioObjectPropertyElementMain,
    }
}
fn property<T: Copy + Default>(id: u32, selector: u32) -> MacResult<T> {
    let mut result = T::default();
    let mut size = size_of::<T>() as u32;
    let mut addr = address(selector, kAudioObjectPropertyScopeGlobal);
    let status = unsafe {
        AudioObjectGetPropertyData(
            id,
            NonNull::from(&mut addr),
            0,
            std::ptr::null(),
            NonNull::from(&mut size),
            NonNull::from(&mut result).cast(),
        )
    };
    if status != 0 || size != size_of::<T>() as u32 {
        return Err(MacError::TargetUnavailable);
    }
    Ok(result)
}
fn string_property(id: u32, selector: u32) -> MacResult<String> {
    let pointer: *mut CFString = property(id, selector)?;
    let pointer = NonNull::new(pointer).ok_or(MacError::TargetUnavailable)?;
    // CoreAudio transfers a retained CFString for name/UID properties.
    Ok(unsafe { CFRetained::from_raw(pointer) }.to_string())
}
pub fn input_devices() -> MacResult<Vec<InputDevice>> {
    let default: u32 = property(
        kAudioObjectSystemObject as u32,
        kAudioHardwarePropertyDefaultInputDevice,
    )?;
    let mut addr = address(
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
    );
    let mut size = 0;
    let status = unsafe {
        AudioObjectGetPropertyDataSize(
            kAudioObjectSystemObject as u32,
            NonNull::from(&mut addr),
            0,
            std::ptr::null(),
            NonNull::from(&mut size),
        )
    };
    if status != 0 || size % 4 != 0 {
        return Err(MacError::TargetUnavailable);
    }
    let mut ids = vec![0u32; size as usize / 4];
    if ids.is_empty() {
        return Ok(vec![]);
    }
    let status = unsafe {
        AudioObjectGetPropertyData(
            kAudioObjectSystemObject as u32,
            NonNull::from(&mut addr),
            0,
            std::ptr::null(),
            NonNull::from(&mut size),
            NonNull::new(ids.as_mut_ptr().cast()).unwrap(),
        )
    };
    if status != 0 || size as usize > ids.len() * 4 {
        return Err(MacError::TargetUnavailable);
    }
    let mut devices = Vec::new();
    for &id in &ids[..size as usize / 4] {
        let mut addr = address(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput);
        let mut size = 0;
        let status = unsafe {
            AudioObjectGetPropertyDataSize(
                id,
                NonNull::from(&mut addr),
                0,
                std::ptr::null(),
                NonNull::from(&mut size),
            )
        };
        if status == 0 && size > 0 {
            devices.push(InputDevice {
                id,
                uid: string_property(id, kAudioDevicePropertyDeviceUID)?,
                name: string_property(id, kAudioObjectPropertyName)?,
                is_default: id == default,
            });
        }
    }
    Ok(devices)
}
pub fn microphone_authorized() -> bool {
    unsafe {
        AVCaptureDevice::authorizationStatusForMediaType(
            AVMediaTypeAudio.expect("AVMediaTypeAudio is available on macOS 15"),
        ) == AVAuthorizationStatus::Authorized
    }
}
/// Explicit asynchronous permission request. The host must provide microphone usage metadata.
pub fn request_microphone_access(completion: impl Fn(bool) + Send + 'static) {
    let block = RcBlock::new(move |granted: objc2::runtime::Bool| completion(granted.as_bool()));
    unsafe {
        AVCaptureDevice::requestAccessForMediaType_completionHandler(
            AVMediaTypeAudio.expect("AVMediaTypeAudio is available on macOS 15"),
            &block,
        );
    }
}

pub struct MicrophoneStream {
    uid: String,
    engine: Retained<AVAudioEngine>,
    samples: Receiver<AudioSamples>,
    dropped: Arc<AtomicU64>,
}
// SAFETY: engine control has one owner; the callback communicates through bounded channels.
unsafe impl Send for MicrophoneStream {}
impl MicrophoneStream {
    pub fn start(uid: Option<&str>) -> MacResult<Self> {
        if !microphone_authorized() {
            return Err(MacError::MicrophonePermissionDenied);
        }
        let device = input_devices()?
            .into_iter()
            .find(|d| uid.map_or(d.is_default, |uid| d.uid == uid))
            .ok_or(MacError::TargetUnavailable)?;
        let engine = unsafe { AVAudioEngine::new() };
        let node = unsafe { engine.inputNode() };
        let unit = unsafe { node.audioUnit() };
        let status = unsafe {
            AudioUnitSetProperty(
                unit,
                kAudioOutputUnitProperty_CurrentDevice,
                kAudioUnitScope_Global,
                0,
                (&raw const device.id).cast(),
                size_of::<u32>() as u32,
            )
        };
        if status != 0 {
            return Err(MacError::TargetUnavailable);
        }
        let format = unsafe { node.outputFormatForBus(0) };
        let channels = unsafe { format.channelCount() } as usize;
        let rate = unsafe { format.sampleRate() };
        if channels == 0 || channels > 8 || !rate.is_finite() || rate <= 0.0 {
            return Err(MacError::Unsupported("invalid microphone format".into()));
        }
        let (sender, samples) = crossbeam_channel::bounded(8);
        let (recycle, available) = crossbeam_channel::bounded(8);
        for _ in 0..8 {
            let _ = recycle.try_send(Vec::<f32>::with_capacity(8192 * channels));
        }
        let dropped = Arc::new(AtomicU64::new(0));
        let loss = dropped.clone();
        let tap = RcBlock::new(
            move |buffer: NonNull<AVAudioPCMBuffer>, when: NonNull<AVAudioTime>| unsafe {
                let buffer = buffer.as_ref();
                let when = when.as_ref();
                let frames = buffer.frameLength() as usize;
                let ptr = buffer.floatChannelData();
                if ptr.is_null()
                    || frames > 8192
                    || !when.isHostTimeValid()
                    || buffer.format().channelCount() as usize != channels
                    || buffer.format().sampleRate() != rate
                {
                    loss.fetch_add(1, Ordering::Relaxed);
                    return;
                }
                let Ok(mut data) = available.try_recv() else {
                    loss.fetch_add(1, Ordering::Relaxed);
                    return;
                };
                let stride = buffer.stride();
                data.clear();
                for frame in 0..frames {
                    for channel in 0..channels {
                        data.push(*(*ptr.add(channel)).as_ptr().add(frame * stride));
                    }
                }
                let time = CMClock::make_host_time_from_system_units(when.hostTime());
                let sample = AudioSamples {
                    data,
                    channels: channels as u16,
                    sample_rate: rate as u32,
                    timestamp: MediaTime {
                        value: time.value,
                        timescale: time.timescale as u32,
                        domain: ClockDomain::MacHostTime,
                        epoch: time.epoch,
                    },
                    discontinuity: false,
                    recycle: Some(recycle.clone()),
                };
                if sender.try_send(sample).is_err() {
                    loss.fetch_add(1, Ordering::Relaxed);
                }
            },
        );
        unsafe {
            node.installTapOnBus_bufferSize_format_block(
                0,
                1024,
                Some(&format),
                RcBlock::as_ptr(&tap),
            );
            engine.prepare();
        }
        let result = Self {
            uid: device.uid,
            engine,
            samples,
            dropped,
        };
        unsafe { result.engine.startAndReturnError() }.map_err(|e| MacError::from_native(&e))?;
        Ok(result)
    }
    pub fn device_uid(&self) -> &str {
        &self.uid
    }
    pub fn is_running(&self) -> bool {
        unsafe { self.engine.isRunning() }
    }
    pub fn next_samples(&self, timeout: Duration) -> MacResult<AudioSamples> {
        let mut result = self
            .samples
            .recv_timeout(timeout)
            .map_err(|_| MacError::Timeout)?;
        result.discontinuity = self.dropped.swap(0, Ordering::AcqRel) != 0;
        Ok(result)
    }
}
impl Drop for MicrophoneStream {
    fn drop(&mut self) {
        unsafe {
            self.engine.stop();
            self.engine.inputNode().removeTapOnBus(0);
        }
    }
}

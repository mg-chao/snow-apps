//! ScreenCaptureKit system audio, delivered without conversion on the callback queue.
use crate::{MacError, MacResult};
use block2::RcBlock;
use crossbeam_channel::{Receiver, Sender};
use dispatch2::{DispatchQueue, DispatchRetained};
use objc2::{
    AnyThread, DefinedClass, define_class, msg_send, rc::Retained, runtime::ProtocolObject,
};
use objc2_core_audio_types::{
    AudioBuffer, AudioBufferList, kAudioFormatFlagIsBigEndian, kAudioFormatFlagIsFloat,
    kAudioFormatLinearPCM,
};
use objc2_core_foundation::CFRetained;
use objc2_core_media::{
    CMAudioFormatDescriptionGetStreamBasicDescription, CMBlockBuffer, CMSampleBuffer,
};
use objc2_foundation::{NSError, NSObject, NSObjectProtocol};
use objc2_screen_capture_kit::*;
use snow_media::time::{ClockDomain, MediaTime};
use std::{
    ptr::NonNull,
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    time::Duration,
};

pub struct AudioSamples {
    pub data: Vec<f32>,
    pub channels: u16,
    pub sample_rate: u32,
    pub timestamp: MediaTime,
    pub discontinuity: bool,
    pub(crate) recycle: Option<Sender<Vec<f32>>>,
}
struct Sample(CFRetained<CMSampleBuffer>);
// SAFETY: retained ready audio samples are immutable; conversion runs on one consumer.
unsafe impl Send for Sample {}
impl Sample {
    fn decode(&self) -> MacResult<AudioSamples> {
        unsafe {
            let format = self.0.format_description().ok_or(MacError::Inactive)?;
            let description = CMAudioFormatDescriptionGetStreamBasicDescription(&format)
                .as_ref()
                .ok_or(MacError::Inactive)?;
            if description.mFormatID != kAudioFormatLinearPCM
                || description.mBitsPerChannel != 32
                || description.mFormatFlags & kAudioFormatFlagIsFloat == 0
                || description.mFormatFlags & kAudioFormatFlagIsBigEndian != 0
                || !(1..=8).contains(&description.mChannelsPerFrame)
            {
                return Err(MacError::Unsupported(
                    "expected 1..=8 channels of little-endian float PCM".into(),
                ));
            }
            #[repr(C)]
            struct BufferList {
                count: u32,
                buffers: [AudioBuffer; 8],
            }
            let mut list = BufferList {
                count: 0,
                buffers: [AudioBuffer {
                    mNumberChannels: 0,
                    mDataByteSize: 0,
                    mData: std::ptr::null_mut(),
                }; 8],
            };
            let mut block: *mut CMBlockBuffer = std::ptr::null_mut();
            let mut needed = 0;
            let query = self.0.audio_buffer_list_with_retained_block_buffer(
                &raw mut needed,
                std::ptr::null_mut(),
                0,
                None,
                None,
                0,
                std::ptr::null_mut(),
            );
            if query != 0 || needed > size_of::<BufferList>() {
                return Err(MacError::Unsupported(format!(
                    "audio buffer list size query failed: {query}, size={needed}"
                )));
            }
            let status = self.0.audio_buffer_list_with_retained_block_buffer(
                std::ptr::null_mut(),
                (&raw mut list).cast::<AudioBufferList>(),
                needed,
                None,
                None,
                0,
                &raw mut block,
            );
            let _retained = NonNull::new(block).map(|ptr| CFRetained::from_raw(ptr));
            if status != 0 || list.count == 0 || list.count > 8 {
                return Err(MacError::InvalidConfig(format!(
                    "invalid native audio buffer list: status={status}, buffers={}, frames={}, needed={needed}, allocated={}, format={description:?}",
                    list.count,
                    self.0.num_samples(),
                    size_of::<BufferList>()
                )));
            }
            let frames = usize::try_from(self.0.num_samples()).map_err(|_| MacError::Inactive)?;
            let channels = description.mChannelsPerFrame as usize;
            let len = frames
                .checked_mul(channels)
                .filter(|n| *n <= isize::MAX as usize / 4)
                .ok_or_else(|| MacError::InvalidConfig("audio buffer overflow".into()))?;
            let mut data = vec![0.0; len];
            let mut channel_offset = 0;
            for buffer in &list.buffers[..list.count as usize] {
                let count = buffer.mNumberChannels as usize;
                let needed = frames
                    .checked_mul(count)
                    .and_then(|v| v.checked_mul(4))
                    .ok_or(MacError::Inactive)?;
                if count == 0
                    || channel_offset + count > channels
                    || buffer.mData.is_null()
                    || needed > buffer.mDataByteSize as usize
                {
                    return Err(MacError::Inactive);
                }
                for frame in 0..frames {
                    for channel in 0..count {
                        data[frame * channels + channel_offset + channel] =
                            std::ptr::read_unaligned(
                                buffer.mData.cast::<f32>().add(frame * count + channel),
                            );
                    }
                }
                channel_offset += count;
            }
            if channel_offset != channels {
                return Err(MacError::Inactive);
            }
            let time = self.0.presentation_time_stamp();
            if time.timescale <= 0 {
                return Err(MacError::Inactive);
            }
            Ok(AudioSamples {
                data,
                channels: channels as u16,
                sample_rate: description.mSampleRate as u32,
                timestamp: MediaTime {
                    value: time.value,
                    timescale: time.timescale as u32,
                    domain: ClockDomain::MacHostTime,
                    epoch: time.epoch,
                },
                discontinuity: false,
                recycle: None,
            })
        }
    }
}
struct State {
    cancellation: crate::CancellationToken,
    sender: Sender<Sample>,
    errors: Sender<MacError>,
    closed: AtomicBool,
    dropped: AtomicU64,
}
define_class!(
    #[unsafe(super(NSObject))]
    #[name = "SnowSystemAudioOutput"]
    #[ivars = Arc<State>]
    struct Output;
    unsafe impl NSObjectProtocol for Output {}
    unsafe impl SCStreamOutput for Output {
        #[unsafe(method(stream:didOutputSampleBuffer:ofType:))]
        unsafe fn receive(&self, _stream: &SCStream, sample: &CMSampleBuffer, kind: SCStreamOutputType) {
            let state = self.ivars();
            if kind != SCStreamOutputType::Audio || state.closed.load(Ordering::Acquire) || state.cancellation.is_canceled() { return; }
            if !unsafe { sample.is_valid() && sample.data_is_ready() } { return; }
            let retained = unsafe { CFRetained::retain(NonNull::from(sample)) };
            if state.sender.try_send(Sample(retained)).is_err() { state.dropped.fetch_add(1, Ordering::Relaxed); }
        }
    }
    unsafe impl SCStreamDelegate for Output {
        #[unsafe(method(stream:didStopWithError:))]
        unsafe fn stopped(&self, _stream: &SCStream, error: &NSError) {
            if !self.ivars().closed.swap(true, Ordering::AcqRel) { let _ = self.ivars().errors.try_send(MacError::from_native(error)); }
        }
    }
);
pub struct SystemAudioStream {
    stream: Retained<SCStream>,
    output: Retained<Output>,
    _queue: DispatchRetained<DispatchQueue>,
    samples: Receiver<Sample>,
    errors: Receiver<MacError>,
}
// SAFETY: exclusive session ownership; callback state is synchronized and immutable.
unsafe impl Send for SystemAudioStream {}

fn configure_system_audio(config: &SCStreamConfiguration) {
    unsafe {
        // This stream has no screen-output consumer. Only the recording's video
        // stream should request cursor capture or mouse-click visualization.
        config.setShowsCursor(false);
        config.setShowMouseClicks(false);
        config.setCapturesAudio(true);
        config.setExcludesCurrentProcessAudio(true);
        config.setSampleRate(48_000);
        config.setChannelCount(2);
    }
}

impl SystemAudioStream {
    pub fn start() -> MacResult<Self> {
        Self::start_cancelable(crate::CancellationToken::default(), Duration::from_secs(5))
    }
    pub fn start_cancelable(
        cancellation: crate::CancellationToken,
        timeout: Duration,
    ) -> MacResult<Self> {
        if cancellation.is_canceled() {
            return Err(MacError::Canceled);
        }
        let deadline = crate::deadline::Deadline::new(timeout)?;
        let display = crate::content::displays_cancelable(deadline.remaining()?, &cancellation)?
            .into_iter()
            .find(|d| d.primary)
            .ok_or(MacError::TargetUnavailable)?;
        let mut options =
            crate::capture::CaptureConfig::new(crate::capture::Target::Display(display.id));
        options.output = Some(snow_media::geometry::PixelSize {
            width: 2,
            height: 2,
        });
        options.frames_per_second = 1;
        options.cancellation = cancellation.clone();
        options.timeout = deadline.remaining()?;
        let prepared = crate::capture::prepare(&options)?;
        configure_system_audio(&prepared.config);
        let (sender, samples) = crossbeam_channel::bounded(64);
        let (error_tx, errors) = crossbeam_channel::bounded(1);
        let state = Arc::new(State {
            cancellation: cancellation.clone(),
            sender,
            errors: error_tx,
            closed: AtomicBool::new(false),
            dropped: AtomicU64::new(0),
        });
        let allocated = Output::alloc().set_ivars(state);
        let output: Retained<Output> = unsafe { msg_send![super(allocated), init] };
        let queue = DispatchQueue::new("app.snow.capture.audio", None);
        let stream = unsafe {
            SCStream::initWithFilter_configuration_delegate(
                SCStream::alloc(),
                &prepared.filter,
                &prepared.config,
                Some(ProtocolObject::from_ref(&*output)),
            )
        };
        unsafe {
            stream.addStreamOutput_type_sampleHandlerQueue_error(
                ProtocolObject::from_ref(&*output),
                SCStreamOutputType::Audio,
                Some(&queue),
            )
        }
        .map_err(|e| MacError::from_native(&e))?;
        let result = Self {
            stream,
            output,
            _queue: queue,
            samples,
            errors,
        };
        let (tx, rx) = crossbeam_channel::bounded(1);
        let pending_stream = result.stream.clone();
        let pending_state = result.output.ivars().clone();
        let completion = RcBlock::new(move |error: *mut NSError| {
            let status = unsafe {
                error
                    .as_ref()
                    .map_or(Ok(()), |e| Err(MacError::from_native(e)))
            };
            if tx.try_send(status).is_err()
                || pending_state.closed.load(Ordering::Acquire)
                || pending_state.cancellation.is_canceled()
            {
                pending_state.closed.store(true, Ordering::Release);
                unsafe {
                    pending_stream.stopCaptureWithCompletionHandler(None);
                }
            }
        });
        unsafe {
            result
                .stream
                .startCaptureWithCompletionHandler(Some(&completion));
        }
        cancellation.wait_for(&rx, deadline.remaining()?)??;
        Ok(result)
    }
    pub fn next_samples(&self, timeout: Duration) -> MacResult<AudioSamples> {
        if self.output.ivars().cancellation.is_canceled() {
            return Err(MacError::Canceled);
        }
        if self.output.ivars().closed.load(Ordering::Acquire) {
            return Err(self.errors.try_recv().unwrap_or(MacError::Canceled));
        }
        crossbeam_channel::select_biased! {
            recv(self.output.ivars().cancellation.receiver()) -> _ => Err(MacError::Canceled),
            recv(self.errors) -> e => Err(e.unwrap_or(MacError::Canceled)),
            recv(self.samples) -> sample => {
                let mut samples = sample.map_err(|_| MacError::Canceled)?.decode()?;
                samples.discontinuity = self.output.ivars().dropped.swap(0, Ordering::AcqRel) != 0;
                if self.output.ivars().cancellation.is_canceled() { return Err(MacError::Canceled); }
                Ok(samples)
            },
            default(timeout) => Err(MacError::Timeout),
        }
    }
}
impl Drop for SystemAudioStream {
    fn drop(&mut self) {
        self.output.ivars().closed.store(true, Ordering::Release);
        let (tx, rx) = crossbeam_channel::bounded(1);
        let completion = RcBlock::new(move |_error: *mut NSError| {
            let _ = tx.try_send(());
        });
        unsafe {
            self.stream
                .stopCaptureWithCompletionHandler(Some(&completion));
        }
        // Destruction must not wait indefinitely for an OS callback. The callback
        // owns only its channel and remains safe after this session is released.
        let _ = rx.recv_timeout(Duration::from_secs(2));
    }
}

impl Drop for AudioSamples {
    fn drop(&mut self) {
        if let Some(recycle) = &self.recycle {
            let _ = recycle.try_send(std::mem::take(&mut self.data));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use objc2_core_audio_types::{
        AudioStreamBasicDescription, kAudioFormatFlagIsNonInterleaved, kAudioFormatFlagIsPacked,
    };
    use objc2_core_media::{
        CMAudioFormatDescriptionCreate, CMSampleTimingInfo, CMTime, CMTimeFlags,
    };
    #[test]
    fn system_audio_does_not_capture_the_cursor() {
        // Exercise the native configuration without starting a capture or asking
        // for permissions. Video defaults must not leak into the audio-only stream.
        let config = unsafe { SCStreamConfiguration::new() };
        unsafe {
            config.setShowsCursor(true);
            config.setShowMouseClicks(true);
        }
        configure_system_audio(&config);
        unsafe {
            assert!(!config.showsCursor());
            assert!(!config.showMouseClicks());
            assert!(config.capturesAudio());
            assert!(config.excludesCurrentProcessAudio());
            assert_eq!(config.sampleRate(), 48_000);
            assert_eq!(config.channelCount(), 2);
        }
    }
    #[test]
    fn canceled_system_audio_does_not_enumerate_or_request_permission() {
        let cancellation = crate::CancellationToken::default();
        cancellation.cancel();
        assert!(matches!(
            SystemAudioStream::start_cancelable(cancellation, Duration::from_secs(1)),
            Err(MacError::Canceled)
        ));
    }
    #[test]
    fn native_planar_audio_uses_queried_list_size_and_interleaves_channels() {
        unsafe {
            let mut description = AudioStreamBasicDescription {
                mSampleRate: 48_000.0,
                mFormatID: kAudioFormatLinearPCM,
                mFormatFlags: kAudioFormatFlagIsFloat
                    | kAudioFormatFlagIsPacked
                    | kAudioFormatFlagIsNonInterleaved,
                mBytesPerPacket: 4,
                mFramesPerPacket: 1,
                mBytesPerFrame: 4,
                mChannelsPerFrame: 2,
                mBitsPerChannel: 32,
                mReserved: 0,
            };
            let mut format = std::ptr::null();
            assert_eq!(
                CMAudioFormatDescriptionCreate(
                    None,
                    NonNull::from(&mut description),
                    0,
                    std::ptr::null(),
                    0,
                    std::ptr::null(),
                    None,
                    NonNull::from(&mut format)
                ),
                0
            );
            let format = CFRetained::from_raw(NonNull::new(format.cast_mut()).unwrap());
            let time = CMTime {
                value: 0,
                timescale: 48_000,
                flags: CMTimeFlags::Valid,
                epoch: 0,
            };
            let timing = CMSampleTimingInfo {
                duration: CMTime { value: 1, ..time },
                presentationTimeStamp: time,
                decodeTimeStamp: time,
            };
            let mut sample = std::ptr::null_mut();
            let sample_size = 4;
            assert_eq!(
                CMSampleBuffer::create_ready(
                    None,
                    None,
                    Some(&format),
                    4,
                    1,
                    &timing,
                    1,
                    &sample_size,
                    NonNull::from(&mut sample)
                ),
                0
            );
            let sample = CFRetained::from_raw(NonNull::new(sample).unwrap());
            #[repr(C)]
            struct StereoList {
                count: u32,
                buffers: [AudioBuffer; 2],
            }
            let mut left = [0.25_f32, -0.5, 0.75, 1.0];
            let mut right = [-0.25_f32, 0.5, -0.75, -1.0];
            let mut list = StereoList {
                count: 2,
                buffers: [
                    AudioBuffer {
                        mNumberChannels: 1,
                        mDataByteSize: 16,
                        mData: left.as_mut_ptr().cast(),
                    },
                    AudioBuffer {
                        mNumberChannels: 1,
                        mDataByteSize: 16,
                        mData: right.as_mut_ptr().cast(),
                    },
                ],
            };
            assert_eq!(
                sample.set_data_buffer_from_audio_buffer_list(
                    None,
                    None,
                    0,
                    NonNull::from(&mut list).cast()
                ),
                0
            );
            let decoded = Sample(sample).decode().unwrap();
            assert_eq!(
                decoded.data,
                [0.25, -0.25, -0.5, 0.5, 0.75, -0.75, 1.0, -1.0]
            );
            assert_eq!(decoded.channels, 2);
            assert_eq!(decoded.sample_rate, 48_000);
        }
    }
}

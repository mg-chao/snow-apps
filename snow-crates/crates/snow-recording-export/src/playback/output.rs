#[cfg(windows)]
mod native {
    use windows::Win32::Media::Audio::*;
    use windows::Win32::System::Com::*;
    struct Apartment;
    impl Drop for Apartment {
        fn drop(&mut self) {
            unsafe {
                CoUninitialize();
            }
        }
    }

    pub struct Output {
        client: IAudioClient,
        render: IAudioRenderClient,
        capacity: u32,
        written: u64,
        started: bool,
        _apartment: Apartment,
    }
    impl Output {
        pub fn open() -> Option<Self> {
            unsafe { Self::create().ok() }
        }
        unsafe fn create() -> windows::core::Result<Self> {
            unsafe {
                // The playback worker is dedicated to this apartment and its lifetime.
                CoInitializeEx(None, COINIT_MULTITHREADED).ok()?;
                let apartment = Apartment;
                let devices: IMMDeviceEnumerator =
                    CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)?;
                let device = devices.GetDefaultAudioEndpoint(eRender, eMultimedia)?;
                let client: IAudioClient = device.Activate(CLSCTX_ALL, None)?;
                let format = WAVEFORMATEX {
                    wFormatTag: 1,
                    nChannels: 2,
                    nSamplesPerSec: 48_000,
                    nAvgBytesPerSec: 192_000,
                    nBlockAlign: 4,
                    wBitsPerSample: 16,
                    cbSize: 0,
                };
                client.Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    1_000_000,
                    0,
                    &format,
                    None,
                )?;
                let render = client.GetService()?;
                let capacity = client.GetBufferSize()?;
                Ok(Self {
                    client,
                    render,
                    capacity,
                    written: 0,
                    started: false,
                    _apartment: apartment,
                })
            }
        }
        pub fn reset(&mut self) {
            unsafe {
                let _ = self.client.Stop();
                let _ = self.client.Reset();
                self.written = 0;
                self.started = false;
            }
        }
        pub fn available(&self) -> u64 {
            unsafe {
                self.client.GetCurrentPadding().map_or(0, |padding| {
                    u64::from(self.capacity.saturating_sub(padding))
                })
            }
        }
        pub fn played(&self) -> u64 {
            unsafe {
                self.written
                    .saturating_sub(u64::from(self.client.GetCurrentPadding().unwrap_or(0)))
            }
        }
        pub fn push(&mut self, samples: &[i16]) -> bool {
            unsafe {
                let frames = (samples.len() / 2) as u32;
                let Ok(data) = self.render.GetBuffer(frames) else {
                    return false;
                };
                std::ptr::copy_nonoverlapping(samples.as_ptr(), data.cast(), samples.len());
                if self.render.ReleaseBuffer(frames, 0).is_err() {
                    return false;
                }
                self.written += u64::from(frames);
                if !self.started {
                    if self.client.Start().is_err() {
                        return false;
                    }
                    self.started = true;
                }
                true
            }
        }
    }
    impl Drop for Output {
        fn drop(&mut self) {
            self.reset();
        }
    }
}

#[cfg(target_os = "macos")]
mod native {
    use std::ffi::c_void;
    use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
    type Queue = *mut c_void;
    #[repr(C)]
    struct Format {
        rate: f64,
        id: u32,
        flags: u32,
        packet_bytes: u32,
        packet_frames: u32,
        frame_bytes: u32,
        channels: u32,
        bits: u32,
        reserved: u32,
    }
    #[repr(C)]
    struct Buffer {
        capacity: u32,
        data: *mut c_void,
        size: u32,
        user: *mut c_void,
        packet_capacity: u32,
        packets: *mut c_void,
        packet_count: u32,
    }
    #[link(name = "AudioToolbox", kind = "framework")]
    unsafe extern "C" {
        fn AudioQueueNewOutput(
            format: *const Format,
            callback: unsafe extern "C" fn(*mut c_void, Queue, *mut Buffer),
            user: *mut c_void,
            run_loop: *mut c_void,
            mode: *mut c_void,
            flags: u32,
            queue: *mut Queue,
        ) -> i32;
        fn AudioQueueAllocateBuffer(queue: Queue, size: u32, buffer: *mut *mut Buffer) -> i32;
        fn AudioQueueEnqueueBuffer(
            queue: Queue,
            buffer: *mut Buffer,
            count: u32,
            packets: *const c_void,
        ) -> i32;
        fn AudioQueueStart(queue: Queue, time: *const c_void) -> i32;
        fn AudioQueueStop(queue: Queue, immediate: u8) -> i32;
        fn AudioQueueReset(queue: Queue) -> i32;
        fn AudioQueueDispose(queue: Queue, immediate: u8) -> i32;
    }
    struct State {
        available: [AtomicBool; 3],
        played: AtomicU64,
    }
    unsafe extern "C" fn returned(user: *mut c_void, _: Queue, buffer: *mut Buffer) {
        unsafe {
            let state = &*user.cast::<State>();
            state
                .played
                .fetch_add(u64::from((*buffer).size / 4), Ordering::Relaxed);
            state.available[(*buffer).user as usize].store(true, Ordering::Release);
        }
    }
    pub struct Output {
        queue: Queue,
        buffers: [*mut Buffer; 3],
        state: Box<State>,
        started: bool,
    }
    impl Output {
        pub fn open() -> Option<Self> {
            unsafe {
                let mut output = Self {
                    queue: std::ptr::null_mut(),
                    buffers: [std::ptr::null_mut(); 3],
                    state: Box::new(State {
                        available: std::array::from_fn(|_| AtomicBool::new(true)),
                        played: AtomicU64::new(0),
                    }),
                    started: false,
                };
                let format = Format {
                    rate: 48_000.0,
                    id: u32::from_be_bytes(*b"lpcm"),
                    flags: 4 | 8,
                    packet_bytes: 4,
                    packet_frames: 1,
                    frame_bytes: 4,
                    channels: 2,
                    bits: 16,
                    reserved: 0,
                };
                if AudioQueueNewOutput(
                    &format,
                    returned,
                    (&mut *output.state as *mut State).cast(),
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    0,
                    &mut output.queue,
                ) != 0
                {
                    return None;
                }
                for (index, buffer) in output.buffers.iter_mut().enumerate() {
                    if AudioQueueAllocateBuffer(output.queue, 960 * 4, buffer) != 0 {
                        return None;
                    }
                    (**buffer).user = index as *mut c_void;
                }
                Some(output)
            }
        }
        pub fn reset(&mut self) {
            unsafe {
                AudioQueueStop(self.queue, 1);
                AudioQueueReset(self.queue);
                for slot in &self.state.available {
                    slot.store(true, Ordering::Release);
                }
                self.state.played.store(0, Ordering::Relaxed);
                self.started = false;
            }
        }
        pub fn available(&self) -> u64 {
            self.state
                .available
                .iter()
                .filter(|slot| slot.load(Ordering::Acquire))
                .count() as u64
                * 960
        }
        pub fn played(&self) -> u64 {
            self.state.played.load(Ordering::Relaxed)
        }
        pub fn push(&mut self, samples: &[i16]) -> bool {
            unsafe {
                let Some(index) = self
                    .state
                    .available
                    .iter()
                    .position(|slot| slot.swap(false, Ordering::AcqRel))
                else {
                    return false;
                };
                let buffer = self.buffers[index];
                (*buffer).size = std::mem::size_of_val(samples) as u32;
                std::ptr::copy_nonoverlapping(
                    samples.as_ptr(),
                    (*buffer).data.cast(),
                    samples.len(),
                );
                if AudioQueueEnqueueBuffer(self.queue, buffer, 0, std::ptr::null()) != 0 {
                    return false;
                }
                if !self.started {
                    if AudioQueueStart(self.queue, std::ptr::null()) != 0 {
                        return false;
                    }
                    self.started = true;
                }
                true
            }
        }
    }
    impl Drop for Output {
        fn drop(&mut self) {
            unsafe {
                if !self.queue.is_null() {
                    AudioQueueDispose(self.queue, 1);
                }
            }
        }
    }
}

#[cfg(not(any(windows, target_os = "macos")))]
mod native {
    pub struct Output;
    impl Output {
        pub fn open() -> Option<Self> {
            None
        }
        pub fn reset(&mut self) {}
        pub fn available(&self) -> u64 {
            0
        }
        pub fn played(&self) -> u64 {
            0
        }
        pub fn push(&mut self, _: &[i16]) -> bool {
            false
        }
    }
}
pub use native::Output;

#[cfg(all(test, any(windows, target_os = "macos")))]
mod tests {
    use super::Output;
    use std::time::{Duration, Instant};

    #[test]
    #[ignore = "requires a native audio output device"]
    fn native_output_plays_silence_and_resets_its_clock() {
        let mut output = Output::open().expect("native audio output device");
        for _ in 0..3 {
            assert!(output.available() >= 960 && output.available() <= 48_000);
            assert!(output.push(&[0; 960 * 2]));
            let deadline = Instant::now() + Duration::from_secs(2);
            while output.played() < 960 && Instant::now() < deadline {
                std::thread::sleep(Duration::from_millis(5));
            }
            assert_eq!(
                output.played(),
                960,
                "native clock consumes the queued silence"
            );
            output.reset();
            assert_eq!(output.played(), 0);
        }
    }
}

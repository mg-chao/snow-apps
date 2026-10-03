//! Bounded, cancellable preview. A single worker owns every decoder and audio device.
use crate::clip::{AudioReader, ClipSource, VideoReader};
use crate::error::{RecordingExportError, Result};
use snow_core::cancellation::CancellationToken;
use snow_memory::RasterBuffer;
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

mod output;

trait AudioSink {
    fn reset(&mut self);
    fn available(&self) -> u64;
    fn played(&self) -> u64;
    fn push(&mut self, samples: &[i16]) -> bool;
}
impl AudioSink for output::Output {
    fn reset(&mut self) {
        self.reset();
    }
    fn available(&self) -> u64 {
        self.available()
    }
    fn played(&self) -> u64 {
        self.played()
    }
    fn push(&mut self, samples: &[i16]) -> bool {
        self.push(samples)
    }
}

#[derive(Clone, Default)]
pub struct PreviewSnapshot {
    pub revision: u64,
    pub position_us: u64,
    pub playing: bool,
    pub pixels: Arc<RasterBuffer>,
    pub error: String,
}

struct Command {
    revision: u64,
    position_us: u64,
    end_us: u64,
    playing: bool,
    closed: bool,
    cancellation: CancellationToken,
}
struct State {
    command: Mutex<Command>,
    wake: Condvar,
    snapshot: Mutex<PreviewSnapshot>,
}
pub struct Playback {
    state: Arc<State>,
    worker: Option<JoinHandle<()>>,
}

impl Playback {
    pub fn open(source: ClipSource) -> Result<Self> {
        let epoch = Instant::now();
        let has_audio = !source.config.audio.is_empty();
        Self::open_with(
            source,
            move || epoch.elapsed(),
            move || {
                if has_audio {
                    output::Output::open().map(|output| Box::new(output) as Box<dyn AudioSink>)
                } else {
                    None
                }
            },
        )
    }

    fn open_with(
        source: ClipSource,
        clock: impl Fn() -> Duration + Send + 'static,
        output: impl FnOnce() -> Option<Box<dyn AudioSink>> + Send + 'static,
    ) -> Result<Self> {
        let state = Arc::new(State {
            command: Mutex::new(Command {
                revision: 1,
                position_us: 0,
                end_us: source.duration_us,
                playing: false,
                closed: false,
                cancellation: Default::default(),
            }),
            wake: Condvar::new(),
            snapshot: Default::default(),
        });
        let shared = Arc::clone(&state);
        let worker = std::thread::Builder::new()
            .name("snow-recording-playback".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    run(&source, &shared, &clock, output())
                }))
                .unwrap_or_else(|_| {
                    Err(RecordingExportError::Decode(
                        "preview worker panicked".into(),
                    ))
                });
                if let Err(error) = result {
                    let mut snapshot = shared.snapshot.lock().unwrap_or_else(|e| e.into_inner());
                    snapshot.error = error.to_string();
                    snapshot.playing = false;
                }
            })?;
        Ok(Self {
            state,
            worker: Some(worker),
        })
    }

    pub fn seek(&self, position_us: u64, end_us: u64, playing: bool) -> u64 {
        let mut command = self.state.command.lock().unwrap_or_else(|e| e.into_inner());
        command.cancellation.cancel();
        command.cancellation = Default::default();
        command.revision += 1;
        command.position_us = position_us;
        command.end_us = end_us;
        command.playing = playing;
        self.state.wake.notify_one();
        command.revision
    }

    pub fn snapshot(&self) -> PreviewSnapshot {
        self.state
            .snapshot
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
    }
}

impl Drop for Playback {
    fn drop(&mut self) {
        {
            let mut command = self.state.command.lock().unwrap_or_else(|e| e.into_inner());
            command.closed = true;
            command.cancellation.cancel();
            self.state.wake.notify_one();
        }
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

fn run(
    source: &ClipSource,
    state: &State,
    clock: &impl Fn() -> Duration,
    mut sink: Option<Box<dyn AudioSink>>,
) -> Result<()> {
    let mut video = VideoReader::open(source.clone(), true)?;
    if source.config.audio.is_empty() {
        sink = None;
    }
    let mut audio = if sink.is_some() {
        Some(AudioReader::open(source)?)
    } else {
        None
    };
    let mut revision = 0;
    let mut origin = 0;
    let mut end = source.duration_us;
    let mut started = clock();
    let mut audio_advanced = clock();
    let mut audio_position = 0;
    let mut playing = false;
    let mut next_audio = 0;
    let mut last_frame = None;
    let mut pixels = RasterBuffer::new();
    let mut mixed = vec![0i16; 960 * 2];
    loop {
        let command = state.command.lock().unwrap_or_else(|e| e.into_inner());
        let command = if command.revision == revision && !playing && !command.closed {
            state.wake.wait(command).unwrap_or_else(|e| e.into_inner())
        } else {
            command
        };
        if command.closed {
            return Ok(());
        }
        let token = command.cancellation.clone();
        if command.revision != revision {
            revision = command.revision;
            origin = command
                .position_us
                .min(source.duration_us.saturating_sub(1));
            end = command.end_us.min(source.duration_us).max(origin + 1);
            playing = command.playing;
            started = clock();
            audio_advanced = started;
            audio_position = 0;
            next_audio = 0;
            last_frame = None;
            if let Some(sink) = &mut sink {
                sink.reset();
            }
        }
        drop(command);
        if playing && let Some(device) = &sink {
            let position = device.played();
            if position != audio_position {
                audio_position = position;
                audio_advanced = clock();
            } else if clock().saturating_sub(audio_advanced) > Duration::from_millis(500) {
                // A disconnected or stalled device must never prevent silent playback.
                started =
                    clock().saturating_sub(Duration::from_micros(position * 1_000_000 / 48_000));
                sink = None;
            }
        }
        let elapsed = if playing {
            sink.as_ref()
                .map_or(clock().saturating_sub(started).as_micros() as u64, |sink| {
                    sink.played() * 1_000_000 / 48_000
                })
        } else {
            0
        };
        let at = origin.saturating_add(elapsed).min(end.saturating_sub(1));
        let frame = source.frame_at(at);
        let render = (|| -> Result<()> {
            if last_frame != Some(frame) {
                video.read(source.boundary(frame), &token)?;
                video.copy_rgba_buffer(&mut pixels)?;
                if last_frame.is_none() {
                    started = clock();
                    audio_advanced = started;
                }
            }
            if playing && let Some(device) = &mut sink {
                let total_audio = (end - origin) * 48_000 / 1_000_000;
                // Never queue more than the device's bounded buffer, or beyond the cut.
                while next_audio < total_audio && device.available() >= 960 {
                    let count = (total_audio - next_audio).min(960) as usize;
                    mixed[..count * 2].fill(0);
                    let tracks = audio.as_mut().expect("audio device reader").read(
                        origin * 48_000 / 1_000_000 + next_audio,
                        count,
                        &token,
                    )?;
                    for samples in tracks {
                        for (destination, sample) in mixed.iter_mut().zip(samples) {
                            *destination = destination.saturating_add(*sample);
                        }
                    }
                    if !device.push(&mixed[..count * 2]) {
                        break;
                    }
                    next_audio += count as u64;
                }
            }
            Ok(())
        })();
        if token.is_canceled() {
            continue;
        }
        render?;
        if playing && elapsed >= end - origin {
            playing = false;
        }
        // Audio duration is rounded to samples; don't wait forever for a fractional sample.
        if playing
            && sink
                .as_ref()
                .is_some_and(|sink| sink.played() >= (end - origin) * 48_000 / 1_000_000)
        {
            playing = false;
        }
        let command = state.command.lock().unwrap_or_else(|e| e.into_inner());
        if command.revision == revision {
            let mut snapshot = state.snapshot.lock().unwrap_or_else(|e| e.into_inner());
            snapshot.revision = revision;
            snapshot.position_us = if playing {
                at
            } else if command.playing {
                end
            } else {
                origin
            };
            snapshot.playing = playing;
            if last_frame != Some(frame) {
                // Swap an unshared retired buffer back into the decoder when possible.
                let retired =
                    std::mem::replace(&mut snapshot.pixels, Arc::new(std::mem::take(&mut pixels)));
                pixels = Arc::try_unwrap(retired).unwrap_or_default();
            }
            last_frame = Some(frame);
        }
        if playing {
            let _ = state
                .wake
                .wait_timeout(command, Duration::from_millis(8))
                .unwrap_or_else(|e| e.into_inner());
        }
    }
}

#[derive(Clone, Default)]
pub struct ClipExportProgress {
    pub state: u32,
    pub percent: f32,
    pub error: String,
}
pub struct ClipExportTask {
    token: CancellationToken,
    progress: Arc<Mutex<ClipExportProgress>>,
    worker: Option<JoinHandle<()>>,
}
impl ClipExportTask {
    pub fn start(
        source: ClipSource,
        range: snow_recording_model::TrimRange,
        path: std::path::PathBuf,
    ) -> Result<Self> {
        let token = CancellationToken::default();
        let worker_token = token.clone();
        let progress = Arc::new(Mutex::new(ClipExportProgress::default()));
        let shared = Arc::clone(&progress);
        let worker = std::thread::Builder::new()
            .name("snow-recording-trim-export".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    source.export(range, &path, &worker_token, |percent| {
                        shared.lock().unwrap_or_else(|e| e.into_inner()).percent = percent;
                    })
                }))
                .unwrap_or_else(|_| {
                    Err(RecordingExportError::Export("trim worker panicked".into()))
                });
                let mut progress = shared.lock().unwrap_or_else(|e| e.into_inner());
                match result {
                    Ok(()) => {
                        progress.state = 1;
                        progress.percent = 100.0;
                    }
                    Err(RecordingExportError::ExportCanceled) => progress.state = 2,
                    Err(error) => {
                        progress.state = 3;
                        progress.error = error.to_string();
                    }
                }
            })?;
        Ok(Self {
            token,
            progress,
            worker: Some(worker),
        })
    }
    pub fn cancel(&self) {
        self.token.cancel();
    }
    pub fn snapshot(&self) -> ClipExportProgress {
        self.progress
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
    }
}
impl Drop for ClipExportTask {
    fn drop(&mut self) {
        self.cancel();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoder,
        StreamingEncoderConfig,
    };
    use snow_recording_model::{VideoCodec, VideoEncodeConfig, VideoEncodingSpeed};
    use std::sync::atomic::{AtomicU64, Ordering};

    fn fixture() -> (tempfile::TempDir, ClipSource) {
        let directory = tempfile::tempdir().unwrap();
        let config = StreamingEncoderConfig {
            output_path: directory.path().join("preview.mp4"),
            format: ExportFormat::Mp4,
            width: 32,
            height: 24,
            fps: 10,
            codec: VideoCodec::H264,
            prefer_hardware_h264: false,
            execution_mode: ExportExecutionMode::SoftwareOnly,
            software_h264_priority: SoftwareH264Priority::X264First,
            video: VideoEncodeConfig {
                quality: 90,
                speed: VideoEncodingSpeed::UltraFast,
            },
            encode_threads: 1,
            audio: Vec::new(),
            loop_animated_images: false,
        };
        let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
        for index in 0..10 {
            encoder
                .push_rgba_frame_at_pts(
                    index,
                    &[20 + index as u8 * 20, 50, 100, 255].repeat(32 * 24),
                )
                .unwrap();
        }
        encoder.finish_at_duration_ms(1000).unwrap();
        let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
        (directory, source)
    }
    fn wait(playback: &Playback, predicate: impl Fn(&PreviewSnapshot) -> bool) -> PreviewSnapshot {
        let deadline = Instant::now();
        loop {
            let snapshot = playback.snapshot();
            assert!(snapshot.error.is_empty(), "{}", snapshot.error);
            if predicate(&snapshot) {
                return snapshot;
            }
            assert!(
                deadline.elapsed() < Duration::from_secs(5),
                "preview did not reach the requested deterministic state"
            );
            playback.state.wake.notify_one();
            std::thread::yield_now();
        }
    }
    #[test]
    fn playback_autoplays_once_replays_the_range_and_rejects_stale_seeks() {
        let (_directory, source) = fixture();
        let clock = Arc::new(AtomicU64::new(0));
        let worker_clock = clock.clone();
        let mut expected = VideoReader::open(source.clone(), false).unwrap();
        expected
            .read(600_000, &CancellationToken::default())
            .unwrap();
        let mut last_included = Vec::new();
        expected.copy_rgba(&mut last_included).unwrap();
        let playback = Playback::open_with(
            source,
            move || Duration::from_micros(worker_clock.load(Ordering::SeqCst)),
            || None,
        )
        .unwrap();
        let revision = playback.seek(200_000, 700_000, true);
        wait(&playback, |s| s.revision == revision && s.playing);
        clock.store(600_000, Ordering::SeqCst);
        let ended = wait(&playback, |s| s.revision == revision && !s.playing);
        assert_eq!(ended.position_us, 700_000);
        assert_eq!(
            *ended.pixels, last_included,
            "playback must retain the final included picture, not the end boundary"
        );
        clock.store(5_000_000, Ordering::SeqCst);
        assert_eq!(playback.snapshot().revision, revision);
        assert!(!playback.snapshot().playing);
        let replay = playback.seek(200_000, 700_000, true);
        wait(&playback, |s| {
            s.revision == replay && s.playing && s.position_us == 200_000
        });
        let mut last = replay;
        for index in 0..100 {
            last = playback.seek((index % 6) * 100_000, 700_000, false);
        }
        let scrub = wait(&playback, |s| s.revision == last);
        assert_eq!(scrub.position_us, 300_000);
        assert!(!scrub.playing);
        // Teardown races a fresh request, while an independently held frame stays valid.
        playback.seek(0, 1_000_000, true);
        drop(playback);
        assert_eq!(scrub.pixels.len(), ended.pixels.len());
    }

    #[derive(Default)]
    struct SinkState {
        written: AtomicU64,
        played: AtomicU64,
        largest_queue: AtomicU64,
        dropped: std::sync::atomic::AtomicBool,
    }
    struct FakeSink(Arc<SinkState>);
    impl Drop for FakeSink {
        fn drop(&mut self) {
            self.0.dropped.store(true, Ordering::SeqCst);
        }
    }
    impl AudioSink for FakeSink {
        fn reset(&mut self) {
            self.0.written.store(0, Ordering::SeqCst);
            self.0.played.store(0, Ordering::SeqCst);
        }
        fn available(&self) -> u64 {
            1920u64.saturating_sub(
                self.0
                    .written
                    .load(Ordering::SeqCst)
                    .saturating_sub(self.played()),
            )
        }
        fn played(&self) -> u64 {
            self.0.played.load(Ordering::SeqCst)
        }
        fn push(&mut self, samples: &[i16]) -> bool {
            let frames = samples.len() as u64 / 2;
            assert!(frames <= self.available());
            let written = self.0.written.fetch_add(frames, Ordering::SeqCst) + frames;
            self.0
                .largest_queue
                .fetch_max(written - self.played(), Ordering::SeqCst);
            true
        }
    }
    #[test]
    fn playback_audio_is_bounded_and_device_loss_falls_back_to_a_silent_clock() {
        let (_directory, mut source) = fixture();
        source.config.audio.push(Default::default());
        let clock = Arc::new(AtomicU64::new(0));
        let worker_clock = clock.clone();
        let audio = Arc::new(SinkState::default());
        let worker_audio = audio.clone();
        let playback = Playback::open_with(
            source,
            move || Duration::from_micros(worker_clock.load(Ordering::SeqCst)),
            move || Some(Box::new(FakeSink(worker_audio))),
        )
        .unwrap();
        let revision = playback.seek(100_000, 500_000, true);
        wait(&playback, |s| s.revision == revision && s.playing);
        assert_eq!(audio.written.load(Ordering::SeqCst), 1920);
        assert!(audio.largest_queue.load(Ordering::SeqCst) <= 1920);
        audio.played.store(960, Ordering::SeqCst);
        wait(&playback, |s| s.position_us == 120_000);
        clock.store(600_000, Ordering::SeqCst);
        // Let the worker observe the stalled device and switch clocks before advancing time.
        wait(&playback, |_| audio.dropped.load(Ordering::SeqCst));
        clock.store(1_200_000, Ordering::SeqCst);
        wait(&playback, |s| !s.playing && s.position_us == 500_000);
    }
}

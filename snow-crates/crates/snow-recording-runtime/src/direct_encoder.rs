//! A single owner for encoding, muxing and audio; optional bounded video overlap.
use super::*;

struct VideoWork {
    pts: u64,
    pixels: VideoBuffer,
    #[cfg(feature = "bench-pipeline-timing")]
    admitted: Instant,
}

#[cfg(any(test, feature = "bench-synthetic-input"))]
#[derive(Default)]
struct Mailbox {
    video: Option<VideoWork>,
    replacements: u64,
    #[cfg(test)]
    before_video: Option<(Sender<()>, Receiver<()>)>,
}

#[cfg(test)]
fn wait_for_video_gate(mailbox: &Mutex<Mailbox>) {
    let gate = mailbox.lock().unwrap().before_video.take();
    if let Some((entered, release)) = gate {
        entered.send(()).unwrap();
        release.recv().unwrap();
    }
}

#[cfg(any(test, feature = "bench-synthetic-input"))]
impl Mailbox {
    fn replace(&mut self, video: VideoWork) -> Option<VideoBuffer> {
        self.video.replace(video).map(|previous| {
            self.replacements += 1;
            previous.pixels
        })
    }
}

#[cfg(any(test, feature = "bench-synthetic-input"))]
enum Command {
    Pause(Sender<()>),
    Resume(Sender<()>),
    Finish(u64),
    Cancel,
}

struct OwnedEncoder {
    encoder: StreamingEncoder,
    clock: RecordingClock,
    audio: Option<AudioStreamHandle>,
    mixer: Option<LiveAudioMixer>,
    paused: bool,
    pending_history: Option<damage::History>,
}

impl OwnedEncoder {
    fn new(
        config: StreamingEncoderBuilder,
        audio: Option<AudioStreamHandle>,
        selection: (bool, bool),
        clock: RecordingClock,
        bench: (u8, bool),
    ) -> Result<Self> {
        #[cfg(feature = "bench-synthetic-input")]
        let encoder = config
            .force_hardware_failure(bench.1)
            .create()
            .and_then(|mut encoder| {
                encoder.set_conversion_threads(bench.0)?;
                Ok(encoder)
            })?;
        #[cfg(not(feature = "bench-synthetic-input"))]
        let encoder = {
            let _ = bench;
            config.create()?
        };
        let mixer = encoder
            .has_audio()
            .then(|| LiveAudioMixer::new(selection.0, selection.1));
        Ok(Self {
            encoder,
            clock,
            audio,
            mixer,
            paused: false,
            pending_history: None,
        })
    }

    fn tick(&mut self) -> Result<()> {
        drain_audio_events(
            self.audio.as_ref(),
            &self.clock,
            self.paused,
            self.mixer.as_mut(),
        );
        if !self.paused
            && let Some(mixer) = self.mixer.as_mut()
        {
            mixer.emit_ready(
                self.clock.active_elapsed_duration(Instant::now()),
                false,
                &mut self.encoder,
            )?;
        }
        Ok(())
    }

    fn pause(&mut self) {
        if let Some(audio) = &self.audio {
            audio.pause();
        }
        if let Some(mixer) = &mut self.mixer {
            mixer.reset_alignment(None);
        }
        self.paused = true;
    }

    fn resume(&mut self) {
        if let Some(audio) = &self.audio {
            audio.resume();
        }
        self.paused = false;
    }

    fn video(&mut self, frame: VideoWork) -> Result<VideoBuffer> {
        #[cfg(feature = "bench-pipeline-timing")]
        self.encoder
            .record_worker_queue_time(frame.admitted.elapsed());
        let pixels = self
            .encoder
            .push_owned_rgba_frame_at_pts(frame.pts, frame.pixels.pixels)?;
        let history = std::mem::replace(&mut self.pending_history, frame.pixels.history);
        Ok(VideoBuffer {
            history: (!pixels.is_empty()).then_some(history).flatten(),
            pixels,
        })
    }

    fn finish(mut self, endpoint: u64) -> Result<StreamingEncoderReport> {
        let mut dropped_audio = 0;
        if let Some(audio) = self.audio.take() {
            let stats = Arc::clone(audio.stats());
            for event in audio.stop_and_drain() {
                process_audio_event(event, &self.clock, false, self.mixer.as_mut());
            }
            dropped_audio = stats.snapshot().frames_dropped;
        }
        if let Some(mixer) = self.mixer.as_mut() {
            mixer.emit_ready(
                self.clock.active_elapsed_duration(Instant::now()),
                true,
                &mut self.encoder,
            )?;
            dropped_audio += mixer.dropped_frames;
        }
        let mut report = self.encoder.finish_at_pts(endpoint)?;
        report.dropped_audio_frames = report.dropped_audio_frames.saturating_add(dropped_audio);
        Ok(report)
    }
}

#[cfg(any(test, feature = "bench-synthetic-input"))]
struct ThreadedEncoder {
    video_encoder: String,
    hardware: bool,
    mailbox: Arc<Mutex<Mailbox>>,
    commands: Sender<Command>,
    wake: Sender<()>,
    recycled: Receiver<VideoBuffer>,
    stopped: Receiver<()>,
    failure: Arc<Mutex<Option<String>>>,
    worker: JoinHandle<Result<StreamingEncoderReport>>,
}

#[cfg(any(test, feature = "bench-synthetic-input"))]
impl ThreadedEncoder {
    fn acknowledge(&self, acknowledgment: &Receiver<()>) -> Result<()> {
        // A disconnected command receiver may leave queued commands alive
        // while this owner still holds its sender. Their embedded ack senders
        // therefore cannot act as worker-lifetime signals.
        crossbeam_channel::select! {
            recv(acknowledgment) -> result => result.map_err(|_| self.error()),
            recv(self.stopped) -> _ => Err(self.error()),
        }
    }
    fn error(&self) -> ScreenRecorderError {
        ScreenRecorderError::Encode(
            self.failure
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .clone()
                .unwrap_or_else(|| "recording encoder worker stopped unexpectedly".into()),
        )
    }
    fn check(&self) -> Result<()> {
        if self.worker.is_finished() {
            Err(self.error())
        } else {
            Ok(())
        }
    }
    fn command(&self, command: Command) -> Result<()> {
        self.commands.send(command).map_err(|_| self.error())?;
        let _ = self.wake.try_send(());
        Ok(())
    }
}

enum Driver {
    Inline(Box<OwnedEncoder>),
    #[cfg(any(test, feature = "bench-synthetic-input"))]
    Threaded(ThreadedEncoder),
}

pub(super) struct RecordingEncoder {
    driver: Option<Driver>,
}

impl RecordingEncoder {
    pub(super) fn new(
        config: impl Into<StreamingEncoderBuilder>,
        audio: Option<AudioStreamHandle>,
        selection: (bool, bool),
        clock: RecordingClock,
        asynchronous: bool,
        bench: (u8, bool),
    ) -> Result<Self> {
        let config = config.into();
        #[cfg(any(test, feature = "bench-synthetic-input"))]
        if asynchronous {
            return Self::new_threaded(config, audio, selection, clock, bench);
        }
        #[cfg(not(any(test, feature = "bench-synthetic-input")))]
        let _ = asynchronous;
        Ok(Self {
            driver: Some(Driver::Inline(Box::new(OwnedEncoder::new(
                config, audio, selection, clock, bench,
            )?))),
        })
    }

    #[cfg(any(test, feature = "bench-synthetic-input"))]
    fn new_threaded(
        config: StreamingEncoderBuilder,
        audio: Option<AudioStreamHandle>,
        selection: (bool, bool),
        clock: RecordingClock,
        bench: (u8, bool),
    ) -> Result<Self> {
        let mailbox = Arc::new(Mutex::new(Mailbox::default()));
        let failure = Arc::new(Mutex::new(None));
        let (commands, control) = crossbeam_channel::bounded(1);
        let (wake, wake_rx) = crossbeam_channel::bounded(1);
        let (recycle, recycled) = crossbeam_channel::bounded(2);
        let (ready, started) = crossbeam_channel::bounded(1);
        let (lifetime, stopped) = crossbeam_channel::bounded::<()>(0);
        let worker_mailbox = Arc::clone(&mailbox);
        let worker_failure = Arc::clone(&failure);
        let worker = std::thread::Builder::new()
            .name("snow-direct-encoder".into())
            .spawn(move || {
                // Drop on success, failure or unwind to wake every control waiter.
                let _lifetime = lifetime;
                let result = (|| {
                    let owned = OwnedEncoder::new(config, audio, selection, clock, bench);
                    match owned {
                        Ok(mut owned) => {
                            let (name, hardware) = owned.encoder.opened_video_encoder();
                            let _ = ready.send(Ok((name.to_owned(), hardware)));
                            loop {
                                if let Ok(command) = control.try_recv() {
                                    if matches!(command, Command::Cancel) {
                                        return Err(ScreenRecorderError::ExportCanceled);
                                    }
                                    // Complete the accepted pre-boundary frame before acknowledging
                                    // pause or finalization. New video cannot be enqueued during an ack.
                                    let accepted = worker_mailbox
                                        .lock()
                                        .unwrap_or_else(|e| e.into_inner())
                                        .video
                                        .take();
                                    if let Some(frame) = accepted {
                                        #[cfg(test)]
                                        wait_for_video_gate(&worker_mailbox);
                                        let _ = recycle.try_send(owned.video(frame)?);
                                    }
                                    match command {
                                        Command::Pause(ack) => {
                                            owned.pause();
                                            let _ = ack.send(());
                                        }
                                        Command::Resume(ack) => {
                                            owned.resume();
                                            let _ = ack.send(());
                                        }
                                        Command::Finish(endpoint) => {
                                            let mut report = owned.finish(endpoint)?;
                                            report.queued_video_replacements = worker_mailbox
                                                .lock()
                                                .unwrap_or_else(|e| e.into_inner())
                                                .replacements;
                                            return Ok(report);
                                        }
                                        Command::Cancel => unreachable!(),
                                    }
                                }
                                owned.tick()?;
                                let video = worker_mailbox
                                    .lock()
                                    .unwrap_or_else(|e| e.into_inner())
                                    .video
                                    .take();
                                if let Some(frame) = video {
                                    #[cfg(test)]
                                    wait_for_video_gate(&worker_mailbox);
                                    let _ = recycle.try_send(owned.video(frame)?);
                                } else {
                                    match wake_rx.recv_timeout(Duration::from_millis(10)) {
                                        Ok(())
                                        | Err(crossbeam_channel::RecvTimeoutError::Timeout) => {}
                                        Err(crossbeam_channel::RecvTimeoutError::Disconnected) => {
                                            return Err(ScreenRecorderError::ExportCanceled);
                                        }
                                    }
                                }
                            }
                        }
                        Err(error) => {
                            let _ = ready.send(Err(error.to_string()));
                            Err(error)
                        }
                    }
                })();
                if let Err(error) = &result {
                    *worker_failure.lock().unwrap_or_else(|e| e.into_inner()) =
                        Some(error.to_string());
                }
                result
            })
            .map_err(ScreenRecorderError::Io)?;
        match started.recv() {
            Ok(Ok((video_encoder, hardware))) => Ok(Self {
                driver: Some(Driver::Threaded(ThreadedEncoder {
                    video_encoder,
                    hardware,
                    mailbox,
                    commands,
                    wake,
                    recycled,
                    stopped,
                    failure,
                    worker,
                })),
            }),
            result => {
                let _ = worker.join();
                Err(ScreenRecorderError::Encode(match result {
                    Ok(Err(message)) => message,
                    _ => "encoder initialization worker stopped".into(),
                }))
            }
        }
    }

    pub(super) fn opened_video_encoder(&self) -> (&str, bool) {
        match self.driver.as_ref().expect("encoder driver") {
            Driver::Inline(owned) => owned.encoder.opened_video_encoder(),
            #[cfg(any(test, feature = "bench-synthetic-input"))]
            Driver::Threaded(threaded) => (&threaded.video_encoder, threaded.hardware),
        }
    }

    pub(super) fn tick(&mut self) -> Result<()> {
        match self.driver.as_mut().expect("encoder driver") {
            Driver::Inline(owned) => owned.tick(),
            #[cfg(any(test, feature = "bench-synthetic-input"))]
            Driver::Threaded(threaded) => threaded.check(),
        }
    }

    pub(super) fn pause(&mut self) -> Result<()> {
        self.set_paused(true)
    }
    pub(super) fn resume(&mut self) -> Result<()> {
        self.set_paused(false)
    }
    fn set_paused(&mut self, paused: bool) -> Result<()> {
        match self.driver.as_mut().expect("encoder driver") {
            Driver::Inline(owned) => {
                if paused {
                    owned.pause();
                } else {
                    owned.resume();
                }
                Ok(())
            }
            #[cfg(any(test, feature = "bench-synthetic-input"))]
            Driver::Threaded(threaded) => {
                let (ack, receiver) = crossbeam_channel::bounded(1);
                threaded.command(if paused {
                    Command::Pause(ack)
                } else {
                    Command::Resume(ack)
                })?;
                threaded.acknowledge(&receiver)
            }
        }
    }

    pub(super) fn push_owned_rgba_frame_at_pts(
        &mut self,
        pts: u64,
        pixels: VideoBuffer,
    ) -> Result<VideoBuffer> {
        let frame = VideoWork {
            pts,
            pixels,
            #[cfg(feature = "bench-pipeline-timing")]
            admitted: Instant::now(),
        };
        match self.driver.as_mut().expect("encoder driver") {
            Driver::Inline(owned) => owned.video(frame),
            #[cfg(any(test, feature = "bench-synthetic-input"))]
            Driver::Threaded(threaded) => {
                threaded.check()?;
                let previous = threaded
                    .mailbox
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .replace(frame);
                let _ = threaded.wake.try_send(());
                Ok(previous
                    .or_else(|| threaded.recycled.try_recv().ok())
                    .unwrap_or_default())
            }
        }
    }

    pub(super) fn finish_at_pts(mut self, endpoint: u64) -> Result<StreamingEncoderReport> {
        match self.driver.take().expect("encoder driver") {
            Driver::Inline(owned) => owned.finish(endpoint),
            #[cfg(any(test, feature = "bench-synthetic-input"))]
            Driver::Threaded(threaded) => {
                // Join even if the channel closed after a worker failure.
                let _ = threaded.command(Command::Finish(endpoint));
                threaded.worker.join().map_err(|_| {
                    ScreenRecorderError::Encode("recording encoder worker panicked".into())
                })?
            }
        }
    }
}

#[cfg(any(test, feature = "bench-synthetic-input"))]
impl Drop for RecordingEncoder {
    fn drop(&mut self) {
        if let Some(Driver::Threaded(threaded)) = self.driver.take() {
            let _ = threaded.command(Command::Cancel);
            let _ = threaded.worker.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config(path: PathBuf) -> StreamingEncoderConfig {
        StreamingEncoderConfig {
            output_path: path,
            format: ExportFormat::Mp4,
            width: 16,
            height: 16,
            fps: 10,
            codec: VideoCodec::H264,
            prefer_hardware_h264: false,
            execution_mode: ExportExecutionMode::SoftwareOnly,
            software_h264_priority: SoftwareH264Priority::X264First,
            video: VideoEncodeConfig {
                quality: 80,
                speed: VideoEncodingSpeed::VeryFast,
            },
            encode_threads: 1,
            audio: None,
        }
    }

    #[test]
    fn worker_preserves_accepted_pts_and_durations_across_control_acknowledgments() {
        let directory = tempfile::tempdir().unwrap();
        let mut outputs = Vec::new();
        for asynchronous in [false, true] {
            let path = directory.path().join(format!("{asynchronous}.mp4"));
            let mut encoder = RecordingEncoder::new(
                config(path.clone()),
                None,
                (false, false),
                RecordingClock::new(Instant::now()),
                asynchronous,
                (0, false),
            )
            .unwrap();
            for pts in [0, 2, 6, 9] {
                encoder
                    .push_owned_rgba_frame_at_pts(pts, vec![pts as u8 * 20; 16 * 16 * 4].into())
                    .unwrap();
                // Acknowledgment drains previously accepted video; deterministic
                // ordering without sleeps or assumptions about thread scheduling.
                encoder.pause().unwrap();
                encoder.resume().unwrap();
            }
            assert_eq!(encoder.finish_at_pts(10).unwrap().encoded_frames, 4);
            let mut media = ffmpeg_next::format::input(&path).unwrap();
            let packets: Vec<_> = media
                .packets()
                .map(|(_, packet)| (packet.pts(), packet.duration()))
                .collect();
            assert_eq!(packets.len(), 4);
            outputs.push(packets);
        }
        assert_eq!(outputs[0], outputs[1]);
    }

    #[test]
    fn worker_preserves_encoded_pixels_for_common_edge_and_text_sequence() {
        let directory = tempfile::tempdir().unwrap();
        let mut outputs = Vec::new();
        for asynchronous in [false, true] {
            let path = directory.path().join(format!("quality-{asynchronous}.mp4"));
            let mut settings = config(path.clone());
            settings.width = 128;
            settings.height = 96;
            let mut encoder = RecordingEncoder::new(
                settings,
                None,
                (false, false),
                RecordingClock::new(Instant::now()),
                asynchronous,
                (0, false),
            )
            .unwrap();
            for pts in 0..12 {
                assert_eq!(encoder.opened_video_encoder(), ("libx264", false));
                let mut pixels = vec![0; 128 * 96 * 4];
                for y in 0..96 {
                    for x in 0..128 {
                        // One-pixel edges, moving color boundaries and repeated
                        // bitmap E glyphs expose changes hidden by flat fixtures.
                        let glyph_x = x % 8;
                        let glyph_y = y % 12;
                        let glyph = glyph_x == 1 || (glyph_x < 6 && [1, 5, 9].contains(&glyph_y));
                        let color = if y < 48 {
                            if glyph {
                                [255, 255, 255, 255]
                            } else {
                                [0, 0, 0, 255]
                            }
                        } else {
                            [
                                (x + pts as usize * 7) as u8,
                                (y * 2) as u8,
                                if (x + y) % 2 == 0 { 255 } else { 0 },
                                255,
                            ]
                        };
                        pixels[(y * 128 + x) * 4..(y * 128 + x + 1) * 4].copy_from_slice(&color);
                    }
                }
                encoder
                    .push_owned_rgba_frame_at_pts(pts, pixels.into())
                    .unwrap();
                // Compare an identical admitted sequence, independently of the
                // mailbox's separately tested replacement policy.
                encoder.pause().unwrap();
                encoder.resume().unwrap();
            }
            assert_eq!(encoder.finish_at_pts(12).unwrap().encoded_frames, 12);
            let mut media = ffmpeg_next::format::input(&path).unwrap();
            let packets: Vec<_> = media
                .packets()
                .map(|(_, packet)| {
                    (
                        packet.pts(),
                        packet.duration(),
                        packet.data().unwrap().to_vec(),
                    )
                })
                .collect();
            assert_eq!(packets.len(), 12);
            outputs.push(packets);
        }
        // Identical H.264 packets imply identical decoded pixels, a stronger
        // condition than a PSNR/SSIM tolerance for this execution-only change.
        assert_eq!(outputs[0], outputs[1]);
    }

    #[test]
    fn worker_initialization_failure_and_invalid_frame_do_not_publish_output() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("failed.mp4");
        let mut invalid = config(path.clone());
        invalid.fps = 0;
        assert!(
            RecordingEncoder::new(
                invalid,
                None,
                (false, false),
                RecordingClock::new(Instant::now()),
                true,
                (0, false)
            )
            .is_err()
        );
        let mut encoder = RecordingEncoder::new(
            config(path.clone()),
            None,
            (false, false),
            RecordingClock::new(Instant::now()),
            true,
            (0, false),
        )
        .unwrap();
        let _ = encoder.push_owned_rgba_frame_at_pts(0, vec![0; 4].into());
        assert!(encoder.pause().is_err());
        assert!(encoder.finish_at_pts(1).is_err());
        assert!(!path.exists());
        assert_eq!(std::fs::read_dir(directory.path()).unwrap().count(), 0);
    }

    #[test]
    fn dropping_worker_cancels_and_removes_staging_output() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("cancel.mp4");
        let mut encoder = RecordingEncoder::new(
            config(path.clone()),
            None,
            (false, false),
            RecordingClock::new(Instant::now()),
            true,
            (0, false),
        )
        .unwrap();
        encoder
            .push_owned_rgba_frame_at_pts(0, vec![0; 16 * 16 * 4].into())
            .unwrap();
        drop(encoder);
        assert!(!path.exists());
        assert_eq!(std::fs::read_dir(directory.path()).unwrap().count(), 0);
    }

    #[test]
    fn slow_worker_replaces_only_waiting_frames_and_preserves_storage_history() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("slow.mp4");
        let mut encoder = RecordingEncoder::new(
            config(path),
            None,
            (false, false),
            RecordingClock::new(Instant::now()),
            true,
            (0, false),
        )
        .unwrap();
        let (entered, observed) = crossbeam_channel::bounded(1);
        let (release, proceed) = crossbeam_channel::bounded(1);
        let Driver::Threaded(worker) = encoder.driver.as_ref().unwrap() else {
            panic!("worker");
        };
        worker.mailbox.lock().unwrap().before_video = Some((entered, proceed));
        let buffer = |value| VideoBuffer {
            pixels: vec![value as u8; 16 * 16 * 4],
            history: Some(History {
                generation: value,
                overlays: Rows::new((16, 16)),
            }),
        };
        encoder.push_owned_rgba_frame_at_pts(0, buffer(10)).unwrap();
        observed.recv_timeout(Duration::from_secs(5)).unwrap();
        assert!(
            encoder
                .push_owned_rgba_frame_at_pts(1, buffer(20))
                .unwrap()
                .pixels
                .is_empty()
        );
        for (pts, expected, replacement) in [(2, 20, 30), (5, 30, 40)] {
            let recycled = encoder
                .push_owned_rgba_frame_at_pts(pts, buffer(replacement))
                .unwrap();
            assert_eq!(recycled.pixels, vec![expected as u8; 16 * 16 * 4]);
            assert_eq!(recycled.history.unwrap().generation, expected);
        }
        let Driver::Threaded(worker) = encoder.driver.as_ref().unwrap() else {
            panic!("worker");
        };
        assert_eq!(
            worker.mailbox.lock().unwrap().video.as_ref().unwrap().pts,
            5
        );
        release.send(()).unwrap();
        encoder.pause().unwrap();
        let report = encoder.finish_at_pts(10).unwrap();
        assert_eq!(report.encoded_frames, 2);
        assert_eq!(report.queued_video_replacements, 2);
    }

    #[test]
    fn cancellation_does_not_drain_waiting_video_after_current_work_finishes() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("cancel-slow.mp4");
        let mut encoder = RecordingEncoder::new(
            config(path.clone()),
            None,
            (false, false),
            RecordingClock::new(Instant::now()),
            true,
            (0, false),
        )
        .unwrap();
        let (entered, observed) = crossbeam_channel::bounded(1);
        let (release, proceed) = crossbeam_channel::bounded(1);
        let Driver::Threaded(worker) = encoder.driver.as_ref().unwrap() else {
            panic!("worker");
        };
        worker.mailbox.lock().unwrap().before_video = Some((entered, proceed));
        encoder
            .push_owned_rgba_frame_at_pts(0, vec![0; 16 * 16 * 4].into())
            .unwrap();
        observed.recv_timeout(Duration::from_secs(5)).unwrap();
        // Invalid waiting storage must never reach FFmpeg after cancellation.
        encoder
            .push_owned_rgba_frame_at_pts(1, vec![0; 4].into())
            .unwrap();
        let Driver::Threaded(worker) = encoder.driver.as_ref().unwrap() else {
            panic!("worker");
        };
        worker.command(Command::Cancel).unwrap();
        release.send(()).unwrap();
        assert!(matches!(
            encoder.finish_at_pts(2),
            Err(ScreenRecorderError::ExportCanceled)
        ));
        assert!(!path.exists());
        assert_eq!(std::fs::read_dir(directory.path()).unwrap().count(), 0);
    }

    #[test]
    fn worker_failure_wakes_acknowledgment_even_when_queued_command_retains_its_sender() {
        let directory = tempfile::tempdir().unwrap();
        let mut encoder = RecordingEncoder::new(
            config(directory.path().join("ack-failure.mp4")),
            None,
            (false, false),
            RecordingClock::new(Instant::now()),
            true,
            (0, false),
        )
        .unwrap();
        let (entered, observed) = crossbeam_channel::bounded(1);
        let (release, proceed) = crossbeam_channel::bounded(1);
        let Driver::Threaded(worker) = encoder.driver.as_ref().unwrap() else {
            panic!("worker");
        };
        worker.mailbox.lock().unwrap().before_video = Some((entered, proceed));
        encoder
            .push_owned_rgba_frame_at_pts(0, vec![0; 4].into())
            .unwrap();
        observed.recv_timeout(Duration::from_secs(5)).unwrap();
        let Driver::Threaded(worker) = encoder.driver.as_ref().unwrap() else {
            panic!("worker");
        };
        let (ack, acknowledgment) = crossbeam_channel::bounded(1);
        worker.command(Command::Pause(ack)).unwrap();
        release.send(()).unwrap();
        assert!(matches!(
            worker.stopped.recv_timeout(Duration::from_secs(5)),
            Err(crossbeam_channel::RecvTimeoutError::Disconnected)
        ));
        assert!(worker.acknowledge(&acknowledgment).is_err());
        assert!(encoder.finish_at_pts(1).is_err());
        assert_eq!(std::fs::read_dir(directory.path()).unwrap().count(), 0);
    }

    #[test]
    fn latest_video_replaces_only_waiting_storage() {
        let mut mailbox = Mailbox::default();
        let frame = |pts| VideoWork {
            pts,
            pixels: vec![pts as u8; 64].into(),
            #[cfg(feature = "bench-pipeline-timing")]
            admitted: Instant::now(),
        };
        assert!(mailbox.replace(frame(0)).is_none());
        let processing = mailbox.video.take().unwrap();
        assert!(mailbox.replace(frame(1)).is_none());
        assert_eq!(mailbox.replace(frame(2)).unwrap().pixels, vec![1; 64]);
        assert_eq!(processing.pts, 0);
        assert_eq!(mailbox.video.as_ref().unwrap().pts, 2);
        assert_eq!(mailbox.replacements, 1);
    }
}

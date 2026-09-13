//! Independent codec instances share an active timeline, never H.264 parameters.
use super::*;
use ffmpeg::Rescale;

fn error(value: impl std::fmt::Display) -> RecordingExportError {
    RecordingExportError::Encode(format!("recording recovery: {value}"))
}

struct Segment {
    path: PathBuf,
    start: i64,
    packets: u64,
}

pub(super) struct RecoveryState {
    config: StreamingEncoderConfig,
    directory: PathBuf,
    segments: Vec<Segment>,
    pub(super) current_start: Option<i64>,
    audio_path: Option<PathBuf>,
    preserve: bool,
    recovered: bool,
    previous_frames: u64,
}

impl Drop for RecoveryState {
    fn drop(&mut self) {
        if !self.preserve {
            // This exact directory was created with create_dir for this session.
            // No externally supplied path is recursively removed.
            let _ = fs::remove_dir_all(&self.directory);
        }
    }
}

pub(super) fn create(builder: StreamingEncoderBuilder) -> Result<StreamingEncoder> {
    ensure_ffmpeg_initialized()?;
    builder
        .config
        .validate()
        .map_err(RecordingExportError::InvalidConfig)?;
    if builder.config.format != ExportFormat::Mp4 || builder.config.codec != VideoCodec::H264 {
        return Err(error("segmented recovery requires H.264 MP4"));
    }
    let parent = builder
        .config
        .output_path
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(parent)?;
    let directory = parent.join(format!(
        "{DIRECT_STAGING_PREFIX}{}.recovery",
        Uuid::new_v4().simple()
    ));
    fs::create_dir(&directory)?;
    let mut recovery = RecoveryState {
        config: builder.config.clone(),
        directory,
        segments: Vec::new(),
        audio_path: None,
        preserve: false,
        recovered: false,
        previous_frames: 0,
        current_start: None,
    };
    let mut config = builder.config;
    config.audio = None;
    config.output_path = recovery.directory.join("video-0.mp4");
    let mut encoder = segment(
        config,
        builder.software_fallback_threads,
        #[cfg(windows)]
        builder.gpu_input,
    )?;
    let audio_path = recovery.directory.join("audio.mp4");
    if recovery.config.audio.is_some() {
        let mut output = ffmpeg::format::output(&audio_path).map_err(error)?;
        let global_header = output
            .format()
            .flags()
            .contains(ffmpeg::format::Flags::GLOBAL_HEADER);
        let mut audio = create_audio_state(
            &mut output,
            &audio_path,
            ExportFormat::Mp4,
            global_header,
            recovery.config.audio.as_ref(),
            recovery.config.encode_threads,
        )?;
        output.write_header().map_err(error)?;
        if let Some(state) = audio.as_mut() {
            state.stream_time_base = output
                .stream(state.stream_index)
                .ok_or_else(|| error("missing audio stream"))?
                .time_base();
        }
        encoder.audio = audio;
        encoder.audio_output = Some(output);
        encoder.report.audio_encoder = Some("aac".into());
        recovery.audio_path = Some(audio_path);
    }
    encoder.recovery = Some(recovery);
    Ok(encoder)
}

fn segment(
    config: StreamingEncoderConfig,
    fallback_threads: Option<u8>,
    #[cfg(windows)] gpu: Option<crate::gpu::GpuInputConfig>,
) -> Result<StreamingEncoder> {
    let path = create_staging_path(&config.output_path)?;
    match StreamingEncoder::create_at_path(
        config,
        path.clone(),
        false,
        fallback_threads,
        #[cfg(windows)]
        gpu,
        true,
    ) {
        Ok(mut encoder) => {
            encoder.staging_path = Some(path);
            Ok(encoder)
        }
        Err(error) => {
            let _ = fs::remove_file(path);
            Err(error)
        }
    }
}

impl StreamingEncoder {
    /// Abandon queued GPU work once; emitted packets and continuous audio survive.
    pub fn recover_to_software(&mut self, reason: &str) -> Result<()> {
        let state = self
            .recovery
            .as_ref()
            .ok_or_else(|| error("encoder does not support recovery"))?;
        if state.recovered {
            return Err(error("software recording failed after GPU recovery"));
        }
        let mut config = state.config.clone();
        config.output_path = state.directory.join("video-1.mp4");
        config.audio = None;
        config.prefer_hardware_h264 = false;
        config.execution_mode = ExportExecutionMode::SoftwareOnly;
        config.software_h264_priority = SoftwareH264Priority::X264First;
        let next = segment(
            config,
            None,
            #[cfg(windows)]
            None,
        );
        let mut next = match next {
            Ok(next) => next,
            Err(cause) => {
                return Err(self
                    .preserve_recovery_failure(&cause.to_string())
                    .unwrap_or(cause));
            }
        };
        // No send_eof or receive_frame is required from the failed device.
        self.seal_segment(false)?;
        let mut state = self.recovery.take().expect("recovery state");
        state.previous_frames += self.report.video_packets;
        state.recovered = true;
        state.current_start = None;
        next.audio = self.audio.take();
        next.audio_output = self.audio_output.take();
        next.report.audio_encoder = self.report.audio_encoder.clone();
        next.report.encoded_audio_frames = self.report.encoded_audio_frames;
        next.report.inserted_silence_frames = self.report.inserted_silence_frames;
        next.report.dropped_audio_frames = self.report.dropped_audio_frames;
        next.report.abandoned_frames = self.report.abandoned_frames;
        next.report.recovery_count = 1;
        next.report.recovery_reason = Some(reason.to_owned());
        next.report.hardware_fallback = true;
        next.report.requested_video_encoder = self.report.requested_video_encoder.clone();
        #[cfg(feature = "bench-timing")]
        {
            next.report.timings = std::mem::take(&mut self.report.timings);
        }
        next.recovery = Some(state);
        *self = next;
        Ok(())
    }

    fn seal_segment(&mut self, require_trailer: bool) -> Result<()> {
        self.report.abandoned_frames += self
            .admitted_frames
            .saturating_sub(self.report.video_packets);
        self.pending.take();
        if let Some(mut output) = self.output.take() {
            let result = output.write_trailer();
            drop(output);
            if require_trailer {
                result.map_err(error)?;
            }
        }
        let path = self
            .staging_path
            .take()
            .ok_or_else(|| error("missing video segment"))?;
        let state = self.recovery.as_mut().expect("recovery state");
        // Keep the owned staging file directly; it is never published to the user.
        state.segments.push(Segment {
            path,
            start: state.current_start.unwrap_or(0),
            packets: self.report.video_packets,
        });
        Ok(())
    }

    pub(super) fn finish_recoverable(mut self, endpoint: i64) -> Result<StreamingEncoderReport> {
        #[cfg(feature = "bench-timing")]
        let started = std::time::Instant::now();
        let result = self.finalize_segments(endpoint);
        if let Err(cause) = result {
            return Err(self
                .retain_failure(&cause.to_string(), Some(endpoint))
                .expect("recovery state"));
        }
        self.finished = true;
        #[cfg(feature = "bench-timing")]
        self.report.timings.record("encode.finalize", started);
        Ok(self.report.clone())
    }

    /// Retain recoverable tracks when reopening capture or another live stage
    /// fails after startup. Explicit cancellation still drops session staging.
    pub fn preserve_recovery_failure(&mut self, cause: &str) -> Option<RecordingExportError> {
        self.retain_failure(cause, None)
    }

    fn retain_failure(
        &mut self,
        cause: &str,
        endpoint: Option<i64>,
    ) -> Option<RecordingExportError> {
        self.recovery.as_ref()?;
        if self.staging_path.is_some() {
            let _ = self.seal_segment(false);
        }
        if let (Some(audio), Some(output)) = (self.audio.as_mut(), self.audio_output.as_mut()) {
            let _ = audio.finish(output, &mut self.report);
            let _ = output.write_trailer();
        }
        self.output.take();
        self.audio_output.take();
        let state = self.recovery.as_mut().expect("recovery");
        state.preserve = true;
        let mut timeline = format!(
            "fps={}\nwidth={}\nheight={}\nendpoint={}\n",
            state.config.fps,
            state.config.width,
            state.config.height,
            endpoint.map_or_else(|| "unknown".into(), |value| value.to_string())
        );
        for segment in &state.segments {
            timeline.push_str(&format!(
                "video\t{}\t{}\t{}\n",
                segment.start,
                segment.packets,
                segment.path.display()
            ));
        }
        if let Some(audio) = &state.audio_path {
            timeline.push_str(&format!("audio\t{}\n", audio.display()));
        }
        let _ = fs::write(state.directory.join("timeline.txt"), timeline);
        Some(error(format!(
            "{cause}; recoverable media is retained in {}",
            state.directory.display()
        )))
    }

    fn finalize_segments(&mut self, endpoint: i64) -> Result<()> {
        let duration = self
            .pending
            .as_ref()
            .map_or(1, |frame| (endpoint - frame.pts).max(1));
        let video_result = (|| {
            #[cfg(any(test, feature = "bench-experiments"))]
            self.check_injected_failure(GpuFailureStage::Stop)?;
            self.encode_pending(duration)?;
            retry_send(
                self,
                |state| state.encoder.send_eof(),
                |state| state.drain_available_packets(),
            )?;
            self.drain_packets(true)?;
            Ok(())
        })();
        if let Err(cause) = video_result {
            let state = self.recovery.as_mut().expect("recovery");
            if state.recovered {
                return Err(cause);
            }
            state.recovered = true;
            self.report.recovery_count = 1;
            self.report.recovery_reason = Some(format!("GPU stop: {cause}"));
        }
        self.seal_segment(false)?;
        if let (Some(audio), Some(output)) = (self.audio.as_mut(), self.audio_output.as_mut()) {
            audio.finish(output, &mut self.report)?;
            output.write_trailer().map_err(error)?;
        }
        self.audio_output.take();
        let state = self.recovery.as_ref().expect("recovery");
        self.report.video_packets += state.previous_frames;
        self.report.encoded_frames = self.report.video_packets;
        let normalized;
        let video = if state.recovered {
            normalized = state.directory.join("normalized.mp4");
            self.report.encoded_frames =
                normalize(&state.segments, &state.config, &normalized, endpoint)?;
            self.report.video_encoder = "libx264".into();
            self.report.used_hardware_video_encoder = false;
            self.report.hardware_fallback = true;
            &normalized
        } else {
            &state
                .segments
                .first()
                .ok_or_else(|| error("no recoverable video segment"))?
                .path
        };
        let assembled = state.directory.join("assembled.mp4");
        let video_start = if state.recovered {
            0
        } else {
            state.segments[0].start
        };
        remux(
            video,
            state.audio_path.as_deref(),
            &assembled,
            video_start,
            state.config.fps,
        )?;
        publish_staging_file(&assembled, &state.config.output_path)?;
        Ok(())
    }
}

fn remux(
    video: &Path,
    audio: Option<&Path>,
    destination: &Path,
    video_start: i64,
    fps: u32,
) -> Result<()> {
    let mut inputs = vec![ffmpeg::format::input(video).map_err(error)?];
    if let Some(audio) = audio {
        inputs.push(ffmpeg::format::input(audio).map_err(error)?);
    }
    let mut output = ffmpeg::format::output(destination).map_err(error)?;
    let mut streams = Vec::new();
    for (index, input) in inputs.iter().enumerate() {
        let medium = if index == 0 {
            ffmpeg::media::Type::Video
        } else {
            ffmpeg::media::Type::Audio
        };
        let source = input
            .streams()
            .best(medium)
            .ok_or_else(|| error("missing staged track"))?;
        let mut target = output
            .add_stream(ffmpeg::encoder::find(ffmpeg::codec::Id::None))
            .map_err(error)?;
        target.set_parameters(source.parameters());
        target.set_time_base(source.time_base());
        unsafe {
            (*target.parameters().as_mut_ptr()).codec_tag = 0;
        }
        streams.push((source.index(), source.time_base(), target.index()));
    }
    output.write_header().map_err(error)?;
    for (input, (source_index, time_base, target_index)) in inputs.iter_mut().zip(streams) {
        let target_time_base = output
            .stream(target_index)
            .ok_or_else(|| error("missing mux track"))?
            .time_base();
        let mut video_offset = None;
        for (stream, mut packet) in input.packets() {
            if stream.index() != source_index {
                continue;
            }
            if target_index == 0 {
                let offset = *video_offset.get_or_insert_with(|| {
                    video_start.rescale((1, fps as i32), time_base) - packet.pts().unwrap_or(0)
                });
                packet.set_pts(packet.pts().map(|pts| pts + offset));
                packet.set_dts(packet.dts().map(|dts| dts + offset));
            }
            packet.rescale_ts(time_base, target_time_base);
            packet.set_stream(target_index);
            packet.set_position(-1);
            packet.write_interleaved(&mut output).map_err(error)?;
        }
    }
    output.write_trailer().map_err(error)
}

/// Decoding only happens after recovery, with one software decoder per segment.
/// Pending-frame durations hold the last decoded image over the transition gap.
fn normalize(
    segments: &[Segment],
    config: &StreamingEncoderConfig,
    path: &Path,
    endpoint: i64,
) -> Result<u64> {
    let mut settings = config.clone();
    settings.output_path = path.to_owned();
    settings.audio = None;
    let mut output = StreamingEncoder::builder(settings)
        .software_only()
        .create()?;
    let mut last_pts = None;
    for segment in segments {
        // A failed probe/first submission can leave only an empty MP4 header.
        // It has no image to recover, and must not prevent decoding later segments.
        if segment.packets == 0 {
            continue;
        }
        let mut input = ffmpeg::format::input(&segment.path).map_err(error)?;
        let stream = input
            .streams()
            .best(ffmpeg::media::Type::Video)
            .ok_or_else(|| error("segment has no video track"))?;
        let index = stream.index();
        let time_base = stream.time_base();
        let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
            .map_err(error)?
            .decoder()
            .video()
            .map_err(error)?;
        let mut scaler = None;
        let mut offset = None;
        let mut receive = |decoder: &mut ffmpeg::decoder::Video| -> Result<()> {
            let mut decoded = ffmpeg::frame::Video::empty();
            loop {
                match decoder.receive_frame(&mut decoded) {
                    Ok(()) => {}
                    Err(cause)
                        if crate::ffmpeg_util::is_eagain(&cause) || cause == ffmpeg::Error::Eof =>
                    {
                        break;
                    }
                    Err(cause) => return Err(error(cause)),
                }
                let pts = decoded
                    .timestamp()
                    .or(decoded.pts())
                    .ok_or_else(|| error("decoded frame has no timestamp"))?
                    .rescale(time_base, (1, config.fps as i32));
                let offset = *offset.get_or_insert(segment.start - pts);
                let pts = pts + offset;
                if pts < 0 || pts >= endpoint || last_pts.is_some_and(|last| pts <= last) {
                    continue;
                }
                if scaler.is_none() {
                    scaler = Some(
                        ffmpeg::software::scaling::Context::get(
                            decoded.format(),
                            decoded.width(),
                            decoded.height(),
                            ffmpeg::format::Pixel::RGBA,
                            config.width,
                            config.height,
                            ffmpeg::software::scaling::flag::Flags::BICUBIC,
                        )
                        .map_err(error)?,
                    );
                }
                let mut rgba = ffmpeg::frame::Video::empty();
                // Every segment uses limited-range BT.709; the CPU recovery
                // intermediate uses the same full-range RGB contract as capture.
                let matrix =
                    unsafe { ffmpeg::ffi::sws_getCoefficients(ffmpeg::ffi::SWS_CS_ITU709) };
                let code = unsafe {
                    ffmpeg::ffi::sws_setColorspaceDetails(
                        scaler.as_mut().expect("scaler").as_mut_ptr(),
                        matrix,
                        0,
                        matrix,
                        1,
                        0,
                        1 << 16,
                        1 << 16,
                    )
                };
                if code < 0 {
                    return Err(error(ffmpeg::Error::from(code)));
                }
                scaler
                    .as_mut()
                    .expect("scaler")
                    .run(&decoded, &mut rgba)
                    .map_err(error)?;
                let row_bytes = config.width as usize * 4;
                let mut pixels = Vec::with_capacity(row_bytes * config.height as usize);
                for row in 0..config.height as usize {
                    pixels.extend_from_slice(
                        &rgba.data(0)[row * rgba.stride(0)..row * rgba.stride(0) + row_bytes],
                    );
                }
                if last_pts.is_none() && pts > 0 {
                    output.push_owned_rgba_frame_at_pts(0, pixels.clone())?;
                }
                output.push_owned_rgba_frame_at_pts(pts as u64, pixels)?;
                last_pts = Some(pts);
            }
            Ok(())
        };
        for (stream, packet) in input.packets() {
            if stream.index() != index {
                continue;
            }
            match decoder.send_packet(&packet) {
                Err(cause) if crate::ffmpeg_util::is_eagain(&cause) => {
                    receive(&mut decoder)?;
                    decoder.send_packet(&packet).map_err(error)?;
                }
                Err(cause) => return Err(error(cause)),
                Ok(()) => {}
            }
            receive(&mut decoder)?;
        }
        decoder.send_eof().map_err(error)?;
        receive(&mut decoder)?;
    }
    if last_pts.is_none() {
        return Err(error("no decodable video remains"));
    }
    Ok(output.finish_at_pts(endpoint as u64)?.encoded_frames)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn config(path: PathBuf) -> StreamingEncoderConfig {
        StreamingEncoderConfig {
            output_path: path,
            format: ExportFormat::Mp4,
            width: 32,
            height: 32,
            fps: 10,
            codec: VideoCodec::H264,
            prefer_hardware_h264: false,
            execution_mode: ExportExecutionMode::SoftwareOnly,
            software_h264_priority: SoftwareH264Priority::X264First,
            video: VideoEncodeConfig {
                quality: 80,
                speed: snow_recording_model::VideoEncodingSpeed::VeryFast,
            },
            encode_threads: 1,
            audio: Some(StreamingAudioConfig {
                sample_rate_hz: 48_000,
                channels: 2,
                bitrate_kbps: 128,
            }),
        }
    }

    #[test]
    #[cfg(windows)]
    #[ignore = "requires offscreen VideoProcessor and a native H.264 encoder"]
    fn hardware_static_image_survives_failure_without_a_successor() -> anyhow::Result<()> {
        use snow_d3d11::{Rect, SharedDevice, VideoLayer, VideoProcessor};
        use windows::Win32::Graphics::Direct3D11::*;
        use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_B8G8R8A8_UNORM;
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;

        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }?;
        let adapter = unsafe { factory.EnumAdapters1(0) }?.cast()?;
        let device = SharedDevice::create(&adapter)?;
        let size = (640, 480);
        let processor = VideoProcessor::new(device.clone(), size, size, 30)?;
        let texture = device.texture(
            size.0,
            size.1,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET).0 as u32,
        )?;
        {
            let _lock = device.lock();
            let mut view = None;
            unsafe {
                device
                    .device()
                    .CreateRenderTargetView(texture.raw(), None, Some(&mut view))?;
                device
                    .context()
                    .ClearRenderTargetView(view.as_ref().unwrap(), &[0.2, 0.4, 0.6, 1.0]);
            }
        }
        let layer = VideoLayer {
            texture,
            source: Rect::full(size),
            destination: Rect::full(size),
            rotation: 0,
            alpha: false,
        };
        let directory = tempfile::tempdir()?;
        let path = directory.path().join("static.mp4");
        let mut settings = config(path.clone());
        settings.width = size.0;
        settings.height = size.1;
        settings.fps = 30;
        settings.audio = None;
        settings.prefer_hardware_h264 = true;
        settings.execution_mode = ExportExecutionMode::HardwarePreferred;
        let mut encoder = StreamingEncoder::builder(settings)
            .gpu_input(crate::gpu::GpuInputConfig { device })
            .recoverable()
            .create()?;
        assert!(encoder.needs_recovery_image());
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(3);
        let mut pts = 0;
        while encoder.needs_recovery_image() {
            anyhow::ensure!(
                std::time::Instant::now() < deadline,
                "no recoverable initial GPU packet"
            );
            encoder.poll_gpu_packets()?;
            if !encoder.needs_recovery_image() {
                break;
            }
            if let Some(surface) = encoder.allocate_gpu_frame()? {
                processor.blit(
                    std::slice::from_ref(&layer),
                    &surface.texture()?,
                    surface.slice(),
                )?;
                encoder.push_gpu_frame_at_pts(pts, surface)?;
                pts += 1;
            }
            std::thread::sleep(std::time::Duration::from_millis(34));
        }
        // No desktop changes, successor capture, or successful hardware flush.
        // The initial encoded image must cover the entire remaining interval.
        let endpoint = pts + 30;
        let outstanding = encoder
            .hardware_frames
            .as_ref()
            .unwrap()
            .outstanding_tracker();
        encoder.inject_gpu_failure(GpuFailureStage::Stop);
        let report = encoder.finish_at_pts(endpoint)?;
        assert_eq!(
            outstanding.load(std::sync::atomic::Ordering::Acquire),
            0,
            "closing a failed hardware encoder must release its surface references"
        );
        assert_eq!(report.recovery_count, 1);
        assert_eq!(report.video_encoder, "libx264");
        let mut media = ffmpeg::format::input(&path)?;
        let stream = media.streams().best(ffmpeg::media::Type::Video).unwrap();
        let base = stream.time_base();
        let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())?
            .decoder()
            .video()?;
        let mut decoded = 0;
        let mut end = 0;
        for (_, packet) in media.packets() {
            end = (packet.pts().unwrap() + packet.duration()).rescale(base, (1, 30));
            decoder.send_packet(&packet)?;
            while decoder
                .receive_frame(&mut ffmpeg::frame::Video::empty())
                .is_ok()
            {
                decoded += 1;
            }
        }
        decoder.send_eof()?;
        while decoder
            .receive_frame(&mut ffmpeg::frame::Video::empty())
            .is_ok()
        {
            decoded += 1;
        }
        assert!(decoded > 0);
        assert_eq!(end, endpoint as i64);
        Ok(())
    }

    #[test]
    fn live_failure_preserves_emitted_media_and_manifest() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("unpublished.mp4");
        let mut encoder = StreamingEncoder::builder(config(path.clone()))
            .recoverable()
            .create()
            .unwrap();
        for pts in [0, 2] {
            encoder
                .push_owned_rgba_frame_at_pts(pts, vec![50; 32 * 32 * 4])
                .unwrap();
        }
        encoder.push_audio_pcm_i16(0, &vec![300; 19_200]).unwrap();
        let failure = encoder
            .preserve_recovery_failure("CPU capture could not reopen")
            .unwrap();
        let retained = encoder.recovery.as_ref().unwrap().directory.clone();
        drop(encoder);
        assert!(
            failure
                .to_string()
                .contains("recoverable media is retained")
        );
        assert!(!path.exists());
        let manifest = fs::read_to_string(retained.join("timeline.txt")).unwrap();
        assert!(manifest.contains("video\t0\t1\t"));
        assert!(manifest.contains("audio\t"));
        assert!(
            ffmpeg::format::input(&retained.join("audio.mp4"))
                .unwrap()
                .packets()
                .next()
                .is_some()
        );
    }

    #[test]
    fn recovery_before_first_packet_skips_empty_segment_and_fills_initial_gap() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("early.mp4");
        let mut settings = config(path.clone());
        settings.audio = None;
        let mut encoder = StreamingEncoder::builder(settings)
            .recoverable()
            .create()
            .unwrap();
        encoder
            .push_owned_rgba_frame_at_pts(0, vec![50; 32 * 32 * 4])
            .unwrap();
        encoder
            .recover_to_software("failure before first packet")
            .unwrap();
        encoder
            .push_owned_rgba_frame_at_pts(5, vec![200; 32 * 32 * 4])
            .unwrap();
        let report = encoder.finish_at_pts(10).unwrap();
        assert_eq!(report.abandoned_frames, 1);
        assert_eq!(report.encoded_frames, 2);
        let mut media = ffmpeg::format::input(&path).unwrap();
        let points: Vec<_> = media
            .packets()
            .map(|(stream, packet)| {
                (
                    packet.pts().unwrap().rescale(stream.time_base(), (1, 10)),
                    packet.duration().rescale(stream.time_base(), (1, 10)),
                )
            })
            .collect();
        assert_eq!(points, [(0, 5), (5, 5)]);
    }

    #[test]
    fn healthy_remux_preserves_nonzero_first_pts_and_endpoint() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("late.mp4");
        let mut settings = config(path.clone());
        settings.audio = None;
        let mut encoder = StreamingEncoder::builder(settings)
            .recoverable()
            .create()
            .unwrap();
        for pts in [3, 7] {
            encoder
                .push_owned_rgba_frame_at_pts(pts, vec![50; 32 * 32 * 4])
                .unwrap();
        }
        let report = encoder.finish_at_pts(10).unwrap();
        assert_eq!(report.abandoned_frames, 0);
        let mut media = ffmpeg::format::input(&path).unwrap();
        let points: Vec<_> = media
            .packets()
            .map(|(stream, packet)| {
                (
                    packet.pts().unwrap().rescale(stream.time_base(), (1, 10)),
                    packet.duration().rescale(stream.time_base(), (1, 10)),
                )
            })
            .collect();
        assert_eq!(points, [(3, 4), (7, 3)]);
    }

    #[test]
    fn recovery_retains_video_timeline_and_one_continuous_audio_track() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("recovered.mp4");
        let mut encoder = StreamingEncoder::builder(config(path.clone()))
            .recoverable()
            .create()
            .unwrap();
        for pts in [0, 2, 4] {
            encoder
                .push_owned_rgba_frame_at_pts(pts, vec![50; 32 * 32 * 4])
                .unwrap();
        }
        encoder.push_audio_pcm_i16(0, &vec![300; 48_000]).unwrap();
        encoder.recover_to_software("injected device loss").unwrap();
        assert!(encoder.recover_to_software("second loss").is_err());
        for pts in [8, 10] {
            encoder
                .push_owned_rgba_frame_at_pts(pts, vec![200; 32 * 32 * 4])
                .unwrap();
        }
        encoder.push_audio_pcm_i16(500, &vec![300; 67_200]).unwrap();
        let report = encoder.finish_at_pts(12).unwrap();
        assert_eq!(report.recovery_count, 1);
        assert_eq!(report.abandoned_frames, 1);
        assert_eq!(report.audio_encoder.as_deref(), Some("aac"));
        let mut media = ffmpeg::format::input(&path).unwrap();
        assert_eq!(media.nb_streams(), 2);
        let stream = media.streams().best(ffmpeg::media::Type::Video).unwrap();
        let index = stream.index();
        let base = stream.time_base();
        let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
            .unwrap()
            .decoder()
            .video()
            .unwrap();
        let mut points = Vec::new();
        let mut audio_pts = Vec::new();
        let mut decoded = 0;
        for (stream, packet) in media.packets() {
            if stream.index() == index {
                points.push((
                    packet.pts().unwrap().rescale(base, (1, 10)),
                    packet.duration().rescale(base, (1, 10)),
                ));
                decoder.send_packet(&packet).unwrap();
                while decoder
                    .receive_frame(&mut ffmpeg::frame::Video::empty())
                    .is_ok()
                {
                    decoded += 1;
                }
            } else {
                audio_pts.push(packet.pts().unwrap());
            }
        }
        decoder.send_eof().unwrap();
        while decoder
            .receive_frame(&mut ffmpeg::frame::Video::empty())
            .is_ok()
        {
            decoded += 1;
        }
        assert_eq!(points, [(0, 2), (2, 6), (8, 2), (10, 2)]);
        assert_eq!(decoded, 4);
        assert!(audio_pts.len() > 50);
        assert!(audio_pts.windows(2).all(|pair| pair[1] - pair[0] == 1024));
        assert_eq!(fs::read_dir(directory.path()).unwrap().count(), 1);
    }

    #[test]
    fn cancellation_removes_session_resources_and_publication_failure_preserves_them() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("existing.mp4");
        let make = || {
            StreamingEncoder::builder(config(path.clone()))
                .recoverable()
                .create()
                .unwrap()
        };
        drop(make());
        assert_eq!(fs::read_dir(directory.path()).unwrap().count(), 0);
        let mut encoder = make();
        encoder
            .push_owned_rgba_frame_at_pts(0, vec![50; 32 * 32 * 4])
            .unwrap();
        fs::write(&path, b"existing file").unwrap();
        let failure = encoder.finish_at_pts(2).unwrap_err().to_string();
        assert!(
            failure.contains("recoverable media is retained"),
            "{failure}"
        );
        assert_eq!(fs::read(&path).unwrap(), b"existing file");
        assert_eq!(fs::read_dir(directory.path()).unwrap().count(), 2);
    }
}

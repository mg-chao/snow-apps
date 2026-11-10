//! Bounded, deterministic replay of a finalized clean recording.
//! Capture and replay share `StreamingEncoder`; publication is transactional.
use snow_memory::RasterBuffer;
#[cfg(test)]
mod tests;
use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};
use std::thread::JoinHandle;
use std::time::Instant;

use bincode::Options;
use ffmpeg::Rescale;
use ffmpeg_next as ffmpeg;
use snow_core::cancellation::CancellationToken;
use snow_recording_effects::RecordedEffects;
use snow_recording_model::{
    BundleAssetKind, FinalizedTimeline, FixedFrameTimeline, InputStoreReader, PlaybackSpeed,
    RecordingBundleFooter, RenderMetadata, VideoIndexReader, read_bundle_render_metadata,
    read_recording_bundle_footer,
};

use crate::error::Result;
use crate::{
    ExportResult, ExportRuntimeReport, ExportStage, RecordingExportError, StreamingEncoder,
    StreamingEncoderConfig,
};

const SETTINGS_MAGIC: &[u8] = b"SNOWOUTPUT\0\x01";
const SETTINGS_LIMIT: u64 = 4 * 1024 * 1024;

pub fn write_deferred_output_settings(path: &Path, config: &StreamingEncoderConfig) -> Result<()> {
    config
        .validate()
        .map_err(RecordingExportError::InvalidConfig)?;
    let mut output = std::io::BufWriter::new(File::create(path)?);
    output.write_all(SETTINGS_MAGIC)?;
    bincode::serialize_into(&mut output, config).map_err(|e| decode(e.to_string()))?;
    output.flush()?;
    Ok(())
}

pub fn read_deferred_output_settings(path: &Path) -> Result<StreamingEncoderConfig> {
    let footer = read_recording_bundle_footer(path)?;
    let asset = footer
        .asset(BundleAssetKind::OutputSettings, None)
        .ok_or_else(|| decode("recording output settings are missing"))?;
    if asset.len > SETTINGS_LIMIT {
        return Err(decode("output settings exceed maximum size"));
    }
    let mut input = File::open(path)?;
    input.seek(SeekFrom::Start(asset.offset))?;
    let mut input = input.take(asset.len);
    let mut magic = [0; 12];
    input.read_exact(&mut magic)?;
    if magic != SETTINGS_MAGIC {
        return Err(decode("unsupported recording output settings"));
    }
    let config: StreamingEncoderConfig = bincode::DefaultOptions::new()
        .with_fixint_encoding()
        .with_limit(SETTINGS_LIMIT)
        .deserialize_from(input)
        .map_err(|e| decode(e.to_string()))?;
    config
        .validate()
        .map_err(RecordingExportError::InvalidConfig)?;
    Ok(config)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeferredRenderState {
    Running,
    Succeeded,
    Canceled,
    Failed,
}

#[derive(Clone, Debug)]
pub struct DeferredRenderProgress {
    pub state: DeferredRenderState,
    pub stage: ExportStage,
    pub percent: f32,
    pub completed_frames: u64,
    pub total_frames: u64,
    pub duration_ms: u64,
    pub eta_ms: Option<u64>,
    pub error: Option<String>,
    pub output_path: Option<PathBuf>,
}

pub struct DeferredRenderTask {
    cancellation: CancellationToken,
    progress: Arc<Mutex<DeferredRenderProgress>>,
    join: Option<JoinHandle<Result<ExportResult>>>,
}

impl DeferredRenderTask {
    /// `active` is the source's single-render lease, also checked by discard.
    pub fn start(
        path: PathBuf,
        config: StreamingEncoderConfig,
        active: Arc<AtomicBool>,
    ) -> Result<Self> {
        Self::start_inner(
            path,
            config,
            active,
            #[cfg(test)]
            None,
        )
    }
    fn start_inner(
        path: PathBuf,
        config: StreamingEncoderConfig,
        active: Arc<AtomicBool>,
        #[cfg(test)] gate: Option<crossbeam_channel::Receiver<()>>,
    ) -> Result<Self> {
        config
            .validate()
            .map_err(RecordingExportError::InvalidConfig)?;
        let metadata = read_bundle_render_metadata(&path)?
            .ok_or_else(|| decode("render metadata is missing"))?;
        if active
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return Err(RecordingExportError::InvalidConfig(
                "a render is already using this source".into(),
            ));
        }
        let cancellation = CancellationToken::default();
        let progress = Arc::new(Mutex::new(DeferredRenderProgress {
            state: DeferredRenderState::Running,
            stage: ExportStage::Plan,
            percent: 0.0,
            completed_frames: 0,
            total_frames: metadata.timeline.frame_count(),
            duration_ms: metadata.timeline.duration_ms(),
            eta_ms: None,
            error: None,
            output_path: None,
        }));
        let worker_progress = Arc::clone(&progress);
        let worker_cancel = cancellation.clone();
        let worker_active = Arc::clone(&active);
        let join = std::thread::Builder::new()
            .name("snow-recording-render".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                let _lease = RenderLease(worker_active);
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    #[cfg(test)]
                    wait_for_render_gate(&worker_cancel, gate)?;
                    render_bundle(
                        &path,
                        config,
                        metadata,
                        &worker_cancel,
                        |stage, completed, total, started| {
                            update_render_progress(
                                &worker_progress,
                                stage,
                                completed,
                                total,
                                started,
                            );
                        },
                    )
                }))
                .unwrap_or_else(|_| {
                    Err(RecordingExportError::Export(
                        "render worker panicked".into(),
                    ))
                });
                if result.is_ok() {
                    let _ = std::fs::remove_file(&path);
                }
                finish_render_progress(&worker_progress, &result);
                result
            });
        let join = match join {
            Ok(join) => join,
            Err(error) => {
                active.store(false, Ordering::Release);
                return Err(error.into());
            }
        };
        Ok(Self {
            cancellation,
            progress,
            join: Some(join),
        })
    }
    pub fn cancel(&self) {
        self.cancellation.cancel();
    }
    pub fn snapshot(&self) -> DeferredRenderProgress {
        self.progress
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
    }
    pub fn wait(mut self) -> Result<ExportResult> {
        self.join
            .take()
            .ok_or_else(|| decode("render task already awaited"))?
            .join()
            .map_err(|_| RecordingExportError::Export("render worker panicked".into()))?
    }
}

struct RenderLease(Arc<AtomicBool>);

impl Drop for RenderLease {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

fn update_render_progress(
    progress: &Mutex<DeferredRenderProgress>,
    stage: ExportStage,
    completed: u64,
    total: u64,
    started: Instant,
) {
    let mut state = progress.lock().unwrap_or_else(|error| error.into_inner());
    state.stage = stage;
    state.completed_frames = completed;
    state.total_frames = total;
    let percent = if stage == ExportStage::Finalize {
        98.0
    } else {
        (5.0 + completed as f64 / total.max(1) as f64 * 90.0) as f32
    };
    state.percent = state.percent.max(percent);
    state.eta_ms = (completed > 0).then(|| {
        let remaining = started
            .elapsed()
            .as_millis()
            .saturating_mul(u128::from(total.saturating_sub(completed)))
            / u128::from(completed);
        remaining.min(u128::from(u64::MAX)) as u64
    });
}

fn finish_render_progress(progress: &Mutex<DeferredRenderProgress>, result: &Result<ExportResult>) {
    let mut state = progress.lock().unwrap_or_else(|error| error.into_inner());
    match result {
        Ok(result) => {
            state.state = DeferredRenderState::Succeeded;
            state.percent = 100.0;
            state.stage = ExportStage::Finalize;
            state.eta_ms = Some(0);
            state.output_path = Some(result.output_path.clone());
        }
        Err(RecordingExportError::ExportCanceled) => {
            state.state = DeferredRenderState::Canceled;
        }
        Err(error) => {
            state.state = DeferredRenderState::Failed;
            state.error = Some(error.to_string());
        }
    }
}

#[cfg(test)]
fn wait_for_render_gate(
    cancellation: &CancellationToken,
    gate: Option<crossbeam_channel::Receiver<()>>,
) -> Result<()> {
    if let Some(gate) = gate {
        crossbeam_channel::select! {
            recv(cancellation.receiver()) -> _ => Err(RecordingExportError::ExportCanceled),
            recv(gate) -> _ => Ok(()),
        }
    } else {
        Ok(())
    }
}
impl Drop for DeferredRenderTask {
    fn drop(&mut self) {
        if let Some(join) = self.join.take() {
            self.cancel();
            let _ = join.join();
        }
    }
}

fn decode(message: impl Into<String>) -> RecordingExportError {
    RecordingExportError::Decode(message.into())
}
fn check_cancel(cancellation: &CancellationToken) -> Result<()> {
    if cancellation.is_canceled() {
        Err(RecordingExportError::ExportCanceled)
    } else {
        Ok(())
    }
}

/// The same replay engine is available to an EditingSession opened from a kept v3 source.
pub(crate) fn render_bundle(
    path: &Path,
    config: StreamingEncoderConfig,
    metadata: RenderMetadata,
    cancellation: &CancellationToken,
    progress: impl FnMut(ExportStage, u64, u64, Instant),
) -> Result<ExportResult> {
    let range = snow_recording_model::TrimRange::new(
        0,
        metadata.timeline.duration_ms() * 1000,
        metadata.timeline.duration_ms() * 1000,
    )
    .map_err(RecordingExportError::InvalidConfig)?;
    render_bundle_range(path, config, metadata, range, cancellation, progress)
}

/// A non-destructive render attempt. Source ownership belongs to the clip/caller.
pub fn render_bundle_range(
    path: &Path,
    config: StreamingEncoderConfig,
    metadata: RenderMetadata,
    range: snow_recording_model::TrimRange,
    cancellation: &CancellationToken,
    progress: impl FnMut(ExportStage, u64, u64, Instant),
) -> Result<ExportResult> {
    render_bundle_range_with_speed(
        path,
        config,
        metadata,
        range,
        PlaybackSpeed::default(),
        cancellation,
        progress,
    )
}

/// Retiming changes the output clock while input effects remain on the source clock.
pub fn render_bundle_range_with_speed(
    path: &Path,
    config: StreamingEncoderConfig,
    metadata: RenderMetadata,
    range: snow_recording_model::TrimRange,
    speed: PlaybackSpeed,
    cancellation: &CancellationToken,
    mut progress: impl FnMut(ExportStage, u64, u64, Instant),
) -> Result<ExportResult> {
    check_cancel(cancellation)?;
    if let Ok(output) = config.output_path.canonicalize()
        && output == path.canonicalize()?
    {
        return Err(RecordingExportError::InvalidConfig(
            "render destination must differ from the recording source".into(),
        ));
    }
    crate::ffmpeg_util::ensure_ffmpeg_initialized()?;
    let footer = read_recording_bundle_footer(path)?;
    let source = snow_recording_model::SourceDescription::deferred(&footer.manifest, &metadata)?;
    let source_hdr = source.media.color == snow_media::ColorDescription::HDR10;
    let output_hdr = crate::preserves_hdr_output(source_hdr, config.format, config.codec);
    let fixed_rate_retime = speed.percent() != 100 && !config.format.is_animated_image();
    snow_recording_model::TrimRange::new(
        range.start_us,
        range.end_us,
        source.timeline.duration_ms() * 1000,
    )
    .map_err(RecordingExportError::InvalidConfig)?;
    let first =
        (u128::from(range.start_us) * u128::from(source.timeline.fps())).div_ceil(1_000_000) as u64;
    let last = ((u128::from(range.end_us) * u128::from(source.timeline.fps())).div_ceil(1_000_000)
        as u64)
        .saturating_sub(1)
        .min(source.timeline.frame_count() - 1);
    let origin_ms = source
        .timeline
        .frame(first)
        .ok_or_else(|| decode("empty trim range"))?
        .timestamp_ms;
    let timing =
        FixedFrameTimeline::new(source.timeline.duration_ms() * 1000, source.timeline.fps())
            .and_then(|timeline| timeline.retime(range, speed))
            .map_err(RecordingExportError::InvalidConfig)?;
    let output_duration_us = timing
        .duration_us()
        .map_err(RecordingExportError::InvalidConfig)?;
    let timeline =
        FinalizedTimeline::new((output_duration_us / 1000).max(1), source.timeline.fps())
            .map_err(RecordingExportError::InvalidConfig)?;
    let total = timing
        .frame_count()
        .map_err(RecordingExportError::InvalidConfig)?;
    let audio_samples = if speed.percent() == 100 {
        range.sample_count(48_000)
    } else {
        timing
            .sample_count(48_000)
            .map_err(RecordingExportError::InvalidConfig)?
    };
    if config.fps != timeline.fps()
        || (config.width, config.height)
            != (metadata.render.output_width, metadata.render.output_height)
    {
        return Err(RecordingExportError::InvalidConfig(
            "render output disagrees with finalized timeline/canvas".into(),
        ));
    }
    let started = Instant::now();
    progress(ExportStage::Plan, 0, total, started);
    #[cfg(feature = "bench-timing")]
    let mut stage_times = [std::time::Duration::ZERO; 6];
    let rasterizer = metadata
        .render
        .effects
        .keyboard
        .as_ref()
        .map(snow_recording_effects::keyboard_rasterizer::create)
        .transpose()
        .map_err(RecordingExportError::Export)?;
    let mut effects = RecordedEffects::new(
        metadata.render.clone(),
        (source.logical_width, source.logical_height),
        rasterizer,
    )
    .map_err(RecordingExportError::Export)?
    .with_timeline(timeline)
    .with_playback_origin(origin_ms);
    let mut inputs = InputStoreReader::from_bundle(path)?
        .ok_or_else(|| decode("recording input timeline is missing"))?;
    let mut encoder_config = config.clone();
    let software_threads = deferred_software_threads(
        &config,
        crate::codec::auto_thread_count_from_physical_cores(),
    );
    if let Some(threads) = software_threads
        && !encoder_config.prefer_hardware_h264
    {
        encoder_config.encode_threads = threads;
    }
    let mut builder = StreamingEncoder::builder(encoder_config).offline_output();
    if let Some(threads) = software_threads {
        builder = builder.software_fallback_threads(threads);
    }
    if output_hdr {
        builder = builder.hdr10_cpu_input();
    }
    let mut encoder = builder.create()?;
    encoder.configure_conversion_threads(deferred_conversion_threads(
        &config,
        software_threads,
        output_hdr,
    ))?;
    let mut video = SequentialVideoSource::open(
        path,
        source.timeline,
        source_hdr,
        (source.coded_width, source.coded_height),
        (config.width, config.height),
        cancellation,
    )?;
    video.seek_to(first, cancellation)?;
    let mut webcam = crate::webcam::WebcamReplay::open(path, cancellation)?;
    let mut audio = ExportAudioReplay::open(path, &footer, &config, range, speed)?;
    // Allocate only when composition needs storage. A wholly held output never
    // needs a second canvas, and HDR composition never uses an RGBA canvas.
    let mut rgba = RasterBuffer::default();
    let mut hdr_conversion = if output_hdr {
        let mut conversion = ffmpeg::software::scaling::Context::get(
            ffmpeg::format::Pixel::RGB48LE,
            config.width,
            config.height,
            encoder.input_pixel_format(),
            config.width,
            config.height,
            ffmpeg::software::scaling::Flags::BICUBIC,
        )
        .map_err(|e| decode(e.to_string()))?;
        crate::hdr::scaler_colors(&mut conversion, true, false)?;
        Some(conversion)
    } else {
        None
    };
    let mut hdr_rgb = output_hdr.then(|| {
        ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, config.width, config.height)
    });
    let mut hdr_encoded = output_hdr.then(|| {
        ffmpeg::frame::Video::new(encoder.input_pixel_format(), config.width, config.height)
    });
    let mut previous_hdr = output_hdr.then(|| {
        ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, config.width, config.height)
    });
    let mut hdr_valid = false;
    #[cfg(feature = "bench-timing")]
    let mut hdr_conversions = 0u64;
    #[cfg(feature = "bench-timing")]
    let mut hdr_reuse_checks = 0u64;
    #[cfg(feature = "bench-timing")]
    {
        stage_times[0] = started.elapsed();
    }
    let mut previous_telemetry = Instant::now();
    let mut previous_progress_width = None;
    let mut previous_effect_identity = None;
    let mut previous_effects_idle = false;
    #[cfg(feature = "bench-timing")]
    let mut skipped_compositions = 0u64;
    for index in 0..total {
        let output_frame = snow_recording_model::RenderFrame {
            index,
            pts: index,
            timestamp_ms: (u128::from(index) * 1000 / u128::from(config.fps)) as u64,
            progress: if total == 1 {
                1.0
            } else {
                index as f64 / (total - 1) as f64
            },
        };
        let source_index = if speed.percent() == 100 {
            first + index
        } else {
            first
                .saturating_add((u128::from(index) * u128::from(speed.percent()) / 100) as u64)
                .min(last)
        };
        let frame = snow_recording_model::RenderFrame {
            timestamp_ms: if speed.percent() == 100 {
                source
                    .timeline
                    .frame(source_index)
                    .ok_or_else(|| decode("trim exceeds source frames"))?
                    .timestamp_ms
            } else {
                // Keep the source origin and output slot in one rational expression;
                // independently rounded microseconds can miss a boundary observation.
                (((u128::from(first) * 100 + u128::from(index) * u128::from(speed.percent()))
                    * 1_000_000
                    / (u128::from(config.fps) * 100))
                    .min(u128::from(range.end_us - 1))
                    / 1000) as u64
            },
            ..output_frame
        };
        let playback_ms = if speed.percent() == 100 {
            frame.timestamp_ms.saturating_sub(origin_ms)
        } else {
            output_frame.timestamp_ms
        };
        check_cancel(cancellation)?;
        #[cfg(feature = "bench-timing")]
        let stage_started = Instant::now();
        let source_changed = video.advance(source_index, cancellation)?;
        if let Some(webcam) = &mut webcam {
            let image = webcam.frame_at(frame.timestamp_ms, cancellation)?;
            effects.set_webcam_frame(image.as_deref(), webcam.style());
        }
        #[cfg(feature = "bench-timing")]
        {
            stage_times[1] += stage_started.elapsed();
        }
        #[cfg(feature = "bench-timing")]
        let stage_started = Instant::now();
        inputs.replay_until(frame.timestamp_ms, |event| {
            effects
                .observe(&event)
                .map_err(snow_recording_model::RecordingModelError::Decode)
        })?;
        let progress_width = match metadata.render.playback_overlay {
            snow_recording_model::PlaybackOverlay::ProgressBar { .. } => Some(
                snow_recording_effects::playback::progress_bar_width(config.width, frame.progress),
            ),
            _ => None,
        };
        // A changed progress width is already proof of changed pixels. Compare
        // complete composed images only when the bar could actually be held.
        let playback_unchanged = progress_width == previous_progress_width;
        previous_progress_width = progress_width;
        let effect_identity = (
            effects.observation_revision(),
            snow_recording_effects::playback::playback_content_key(
                metadata.render.playback_overlay,
                config.width,
                snow_recording_model::RenderFrame {
                    timestamp_ms: playback_ms,
                    ..frame
                },
                Some(timeline),
            )
            .map_err(RecordingExportError::Export)?,
        );
        // Replay owns all effect mutations through observe/set_webcam_frame. A held
        // source plus unchanged observations and settled effects proves pixel identity
        // before copying the canvas. Check both sides of animation expiry: the first
        // settled frame must still erase the final visible animation pixels.
        // The encoder retains each format's bounded delay/frame-grid policy when
        // repeating the pending image, including fixed-rate retimed output.
        let hold_composition = !source_changed
            && previous_effects_idle
            && previous_effect_identity == Some(effect_identity)
            && reusable_effect_frame(&mut effects, frame.timestamp_ms);
        if hold_composition {
            #[cfg(feature = "bench-timing")]
            {
                skipped_compositions += 1;
                stage_times[2] += stage_started.elapsed();
            }
            #[cfg(feature = "bench-timing")]
            let stage_started = Instant::now();
            encoder.repeat_video_frame_at_pts(frame.pts, fixed_rate_retime)?;
            #[cfg(feature = "bench-timing")]
            {
                stage_times[3] += stage_started.elapsed();
            }
        } else if let Some(conversion) = &mut hdr_conversion {
            let rgb = hdr_rgb.as_mut().expect("HDR composition frame");
            video.copy_rgb48(rgb)?;
            let tiles = effects
                .render_tiles_at_playback_time(frame, playback_ms)
                .map_err(RecordingExportError::Export)?;
            blend_hdr_tiles(rgb, &tiles);
            let converted = hdr_encoded.as_mut().expect("HDR encode frame");
            let reusable = reusable_effect_frame(&mut effects, frame.timestamp_ms);
            #[cfg(feature = "bench-timing")]
            if reusable && playback_unchanged && hdr_valid {
                hdr_reuse_checks += 1;
            }
            let unchanged = reusable
                && playback_unchanged
                && previous_hdr
                    .as_ref()
                    .is_some_and(|previous| hdr_valid && equal_rgb48(rgb, previous));
            if !unchanged {
                #[cfg(feature = "bench-timing")]
                {
                    hdr_conversions += 1;
                }
                crate::codec::ensure_video_frame_writable(converted)?;
                conversion
                    .run(rgb, converted)
                    .map_err(|e| decode(e.to_string()))?;
                cache_rgb48(rgb, &mut previous_hdr, &mut hdr_valid, reusable);
            }
            #[cfg(feature = "bench-timing")]
            {
                stage_times[2] += stage_started.elapsed();
            }
            #[cfg(feature = "bench-timing")]
            let stage_started = Instant::now();
            if !unchanged || fixed_rate_retime {
                encoder.push_prepared_video_frame_ref_at_pts(frame.pts, converted)?;
            }
            #[cfg(feature = "bench-timing")]
            {
                stage_times[3] += stage_started.elapsed();
            }
        } else {
            rgba.resize(config.width as usize * config.height as usize * 4, 0);
            video.copy_rgba(&mut rgba)?;
            effects
                .apply_rgba_at_playback_time(&mut rgba, frame, playback_ms)
                .map_err(RecordingExportError::Export)?;
            // Known changing animations cannot reuse prepared pixels. For
            // other slots, exact complete-pixel equality remains authoritative.
            encoder.set_frame_reuse(
                if playback_unchanged && reusable_effect_frame(&mut effects, frame.timestamp_ms) {
                    if fixed_rate_retime {
                        crate::streaming::VideoFrameReuse::ReuseConversion
                    } else {
                        crate::streaming::VideoFrameReuse::Coalesce
                    }
                } else {
                    crate::streaming::VideoFrameReuse::Encode
                },
            );
            #[cfg(feature = "bench-timing")]
            {
                stage_times[2] += stage_started.elapsed();
            }
            #[cfg(feature = "bench-timing")]
            let stage_started = Instant::now();
            rgba = encoder.push_raster_rgba_frame_at_pts(frame.pts, rgba)?;
            #[cfg(feature = "bench-timing")]
            {
                stage_times[3] += stage_started.elapsed();
            }
        }
        previous_effect_identity = Some(effect_identity);
        previous_effects_idle = reusable_effect_frame(&mut effects, frame.timestamp_ms);
        #[cfg(feature = "bench-timing")]
        let stage_started = Instant::now();
        audio.emit_until(
            (((u128::from(frame.pts + 1) * 48_000) / u128::from(config.fps))
                .min(u128::from(audio_samples))) as u64,
            &mut encoder,
            cancellation,
        )?;
        #[cfg(feature = "bench-timing")]
        {
            stage_times[4] += stage_started.elapsed();
        }
        if previous_telemetry.elapsed().as_millis() >= 50 || frame.index + 1 == total {
            progress(ExportStage::VideoEncode, frame.index + 1, total, started);
            previous_telemetry = Instant::now();
        }
    }
    check_cancel(cancellation)?;
    // A reset/key observation can follow the last visible slot. Validate the
    // bounded store's tail and completion marker before publishing output.
    while inputs.next_event()?.is_some() {
        check_cancel(cancellation)?;
    }
    progress(ExportStage::Finalize, total, total, started);
    #[cfg(feature = "bench-timing")]
    let stage_started = Instant::now();
    audio.emit_until(audio_samples, &mut encoder, cancellation)?;
    let report = encoder.finish_at_duration_us_with_end_pts_cancelable(
        output_duration_us,
        total,
        cancellation,
    )?;
    #[cfg(feature = "bench-timing")]
    {
        let conversions = if output_hdr {
            hdr_conversions
        } else {
            report.timings.cpu_conversions
        };
        eprintln!(
            "deferred prepared pixels: slots={total}, conversions={conversions}, reused={}, equality_checks={}, coalesced={}",
            total.saturating_sub(conversions),
            if output_hdr {
                hdr_reuse_checks
            } else {
                report.timings.cpu_reuse_checks
            },
            total.saturating_sub(report.encoded_frames),
        );
        eprintln!("deferred composition: skipped={skipped_compositions}");
    }
    #[cfg(feature = "bench-timing")]
    let durations = {
        stage_times[5] = stage_started.elapsed();
        let ms = |index: usize| stage_times[index].as_millis().min(u128::from(u64::MAX)) as u64;
        crate::ExportStageDurationsMs {
            plan: ms(0),
            decode: ms(1),
            compose: ms(2),
            video_encode: ms(3),
            audio_encode: ms(4),
            finalize: ms(5),
            mux: 0,
        }
    };
    #[cfg(not(feature = "bench-timing"))]
    let durations = crate::ExportStageDurationsMs::default();
    Ok(ExportResult {
        output_path: config.output_path,
        duration_ms: timeline.duration_ms(),
        format: config.format,
        runtime_report: ExportRuntimeReport {
            video_decoder: Some(video.decoder_name),
            video_encoder: Some(report.video_encoder),
            audio_encoder: report.audio_encoder,
            used_hardware_encode: report.used_hardware_video_encoder,
            stage_durations_ms: durations,
            ..Default::default()
        },
    })
}

fn deferred_software_threads(config: &StreamingEncoderConfig, physical: usize) -> Option<u8> {
    // Windows 4K/30/VeryFast x264 replay: eight threads saved about 19–21% private
    // memory, improved moving throughput and traded about 6–7% static throughput.
    // 4K/60 reaches the same throughput with four workers while using about
    // 41% less private memory than twelve. Preserve explicit settings and
    // unmeasured domains, including the existing eight-worker 30-fps policy.
    (cfg!(windows)
        && config.encode_threads == 0
        && (config.width, config.height) == (3840, 2160)
        && matches!(config.fps, 30 | 60)
        && config.format == crate::ExportFormat::Mp4
        && config.codec == crate::VideoCodec::H264
        && config.software_h264_priority == crate::SoftwareH264Priority::X264First
        && config.video.speed == snow_recording_model::VideoEncodingSpeed::VeryFast)
        .then(|| physical.clamp(1, if config.fps == 60 { 4 } else { 8 }) as u8)
}

fn deferred_conversion_threads(
    config: &StreamingEncoderConfig,
    software_threads: Option<u8>,
    output_hdr: bool,
) -> u8 {
    // Share the measured automatic 4K/60 domain with the encoder policy. HDR
    // retains its P010 scaler; low-core machines retain the serial converter.
    if !output_hdr && config.fps == 60 && software_threads.is_some_and(|threads| threads >= 4) {
        4
    } else {
        0
    }
}

fn reusable_effect_frame(effects: &mut RecordedEffects, at_ms: u64) -> bool {
    !effects.needs_frame(at_ms)
}

fn equal_rgb48(a: &ffmpeg::frame::Video, b: &ffmpeg::frame::Video) -> bool {
    let width = a.width() as usize * 6;
    (0..a.height() as usize).all(|y| {
        a.data(0)[y * a.stride(0)..y * a.stride(0) + width]
            == b.data(0)[y * b.stride(0)..y * b.stride(0) + width]
    })
}

fn cache_rgb48(
    pixels: &mut ffmpeg::frame::Video,
    previous: &mut Option<ffmpeg::frame::Video>,
    valid: &mut bool,
    reusable: bool,
) {
    // Animated frames have no reusable history. Cache the first eligible frame
    // after animation settles before comparing again, avoiding a full copy while busy.
    *valid = false;
    if reusable && let Some(previous) = previous.as_mut() {
        // The composition buffer and history exchange ownership; no canvas
        // copy is needed, and the encoder retains only converted immutable pixels.
        std::mem::swap(pixels, previous);
        *valid = true;
    }
}

fn blend_hdr_tiles(
    frame: &mut ffmpeg::frame::Video,
    tiles: &[snow_recording_effects::surface::Tile],
) {
    use snow_recording_effects::surface::TILE_SIZE;
    let width = frame.width();
    let height = frame.height();
    let stride = frame.stride(0);
    for tile in tiles {
        for y in 0..TILE_SIZE.min(height.saturating_sub(tile.y)) {
            for x in 0..TILE_SIZE.min(width.saturating_sub(tile.x)) {
                let from = ((y * TILE_SIZE + x) * 4) as usize;
                let alpha = tile.pixels[from + 3];
                if alpha == 0 {
                    continue;
                }
                // Sparse surfaces store premultiplied RGB; PQ blending consumes straight alpha.
                let mut color = [0, 0, 0, alpha];
                for (c, channel) in color.iter_mut().enumerate().take(3) {
                    *channel = ((u32::from(tile.pixels[from + c]) * 255 + u32::from(alpha) / 2)
                        / u32::from(alpha))
                    .min(255) as u8;
                }
                let to = (tile.y + y) as usize * stride + (tile.x + x) as usize * 6;
                let pixel: &mut [u8; 6] = (&mut frame.data_mut(0)[to..to + 6])
                    .try_into()
                    .expect("HDR pixel");
                snow_media::color::blend_srgb_into_pq(pixel, color);
            }
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct DecodedSdrColors {
    matrix: i32,
    full_range: bool,
}
impl DecodedSdrColors {
    fn from_frame(frame: &ffmpeg::frame::Video) -> Self {
        let matrix = match frame.color_space() as i32 {
            // App-owned SDR recordings use BT.709. Untagged legacy sources
            // must not inherit libswscale's dimension-dependent BT.601 default.
            0 | 2 | 3 => ffmpeg::ffi::SWS_CS_ITU709,
            matrix => matrix,
        };
        Self {
            matrix,
            full_range: crate::hdr::is_rgb(frame.format())
                || frame.color_range() == ffmpeg::color::Range::JPEG,
        }
    }
    fn configure(self, conversion: &mut ffmpeg::software::scaling::Context) -> Result<()> {
        // SAFETY: the exclusively owned initialized scaler remains live for
        // this call, and FFmpeg owns the immutable coefficient table.
        let status = unsafe {
            let matrix = ffmpeg::ffi::sws_getCoefficients(self.matrix);
            ffmpeg::ffi::sws_setColorspaceDetails(
                conversion.as_mut_ptr(),
                matrix,
                i32::from(self.full_range),
                matrix,
                1,
                0,
                1 << 16,
                1 << 16,
            )
        };
        if status < 0 {
            return Err(decode(format!(
                "source SDR color conversion: {}",
                ffmpeg::Error::from(status)
            )));
        }
        Ok(())
    }
}

pub(crate) struct SequentialVideoSource {
    input: ffmpeg::format::context::Input,
    decoder: ffmpeg::decoder::Video,
    stream_index: usize,
    time_base: ffmpeg::Rational,
    source_offset_us: i64,
    seek_index: bool,
    index: VideoIndexReader,
    timeline: FinalizedTimeline,
    eof: bool,
    next: Option<(u64, ffmpeg::frame::Video)>,
    current: Option<ffmpeg::frame::Video>,
    converted: ffmpeg::frame::Video,
    tone_mapped: RasterBuffer,
    tone_mapped_valid: bool,
    conversion: ffmpeg::software::scaling::Context,
    sdr_colors: Option<DecodedSdrColors>,
    sdr_color_override: Option<DecodedSdrColors>,
    output: (u32, u32),
    hdr: bool,
    decoder_name: String,
}
impl SequentialVideoSource {
    pub(crate) fn recorded_sdr_colors(&mut self) -> Result<()> {
        // Retain the camera-track replay policy independently of the decoded
        // screen source's explicit limited/full-range metadata.
        let colors = DecodedSdrColors {
            matrix: ffmpeg::ffi::SWS_CS_ITU709,
            full_range: true,
        };
        colors.configure(&mut self.conversion)?;
        self.sdr_color_override = Some(colors);
        self.sdr_colors = Some(colors);
        Ok(())
    }
    fn open(
        path: &Path,
        timeline: FinalizedTimeline,
        hdr: bool,
        coded: (u32, u32),
        output: (u32, u32),
        cancellation: &CancellationToken,
    ) -> Result<Self> {
        let index = VideoIndexReader::from_bundle(path)?
            .ok_or_else(|| decode("clean source video index is missing"))?;
        Self::open_indexed(path, index, timeline, hdr, coded, output, cancellation)
    }
    pub(crate) fn open_indexed(
        path: &Path,
        mut index: VideoIndexReader,
        timeline: FinalizedTimeline,
        hdr: bool,
        coded: (u32, u32),
        output: (u32, u32),
        cancellation: &CancellationToken,
    ) -> Result<Self> {
        check_cancel(cancellation)?;
        let input =
            ffmpeg::format::input(path).map_err(|e| decode(format!("open clean source: {e}")))?;
        let stream = input
            .streams()
            .best(ffmpeg::media::Type::Video)
            .ok_or_else(|| decode("source video stream missing"))?;
        let stream_index = stream.index();
        if index.remaining() > timeline.frame_count() {
            return Err(decode(
                "clean source index has more images than output slots",
            ));
        }
        let first = index
            .next_frame()?
            .ok_or_else(|| decode("clean source video index is empty"))?;
        let first_pts =
            (u128::from(first.timestamp_ms) * u128::from(timeline.fps())).div_ceil(1000);
        let source_offset_us = (first_pts * 1_000_000 / u128::from(timeline.fps())) as i64
            - stream
                .start_time()
                .max(0)
                .rescale(stream.time_base(), (1, 1_000_000));
        index.seek_before(0)?;
        let time_base = stream.time_base();
        let decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
            .map_err(|e| decode(e.to_string()))?
            .decoder()
            .video()
            .map_err(|e| decode(e.to_string()))?;
        if (decoder.width(), decoder.height()) != coded || output.0 > coded.0 || output.1 > coded.1
        {
            return Err(decode("source coded dimensions disagree with metadata"));
        }
        let conversion = ffmpeg::software::scaling::Context::get(
            decoder.format(),
            coded.0,
            coded.1,
            if hdr {
                ffmpeg::format::Pixel::RGB48LE
            } else {
                ffmpeg::format::Pixel::RGBA
            },
            coded.0,
            coded.1,
            ffmpeg::software::scaling::Flags::BICUBIC,
        )
        .map_err(|e| decode(e.to_string()))?;
        let decoder_name = decoder
            .codec()
            .map(|c| c.name().to_string())
            .unwrap_or_default();
        let mut source = Self {
            input,
            decoder,
            stream_index,
            time_base,
            source_offset_us,
            seek_index: false,
            index,
            timeline,
            eof: false,
            next: None,
            current: None,
            converted: ffmpeg::frame::Video::empty(),
            tone_mapped: RasterBuffer::default(),
            tone_mapped_valid: false,
            conversion,
            sdr_colors: None,
            sdr_color_override: None,
            output,
            hdr,
            decoder_name,
        };
        if hdr {
            crate::hdr::scaler_colors(&mut source.conversion, true, true)?;
        }
        source.next = source.decode_next(cancellation)?;
        Ok(source)
    }
    fn decode_next(
        &mut self,
        cancellation: &CancellationToken,
    ) -> Result<Option<(u64, ffmpeg::frame::Video)>> {
        loop {
            check_cancel(cancellation)?;
            let mut frame = ffmpeg::frame::Video::empty();
            match self.decoder.receive_frame(&mut frame) {
                Ok(()) => {
                    if self.seek_index {
                        let at = frame
                            .timestamp()
                            .unwrap_or(0)
                            .rescale(self.time_base, (1, 1_000_000))
                            .saturating_add(self.source_offset_us)
                            .max(0) as u64;
                        // Reconnect the decoder checkpoint to the authoritative admission index.
                        self.index.seek_before((at + 500) / 1000)?;
                        self.seek_index = false;
                    }
                    let entry = self.index.next_frame()?.ok_or_else(|| {
                        decode("clean source contains more decoded images than its index")
                    })?;
                    // Containers and hardware encoders can normalize the first
                    // image timestamp. Indexed active-time admission is authoritative.
                    let pts = (u128::from(entry.timestamp_ms) * u128::from(self.timeline.fps()))
                        .div_ceil(1000);
                    if pts >= u128::from(self.timeline.frame_count())
                        || pts * 1000 / u128::from(self.timeline.fps())
                            != u128::from(entry.timestamp_ms)
                        || entry
                            .timestamp_ms
                            .saturating_add(u64::from(entry.duration_ms))
                            > self.timeline.duration_ms()
                    {
                        return Err(decode(
                            "clean source index is outside its finalized output grid",
                        ));
                    }
                    return Ok(Some((pts as u64, frame)));
                }
                Err(ffmpeg::Error::Eof) => {
                    if self.index.remaining() != 0 {
                        return Err(decode(
                            "clean source has fewer decoded images than its index",
                        ));
                    }
                    return Ok(None);
                }
                Err(error) if crate::ffmpeg_util::is_eagain(&error) && !self.eof => {}
                Err(error) => return Err(decode(format!("source decode: {error}"))),
            }
            loop {
                check_cancel(cancellation)?;
                let mut packet = ffmpeg::Packet::empty();
                match packet.read(&mut self.input) {
                    Ok(()) if packet.stream() == self.stream_index => {
                        self.decoder
                            .send_packet(&packet)
                            .map_err(|e| decode(e.to_string()))?;
                        break;
                    }
                    Ok(()) => continue,
                    Err(ffmpeg::Error::Eof) => {
                        self.decoder.send_eof().map_err(|e| decode(e.to_string()))?;
                        self.eof = true;
                        break;
                    }
                    Err(error) => return Err(decode(format!("source demux: {error}"))),
                }
            }
        }
    }
    pub(crate) fn seek_to(&mut self, pts: u64, cancellation: &CancellationToken) -> Result<()> {
        if pts < u64::from(self.timeline.fps()) {
            return Ok(());
        }
        check_cancel(cancellation)?;
        let at = (u128::from(pts) * 1_000_000 / u128::from(self.timeline.fps())) as i64;
        let seek = (at - self.source_offset_us).max(0);
        self.input
            .seek(seek, ..seek)
            .map_err(|e| decode(e.to_string()))?;
        self.decoder.flush();
        self.current = None;
        self.eof = false;
        self.seek_index = true;
        self.next = self.decode_next(cancellation)?;
        Ok(())
    }
    pub(crate) fn advance(&mut self, pts: u64, cancellation: &CancellationToken) -> Result<bool> {
        let mut changed = false;
        while self
            .next
            .as_ref()
            .is_some_and(|(next, _)| *next <= pts || self.current.is_none())
        {
            let (_, frame) = self.next.take().expect("pending decoded frame");
            self.current = Some(frame);
            changed = true;
            self.next = self.decode_next(cancellation)?;
        }
        if changed {
            self.tone_mapped_valid = false;
            let frame = self.current.as_ref().expect("decoded source");
            if !self.hdr {
                let colors = self
                    .sdr_color_override
                    .unwrap_or_else(|| DecodedSdrColors::from_frame(frame));
                if self.sdr_colors != Some(colors) {
                    colors.configure(&mut self.conversion)?;
                    self.sdr_colors = Some(colors);
                }
            }
            self.conversion
                .run(frame, &mut self.converted)
                .map_err(|e| decode(e.to_string()))?;
        }
        if self.current.is_none() {
            return Err(decode("clean source contains no video frames"));
        }
        Ok(changed)
    }
    pub(crate) fn copy_rgba(&mut self, output: &mut [u8]) -> Result<()> {
        if self.hdr && self.tone_mapped_valid {
            output.copy_from_slice(&self.tone_mapped);
            return Ok(());
        }
        let stride = self.converted.stride(0);
        for y in 0..self.output.1 as usize {
            let row = &self.converted.data(0)
                [y * stride..y * stride + self.output.0 as usize * if self.hdr { 6 } else { 4 }];
            let dest =
                &mut output[y * self.output.0 as usize * 4..(y + 1) * self.output.0 as usize * 4];
            if self.hdr {
                snow_media::color::tone_map_rgb48_row(row, dest).map_err(decode)?;
            } else {
                dest.copy_from_slice(row);
            }
        }
        if self.hdr {
            // Tone mapping is source work. Repeated composition (e.g. a moving
            // playback bar over a held HDR picture) must not redo it per slot.
            // Allocate lazily so HDR-preserving exports need no SDR canvas.
            self.tone_mapped.resize(output.len(), 0);
            self.tone_mapped.copy_from_slice(output);
            self.tone_mapped_valid = true;
        }
        Ok(())
    }
    fn copy_rgb48(&self, result: &mut ffmpeg::frame::Video) -> Result<()> {
        if !self.hdr {
            return Err(decode("HDR source expected"));
        }
        crate::codec::ensure_video_frame_writable(result)?;
        let from_stride = self.converted.stride(0);
        let to_stride = result.stride(0);
        for y in 0..self.output.1 as usize {
            let len = self.output.0 as usize * 6;
            result.data_mut(0)[y * to_stride..y * to_stride + len]
                .copy_from_slice(&self.converted.data(0)[y * from_stride..y * from_stride + len]);
        }
        crate::hdr::frame(result, true);
        Ok(())
    }
}

struct PcmTrack {
    file: BufReader<File>,
    remaining: u64,
    id: String,
    channels: usize,
    bytes: Vec<u8>,
    samples: Vec<i16>,
}

enum ExportAudioReplay {
    Original(AudioReplay),
    Retimed {
        reader: Box<crate::tempo::RetimedAudioReader>,
        output: Vec<crate::StreamingAudioConfig>,
    },
}

impl ExportAudioReplay {
    fn open(
        path: &Path,
        footer: &RecordingBundleFooter,
        config: &StreamingEncoderConfig,
        range: snow_recording_model::TrimRange,
        speed: PlaybackSpeed,
    ) -> Result<Self> {
        if speed.percent() == 100 || config.audio.is_empty() {
            let mut original = AudioReplay::open(path, footer, config)?;
            original.skip_samples(range.sample_offset(48_000))?;
            return Ok(Self::Original(original));
        }
        let mut source = crate::clip::ClipSource::open(path, None)?;
        source.config.audio = config.audio.clone();
        Ok(Self::Retimed {
            reader: Box::new(crate::tempo::RetimedAudioReader::open(
                &source, range, speed,
            )?),
            output: config.audio.clone(),
        })
    }

    fn emit_until(
        &mut self,
        end: u64,
        encoder: &mut StreamingEncoder,
        cancellation: &CancellationToken,
    ) -> Result<()> {
        match self {
            Self::Original(original) => original.emit_until(end, encoder, cancellation),
            Self::Retimed { reader, output } => {
                while reader.position() < end.min(reader.total_samples()) {
                    check_cancel(cancellation)?;
                    let position = reader.position();
                    let count = (end.min(reader.total_samples()) - position).min(960) as usize;
                    let tracks = reader.read(count, cancellation)?;
                    for (track, samples) in output.iter().zip(tracks) {
                        encoder.push_audio_track_pcm_i16_at_frame(
                            &track.track_id,
                            position,
                            samples,
                        )?;
                    }
                }
                Ok(())
            }
        }
    }
}

struct AudioReplay {
    sources: Vec<PcmTrack>,
    output: Vec<crate::StreamingAudioConfig>,
    next: u64,
    mixed: Vec<i16>,
}
impl AudioReplay {
    fn skip_samples(&mut self, samples: u64) -> Result<()> {
        for source in &mut self.sources {
            let bytes = samples
                .saturating_mul(source.channels as u64 * 2)
                .min(source.remaining);
            source.file.seek(SeekFrom::Current(bytes as i64))?;
            source.remaining -= bytes;
        }
        Ok(())
    }
    fn open(
        path: &Path,
        footer: &RecordingBundleFooter,
        config: &StreamingEncoderConfig,
    ) -> Result<Self> {
        let mut sources = Vec::new();
        if !config.audio.is_empty() {
            for track in footer.manifest.audio_tracks.iter().filter(|t| t.recorded) {
                if track.sample_rate_hz != 48_000 || track.channels != 2 {
                    return Err(decode("deferred PCM must be 48 kHz stereo"));
                }
                let asset = footer
                    .asset(BundleAssetKind::AudioTrack, Some(&track.asset_id))
                    .ok_or_else(|| decode("source PCM asset missing"))?;
                let mut file = File::open(path)?;
                file.seek(SeekFrom::Start(asset.offset))?;
                sources.push(PcmTrack {
                    file: BufReader::with_capacity(crate::clip::audio::PCM_READ_BUFFER_BYTES, file),
                    remaining: asset.len,
                    id: track.track_id.clone(),
                    channels: usize::from(track.channels),
                    bytes: Vec::new(),
                    samples: Vec::new(),
                });
            }
        }
        Ok(Self {
            sources,
            output: config.audio.clone(),
            next: 0,
            mixed: Vec::new(),
        })
    }
    fn emit_until(
        &mut self,
        end: u64,
        encoder: &mut StreamingEncoder,
        cancellation: &CancellationToken,
    ) -> Result<()> {
        while self.next < end && !self.output.is_empty() {
            check_cancel(cancellation)?;
            let count = (end - self.next).min(480) as usize;
            for source in &mut self.sources {
                source.bytes.resize(count * source.channels * 2, 0);
                source.bytes.fill(0);
                let read = (source.remaining as usize).min(source.bytes.len());
                source.file.read_exact(&mut source.bytes[..read])?;
                source.remaining -= read as u64;
                source.samples.clear();
                source.samples.extend(
                    source
                        .bytes
                        .chunks_exact(2)
                        .map(|v| i16::from_le_bytes([v[0], v[1]])),
                );
            }
            for track in &self.output {
                self.mixed.resize(count * 2, 0);
                self.mixed.fill(0);
                for source in &self.sources {
                    if track.track_id == "mixed" || track.track_id == source.id {
                        for (dst, src) in self.mixed.iter_mut().zip(&source.samples) {
                            *dst = dst.saturating_add(*src);
                        }
                    }
                }
                encoder.push_audio_track_pcm_i16_at_frame(
                    &track.track_id,
                    self.next,
                    &self.mixed,
                )?;
            }
            self.next += count as u64;
        }
        Ok(())
    }
}

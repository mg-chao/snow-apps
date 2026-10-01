use crate::codec::*;
use crate::resize::{NearestResizePlan, resize_rgba_fast_into};
use snow_core::cancellation::CancellationToken;
use std::cell::Cell;
use std::collections::HashMap;
use std::ffi::OsString;
use std::ffi::c_void;
use std::fs;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, Sender};
use ffmpeg_next as ffmpeg;
use rayon::prelude::*;
use snow_audio_recorder::duration_to_frames_round;
use snow_recording_model::{
    AudioSampleFormat, BundleAssetKind, CursorShapeCompositionMode, CursorShapeRecord,
    FinalizedTimeline, MouseStore, RecordingArtifact, RecordingBundleFooter, SessionManifest,
    SourceDescription, StoredFrame, decode_mouse_records, read_recording_bundle_footer,
};

use crate::config::{
    ExportAudioTrackRequest, ExportExecutionMode, ExportFormat, ExportPerformanceConfig,
    ExportRequest, MouseEditConfig, VideoCodec, VideoEncodeConfig, VideoEncodingSpeed,
};
use crate::error::{RecordingExportError as ScreenRecorderError, Result};
use crate::export::{
    ExportPathKind, ExportProgress, ExportResult, ExportRuntimeReport, ExportStage,
    ExportStageDurationsMs, ExportTask,
};
use crate::ffmpeg_util::{copy_rgba_into_frame, ensure_ffmpeg_initialized, is_eagain};
use crate::streaming::{StreamingAudioConfig, StreamingEncoder, StreamingEncoderConfig};

const VIDEO_INDEX_MAGIC: &[u8] = b"SVIDX\0\0";

#[derive(Clone, Debug, Default)]
struct ExportCodecTelemetry {
    video_decoder: Option<String>,
    video_encoder: Option<String>,
    audio_encoder: Option<String>,
    used_hardware_decode: bool,
    used_hardware_encode: bool,
    stage_durations_ms: ExportStageDurationsMs,
}

pub struct EditingSession {
    artifact: RecordingArtifact,
    manifest: SessionManifest,
    bundle_footer: RecordingBundleFooter,
}

impl EditingSession {
    pub fn open(artifact: RecordingArtifact) -> Result<Self> {
        let bundle_footer = read_recording_bundle_footer(&artifact.bundle_path)?;
        let manifest = bundle_footer.manifest.clone();
        validate_bundle_artifact(&artifact, &manifest, &bundle_footer)?;

        Ok(Self {
            artifact,
            manifest,
            bundle_footer,
        })
    }

    pub fn export_request(&self) -> ExportRequest {
        if let Ok(config) =
            crate::deferred::read_deferred_output_settings(&self.artifact.bundle_path)
        {
            let mouse =
                snow_recording_model::read_bundle_render_metadata(&self.artifact.bundle_path)
                    .ok()
                    .flatten()
                    .and_then(|metadata| {
                        SourceDescription::deferred(&self.manifest, &metadata).ok()
                    })
                    .and_then(|source| source.render)
                    .map(|render| {
                        let effects = render.effects;
                        MouseEditConfig {
                            visible: effects.show_cursor,
                            trail_enabled: effects.mouse_trail_rgba[3] != 0,
                            trail_window_ms: effects.mouse_trail_duration_ms,
                            trail_max_alpha: effects.mouse_trail_rgba[3],
                            trail_color: effects.mouse_trail_rgba[..3].try_into().unwrap(),
                            click_enabled: effects.mouse_click_rgba[3] != 0,
                            ..MouseEditConfig::default()
                        }
                    })
                    .unwrap_or_default();
            return ExportRequest {
                audio_tracks: self
                    .manifest
                    .audio_tracks
                    .iter()
                    .map(|track| ExportAudioTrackRequest {
                        track_id: track.track_id.clone(),
                        enabled: config.audio.iter().any(|output| {
                            output.track_id == "mixed" || output.track_id == track.track_id
                        }),
                        ..ExportAudioTrackRequest::default()
                    })
                    .collect(),
                audio_output: crate::config::ExportAudioOutputConfig {
                    bitrate_kbps: config.audio.first().map_or(192, |audio| audio.bitrate_kbps),
                },
                mouse,
                output_path: config.output_path,
                format: config.format,
                video: config.video,
                codec: config.codec,
                prefer_hardware_h264: config.prefer_hardware_h264,
                performance: ExportPerformanceConfig {
                    mode: config.execution_mode,
                    software_h264_priority: config.software_h264_priority,
                    encode_threads: config.encode_threads,
                    ..ExportPerformanceConfig::default()
                },
                ..ExportRequest::default()
            };
        }
        ExportRequest {
            playback_speed: 1.0,
            audio_tracks: self
                .manifest
                .audio_tracks
                .iter()
                .map(|track| ExportAudioTrackRequest {
                    track_id: track.track_id.clone(),
                    enabled: true,
                    ..ExportAudioTrackRequest::default()
                })
                .collect(),
            audio_output: crate::config::ExportAudioOutputConfig::default(),
            mouse: MouseEditConfig {
                visible: true,
                ..MouseEditConfig::default()
            },
            format: ExportFormat::Mp4,
            output_path: self
                .manifest
                .output_dir
                .join(format!("{}.mp4", self.manifest.session_id)),
            video: VideoEncodeConfig {
                quality: 60,
                speed: VideoEncodingSpeed::UltraFast,
            },
            codec: self.manifest.video_codec,
            prefer_hardware_h264: false,
            performance: crate::config::ExportPerformanceConfig::default(),
            maximum_width: None,
            maximum_height: None,
            target_fps: None,
        }
    }

    pub fn export(self, request: ExportRequest) -> Result<ExportResult> {
        self.export_with_request(
            request,
            None,
            Arc::new(AtomicBool::new(false)),
            CancellationToken::default(),
        )
    }

    pub fn export_async(self, request: ExportRequest) -> Result<ExportTask> {
        let (progress_tx, progress_rx) = ProgressReporter::channel();
        let cancellation = CancellationToken::default();
        let worker_cancellation = cancellation.clone();
        let cancel_flag = Arc::new(AtomicBool::new(false));
        let cancel_for_worker = Arc::clone(&cancel_flag);
        let handle = thread::Builder::new()
            .name("snow-screen-recorder-export".to_string())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                self.export_with_request(
                    request,
                    Some(progress_tx),
                    cancel_for_worker,
                    worker_cancellation,
                )
            })
            .map_err(|err| ScreenRecorderError::Io(std::io::Error::other(err)))?;

        Ok(ExportTask::new(
            cancel_flag,
            cancellation,
            progress_rx,
            handle,
        ))
    }

    fn export_with_request(
        self,
        request: ExportRequest,
        progress_tx: Option<Arc<ProgressReporter>>,
        cancel_flag: Arc<AtomicBool>,
        cancellation: CancellationToken,
    ) -> Result<ExportResult> {
        request
            .validate()
            .map_err(ScreenRecorderError::InvalidConfig)?;
        let request = normalize_export_request(request, &self.manifest);

        if let Some(metadata) =
            snow_recording_model::read_bundle_render_metadata(&self.artifact.bundle_path)?
        {
            let source = SourceDescription::deferred(&self.manifest, &metadata)?;
            return self.export_finalized_source(request, source, &progress_tx, &cancellation);
        }
        self.export_inner(request, &progress_tx, &cancel_flag, &cancellation)
    }

    fn export_finalized_source(
        self,
        request: ExportRequest,
        source: SourceDescription,
        progress_tx: &Option<Arc<ProgressReporter>>,
        cancellation: &CancellationToken,
    ) -> Result<ExportResult> {
        let metadata = snow_recording_model::RenderMetadata {
            render: source.render.ok_or_else(|| {
                ScreenRecorderError::InvalidConfig(
                    "finalized source rendering policy is missing".into(),
                )
            })?,
            timeline: source.timeline,
            coded_width: source.coded_width,
            coded_height: source.coded_height,
        };
        if (request.playback_speed - 1.0).abs() > f32::EPSILON
            || request
                .target_fps
                .is_some_and(|fps| fps != metadata.timeline.fps())
            || request
                .maximum_width
                .is_some_and(|width| width < metadata.render.output_width)
            || request
                .maximum_height
                .is_some_and(|height| height < metadata.render.output_height)
        {
            return Err(ScreenRecorderError::InvalidConfig(
                "a finalized recording uses its captured timeline and output canvas".into(),
            ));
        }
        let mut config =
            crate::deferred::read_deferred_output_settings(&self.artifact.bundle_path)?;
        config.output_path = request.output_path;
        config.format = request.format;
        config.video = request.video;
        config.codec = request.codec;
        config.prefer_hardware_h264 = request.prefer_hardware_h264;
        config.execution_mode = request.performance.mode;
        config.software_h264_priority = request.performance.software_h264_priority;
        config.encode_threads = request.performance.encode_threads;
        if config.format.is_animated_image() {
            config.audio.clear();
        }
        // Looping, fonts, labels, effects and audio routing remain the immutable
        // source snapshot because the legacy request cannot represent them.
        let result = crate::deferred::render_bundle(
            &self.artifact.bundle_path,
            config,
            metadata,
            cancellation,
            |stage, completed, total, started| {
                let percent = if stage == ExportStage::Plan {
                    0.0
                } else if stage == ExportStage::Finalize {
                    98.0
                } else {
                    5.0 + (completed as f64 / total.max(1) as f64 * 90.0) as f32
                };
                let fps = completed as f32 / started.elapsed().as_secs_f32().max(0.001);
                emit_progress(progress_tx, stage, percent, fps, None);
            },
        )?;
        emit_progress(progress_tx, ExportStage::Finalize, 100.0, 0.0, Some(0));
        Ok(result)
    }

    fn export_inner(
        self,
        mut request: ExportRequest,
        progress_tx: &Option<Arc<ProgressReporter>>,
        cancel_flag: &Arc<AtomicBool>,
        cancellation: &CancellationToken,
    ) -> Result<ExportResult> {
        let output_path = request.output_path.clone();
        let output_directory = output_path
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
            .unwrap_or_else(|| Path::new("."));
        fs::create_dir_all(output_directory)?;

        let mut staging_suffix = OsString::new();
        if let Some(extension) = output_path.extension() {
            staging_suffix.push(".");
            staging_suffix.push(extension);
        }
        let staging_path = tempfile::Builder::new()
            .prefix(".snow-recording-export-")
            .suffix(&staging_suffix)
            .tempfile_in(output_directory)?
            .into_temp_path();
        request.output_path = staging_path.to_path_buf();

        let mut result = self.export_inner_impl(request, progress_tx, cancel_flag, cancellation)?;
        cancellation
            .commit(|| staging_path.persist(&output_path))
            .map_err(|_| ScreenRecorderError::ExportCanceled)?
            .map_err(|err| ScreenRecorderError::Io(err.error))?;
        result.output_path = output_path;
        emit_progress(progress_tx, ExportStage::Finalize, 100.0, 0.0, Some(0));
        Ok(result)
    }

    fn export_inner_impl(
        self,
        request: ExportRequest,
        progress_tx: &Option<Arc<ProgressReporter>>,
        cancel_flag: &Arc<AtomicBool>,
        cancellation: &CancellationToken,
    ) -> Result<ExportResult> {
        check_canceled(cancel_flag)?;
        emit_progress(progress_tx, ExportStage::Plan, 1.0, 0.0, None);

        if let Some(parent) = request.output_path.parent()
            && !parent.as_os_str().is_empty()
        {
            fs::create_dir_all(parent)?;
        }

        let output_path = request.output_path.clone();
        let mut runtime_report = ExportRuntimeReport::default();
        let wants_mouse_overlay =
            request.mouse.visible || request.mouse.click_enabled || request.mouse.trail_enabled;
        let export_fps = choose_export_fps(self.manifest.fps, request.format, request.target_fps);
        let source_index = read_video_index(&self.artifact.bundle_path, &self.bundle_footer)?;
        if source_index.is_empty() {
            return Err(ScreenRecorderError::Export(
                "no frames available for export".to_string(),
            ));
        }

        let (resolved_width, resolved_height) =
            if self.manifest.width > 0 && self.manifest.height > 0 {
                (self.manifest.width, self.manifest.height)
            } else {
                probe_intermediate_video_dimensions(
                    &self.artifact.local_paths.video_intermediate_path,
                    self.manifest.width.max(1),
                    self.manifest.height.max(1),
                )
            };
        // The legacy version adapter resolves older dimension-less manifests;
        // timing and media policy then use the same normalized source model.
        let mut source_manifest = self.manifest.clone();
        source_manifest.width = resolved_width;
        source_manifest.height = resolved_height;
        let timeline = FinalizedTimeline::new(
            video_index_duration_ms(&source_index).max(1),
            self.manifest.fps,
        )
        .map_err(ScreenRecorderError::InvalidConfig)?;
        let source = SourceDescription::legacy(&source_manifest, timeline)?;
        let (source_w, source_h) = (source.logical_width, source.logical_height);
        let (output_w, output_h) = output_dimensions(
            source_w,
            source_h,
            request.maximum_width,
            request.maximum_height,
            request.format.requires_even_dimensions(),
        );
        let needs_resize = source_w != output_w || source_h != output_h;

        emit_progress(progress_tx, ExportStage::Decode, 2.0, 0.0, None);
        let should_load_mouse = wants_mouse_overlay;
        let mut mouse_tracks = if should_load_mouse {
            let mouse_store = read_mouse_store(&self.artifact.bundle_path, &self.bundle_footer)?;
            build_mouse_tracks(mouse_store)
        } else {
            MouseTracks::default()
        };
        let needs_overlay = !mouse_tracks.samples.is_empty() && wants_mouse_overlay;
        let source_hdr = source.media.color == snow_media::ColorDescription::HDR10;
        if source_hdr && needs_overlay && request.mouse.visible {
            validate_hdr_cursor_shapes(&mouse_tracks)?;
        }
        if !source_hdr
            && matches!(
                source.media.color.transfer,
                snow_media::TransferFunction::Pq
                    | snow_media::TransferFunction::Hlg
                    | snow_media::TransferFunction::Linear
            )
        {
            return Err(ScreenRecorderError::InvalidConfig(
                "unsupported editable source color; expected SDR or canonical HDR10".into(),
            ));
        }

        let retime =
            build_retime_plan_from_index(&source_index, request.playback_speed, export_fps)?;
        if retime.frame_count() == 0 {
            return Err(ScreenRecorderError::Export(
                "retiming produced no frames".to_string(),
            ));
        }

        let duration_ms = retime.output_duration_ms().max(1);

        emit_progress(progress_tx, ExportStage::Compose, 25.0, 0.0, None);
        let mixed_audio = if request.format.is_animated_image() {
            None
        } else {
            build_mixed_audio(
                &self.artifact.bundle_path,
                &self.bundle_footer,
                &self.manifest,
                &request,
                duration_ms,
            )?
        };
        emit_progress(progress_tx, ExportStage::Compose, 35.0, 0.0, None);
        check_canceled(cancel_flag)?;

        if needs_overlay {
            retime_mouse_tracks(&mut mouse_tracks, request.playback_speed)?;
        }
        let output_len = output_w as usize * output_h as usize * 4;
        let mut overlay_tracks = if needs_overlay && needs_resize {
            Some(scale_mouse_tracks(
                &mouse_tracks,
                source_w,
                source_h,
                output_w,
                output_h,
            ))
        } else {
            None
        };
        if needs_overlay {
            if let Some(tracks) = overlay_tracks.as_mut() {
                compile_mouse_trail_segments(tracks, &request.mouse, output_w, output_h);
            } else {
                compile_mouse_trail_segments(&mut mouse_tracks, &request.mouse, source_w, source_h);
            }
        }
        let overlay_tracks_ref = overlay_tracks.as_ref().unwrap_or(&mouse_tracks);

        if !needs_overlay || source_hdr {
            emit_progress(progress_tx, ExportStage::Decode, 20.0, 0.0, None);
            let telemetry = match export_video_generated_from_source(
                &self.artifact.local_paths.video_intermediate_path,
                &output_path,
                &retime,
                source_hdr,
                needs_overlay.then_some((overlay_tracks_ref, &request.mouse)),
                output_w,
                output_h,
                export_fps,
                request.format,
                request.codec,
                request.prefer_hardware_h264,
                mixed_audio.as_ref(),
                request.audio_output.bitrate_kbps.max(8),
                &request.video,
                &request.performance,
                cancel_flag,
                Some(cancellation),
                progress_tx,
            ) {
                Ok(telemetry) => telemetry,
                Err(err) => {
                    if matches!(err, ScreenRecorderError::ExportCanceled) {
                        let _ = fs::remove_file(&output_path);
                    }
                    return Err(err);
                }
            };
            runtime_report.path = ExportPathKind::FullTranscode;
            apply_codec_telemetry(&mut runtime_report, telemetry);
            emit_progress(progress_tx, ExportStage::Finalize, 98.0, 0.0, Some(0));
            return Ok(ExportResult {
                output_path,
                duration_ms,
                format: request.format,
                runtime_report,
            });
        }

        emit_progress(progress_tx, ExportStage::Decode, 20.0, 0.0, None);
        match export_video_generated_from_source_with_overlay(
            &self.artifact.local_paths.video_intermediate_path,
            &output_path,
            &retime,
            output_w,
            output_h,
            export_fps,
            request.format,
            request.codec,
            request.prefer_hardware_h264,
            mixed_audio.as_ref(),
            request.audio_output.bitrate_kbps.max(8),
            &request.video,
            &request.performance,
            overlay_tracks_ref,
            &request.mouse,
            cancel_flag,
            Some(cancellation),
            progress_tx,
        ) {
            Ok(telemetry) => {
                runtime_report.path = ExportPathKind::FullTranscode;
                apply_codec_telemetry(&mut runtime_report, telemetry);
                emit_progress(progress_tx, ExportStage::Finalize, 98.0, 0.0, Some(0));
                return Ok(ExportResult {
                    output_path,
                    duration_ms,
                    format: request.format,
                    runtime_report,
                });
            }
            Err(ScreenRecorderError::ExportCanceled) => {
                let _ = fs::remove_file(&output_path);
                return Err(ScreenRecorderError::ExportCanceled);
            }
            Err(_) => {
                // Fall back to staged RGBA generation path when the source-overlay fast path fails.
            }
        }

        let decode_queue_depth = effective_decode_queue_depth(
            request.performance.queue_depth,
            request.performance.memory_budget_mb,
            source_w,
            source_h,
        );
        let required_source_indices = collect_required_source_indices(retime.source_indices());
        let mut frame_source = StreamingVideoFrameSource::spawn(
            &self.artifact.local_paths.video_intermediate_path,
            required_source_indices,
            self.manifest.fps,
            decode_queue_depth,
            request.performance.decode_threads,
            Arc::clone(cancel_flag),
        )?;
        emit_progress(progress_tx, ExportStage::Decode, 20.0, 0.0, None);
        check_canceled(cancel_flag)?;

        let resize_plan =
            needs_resize.then(|| NearestResizePlan::new(source_w, source_h, output_w, output_h));
        let process_pool = build_process_pool(request.performance.process_threads);
        let mut overlay_state = OverlaySearchState::default();
        // Overlay exports often repeat the same source frame when slowing playback.
        // Cache the resized base frame so repeated source indices avoid redundant scaling.
        let mut resized_cache_key = None::<(usize, u32, u32)>;
        let mut resized_cache = if needs_resize {
            vec![0u8; output_len]
        } else {
            Vec::new()
        };

        let telemetry = match export_video_generated(
            &output_path,
            output_w,
            output_h,
            retime.timeline,
            request.format,
            request.codec,
            request.prefer_hardware_h264,
            mixed_audio.as_ref(),
            request.audio_output.bitrate_kbps.max(8),
            &request.video,
            &request.performance,
            cancel_flag,
            Some(cancellation),
            progress_tx,
            |index, output_rgba| {
                check_canceled(cancel_flag)?;
                debug_assert_eq!(output_rgba.len(), output_len);

                let src_idx = retime.source_index(index);
                let source = frame_source.frame_at(src_idx)?;
                let output_ts = retime.timestamp_ms(index);
                prepare_overlay_base_rgba(
                    source,
                    src_idx,
                    output_w,
                    output_h,
                    resize_plan.as_ref(),
                    process_pool.as_ref(),
                    &mut resized_cache_key,
                    &mut resized_cache,
                    output_rgba,
                );
                let (overlay_w, overlay_h) = if needs_resize {
                    (output_w, output_h)
                } else {
                    (source.width, source.height)
                };
                apply_mouse_overlays_rgba_incremental(
                    output_ts,
                    overlay_w,
                    overlay_h,
                    output_rgba,
                    overlay_tracks_ref,
                    &request.mouse,
                    &mut overlay_state,
                );
                Ok(())
            },
        ) {
            Ok(telemetry) => telemetry,
            Err(err) => {
                if matches!(err, ScreenRecorderError::ExportCanceled) {
                    let _ = fs::remove_file(&output_path);
                }
                return Err(err);
            }
        };
        runtime_report.path = ExportPathKind::FullTranscode;
        apply_codec_telemetry(&mut runtime_report, telemetry);
        emit_progress(progress_tx, ExportStage::Finalize, 98.0, 0.0, Some(0));

        Ok(ExportResult {
            output_path,
            duration_ms,
            format: request.format,
            runtime_report,
        })
    }
}

fn normalize_export_request(
    mut request: ExportRequest,
    manifest: &SessionManifest,
) -> ExportRequest {
    request
        .audio_tracks
        .retain(|requested| manifest_audio_track(manifest, &requested.track_id).is_some());
    request
}

fn manifest_audio_track<'a>(
    manifest: &'a SessionManifest,
    track_id: &str,
) -> Option<&'a snow_recording_model::AudioTrackManifest> {
    manifest
        .audio_tracks
        .iter()
        .find(|track| track.track_id == track_id && track.recorded)
}

fn requested_recorded_audio_tracks<'a>(
    manifest: &'a SessionManifest,
    request: &'a ExportRequest,
) -> Vec<(
    &'a snow_recording_model::AudioTrackManifest,
    &'a ExportAudioTrackRequest,
)> {
    request
        .audio_tracks
        .iter()
        .filter(|track| track.enabled)
        .filter_map(|request_track| {
            manifest_audio_track(manifest, &request_track.track_id)
                .map(|manifest_track| (manifest_track, request_track))
        })
        .collect()
}

fn apply_codec_telemetry(report: &mut ExportRuntimeReport, telemetry: ExportCodecTelemetry) {
    report.video_decoder = telemetry.video_decoder;
    report.video_encoder = telemetry.video_encoder;
    report.audio_encoder = telemetry.audio_encoder;
    report.used_hardware_decode = telemetry.used_hardware_decode;
    report.used_hardware_encode = telemetry.used_hardware_encode;
    report.used_hardware_compose = false;
    report.stage_durations_ms = telemetry.stage_durations_ms;
}

fn choose_export_fps(record_fps: u32, format: ExportFormat, requested_fps: Option<u32>) -> u32 {
    if let Some(requested_fps) = requested_fps {
        return requested_fps.max(1);
    }
    let fps = record_fps.max(1);
    if format.is_animated_image() {
        fps.min(20)
    } else {
        fps
    }
}

pub(crate) fn output_dimensions(
    src_w: u32,
    src_h: u32,
    maximum_width: Option<u32>,
    maximum_height: Option<u32>,
    force_even: bool,
) -> (u32, u32) {
    let mut w = src_w.max(1);
    let mut h = src_h.max(1);

    if let (Some(maximum_width), Some(maximum_height)) = (maximum_width, maximum_height) {
        let maximum_width = maximum_width.max(1);
        let maximum_height = maximum_height.max(1);
        if w > maximum_width || h > maximum_height {
            let scale = (maximum_width as f64 / w as f64).min(maximum_height as f64 / h as f64);
            w = ((w as f64 * scale).floor() as u32).max(1);
            h = ((h as f64 * scale).floor() as u32).max(1);
        }
    }

    if force_even {
        if !w.is_multiple_of(2) {
            w = w.saturating_sub(1).max(2);
        }
        if !h.is_multiple_of(2) {
            h = h.saturating_sub(1).max(2);
        }
    }

    (w, h)
}

fn probe_intermediate_video_dimensions(
    path: &Path,
    fallback_w: u32,
    fallback_h: u32,
) -> (u32, u32) {
    let fallback = (fallback_w.max(1), fallback_h.max(1));
    if ensure_ffmpeg_initialized().is_err() {
        return fallback;
    }
    let Ok(input) = ffmpeg::format::input(path) else {
        return fallback;
    };
    let Some(video_stream) = input.streams().best(ffmpeg::media::Type::Video) else {
        return fallback;
    };
    let Ok(context) = ffmpeg::codec::context::Context::from_parameters(video_stream.parameters())
    else {
        return fallback;
    };
    let Ok(decoder) = context.decoder().video() else {
        return fallback;
    };
    let width = decoder.width().max(1);
    let height = decoder.height().max(1);
    (width, height)
}

#[inline]
fn hardware_video_decode_allowed(mode: ExportExecutionMode) -> bool {
    matches!(
        mode,
        ExportExecutionMode::HardwarePreferred | ExportExecutionMode::HardwareOnly
    )
}

struct HardwareDecodeSelection {
    hw_pix_fmt: ffmpeg::ffi::AVPixelFormat,
}

struct HardwareDecodeState {
    device_ctx: *mut ffmpeg::ffi::AVBufferRef,
    _selection: Box<HardwareDecodeSelection>,
    hw_pixel_format: ffmpeg::format::Pixel,
    device_name: &'static str,
}

impl Drop for HardwareDecodeState {
    fn drop(&mut self) {
        unsafe {
            if !self.device_ctx.is_null() {
                ffmpeg::ffi::av_buffer_unref(&mut self.device_ctx);
            }
        }
    }
}

struct SourceVideoDecoder<Decoder = ffmpeg::decoder::Video> {
    // Fields drop in declaration order. Codec shutdown joins decoding threads
    // before the hardware state's selection, referenced by AVCodecContext::opaque,
    // can be released.
    decoder: Decoder,
    hardware: Option<HardwareDecodeState>,
}

fn prepare_hardware_decoder_context(
    mut hardware: HardwareDecodeState,
    perf_config: &ExportPerformanceConfig,
    create_context: impl FnOnce() -> std::result::Result<ffmpeg::codec::context::Context, ffmpeg::Error>,
) -> Result<SourceVideoDecoder<ffmpeg::codec::context::Context>> {
    // Own the device before any fallible decoder setup. Both context creation and
    // codec opening can fail after native hardware resources have been allocated.
    let mut context = create_context().map_err(|err| {
        ScreenRecorderError::Export(format!(
            "failed to create source video decoder context: {err}"
        ))
    })?;
    configure_codec_threads(
        &mut context,
        perf_config.decode_threads,
        ffmpeg::codec::threading::Type::Frame,
    );
    unsafe {
        let device_ref = ffmpeg::ffi::av_buffer_ref(hardware.device_ctx);
        if device_ref.is_null() {
            return Err(ScreenRecorderError::Export(
                "failed to retain source video decoder device".into(),
            ));
        }
        let codec_ctx = context.as_mut_ptr();
        (*codec_ctx).get_format = Some(select_hardware_decoder_pixel_format);
        (*codec_ctx).opaque =
            hardware._selection.as_mut() as *mut HardwareDecodeSelection as *mut c_void;
        (*codec_ctx).hw_device_ctx = device_ref;
    }
    Ok(SourceVideoDecoder {
        decoder: context,
        hardware: Some(hardware),
    })
}

unsafe extern "C" fn select_hardware_decoder_pixel_format(
    codec_ctx: *mut ffmpeg::ffi::AVCodecContext,
    pixel_formats: *const ffmpeg::ffi::AVPixelFormat,
) -> ffmpeg::ffi::AVPixelFormat {
    if codec_ctx.is_null() || pixel_formats.is_null() {
        return ffmpeg::ffi::AVPixelFormat::AV_PIX_FMT_NONE;
    }

    let selection = unsafe { (*codec_ctx).opaque as *const HardwareDecodeSelection };
    if !selection.is_null() {
        let wanted = unsafe { (*selection).hw_pix_fmt };
        let mut current = pixel_formats;
        loop {
            let pixel_format = unsafe { *current };
            if pixel_format == ffmpeg::ffi::AVPixelFormat::AV_PIX_FMT_NONE {
                break;
            }
            if pixel_format == wanted {
                return wanted;
            }
            current = unsafe { current.add(1) };
        }
    }

    unsafe { ffmpeg::ffi::avcodec_default_get_format(codec_ctx, pixel_formats) }
}

fn preferred_hardware_decode_device_types() -> &'static [(ffmpeg::ffi::AVHWDeviceType, &'static str)]
{
    #[cfg(target_os = "windows")]
    {
        &[
            (
                ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_D3D11VA,
                "d3d11va",
            ),
            (ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_DXVA2, "dxva2"),
            (ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_QSV, "qsv"),
        ]
    }
    #[cfg(target_os = "macos")]
    {
        &[(
            ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
            "videotoolbox",
        )]
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        &[
            (ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_VAAPI, "vaapi"),
            (ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_QSV, "qsv"),
            (ffmpeg::ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_CUDA, "cuda"),
        ]
    }
    #[cfg(not(any(target_os = "windows", target_os = "macos", unix)))]
    {
        &[]
    }
}

fn find_hardware_decoder_pixel_format(
    codec: ffmpeg::Codec,
    device_type: ffmpeg::ffi::AVHWDeviceType,
) -> Option<ffmpeg::ffi::AVPixelFormat> {
    let mut index = 0;
    loop {
        let config = unsafe { ffmpeg::ffi::avcodec_get_hw_config(codec.as_ptr(), index) };
        if config.is_null() {
            return None;
        }

        let config_ref = unsafe { &*config };
        let supports_device_ctx =
            (config_ref.methods & ffmpeg::ffi::AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX as i32) != 0;
        if supports_device_ctx && config_ref.device_type == device_type {
            return Some(config_ref.pix_fmt);
        }

        index += 1;
    }
}

fn try_open_hardware_video_decoder(
    parameters: &ffmpeg::codec::Parameters,
    perf_config: &ExportPerformanceConfig,
) -> Result<Option<SourceVideoDecoder>> {
    if !hardware_video_decode_allowed(perf_config.mode) {
        return Ok(None);
    }

    let Some(codec) = ffmpeg::codec::decoder::find(parameters.id()) else {
        return Ok(None);
    };

    for (device_type, device_name) in preferred_hardware_decode_device_types() {
        let Some(hw_pix_fmt) = find_hardware_decoder_pixel_format(codec, *device_type) else {
            continue;
        };

        let mut hardware = HardwareDecodeState {
            device_ctx: ptr::null_mut(),
            _selection: Box::new(HardwareDecodeSelection { hw_pix_fmt }),
            hw_pixel_format: ffmpeg::format::Pixel::from(hw_pix_fmt),
            device_name,
        };
        let create_status = unsafe {
            ffmpeg::ffi::av_hwdevice_ctx_create(
                &mut hardware.device_ctx,
                *device_type,
                ptr::null(),
                ptr::null_mut(),
                0,
            )
        };
        if create_status < 0 || hardware.device_ctx.is_null() {
            continue;
        }

        let SourceVideoDecoder {
            decoder: decode_context,
            hardware,
        } = prepare_hardware_decoder_context(hardware, perf_config, || {
            ffmpeg::codec::context::Context::from_parameters(parameters.clone())
        })?;

        let decoder = match decode_context
            .decoder()
            .open_as(codec)
            .and_then(|opened| opened.video())
        {
            Ok(decoder) => decoder,
            Err(_) => continue,
        };

        return Ok(Some(SourceVideoDecoder { decoder, hardware }));
    }

    Ok(None)
}

fn open_source_video_decoder(
    parameters: &ffmpeg::codec::Parameters,
    perf_config: &ExportPerformanceConfig,
    allow_hardware_decode: bool,
) -> Result<SourceVideoDecoder> {
    if allow_hardware_decode
        && let Some(decoder) = try_open_hardware_video_decoder(parameters, perf_config)?
    {
        return Ok(decoder);
    }

    let mut decode_context = ffmpeg::codec::context::Context::from_parameters(parameters.clone())
        .map_err(|err| {
        ScreenRecorderError::Export(format!(
            "failed to create source video decoder context: {err}"
        ))
    })?;
    configure_codec_threads(
        &mut decode_context,
        perf_config.decode_threads,
        ffmpeg::codec::threading::Type::Frame,
    );
    let decoder = decode_context.decoder().video().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to open source video decoder: {err}"))
    })?;
    Ok(SourceVideoDecoder {
        decoder,
        hardware: None,
    })
}

fn decoder_software_output_format(
    decoder: &ffmpeg::decoder::Video,
    hw_state: Option<&HardwareDecodeState>,
) -> ffmpeg::format::Pixel {
    if hw_state.is_some() {
        let sw_format = unsafe { ffmpeg::format::Pixel::from((*decoder.as_ptr()).sw_pix_fmt) };
        if sw_format != ffmpeg::format::Pixel::None {
            return sw_format;
        }
    }

    decoder.format()
}

fn normalize_decoded_video_frame<'a>(
    decoded: &'a mut ffmpeg::frame::Video,
    transferred: &'a mut ffmpeg::frame::Video,
    hw_state: Option<&HardwareDecodeState>,
) -> Result<&'a mut ffmpeg::frame::Video> {
    let Some(hw_state) = hw_state else {
        return Ok(decoded);
    };
    if decoded.format() != hw_state.hw_pixel_format {
        return Ok(decoded);
    }

    unsafe {
        ffmpeg::ffi::av_frame_unref(transferred.as_mut_ptr());
        let transfer_status =
            ffmpeg::ffi::av_hwframe_transfer_data(transferred.as_mut_ptr(), decoded.as_ptr(), 0);
        if transfer_status < 0 {
            return Err(ScreenRecorderError::Export(format!(
                "failed to transfer hardware-decoded video frame to system memory: {}",
                ffmpeg::Error::from(transfer_status)
            )));
        }

        let copy_props_status =
            ffmpeg::ffi::av_frame_copy_props(transferred.as_mut_ptr(), decoded.as_ptr());
        if copy_props_status < 0 {
            return Err(ScreenRecorderError::Export(format!(
                "failed to copy hardware-decoded video frame properties: {}",
                ffmpeg::Error::from(copy_props_status)
            )));
        }
    }

    Ok(transferred)
}

fn video_index_duration_ms(index: &[VideoIndexEntry]) -> u64 {
    index
        .iter()
        .map(|entry| {
            entry
                .timestamp_ms
                .saturating_add(u64::from(entry.duration_ms.max(1)))
        })
        .max()
        .unwrap_or(1)
        .max(1)
}

fn effective_decode_queue_depth(
    queue_depth: u16,
    memory_budget_mb: u32,
    width: u32,
    height: u32,
) -> u16 {
    let depth = queue_depth.max(1);
    let frame_bytes = usize::try_from(width.max(1))
        .ok()
        .and_then(|w| {
            usize::try_from(height.max(1))
                .ok()
                .and_then(|h| w.checked_mul(h))
        })
        .and_then(|px| px.checked_mul(4))
        .unwrap_or(4);
    let budget_bytes = usize::try_from(memory_budget_mb)
        .ok()
        .and_then(|mb| mb.checked_mul(1024 * 1024))
        .unwrap_or(usize::MAX);
    let max_depth_by_budget = (budget_bytes / frame_bytes).max(1);
    depth.min(max_depth_by_budget.min(u16::MAX as usize) as u16)
}

fn build_process_pool(process_threads: u8) -> Option<rayon::ThreadPool> {
    let count = match process_threads {
        0 => auto_thread_count_from_physical_cores(),
        value => usize::from(value),
    };
    if count <= 1 {
        return None;
    }
    rayon::ThreadPoolBuilder::new()
        .start_handler(|_| snow_core::qos::apply_current_thread())
        .num_threads(count)
        .thread_name(|idx| format!("snow-export-process-{idx}"))
        .build()
        .ok()
}

fn check_canceled(cancel_flag: &Arc<AtomicBool>) -> Result<()> {
    if cancel_flag.load(Ordering::Acquire) {
        return Err(ScreenRecorderError::ExportCanceled);
    }
    Ok(())
}

/// A single latest update avoids an unbounded queue for unattended exporters.
struct ProgressReporter {
    sender: Sender<ExportProgress>,
    stale: Receiver<ExportProgress>,
    percent: Mutex<f32>,
}

impl ProgressReporter {
    fn channel() -> (Arc<Self>, Receiver<ExportProgress>) {
        let (sender, receiver) = crossbeam_channel::bounded(1);
        (
            Arc::new(Self {
                sender,
                stale: receiver.clone(),
                percent: Mutex::new(0.0),
            }),
            receiver,
        )
    }
    fn emit(&self, mut progress: ExportProgress) {
        let mut percent = self
            .percent
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        progress.percent = if progress.percent.is_finite() {
            progress.percent.clamp(*percent, 100.0)
        } else {
            *percent
        };
        *percent = progress.percent;
        match self.sender.try_send(progress) {
            Err(crossbeam_channel::TrySendError::Full(progress)) => {
                let _ = self.stale.try_recv();
                let _ = self.sender.try_send(progress);
            }
            Ok(()) | Err(crossbeam_channel::TrySendError::Disconnected(_)) => {}
        }
    }
}

fn emit_progress(
    progress_tx: &Option<Arc<ProgressReporter>>,
    stage: ExportStage,
    percent: f32,
    video_fps: f32,
    eta_ms: Option<u64>,
) {
    if let Some(tx) = progress_tx {
        tx.emit(ExportProgress {
            stage,
            percent,
            video_fps: video_fps.max(0.0),
            eta_ms,
            queue_utilization: 0.0,
            peak_memory_mb: 0,
        });
    }
}

#[derive(Clone, Debug)]
struct RetimePlan {
    source_starts_ms: Vec<u64>,
    speed: f32,
    timeline: FinalizedTimeline,
}

impl RetimePlan {
    fn frame_count(&self) -> usize {
        self.timeline.frame_count() as usize
    }
    fn output_duration_ms(&self) -> u64 {
        self.timeline.duration_ms()
    }
    fn timestamp_ms(&self, index: usize) -> u64 {
        self.timeline
            .frame(index as u64)
            .expect("retimed observation must be in range")
            .timestamp_ms
    }
    fn source_index(&self, index: usize) -> usize {
        let source_ms = (self.timestamp_ms(index) as f64 * f64::from(self.speed)).round() as u64;
        self.source_starts_ms
            .partition_point(|timestamp| *timestamp <= source_ms)
            .saturating_sub(1)
    }
    fn source_indices(&self) -> impl Iterator<Item = usize> + '_ {
        let mut source = 0;
        (0..self.frame_count()).map(move |index| {
            let source_ms =
                (self.timestamp_ms(index) as f64 * f64::from(self.speed)).round() as u64;
            while source + 1 < self.source_starts_ms.len()
                && self.source_starts_ms[source + 1] <= source_ms
            {
                source += 1;
            }
            source
        })
    }
}

#[derive(Clone, Debug)]
struct VideoIndexEntry {
    timestamp_ms: u64,
    duration_ms: u32,
}

fn build_retime_plan_from_index(
    source_index: &[VideoIndexEntry],
    playback_speed: f32,
    export_fps: u32,
) -> Result<RetimePlan> {
    if source_index.is_empty() {
        return Err(ScreenRecorderError::Export(
            "cannot retime an empty source frame sequence".into(),
        ));
    }
    if source_index
        .windows(2)
        .any(|entries| entries[0].timestamp_ms > entries[1].timestamp_ms)
    {
        return Err(ScreenRecorderError::Decode(
            "source frame timestamps run backwards".into(),
        ));
    }
    let source_starts_ms = source_index
        .iter()
        .map(|entry| entry.timestamp_ms)
        .collect();
    let accumulated = video_index_duration_ms(source_index);
    let speed = playback_speed.clamp(0.25, 4.0);
    let fps = export_fps.max(1);
    let output_duration_ms = (accumulated as f64 / f64::from(speed)).ceil().max(1.0) as u64;
    let timeline = FinalizedTimeline::new(output_duration_ms, fps)
        .map_err(ScreenRecorderError::InvalidConfig)?;
    usize::try_from(timeline.frame_count()).map_err(|_| {
        ScreenRecorderError::Export("retimed frame count exceeds addressable range".into())
    })?;
    Ok(RetimePlan {
        source_starts_ms,
        speed,
        timeline,
    })
}

fn read_video_index(
    bundle_path: &Path,
    bundle_footer: &RecordingBundleFooter,
) -> Result<Vec<VideoIndexEntry>> {
    let bytes = read_bundle_asset_bytes(
        bundle_path,
        bundle_footer,
        BundleAssetKind::VideoIndex,
        None,
    )?;
    const RECORD_BYTES: usize = 8 + 8 + 4;
    if bytes.len() < VIDEO_INDEX_MAGIC.len()
        || &bytes[..VIDEO_INDEX_MAGIC.len()] != VIDEO_INDEX_MAGIC
    {
        return Err(ScreenRecorderError::Decode(format!(
            "invalid video index header in {}",
            bundle_path.display()
        )));
    }

    let payload = &bytes[VIDEO_INDEX_MAGIC.len()..];
    if payload.len() % RECORD_BYTES != 0 {
        return Err(ScreenRecorderError::Decode(format!(
            "corrupt video index records in {}",
            bundle_path.display()
        )));
    }

    let record_count = payload.len() / RECORD_BYTES;
    let mut index = Vec::with_capacity(record_count);
    let mut expected_index = 0u64;
    let mut previous_timestamp_ms = None::<u64>;
    let mut cursor = payload.as_ptr();
    for _ in 0..record_count {
        // SAFETY:
        // - `payload` size is validated to be an exact multiple of `RECORD_BYTES`.
        // - `cursor` advances by `RECORD_BYTES` each iteration and always points within `payload`.
        // - Unaligned reads are allowed through `read_unaligned`.
        let idx = unsafe { u64::from_le(ptr::read_unaligned(cursor as *const u64)) };
        if idx != expected_index {
            return Err(ScreenRecorderError::Decode(format!(
                "invalid video index sequence in {}: expected frame index {}, got {}",
                bundle_path.display(),
                expected_index,
                idx
            )));
        }
        expected_index = expected_index.saturating_add(1);
        // SAFETY:
        // - Same bounds and alignment guarantees as for `idx`.
        let timestamp_ms =
            unsafe { u64::from_le(ptr::read_unaligned(cursor.add(8) as *const u64)) };
        if let Some(previous) = previous_timestamp_ms
            && timestamp_ms < previous
        {
            return Err(ScreenRecorderError::Decode(format!(
                "invalid video index timestamps in {}: frame {} timestamp {} is earlier than previous {}",
                bundle_path.display(),
                idx,
                timestamp_ms,
                previous
            )));
        }
        previous_timestamp_ms = Some(timestamp_ms);
        // SAFETY:
        // - Same bounds and alignment guarantees as for `idx`.
        let duration_ms =
            unsafe { u32::from_le(ptr::read_unaligned(cursor.add(16) as *const u32)) };
        if duration_ms == 0 {
            return Err(ScreenRecorderError::Decode(format!(
                "invalid video index record in {}: frame {} has zero duration",
                bundle_path.display(),
                idx
            )));
        }
        // SAFETY:
        // - Advancing by `RECORD_BYTES` stays in bounds due loop limit `record_count`.
        cursor = unsafe { cursor.add(RECORD_BYTES) };
        index.push(VideoIndexEntry {
            timestamp_ms,
            duration_ms,
        });
    }
    Ok(index)
}

fn read_mouse_store(
    bundle_path: &Path,
    bundle_footer: &RecordingBundleFooter,
) -> Result<MouseStore> {
    let bytes = read_bundle_asset_bytes(
        bundle_path,
        bundle_footer,
        BundleAssetKind::MouseStore,
        None,
    )?;
    decode_mouse_records(&bytes).map_err(Into::into)
}

enum DecodedFrameMessage {
    Frame {
        source_index: usize,
        frame: StoredFrame,
    },
    End,
    Error(String),
}

#[derive(Clone)]
struct DecodeWorkerControl {
    export_canceled: Arc<AtomicBool>,
    stopped: Arc<AtomicBool>,
}

impl DecodeWorkerControl {
    fn new(export_canceled: Arc<AtomicBool>) -> Self {
        Self {
            export_canceled,
            stopped: Arc::new(AtomicBool::new(false)),
        }
    }

    fn check_canceled(&self) -> Result<()> {
        if self.stopped.load(Ordering::Acquire) {
            return Err(ScreenRecorderError::ExportCanceled);
        }
        check_canceled(&self.export_canceled)
    }

    fn stop(&self) {
        self.stopped.store(true, Ordering::Release);
    }
}

struct StreamingVideoFrameSource {
    rx: Option<Receiver<DecodedFrameMessage>>,
    recycle_tx: Sender<Vec<u8>>,
    current_index: Option<usize>,
    current_frame: Option<StoredFrame>,
    control: DecodeWorkerControl,
    worker: Option<thread::JoinHandle<()>>,
}

impl StreamingVideoFrameSource {
    fn spawn(
        video_path: &Path,
        required_indices: Vec<usize>,
        fallback_fps: u32,
        queue_depth: u16,
        decode_threads: u8,
        cancel_flag: Arc<AtomicBool>,
    ) -> Result<Self> {
        let depth = usize::from(queue_depth.max(1));
        let (tx, rx) = crossbeam_channel::bounded(depth);
        let (recycle_tx, recycle_rx) = crossbeam_channel::bounded(depth);
        for _ in 0..depth {
            let _ = recycle_tx.try_send(Vec::new());
        }
        let path = video_path.to_path_buf();
        let control = DecodeWorkerControl::new(cancel_flag);
        let worker_control = control.clone();
        let worker = std::thread::Builder::new()
            .name("snow-screen-recorder-export-decode".to_string())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                decode_video_stream_worker(
                    path,
                    required_indices,
                    fallback_fps,
                    decode_threads,
                    worker_control,
                    tx,
                    recycle_rx,
                )
            })
            .map_err(|err| ScreenRecorderError::Io(std::io::Error::other(err)))?;
        Ok(Self {
            rx: Some(rx),
            recycle_tx,
            current_index: None,
            current_frame: None,
            control,
            worker: Some(worker),
        })
    }

    fn frame_at(&mut self, index: usize) -> Result<&StoredFrame> {
        while self.current_index.map(|i| i < index).unwrap_or(true) {
            self.read_next()?;
        }
        if self.current_index == Some(index) {
            return self.current_frame.as_ref().ok_or_else(|| {
                ScreenRecorderError::Export("decoded frame state is unexpectedly empty".to_string())
            });
        }
        Err(ScreenRecorderError::Export(format!(
            "decode stream cannot seek to an earlier frame index (requested index {index})"
        )))
    }

    fn read_next(&mut self) -> Result<()> {
        let msg = self
            .rx
            .as_ref()
            .expect("decode source receiver must exist before shutdown")
            .recv()
            .map_err(|_| {
                ScreenRecorderError::Export("decode worker stopped unexpectedly".to_string())
            })?;
        match msg {
            DecodedFrameMessage::Frame {
                source_index,
                frame,
            } => {
                if let Some(previous) = self.current_frame.take() {
                    let _ = self.recycle_tx.try_send(previous.rgba);
                }
                self.current_index = Some(source_index);
                self.current_frame = Some(frame);
                Ok(())
            }
            DecodedFrameMessage::End => Err(ScreenRecorderError::Export(
                "decode stream ended before requested frame".to_string(),
            )),
            DecodedFrameMessage::Error(message) => Err(ScreenRecorderError::Export(message)),
        }
    }
}

impl Drop for StreamingVideoFrameSource {
    fn drop(&mut self) {
        // Releasing the receiver wakes a decoder blocked on a full frame or
        // terminal-message queue. The local stop flag also ends active decoding
        // without canceling an export that has consumed all requested frames.
        self.control.stop();
        drop(self.rx.take());
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

fn decode_video_stream_worker(
    path: std::path::PathBuf,
    required_indices: Vec<usize>,
    fallback_fps: u32,
    decode_threads: u8,
    control: DecodeWorkerControl,
    tx: crossbeam_channel::Sender<DecodedFrameMessage>,
    recycle_rx: crossbeam_channel::Receiver<Vec<u8>>,
) {
    let result = (|| -> Result<()> {
        control.check_canceled()?;
        ensure_ffmpeg_initialized()?;
        let mut input = ffmpeg::format::input(&path).map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to open temporary recording video {}: {err}",
                path.display()
            ))
        })?;
        let video_stream = input
            .streams()
            .best(ffmpeg::media::Type::Video)
            .ok_or_else(|| {
                ScreenRecorderError::Export(format!(
                    "temporary recording video {} has no video stream",
                    path.display()
                ))
            })?;
        let stream_index = video_stream.index();
        let stream_time_base = video_stream.time_base();
        let mut context = ffmpeg::codec::context::Context::from_parameters(
            video_stream.parameters(),
        )
        .map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to create decoder context for {}: {err}",
                path.display()
            ))
        })?;
        configure_codec_threads(
            &mut context,
            decode_threads,
            ffmpeg::codec::threading::Type::Frame,
        );
        let mut decoder = context.decoder().video().map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to open temporary recording video decoder for {}: {err}",
                path.display()
            ))
        })?;

        let nominal_duration_ms = ((1000.0 / fallback_fps.max(1) as f64).round() as u32).max(1);
        let mut scaler = None::<ffmpeg::software::scaling::Context>;
        let mut rgba_frame = None::<ffmpeg::frame::Video>;
        let mut last_timestamp_ms = None::<u64>;
        let mut decoded = ffmpeg::frame::Video::empty();
        let mut decoded_index = 0usize;
        let mut required_cursor = 0usize;

        if required_indices.is_empty() {
            return Ok(());
        }

        for (stream, packet) in input.packets() {
            control.check_canceled()?;
            if stream.index() != stream_index {
                continue;
            }
            decoder.send_packet(&packet).map_err(|err| {
                ScreenRecorderError::Export(format!(
                    "failed to feed packet into temporary recording video decoder: {err}"
                ))
            })?;
            loop {
                control.check_canceled()?;
                match decoder.receive_frame(&mut decoded) {
                    Ok(()) => {
                        if required_indices
                            .get(required_cursor)
                            .is_some_and(|required| *required == decoded_index)
                        {
                            let frame = decoded_to_stored_frame(
                                &decoded,
                                stream_time_base,
                                nominal_duration_ms,
                                &mut scaler,
                                &mut rgba_frame,
                                &mut last_timestamp_ms,
                                &recycle_rx,
                            )?;
                            if tx
                                .send(DecodedFrameMessage::Frame {
                                    source_index: decoded_index,
                                    frame,
                                })
                                .is_err()
                            {
                                return Ok(());
                            }
                            required_cursor += 1;
                            if required_cursor >= required_indices.len() {
                                return Ok(());
                            }
                        }
                        decoded_index = decoded_index.saturating_add(1);
                    }
                    Err(err) if is_eagain(&err) => break,
                    Err(ffmpeg::Error::Eof) => break,
                    Err(err) => {
                        return Err(ScreenRecorderError::Export(format!(
                            "failed to decode temporary recording video frame: {err}"
                        )));
                    }
                }
            }
        }

        control.check_canceled()?;
        decoder.send_eof().map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to flush temporary recording video decoder: {err}"
            ))
        })?;
        loop {
            control.check_canceled()?;
            match decoder.receive_frame(&mut decoded) {
                Ok(()) => {
                    if required_indices
                        .get(required_cursor)
                        .is_some_and(|required| *required == decoded_index)
                    {
                        let frame = decoded_to_stored_frame(
                            &decoded,
                            stream_time_base,
                            nominal_duration_ms,
                            &mut scaler,
                            &mut rgba_frame,
                            &mut last_timestamp_ms,
                            &recycle_rx,
                        )?;
                        if tx
                            .send(DecodedFrameMessage::Frame {
                                source_index: decoded_index,
                                frame,
                            })
                            .is_err()
                        {
                            return Ok(());
                        }
                        required_cursor += 1;
                        if required_cursor >= required_indices.len() {
                            return Ok(());
                        }
                    }
                    decoded_index = decoded_index.saturating_add(1);
                }
                Err(err) if is_eagain(&err) => continue,
                Err(ffmpeg::Error::Eof) => break,
                Err(err) => {
                    return Err(ScreenRecorderError::Export(format!(
                        "failed to drain temporary recording video decoder: {err}"
                    )));
                }
            }
        }
        Ok(())
    })();

    match result {
        Ok(()) | Err(ScreenRecorderError::ExportCanceled) => {
            let _ = tx.send(DecodedFrameMessage::End);
        }
        Err(err) => {
            let _ = tx.send(DecodedFrameMessage::Error(format!("{err}")));
        }
    }
}

fn collect_required_source_indices(source_indices: impl Iterator<Item = usize>) -> Vec<usize> {
    let mut required = Vec::new();
    for index in source_indices {
        if required.last().copied() != Some(index) {
            required.push(index);
        }
    }
    required
}

fn decoded_to_stored_frame(
    decoded: &ffmpeg::frame::Video,
    stream_time_base: ffmpeg::Rational,
    nominal_duration_ms: u32,
    scaler: &mut Option<ffmpeg::software::scaling::Context>,
    rgba_frame: &mut Option<ffmpeg::frame::Video>,
    last_timestamp_ms: &mut Option<u64>,
    recycle_rx: &crossbeam_channel::Receiver<Vec<u8>>,
) -> Result<StoredFrame> {
    let width = decoded.width();
    let height = decoded.height();
    if width == 0 || height == 0 {
        return Err(ScreenRecorderError::Export(
            "decoded frame has zero dimensions".to_string(),
        ));
    }

    let mut timestamp_ms = decoded
        .timestamp()
        .or_else(|| decoded.pts())
        .map(|pts| pts_to_millis(pts, stream_time_base))
        .unwrap_or_else(|| {
            last_timestamp_ms
                .unwrap_or(0)
                .saturating_add(u64::from(nominal_duration_ms))
        });
    if let Some(previous) = *last_timestamp_ms
        && timestamp_ms <= previous
    {
        timestamp_ms = previous.saturating_add(1);
    }
    *last_timestamp_ms = Some(timestamp_ms);

    let expected_rgba_len = width as usize * height as usize * 4;
    let mut rgba = recycle_rx.try_recv().unwrap_or_default();
    if rgba.len() != expected_rgba_len {
        rgba.resize(expected_rgba_len, 0);
    }

    let decoded_format = decoded.format();
    if decoded_format == ffmpeg::format::Pixel::RGBA {
        extract_rgba_from_frame_into(decoded, width, height, &mut rgba)?;
    } else if decoded_format == ffmpeg::format::Pixel::BGRA {
        extract_bgra_from_frame_into(decoded, width, height, &mut rgba)?;
    } else {
        let needs_reset = scaler.is_none()
            || rgba_frame
                .as_ref()
                .map(|frame| frame.width() != width || frame.height() != height)
                .unwrap_or(false);
        if needs_reset {
            *scaler = Some(
                ffmpeg::software::scaling::Context::get(
                    decoded_format,
                    width,
                    height,
                    ffmpeg::format::Pixel::RGBA,
                    width,
                    height,
                    ffmpeg::software::scaling::flag::Flags::BICUBIC,
                )
                .map_err(|err| {
                    ScreenRecorderError::Export(format!(
                        "failed to create temporary video decode scaler: {err}"
                    ))
                })?,
            );
            *rgba_frame = Some(ffmpeg::frame::Video::new(
                ffmpeg::format::Pixel::RGBA,
                width,
                height,
            ));
        }

        let scaler_ref = scaler.as_mut().ok_or_else(|| {
            ScreenRecorderError::Export("video scaler is uninitialized".to_string())
        })?;
        let rgba_ref = rgba_frame.as_mut().ok_or_else(|| {
            ScreenRecorderError::Export("video frame buffer is uninitialized".to_string())
        })?;
        scaler_ref.run(decoded, rgba_ref).map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to convert decoded temporary video frame into RGBA: {err}"
            ))
        })?;
        extract_rgba_from_frame_into(rgba_ref, width, height, &mut rgba)?;
    }

    Ok(StoredFrame {
        timestamp_ms,
        duration_ms: nominal_duration_ms.max(1),
        width,
        height,
        rgba,
    })
}

fn extract_rgba_from_frame_into(
    frame: &ffmpeg::frame::Video,
    width: u32,
    height: u32,
    out: &mut [u8],
) -> Result<()> {
    let stride = frame.stride(0);
    let row_bytes = width as usize * 4;
    let height_usize = height as usize;
    let src = frame.data(0);

    let total = row_bytes * height_usize;
    debug_assert_eq!(out.len(), total);
    if stride == row_bytes {
        if src.len() < total {
            return Err(ScreenRecorderError::Export(
                "decoded RGBA frame is smaller than expected".to_string(),
            ));
        }
        // SAFETY:
        // - `src` and `out` are valid for `total` bytes and non-overlapping.
        unsafe {
            ptr::copy_nonoverlapping(src.as_ptr(), out.as_mut_ptr(), total);
        }
        return Ok(());
    }
    for y in 0..height_usize {
        let src_start = y * stride;
        let src_end = src_start + row_bytes;
        if src_end > src.len() {
            return Err(ScreenRecorderError::Export(
                "decoded RGBA frame stride exceeds available data".to_string(),
            ));
        }
        let dst_start = y * row_bytes;
        // SAFETY:
        // - Source and destination ranges are bounds-checked above.
        // - Source and destination do not overlap.
        unsafe {
            ptr::copy_nonoverlapping(
                src.as_ptr().add(src_start),
                out.as_mut_ptr().add(dst_start),
                row_bytes,
            );
        }
    }

    Ok(())
}

fn extract_bgra_from_frame_into(
    frame: &ffmpeg::frame::Video,
    width: u32,
    height: u32,
    out: &mut [u8],
) -> Result<()> {
    let stride = frame.stride(0);
    let row_bytes = width as usize * 4;
    let height_usize = height as usize;
    let src = frame.data(0);

    let total = row_bytes * height_usize;
    debug_assert_eq!(out.len(), total);

    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    let use_avx2 = std::is_x86_feature_detected!("avx2");

    for y in 0..height_usize {
        let src_start = y * stride;
        let src_end = src_start + row_bytes;
        if src_end > src.len() {
            return Err(ScreenRecorderError::Export(
                "decoded BGRA frame stride exceeds available data".to_string(),
            ));
        }
        let dst_start = y * row_bytes;
        let src_row = &src[src_start..src_end];
        let dst_row = &mut out[dst_start..dst_start + row_bytes];
        #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
        {
            if use_avx2 {
                // SAFETY:
                // - AVX2 availability is checked once before the loop.
                // - Row slices are valid and non-overlapping.
                unsafe {
                    convert_bgra_row_into_rgba_avx2(dst_row, src_row);
                }
            } else {
                convert_bgra_row_into_rgba_scalar(dst_row, src_row);
            }
        }
        #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
        {
            convert_bgra_row_into_rgba_scalar(dst_row, src_row);
        }
    }
    Ok(())
}

#[inline]
fn convert_bgra_row_into_rgba_scalar(dst_row: &mut [u8], src_row: &[u8]) {
    debug_assert_eq!(dst_row.len(), src_row.len());
    debug_assert_eq!(dst_row.len() % 4, 0);
    let pixel_count = src_row.len() / 4;
    // SAFETY:
    // - Pointers are derived from valid slices and used within bounds.
    // - `dst_row` and `src_row` do not overlap.
    unsafe {
        let src_ptr = src_row.as_ptr() as *const u32;
        let dst_ptr = dst_row.as_mut_ptr() as *mut u32;
        for idx in 0..pixel_count {
            let bgra = ptr::read_unaligned(src_ptr.add(idx));
            let rgba =
                (bgra & 0xFF00_FF00) | ((bgra & 0x00FF_0000) >> 16) | ((bgra & 0x0000_00FF) << 16);
            ptr::write_unaligned(dst_ptr.add(idx), rgba);
        }
    }
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn convert_bgra_row_into_rgba_avx2(dst_row: &mut [u8], src_row: &[u8]) {
    #[cfg(target_arch = "x86")]
    use std::arch::x86::{
        __m256i, _mm256_loadu_si256, _mm256_setr_epi8, _mm256_shuffle_epi8, _mm256_storeu_si256,
    };
    #[cfg(target_arch = "x86_64")]
    use std::arch::x86_64::{
        __m256i, _mm256_loadu_si256, _mm256_setr_epi8, _mm256_shuffle_epi8, _mm256_storeu_si256,
    };

    let len = src_row.len();
    let mut offset = 0usize;
    let shuffle: __m256i = _mm256_setr_epi8(
        2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15, 2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11,
        14, 13, 12, 15,
    );

    while offset + 32 <= len {
        let src_vec = unsafe { _mm256_loadu_si256(src_row.as_ptr().add(offset) as *const __m256i) };
        let rgba_vec = _mm256_shuffle_epi8(src_vec, shuffle);
        unsafe {
            _mm256_storeu_si256(dst_row.as_mut_ptr().add(offset) as *mut __m256i, rgba_vec);
        }
        offset += 32;
    }

    let remaining_src = &src_row[offset..];
    let remaining_dst = &mut dst_row[offset..];
    let pixels = remaining_src.len() / 4;
    let src_ptr = remaining_src.as_ptr() as *const u32;
    let dst_ptr = remaining_dst.as_mut_ptr() as *mut u32;
    for idx in 0..pixels {
        let bgra = unsafe { ptr::read_unaligned(src_ptr.add(idx)) };
        let rgba =
            (bgra & 0xFF00_FF00) | ((bgra & 0x00FF_0000) >> 16) | ((bgra & 0x0000_00FF) << 16);
        unsafe {
            ptr::write_unaligned(dst_ptr.add(idx), rgba);
        }
    }
}

fn pts_to_millis(pts: i64, time_base: ffmpeg::Rational) -> u64 {
    let numerator = i128::from(time_base.numerator().max(1));
    let denominator = i128::from(time_base.denominator().max(1));
    let millis = i128::from(pts)
        .saturating_mul(numerator)
        .saturating_mul(1_000)
        / denominator;
    millis.clamp(0, i128::from(u64::MAX)) as u64
}

fn prepare_overlay_base_rgba(
    source: &StoredFrame,
    source_index: usize,
    output_w: u32,
    output_h: u32,
    resize_plan: Option<&NearestResizePlan>,
    process_pool: Option<&rayon::ThreadPool>,
    resized_cache_key: &mut Option<(usize, u32, u32)>,
    resized_cache: &mut Vec<u8>,
    output_rgba: &mut [u8],
) {
    if source.width == output_w && source.height == output_h {
        output_rgba.copy_from_slice(&source.rgba);
        return;
    }

    let expected_len = output_w.max(1) as usize * output_h.max(1) as usize * 4;
    if resized_cache.len() != expected_len {
        resized_cache.resize(expected_len, 0);
        *resized_cache_key = None;
    }

    let source_cache_key = (source_index, source.width, source.height);
    if *resized_cache_key != Some(source_cache_key) {
        resize_rgba_fast_into(
            &source.rgba,
            source.width,
            source.height,
            output_w,
            output_h,
            resize_plan,
            resized_cache,
            process_pool,
        );
        *resized_cache_key = Some(source_cache_key);
    }

    output_rgba.copy_from_slice(resized_cache);
}

#[derive(Clone, Debug)]
struct MouseSample {
    ts_ms: u64,
    x: i32,
    y: i32,
    visible: bool,
    shape_id: Option<u64>,
}

#[derive(Clone, Debug)]
struct MouseClickDown {
    ts_ms: u64,
    x: i32,
    y: i32,
}

#[derive(Clone, Debug, Default)]
struct MouseTracks {
    samples: Vec<MouseSample>,
    trail_points: Vec<TrailCurvePoint>,
    trail_segments: Vec<TrailRenderSegment>,
    click_downs: Vec<MouseClickDown>,
    cursor_shapes: Arc<HashMap<u64, CompiledCursorShape>>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum CompiledCursorRunKind {
    AlphaCopy,
    MaskCopy,
    Xor,
    Blend,
}

#[derive(Clone, Copy, Debug)]
struct CompiledCursorRun {
    start_x: usize,
    end_x: usize,
    src_byte_start: usize,
    kind: CompiledCursorRunKind,
}

#[derive(Clone, Debug, Default)]
struct CompiledCursorRow {
    runs: Box<[CompiledCursorRun]>,
}

#[derive(Clone, Debug)]
struct CompiledCursorShapePlan {
    hotspot_x: i32,
    hotspot_y: i32,
    width: usize,
    height: usize,
    yuv420p_compatible: bool,
    rgba: Box<[u8]>,
    yuva: Box<[u8]>,
    rows: Box<[CompiledCursorRow]>,
}

#[derive(Clone, Debug)]
enum CompiledCursorShape {
    Invalid,
    Plan(CompiledCursorShapePlan),
}

fn retime_mouse_tracks(tracks: &mut MouseTracks, speed: f32) -> Result<()> {
    if !speed.is_finite() || speed <= 0.0 {
        return Err(ScreenRecorderError::InvalidConfig(
            "invalid mouse playback speed".into(),
        ));
    }
    let speed = f64::from(speed.clamp(0.25, 4.0));
    let retime = |timestamp: u64| -> Result<u64> {
        let value = (timestamp as f64 / speed).round();
        if value >= u64::MAX as f64 {
            return Err(ScreenRecorderError::InvalidConfig(
                "mouse timeline overflow".into(),
            ));
        }
        Ok(value as u64)
    };
    for sample in &mut tracks.samples {
        sample.ts_ms = retime(sample.ts_ms)?;
    }
    for click in &mut tracks.click_downs {
        click.ts_ms = retime(click.ts_ms)?;
    }
    for point in &mut tracks.trail_points {
        point.ts_ms /= speed as f32;
    }
    tracks.trail_segments.clear();
    Ok(())
}

fn validate_hdr_cursor_shapes(tracks: &MouseTracks) -> Result<()> {
    for shape in tracks.cursor_shapes.values() {
        match shape {
            CompiledCursorShape::Invalid => {
                return Err(ScreenRecorderError::InvalidConfig(
                    "invalid HDR cursor asset".into(),
                ));
            }
            CompiledCursorShape::Plan(plan)
                if plan.rows.iter().flat_map(|row| row.runs.iter()).any(|run| {
                    matches!(
                        run.kind,
                        CompiledCursorRunKind::MaskCopy | CompiledCursorRunKind::Xor
                    )
                }) =>
            {
                return Err(ScreenRecorderError::InvalidConfig(
                    "destination-dependent cursor masks are unsupported in HDR".into(),
                ));
            }
            _ => {}
        }
    }
    Ok(())
}

fn build_mouse_tracks(store: MouseStore) -> MouseTracks {
    let mut tracks = MouseTracks {
        samples: Vec::with_capacity(store.cursor_frames.len()),
        trail_points: Vec::with_capacity(store.cursor_frames.len()),
        trail_segments: Vec::new(),
        click_downs: Vec::with_capacity(store.clicks.len()),
        cursor_shapes: Arc::default(),
    };

    for sample in store.cursor_frames {
        tracks.samples.push(MouseSample {
            ts_ms: sample.timestamp_ms,
            x: sample.x,
            y: sample.y,
            visible: sample.visible,
            shape_id: sample.shape_id,
        });
    }

    let mut cursor_shapes = HashMap::with_capacity(store.cursor_shapes.len());
    for shape in store.cursor_shapes {
        cursor_shapes
            .entry(shape.shape_id)
            .or_insert_with(|| compile_cursor_shape(shape));
    }

    for click in store.clicks {
        if click.down {
            tracks.click_downs.push(MouseClickDown {
                ts_ms: click.timestamp_ms,
                x: click.x,
                y: click.y,
            });
        }
    }

    tracks.samples.sort_by_key(|s| s.ts_ms);
    tracks
        .trail_points
        .extend(
            tracks
                .samples
                .iter()
                .filter(|sample| sample.visible)
                .map(|sample| TrailCurvePoint {
                    x: sample.x as f32,
                    y: sample.y as f32,
                    ts_ms: sample.ts_ms as f32,
                }),
        );
    tracks.click_downs.sort_by_key(|c| c.ts_ms);
    tracks.cursor_shapes = Arc::new(cursor_shapes);
    tracks
}

fn compile_cursor_shape(shape: CursorShapeRecord) -> CompiledCursorShape {
    let width = shape.width as usize;
    let height = shape.height as usize;
    let expected_len = width
        .checked_mul(height)
        .and_then(|px| px.checked_mul(4))
        .unwrap_or(0);
    if expected_len == 0 || shape.shape_rgba.len() < expected_len {
        return CompiledCursorShape::Invalid;
    }

    let mut rgba = shape.shape_rgba;
    rgba.truncate(expected_len);
    let rgba = rgba.into_boxed_slice();
    let yuva = rgba_to_yuva(&rgba);
    let Some((rows, yuv420p_compatible)) = compile_cursor_rows(shape.mode, &rgba, width, height)
    else {
        return CompiledCursorShape::Invalid;
    };

    CompiledCursorShape::Plan(CompiledCursorShapePlan {
        hotspot_x: shape.hotspot_x.min(i32::MAX as u32) as i32,
        hotspot_y: shape.hotspot_y.min(i32::MAX as u32) as i32,
        width,
        height,
        yuv420p_compatible,
        rgba,
        yuva,
        rows,
    })
}

fn compile_cursor_rows(
    mode: CursorShapeCompositionMode,
    rgba: &[u8],
    width: usize,
    height: usize,
) -> Option<(Box<[CompiledCursorRow]>, bool)> {
    let src_row_bytes = width.saturating_mul(4);
    let mut rows = Vec::with_capacity(height);
    let mut has_effect = false;
    let mut yuv420p_compatible = true;

    for row_idx in 0..height {
        let row_start = row_idx.saturating_mul(src_row_bytes);
        let row_rgba = &rgba[row_start..row_start + src_row_bytes];
        let mut runs = Vec::new();
        let mut x = 0usize;

        while x < width {
            let px_start = x.saturating_mul(4);
            let Some(kind) = classify_cursor_pixel(mode, &row_rgba[px_start..px_start + 4]) else {
                x += 1;
                continue;
            };
            let start_x = x;
            x += 1;

            while x < width {
                let next_start = x.saturating_mul(4);
                if classify_cursor_pixel(mode, &row_rgba[next_start..next_start + 4]) != Some(kind)
                {
                    break;
                }
                x += 1;
            }

            has_effect = true;
            yuv420p_compatible &= matches!(
                kind,
                CompiledCursorRunKind::AlphaCopy | CompiledCursorRunKind::Blend
            );
            runs.push(CompiledCursorRun {
                start_x,
                end_x: x,
                src_byte_start: row_start + start_x.saturating_mul(4),
                kind,
            });
        }

        rows.push(CompiledCursorRow {
            runs: runs.into_boxed_slice(),
        });
    }

    has_effect.then(|| (rows.into_boxed_slice(), yuv420p_compatible))
}

fn classify_cursor_pixel(
    mode: CursorShapeCompositionMode,
    rgba: &[u8],
) -> Option<CompiledCursorRunKind> {
    match mode {
        CursorShapeCompositionMode::AlphaBlend => match rgba[3] {
            0 => None,
            255 => Some(CompiledCursorRunKind::AlphaCopy),
            _ => Some(CompiledCursorRunKind::Blend),
        },
        CursorShapeCompositionMode::MaskedColor => match rgba[3] {
            0x00 => Some(CompiledCursorRunKind::MaskCopy),
            0xFF if rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 => None,
            0xFF => Some(CompiledCursorRunKind::Xor),
            _ => Some(CompiledCursorRunKind::Blend),
        },
    }
}

fn scale_mouse_tracks(
    tracks: &MouseTracks,
    src_w: u32,
    src_h: u32,
    dst_w: u32,
    dst_h: u32,
) -> MouseTracks {
    if tracks.samples.is_empty() && tracks.click_downs.is_empty() {
        return MouseTracks {
            samples: Vec::new(),
            trail_points: Vec::new(),
            trail_segments: Vec::new(),
            click_downs: Vec::new(),
            cursor_shapes: Arc::clone(&tracks.cursor_shapes),
        };
    }

    let mut scaled = MouseTracks {
        samples: Vec::with_capacity(tracks.samples.len()),
        trail_points: Vec::with_capacity(tracks.trail_points.len()),
        trail_segments: Vec::new(),
        click_downs: Vec::with_capacity(tracks.click_downs.len()),
        cursor_shapes: Arc::clone(&tracks.cursor_shapes),
    };

    for sample in &tracks.samples {
        scaled.samples.push(MouseSample {
            ts_ms: sample.ts_ms,
            x: scale_mouse_coord(sample.x, src_w, dst_w),
            y: scale_mouse_coord(sample.y, src_h, dst_h),
            visible: sample.visible,
            shape_id: sample.shape_id,
        });
    }
    for point in &tracks.trail_points {
        scaled.trail_points.push(TrailCurvePoint {
            x: scale_mouse_coord(point.x.round() as i32, src_w, dst_w) as f32,
            y: scale_mouse_coord(point.y.round() as i32, src_h, dst_h) as f32,
            ts_ms: point.ts_ms,
        });
    }
    for click in &tracks.click_downs {
        scaled.click_downs.push(MouseClickDown {
            ts_ms: click.ts_ms,
            x: scale_mouse_coord(click.x, src_w, dst_w),
            y: scale_mouse_coord(click.y, src_h, dst_h),
        });
    }
    scaled
}

fn compile_mouse_trail_segments(
    tracks: &mut MouseTracks,
    config: &MouseEditConfig,
    width: u32,
    height: u32,
) {
    if !config.trail_enabled || tracks.trail_points.len() < 2 {
        tracks.trail_segments.clear();
        return;
    }

    tracks.trail_segments = compile_trail_segments(
        &tracks.trail_points,
        config.trail_smooth_step_px,
        config.trail_thickness,
        width,
        height,
    );
}

#[inline(always)]
fn scale_mouse_coord(value: i32, src_extent: u32, dst_extent: u32) -> i32 {
    let src = i64::from(src_extent.max(1));
    let dst = i64::from(dst_extent.max(1));
    let num = i64::from(value).saturating_mul(dst);
    let rounded = if num >= 0 {
        num.saturating_add(src / 2)
    } else {
        num.saturating_sub(src / 2)
    };
    rounded
        .saturating_div(src)
        .clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32
}

#[cfg(test)]
fn apply_mouse_overlays(frame: &mut StoredFrame, tracks: &MouseTracks, config: &MouseEditConfig) {
    apply_mouse_overlays_rgba(
        frame.timestamp_ms,
        frame.width,
        frame.height,
        &mut frame.rgba,
        tracks,
        config,
    );
}

#[cfg_attr(not(test), allow(dead_code))]
fn apply_mouse_overlays_rgba(
    timestamp_ms: u64,
    width: u32,
    height: u32,
    rgba: &mut [u8],
    tracks: &MouseTracks,
    config: &MouseEditConfig,
) {
    let mut surface = FrameSurfaceMut {
        hdr: None,
        timestamp_ms,
        width,
        height,
        rgba,
    };
    apply_mouse_overlays_surface(&mut surface, tracks, config);
}

#[derive(Clone, Debug, Default)]
struct OverlaySearchState {
    cursor_next_idx: usize,
    trail_segment_next_idx: usize,
    trail_segment_start_idx: usize,
    click_start_idx: usize,
    last_ts_ms: Option<u64>,
}

#[derive(Clone, Copy, Debug, Default)]
struct OverlayDecision {
    current_idx: Option<usize>,
    has_trail: bool,
    has_clicks: bool,
    has_cursor: bool,
}

impl OverlayDecision {
    #[inline]
    fn needs_draw(self) -> bool {
        self.has_trail || self.has_clicks || self.has_cursor
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RepeatableOverlayFrameKey {
    NoDraw,
    CursorOnly(usize),
}

#[inline]
fn repeatable_overlay_frame_key(decision: OverlayDecision) -> Option<RepeatableOverlayFrameKey> {
    if !decision.needs_draw() {
        return Some(RepeatableOverlayFrameKey::NoDraw);
    }
    if decision.has_cursor && !decision.has_trail && !decision.has_clicks {
        return decision
            .current_idx
            .map(RepeatableOverlayFrameKey::CursorOnly);
    }
    None
}

const CLICK_RIPPLE_MS: u64 = 350;

fn apply_mouse_overlays_rgba_incremental(
    timestamp_ms: u64,
    width: u32,
    height: u32,
    rgba: &mut [u8],
    tracks: &MouseTracks,
    config: &MouseEditConfig,
    state: &mut OverlaySearchState,
) {
    let mut surface = FrameSurfaceMut {
        hdr: None,
        timestamp_ms,
        width,
        height,
        rgba,
    };
    apply_mouse_overlays_surface_incremental(&mut surface, tracks, config, state);
}

struct FrameSurfaceMut<'a> {
    timestamp_ms: u64,
    width: u32,
    height: u32,
    rgba: &'a mut [u8],
    hdr: Option<(&'a mut [u8], usize)>,
}

#[cfg_attr(not(test), allow(dead_code))]
fn apply_mouse_overlays_surface(
    surface: &mut FrameSurfaceMut<'_>,
    tracks: &MouseTracks,
    config: &MouseEditConfig,
) {
    if tracks.samples.is_empty() {
        return;
    }

    let ts = surface.timestamp_ms;
    let cursor_idx = tracks.samples.partition_point(|s| s.ts_ms <= ts);
    if cursor_idx == 0 {
        return;
    }
    let current = &tracks.samples[cursor_idx - 1];

    if config.trail_enabled {
        if tracks.trail_segments.is_empty() {
            draw_mouse_trail_points(surface, &tracks.trail_points, ts, config);
        } else {
            let trail_window_ms = config.trail_window_ms.max(1);
            let trail_start_idx = tracks
                .trail_segments
                .partition_point(|segment| segment.end_ts_ms.saturating_add(trail_window_ms) < ts);
            let trail_end_idx = tracks
                .trail_segments
                .partition_point(|segment| segment.end_ts_ms <= ts);
            draw_compiled_trail_segments(
                surface,
                &tracks.trail_segments[trail_start_idx..trail_end_idx],
                ts,
                config,
            );
        }
    }
    if config.click_enabled {
        draw_click_ripples(surface, &tracks.click_downs, ts);
    }
    if config.visible && current.visible {
        draw_cursor(surface, current, tracks);
    }
}

fn apply_mouse_overlays_surface_incremental(
    surface: &mut FrameSurfaceMut<'_>,
    tracks: &MouseTracks,
    config: &MouseEditConfig,
    state: &mut OverlaySearchState,
) {
    let decision = advance_overlay_state(surface.timestamp_ms, tracks, config, state);
    if !decision.needs_draw() {
        return;
    }

    apply_mouse_overlays_surface_from_decision(surface, tracks, config, state, decision);
}

fn advance_overlay_state(
    ts: u64,
    tracks: &MouseTracks,
    config: &MouseEditConfig,
    state: &mut OverlaySearchState,
) -> OverlayDecision {
    if tracks.samples.is_empty() {
        return OverlayDecision::default();
    }

    if state.last_ts_ms.is_none_or(|prev| ts < prev) {
        state.cursor_next_idx = 0;
        state.trail_segment_next_idx = 0;
        state.trail_segment_start_idx = 0;
        state.click_start_idx = 0;
    }
    state.last_ts_ms = Some(ts);

    while state.cursor_next_idx < tracks.samples.len()
        && tracks.samples[state.cursor_next_idx].ts_ms <= ts
    {
        state.cursor_next_idx += 1;
    }
    if state.cursor_next_idx == 0 {
        return OverlayDecision::default();
    }

    let current_idx = state.cursor_next_idx - 1;
    let current = &tracks.samples[current_idx];
    let mut decision = OverlayDecision {
        current_idx: Some(current_idx),
        has_cursor: config.visible
            && current.visible
            && compiled_cursor_shape(current, tracks).is_some(),
        ..OverlayDecision::default()
    };

    if config.trail_enabled {
        let trail_window_ms = config.trail_window_ms.max(1);
        while state.trail_segment_next_idx < tracks.trail_segments.len()
            && tracks.trail_segments[state.trail_segment_next_idx].end_ts_ms <= ts
        {
            state.trail_segment_next_idx += 1;
        }
        while state.trail_segment_start_idx < state.trail_segment_next_idx
            && tracks.trail_segments[state.trail_segment_start_idx]
                .end_ts_ms
                .saturating_add(trail_window_ms)
                < ts
        {
            state.trail_segment_start_idx += 1;
        }
        decision.has_trail = state.trail_segment_start_idx < state.trail_segment_next_idx;
    }

    if config.click_enabled {
        while state.click_start_idx < tracks.click_downs.len()
            && tracks.click_downs[state.click_start_idx]
                .ts_ms
                .saturating_add(CLICK_RIPPLE_MS)
                < ts
        {
            state.click_start_idx += 1;
        }
        decision.has_clicks =
            has_active_click_ripple_from(&tracks.click_downs, state.click_start_idx, ts);
    }

    decision
}

fn apply_mouse_overlays_surface_from_decision(
    surface: &mut FrameSurfaceMut<'_>,
    tracks: &MouseTracks,
    config: &MouseEditConfig,
    state: &mut OverlaySearchState,
    decision: OverlayDecision,
) {
    let ts = surface.timestamp_ms;
    let Some(current_idx) = decision.current_idx else {
        return;
    };
    let current = &tracks.samples[current_idx];

    if decision.has_trail {
        draw_compiled_trail_segments(
            surface,
            &tracks.trail_segments[state.trail_segment_start_idx..state.trail_segment_next_idx],
            ts,
            config,
        );
    }

    if decision.has_clicks {
        draw_click_ripples_from(surface, &tracks.click_downs, state.click_start_idx, ts);
    }

    if decision.has_cursor {
        draw_cursor(surface, current, tracks);
    }
}

fn has_active_click_ripple_from(clicks: &[MouseClickDown], start: usize, ts: u64) -> bool {
    for click in &clicks[start..] {
        if click.ts_ms > ts {
            break;
        }
        if ts <= click.ts_ms.saturating_add(CLICK_RIPPLE_MS) {
            return true;
        }
    }
    false
}

fn draw_compiled_trail_segments(
    surface: &mut FrameSurfaceMut<'_>,
    segments: &[TrailRenderSegment],
    ts: u64,
    config: &MouseEditConfig,
) {
    let trail_window_ms = config.trail_window_ms.max(1);
    for segment in segments {
        let age = ts.saturating_sub(segment.end_ts_ms).min(trail_window_ms);
        let alpha = compute_trail_alpha(age, trail_window_ms, config.trail_max_alpha);
        if alpha == 0 {
            continue;
        }
        let color = [
            config.trail_color[0],
            config.trail_color[1],
            config.trail_color[2],
            alpha,
        ];
        for span in segment.spans.iter().copied() {
            blend_horizontal_span(surface, span.y, span.x_start, span.x_end, color);
        }
    }
}

#[inline(always)]
fn compute_trail_alpha(age: u64, trail_window_ms: u64, max_alpha: u8) -> u8 {
    let remaining = trail_window_ms.saturating_sub(age.min(trail_window_ms));
    (((remaining.saturating_mul(u64::from(max_alpha))) + trail_window_ms / 2) / trail_window_ms)
        .min(255) as u8
}

#[inline(always)]
fn blend_horizontal_span(
    surface: &mut FrameSurfaceMut<'_>,
    y: i32,
    x_start: i32,
    x_end: i32,
    color: [u8; 4],
) {
    if y < 0 || y >= surface.height as i32 {
        return;
    }

    let x_start = x_start.max(0);
    let x_end = x_end.min(surface.width as i32 - 1);
    if x_start > x_end {
        return;
    }

    let alpha = color[3];
    if alpha == 0 {
        return;
    }

    if surface.hdr.is_some() {
        for x in x_start..=x_end {
            set_pixel_blended(surface, x, y, color);
        }
        return;
    }
    let row_offset = y as usize * surface.width as usize;
    let mut idx = (row_offset + x_start as usize) * 4;
    let end = (row_offset + x_end as usize + 1) * 4;
    let rgba = &mut surface.rgba;

    if alpha == 255 {
        while idx < end {
            rgba[idx] = color[0];
            rgba[idx + 1] = color[1];
            rgba[idx + 2] = color[2];
            rgba[idx + 3] = 255;
            idx += 4;
        }
        return;
    }

    let alpha_u16 = u16::from(alpha);
    let inv_alpha = 255u16.saturating_sub(alpha_u16);
    let src_r = u16::from(color[0]);
    let src_g = u16::from(color[1]);
    let src_b = u16::from(color[2]);

    while idx < end {
        rgba[idx] = ((src_r * alpha_u16 + u16::from(rgba[idx]) * inv_alpha) / 255) as u8;
        rgba[idx + 1] = ((src_g * alpha_u16 + u16::from(rgba[idx + 1]) * inv_alpha) / 255) as u8;
        rgba[idx + 2] = ((src_b * alpha_u16 + u16::from(rgba[idx + 2]) * inv_alpha) / 255) as u8;
        rgba[idx + 3] = 255;
        idx += 4;
    }
}

fn draw_mouse_trail_points(
    surface: &mut FrameSurfaceMut<'_>,
    points: &[TrailCurvePoint],
    ts: u64,
    config: &MouseEditConfig,
) {
    let mut visible = Vec::new();
    let mut smoothed = Vec::new();
    draw_mouse_trail_windowed_with_scratch(
        surface,
        points,
        ts,
        config,
        &mut visible,
        &mut smoothed,
    );
}

fn draw_mouse_trail_windowed_with_scratch(
    surface: &mut FrameSurfaceMut<'_>,
    points: &[TrailCurvePoint],
    ts: u64,
    config: &MouseEditConfig,
    visible: &mut Vec<TrailCurvePoint>,
    smoothed: &mut Vec<TrailCurvePoint>,
) {
    let trail_window_ms = config.trail_window_ms.max(1);
    let cutoff = ts.saturating_sub(trail_window_ms) as f32;
    collect_visible_trail_window_points_into(points, cutoff, visible);
    if visible.len() < 2 {
        return;
    }

    smoothed.clear();

    for segment in visible.windows(2) {
        let a = segment[0];
        let b = segment[1];
        let b_ts_ms = b.ts_ms.max(0.0).round() as u64;
        let age = ts.saturating_sub(b_ts_ms).min(trail_window_ms);
        let alpha = compute_trail_alpha(age, trail_window_ms, config.trail_max_alpha);
        if alpha == 0 {
            continue;
        }
        draw_line(
            surface,
            a.x.round() as i32,
            a.y.round() as i32,
            b.x.round() as i32,
            b.y.round() as i32,
            [
                config.trail_color[0],
                config.trail_color[1],
                config.trail_color[2],
                alpha,
            ],
            config.trail_thickness.max(0),
        );
    }
}

#[derive(Clone, Copy, Debug)]
struct TrailCurvePoint {
    x: f32,
    y: f32,
    ts_ms: f32,
}

#[derive(Clone, Debug)]
struct TrailRenderSegment {
    end_ts_ms: u64,
    spans: Box<[TrailRasterSpan]>,
}

#[derive(Clone, Copy, Debug)]
struct TrailRasterPoint {
    x: i32,
    y: i32,
}

#[derive(Clone, Copy, Debug)]
struct TrailRasterSpan {
    y: i32,
    x_start: i32,
    x_end: i32,
}

fn compile_trail_segments(
    points: &[TrailCurvePoint],
    smooth_step_px: f32,
    thickness: i32,
    width: u32,
    height: u32,
) -> Vec<TrailRenderSegment> {
    if points.len() < 2 || width == 0 || height == 0 {
        return Vec::new();
    }

    let mut smoothed = Vec::new();
    let effective_smooth_step_px =
        smooth_step_px.max((thickness.max(0).saturating_mul(2) + 1) as f32);
    build_smoothed_trail_points_into(points, effective_smooth_step_px, &mut smoothed);
    compact_trail_render_points(&mut smoothed);
    if smoothed.len() < 2 {
        return Vec::new();
    }

    let width_usize = width as usize;
    let height_i32 = height.min(i32::MAX as u32) as i32;
    let width_i32 = width.min(i32::MAX as u32) as i32;
    let mut latest_ts_keys = vec![0u32; width as usize * height as usize];

    for pair in smoothed.windows(2) {
        let a = pair[0];
        let b = pair[1];
        let end_ts_ms = b.ts_ms.max(0.0).round() as u64;
        let end_ts_key =
            (end_ts_ms.min(u64::from(u32::MAX).saturating_sub(1)) as u32).saturating_add(1);
        for span in rasterize_line_spans(
            a.x.round() as i32,
            a.y.round() as i32,
            b.x.round() as i32,
            b.y.round() as i32,
            thickness.max(0),
        ) {
            if span.y < 0 || span.y >= height_i32 {
                continue;
            }
            let x_start = span.x_start.max(0);
            let x_end = span.x_end.min(width_i32.saturating_sub(1));
            if x_start > x_end {
                continue;
            }

            let row_offset = span.y as usize * width_usize;
            for x in x_start..=x_end {
                let cell = &mut latest_ts_keys[row_offset + x as usize];
                if *cell < end_ts_key {
                    *cell = end_ts_key;
                }
            }
        }
    }

    let mut grouped_spans = HashMap::<u32, Vec<TrailRasterSpan>>::new();
    for y in 0..height as usize {
        let row = &latest_ts_keys[y * width_usize..(y + 1) * width_usize];
        let mut x = 0usize;
        while x < row.len() {
            let ts_key = row[x];
            if ts_key == 0 {
                x += 1;
                continue;
            }
            let start = x;
            x += 1;
            while x < row.len() && row[x] == ts_key {
                x += 1;
            }
            grouped_spans
                .entry(ts_key - 1)
                .or_default()
                .push(TrailRasterSpan {
                    y: y as i32,
                    x_start: start as i32,
                    x_end: x.saturating_sub(1) as i32,
                });
        }
    }

    let mut timestamps = grouped_spans.keys().copied().collect::<Vec<_>>();
    timestamps.sort_unstable();
    let mut segments = Vec::with_capacity(timestamps.len());
    for ts in timestamps {
        if let Some(spans) = grouped_spans.remove(&ts) {
            segments.push(TrailRenderSegment {
                end_ts_ms: u64::from(ts),
                spans: spans.into_boxed_slice(),
            });
        }
    }

    segments
}

fn rasterize_line_spans(
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,
    thickness: i32,
) -> Vec<TrailRasterSpan> {
    let points = rasterize_line_points(x0, y0, x1, y1);
    if points.is_empty() {
        return Vec::new();
    }

    let min_y = points.iter().map(|point| point.y).min().unwrap_or(0) - thickness;
    let max_y = points.iter().map(|point| point.y).max().unwrap_or(0) + thickness;
    let row_count = (max_y - min_y + 1).max(0) as usize;
    let mut row_intervals = vec![Vec::<(i32, i32)>::new(); row_count];

    for point in points {
        let span_start = point.x - thickness;
        let span_end = point.x + thickness;
        for y in point.y - thickness..=point.y + thickness {
            row_intervals[(y - min_y) as usize].push((span_start, span_end));
        }
    }

    let mut spans = Vec::new();
    for (row_idx, intervals) in row_intervals.iter_mut().enumerate() {
        if intervals.is_empty() {
            continue;
        }

        intervals.sort_unstable_by_key(|(start, end)| (*start, *end));
        let mut current = intervals[0];
        for &(start, end) in &intervals[1..] {
            if start <= current.1.saturating_add(1) {
                current.1 = current.1.max(end);
            } else {
                spans.push(TrailRasterSpan {
                    y: min_y + row_idx as i32,
                    x_start: current.0,
                    x_end: current.1,
                });
                current = (start, end);
            }
        }
        spans.push(TrailRasterSpan {
            y: min_y + row_idx as i32,
            x_start: current.0,
            x_end: current.1,
        });
    }

    spans
}

fn rasterize_line_points(x0: i32, y0: i32, x1: i32, y1: i32) -> Vec<TrailRasterPoint> {
    let mut points = Vec::new();
    let mut x0 = x0;
    let mut y0 = y0;
    let dx = (x1 - x0).abs();
    let sx = if x0 < x1 { 1 } else { -1 };
    let dy = -(y1 - y0).abs();
    let sy = if y0 < y1 { 1 } else { -1 };
    let mut err = dx + dy;

    loop {
        points.push(TrailRasterPoint { x: x0, y: y0 });
        if x0 == x1 && y0 == y1 {
            break;
        }
        let e2 = err * 2;
        if e2 >= dy {
            err += dy;
            x0 += sx;
        }
        if e2 <= dx {
            err += dx;
            y0 += sy;
        }
    }

    points
}

fn compact_trail_render_points(points: &mut Vec<TrailCurvePoint>) {
    if points.len() < 2 {
        return;
    }

    let mut compacted: Vec<TrailCurvePoint> = Vec::with_capacity(points.len());
    for point in points.iter().copied() {
        let rounded = TrailCurvePoint {
            x: point.x.round(),
            y: point.y.round(),
            ts_ms: point.ts_ms,
        };
        if let Some(last) = compacted.last_mut()
            && last.x == rounded.x
            && last.y == rounded.y
        {
            last.ts_ms = rounded.ts_ms;
            continue;
        }
        compacted.push(rounded);
    }

    *points = compacted;
}

#[cfg_attr(not(test), allow(dead_code))]
fn collect_visible_trail_window(samples: &[MouseSample], cutoff_ms: u64) -> Vec<TrailCurvePoint> {
    let mut window = Vec::new();
    collect_visible_trail_window_into(samples, cutoff_ms, &mut window);
    window
}

fn collect_visible_trail_window_into(
    samples: &[MouseSample],
    cutoff_ms: u64,
    out: &mut Vec<TrailCurvePoint>,
) {
    let mut points = Vec::with_capacity(samples.len());
    points.extend(
        samples
            .iter()
            .filter(|sample| sample.visible)
            .map(|sample| TrailCurvePoint {
                x: sample.x as f32,
                y: sample.y as f32,
                ts_ms: sample.ts_ms as f32,
            }),
    );
    collect_visible_trail_window_points_into(&points, cutoff_ms as f32, out);
}

fn collect_visible_trail_window_points_into(
    points: &[TrailCurvePoint],
    cutoff: f32,
    out: &mut Vec<TrailCurvePoint>,
) {
    out.clear();

    let mut last_before_cutoff = None::<TrailCurvePoint>;
    let mut pushed_window_start = false;

    for &point in points {
        if point.ts_ms < cutoff {
            last_before_cutoff = Some(point);
            continue;
        }

        if !pushed_window_start {
            if let Some(prev) = last_before_cutoff
                && cutoff > prev.ts_ms
                && cutoff < point.ts_ms
            {
                let t = (cutoff - prev.ts_ms) / (point.ts_ms - prev.ts_ms);
                out.push(TrailCurvePoint {
                    x: prev.x + (point.x - prev.x) * t,
                    y: prev.y + (point.y - prev.y) * t,
                    ts_ms: cutoff,
                });
            }
            pushed_window_start = true;
        }

        out.push(point);
    }

    if !pushed_window_start {
        out.clear();
    }
}

#[cfg_attr(not(test), allow(dead_code))]
fn build_smoothed_trail_points(samples: &[TrailCurvePoint], step_px: f32) -> Vec<TrailCurvePoint> {
    let mut points = Vec::new();
    build_smoothed_trail_points_into(samples, step_px, &mut points);
    points
}

fn build_smoothed_trail_points_into(
    samples: &[TrailCurvePoint],
    step_px: f32,
    out: &mut Vec<TrailCurvePoint>,
) {
    out.clear();
    if samples.is_empty() {
        return;
    }

    let step_px = step_px.max(1.0);
    let first = samples[0];
    out.push(first);
    if samples.len() == 1 {
        return;
    }

    if samples.len() == 2 {
        append_linear_segment(out, first, samples[1], step_px);
        return;
    }

    let first_mid = midpoint(samples[0], samples[1]);
    append_linear_segment(out, first, first_mid, step_px);

    for idx in 1..samples.len() - 1 {
        let prev = samples[idx - 1];
        let current = samples[idx];
        let next = samples[idx + 1];
        let start = midpoint(prev, current);
        let end = midpoint(current, next);
        append_quadratic_segment(out, start, current, end, step_px);
    }

    let last_mid = midpoint(samples[samples.len() - 2], samples[samples.len() - 1]);
    let last = samples[samples.len() - 1];
    append_linear_segment(out, last_mid, last, step_px);
}

fn midpoint(a: TrailCurvePoint, b: TrailCurvePoint) -> TrailCurvePoint {
    TrailCurvePoint {
        x: (a.x + b.x) * 0.5,
        y: (a.y + b.y) * 0.5,
        ts_ms: (a.ts_ms + b.ts_ms) * 0.5,
    }
}

fn append_linear_segment(
    out: &mut Vec<TrailCurvePoint>,
    start: TrailCurvePoint,
    end: TrailCurvePoint,
    step_px: f32,
) {
    let dx = end.x - start.x;
    let dy = end.y - start.y;
    let dist = (dx * dx + dy * dy).sqrt();
    let steps = (dist / step_px).ceil().max(1.0) as usize;

    for step in 1..=steps {
        let t = step as f32 / steps as f32;
        let x = start.x + (end.x - start.x) * t;
        let y = start.y + (end.y - start.y) * t;
        let ts_ms = start.ts_ms + (end.ts_ms - start.ts_ms) * t;
        out.push(TrailCurvePoint { x, y, ts_ms });
    }
}

fn append_quadratic_segment(
    out: &mut Vec<TrailCurvePoint>,
    start: TrailCurvePoint,
    control: TrailCurvePoint,
    end: TrailCurvePoint,
    step_px: f32,
) {
    let approx_len = ((control.x - start.x).powi(2) + (control.y - start.y).powi(2)).sqrt()
        + ((end.x - control.x).powi(2) + (end.y - control.y).powi(2)).sqrt();
    let steps = (approx_len / step_px).ceil().max(1.0) as usize;

    for step in 1..=steps {
        let t = step as f32 / steps as f32;
        let inv = 1.0 - t;
        let x = inv * inv * start.x + 2.0 * inv * t * control.x + t * t * end.x;
        let y = inv * inv * start.y + 2.0 * inv * t * control.y + t * t * end.y;
        let ts_ms = inv * inv * start.ts_ms + 2.0 * inv * t * control.ts_ms + t * t * end.ts_ms;
        out.push(TrailCurvePoint { x, y, ts_ms });
    }
}

#[cfg_attr(not(test), allow(dead_code))]
fn draw_click_ripples(surface: &mut FrameSurfaceMut<'_>, clicks: &[MouseClickDown], ts: u64) {
    let start = clicks.partition_point(|click| click.ts_ms.saturating_add(CLICK_RIPPLE_MS) < ts);
    draw_click_ripples_from(surface, clicks, start, ts);
}

fn draw_click_ripples_from(
    surface: &mut FrameSurfaceMut<'_>,
    clicks: &[MouseClickDown],
    start: usize,
    ts: u64,
) {
    let ripple_ms = 350u64;
    for click in &clicks[start..] {
        if click.ts_ms > ts {
            break;
        }
        if ts > click.ts_ms.saturating_add(ripple_ms) {
            continue;
        }
        let t = (ts - click.ts_ms) as f32 / ripple_ms as f32;
        let radius = (5.0 + 26.0 * t).round() as i32;
        let alpha = ((1.0 - t) * 220.0).round() as u8;
        draw_circle_outline(surface, click.x, click.y, radius, [255, 0, 0, alpha]);
    }
}

fn draw_cursor(surface: &mut FrameSurfaceMut<'_>, current: &MouseSample, tracks: &MouseTracks) {
    if let Some(shape) = compiled_cursor_shape(current, tracks) {
        draw_compiled_cursor_shape(
            surface,
            current.x.saturating_sub(shape.hotspot_x),
            current.y.saturating_sub(shape.hotspot_y),
            shape,
        );
    }
}

fn compiled_cursor_shape<'a>(
    current: &MouseSample,
    tracks: &'a MouseTracks,
) -> Option<&'a CompiledCursorShapePlan> {
    current.shape_id.and_then(|shape_id| {
        let CompiledCursorShape::Plan(shape) = tracks.cursor_shapes.get(&shape_id)? else {
            return None;
        };
        Some(shape)
    })
}

struct Yuv420pFrameViewMut<'a> {
    width: usize,
    height: usize,
    y: &'a mut [u8],
    y_stride: usize,
    u: &'a mut [u8],
    u_stride: usize,
    v: &'a mut [u8],
    v_stride: usize,
}

struct Nv12FrameViewMut<'a> {
    width: usize,
    height: usize,
    y: &'a mut [u8],
    y_stride: usize,
    uv: &'a mut [u8],
    uv_stride: usize,
}

enum NativeFrameViewMut<'a> {
    Yuv420p(Yuv420pFrameViewMut<'a>),
    Nv12(Nv12FrameViewMut<'a>),
}

#[derive(Clone, Copy)]
struct YuvBlendColor {
    y: u8,
    u: u8,
    v: u8,
    a: u8,
}

#[cfg(test)]
impl YuvBlendColor {
    #[inline(always)]
    fn from_rgba(color: [u8; 4]) -> Self {
        let (y, u, v) = rgb_to_yuv420p_pixel(color[0], color[1], color[2]);
        Self {
            y,
            u,
            v,
            a: color[3],
        }
    }
}

impl NativeFrameViewMut<'_> {
    #[inline(always)]
    fn width(&self) -> usize {
        match self {
            Self::Yuv420p(frame) => frame.width,
            Self::Nv12(frame) => frame.width,
        }
    }

    #[inline(always)]
    fn height(&self) -> usize {
        match self {
            Self::Yuv420p(frame) => frame.height,
            Self::Nv12(frame) => frame.height,
        }
    }

    #[inline(always)]
    fn blend_pixel(&mut self, x: i32, y: i32, color: YuvBlendColor) {
        if color.a == 0 || x < 0 || y < 0 || x >= self.width() as i32 || y >= self.height() as i32 {
            return;
        }

        let x = x as usize;
        let y = y as usize;
        match self {
            Self::Yuv420p(frame) => {
                let y_idx = y.saturating_mul(frame.y_stride) + x;
                if let Some(dst_y) = frame.y.get_mut(y_idx) {
                    blend_yuv_sample(dst_y, color.y, color.a);
                }

                let chroma_x = x / 2;
                let chroma_y = y / 2;
                let u_idx = chroma_y.saturating_mul(frame.u_stride) + chroma_x;
                let v_idx = chroma_y.saturating_mul(frame.v_stride) + chroma_x;
                if let Some(dst_u) = frame.u.get_mut(u_idx) {
                    blend_yuv_sample(dst_u, color.u, color.a);
                }
                if let Some(dst_v) = frame.v.get_mut(v_idx) {
                    blend_yuv_sample(dst_v, color.v, color.a);
                }
            }
            Self::Nv12(frame) => {
                let y_idx = y.saturating_mul(frame.y_stride) + x;
                if let Some(dst_y) = frame.y.get_mut(y_idx) {
                    blend_yuv_sample(dst_y, color.y, color.a);
                }

                let chroma_x = x / 2;
                let chroma_y = y / 2;
                let uv_idx = chroma_y.saturating_mul(frame.uv_stride) + chroma_x.saturating_mul(2);
                if uv_idx + 1 < frame.uv.len() {
                    blend_yuv_sample(&mut frame.uv[uv_idx], color.u, color.a);
                    blend_yuv_sample(&mut frame.uv[uv_idx + 1], color.v, color.a);
                }
            }
        }
    }

    #[inline(always)]
    fn blend_horizontal_span(&mut self, y: i32, x_start: i32, x_end: i32, color: YuvBlendColor) {
        if color.a == 0 || y < 0 || y >= self.height() as i32 {
            return;
        }

        let x_start = x_start.max(0) as usize;
        let x_end = x_end.min(self.width() as i32 - 1) as usize;
        if x_start > x_end {
            return;
        }

        let y = y as usize;
        match self {
            Self::Yuv420p(frame) => blend_horizontal_span_yuv420p(frame, y, x_start, x_end, color),
            Self::Nv12(frame) => blend_horizontal_span_nv12(frame, y, x_start, x_end, color),
        }
    }

    #[inline(always)]
    fn blend_cursor_row(
        &mut self,
        dst_x: usize,
        dst_y: usize,
        src_yuva: &[u8],
        kind: CompiledCursorRunKind,
    ) {
        match self {
            Self::Yuv420p(frame) => blend_cursor_row_yuv420p(frame, dst_x, dst_y, src_yuva, kind),
            Self::Nv12(frame) => blend_cursor_row_nv12(frame, dst_x, dst_y, src_yuva, kind),
        }
    }
}

#[inline(always)]
fn supports_native_overlay(format: ffmpeg::format::Pixel) -> bool {
    matches!(
        format,
        ffmpeg::format::Pixel::YUV420P | ffmpeg::format::Pixel::NV12
    )
}

fn native_plane_geometry(
    format: ffmpeg::format::Pixel,
    plane: usize,
    width: u32,
    height: u32,
) -> Option<(usize, usize)> {
    let width = width.max(1) as usize;
    let height = height.max(1) as usize;
    match format {
        ffmpeg::format::Pixel::YUV420P => match plane {
            0 => Some((width, height)),
            1 | 2 => Some((width.div_ceil(2), height.div_ceil(2))),
            _ => None,
        },
        ffmpeg::format::Pixel::NV12 => match plane {
            0 => Some((width, height)),
            1 => Some((width.div_ceil(2) * 2, height.div_ceil(2))),
            _ => None,
        },
        _ => None,
    }
}

fn copy_native_video_frame_into(
    dst: &mut ffmpeg::frame::Video,
    src: &ffmpeg::frame::Video,
) -> Result<()> {
    if dst.format() != src.format() || dst.width() != src.width() || dst.height() != src.height() {
        return Err(ScreenRecorderError::Export(
            "native overlay frame copy requires matching format and dimensions".to_string(),
        ));
    }
    if !supports_native_overlay(src.format()) {
        return Err(ScreenRecorderError::Export(
            "native overlay frame copy requires a supported pixel format".to_string(),
        ));
    }

    ensure_video_frame_writable(dst)?;
    for plane in 0..4 {
        let Some((row_bytes, rows)) =
            native_plane_geometry(src.format(), plane, src.width(), src.height())
        else {
            break;
        };
        let src_stride = src.stride(plane);
        let dst_stride = dst.stride(plane);
        let src_data = src.data(plane);
        let dst_data = dst.data_mut(plane);

        if src_stride == dst_stride {
            let plane_bytes = src_stride.saturating_mul(rows);
            if plane_bytes <= src_data.len() && plane_bytes <= dst_data.len() {
                unsafe {
                    ptr::copy_nonoverlapping(src_data.as_ptr(), dst_data.as_mut_ptr(), plane_bytes);
                }
                continue;
            }
        }

        for row in 0..rows {
            let src_start = row.saturating_mul(src_stride);
            let dst_start = row.saturating_mul(dst_stride);
            let src_end = src_start.saturating_add(row_bytes);
            let dst_end = dst_start.saturating_add(row_bytes);
            if src_end > src_data.len() || dst_end > dst_data.len() {
                return Err(ScreenRecorderError::Export(
                    "native overlay frame plane copy exceeded available data".to_string(),
                ));
            }

            unsafe {
                ptr::copy_nonoverlapping(
                    src_data.as_ptr().add(src_start),
                    dst_data.as_mut_ptr().add(dst_start),
                    row_bytes,
                );
            }
        }
    }

    Ok(())
}

fn prepare_native_overlay_frame(
    src: &ffmpeg::frame::Video,
    scratch: &mut Option<ffmpeg::frame::Video>,
    scratch_key: &mut Option<(u32, u32, ffmpeg::format::Pixel)>,
) -> Result<bool> {
    let format = src.format();
    if !supports_native_overlay(format) {
        return Ok(false);
    }

    let key = (src.width(), src.height(), format);
    if scratch_key.as_ref() != Some(&key) {
        *scratch = Some(ffmpeg::frame::Video::new(format, src.width(), src.height()));
        *scratch_key = Some(key);
    }

    let scratch_ref = scratch.as_mut().ok_or_else(|| {
        ScreenRecorderError::Export("native overlay scratch frame is uninitialized".to_string())
    })?;
    copy_native_video_frame_into(scratch_ref, src)?;
    Ok(true)
}

fn native_frame_view_mut(
    frame: &mut ffmpeg::frame::Video,
) -> Result<Option<NativeFrameViewMut<'_>>> {
    Ok(match frame.format() {
        ffmpeg::format::Pixel::YUV420P => {
            Some(NativeFrameViewMut::Yuv420p(yuv420p_frame_view_mut(frame)?))
        }
        ffmpeg::format::Pixel::NV12 => Some(NativeFrameViewMut::Nv12(nv12_frame_view_mut(frame)?)),
        _ => None,
    })
}

fn nv12_frame_view_mut(frame: &mut ffmpeg::frame::Video) -> Result<Nv12FrameViewMut<'_>> {
    if frame.format() != ffmpeg::format::Pixel::NV12 {
        return Err(ScreenRecorderError::Export(
            "decoded frame is not NV12".to_string(),
        ));
    }

    let width = frame.width() as usize;
    let height = frame.height() as usize;
    let y_stride = frame.stride(0);
    let uv_stride = frame.stride(1);
    let chroma_h = height.div_ceil(2);
    let raw = unsafe { frame.as_mut_ptr() };
    let (y, uv) = unsafe {
        let y_ptr = (*raw).data[0];
        let uv_ptr = (*raw).data[1];
        if y_ptr.is_null() || uv_ptr.is_null() {
            return Err(ScreenRecorderError::Export(
                "decoded NV12 frame is missing plane data".to_string(),
            ));
        }
        (
            std::slice::from_raw_parts_mut(y_ptr, y_stride.saturating_mul(height)),
            std::slice::from_raw_parts_mut(uv_ptr, uv_stride.saturating_mul(chroma_h)),
        )
    };

    Ok(Nv12FrameViewMut {
        width,
        height,
        y,
        y_stride,
        uv,
        uv_stride,
    })
}

fn draw_compiled_trail_segments_native(
    surface: &mut NativeFrameViewMut<'_>,
    segments: &[TrailRenderSegment],
    ts: u64,
    config: &MouseEditConfig,
) {
    let trail_window_ms = config.trail_window_ms.max(1);
    let (trail_y, trail_u, trail_v) = rgb_to_yuv420p_pixel(
        config.trail_color[0],
        config.trail_color[1],
        config.trail_color[2],
    );
    for segment in segments {
        let age = ts.saturating_sub(segment.end_ts_ms).min(trail_window_ms);
        let alpha = compute_trail_alpha(age, trail_window_ms, config.trail_max_alpha);
        if alpha == 0 {
            continue;
        }

        let color = YuvBlendColor {
            y: trail_y,
            u: trail_u,
            v: trail_v,
            a: alpha,
        };
        for span in segment.spans.iter().copied() {
            surface.blend_horizontal_span(span.y, span.x_start, span.x_end, color);
        }
    }
}

fn draw_click_ripples_from_native(
    surface: &mut NativeFrameViewMut<'_>,
    clicks: &[MouseClickDown],
    start: usize,
    ts: u64,
) {
    let ripple_ms = 350u64;
    let (click_y, click_u, click_v) = rgb_to_yuv420p_pixel(255, 0, 0);
    for click in &clicks[start..] {
        if click.ts_ms > ts {
            break;
        }
        if ts > click.ts_ms.saturating_add(ripple_ms) {
            continue;
        }
        let t = (ts - click.ts_ms) as f32 / ripple_ms as f32;
        let radius = (5.0 + 26.0 * t).round() as i32;
        let alpha = ((1.0 - t) * 220.0).round() as u8;
        draw_circle_outline_native(
            surface,
            click.x,
            click.y,
            radius,
            YuvBlendColor {
                y: click_y,
                u: click_u,
                v: click_v,
                a: alpha,
            },
        );
    }
}

fn draw_cursor_native(
    surface: &mut NativeFrameViewMut<'_>,
    current: &MouseSample,
    tracks: &MouseTracks,
) {
    if let Some(shape) = compiled_cursor_shape(current, tracks)
        && shape.yuv420p_compatible
    {
        draw_compiled_cursor_shape_native(
            surface,
            current.x.saturating_sub(shape.hotspot_x),
            current.y.saturating_sub(shape.hotspot_y),
            shape,
        );
    }
}

fn draw_compiled_cursor_shape_native(
    surface: &mut NativeFrameViewMut<'_>,
    origin_x: i32,
    origin_y: i32,
    shape: &CompiledCursorShapePlan,
) {
    let Some(rect) = cursor_draw_rect(
        surface.width() as u32,
        surface.height() as u32,
        origin_x,
        origin_y,
        shape.width,
        shape.height,
    ) else {
        return;
    };

    let visible_src_start_x = rect.src_start_x;
    let visible_src_end_x = rect.src_start_x + rect.draw_w;

    for row in 0..rect.draw_h {
        let compiled_row = &shape.rows[rect.src_start_y + row];
        if compiled_row.runs.is_empty() {
            continue;
        }

        let dst_y = rect.dst_start_y + row;
        for run in compiled_row.runs.iter().copied() {
            let clipped_start_x = run.start_x.max(visible_src_start_x);
            let clipped_end_x = run.end_x.min(visible_src_end_x);
            if clipped_start_x >= clipped_end_x {
                continue;
            }

            let clipped_px_offset = clipped_start_x.saturating_sub(run.start_x);
            let dst_x = rect.dst_start_x + clipped_start_x.saturating_sub(visible_src_start_x);
            let draw_bytes = clipped_end_x
                .saturating_sub(clipped_start_x)
                .saturating_mul(4);
            let src_start = run.src_byte_start + clipped_px_offset.saturating_mul(4);
            let src_row = &shape.yuva[src_start..src_start + draw_bytes];
            blend_cursor_yuva_into_native_row(surface, dst_x, dst_y, src_row, run.kind);
        }
    }
}

fn blend_cursor_yuva_into_native_row(
    surface: &mut NativeFrameViewMut<'_>,
    dst_x: usize,
    dst_y: usize,
    src_yuva: &[u8],
    kind: CompiledCursorRunKind,
) {
    surface.blend_cursor_row(dst_x, dst_y, src_yuva, kind);
}

fn set_native_pixel_blended(
    surface: &mut NativeFrameViewMut<'_>,
    x: i32,
    y: i32,
    color: YuvBlendColor,
) {
    surface.blend_pixel(x, y, color);
}

fn draw_circle_outline_native(
    surface: &mut NativeFrameViewMut<'_>,
    cx: i32,
    cy: i32,
    radius: i32,
    color: YuvBlendColor,
) {
    if radius <= 0 {
        return;
    }
    let mut x = radius;
    let mut y = 0;
    let mut err = 0;

    while x >= y {
        for (dx, dy) in [
            (x, y),
            (y, x),
            (-y, x),
            (-x, y),
            (-x, -y),
            (-y, -x),
            (y, -x),
            (x, -y),
        ] {
            set_native_pixel_blended(surface, cx + dx, cy + dy, color);
        }

        y += 1;
        if err <= 0 {
            err += 2 * y + 1;
        }
        if err > 0 {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
}

fn try_apply_mouse_overlays_native_from_decision_impl(
    frame: &mut ffmpeg::frame::Video,
    timestamp_ms: u64,
    tracks: &MouseTracks,
    config: &MouseEditConfig,
    state: &mut OverlaySearchState,
    decision: OverlayDecision,
    ensure_writable: bool,
) -> Result<bool> {
    if !decision.needs_draw() {
        return Ok(false);
    }

    if ensure_writable {
        ensure_video_frame_writable(frame)?;
    }
    let Some(mut surface) = native_frame_view_mut(frame)? else {
        return Ok(false);
    };
    let Some(current_idx) = decision.current_idx else {
        return Ok(false);
    };
    if decision.has_cursor
        && !compiled_cursor_shape(&tracks.samples[current_idx], tracks)
            .is_some_and(|shape| shape.yuv420p_compatible)
    {
        // Mask-copy/XOR cursor operations depend on the RGB destination and
        // cannot be reproduced exactly in subsampled YUV. Let the caller use
        // the RGBA compositor for the entire overlay frame.
        return Ok(false);
    }

    if decision.has_trail {
        draw_compiled_trail_segments_native(
            &mut surface,
            &tracks.trail_segments[state.trail_segment_start_idx..state.trail_segment_next_idx],
            timestamp_ms,
            config,
        );
    }
    if decision.has_clicks {
        draw_click_ripples_from_native(
            &mut surface,
            &tracks.click_downs,
            state.click_start_idx,
            timestamp_ms,
        );
    }
    if decision.has_cursor {
        draw_cursor_native(&mut surface, &tracks.samples[current_idx], tracks);
    }

    Ok(true)
}

#[cfg_attr(not(test), allow(dead_code))]
fn try_apply_mouse_overlays_native_from_decision(
    frame: &mut ffmpeg::frame::Video,
    timestamp_ms: u64,
    tracks: &MouseTracks,
    config: &MouseEditConfig,
    state: &mut OverlaySearchState,
    decision: OverlayDecision,
) -> Result<bool> {
    try_apply_mouse_overlays_native_from_decision_impl(
        frame,
        timestamp_ms,
        tracks,
        config,
        state,
        decision,
        true,
    )
}

fn yuv420p_frame_view_mut(frame: &mut ffmpeg::frame::Video) -> Result<Yuv420pFrameViewMut<'_>> {
    if frame.format() != ffmpeg::format::Pixel::YUV420P {
        return Err(ScreenRecorderError::Export(
            "decoded frame is not YUV420P".to_string(),
        ));
    }

    let width = frame.width() as usize;
    let height = frame.height() as usize;
    let y_stride = frame.stride(0);
    let u_stride = frame.stride(1);
    let v_stride = frame.stride(2);
    let chroma_h = height.div_ceil(2);
    let raw = unsafe { frame.as_mut_ptr() };
    let (y, u, v) = unsafe {
        let y_ptr = (*raw).data[0];
        let u_ptr = (*raw).data[1];
        let v_ptr = (*raw).data[2];
        if y_ptr.is_null() || u_ptr.is_null() || v_ptr.is_null() {
            return Err(ScreenRecorderError::Export(
                "decoded YUV420P frame is missing plane data".to_string(),
            ));
        }
        (
            std::slice::from_raw_parts_mut(y_ptr, y_stride.saturating_mul(height)),
            std::slice::from_raw_parts_mut(u_ptr, u_stride.saturating_mul(chroma_h)),
            std::slice::from_raw_parts_mut(v_ptr, v_stride.saturating_mul(chroma_h)),
        )
    };

    Ok(Yuv420pFrameViewMut {
        width,
        height,
        y,
        y_stride,
        u,
        u_stride,
        v,
        v_stride,
    })
}

#[inline(always)]
fn blend_yuv_sample(dst: &mut u8, src: u8, alpha: u8) {
    if alpha == 255 {
        *dst = src;
        return;
    }

    let alpha_u16 = u16::from(alpha);
    let inv_alpha = 255u16.saturating_sub(alpha_u16);
    *dst = ((u16::from(src) * alpha_u16 + u16::from(*dst) * inv_alpha) / 255) as u8;
}

#[inline(always)]
fn blend_yuv_sample_repeated(dst: &mut u8, src: u8, alpha: u8, repeats: usize) {
    match repeats {
        0 => {}
        _ if alpha == 255 => *dst = src,
        _ => {
            for _ in 0..repeats {
                blend_yuv_sample(dst, src, alpha);
            }
        }
    }
}

#[inline(always)]
fn blend_horizontal_span_yuv420p(
    frame: &mut Yuv420pFrameViewMut<'_>,
    y: usize,
    x_start: usize,
    x_end: usize,
    color: YuvBlendColor,
) {
    debug_assert!(y < frame.height);
    debug_assert!(x_start <= x_end && x_end < frame.width);

    let luma_row_start = y.saturating_mul(frame.y_stride).saturating_add(x_start);
    let luma_len = x_end.saturating_sub(x_start).saturating_add(1);
    let luma_row = &mut frame.y[luma_row_start..luma_row_start + luma_len];
    if color.a == 255 {
        luma_row.fill(color.y);
    } else {
        for sample in luma_row {
            blend_yuv_sample(sample, color.y, color.a);
        }
    }

    let chroma_y = y / 2;
    let u_row_start = chroma_y.saturating_mul(frame.u_stride);
    let v_row_start = chroma_y.saturating_mul(frame.v_stride);
    for chroma_x in (x_start / 2)..=(x_end / 2) {
        let sample_x_start = chroma_x.saturating_mul(2);
        let overlap_start = x_start.max(sample_x_start);
        let overlap_end = x_end.min(sample_x_start.saturating_add(1));
        let repeats = overlap_end.saturating_sub(overlap_start).saturating_add(1);
        blend_yuv_sample_repeated(
            &mut frame.u[u_row_start + chroma_x],
            color.u,
            color.a,
            repeats,
        );
        blend_yuv_sample_repeated(
            &mut frame.v[v_row_start + chroma_x],
            color.v,
            color.a,
            repeats,
        );
    }
}

#[inline(always)]
fn blend_horizontal_span_nv12(
    frame: &mut Nv12FrameViewMut<'_>,
    y: usize,
    x_start: usize,
    x_end: usize,
    color: YuvBlendColor,
) {
    debug_assert!(y < frame.height);
    debug_assert!(x_start <= x_end && x_end < frame.width);

    let luma_row_start = y.saturating_mul(frame.y_stride).saturating_add(x_start);
    let luma_len = x_end.saturating_sub(x_start).saturating_add(1);
    let luma_row = &mut frame.y[luma_row_start..luma_row_start + luma_len];
    if color.a == 255 {
        luma_row.fill(color.y);
    } else {
        for sample in luma_row {
            blend_yuv_sample(sample, color.y, color.a);
        }
    }

    let chroma_y = y / 2;
    let uv_row_start = chroma_y.saturating_mul(frame.uv_stride);
    for chroma_x in (x_start / 2)..=(x_end / 2) {
        let sample_x_start = chroma_x.saturating_mul(2);
        let overlap_start = x_start.max(sample_x_start);
        let overlap_end = x_end.min(sample_x_start.saturating_add(1));
        let repeats = overlap_end.saturating_sub(overlap_start).saturating_add(1);
        let uv_idx = uv_row_start + chroma_x.saturating_mul(2);
        blend_yuv_sample_repeated(&mut frame.uv[uv_idx], color.u, color.a, repeats);
        blend_yuv_sample_repeated(&mut frame.uv[uv_idx + 1], color.v, color.a, repeats);
    }
}

#[inline(always)]
fn blend_cursor_row_yuv420p(
    frame: &mut Yuv420pFrameViewMut<'_>,
    dst_x: usize,
    dst_y: usize,
    src_yuva: &[u8],
    kind: CompiledCursorRunKind,
) {
    debug_assert_eq!(src_yuva.len() % 4, 0);
    let pixel_count = src_yuva.len() / 4;
    debug_assert!(dst_y < frame.height);
    debug_assert!(dst_x.saturating_add(pixel_count) <= frame.width);

    let y_row_start = dst_y.saturating_mul(frame.y_stride).saturating_add(dst_x);
    let y_row = &mut frame.y[y_row_start..y_row_start + pixel_count];
    match kind {
        CompiledCursorRunKind::AlphaCopy => {
            for (dst, src) in y_row.iter_mut().zip(src_yuva.chunks_exact(4)) {
                *dst = src[0];
            }
            let chroma_y = dst_y / 2;
            let u_row_start = chroma_y.saturating_mul(frame.u_stride);
            let v_row_start = chroma_y.saturating_mul(frame.v_stride);
            for (pixel_idx, src) in src_yuva.chunks_exact(4).enumerate() {
                let chroma_x = (dst_x + pixel_idx) / 2;
                frame.u[u_row_start + chroma_x] = src[1];
                frame.v[v_row_start + chroma_x] = src[2];
            }
        }
        CompiledCursorRunKind::Blend => {
            for (dst, src) in y_row.iter_mut().zip(src_yuva.chunks_exact(4)) {
                if src[3] != 0 {
                    blend_yuv_sample(dst, src[0], src[3]);
                }
            }
            let chroma_y = dst_y / 2;
            let u_row_start = chroma_y.saturating_mul(frame.u_stride);
            let v_row_start = chroma_y.saturating_mul(frame.v_stride);
            for (pixel_idx, src) in src_yuva.chunks_exact(4).enumerate() {
                if src[3] == 0 {
                    continue;
                }
                let chroma_x = (dst_x + pixel_idx) / 2;
                blend_yuv_sample(&mut frame.u[u_row_start + chroma_x], src[1], src[3]);
                blend_yuv_sample(&mut frame.v[v_row_start + chroma_x], src[2], src[3]);
            }
        }
        _ => {}
    }
}

#[inline(always)]
fn blend_cursor_row_nv12(
    frame: &mut Nv12FrameViewMut<'_>,
    dst_x: usize,
    dst_y: usize,
    src_yuva: &[u8],
    kind: CompiledCursorRunKind,
) {
    debug_assert_eq!(src_yuva.len() % 4, 0);
    let pixel_count = src_yuva.len() / 4;
    debug_assert!(dst_y < frame.height);
    debug_assert!(dst_x.saturating_add(pixel_count) <= frame.width);

    let y_row_start = dst_y.saturating_mul(frame.y_stride).saturating_add(dst_x);
    let y_row = &mut frame.y[y_row_start..y_row_start + pixel_count];
    match kind {
        CompiledCursorRunKind::AlphaCopy => {
            for (dst, src) in y_row.iter_mut().zip(src_yuva.chunks_exact(4)) {
                *dst = src[0];
            }
            let chroma_y = dst_y / 2;
            let uv_row_start = chroma_y.saturating_mul(frame.uv_stride);
            for (pixel_idx, src) in src_yuva.chunks_exact(4).enumerate() {
                let chroma_x = (dst_x + pixel_idx) / 2;
                let uv_idx = uv_row_start + chroma_x.saturating_mul(2);
                frame.uv[uv_idx] = src[1];
                frame.uv[uv_idx + 1] = src[2];
            }
        }
        CompiledCursorRunKind::Blend => {
            for (dst, src) in y_row.iter_mut().zip(src_yuva.chunks_exact(4)) {
                if src[3] != 0 {
                    blend_yuv_sample(dst, src[0], src[3]);
                }
            }
            let chroma_y = dst_y / 2;
            let uv_row_start = chroma_y.saturating_mul(frame.uv_stride);
            for (pixel_idx, src) in src_yuva.chunks_exact(4).enumerate() {
                if src[3] == 0 {
                    continue;
                }
                let chroma_x = (dst_x + pixel_idx) / 2;
                let uv_idx = uv_row_start + chroma_x.saturating_mul(2);
                blend_yuv_sample(&mut frame.uv[uv_idx], src[1], src[3]);
                blend_yuv_sample(&mut frame.uv[uv_idx + 1], src[2], src[3]);
            }
        }
        _ => {}
    }
}

#[inline(always)]
fn rgb_to_yuv420p_pixel(r: u8, g: u8, b: u8) -> (u8, u8, u8) {
    let r = i32::from(r);
    let g = i32::from(g);
    let b = i32::from(b);

    let y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    let u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    let v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

    (
        y.clamp(0, 255) as u8,
        u.clamp(0, 255) as u8,
        v.clamp(0, 255) as u8,
    )
}

fn rgba_to_yuva(rgba: &[u8]) -> Box<[u8]> {
    let mut yuva = Vec::with_capacity(rgba.len());
    for pixel in rgba.chunks_exact(4) {
        let (y, u, v) = rgb_to_yuv420p_pixel(pixel[0], pixel[1], pixel[2]);
        yuva.extend_from_slice(&[y, u, v, pixel[3]]);
    }
    yuva.into_boxed_slice()
}

fn draw_compiled_cursor_shape(
    surface: &mut FrameSurfaceMut<'_>,
    origin_x: i32,
    origin_y: i32,
    shape: &CompiledCursorShapePlan,
) {
    let Some(rect) = cursor_draw_rect(
        surface.width,
        surface.height,
        origin_x,
        origin_y,
        shape.width,
        shape.height,
    ) else {
        return;
    };

    if surface.hdr.is_some() {
        // HDR preflight rejects destination-dependent mask/XOR cursor operations.
        for row in 0..rect.draw_h {
            for col in 0..rect.draw_w {
                let offset = ((rect.src_start_y + row) * shape.width + rect.src_start_x + col) * 4;
                set_pixel_blended(
                    surface,
                    (rect.dst_start_x + col) as i32,
                    (rect.dst_start_y + row) as i32,
                    shape.rgba[offset..offset + 4].try_into().unwrap(),
                );
            }
        }
        return;
    }

    let dst_row_bytes = surface.width as usize * 4;
    let dst_visible_start = rect.dst_start_x.saturating_mul(4);
    let draw_bytes = rect.draw_w.saturating_mul(4);
    let visible_src_start_x = rect.src_start_x;
    let visible_src_end_x = rect.src_start_x + rect.draw_w;

    for row in 0..rect.draw_h {
        let compiled_row = &shape.rows[rect.src_start_y + row];
        if compiled_row.runs.is_empty() {
            continue;
        }

        let dst_row_start = (rect.dst_start_y + row) * dst_row_bytes;
        let dst_visible_row = &mut surface.rgba
            [dst_row_start + dst_visible_start..dst_row_start + dst_visible_start + draw_bytes];
        render_compiled_cursor_row(
            dst_visible_row,
            visible_src_start_x,
            visible_src_end_x,
            &shape.rgba,
            &compiled_row.runs,
        );
    }
}

fn render_compiled_cursor_row(
    dst_visible_row: &mut [u8],
    visible_src_start_x: usize,
    visible_src_end_x: usize,
    rgba: &[u8],
    runs: &[CompiledCursorRun],
) {
    for run in runs {
        let clipped_start_x = run.start_x.max(visible_src_start_x);
        let clipped_end_x = run.end_x.min(visible_src_end_x);
        if clipped_start_x >= clipped_end_x {
            continue;
        }

        let clipped_px_offset = clipped_start_x.saturating_sub(run.start_x);
        let dst_px_offset = clipped_start_x.saturating_sub(visible_src_start_x);
        let draw_bytes = clipped_end_x
            .saturating_sub(clipped_start_x)
            .saturating_mul(4);
        let src_start = run.src_byte_start + clipped_px_offset.saturating_mul(4);
        let dst_start = dst_px_offset.saturating_mul(4);
        let src_row = &rgba[src_start..src_start + draw_bytes];
        let dst_row = &mut dst_visible_row[dst_start..dst_start + draw_bytes];

        match run.kind {
            CompiledCursorRunKind::AlphaCopy => dst_row.copy_from_slice(src_row),
            CompiledCursorRunKind::MaskCopy => copy_masked_cursor_row(dst_row, src_row),
            CompiledCursorRunKind::Xor => xor_cursor_row(dst_row, src_row),
            CompiledCursorRunKind::Blend => blend_cursor_alpha_row(dst_row, src_row),
        }
    }
}

#[derive(Clone, Copy)]
struct CursorDrawRect {
    dst_start_x: usize,
    dst_start_y: usize,
    src_start_x: usize,
    src_start_y: usize,
    draw_w: usize,
    draw_h: usize,
}

fn cursor_draw_rect(
    surface_w: u32,
    surface_h: u32,
    origin_x: i32,
    origin_y: i32,
    width: usize,
    height: usize,
) -> Option<CursorDrawRect> {
    if width == 0 || height == 0 || surface_w == 0 || surface_h == 0 {
        return None;
    }

    let frame_w = i64::from(surface_w);
    let frame_h = i64::from(surface_h);
    let left = i64::from(origin_x);
    let top = i64::from(origin_y);
    let right = left.saturating_add(width as i64);
    let bottom = top.saturating_add(height as i64);

    let clipped_left = left.clamp(0, frame_w);
    let clipped_top = top.clamp(0, frame_h);
    let clipped_right = right.clamp(0, frame_w);
    let clipped_bottom = bottom.clamp(0, frame_h);

    if clipped_left >= clipped_right || clipped_top >= clipped_bottom {
        return None;
    }

    Some(CursorDrawRect {
        dst_start_x: clipped_left as usize,
        dst_start_y: clipped_top as usize,
        src_start_x: clipped_left.saturating_sub(left) as usize,
        src_start_y: clipped_top.saturating_sub(top) as usize,
        draw_w: clipped_right.saturating_sub(clipped_left) as usize,
        draw_h: clipped_bottom.saturating_sub(clipped_top) as usize,
    })
}

fn blend_cursor_alpha_row(dst_rgba: &mut [u8], src_rgba: &[u8]) {
    debug_assert_eq!(dst_rgba.len(), src_rgba.len());
    debug_assert_eq!(dst_rgba.len() % 4, 0);

    let px_count = src_rgba.len() / 4;
    // SAFETY:
    // - Buffers are valid for `src_rgba.len()` / `dst_rgba.len()` bytes.
    // - Source and destination do not overlap.
    unsafe {
        let src = src_rgba.as_ptr();
        let dst = dst_rgba.as_mut_ptr();
        for px in 0..px_count {
            let idx = px * 4;
            let src_px = src.add(idx);
            let dst_px = dst.add(idx);
            let alpha = *src_px.add(3);
            if alpha == 0 {
                continue;
            }

            if alpha == 255 {
                *dst_px = *src_px;
                *dst_px.add(1) = *src_px.add(1);
                *dst_px.add(2) = *src_px.add(2);
                *dst_px.add(3) = 255;
                continue;
            }

            let alpha_u16 = u16::from(alpha);
            let inv_alpha = 255u16.saturating_sub(alpha_u16);
            *dst_px =
                ((u16::from(*src_px) * alpha_u16 + u16::from(*dst_px) * inv_alpha) / 255) as u8;
            *dst_px.add(1) = ((u16::from(*src_px.add(1)) * alpha_u16
                + u16::from(*dst_px.add(1)) * inv_alpha)
                / 255) as u8;
            *dst_px.add(2) = ((u16::from(*src_px.add(2)) * alpha_u16
                + u16::from(*dst_px.add(2)) * inv_alpha)
                / 255) as u8;
            *dst_px.add(3) = 255;
        }
    }
}

fn copy_masked_cursor_row(dst_rgba: &mut [u8], src_rgba: &[u8]) {
    debug_assert_eq!(dst_rgba.len(), src_rgba.len());
    debug_assert_eq!(dst_rgba.len() % 4, 0);

    let px_count = src_rgba.len() / 4;
    unsafe {
        let src = src_rgba.as_ptr();
        let dst = dst_rgba.as_mut_ptr();
        for px in 0..px_count {
            let idx = px * 4;
            let src_px = src.add(idx);
            let dst_px = dst.add(idx);
            *dst_px = *src_px;
            *dst_px.add(1) = *src_px.add(1);
            *dst_px.add(2) = *src_px.add(2);
            *dst_px.add(3) = 255;
        }
    }
}

fn xor_cursor_row(dst_rgba: &mut [u8], src_rgba: &[u8]) {
    debug_assert_eq!(dst_rgba.len(), src_rgba.len());
    debug_assert_eq!(dst_rgba.len() % 4, 0);

    let px_count = src_rgba.len() / 4;
    unsafe {
        let src = src_rgba.as_ptr();
        let dst = dst_rgba.as_mut_ptr();
        for px in 0..px_count {
            let idx = px * 4;
            let src_px = src.add(idx);
            let dst_px = dst.add(idx);
            *dst_px ^= *src_px;
            *dst_px.add(1) ^= *src_px.add(1);
            *dst_px.add(2) ^= *src_px.add(2);
            *dst_px.add(3) = 255;
        }
    }
}

fn pixel_offset(width: u32, height: u32, x: i32, y: i32) -> Option<usize> {
    if x < 0 || y < 0 || x >= width as i32 || y >= height as i32 {
        return None;
    }
    Some((y as usize * width as usize + x as usize) * 4)
}

#[inline(always)]
fn blend_u8(src: u8, dst: u8, alpha: u8) -> u8 {
    let alpha_u16 = u16::from(alpha);
    let inv_alpha = 255u16.saturating_sub(alpha_u16);
    ((u16::from(src) * alpha_u16 + u16::from(dst) * inv_alpha) / 255) as u8
}

fn set_pixel_blended(surface: &mut FrameSurfaceMut<'_>, x: i32, y: i32, color: [u8; 4]) {
    let Some(idx) = pixel_offset(surface.width, surface.height, x, y) else {
        return;
    };
    if let Some((rgb, stride)) = surface.hdr.as_mut() {
        let start = y as usize * *stride + x as usize * 6;
        snow_media::color::blend_srgb_into_pq(
            (&mut rgb[start..start + 6]).try_into().unwrap(),
            color,
        );
        return;
    }
    let rgba = &mut surface.rgba;
    let alpha = color[3];
    if alpha == 0 {
        return;
    }
    if alpha == 255 {
        rgba[idx] = color[0];
        rgba[idx + 1] = color[1];
        rgba[idx + 2] = color[2];
        rgba[idx + 3] = 255;
        return;
    }
    rgba[idx] = blend_u8(color[0], rgba[idx], alpha);
    rgba[idx + 1] = blend_u8(color[1], rgba[idx + 1], alpha);
    rgba[idx + 2] = blend_u8(color[2], rgba[idx + 2], alpha);
    rgba[idx + 3] = 255;
}

fn draw_line(
    surface: &mut FrameSurfaceMut<'_>,
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,
    color: [u8; 4],
    thickness: i32,
) {
    let mut x0 = x0;
    let mut y0 = y0;
    let dx = (x1 - x0).abs();
    let sx = if x0 < x1 { 1 } else { -1 };
    let dy = -(y1 - y0).abs();
    let sy = if y0 < y1 { 1 } else { -1 };
    let mut err = dx + dy;

    loop {
        for oy in -thickness..=thickness {
            for ox in -thickness..=thickness {
                set_pixel_blended(surface, x0 + ox, y0 + oy, color);
            }
        }
        if x0 == x1 && y0 == y1 {
            break;
        }
        let e2 = err * 2;
        if e2 >= dy {
            err += dy;
            x0 += sx;
        }
        if e2 <= dx {
            err += dx;
            y0 += sy;
        }
    }
}

fn draw_circle_outline(
    surface: &mut FrameSurfaceMut<'_>,
    cx: i32,
    cy: i32,
    radius: i32,
    color: [u8; 4],
) {
    if radius <= 0 {
        return;
    }
    let mut x = radius;
    let mut y = 0;
    let mut err = 0;

    while x >= y {
        for (dx, dy) in [
            (x, y),
            (y, x),
            (-y, x),
            (-x, y),
            (-x, -y),
            (-y, -x),
            (y, -x),
            (x, -y),
        ] {
            set_pixel_blended(surface, cx + dx, cy + dy, color);
        }

        y += 1;
        if err <= 0 {
            err += 2 * y + 1;
        }
        if err > 0 {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
}

#[derive(Clone, Debug)]
struct AudioTrackPlan {
    asset_offset: u64,
    frame_count: usize,
    volume: f32,
}

#[derive(Clone, Debug)]
struct AudioMixPlan {
    bundle_path: PathBuf,
    sample_rate_hz: u32,
    channels: u16,
    playback_speed: f32,
    target_frames: usize,
    input_frame_count: usize,
    tracks: Vec<AudioTrackPlan>,
}

fn build_mixed_audio(
    bundle_path: &Path,
    bundle_footer: &RecordingBundleFooter,
    manifest: &SessionManifest,
    request: &ExportRequest,
    target_duration_ms: u64,
) -> Result<Option<AudioMixPlan>> {
    let requested_tracks = requested_recorded_audio_tracks(manifest, request);
    let Some((first_track, _)) = requested_tracks.first() else {
        return Ok(None);
    };
    let channels = first_track.channels.max(1);
    let sample_rate_hz = first_track.sample_rate_hz.max(1);
    let playback_speed = request.playback_speed.clamp(0.25, 4.0);
    let target_frames =
        duration_to_frames_round(Duration::from_millis(target_duration_ms), sample_rate_hz)
            as usize;
    let mut tracks = Vec::<AudioTrackPlan>::new();

    for (track_manifest, request_track) in requested_tracks {
        if track_manifest.channels.max(1) != channels
            || track_manifest.sample_rate_hz.max(1) != sample_rate_hz
        {
            return Err(ScreenRecorderError::InvalidConfig(format!(
                "audio track {} format does not match the export mix format",
                track_manifest.track_id
            )));
        }
        if let Some(track) = build_audio_track_plan(
            bundle_path,
            bundle_footer,
            track_manifest,
            request_track.volume,
        )? {
            tracks.push(track);
        }
    }

    if tracks.is_empty() {
        return Ok(None);
    }

    let input_frame_count = tracks
        .iter()
        .map(|track| track.frame_count)
        .max()
        .unwrap_or(0);
    if input_frame_count == 0 || target_frames == 0 {
        return Ok(None);
    }

    Ok(Some(AudioMixPlan {
        bundle_path: bundle_path.to_path_buf(),
        sample_rate_hz,
        channels,
        playback_speed,
        target_frames,
        input_frame_count,
        tracks,
    }))
}

fn build_audio_track_plan(
    bundle_path: &Path,
    bundle_footer: &RecordingBundleFooter,
    track_manifest: &snow_recording_model::AudioTrackManifest,
    volume: f32,
) -> Result<Option<AudioTrackPlan>> {
    let kind = BundleAssetKind::AudioTrack;
    let asset = bundle_footer
        .asset(kind, Some(track_manifest.asset_id.as_str()))
        .ok_or_else(|| {
            ScreenRecorderError::Decode(format!(
                "bundle {} is missing required asset {}",
                bundle_path.display(),
                bundle_asset_label(kind)
            ))
        })?;
    if asset.len == 0 {
        return Ok(None);
    }

    // The asset is raw interleaved PCM laid out exactly as the track manifest
    // describes; the manifest is the only source of format metadata.  The
    // renderer decodes i16 samples, so a new sample format must be handled
    // here explicitly rather than misread.
    match track_manifest.sample_format {
        AudioSampleFormat::PcmS16Le => {}
    }
    let frame_bytes = track_manifest.frame_bytes();
    if asset.len % frame_bytes != 0 {
        return Err(ScreenRecorderError::Decode(format!(
            "bundle asset {} in {} is not aligned to {}-byte frames",
            bundle_asset_label(kind),
            bundle_path.display(),
            frame_bytes
        )));
    }
    let frame_count = usize::try_from(asset.len / frame_bytes).map_err(|_| {
        ScreenRecorderError::Decode(format!(
            "bundle asset {} in {} exceeds addressable memory",
            bundle_asset_label(kind),
            bundle_path.display()
        ))
    })?;

    Ok(Some(AudioTrackPlan {
        asset_offset: asset.offset,
        frame_count,
        volume: volume.clamp(0.0, 2.0),
    }))
}

const AUDIO_RENDER_CACHE_FRAMES: usize = 16_384;
const I16_TO_F32_PCM_SCALE: f32 = 1.0 / 32768.0;
const MIX_LIMITER_START: f32 = 0.82;
const MIX_LIMITER_CEILING: f32 = 0.891_250_9;

fn mixed_sample_normalized(accumulated_i16: f32, track_count: usize) -> f32 {
    let bus_gain = 1.0 / (track_count.max(1) as f32).sqrt();
    soft_limit_normalized(accumulated_i16 * I16_TO_F32_PCM_SCALE * bus_gain)
}

fn mixed_sample_i16(accumulated_i16: f32, track_count: usize) -> i16 {
    (mixed_sample_normalized(accumulated_i16, track_count) * 32768.0)
        .round()
        .clamp(i16::MIN as f32, i16::MAX as f32) as i16
}

fn soft_limit_normalized(sample: f32) -> f32 {
    let magnitude = sample.abs();
    if magnitude <= MIX_LIMITER_START {
        return sample;
    }

    let knee = MIX_LIMITER_CEILING - MIX_LIMITER_START;
    let limited =
        MIX_LIMITER_START + knee * (1.0 - (-(magnitude - MIX_LIMITER_START) / knee).exp());
    limited.copysign(sample)
}

#[derive(Debug)]
struct AudioTrackReader {
    plan: AudioTrackPlan,
    file: fs::File,
    channels_usize: usize,
    cache_start_frame: usize,
    cache_frame_count: usize,
    cache_samples_i16: Vec<i16>,
}

impl AudioTrackReader {
    fn open(bundle_path: &Path, plan: AudioTrackPlan, channels: u16) -> Result<Self> {
        Ok(Self {
            plan,
            file: fs::File::open(bundle_path)?,
            channels_usize: usize::from(channels.max(1)),
            cache_start_frame: 0,
            cache_frame_count: 0,
            cache_samples_i16: Vec::new(),
        })
    }

    fn ensure_cached_range(
        &mut self,
        start_frame: usize,
        end_frame_exclusive: usize,
    ) -> Result<()> {
        let start_frame = start_frame.min(self.plan.frame_count);
        let end_frame_exclusive = end_frame_exclusive.min(self.plan.frame_count);
        if start_frame >= end_frame_exclusive {
            self.cache_start_frame = start_frame;
            self.cache_frame_count = 0;
            self.cache_samples_i16.clear();
            return Ok(());
        }

        let cache_end_frame = self
            .cache_start_frame
            .saturating_add(self.cache_frame_count);
        if start_frame >= self.cache_start_frame && end_frame_exclusive <= cache_end_frame {
            return Ok(());
        }

        let required_frames = end_frame_exclusive - start_frame;
        let cache_frame_count = required_frames
            .max(AUDIO_RENDER_CACHE_FRAMES)
            .min(self.plan.frame_count - start_frame);
        read_pcm_i16_frame_range(
            &mut self.file,
            self.plan.asset_offset,
            start_frame,
            cache_frame_count,
            self.channels_usize,
            &mut self.cache_samples_i16,
        )?;
        self.cache_start_frame = start_frame;
        self.cache_frame_count = cache_frame_count;
        Ok(())
    }

    fn sample_at(&self, frame_index: usize, channel_index: usize) -> i16 {
        if channel_index >= self.channels_usize || frame_index < self.cache_start_frame {
            return 0;
        }
        let local_frame_index = frame_index - self.cache_start_frame;
        if local_frame_index >= self.cache_frame_count {
            return 0;
        }

        self.cache_samples_i16[local_frame_index * self.channels_usize + channel_index]
    }
}

#[derive(Debug)]
struct AudioMixRenderer {
    plan: AudioMixPlan,
    channels_usize: usize,
    natural_output_frames: usize,
    readers: Vec<AudioTrackReader>,
}

impl AudioMixRenderer {
    fn new(plan: &AudioMixPlan) -> Result<Self> {
        let mut readers = Vec::with_capacity(plan.tracks.len());
        for track in &plan.tracks {
            readers.push(AudioTrackReader::open(
                &plan.bundle_path,
                track.clone(),
                plan.channels,
            )?);
        }

        Ok(Self {
            plan: plan.clone(),
            channels_usize: usize::from(plan.channels.max(1)),
            natural_output_frames: ((plan.input_frame_count as f64)
                / f64::from(plan.playback_speed.max(0.25)))
            .ceil()
            .max(1.0) as usize,
            readers,
        })
    }

    #[cfg_attr(not(test), allow(dead_code))]
    fn render(
        &mut self,
        start_output_frame: usize,
        frame_count: usize,
        out: &mut Vec<i16>,
    ) -> Result<()> {
        out.clear();
        out.resize(frame_count.saturating_mul(self.channels_usize), 0);
        self.render_into(start_output_frame, frame_count, out)
    }

    fn render_into(
        &mut self,
        start_output_frame: usize,
        frame_count: usize,
        out: &mut [i16],
    ) -> Result<()> {
        let expected_samples = frame_count.saturating_mul(self.channels_usize);
        if out.len() < expected_samples {
            return Err(ScreenRecorderError::Export(
                "audio render target buffer is too small".to_string(),
            ));
        }
        out[..expected_samples].fill(0);
        if frame_count == 0 || self.readers.is_empty() {
            return Ok(());
        }

        let active_end_frame = start_output_frame
            .saturating_add(frame_count)
            .min(self.plan.target_frames)
            .min(self.natural_output_frames);
        if active_end_frame <= start_output_frame {
            return Ok(());
        }

        let active_frame_count = active_end_frame - start_output_frame;

        if (self.plan.playback_speed - 1.0).abs() < f32::EPSILON {
            if self.readers.len() == 1 {
                self.render_direct_single_track(
                    start_output_frame,
                    active_frame_count,
                    &mut out[..expected_samples],
                )?;
            } else {
                self.render_direct_mixed_tracks(
                    start_output_frame,
                    active_frame_count,
                    &mut out[..expected_samples],
                )?;
            }
            return Ok(());
        }

        if self.readers.len() == 1 {
            self.render_retimed_single_track(
                start_output_frame,
                active_frame_count,
                &mut out[..expected_samples],
            )
        } else {
            self.render_retimed_tracks(
                start_output_frame,
                active_frame_count,
                &mut out[..expected_samples],
            )
        }
    }

    fn render_direct_single_track(
        &mut self,
        start_output_frame: usize,
        active_frame_count: usize,
        out: &mut [i16],
    ) -> Result<()> {
        let Some(reader) = self.readers.first_mut() else {
            return Ok(());
        };
        reader.ensure_cached_range(
            start_output_frame,
            start_output_frame.saturating_add(active_frame_count),
        )?;

        let sample_count = active_frame_count * self.channels_usize;
        let track_start = (start_output_frame - reader.cache_start_frame) * self.channels_usize;
        out[..sample_count]
            .copy_from_slice(&reader.cache_samples_i16[track_start..track_start + sample_count]);
        if (reader.plan.volume - 1.0).abs() >= f32::EPSILON {
            scale_samples_i16_in_place(&mut out[..sample_count], reader.plan.volume);
        }
        Ok(())
    }

    fn render_direct_mixed_tracks(
        &mut self,
        start_output_frame: usize,
        active_frame_count: usize,
        out: &mut [i16],
    ) -> Result<()> {
        let end_frame = start_output_frame.saturating_add(active_frame_count);
        for reader in &mut self.readers {
            reader.ensure_cached_range(start_output_frame, end_frame)?;
        }

        for frame_offset in 0..active_frame_count {
            let out_base = frame_offset * self.channels_usize;
            let source_frame = start_output_frame + frame_offset;
            for channel_index in 0..self.channels_usize {
                let mut acc = 0.0f32;
                for reader in &self.readers {
                    acc +=
                        reader.sample_at(source_frame, channel_index) as f32 * reader.plan.volume;
                }
                out[out_base + channel_index] = mixed_sample_i16(acc, self.readers.len());
            }
        }
        Ok(())
    }

    fn render_retimed_single_track(
        &mut self,
        start_output_frame: usize,
        active_frame_count: usize,
        out: &mut [i16],
    ) -> Result<()> {
        let Some(reader) = self.readers.first_mut() else {
            return Ok(());
        };

        let speed = f64::from(self.plan.playback_speed);
        let max_source_frame = self.plan.input_frame_count.saturating_sub(1);
        let first_source_frame = ((start_output_frame as f64) * speed).floor() as usize;
        let last_source_frame =
            ((((start_output_frame + active_frame_count - 1) as f64) * speed).floor() as usize + 1)
                .min(max_source_frame);
        let end_source_frame_exclusive = last_source_frame.saturating_add(1);
        reader.ensure_cached_range(first_source_frame, end_source_frame_exclusive)?;

        let cache = &reader.cache_samples_i16;
        let cache_start_frame = reader.cache_start_frame;
        let volume = reader.plan.volume;
        let apply_volume = (volume - 1.0).abs() >= f32::EPSILON;

        let mut src_pos = start_output_frame as f64 * speed;
        for frame_offset in 0..active_frame_count {
            let src_index = src_pos.floor() as usize;
            let next_index = (src_index + 1).min(max_source_frame);
            let frac = (src_pos - src_index as f64) as f32;
            let out_base = frame_offset * self.channels_usize;
            let src_base = (src_index - cache_start_frame) * self.channels_usize;

            if src_index == next_index || frac <= f32::EPSILON {
                if !apply_volume {
                    out[out_base..out_base + self.channels_usize]
                        .copy_from_slice(&cache[src_base..src_base + self.channels_usize]);
                } else {
                    for channel_index in 0..self.channels_usize {
                        let sample = cache[src_base + channel_index] as f32 * volume;
                        out[out_base + channel_index] =
                            sample.round().clamp(i16::MIN as f32, i16::MAX as f32) as i16;
                    }
                }
                src_pos += speed;
                continue;
            }

            let next_base = (next_index - cache_start_frame) * self.channels_usize;
            for channel_index in 0..self.channels_usize {
                let sample_a = cache[src_base + channel_index] as f32;
                let sample_b = cache[next_base + channel_index] as f32;
                let mut interpolated = sample_a + (sample_b - sample_a) * frac;
                if apply_volume {
                    interpolated *= volume;
                }
                out[out_base + channel_index] =
                    interpolated.round().clamp(i16::MIN as f32, i16::MAX as f32) as i16;
            }
            src_pos += speed;
        }

        Ok(())
    }

    fn render_retimed_tracks(
        &mut self,
        start_output_frame: usize,
        active_frame_count: usize,
        out: &mut [i16],
    ) -> Result<()> {
        let speed = f64::from(self.plan.playback_speed);
        let first_source_frame = ((start_output_frame as f64) * speed).floor() as usize;
        let last_source_frame =
            ((((start_output_frame + active_frame_count - 1) as f64) * speed).floor() as usize + 1)
                .min(self.plan.input_frame_count.saturating_sub(1));
        let end_source_frame_exclusive = last_source_frame.saturating_add(1);

        for reader in &mut self.readers {
            reader.ensure_cached_range(first_source_frame, end_source_frame_exclusive)?;
        }

        let mut src_pos = start_output_frame as f64 * speed;
        for frame_offset in 0..active_frame_count {
            let src_index = src_pos.floor() as usize;
            let next_index = (src_index + 1).min(self.plan.input_frame_count.saturating_sub(1));
            let frac = (src_pos - src_index as f64) as f32;
            let out_base = frame_offset * self.channels_usize;

            for channel_index in 0..self.channels_usize {
                let mut acc = 0.0f32;
                for reader in &self.readers {
                    let sample_a = reader.sample_at(src_index, channel_index) as f32;
                    let sample_b = reader.sample_at(next_index, channel_index) as f32;
                    let interpolated = sample_a + (sample_b - sample_a) * frac;
                    acc += interpolated * reader.plan.volume;
                }
                out[out_base + channel_index] = mixed_sample_i16(acc, self.readers.len());
            }
            src_pos += speed;
        }

        Ok(())
    }
}

fn read_pcm_i16_frame_range(
    file: &mut fs::File,
    asset_offset: u64,
    start_frame: usize,
    frame_count: usize,
    channels_usize: usize,
    out: &mut Vec<i16>,
) -> Result<()> {
    let sample_count = frame_count.saturating_mul(channels_usize);
    if sample_count == 0 {
        out.clear();
        return Ok(());
    }

    let byte_offset = u64::try_from(start_frame)
        .ok()
        .and_then(|frame| frame.checked_mul(channels_usize as u64))
        .and_then(|samples| samples.checked_mul(2))
        .and_then(|offset| asset_offset.checked_add(offset))
        .ok_or_else(|| {
            ScreenRecorderError::Decode("audio frame range exceeds addressable memory".to_string())
        })?;

    file.seek(SeekFrom::Start(byte_offset))?;
    out.resize(sample_count, 0);
    #[cfg(target_endian = "little")]
    {
        let sample_bytes = unsafe {
            std::slice::from_raw_parts_mut(out.as_mut_ptr() as *mut u8, sample_count * 2)
        };
        file.read_exact(sample_bytes)?;
    }
    #[cfg(target_endian = "big")]
    {
        let byte_len = sample_count.checked_mul(2).ok_or_else(|| {
            ScreenRecorderError::Decode("audio frame range exceeds addressable memory".to_string())
        })?;
        let mut bytes = vec![0u8; byte_len];
        file.read_exact(&mut bytes)?;
        unsafe {
            ptr::copy_nonoverlapping(bytes.as_ptr(), out.as_mut_ptr() as *mut u8, byte_len);
        }
        for sample in out.iter_mut() {
            *sample = i16::from_le(*sample);
        }
    }

    Ok(())
}

#[cfg(test)]
fn read_pcm_i16(
    bundle_path: &Path,
    bundle_footer: &RecordingBundleFooter,
    asset_id: &str,
    channels: u16,
) -> Result<Vec<i16>> {
    let kind = BundleAssetKind::AudioTrack;
    let asset = bundle_footer.asset(kind, Some(asset_id)).ok_or_else(|| {
        ScreenRecorderError::Decode(format!(
            "bundle {} is missing required asset {}",
            bundle_path.display(),
            bundle_asset_label(kind)
        ))
    })?;
    if asset.len == 0 {
        return Ok(Vec::new());
    }
    if asset.len % 2 != 0 {
        return Err(ScreenRecorderError::Decode(format!(
            "bundle asset {} in {} has odd byte length",
            bundle_asset_label(kind),
            bundle_path.display()
        )));
    }

    let sample_count = usize::try_from(asset.len / 2).map_err(|_| {
        ScreenRecorderError::Decode(format!(
            "bundle asset {} in {} exceeds addressable memory",
            bundle_asset_label(kind),
            bundle_path.display()
        ))
    })?;
    let mut samples = vec![0i16; sample_count];
    let mut file = fs::File::open(bundle_path)?;
    file.seek(SeekFrom::Start(asset.offset))?;
    #[cfg(target_endian = "little")]
    {
        let sample_bytes = unsafe {
            std::slice::from_raw_parts_mut(samples.as_mut_ptr() as *mut u8, sample_count * 2)
        };
        file.read_exact(sample_bytes)?;
    }
    #[cfg(target_endian = "big")]
    {
        let byte_len = usize::try_from(asset.len).map_err(|_| {
            ScreenRecorderError::Decode(format!(
                "bundle asset {} in {} exceeds addressable memory",
                bundle_asset_label(kind),
                bundle_path.display()
            ))
        })?;
        let mut bytes = vec![0u8; byte_len];
        file.read_exact(&mut bytes)?;
        unsafe {
            ptr::copy_nonoverlapping(bytes.as_ptr(), samples.as_mut_ptr() as *mut u8, byte_len);
        }
    }
    #[cfg(target_endian = "big")]
    for sample in &mut samples {
        *sample = i16::from_le(*sample);
    }

    let channels = usize::from(channels.max(1));
    let aligned = (samples.len() / channels) * channels;
    samples.truncate(aligned);
    Ok(samples)
}

fn read_bundle_asset_bytes(
    bundle_path: &Path,
    bundle_footer: &RecordingBundleFooter,
    kind: BundleAssetKind,
    asset_id: Option<&str>,
) -> Result<Vec<u8>> {
    let asset = bundle_footer.asset(kind, asset_id).ok_or_else(|| {
        ScreenRecorderError::Decode(format!(
            "bundle {} is missing required asset {}",
            bundle_path.display(),
            bundle_asset_label(kind)
        ))
    })?;
    let len = usize::try_from(asset.len).map_err(|_| {
        ScreenRecorderError::Decode(format!(
            "bundle asset {} in {} exceeds addressable memory",
            bundle_asset_label(kind),
            bundle_path.display()
        ))
    })?;
    let mut file = fs::File::open(bundle_path)?;
    file.seek(SeekFrom::Start(asset.offset))?;
    let mut bytes = vec![0u8; len];
    file.read_exact(&mut bytes)?;
    Ok(bytes)
}

const fn bundle_asset_label(kind: BundleAssetKind) -> &'static str {
    match kind {
        BundleAssetKind::VideoIndex => "video-index",
        BundleAssetKind::AudioTrack => "audio-track",
        BundleAssetKind::MouseStore => "mouse-store",
        BundleAssetKind::InputEvents => "input-events",
        BundleAssetKind::RenderMetadata => "render-metadata",
        BundleAssetKind::OutputSettings => "output-settings",
    }
}

#[cfg(test)]
fn mix_audio_tracks_i16_interleaved_owned(
    mut tracks: Vec<(Vec<i16>, f32)>,
    channels: u16,
) -> Vec<i16> {
    let channels_usize = usize::from(channels.max(1));
    if tracks.is_empty() {
        return Vec::new();
    }
    if tracks.len() == 1 {
        let Some((mut samples, volume)) = tracks.pop() else {
            return Vec::new();
        };
        if (volume - 1.0).abs() < f32::EPSILON {
            return samples;
        }
        scale_samples_i16_in_place(&mut samples, volume);
        return samples;
    }

    if tracks.len() == 2 {
        let Some((samples_b, volume_b)) = tracks.pop() else {
            return Vec::new();
        };
        let Some((samples_a, volume_a)) = tracks.pop() else {
            return Vec::new();
        };
        return mix_two_tracks_i16_interleaved(
            &samples_a,
            volume_a,
            &samples_b,
            volume_b,
            channels_usize,
        );
    }

    let track_views: Vec<(&[i16], f32)> = tracks
        .iter()
        .map(|(samples, volume)| (samples.as_slice(), *volume))
        .collect();
    let max_frames = track_views
        .iter()
        .map(|(samples, _)| samples.len() / channels_usize)
        .max()
        .unwrap_or(0);
    if max_frames == 0 {
        return Vec::new();
    }

    let mut mixed = vec![0i16; max_frames * channels_usize];
    let process_chunk = |(frame_idx, frame): (usize, &mut [i16])| {
        for (ch, sample_out) in frame.iter_mut().enumerate() {
            let mut acc = 0.0f32;
            let sample_index = frame_idx * channels_usize + ch;
            for (samples, volume) in &track_views {
                if sample_index < samples.len() {
                    acc += samples[sample_index] as f32 * *volume;
                }
            }
            *sample_out = mixed_sample_i16(acc, track_views.len());
        }
    };
    if max_frames >= 16_384 {
        mixed
            .par_chunks_exact_mut(channels_usize)
            .enumerate()
            .for_each(process_chunk);
    } else {
        mixed
            .chunks_exact_mut(channels_usize)
            .enumerate()
            .for_each(process_chunk);
    }
    mixed
}

fn scale_samples_i16_in_place(samples: &mut [i16], volume: f32) {
    if samples.is_empty() {
        return;
    }
    let volume = volume.clamp(0.0, 2.0);
    if (volume - 1.0).abs() < f32::EPSILON {
        return;
    }

    if samples.len() >= 262_144 {
        samples.par_iter_mut().for_each(|sample| {
            let scaled = *sample as f32 * volume;
            *sample = scaled.round().clamp(i16::MIN as f32, i16::MAX as f32) as i16;
        });
    } else {
        for sample in samples {
            let scaled = *sample as f32 * volume;
            *sample = scaled.round().clamp(i16::MIN as f32, i16::MAX as f32) as i16;
        }
    }
}

#[cfg(test)]
fn mix_two_tracks_i16_interleaved(
    samples_a: &[i16],
    volume_a: f32,
    samples_b: &[i16],
    volume_b: f32,
    channels_usize: usize,
) -> Vec<i16> {
    let out_len = (samples_a.len().max(samples_b.len()) / channels_usize) * channels_usize;
    if out_len == 0 {
        return Vec::new();
    }

    let volume_a = volume_a.clamp(0.0, 2.0);
    let volume_b = volume_b.clamp(0.0, 2.0);
    let mut mixed = vec![0i16; out_len];
    let chunk_samples = 32_768usize.max(channels_usize);
    if out_len >= 262_144 {
        mixed
            .par_chunks_mut(chunk_samples)
            .enumerate()
            .for_each(|(chunk_idx, chunk)| {
                let start = chunk_idx * chunk_samples;
                mix_two_tracks_chunk(chunk, start, samples_a, volume_a, samples_b, volume_b);
            });
    } else {
        mix_two_tracks_chunk(&mut mixed, 0, samples_a, volume_a, samples_b, volume_b);
    }
    mixed
}

#[cfg(test)]
fn mix_two_tracks_chunk(
    out: &mut [i16],
    start: usize,
    samples_a: &[i16],
    volume_a: f32,
    samples_b: &[i16],
    volume_b: f32,
) {
    for (offset, sample_out) in out.iter_mut().enumerate() {
        let idx = start + offset;
        let mut acc = 0.0f32;
        if idx < samples_a.len() {
            acc += samples_a[idx] as f32 * volume_a;
        }
        if idx < samples_b.len() {
            acc += samples_b[idx] as f32 * volume_b;
        }
        *sample_out = mixed_sample_i16(acc, 2);
    }
}

#[cfg(test)]
fn retime_audio_i16_interleaved_owned(
    samples: Vec<i16>,
    channels: u16,
    playback_speed: f32,
) -> Vec<i16> {
    let channels_usize = usize::from(channels.max(1));
    if samples.is_empty() {
        return Vec::new();
    }

    let frame_count = samples.len() / channels_usize;
    if frame_count == 0 {
        return Vec::new();
    }

    let speed = playback_speed.clamp(0.25, 4.0);
    if (speed - 1.0).abs() < f32::EPSILON {
        return samples;
    }

    let output_frames = ((frame_count as f64) / speed as f64).ceil().max(1.0) as usize;
    let mut out = vec![0i16; output_frames * channels_usize];
    let process_chunk = |(out_frame, out_frame_samples): (usize, &mut [i16])| {
        let src_pos = out_frame as f64 * speed as f64;
        let src_index = src_pos.floor() as usize;
        let next_index = (src_index + 1).min(frame_count.saturating_sub(1));
        let frac = (src_pos - src_index as f64) as f32;

        for ch in 0..channels_usize {
            let a = samples[src_index * channels_usize + ch] as f32;
            let b = samples[next_index * channels_usize + ch] as f32;
            let mixed = a + (b - a) * frac;
            out_frame_samples[ch] = mixed.round().clamp(i16::MIN as f32, i16::MAX as f32) as i16;
        }
    };
    if output_frames >= 16_384 {
        out.par_chunks_exact_mut(channels_usize)
            .enumerate()
            .for_each(process_chunk);
    } else {
        out.chunks_exact_mut(channels_usize)
            .enumerate()
            .for_each(process_chunk);
    }
    out
}

/// Legacy editable recordings keep their decode/retime/composition choices while
/// using the same encoder, bounded audio admission and transactional muxing as
/// live capture and finalized-source replay.
struct EditingVideoEncoder<'a> {
    encoder: StreamingEncoder,
    audio: Option<AudioMixRenderer>,
    audio_cursor: usize,
    audio_samples: Vec<i16>,
    pending_frame: Option<ffmpeg::frame::Video>,
    pending_pts: usize,
    total_frames: usize,
    fps: u32,
    started: Instant,
    audio_elapsed: Duration,
    cancel: &'a Arc<AtomicBool>,
    cancellation: Option<&'a CancellationToken>,
    progress_tx: &'a Option<Arc<ProgressReporter>>,
}

impl<'a> EditingVideoEncoder<'a> {
    fn create(
        output_path: &Path,
        width: u32,
        height: u32,
        fps: u32,
        format: ExportFormat,
        codec: VideoCodec,
        prefer_hardware_h264: bool,
        mixed_audio: Option<&AudioMixPlan>,
        audio_bitrate_kbps: u16,
        video: &VideoEncodeConfig,
        performance: &ExportPerformanceConfig,
        output_hdr: bool,
        source_hint: Option<ffmpeg::format::Pixel>,
        total_frames: usize,
        cancel: &'a Arc<AtomicBool>,
        cancellation: Option<&'a CancellationToken>,
        progress_tx: &'a Option<Arc<ProgressReporter>>,
    ) -> Result<Self> {
        check_canceled(cancel)?;
        if total_frames == 0 {
            return Err(ScreenRecorderError::Export(
                "retiming produced no frames".into(),
            ));
        }
        // The outer export transaction reserves an empty temporary destination.
        // It is private to this job, so release it for the shared encoder's
        // create-new publication policy. The public destination stays untouched.
        if output_path.exists() {
            fs::remove_file(output_path)?;
        }
        let mixed_audio = mixed_audio.filter(|_| !format.is_animated_image());
        let audio = mixed_audio.map(AudioMixRenderer::new).transpose()?;
        let audio_config = mixed_audio
            .map(|plan| StreamingAudioConfig {
                track_id: "mixed".into(),
                title: "Audio".into(),
                default: true,
                sample_rate_hz: plan.sample_rate_hz,
                channels: plan.channels,
                bitrate_kbps: audio_bitrate_kbps,
            })
            .into_iter()
            .collect();
        let builder = StreamingEncoder::builder(StreamingEncoderConfig {
            loop_animated_images: true,
            output_path: output_path.to_path_buf(),
            format,
            width,
            height,
            fps: fps.max(1),
            codec,
            prefer_hardware_h264,
            execution_mode: performance.mode,
            software_h264_priority: performance.software_h264_priority,
            video: *video,
            encode_threads: performance.encode_threads,
            audio: audio_config,
        });
        let builder = if let Some(format) = source_hint {
            builder.cpu_input_format_hint(format)
        } else {
            builder
        };
        let encoder = if output_hdr {
            builder.hdr10_cpu_input()
        } else {
            builder
        }
        .create()?;
        Ok(Self {
            encoder,
            audio,
            audio_cursor: 0,
            audio_samples: Vec::new(),
            pending_frame: None,
            pending_pts: 0,
            total_frames,
            fps: fps.max(1),
            started: Instant::now(),
            audio_elapsed: Duration::ZERO,
            cancel,
            cancellation,
            progress_tx,
        })
    }

    fn queue_prepared_frame(
        &mut self,
        frame: &ffmpeg::frame::Video,
        duration_ticks: usize,
        allow_repeat_collapse: bool,
        scheduled: &Cell<usize>,
    ) -> Result<()> {
        check_canceled(self.cancel)?;
        let pts = scheduled.get();
        let next = pts.saturating_add(duration_ticks.max(1));
        let same = allow_repeat_collapse
            && can_collapse_repeated_video_frames(frame)
            && self
                .pending_frame
                .as_ref()
                .is_some_and(|pending| video_frames_match(pending, frame));
        if !same {
            self.flush_prepared_frame()?;
            let mut retained = ffmpeg::frame::Video::empty();
            // Keep a bounded immutable lease; decoder/scaler scratch buffers use
            // make_writable before reuse instead of a full frame copy here.
            let result =
                unsafe { ffmpeg::ffi::av_frame_ref(retained.as_mut_ptr(), frame.as_ptr()) };
            if result < 0 {
                return Err(ScreenRecorderError::Export(format!(
                    "failed to reference prepared output frame: {}",
                    ffmpeg::Error::from(result)
                )));
            }
            self.pending_frame = Some(retained);
            self.pending_pts = pts;
        }
        scheduled.set(next);
        self.pump_audio(next)?;
        self.progress(next);
        Ok(())
    }

    fn flush_prepared_frame(&mut self) -> Result<()> {
        if let Some(frame) = self.pending_frame.take() {
            self.encoder
                .push_prepared_video_frame_at_pts(self.pending_pts as u64, frame)?;
        }
        Ok(())
    }

    fn pump_audio(&mut self, completed_pts: usize) -> Result<()> {
        let Some(renderer) = self.audio.as_mut() else {
            return Ok(());
        };
        let end = (completed_pts as u128 * u128::from(renderer.plan.sample_rate_hz)
            / u128::from(self.fps))
        .min(usize::MAX as u128) as usize;
        let end = end.min(renderer.plan.target_frames);
        let started = Instant::now();
        while self.audio_cursor < end {
            check_canceled(self.cancel)?;
            let take = (end - self.audio_cursor).min(4096);
            renderer.render(self.audio_cursor, take, &mut self.audio_samples)?;
            self.encoder.push_audio_track_pcm_i16_at_frame(
                "mixed",
                self.audio_cursor as u64,
                &self.audio_samples,
            )?;
            self.audio_cursor += take;
        }
        self.audio_elapsed += started.elapsed();
        Ok(())
    }

    fn progress(&self, completed: usize) {
        if !completed.is_multiple_of(10) && completed != self.total_frames {
            return;
        }
        let fps = completed as f32 / self.started.elapsed().as_secs_f32().max(0.001);
        let eta = (fps > 0.0)
            .then(|| (self.total_frames.saturating_sub(completed) as f32 / fps * 1000.0) as u64);
        emit_progress(
            self.progress_tx,
            ExportStage::VideoEncode,
            35.0 + completed as f32 / self.total_frames.max(1) as f32 * 55.0,
            fps,
            eta,
        );
    }

    fn finish(mut self, duration_ms: u64) -> Result<ExportCodecTelemetry> {
        check_canceled(self.cancel)?;
        self.flush_prepared_frame()?;
        // Audio is rendered only on demand up to the exact retimed endpoint;
        // no complete recording or encoded packet collection lives in memory.
        if self.audio.is_some() {
            emit_progress(self.progress_tx, ExportStage::AudioEncode, 90.0, 0.0, None);
            self.pump_audio(self.total_frames)?;
        }
        let video_elapsed = self.started.elapsed().saturating_sub(self.audio_elapsed);
        check_canceled(self.cancel)?;
        emit_progress(self.progress_tx, ExportStage::Mux, 95.0, 0.0, None);
        let mux_started = Instant::now();
        let report = if let Some(cancellation) = self.cancellation {
            self.encoder
                .finish_at_duration_ms_cancelable(duration_ms.max(1), cancellation)?
        } else {
            self.encoder.finish_at_duration_ms(duration_ms.max(1))?
        };
        check_canceled(self.cancel)?;
        Ok(ExportCodecTelemetry {
            video_encoder: Some(report.video_encoder),
            audio_encoder: report.audio_encoder,
            used_hardware_encode: report.used_hardware_video_encoder,
            stage_durations_ms: ExportStageDurationsMs {
                video_encode: video_elapsed.as_millis().min(u128::from(u64::MAX)) as u64,
                audio_encode: self.audio_elapsed.as_millis().min(u128::from(u64::MAX)) as u64,
                mux: mux_started.elapsed().as_millis().min(u128::from(u64::MAX)) as u64,
                ..ExportStageDurationsMs::default()
            },
            ..ExportCodecTelemetry::default()
        })
    }
}

fn export_video_generated<F>(
    output_path: &Path,
    width: u32,
    height: u32,
    timeline: FinalizedTimeline,
    format: ExportFormat,
    requested_codec: VideoCodec,
    prefer_hardware_h264: bool,
    mixed_audio: Option<&AudioMixPlan>,
    audio_bitrate_kbps: u16,
    video_config: &VideoEncodeConfig,
    perf_config: &ExportPerformanceConfig,
    cancel_flag: &Arc<AtomicBool>,
    cancellation: Option<&CancellationToken>,
    progress_tx: &Option<Arc<ProgressReporter>>,
    mut rgba_provider: F,
) -> Result<ExportCodecTelemetry>
where
    F: FnMut(usize, &mut [u8]) -> Result<()>,
{
    let frame_count = usize::try_from(timeline.frame_count()).map_err(|_| {
        ScreenRecorderError::Export("output timeline contains too many frames".into())
    })?;
    let export_fps = timeline.fps();
    check_canceled(cancel_flag)?;
    let mut encoder = EditingVideoEncoder::create(
        output_path,
        width,
        height,
        export_fps,
        format,
        requested_codec,
        prefer_hardware_h264,
        mixed_audio,
        audio_bitrate_kbps,
        video_config,
        perf_config,
        false,
        None,
        frame_count,
        cancel_flag,
        cancellation,
        progress_tx,
    )?;
    let rgba_len = width as usize * height as usize * 4;
    let mut rgba = vec![0u8; rgba_len];
    for index in 0..frame_count {
        check_canceled(cancel_flag)?;
        rgba.resize(rgba_len, 0);
        rgba_provider(index, &mut rgba)?;
        rgba = encoder
            .encoder
            .push_owned_rgba_frame_at_pts(index as u64, rgba)?;
        encoder.pump_audio(index.saturating_add(1))?;
        encoder.progress(index.saturating_add(1));
    }
    encoder.finish(timeline.duration_ms())
}

fn build_source_frame_repeat_counts(retime: &RetimePlan) -> Vec<usize> {
    let mut repeat_counts = vec![0usize; retime.source_starts_ms.len()];
    for source_index in retime.source_indices() {
        repeat_counts[source_index] = repeat_counts[source_index].saturating_add(1);
    }
    repeat_counts
}

fn export_video_generated_from_source(
    input_video_path: &Path,
    output_path: &Path,
    retime: &RetimePlan,
    source_hdr: bool,
    overlays: Option<(&MouseTracks, &MouseEditConfig)>,
    width: u32,
    height: u32,
    export_fps: u32,
    format: ExportFormat,
    requested_codec: VideoCodec,
    prefer_hardware_h264: bool,
    mixed_audio: Option<&AudioMixPlan>,
    audio_bitrate_kbps: u16,
    video_config: &VideoEncodeConfig,
    perf_config: &ExportPerformanceConfig,
    cancel_flag: &Arc<AtomicBool>,
    cancellation: Option<&CancellationToken>,
    progress_tx: &Option<Arc<ProgressReporter>>,
) -> Result<ExportCodecTelemetry> {
    ensure_ffmpeg_initialized()?;
    let output_hdr = crate::preserves_hdr_output(source_hdr, format, requested_codec);
    validate_export_dimensions(width, height, format.requires_even_dimensions())?;

    if retime.frame_count() == 0 {
        return Err(ScreenRecorderError::Export(
            "retiming produced no frames".to_string(),
        ));
    }
    let repeat_counts = build_source_frame_repeat_counts(retime);
    if repeat_counts.is_empty() {
        return Err(ScreenRecorderError::Export(
            "retiming produced no source mapping".to_string(),
        ));
    }

    let mut input = ffmpeg::format::input(input_video_path).map_err(|err| {
        ScreenRecorderError::Export(format!(
            "failed to open source video {} for transcode export: {err}",
            input_video_path.display()
        ))
    })?;
    let input_video_stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .ok_or_else(|| {
            ScreenRecorderError::Export(format!(
                "source video {} has no video stream",
                input_video_path.display()
            ))
        })?;
    let input_video_stream_index = input_video_stream.index();
    let input_video_parameters = input_video_stream.parameters();
    let mut source_decoder = open_source_video_decoder(&input_video_parameters, perf_config, true)?;
    let SourceVideoDecoder {
        decoder,
        hardware: hw_decode_state,
    } = &mut source_decoder;

    let mut encoder = EditingVideoEncoder::create(
        output_path,
        width,
        height,
        export_fps,
        format,
        requested_codec,
        prefer_hardware_h264,
        mixed_audio,
        audio_bitrate_kbps,
        video_config,
        perf_config,
        output_hdr,
        Some(if source_hdr && !output_hdr {
            ffmpeg::format::Pixel::RGBA
        } else {
            decoder_software_output_format(decoder, hw_decode_state.as_ref())
        }),
        retime.frame_count(),
        cancel_flag,
        cancellation,
        progress_tx,
    )?;
    let pixel_format = encoder.encoder.input_pixel_format();
    let video_decoder = hw_decode_state
        .as_ref()
        .map(|state| state.device_name)
        .unwrap_or("software_decode")
        .to_string();
    let used_hardware_decode = hw_decode_state.is_some();

    let mut hdr_overlay_scaler = None::<ffmpeg::software::scaling::Context>;
    let mut hdr_overlay_base = ffmpeg::frame::Video::empty();
    let mut hdr_overlay_frame = ffmpeg::frame::Video::empty();
    let mut overlay_state = OverlaySearchState::default();
    let mut tone_mapper = crate::hdr::ToneMapper::default();
    let mut scaler = None::<crate::output_scaler::OutputScaler>;
    let mut encode_frame = None::<ffmpeg::frame::Video>;
    let mut decoded = ffmpeg::frame::Video::empty();
    let mut transferred_decoded = ffmpeg::frame::Video::empty();
    let mut source_frame_index = 0usize;
    let scheduled_output_count = Cell::new(0usize);
    let mut decode_complete = false;

    let mut process_decoded_frame = |decoded: &mut ffmpeg::frame::Video| -> Result<()> {
        check_canceled(cancel_flag)?;
        let repeats = repeat_counts.get(source_frame_index).copied().unwrap_or(0);
        source_frame_index = source_frame_index.saturating_add(1);
        if repeats == 0 {
            return Ok(());
        }

        // Decode/resize once for this source. Each output overlay starts from
        // an immutable base, so repeated frames cannot accumulate old effects.
        if overlays.is_some() {
            let key = (decoded.format(), decoded.width(), decoded.height());
            if hdr_overlay_scaler
                .as_ref()
                .is_none_or(|s| (s.input().format, s.input().width, s.input().height) != key)
            {
                let mut conversion = ffmpeg::software::scaling::Context::get(
                    key.0,
                    key.1,
                    key.2,
                    ffmpeg::format::Pixel::RGB48LE,
                    width,
                    height,
                    ffmpeg::software::scaling::Flags::BICUBIC,
                )
                .map_err(|e| ScreenRecorderError::Export(format!("HDR overlay conversion: {e}")))?;
                crate::hdr::scaler_colors(&mut conversion, true, true)?;
                hdr_overlay_scaler = Some(conversion);
                hdr_overlay_base =
                    ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, width, height);
            }
            ensure_video_frame_writable(&mut hdr_overlay_base)?;
            hdr_overlay_scaler
                .as_mut()
                .unwrap()
                .run(decoded, &mut hdr_overlay_base)
                .map_err(|e| ScreenRecorderError::Export(format!("HDR overlay conversion: {e}")))?;
            crate::hdr::frame(&mut hdr_overlay_base, true);
        }
        // Re-evaluate the overlay on every output observation: retiming may
        // repeat a static source while the cursor or ripple continues moving.
        for _ in 0..if overlays.is_some() { repeats } else { 1 } {
            check_canceled(cancel_flag)?;
            let decoded = if let Some((tracks, config)) = overlays {
                unsafe {
                    ffmpeg::ffi::av_frame_unref(hdr_overlay_frame.as_mut_ptr());
                }
                if unsafe {
                    ffmpeg::ffi::av_frame_ref(
                        hdr_overlay_frame.as_mut_ptr(),
                        hdr_overlay_base.as_ptr(),
                    )
                } < 0
                {
                    return Err(ScreenRecorderError::Export(
                        "HDR overlay frame reference failed".into(),
                    ));
                }
                ensure_video_frame_writable(&mut hdr_overlay_frame)?;
                let stride = hdr_overlay_frame.stride(0);
                let mut surface = FrameSurfaceMut {
                    timestamp_ms: retime.timestamp_ms(scheduled_output_count.get()),
                    width,
                    height,
                    rgba: &mut [],
                    hdr: Some((hdr_overlay_frame.data_mut(0), stride)),
                };
                apply_mouse_overlays_surface_incremental(
                    &mut surface,
                    tracks,
                    config,
                    &mut overlay_state,
                );
                &mut hdr_overlay_frame
            } else {
                &mut *decoded
            };
            let repeats = if overlays.is_some() { 1 } else { repeats };
            let decoded = if source_hdr && !output_hdr {
                tone_mapper.convert(decoded)?
            } else {
                decoded
            };
            let source_width = decoded.width();
            let source_height = decoded.height();
            if source_width == 0 || source_height == 0 {
                return Err(ScreenRecorderError::Export(
                    "decoded frame has zero dimensions".to_string(),
                ));
            }

            let direct_frame_passthrough = source_width == width
                && source_height == height
                && decoded.format() == pixel_format;
            if direct_frame_passthrough {
                encoder.queue_prepared_frame(
                    decoded,
                    repeats,
                    overlays.is_none(),
                    &scheduled_output_count,
                )?;
                continue;
            }

            let needs_reset = scaler.as_ref().is_none_or(|s| {
                (s.input().format, s.input().width, s.input().height)
                    != (decoded.format(), source_width, source_height)
            }) || encode_frame
                .as_ref()
                .map(|frame| frame.width() != width || frame.height() != height)
                .unwrap_or(false);
            if needs_reset {
                scaler = Some(
                    crate::output_scaler::OutputScaler::get(
                        decoded.format(),
                        source_width,
                        source_height,
                        pixel_format,
                        width,
                        height,
                        ffmpeg::software::scaling::flag::Flags::BICUBIC,
                    )
                    .map_err(|err| {
                        ScreenRecorderError::Export(format!(
                            "failed to create source-to-output video scaler: {err}"
                        ))
                    })?,
                );
                if source_hdr {
                    crate::hdr::scaler_colors(
                        scaler.as_mut().unwrap(),
                        output_hdr,
                        crate::hdr::is_rgb(pixel_format),
                    )?;
                }
                encode_frame = Some(ffmpeg::frame::Video::new(pixel_format, width, height));
            }

            let scaler_ref = scaler.as_mut().ok_or_else(|| {
                ScreenRecorderError::Export("video scaler is uninitialized".to_string())
            })?;
            let encode_frame_ref = encode_frame.as_mut().ok_or_else(|| {
                ScreenRecorderError::Export("video frame buffer is uninitialized".to_string())
            })?;
            ensure_video_frame_writable(encode_frame_ref)?;
            scaler_ref.run(decoded, encode_frame_ref).map_err(|err| {
                ScreenRecorderError::Export(format!(
                    "failed to convert decoded frame for video export: {err}"
                ))
            })?;

            if source_hdr {
                crate::hdr::frame(encode_frame_ref, output_hdr);
            }
            encoder.queue_prepared_frame(
                encode_frame_ref,
                repeats,
                overlays.is_none(),
                &scheduled_output_count,
            )?;
        }
        Ok(())
    };

    for (stream, packet) in input.packets() {
        if decode_complete || scheduled_output_count.get() >= retime.frame_count() {
            break;
        }
        if stream.index() != input_video_stream_index {
            continue;
        }
        check_canceled(cancel_flag)?;
        decoder.send_packet(&packet).map_err(|err| {
            ScreenRecorderError::Export(format!("failed to feed packet into source decoder: {err}"))
        })?;
        loop {
            match decoder.receive_frame(&mut decoded) {
                Ok(()) => {
                    let decoded_frame = normalize_decoded_video_frame(
                        &mut decoded,
                        &mut transferred_decoded,
                        hw_decode_state.as_ref(),
                    )?;
                    process_decoded_frame(decoded_frame)?;
                    if scheduled_output_count.get() >= retime.frame_count() {
                        decode_complete = true;
                        break;
                    }
                }
                Err(err) if is_eagain(&err) => break,
                Err(ffmpeg::Error::Eof) => break,
                Err(err) => {
                    return Err(ScreenRecorderError::Export(format!(
                        "failed to decode source video frame: {err}"
                    )));
                }
            }
        }
    }

    if !decode_complete {
        decoder.send_eof().map_err(|err| {
            ScreenRecorderError::Export(format!("failed to flush source video decoder: {err}"))
        })?;
        loop {
            check_canceled(cancel_flag)?;
            match decoder.receive_frame(&mut decoded) {
                Ok(()) => {
                    let decoded_frame = normalize_decoded_video_frame(
                        &mut decoded,
                        &mut transferred_decoded,
                        hw_decode_state.as_ref(),
                    )?;
                    process_decoded_frame(decoded_frame)?;
                    if scheduled_output_count.get() >= retime.frame_count() {
                        break;
                    }
                }
                Err(err) if is_eagain(&err) => continue,
                Err(ffmpeg::Error::Eof) => break,
                Err(err) => {
                    return Err(ScreenRecorderError::Export(format!(
                        "failed to drain source video decoder: {err}"
                    )));
                }
            }
        }
    }

    #[allow(clippy::drop_non_drop)]
    drop(process_decoded_frame);

    if scheduled_output_count.get() != retime.frame_count() {
        return Err(ScreenRecorderError::Export(format!(
            "source decode ended before all retimed frames were produced (scheduled {}, expected {})",
            scheduled_output_count.get(),
            retime.frame_count()
        )));
    }

    let mut telemetry = encoder.finish(retime.output_duration_ms())?;
    telemetry.video_decoder = Some(video_decoder);
    telemetry.used_hardware_decode = used_hardware_decode;
    Ok(telemetry)
}

fn export_video_generated_from_source_with_overlay(
    input_video_path: &Path,
    output_path: &Path,
    retime: &RetimePlan,
    width: u32,
    height: u32,
    export_fps: u32,
    format: ExportFormat,
    requested_codec: VideoCodec,
    prefer_hardware_h264: bool,
    mixed_audio: Option<&AudioMixPlan>,
    audio_bitrate_kbps: u16,
    video_config: &VideoEncodeConfig,
    perf_config: &ExportPerformanceConfig,
    overlay_tracks: &MouseTracks,
    mouse_config: &MouseEditConfig,
    cancel_flag: &Arc<AtomicBool>,
    cancellation: Option<&CancellationToken>,
    progress_tx: &Option<Arc<ProgressReporter>>,
) -> Result<ExportCodecTelemetry> {
    ensure_ffmpeg_initialized()?;
    validate_export_dimensions(width, height, format.requires_even_dimensions())?;

    if retime.frame_count() == 0 {
        return Err(ScreenRecorderError::Export(
            "retiming produced no frames".to_string(),
        ));
    }
    let repeat_counts = build_source_frame_repeat_counts(retime);
    if repeat_counts.is_empty() {
        return Err(ScreenRecorderError::Export(
            "retiming produced no source mapping".to_string(),
        ));
    }

    let mut input = ffmpeg::format::input(input_video_path).map_err(|err| {
        ScreenRecorderError::Export(format!(
            "failed to open source video {} for overlay transcode export: {err}",
            input_video_path.display()
        ))
    })?;
    let input_video_stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .ok_or_else(|| {
            ScreenRecorderError::Export(format!(
                "source video {} has no video stream",
                input_video_path.display()
            ))
        })?;
    let input_video_stream_index = input_video_stream.index();
    let input_video_parameters = input_video_stream.parameters();
    let mut source_decoder =
        open_source_video_decoder(&input_video_parameters, perf_config, false)?;
    let SourceVideoDecoder {
        decoder,
        hardware: hw_decode_state,
    } = &mut source_decoder;

    let mut encoder = EditingVideoEncoder::create(
        output_path,
        width,
        height,
        export_fps,
        format,
        requested_codec,
        prefer_hardware_h264,
        mixed_audio,
        audio_bitrate_kbps,
        video_config,
        perf_config,
        false,
        None,
        retime.frame_count(),
        cancel_flag,
        cancellation,
        progress_tx,
    )?;
    let pixel_format = encoder.encoder.input_pixel_format();
    let video_decoder = hw_decode_state
        .as_ref()
        .map(|state| state.device_name)
        .unwrap_or("software_decode")
        .to_string();
    let used_hardware_decode = hw_decode_state.is_some();

    let mut encode_scaler = crate::output_scaler::OutputScaler::get(
        ffmpeg::format::Pixel::RGBA,
        width,
        height,
        pixel_format,
        width,
        height,
        ffmpeg::software::scaling::flag::Flags::BICUBIC,
    )
    .map_err(|err| {
        ScreenRecorderError::Export(format!("failed to create RGBA video scaler: {err}"))
    })?;
    let mut rgba_frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height);
    let mut encode_frame = ffmpeg::frame::Video::new(pixel_format, width, height);
    let rgba_row_bytes = width as usize * 4;
    let rgba_len = rgba_row_bytes * height as usize;
    let rgba_stride = rgba_frame.stride(0);
    let can_write_direct_rgba = rgba_stride == rgba_row_bytes;
    let mut base_rgba = vec![0u8; rgba_len];
    let mut generated_rgba = if can_write_direct_rgba {
        Vec::new()
    } else {
        vec![0u8; rgba_len]
    };
    let mut overlay_state = OverlaySearchState::default();
    let mut decode_rgba_scaler = None::<ffmpeg::software::scaling::Context>;
    let mut decode_rgba_frame = None::<ffmpeg::frame::Video>;
    let mut decode_rgba_key = None::<(u32, u32, ffmpeg::format::Pixel)>;
    let mut source_encode_scaler = None::<crate::output_scaler::OutputScaler>;
    let mut source_encode_frame = None::<ffmpeg::frame::Video>;
    let mut source_encode_key = None::<(u32, u32, ffmpeg::format::Pixel)>;
    let mut native_overlay_frame = None::<ffmpeg::frame::Video>;
    let mut native_overlay_key = None::<(u32, u32, ffmpeg::format::Pixel)>;
    let mut decoded = ffmpeg::frame::Video::empty();
    let mut transferred_decoded = ffmpeg::frame::Video::empty();
    let mut source_frame_index = 0usize;
    let scheduled_output_count = Cell::new(0usize);
    let mut decode_complete = false;

    let mut process_decoded_frame = |decoded: &mut ffmpeg::frame::Video| -> Result<()> {
        check_canceled(cancel_flag)?;
        let repeats = repeat_counts.get(source_frame_index).copied().unwrap_or(0);
        source_frame_index = source_frame_index.saturating_add(1);
        if repeats == 0 {
            return Ok(());
        }

        let source_width = decoded.width();
        let source_height = decoded.height();
        if source_width == 0 || source_height == 0 {
            return Err(ScreenRecorderError::Export(
                "decoded frame has zero dimensions".to_string(),
            ));
        }

        let direct_frame_passthrough =
            source_width == width && source_height == height && decoded.format() == pixel_format;
        let mut prepared_base_rgba = false;
        let mut prepared_direct_output = false;
        let mut repeat_index = 0usize;

        while repeat_index < repeats {
            let output_index = scheduled_output_count.get();
            let output_ts = retime.timestamp_ms(output_index);
            let decision =
                advance_overlay_state(output_ts, overlay_tracks, mouse_config, &mut overlay_state);
            let repeat_key = repeatable_overlay_frame_key(decision);
            let allow_repeat_collapse = repeat_key.is_some();
            let mut frame_duration_ticks = 1usize;
            if let Some(repeat_key) = repeat_key {
                let mut scan_state = overlay_state.clone();
                while repeat_index + frame_duration_ticks < repeats {
                    let next_output_index = output_index + frame_duration_ticks;
                    let next_output_ts = retime.timestamp_ms(next_output_index);
                    let mut candidate_state = scan_state.clone();
                    let next_decision = advance_overlay_state(
                        next_output_ts,
                        overlay_tracks,
                        mouse_config,
                        &mut candidate_state,
                    );
                    if repeatable_overlay_frame_key(next_decision) != Some(repeat_key) {
                        break;
                    }
                    frame_duration_ticks += 1;
                    scan_state = candidate_state;
                }
                if frame_duration_ticks > 1 {
                    overlay_state = scan_state;
                }
            }

            if source_width == width
                && source_height == height
                && decision.needs_draw()
                && prepare_native_overlay_frame(
                    decoded,
                    &mut native_overlay_frame,
                    &mut native_overlay_key,
                )?
            {
                let native_frame = native_overlay_frame.as_mut().ok_or_else(|| {
                    ScreenRecorderError::Export(
                        "native overlay scratch frame is unexpectedly empty".to_string(),
                    )
                })?;
                if try_apply_mouse_overlays_native_from_decision_impl(
                    native_frame,
                    output_ts,
                    overlay_tracks,
                    mouse_config,
                    &mut overlay_state,
                    decision,
                    false,
                )? {
                    if pixel_format == native_frame.format() {
                        encoder.queue_prepared_frame(
                            native_frame,
                            frame_duration_ticks,
                            allow_repeat_collapse,
                            &scheduled_output_count,
                        )?;
                    } else {
                        scale_decoded_frame_to_output_format(
                            native_frame,
                            width,
                            height,
                            pixel_format,
                            &mut source_encode_scaler,
                            &mut source_encode_frame,
                            &mut source_encode_key,
                        )?;
                        let direct_frame = source_encode_frame.as_mut().ok_or_else(|| {
                            ScreenRecorderError::Export(
                                "source output frame buffer is uninitialized".to_string(),
                            )
                        })?;
                        encoder.queue_prepared_frame(
                            direct_frame,
                            frame_duration_ticks,
                            allow_repeat_collapse,
                            &scheduled_output_count,
                        )?;
                    }
                    repeat_index += frame_duration_ticks;
                    continue;
                }
            }

            if !decision.needs_draw() {
                if direct_frame_passthrough {
                    encoder.queue_prepared_frame(
                        decoded,
                        frame_duration_ticks,
                        allow_repeat_collapse,
                        &scheduled_output_count,
                    )?;
                } else {
                    if !prepared_direct_output {
                        scale_decoded_frame_to_output_format(
                            decoded,
                            width,
                            height,
                            pixel_format,
                            &mut source_encode_scaler,
                            &mut source_encode_frame,
                            &mut source_encode_key,
                        )?;
                        prepared_direct_output = true;
                    }

                    let direct_frame = source_encode_frame.as_mut().ok_or_else(|| {
                        ScreenRecorderError::Export(
                            "source output frame buffer is uninitialized".to_string(),
                        )
                    })?;
                    encoder.queue_prepared_frame(
                        direct_frame,
                        frame_duration_ticks,
                        allow_repeat_collapse,
                        &scheduled_output_count,
                    )?;
                }
                repeat_index += frame_duration_ticks;
                continue;
            }

            if repeats == 1 && can_write_direct_rgba {
                {
                    let plane = rgba_frame.data_mut(0);
                    decode_frame_to_output_rgba(
                        decoded,
                        width,
                        height,
                        &mut decode_rgba_scaler,
                        &mut decode_rgba_frame,
                        &mut decode_rgba_key,
                        &mut plane[..rgba_len],
                    )?;
                    let mut surface = FrameSurfaceMut {
                        hdr: None,
                        timestamp_ms: output_ts,
                        width,
                        height,
                        rgba: &mut plane[..rgba_len],
                    };
                    apply_mouse_overlays_surface_from_decision(
                        &mut surface,
                        overlay_tracks,
                        mouse_config,
                        &mut overlay_state,
                        decision,
                    );
                }

                ensure_video_frame_writable(&mut encode_frame)?;
                encode_scaler
                    .run(&rgba_frame, &mut encode_frame)
                    .map_err(|err| {
                        ScreenRecorderError::Export(format!(
                            "failed to convert overlay frame for video export: {err}"
                        ))
                    })?;
                encoder.queue_prepared_frame(
                    &encode_frame,
                    1,
                    allow_repeat_collapse,
                    &scheduled_output_count,
                )?;
                repeat_index += 1;
                continue;
            }

            if !prepared_base_rgba {
                decode_frame_to_output_rgba(
                    decoded,
                    width,
                    height,
                    &mut decode_rgba_scaler,
                    &mut decode_rgba_frame,
                    &mut decode_rgba_key,
                    &mut base_rgba,
                )?;
                prepared_base_rgba = true;
            }

            if can_write_direct_rgba {
                let plane = rgba_frame.data_mut(0);
                plane[..rgba_len].copy_from_slice(&base_rgba);
                let mut surface = FrameSurfaceMut {
                    hdr: None,
                    timestamp_ms: output_ts,
                    width,
                    height,
                    rgba: &mut plane[..rgba_len],
                };
                apply_mouse_overlays_surface_from_decision(
                    &mut surface,
                    overlay_tracks,
                    mouse_config,
                    &mut overlay_state,
                    decision,
                );
            } else {
                generated_rgba.copy_from_slice(&base_rgba);
                let mut surface = FrameSurfaceMut {
                    hdr: None,
                    timestamp_ms: output_ts,
                    width,
                    height,
                    rgba: &mut generated_rgba,
                };
                apply_mouse_overlays_surface_from_decision(
                    &mut surface,
                    overlay_tracks,
                    mouse_config,
                    &mut overlay_state,
                    decision,
                );
                copy_rgba_into_frame(&mut rgba_frame, width, &generated_rgba);
            }

            ensure_video_frame_writable(&mut encode_frame)?;
            encode_scaler
                .run(&rgba_frame, &mut encode_frame)
                .map_err(|err| {
                    ScreenRecorderError::Export(format!(
                        "failed to convert overlay frame for video export: {err}"
                    ))
                })?;
            encoder.queue_prepared_frame(
                &encode_frame,
                frame_duration_ticks,
                allow_repeat_collapse,
                &scheduled_output_count,
            )?;
            repeat_index += frame_duration_ticks;
        }
        Ok(())
    };

    for (stream, packet) in input.packets() {
        if decode_complete || scheduled_output_count.get() >= retime.frame_count() {
            break;
        }
        if stream.index() != input_video_stream_index {
            continue;
        }
        check_canceled(cancel_flag)?;
        decoder.send_packet(&packet).map_err(|err| {
            ScreenRecorderError::Export(format!("failed to feed packet into source decoder: {err}"))
        })?;
        loop {
            match decoder.receive_frame(&mut decoded) {
                Ok(()) => {
                    let decoded_frame = normalize_decoded_video_frame(
                        &mut decoded,
                        &mut transferred_decoded,
                        hw_decode_state.as_ref(),
                    )?;
                    process_decoded_frame(decoded_frame)?;
                    if scheduled_output_count.get() >= retime.frame_count() {
                        decode_complete = true;
                        break;
                    }
                }
                Err(err) if is_eagain(&err) => break,
                Err(ffmpeg::Error::Eof) => break,
                Err(err) => {
                    return Err(ScreenRecorderError::Export(format!(
                        "failed to decode source video frame: {err}"
                    )));
                }
            }
        }
    }

    if !decode_complete {
        decoder.send_eof().map_err(|err| {
            ScreenRecorderError::Export(format!("failed to flush source video decoder: {err}"))
        })?;
        loop {
            check_canceled(cancel_flag)?;
            match decoder.receive_frame(&mut decoded) {
                Ok(()) => {
                    let decoded_frame = normalize_decoded_video_frame(
                        &mut decoded,
                        &mut transferred_decoded,
                        hw_decode_state.as_ref(),
                    )?;
                    process_decoded_frame(decoded_frame)?;
                    if scheduled_output_count.get() >= retime.frame_count() {
                        break;
                    }
                }
                Err(err) if is_eagain(&err) => continue,
                Err(ffmpeg::Error::Eof) => break,
                Err(err) => {
                    return Err(ScreenRecorderError::Export(format!(
                        "failed to drain source video decoder: {err}"
                    )));
                }
            }
        }
    }

    #[allow(clippy::drop_non_drop)]
    drop(process_decoded_frame);

    if scheduled_output_count.get() != retime.frame_count() {
        return Err(ScreenRecorderError::Export(format!(
            "source decode ended before all overlay frames were produced (scheduled {}, expected {})",
            scheduled_output_count.get(),
            retime.frame_count()
        )));
    }

    let mut telemetry = encoder.finish(retime.output_duration_ms())?;
    telemetry.video_decoder = Some(video_decoder);
    telemetry.used_hardware_decode = used_hardware_decode;
    Ok(telemetry)
}

fn decode_frame_to_output_rgba(
    decoded: &ffmpeg::frame::Video,
    output_w: u32,
    output_h: u32,
    scaler: &mut Option<ffmpeg::software::scaling::Context>,
    rgba_frame: &mut Option<ffmpeg::frame::Video>,
    scaler_key: &mut Option<(u32, u32, ffmpeg::format::Pixel)>,
    out: &mut [u8],
) -> Result<()> {
    let source_w = decoded.width();
    let source_h = decoded.height();
    if source_w == 0 || source_h == 0 {
        return Err(ScreenRecorderError::Export(
            "decoded frame has zero dimensions".to_string(),
        ));
    }

    let decoded_format = decoded.format();
    let expected_len = output_w as usize * output_h as usize * 4;
    debug_assert_eq!(out.len(), expected_len);

    if source_w == output_w && source_h == output_h {
        if decoded_format == ffmpeg::format::Pixel::RGBA {
            return extract_rgba_from_frame_into(decoded, output_w, output_h, out);
        }
        if decoded_format == ffmpeg::format::Pixel::BGRA {
            return extract_bgra_from_frame_into(decoded, output_w, output_h, out);
        }
    }

    let key = (source_w, source_h, decoded_format);
    if scaler_key.as_ref() != Some(&key) {
        *scaler = Some(
            ffmpeg::software::scaling::Context::get(
                decoded_format,
                source_w,
                source_h,
                ffmpeg::format::Pixel::RGBA,
                output_w,
                output_h,
                ffmpeg::software::scaling::flag::Flags::BICUBIC,
            )
            .map_err(|err| {
                ScreenRecorderError::Export(format!(
                    "failed to create source-to-overlay RGBA scaler: {err}"
                ))
            })?,
        );
        *rgba_frame = Some(ffmpeg::frame::Video::new(
            ffmpeg::format::Pixel::RGBA,
            output_w,
            output_h,
        ));
        *scaler_key = Some(key);
    }

    let scaler_ref = scaler.as_mut().ok_or_else(|| {
        ScreenRecorderError::Export("overlay RGBA scaler is uninitialized".to_string())
    })?;
    let rgba_ref = rgba_frame.as_mut().ok_or_else(|| {
        ScreenRecorderError::Export("overlay RGBA frame buffer is uninitialized".to_string())
    })?;
    scaler_ref.run(decoded, rgba_ref).map_err(|err| {
        ScreenRecorderError::Export(format!(
            "failed to convert decoded frame into overlay RGBA: {err}"
        ))
    })?;
    extract_rgba_from_frame_into(rgba_ref, output_w, output_h, out)
}

fn scale_decoded_frame_to_output_format(
    decoded: &ffmpeg::frame::Video,
    output_w: u32,
    output_h: u32,
    output_format: ffmpeg::format::Pixel,
    scaler: &mut Option<crate::output_scaler::OutputScaler>,
    scaled_frame: &mut Option<ffmpeg::frame::Video>,
    scaler_key: &mut Option<(u32, u32, ffmpeg::format::Pixel)>,
) -> Result<()> {
    let source_w = decoded.width();
    let source_h = decoded.height();
    if source_w == 0 || source_h == 0 {
        return Err(ScreenRecorderError::Export(
            "decoded frame has zero dimensions".to_string(),
        ));
    }

    let decoded_format = decoded.format();
    let key = (source_w, source_h, decoded_format);
    if scaler_key.as_ref() != Some(&key) {
        *scaler = Some(
            crate::output_scaler::OutputScaler::get(
                decoded_format,
                source_w,
                source_h,
                output_format,
                output_w,
                output_h,
                ffmpeg::software::scaling::flag::Flags::BICUBIC,
            )
            .map_err(|err| {
                ScreenRecorderError::Export(format!(
                    "failed to create source-to-output video scaler: {err}"
                ))
            })?,
        );
        *scaled_frame = Some(ffmpeg::frame::Video::new(output_format, output_w, output_h));
        *scaler_key = Some(key);
    }

    let scaler_ref = scaler.as_mut().ok_or_else(|| {
        ScreenRecorderError::Export("source output video scaler is uninitialized".to_string())
    })?;
    let frame_ref = scaled_frame.as_mut().ok_or_else(|| {
        ScreenRecorderError::Export("source output video frame buffer is uninitialized".to_string())
    })?;
    ensure_video_frame_writable(frame_ref)?;
    scaler_ref.run(decoded, frame_ref).map_err(|err| {
        ScreenRecorderError::Export(format!(
            "failed to convert decoded frame into output video format: {err}"
        ))
    })
}

fn can_collapse_repeated_video_frames(frame: &ffmpeg::frame::Video) -> bool {
    match frame.format() {
        ffmpeg::format::Pixel::YUV420P
        | ffmpeg::format::Pixel::NV12
        | ffmpeg::format::Pixel::YUV422P
        | ffmpeg::format::Pixel::RGB24
        | ffmpeg::format::Pixel::RGBA => frame.width() > 0 && frame.height() > 0,
        _ => false,
    }
}

fn video_frame_plane_geometry(
    format: ffmpeg::format::Pixel,
    plane: usize,
    width: u32,
    height: u32,
) -> Option<(usize, usize)> {
    let width = width.max(1) as usize;
    let height = height.max(1) as usize;
    match format {
        ffmpeg::format::Pixel::YUV420P => match plane {
            0 => Some((width, height)),
            1 | 2 => Some((width.div_ceil(2), height.div_ceil(2))),
            _ => None,
        },
        ffmpeg::format::Pixel::NV12 => match plane {
            0 => Some((width, height)),
            1 => Some((width.div_ceil(2) * 2, height.div_ceil(2))),
            _ => None,
        },
        ffmpeg::format::Pixel::YUV422P => match plane {
            0 => Some((width, height)),
            1 | 2 => Some((width.div_ceil(2), height)),
            _ => None,
        },
        ffmpeg::format::Pixel::RGB24 => match plane {
            0 => Some((width.saturating_mul(3), height)),
            _ => None,
        },
        ffmpeg::format::Pixel::RGBA => match plane {
            0 => Some((width.saturating_mul(4), height)),
            _ => None,
        },
        _ => None,
    }
}

fn video_frames_match(lhs: &ffmpeg::frame::Video, rhs: &ffmpeg::frame::Video) -> bool {
    if lhs.format() != rhs.format() || lhs.width() != rhs.width() || lhs.height() != rhs.height() {
        return false;
    }
    if !can_collapse_repeated_video_frames(lhs) {
        return false;
    }

    for plane in 0..4 {
        let Some((row_bytes, rows)) =
            video_frame_plane_geometry(lhs.format(), plane, lhs.width(), lhs.height())
        else {
            break;
        };
        let lhs_stride = lhs.stride(plane);
        let rhs_stride = rhs.stride(plane);
        let lhs_data = lhs.data(plane);
        let rhs_data = rhs.data(plane);

        for row in 0..rows {
            let lhs_start = row.saturating_mul(lhs_stride);
            let rhs_start = row.saturating_mul(rhs_stride);
            let lhs_end = lhs_start.saturating_add(row_bytes);
            let rhs_end = rhs_start.saturating_add(row_bytes);
            if lhs_end > lhs_data.len() || rhs_end > rhs_data.len() {
                return false;
            }
            if lhs_data[lhs_start..lhs_end] != rhs_data[rhs_start..rhs_end] {
                return false;
            }
        }
    }

    true
}

fn validate_export_dimensions(width: u32, height: u32, require_even: bool) -> Result<()> {
    if width == 0 || height == 0 {
        return Err(ScreenRecorderError::Export(
            "video export requires non-zero frame dimensions".to_string(),
        ));
    }

    if require_even && (!width.is_multiple_of(2) || !height.is_multiple_of(2)) {
        return Err(ScreenRecorderError::Export(
            "selected export requires even width and height".to_string(),
        ));
    }

    Ok(())
}

fn validate_bundle_artifact(
    artifact: &RecordingArtifact,
    manifest: &SessionManifest,
    bundle_footer: &RecordingBundleFooter,
) -> Result<()> {
    if !artifact.bundle_path.is_file() {
        return Err(ScreenRecorderError::Decode(format!(
            "required artifact is missing: {}",
            artifact.bundle_path.display()
        )));
    }

    for kind in [BundleAssetKind::VideoIndex, BundleAssetKind::MouseStore] {
        if bundle_footer.asset(kind, None).is_none() {
            return Err(ScreenRecorderError::Decode(format!(
                "bundle {} is missing required asset {}",
                artifact.bundle_path.display(),
                bundle_asset_label(kind)
            )));
        }
    }

    for track in manifest.audio_tracks.iter().filter(|track| track.recorded) {
        if bundle_footer
            .asset(BundleAssetKind::AudioTrack, Some(track.asset_id.as_str()))
            .is_none()
        {
            return Err(ScreenRecorderError::Decode(format!(
                "bundle {} is missing required asset {}",
                artifact.bundle_path.display(),
                track.asset_id
            )));
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::path::PathBuf;

    use snow_audio_recorder::align_i16_interleaved_to_duration;
    use snow_recording_model::{
        AudioSampleFormat, AudioTrackManifest, AudioTrackRole, BundleAssetRecord,
        LocalRecordingPaths,
    };
    use tempfile::tempdir;

    fn test_hardware_decode_state() -> (HardwareDecodeState, *mut ffmpeg::ffi::AVBufferRef) {
        unsafe {
            let device_ctx = ffmpeg::ffi::av_buffer_alloc(1);
            assert!(!device_ctx.is_null());
            let witness = ffmpeg::ffi::av_buffer_ref(device_ctx);
            assert!(!witness.is_null());
            (
                HardwareDecodeState {
                    device_ctx,
                    _selection: Box::new(HardwareDecodeSelection {
                        hw_pix_fmt: ffmpeg::ffi::AVPixelFormat::AV_PIX_FMT_NONE,
                    }),
                    hw_pixel_format: ffmpeg::format::Pixel::None,
                    device_name: "test",
                },
                witness,
            )
        }
    }

    #[test]
    fn hardware_decoder_context_creation_failure_releases_device() {
        let (hardware, mut witness) = test_hardware_decode_state();
        let result =
            prepare_hardware_decoder_context(hardware, &ExportPerformanceConfig::default(), || {
                Err(ffmpeg::Error::InvalidData)
            });
        assert!(matches!(result, Err(ScreenRecorderError::Export(_))));
        unsafe {
            assert_eq!(ffmpeg::ffi::av_buffer_get_ref_count(witness), 1);
            ffmpeg::ffi::av_buffer_unref(&mut witness);
        }
    }

    #[test]
    fn hardware_decoder_context_owns_an_independent_device_reference() {
        let (hardware, mut witness) = test_hardware_decode_state();
        let context =
            prepare_hardware_decoder_context(hardware, &ExportPerformanceConfig::default(), || {
                Ok(ffmpeg::codec::context::Context::new())
            })
            .unwrap();
        unsafe {
            assert_eq!(ffmpeg::ffi::av_buffer_get_ref_count(witness), 3);
        }
        drop(context);
        unsafe {
            assert_eq!(ffmpeg::ffi::av_buffer_get_ref_count(witness), 1);
            ffmpeg::ffi::av_buffer_unref(&mut witness);
        }
    }

    struct DecoderShutdownWitness {
        device: *mut ffmpeg::ffi::AVBufferRef,
        references_at_shutdown: Arc<std::sync::atomic::AtomicUsize>,
    }

    unsafe extern "C" fn observe_decoder_shutdown(opaque: *mut c_void, bytes: *mut u8) {
        unsafe {
            let witness = Box::from_raw(opaque.cast::<DecoderShutdownWitness>());
            witness.references_at_shutdown.store(
                ffmpeg::ffi::av_buffer_get_ref_count(witness.device) as usize,
                Ordering::SeqCst,
            );
            ffmpeg::ffi::av_free(bytes.cast());
        }
    }

    #[test]
    fn hardware_decoder_context_shuts_down_before_hardware_state() {
        let (hardware, mut witness) = test_hardware_decode_state();
        let mut context =
            prepare_hardware_decoder_context(hardware, &ExportPerformanceConfig::default(), || {
                Ok(ffmpeg::codec::context::Context::new())
            })
            .unwrap();
        let references_at_shutdown = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        unsafe {
            let bytes = ffmpeg::ffi::av_malloc(1).cast::<u8>();
            assert!(!bytes.is_null());
            let observer = Box::new(DecoderShutdownWitness {
                device: witness,
                references_at_shutdown: references_at_shutdown.clone(),
            });
            let observer = Box::into_raw(observer).cast();
            let buffer = ffmpeg::ffi::av_buffer_create(
                bytes,
                1,
                Some(observe_decoder_shutdown),
                observer,
                0,
            );
            if buffer.is_null() {
                observe_decoder_shutdown(observer, bytes);
            }
            assert!(!buffer.is_null());
            // FFmpeg releases hw_frames_ctx during codec shutdown, before its
            // device reference. This observes the real native destructor without
            // a hardware decoder or dereferencing a possibly freed selection.
            (*context.decoder.as_mut_ptr()).hw_frames_ctx = buffer;
        }
        drop(context);
        assert_eq!(
            references_at_shutdown.load(Ordering::SeqCst),
            3,
            "hardware state and its selection must remain owned during codec shutdown"
        );
        unsafe {
            assert_eq!(ffmpeg::ffi::av_buffer_get_ref_count(witness), 1);
            ffmpeg::ffi::av_buffer_unref(&mut witness);
        }
    }

    fn test_streaming_frame_source(
        rx: Receiver<DecodedFrameMessage>,
        control: DecodeWorkerControl,
        worker: thread::JoinHandle<()>,
    ) -> StreamingVideoFrameSource {
        let (recycle_tx, _) = crossbeam_channel::bounded(1);
        StreamingVideoFrameSource {
            rx: Some(rx),
            recycle_tx,
            current_index: None,
            current_frame: None,
            control,
            worker: Some(worker),
        }
    }

    #[test]
    fn streaming_video_frame_source_drop_joins_blocked_worker() {
        let canceled = Arc::new(AtomicBool::new(false));
        let control = DecodeWorkerControl::new(canceled.clone());
        let resource = Arc::new(());
        let worker_resource = resource.clone();
        let (tx, rx) = crossbeam_channel::bounded(1);
        let (started_tx, started_rx) = crossbeam_channel::bounded(1);
        let (finished_tx, finished_rx) = crossbeam_channel::bounded(1);
        let worker = thread::spawn(move || {
            tx.send(DecodedFrameMessage::Frame {
                source_index: 0,
                frame: StoredFrame {
                    timestamp_ms: 0,
                    duration_ms: 33,
                    width: 2,
                    height: 2,
                    rgba: vec![0x40; 16],
                },
            })
            .unwrap();
            started_tx.send(()).unwrap();
            // The queue remains full until source shutdown releases its receiver.
            let disconnected = tx.send(DecodedFrameMessage::End).is_err();
            drop(worker_resource);
            finished_tx.send(disconnected).unwrap();
        });
        let source = test_streaming_frame_source(rx, control.clone(), worker);
        started_rx.recv().unwrap();
        let (dropped_tx, dropped_rx) = crossbeam_channel::bounded(1);
        let resource_witness = resource.clone();
        let dropper = thread::spawn(move || {
            drop(source);
            dropped_tx
                .send(Arc::strong_count(&resource_witness))
                .unwrap();
        });
        let remaining_owners = dropped_rx
            .recv_timeout(Duration::from_secs(10))
            .expect("source shutdown must unblock the full decode queue");
        dropper.join().unwrap();
        assert!(finished_rx.recv().unwrap());
        assert_eq!(remaining_owners, 2, "source Drop must join worker cleanup");
        assert!(control.stopped.load(Ordering::Acquire));
        assert!(!canceled.load(Ordering::Acquire));
    }

    #[test]
    fn streaming_video_frame_source_drop_stops_and_joins_active_worker() {
        let canceled = Arc::new(AtomicBool::new(false));
        let control = DecodeWorkerControl::new(canceled.clone());
        let worker_control = control.clone();
        let resource = Arc::new(());
        let worker_resource = resource.clone();
        let (_, rx) = crossbeam_channel::bounded(1);
        let (started_tx, started_rx) = crossbeam_channel::bounded(1);
        let worker = thread::spawn(move || {
            started_tx.send(()).unwrap();
            while worker_control.check_canceled().is_ok() {
                thread::yield_now();
            }
            drop(worker_resource);
        });
        let source = test_streaming_frame_source(rx, control.clone(), worker);
        started_rx.recv().unwrap();
        let (dropped_tx, dropped_rx) = crossbeam_channel::bounded(1);
        let resource_witness = resource.clone();
        let dropper = thread::spawn(move || {
            drop(source);
            dropped_tx
                .send(Arc::strong_count(&resource_witness))
                .unwrap();
        });
        let remaining_owners = dropped_rx
            .recv_timeout(Duration::from_secs(10))
            .expect("source shutdown must stop active decode work");
        dropper.join().unwrap();
        // Also release a worker if a regression removes the local stop signal.
        control.stop();
        assert_eq!(remaining_owners, 2, "source Drop must join worker cleanup");
        assert!(!canceled.load(Ordering::Acquire));
    }

    #[test]
    fn streaming_video_frame_source_preserves_frames_and_end_message() {
        let canceled = Arc::new(AtomicBool::new(false));
        let control = DecodeWorkerControl::new(canceled.clone());
        let (tx, rx) = crossbeam_channel::bounded(2);
        let worker = thread::spawn(move || {
            tx.send(DecodedFrameMessage::Frame {
                source_index: 0,
                frame: StoredFrame {
                    timestamp_ms: 42,
                    duration_ms: 33,
                    width: 2,
                    height: 2,
                    rgba: vec![0x40; 16],
                },
            })
            .unwrap();
            tx.send(DecodedFrameMessage::End).unwrap();
        });
        let mut source = test_streaming_frame_source(rx, control, worker);
        let frame = source.frame_at(0).unwrap();
        assert_eq!(frame.timestamp_ms, 42);
        assert_eq!(frame.rgba, vec![0x40; 16]);
        let err = source.frame_at(1).unwrap_err();
        assert!(
            matches!(err, ScreenRecorderError::Export(message) if message == "decode stream ended before requested frame")
        );
        drop(source);
        assert!(!canceled.load(Ordering::Acquire));
    }

    #[test]
    fn streaming_video_frame_source_preserves_decoder_error() {
        let directory = tempdir().unwrap();
        let canceled = Arc::new(AtomicBool::new(false));
        let mut source = StreamingVideoFrameSource::spawn(
            &directory.path().join("missing-source.mkv"),
            vec![0],
            30,
            1,
            1,
            canceled.clone(),
        )
        .unwrap();
        let err = source.frame_at(0).unwrap_err();
        assert!(
            matches!(err, ScreenRecorderError::Export(message) if message.contains("failed to open temporary recording video") && message.contains("missing-source.mkv"))
        );
        drop(source);
        assert!(!canceled.load(Ordering::Acquire));
    }

    fn test_track(
        track_id: &str,
        role: AudioTrackRole,
        recorded: bool,
        sample_rate_hz: u32,
        channels: u16,
    ) -> AudioTrackManifest {
        AudioTrackManifest {
            track_id: track_id.to_string(),
            role,
            asset_id: format!("audio/{track_id}.pcm"),
            sample_rate_hz,
            channels,
            sample_format: AudioSampleFormat::PcmS16Le,
            duration_frames: 0,
            recorded,
        }
    }

    fn set_track_enabled(request: &mut ExportRequest, track_id: &str, enabled: bool, volume: f32) {
        let track = request
            .audio_tracks
            .iter_mut()
            .find(|track| track.track_id == track_id)
            .expect("expected test audio track to exist");
        track.enabled = enabled;
        track.volume = volume;
    }

    fn test_editing_session(system: bool, microphone: bool) -> EditingSession {
        let manifest = SessionManifest {
            video_codec: snow_recording_model::VideoCodec::H264,
            media: snow_recording_model::media::RecordedMedia::new(
                snow_media::ColorDescription::SRGB,
                snow_media::CursorMode::Separate,
                vec![],
            ),
            session_id: "session".to_string(),
            output_dir: PathBuf::from("recordings"),
            keep_temp_files: false,
            fps: 30,
            intermediate_profile: snow_recording_model::IntermediateRecordingProfile::EditFast,
            recording_video: VideoEncodeConfig::default(),
            width: 1920,
            height: 1080,
            capture_origin_x: 0,
            capture_origin_y: 0,
            audio_tracks: vec![
                test_track("system", AudioTrackRole::SystemOutput, system, 48_000, 2),
                test_track(
                    "microphone",
                    AudioTrackRole::MicrophoneInput,
                    microphone,
                    48_000,
                    2,
                ),
            ],
            pause_intervals: Vec::new(),
        };
        let mut assets = vec![
            BundleAssetRecord {
                kind: BundleAssetKind::VideoIndex,
                asset_id: None,
                offset: 0,
                len: 0,
            },
            BundleAssetRecord {
                kind: BundleAssetKind::MouseStore,
                asset_id: None,
                offset: 0,
                len: 0,
            },
        ];
        if system {
            assets.push(BundleAssetRecord {
                kind: BundleAssetKind::AudioTrack,
                asset_id: Some("audio/system.pcm".to_string()),
                offset: 0,
                len: 0,
            });
        }
        if microphone {
            assets.push(BundleAssetRecord {
                kind: BundleAssetKind::AudioTrack,
                asset_id: Some("audio/microphone.pcm".to_string()),
                offset: 0,
                len: 0,
            });
        }

        EditingSession {
            artifact: RecordingArtifact {
                session_id: "session".to_string(),
                output_dir: PathBuf::from("recordings"),
                local_paths: LocalRecordingPaths {
                    temp_dir: PathBuf::from("recordings/tmp"),
                    video_intermediate_path: PathBuf::from("recordings/session.snowrec"),
                    video_index_path: PathBuf::from("recordings/tmp/video_index.bin"),
                    mouse_path: PathBuf::from("recordings/tmp/mouse.bin"),
                },
                bundle_path: PathBuf::from("recordings/session.snowrec"),
                audio_tracks: manifest.audio_tracks.clone(),
            },
            manifest: manifest.clone(),
            bundle_footer: RecordingBundleFooter {
                manifest,
                video_payload_len: 0,
                assets,
            },
        }
    }

    #[test]
    fn hdr_overlay_surface_preserves_padding_and_rejects_masks() {
        let mut bytes = vec![0; 32 * 2];
        bytes[24..32].fill(0xa5);
        bytes[56..64].fill(0xa5);
        let mut surface = FrameSurfaceMut {
            timestamp_ms: 0,
            width: 4,
            height: 2,
            rgba: &mut [],
            hdr: Some((&mut bytes, 32)),
        };
        blend_horizontal_span(&mut surface, 0, -1, 1, [255; 4]);
        set_pixel_blended(&mut surface, 3, 1, [255, 0, 0, 128]);
        set_pixel_blended(&mut surface, 4, 1, [255; 4]);
        assert_eq!(&bytes[24..32], &[0xa5; 8]);
        assert_eq!(&bytes[56..64], &[0xa5; 8]);
        assert_eq!(&bytes[12..24], &[0; 12]);
        let nits = snow_media::color::pq_to_nits(
            f32::from(u16::from_le_bytes([bytes[0], bytes[1]])) / 65535.0,
        );
        assert!((nits - 203.0).abs() < 0.1);
        let mut store = MouseStore::new();
        store.cursor_shapes.push(CursorShapeRecord {
            shape_id: 1,
            width: 1,
            height: 1,
            hotspot_x: 0,
            hotspot_y: 0,
            mode: CursorShapeCompositionMode::MaskedColor,
            shape_rgba: vec![255; 4],
        });
        assert!(validate_hdr_cursor_shapes(&build_mouse_tracks(store)).is_err());
    }

    #[test]
    fn mouse_assets_follow_video_playback_speed_and_check_overflow() {
        let mut tracks = MouseTracks {
            samples: vec![MouseSample {
                ts_ms: 33,
                x: 0,
                y: 0,
                visible: true,
                shape_id: None,
            }],
            click_downs: vec![MouseClickDown {
                ts_ms: 66,
                x: 0,
                y: 0,
            }],
            ..Default::default()
        };
        retime_mouse_tracks(&mut tracks, 0.5).unwrap();
        assert_eq!(tracks.samples[0].ts_ms, 66);
        assert_eq!(tracks.click_downs[0].ts_ms, 132);
        assert!(retime_mouse_tracks(&mut tracks, f32::NAN).is_err());
        tracks.samples[0].ts_ms = u64::MAX;
        assert!(retime_mouse_tracks(&mut tracks, 0.25).is_err());
    }

    #[test]
    fn legacy_export_defaults_keep_historical_cursor_and_recorded_audio() {
        let editing = test_editing_session(true, true);
        let request = editing.export_request();
        assert!(request.mouse.visible);
        assert!(request.audio_tracks.iter().all(|track| track.enabled));
        assert!(!request.mouse.trail_enabled && !request.mouse.click_enabled);
    }

    #[test]
    fn normalize_request_disables_unrecorded_audio_tracks() {
        let editing = test_editing_session(false, false);
        let mut request = editing.export_request();
        request.audio_tracks.push(ExportAudioTrackRequest {
            track_id: "ghost".to_string(),
            enabled: true,
            volume: 1.0,
        });

        let normalized = normalize_export_request(request, &editing.manifest);
        assert!(normalized.audio_tracks.is_empty());
    }

    #[test]
    fn normalize_request_keeps_recorded_audio_tracks_enabled() {
        let editing = test_editing_session(true, true);
        let mut request = editing.export_request();
        set_track_enabled(&mut request, "system", true, 1.0);
        set_track_enabled(&mut request, "microphone", true, 1.0);

        let normalized = normalize_export_request(request, &editing.manifest);
        assert_eq!(normalized.audio_tracks.len(), 2);
        assert!(normalized.audio_tracks.iter().all(|track| track.enabled));
    }

    #[test]
    fn failed_export_preserves_an_existing_destination() {
        let directory = tempdir().expect("temporary directory should be available");
        let output_path = directory.path().join("existing.mp4");
        fs::write(&output_path, b"keep this file").expect("destination fixture should be written");

        let editing = test_editing_session(false, false);
        let mut request = editing.export_request();
        request.output_path = output_path.clone();

        assert!(editing.export(request).is_err());
        assert_eq!(
            fs::read(&output_path).expect("existing destination should remain readable"),
            b"keep this file"
        );
        let staged_files = fs::read_dir(directory.path())
            .expect("temporary directory should remain readable")
            .filter_map(std::result::Result::ok)
            .filter(|entry| {
                entry
                    .file_name()
                    .to_string_lossy()
                    .starts_with(".snow-recording-export-")
            })
            .count();
        assert_eq!(staged_files, 0, "failed exports must clean staging files");
    }

    #[test]
    fn collect_required_source_indices_deduplicates_adjacent_indices() {
        assert_eq!(
            collect_required_source_indices([0, 0, 1, 1, 1, 3, 3, 8].into_iter()),
            vec![0, 1, 3, 8]
        );
        assert!(collect_required_source_indices([].into_iter()).is_empty());
    }

    #[test]
    fn choose_export_fps_keeps_recording_fps() {
        assert_eq!(choose_export_fps(60, ExportFormat::Mp4, None), 60);
        assert_eq!(choose_export_fps(30, ExportFormat::Avi, None), 30);
    }

    #[test]
    fn configured_mp4_codecs_use_exact_gpl_encoders() {
        assert_eq!(exact_mp4_encoder(VideoCodec::H264), ("libx264", "H.264"));
        assert_eq!(exact_mp4_encoder(VideoCodec::H265), ("libx265", "H.265"));
    }

    #[test]
    fn choose_export_fps_clamps_gif() {
        assert_eq!(choose_export_fps(60, ExportFormat::Gif, None), 20);
        assert_eq!(choose_export_fps(10, ExportFormat::Gif, None), 10);
        assert_eq!(choose_export_fps(60, ExportFormat::Gif, Some(24)), 24);
        assert_eq!(choose_export_fps(60, ExportFormat::Apng, Some(15)), 15);
        assert_eq!(choose_export_fps(60, ExportFormat::Webp, Some(10)), 10);
    }

    #[test]
    fn gif_source_and_overlay_exports_use_adaptive_colors() {
        let directory = tempdir().unwrap();
        let cancel = Arc::new(AtomicBool::new(false));
        let performance = ExportPerformanceConfig {
            mode: ExportExecutionMode::SoftwareOnly,
            ..Default::default()
        };
        let source = directory.path().join("source.apng");
        let colors = [[23, 37, 51, 255], [209, 151, 77, 255], [23, 37, 51, 255]];
        export_video_generated(
            &source,
            128,
            96,
            FinalizedTimeline::new(300, 10).unwrap(),
            ExportFormat::Apng,
            VideoCodec::H264,
            false,
            None,
            8,
            &VideoEncodeConfig::default(),
            &performance,
            &cancel,
            None,
            &None,
            |index, rgba| {
                for pixel in rgba.chunks_exact_mut(4) {
                    pixel.copy_from_slice(&colors[index]);
                }
                Ok(())
            },
        )
        .unwrap();
        let retime = RetimePlan {
            source_starts_ms: vec![0, 200, 300],
            speed: 1.0,
            timeline: FinalizedTimeline::new(400, 10).unwrap(),
        };
        let tracks = MouseTracks {
            click_downs: vec![MouseClickDown {
                ts_ms: 100,
                x: 64,
                y: 48,
            }],
            ..Default::default()
        };
        let mouse = MouseEditConfig {
            click_enabled: true,
            ..Default::default()
        };
        for overlay in [false, true] {
            let path = directory.path().join(format!("edited-{overlay}.gif"));
            if overlay {
                export_video_generated_from_source_with_overlay(
                    &source,
                    &path,
                    &retime,
                    128,
                    96,
                    10,
                    ExportFormat::Gif,
                    VideoCodec::H264,
                    false,
                    None,
                    8,
                    &VideoEncodeConfig::default(),
                    &performance,
                    &tracks,
                    &mouse,
                    &cancel,
                    None,
                    &None,
                )
                .unwrap();
            } else {
                export_video_generated_from_source(
                    &source,
                    &path,
                    &retime,
                    false,
                    None,
                    128,
                    96,
                    10,
                    ExportFormat::Gif,
                    VideoCodec::H264,
                    false,
                    None,
                    8,
                    &VideoEncodeConfig::default(),
                    &performance,
                    &cancel,
                    None,
                    &None,
                )
                .unwrap();
            }
            let mut options = gif::DecodeOptions::new();
            options.set_color_output(gif::ColorOutput::RGBA);
            let mut decoder = options.read_info(fs::File::open(path).unwrap()).unwrap();
            let mut count = 0;
            let mut delay = 0;
            let mut corner = [0; 4];
            while let Some(frame) = decoder.read_next_frame().unwrap() {
                if frame.left == 0 && frame.top == 0 && frame.buffer[3] != 0 {
                    corner.copy_from_slice(&frame.buffer[..4]);
                }
                assert_eq!(
                    corner,
                    colors[retime.source_index(usize::from(delay / 10))],
                    "overlay={overlay}, frame={count}"
                );
                delay += frame.delay;
                count += 1;
            }
            assert!(
                (3..=4).contains(&count),
                "identical observations may share a longer GIF delay"
            );
            assert_eq!(delay, 40);
        }
    }

    #[test]
    fn production_exporters_emit_valid_container_signatures() {
        let directory = tempdir().expect("temporary output directory should be available");
        let cancel_flag = Arc::new(AtomicBool::new(false));
        let performance = ExportPerformanceConfig {
            mode: ExportExecutionMode::SoftwareOnly,
            ..ExportPerformanceConfig::default()
        };
        let mut muxer_names = Vec::new();
        let mut muxer_opaque = ptr::null_mut();
        loop {
            let muxer = unsafe { ffmpeg::ffi::av_muxer_iterate(&mut muxer_opaque) };
            if muxer.is_null() {
                break;
            }
            let name = unsafe { std::ffi::CStr::from_ptr((*muxer).name) };
            muxer_names.push(name.to_string_lossy().into_owned());
        }
        muxer_names.sort_unstable();
        assert_eq!(
            muxer_names,
            if cfg!(target_os = "macos") {
                vec![
                    "apng", "avi", "gif", "matroska", "mov", "mp4", "wav", "webp",
                ]
            } else {
                vec!["apng", "avi", "gif", "matroska", "mov", "mp4", "webp"]
            },
            "FFmpeg muxer registry does not match the production profile"
        );

        let cases = [
            (ExportFormat::Mp4, VideoCodec::H264, "h264", "libx264"),
            (ExportFormat::Mp4, VideoCodec::H265, "h265", "libx265"),
            (ExportFormat::Avi, VideoCodec::H264, "mpeg4", "mpeg4"),
            (ExportFormat::Gif, VideoCodec::H264, "gif", "gif"),
            (ExportFormat::Apng, VideoCodec::H264, "apng", "apng"),
            (ExportFormat::Webp, VideoCodec::H264, "webp", "libwebp_anim"),
        ];

        for (format, codec, label, expected_encoder) in cases {
            let output_path = directory
                .path()
                .join(format!("{label}-test.{}", format.file_extension()));
            let result = export_video_generated(
                &output_path,
                16,
                16,
                FinalizedTimeline::new(200, 10).unwrap(),
                format,
                codec,
                false,
                None,
                8,
                &VideoEncodeConfig::default(),
                &performance,
                &cancel_flag,
                None,
                &None,
                |index, rgba| {
                    for (pixel_index, pixel) in rgba.chunks_exact_mut(4).enumerate() {
                        pixel[0] = if index == 0 { 0x20 } else { 0xE0 };
                        pixel[1] = (pixel_index % 16) as u8 * 16;
                        pixel[2] = (pixel_index / 16) as u8 * 16;
                        pixel[3] = 0xFF;
                    }
                    Ok(())
                },
            )
            .unwrap_or_else(|error| panic!("{label} should encode: {error}"));

            assert_eq!(result.video_encoder.as_deref(), Some(expected_encoder));
            let bytes = fs::read(&output_path)
                .unwrap_or_else(|error| panic!("{label} output should be readable: {error}"));
            assert!(
                bytes.len() > 16,
                "{label} output should contain encoded data"
            );

            match format {
                ExportFormat::Mp4 => assert_eq!(&bytes[4..8], b"ftyp"),
                ExportFormat::Avi => {
                    assert_eq!(&bytes[0..4], b"RIFF");
                    assert_eq!(&bytes[8..12], b"AVI ");
                }
                ExportFormat::Gif => {
                    assert!(bytes.starts_with(b"GIF87a") || bytes.starts_with(b"GIF89a"));
                }
                ExportFormat::Apng => {
                    assert!(bytes.starts_with(b"\x89PNG\r\n\x1a\n"));
                    assert!(
                        bytes.windows(4).any(|window| window == b"acTL"),
                        "APNG output should contain an animation control chunk"
                    );
                }
                ExportFormat::Webp => {
                    assert_eq!(&bytes[0..4], b"RIFF");
                    assert_eq!(&bytes[8..12], b"WEBP");
                    assert!(
                        bytes.windows(4).any(|window| window == b"ANIM"),
                        "WebP output should contain an animation header"
                    );
                    assert!(
                        bytes.windows(4).any(|window| window == b"ANMF"),
                        "WebP output should contain an animation frame"
                    );
                }
            }
        }
    }

    #[test]
    fn mp4_hardware_preference_falls_back_to_software_encoder() {
        let directory = tempdir().expect("temporary output directory should be available");
        let cancel_flag = Arc::new(AtomicBool::new(false));
        let output_path = directory.path().join("h264-hardware-test.mp4");
        // Use a realistic frame size: Media Foundation rejects tiny outputs.
        let result = export_video_generated(
            &output_path,
            640,
            360,
            FinalizedTimeline::new(200, 10).unwrap(),
            ExportFormat::Mp4,
            VideoCodec::H264,
            true,
            None,
            8,
            &VideoEncodeConfig::default(),
            &ExportPerformanceConfig::default(),
            &cancel_flag,
            None,
            &None,
            |index, rgba| {
                for pixel in rgba.chunks_exact_mut(4) {
                    pixel[0] = if index == 0 { 0x20 } else { 0xE0 };
                    pixel[1] = 0x80;
                    pixel[2] = 0x80;
                    pixel[3] = 0xFF;
                }
                Ok(())
            },
        )
        .unwrap_or_else(|error| panic!("hardware-preferred export should encode: {error}"));

        // h264_mf when a Media Foundation H.264 encoder is available, otherwise
        // the software fallback must have produced the file instead.
        assert!(
            matches!(
                result.video_encoder.as_deref(),
                Some("h264_mf") | Some("h264_videotoolbox") | Some("libx264") | Some("libopenh264")
            ),
            "unexpected encoder for hardware-preferred export: {:?}",
            result.video_encoder
        );
        let bytes = fs::read(&output_path).expect("hardware-preferred output should be readable");
        assert!(
            bytes.len() > 16 && &bytes[4..8] == b"ftyp",
            "hardware-preferred output should be a valid MP4"
        );
    }

    #[test]
    fn output_dimensions_preserve_aspect_ratio_and_do_not_upscale() {
        assert_eq!(
            output_dimensions(3840, 2160, Some(1920), Some(1080), true),
            (1920, 1080)
        );
        assert_eq!(
            output_dimensions(1920, 1080, Some(3840), Some(2160), true),
            (1920, 1080)
        );
        assert_eq!(
            output_dimensions(1080, 1920, Some(1080), Some(1920), true),
            (1080, 1920)
        );
        assert_eq!(
            output_dimensions(3000, 2000, Some(1920), Some(1080), true),
            (1620, 1080)
        );
        assert_eq!(output_dimensions(1001, 777, None, None, true), (1000, 776));
    }

    #[test]
    fn hardware_video_encode_allowed_matches_mode() {
        assert!(hardware_video_encode_allowed(
            ExportExecutionMode::HardwarePreferred
        ));
        assert!(hardware_video_encode_allowed(
            ExportExecutionMode::HardwareOnly
        ));
        assert!(!hardware_video_encode_allowed(
            ExportExecutionMode::SoftwareOnly
        ));
    }

    #[test]
    fn retiming_uses_a_rational_iterator_without_output_sized_storage() {
        let index = [
            VideoIndexEntry {
                timestamp_ms: 0,
                duration_ms: 1000,
            },
            VideoIndexEntry {
                timestamp_ms: 1000,
                duration_ms: 1000,
            },
        ];
        let plan = build_retime_plan_from_index(&index, 0.25, 144).unwrap();
        assert_eq!(plan.frame_count(), 1152);
        assert_eq!(plan.source_starts_ms.len(), 2);
        assert_eq!(plan.timestamp_ms(144), 1000);
        assert_eq!(plan.source_index(576), 1);
        assert_eq!(
            plan.source_indices().collect::<Vec<_>>(),
            (0..plan.frame_count())
                .map(|index| plan.source_index(index))
                .collect::<Vec<_>>()
        );
        let invalid = [
            VideoIndexEntry {
                timestamp_ms: 50,
                duration_ms: 1,
            },
            VideoIndexEntry {
                timestamp_ms: 40,
                duration_ms: 1,
            },
        ];
        assert!(build_retime_plan_from_index(&invalid, 1.0, 30).is_err());
        let fractional = build_retime_plan_from_index(
            &[VideoIndexEntry {
                timestamp_ms: 0,
                duration_ms: 1001,
            }],
            1.0,
            83,
        )
        .unwrap();
        let canonical = FinalizedTimeline::new(1001, 83).unwrap();
        assert_eq!(fractional.timeline, canonical);
        assert_eq!(fractional.timestamp_ms(11), 132);
        for frame in canonical.iter() {
            assert_eq!(
                fractional.timestamp_ms(frame.index as usize),
                frame.timestamp_ms
            );
        }
    }

    #[test]
    fn progress_retains_only_the_latest_monotonic_update() {
        let (reporter, receiver) = ProgressReporter::channel();
        let tx = Some(reporter);
        for index in 0..1000 {
            emit_progress(
                &tx,
                ExportStage::VideoEncode,
                index as f32 / 10.0,
                30.0,
                None,
            );
        }
        assert_eq!(receiver.len(), 1);
        assert_eq!(receiver.try_recv().unwrap().percent, 99.9);
        emit_progress(&tx, ExportStage::Decode, 20.0, 0.0, None);
        assert_eq!(receiver.try_recv().unwrap().percent, 99.9);
        emit_progress(&tx, ExportStage::Finalize, 100.0, 0.0, Some(0));
        assert_eq!(receiver.try_recv().unwrap().percent, 100.0);
        emit_progress(&tx, ExportStage::Finalize, f32::NAN, 0.0, None);
        assert_eq!(receiver.try_recv().unwrap().percent, 100.0);
    }

    #[test]
    fn bundled_media_exports_real_mp4_with_requested_encoder_quality() {
        use snow_recording_model::{
            RecordingBundleAsset, write_mouse_records, write_recording_bundle,
        };
        let directory = tempdir().unwrap();
        let bundle = directory.path().join("source.snowrec");
        let index = directory.path().join("index.bin");
        let mouse = directory.path().join("mouse.bin");
        let source = directory.path().join("source.mp4");
        let mut encoder = StreamingEncoder::create(StreamingEncoderConfig {
            loop_animated_images: true,
            output_path: source.clone(),
            format: ExportFormat::Mp4,
            width: 64,
            height: 48,
            fps: 10,
            codec: VideoCodec::H264,
            prefer_hardware_h264: false,
            execution_mode: ExportExecutionMode::SoftwareOnly,
            software_h264_priority: crate::SoftwareH264Priority::X264First,
            video: VideoEncodeConfig {
                quality: 20,
                speed: VideoEncodingSpeed::UltraFast,
            },
            encode_threads: 1,
            audio: Vec::new(),
        })
        .unwrap();
        encoder.push_rgba_frame(0, &vec![80; 64 * 48 * 4]).unwrap();
        encoder
            .push_rgba_frame(100, &vec![160; 64 * 48 * 4])
            .unwrap();
        encoder.finish_at_pts(2).unwrap();
        fs::rename(&source, &bundle).unwrap();
        let mut bytes = VIDEO_INDEX_MAGIC.to_vec();
        for frame in 0..2u64 {
            bytes.extend_from_slice(&frame.to_le_bytes());
            bytes.extend_from_slice(&(frame * 100).to_le_bytes());
            bytes.extend_from_slice(&100u32.to_le_bytes());
        }
        fs::write(&index, bytes).unwrap();
        write_mouse_records(&mouse, &MouseStore::new()).unwrap();
        let mut fixture = test_editing_session(false, false);
        fixture.manifest.width = 64;
        fixture.manifest.height = 48;
        fixture.manifest.fps = 10;
        fixture.manifest.audio_tracks.clear();
        write_recording_bundle(
            &bundle,
            &fixture.manifest,
            &[
                RecordingBundleAsset {
                    kind: BundleAssetKind::VideoIndex,
                    asset_id: None,
                    path: &index,
                },
                RecordingBundleAsset {
                    kind: BundleAssetKind::MouseStore,
                    asset_id: None,
                    path: &mouse,
                },
            ],
        )
        .unwrap();
        fixture.artifact.bundle_path = bundle.clone();
        fixture.artifact.local_paths.video_intermediate_path = bundle;
        let editing = EditingSession::open(fixture.artifact).unwrap();
        let mut request = editing.export_request();
        request.output_path = directory.path().join("published.mp4");
        request.video.quality = 95;
        request.performance.mode = ExportExecutionMode::SoftwareOnly;
        request.performance.encode_threads = 1;
        let result = editing.export(request).unwrap();
        assert_eq!(result.runtime_report.path, ExportPathKind::FullTranscode);
        assert_eq!(
            result.runtime_report.video_encoder.as_deref(),
            Some("libx264")
        );
        let bytes = fs::read(&result.output_path).unwrap();
        assert_eq!(&bytes[4..8], b"ftyp");
        let input = ffmpeg::format::input(&result.output_path).unwrap();
        let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
        assert_eq!(stream.parameters().id(), ffmpeg::codec::Id::H264);
    }

    #[test]
    fn prepare_overlay_base_rgba_reuses_resized_cache_for_same_source_index() {
        let source = StoredFrame {
            timestamp_ms: 0,
            duration_ms: 16,
            width: 1,
            height: 1,
            rgba: vec![10, 20, 30, 255],
        };
        let changed = StoredFrame {
            timestamp_ms: 1,
            duration_ms: 16,
            width: 1,
            height: 1,
            rgba: vec![220, 210, 200, 255],
        };
        let plan = NearestResizePlan::new(1, 1, 2, 2);
        let mut cache_key = None::<(usize, u32, u32)>;
        let mut cache = Vec::<u8>::new();
        let mut output = vec![0u8; 2 * 2 * 4];

        prepare_overlay_base_rgba(
            &source,
            42,
            2,
            2,
            Some(&plan),
            None,
            &mut cache_key,
            &mut cache,
            &mut output,
        );
        let first = output.clone();

        prepare_overlay_base_rgba(
            &changed,
            42,
            2,
            2,
            Some(&plan),
            None,
            &mut cache_key,
            &mut cache,
            &mut output,
        );
        assert_eq!(output, first);
    }

    #[test]
    fn prepare_overlay_base_rgba_refreshes_cache_when_source_index_changes() {
        let first_source = StoredFrame {
            timestamp_ms: 0,
            duration_ms: 16,
            width: 1,
            height: 1,
            rgba: vec![5, 6, 7, 255],
        };
        let second_source = StoredFrame {
            timestamp_ms: 1,
            duration_ms: 16,
            width: 1,
            height: 1,
            rgba: vec![50, 60, 70, 255],
        };
        let plan = NearestResizePlan::new(1, 1, 2, 2);
        let mut cache_key = None::<(usize, u32, u32)>;
        let mut cache = Vec::<u8>::new();
        let mut output = vec![0u8; 2 * 2 * 4];

        prepare_overlay_base_rgba(
            &first_source,
            1,
            2,
            2,
            Some(&plan),
            None,
            &mut cache_key,
            &mut cache,
            &mut output,
        );
        let first = output.clone();

        prepare_overlay_base_rgba(
            &second_source,
            2,
            2,
            2,
            Some(&plan),
            None,
            &mut cache_key,
            &mut cache,
            &mut output,
        );
        assert_ne!(output, first);
        assert_eq!(&output[0..4], &[50, 60, 70, 255]);
    }

    #[test]
    fn align_audio_pads_or_truncates_to_duration() {
        let padded =
            align_i16_interleaved_to_duration(vec![1; 20], 10, 2, Duration::from_millis(1_500));
        assert_eq!(padded.len(), 30);

        let truncated =
            align_i16_interleaved_to_duration(vec![1; 40], 10, 2, Duration::from_millis(1_500));
        assert_eq!(truncated.len(), 30);
    }

    fn audio_track_footer(
        manifest: &AudioTrackManifest,
        offset: u64,
        len: u64,
    ) -> RecordingBundleFooter {
        RecordingBundleFooter {
            manifest: SessionManifest {
                video_codec: snow_recording_model::VideoCodec::H264,
                audio_tracks: vec![manifest.clone()],
                ..test_editing_session(false, false).manifest
            },
            video_payload_len: 0,
            assets: vec![BundleAssetRecord {
                kind: BundleAssetKind::AudioTrack,
                asset_id: Some(manifest.asset_id.clone()),
                offset,
                len,
            }],
        }
    }

    #[test]
    fn audio_track_plan_reads_raw_pcm_described_by_the_manifest() {
        let temp = tempdir().expect("temporary directory should be created");
        let path = temp.path().join("track.bundle");
        // Leading bytes stand in for the video payload; the asset starts after them.
        let video_payload = [0xAAu8; 7];
        let samples = [100i16, -200, 300, -400];
        let mut file = fs::File::create(&path).expect("bundle should be created");
        file.write_all(&video_payload).unwrap();
        write_pcm_i16_le(&mut file, &samples);
        file.flush().unwrap();

        let manifest = test_track("system", AudioTrackRole::SystemOutput, true, 48_000, 2);
        let asset_offset = video_payload.len() as u64;
        let asset_len = (samples.len() * std::mem::size_of::<i16>()) as u64;
        let footer = audio_track_footer(&manifest, asset_offset, asset_len);

        let plan = build_audio_track_plan(&path, &footer, &manifest, 1.0)
            .expect("track plan should build")
            .expect("track should be present");
        assert_eq!(plan.asset_offset, asset_offset);
        assert_eq!(plan.frame_count, 2);

        let mut reader = AudioTrackReader::open(&path, plan, 2).unwrap();
        reader.ensure_cached_range(0, 2).unwrap();
        assert_eq!(reader.sample_at(0, 0), samples[0]);
        assert_eq!(reader.sample_at(0, 1), samples[1]);
        assert_eq!(reader.sample_at(1, 0), samples[2]);
        assert_eq!(reader.sample_at(1, 1), samples[3]);
    }

    #[test]
    fn generated_export_keeps_the_accepted_endpoint_and_cancels_before_publication() {
        let directory = tempdir().unwrap();
        let timeline = FinalizedTimeline::new(1001, 83).unwrap();
        let performance = ExportPerformanceConfig {
            mode: ExportExecutionMode::SoftwareOnly,
            encode_threads: 1,
            ..Default::default()
        };
        let cancel = Arc::new(AtomicBool::new(false));
        let path = directory.path().join("exact.mp4");
        export_video_generated(
            &path,
            32,
            24,
            timeline,
            ExportFormat::Mp4,
            VideoCodec::H264,
            false,
            None,
            192,
            &VideoEncodeConfig::default(),
            &performance,
            &cancel,
            None,
            &None,
            |_, rgba| {
                rgba.chunks_exact_mut(4)
                    .for_each(|pixel| pixel.copy_from_slice(&[20, 80, 120, 255]));
                Ok(())
            },
        )
        .unwrap();
        let input = ffmpeg::format::input(&path).unwrap();
        let video = input.streams().best(ffmpeg::media::Type::Video).unwrap();
        use ffmpeg::Rescale;
        let represented_ms = video.duration().rescale(video.time_base(), (1, 1000));
        assert!(
            (represented_ms - 1001).abs() <= 1,
            "container time-base quantization must stay within one millisecond of accepted Stop"
        );

        let canceled_path = directory.path().join("canceled.mp4");
        let cancellation = CancellationToken::default();
        let result = export_video_generated(
            &canceled_path,
            32,
            24,
            timeline,
            ExportFormat::Mp4,
            VideoCodec::H264,
            false,
            None,
            192,
            &VideoEncodeConfig::default(),
            &performance,
            &cancel,
            Some(&cancellation),
            &None,
            |index, rgba| {
                rgba.fill(80);
                if index as u64 + 1 == timeline.frame_count() {
                    cancellation.cancel();
                }
                Ok(())
            },
        );
        assert!(matches!(result, Err(ScreenRecorderError::ExportCanceled)));
        assert!(!canceled_path.exists());
    }

    #[test]
    fn legacy_streaming_audio_preserves_positions_with_bounded_retimed_chunks() {
        let directory = tempdir().unwrap();
        let bundle = directory.path().join("audio.bundle");
        let input_frames = 66_150;
        let mut file = fs::File::create(&bundle).unwrap();
        write_pcm_i16_le(&mut file, &vec![1000; input_frames * 2]);
        file.flush().unwrap();
        let plan = AudioMixPlan {
            bundle_path: bundle,
            sample_rate_hz: 44_100,
            channels: 2,
            playback_speed: 1.5,
            target_frames: 44_100,
            input_frame_count: input_frames,
            tracks: vec![AudioTrackPlan {
                asset_offset: 0,
                frame_count: input_frames,
                volume: 0.75,
            }],
        };
        let path = directory.path().join("audio.mp4");
        let cancel = Arc::new(AtomicBool::new(false));
        let progress = None;
        let mut encoder = EditingVideoEncoder::create(
            &path,
            32,
            24,
            83,
            ExportFormat::Mp4,
            VideoCodec::H264,
            false,
            Some(&plan),
            192,
            &VideoEncodeConfig::default(),
            &ExportPerformanceConfig {
                mode: ExportExecutionMode::SoftwareOnly,
                encode_threads: 1,
                ..Default::default()
            },
            false,
            None,
            83,
            &cancel,
            None,
            &progress,
        )
        .unwrap();
        let mut frame = ffmpeg::frame::Video::new(encoder.encoder.input_pixel_format(), 32, 24);
        frame.data_mut(0).fill(80);
        frame.data_mut(1).fill(128);
        frame.data_mut(2).fill(128);
        let scheduled = Cell::new(0);
        for _ in 0..83 {
            encoder
                .queue_prepared_frame(&frame, 1, true, &scheduled)
                .unwrap();
            assert!(encoder.audio_samples.len() <= 4096 * 2);
        }
        assert_eq!(encoder.audio_cursor, 44_100);
        let telemetry = encoder.finish(1000).unwrap();
        assert_eq!(telemetry.audio_encoder.as_deref(), Some("aac"));
        let input = ffmpeg::format::input(&path).unwrap();
        let audio = input.streams().best(ffmpeg::media::Type::Audio).unwrap();
        use ffmpeg::Rescale;
        assert!((1000..=1030).contains(&audio.duration().rescale(audio.time_base(), (1, 1000))));
    }

    #[test]
    fn audio_track_plan_rejects_assets_that_are_not_frame_aligned() {
        let temp = tempdir().expect("temporary directory should be created");
        let path = temp.path().join("track.bundle");
        let mut file = fs::File::create(&path).expect("bundle should be created");
        // Three i16 samples cannot form whole stereo frames.
        write_pcm_i16_le(&mut file, &[1i16, 2, 3]);
        file.flush().unwrap();

        let manifest = test_track("system", AudioTrackRole::SystemOutput, true, 48_000, 2);
        let footer = audio_track_footer(&manifest, 0, 6);

        let error = build_audio_track_plan(&path, &footer, &manifest, 1.0)
            .expect_err("partial frames must be rejected");
        assert!(
            error.to_string().contains("not aligned to 4-byte frames"),
            "unexpected error: {error}"
        );
    }

    #[test]
    fn multi_track_mix_keeps_linear_headroom_and_limits_overload() {
        let moderate = mixed_sample_i16(8_000.0 + 8_000.0, 2);
        let expected = (16_000.0 / 2.0f32.sqrt()).round() as i16;
        assert_eq!(moderate, expected);

        let overloaded = mixed_sample_i16(i16::MAX as f32 * 2.0, 2);
        assert!(overloaded > 0);
        assert!(overloaded < i16::MAX);
        assert!(
            f32::from(overloaded) / 32768.0 <= MIX_LIMITER_CEILING,
            "limited sample exceeded the mix ceiling"
        );
        let negative_overload = mixed_sample_i16(i16::MIN as f32 * 2.0, 2);
        assert!(negative_overload < 0);
        assert!((i32::from(negative_overload) + i32::from(overloaded)).abs() <= 1);
    }

    fn write_pcm_i16_le(file: &mut fs::File, samples: &[i16]) {
        for sample in samples {
            file.write_all(&sample.to_le_bytes())
                .expect("test pcm write should succeed");
        }
    }

    #[test]
    fn streaming_audio_renderer_matches_buffered_mix_pipeline() {
        let temp = tempdir().expect("temp dir should be created");
        let bundle_path = temp.path().join("audio.bundle");
        let mut bundle_file = fs::File::create(&bundle_path).expect("bundle should be created");

        let system_samples = vec![
            1_000, -1_000, 2_000, -2_000, 3_000, -3_000, 4_000, -4_000, 5_000, -5_000,
        ];
        let mic_samples = vec![300, -600, 900, -1_200, 1_500, -1_800];
        write_pcm_i16_le(&mut bundle_file, &system_samples);
        let mic_offset = (system_samples.len() * std::mem::size_of::<i16>()) as u64;
        write_pcm_i16_le(&mut bundle_file, &mic_samples);

        let session = test_editing_session(true, true);
        let mut manifest = session.manifest.clone();
        manifest.audio_tracks[0].sample_rate_hz = 10;
        manifest.audio_tracks[1].sample_rate_hz = 10;
        manifest.audio_tracks[0].channels = 2;
        manifest.audio_tracks[1].channels = 2;

        let footer = RecordingBundleFooter {
            manifest: manifest.clone(),
            video_payload_len: 0,
            assets: vec![
                BundleAssetRecord {
                    kind: BundleAssetKind::AudioTrack,
                    asset_id: Some(manifest.audio_tracks[0].asset_id.clone()),
                    offset: 0,
                    len: (system_samples.len() * std::mem::size_of::<i16>()) as u64,
                },
                BundleAssetRecord {
                    kind: BundleAssetKind::AudioTrack,
                    asset_id: Some(manifest.audio_tracks[1].asset_id.clone()),
                    offset: mic_offset,
                    len: (mic_samples.len() * std::mem::size_of::<i16>()) as u64,
                },
            ],
        };

        let mut request = session.export_request();
        set_track_enabled(&mut request, "system", true, 0.75);
        set_track_enabled(&mut request, "microphone", true, 1.25);
        request.playback_speed = 1.5;
        let target_duration_ms = 1_100;

        let expected = {
            let system = read_pcm_i16(
                &bundle_path,
                &footer,
                manifest.audio_tracks[0].asset_id.as_str(),
                manifest.audio_tracks[0].channels,
            )
            .expect("system pcm should load");
            let microphone = read_pcm_i16(
                &bundle_path,
                &footer,
                manifest.audio_tracks[1].asset_id.as_str(),
                manifest.audio_tracks[1].channels,
            )
            .expect("mic pcm should load");
            let mixed = mix_audio_tracks_i16_interleaved_owned(
                vec![(system, 0.75), (microphone, 1.25)],
                manifest.audio_tracks[0].channels,
            );
            let retimed = retime_audio_i16_interleaved_owned(
                mixed,
                manifest.audio_tracks[0].channels,
                request.playback_speed,
            );
            align_i16_interleaved_to_duration(
                retimed,
                manifest.audio_tracks[0].sample_rate_hz,
                manifest.audio_tracks[0].channels,
                Duration::from_millis(target_duration_ms),
            )
        };

        let plan = build_mixed_audio(
            &bundle_path,
            &footer,
            &manifest,
            &request,
            target_duration_ms,
        )
        .expect("mix plan should build")
        .expect("mix plan should exist");
        let mut renderer = AudioMixRenderer::new(&plan).expect("renderer should open");
        let mut actual = Vec::new();
        let mut chunk = Vec::new();
        let mut output_frame = 0usize;
        while output_frame < plan.target_frames {
            let take_frames = (plan.target_frames - output_frame).min(2);
            renderer
                .render(output_frame, take_frames, &mut chunk)
                .expect("chunk render should succeed");
            actual.extend_from_slice(&chunk);
            output_frame += take_frames;
        }

        assert_eq!(actual.len(), expected.len());
        for (actual_sample, expected_sample) in actual.iter().zip(&expected) {
            assert!(
                (i32::from(*actual_sample) - i32::from(*expected_sample)).abs() <= 1,
                "streaming and buffered mix differed by more than one LSB"
            );
        }
    }

    #[test]
    fn streaming_audio_renderer_matches_buffered_single_track_retime_pipeline() {
        let temp = tempdir().expect("temp dir should be created");
        let bundle_path = temp.path().join("audio.bundle");
        let mut bundle_file = fs::File::create(&bundle_path).expect("bundle should be created");

        let system_samples = vec![
            1_000, -1_000, 2_000, -2_000, 3_000, -3_000, 4_000, -4_000, 5_000, -5_000,
        ];
        write_pcm_i16_le(&mut bundle_file, &system_samples);

        let session = test_editing_session(true, false);
        let mut manifest = session.manifest.clone();
        manifest.audio_tracks[0].sample_rate_hz = 10;
        manifest.audio_tracks[0].channels = 2;

        let footer = RecordingBundleFooter {
            manifest: manifest.clone(),
            video_payload_len: 0,
            assets: vec![BundleAssetRecord {
                kind: BundleAssetKind::AudioTrack,
                asset_id: Some(manifest.audio_tracks[0].asset_id.clone()),
                offset: 0,
                len: (system_samples.len() * std::mem::size_of::<i16>()) as u64,
            }],
        };

        let mut request = session.export_request();
        set_track_enabled(&mut request, "system", true, 0.75);
        request.playback_speed = 1.5;
        let target_duration_ms = 700;

        let expected = {
            let system = read_pcm_i16(
                &bundle_path,
                &footer,
                manifest.audio_tracks[0].asset_id.as_str(),
                manifest.audio_tracks[0].channels,
            )
            .expect("system pcm should load");
            let mut scaled = system;
            scale_samples_i16_in_place(&mut scaled, 0.75);
            let retimed = retime_audio_i16_interleaved_owned(
                scaled,
                manifest.audio_tracks[0].channels,
                request.playback_speed,
            );
            align_i16_interleaved_to_duration(
                retimed,
                manifest.audio_tracks[0].sample_rate_hz,
                manifest.audio_tracks[0].channels,
                Duration::from_millis(target_duration_ms),
            )
        };

        let plan = build_mixed_audio(
            &bundle_path,
            &footer,
            &manifest,
            &request,
            target_duration_ms,
        )
        .expect("mix plan should build")
        .expect("mix plan should exist");
        let mut renderer = AudioMixRenderer::new(&plan).expect("renderer should open");
        let mut actual = Vec::new();
        let mut chunk = Vec::new();
        let mut output_frame = 0usize;
        while output_frame < plan.target_frames {
            let take_frames = (plan.target_frames - output_frame).min(2);
            renderer
                .render(output_frame, take_frames, &mut chunk)
                .expect("chunk render should succeed");
            actual.extend_from_slice(&chunk);
            output_frame += take_frames;
        }

        assert_eq!(actual, expected);
    }

    #[test]
    fn build_mouse_tracks_keeps_shape_binding() {
        let store = MouseStore {
            cursor_shapes: vec![CursorShapeRecord {
                shape_id: 7,
                hotspot_x: 3,
                hotspot_y: 4,
                width: 8,
                height: 8,
                mode: CursorShapeCompositionMode::MaskedColor,
                shape_rgba: vec![255; 8 * 8 * 4],
            }],
            cursor_frames: vec![snow_recording_model::CursorFrameRecord {
                timestamp_ms: 12,
                x: 100,
                y: 200,
                visible: true,
                shape_id: Some(7),
            }],
            clicks: vec![],
        };

        let tracks = build_mouse_tracks(store);
        assert_eq!(tracks.samples.len(), 1);
        assert_eq!(tracks.samples[0].shape_id, Some(7));
        assert!(tracks.cursor_shapes.contains_key(&7));
        assert!(matches!(
            tracks.cursor_shapes.get(&7),
            Some(CompiledCursorShape::Plan(shape))
                if shape.hotspot_x == 3
                    && shape.hotspot_y == 4
                    && shape.width == 8
                    && shape.height == 8
        ));
    }

    #[test]
    fn apply_mouse_overlays_draws_cursor_shape_pixels() {
        let mut frame = StoredFrame {
            timestamp_ms: 0,
            duration_ms: 16,
            width: 4,
            height: 4,
            rgba: vec![0; 4 * 4 * 4],
        };
        let store = MouseStore {
            cursor_shapes: vec![CursorShapeRecord {
                shape_id: 1,
                hotspot_x: 0,
                hotspot_y: 0,
                width: 1,
                height: 1,
                mode: CursorShapeCompositionMode::AlphaBlend,
                shape_rgba: vec![200, 10, 20, 255],
            }],
            cursor_frames: vec![snow_recording_model::CursorFrameRecord {
                timestamp_ms: 0,
                x: 2,
                y: 1,
                visible: true,
                shape_id: Some(1),
            }],
            clicks: vec![],
        };
        let tracks = build_mouse_tracks(store);

        apply_mouse_overlays(
            &mut frame,
            &tracks,
            &MouseEditConfig {
                visible: true,
                trail_enabled: false,
                click_enabled: false,
                ..MouseEditConfig::default()
            },
        );

        let px = (4usize + 2usize) * 4;
        assert_eq!(&frame.rgba[px..px + 4], &[200, 10, 20, 255]);
    }

    #[test]
    fn apply_mouse_overlays_skips_fully_transparent_alpha_shape() {
        let mut frame = StoredFrame {
            timestamp_ms: 0,
            duration_ms: 16,
            width: 16,
            height: 16,
            rgba: vec![0; 16 * 16 * 4],
        };
        let store = MouseStore {
            cursor_shapes: vec![CursorShapeRecord {
                shape_id: 99,
                hotspot_x: 0,
                hotspot_y: 0,
                width: 2,
                height: 2,
                mode: CursorShapeCompositionMode::AlphaBlend,
                shape_rgba: vec![
                    255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0,
                ],
            }],
            cursor_frames: vec![snow_recording_model::CursorFrameRecord {
                timestamp_ms: 0,
                x: 8,
                y: 8,
                visible: true,
                shape_id: Some(99),
            }],
            clicks: vec![],
        };
        let tracks = build_mouse_tracks(store);

        apply_mouse_overlays(
            &mut frame,
            &tracks,
            &MouseEditConfig {
                visible: true,
                trail_enabled: false,
                click_enabled: false,
                ..MouseEditConfig::default()
            },
        );

        assert!(
            frame.rgba.iter().all(|&value| value == 0),
            "invalid cursor data must not be replaced by an invented shape"
        );
    }

    #[test]
    fn apply_mouse_overlays_renders_masked_color_shape_without_black_box() {
        let mut frame = StoredFrame {
            timestamp_ms: 0,
            duration_ms: 16,
            width: 4,
            height: 2,
            rgba: vec![
                10, 20, 30, 255, 20, 40, 60, 255, 100, 120, 140, 255, 0, 0, 0, 255, // row 1
                0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255,
            ],
        };
        let store = MouseStore {
            cursor_shapes: vec![CursorShapeRecord {
                shape_id: 5,
                hotspot_x: 0,
                hotspot_y: 0,
                width: 3,
                height: 1,
                mode: CursorShapeCompositionMode::MaskedColor,
                shape_rgba: vec![
                    0, 0, 0, 0xFF, // alpha=0xFF + non-zero mask => XOR
                    0xFF, 0xFF, 0xFF, 0xFF, // alpha=0x00 => source copy
                    5, 6, 7, 0x00,
                ],
            }],
            cursor_frames: vec![snow_recording_model::CursorFrameRecord {
                timestamp_ms: 0,
                x: 0,
                y: 0,
                visible: true,
                shape_id: Some(5),
            }],
            clicks: vec![],
        };
        let tracks = build_mouse_tracks(store);

        apply_mouse_overlays(
            &mut frame,
            &tracks,
            &MouseEditConfig {
                visible: true,
                trail_enabled: false,
                click_enabled: false,
                ..MouseEditConfig::default()
            },
        );

        assert_eq!(&frame.rgba[0..4], &[10, 20, 30, 255]);
        assert_eq!(&frame.rgba[4..8], &[235, 215, 195, 255]);
        assert_eq!(&frame.rgba[8..12], &[5, 6, 7, 255]);
    }

    #[test]
    fn apply_mouse_overlays_skips_noop_masked_shape() {
        let mut frame = StoredFrame {
            timestamp_ms: 0,
            duration_ms: 16,
            width: 16,
            height: 16,
            rgba: vec![0; 16 * 16 * 4],
        };
        let store = MouseStore {
            cursor_shapes: vec![CursorShapeRecord {
                shape_id: 123,
                hotspot_x: 0,
                hotspot_y: 0,
                width: 2,
                height: 2,
                mode: CursorShapeCompositionMode::MaskedColor,
                shape_rgba: vec![0, 0, 0, 0xFF, 0, 0, 0, 0xFF, 0, 0, 0, 0xFF, 0, 0, 0, 0xFF],
            }],
            cursor_frames: vec![snow_recording_model::CursorFrameRecord {
                timestamp_ms: 0,
                x: 8,
                y: 8,
                visible: true,
                shape_id: Some(123),
            }],
            clicks: vec![],
        };
        let tracks = build_mouse_tracks(store);

        apply_mouse_overlays(
            &mut frame,
            &tracks,
            &MouseEditConfig {
                visible: true,
                trail_enabled: false,
                click_enabled: false,
                ..MouseEditConfig::default()
            },
        );

        assert!(
            frame.rgba.iter().all(|&value| value == 0),
            "a no-op mask must remain a no-op instead of drawing a substitute cursor"
        );
    }

    #[test]
    fn apply_mouse_overlays_trail_ignores_hidden_cursor_samples() {
        let mut frame = StoredFrame {
            timestamp_ms: 25,
            duration_ms: 16,
            width: 32,
            height: 32,
            rgba: vec![0; 32 * 32 * 4],
        };
        let store = MouseStore {
            cursor_shapes: vec![],
            cursor_frames: vec![
                snow_recording_model::CursorFrameRecord {
                    timestamp_ms: 0,
                    x: 10,
                    y: 10,
                    visible: true,
                    shape_id: None,
                },
                snow_recording_model::CursorFrameRecord {
                    timestamp_ms: 10,
                    x: 0,
                    y: 0,
                    visible: false,
                    shape_id: None,
                },
                snow_recording_model::CursorFrameRecord {
                    timestamp_ms: 20,
                    x: 20,
                    y: 20,
                    visible: true,
                    shape_id: None,
                },
            ],
            clicks: vec![],
        };
        let tracks = build_mouse_tracks(store);

        apply_mouse_overlays(
            &mut frame,
            &tracks,
            &MouseEditConfig {
                visible: false,
                trail_enabled: true,
                click_enabled: false,
                ..MouseEditConfig::default()
            },
        );

        let top_left = (32usize + 1usize) * 4;
        assert_eq!(&frame.rgba[top_left..top_left + 4], &[0, 0, 0, 0]);

        let mid = (15usize * 32 + 15usize) * 4;
        assert!(
            frame.rgba[mid] > 0 || frame.rgba[mid + 1] > 0 || frame.rgba[mid + 2] > 0,
            "visible cursor samples should still render trail segments"
        );
    }

    #[test]
    fn build_smoothed_trail_points_rounds_corners() {
        let points = vec![
            TrailCurvePoint {
                ts_ms: 0.0,
                x: 8.0,
                y: 8.0,
            },
            TrailCurvePoint {
                ts_ms: 10.0,
                x: 8.0,
                y: 24.0,
            },
            TrailCurvePoint {
                ts_ms: 20.0,
                x: 24.0,
                y: 24.0,
            },
        ];
        let smoothed = build_smoothed_trail_points(&points, 2.0);

        assert!(
            smoothed.len() > points.len(),
            "curve sampling should emit intermediate points"
        );
        assert!(
            smoothed
                .iter()
                .any(|p| p.x > 8.0 && p.x < 16.0 && p.y > 16.0 && p.y < 24.0),
            "smoothed trail should include rounded corner points between line segments"
        );
    }

    #[test]
    fn collect_visible_trail_window_moves_tail_continuously() {
        let samples = vec![
            MouseSample {
                ts_ms: 0,
                x: 0,
                y: 0,
                visible: true,
                shape_id: None,
            },
            MouseSample {
                ts_ms: 100,
                x: 100,
                y: 0,
                visible: true,
                shape_id: None,
            },
            MouseSample {
                ts_ms: 200,
                x: 200,
                y: 0,
                visible: true,
                shape_id: None,
            },
        ];

        let window_99 = collect_visible_trail_window(&samples, 99);
        let window_100 = collect_visible_trail_window(&samples, 100);
        let window_101 = collect_visible_trail_window(&samples, 101);

        assert!(!window_99.is_empty());
        assert!(!window_100.is_empty());
        assert!(!window_101.is_empty());
        assert!((window_99[0].x - 99.0).abs() < 0.01);
        assert!((window_100[0].x - 100.0).abs() < 0.01);
        assert!((window_101[0].x - 101.0).abs() < 0.01);
    }

    #[test]
    fn native_overlay_path_skips_missing_cursor_shape_on_yuv420p_frame() {
        let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::YUV420P, 16, 16);
        frame.data_mut(0).fill(16);
        frame.data_mut(1).fill(128);
        frame.data_mut(2).fill(128);

        let tracks = build_mouse_tracks(MouseStore {
            cursor_shapes: vec![],
            cursor_frames: vec![snow_recording_model::CursorFrameRecord {
                timestamp_ms: 0,
                x: 4,
                y: 4,
                visible: true,
                shape_id: None,
            }],
            clicks: vec![],
        });
        let config = MouseEditConfig {
            visible: true,
            trail_enabled: false,
            click_enabled: false,
            ..MouseEditConfig::default()
        };
        let mut state = OverlaySearchState::default();
        let decision = advance_overlay_state(0, &tracks, &config, &mut state);

        assert!(
            !try_apply_mouse_overlays_native_from_decision(
                &mut frame, 0, &tracks, &config, &mut state, decision,
            )
            .unwrap()
        );
        assert!(frame.data(0).iter().all(|&value| value == 16));
        assert!(frame.data(1).iter().all(|&value| value == 128));
        assert!(frame.data(2).iter().all(|&value| value == 128));
    }

    #[test]
    fn native_overlay_path_skips_missing_cursor_shape_on_nv12_frame() {
        let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::NV12, 16, 16);
        frame.data_mut(0).fill(16);
        frame.data_mut(1).fill(128);

        let tracks = build_mouse_tracks(MouseStore {
            cursor_shapes: vec![],
            cursor_frames: vec![snow_recording_model::CursorFrameRecord {
                timestamp_ms: 0,
                x: 5,
                y: 5,
                visible: true,
                shape_id: None,
            }],
            clicks: vec![],
        });
        let config = MouseEditConfig {
            visible: true,
            trail_enabled: false,
            click_enabled: false,
            ..MouseEditConfig::default()
        };
        let mut state = OverlaySearchState::default();
        let decision = advance_overlay_state(0, &tracks, &config, &mut state);

        assert!(
            !try_apply_mouse_overlays_native_from_decision(
                &mut frame, 0, &tracks, &config, &mut state, decision,
            )
            .unwrap()
        );
        assert!(frame.data(0).iter().all(|&value| value == 16));
        assert!(frame.data(1).iter().all(|&value| value == 128));
    }

    #[test]
    fn native_horizontal_span_matches_per_pixel_yuv420p() {
        let mut optimized = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::YUV420P, 8, 8);
        optimized.data_mut(0).fill(16);
        optimized.data_mut(1).fill(128);
        optimized.data_mut(2).fill(128);
        let mut reference = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::YUV420P, 8, 8);
        reference.data_mut(0).fill(16);
        reference.data_mut(1).fill(128);
        reference.data_mut(2).fill(128);
        let color = YuvBlendColor::from_rgba([12, 140, 220, 173]);

        {
            let mut view = native_frame_view_mut(&mut optimized).unwrap().unwrap();
            view.blend_horizontal_span(3, 1, 6, color);
        }
        {
            let mut view = native_frame_view_mut(&mut reference).unwrap().unwrap();
            for x in 1..=6 {
                view.blend_pixel(x, 3, color);
            }
        }

        assert_eq!(optimized.data(0), reference.data(0));
        assert_eq!(optimized.data(1), reference.data(1));
        assert_eq!(optimized.data(2), reference.data(2));
    }

    #[test]
    fn native_horizontal_span_matches_per_pixel_nv12() {
        let mut optimized = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::NV12, 8, 8);
        optimized.data_mut(0).fill(16);
        optimized.data_mut(1).fill(128);
        let mut reference = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::NV12, 8, 8);
        reference.data_mut(0).fill(16);
        reference.data_mut(1).fill(128);
        let color = YuvBlendColor::from_rgba([200, 32, 90, 117]);

        {
            let mut view = native_frame_view_mut(&mut optimized).unwrap().unwrap();
            view.blend_horizontal_span(4, 0, 7, color);
        }
        {
            let mut view = native_frame_view_mut(&mut reference).unwrap().unwrap();
            for x in 0..=7 {
                view.blend_pixel(x, 4, color);
            }
        }

        assert_eq!(optimized.data(0), reference.data(0));
        assert_eq!(optimized.data(1), reference.data(1));
    }
    #[test]
    fn prepared_export_preserves_reordered_pts_and_final_hold_duration() {
        let directory = tempdir().unwrap();
        let path = directory.path().join("reordered.mp4");
        let cancel = Arc::new(AtomicBool::new(false));
        let progress = None;
        let mut encoder = EditingVideoEncoder::create(
            &path,
            64,
            64,
            30,
            ExportFormat::Mp4,
            VideoCodec::H264,
            false,
            None,
            192,
            &VideoEncodeConfig {
                quality: 80,
                speed: VideoEncodingSpeed::VeryFast,
            },
            &ExportPerformanceConfig {
                mode: ExportExecutionMode::SoftwareOnly,
                encode_threads: 1,
                ..Default::default()
            },
            false,
            None,
            35,
            &cancel,
            None,
            &progress,
        )
        .unwrap();
        let mut frame = ffmpeg::frame::Video::new(encoder.encoder.input_pixel_format(), 64, 64);
        frame.data_mut(0).fill(80);
        frame.data_mut(1).fill(128);
        frame.data_mut(2).fill(128);
        let count = Cell::new(0);
        let mut expected = Vec::new();
        for duration in [1, 4, 2, 7, 3, 5, 2, 11] {
            expected.push((count.get() as i64, duration as i64));
            encoder
                .queue_prepared_frame(&frame, duration, false, &count)
                .unwrap();
        }
        encoder.finish(1167).unwrap();
        use ffmpeg::Rescale;
        let mut input = ffmpeg::format::input(&path).unwrap();
        let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
        let time_base = stream.time_base();
        let index = stream.index();
        let mut packets: Vec<_> = input
            .packets()
            .filter_map(|(stream, packet)| {
                (stream.index() == index).then(|| {
                    (
                        packet.pts().unwrap().rescale(time_base, (1, 30)),
                        packet.duration().rescale(time_base, (1, 30)),
                    )
                })
            })
            .collect();
        assert!(packets.windows(2).any(|pair| pair[0].0 > pair[1].0));
        // MP4 stores decode-order sample durations; reordered B-frame packet
        // durations after demux need not equal the submitted presentation holds.
        // Every presentation PTS and the exclusive final endpoint must survive.
        assert!(packets.iter().all(|(_, duration)| *duration > 0));
        packets.sort_unstable();
        assert_eq!(
            packets.iter().map(|(pts, _)| *pts).collect::<Vec<_>>(),
            expected.iter().map(|(pts, _)| *pts).collect::<Vec<_>>()
        );
        assert_eq!(input.duration().rescale((1, 1_000_000), (1, 30)), 35);
    }

    #[test]
    fn hardware_quality_targets_follow_requested_quality() {
        ensure_ffmpeg_initialized().unwrap();
        for (name, keys) in [
            ("h264_nvenc", &["qp"][..]),
            ("h264_qsv", &["global_quality"][..]),
            ("h264_amf", &["qp_i", "qp_p"][..]),
        ] {
            let Some(codec) = ffmpeg::encoder::find_by_name(name) else {
                continue;
            };
            let mut lower = ffmpeg::Dictionary::new();
            let mut higher = ffmpeg::Dictionary::new();
            assert!(apply_hardware_encoder_options(&mut lower, &codec, 40));
            assert!(apply_hardware_encoder_options(&mut higher, &codec, 80));
            for key in keys {
                assert!(
                    higher.get(key).unwrap().parse::<u8>().unwrap()
                        < lower.get(key).unwrap().parse::<u8>().unwrap(),
                    "{name}: {key}"
                );
            }
        }
    }

    #[test]
    fn media_foundation_hardware_selection_is_explicit() {
        if let Some(codec) = ffmpeg::encoder::find_by_name("h264_mf") {
            let mut options = ffmpeg::Dictionary::new();
            assert!(apply_hardware_encoder_options(&mut options, &codec, 80));
            assert_eq!(options.get("hw_encoding"), Some("1"));
        }
    }
}
